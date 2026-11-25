# 第 3 阶段勘误

> 基线：Linux 6.12.93。本阶段 2026-08 整体重构（从"按机制铺陈"改为"框架 → 主流程 →
> 8 问"）过程中，对照源码逐条核查了旧稿结论。以下条目记录**旧稿或上游内核里确实存在
> 的错误/不一致**，以及修正后的正确信息与引用。全部可用
> [`practice/check-citations.sh`](practice/check-citations.sh) 复核。

---

## 勘误 1：`si_domain` / `domain_add_dev_info` 在 6.12 已不存在

**旧稿写法**：沿用多篇早期文章的说法，把 `si_domain`（静态/恒等域全局指针）和
`domain_add_dev_info()`（按设备查域信息）当作 6.12 里仍可直接引用的符号。

**实际**：二者在 6.12.93 的 `drivers/iommu/` 里已经**彻底消失**。全词检索
（`grep -rnw --include='*.[ch]' si_domain drivers/iommu/` 与
`grep -rnw --include='*.[ch]' domain_add_dev_info drivers/iommu/`）均为零命中。
恒等域现在是 `intel_iommu_ops.identity_domain`（`drivers/iommu/intel/iommu.c:4647`）指向的
静态 `identity_domain` 对象（同文件 `:4636-4642`），设备与域的关系改由 `dev_iommu` /
`group->default_domain` 承载。

**修正**：重构后的文档只把它们当"墓碑符号"提及，见
[`backends.md` B.7](backends.md#b7-墓碑612-里已经不存在的符号)。

---

## 勘误 2：上游内核注释与代码不符——`-EBUSY` 还是 `-EEXIST`

**位置**：`drivers/iommu/iommufd/device.c:322-324` 的注释：

```c
/* 来源: drivers/iommu/iommufd/device.c:322-324 */
		/*
		 * iommu_get_msi_cookie() can only be called once per domain,
		 * it returns -EBUSY on later calls.
		 */
```

**实际**：`iommu_get_msi_cookie()` 重复调用返回的是 **`-EEXIST`**：

```c
/* 来源: drivers/iommu/dma-iommu.c:416-424 —— iommu_get_msi_cookie() 节选 */
int iommu_get_msi_cookie(struct iommu_domain *domain, dma_addr_t base)
{
	...
	if (domain->iova_cookie)
		return -EEXIST;
```

**修正**：这是上游注释本身写错，不是本仓文档的错——但旧稿曾照注释转述成 `-EBUSY`。
重构后以代码为准，并在 [`interrupts.md` IR.5](interrupts.md) 明确标注了这处不符。
写工具解析错误码时按 `-EEXIST` 处理。

---

## 勘误 3：虚构函数名 `iommu_get_direct_mapeeings`

**旧稿写法**：早期草稿引用了一个不存在的函数名 `iommu_get_direct_mapeeings`
（拼写也不对），用来描述"把固件保留区建进域"的步骤。

**实际**：该函数叫 `iommu_create_device_direct_mappings()`，定义在
`drivers/iommu/iommu.c:1091`，在 probe 建组（`:563`）与切域等路径被调用，负责把
`IOMMU_RESV_DIRECT` 等保留区映射进目标域。

**修正**：重构后统一用正确名字，走读见
[`annotations.md` A.1/A.2](annotations.md)、[`iova.md` IO.5](iova.md)。

---

## 勘误 4：Intel 保留窗口的宏名是 `IOAPIC_RANGE_*`，不是 MSI 命名

**旧稿写法**：描述 Intel 为 IO-APIC/MSI 保留的恒等窗口时，凭直觉引用了
"MSI_RANGE" 之类的宏名。

**实际**：宏就叫 `IOAPIC_RANGE_START` / `IOAPIC_RANGE_END`：

```c
/* 来源: drivers/iommu/intel/iommu.c:43-44 */
#define IOAPIC_RANGE_START	(0xfee00000)
#define IOAPIC_RANGE_END	(0xfeefffff)
```

`0xFEE00000` 段同时覆盖 local APIC/MSI 地址空间，但宏名只提 IO-APIC。dma-iommu 侧
对应的辅助函数是 `iommu_dma_get_msi_page()`（`drivers/iommu/dma-iommu.c:1765`）。

**修正**：[`interrupts.md`](interrupts.md) 与 [`backends.md`](backends.md) 已按实际
宏名与行号引用。

---

## 勘误 5：AMD-Vi 没有 `.identity_domain`——identity 不是静态单例

**旧稿写法**：由 Intel/ARM 的写法类推，说"三家后端都在 `iommu_ops` 里挂一个静态
identity 域对象"。

**实际**（就本项目讨论的三家而言）：只有 Intel（`drivers/iommu/intel/iommu.c:4647`，对象
定义 `:4636`）和 ARM SMMUv3（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:3527`，对象定义
`:3081`）挂了 `.identity_domain`。`intel/iommu.c:4646` 是 `.release_domain`、
`arm-smmu-v3.c:3528` 是 `.blocked_domain`——本条最初的引用各偏了一行，已订正。
（注：全内核共有 14 个驱动填了 `.identity_domain`，包括 arm-smmu(v2)、apple-dart、mtk、
exynos、omap、msm、tegra、rockchip、sun50i、ipmmu-vmsa、qcom_iommu 等；
本条的比较范围只限 Intel/AMD/SMMUv3 三家，不要读成"全内核只有两家"。）
AMD 的 `amd_iommu_ops`（`drivers/iommu/amd/iommu.c:3058` 起）**没有这个字段**，
其恒等语义由 `.def_domain_type = amd_iommu_def_domain_type`（`:3069`，定义 `:2987`）
投票 + **每次真实分配的域**表达（硬件上落到 `DTE[Mode]=0` / `PAGE_MODE_NONE`，
`drivers/iommu/amd/iommu.c:221-223`）。且 SNP 场景直接禁止 IDENTITY 域
（`drivers/iommu/amd/iommu.c:2596-2601`）。

**修正**：[`backends.md` B.4](backends.md#b4-identity-的三种硬件表达) 已按
"静态单例 vs 分配型域"区分三家。

---

## 勘误 6：invalidation.md 的目录与小节编号一度错位

**现象**：重构把 Q5 的正文编号定为 I.1–I.16，但目录（TOC）与"本文怎么展开"段落
残留了旧编号：目录条目写着"I.3 一次 `iommu_unmap()` 的完整失效链"却链接到
`#i1-...` 锚点，且 I.12–I.16 出现两组重复条目。

**实际**：正文标题本身（I.1–I.16）始终正确；错的是目录映射。

**修正**：已在 [`invalidation.md`](invalidation.md) 内重写目录与展开段落，16 个条目
与正文标题一一对应，锚点全部可解析（校验脚本见
[`practice/check-citations.sh`](practice/check-citations.sh)）。

---

## 勘误 7：README「规范来源说明」与实际规范库不符

**旧稿写法**：[`README.md`](README.md) 的"规范来源说明"写着"本仓库的规范资料只有
三份……**没有 ARM SMMU 规范（ARM IHI 0070），也没有 PCI Express Base
Specification**"。

**实际**：仓库已有 `arm-smmuv3.pdf`（Arm SMMUv3 架构规范）与
`pcie-base-spec-r6.0.pdf`（PCIe 6.0），见
[AGENTS.md 参考资料表](../AGENTS.md)。唯一确实缺失的是 **AMD-Vi 规范**。

**修正**：已重写该节为五份规范的清单 + 引用规则，与
[`invalidation.md`](invalidation.md)、[`backends.md`](backends.md) 开头的
"规范可用性声明"表一致。

---

## 勘误 8：交叉引用沿用旧 phase 编号

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置与修正**:
  | 位置 | 错误 | 正确 |
  |---|---|---|
  | `README.md:28-29` | "phase4 说…IRTE"、"phase6 说…ACS" | phase5（中断）、phase7（VFIO）；"前几阶段"改"其它阶段"（5/7 编号在本阶段 3 之后，"前几"自相矛盾） |
  | `group.md:79` | 链接文字 "[phase6 corrections]" | phase7（链接目标 `../phase7-vfio/corrections.md` 本就正确，仅文字旧编号） |
  | `domains.md:276` | "见 phase6 §1.4 的实测方法" | phase7 §1.4 |
  | `domains.md:278` | 链接文字 "[phase6 corrections]" | phase7 |
- **成因**: phase4-pcie 插入后中断 4→5、VFIO 6→7，本文未同步。
- **已同步修正**: `README.md`、`group.md`、`domains.md`
- **2026-11-26 补充**：本条当时写的"已同步修正"**不完整**——只改了 4 处，还留下
  12 处 `phase6`。现已全部改完并复核（`grep -rn phase6 *.md` 除本条表格外零命中）：
  | 文件 | 行 | 改动 |
  |---|---|---|
  | `group.md` | 13、23、39、42、136、138、142、146 | 链接文字与小节标题 `phase6` → `phase7`；TOC 锚点同步为 `#g4-与-phase7-的接缝` |
  | `domains.md` | 64、303、306、323 | 同上；`:323` 原句里 `phase6` 与"第七阶段"并存，属自相矛盾 |
  | `invalidation.md` | 1048、1157 | `:1157` 文字改 phase7；`:1048` 的"ATS 前提成立与否"改指 `phase4-pcie/ats.md`（+ phase7 §1.5 的直通用法），因为那是 ATS 语义而非 VFIO 记账 |

---

## 勘误 9：`FS21GP/FS11GP/FS5LP/FS2LP/FS1LP` 是凭空造出来的规范字段名

**旧稿写法**（`translation.md` T.3.3）：讲二级大页能力时说"二级看由
`FS21GP/FS11GP/FS5LP/FS2LP/FS1LP` 组成的 `SUPER_PAGE_VAL`，取 `fls()`"。

**实际**：VT-d Rev 4.1 里**没有** `FS21GP`/`FS11GP`/`FS2LP`/`FS1LP` 这几个字段
（`pdftotext` 后全文零命中）。`FS5LP` 存在，但它是 **Capability Register bit 60 的
"First Stage 5-level Paging Support"**（`cap_fl5lp_support`，`drivers/iommu/intel/iommu.h:164`），
跟二级大页无关。二级大页的规范字段是 **SSLPS: Second Stage Large Page Support，
bits 37:34**。

**规范引用**: `intel-vtd.pdf` Rev 4.1, Capability Register 字段表（SSLPS, bits 37:34）
——"A value of 1 in any of these bits indicates the corresponding large-page size is
supported: 0 = 21-bit offset (2MB), 1 = 30-bit offset (1GB), 2/3 = Reserved … only
valid values for this field are 0000b, 0001b, 0011b."

**源码引用**: `drivers/iommu/intel/iommu.h:174` → `cap_super_page_val(c) = ((c) >> 34) & 0xf`
（正是 SSLPS 那 4 位）；`drivers/iommu/intel/iommu.h:167` → `cap_fl1gp_support(c) = ((c) >> 56) & 1`（一级 1GB）。

**修正**：`translation.md` T.3.3 已改成一级 FS1GP(bit56) / 二级 SSLPS(bits37:34) 的
两行对照表，并保留"为什么 `fls()` 的结果恰好等于 `iommu_superpage`"的推导。

---

## 勘误 10：P2P 一节三处引用全错，其中一个是虚构函数

**旧稿写法**（`iova.md` IO.6 P2P，三处"来源"标注都错；这里故意**不用** `路径:行号`
的可机读形式，以免 `practice/check-citations.sh` 把已知的错误引用当成待核引用）：

```text
旧标注 1：来源 drivers/pci/p2pdma.c 第 705-794 行（简化）   ← calc_map_type_and_dist()
旧标注 2：来源 drivers/pci/p2pdma.c 第 497-512 行          ← pci_bridge_has_acs_redir()
旧标注 3：来源 include/linux/pci-p2pdma.h 第 210-213 行    ← pci_p2pdma_bus_addr_map()
旧注释：  RR (Request Redirect), CR (Completion Redirect), EC (ECRC)
```

**实际**：

| 项 | 错在哪 | 正确 |
|---|---|---|
| `calc_map_type_and_dist()` | 不在 705-794（708 只是一个调用点） | `drivers/pci/p2pdma.c:575-668` |
| `pci_bridge_has_acs_redir()` | 不在 497-512（那段是 host bridge whitelist 检查） | `drivers/pci/p2pdma.c:383-403`，**函数体旧稿抄得逐行正确**，只是行号错 |
| `pci_p2pdma_bus_addr_map()` | **内核里没有这个函数**；该头文件 `include/linux/pci-p2pdma.h` 总共只有 107 行，旧标注的 210-213 行越界 | 换算是 `pci_p2pdma_map_segment()` 里的 `sg->dma_address = sg_phys(sg) + state->bus_off;`（`drivers/pci/p2pdma.c:1026`；函数 `:1016-1033`），偏移在 `:324-325` `bus_offset = pci_bus_address(pdev, bar) - pci_resource_start(pdev, bar)` 算好 |
| `EC (ECRC)` | EC 不是 ECRC | `PCI_ACS_EC = 0x0020 /* P2P Egress Control */`（`include/uapi/linux/pci_regs.h:996`）。ECRC 属于 TLP prefix / AER，压根不在 ACS Control 里 |

**修正**：`iova.md` IO.6 已换成上面四处正确引用，并把 ACS 三个被检查位做成表格
（RR=0x0004 / CR=0x0008 / EC=0x0020，另注 SV/TB/UF/DT 不在判据内）。

---

## 勘误 11：`device_group()` 被挂在错误的调用帧上

**旧稿写法**（`group.md` G.2 调用树）：

```
├─ iommu_group_alloc_device()  drivers/iommu/iommu.c:549
│    ├─ bus->iommu_ops->device_group()   ← 分组判定在这里
│    └─ iommu_group_alloc()    drivers/iommu/iommu.c:963
```

**实际**：`iommu_group_alloc_device(struct iommu_group *group, struct device *dev)`
（定义 `drivers/iommu/iommu.c:1158`）的**形参里已经带着 group**，它只做
`group_device` 记账与 sysfs 链接，不调 `device_group()`。真正的调用是
`iommu_init_device()` 里的 `group = ops->device_group(dev);`
（`drivers/iommu/iommu.c:427`；该函数定义 `:402`，由 `__iommu_probe_device()` 在 `drivers/iommu/iommu.c:544` 调用，
结果存进 `dev->iommu_group` 于 `:434`，`:548` 取回后才传给 `:549`）。

**后果**：分组判定与后端的 `ops->probe_device()`（`:416`）在**同一帧**、且**早于链表加入**
（`:560`）。挂错帧会让人以为组成员是记账阶段才定的，从而读不懂 [Q3](domains.md) 的
"选域要遍历组成员投票"。

**修正**：`group.md` G.2 的调用树重画（含 `:416/:423/:427/:434/:548/:549` 六个锚点）并加勘误框；
`backends.md` B.5 的简化链 `:600 → :513 → :416` 同样补上 `iommu_init_device()` 这一帧。

---

## 勘误 12：IOMMUFD nested 的翻译方向整体下移了一级

**旧稿写法**（`iommufd-objects.md`）：
`Parent HWPT (stage 2, HPA → PA)`、`Nested HWPT (stage 1, GPA → HPA)`、
`翻译链：GPA → (Nested) → HPA → (Parent) → PA`。

**实际**：`parent` 是 **stage-2 = GPA→HPA**，`nested` 携带的是 **stage-1 = 进程地址(VA)→GPA**，
根本不存在 `HPA → PA` 这一级。

**规范引用**: `intel-vtd.pdf` Rev 4.1 §3.6（first-stage 输入必须是 canonical address，
"address bits 63:N have the same value as address bit [N-1]"）、§3.7（second-stage 输入受
MGAW 检查，输出才是宿主物理地址）、§3.8（"requests translated through first-stage
translation are also subjected to nested second-stage translation"）。

**源码引用**: `include/uapi/linux/iommufd.h:356/364`（`IOMMU_HWPT_ALLOC_NEST_PARENT`）、
`:381-394`（`struct iommu_hwpt_vtd_s1 { pgtbl_addr; addr_width; flags; }` —— 用户态递上来的
是 **stage-1** 表基址）；`drivers/iommu/iommufd/iommufd_private.h:301-310`
（`iommufd_hwpt_paging.nest_parent`）、`:312-315`（`iommufd_hwpt_nested { … *parent; }`，
nested 不挂 IOAS）、`:258-263`（`iommufd_ioas.hwpt_list`，一对多只存在于 IOAS→paging HWPT）。

**修正**：`iommufd-objects.md` 的 nested 小节重写为
`VA ──stage-1──▶ GPA ──stage-2──▶ HPA`，并补上对象层的四处源码引用。

---

## 勘误 13：两处 sysfs 观测引用的函数行号指到了属性表

| 位置 | 旧引用 | 正确 |
|---|---|---|
| `domains.md` D.6 表 | `iommu_group_show_resv_regions`（`drivers/iommu/iommu.c:922`） | 函数在 `drivers/iommu/iommu.c:868`；`:922` 是 `IOMMU_GROUP_ATTR(reserved_regions, 0444, …)` 挂属性那一行（顺带确认：**0444 只读**） |
| `domains.md` D.6 表 | "`type` … **0644 可写**，走 `iommu_group_store_type()`" | 核实无误：`drivers/iommu/iommu.c:925` 定义 `IOMMU_GROUP_ATTR(type, 0644, iommu_group_show_type, iommu_group_store_type)`，宿主实测 `-rw-r--r--` |

**修正**：`domains.md` 已改第一条并补注第二条。

---

## 勘误 14：本阶段抽查里"核实为正确"的高风险点（留档，不是错误）

2026-11-26 复核确认下列结论与源码/规范一致，后续改动不要把它们改回去：

- **identity 域全驱动单例**：`__iommu_domain_alloc()` 对 IDENTITY/BLOCKED 直接
  `return ops->identity_domain/ops->blocked_domain`（`drivers/iommu/iommu.c:1947-1950`），
  不分配内存；因此 identity 域走不到 `iommu_is_dma_domain()` 分支（`:1980-1988`），
  没有 per-group IOVA 分配器。`domains.md` D.5 写法正确。
- **`iommu.passthrough` 是默认域类型选择器**，不是"IOMMU 开关"：
  `early_param`（`drivers/iommu/iommu.c:697`）→ `iommu_set_default_passthrough/translated(true)`
  （`:2809-2821`，置 `IOMMU_CMD_LINE_DMA_API` 让驱动无法覆盖）。
  且 `iommu_setup_dma_ops()` 里 `dev->dma_iommu = iommu_is_dma_domain(domain)`
  （`drivers/iommu/dma-iommu.c:1754`）——identity 域下为 false。
- **IR 与 DMA 重映射是两套**：能力位 `ecap_ir_support`（`intel/iommu.h:222`）vs
  `dmar_disabled`/`intel_iommu_setup`（`intel/iommu.c:205/240`）；
  命令行 `intremap=`/`nointremap`（`drivers/iommu/irq_remapping.c:50-84`）vs `intel_iommu=`；
  `dmar_in_use() = irq_remapping_enabled || intel_iommu_enabled`（`intel/dmar.c:2123-2126`）；
  IR 的 prepare 在 BSP APIC 阶段（`arch/x86/kernel/apic/apic.c:1903` →
  `irq_remapping_prepare()`，`drivers/iommu/irq_remapping.c:100-107` →
  `intel_prepare_irq_remapping()`，`intel/irq_remapping.c:703`），早于 `intel_iommu_init()`。
- **pass-through 的硬件语义**：`intel-vtd.pdf` §3.9 原文逐字符合 `domains.md` D.4 的引用
  （"does not perform any access rights checking"、"subject to Host Address Width (HAW)
  address checking … treated as a translation fault"）。
- **AGAW 定义**（`(AGAW-12)` 是 9 的倍数）与 first-stage 页表格式
  （"supports the same paging structures as Intel 64 processors when operating in 64-bit mode"）
  两条规范原文引用正确（§3.5/§3.6）。

---

## 附：本阶段未重犯、但值得留档的既有陷阱

以下不是新发现的错误，而是与 AGENTS.md「已知陷阱」直接相关、本阶段文档写作时
刻意核对过的点，留作索引：

- **陷阱 11/12/13（ACS 判定）**：[Q2 `group.md`](group.md) 的分组判定完全按
  `pci_acs_flags_enabled()` 的"Cap 掩码 + quirk 先行"语义写，未照名字猜方向。
- **陷阱 14（ATS 命名与时序）**：[Q5 `invalidation.md`](invalidation.md) 讲
  Device-TLB 失效时，明确写了 Linux 把 VT-d spec 4.5.2 推荐的第 3、4 步调换
  （`device_block_translation()` 先清 `E`、Device-TLB 失效推迟到
  `__context_flush_dev_iotlb()`），没有说成"严格按规范顺序"。
- **`iommu.passthrough` 不是开关**：旧资料常见的"开/关 IOMMU"表述在
  [`domains.md`](domains.md)（Q3）中已按"默认域类型选择"重写。
