# 第 11 阶段勘误

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
恒等域现在是 `intel_iommu_ops.identity_domain` 指向的静态 `identity_domain` 对象
（`drivers/iommu/intel/iommu.c:4646`），设备与域的关系改由 `dev_iommu` /
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

**实际**：只有 Intel（`drivers/iommu/intel/iommu.c:4646`）和 ARM SMMUv3
（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:3528`）挂了 `.identity_domain`。
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
