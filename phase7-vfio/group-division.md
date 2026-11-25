# IOMMU 组是怎么划出来的（VFIO 直通粒度）

> 承接：[README](README.md) §1.4 核心概念 + [phase3 group.md](../phase3-iommu/group.md)（core 分组算法）。
> 本文回答：组不是 VFIO 划的——划分在设备 probe 的 IOMMU core 里，VFIO 只消费既成结果。
> 引出：ACS/ATS 能力本身 → [acs-ats.md](acs-ats.md)；真机取证 → [practice/](practice/README.md)。
>
> 本文自 README §1.4 拆出（2026-11-25 框架拆分），内容未改。

### 1.4 IOMMU 组是怎么划出来的

**组不是 VFIO 划的。** 划分发生在设备 probe 阶段的 IOMMU core 里，VFIO 只是消费既成结果 ——
启动时读 `/sys/kernel/iommu_groups/`，无法改变分组。

```c
/* 来源: drivers/iommu/iommu.c:427 —— iommu_init_device()（定义 :402）
 * 调用关系: __iommu_probe_device()（:513）在 :544 调 iommu_init_device(dev, ops) */
	group = ops->device_group(dev);
```

Intel IOMMU 的回调按总线类型分流：

```c
/* 来源: drivers/iommu/intel/iommu.c:4085 */
static struct iommu_group *intel_iommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	return generic_device_group(dev);
}
```

> **判据不是拓扑位置，而是硬件能否保证 peer-to-peer DMA 隔离。**
> 常见误解是「同一个 switch 下的设备属于一个组」—— 这只在 switch 下游端口缺 ACS 时才成立。
> ACS 齐备时，同一个 switch 下每个设备各自独占一个组（下文有实测）。

#### 1.4.1 `pci_device_group()` 的四步判定

`drivers/iommu/iommu.c:1515-1576`，按序尝试，任一步撞到既有组就复用，全落空才新建：

| 步 | 行号 | 做什么 |
|---|---|---|
| 1 | `:1532` | `pci_for_each_dma_alias()` 沿 DMA alias 向上找，途中遇到已有组就用它 |
| 2 | `:1543-1555` | **核心**：向上走 PCI 层级，直到 ACS 能保证隔离 |
| 3 | `:1561` | 查 DMA alias 关系的既有组（quirk 声明的 requester ID 伪装） |
| 4 | `:1570` | 查同 slot 上未隔离的其他 function |
| — | `:1575` | 都没有 → `iommu_group_alloc()` 独占一组 |

第 2 步是决定性的：

```c
/* 来源: drivers/iommu/iommu.c:1543 —— pci_device_group() */
for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
	if (!bus->self)
		continue;

	if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
		break;                 /* 隔离成立 → 就此收手 */

	pdev = bus->self;          /* 不成立 → 把这个桥拉进同一组 */

	group = iommu_group_get(&pdev->dev);
	if (group)
		return group;
}
```

注意 `pci_acs_path_enabled(bus->self, NULL, ...)` 检查的是**整条路径**
（`drivers/pci/pci.c:3693`，任一跳不满足即返回 false），所以第一轮迭代就把
「设备的父桥一直到 root bus」全查了。全通过 → 立即 `break`，`pdev` 仍是设备本身
→ 走到 `:1575` 独占一组。

要求的四个 flag：

```c
/* 来源: drivers/iommu/iommu.c:1383 */
#define REQ_ACS_FLAGS   (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)
```

即 Source Validation、Request Redirect、Completer Redirect、Upstream Forwarding。
**不含** `TransBlk` / `EgressCtrl` / `DirectTrans` —— 所以 `lspci` 里看到 `TransBlk-`
不影响判定，不要误以为隔离不成立。

#### 1.4.2 关键坑：`pci_acs_enabled()` 查的是「有效能力」不是「实际能力」

`drivers/pci/pci.c:3620` 的判定分两层：**先查设备专属 quirk，再按 PCIe 类型分类**。

| 层 | 行号 | 行为 | 这一类的 ACS 位管的是什么 |
|---|---|---|---|
| **0. 设备专属 quirk** | `:3624-3626` | 命中就直接定论，**下面几行全部不执行** | 视 quirk 而定 |
| 1. Downstream Port / Root Port | `:3657-3659` | 读配置空间校验，但**要求集先被 ACSCap 掩掉**（见下） | **穿过该端口的跨设备 P2P**；单功能也照样要求实现 |
| 2. 单功能 Endpoint / Upstream Port / Leg End / RC End | `:3667-3670` → `:3671` `break` → `:3681` | **没有 ACS capability 也返回 true** | 无事可管（没有第二个口） |
| 2'. 同上，但多功能 | `:3674` | 改读设备**自己的** ACS 标志位，没有 capability 即 false | 同设备内 func↔func（多功能端点）；跨设备（多功能 USP） |
| 3. PCIe-to-PCI 桥、PCI 桥、RC-EC | `:3642-3651` | 无条件 `return false` | 桥后的传统总线共享，无法管 |
| 4. 非 PCIe（传统 PCI / PCI-X） | `:3633` | `return false` | 共享总线，无法管 |

第 2 类的理由写在注释里：

> most single function endpoints are not required to support ACS because they have no
> opportunity for peer-to-peer access. We therefore return 'true' regardless of whether
> the device exposes an ACS capability.
>
> —— `drivers/pci/pci.c:3612-3618`

所以 `lspci` 里某一跳**完全没有** ACS capability，并不等于隔离不成立。但要看准是哪个条件在起作用：
**决定「跳到 `pci.c:3681` 返回 true」的是 header type 的 multifunction 位**（`pci.c:3671`，
由 `drivers/pci/probe.c:1940` 从配置空间 `0x0e` bit7 读出），**PCIe 类型决定的是另一件事**
—— 这一支到底读不读设备自己的 ACS 位。桥类即使在单功能下也照样 false，因为
`drivers/pci/pci.c:3649-3651` 在读 `multifunction` 之前就 return 了。所以不能简写成「单功能 ⇒ true」。

第 2' 类为什么要读设备自己的位？因为**多功能设备内部的 func↔func 通路在 PCI 拓扑上是隐形的**
—— 内核从 bus/slot 根本看不见第二个口，只能靠设备在配置空间自己声明
（`PCI_ACS_DT`，"Direct Translated P2P"，`include/uapi/linux/pci_regs.h:997`）。
这正是 [1.4.4 ②](#144-三种真正会并组的情形) 归并同 slot function 的前提。
而单功能设备没有第二个口，问它 ACS 问不出任何信息 —— 它的横向转发决策点在上游的
Downstream Port，那里由 `drivers/pci/pci.c:3657-3659` 单独查。`return true` 的含义是**「本跳不提供信息」**，
不是「本跳没有风险」；隔离性由 `pci_acs_path_enabled()`（`drivers/pci/pci.c:3693`）逐跳问上去共同保证。

第 1 类也不是「四个 flag 必须全在 `ACSCtl` 里置位」那么简单。校验前先按设备**声明**的
`ACSCap` 做一次掩码：

```c
/* 来源: drivers/pci/pci.c:3597 —— pci_acs_flags_enabled() */
	pci_read_config_word(pdev, pos + PCI_ACS_CAP, &cap);
	acs_flags &= (cap | PCI_ACS_EC);
```

**没声明的能力被当作「硬连线已启用」直接跳过检查** ——「`ACSCap` 里是 `-`」不算失败，
只有「`ACSCap` 里有、`ACSCtl` 里没开」才算。宿主上 `00:1c.4` 就是这样通过的：

```
ACSCap: SrcValid+ TransBlk+ ReqRedir+ CmpltRedir+ UpstreamFwd- ...
ACSCtl: SrcValid+ TransBlk- ReqRedir+ CmpltRedir+ UpstreamFwd- ...
```

`UpstreamFwd` 在 Cap 里就是 `-`，被 `:3598` 从 `REQ_ACS_FLAGS` 里掩掉，剩下三个
Cap/Ctl 都置位 → 返回 true。这也解释了为什么它下游的 `02:00.0` 没有并进 `00:1c.4`
所在的 group 99。

##### 别把「设备层的 ACS」等同于「管 function 之间」

一个自然的推论是：既然端点的 ACS 只被 `iommu.c:1397` 用来决定同 slot 的 function 拆不拆，
那 ACS 就是「设备内部 function 之间的开关」。**不对** —— 真正的判据是
**「这个器件有没有 ≥2 个口、能不能横向转发报文」**，与它是端点还是桥无关。
表里第 1 类占了最大一块篇幅：Downstream Port 和 Root Port **单功能也照样要求实现 ACS**
（spec 6.12.1.1，`pci.c:3653-3655` 转述：「regardless of whether they are single- or
multi-function devices」），它们管的是**穿过这个端口的跨设备 P2P**，跟 function 一点关系都没有。

两个调用点的语义分工很能说明问题，注意**问的对象不一样**：

```c
/* 来源: drivers/iommu/iommu.c:1397 —— 问「这个器件自己的内部通路隔不隔离」 */
	if (!pdev->multifunction || pci_acs_enabled(pdev, REQ_ACS_FLAGS))

/* 来源: drivers/iommu/iommu.c:1547 —— 问「这条路径上所有转发点隔不隔离」 */
		if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
```

第二处的起点是 `bus->self`，即**父桥**，不是设备本身：

```c
/* 来源: drivers/pci/pci.c:3695 —— pci_acs_path_enabled() */
	struct pci_dev *pdev, *parent = start;

	do {
		pdev = parent;

		if (!pci_acs_enabled(pdev, acs_flags))
			return false;
```

所以**端点自己的 ACS 位从头到尾都不参与路径检查**。对端点来说，它的 ACS 确实只服务于
`iommu.c:1397` 那一处，也就是只决定「这个多功能设备要不要拆成多个组」；
单功能端点的位则完全没人问。

**反例：多功能 USP。** 上游端口也走 `:3674` 读自己的位，但它内部的横向转发是**跨设备**的
（switch 各下游端口的流量都从它这里过），同一个位、同一条代码路径，语义完全不同。

最后，ACS 不只服务分组，它还门控特性启用。开 PASID 前必须保证路径上没人能就地横穿
translated 请求，否则设备拿着自己缓存的地址就能绕开 IOMMU 读别人的内存：

```c
/* 来源: drivers/pci/ats.c:419 —— pci_enable_pasid() */
	if (!pci_acs_path_enabled(pdev, NULL, PCI_ACS_RR | PCI_ACS_UF))
		return -EINVAL;
```

这里要的是 `RR + UF` 两位而非 `REQ_ACS_FLAGS` 全集 —— 用途不同，要求集也不同。
（对比：ATS 本身只看 `ats_cap` 和 `untrusted`，`ats.c:41-47`，没有 ACS 检查。）

> 顺带：`!CONFIG_PCI` 时 `pci_acs_enabled()` 是恒为 false 的桩（`include/linux/pci.h:2066`）。

##### 第 0 层：设备专属 quirk 可以完全绕过上面所有规则

这一层最容易被忽略 —— 它在函数最开头，命中就直接返回：

```c
/* 来源: drivers/pci/pci.c:3620 —— pci_acs_enabled() */
bool pci_acs_enabled(struct pci_dev *pdev, u16 acs_flags)
{
	int ret;

	ret = pci_dev_specific_acs_enabled(pdev, acs_flags);
	if (ret >= 0)
		return ret > 0;
```

`pci_dev_specific_acs_enabled()`（`drivers/pci/quirks.c:5234`）遍历 `pci_dev_acs_enabled[]`
表（`quirks.c:5052`），三态返回：`-ENOTTY` = 无 quirk 适用（继续走类型判断）、`0` = 不满足、
`>0` = 满足。用途是让**没有实现 ACS capability、但厂商确认不做内部 p2p** 的设备也算隔离成立。

x86 上影响面最大的一条匹配**所有 Intel 设备**：

```c
/* 来源: drivers/pci/quirks.c:5122 */
	{ PCI_VENDOR_ID_INTEL, PCI_ANY_ID, pci_quirk_rciep_acs },
```
```c
/* 来源: drivers/pci/quirks.c:4991 */
static int pci_quirk_rciep_acs(struct pci_dev *dev, u16 acs_flags)
{
	/*
	 * Intel RCiEP's are required to allow p2p only on translated
	 * addresses.  Refer to Intel VT-d specification, r3.1, sec 3.16,
	 * "Root-Complex Peer to Peer Considerations".
	 */
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_RC_END)
		return -ENOTTY;

	return pci_acs_ctrl_enabled(acs_flags,
		PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);
}
```

给出的四位正好等于 `REQ_ACS_FLAGS`，`pci_acs_ctrl_enabled()`（`quirks.c:4653`）做的是
「请求集是否被提供集覆盖」，所以**对 Intel RCiEP 一律返回 1** —— 哪怕设备根本没有 ACS
capability，也算隔离成立。另一条常见的是 `pci_quirk_mf_endpoint_acs()`（`quirks.c:4975`），
按型号白名单给多功能网卡（大量 Intel igb/ixgbe、Broadcom、Solarflare）放行。

**实测对照 —— 同为 3 个 function 的两组 Intel 设备，结果完全相反：**

| 设备 | PCIe 类型 | `pci_acs_enabled()` | group |
|---|---|---|---|
| `00:08.0` Ubox Registers | RCiEP | true（`pci_quirk_rciep_acs`） | 91 |
| `00:08.1` Performance counters | **无任何 capability**（`Status: Cap-`） | false（`pci.c:3633`） | 92 |
| `00:08.2` Ubox Registers | RCiEP | true（`pci_quirk_rciep_acs`） | 93 |
| `00:16.0/.1/.4` MEI Controller | 无 PCIe capability（只有 PM + MSI） | 三个都 false | **96，三个一组** |

`00:08.x` 三个 function 全部独占，但**原因是两种**：

- `.0` / `.2` 被 quirk 判成隔离成立，`iommu.c:1397` 的门直接 `return NULL`；
- `.1` 自己是 false、门是通过的，但循环要求候选方**也**不隔离（`iommu.c:1401-1404` 的
  `pci_acs_enabled(tmp, ...)` → `continue`），同 slot 的 `.0` / `.2` 全被跳过 →
  找不到搭档 → 仍然 `return NULL`。**它不是「已隔离」，是「想并组但没人肯跟它并」。**

`00:16.x` 逃掉了这条 quirk（不是 RCiEP），三个都 false，于是真的并成 group 96。

> 判断某个设备是否被 quirk 放行：不能只看「没有 ACS capability 又独占一组」——
> 桥类设备与「没有同 slot 搭档」的多功能设备都会误入。可用的判据（同一 slot 的多个
> function 被拆进不同组、且整个 slot 没有 ACS capability）与实测结果见
> [1.5.8](#158-观测) 的 ④。
>
> Intel RCiEP 认作隔离的依据是 VT-d 规范而非 PCIe ACS。本仓 `intel-vtd.pdf` 是 **Rev 4.1**，
> 对应 **Section 3.17 (Root-Complex Peer to Peer Considerations)**（quirk 注释引的是 r3.1 的
> 3.16，章节号随版本挪过）：peer 请求「must be done only on the translated HPA」——
> 即 p2p 只能发生在已翻译地址上，因此 IOMMU 的隔离不会被绕过。

#### 1.4.3 实测：一个 switch，30+ 个组

宿主上 `48:00.0`（Upstream Port）带 32 个 Downstream Port `49:00.0`–`49:1f.0`，
构成一个完整的 PCIe switch。实际划分结果：

| 设备 | 角色 | group |
|---|---|---|
| `48:00.0` | switch 上游端口 | 1 |
| `49:00.0` … `49:1f.0` | 32 个下游端口 | **2 … 33，各自一组** |
| `4a:00.0` | 端点（挂 `49:00.0`） | 34 |
| `4b:00.0` | 端点（挂 `49:01.0`） | 35 |
| `5b:00.0` | 端点（挂 `49:11.0`） | 36 |
| `69:00.0` | 端点（挂 `49:1f.0`） | 37 |

同一个 switch 下拆出了 30 多个独立组。以 `4b:00.0` 为例逐跳验证：

| BDF | PCIe 类型 | ACS 实际情况 | `pci_acs_enabled()` |
|---|---|---|---|
| `49:01.0` | Downstream Port | `ACSCtl: SrcValid+ ReqRedir+ CmpltRedir+ UpstreamFwd+` 四个全置 | true（走 `pci.c:3659`） |
| `48:00.0` | Upstream Port | **完全没有 ACS capability** | true（单功能，走 `pci.c:3681`） |
| `47:00.0` | Root Port | `ACSCtl` 四个全置 | true（走 `pci.c:3659`） |

三跳全通 → `pci_acs_path_enabled()` = true → 循环第一轮就 `break` → 独占 group 35。

复现命令：

```bash
# 列出所有多设备组，一眼看出哪些设备无法单独直通
cd /sys/kernel/iommu_groups
for g in $(ls -v); do
    n=$(ls $g/devices | wc -l)
    [ "$n" -gt 1 ] && echo "group $g ($n): $(ls $g/devices | tr '\n' ' ')"
done

# 沿上游链路逐跳看 PCIe 类型与 ACS
lspci -vvv -s 49:01.0 | grep -A2 "Access Control Services"
```

#### 1.4.4 三种真正会并组的情形

**① 上游有 PCIe-to-PCI 桥** —— 同机 group 101 = `02:00.0` + `03:00.0`：

```
02:00.0 PCI bridge: ASPEED AST1150 PCI-to-PCI Bridge
        Capabilities: [80] Express (v2) PCI-Express to PCI/PCI-X Bridge
03:00.0 VGA compatible controller: ASPEED Graphics Family
        # 无任何 Express capability —— 传统 PCI 设备
```

`lspci` 印出的 "PCI-Express to PCI/PCI-X Bridge" 对应的常量是
**`PCI_EXP_TYPE_PCI_BRIDGE`（0x7）**，不是名字更像的 `PCI_EXP_TYPE_PCIE_BRIDGE`（0x8）；
后者是反方向的 "PCI/PCI-X to PCIe Bridge"（`include/uapi/linux/pci_regs.h:482-483`）。
实测确认：

```bash
setpci -s 02:00.0 0x82.w      # PCIe Cap @0x80 + 2 = PCI_EXP_FLAGS
# 0072  → bits 7:4 = 0x7 = PCI_EXP_TYPE_PCI_BRIDGE
```

并组发生在**第 1 步 DMA alias**，不是第 2 步的 ACS 循环。`03:00.0` 是传统 PCI 设备，
自身没有 PCIe requester ID，桥代它发 TLP，所以别名枚举在桥这一跳会真的回调：

```c
/* 来源: drivers/pci/search.c:84 —— pci_for_each_dma_alias() */
		case PCI_EXP_TYPE_ROOT_PORT:
		case PCI_EXP_TYPE_UPSTREAM:
		case PCI_EXP_TYPE_DOWNSTREAM:
			continue;                    /* 纯 PCIe 路径：跳过，不回调 */
		case PCI_EXP_TYPE_PCI_BRIDGE:
			ret = fn(tmp,
				 PCI_DEVID(tmp->subordinate->number,
					   PCI_DEVFN(0, 0)), data);
```

`02:00.0` 的 `subordinate` 就是 bus 03，且它先于 `03:00.0` 被 probe、已经独占了 group 101。
回调 `get_pci_alias_or_group()`（`iommu.c:1470`）只看 `tmp` 有没有组，一看有 → 返回非 0
→ `pci_device_group()` 在 `iommu.c:1533` 直接 `return data.group`，**ACS 循环根本没执行到**。

> 桥的 `pci_acs_enabled()` 确实也是无条件 false（`pci.c:3649-3651`），若 probe 顺序反过来，
> 第 2 步同样会把它拉进组 —— 两条路殊途同归，但本机实际走的是第 1 步。

**② 多功能设备各 function 之间无 ACS** —— 同机 group 96 = `00:16.0/.1/.4`
（Intel MEI Controller，ACS capability 数量为 0），由第 4 步归并：

```c
/* 来源: drivers/iommu/iommu.c:1397 —— get_pci_function_alias_group() */
	if (!pdev->multifunction || pci_acs_enabled(pdev, REQ_ACS_FLAGS))
		return NULL;

	for_each_pci_dev(tmp) {
		if (tmp == pdev || tmp->bus != pdev->bus ||
		    PCI_SLOT(tmp->devfn) != PCI_SLOT(pdev->devfn) ||
		    pci_acs_enabled(tmp, REQ_ACS_FLAGS))
			continue;
```

条件是**同 bus 同 slot**、且双方都没有 ACS。同机 group 105–114、168–177 的
「8 个 function 一组」都走这条路径。

反过来，`00:08.0/.1/.2` 同样是 3 个 function 却各自独占（91/92/93），因为前两个被
[第 0 层的 Intel RCiEP quirk](#第-0-层设备专属-quirk-可以完全绕过上面所有规则) 判成隔离成立 ——
**多功能设备是否同组，先看有没有 quirk 放行，再看 ACS。**

**③ DMA alias** —— 设备发出的 DMA 携带的 requester ID (RID) 不等于自己的 BDF。
IOMMU 是按 RID 查上下文表的，两个设备共用同一个 RID 就无法分辨、必须同组。
`pci_for_each_dma_alias()`（`drivers/pci/search.c:28`）枚举四个来源：

| 来源 | 代码位置 | 说明 |
|---|---|---|
| 拓扑：桥代发 | `search.c:88` / `:95` / `:102` | 桥后的传统 PCI 设备没有 PCIe RID，桥用 `(subordinate_bus, 00.0)` 代发 —— 即情形 ① |
| `dma_alias_mask` | `search.c:49-58` | quirk 为有 bug 的硬件声明的额外 RID，`pci_add_dma_alias()`（`pci.c:6497`）设置 |
| `pci_real_dma_dev()` | `search.c:39` | 架构钩子，DMA 实际由另一条总线上的设备发出；x86 上只有 VMD 覆盖（`arch/x86/pci/common.c:727`），默认返回自身（`pci.c:6566`） |
| 桥的 dev_flags | `search.c:70` / `:102` | `PCI_DEV_FLAGS_BRIDGE_XLATE_ROOT`（`quirks.c:4467`）截断向上枚举；`PCI_DEV_FLAG_PCIE_BRIDGE_ALIAS`（`quirks.c:4402`）让非 PCIe 桥也用 subordinate bus 代发 |

典型 quirk：Ricoh / Marvell 的多 function 设备把 DMA 全挂到 func0 或 func1
（`quirks.c:4269`、`:4287`）；PLX 8000 NTB 直接 `pci_add_dma_alias(pdev, 0, 256)`
把整条 bus 的 256 个 devfn 全声明成别名（`quirks.c:6057`）。

别名关系由 `pci_devs_are_dma_aliases()`（`pci.c:6523`）判定，第 3 步
`get_pci_alias_group()`（`iommu.c:1425`）用它反查既有组。注意**别名只能来自 quirk**，
因为组的创建早于任何驱动 probe：

> IOMMU group creation is performed during device discovery or addition, prior to any
> potential DMA mapping and therefore prior to driver probing ... DMA aliases should
> therefore be configured via quirks, such as the PCI fixup header quirk.
>
> —— `drivers/pci/pci.c:6491-6495`

本机没有任何别名 quirk 生效（`dmesg` 无 "DMA alias" 行），唯一的别名来源就是情形 ① 的桥。

#### 1.4.5 这对 VFIO 意味着什么

组是**所有权的最小单位**。`VFIO_GROUP_SET_CONTAINER` 会调 `iommu_group_claim_dma_owner()`，
组内只要还有设备绑在普通驱动上就直接拒绝：

```c
/* 来源: drivers/iommu/iommu.c:3214 —— iommu_group_claim_dma_owner() */
	if (group->owner_cnt) {
		ret = -EPERM;
		goto unlock_out;
	}
```

失败点是 `SET_CONTAINER` 而**不是** bind —— `vfio-pci` 声明了 `.driver_managed_dma = true`，
绑定阶段刻意不动 `owner_cnt`，所以绑定一定成功。详见
[corrections.md](corrections.md) 勘误 1（含 kprobe 实测的完整接管时序）。

要分清 ACS 的**能力位**与**控制位**：能力位是硬件属性，软件变不出来；控制位则是可写的，
而且**内核检测到 IOMMU 后默认就会全部打开**（`pci_request_acs()`，见 [1.5.2](#152-linux-不只读-acs它还会主动写)）。
上游内核确实有改控制位的参数（`pci=config_acs=` / `pci=disable_acs_redir=`），但两者都只会
**削弱**隔离，帮不上「组太大」的忙；而社区流传的 `pcie_acs_override=` **不在上游内核里**。
硬件不提供隔离时唯一正规解法仍是整组一起直通。
详见 [陷阱2](#陷阱2acs未启用) 与 [corrections.md](corrections.md) 勘误 2。

> 本节初版把 PCIe 桥的类型常量、group 101 的成因、以及 Root Port 的 ACS 校验条件都写错了，
> 修正过程见 [corrections.md](corrections.md) 勘误 5。

