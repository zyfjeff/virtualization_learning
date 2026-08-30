# Q1：硬件怎么把设备的 DMA 地址翻译成物理地址？

> **问题**：设备带着一个 IOVA 和自己的 Requester ID 发出 DMA 请求，硬件怎么找到
> 对应的那棵页表、走完它、得到最终物理地址？两级翻译是什么分工、表怎么选出来、
> 页表长什么样、页大小谁说了算？
>
> **为什么值得问**：主流程（[README 三](README.md#三主流程一次设备-dma-的完整旅程)）的
> 第 ⑥ 步——硬件翻译——是软件完全不在场的一步。看不懂这一步，就读不懂 fault 记录、
> 看不懂为什么 attach 写的只是"路由"而不是映射（T.2），也看不懂三家后端页表格式不同
> 却能被同一套 `pgsize_bitmap` 协商收编（T.3）。
>
> **核查基线**：Linux 6.12.93；`intel-vtd.pdf`（Rev 4.1）。ARM IHI 0070（SMMUv3）
> 与 PCIe Base Spec 本地均无，涉及处只写源码事实并标明边界。

---

## 📖 目录

- [T.1 两级翻译：不是历史包袱，是两种用途](#t1-两级翻译不是历史包袱是两种用途)
- [T.2 谁把 RID 映射到页表根](#t2-谁把-rid-映射到页表根)
- [T.3 `pgsize_bitmap`：core 与后端之间唯一的协商语言](#t3-pgsize_bitmapcore-与后端之间唯一的协商语言)
- [T.4 VT-d：一张 stride 固定为 9 的表](#t4-vt-d一张-stride-固定为-9-的表)
- [T.5 SMMUv3：同一件事在通用库里长了另一套参数](#t5-smmuv3同一件事在通用库里长了另一套参数)
- [T.6 自检问题](#t6-自检问题)

---

## T.1 两级翻译：不是历史包袱，是两种用途

初学者容易以为"第一级/第二级"是像 32 位保护模式那样的旧设计。实际上 VT-d 的
scalable mode 里两者**同时可用**，且各自服务完全不同的场景：

| 配置 | 规范 | 谁在用 | Linux 里的对应 |
|---|---|---|---|
| 只用第二级 | 3.7 | 设备 DMA 进 host 地址空间；VFIO 给 VM 建的映射 | `dmar_domain` 普通页表；`IOMMU_DOMAIN_UNMANAGED` |
| 只用第一级 | 3.6 | 设备直接用**进程页表**（PASID 粒度） | SVA；`IOMMU_DOMAIN_SVA`；`iommu_sva_bind_device()`（`include/linux/iommu.h:1528`） |
| 第一级套第二级（nested） | 3.8 | 直通设备 + Guest 里有自己的页表 | nested 翻译，`IOMMU_HWPT_ALLOC_NEST_PARENT`（`include/uapi/linux/iommufd.h:364`） |
| 都不翻译（pass-through） | 3.9 | 性能场景 / 不需要隔离 | `IOMMU_DOMAIN_IDENTITY`（硬件语义见 [Q3](domains.md)） |

> **术语提醒**：规范 Rev 4.1 用 *first-stage / second-stage*，Linux 代码里仍是旧命名
> `use_first_level` / `FLPT_DEFAULT_DID`（`intel/pasid.h:29`）/ `pasid_set_flptr()`
> （`intel/pasid.h:285`）/ `pasid_set_flpm()`（`intel/pasid.h:295`）。搜内核时搜
> `first_level` / `flptr`，写文档时用规范名，两者别混着当同一个标识符。

关键一点：**第一级翻译的页表格式和 Intel 64 处理器 64 位模式的页表格式相同**
（规范 3.6 原文 "First-stage translation supports the same paging structures as
Intel® 64 processors when operating in 64-bit mode"）。这就是 SVA 能把进程的
`mm_struct` 页表**直接**挂给设备的原因——不需要翻译，硬件读的就是同一份表。SMMUv3 的
stage-1 同理，其 CD 里描述的就是 Arm 架构的翻译表。

而第二级翻译（EPT 的类比物）用的是自己的格式：`SS-PML5E/SS-PML4E/SS-PDPE/SS-PDE/SS-PTE`，
9 位一层，`AGAW` 表示层数（规范原文：AGAW 是 GAW 调整后使 `(AGAW-12)` 为 9 的倍数），
支持 4K/2M/1G 三种粒度。这跟 EPT 的 4 层 9 位分片完全同构，所以
[第二阶段](../phase2-mem-virt/README.md)里关于 EPT 大页拆分的所有直觉都可以直接迁移。

源码侧的对应物是 `struct dmar_domain`，它的 `gaw/agaw` 就是规范里的 MGAW/AGAW；
两条规范约束落到代码上只差一位：

```c
/* 来源: drivers/iommu/intel/iommu.c:468-479 */
	/*
	 * First-level translation restricts the input-address to a
	 * canonical address (i.e., address bits 63:N have the same
	 * value as address bit [N-1], where N is 48-bits with 4-level
	 * paging and 57-bits with 5-level paging). Hence, skip bit
	 * [N-1].
	 */
	if (domain->use_first_level)
		domain->domain.geometry.aperture_end = __DOMAIN_MAX_ADDR(domain->gaw - 1);
	else
		domain->domain.geometry.aperture_end = __DOMAIN_MAX_ADDR(domain->gaw);
```

**第一级翻译要求输入是 canonical 地址**（`gaw - 1`，规范 3.6 原文 "address bits 63:N
have the same value as address bit [N-1]"），**第二级翻译只要求宽度不超过 MGAW**
（规范 3.7 原文 "restricts the input-address to an implementation-specific
address-width reported through the Maximum Guest Address Width"）。

**规范引用**: `intel-vtd.pdf`, Section 3.5 (Hierarchical Translation Structures),
3.6 (First-Stage), 3.7 (Second-Stage), 3.8 (Nested), 3.9 (Pass-through),
9.7 (First-Stage Paging Entries), 9.8 (Second-Stage Paging Entries)

---

## T.2 谁把 RID 映射到页表根

主流程的 ①–④ 都发生在设备发 DMA 之前；⑥ 的"选表"由硬件自动完成，但**把页表根指针
装进选表结构，是软件在 attach 时做的**。这一层**只在 attach 时写一次**，之后
`iommu_map()` 完全不碰它。三个后端的做法是同构的：

| 后端 | 结构 | 装进去的三样东西 | 源码 |
|---|---|---|---|
| VT-d legacy | context entry | `TT` 翻译类型 + 根指针 + `AW` 宽度 | `intel/iommu.c:1644` |
| VT-d scalable | PASID table entry | `PGTT` + `SLPTPTR`/`FLPTPTR` + `AW` | `intel/pasid.c:397` |
| SMMUv3 | STE | `CFG` + `S1CTXPTR`(→CD) 或 `S2TTB` | `arm/arm-smmu-v3/arm-smmu-v3.c:1596`、`:1654` |
| AMD-Vi | device table entry | `V` + `Mode` + Page Table Root + Domain ID | `amd/init.c` |

（"选表"的另一半——硬件怎么拿 RID 索引到这张表——在
[README 1.3](README.md#13-iommu-多出来的三个维度) 讲过：root table 按总线号索引，
`iommu_context_addr()`（`drivers/iommu/intel/iommu.c:483`）就是它的软件侧对应物。
attach 的调用链在 [Q2 G.3](group.md#g3-attach-路径域如何装到设备上)。）

VT-d legacy 模式这段值得整段读，它把"attach 装的是路由，不是映射"说得非常直白：

```c
/* 来源: drivers/iommu/intel/iommu.c:1672-1694（节选） */
	context_set_domain_id(context, did);

	/*
	 * Skip top levels of page tables for iommu which has
	 * less agaw than default. Unnecessary for PT mode.
	 */
	for (agaw = domain->agaw; agaw > iommu->agaw; agaw--) {
		ret = -ENOMEM;
		pgd = phys_to_virt(dma_pte_addr(pgd));
		if (!dma_pte_present(pgd))
			goto out_unlock;
	}

	if (info && info->ats_supported)
		translation = CONTEXT_TT_DEV_IOTLB;
	else
		translation = CONTEXT_TT_MULTI_LEVEL;

	context_set_address_root(context, virt_to_phys(pgd));
	context_set_address_width(context, agaw);
	context_set_translation_type(context, translation);
	context_set_fault_enable(context);
	context_set_present(context);
```

三个容易看漏的点：

1. **`CONTEXT_TT_DEV_IOTLB`(=1) 与 `CONTEXT_TT_MULTI_LEVEL`(=0) 都会翻译**
   （`intel/iommu.h:59-60`）。区别是硬件是否接受来自该设备的 `Translated` 请求，
   即是否允许它带 ATS。别把 `DEV_IOTLB` 读成"跳过 IOMMU"。规范里的 pass-through 是
   `CONTEXT_TT_PASS_THROUGH`(=2)，对应 [Q3](domains.md) 的
   `intel_pasid_setup_pass_through()`（`intel/pasid.c:529`）。
2. **同一棵页表可以以不同层数挂到不同的 IOMMU 上**。`agaw > iommu->agaw` 时把 `pgd`
   往下挪几层，等于"这台硬件单元地址宽度小，从中间某层开始给它看"。scalable mode 里
   同一个技巧抽成了 `iommu_skip_agaw()`（`intel/pasid.c:379`）。所以 `dmar_domain`
   的页表**不属于**某个 IOMMU 单元，属于 domain。
3. **顺序是"建表 → 装路由 → 失效"**。这段之后的 `context_present_cache_flush()`
   （`intel/iommu.c:1696`）就是 [Q2 G.3](group.md#g3-attach-路径域如何装到设备上) 里
   那句"先建表再上线才是正确顺序"在硬件层面的落点。

SMMUv3 在这里多出一件事：STE 是 64 字节，而硬件只在"已使用的位"变化时才必须看到一致
结果（`arm_smmu_get_ste_used()` 的注释
"`Based on the value of ent report which bits of the STE the HW will access`"，
`arm/arm-smmu-v3/arm-smmu-v3.c:1020-1023`）。于是 Linux 用一套分步写协议：先写不影响
硬件的字段，最后一步才翻转决定性的 `V`/`CFG`（`arm_smmu_write_ste()`，`:1543`）。
CPU 侧换页表根只需要一次 `mov cr3`，IOMMU 侧换一个 64 字节表项要写若干次并保证中间
状态对硬件不可观测——这是 [README 1.3](README.md#13-iommu-多出来的三个维度)
"缓存与失效的时序设计者不是你"的第一个具体后果。

---

## T.3 `pgsize_bitmap`：core 与后端之间唯一的协商语言

很多材料把"页表用多大页"写成后端自己的事。实际上**决定权在 core**：后端只申报一个
位图，core 每次 `iommu_map()` 都重新裁一遍。

### T.3.1 core 侧的裁剪算法

```c
/* 来源: drivers/iommu/iommu.c:2391 */
static size_t iommu_pgsize(struct iommu_domain *domain, unsigned long iova,
			   phys_addr_t paddr, size_t size, size_t *count)
{
	unsigned long addr_merge = paddr | iova;            /* :2398 */

	/* Page sizes supported by the hardware and small enough for @size */
	pgsizes = domain->pgsize_bitmap & GENMASK(__fls(size), 0);   /* :2401 */

	/* Constrain the page sizes further based on the maximum alignment */
	if (likely(addr_merge))
		pgsizes &= GENMASK(__ffs(addr_merge), 0);            /* :2404-2405 */
	...
	pgsize_idx = __fls(pgsizes);                                 /* :2411 */
	pgsize = BIT(pgsize_idx);                                    /* :2412 */
```

三行就是全部逻辑，每行对应一个物理约束：

| 行 | 约束 | 说人话 |
|---|---|---|
| `:2401` | `pgsize ≤ size` | 剩余长度装不下一页，就不能选这个尺寸 |
| `:2404-2405` | `pgsize ≤ 2^__ffs(iova\|paddr)` | **IOVA 和物理地址都要对齐**，取二者对齐度的较小值 |
| `:2411-2412` | 取剩下里最大的 | 尽量用大页 |

关键在 `:2398` 的 `addr_merge = paddr | iova` 配上 `:2405` 的 `__ffs(addr_merge)`：
**两个地址或起来再取最低置位**，等价于"各自能容忍的最大对齐"里取小的那个，一次 `|`
同时完成两次对齐检查。VT-d 自己算 superpage 时用的是逐字相同的技巧（T.4.2）。

`count` 是第二层输出：core 还会试探"下一个更大的受支持尺寸能否提前用上"，让
`map_pages()` 一次拿到 `count` 个同尺寸页（`drivers/iommu/iommu.c:2416-2440`）。其中

```c
/* 来源: drivers/iommu/iommu.c:2424-2429 */
	/*
	 * There's no point trying a bigger page size unless the virtual
	 * and physical addresses are similarly offset within the larger page.
	 */
	if ((iova ^ paddr) & (pgsize_next - 1))
		goto out_set_count;
```

说的是 **IOVA 与物理地址在大页内的偏移必须一致**，否则大页根本表达不了这段映射。
这也顺手解释了一件事：identity 域天然比 DMA 域更容易凑出大页 —— identity 下
`iova == paddr`，异或恒为 0，这一关永不拦截。

### T.3.2 入口检查与"失败回滚会顺带做一次同步失效"

```c
/* 来源: drivers/iommu/iommu.c:2463-2474 */
	min_pagesz = 1 << __ffs(domain->pgsize_bitmap);

	/*
	 * both the virtual address and the physical one, as well as
	 * the size of the mapping, must be aligned (at least) to the
	 * size of the smallest page supported by the hardware
	 */
	if (!IS_ALIGNED(iova | paddr | size, min_pagesz)) {
		pr_err("unaligned: iova 0x%lx pa %pa size 0x%zx min_pagesz 0x%x\n",
		       iova, &paddr, size, min_pagesz);
		return -EINVAL;
	}
```

又是"一个表达式查三个对齐"。这里的实践含义是 **`min_pagesz` 不恒等于 4096**：
若某后端的 `pgsize_bitmap` 最低档是 64K（Mali LPAE 等），`iommu_map()` 会直接拒绝
4K 对齐的请求并打出上面那行 `pr_err`。看到 `min_pagesz 0x10000` 不要怀疑硬件，
是位图里就没有 4K。

循环体里唯一要记的是**部分成功**：`ops->map_pages()` 允许"填了一部分再报错"，core
用 `size -= mapped`（`:2491`）扣掉已建立的部分，最后整段回滚：

```c
/* 来源: drivers/iommu/iommu.c:2500-2504 */
	/* unroll mapping in case something went wrong */
	if (ret)
		iommu_unmap(domain, orig_iova, orig_size - size);
	else
		trace_map(orig_iova, orig_paddr, orig_size);
```

注意回滚用的是 `iommu_unmap()` 而**不是** `iommu_unmap_fast()` —— 意味着
**一次 map 失败会顺带产生一次同步失效**。IOVA 压力大、map 频繁失败时，失效队列的
耗时会记到 map 路径的账上；在 perf 里看到 map 调用栈中出现 `qi_submit_sync`，就是
这一行，不是正常路径。

`else` 分支的 `trace_map` 也值得单独一句：它**只在成功时触发**，所以用
`iommu:map` tracepoint 统计映射次数比统计函数调用次数准。

### T.3.3 后端侧：位图是谁写进去的

VT-d 在域分配时就定死：

```c
/* 来源: drivers/iommu/intel/iommu.c:3508-3511 */
	/* pagesize bitmap */
	domain->domain.pgsize_bitmap = SZ_4K;
	domain->iommu_superpage = iommu_superpage_capability(iommu, first_stage);
	domain->domain.pgsize_bitmap |= domain_super_pgsize_bitmap(domain);
```

`iommu_superpage_capability()` 的两条分支值得单独看：

```c
/* 来源: drivers/iommu/intel/iommu.c:3465-3474 */
static int iommu_superpage_capability(struct intel_iommu *iommu, bool first_stage)
{
	if (!intel_iommu_superpage)
		return 0;

	if (first_stage)
		return cap_fl1gp_support(iommu->cap) ? 2 : 1;

	return fls(cap_super_page_val(iommu->cap));
}
```

**一级和二级的大页能力取自 `CAP_REG` 的不同位段**：一级只看 `FS1GP`（是否支持
1GB first-stage 大页），是/否二值；二级看由 `FS21GP/FS11GP/FS5LP/FS2LP/FS1LP`
组成的 `SUPER_PAGE_VAL`，取 `fls()`。这跟 T.1 里"一级/二级是两套页表格式"是同一件事
的两个侧面。

`intel_iommu_superpage` 是全局开关（`drivers/iommu/intel/iommu.c:211`），被命令行
`intel_iommu=sp_off` 清零（`drivers/iommu/intel/iommu.c:262-264`）。清零后所有域只有 4K，
`count` 恒为 1，页表深度和内存占用都会明显上升 —— practice 里"关掉大页看
`dmar_perf_latency` 与 RSS 变化"就是一个便宜的对照实验。

`domain_super_pgsize_bitmap()` 的注释直接给了编码表：

```c
/* 来源: drivers/iommu/intel/iommu.c:443-450 */
	/*
	 * 1-level super page supports page size of 2MiB, 2-level super page
	 * supports page size of both 2MiB and 1GiB.
	 */
	if (domain->iommu_superpage == 1)
		bitmap |= SZ_2M;
	else if (domain->iommu_superpage == 2)
		bitmap |= SZ_2M | SZ_1G;
```

即 `iommu_superpage` 的语义是"**能跳几级**"，不是"最大页有多大"。这决定了 T.4.2
那个 `while` 循环的迭代上限。

---

## T.4 VT-d：一张 stride 固定为 9 的表

### T.4.1 AGAW / level / width 的换算是全部复杂度的来源

```c
/* 来源: drivers/iommu/intel/iommu.h:40-41 */
#define VTD_STRIDE_SHIFT        (9)
#define VTD_STRIDE_MASK         (((u64)-1) << VTD_STRIDE_SHIFT)
```

```c
/* 来源: drivers/iommu/intel/iommu.h:890-893 */
#define LEVEL_STRIDE		(9)
#define LEVEL_MASK		(((u64)1 << LEVEL_STRIDE) - 1)
#define MAX_AGAW_WIDTH		(64)
#define MAX_AGAW_PFN_WIDTH	(MAX_AGAW_WIDTH - VTD_PAGE_SHIFT)
```

`VTD_STRIDE_SHIFT` 与 `LEVEL_STRIDE` 是**同一个数字 9 写了两遍**：前者作用在 PFN /
页数域，后者作用在位偏移域。三个换算函数：

```c
/* 来源: drivers/iommu/intel/iommu.h:895-912 */
static inline int agaw_to_level(int agaw)
{
	return agaw + 2;
}

static inline int agaw_to_width(int agaw)
{
	return min_t(int, 30 + agaw * LEVEL_STRIDE, MAX_AGAW_WIDTH);
}

static inline unsigned int level_to_offset_bits(int level)
{
	return (level - 1) * LEVEL_STRIDE;
}
```

代进去：`AGAW=1` → 3 级 → 39 位；`AGAW=2` → 4 级 → 48 位；`AGAW=3` → 5 级 → 57 位。
**照这三个函数的公式记，别照规范正文的 "4-level / 5-level paging" 措辞记**：规范讲
"页表级数"，Linux 讲"AGAW 索引"，而 `agaw_to_level()` 里那个 `+2` 又把它们错开一格。
T.2 的 `iommu_skip_agaw()` —— 同一棵页表树挂在不同 IOMMU 单元的不同深度 ——
正是被这个错开放大的。三套编号的对应关系：

| `AGAW` | `agaw_to_level()` = 根级号 | `agaw_to_width()` = 输入地址宽度 | 规范里的说法 |
|---|---|---|---|
| 1 | 3 | 39 | 3 级 |
| 2 | 4 | 48 | 4 级 |
| 3 | 5 | 57 | 5 级 |
| 4 | 6 | 64（被 `MAX_AGAW_WIDTH` 截断） | 保留 |

`level_to_offset_bits(level) = (level-1) * 9` 说明 **level 1 的索引位是 0 位**（叶子
直接给地址），level 2 有 9 位索引 → 覆盖 512 × 4K = 2MB。所以：

```c
/* 来源: drivers/iommu/intel/iommu.h:935-937 */
static inline unsigned long lvl_to_nr_pages(unsigned int lvl)
{
	return 1UL << min_t(int, (lvl - 1) * LEVEL_STRIDE, MAX_AGAW_PFN_WIDTH);
}
```

`lvl=2 → 512 页 = 2MB`、`lvl=3 → 262144 页 = 1GB`。**VT-d 的大页只有 2MB 和 1GB
两档，是这条公式的直接推论**：stride 恒为 9，从 4K 跳一级是 2MB、跳两级是 1GB，
再跳就是 512GB，而硬件不提供一个 512GB 的中间级位（`MAX_AGAW_PFN_WIDTH = 52`
封顶）。这就是 `pgsize_bitmap` 永远只有 `SZ_4K | SZ_2M | SZ_1G` 三个 bit 的原因，
也是 T.5.1 里 ARM 能有 16K/64K 粒度的对照面。

顺带一个常在文档里写错的数字：`MAX_AGAW_PFN_WIDTH = 52`（`drivers/iommu/intel/iommu.h:893`）。它后面
在 [`invalidation.md`](invalidation.md) 的 Device-TLB 全域失效里还会以"mask"的身份
出现一次。

### T.4.2 superpage 判定：`pfnmerge` 与逐级右移

```c
/* 来源: drivers/iommu/intel/iommu.c:1731-1755 */
/* Return largest possible superpage level for a given mapping */
static int hardware_largepage_caps(struct dmar_domain *domain, unsigned long iov_pfn,
				   unsigned long phy_pfn, unsigned long pages)
{
	int support, level = 1;
	unsigned long pfnmerge;

	support = domain->iommu_superpage;

	/* To use a large page, the virtual *and* physical addresses
	   must be aligned to 2MiB/1GiB/etc. Lower bits set in either
	   of them will mean we have to use smaller pages. So just
	   merge them and check both at once. */
	pfnmerge = iov_pfn | phy_pfn;

	while (support && !(pfnmerge & ~VTD_STRIDE_MASK)) {
		pages >>= VTD_STRIDE_SHIFT;
		if (!pages)
			break;
		pfnmerge >>= VTD_STRIDE_SHIFT;
		level++;
		support--;
	}
	return level;
}
```

三点要单独拎出来：

1. `pfnmerge = iov_pfn | phy_pfn` 与 core 的 `addr_merge = paddr | iova`
   （`drivers/iommu/iommu.c:2399`）是**同一个想法**，只是这里在 PFN 域上、用
   `VTD_STRIDE_MASK`（低 9 位为 0）代替了 `__ffs`。也就是说 core 与后端**各自独立**
   做了一遍对齐裁剪。core 已经保证 `pgsize` 合法，后端为何还要再判？因为 core 的
   位图只有 `4K/2M/1G` 三档，而 VT-d 的 `level` 是 1..5 的连续整数 ——
   **只有后端知道"这一级在当前 AGAW 下到底存不存在"**。
2. `pages >>= 9` 后紧跟 `if (!pages) break;` —— **剩余长度装不下该级大页就停**。
   于是"低地址对齐够、但映射长度不够"也会掉到大页。想验证：`iommu_dma_map_page()`
   拿到的物理块 order 由分配器决定，同一段 IOVA 出现"中间 1GB、两头 2MB/4K"的
   页表形状是正常现象。
3. `support--` 把循环上限锁死在 `iommu_superpage`，即 T.3.3 那张"能跳几级"的表。

`level` 最终成为 `__domain_mapping()`（`drivers/iommu/intel/iommu.c:1794` 起）逐级建表的起点。
这条链上有一个不太直观的性质值得记住：**IOVA 分配器的策略会影响页表形状**
（[iova.md](iova.md)），进而影响 IOTLB 命中率与 walk 深度。[iova.md](iova.md) 说"IOVA 是分配出来的"，这里是它的第二个后果 —— 第一个后果是保留区。

### T.4.3 小页换大页：`switch_to_super_page()` 是顺序约束的"反例现场"

```c
/* 来源: drivers/iommu/intel/iommu.c:1757-1784 */
/*
 * Ensure that old small page tables are removed to make room for superpage(s).
 * We're going to add new large pages, so make sure we don't remove their parent
 * tables. The IOTLB/devTLBs should be flushed if any PDE/PTEs are cleared.
 */
static void switch_to_super_page(struct dmar_domain *domain,
				 unsigned long start_pfn,
				 unsigned long end_pfn, int level)
{
	unsigned long lvl_pages = lvl_to_nr_pages(level);
...
		if (dma_pte_present(pte)) {
			dma_pte_free_pagetable(domain, start_pfn,
					       start_pfn + lvl_pages - 1,
					       level + 1);

			cache_tag_flush_range(domain, start_pfn << VTD_PAGE_SHIFT,
					      end_pfn << VTD_PAGE_SHIFT, 0);
		}
```

[invalidation.md](invalidation.md) 强调过"必须先失效、再释放页表页"。这里**表面上是反的**：
`dma_pte_free_pagetable()` 在前、`cache_tag_flush_range()` 在后。差在哪？
`dma_pte_free_pagetable()` 只做"从页表上摘下子表 + 挂进收集链表"，真正
`free_page()` 发生在失效之后（同 `dma_pte_list_pagetables()` 那一套，见
`drivers/iommu/intel/iommu.c:1001-1004` 的注释）。所以"摘指针 → 失效 → 释放"三步仍然成立，
只是"失效"写在了显式位置而不是隐式在 unmap 尾部。读代码时如果只看见
`free` 字样在 `flush` 前面就断言 Linux 违反顺序，是错的。

最后一个参数 `0` 是 `ih`，即**强制 IH=0**：既然下面整棵子树都要拆掉，
paging-structure cache 里指向这些中间级的条目必须一起失效。这与 [invalidation.md](invalidation.md) 里
"IH=1 只在叶子级失效时才安全"是互补的两处证据。

### T.4.4 大页位：一级和二级各有一套位定义

```c
/* 来源: drivers/iommu/intel/iommu.h:43-46 */
#define DMA_PTE_READ		BIT_ULL(0)
#define DMA_PTE_WRITE		BIT_ULL(1)
#define DMA_PTE_LARGE_PAGE	BIT_ULL(7)
#define DMA_PTE_SNP		BIT_ULL(11)
```

`dma_pte_superpage()`（`drivers/iommu/intel/iommu.h:869-872`）就是 `pte->val & DMA_PTE_LARGE_PAGE`，
**二级用 bit 7 表示"这一项是叶子大页而不是下一级指针"**。一级是另一套：

```c
/* 来源: drivers/iommu/intel/iommu.h:48-54 */
#define DMA_FL_PTE_PRESENT	BIT_ULL(0)
#define DMA_FL_PTE_US		BIT_ULL(2)
#define DMA_FL_PTE_ACCESS	BIT_ULL(5)
#define DMA_FL_PTE_DIRTY	BIT_ULL(6)

#define DMA_SL_PTE_DIRTY_BIT	9
#define DMA_SL_PTE_DIRTY	BIT_ULL(DMA_SL_PTE_DIRTY_BIT)
```

从这张表能直接读出三条 nested 场景的实操前提：

- 一级与二级**不是同一种页表**，也没有统一的 walk 代码：`iommu.c` 里
  `dma_fl_*` 与 `dma_sl_*` 各一套 helper，脏位分别在 bit 6 与 bit 9
  （`drivers/iommu/intel/iommu.h:51`、`:53-54`）。做 IOMMU dirty log（live migration）必须先确定
  这棵树是哪一级，才能读对 A/D 位。
- 一级有 `US`（bit 2，用户/超级），二级没有 —— 二级是"总线地址到物理地址"，
  特权级概念只存在于一级。这正是 VT-d Spec 3.6 / 3.7 的分工，也是 Linux 里
  `use_first_level` 域主要服务 SVA 而不是普通 DMA 的原因之一。
- 一级的 A/D（bit 5/6）语义与 CPU 侧 page table 一致，因为**一级翻译就是 CPU 页表
  格式**：`drivers/iommu/intel/iommu.h:56-57` 的 `ADDR_WIDTH_5LEVEL`(57) 与 `ADDR_WIDTH_4LEVEL`(48)
  就是 x86 五级/四级分页的地址宽度。这也是 nested 翻译在 VT-d 上几乎零成本的根本
  原因：硬件用同一套 walker 逻辑。

`first_pte_in_page()` / `nr_pte_to_next_page()` 解决"一个 4K 表页有 512 项，
不能跨表页连续走"：

```c
/* 来源: drivers/iommu/intel/iommu.h:874-883 */
static inline bool first_pte_in_page(struct dma_pte *pte)
{
	return IS_ALIGNED((unsigned long)pte, VTD_PAGE_SIZE);
}

static inline int nr_pte_to_next_page(struct dma_pte *pte)
{
	return first_pte_in_page(pte) ? BIT_ULL(VTD_STRIDE_SHIFT) :
		(struct dma_pte *)ALIGN((unsigned long)pte, VTD_PAGE_SIZE) - pte;
}
```

即"到下一个表页边界的项数"，从表页开头算起正好 512。**用指针对齐当循环计数器**，
让 `__domain_mapping()` 批量填连续 PTE 时省掉一次除法。三个数字互相自洽：
`struct dma_pte` 只有一个 `u64 val`（`drivers/iommu/intel/iommu.h:835-837`）即 8 字节，一张 4K 表页
放 4096/8 = 512 项，正好等于 `BIT_ULL(VTD_STRIDE_SHIFT)`，也等于一级 9 位索引的
取值空间 —— **VT-d 的"stride 9"意思就是"每个表页 512 项"**，与 x86 CPU 页表同构。

之所以还需要这个函数，是因为 `pte` 指针**不一定从表页开头进来**：
`pfn_to_dma_pte()` 会返回区间中间那一项，此后调用方连续 `pte++` 填充，
必须知道何时越出当前 4K 表页 —— 页边界之外的下一个物理页属于**另一张表**，
继续走会写坏别人的页表。返回值就是"本表内还能填几项"。

---

## T.5 SMMUv3：同一件事在通用库里长了另一套参数

`drivers/iommu/io-pgtable-arm.c` 是 ARM 的 LPAE 页表库，被 SMMUv3 与 GPU 的
MMU/DMU 共用。它与 VT-d 的差别恰好说明了 T.3 那层抽象为什么必须存在。

### T.5.1 stride 不是常数

```c
/* 来源: drivers/iommu/io-pgtable-arm.c:918-923 */
	pg_shift = __ffs(cfg->pgsize_bitmap);
	data->bits_per_level = pg_shift - ilog2(sizeof(arm_lpae_iopte));

	va_bits = cfg->ias - pg_shift;
	levels = DIV_ROUND_UP(va_bits, data->bits_per_level);
	data->start_level = ARM_LPAE_MAX_LEVELS - levels;
```

`bits_per_level` 由**页粒度**推出：4K → `12 - 3 = 9`（与 VT-d 相同）；
16K → `14 - 3 = 11`；64K → `16 - 3 = 13`。一个数同时决定了 stride、每表项数、
每级覆盖的地址宽度。对照 T.4.1 里 VT-d 把 9 写死成两个宏 —— 差别是架构性的：
VT-d 二级翻译只接受 4K 粒度，ARM LPAE 三档粒度全支持，所以必须算。

`ARM_LPAE_MAX_LEVELS` = 4（`io-pgtable-arm.c:28`）、`ARM_LPAE_MAX_ADDR_BITS` = 52
（`:26`）。**ARM LPAE 没有第 5 级、也不支持 57 位输入地址**，与 x86 的
5-level EPT / `AGAW=3` 不是一回事。这是把 GB300 这类 Grace 平台跟 x86 平台做
IOVA 空间规划对照时必须知道的上限。

`data->start_level = MAX_LEVELS - levels` 是 VT-d `iommu_skip_agaw()` 的对应物
—— 树可以从中间某级开始。区别在**输入**：VT-d 的级数由硬件上报的 AGAW 决定，
LPAE 的级数由软件申请的 `cfg->ias` 反推。后者才允许"SMMU 支持 52 位但这个设备
只需要 40 位"这种裁剪；代价是 `ias` 超过 52 直接 `return NULL`
（`io-pgtable-arm.c:908-912`）。

级内索引与块大小都由一个宏派生：

```c
/* 来源: drivers/iommu/io-pgtable-arm.c:41-43 */
#define ARM_LPAE_LVL_SHIFT(l,d)						\
	(((ARM_LPAE_MAX_LEVELS - (l)) * (d)->bits_per_level) +		\
	ilog2(sizeof(arm_lpae_iopte)))
```

`ARM_LPAE_BLOCK_SIZE(l,d)`（`:65`）与 `ARM_LPAE_LVL_IDX(a,l,d)`（`:60`）都建立在它
之上。注意**编号方向与 VT-d 正好相反**：LPAE 里 `lvl` 越大越靠近叶子
（`__arm_lpae_map` 递归传的是 `lvl + 1`，`:441`），而 VT-d 里 `level` 越小越靠近
叶子（`agaw_to_level()` 给的是根、`level - 1` 是往下）。两边对照读代码时这是第一
个要换算的东西。

### T.5.2 map 是一个自递归

```c
/* 来源: drivers/iommu/io-pgtable-arm.c:388-443（节选） */
static int __arm_lpae_map(struct arm_lpae_io_pgtable *data, unsigned long iova,
			  phys_addr_t paddr, size_t size, size_t pgcount,
			  arm_lpae_iopte prot, int lvl, arm_lpae_iopte *ptep, ...)
{
	size_t block_size = ARM_LPAE_BLOCK_SIZE(lvl, data);
	int ret = 0, num_entries, max_entries, map_idx_start;

	map_idx_start = ARM_LPAE_LVL_IDX(iova, lvl, data);
	ptep += map_idx_start;

	/* If we can install a leaf entry at this level, then do so */
	if (size == block_size) {
		max_entries = arm_lpae_max_entries(map_idx_start, data);
		num_entries = min_t(int, pgcount, max_entries);
		ret = arm_lpae_init_pte(data, iova, paddr, prot, lvl, num_entries, ptep);
		if (!ret)
			*mapped += num_entries * size;
		return ret;
	}
	...
	if (pte && !iopte_leaf(pte, lvl, data->iop.fmt)) {
		cptep = iopte_deref(pte, data);
	} else if (pte) {
		/* We require an unmap first */
		WARN_ON(!selftest_running);
		return -EEXIST;
	}

	/* Rinse, repeat */
	return __arm_lpae_map(data, iova, paddr, size, pgcount, prot, lvl + 1,
			      cptep, gfp, mapped);
```

与 VT-d 迭代式的 `pfn_to_dma_pte()` 相比，这里是"每级判一次，能落叶子就落，
否则装表往下递归"。三处细节对实践有用：

- `if (size == block_size)` —— **只有正好等于本级块大小才允许在这一级落叶子**。
  `iommu_pgsize()` 已保证 `size` 必是位图里某一档，所以正常路径上这个等式在某个
  `lvl` 必然成立一次。
- `arm_lpae_max_entries()`（`:207-211`）处理 **PGD 拼接（concatenation）**：
  根表可以小于一页或多于一页，`ptes_per_table - (i & (ptes_per_table - 1))`
  给出"从当前索引到本表页末尾还剩几项"，用来限制一次能连续填几个 block。
  VT-d 无对应概念（根表恒为一页）。
- `else if (pte) { WARN_ON(!selftest_running); return -EEXIST; }` ——
  **map 到已有映射上是 `WARN_ON` + `-EEXIST`，不是原地覆盖**。而 VT-d 的
  `__domain_mapping()` 允许改属性。也就是说 `iommu_map()` 在 arm-lpae 后端上不
  幂等，重复 map 同一区间会打出内核警告。用户态直通路径看不见这个差异，因为
  IOMMUFD 的 `IOAS_MAP` 在 core 的 `iommu_map()`（`drivers/iommu/iommu.c:2510`）之外自己先做了
  区间冲突检查；只有自己写驱动直接调 `iommu_map()` 才会踩到。

叶子位的对应关系：

```c
/* 来源: drivers/iommu/io-pgtable-arm.c:312-315 */
	if (data->iop.fmt != ARM_MALI_LPAE && lvl == ARM_LPAE_MAX_LEVELS - 1)
		pte |= ARM_LPAE_PTE_TYPE_PAGE;
	else
		pte |= ARM_LPAE_PTE_TYPE_BLOCK;
```

`ARM_LPAE_PTE_TYPE_BLOCK = 1`（`:71`）、`ARM_LPAE_PTE_TYPE_PAGE = 3`（`:73`），
`iopte_leaf()` 的判定就在 `:167-171`。**"2MB 大页"在 ARM 里是 Block 描述符，与
VT-d 用 `DMA_PTE_LARGE_PAGE`（bit 7）在中间级位置放叶子，是同一种设计的两种编码。**

### T.5.3 S1 与 S2 的属性编码同样是两套

```c
/* 来源: drivers/iommu/io-pgtable-arm.c:449-464（arm_lpae_prot_to_pte 内） */
	if (data->iop.fmt == ARM_64_LPAE_S1 ||
	    data->iop.fmt == ARM_32_LPAE_S1) {
		pte = ARM_LPAE_PTE_nG;
		if (!(prot & IOMMU_WRITE) && (prot & IOMMU_READ))
			pte |= ARM_LPAE_PTE_AP_RDONLY;
		else if (data->iop.cfg.quirks & IO_PGTABLE_QUIRK_ARM_HD)
			pte |= ARM_LPAE_PTE_DBM;
		if (!(prot & IOMMU_PRIV))
			pte |= ARM_LPAE_PTE_AP_UNPRIV;
	} else {
		pte = ARM_LPAE_PTE_HAP_FAULT;
		if (prot & IOMMU_READ)
			pte |= ARM_LPAE_PTE_HAP_READ;
		if (prot & IOMMU_WRITE)
			pte |= ARM_LPAE_PTE_HAP_WRITE;
	}
```

S1 用 `AP`（`ARM_LPAE_PTE_AP_UNPRIV` = bit 6、`AP_RDONLY` = bit 7，`:96-99`）+ `nG`；
S2 用 `S2AP`（代码里叫 `HAP`，`HAP_READ = 1<<6`、`HAP_WRITE = 2<<6`，`:106-108`）。
**与 T.4.4 里 VT-d 一级/二级两套位定义完全对称**：两个架构都把 stage-1 做成"带
特权级的进程式视图"、stage-2 做成"只有读写的字母表"。
`IO_PGTABLE_QUIRK_ARM_HD` 对应 FEAT_HAFDBS（硬件 A/D 更新），位是
`ARM_LPAE_PTE_DBM = 1<<51`（`:79`）；它与 VT-d 的 `DMA_SL_PTE_DIRTY`（bit 9）一起
构成两个后端的 dirty-log 基础。

一句话收口：**T.3 的 `pgsize_bitmap` 协商之所以必须存在，是因为后端树形状的差异大到
只能靠"我支持哪些 2 的幂"这一种语言传话**。VT-d 用常数 9 + AGAW 索引，LPAE 用
`bits_per_level` + `ias` 反推，两者都不向 core 暴露自己的结构。

---

## T.6 自检问题

1. 同一个 RID 的两张表（root entry、context entry）各按什么索引？scalable mode 下
   `devfn` 为什么要先乘 2？
   （T.2：root table 按总线号，`drivers/iommu/intel/iommu.c:483` 起；scalable mode
   context entry 是 128 位，devfn 高低半区各占一张表，索引前要 `devfn *= 2`）
2. `CONTEXT_TT_DEV_IOTLB` 与 `CONTEXT_TT_MULTI_LEVEL` 的差别是什么？哪个不翻译？
   （T.2.2：两者都做二级翻译，只是前者允许设备带 ATS/Device-TLB；
   `drivers/iommu/intel/iommu.h:59-60`。不翻译的是 `IOMMU_DOMAIN_IDENTITY`，
   见 [Q3 D.4](domains.md)）
3. 为什么 SVA 能直接把进程页表挂给设备，不需要任何格式转换？
   （T.1：一级翻译的页表格式与 Intel 64 处理器 64 位模式相同，规范 3.6；
   `DMA_FL_PTE_*` 与 x86 页表位一一对应，T.4.4）
4. `iommu_map()` 传入未对齐地址会怎样？怎么在日志里认出来？
   （T.3.2：直接 `-EINVAL` 并打 `pr_err "unaligned: ..."`，`drivers/iommu/iommu.c:2463-2474`；
   注意 `min_pagesz` 不恒等于 4096）
5. 为什么 `pgsize_bitmap` 永远没有 512GB 这一档？
   （T.4.1：stride 恒为 9，跳三级是 512GB，但 `MAX_AGAW_PFN_WIDTH = 52` 封顶，
   硬件不提供该中间级位）
6. 同一段连续物理内存，映射进页表后出现"中间 2MB、两头 4K"的形状，正常吗？
   （T.4.2：正常。`pages >>= 9` 后 `if (!pages) break;` —— 长度不够也掉回小页，
   与对齐无关）
7. `switch_to_super_page()` 里 `dma_pte_free_pagetable()` 写在
   `cache_tag_flush_range()` 之前，是否违反"先失效再释放"？
   （T.4.3：不违反。前者只摘指针挂收集链表，真正 `free_page()` 在失效之后；
   顺序细节见 [Q5](invalidation.md)）
