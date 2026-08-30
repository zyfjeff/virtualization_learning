# 第五阶段：VFIO 设备直通

> 基于 Linux 6.12.93 内核源码 | 预计学习时间：2-3 周
>
> **前置依赖**:
> - 第三阶段（中断虚拟化 + VT-d IR）：直通设备的中断投递依赖 PI + IRTE 机制
> - 第四阶段（Virtio）：理解半虚拟化 I/O 与直通的区别和取舍

---

## 📋 学习目标

本阶段聚焦 **VFIO（Virtual Function I/O）** 框架和 KVM 设备直通机制。VFIO 是
Linux 中实现安全设备直通的标准框架，它允许虚拟机直接访问物理硬件设备，同时通过
IOMMU 保证隔离性。

完成本阶段后，你应该能够：
1. 理解 VFIO 的分层架构（设备驱动 → 核心框架 → IOMMU 驱动）
2. 掌握 DMA 映射的完整路径（设备 DMA → IOMMU 翻译 → 物理内存）
3. 理解 KVM-VFIO 桥接层如何将 VFIO 组与虚拟机关联
4. 使用工具调试设备直通问题

---

## 📂 本章文件

| 文件 | 内容 |
|------|------|
| `README.md` | 本文件：VFIO 架构全景 + IOMMU 组划分规则 + ACS 七个能力位与 ATS 机制 + MSI-X 表 mmap 安全与 Relocation + DMA 映射路径 + KVM-VFIO 桥接 + 常见陷阱 |
| `annotations.md` | 源码精读：VFIO 分层实现、container/group/device、Type1 IOMMU 后端 |
| `corrections.md` | ★ 勘误：DMA ownership 认领时机、ACS 认知误区、sysfs 观测盲区、IRTE Posted 化的真实触发路径（均有实测印证） |
| `practice/` | ★ 3 个实验程序：ownership 认领时机追踪 / IOVA→HPA 映射验证 / MSI-X 中断直通与 IRTE Posted 化 |

> 陷阱1、陷阱2、陷阱4 的表述已按 `corrections.md` 修正过，两者冲突时以 `corrections.md` 为准。

---

## 🏗️ VFIO 架构总览

### 1.1 为什么需要 VFIO？

```
┌──────────────────────────────────────────────────────────────────┐
│                  设备直通 vs 设备模拟                              │
│                                                                  │
│  设备模拟（如 virtio, emulated NIC）:                             │
│    Guest I/O → VM-Exit → QEMU 模拟 → 物理设备                    │
│    ✓ 安全性好   ✗ 性能差（每次 I/O 都触发 VM-Exit）              │
│                                                                  │
│  VFIO 设备直通:                                                  │
│    Guest I/O → 设备直接 DMA → 物理内存                            │
│    ✓ 性能接近裸机   ✓ 设备功能完整                                │
│    △ 需要 IOMMU 支持   △ 设备被独占（不能多 VM 共享）            │
│                                                                  │
│  安全性保证:                                                      │
│    IOMMU 确保设备只能访问 Guest 分配的内存区域                     │
│    设备无法 DMA 到其他 Guest 或宿主机的内存                        │
│    DMA 地址翻译由 IOMMU 硬件完成，对设备透明                      │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 VFIO 分层架构

```
┌──────────────────────────────────────────────────────────────────┐
│                    VFIO 分层架构                                  │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │                    用户态 (QEMU)                            │ │
│  │                                                             │ │
│  │   /dev/vfio/vfio ───── /dev/vfio/$GROUP_ID                 │ │
│  │   (容器控制)            (组设备访问)                          │ │
│  │                                                             │ │
│  │   ioctl:                                                    │ │
│  │     VFIO_GET_API_VERSION                                    │ │
│  │     VFIO_GROUP_SET_CONTAINER                                │ │
│  │     VFIO_GROUP_GET_DEVICE_FD                                │ │
│  │     VFIO_IOMMU_MAP_DMA                                     │ │
│  │     VFIO_DEVICE_RESET                                      │ │
│  └──────────────────┬──────────────────────────────────────────┘ │
│                     │ ioctl                                      │
│  ┌──────────────────▼──────────────────────────────────────────┐ │
│  │                VFIO 核心框架                                 │ │
│  │             (drivers/vfio/vfio_main.c)                      │ │
│  │                                                             │ │
│  │  ┌─────────────┐  ┌──────────────┐  ┌────────────────┐     │ │
│  │  │ VFIO 设备   │  │  VFIO 组     │  │  VFIO 容器     │     │ │
│  │  │ 管理        │  │  管理        │  │  管理          │     │ │
│  │  │             │  │              │  │                │     │ │
│  │  │ vfio_device │  │ vfio_group   │  │ vfio_container │     │ │
│  │  └──────┬──────┘  └──────┬───────┘  └───────┬────────┘     │ │
│  └─────────┼────────────────┼──────────────────┼───────────────┘ │
│            │                │                  │                 │
│  ┌─────────▼────────────────▼──────────────────▼───────────────┐ │
│  │              VFIO 设备驱动层                                  │ │
│  │           (drivers/vfio/pci/vfio_pci_core.c)                │ │
│  │                                                             │ │
│  │  提供设备特定的操作:                                          │ │
│  │    - open/release                                            │ │
│  │    - read/write (配置空间访问)                                │ │
│  │    - mmap (MMIO 区域映射)                                    │ │
│  │    - ioctl (设备特定操作)                                     │ │
│  │    - irq (中断管理: INTx, MSI, MSI-X)                       │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │              IOMMU 驱动层                                    │ │
│  │         (drivers/vfio/vfio_iommu_type1.c)                   │ │
│  │                                                             │ │
│  │  Type 1 IOMMU 驱动:                                         │ │
│  │    - DMA 映射/解映射                                         │ │
│  │    - IOMMU 域管理                                            │ │
│  │    - 页表管理                                                │ │
│  │    - 支持 Intel VT-d 和 AMD-Vi                               │ │
│  │                                                             │ │
│  │  ┌───────────────────────────────────────────────────────┐  │ │
│  │  │  IOMMU 核心 (drivers/iommu/)                          │  │ │
│  │  │  - iommu_domain 管理                                   │  │ │
│  │  │  - IOVA 分配                                           │  │ │
│  │  │  - 页表操作                                            │  │ │
│  │  │  - 硬件特定驱动 (intel/, amd/)                         │  │ │
│  │  └───────────────────────────────────────────────────────┘  │ │
│  └─────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 1.3 核心概念

| 概念 | 说明 |
|------|------|
| **VFIO 容器** | 最高层抽象，对应 `/dev/vfio/vfio`，管理 IOMMU 域 |
| **VFIO 组** | IOMMU 组的用户态接口，对应 `/dev/vfio/$GROUP_ID` |
| **VFIO 设备** | 具体设备的接口，通过组的 `DEVICE_FD` ioctl 获取 |
| **IOMMU 域** | 一组设备的 DMA 地址空间，由 IOMMU 硬件隔离 |
| **IOMMU 组** | IOMMU 拓扑中不可分割的设备集合 |
| **DMA 映射** | 将 Guest 物理地址映射到设备可见的 IOVA |
| **MSI-X** | 设备的中断机制，通过 MSIX 表配置 |

### 1.4 IOMMU 组是怎么划出来的

**组不是 VFIO 划的。** 划分发生在设备 probe 阶段的 IOMMU core 里，VFIO 只是消费既成结果 ——
启动时读 `/sys/kernel/iommu_groups/`，无法改变分组。

```c
/* 来源: drivers/iommu/iommu.c:427 —— __iommu_probe_device() */
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
**决定「跳到 `:3681` 返回 true」的是 header type 的 multifunction 位**（`:3671`，
由 `drivers/pci/probe.c:1940` 从配置空间 `0x0e` bit7 读出），**PCIe 类型决定的是另一件事**
—— 这一支到底读不读设备自己的 ACS 位。桥类即使在单功能下也照样 false，因为
`:3649-3651` 在读 `multifunction` 之前就 return 了。所以不能简写成「单功能 ⇒ true」。

第 2' 类为什么要读设备自己的位？因为**多功能设备内部的 func↔func 通路在 PCI 拓扑上是隐形的**
—— 内核从 bus/slot 根本看不见第二个口，只能靠设备在配置空间自己声明
（`PCI_ACS_DT`，"Direct Translated P2P"，`include/uapi/linux/pci_regs.h:997`）。
这正是 [1.4.4 ②](#144-三种真正会并组的情形) 归并同 slot function 的前提。
而单功能设备没有第二个口，问它 ACS 问不出任何信息 —— 它的横向转发决策点在上游的
Downstream Port，那里由 `:3657-3659` 单独查。`return true` 的含义是**「本跳不提供信息」**，
不是「本跳没有风险」；隔离性由 `pci_acs_path_enabled()`（`:3693`）逐跳问上去共同保证。

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

### 1.5 ACS 与 ATS：直通依赖的两个 PCIe 能力

1.4 反复用到 ACS，但只讲了它「怎么被读来判断分组」。这一节把两个能力本身讲清楚 ——
它们经常被混为一谈，其实是**正交的两件事**：

| | ACS | ATS |
|---|---|---|
| 装在哪 | 转发点（端口、多功能设备） | 端点自己 |
| 管什么 | **转发策略**：进来的报文能不能就地横穿、要不要校验来源 | **翻译归属**：设备能不能自己查地址并缓存结果 |
| 谁在意 | IOMMU core 分组、PASID 门控 | IOMMU 驱动、unmap 路径的性能 |
| 关掉的结果 | 隔离性变差（组变大） | 性能变差（每次 DMA 都过 IOMMU） |

> **规范来源说明**：本仓没有 PCIe Base Spec，以下涉及 spec 6.12（ACS）、10.5（ATS）
> 的表述均引自内核注释的转述并标注行号；ATS 的 IOMMU 侧行为以 `intel-vtd.pdf`
> **Rev 4.1** 为准（Chapter 4，Section 4.1~4.5）。

#### 1.5.1 ACS：七个能力位各管什么

寄存器布局（`include/uapi/linux/pci_regs.h:990-1000`）：

```
PCI_ACS_CAP		0x04	/* ACS Capability Register —— 我有哪些能力 */
PCI_ACS_EGRESS_BITS	0x05	/* Egress Control Vector 的位宽 */
PCI_ACS_CTRL		0x06	/* ACS Control Register —— 现在开着哪些 */
PCI_ACS_EGRESS_CTL_V	0x08	/* ACS Egress Control Vector —— 白名单位图 */
```

| 位 | 值 | 全称 | 作用 | Linux 哪里在用 |
|---|---|---|---|---|
| bit0 | `0x0001` | `SV` Source Validation | 校验进来的请求所声称的 bus number 是否真属于该下游，防止**伪造 RID** | 分组 `REQ_ACS_FLAGS` |
| bit1 | `0x0002` | `TB` Translation Blocking | **入口拒收 `AT≠00b` 的请求**，即不接受已翻译地址 | 不参与分组；`pci.c:1067` 仅对 external_facing / untrusted / noats 启用 |
| bit2 | `0x0004` | `RR` P2P Request Redirect | 请求不许就地横穿，重定向到上游 | 分组 + PASID |
| bit3 | `0x0008` | `CR` P2P Completion Redirect | 完成包同样重定向到上游 | 分组 |
| bit4 | `0x0010` | `UF` Upstream Forwarding | 强制上游转发 —— 保证报文**一定经过** IOMMU | 分组 + PASID |
| bit5 | `0x0020` | `EC` P2P Egress Control | 配合 Egress Vector 做**按对端**的细粒度白名单 | 不参与分组判定 |
| bit6 | `0x0040` | `DT` Direct Translated P2P | 已翻译地址的 p2p 是否允许直通 | 不参与分组判定 |

`RR` / `CR` / `UF` 三者合起来才是「把流量强制上提到 RC，交给 IOMMU 裁决」；
`SV` 管的是「别谎报自己是谁」；`TB` 管的是「别拿别人翻译好的地址回来」（1.5.4）。

**`EC` 是唯一连「能力位缺失」都要当失败的一位** —— 看 `pci_acs_flags_enabled()` 的掩码：

```c
/* 来源: drivers/pci/pci.c:3592-3598 —— pci_acs_flags_enabled() */
	/*
	 * Except for egress control, capabilities are either required
	 * or only required if controllable.  Features missing from the
	 * capability field can therefore be assumed as hard-wired enabled.
	 */
	pci_read_config_word(pdev, pos + PCI_ACS_CAP, &cap);
	acs_flags &= (cap | PCI_ACS_EC);
```

这段掩码的实际含义：**`ACSCap` 里是 0 的位被直接从要求集里剔掉**，只有 `EC` 例外 ——
只要调用方要求 `EC`，就必须真的在 `ACSCtl` 里读到它。也就是说 lspci 里 `ACSCap` 显示 `-`
**不等于判定失败**（1.4.2 的实测就是靠这条过下来的）。内核注释给出的理由是
「能力要么必需、要么只在可控时必需，能力位缺失可以按硬连线已启用处理」，
但**没有解释 `EC` 为什么被排除在这个规则之外**，此处不作推断。

哪些位算「不适用于多功能设备」，quirk 代码直接给了答案：

```c
/* 来源: drivers/pci/quirks.c:4702-4703 —— pci_quirk_amd_sb_acs() */
	/* Filter out flags not applicable to multifunction */
	acs_flags &= (PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC | PCI_ACS_DT);
```

`pci_quirk_mf_endpoint_acs()`（`quirks.c:4975`）说得更完整：

```c
/* 来源: drivers/pci/quirks.c:4977-4984 —— pci_quirk_mf_endpoint_acs() */
	/*
	 * SV, TB, and UF are not relevant to multifunction endpoints.
	 *
	 * Multifunction devices are only required to implement RR, CR, and DT
	 * in their ACS capability if they support peer-to-peer transactions.
	 * Devices matching this quirk have been verified by the vendor to not
	 * perform peer-to-peer with other functions, allowing us to mask out
	 * these bits as if they were unimplemented in the ACS capability.
	 */
```

**即多功能端点真正要求实现的只有 `RR` / `CR` / `DT`**，`SV` / `TB` / `UF` 与它无关。
注释里没写原因；按 PCIe 拓扑理解，多功能设备只有**一个**对外链路口，「校验来源」「上提」
这些动作发生在链路对端的 Downstream Port 上，设备内部 func↔func 的横向通路只能靠
`RR`/`CR` 拦。

对**没走 quirk 表**的标准多功能设备，这个「不适用」不必显式掩码：规范只要求它们实现
`RR`/`CR`/`DT`，所以设备**只要没在 `ACSCap` 里置起 `SV`/`UF`/`TB`**，上面那句
`acs_flags &= (cap | PCI_ACS_EC)` 就会自动把这几位从要求集里剔掉 —— 这正是
`pci_acs_enabled()` 内核注释里那句 "Automatically filters out flags that are not
implemented on multifunction devices"（`pci.c:3609-3610`）的实际机制。反过来说，
如果某个多功能设备自己在 `ACSCap` 里宣告了 `SV`，判定就会真的去 `ACSCtl` 里查它开没开。

#### 1.5.2 Linux 不只读 ACS，它还会主动写

多数人只把 ACS 当「只读的属性位」，其实内核在两个方向上都用它。

**写侧 —— 开机时默认全部打开。** 检测到 IOMMU 后：

```c
/* 来源: drivers/iommu/intel/dmar.c:933 */
		iommu_detected = 1;
		/* Make sure ACS will be enabled */
		pci_request_acs();
```

`pci_request_acs()`（`drivers/pci/pci.c:943`）置起 `pci_acs_enable`，之后每个设备 probe 时
`pci_acs_init()`（`:3717`）→ `pci_enable_acs()`（`:1075`）→ `pci_std_enable_acs()`（`:1052`）
把 `SV` / `RR` / `CR` / `UF` 全置上：

```c
/* 来源: drivers/pci/pci.c:1055 —— pci_std_enable_acs() */
	/* Source Validation */
	caps->ctrl |= (caps->cap & PCI_ACS_SV);
	/* P2P Request Redirect */
	caps->ctrl |= (caps->cap & PCI_ACS_RR);
	...
```

注意 `|= (caps->cap & ...)` 这个写法：**只能打开声明过的能力**，写不进硬件没有的位。

`pci_enable_acs()` 的完整控制流有个反直觉的地方，值得单独看一眼：

```c
/* 来源: drivers/pci/pci.c:1081-1096 —— pci_enable_acs() */
	/* If an iommu is present we start with kernel default caps */
	if (pci_acs_enable) {
		if (pci_dev_specific_enable_acs(dev))
			enable_acs = true;
	}

	pos = dev->acs_cap;
	if (!pos)
		return;
	...
	if (enable_acs)
		pci_std_enable_acs(dev, &caps);
```

- `pci_dev_specific_enable_acs()`（`quirks.c:5438`）**命中 quirk 时返回 0**（quirk 自己已经把等效
  能力写进去了，见 1.5.3），**没有 quirk 才返回 `-ENOTTY`**。而这里的判据是 `if (返回值)` ——
  非零即真，所以「没有专属 quirk」反而走 `pci_std_enable_acs()`，「有专属 quirk」反而跳过标准流程。
- `if (!pos) return;`：**没有 ACS capability 的设备根本到不了写操作**，
  标准路径对它们无事可做，只剩 `pci_dev_specific_enable_acs()` 那条 quirk 还有可能改到寄存器。
- 无论走哪条，`:1107` 都会把 `caps.ctrl` 写回配置空间（`disable_acs_redir=` / `config_acs=`
  的命令行覆盖在 `:1102-1105` 注入，且注释明说"even if there is no iommu"也照样生效）。

AMD、设备树、ARM SMMU 走的是同一个入口（`drivers/iommu/amd/init.c:3207`、
`drivers/iommu/of_iommu.c:146`、`drivers/acpi/arm64/iort.c:1899`、
`drivers/acpi/viot.c:265`）。

唯一有条件的一位是 `TB`：

```c
/* 来源: drivers/pci/pci.c:1067 */
	/* Enable Translation Blocking for external devices and noats */
	if (pci_ats_disabled() || dev->external_facing || dev->untrusted)
		caps->ctrl |= (caps->cap & PCI_ACS_TB);
```

只对**对外暴露的 / 不可信的 / 禁了 ATS 的**设备开 —— 因为 `TB` 会把 `AT≠00b` 的上行请求
（Translation Request 和 Translated Request 都算）当 ACS Violation 拦掉，内部可信设备之间
开了只会掉性能。

**恢复侧 —— 内核把 ACS 当作需要恢复的状态。** `pci_restore_state()`（`pci.c:1943`）在收尾时
专门再调一次 `pci_enable_acs(dev)`（`pci.c:1963`，上一行注释即 "Restore ACS and IOV
configuration state"）。注意它在 `pci_restore_config_space()`（`:1957`）**之后**执行：
先把保存过的原始值写回去，再按当前策略重设一遍 ACS Control。

**改侧 —— 存在上游支持的内核参数。** 这和「`pcie_acs_override=` 不存在」是两件不同的事，
别混（见 [corrections.md](corrections.md) 勘误 2）：

```
pci=disable_acs_redir=<pci_dev>[;...]     # 强关 RR/CR/EC，放行 switch 内 P2P
pci=config_acs=<ACS flags>@<pci_dev>[;...]  # 逐位强制开/关/保持不变
```

`config_acs` 的 flags 每一位取 `0`（强制关）/ `1`（强制开）/ `x`（不动），对应
`bit0 SV / bit1 TB / bit2 RR / bit3 CR / bit4 UF / bit5 EC / bit6 DT`
（`kernel-parameters.txt:4676-4687`）。**但字符串是从右往左写的** ——
`__pci_config_acs()` 从 `@` 前一个字符开始倒着走、`shift` 递增（`pci.c:976-997`），
所以最右边那位才是 bit0。文档的例子正好印证：

> `pci=config_acs=10x` — enable P2P Request Redirect, disable Translation Blocking,
> and leave Source Validation unchanged from whatever power-up or firmware set it to.
>
> —— `kernel-parameters.txt:4689-4694`

`10x` 从右读：`x`→bit0 SV 不动、`0`→bit1 TB 关、`1`→bit2 RR 开。按字面顺序读会全错。

两个条目各自都带了代价警告，措辞不一样：`disable_acs_redir` 是 "this **removes** isolation
between devices and may put more devices in an IOMMU group."
（`kernel-parameters.txt:4662-4665`），`config_acs` 是 "this **may remove** isolation
between devices and may put more devices in an IOMMU group."（`:4696-4697`）。
代码侧的落地位置是 `pci.c:1102-1105`：`disable_acs_redir` 固定掩
`RR|CR|EC` 三位，`config_acs` 原样透传给 `__pci_config_acs()`。

#### 1.5.3 端点设备上的 ACS：存在吗，用来干什么

先给规范结论：**只有多功能设备才需要实现，单功能设备不该实现**
（spec 6.12.1.3，`pci.c:3677-3679` 转述：no ACS capabilities are applicable to single
function devices **with the exception of downstream ports**）。
所以「端点带 ACS capability」基本等价于「这是个多功能设备」。

**实测印证**：本机 350 个 PCI 设备（其中 **187 个是 PCIe 设备**，另外 163 个连 Express
capability 都没有）里 **66 个**实现了 ACS capability，按类型逐台核对的结果是
Root Port 16 个 + Downstream Port 50 个，**端点 0 个** —— 3 个 Endpoint、3 个 Upstream Port
（含 switch 的 `48:00.0`）、112 个 RCiEP 全都没有。核查命令见 1.5.8。

那么端点上真实现了的话，它管的是什么？看内核自己收的"作业"最准 ——
`pci_dev_acs_enabled[]`（`quirks.c:5056-5221`，除结尾的 `{ 0 }` 外共 **132 条**）这张
白名单的构成说明了全部现实场景，五行加起来正好覆盖全部 132 条：

| 设备类别 | quirk | 内核注释给出的理由 |
|---|---|---|
| **多功能网卡**（Solarflare SFxxx、Intel 82575/82576/82580/I350/I219…） | `pci_quirk_mf_endpoint_acs`（`quirks.c:4975`）——**67 条，超过全表一半** | 多功能设备只需实现 `RR`/`CR`/`DT`；厂商确认不做 func 间 p2p，按未实现处理 |
| Wangxun 1G/10G/25G/40G 多功能网卡 | `pci_quirk_wangxun_nic_acs`（`quirks.c:5038`，1 条 `PCI_ANY_ID`） | `:5030-5034`：「硬件把所有 p2p 流量都导向上游，效果等同于 `RR`+`CR` 被置起」 |
| AMD 南桥（FCH）根总线上的多功能设备 | `pci_quirk_amd_sb_acs`（`quirks.c:4685`，8 条，另有「非多功能或非根总线即不适用」的门控） | 门控在 `:4692`；先剔掉对多功能不适用的位（`:4702`），实际提供集只有 `RR` 与 `CR`（`:4705`） |
| 不通告 ACS 的 **Root Port / PCIe 端口**（Broadcom iProc PAXB、Loongson、NXP、X-Gene、QCOM、Cavium、Zhaoxin、Intel PCH） | `quirks.c:5005` / `:5017` / `:4856` / `:4834` 等，合计 55 条（`zhaoxin` 那条连 Downstream Port 也算，`quirks.c:4766-4767`） | 硬件不提供 Root Port 之间的 p2p，掩掉 `SV`/`RR`/`CR`/`UF` |
| Intel **RCiEP** | `pci_quirk_rciep_acs`（`quirks.c:4991`，1 条匹配全部 Intel 设备） | 依据是 VT-d 规范而非 PCIe ACS（见 1.4.2） |

**一句话概括端点侧 ACS 的用途**：它管的永远是**同一个设备内部、function 之间**能否横向
转发 p2p 请求与完成包（`RR`/`CR`，加上专门管已翻译地址的 `DT`）。这也是为什么
`SV`/`TB`/`UF` 对端点不适用 —— 那三位描述的是「一个端口如何处理从别的端口来的报文」，
端点只有一个对外链路，没有「别的端口」。

**现实是端点侧基本没人实现它**（本机实测见 1.5.3 开头）。所以内核的实际策略是
**不指望端点自报**，改用 quirk 表按 `vendor:device` 逐个放行。
代价是这套判断是**静态的、按型号的**：同一颗芯片换个固件或换个 p2p 配置，白名单不会知道。
`pci_dev_specific_acs_enabled()` 的函数头注释直说了这层的定位：

```c
/* 来源: drivers/pci/quirks.c:5240-5243 —— pci_dev_specific_acs_enabled() */
	 * Allow devices that do not expose standard PCIe ACS capabilities
	 * or control to indicate their support here.  Multi-function express
	 * devices which do not allow internal peer-to-peer between functions,
	 * but do not implement PCIe ACS may wish to return true here.
```

更极端的是 Intel PCH root port：**硬件根本没有 ACS capability**，内核用
`pci_quirk_enable_intel_pch_acs()`（`quirks.c:5350`）直接改 LPC/MPC 寄存器把等效能力关掉
peer 转发，再打上 `PCI_DEV_FLAGS_ACS_ENABLED_QUIRK`（`:5362`）并印
"Intel PCH root port ACS workaround enabled"（`:5364`）。这个 flag 唯一的消费者就是
`pci_quirk_intel_pch_acs()`（`quirks.c:4834-4843`）：**没有它一律判 false**，有它才把
`SV|RR|CR|UF` 当作已提供。配套的 `pci_acs_init()` 注释也解释了为什么没有 capability 还要试：

```c
/* 来源: drivers/pci/pci.c:3721-3725 —— pci_acs_init() 注释 */
	 * Attempt to enable ACS regardless of capability because some Root
	 * Ports (e.g. those quirked with *_intel_pch_acs_*) do not have
	 * the standard ACS capability but still support ACS via those
	 * quirks.
```

#### 1.5.4 ATS：让设备自己去查地址

ATS 完全不是转发策略，它让**端点自己**向 IOMMU 请求地址翻译并缓存在设备内部的
Device-TLB 里，之后直接用翻译好的物理地址发请求。

`intel-vtd.pdf` Rev 4.1, Section 4.1 (Device-TLB Operation) 给出事务头里的 `AT` 字段编码：

> The AT field indicates if transaction is a memory request with 'Untranslated' address
> (**AT=00b**), 'Translation Request' (**AT=01b**), or memory request with 'Translated'
> address (**AT=10b**).

流程（Section 4.1.1 / 4.1.2 / 4.1.3）：

1. 设备发 `AT=01b` 的 Translation Request；
2. IOMMU 按**该设备自己的 RID** 查上下文表 + 页表，返回 Translation Completion，
   带读/写权限位和 **`U`（Untranslated access only）位**；
3. 设备把结果存进 Device-TLB，之后发 `AT=10b` 的 Translated Request，**直接携带物理地址**。

**关键安全含义**：`AT=10b` 的请求**不再经过地址翻译**。Section 4.2.4 (Handling of
Translated Requests) 的原文是：

> If none of the error conditions above are detected, remapping hardware **bypasses address
> translation and sets the output address for the translated-request equal to the input
> address**.

硬件仍然做的检查只有三类：带 PASID 前缀的 Translated 请求被 root complex 拦下、打到
中断地址区间 `FEEx_xxxxh` 的被当作 fault（LGN.4/SGN.8）、以及其他 §7.1.3 列举的阻断条件。
除了这些，它就是一次**物理地址直写**。所以两件事必须成立，否则 ATS 就是后门：

- 翻译结果**一定由 IOMMU 给出**（按 RID 授权），设备不能自己编；
- 一旦 IOMMU 侧改了映射，设备缓存的旧翻译**必须被强制作废**。

第一条由 RID 查表保证（Section 4.2.3 明确 "The requester-id in the translation-request is
used to parse the respective legacy root/context entry … as described in Section 3.4"）。
第二条由 Device-TLB invalidation 保证，也就是 1.5.5 的全部内容。

而 `TB` 是**第三种立场**：干脆不允许 `AT≠00b` 的请求进门。VT-d spec Section 4.2.2
(Root-Port Control of ATS Address Types) 说得很直白：

> Root-ports supporting Access Control Services (ACS) capability can support 'Translation
> Blocking' control to block upstream memory requests with non-zero value in the AT field.
> When enabled, such requests are reported as ACS violation ... **blocked at the root-port
> as error and are not presented to remapping hardware.**
>
> —— intel-vtd.pdf Rev 4.1, Section 4.2.2 (Root-Port Control of ATS Address Types)

Linux 默认**不**用这个立场：`pci_std_enable_acs()` 只在 `pci_ats_disabled()`（`pci=noats`）、
`dev->external_facing` 或 `dev->untrusted` 三者之一成立时才写 `TB`（`pci.c:1066-1068`）。
这正是 1.5.8 ①② 里那个内部 Downstream Port（`49:01.0`）呈现 `ACSCap: TransBlk+`
而 `ACSCtl: TransBlk-` 的原因 —— 能力在，内核判断不需要开。

#### 1.5.5 失效与代价：直通场景最容易被低估的一环

软件通过 invalidation queue 下发 Device-TLB Invalidation descriptor（Section 6.5.2.5），
配对工作由硬件完成：硬件分配空闲 **ITag** 标识每条下发到端点的失效请求，没有空闲 ITag
时该请求会被**推迟**，同时为每个 ITag 启动失效完成定时器（Section 4.3）。Linux 的实现：

```c
/* 来源: drivers/iommu/intel/cache.c:390 —— cache_tag_flush_devtlb_psi() */
	info = dev_iommu_priv_get(tag->dev);
	sid = PCI_DEVID(info->bus, info->devfn);

	if (tag->pasid == IOMMU_NO_PASID) {
		qi_batch_add_dev_iotlb(iommu, sid, info->pfsid, info->ats_qdep,
				       addr, mask, domain->qi_batch);
```

**这对 VFIO 意味着什么**：设备带着自己的 TLB，`VFIO_IOMMU_UNMAP_DMA` 就不能只改页表了事 ——
必须**同步等到设备侧缓存失效完成**才能返回，否则 guest 能拿旧翻译访问已释放的宿主页。
这就是直通设备上 `unmap` 明显比 `map` 慢、且延迟取决于设备响应时间的根因。
`ats_qdep`（设备的 Invalidate Queue Depth）就是这条路径上的窗口大小。

嵌套翻译下更粗暴 —— 改了 stage-2 无法反查哪些嵌套条目受影响，只能全刷：

```c
/* 来源: drivers/iommu/intel/cache.c:458-468 */
		case CACHE_TAG_NESTING_DEVTLB:
			/*
			 * Address translation cache in device side caches the
			 * result of nested translation. There is no easy way
			 * to identify the exact set of nested translations
			 * affected by a change in S2. So just flush the entire
			 * device cache.
			 */
			addr = 0;
			mask = MAX_AGAW_PFN_WIDTH;
```

#### 1.5.6 Linux 侧的 ATS 配置与启用门控

寄存器（`include/uapi/linux/pci_regs.h:914-921`）：

```
PCI_ATS_CAP	0x04	  /* bit5 Page Aligned Request(0x0020)；bits4:0 Invalidate Queue Depth */
PCI_ATS_CTRL	0x06	  /* bit15 Enable(0x8000)；bits4:0 Smallest Translation Unit (STU) */
```

`STU`（Smallest Translation Unit）**不是缓存粒度**，而是「这个 function 能发起翻译请求
和失效请求的最小自然对齐地址块」。寄存器里存的是位移量之差：

```c
/* 来源: drivers/pci/ats.c:115 —— pci_enable_ats() */
		ctrl |= PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU);
```

`PCI_ATS_MIN_STU` = 12（`pci_regs.h:921`，注释 "shift of minimum STU block"），
所以字段值 `n` 表示最小单元 `2^(12+n)` 字节，`n=0` 即 4KB。Intel IOMMU 的启用条件是三合一：

```c
/* 来源: drivers/iommu/intel/iommu.c:1295-1297 —— iommu_enable_pci_caps()（定义在 :1287） */
	if (info->ats_supported && pci_ats_page_aligned(pdev) &&
	    !pci_enable_ats(pdev, VTD_PAGE_SHIFT))
		info->ats_enabled = 1;
```

- `ats_supported`：设备有 ATS capability、**且不是不可信设备**（`ats.c:41-47` 只查
  `ats_cap` 与 `untrusted`，没有 ACS 检查），且 IOMMU 支持 Device-TLB
  （`intel/iommu.c:3924-3928`，还要 `ecap_dev_iotlb_support()` 与 `dmar_ats_supported()`）；
- `pci_ats_page_aligned()`：设备声称产生的非翻译地址总是 4K 对齐（`ats.c:193`），
  否则不给开 —— 这对应 PCIe spec r4.0 sec 10.5.1.2（`ats.c:189` 注释转述）；
- `pci_enable_ats(pdev, VTD_PAGE_SHIFT)`：传入 `ps = 12` → `STU` 字段写 0，即最小单元 4KB。

**VF 的 ATS 依附于 PF**：VF 不写 `STU` 字段，只校验传入值是否与 PF 已有的一致，
不一致就直接 `-EINVAL`（所以 VF 的 ATS 一定在 PF 之后、以相同 `ps` 开启）：

```c
/* 来源: drivers/pci/ats.c:104-116 —— pci_enable_ats() */
	/*
	 * Note that enabling ATS on a VF fails unless it's already enabled
	 * with the same STU on the PF.
	 */
	ctrl = PCI_ATS_CTRL_ENABLE;
	if (dev->is_virtfn) {
		pdev = pci_physfn(dev);
		if (pdev->ats_stu != ps)
			return -EINVAL;
	} else {
		dev->ats_stu = ps;
		ctrl |= PCI_ATS_CTRL_STU(dev->ats_stu - PCI_ATS_MIN_STU);
	}
```

失效队列深度也有个坑：**寄存器字段值 `0`** 和 **函数返回值 `0`** 不是一回事。
`pci_ats_queue_depth()` 的注释解释了字段的语义，实现把它换算成了「真实值」约定：

```c
/* 来源: drivers/pci/ats.c:162-166 —— 注释 */
 * The ATS spec uses 0 in the Invalidate Queue Depth field to
 * indicate that the function can accept 32 Invalidate Request.
 * But here we use the `real' values (i.e. 1~32) for the Queue
 * Depth; and 0 indicates the function shares the Queue with
 * other functions (doesn't exclusively own a Queue).

/* 来源: drivers/pci/ats.c:168-180 —— 实现 */
int pci_ats_queue_depth(struct pci_dev *dev)
{
	...
	if (dev->is_virtfn)
		return 0;
	pci_read_config_word(dev, dev->ats_cap + PCI_ATS_CAP, &cap);
	return PCI_ATS_CAP_QDEP(cap) ? PCI_ATS_CAP_QDEP(cap) : PCI_ATS_MAX_QDEP;
}
```

- 字段 `0` 的 PF → 返回 `PCI_ATS_MAX_QDEP` = 32（`pci_regs.h:916`）；
- **VF 一律返回 `0`**，即注释所说的「不独占失效队列」（`ats.c:175-176` 直接短路，不读寄存器）；
- 这个返回值在 `intel/iommu.c:3940` 存进 `info->ats_qdep`，1.5.5 的
  `qi_batch_add_dev_iotlb()` 原样带上，最后由 `qi_desc_dev_iotlb()` 编码成描述符
  bits 20:16 的 5 位字段：

```c
/* 来源: drivers/iommu/intel/iommu.h:1115-1118 */
	if (qdep >= QI_DEV_IOTLB_MAX_INVS)
		qdep = 0;

	desc->qw0 = QI_DEV_IOTLB_SID(sid) | QI_DEV_IOTLB_QDEP(qdep) |
```

`QI_DEV_IOTLB_MAX_INVS` = 32（`iommu.h:426`），所以上面那条「32」又被折回 `0` 写进
描述符 —— 硬件看到的仍然是 ATS spec 的编码。**换句话说 `pci_ats_queue_depth()`
的返回值只是内核内部的约定，不下硬件**；VF 与「字段为 0 的 PF」在描述符里都是 `0`。

**启用/关闭有推荐的先后顺序**，因为设备侧 `E` 位和 IOMMU 侧 context entry 是两个
独立的位，中间存在不一致窗口（Section 4.5）。关闭时序（Section 4.5.2）：

> 1. quiesce DMA；2. 清 ATS Control 的 Enable 位；3. 下发 global Device-TLB Invalidate
> + Invalidation Wait，**作废缓存并把此前的 ATS 流量排空到全局可观测点**；
> 4. 最后才改 context entry 阻止 ATS 请求。

**推荐顺序只是建议，Linux 的实现并不照抄**。整节标题是 "Guidance to Software on
Enabling and Disabling ATS"，两个小节都是 "Recommended Software Sequence"，四条步骤
全部用 "should"（`intel-vtd.pdf` Section 4.5 / 4.5.1 / 4.5.2）；开头一句是
"It is **strongly recommended** that software quiesce DMA operations from the device
before programming the required bits."，而 Linux 侧
`device_block_translation()`（`iommu.c:3394`）的真实顺序是：

| 步骤 | 规范 §4.5.2 | Linux 6.12.93 实际 |
|---|---|---|
| 1 quiesce DMA | 由软件负责 | **不在这个函数里**，`device_block_translation()` 只做 2/3/4 |
| 2 清 `E` 位 | 第 2 步 | `iommu_disable_pci_caps()`（`:3407` → 定义 `:1300`）→ `pci_disable_ats()` |
| 4 改 context entry | **第 4 步** | `domain_context_clear()`（`:3413`）—— **排在第 3 步之前** |
| 3 global DevTLB Invalidate + Wait | 第 3 步 | 在 `intel_context_flush_present()` 末尾的 `__context_flush_dev_iotlb()`（`pasid.c:971` → 定义 `:885`，实际下发 `qi_flush_dev_iotlb()` @ `:898`）|

也就是说，Linux 把「先失效设备缓存、再关 context」倒成了「先关 context，再走一遍
统一的 context 失效路径」。跟在这个倒置后面的是一个更要命的细节：
`__context_flush_dev_iotlb()` 开头有道门控 —— `if (!info->ats_enabled) return;`
（`pasid.c:887-888`），而 `info->ats_enabled` 在第 2 步里已经被
`iommu_disable_pci_caps()` 清成了 0（`iommu.c:1311`）。**这条 Device-TLB 失效在
detach 路径上是否会真的下发，取决于走到这里时那个标志的状态；源码注释没有解释这一层，
本节也不推断。** 想确认的话，只能在 `qi_flush_dev_iotlb()` 上挂 kprobe 实测。

规范侧唯一能确定的是措辞：整节标题是 "Guidance to Software…"、两个小节是
"Recommended Software Sequence"，四条步骤全部用 "should"（`vtd-spec.txt:4008-4029`），
不是强制要求。

#### 1.5.7 ACS 与 ATS 的交汇点

两者在分组上是**叠加**的，不是替代：

| 判定 | 要求 | 位置 |
|---|---|---|
| IOMMU 分组 | 整条路径 `REQ_ACS_FLAGS`（SV+RR+CR+UF） | `iommu.c:1547` |
| 开 PASID | 整条路径 `RR + UF` | `ats.c:419` |
| 用 ATS 本身 | **不查 ACS**，只看 `untrusted` | `ats.c:41` |

开 PASID 为什么要额外卡 ACS？因为 PASID 场景下设备会发 **translated** 请求，
中途任何 switch 若允许就地横穿，设备就能拿着缓存的翻译访问别的设备 ——
所以 `RR`（不许横穿）+ `UF`（必须上提）是硬性前提。

**一句话**：`ATS` 让设备绕过 IOMMU 的**翻译开销**，`ACS` 保证没人绕过 IOMMU 的**授权**。
前者是性能特性，后者是安全特性；直通时你必然需要后者，前者则要看设备与拓扑是否支持、
以及能否接受 1.5.5 那条 unmap 同步代价。

#### 1.5.8 观测

以下每条都在本机执行过，注释里是真实输出（宿主内核 6.8.0-51-generic，涉及的代码路径与
6.12.93 一致，见 [corrections.md](corrections.md) 开头说明）。

```bash
# ① ACS：能力 vs 实际启用，两行必须对照看
$ lspci -vv -s 49:01.0 | grep -E "ACSCap|ACSCtl"
        ACSCap: SrcValid+ TransBlk+ ReqRedir+ CmpltRedir+ UpstreamFwd+ EgressCtrl- DirectTrans-
        ACSCtl: SrcValid+ TransBlk- ReqRedir+ CmpltRedir+ UpstreamFwd+ EgressCtrl- DirectTrans-

# ② 原始值：这台机器上该设备的 ACS 扩展能力位于 0x100，Cap @ +0x04、Ctl @ +0x06
$ setpci -s 49:01.0 0x104.w 0x106.w
001f
001d
```

`0x1f` = `SV+TB+RR+CR+UF`，`0x1d` 与它只差 `0x02`（`TB`）—— 正好对应
`pci_std_enable_acs()` 只在 external_facing / untrusted / noats 时才写 `TB`（1.5.2）。

```bash
# ③ ATS：先确认整机到底有没有设备实现
$ lspci -vv | grep -c "Address Translation services"
0
```

**本机 350 个 PCI 设备（187 个 PCIe）里一个 ATS-capable 都没有** —— 全是 Root Port、
Downstream Port、RCiEP 和普通端点。
所以 1.5.5 那条 Device-TLB 失效代价在本机测不到，要观察得换 SR-IOV 网卡或支持 ATS 的 NVMe；
有 ATS 的设备上，`lspci -vv` 会多出一段 `Address Translation services`（含 Invalidate Queue
Depth 与 STU），`dmesg` 里 Intel IOMMU 也会印出对应的 `DMAR` 支持信息。

```bash
# ④ 找「被 quirk 判成隔离」的多功能设备
#    必要条件：同一 slot 有多个 function + 整个 slot 都没有 ACS capability + 却被拆成 N 个组
for slot in $(lspci -Dn | cut -d' ' -f1 | sed 's/\.[0-9a-f]$//' | sort | uniq -d); do
    funs=$(ls -d /sys/bus/pci/devices/${slot}.* | wc -l)
    nacs=$(for f in /sys/bus/pci/devices/${slot}.*; do
               lspci -vv -s "${f##*/}" | grep -q "Access Control Services" && echo x
           done | wc -l)
    grps=$(for f in /sys/bus/pci/devices/${slot}.*; do
               basename "$(readlink -f "$f/iommu_group")"
           done | sort -u | wc -l)
    if [ "$nacs" -eq 0 ] && [ "$grps" -eq "$funs" ]; then
        echo "${slot#0000:}: $funs functions -> $grps groups"
    fi
done
```

本机命中 **26 个 slot、106 个 function**，其中 104 个是 `Sky Lake-E` 系列的 RCiEP
（`pci_quirk_rciep_acs`，`quirks.c:4991`），另外 2 个（`00:08.1`、`80:08.1`）连 PCIe
capability 都没有 —— 它们靠的不是 quirk，而是「同 slot 的搭档被 quirk 判成隔离、
没人肯跟它并组」，机制见 [1.4.2](#142-关键坑pci_acs_enabled-查的是有效能力不是实际能力)。

```bash
# ⑤ 按 PCIe 类型统计「谁真的有 ACS capability」（1.5.3 那句实测结论的来源）
$ lspci -Dvvv | awk '
      /^[0-9a-f]{4}:/{ if(dev!=""){printf "%-34s acs=%d\n", typ, acs} dev=$1; typ="no-Express"; acs=0 }
      /Capabilities.*Express .v/ { if (typ=="no-Express") { typ=$0; sub(/.*Express .v[1-9]. /,"",typ); sub(/,.*/,"",typ) } }
      /ACSCap/ { acs=1 }
      END{ if(dev!="") printf "%-34s acs=%d\n", typ, acs }' | sort | uniq -c | sort -rn
    163 no-Express                         acs=0
    112 Root Complex Integrated Endpoint   acs=0
     50 Downstream Port (Slot+)            acs=1
     15 Root Port (Slot+)                  acs=1
      3 Upstream Port                      acs=0
      3 Endpoint                           acs=0
      1 Root Port (Slot-)                  acs=1
      1 Root Port (Slot-)                  acs=0
      1 Root Port (Slot+)                  acs=0
      1 PCI-Express to PCI/PCI-X Bridge    acs=0
```

`acs=1` 合计 **66** 个，全部落在 Root Port / Downstream Port 上；112 个 RCiEP、
3 个 Upstream Port、3 个 Endpoint 一个都没有。

> **判据为什么必须写成"和同 slot 的搭档被拆开"**，而不是"独占一组 + 没有 ACS capability"？
> 后者会让两类设备误入：
>
> - **Root Port / Downstream Port** —— 分组时判定的是它的**上游那一跳**，设备自己的 ACS
>   从不参与对**它自己**的判定（`iommu.c:1397` 传 `pdev`、`:1547` 传 `bus->self`），
>   所以桥独占一组与它有没有 capability 无关；
> - **找不到同 slot 搭档的多功能设备** —— 自己判 false、门是过的，但没人可并，
>   仍然拿到新组。

> ①④⑤ 依赖扩展能力列表，**必须 root**：普通用户下 `lspci -vv` 一条 `ACSCap` 都读不到
> （本机实测 0 行 vs root 下 66 行），会得出「整机没有 ACS」的错误结论。

`ACSCap` 与 `ACSCtl` 的差别是 ①② 的全部信息量：**Cap 里是 `-` 的位不算失败**
（会被 `pci.c:3598` 掩掉），只有「Cap 有、Ctl 没开」才是真正的隔离缺失。

#### 1.5.9 MSI-X 表与 BAR mmap：为什么 VFIO 要剥掉那一页

VFIO 在 mmap BAR 时会把 MSI-X 表所在的页面从映射中剔除。这不是 bug，是安全设计。

##### MSI-X 表为什么不能暴露给 guest

MSI-X 表驻留在设备的某个 BAR 中，每个条目 16 字节，核心字段是 **Message Address**——
它决定了设备中断写到哪个地址：

```
MSI-X Table Entry (16 bytes):
┌───────────────────────────────────────────────┐
│  Message Address (8 bytes)  │  中断投递目标地址 │ ← 关键字段
├─────────────────────────────┼─────────────────┤
│  Message Data  (4 bytes)    │  中断向量/数据    │
├─────────────────────────────┼─────────────────┤
│  Vector Control (4 bytes)   │  掩码位          │
└─────────────────────────────┴─────────────────┘
```

如果 guest 能直接写 Message Address，就能把中断投递到**宿主机任意地址**：

```
正常:  Message Address = 0xFEE0_0000  → 本机 LAPIC，正常中断
恶意:  Message Address = 任意物理地址  → 覆盖宿主页内存 / 攻击其他 VM
```

所以 **没有中断重映射（IR）保护时，绝不能让 guest 直接写 MSI-X 表**。

##### IR 如何从根本上消除 MSI-X 表的安全风险

IR 启用后，VFIO 在分配中断时**改写设备 MSI-X 表的 Message Address/Data**：

```c
/* 来源: drivers/iommu/intel/irq_remapping.c (简化) */
msg.address = IR_BASE_ADDR + index;   /* 指向 IOMMU 的 IR 表，不是 APIC */
msg.data    = index;                   /* IRTE 索引 */
```

**设备 MSI-X 表里的 Message Address/Data 不再决定中断投递目标——IOMMU 才决定。**

```
无 IR:  Message Address = 0xFEE0_0000  → 直接指向 LAPIC，IOMMU 不介入
有 IR:  Message Address = IR_BASE+idx  → 指向 IOMMU，被 IOMMU 拦截并重映射
```

IOMMU 拦截 MSI TLP 后的完整处理流程：

```
MSI TLP 到达 IOMMU
    │
    ├─ ① 提取 Requester ID (RID) —— 从 PCIe TLP 头取设备 BDF
    │
    ├─ ② 用 Data 中的 index 查 IRTE
    │    IRTE[index] = {
    │      SID  = 0x4b00,      ← 该中断授权的设备 BDF
    │      SVT  = 1,           ← 严格验证 SID
    │      Vector = 0x21,      ← 真正投递的向量号（host 决定）
    │      DST  = APIC_ID_3,   ← 真正投递的目标（host 决定）
    │    }
    │
    ├─ ③ Source ID 验证（关键安全门控）
    │    IRTE 的 SVT 字段决定验证精度：
    │      SVT=0 (NO_VERIFY):      跳过检查
    │      SVT=1 (VERIFY_SID_SQ):  RID 与 SID 比较（SQ 控制精度）
    │      SVT=2 (VERIFY_BUS):     只比较 Bus 号（桥后设备）
    │    SQ 字段进一步控制比较粒度：
    │      SQ=0: Bus+Dev+Fn 全匹配   SQ=2: Bus+Dev（忽略 Fn）
    │      SQ=1: 高 15 位匹配         SQ=3: 仅 Bus
    │    Linux 默认用 SVT=1。RID 不匹配 → 阻断并报告 fault。
    │
    ├─ ④ 重映射：原始 MSI 的 Address/Data 被完全丢弃
    │    中断的向量、目标、投递模式全部来自 IRTE（host 写入）
    │    Remapped 模式 (IM=0): Vector←IRTE.V, DST←IRTE.DST
    │    Posted 模式 (IM=1): 直接写 PI Descriptor（PDA→vCPU）
    │
    └─ ⑤ 投递到 IRTE 指定的目标
```

**三层安全保证**：

| 层 | 机制 | 防护效果 |
|----|------|---------|
| ① | Message Address 指向 IOMMU，不是 APIC | guest 改 Address → 不再是有效 IR 请求 → 被阻断 |
| ② | Source ID 验证 (SVT/SID/SQ) | 设备 A 无法冒充设备 B 触发 B 的 IRTE |
| ③ | 目标重映射由 Host 控制 | IRTE 中的 Vector/DST 由 host 写入，guest 无法修改投递目标 |

Guest 即使恶意篡改 MSI-X 表也无效：

```
Guest 改 MSI-X: Message Address = 0x0001_0000, Data = 0xDEAD
→ 该地址不在 IR 范围内 → IOMMU 不当作 IR 请求 → 阻断为 fault
→ 目标内存不受影响

Guest 改 MSI-X: Message Address = IR_BASE + other_index
→ 即使 index 碰巧合法 → RID 必须匹配 IRTE.SID
→ 设备拿不到其他设备的 IRTE 索引 → 仍然被阻断
```

> **详见 phase3-interrupts/posted-interrupts.md §2** 的 IRTE 格式详解和 SVT/SID/SQ 字段定义。

##### 内核侧：有 IR 和没有 IR 的区别

内核通过 `VFIO_REGION_INFO_CAP_MSIX_MAPPABLE` 能力标记告知用户态是否允许 mmap MSI-X 区域：

```c
/* include/uapi/linux/vfio.h */
/*
 * The MSIX mappable capability informs that MSIX data of a BAR can be mmapped
 * which allows direct access to non-MSIX registers which happened to be within
 * the same system page.
 *
 * Even though the userspace gets direct access to the MSIX data, the existing
 * VFIO_DEVICE_SET_IRQS interface must still be used for MSIX configuration.
 */
#define VFIO_REGION_INFO_CAP_MSIX_MAPPABLE	3
```

| 场景 | 内核行为 |
|------|---------|
| **无 IR** | 剥掉 MSI-X 页，返回 sparse mmap；无 `MSIX_MAPPABLE` 标记 |
| **有 IR** | 整 BAR 可 mmap，带 `MSIX_MAPPABLE` 标记；IR 保证设备只能向授权的 APIC 地址发中断 |

没有 IR 时，BAR 的 mmap 被切成 MSI-X 表两侧的碎片：

```
BAR 布局（无 IR）:
┌─────────────┬──────────────┬──────────────┐
│  设备寄存器  │  MSI-X Table │  设备寄存器   │
│  (mmap 区域0)│  (剥掉!)     │  (mmap 区域1) │
└─────────────┴──────────────┴──────────────┘
```

##### QEMU 侧：有 IR 时仍然模拟 MSI-X 表

即使内核允许整 BAR mmap（有 IR + `MSIX_MAPPABLE`），QEMU **默认仍然拦截 MSI-X 表的访问**。
原因在于 QEMU 的内存区域层级设计：

```c
/* hw/pci/msix.c:383-385 */
memory_region_init_io(&dev->msix_table_mmio, OBJECT(dev),
                      &msix_table_mmio_ops, dev, "msix-table", table_size);
memory_region_add_subregion(table_bar, table_offset, &dev->msix_table_mmio);
```

`msix_table_mmio` 作为 BAR MemoryRegion 的 **subregion**，访问优先级高于底层的 mmap。
所以即使整 BAR 都 mmap 了，guest 访问 MSI-X 表区域仍命中模拟 handler：

```
Guest 访问 BAR
    │
    ├─ 访问 MSI-X 表区域 → msix_table_mmio (QEMU 模拟)
    │    └─ 写 dev->msix_table 软件副本 → 触发 vfio_msix_vector_do_use()
    │         └─ VFIO_DEVICE_SET_IRQS → 建立中断路由 (eventfd → KVM irq route → vCPU)
    │
    └─ 访问 BAR 其余区域 → mmap 直接到硬件
```

**QEMU 必须拦截 MSI-X 表写操作**，因为它要从中提取 vector 配置信息来建立中断路由。
没有这个拦截，QEMU 不知道 guest 配了哪些 vector，也就无法调用 `VFIO_DEVICE_SET_IRQS`，
中断到不了 guest。

`vfio_pci_fixup_msix_region()` 的逻辑（`hw/vfio/pci.c:1530-1605`）：

```c
/* 有 MSIX_MAPPABLE 标记 → 不切分，整 BAR 保持一个 mmap */
if (vfio_device_has_region_cap(&vdev->vbasedev, region->nr,
                               VFIO_REGION_INFO_CAP_MSIX_MAPPABLE)) {
    return;  /* 但 msix_table_mmio subregion 仍然会拦截 MSI-X 表区域 */
}

/* 无标记 → 切分 mmap，把 MSI-X 表页排除在外 */
/* ... 三种情况：MSI-X 在头部 / 尾部 / 中间 ... */
```

##### `vfio-no-msix-emulation`：什么时候关掉模拟

QEMU 提供了一个机器属性来禁用 MSI-X 表模拟：

```bash
-machine q35,vfio-no-msix-emulation=true
```

对应代码（`hw/vfio/pci.c:1848-1858`）：

```c
/*
 * The emulated machine may provide a paravirt interface for MSIX setup
 * so it is not strictly necessary to emulate MSIX here. This becomes
 * helpful when frequently accessed MMIO registers are located in
 * subpages adjacent to the MSIX table but the MSIX data containing page
 * cannot be mapped because of a host page size bigger than the MSIX table
 * alignment.
 */
if (object_property_get_bool(OBJECT(qdev_get_machine()),
                             "vfio-no-msix-emulation", NULL)) {
    memory_region_set_enabled(&vdev->pdev.msix_table_mmio, false);
}
```

关掉模拟后，guest 对 MSI-X 表的写直接走 mmap 打到真实硬件。适用场景：

| 场景 | 说明 |
|------|------|
| **半虚拟化中断** | guest 通过 hypercall 配置中断，不依赖硬件 MSI-X 表 |
| **设备轮询模式** | 不用中断，MSI-X 表无人写入 |
| **同页性能问题** | MSI-X 表与高频 MMIO 寄存器在同一页，模拟拖累性能（ARM 64KB 页尤为突出） |

**标准 KVM 直通场景下不能关**——关了就没有中断路由。

##### MSI-X Relocation：更干净的解法

与其关掉模拟，不如把 MSI-X 表**搬到独立的 BAR**——原始 BAR 完整 mmap，新 BAR 专门模拟。

QEMU 通过 `x-msix-relocation` 属性实现（`hw/vfio/pci.c:1607-1698`）：

```bash
-device vfio-pci,host=03:00.0,x-msix-relocation=bar5
```

Relocation 的两种模式：

```c
/* hw/vfio/pci.c:1670-1687 */
if (!vdev->bars[target_bar].size) {
    /* 模式 A: 目标 BAR 空闲 → 创建新 BAR，仅分配 MSI-X 所需空间 */
    vdev->bars[target_bar].size = msix_sz;
    vdev->bars[target_bar].mem64 = true;
    vdev->bars[target_bar].type |= PCI_BASE_ADDRESS_MEM_PREFETCH;
    vdev->msix->table_offset = 0;
} else {
    /* 模式 B: 目标 BAR 已有数据 → 扩展它，MSI-X 放后半段 */
    vdev->bars[target_bar].size *= 2;
    vdev->msix->table_offset = vdev->bars[target_bar].size / 2;
}
```

Relocation 后的布局：

```
BAR 4 (原始):
┌──────────────────────────────────────┐
│            设备 MMIO 寄存器           │
│         整 BAR 直接 mmap ✓           │  ← 无模拟，无切分
└──────────────────────────────────────┘

BAR 5 (新建/扩展):
┌──────────────────────────────────────┐
│  MSI-X table + PBA                   │
│  全部走 QEMU 模拟 ✓                  │  ← 中断路由正常工作
│  体积小，不影响性能关键路径             │
└──────────────────────────────────────┘
```

与 `vfio-no-msix-emulation` 的本质区别：

| | `vfio-no-msix-emulation` | MSI-X Relocation |
|---|---|---|
| **做法** | 关掉模拟，guest 直接写硬件 MSI-X 表 | 搬 MSI-X 到独立 BAR，原始 BAR 完整 mmap |
| **中断处理** | QEMU 不再拦截，需要 paravirt 或不用中断 | **仍正常工作**（在新 BAR 上模拟） |
| **安全要求** | 必须有 IR | 有无 IR 均可 |
| **适用性** | 特定场景（paravirt / 轮询） | **通用** |

Relocation 是更干净的解法——既解决了同页性能问题（尤其 ARM 大页场景），又不丢失中断功能。

---

## 🔍 DMA 映射详解

### 2.1 DMA 地址翻译

```
┌──────────────────────────────────────────────────────────────────┐
│                DMA 地址翻译流程                                   │
│                                                                  │
│  Guest 中:                                                       │
│    设备驱动发起 DMA:                                              │
│      DMA 地址 = GPA (Guest 物理地址)                             │
│                                                                  │
│  IOMMU 翻译:                                                     │
│    ┌──────────┐    IOVA     ┌──────────────┐    HPA             │
│    │  设备    │ ──────────▶ │   IOMMU      │ ─────────▶ ┌─────┐ │
│    │ (PCIe)  │  DMA请求    │  页表翻译     │  物理地址   │ 内存 │ │
│    └──────────┘             └──────────────┘             └─────┘ │
│                                                                  │
│  两层映射:                                                        │
│    Guest: GPA → IOVA (由 QEMU/KVM 在 IOMMU 中设置)              │
│    设备:  看到的是 IOVA (设备认为自己 DMA 到 GPA)                 │
│                                                                  │
│  或者直接使用 GPA 作为 IOVA:                                      │
│    Guest GPA = IOVA (当 Guest 不使用虚拟 IOMMU 时)               │
│    KVM 将 GPA 映射到 HPA 到 IOMMU 页表中                         │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 DMA 映射路径

```
用户态 (QEMU) DMA 映射流程:

  ioctl(VFIO_IOMMU_MAP_DMA)
      │
      ▼
  vfio_iommu_type1.c: vfio_dma_do_map()
      │
      ├── 验证参数（IOVA, 大小, 用户空间 VADDR）
      │
      ▼
  vfio_find_dma_valid() — 检查 IOVA 范围是否已映射
      │
      ├── 如果已映射 → 返回错误
      │
      ▼
  vfio_pin_pages_remote() — 固定用户空间页面
      │
      ├── get_user_pages_fast() 固定物理页
      ├── 防止页面被换出
      │
      ▼
  iommu_map() — 写入 IOMMU 页表
      │
      ├── IOVA → PFN 映射
      ├── 权限设置 (Read/Write)
      │
      ▼
  IOMMU 硬件生效
      │
      └── 设备 DMA 现在可以通过 IOMMU 访问这些物理页
```

---

## 🔗 KVM-VFIO 桥接

### 3.1 KVM 如何与 VFIO 交互

```
┌──────────────────────────────────────────────────────────────────┐
│                  KVM-VFIO 桥接架构                                │
│                                                                  │
│  ┌──────────────────┐              ┌──────────────────┐          │
│  │    QEMU          │              │    KVM           │          │
│  │                  │              │                  │          │
│  │  /dev/vfio/5     │              │  /dev/kvm        │          │
│  │  (设备文件)      │              │  (KVM 设备文件)  │          │
│  └────────┬─────────┘              └────────┬─────────┘          │
│           │                                  │                    │
│           │ KVM_DEV_VFIO_GROUP               │                    │
│           │ (将 VFIO 组关联到 KVM VM)        │                    │
│           └──────────────┬───────────────────┘                    │
│                          │                                        │
│                          ▼                                        │
│              ┌───────────────────────┐                            │
│              │  virt/kvm/vfio.c     │                            │
│              │  kvm_vfio 模块       │                            │
│              │                      │                            │
│              │  功能:               │                            │
│              │  - 管理 VFIO 组列表  │                            │
│              │  - DMA coherency     │                            │
│              │  - 中断路由关联      │                            │
│              │  - 设备生命周期管理  │                            │
│              └───────────────────────┘                            │
│                                                                  │
│  关键交互点:                                                      │
│    1. QEMU 将 VFIO 设备/组的 fd 传给 KVM (KVM_DEV_VFIO_FILE_ADD)  │
│    2. KVM 取引用，并递增 assigned_device_count                    │
│    3. 根据设备是否 DMA coherent，注册/注销 noncoherent DMA        │
│                                                                  │
│  注意: IRTE 改成 Posted 模式**不走这条路**。                      │
│  组列表只提供 PI 的前提条件之一(assigned_device_count > 0)，       │
│  真正的触发是 irq_bypass 的 token 配对，见 3.3 节。               │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 KVM VFIO 操作

```c
/* 来源: virt/kvm/vfio.c */

/*
 * KVM VFIO 桥接支持的操作:
 *
 * KVM_DEV_VFIO_GROUP_ADD:
 *   将 VFIO 组添加到 KVM VM
 *   - 获取 vfio_group 引用
 *   - 加入 kvm->vfio_groups 列表
 *   - 用于后续 PI 配置
 *
 * KVM_DEV_VFIO_GROUP_DEL:
 *   从 KVM VM 移除 VFIO 组
 *   - 释放 vfio_group 引用
 *   - 从列表中删除
 *
 * KVM_DEV_VFIO_GROUP_SET_SPAPR_TCE:
 *   (PowerPC 特有) 设置 TCE 表
 *
 * KVM_DEV_VFIO_FILE_ADD / DEL:
 *   与 GROUP_ADD/DEL 类似，但使用文件描述符
 */
```

### 3.3 MSI-X 中断直通：从 Remapped 到 Posted

这是 phase3（中断虚拟化）与 phase5 真正接壤的地方。要点是**职责分工**：

| 阶段 | 谁做 | IRTE 状态 |
|---|---|---|
| `VFIO_DEVICE_SET_IRQS` 启用 MSI-X | VFIO | Remapped（`IM=0`），中断打进宿主 |
| irqfd 注册 + token 配对 | irq_bypass | 不变 |
| `__connect()` 回调进 KVM | KVM | 改写为 Posted（`IM=1`），硬件直投 vCPU |

**VFIO 自己永远建不出 Posted IRTE** —— 它不知道 vCPU 的 PI Descriptor 在哪。
Posted 化只能由 KVM 发起，而 KVM 要发起就必须先和 VFIO 对上。

#### 接缝：eventfd 指针当 token

VFIO 在装配每个向量的最后一步注册 producer：

```c
/* 来源: drivers/vfio/pci/vfio_pci_intrs.c:515 */
	ctx->producer.token = trigger;
	ctx->producer.irq = irq;
	ret = irq_bypass_register_producer(&ctx->producer);
```

KVM 在建立 irqfd 时注册 consumer：

```c
/* 来源: virt/kvm/eventfd.c:444 */
		irqfd->consumer.token = (void *)irqfd->eventfd;
		irqfd->consumer.add_producer = kvm_arch_irq_bypass_add_producer;
		irqfd->consumer.del_producer = kvm_arch_irq_bypass_del_producer;
		irqfd->consumer.stop = kvm_arch_irq_bypass_stop;
		irqfd->consumer.start = kvm_arch_irq_bypass_start;
		ret = irq_bypass_register_consumer(&irqfd->consumer);
```

两个 `token` 都是**同一个 eventfd 上下文的指针** —— QEMU 把同一个 eventfd 分别交给
`VFIO_DEVICE_SET_IRQS` 和 `KVM_IRQFD`，内核两侧便自动认亲。注册**无先后要求**，
producer 和 consumer 两侧都会扫描对面的链表（`virt/lib/irqbypass.c:108` 与 `:204`）。

#### 配对成功后的调用链

```
irq_bypass_register_{producer,consumer}()
  └─ __connect()                                  virt/lib/irqbypass.c:30
       └─ cons->add_producer()
            = kvm_arch_irq_bypass_add_producer()   arch/x86/kvm/x86.c:13665
                 ├─ kvm_arch_start_assignment()    :13673
                 └─ pi_update_irte()
                      = vmx_pi_update_irte()       arch/x86/kvm/vmx/posted_intr.c:272
                           └─ irq_set_vcpu_affinity()
                                = intel_ir_set_vcpu_affinity()
                                                   drivers/iommu/intel/irq_remapping.c:1248
                                     └─ modify_irte()   ← IRTE 落盘为 Posted
```

KVM 提供的两个关键信息就是 Posted IRTE 里 VFIO 拿不到的部分：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:319 */
		vcpu_info.pi_desc_addr = __pa(vcpu_to_pi_desc(vcpu));
		vcpu_info.vector = irq.vector;
```

`pi_desc_addr` 变成 IRTE 的 `PDAL`/`PDAH`，`vector` 变成 `VV`（Virtual Vector）。

#### 不是所有中断都会被 Posted 化

`vmx_pi_update_irte()` 先查四个全局前提：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:135 */
static bool vmx_can_use_vtd_pi(struct kvm *kvm)
{
	return irqchip_in_kernel(kvm) && enable_apicv &&
		kvm_arch_has_assigned_device(kvm) &&
		irq_remapping_cap(IRQ_POSTING_CAP);
}
```

再逐条路由表项筛，只有单目标 vCPU 的普通中断能过：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:314 */
		kvm_set_msi_irq(kvm, e, &irq);
		if (!kvm_intr_is_single_vcpu(kvm, &irq, &vcpu) ||
		    !kvm_irq_is_postable(&irq))
			continue;
```

被筛掉的（多播、广播、多目标低优先级）会走 `posted_intr.c:338` 的
`irq_set_vcpu_affinity(host_irq, NULL)` **退回 Remapped 模式**。
所以一台机器上同一个设备的不同向量，完全可能一部分 Posted、一部分 Remapped。

##### 两种不同的 Posted Interrupt：VT-d PI vs KVM-side PI

> ⚠️ **容易混淆的点**：`vmx_can_use_vtd_pi()` 中有 `has_assigned_device` 条件，
> 容易让人以为"没有直通设备就不能用 Posted Interrupt"。这是**错误**的理解。

PI Descriptor 机制有**两种触发源**，共享同一套底层硬件（PIR→IRR 自动同步），但入口不同：

| | **VT-d PI** (IOMMU 路径) | **KVM-side PI** (APICv 路径) |
|---|---|---|
| **谁写 PI Descriptor** | IOMMU 硬件直接写 | KVM 软件写 (`vmx_deliver_posted_interrupt`) |
| **触发源** | 直通设备 MSI → IOMMU 拦截 | 任何中断源 → KVM APIC 模拟 |
| **调用入口** | `vmx_pi_update_irte()` | `vmx_deliver_interrupt()` |
| **需要 `has_assigned_device`** | ✅ 是（没有直通设备就没有 MSI 给 IOMMU） | ❌ 否 |
| **需要的条件** | `vmx_can_use_vtd_pi()` 四条件全满足 | `apicv_active` |

```
中断来源 A: 直通设备 (VT-d PI)
  设备 MSI → IOMMU → PI Descriptor ← IOMMU 硬件直接写
                    ↑
                    需要 vmx_can_use_vtd_pi() = true

中断来源 B: 模拟设备 (KVM-side PI)
  QEMU → kvm_set_irq() → APIC 模拟 → deliver_interrupt()
                                        → vmx_deliver_posted_interrupt()
                                        → PI Descriptor ← KVM 软件写
                                          ↑
                                          只需 apicv_active = true

中断来源 C: vCPU 间 IPI (KVM-side PI)
  kvm_apic_send_ipi() → deliver_interrupt() → vmx_deliver_posted_interrupt()
```

**`vmx_deliver_interrupt()`** 注册为 `kvm_x86_ops.deliver_interrupt`，由
`lapic.c:1352` 的 `kvm_x86_call(deliver_interrupt)()` 调用。这意味着 **KVM 侧的
PI Descriptor 机制对所有中断源都可用**——包括模拟设备、IPI 等，不限于直通设备。

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:4299 */
void vmx_deliver_interrupt(struct kvm_lapic *apic, int delivery_mode,
                           int trig_mode, int vector)
{
    if (vmx_deliver_posted_interrupt(vcpu, vector)) {
        /* Posted 失败 → 退回传统路径 */
        kvm_lapic_set_irr(vector, apic);
        kvm_make_request(KVM_REQ_EVENT, vcpu);
        kvm_vcpu_kick(vcpu);
    }
}
```

`vmx_deliver_posted_interrupt()` 只检查 `apicv_active`，**不检查 `has_assigned_device`**。
所以即使纯模拟 VM，只要 APICv 开启，中断也能走 PI Descriptor 路径减少 VM-Exit。

**总结**：`has_assigned_device` 只影响 **IOMMU 直接 posted**（VT-d PI 路径），
不影响 **KVM 自己写 PI Descriptor**（KVM-side PI 路径）。两者共享 PI Descriptor
结构和 PIR→IRR 硬件同步机制，但触发源和门控条件不同。

#### 实测：一个向量的 IRTE 前后对比

在 `0000:4b:00.0` 上抓 `modify_irte` 的原始 128 位（完整过程见
[practice/README.md](practice/README.md) 练习3）：

| | `pst` (spec **IM**) | 向量 | 目标信息 |
|---|---|---|---|
| VFIO 建好时 | **0** = Remapped | `0x21` = 33 | `dest_id` = 宿主 APIC ID |
| KVM 改写后 | **1** = Posted | `VV` = 33 | `PDAL=0x2e5210b` `PDAH=0x29` → PI Desc `0x29b94842c0` |

`SID` 两次都是 `0x4b00`，即设备 BDF `4b:00.0`，可用来校验探针取参是否正确。
从 KVM 注册 consumer 到 Posted IRTE 落盘实测约 53 µs。

**Posted 化之后宿主彻底看不到这个中断了**：Guest 内压 80 MB 只读 I/O，
Guest 侧中断计数 +629，而宿主 `/proc/interrupts` 上对应的三个 IRQ 计数**全程为 0** ——
`vfio_msihandler()` 根本没被调用，也就没有 `eventfd_signal()`，更没有 VM-Exit。

> ⚠️ 别把它和 `CONFIG_X86_POSTED_MSI` 搞混。后者（6.11+）是**宿主自己**的
> posted MSI 优化，与 Guest 无关，走的是 `prepare_irte_posted()`
> （`irq_remapping.c:1111`）+ `posted_msi_supported()` 门控。
> 本节讲的是 KVM 的 Guest VT-d PI，走 `intel_ir_set_vcpu_affinity()`，两条路互不相干。

---

## 📖 源码阅读路线

### 推荐阅读顺序

```
┌─────────────────────────────────────────────────────────────┐
│                    源码阅读路线                                │
│                                                             │
│  Step 1: VFIO 核心框架                                       │
│  ├── drivers/vfio/vfio_main.c       ← VFIO 核心            │
│  │   ├── vfio_group 管理                                    │
│  │   ├── vfio_device 管理                                   │
│  │   └── ioctl 分发                                          │
│  └── include/linux/vfio.h           ← 接口定义              │
│                                                             │
│  Step 2: IOMMU Type 1 驱动                                   │
│  ├── drivers/vfio/vfio_iommu_type1.c ← DMA 映射核心        │
│  │   ├── vfio_dma_do_map()                                   │
│  │   ├── vfio_pin_pages_remote()                             │
│  │   └── iommu_map()                                         │
│  └── drivers/iommu/iommu.c          ← IOMMU 核心           │
│                                                             │
│  Step 3: VFIO PCI 驱动                                       │
│  ├── drivers/vfio/pci/vfio_pci_core.c ← PCI 设备直通       │
│  │   ├── 配置空间模拟                                         │
│  │   ├── MMIO 区域映射                                        │
│  │   └── 中断管理 (INTx/MSI/MSI-X)                          │
│  └── drivers/vfio/pci/vfio_pci_priv.h                       │
│                                                             │
│  Step 4: KVM-VFIO 桥接                                       │
│  └── virt/kvm/vfio.c                ← KVM VFIO 桥接        │
│      ├── kvm_vfio_group 管理                                 │
│      └── kvm_dev_vfio_ops                                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 关键函数索引

| 函数名 | 文件 | 作用 |
|--------|------|------|
| `pci_device_group()` | `drivers/iommu/iommu.c:1515` | PCI 设备的组划分主逻辑，四步判定 |
| `pci_acs_path_enabled()` | `drivers/pci/pci.c:3693` | 检查整条上游路径的 ACS，任一跳不满足即 false |
| `pci_acs_enabled()` | `drivers/pci/pci.c:3620` | 单设备 ACS 判定：先查 quirk（`:3624`），再按 PCIe 类型分类 |
| `pci_dev_specific_acs_enabled()` | `drivers/pci/quirks.c:5234` | ACS quirk 表分发，三态返回；`pci_quirk_rciep_acs()` 匹配所有 Intel RCiEP |
| `get_pci_function_alias_group()` | `drivers/iommu/iommu.c:1391` | 归并同 slot 上未隔离的多功能 function |
| `pci_acs_init()` | `drivers/pci/pci.c:3717` | probe 时找 ACS extended capability 并尝试启用 |
| `pci_enable_acs()` | `drivers/pci/pci.c:1075` | 写 ACS Control：quirk 优先，**返回 0 才跳过标准写入** |
| `pci_std_enable_acs()` | `drivers/pci/pci.c:1052` | 标准写入 SV/RR/CR/UF；`TB` 只给外部/不可信设备或 `pci=noats` |
| `pci_request_acs()` | `drivers/pci/pci.c:943` | 置位「需要 ACS」，由 IOMMU 探测时调用（Intel：`dmar.c:935`） |
| `pci_dev_specific_enable_acs()` | `drivers/pci/quirks.c:5438` | ACS 启用 quirk 表分发，命中返回 0 |
| `pci_ats_supported()` | `drivers/pci/ats.c:41` | 只看 `ats_cap` 与 `untrusted`，**不查 ACS** |
| `pci_enable_ats()` | `drivers/pci/ats.c:90` | 写 ATS Control 的 `E` 与 `STU`；VF 只校验不新写 |
| `pci_ats_page_aligned()` | `drivers/pci/ats.c:193` | 读 `PCI_ATS_CAP` bit5，Intel 开 ATS 的硬前提 |
| `pci_ats_queue_depth()` | `drivers/pci/ats.c:168` | 读 Invalidate Queue Depth，`0` 含义见规范 |
| `iommu_enable_pci_caps()` | `drivers/iommu/intel/iommu.c:1287` | Intel 侧 ATS 启用门控（三合一判断在 `:1295-1297`） |
| `iommu_disable_pci_caps()` | `drivers/iommu/intel/iommu.c:1300` | 清 `E` 位；与规范 §4.5.2 的顺序差异见 1.5.6 |
| `pci_enable_pasid()` | `drivers/pci/ats.c:395` | 前置 `pci_acs_path_enabled(RR+UF)` 判断在 `ats.c:419` |
| `cache_tag_flush_devtlb_psi()` | `drivers/iommu/intel/cache.c:390` | 把 Device-TLB 失效排进 qi batch |
| `iommu_group_claim_dma_owner()` | `drivers/iommu/iommu.c:3214` | 认领组的 DMA ownership，组内有普通驱动则 `-EPERM`（判断在 `:3222`） |
| `vfio_file_iommu_group()` | `vfio_main.c` | 获取文件的 IOMMU 组 |
| `vfio_dma_do_map()` | `vfio_iommu_type1.c` | DMA 映射操作 |
| `vfio_pin_pages_remote()` | `vfio_iommu_type1.c` | 固定用户页面 |
| `vfio_pci_core_enable()` | `vfio_pci_core.c` | 启用 PCI 设备直通 |
| `vfio_pci_mmap()` | `vfio_pci_core.c` | 映射设备 MMIO |
| `kvm_vfio_group_add()` | `virt/kvm/vfio.c` | 添加 VFIO 组到 KVM |
| `kvm_vfio_update_coherency()` | `virt/kvm/vfio.c` | 更新 DMA 一致性 |
| `vfio_pci_set_msi_trigger()` | `vfio_pci_intrs.c` | `SET_IRQS` 的 MSI/MSI-X 入口 |
| `vfio_msi_set_vector_signal()` | `vfio_pci_intrs.c:447` | 单向量装配：`request_irq` + 注册 producer |
| `vfio_msihandler()` | `vfio_pci_intrs.c:373` | 宿主中断处理，仅 Remapped 模式下执行 |
| `irq_bypass_register_producer()` | `virt/lib/irqbypass.c:84` | VFIO 侧注册，按 token 找 consumer |
| `irq_bypass_register_consumer()` | `virt/lib/irqbypass.c:179` | KVM 侧注册，按 token 找 producer |
| `kvm_arch_irq_bypass_add_producer()` | `arch/x86/kvm/x86.c:13665` | 配对成功回调，转发到 PI 更新 |
| `vmx_pi_update_irte()` | `vmx/posted_intr.c:272` | 判定可否 Posted，取 PI Desc 地址 |
| `intel_ir_set_vcpu_affinity()` | `intel/irq_remapping.c:1248` | 把 IRTE 实际改写为 Posted 格式 |

---

## 🔬 实践练习

> 下面是手工验证步骤。带实测输出的可运行实验（ownership 认领时机、IOVA→HPA 映射验证、
> MSI-X 中断直通与 IRTE Posted 化）见 [practice/README.md](practice/README.md)。

### 练习 1：基本设备直通

```bash
# 1. 检查 IOMMU 是否启用
dmesg | grep -i iommu
# 应看到类似: DMAR: IOMMU enabled

# 2. 检查 VFIO 模块是否加载
lsmod | grep vfio

# 3. 加载 VFIO 模块（如果未加载）
modprobe vfio
modprobe vfio-pci
modprobe vfio_iommu_type1

# 4. 查看设备的 IOMMU 组
readlink /sys/bus/pci/devices/0000:03:00.0/iommu_group
# 输出类似: ../../../../kernel/iommu_groups/5

# 5. 绑定设备到 VFIO 驱动
echo "8086 1533" > /sys/bus/pci/drivers/vfio-pci/new_id
# 或使用 vendor:device ID

# 6. 在 QEMU 中使用设备直通
# qemu-system-x86_64 ... \
#   -device vfio-pci,host=0000:03:00.0
```

### 练习 2：DMA 映射跟踪

```bash
# 跟踪 IOMMU DMA 映射操作
echo iommu_map > /sys/kernel/debug/tracing/set_event
echo iommu_unmap >> /sys/kernel/debug/tracing/set_event
echo vfio_iommu_type1 >> /sys/kernel/debug/tracing/set_event

echo 1 > /sys/kernel/debug/tracing/tracing_on

# 启动带设备直通的虚拟机
# 然后在 trace 中观察 DMA 映射活动

echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace
```

### 练习 3：分析 IOMMU 域

```bash
# 查看 IOMMU 域信息
ls /sys/kernel/iommu_groups/

# 查看特定组的设备
ls /sys/kernel/iommu_groups/5/devices/

# 查看 IOMMU 设备信息
dmesg | grep -E "iommu|DMAR|AMD-Vi"

# 查看 DMAR 表
cat /sys/firmware/acpi/tables/DMAR 2>/dev/null | hexdump -C | head
```

> 拿到本机的分组结果后，对照 [1.4 IOMMU 组是怎么划出来的](#14-iommu-组是怎么划出来的)
> 逐条推导：哪些组是因为 PCIe-to-PCI 桥而并的，哪些是同 slot 多功能设备，
> 哪些设备独占一组、以及它上游每一跳的 ACS 状况。

### 练习 4：性能测试

```bash
# 使用 iperf3 测试直通网卡性能
# 在 Host 上:
iperf3 -s

# 在 Guest 上:
iperf3 -c <host_ip> -t 30

# 对比:
# 1. 纯模拟网卡 (virtio) 的吞吐量
# 2. VFIO 直通的吞吐量
# 3. 裸机（无虚拟化）的吞吐量

# 使用 perf 分析 IOMMU 开销
perf record -e intel_iommu:dtlb_walk -a -- sleep 10
```

### 练习 5：中断分析

```bash
# 1. 查看直通设备的宿主 IRQ（名字形如 vfio-msix[0](0000:4b:00.0)）
grep vfio /proc/interrupts
cat /sys/bus/pci/devices/0000:4b:00.0/msi_irqs

# 2. 判断有没有走中断重映射（IR- 前缀）
for n in $(ls /sys/bus/pci/devices/0000:4b:00.0/msi_irqs); do
  echo "irq $n -> $(cat /sys/kernel/irq/$n/chip_name)"
done

# 3. 观察 KVM 侧的 IRTE 更新（这个 tracepoint 真实存在，
#    定义在 arch/x86/kvm/trace.h:1080）
sudo bash -c 'cd /sys/kernel/tracing
echo 1 > events/kvm/kvm_pi_irte_update/enable
echo 1 > tracing_on'
# 启动带直通设备的 VM，然后看 trace
sudo cat /sys/kernel/tracing/trace | grep kvm_pi_irte_update
```

> 注意：内核里**没有** `vfio_irq_set` tracepoint（`vfio_irq_set` 只是
> `include/uapi/linux/vfio.h:584` 的一个结构体名），本仓早期版本写错过。
> 要观测 VFIO 侧的装配过程只能下 kprobe，做法见
> [practice/README.md](practice/README.md) 练习3。

**判读要点**：Posted 模式生效后，第 1 步里那些 IRQ 的计数应当**始终为 0**。
计数在涨说明中断仍走 Remapped 路径，排查见 [陷阱4](#陷阱4中断停在-remapped-模式性能达不到预期)。

---

## 🔧 常见问题排查

### 设备直通失败

```bash
# 1. 检查 IOMMU 启用状态
cat /proc/cmdline | grep -E "intel_iommu|amd_iommu"
# 需要: intel_iommu=on 或 amd_iommu=on

# 2. 检查 ACS (Access Control Services)
# ACS 确保设备间 DMA 隔离
setpci -s 00:1c.0 ECAP_ACS+6.b  # 读取 ACS 能力

# 3. 检查 IOMMU 组完整性
# 如果多个设备在同一组且不能分离，可能无法直通单个设备

# 4. 检查 dmesg 错误
dmesg | grep -i vfio
dmesg | grep -i iommu
```

---

## 📚 参考资料

- Linux kernel: `Documentation/driver-api/vfio.rst`
- Intel SDM Vol 3, Chapter 10: Advanced Programmable Interrupt Controller
- Intel VT-d Specification: IOMMU 硬件细节
- KVM Forum talks on VFIO and device passthrough
- 论文: *"VFIO: The Virtual Function I/O Framework"*

---

## ✅ 阶段检验清单

- [ ] VFIO 容器、组、设备三者的关系是什么？
- [ ] IOMMU 组为什么有时包含多个设备？如何处理？
- [ ] ACS 七个能力位分别管什么？为什么 `ACSCap` 里的 `-` 不算隔离失败？
- [ ] 内核在哪些时机会**写** ACS Control？`pci=config_acs=` 的标志串该怎么读？
- [ ] ATS 开启后 `AT=10b` 的请求硬件还做哪些检查？为什么直通时 `unmap` 会变慢？
- [ ] `vfio_dma_do_map()` 的完整路径是什么？从 ioctl 到 IOMMU 页表更新
- [ ] KVM-VFIO 桥接的作用是什么？为什么 KVM 需要知道 VFIO 组？
- [ ] 直通设备的 MSI-X 中断是如何路由到 Guest 的？
- [ ] 设备直通的 DMA 一致性如何保证？

---

## 🔍 VMM视角对比

### 用户态VMM vs KVM内核态设备直通

| 方面 | 用户态VMM (QEMU) | KVM内核态 |
|------|------------------|-----------|
| **设备管理** | 通过VFIO ioctl管理 | VFIO框架直接集成 |
| **DMA映射** | ioctl(VFIO_IOMMU_MAP_DMA) | 通过KVM memslot自动映射 |
| **中断路由** | ioctl配置中断路由 | KVM-VFIO桥接自动处理 |
| **一致性管理** | 手动处理 | KVM自动处理coherency |

### 关键差异：DMA映射路径

```
用户态VMM:
  Guest访问GPA → EPT翻译 → IOMMU翻译 → HPA
  DMA映射: ioctl(VFIO_IOMMU_MAP_DMA) → IOMMU页表

KVM内核态:
  Guest访问GPA → EPT翻译 → IOMMU翻译 → HPA
  DMA映射: KVM memslot自动映射 → IOMMU页表
```

---

## ⚡ 性能优化技术

### 1. IOTLB缓存

**问题**：IOMMU翻译开销大

**解决**：使用IOTLB缓存IOMMU翻译结果

```c
/* intel_iommu.c 中配置 */
if (iommu->iommu_caps & VTD_CAP_IOTLB_CACHE) {
    /* 启用IOTLB缓存 */
    iommu_enable_iotlb_cache(iommu);
}
```

**效果**：
- 减少IOMMU翻译延迟
- DMA密集负载性能提升20-30%

### 2. DMA批处理

**问题**：频繁的DMA映射/解映射开销大

**解决**：批量处理DMA映射

```c
/* vfio_dma_do_map() 中优化 */
/* 批量映射多个页面 */
for (i = 0; i < nr_pages; i++) {
    iommu_map(domain, iova + i * PAGE_SIZE, pfn[i] * PAGE_SIZE, PAGE_SIZE, prot);
}
```

**效果**：
- 减少IOMMU页表更新次数
- 性能提升10-15%

### 3. 中断亲和性

**问题**：中断在多个pCPU间迁移

**症状**：中断延迟不稳定

**解决**：
```bash
# 绑定中断到特定pCPU
echo 2 > /proc/irq/<irq_num>/smp_affinity
# 绑定到pCPU 1
```

**效果**：
- 减少TLB刷新
- 中断延迟稳定

---

## ⚠️ 常见陷阱

### 陷阱1：IOMMU组包含多个设备

**场景**：无法直通单个设备

**症状**：绑定 `vfio-pci` **会成功**，但随后 `VFIO_GROUP_SET_CONTAINER` 返回 `-EPERM`；
`VFIO_GROUP_GET_STATUS` 中 `VIABLE` 标志为 0。

**原因**：IOMMU组包含多个设备，组内仍有设备绑在普通驱动上，DMA ownership 无法认领。

`vfio-pci` 声明了 `.driver_managed_dma = true`（`drivers/vfio/pci/vfio_pci.c:205`），
probe 阶段刻意不动 `owner_cnt`，所以绑定不会失败。认领发生在 container 附加时
（`drivers/vfio/container.c:437`），此时才检查：

```c
/* 来源: drivers/iommu/iommu.c:3214 —— iommu_group_claim_dma_owner() */
	if (group->owner_cnt) {
		ret = -EPERM;
		goto unlock_out;
	}
```

**解决**：
```bash
# 列出组内全部成员
ls /sys/kernel/iommu_groups/<N>/devices/

# 组内所有设备都必须绑到 vfio-pci（或处于 unbound），然后整组一起直通
qemu-system-x86_64 ... \
  -device vfio-pci,host=0000:03:00.0 \
  -device vfio-pci,host=0000:03:00.1  # 同组的其他设备
```

> 详见 [corrections.md](corrections.md) 勘误 1，含 kprobe 实测的完整接管时序。

### 陷阱2：ACS未启用

**场景**：设备与上游桥被划入同一个 IOMMU group，无法单独直通

**症状**：`/sys/kernel/iommu_groups/<N>/devices/` 里除目标设备外还有上游桥

**原因**：上游链路某一跳不满足 `REQ_ACS_FLAGS`，内核认为无法保证 peer-to-peer DMA 隔离，
于是把桥拉进同一组：

```c
/* 来源: drivers/iommu/iommu.c:1543 —— pci_device_group() */
	if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
		break;

	pdev = bus->self;          /* 隔离不成立 → 把桥拉进来 */
```

**排查**：
```bash
# 看上游链路每一跳的 PCIe 类型与 ACS 能力
lspci -vvv -s 49:01.0 | grep -A2 "Access Control Services"
```

**注意**：要分清 ACS 的**能力位**（`ACSCap`）与**控制位**（`ACSCtl`）。能力位是硬件属性，
**没有任何内核参数能变出一块硬件没有的能力**；控制位是可写的，而且只要内核检测到 IOMMU，
`pci_request_acs()` 就会在设备 probe 时把 `SV`/`RR`/`CR`/`UF` 全部置上 —— 也就是说
"忘了开 ACS" 这个前提在上游内核里基本不成立。写控制位时 `caps->ctrl |= (caps->cap & ...)`
会被 `ACSCap` 掩掉（`pci.c:1055`），所以 `ACSCtl` 里显示 `-` 的位，对应 `ACSCap` 里一定也是 `-`
—— **缺的是能力，不是开关**（见 [1.5.2](#152-linux-不只读-acs它还会主动写)）。上游确实存在两个能改控制位的参数
`pci=config_acs=` 与 `pci=disable_acs_redir=`，但它们的作用都是**进一步关掉**隔离，
解决不了「组太大」。社区流传的 `pcie_acs_override=downstream,multifunction` 来自第三方补丁，
**不在上游内核中**（6.12.93 的 `drivers/pci/` 与 `kernel-parameters.txt` 均无此参数），
在原生内核上写了会被静默忽略；`pci=noaer` 关的是 AER，与 ACS 无关。
硬件不提供隔离时唯一正规解法是整组一起直通 —— 强行拆组会真实破坏 DMA 隔离。

> 单功能设备"没有 ACS 能力"反而算隔离成立，这一点极易误判，
> 详见 [corrections.md](corrections.md) 勘误 2。
>
> 完整的划分规则（四步判定、`pci_acs_enabled()` 的三类处理、一个 switch 拆出 30+ 组的实测）
> 见 [1.4 IOMMU 组是怎么划出来的](#14-iommu-组是怎么划出来的)。

### 陷阱3：DMA一致性未处理

**场景**：Guest和Host看到不一致的数据

**症状**：Guest读取到过时的数据

**原因**：DMA一致性未处理

**解决**：
```c
// KVM-VFIO桥接中自动处理
kvm_vfio_update_coherency(kvm, vfio_group);
// 确保DMA操作使用正确的内存类型
```

### 陷阱4：中断停在 Remapped 模式，性能达不到预期

**场景**：直通设备中断能到 Guest，但延迟偏高、宿主 CPU 有可观的中断开销

**症状**：宿主 `/proc/interrupts` 上该设备的 IRQ 计数**在涨**。
Posted 模式下这些计数应当**始终为 0**，因为 `vfio_msihandler()` 不会被调用。

```bash
# 找出设备的宿主 IRQ 号，观察计数是否增长
cat /sys/bus/pci/devices/0000:4b:00.0/msi_irqs
watch -n1 'grep -E "^ *(112|113|114):" /proc/interrupts'
```

**原因**：IRTE 没有被改写成 Posted 模式（`IM=0`），中断仍然先进宿主、再经 eventfd
唤醒 irqfd，多绕一整圈。常见诱因：

1. `enable_apicv=N`，或硬件不支持 IRQ posting；
2. 该向量的目标不是单个 vCPU（多播/广播/多目标低优先级），被
   `posted_intr.c:315` 主动排除，这是**设计如此，不是故障**；
3. Guest 里做了 irqbalance，把向量的 affinity 改成多 CPU 掩码。

**排查**：
```bash
cat /sys/module/kvm_intel/parameters/enable_apicv    # 需要 Y
dmesg | grep -i "posting\|Posted-Interrupt"

# 区分 Remapped / Posted 需要看 IRTE 原始位，chip_name 只能告诉你有没有重映射
cat /sys/kernel/irq/112/chip_name                   # IR- 前缀 = 有重映射
```

`/sys/kernel/debug/kvm/` 下**没有** `irq_routing` 这个文件（本仓早期版本写错过）。
可靠的观测手段是 `kvm_pi_irte_update` tracepoint 或对 `modify_irte` 下 kprobe。

> 完整机制见 [3.3 节](#33-msi-x-中断直通从-remapped-到-posted)，
> 含实测数据的动手实验见 [practice/README.md](practice/README.md) 练习3。
