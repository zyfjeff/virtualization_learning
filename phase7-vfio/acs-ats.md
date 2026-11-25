# ACS 与 ATS：直通依赖的两个 PCIe 能力

> 承接：[group-division.md](group-division.md)（分组反复用到 ACS）+ [phase4](../phase4-pcie/README.md)（PCIe 能力）。
> 本文回答：ACS（隔离）与 ATS（地址翻译）是正交两件事，常被混为一谈。
> 引出：DMA 映射 → [README](README.md) §2；MSI-X 直通 → README §3.3；陷阱 → AGENTS.md #10–14。
>
> 本文自 README §1.5 拆出（2026-11-25 框架拆分），内容未改。
> ⚠️ 本节初版桥类型常量/group 101 成因/Root Port ACS 校验条件曾写错，修正见 [corrections.md](corrections.md) 勘误 5。

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
`device_block_translation()`（`drivers/iommu/intel/iommu.c:3394`）的真实顺序是：

| 步骤 | 规范 §4.5.2 | Linux 6.12.93 实际 |
|---|---|---|
| 1 quiesce DMA | 由软件负责 | **不在这个函数里**，`device_block_translation()` 只做 2/3/4 |
| 2 清 `E` 位 | 第 2 步 | `iommu_disable_pci_caps()`（调用点 `intel/iommu.c:3407` → 定义 `:1300`）→ `pci_disable_ats()` |
| 4 改 context entry | **第 4 步** | `domain_context_clear()`（`:3413`）—— **排在第 3 步之前**；★ 这是 `:3409-3413` 那个 `if (sm_supported(iommu)) … else …` 的 **else** 分支，scalable mode 下走的是 `intel_pasid_tear_down_entry()`（`:3410`） |
| 3 global DevTLB Invalidate + Wait | 第 3 步 | 在 `intel_context_flush_present()` 末尾的 `__context_flush_dev_iotlb()`（`pasid.c:971` → 定义 `:885`，实际下发 `qi_flush_dev_iotlb()` @ `:898`）|

也就是说，Linux 把「先失效设备缓存、再关 context」倒成了「先关 context，再走一遍
统一的 context 失效路径」。跟在这个倒置后面的是一个更要命的细节：
`__context_flush_dev_iotlb()` 开头有道门控 —— `if (!info->ats_enabled) return;`
（`pasid.c:887-888`），而 `info->ats_enabled` 在第 2 步里已经被
`iommu_disable_pci_caps()` 清成了 0（`intel/iommu.c:1311`，函数定义 `:1300`）。**这条 Device-TLB 失效在
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

> **详见 phase5-interrupts/posted-interrupts.md §2** 的 IRTE 格式详解和 SVT/SID/SQ 字段定义。

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

