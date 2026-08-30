# 源码精读：一次设备 DMA 的完整调用链走读

> 本文是 README §3"主流程"的带行号版本：把 **probe → 选域 → attach → map →
> unmap → 失效** 六段调用链逐段贴出源码，每一步标注"它在回答 8 个问题里的哪一个"。
> 先读 README §3 再进本文；机制深挖在各问题文档里，本文不重复。
>
> 源码基线：`/root/code/linux-6.12.93`。行号可用
> [`practice/check-citations.sh`](practice/check-citations.sh) 复核。

## 📖 目录

- [A.1 probe：设备进入 IOMMU 子系统](#a1-probe设备进入-iommu-子系统)
- [A.2 选域：组内投票与四级回落](#a2-选域组内投票与四级回落)
- [A.3 attach：把域装到组上](#a3-attach把域装到组上)
- [A.4 map：一次 `dma_map()` 建进页表](#a4-map一次-dma_map-建进页表)
- [A.5 unmap 与失效：同步语义的落点](#a5-unmap-与失效同步语义的落点)
- [A.6 运行期换域：sysfs 的 `type` 文件](#a6-运行期换域sysfs-的-type-文件)
- [A.7 全链路图与问题映射](#a7-全链路图与问题映射)

---

## A.1 probe：设备进入 IOMMU 子系统

入口 `iommu_probe_device()`（`drivers/iommu/iommu.c:600`）只是加锁外壳，真正
的逻辑在 `__iommu_probe_device()`（`drivers/iommu/iommu.c:513`）：

```c
/* 来源: drivers/iommu/iommu.c:540-544（节选） */
	/* Device is probed already if in a group */
	if (dev->iommu_group)
		return 0;

	ret = iommu_init_device(dev, ops);
```

**幂等判断只看"在不在组里"**——[Q2 G.2](group.md) 的直接证据：设备在 sysfs
里有 group 链接不等于驱动绑定完成，只代表这一步走过。

`iommu_init_device()`（`drivers/iommu/iommu.c:402`）里两次调进后端：

```c
/* 来源: drivers/iommu/iommu.c:416,427（两个调用点） */
	iommu_dev = ops->probe_device(dev);
	...
	group = ops->device_group(dev);
```

- `ops->probe_device` 是三家后端各自实现的设备档案建立（[Q8 B.5](backends.md)）；
- `ops->device_group` 返回设备所属的组——**组的划分判据在
  [phase6 §1.4/§1.5](../phase6-vfio/README.md)（ACS/quirk），构造与复用
  在 [Q2](group.md)**。

回到 `__iommu_probe_device()`，入组之后的顺序是关键：

```c
/* 来源: drivers/iommu/iommu.c:556-571（节选） */
	/*
	 * The gdev must be in the list before calling
	 * iommu_setup_default_domain()
	 */
	list_add_tail(&gdev->list, &group->devices);
	WARN_ON(group->default_domain && !group->domain);
	if (group->default_domain)
		iommu_create_device_direct_mappings(group->default_domain, dev);
	if (group->domain) {
		ret = __iommu_device_set_domain(group, dev, group->domain, 0);
		...
	} else if (!group->default_domain && !group_list) {
		ret = iommu_setup_default_domain(group, 0);
		...
```

三个分支对应组的三种到达状态：组已有域（后来的成员直接挂上）、组首次有成员
（走 A.2 的选域）、批量探测（延后给调用者统一处理）。**先 `list_add_tail`
后选域**的顺序不是随意的：选域要遍历组成员投票（A.2），没入链表的设备不参与
——这就是"顺序不能换"的源码注释。

最后：

```c
/* 来源: drivers/iommu/iommu.c:582-583 */
	if (group->default_domain)
		iommu_setup_dma_ops(dev);
```

设备从此带着 `dev->dma_iommu` 标志（[Q7 U.2](userspace.md)），README §3.2
的总分支在这里装上。

---

## A.2 选域：组内投票与四级回落

`iommu_setup_default_domain()`（`drivers/iommu/iommu.c:2950`）分两步：先**定
类型**，再**分配域**。

### A.2.1 定类型：逐成员投票

`iommu_get_default_domain_type()`（`drivers/iommu/iommu.c:1726`）遍历组：

```c
/* 来源: drivers/iommu/iommu.c:1747-1749 */
	for_each_group_device(group, gdev) {
		driver_type = iommu_get_def_domain_type(group, gdev->dev,
							driver_type);
```

单个成员的投票逻辑在 `iommu_get_def_domain_type()`（`drivers/iommu/iommu.c:1684`）：
后端声明了静态 `default_domain` 就用它的类型（`:1690-1695`）；否则调
`ops->def_domain_type(dev)`（AMD 在这里，[Q8 B.4.3](backends.md)）。**两个成员
投出冲突类型时，IDENTITY 优先**：

```c
/* 来源: drivers/iommu/iommu.c:1716-1719 */
	if (type == IOMMU_DOMAIN_IDENTITY)
		return type;
	return cur_type;
```

回到组级，还有三道后处理（`drivers/iommu/iommu.c:1760-1789`）：

1. **`CONFIG_IOMMU_DMA` 未编入** ⇒ 没有 dma-iommu 可用，只能强制
   IDENTITY（`:1767-1772`）；
2. **untrusted 设备**（雷电/USB4 外接）**强制 DMA 域**，若驱动还想覆盖成
   别的类型直接拒绝探测（`:1774-1783`）——安全策略硬编码在选域里；
3. `target_type`（sysfs 换域时传入）与投票结果冲突则失败（`:1785-1789`）。

**类型返回 0 表示"用全局默认"**——即 `iommu.passthrough`/`iommu.strict`
改的那个 `iommu_def_domain_type`（[Q3](domains.md) 的决策链起点）。

### A.2.2 分配域：四级回落

`iommu_group_alloc_default_domain()`（`drivers/iommu/iommu.c:1605`）：

```c
/* 来源: drivers/iommu/iommu.c:1627-1639（节选） */
	/* The driver gave no guidance on what type to use, try the default */
	dom = __iommu_group_alloc_default_domain(group, iommu_def_domain_type);
	if (!IS_ERR(dom))
		return dom;

	/* Otherwise IDENTITY and DMA_FQ defaults will try DMA */
	if (iommu_def_domain_type == IOMMU_DOMAIN_DMA)
		return ERR_PTR(-EINVAL);
	dom = __iommu_group_alloc_default_domain(group, IOMMU_DOMAIN_DMA);
	if (IS_ERR(dom))
		return dom;

	pr_warn("Failed to allocate default IOMMU domain of type %u for group %s - Falling back to IOMMU_DOMAIN_DMA",
		...);
```

四级：**后端静态 `ops->default_domain`（legacy）→ 要求的类型 → 全局默认
`iommu_def_domain_type` → 回落 DMA 并 `pr_warn`**。看到开机日志里那条
"Falling back to IOMMU_DOMAIN_DMA"，说明命令行想要的类型在这里分配失败了。
IDENTITY/BLOCKED 的"静态单例"捷径在 `__iommu_domain_alloc()` 里
（[Q3 D.5](domains.md)）。

### A.2.3 建域之后：先补直通映射，再挂域

```c
/* 来源: drivers/iommu/iommu.c:2973-2989（节选） */
	/*
	 * IOMMU_RESV_DIRECT and IOMMU_RESV_DIRECT_RELAXABLE regions must be
	 * mapped before their device is attached, in order to guarantee
	 * continuity with any FW activity
	 */
	direct_failed = false;
	for_each_group_device(group, gdev) {
		if (iommu_create_device_direct_mappings(dom, gdev->dev)) {
	...
	/* We must set default_domain early for __iommu_device_set_domain */
	group->default_domain = dom;
```

`IOMMU_RESV_DIRECT`（固件还在用的保留区，[Q4 IO.5](iova.md)）必须在
attach **之前**建好映射——否则设备挂到新域的瞬间，固件的 DMA 就断了。

---

## A.3 attach：把域装到组上

首次挂域走的是"不允许失败"通道：

```c
/* 来源: drivers/iommu/iommu.c:2990-2999（节选） */
	if (!group->domain) {
		/*
		 * Drivers are not allowed to fail the first domain attach.
		 * The only way to recover from this is to fail attaching the
		 * iommu driver and call ops->release_device. Put the domain
		 * in group->default_domain so it is freed after.
		 */
		ret = __iommu_group_set_domain_internal(
			group, dom, IOMMU_SET_DOMAIN_MUST_SUCCEED);
```

`__iommu_group_set_domain_internal()`（`drivers/iommu/iommu.c:111`）逐设备调
`__iommu_device_set_domain()`，最终落到 `ops->attach_dev()`（或新式的
`ops->set_dev_pasid`）。**attach 在硬件上写什么，取决于域类型**：普通域写
页表根指针（VT-d context entry / STE），IDENTITY 域写各家自己的"恒等表达"
（[Q8 B.4](backends.md)：PGTT_PT / bypass STE / DTE[Mode]=0）。

attach 完成后 `cache_tag` 记账（Intel，失效用的反向索引，
[Q5 I.8](invalidation.md)），随后 A.1 末尾的 `iommu_setup_dma_ops()` 给驱动
接上 `dma_map_*()`。

---

## A.4 map：一次 `dma_map()` 建进页表

内核驱动路径（[Q4](iova.md) 全程）：驱动 `dma_map_page()` → DMA API 分发
（README §3.2）→ `iommu_dma_map_page()`（`drivers/iommu/dma-iommu.c:1163`）
分配 IOVA → `iommu_map()`。core 侧：

```c
/* 来源: drivers/iommu/iommu.c:2510-2528（节选） */
int iommu_map(struct iommu_domain *domain, unsigned long iova,
	      phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	...
	ret = __iommu_map(domain, iova, paddr, size, prot, gfp);
	if (ret == 0 && ops->iotlb_sync_map) {
		ret = ops->iotlb_sync_map(domain, iova, size);
		...
```

两个细节：

1. **失败即回滚**：`__iommu_map()` 中途失败，`iommu_map()` 末尾
   `iommu_unmap()` 掉已建部分（`drivers/iommu/iommu.c:2532-2534`）——map 的事务性由
   core 保证，后端不用管。
2. **`iotlb_sync_map` 是"建表方向"的失效钩子**：[Q5 I.2](invalidation.md)
   讲过 caching-mode 硬件上连 map 都要失效，走的就是这个回调
   （`cache_tag_flush_range_np`，Intel）。

`__iommu_map()`（`drivers/iommu/iommu.c:2447`）内部按 `iommu_pgsize()`
逐段循环（[Q1 T.3](translation.md)：`pgsize_bitmap` 是唯一的跨层协商语言），
调后端：

```c
/* 来源: drivers/iommu/iommu.c:2486 */
		ret = ops->map_pages(domain, iova, paddr, pgsize, count, prot,
```

Intel 落到 `try_cmpxchg64()` 改 `dma_pte`，ARM/AMD 落到 io-pgtable 库
（[Q8 B.2](backends.md)）。最后一步是 trace：

```c
/* 来源: drivers/iommu/iommu.c:2505 */
		trace_map(orig_iova, orig_paddr, orig_size);
```

对应 `iommu:map` 事件（`include/trace/events/iommu.h:79`，同时打出
**iova 与 paddr**）——practice 实验 2 就靠它证明"总线地址 ≠ 物理地址"。

---

## A.5 unmap 与失效：同步语义的落点

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

三行就是 [Q5](invalidation.md) 的总纲：`__iommu_unmap()`（`drivers/iommu/iommu.c:2540`）
逐段 `ops->unmap_pages()`（调用点 `:2579`）并把范围累积进 `iotlb_gather`；
随后 `iommu_iotlb_sync()` **同步**等到所有缓存（IOTLB / paging-structure
cache / Device-TLB）确认失效才返回。trace 在拆表后：

```c
/* 来源: drivers/iommu/iommu.c:2590 */
	trace_unmap(orig_iova, size, unmapped);
```

（`iommu:unmap`，`include/trace/events/iommu.h:103`，注意它带
`unmapped_size`——与请求 `size` 不等时说明中途失败回滚了一部分。）

链路的"另一头"——**设备访问了没映射的地址**——走的是反向入口
`iommu_report_device_fault()`，trace 点：

```c
/* 来源: drivers/iommu/iommu.c:2714 */
	trace_io_page_fault(dev, iova, flags);
```

（`iommu:io_page_fault`，`include/trace/events/iommu.h:153`。）Intel 的
`dmar_fault` IRQ（[Q8 B.6](backends.md)）与 ARM 的 evtq 线程最终都汇到这里
或各家的 fault handler；PRI/PRQ 那条"缺页可恢复"的路见
[Q5 I.10](invalidation.md)。practice 实验 4 用它观测。

---

## A.6 运行期换域：sysfs 的 `type` 文件

组有一个可读写的 `type` 属性（`drivers/iommu/iommu.c:925`，show 函数
`:890`——它打印的字符串是 `blocked/identity/DMA/DMA-FQ/auto`，与
`iommu_domain_type_str()` 的 "Passthrough/Translated" 用词**不同**，两处
都是源码里真实存在的命名，写脚本时别混）。

写入路径 `iommu_group_store_type()`（`drivers/iommu/iommu.c:3047`）先解析
四种取值，然后有一处特例：

```c
/* 来源: drivers/iommu/iommu.c:3071-3074（节选） */
	/* We can bring up a flush queue without tearing down the domain. */
	if (req_type == IOMMU_DOMAIN_DMA_FQ &&
	    group->default_domain->type == IOMMU_DOMAIN_DMA) {
		ret = iommu_dma_init_fq(group->default_domain);
```

**`DMA → DMA-FQ` 是唯一可以不解绑驱动在线做的切换**（同一个域对象上长出
flush queue）；其余方向都要先解绑组内所有驱动（[Q3 D.6](domains.md)、
[Q5 I.11](invalidation.md)）。practice 实验 3 用它做 strict/lazy 对照。

---

## A.7 全链路图与问题映射

```
设备出现在总线
   │
   ▼  A.1
iommu_probe_device → __iommu_probe_device (drivers/iommu/iommu.c:513)
   ├─ ops->probe_device            (drivers/iommu/iommu.c:416)   → Q8 B.5
   ├─ ops->device_group            (drivers/iommu/iommu.c:427)   → Q2（判据在 phase6 §1.4）
   ├─ list_add_tail（先入组）       (drivers/iommu/iommu.c:560)   → Q2 G.2
   │
   ▼  A.2（组首次有成员时）
iommu_setup_default_domain (drivers/iommu/iommu.c:2950)
   ├─ 类型投票 (1726/1684)：驱动意见 → 冲突时 IDENTITY 胜 → untrusted 强制 DMA
   ├─ 分配回落 (1605)：要求类型 → iommu_def_domain_type → DMA + pr_warn
   ├─ RESV_DIRECT 预映射 (2979)                    → Q4 IO.5
   │                                                → Q3（iommu.passthrough 改的就是这里）
   ▼  A.3
__iommu_group_set_domain_internal (drivers/iommu/iommu.c:111)
   └─ ops->attach_dev / set_dev_pasid              → Q8 B.4（硬件表达）
                                                     Q5 I.8（cache_tag 记账）
   ▼  A.1 收尾
iommu_setup_dma_ops → dev->dma_iommu               → Q7 U.2 / README §3.2
   │
   │   驱动运行期：
   ▼  A.4
dma_map_* → iommu_dma_map_page (drivers/iommu/dma-iommu.c:1163)  → Q4（IOVA 分配）
        → iommu_map (drivers/iommu/iommu.c:2510)
          → ops->map_pages (2486)                  → Q1（页表）/ Q8 B.2
          → trace_map (2505)                        → practice 实验 2
   ▼  A.5
dma_unmap_* → iommu_unmap (drivers/iommu/iommu.c:2594)           → Q5 全篇
   ├─ ops->unmap_pages (2579) + gather 累积
   ├─ iommu_iotlb_sync（同步等硬件）                → Q5 I.1/I.9
   └─ trace_unmap (2590)
fault 方向：dmar_fault/evtq → iommu_report_device_fault (2714) → Q8 B.6 / Q5 I.10
```

| 链路节点 | 对应问题 | 一句话 |
|---|---|---|
| `ops->device_group` | Q2 | 组边界 = 硬件隔离能力的边界 |
| 类型投票 / `iommu_def_domain_type` | Q3 | `iommu.passthrough` 只是投票的输入之一 |
| IOVA 分配（`iommu_dma_map_page`） | Q4 | DMA 地址是发出来的，不是算出来的 |
| `iommu_iotlb_sync` | Q5 | unmap 的返回 = 失效完成的证据 |
| `IOMMU_RESV_MSI` / SW_MSI | Q6 | MSI 地址走不走翻译，看后端 |
| `iommu_map` 的三个消费者 | Q7 | 动词相同，域类型与页面责任不同 |
| `ops->map_pages` / attach 的硬件写入 | Q8 | 一套抽象，三副实现 |
