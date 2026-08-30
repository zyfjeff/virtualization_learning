# Q5：`iommu_unmap()` 之后，硬件是不是立刻就不用旧翻译了？

**不是。** 页表拆掉时，旧表项只是从内存里消失；但 IOMMU 自己的 IOTLB、
paging-structure cache，以及 PCIe 设备用 ATS 缓存的 **Device-TLB**，都可能还攥着
旧翻译。`iommu_unmap()` 的语义必须是：**返回时，所有缓存里都不会再有任何一条翻译
能命中这个地址**——所以它要同步等硬件完成失效，哪怕缓存副本躺在芯片外、一条异步
链路的另一端。

**为什么值得问**：CPU 侧 `invlpg` 的时序设计者是自己，IOMMU 侧没有这种便利。这条
同步语义决定了 DMA 数据路径的延迟下界，解释了 `iommu.strict` 为什么是一个值得存在
的旋钮、失效队列的排队为什么会成为高并发 unmap 的长尾来源，也决定了 nested 直通在
设备侧能有多贵。

**本文怎么展开**：I.1/I.2 先在 core 层回答问题（完整失效链、粒度怎么选）；
I.3–I.8 深入 Intel 失效队列本身（描述符编码、槽位记账、完成语义、错误恢复、顺序
契约、`cache_tag`）；I.9–I.11 处理三个延伸面——链路另一端的 Device-TLB、反方向的
PRI/PRQ、以及把失效推迟的 strict/lazy；I.12–I.14 是 SMMUv3 与 AMD-Vi 的同题对照；
I.15 给观测手段，I.16 自检。

> 源码基线：`/root/code/linux-6.12.93`。所有 `path:line` 均可用
> [`practice/check-citations.sh`](practice/check-citations.sh) 复核。

## 📖 目录

- [I.1 一次 `iommu_unmap()` 的完整失效链](#i1-一次-iommu_unmap-的完整失效链)
- [I.2 粒度选择：PSI、DSI、全域与三个回落](#i2-粒度选择psidsi全域与三个回落)
- [I.3 一个地址范围如何变成描述符：三套互不兼容的编码](#i3-一个地址范围如何变成描述符三套互不兼容的编码)
- [I.4 Intel 失效队列的槽位宽度与空间记账](#i4-intel-失效队列的槽位宽度与空间记账)
- [I.5 完成语义：Wait Descriptor 与关中断自旋](#i5-完成语义wait-descriptor-与关中断自旋)
- [I.6 错误恢复：IQE / ITE / ICE 三条分岔](#i6-错误恢复iqe--ite--ice-三条分岔)
- [I.7 顺序契约：谁必须排在谁前面](#i7-顺序契约谁必须排在谁前面)
- [I.8 cache_tag：域到缓存位置的反向索引](#i8-cache_tag域到缓存位置的反向索引)
- [I.9 Device-TLB 失效：代价在链路另一端](#i9-device-tlb-失效代价在链路另一端)
- [I.10 PRI 与 PRQ：唯一能把缺页变成可恢复事件的路](#i10-pri-与-prq唯一能把缺页变成可恢复事件的路)
- [I.11 strict 与 lazy：`DMA_FQ` 到底延迟了什么](#i11-strict-与-lazydma_fq-到底延迟了什么)
- [I.12 SMMUv3：无锁乐观预留与 CMD_SYNC](#i12-smmuv3无锁乐观预留与-cmd_sync)
- [I.13 AMD-Vi：8KB 环形队列与惰性完成等待](#i13-amd-vi8kb-环形队列与惰性完成等待)
- [I.14 三家横向对照](#i14-三家横向对照)
- [I.15 观测：tracepoint 与延迟直方图](#i15-观测tracepoint-与延迟直方图)
- [I.16 自检问题](#i16-自检问题)

---

## 规范可用性声明

| 后端 | 一手规范 | 本仓库是否具备 | 本文的处理 |
|---|---|---|---|
| Intel VT-d | `intel-vtd.pdf`（Rev 4.1） | ✅ | 描述符字段与排序规则均引章节号 / 表号 |
| ARM SMMUv3 | Arm IHI 0070（SMMUv3 架构规范） | ✅ `arm-smmuv3.pdf`（版本未在文中标注） | 命令格式**优先按 Linux 代码与注释**陈述，规范用于核对字段语义 |
| AMD-Vi | AMD I/O Virtualization Technology 规范 | ❌ 无 | 同上，仅陈述代码事实 |
| ATS / Device-TLB 范围语义 | PCIe Base Spec 的 ATS 章节 | ✅ `pcie-base-spec-r6.0.pdf`（VT-d 把 S 位语义**委托**给 ATS 规范） | 见 I.3.3，只给位运算事实 + 标出委托边界 |

凡是只能靠代码说话的地方，本文会明确写"代码如此，规范语义未核实"。

---

## I.1 一次 `iommu_unmap()` 的完整失效链

回答 Q5 先看 core 层的契约，就三行：

```c
/* 来源: drivers/iommu/iommu.c:2594-2605 */
size_t iommu_unmap(struct iommu_domain *domain,
		   unsigned long iova, size_t size)
{
	struct iommu_iotlb_gather iotlb_gather;
	size_t ret;

	iommu_iotlb_gather_init(&iotlb_gather);
	ret = __iommu_unmap(domain, iova, size, &iotlb_gather);
	iommu_iotlb_sync(domain, &iotlb_gather);

	return ret;
}
```

`__iommu_unmap()`（`drivers/iommu/iommu.c:2540`）在循环里按 `iommu_pgsize()` 逐段调
`ops->unmap_pages()`，并把每段范围**累积**进 `iotlb_gather`
（`include/linux/iommu.h:345-351`：`start/end/pgsize/freelist/queued`）。
`iommu_iotlb_gather_add_page()`（`include/linux/iommu.h:925-940`）有个反直觉行为：如果新范围与已
累积范围**不相交、或页大小不同**，它会**先把 gather 同步掉再复用**。所以"批量解映射"
只有在地址连续且同尺寸时才真省失效——写批处理代码时的性能悬崖就在这。

`iommu_iotlb_sync()` 落到 `intel_iommu_tlb_sync()`：

```c
/* 来源: drivers/iommu/intel/iommu.c:3794-3801 */
static void intel_iommu_tlb_sync(struct iommu_domain *domain,
				 struct iommu_iotlb_gather *gather)
{
	cache_tag_flush_range(to_dmar_domain(domain), gather->start,
			      gather->end, list_empty(&gather->freelist));
	iommu_put_pages_list(&gather->freelist);
}
```

**先失效，后释放页表页**，顺序不能反。原因写在 `drivers/iommu/intel/iommu.c:1001-1004`：

```c
/* 来源: drivers/iommu/intel/iommu.c:1001-1004 */
/* When a page at a given level is being unlinked from its parent, we don't
   need to *modify* it at all. All we need to do is make a list of all the
   pages which can be freed just as soon as we've flushed the IOTLB and we
   know the hardware page-walk will no longer touch them.
```

`gather->freelist` 装的是被拆掉的中间层页表页。硬件 page walk 可能正走在这些页上——
在失效确认完成前释放它们，等于让硬件读写一块已交回 buddy 的内存。这是"unmap 必须同步"
整个论证里最硬的一块，也意味着**页表页的释放时机本身就是失效正确性的证据**。

第三个实参 `list_empty(&gather->freelist)` 就是 VT-d 的 **IH（Invalidation Hint）** 位。
规范 6.5.2.3 原文：

> "For software usages that updates only the leaf SS-PTEs, the second-stage mappings in
> the paging-structure-caches can be preserved by specifying the Invalidation Hint
> field value of 1."

Linux 的判断完全照此：freelist 为空 ⇒ 只改了叶子 PTE ⇒ 可以带 IH=1 让硬件保留
paging-structure-cache；freelist 非空 ⇒ 中间层被动过 ⇒ IH=0。这个布尔值随后被塞进
`addr` 的 bit 0 一路传下去——那条位通道的完整走读见 I.3.1。

一句话总结本节：**`iommu_unmap()` 的"同步"不是等页表拆完，而是等 `cache_tag_flush_range()`
把失效描述符送到硬件并确认完成**。拆页表只是前置动作。下面 I.2 讲失效粒度怎么选，
I.3 起深入描述符与队列本身。

---

## I.2 粒度选择：PSI、DSI、全域与三个回落

`cache_tag_flush_range()`（`drivers/iommu/intel/cache.c:437-478`）遍历 `domain->cache_tags`——这是
"这个域被哪些 IOMMU 单元、哪些 RID/PASID 引用"的账本（账本结构与反向索引的完整走读见 I.8）。
它决定每次失效用哪一档粒度。

```c
/* 来源: drivers/iommu/intel/cache.c:369-380（节选） */
	/*
	 * Fallback to domain selective flush if no PSI support or the size
	 * is too big.
	 */
	if (!cap_pgsel_inv(iommu->cap) ||
	    mask > cap_max_amask_val(iommu->cap) || pages == -1) {
		addr = 0;
		mask = 0;
		ih = 0;
		type = DMA_TLB_DSI_FLUSH;
	}
```

规范 6.5.1.2 把 IOTLB 失效分成三档——Global / Domain-Selective /
Page-Selective-within-Domain，能降到哪一档由 `cap` 里的 `NWA`（Number of Address
Masks）决定。三个回落条件：

| 触发条件 | 为什么退 |
|---|---|
| `!cap_pgsel_inv()` | 硬件根本不支持页选择性失效 |
| `mask > cap_max_amask_val()` | 要失效的范围超过地址掩码位宽 |
| `pages == -1` | 本来就是全域失效（`cache_tag_flush_all()`，`drivers/iommu/intel/cache.c:484`） |

回落时注意 `addr/mask/ih` 一起清零——这正是 I.3.1 里"回落 DSI 时哪些字段必须一起清"
的出处。

**nested 翻译的失效在这里最贵**：

```c
/* 来源: drivers/iommu/intel/cache.c:460-469 */
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

改第二级翻译会让设备里所有"第一级套第二级"的缓存作废，而硬件无法从 Device-TLB 反查
哪些条目用了被改的 S2 项，只能整台设备全刷。这是 nested 直通设备侧代价高的根本原因，
跟 IOTLB 命中率无关（此分支在 I.8 里从 `cache_tag` 视角再走一遍）。

还有一条容易漏的：**建表方向也可能要失效**。`cache_tag_flush_range_np()`（`drivers/iommu/intel/cache.c:524`）
专门处理"新加了映射"，注释直接引用规范 6.1 与 6.8：

```c
/* 来源: drivers/iommu/intel/cache.c:513-522 */
/*
 * Invalidate a range of IOVA when new mappings are created in the target
 * domain.
 *
 * - VT-d spec, Section 6.1 Caching Mode: When the CM field is reported as
 *   Set, any software updates to remapping structures other than first-
 *   stage mapping requires explicit invalidation of the caches.
 * - VT-d spec, Section 6.8 Write Buffer Flushing: For hardware that requires
 *   write buffer flushing, software must explicitly perform write-buffer
 *   flushing, if cache invalidation is not required.
 */
```

`CM=1`（Caching Mode）的硬件上**连 map 都是同步的**；`use_first_level` 的域则跳过
失效只做 `iommu_flush_write_buffer()`。看到"开了 `iommu.strict` 还没变快"时先查
`cap_caching_mode()`。

大页合并同样要失效。`switch_to_super_page()`（`drivers/iommu/intel/iommu.c:1762`）在拆小页表换超页
时调 `cache_tag_flush_range(domain, ..., 0)`（`drivers/iommu/intel/iommu.c:1783`）——最后一个实参写死 0，即
**IH=0**，因为它动的正是中间层表项。对照本节开头 `list_empty(freelist)` 的自动判断，
能看出 Linux 对 IH 的用法是逐处手工推导的，没有统一规则可套。

---

## I.3 一个地址范围如何变成描述符：三套互不兼容的编码

最反直觉的第一件事：**同一个 Intel 驱动内部就有三套范围编码，AMD 又自成一派，
ARM 第三派**。它们不是"同一思路的三种写法"，而是三种**不同的信息载体**——有的把
长度放进独立字段，有的把长度藏在地址低位，有的用"最高差异位"表达。写通用工具
（比如统计失效覆盖范围的脚本）时把它们当一回事必然算错。

### I.3.1 编码一：legacy IOTLB 描述符的 `AM` 字段 + 藏在 bit 0 的 `IH`

```c
/* 来源: drivers/iommu/intel/iommu.h:1084-1103 */
static inline void qi_desc_iotlb(struct intel_iommu *iommu, u16 did, u64 addr,
				 unsigned int size_order, u64 type,
				 struct qi_desc *desc)
{
	u8 dw = 0, dr = 0;
	int ih = addr & 1;
	...
	desc->qw1 = QI_IOTLB_ADDR(addr) | QI_IOTLB_IH(ih)
		| QI_IOTLB_AM(size_order);
}
```

第二行 `int ih = addr & 1;` 是整段代码里最容易看漏的一笔：**入参 `addr` 的 bit 0
根本不是地址的一部分，而是 IH（Invalidation Hint）**。调用方用最低位当传声筒把
IH 带进来，这里拆出来放进 `QI_IOTLB_IH` 字段，再把地址右移规整。它的另一端在
`drivers/iommu/intel/cache.c:384`：

```c
/* 来源: drivers/iommu/intel/cache.c:383-387 */
	if (ecap_qis(iommu))
		qi_batch_add_iotlb(iommu, tag->domain_id, addr | ih, mask, type,
				   domain->qi_batch);
	else
		__iommu_flush_iotlb(iommu, tag->domain_id, addr | ih, mask, type);
```

`addr | ih` —— 从这里"塞"，到 `qi_desc_iotlb` 里"取"。这种把元数据寄存在另一个
参数最低位的写法，中间任何一次 `addr >>= 1` 或对齐操作都会把它毁掉，是维护时
的高危点。搜索 `ih` 是搜不到的，必须跟着这条位通道读。

规范侧 IH 的定义（`intel-vtd.pdf` Section 6.5.2.3，IOTLB 描述符字段表）：

> **Invalidation Hint (IH)**: For page-selective-within-domain invalidations, the
> Invalidation Hint specifies if the second-stage mappings cached in the
> paging-structure-caches that controls the specified address/mask range needs to
> be invalidated or not. For software usages that updates only the leaf SS-PTEs,
> the second-stage mappings in the paging-structure-caches can be preserved by
> specifying the Invalidation Hint field value of 1. **This field is ignored by
> hardware for global and domain-selective invalidations.**

最后一句是 Linux 两处强制 `ih = 0` 的依据：`drivers/iommu/intel/cache.c:375-381` 在回落到 DSI 时
连 `ih` 一起清零；`drivers/iommu/intel/iommu.c:1757-1784` 的
`switch_to_super_page()` 拆子树时也显式传 `0`（粒度选择与回落条件见 I.4——那里讲
"什么时候回落 DSI"，这里讲"回落时哪些字段必须一起清"）。

### I.3.2 编码二：PASID-based IOTLB 的 `AM`，但用 `ALIGN_DOWN` 保正确性

```c
/* 来源: drivers/iommu/intel/iommu.h:1134-1148 */
	} else {
		int mask = ilog2(__roundup_pow_of_two(npages));
		unsigned long align = (1ULL << (VTD_PAGE_SHIFT + mask));

		if (WARN_ON_ONCE(!IS_ALIGNED(addr, align)))
			addr = ALIGN_DOWN(addr, align);

		desc->qw0 = QI_EIOTLB_PASID(pasid) |
				QI_EIOTLB_DID(did) |
				QI_EIOTLB_GRAN(QI_GRAN_PSI_PASID) |
				QI_EIOTLB_TYPE;
		desc->qw1 = QI_EIOTLB_ADDR(addr) |
				QI_EIOTLB_IH(ih) |
				QI_EIOTLB_AM(mask);
	}
```

与编码一的三点差别，逐条都是实操差异：

1. **`ih` 是正式形参**（`qi_desc_piotlb(..., bool ih, ...)`，`:1124-1127`），不寄生
   在地址位上。同一驱动里两套约定并存，读代码时必须先确认走的是哪条路径
   （`use_first_level` 走 piotlb，见 `drivers/iommu/intel/cache.c:365-369`）。
2. **长度由页数换算**：`mask = ilog2(roundup_pow_of_two(npages))`，先把范围向上
   取整到 2 的幂再取对数。
3. **不满足对齐时 `ALIGN_DOWN` 而不是报错**：向上取整 + 向下对齐 = **宁可多刷
   也不漏刷**，方向是安全的。但 `WARN_ON_ONCE` 意味着这在正常路径上不该发生，
   真打印了就说明调用方给的 `iova/npages` 组合有问题（`npages == -1` 是
   PASID-selective 的哨兵值，走 `:1128-1133` 另一分支，不在此列）。

### I.3.3 编码三：Device-TLB 的 `S` 位 —— 长度藏在地址的**零**位里

这是三套里最诡异的。`qi_desc_dev_iotlb()` 的 `mask` 分支：

```c
/* 来源: drivers/iommu/intel/iommu.h:1108-1113 */
	if (mask) {
		addr |= (1ULL << (VTD_PAGE_SHIFT + mask - 1)) - 1;
		desc->qw1 = QI_DEV_IOTLB_ADDR(addr) | QI_DEV_IOTLB_SIZE;
	} else {
		desc->qw1 = QI_DEV_IOTLB_ADDR(addr);
	}
```

`addr |= (1 << (12 + mask - 1)) - 1` 把从 bit 12 开始的 `12+mask-1` 位全部置 1。
乍看像 off-by-one（为什么不是 `mask` 位？），答案在 PASID 变体里那份权威注释：

```c
/* 来源: drivers/iommu/intel/iommu.h:1162-1170 */
	/*
	 * If S bit is 0, we only flush a single page. If S bit is set,
	 * The least significant zero bit indicates the invalidation address
	 * range. VT-d spec 6.5.2.6.
	 * e.g. address bit 12[0] indicates 8KB, 13[0] indicates 16KB.
	 * size order = 0 is PAGE_SIZE 4KB
	 * Max Invs Pending (MIP) is set to 0 for now until we have DIT in
	 * ECAP.
	 */
```

**"最低的 0 位"表达长度**，不是"最高的 1 位"。于是那段代码的完整逻辑是
（`drivers/iommu/intel/iommu.h:1175-1190`）：

```c
/* 来源: drivers/iommu/intel/iommu.h:1176-1190（节选） */
	desc->qw1 = QI_DEV_EIOTLB_ADDR(addr);

	if (size_order) {
		/*
		 * Existing 0s in address below size_order may be the least
		 * significant bit, we must set them to 1s to avoid having
		 * smaller size than desired.
		 */
		desc->qw1 |= GENMASK_ULL(size_order + VTD_PAGE_SHIFT - 1,
					VTD_PAGE_SHIFT);
		/* Clear size_order bit to indicate size */
		desc->qw1 &= ~mask;
		/* Set the S bit to indicate flushing more than 1 page */
		desc->qw1 |= QI_DEV_EIOTLB_SIZE;
	}
```

三步各自解决一个坑：先把 bit 12 到 bit `12+size_order-1` **全填 1**（否则地址本身
的 0 位会被硬件误读成"更小的范围"），再**单独清掉边界那一位**当作"最低 0 位"
标记，最后置 `S`。少任何一步都会刷错范围。

对照 I.3.1/I.3.2 就能看出这是**完全不同量**：那里长度是独立字段，这里长度是
地址值的一部分。**拿一个 Device-TLB 描述符的 `qw1` 去按地址解析，会得到一个
比真实范围大得多的假地址**。

> **委托边界（未核实）**：`VT-d spec 6.5.2.6` 这句话本身把 S 位置 1 时的语义
> 指向 PCIe ATS 规范的定义（`intel-vtd.pdf` Section 6.5.2.5：Device-TLB 失效
> 请求遵循 ATS 的 Invalidate Request 格式）。因此"最低 0 位"的**规范性**表述
> 需要 ATS 规范原文佐证，本文只到代码与 VT-d 注释为止。另外注意
> `size_order` 的上界是 `MAX_AGAW_PFN_WIDTH` = 52（`drivers/iommu/intel/iommu.h:893`），
> `cache_tag_flush_devtlb_all()` 正是用这个值表达"整设备全刷"
> （`drivers/iommu/intel/cache.c:426-427`）。

同一段还有两个容易忽略的细节：

```c
/* 来源: drivers/iommu/intel/iommu.h:1115-1117 */
	if (qdep >= QI_DEV_IOTLB_MAX_INVS)
		qdep = 0;
```

`qdep`（Invalidate Queue Depth，来自 `pci_ats_queue_depth()`）被截断为 0。配合
上面注释里那句 "Max Invs Pending (MIP) is set to 0 for now until we have DIT in
ECAP"——**Linux 目前不利用设备的失效并发能力**，失效队列深度信息基本白读。这是
一个可验证的性能改进空间，也是一个"为什么我的 ATS 设备失效没并行"的答案。

### I.3.4 范围→(addr, pages, mask) 的换算核心：`calculate_psi_aligned_address`

上面三种编码的长度参数都源自同一个函数：

```c
/* 来源: drivers/iommu/intel/cache.c:248-283（节选） */
static unsigned long calculate_psi_aligned_address(unsigned long start,
						   unsigned long end,
						   unsigned long *_pages,
						   unsigned long *_mask)
{
	unsigned long pages = aligned_nrpages(start, end - start + 1);
	unsigned long aligned_pages = __roundup_pow_of_two(pages);
	unsigned long bitmask = aligned_pages - 1;
	unsigned long mask = ilog2(aligned_pages);
	unsigned long pfn = IOVA_PFN(start);

	/*
	 * PSI masks the low order bits of the base address. If the
	 * address isn't aligned to the mask, then compute a mask value
	 * needed to ensure the target range is flushed.
	 */
	if (unlikely(bitmask & pfn)) {
		unsigned long end_pfn = pfn + pages - 1, shared_bits;
		...
		shared_bits = ~(pfn ^ end_pfn) & ~bitmask;
		mask = shared_bits ? __ffs(shared_bits) : MAX_AGAW_PFN_WIDTH;
		aligned_pages = 1UL << mask;
	}

	*_pages = aligned_pages;
	*_mask = mask;

	return ALIGN_DOWN(start, VTD_PAGE_SIZE << mask);
}
```

值得逐层拆的几点：

- **快路径**：范围向上取整到 2 的幂，且起始 PFN 正好对齐这个 2 的幂
  （`!(bitmask & pfn)`）→ 直接 `mask = ilog2(aligned_pages)`，一次搞定。
- **慢路径的几何直觉**：`pfn ^ end_pfn` 是首尾不同的位；取反后 `~(pfn ^ end_pfn)`
  是首尾**相同**的位；再 `& ~bitmask` 排掉那些"反正会被掩掉"的低估位。
  剩下的最低置位 `__ffs(shared_bits)` 就是**能覆盖整个范围的最小对齐块的数量级**。
  注释里 "the only way bits higher than bitmask can differ ... is by carrying"
  是这条推导的证明。
- **兜底 `MAX_AGAW_PFN_WIDTH`**：`shared_bits == 0` 意味着首尾**没有任何**公共高位
  ——这个范围大到无法用任何 PSI 掩码表达，于是 `mask = 52`，等价"全域"。
  下游 `cache_tag_flush_iotlb()` 的 `mask > cap_max_amask_val(iommu->cap)` 判据
  （`drivers/iommu/intel/cache.c:375-381`）会把它回落成 DSI。
- 返回值是 `ALIGN_DOWN`，**保证 bit `12+mask` 为 0**。这正是 I.3.3 里
  Device-TLB 用"最低 0 位"表达长度时那个边界位一定存在的前提。也就是说
  `qi_desc_dev_iotlb()` 里看着像 off-by-one 的 `mask - 1`，其实建立在
  这个 ALIGN_DOWN 之上——两处代码相隔两个文件，单独看哪一处都像错的。

### I.3.5 编码四（AMD）：`msb_diff` —— 把差值的最高位以下全填 1

```c
/* 来源: drivers/iommu/amd/iommu.c:1241-1277（节选） */
static inline u64 build_inv_address(u64 address, size_t size)
{
	u64 pages, end, msb_diff;

	pages = iommu_num_pages(address, size, PAGE_SIZE);

	if (pages == 1)
		return address & PAGE_MASK;

	end = address + size - 1;

	/*
	 * msb_diff would hold the index of the most significant bit that
	 * flipped between the start and end.
	 */
	msb_diff = fls64(end ^ address) - 1;

	/*
	 * Bits 63:52 are sign extended. If for some reason bit 51 is different
	 * between the start and the end, invalidate everything.
	 */
	if (unlikely(msb_diff > 51)) {
		address = CMD_INV_IOMMU_ALL_PAGES_ADDRESS;
	} else {
		/*
		 * The msb-bit must be clear on the address. Just set all the
		 * lower bits.
		 */
		address |= (1ull << msb_diff) - 1;
	}

	/* Clear bits 11:0 */
	address &= PAGE_MASK;

	/* Set the size bit - we flush more than one 4kb page */
	return address | CMD_INV_IOMMU_PAGES_SIZE_MASK;
}
```

同一族设计，三处与 Intel 相反：

| 维度 | Intel Device-TLB | AMD INV_IOMMU_PAGES |
|---|---|---|
| 长度载体 | 地址里**最低的 0 位** | 地址里**最高的翻转位**以下**全 1** |
| 边界位本身 | 单独清零表达 size | 保持 0（"must be clear on the address"） |
| 表达不了时 | `mask = 52` 交给上层回落 DSI | `msb_diff > 51` 直接 `CMD_INV_IOMMU_ALL_PAGES_ADDRESS` = `0x7fffffffffffffff`（`drivers/iommu/amd/amd_iommu_types.h:211`） |

AMD 这套的代价是**必然过度失效**：只要首尾跨过某个 2^k 边界，就把整个 2^k 块刷掉。
`fls64(end ^ address) - 1` 增长得非常快——刷 8KB 未对齐范围可能刷掉 64KB。这个
代价在真实代码里有明确的症状，见 I.13.4 的 NpCache 分支。

### I.3.6 编码五（ARM）：`num × 2^scale` 的 5 位分段拆解

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:2198-2225（节选） */
	while (iova < end) {
		if (smmu->features & ARM_SMMU_FEAT_RANGE_INV) {
			/*
			 * On each iteration of the loop, the range is 5 bits
			 * worth of the aligned size remaining.
			 * The range in pages is:
			 *
			 * range = (num_pages & (0x1f << __ffs(num_pages)))
			 */
			unsigned long scale, num;

			/* Determine the power of 2 multiple number of pages */
			scale = __ffs(num_pages);
			cmd->tlbi.scale = scale;

			/* Determine how many chunks of 2^scale size we have */
			num = (num_pages >> scale) & CMDQ_TLBI_RANGE_NUM_MAX;
			cmd->tlbi.num = num - 1;

			/* range is num * 2^scale * pgsize */
			inv_range = num << (scale + tg);

			/* Clear out the lower order bits for the next iteration */
			num_pages -= num << scale;
		}
		...
	}
```

**这是五套编码里唯一"长度与地址无关"的**：用 `num`（5 位，最多 31；
`CMDQ_TLBI_RANGE_NUM_MAX` = 31，`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.h:396`）乘 `2^scale` 表达任意页数，
所以**不需要对齐前提**，代价是一条长范围要拆成多条命令（`num_pages -= num << scale`
逐段消耗）。而 Intel 的两套掩码式编码一条命令表达一段，但必须对齐。

注意 `cmd->tlbi.num = num - 1`：**字段存的是"个数减一"**，0 表示 1 个 chunk。
写采集脚本按字段值当页数会少算一页。

`ARM_SMMU_FEAT_RANGE_INV` 不存在时 `num_pages` 保持初值 0、`inv_range` 保持
`granule`（`:2170` 声明处），退化成**逐页一条命令**。所以
"这台 SMMU 刷大页范围慢" 的第一件该查的事是 `RANGE_INV` 特性位。

### I.3.7 `tg` / `ttl`：把页大小与树形塞进命令

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:2175-2196（节选） */
	if (smmu->features & ARM_SMMU_FEAT_RANGE_INV) {
		/* Get the leaf page size */
		tg = __ffs(smmu_domain->domain.pgsize_bitmap);

		num_pages = size >> tg;

		/* Convert page size of 12,14,16 (log2) to 1,2,3 */
		cmd->tlbi.tg = (tg - 10) / 2;
		...
		if (cmd->tlbi.leaf)
			cmd->tlbi.ttl = 4 - ((ilog2(granule) - 3) / (tg - 3));
		else if ((num_pages & CMDQ_TLBI_RANGE_NUM_MAX) == 1)
			num_pages++;
	}
```

`tg = __ffs(pgsize_bitmap)` —— **`pgsize_bitmap` 又一次成为唯一的跨层语言**
（[Q1 T.3](translation.md) 的主题），这里它是失效命令的
颗粒度参数来源。`(tg - 10) / 2` 把 log2 页大小 12/14/16 映射成架构编码 1/2/3。

`else if ((num_pages & CMDQ_TLBI_RANGE_NUM_MAX) == 1) num_pages++;` 这一行是
**为规避硬件/编码边界情况而人为改变页数**：注释（`:2186-2193`）自己说是
"for various errata reasons ... avoid the SVA corner case where both scale and
num could be 0 as well"。**代价是刷的范围比请求的大一页**。这是全章最有用的
一条实操结论：**失效命令覆盖的范围可以严格大于被 unmap 的范围，五套编码里
四套都有这个性质**（Intel 的 `roundup_pow_of_two` + `ALIGN_DOWN`、AMD 的
`msb_diff` 填 1、ARM 的这个 `num_pages++`）。任何"我以为只刷了这 4K"的推理
都不成立；反过来，"多刷"永远安全、"少刷"永远致命，所有实现都朝那个方向偏。

---

## I.4 Intel 失效队列的槽位宽度与空间记账

### I.4.1 槽位是 16 字节还是 32 字节，由 ECAP 决定

```c
/* 来源: drivers/iommu/intel/iommu.h:475 */
#define qi_shift(iommu)		(DMAR_IQ_SHIFT + !!ecap_smts((iommu)->ecap))
```

`ecap_smts` 即 Scalable Mode Translation Support。**legacy 模式下每个队列槽位
16 字节，scalable 模式下 32 字节**，因为 scalable 模式的描述符多了 qw2/qw3 的
用途。规范依据是 `intel-vtd.pdf` 的 Queued Invalidation 描述符类型表
（Table 23，Section 6.5.2.10 内）：它按"模式 × 描述符宽度"枚举合法类型。
完整四格如下（原文照抄）：

| RTADDR_REG.TTM（翻译表模式） | Descriptor Width = 0（128-bit） | Descriptor Width = 1（256-bit） |
|---|---|---|
| legacy mode (`00b`) | `0x1...0x5` | `0x1...0x5` |
| scalable mode (`01b`) | **none** | `0x1...0x9` |
| reserved mode (`10b`) | none | none |
| abort-dma mode (`11b`) | none | `0x1...0x9` |

**scalable 模式 + 128-bit 描述符 = none**，也就是说 scalable 下必须用 256-bit。
这正是 `qi_shift()` 必须在运行时算而不能写死常量的原因；顺带注意 legacy 模式
**两种宽度都合法**，所以"16 字节槽"是 Linux 的选择而不是硬件的限制。

Linux 的类型常量与这张表逐一对得上：

```c
/* 来源: drivers/iommu/intel/iommu.h:387-396 */
#define QI_CC_TYPE		0x1
#define QI_IOTLB_TYPE		0x2
#define QI_DIOTLB_TYPE		0x3
#define QI_IEC_TYPE		0x4
#define QI_IWD_TYPE		0x5
#define QI_EIOTLB_TYPE		0x6
#define QI_PC_TYPE		0x7
#define QI_DEIOTLB_TYPE		0x8
#define QI_PGRP_RESP_TYPE	0x9
#define QI_PSTRM_RESP_TYPE	0xa
```

`0x1…0x5` 恰好就是 legacy 合法集（Context-cache / IOTLB / Device-TLB /
Interrupt Entry Cache / Invalidation Wait），`0x6…0x9` 正是 scalable 新增的四条
（PASID-based IOTLB / PASID-cache / PASID-based Device-TLB / Group Page-request
Response）。**编号不是随意的，而是按"是否需要 scalable 模式"分段**。
`QI_PSTRM_RESP_TYPE`（`0xa`，Stream Page-request Response）在 6.12 里
**只有一处定义、没有任何使用点** —— 全树 `grep` 只命中
`drivers/iommu/intel/iommu.h:396` 这一行，连失败转储用的字符串表
`qi_type_string()`（`drivers/iommu/intel/dmar.c:1213`）都没有为它加 `case`，
真收到了只会打印 `default: return "UNKNOWN"`（`drivers/iommu/intel/dmar.c:1234-1235`）。
所以 Linux 实际发出的类型始终落在 Table 23 的合法集合内。

队列长度是编译期常量，两种模式都是 256 槽：

```c
/* 来源: drivers/iommu/intel/iommu.h:378 */
#define QI_LENGTH	256	/* queue length */
```

于是 `struct q_inval` 里的 `desc` 按 256 × 32B = 8KB 预留，所有偏移换算统一走
`<< qi_shift(iommu)`（`drivers/iommu/intel/dmar.c:1418`、`:1434`、`:1444`）：

```c
/* 来源: drivers/iommu/intel/dmar.c:1417-1419 */
	for (i = 0; i < count; i++) {
		offset = ((index + i) % QI_LENGTH) << shift;
		memcpy(qi->desc + offset, &desc[i], 1 << shift);
```

注意 `memcpy` 只拷 `1 << shift` 字节：**scalable 关闭时 qw2/qw3 根本不会进硬件
队列**，它们是软件私有数据。I.6 里 `qi_dump_fault` 的注释
（`drivers/iommu/intel/dmar.c:1296-1300`，"We won't print out these two qw's for security
consideration"）正是这个约定的另一面。

### I.4.2 `free_cnt < count + 2`：那个 +2 是什么

```c
/* 来源: drivers/iommu/intel/dmar.c:1402-1411 */
	/*
	 * Check if we have enough empty slots in the queue to submit,
	 * the calculation is based on:
	 * # of desc + 1 wait desc + 1 space between head and tail
	 */
	while (qi->free_cnt < count + 2) {
		raw_spin_unlock_irqrestore(&qi->q_lock, flags);
		cpu_relax();
		raw_spin_lock_irqsave(&qi->q_lock, flags);
	}
```

环形队列**必须留一个空槽**来区分"满"和"空"（tail 追上 head 就歧义了），所以
`+2` = 1 个附加的 wait 描述符 + 1 个哨兵空槽。提交后：

```c
/* 来源: drivers/iommu/intel/dmar.c:1437-1444（节选） */
	qi->free_head = (qi->free_head + count + 1) % QI_LENGTH;
	qi->free_cnt -= count + 1;
	...
	writel(qi->free_head << shift, iommu->reg + DMAR_IQT_REG);
```

软件记账 `free_cnt` 因此始终比"硬件还剩多少"保守 1，这正是满/空可辨的代价。

**空间不足时先放锁再 `cpu_relax()`**（`:1408-1410`）：不能抱着 `q_lock` 自旋，
否则持有者永远是等不到空间的自己——回收 `free_cnt` 需要别的 CPU 完成提交并
调用 `reclaim_free_desc()`。

### I.4.3 空间回收只能从尾部推进

```c
/* 来源: drivers/iommu/intel/dmar.c:1202-1211 */
/*
 * Reclaim all the submitted descriptors which have completed its work.
 */
static inline void reclaim_free_desc(struct q_inval *qi)
{
	while (qi->desc_status[qi->free_tail] == QI_FREE && qi->free_tail != qi->free_head) {
		qi->free_tail = (qi->free_tail + 1) % QI_LENGTH;
		qi->free_cnt++;
	}
}
```

关键约束：**队列是 FIFO，但完成状态是逐槽的**。回收遇到第一个非 `QI_FREE`
就停——即使它后面有已完成的槽也不能跳过。所以一条卡住的描述符会**阻塞整条
队列的空间回收**，这是 ITE 场景下故障会放大的机制根源（见 I.6.2）。

`count == 0` 的边界情形有专门处理：

```c
/* 来源: drivers/iommu/intel/dmar.c:1463-1472（节选） */
	/*
	 * The reclaim code can free descriptors from multiple submissions
	 * starting from the tail of the queue. When count == 0, the
	 * status of the standalone wait descriptor at the tail of the queue
	 * must be set to QI_FREE to allow the reclaim code to proceed.
	 * ...
	 */
	for (i = 0; i <= count; i++)
		qi->desc_status[(index + i) % QI_LENGTH] = QI_FREE;
```

循环写的是 `i <= count`（不是 `< count`）——**多出来的那一个正是 wait 描述符**。
`count == 0` 时它就是"纯等待、不发失效"的调用，用来推进回收或 drain。
`qi_global_iec()`（`drivers/iommu/intel/dmar.c:1498-1509`）这类"单条描述符 + 必等"的调用同样受益：
它传的 `count = 1`，回收清的是自己那一格和 wait 那一格。

---

## I.5 完成语义：Wait Descriptor 与关中断自旋

### I.5.1 完成 = 硬件往内存写一个值

```c
/* 来源: drivers/iommu/intel/dmar.c:1426-1435 */
	wait_desc.qw0 = QI_IWD_STATUS_DATA(QI_DONE) |
			QI_IWD_STATUS_WRITE | QI_IWD_TYPE;
	if (options & QI_OPT_WAIT_DRAIN)
		wait_desc.qw0 |= QI_IWD_PRQ_DRAIN;
	wait_desc.qw1 = virt_to_phys(&qi->desc_status[wait_index]);
	wait_desc.qw2 = 0;
	wait_desc.qw3 = 0;

	offset = wait_index << shift;
	memcpy(qi->desc + offset, &wait_desc, 1 << shift);
```

- `QI_IWD_STATUS_WRITE`（bit 5，`drivers/iommu/intel/iommu.h:403`）告诉硬件"把 STATUS_DATA 字段的值
  写到 qw1 指向的物理地址"。
- `QI_IWD_STATUS_DATA(QI_DONE)`（值左移 32 位，`drivers/iommu/intel/iommu.h:402`）要写的值就是
  `QI_DONE`，而目标地址是**这一批最后一个槽自己的软件状态字节**。
- 于是完成判定是 `while (READ_ONCE(qi->desc_status[wait_index]) != QI_DONE)`
  （`drivers/iommu/intel/dmar.c:1446`）——**纯 CPU 自旋读自己的内存**，没有中断、没有等待队列。

**这就是 Q5 的最终答案**：`iommu_unmap()` 之所以是同步的，因为它
返回前必须看到硬件把 `QI_DONE` 写进那个字节。没有中断、没有回调、没有
"稍后确认"的机制存在。`iommu_unmap_fast()` 只是不调这段，页表照样当场拆
（I.3 的结论，这里是它的实现证据）。

`wait_index = (index + count) % QI_LENGTH`（`:1414`）——wait 描述符固定追加在本批
之后。注释也写明："Wait descriptors can be part of the submission but it will not
be polled for completion"（`drivers/iommu/intel/dmar.c:1360-1366`）：批量提交里自己放的 wait 只当
分隔符用，**只有最外层这个才参与判定**。

### I.5.2 为什么自旋期间不能开中断

```c
/* 来源: drivers/iommu/intel/dmar.c:1446-1461（节选） */
	while (READ_ONCE(qi->desc_status[wait_index]) != QI_DONE) {
		/*
		 * We will leave the interrupts disabled, to prevent interrupt
		 * context to queue another cmd while a cmd is already submitted
		 * and waiting for completion on this cpu. This is to avoid
		 * a deadlock where the interrupt context can wait indefinitely
		 * for free slots in the queue.
		 */
		rc = qi_check_fault(iommu, index, wait_index);
		if (rc)
			break;

		raw_spin_unlock(&qi->q_lock);
		cpu_relax();
		raw_spin_lock(&qi->q_lock);
	}
```

死锁形状是这样的：中断上下文里再来一次失效（比如驱动的中断处理里 `dma_unmap`）
→ 它也要 `qi_submit_sync` → 队列空间不足 → 而推进空间需要**当前这个** CPU 完成
轮询 → 但当前 CPU 被中断上下文抢占。**关中断把这条链掐断**。

同时注意它**确实放 `q_lock`**（`raw_spin_unlock` / `raw_spin_lock`，不带 irqrestore）
——让别的 CPU 能提交并完成、从而回收空间。锁的粒度设计是三层的：
irq 保证不被本 CPU 的自己饿死，`q_lock` 的释放保证不被别的 CPU 饿死。

### I.5.3 Fence 位：定义了，但主路径不用

```c
/* 来源: drivers/iommu/intel/iommu.h:402-405 */
#define QI_IWD_STATUS_DATA(d)	(((u64)d) << 32)
#define QI_IWD_STATUS_WRITE	(((u64)1) << 5)
#define QI_IWD_FENCE		(((u64)1) << 6)
#define QI_IWD_PRQ_DRAIN	(((u64)1) << 7)
```

规范 Section 6.5.2.11 明确：

> If the Fence (FN) flag is 0 in a inv_wait_dsc, hardware **may** execute descriptors
> following the inv_wait_dsc before the wait command is completed. If the Fence (FN)
> flag is 1 ..., hardware **must** execute descriptors following the inv_wait_dsc only
> after the wait command is completed.

`qi_submit_sync()` 构造的 wait **不含** `QI_IWD_FENCE`（对照 I.5.1 的原文）。全树
只有两处用这个位，一处是定义，另一处是页请求处理：

```c
/* 来源: drivers/iommu/intel/prq.c:101-120（节选） */
	/*
	 * Perform steps described in VT-d spec CH7.10 to drain page
	 * requests and responses in hardware.
	 */
	memset(desc, 0, sizeof(desc));
	desc[0].qw0 = QI_IWD_STATUS_DATA(QI_DONE) |
			QI_IWD_FENCE |
			QI_IWD_TYPE;
	if (pasid == IOMMU_NO_PASID) {
		qi_desc_iotlb(iommu, did, 0, 0, DMA_TLB_DSI_FLUSH, &desc[1]);
		qi_desc_dev_iotlb(sid, info->pfsid, info->ats_qdep, 0,
				  MAX_AGAW_PFN_WIDTH, &desc[2]);
	} else {
		qi_desc_piotlb(did, pasid, 0, -1, 0, &desc[1]);
		qi_desc_dev_iotlb_pasid(sid, info->pfsid, pasid, info->ats_qdep,
					0, MAX_AGAW_PFN_WIDTH, &desc[2]);
	}
qi_retry:
	reinit_completion(&iommu->prq_complete);
	qi_submit_sync(iommu, desc, 3, QI_OPT_WAIT_DRAIN);
```

这一段信息密度很高，值得当模板读：

1. **Fence wait 排在最前**（`desc[0]`），把后续描述符与之前的页请求隔离开——
   SVA/PASID 拆除时唯一能保证"硬件不再产生新 PRQ"的手段。
2. 一次 `qi_submit_sync(count = 3)` 提交 **wait + IOTLB + Device-TLB**，顺序正是
   I.7 要求的"IOTLB 先于 Device-TLB"——同一个批内按数组顺序 `memcpy`
   （I.4.1），硬件按队列顺序执行。
3. 两条失效都是**全量**：`DMA_TLB_DSI_FLUSH` / `npages = -1`（PASID-selective）
   配 `MAX_AGAW_PFN_WIDTH`（整设备）。因为要拆的是整个 PASID 的地址空间，
   没有范围可言。
4. `QI_OPT_WAIT_DRAIN` 使 wait 带上 `PRQ_DRAIN`，硬件先排空页请求队列。
5. `pasid == IOMMU_NO_PASID` 的分支决定走 legacy 描述符还是 PASID 描述符——
   I.3.1 与 I.3.2 两套编码在这里同框出现，是最好的对照样本。

---

## I.6 错误恢复：IQE / ITE / ICE 三条分岔

`qi_check_fault()`（`drivers/iommu/intel/dmar.c:1270-1358`）是内核里少见的完整硬件错误
恢复实现，FSTS_REG 三个位对应三种完全不同的处置。规范描述见
`intel-vtd.pdf` Section 6.5.2.10；寄存器字段的引用代码自己给了
（"see Intel VT-d spec r4.1, section 11.4.9.9"，`drivers/iommu/intel/dmar.c:1320-1322`）。

### I.6.1 IQE：把 wait 描述符复制进出错槽

```c
/* 来源: drivers/iommu/intel/dmar.c:1291-1307 */
	if (fault & DMA_FSTS_IQE) {
		head = readl(iommu->reg + DMAR_IQH_REG);
		if ((head >> shift) == index) {
			struct qi_desc *desc = qi->desc + head;

			/*
			 * desc->qw2 and desc->qw3 are either reserved or
			 * used by software as private data. We won't print
			 * out these two qw's for security consideration.
			 */
			memcpy(desc, qi->desc + (wait_index << shift),
			       1 << shift);
			writel(DMA_FSTS_IQE, iommu->reg + DMAR_FSTS_REG);
			pr_info("Invalidation Queue Error (IQE) cleared\n");
			return -EINVAL;
		}
	}
```

注释给出前提："If IQE happens, the head points to the descriptor associated with
the error. **No new descriptors are fetched until the IQE is cleared.**"（`:1286-1290`）

硬件卡在出错那条上不取新描述符。Linux 的处置是**用 wait 描述符覆盖出错槽**——
把"一条硬件拒收的垃圾"换成"一条必定能完成的等待"，让队列能继续推进；清 FSTS
位；返回 `-EINVAL`。`head >> shift == index` 的判定确保只处理"我这条"，
别人的 IQE 不在这里越权清理。

`(head >> shift)`：硬件头寄存器给的是**字节偏移**，右移 `qi_shift` 才得到槽号——
又一处 I.4.1 的 16/32 字节换算。

### I.6.2 ITE：中止所有 pending wait，再看设备是否还在

```c
/* 来源: drivers/iommu/intel/dmar.c:1309-1349（节选） */
	/*
	 * If ITE happens, all pending wait_desc commands are aborted.
	 * No new descriptors are fetched until the ITE is cleared.
	 */
	if (fault & DMA_FSTS_ITE) {
		head = readl(iommu->reg + DMAR_IQH_REG);
		head = ((head >> shift) - 1 + QI_LENGTH) % QI_LENGTH;
		tail = readl(iommu->reg + DMAR_IQT_REG);
		tail = ((tail >> shift) - 1 + QI_LENGTH) % QI_LENGTH;
		...
		writel(DMA_FSTS_ITE, iommu->reg + DMAR_FSTS_REG);
		pr_info("Invalidation Time-out Error (ITE) cleared\n");

		do {
			if (qi->desc_status[head] == QI_IN_USE)
				qi->desc_status[head] = QI_ABORT;
			head = (head - 1 + QI_LENGTH) % QI_LENGTH;
		} while (head != tail);
```

**从 head 到 tail 全标 `QI_ABORT`**——不止自己那一个。这与规范 Section 6.5.2.11
最后一条一致："When a Device-TLB invalidation or PASID-based-Device-TLB
invalidation time-out is detected, hardware must not complete any pending
inv_wait_dsc commands." 即硬件已经**放弃了整批等待**，软件只能跟着全部中止，
否则就会永久自旋。

然后是本节最有实操价值的一段：

```c
/* 来源: drivers/iommu/intel/dmar.c:1335-1349 */
		/*
		 * If device was released or isn't present, no need to retry
		 * the ATS invalidate request anymore.
		 *
		 * 0 value of ite_sid means old VT-d device, no ite_sid value.
		 * see Intel VT-d spec r4.1, section 11.4.9.9
		 */
		if (ite_sid) {
			dev = device_rbtree_find(iommu, ite_sid);
			if (!dev || !dev_is_pci(dev) ||
			    !pci_device_is_present(to_pci_dev(dev)))
				return -ETIMEDOUT;
		}
		if (qi->desc_status[wait_index] == QI_ABORT)
			return -EAGAIN;
```

ITE 寄存器里的 **SID 就是超时的那台设备**。如果设备已经拔出 / 驱动已释放
（`pci_device_is_present()` 读配置空间 vendor ID 失败），**直接 `-ETIMEDOUT`
返回、不重试**。这条路径是热拔 ATS 设备时内核不挂死的保证：设备没了 →
Device-TLB 失效永远不会完成 → 若不加这个判定就会在 I.5.2 的自旋里无限循环。

反之 `ite_sid == 0` 表示"老 VT-d 硬件不报 SID"，此时**没有信息判断设备是否
在场**，只能落到 `-EAGAIN` 重试。`-EAGAIN` 会让 `qi_submit_sync` 走
`goto restart`（`drivers/iommu/intel/dmar.c:1477-1478`）从头再提交一次。

顺带一个易被忽略的收尾：`QI_ABORT` 的检查放在 `qi_check_fault()` **开头**：

```c
/* 来源: drivers/iommu/intel/dmar.c:1279-1280 */
	if (qi->desc_status[wait_index] == QI_ABORT)
		return -EAGAIN;
```

即"上一轮 ITE 已经把我的 wait 标成 ABORT 了"这一事实，要在读 FSTS 之前就先反应，
否则会被后续错误覆盖信息。

### I.6.3 ICE：只清位

```c
/* 来源: drivers/iommu/intel/dmar.c:1352-1355 */
	if (fault & DMA_FSTS_ICE) {
		writel(DMA_FSTS_ICE, iommu->reg + DMAR_FSTS_REG);
		pr_info("Invalidation Completion Error (ICE) cleared\n");
	}
```

ICE 是"Device-TLB 失效**完成**响应里带错误"（规范 Section 6.5.2.10），
失效本身已经执行完，所以软件无事可做——**不影响 `desc_status`，也不改返回值**。
一条 ICE 会静默地"算成功"。要发现它只能靠 `pr_info` 与 fault 中断
（`qi_dump_fault()`，`drivers/iommu/intel/dmar.c:1245-1268` 打印 IQEI/ITESID/ICESID，并把**出错槽
前一格**的 `qw0/qw1` 一起打出来——`head - 1`，因为中断到达时 head 已经前进）。

---

## I.7 顺序契约：谁必须排在谁前面

规范 Section 6.5.2.11 给了硬件**必须**保证的排序（摘三条）：

> - Hardware must execute an IOTLB invalidation descriptor (`iotlb_inv_dsc`) or
>   PASID-based-IOTLB invalidation descriptor (`p_iotlb_inv_dsc`) **only after** all
>   Context-cache invalidation descriptors (`cc_inv_dsc`) and PASID-cache invalidation
>   descriptors (`pc_inv_dsc`) ahead of it in the Invalidation Queue are completed.
> - Hardware must execute a Device-TLB invalidation descriptor (`dev_tlb_inv_dsc`)
>   **only after** all IOTLB invalidation descriptors (`iotlb_inv_dsc`) and Interrupt
>   Entry Cache invalidation descriptors (`iec_inv_dsc`) ahead of it ... are completed.
> - Hardware must report completion of an Invalidation Wait Descriptor
>   (`inv_wait_dsc`) **only after** at least all the descriptors ahead of it ... are
>   completed.

另一条是软件义务，Section 6.5.2.5（`intel-vtd.pdf`，紧接 Device-TLB 描述符定义）：

> Since translation requests-without-PASID from a device may be serviced by hardware
> from the IOTLB, **software must always request IOTLB invalidation (`iotlb_inv_dsc`)
> before requesting corresponding Device-TLB (`dev_tlb_inv_dsc`) invalidation.**

**Linux 怎么满足它？** 答案藏在一个链表顺序里，看代码时极易当成无关细节。
`cache_tag_flush_range()` 只遍历 `domain->cache_tags` 一次
（`drivers/iommu/intel/cache.c:448`），按 tag 类型分派——所以"同一设备的 IOTLB
与 Device-TLB 谁先发"完全取决于**它们在链表里的先后**。而这由建 tag 时的顺序
决定：

```c
/* 来源: drivers/iommu/intel/cache.c:137-143 */
	ret = cache_tag_assign(domain, did, dev, pasid, CACHE_TAG_IOTLB);
	if (ret || !info->ats_enabled)
		return ret;

	ret = cache_tag_assign(domain, did, dev, pasid, CACHE_TAG_DEVTLB);
	if (ret)
		cache_tag_unassign(domain, did, dev, pasid, CACHE_TAG_IOTLB);
```

```c
/* 来源: drivers/iommu/intel/cache.c:77 */
	list_add_tail(&tag->node, &domain->cache_tags);
```

**IOTLB 先 assign + `list_add_tail`** ⇒ IOTLB tag 一定排在同设备的 DEVTLB tag 之前
⇒ 遍历发出的描述符顺序就是 IOTLB → Device-TLB ⇒ 满足规范。

这条链值得记住，因为它是**三处分离代码共同维持的不变量**
（assign 顺序 `:137`/`:141`、插入方式 `:77`、遍历方式 `:448`），任何一处改动
（例如有人把 `list_add_tail` 换成 `list_add` 做性能优化）都会**静默违反规范
顺序**，而且不会有任何编译错误或测试失败。这是本项目"已知陷阱"级别的坑。

批量提交的顺序性也参与此事：`qi_batch_flush_descs()` 只在**跨 iommu** 或
链表走完时提交（`drivers/iommu/intel/cache.c:449-451`、`:476`），同一 IOMMU 的多条描述符按数组序
`memcpy` 进队列，硬件按队列序执行——顺序不会因为"攒批"而丢失。

**反向（启用 ATS）的顺序规范不管**：先改 context entry 再使能设备侧 ATS 是
软件自己的事。**关闭**方向的推荐时序与 Linux 的实现不一致，这一点已在
AGENTS.md 已知陷阱 14(d) 记录：规范 Section 4.5.2 用词是 "Recommended / should"，
而 `device_block_translation()`（`drivers/iommu/intel/iommu.c:3394`）在 `:3407`
清 `E`、`:3413` 清 context entry，Device-TLB 失效推迟到 `__context_flush_dev_iotlb()`
（`drivers/iommu/intel/pasid.c:885`）——**顺序被调换了**。本文不重复展开，只补一句
与失效相关的结论：Linux 这么做的前提是"清 `E` 之后设备不会再产生新的
翻译请求"，所以延后刷 Device-TLB 只影响**残留条目的存活时间**，不影响正确性。
这个前提成立与否是 phase6 的话题。

---

## I.8 cache_tag：域到缓存位置的反向索引

`iommu_unmap()` 的入参只有 `domain` 和 `iova`，但失效必须发给**具体的 IOMMU
硬件实例**和**具体的 ATS 设备**。这层"域 → 谁缓存了我的翻译"的映射就是
`cache_tag`：

```c
/* 来源: drivers/iommu/intel/iommu.h:1248-1251 */
	CACHE_TAG_IOTLB,
	CACHE_TAG_DEVTLB,
	CACHE_TAG_NESTING_IOTLB,
	CACHE_TAG_NESTING_DEVTLB,
```

四种类型正好对应 nested 场景的两级 × 两处缓存。`cache_tag_flush_range()` 的
分派（`drivers/iommu/intel/cache.c:453-472`）里藏着本文最实用的一个结论：

```c
/* 来源: drivers/iommu/intel/cache.c:458-470 */
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
			fallthrough;
		case CACHE_TAG_DEVTLB:
			cache_tag_flush_devtlb_psi(domain, tag, addr, mask);
			break;
```

**改 S2（父域）会让整台设备的 ATS 缓存全刷**，而不是按范围刷。原因写在注释里，
是个真实的体系结构限制：设备缓存的是"两级合成的结果"，手里只有一个 IOVA，
**无法反查这个 IOVA 经过了哪些 S2 中间条目**，因此 S2 一变就无从归因。

对照 `CACHE_TAG_NESTING_IOTLB`（S1 变化）走的是**正常按范围失效**
（`:454-457`，与 `CACHE_TAG_IOTLB` 同一条路径）。所以对 nested/SVA 部署的一条
硬结论：**S1 改动便宜，S2 改动对直通设备是核弹**。宿主侧任何会碰 S2 映射的
操作（例如给父域重排 IOVA）都会把 guest 里设备的 ATS 缓存清空，代价是后续
每一次 DMA 都要重新走 ATS 转换请求。这解释了为什么 vDPA / VFIO 的容器内存
布局变更如此昂贵，也是 IOMMUFD 把 HWPT 与 S2 解耦设计的动机之一。

`cache_tag_flush_all()` 走另一个极端：IOTLB 传 `pages = -1`
（`drivers/iommu/intel/cache.c:499`），配合 `cache_tag_flush_iotlb()` 里
`pages == -1` 的判据（`:375-381`）直接落到 domain-selective；Device-TLB 走
`cache_tag_flush_devtlb_all()`，`size_order` 传 `MAX_AGAW_PFN_WIDTH`
（`:426-427`）。

最后是那个"双发"怪癖：

```c
/* 来源: drivers/iommu/intel/cache.c:400-406 */
	if (tag->pasid == IOMMU_NO_PASID) {
		qi_batch_add_dev_iotlb(iommu, sid, info->pfsid, info->ats_qdep,
				       addr, mask, domain->qi_batch);
		if (info->dtlb_extra_inval)
			qi_batch_add_dev_iotlb(iommu, sid, info->pfsid, info->ats_qdep,
					       addr, mask, domain->qi_batch);
		return;
	}
```

完全相同的描述符**连发两次**，由 `dtlb_extra_inval` 门控，赋值点只有一个：
`drivers/iommu/intel/iommu.c:3929` 的
`info->dtlb_extra_inval = dev_needs_extra_dtlb_flush(pdev);`——**特定设备的
硬件 erratum 规避**。做失效次数统计时这一条会让计数翻倍，别误判成重复提交 bug。

---

## I.9 Device-TLB 失效：代价在链路另一端

IOTLB 在 IOMMU 内部，I.3–I.8 讲的全是"家里的事"。Device-TLB 不一样：它在
**PCIe 设备里**，在异步链路的另一端（[README 1.3](README.md#13-iommu-多出来的三个维度)
的第三个维度）。

`qi_flush_dev_iotlb()`（`drivers/iommu/intel/dmar.c:1534`）开头一段是规范 4.3 的直接落地：

```c
/* 来源: drivers/iommu/intel/dmar.c:1538-1545 */
	/*
	 * VT-d spec, section 4.3:
	 *
	 * Software is recommended to not submit any Device-TLB invalidation
	 * requests while address remapping hardware is disabled.
	 */
	if (!(iommu->gcmd & DMA_GCMD_TE))
		return;
```

批量路径 `qi_batch_add_dev_iotlb()` 里有同一个 TE 闸门（`drivers/iommu/intel/cache.c:314-319`）。

真正慢的是规范 4.3 描述的那套硬件流程：硬件先分配一个 ITag（**没有空闲 ITag 时该请求
被推迟**）、启动失效完成定时器、把 Invalidate Request TLP 发下去、等设备回 Invalidate
Completion；超时则释放 ITag 并在 Fault 记录里置 **ITE**（Invalidation Time-out
Error）——ITE 的软件处置见 I.6.2。三件事必须记准：

1. **6.12.93 这条路径里没有软件可见的 ITag。** `ITag` 是 ATS 协议里 VT-d **硬件自己**
   管理 Device-TLB 失效请求/完成配对的标签（规范 4.3），软件看不到它，也就没有"软件分配
   并回收 ITag"这回事。软件侧的同步手段只有 wait descriptor 的状态写（I.5）。
2. **软件侧唯一的旋钮是 `ats_qdep`**——`qi_batch_add_dev_iotlb()` 的 `qdep` 实参
   （`drivers/iommu/intel/cache.c:310`），它来自 `pci_ats_queue_depth()`，也就是
   [phase6 §1.5](../phase6-vfio/README.md#15-acs-与-ats直通依赖的两个-pcie-能力) 里
   那个"PF 返回 32、VF 返回 0"的字段。
3. **Device-TLB 失效的代价不在队列记账里。** 它不占 IOTLB 那条队列的 `free_cnt`
   等待（I.4.2），占的是链路对端的完成时间。于是"缓存副本跑到了异步链路的另一端"
   有了具体的等待对象：**一次 unmap 的延迟下界不再由 IOMMU 决定，而由最慢的那个
   开 ATS 的设备回 completion 的时间决定**。

---

## I.10 PRI 与 PRQ：唯一能把缺页变成可恢复事件的路

前面所有失效都是"软件改了表，通知硬件"。PRI（Page Request Interface）是反方向：
**硬件发现自己缺页，向软件要页**。这是 IOMMU 侧唯一接近 CPU 侧 page fault 的机制，
也是 SVA 能成立的前提（见 [Q1](translation.md) 的两级翻译）。

硬件把 page request 写进 PRQ（Page Request Queue），`prq_event_thread()`
（`drivers/iommu/intel/prq.c:196`）消费它。开头的四道检查——无 PASID、地址非
canonical、特权态读写、execute+read——全部按无效请求回 `QI_RESP_INVALID`
（`handle_bad_prq_event()`，`drivers/iommu/intel/prq.c:135`）。合法请求走
`intel_prq_report()`（`drivers/iommu/intel/prq.c:174`）交给通用 iopf 层
（`drivers/iommu/io-pgfault.c`），最终由 SVA 补页后用 **Page Group Response
Descriptor** 回给设备：

```c
/* 来源: drivers/iommu/intel/prq.c:393-398（节选） */
	desc.qw0 = QI_PGRP_PASID(prm->pasid) | QI_PGRP_DID(sid) |
			QI_PGRP_PASID_P(pasid_present) |
			QI_PGRP_RESP_CODE(msg->code) |
			QI_PGRP_RESP_TYPE;
	desc.qw1 = QI_PGRP_IDX(prm->grpid) | QI_PGRP_LPIG(last_page);
```

注意响应也是**通过失效队列**下发的（`QI_PGRP_RESP_TYPE = 0x9`，
`drivers/iommu/intel/iommu.h:395`）——规范 6.5.2 开头就说了 IQ 也用于提交 Page
Group Response 描述符。所以 PRI 路径与失效路径共用同一条 256 项队列（I.4.1）、
也共用 `q_lock`：**一次 PRQ 洪泛能直接把数据面的 unmap 拖死**。这类互相拖累在
代码里看不见，只有在 `qi_submit_sync()` 的排队时间里才现形。

拆掉一个 PASID 时不能只删表，还必须确认队列里、链路上、设备里都没有在途的 page
request。`intel_iommu_drain_pasid_prq()`（`drivers/iommu/intel/prq.c:60`）的函数
注释就是这段契约：

> "This is supposed to be called after the device driver has stopped DMA, the pasid
> entry has been cleared, and both IOTLB and DevTLB have been invalidated."

它的实现是往失效队列塞三个描述符——一个带 **Fence** 的 wait、一个域选择性 IOTLB
失效、一个全域 Device-TLB 失效——并给 wait descriptor 打开 `PD`（Page-request
Drain）位。这段描述符序列已在 I.5.3 作为 Fence 的模板出现过，这里只补最后的
retry 结构：

```c
/* 来源: drivers/iommu/intel/prq.c:118-125（节选） */
qi_retry:
	reinit_completion(&iommu->prq_complete);
	qi_submit_sync(iommu, desc, 3, QI_OPT_WAIT_DRAIN);
	if (readl(iommu->reg + DMAR_PRS_REG) & DMA_PRS_PRO) {
		wait_for_completion(&iommu->prq_complete);
		goto qi_retry;
	}
```

对应规范 7.10（Software Steps to Drain Page Requests & Responses）、6.5.2.8 的
`Page-request Drain (PD)` 位，以及 6.5.2.11 的 Fence 语义（I.5.3 已引）。最后的
retry 结构在处理 PRQ **溢出**（`DMA_PRS_PRO`，`drivers/iommu/intel/iommu.h:358`）
时是必须的：溢出期间硬件会丢弃新的 page request，所以 drain 之后若溢出位仍置起，
说明期间又有请求进来，必须重来。

---

## I.11 strict 与 lazy：`DMA_FQ` 到底延迟了什么

[Q3](domains.md) 的 D.3 说过 `iommu.strict=0` 会把 `iommu_def_domain_type` 改成
`IOMMU_DOMAIN_DMA_FQ`。这一节说清它换掉的到底是什么——**不是"少做失效"，而是
"把 N 次范围失效换成 1 次全域失效，并把地址与页面的回收推迟"**。

分流点在 `__iommu_dma_unmap()`：

```c
/* 来源: drivers/iommu/dma-iommu.c:837-846 */
	iommu_iotlb_gather_init(&iotlb_gather);
	iotlb_gather.queued = READ_ONCE(cookie->fq_domain);

	unmapped = iommu_unmap_fast(domain, dma_addr, size, &iotlb_gather);
	WARN_ON(unmapped != size);

	if (!iotlb_gather.queued)
		iommu_iotlb_sync(domain, &iotlb_gather);
	iommu_dma_free_iova(cookie, dma_addr, size, &iotlb_gather);
```

`iommu_unmap_fast()`（`drivers/iommu/iommu.c:2608`）与 `iommu_unmap()` 的唯一区别
就是不带那次 `iommu_iotlb_sync()`——**页表照样当场拆掉**，只是不发失效。所以 I.1
的同步结论针对的是 `iommu_unmap()`；`iommu_unmap_fast()` 把同步的义务移交给调用者。
VFIO 也用它（`drivers/vfio/vfio_iommu_type1.c:981`），但自己在批量结束后统一
sync，语义仍是同步的。

进了 flush queue 之后，`iommu_dma_free_iova()` 走 `queue_iova()`
（`drivers/iommu/dma-iommu.c:202`），把 IOVA 挂进环形队列而不是归还分配器：

```c
/* 来源: drivers/iommu/dma-iommu.c:225-244（节选） */
	/*
	 * First remove all entries from the flush queue that have already been
	 * flushed out on another CPU. This makes the fq_full() check below less
	 * likely to be true.
	 */
	fq_ring_free_locked(cookie, fq);

	if (fq_full(fq)) {
		fq_flush_iotlb(cookie);
		fq_ring_free_locked(cookie, fq);
	}

	idx = fq_ring_add(fq);

	fq->entries[idx].iova_pfn = pfn;
	fq->entries[idx].pages    = pages;
	fq->entries[idx].counter  = atomic64_read(&cookie->fq_flush_start_cnt);
```

三个量化事实加一个语义差别：

| 项 | 值 / 行为 | 源码 |
|---|---|---|
| 队列长度 | 每 CPU 256 项（单队列模式 32768） | `drivers/iommu/dma-iommu.c:105-107` |
| 超时 | 10 ms（单队列 1000 ms） | `drivers/iommu/dma-iommu.c:110-111` |
| 队列满 | **当场同步刷一次**，退化成最坏情况的 strict | `drivers/iommu/dma-iommu.c:233-237` |
| 刷的方式 | `ops->flush_iotlb_all()`——**整个域全刷**，不是范围失效 | `drivers/iommu/dma-iommu.c:179-184` |
| 地址何时可复用 | 等 `fq_flush_finish_cnt` 前进后才 `free_iova_fast()` + 释放物理页 | `drivers/iommu/dma-iommu.c:146-163` |

最后一行才是 lazy 的真实代价，也最容易被"性能更好"三个字盖过去：**unmap 之后，那块
物理页和那段 IOVA 都不能立刻复用**，至少压着 10 ms 或到队列满。对高频小映射的负载，
你看到的往往不是吞吐上升，而是 IOVA 碎片化、`alloc_iova_fast()` 更多走慢路径、以及
物理页回收延迟。`flush_iotlb_all` 对应 Intel 的 `intel_flush_iotlb_all()`
（`drivers/iommu/intel/iommu.c:1315`）——它走的是 I.2 里 `pages == -1` 那条全域
DSI 分支。

安全性上，`fq_flush_iotlb()` 只保证"失效已发生"，不保证"设备已停止 DMA"——所以 lazy
的隐含前提是"这个地址被重新分配给别人之前，旧设备不会再拿它发 DMA"。
`iommu_dma_init_domain()` 里那道降级就是这条前提的工程化：

```c
/* 来源: drivers/iommu/dma-iommu.c:721-724 */
	/* If the FQ fails we can simply fall back to strict mode */
	if (domain->type == IOMMU_DOMAIN_DMA_FQ &&
	    (!device_iommu_capable(dev, IOMMU_CAP_DEFERRED_FLUSH) || iommu_dma_init_fq(domain)))
		domain->type = IOMMU_DOMAIN_DMA;
```

Intel 对 `IOMMU_CAP_DEFERRED_FLUSH` 直接返回 `true`（`drivers/iommu/intel/iommu.c:3883`）。
注意这个降级是**改 `domain->type`**，而 `DMA_FQ` 与 `DMA` 是同一个域对象——所以
`cat /sys/kernel/iommu_groups/N/type` 打印出来的类型可能和 cmdline 想要的不同，
这是 Q3 D.6 那张观测表的一个具体坑。

`DMA → DMA-FQ` 可以在线切换（Q3 D.6，`drivers/iommu/iommu.c:3071-3081`），反过来
`DMA-FQ → DMA` 则是普通的换域，需要 group 空闲。practice 3 用前者做对照，正好绕开
解绑驱动。

---

## I.12 SMMUv3：无锁乐观预留与 CMD_SYNC

### I.12.1 六步流水线

`arm_smmu_cmdq_issue_cmdlist()`（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:779-898`）的注释
自己编了号，本文沿用：

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:792-815（节选） */
	/* 1. Allocate some space in the queue */
	local_irq_save(flags);
	llq.val = READ_ONCE(cmdq->q.llq.val);
	do {
		u64 old;

		while (!queue_has_space(&llq, n + sync)) {
			local_irq_restore(flags);
			if (arm_smmu_cmdq_poll_until_not_full(smmu, cmdq, &llq))
				dev_err_ratelimited(smmu->dev, "CMDQ timeout\n");
			local_irq_save(flags);
		}

		head.cons = llq.cons;
		head.prod = queue_inc_prod_n(&llq, n + sync) |
					     CMDQ_PROD_OWNED_FLAG;

		old = cmpxchg_relaxed(&cmdq->q.llq.val, llq.val, head.val);
		if (old == llq.val)
			break;

		llq.val = old;
	} while (1);
	owner = !(llq.prod & CMDQ_PROD_OWNED_FLAG);
```

与 Intel 的根本差别在这里：**没有自旋锁保护整个提交**。`cmpxchg_relaxed` 一次
CAS 就把 n+sync 个槽"预定"下来，失败就重读重试。这是**乐观并发**：多 CPU 可以
同时往 CMDQ 写不同区段。

`CMDQ_PROD_OWNED_FLAG` 是嵌在 prod 值里的一个标志位：**谁抢到 flag=0 的位置，
谁就成为 owner，负责把 prod 推给硬件**。`owner = !(llq.prod & CMDQ_PROD_OWNED_FLAG)`
——判断方式就是"我预定到的那个 prod 里没带 flag"。这是软件层的**领导者选举**，
让 N 个 CPU 的命令由 1 个统一 `writel` 提交，把 MMIO 写次数从 N 降到 1。

步骤 4 是 owner 的交接舞：

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:843-870（节选） */
	if (owner) {
		/* a. Wait for previous owner to finish */
		atomic_cond_read_relaxed(&cmdq->owner_prod, VAL == llq.prod);

		/* b. Stop gathering work by clearing the owned flag */
		prod = atomic_fetch_andnot_relaxed(CMDQ_PROD_OWNED_FLAG,
						   &cmdq->q.llq.atomic.prod);
		prod &= ~CMDQ_PROD_OWNED_FLAG;

		/*
		 * c. Wait for any gathered work to be written to the queue.
		 * Note that we read our own entries so that we have the control
		 * dependency required by (d).
		 */
		arm_smmu_cmdq_poll_valid_map(cmdq, llq.prod, prod);

		/*
		 * d. Advance the hardware prod pointer
		 * Control dependency ordering from the entries becoming valid.
		 */
		writel_relaxed(prod, cmdq->q.prod_reg);

		/*
		 * e. Tell the next owner we're done
		 * Make sure we've updated the hardware first, so that we don't
		 * race to update prod and potentially move it backwards.
		 */
		atomic_set_release(&cmdq->owner_prod, prod);
	}
```

(b) 清 flag 之后新来者就不会再把 prod 并入自己的区间，而是去当下一个 owner，
所以 (d) 的 `writel` 一定是**该区间最后一个推进者**。步骤 (c) 的
`poll_valid_map` 是**等待别人写好**——owner 替整条队列做门铃，所以它必须确认
区间里所有命令都可见（valid map 置位）才敢敲。

### I.12.2 valid 位图与"两圈"问题

Intel 靠 256 槽的环 + 软件记账，ARM 多一个维度：**每个槽有一个 valid bit**
（`arm_smmu_cmdq_set_valid_map`，步骤 3，`:838-840`，前面还夹了一个 `dma_wmb()`
保证"命令先于 valid 可见"）。硬件只认 valid=1 的槽，所以多 CPU 乱序写入也没关系。

代价是判定"我这条命令执行完了"需要能区分 prod 绕了几圈。Linux 的处理是：

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:829-837 */
		/*
		 * In order to determine completion of our CMD_SYNC, we must
		 * ensure that the queue can't wrap twice without us noticing.
		 * We achieve that by taking the cmdq lock as shared before
		 * marking our slot as valid.
		 */
		arm_smmu_cmdq_shared_lock(cmdq);
```

**共享锁不是用来保护写入的，是用来限制"同时在飞的命令数 ≤ 一圈"的**。等 CMD_SYNC
完成后再解锁（步骤 5，`:885-893`），如果自己是最后一个 reader 才更新 `llq.cons`。
这个"锁只服务于回绕检测"的设计在阅读时很容易被误认为普通临界区。

### I.12.3 CMD_SYNC 的完成：轮询或 MSI，两条路

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:735-743 */
static int arm_smmu_cmdq_poll_until_sync(struct arm_smmu_device *smmu,
					 struct arm_smmu_cmdq *cmdq,
					 struct arm_smmu_ll_queue *llq)
{
	if (smmu->options & ARM_SMMU_OPT_MSIPOLL &&
	    !arm_smmu_cmdq_needs_busy_polling(smmu, cmdq))
		return __arm_smmu_cmdq_poll_until_msi(smmu, cmdq, llq);

	return __arm_smmu_cmdq_poll_until_consumed(smmu, cmdq, llq);
}
```

**这是三家唯一提供"中断路径等完成"的实现**。`ARM_SMMU_OPT_MSIPOLL` 打开时
CMD_SYNC 可以带 MSI 标志、由事件队列中断唤醒等待者；`needs_busy_polling` 则用于
"当前已经有很多人在忙等，改用轮询更划算"的回退判定。与 Intel 一律关中断自旋
（I.5.2）对照，这是**架构设计上的真实分歧**，不是实现进度差异：ARM 明确允许
在持有自旋锁/中断关闭时退化为忙等（`ARM_SMMU_OPT_CMDQ_FORCE_SYNC` 也参与该
判定，见 I.12.4）。

### I.12.4 批量：64 条一批

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.h:381 */
#define CMDQ_BATCH_ENTRIES		BITS_PER_LONG
```

`arm_smmu_cmdq_batch_add()`（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:935-962`）攒够
`CMDQ_BATCH_ENTRIES` 就 `issue_cmdlist(..., sync = false)` 提交一次；
`arm_smmu_cmdq_batch_submit()`（`:964-968`）收尾时传 `sync = true`。

三个提前断开批的条件都在 `batch_add` 里（`:939-946`）：
`unsupported_cmd`（该队列不支持这条命令）与 `force_sync`
（**`num == BATCH-1` 且开了 `ARM_SMMU_OPT_CMDQ_FORCE_SYNC`**）。后者值得注意：
`OPT_CMDQ_FORCE_SYNC` 会让"每批最后一条"**换成带 sync 的提交**，
即用户可以用它牺牲吞吐换取更严格的失效点。

> 修正记录：起草本节时曾按 Intel 的 `QI_MAX_BATCHED_DESC_COUNT`（16，
> `drivers/iommu/intel/iommu.h:595`）推断 ARM 也是 16。实测 `BITS_PER_LONG`
> 在 arm64 = **64**。两家的批量上限恰好都是"一个自然常量"，容易串味，
> 引用前必须分别 grep。

### I.12.5 与 Intel 的结构性差异小结

| | Intel VT-d | ARM SMMUv3 |
|---|---|---|
| 并发提交 | `raw_spin_lock_irqsave(&qi->q_lock)` 全程 | `cmpxchg` 乐观预留 + owner 选举 |
| 有效标志 | 软件 `desc_status[]` | 硬件 valid bit + 软件状态 |
| 完成通知 | Wait Descriptor 写内存 + CPU 自旋 | CMD_SYNC；轮询**或** MSI |
| 错误恢复 | IQE/ITE/ICE 完整实现（I.6） | 只有 `dev_err_ratelimited("CMDQ timeout")` 与 `"CMD_SYNC timeout at 0x%08x [hwprod .., hwcons ..]"`（`:878-882`） |
| 队列空间不足 | 放锁 `cpu_relax()` 等 `free_cnt` | `poll_until_not_full`，同样先 `local_irq_restore` |

**ARM 没有与 IQE/ITE 对应的错误恢复路径**（就 `arm-smmu-v3.c` 的 CMDQ 代码而言，
本文未搜到等价机制），超时只是打印。这条差异的规范含义未在本仓库资料内核实，
但它对排障的直接影响是明确的：**Intel 上失效失败会重试并可能自愈，ARM 上会
留下 timeout 日志然后继续**。看 ARM 平台日志时 `CMDQ timeout` / `CMD_SYNC timeout`
是必须当故障处理的信号。

---

## I.13 AMD-Vi：8KB 环形队列与惰性完成等待

### I.13.1 队列基本参数

```c
/* 来源: drivers/iommu/amd/amd_iommu_types.h:236 */
#define CMD_BUFFER_SIZE    8192

/* 来源: drivers/iommu/amd/amd_iommu_types.h:74-75 */
#define MMIO_CMD_HEAD_OFFSET	0x2000
#define MMIO_CMD_TAIL_OFFSET	0x2008
```

命令长度即 `struct iommu_cmd { u32 data[4]; }`（`drivers/iommu/amd/iommu.c:69-71`）
= 16 字节，尾指针是**字节偏移**（不是槽号），推进即
`tail = (tail + sizeof(*cmd)) % CMD_BUFFER_SIZE`：

```c
/* 来源: drivers/iommu/amd/iommu.c:1198-1214 */
static void copy_cmd_to_buffer(struct amd_iommu *iommu,
			       struct iommu_cmd *cmd)
{
	u8 *target;
	u32 tail;

	/* Copy command to buffer */
	tail = iommu->cmd_buf_tail;
	target = iommu->cmd_buf + tail;
	memcpy(target, cmd, sizeof(*cmd));

	tail = (tail + sizeof(*cmd)) % CMD_BUFFER_SIZE;
	iommu->cmd_buf_tail = tail;

	/* Tell the IOMMU about it */
	writel(tail, iommu->mmio_base + MMIO_CMD_TAIL_OFFSET);
}
```

**每写一条命令就 `writel` 一次尾寄存器**（对比 Intel 一批只 `writel` 一次，
`drivers/iommu/intel/dmar.c:1444`；ARM 由 owner 代敲一次，`:862`）。这是 AMD 在高失效率场景 MMIO
写开销更高的直接原因。

空间判定是"剩多少"而不是"够几条"：

```c
/* 来源: drivers/iommu/amd/iommu.c:1359-1379（节选） */
	next_tail = (iommu->cmd_buf_tail + sizeof(*cmd)) % CMD_BUFFER_SIZE;
again:
	left      = (iommu->cmd_buf_head - next_tail) % CMD_BUFFER_SIZE;

	if (left <= 0x20) {
		/* Skip udelay() the first time around */
		if (count++) {
			if (count == LOOP_TIMEOUT) {
				pr_err("Command buffer timeout\n");
				return -EIO;
			}

			udelay(1);
		}

		/* Update head and recheck remaining space */
		iommu->cmd_buf_head = readl(iommu->mmio_base +
					    MMIO_CMD_HEAD_OFFSET);

		goto again;
	}
```

`left <= 0x20` 是"留 32 字节余量"的硬编码水位（不是 `count + 2` 那种可推导的
公式），读 head 要 `readl` MMIO（Intel 全程软件记账）。`LOOP_TIMEOUT` = 100000
（`drivers/iommu/amd/amd_iommu_types.h:469`）× 1μs ≈ **100ms** 上限后返回 `-EIO`。
`count++` 的"第一次跳过 udelay"写法意味着**先检查一轮再睡**，
轻微空闲时不会引入延迟。

### I.13.2 `need_sync`：把"要不要等"变成脏标志

```c
/* 来源: drivers/iommu/amd/iommu.c:1386-1388 */
	/* Do we need to make sure all commands are processed? */
	iommu->need_sync = sync;
```

而 `iommu_completion_wait()` 的第一句就是：

```c
/* 来源: drivers/iommu/amd/iommu.c:1427-1428 */
	if (!iommu->need_sync)
		return 0;
```

`iommu_queue_command()` 传 `sync = true`（`:1405-1408`），
`iommu_completion_wait()` 排队自己的 COM_WAIT 时传 `sync = false`（`:1435`）——
**排队 wait 这个动作本身把脏标志清掉**。

于是 AMD 的模型是：**多条失效命令共享一次完成等待**。真实结构见
`amd_iommu_domain_flush_pages()`（`drivers/iommu/amd/iommu.c:1700-1712`）：
先 `__domain_flush_pages()` 把域内每条 IOMMU 的 TLB 失效 + 每个 ATS 设备的
`device_flush_iotlb()` 全部排进各自队列（`:1686-1696`），**最后**统一
`domain_flush_complete(domain)`（`:1446-1459`，遍历 `domain->iommu_array`
逐台 `iommu_completion_wait`）。

这正是 Intel `qi_batch_*`（16 条一批）与 ARM `batch`（64 条一批）的对应物，
但机制不同：**Intel/ARM 是"攒描述符减少队列往返"，AMD 是"攒命令减少 completion
wait 往返"**——AMD 每条命令仍然单独敲尾寄存器。

### I.13.3 完成等待：单调序号 + `udelay` 轮询

```c
/* 来源: drivers/iommu/amd/iommu.c:1410-1414 */
static u64 get_cmdsem_val(struct amd_iommu *iommu)
{
	lockdep_assert_held(&iommu->lock);
	return ++iommu->cmd_sem_val;
}
```

```c
/* 来源: drivers/iommu/amd/iommu.c:1220-1228 */
	u64 paddr = iommu_virt_to_phys((void *)iommu->cmd_sem);

	memset(cmd, 0, sizeof(*cmd));
	cmd->data[0] = lower_32_bits(paddr) | CMD_COMPL_WAIT_STORE_MASK;
	cmd->data[1] = upper_32_bits(paddr);
	cmd->data[2] = lower_32_bits(data);
	cmd->data[3] = upper_32_bits(data);
	CMD_SET_TYPE(cmd, CMD_COMPL_WAIT);
```

和 Intel 的 wait 描述符同构：**硬件把一个值写到内存地址**。不同处是写的值
是**全局单调递增序号**，因此等待者是"比较大小"而非"等等于常量"：

```c
/* 来源: drivers/iommu/amd/iommu.c:1180-1193 */
	/*
	 * cmd_sem holds a monotonically non-decreasing completion sequence
	 * number.
	 */
	while ((__s64)(READ_ONCE(*iommu->cmd_sem) - data) < 0 &&
	       i < LOOP_TIMEOUT) {
		udelay(1);
		i += 1;
	}

	if (i == LOOP_TIMEOUT) {
		pr_alert("Completion-Wait loop timed out\n");
		return -EIO;
	}
```

`(__s64)(sem - data) < 0` 是**环绕安全的比较**——序号本身可能因溢出回绕，
差值按有符号解读仍然正确。这是三家之中唯一用"序号 + 有符号差"而非"标志位"
表达完成的设计。

但等待方式最差：**`udelay(1)` 逐微秒睡**，而 Intel/ARM 用 `cpu_relax()`。
超时后 `pr_alert`（不是 `pr_err`）+ `-EIO`。也就是说 AMD 的完成判定延迟下限
是 1μs 级，**空转成本比 Intel 高、响应比 Intel 慢**（Intel 的 `cpu_relax` 循环
在硬件写回后纳秒级退出）。

**注意不要把 `udelay` 读成 bug**：它换来的是不需要 `cpu_relax` 那种紧密读
同一缓存行的行为，在共享 `cmd_sem` 的场景下可能更友好。本文只陈述"三家写法
不同、AMD 的粒度是 1μs"这一事实。

### I.13.4 NpCache：过度失效代价的直接暴露

```c
/* 来源: drivers/iommu/amd/iommu.c:1714-1723（节选） */
	/*
	 * When NpCache is on, we infer that we run in a VM and use a vIOMMU.
	 * In such setups it is best to avoid flushes of ranges which are not
	 * naturally aligned, since it would lead to flushes of unmodified
	 * PTEs. Such flushes would require the hypervisor to do more work than
	 * necessary. Therefore, perform repeated flushes of aligned ranges
	 * until you cover the range. Each iteration flushes the smaller
	 * between the natural alignment of the address that we flush and the
	 * greatest naturally aligned region that fits in the range.
	 */
	while (size != 0) {
		int addr_alignment = __ffs(address);
		int size_alignment = __fls(size);
		...
		__domain_flush_pages(domain, address, flush_size);
		address += flush_size;
		size -= flush_size;
	}
```

**这是本章所有"过度失效"讨论的唯一上游证据。** 平时一条 AMD 命令多刷几页无所谓
（硬件自己消化），但一旦跑在 vIOMMU 后面，每一次失效都要陷到宿主 emulate，
多刷 = 多做无用功。于是 AMD 加了一个"把未对齐范围**拆成多条自然对齐失效**"
的分支，用命令数量换覆盖精度。

对 KVM 场景的推论（**这是从代码结构推出的，不是文档既有结论**）：
`amd_iommu=...` 的 NpCache（Not Present Cache）在嵌套虚拟化里打开时，
guest 内每次 `dma_unmap` 会产生**多条**命令而非一条。所以"IOMMU 命令风暴导致
guest DMA 抖动"这个现象在 AMD 平台 + vIOMMU 组合下会被 I.3.5 的对齐策略
放大，且**症状是命令条数变多、每条覆盖范围变小**。Intel 侧对应的放大因子
是 batch 上限 16（I.12.4 表），ARM 侧是 `RANGE_INV` 缺失时逐页一条
（I.3.6）。三家各有各的放大机制，排查前先确认自己在哪一家。

---

## I.14 三家横向对照

| 维度 | Intel VT-d | ARM SMMUv3 | AMD-Vi |
|---|---|---|---|
| 队列容量 | 256 槽（`drivers/iommu/intel/iommu.h:378`） | `max_n_shift` 可配（`CMDQ_CONS`/`PROD`） | 8192 字节 / 512 条（`CMD_BUFFER_SIZE`、`CMD_BUFFER_ENTRIES`，`drivers/iommu/amd/amd_iommu_types.h:235-237`） |
| 槽位宽度 | 16B legacy / 32B scalable，`qi_shift()` 运行期算 | 定长 16B（`CMDQ_ENT_SZ_SHIFT` = 4，`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.h:364`） | 定长 16B（`struct iommu_cmd { u32 data[4]; }`，`drivers/iommu/amd/iommu.c:69-71`） |
| 并发控制 | `raw_spin_lock_irqsave` 全程 | `cmpxchg` 乐观预留 + owner 选举 | `raw_spin_lock_irqsave` 全程 |
| 空间判定 | 软件 `free_cnt < count + 2` | `queue_has_space` + `poll_until_not_full` | `readl(head)`，`left <= 0x20` |
| 门铃 | 一批一次 `writel` | owner 代敲一次 | **每命令一次 `writel`** |
| 批量上限 | 16 描述符（`drivers/iommu/intel/iommu.h:595`） | `BITS_PER_LONG` = 64（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.h:381`） | 无描述符批量，靠 `need_sync` 合并等待 |
| 范围编码 | 三套（AM / AM+PSI / S 位最低 0） | `num × 2^scale`，5 位分段 | `msb_diff` 以下全 1 |
| 完成语义 | wait 写常量 `QI_DONE`，CPU `cpu_relax` | CMD_SYNC，轮询**或 MSI** | COM_WAIT 写单调序号，`udelay(1)` |
| 等待是否关中断 | **是**（注释明示防中断死锁） | 是（`local_irq_save`），但 MSI 路径可让出 | 是（`raw_spin_lock_irqsave`） |
| 硬件错误恢复 | IQE 复制 wait / ITE 中止全批 + 判设备在场 / ICE 清位 | 仅 `dev_err_ratelimited` | 仅 `pr_alert("Completion-Wait loop timed out")` |
| 不遵守推荐时序 | ATS 关闭时 Device-TLB 失效延后（陷阱 14d） | — | — |

**一句话概括三家哲学**：Intel 用**一把锁 + 一条自旋**换最简单正确的同步；
ARM 用**无锁预留 + 领导者选举**换多核扩展性；AMD 用**惰性合并等待**换命令数，
却在门铃和 `udelay` 上把开销还了回来。

---

## I.15 观测：tracepoint 与延迟直方图

失效路径是内核少数"每条命令都有 tracepoint + 硬件计数器"的区域。

### I.15.1 tracepoint

```c
/* 来源: drivers/iommu/intel/trace.h:21 */
TRACE_EVENT(qi_submit,
```

`qi_submit` 在**持锁路径内部**逐条打印 4 个 qw（`drivers/iommu/intel/dmar.c:1421-1422`），所以
打开它会给每次提交加上格式化开销——**测延迟时不要同时开 `qi_submit`**，
否则会测到 trace 本身。

`cache_tag_*` 一族（`drivers/iommu/intel/trace.h:123-184`）：
`cache_tag_assign` / `cache_tag_unassign` / `cache_tag_flush_all` /
`cache_tag_flush_range` / `cache_tag_flush_range_np`。它们打印的是
`(tag, start, end, addr, pages, mask)`——**恰好是 I.3 三套编码的输入侧**，
因此可以直接用来核对 `calculate_psi_aligned_address()` 的换算是否符合预期。
`cache_tag_flush_range` 的调用点在 `drivers/iommu/intel/cache.c:474`，`cache_tag_flush_range_np`
在 `:549`。

### I.15.2 `dmar_perf_latency`：三档可选的延迟直方图

```c
/* 来源: drivers/iommu/intel/debugfs.c:767-768 */
	debugfs_create_file("dmar_perf_latency", 0644, intel_iommu_debug,
			    NULL, &dmar_perf_latency_fops);
```

写入值的语义（`drivers/iommu/intel/debugfs.c:708-739`）：

| `echo N` | 效果 |
|---|---|
| `0` | 关闭全部三档计数 |
| `1` | `DMAR_LATENCY_INV_IOTLB` |
| `2` | `DMAR_LATENCY_INV_DEVTLB` |
| `3` | `DMAR_LATENCY_INV_IEC` |

**`case 1/2/3` 各自只 enable 一种，且都不关闭别的档** —— `echo 1` 之后再
`echo 2` 是**两档同时计数**，不是切换档位。唯一会关闭的分支是 `0`，它把三种
逐个 `dmar_latency_disable()`。

`echo 0` 这一路还有个必须知道的实现细节，`dmar_latency_disable()`
（`drivers/iommu/intel/perf.c:57`）清的是：

```c
/* 来源: drivers/iommu/intel/perf.c:66 */
	memset(&lstat[type], 0, sizeof(*lstat) * DMAR_LATENCY_NUM);
```

**长度按整个数组算（3 × `sizeof(struct latency_statistic)` = 288B），起点却是
`&lstat[type]`**。于是 `type = INV_IOTLB (0)` 时正好一次抹掉三档；
`type = INV_DEVTLB (1)` / `INV_IEC (2)` 时不仅连带抹掉后面的档，还会越过
`kcalloc(DMAR_LATENCY_NUM, sizeof(*lstat))`（`drivers/iommu/intel/perf.c:36-37`）
分配的块尾 `type × 96` 字节。6.12.93 源码如此，**上游是否已修未核实**。
实操结论只有一条：**测之前先 `echo 0` 再 `echo N`，中途不换档**，否则看到的
直方图可能已被旁档污染或清零。

顺带一提，快照用的行名表 `latency_type_names[]`（`drivers/iommu/intel/perf.c:111-114`）
里有第 4 项 `"svm_prq"`，但 `enum latency_type` 只有三档、全树也只有三处
`dmar_latency_update()` 调用点，所以这一行永远打不出来。

读取时 `latency_show()`（`drivers/iommu/intel/debugfs.c:671`）先调
`dmar_latency_snapshot()`（`:667`）再打印——即**读操作会先取快照**，
快照只打印 `enabled` 的档（`drivers/iommu/intel/perf.c:130-131`）。

埋点位置就在 `qi_submit_sync()` 开头与结尾：按描述符类型分别打时间戳
（`drivers/iommu/intel/dmar.c:1384-1396`），返回前 `dmar_latency_update()`
（`:1480-1490`）。

一个必须知道的观测者效应：**`iotlb_start_ktime` 的采集条件是
`dmar_latency_enabled(...)`**（`drivers/iommu/intel/dmar.c:1386-1388`），也就是
`ktime_get()` 只在开启计数时执行。不开就是零开销，开了则每批多 2 次
`ktime_get()`——用它测出来的绝对值本身含测量成本，**只适合看分布与趋势，
不适合当基线**。这正是 practice 3 用它对比 strict / lazy 的合法性来源：
比较的是同一开关状态下的两组分布。

### I.15.3 与 I.1 失效链的观测呼应

I.1 的失效链 + 本节的埋点，凑齐三个可执行问题：

- "这次 unmap 到底刷了多大范围？" → `cache_tag_flush_range` 的 `pages`/`mask`
- "刷得慢是 IOTLB 还是 Device-TLB？" → `echo 1` vs `echo 2` 分别看直方图
- "有没有触到错误恢复？" → `dmesg` 里
  `Invalidation Queue Error (IQE) cleared` / `Invalidation Time-out Error (ITE) cleared`
  / `Invalidation Completion Error (ICE) cleared` / `Using ...`（`pr_info` 级，
  默认控制台可见）

---

## I.16 自检问题

1. `qi_desc_iotlb()` 里 `int ih = addr & 1;` 的 `addr` 从哪来、为什么能带信息？
   如果有人在中间对 `addr` 做过一次 `>>= 1` 会怎样？
   （I.3.1：`drivers/iommu/intel/cache.c:384` 的 `addr | ih`；会静默丢掉 IH → 变成强制全量刷
   paging-structure cache）
2. 为什么 `qi_desc_dev_iotlb()` 用 `mask - 1` 不算 off-by-one？
   （I.3.3 + I.3.4：`calculate_psi_aligned_address()` 的 `ALIGN_DOWN` 保证边界位
   本来就是 0）
3. `qi_submit_sync()` 的 `free_cnt < count + 2` 里两个 `+1` 各是什么？
   （I.4.2：wait 描述符 + 满/空区分的哨兵槽）
4. `count == 0` 时 `for (i = 0; i <= count; i++)` 为什么必须包含等号？
   （I.4.3：让尾部孤立 wait 槽转 `QI_FREE`，否则 `reclaim_free_desc()` 推不动）
5. ITE 时为什么把 **head 到 tail 的所有** `QI_IN_USE` 都标成 `QI_ABORT`，
   而不只标自己那一个？
   （I.6.2：规范 6.5.2.11 规定硬件遇 DEVTLB 超时不再完成任何 pending wait）
6. `pci_device_is_present()` 那次判定防的是什么死锁？
   （I.6.2：设备已拔 → Device-TLB 失效永不完成 → I.5.2 的关中断自旋里无解）
7. 把 `cache_tag_assign()` 的 `list_add_tail` 改成 `list_add` 会违反什么？
   （I.7：规范 6.5.2.5 要求的 IOTLB-before-Device-TLB 顺序，且无任何报错）
8. 为什么改 S2 会全刷设备 ATS 缓存而改 S1 不会？
   （I.8：合成结果无法反查 S2 中间条目）
9. `__ffs(smmu_domain->domain.pgsize_bitmap)` 在 SMMUv3 失效路径里决定什么？
   （I.3.7：`tg`，进而决定 `ttl` 与每命令覆盖页数——`pgsize_bitmap` 第三次成为
   唯一跨层语言）
10. AMD 一次 `amd_iommu_domain_flush_pages()` 里，
    尾寄存器被 `writel` 多少次、`cmd_sem` 被写多少次？
    （I.13.2 + I.13.1：命令条数 × 1 次 tail；COM_WAIT 1 次）
11. 为什么 `udelay(1)` 轮询意味着 AMD 的失效完成延迟下限是微秒级，而 Intel 不是？
    （I.13.3 对照 I.5.2）
12. 用 `dmar_perf_latency` 测出的绝对延迟能当基线吗？为什么？
    （I.15.2：不能，`ktime_get()` 本身就是被测对象的一部分）
