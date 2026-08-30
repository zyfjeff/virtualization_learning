# Q4：驱动拿到的"DMA 地址"为什么不是 CPU 物理地址？

> **问题**：`dma_map_*()` 返回的 `dma_addr_t` 经常被当成"物理地址换个名字"。
> 它到底是什么？为什么同一个设备在 `iommu.passthrough=0/1` 下打印出来的地址完全不一样？
> 为什么 identity 域下"设备 A 看设备 B 的 BAR"（peer/P2P）没有机制能表达？
>
> **为什么值得问**：主流程（[README 三](README.md#三主流程一次设备-dma-的完整旅程)）的
> 第 ③ 步——IOVA 分配——是全流程最容易搞反的一环：输入是 `phys_addr_t`，输出是
> `dma_addr_t`，但**两者之间没有任何函数关系**。把 ① 当成"物理地址换个名字"会一路错到底：
> 错判设备走没走 IOMMU、错判 GDR 为什么不通、错判保留区的含义。本篇把主流程里那条
> 三分支的三个走向各自讲透。

---

## 📖 目录

- [IO.1 分岔点：这台设备到底走不走 IOMMU](#io1-分岔点这台设备到底走不走-iommu)
- [IO.2 IOVA 是分配出来的，不是换算出来的](#io2-iova-是分配出来的不是换算出来的)
- [IO.3 分配器内部：三层结构](#io3-分配器内部三层结构)
- [IO.4 "32 位优先"是两个闸门，不是一个](#io4-32-位优先是两个闸门不是一个)
- [IO.5 保留区：谁能往里放东西，谁消费](#io5-保留区谁能往里放东西谁消费)
- [IO.6 两个"不等号"：phys→dma 平移与 swiotlb](#io6-两个不等号physdma-平移与-swiotlb)
- [IO.7 peer/P2P：identity 域下的死结](#io7-peerp2pidentity-域下的死结)
- [IO.8 自检问题](#io8-自检问题)

---

## IO.1 分岔点：这台设备到底走不走 IOMMU

x86 上 `get_dma_ops()` 恒为 NULL，所以 `dma_map_page_attrs()` 是三分支：

```c
/* 来源: kernel/dma/mapping.c:167-174 */
	if (dma_map_direct(dev, ops) ||
	    arch_dma_map_page_direct(dev, page_to_phys(page) + offset + size))
		addr = dma_direct_map_page(dev, page, offset, size, dir, attrs);
	else if (use_dma_iommu(dev))
		addr = iommu_dma_map_page(dev, page, offset, size, dir, attrs);
	else
		addr = ops->map_page(dev, page, offset, size, dir, attrs);
```

`use_dma_iommu()` 就是读 `dev->dma_iommu`（`include/linux/iommu-dma.h:13-16`），
而它只在一个地方被计算：

```c
/* 来源: drivers/iommu/dma-iommu.c:1747-1756（节选） */
void iommu_setup_dma_ops(struct device *dev)
{
	...
	dev->dma_iommu = iommu_is_dma_domain(domain);
	if (dev->dma_iommu && iommu_dma_init_domain(domain, dev))
		goto out_err;
```

注意它读的是 `iommu_get_domain_for_dev(dev)`（`drivers/iommu/iommu.c:2159`，返回
`group->domain`），也就是**调用那一刻组上正挂着哪个域**。全仓只有三处会调用它：

| 调用点 | 时机 | 后果 |
|---|---|---|
| `drivers/iommu/iommu.c:583` | 设备 probe | 正常路径 |
| `drivers/iommu/iommu.c:1829-1830` | 延迟建立的 group 批量选域后 | 冷启动顺序问题 |
| `drivers/iommu/iommu.c:3093-3095` | 写 sysfs `type` 之后（`iommu_group_store_type()`） | **`dma_iommu` 会随之刷新** |

由此得到两条不对称的行为，排查"这设备到底走没走 IOMMU"时必须一起想：

- 用 `echo DMA > /sys/kernel/iommu_groups/N/type` 换域，`dev->dma_iommu` **会**被重算；
- 但 VFIO 把 group 抢过去 attach 一个 `UNMANAGED` 域时**不会**触发任何一处调用，
  `dev->dma_iommu` 停留在 probe 时刻的值。反过来，一个在 VFIO 已持有 group 之后才
  probe 进来的设备（热插、改 `driver_override` 后重新 probe），
  `iommu_setup_default_domain()` 会在 `drivers/iommu/iommu.c:2970-2971` 因为
  `group->default_domain == dom` 直接返回，且 `if (!group->domain)`（`:2992`）不成立
  而不做 attach——于是它读到的 `group->domain` 是 VFIO 的域，`dma_iommu` 被算成
  **false**。

而 `group->domain` 挂的是哪种域，是 [Q3](domains.md) 的决策链定的。**这条分岔就是
Q3 与 Q4 的接缝**：`iommu.passthrough` 改的是域类型，域类型改的是这里走哪条分支。

---

## IO.2 IOVA 是分配出来的，不是换算出来的

这是全章最容易搞反的一件事。`iommu_dma_map_page()`（`drivers/iommu/dma-iommu.c:1163`）
的输入是 `phys_addr_t phys`，输出是 `dma_addr_t iova`，但**两者之间没有任何函数关系**：

```c
/* 来源: drivers/iommu/dma-iommu.c:762-802（节选） */
static dma_addr_t iommu_dma_alloc_iova(struct iommu_domain *domain,
		size_t size, u64 dma_limit, struct device *dev)
{
	...
	dma_limit = min_not_zero(dma_limit, dev->bus_dma_limit);

	if (domain->geometry.force_aperture)
		dma_limit = min(dma_limit, (u64)domain->geometry.aperture_end);
	...
	if (dma_limit > DMA_BIT_MASK(32) && dev->iommu->pci_32bit_workaround) {
		iova = alloc_iova_fast(iovad, iova_len,
				       DMA_BIT_MASK(32) >> shift, false);
		if (iova)
			goto done;

		dev->iommu->pci_32bit_workaround = false;
		dev_notice(dev, "Using %d-bit DMA addresses\n", bits_per(dma_limit));
	}

	iova = alloc_iova_fast(iovad, iova_len, dma_limit >> shift, true);
```

分配器要同时满足四条约束，每条都会改变你看到的地址：

| 约束 | 代码 | 观测后果 |
|---|---|---|
| 粒度 = 域支持的最小页 | `drivers/iommu/dma-iommu.c:685` `order = __ffs(domain->pgsize_bitmap)` | IOVA 永远 4K 对齐，即使 `dma_map_single` 长度只有 32 字节 |
| 落在 aperture 内 | `:689-703`、`:712-715` `init_iova_domain(iovad, 1UL << order, base_pfn)` | Intel 上 aperture 只有 1 页宽的限制来自 [Q1](translation.md) 的 `__DOMAIN_MAX_ADDR()` |
| 32 位优先 | `:794-801`，由 `iommu_dma_forcedac` 与 `dev->iommu->pci_32bit_workaround` 控制 | 驱动不设 `dma_set_mask_and_coherent()` 就先看到满屏 32 位地址，某次分配失败后才打印 `Using 64-bit DMA addresses`（IO.4 展开） |
| 避开保留区 | `:563-598` `iova_reserve_iommu_regions()` | 见 IO.5 |

保留区这一步里那个减号值得单独说一句：

```c
/* 来源: drivers/iommu/dma-iommu.c:524-531（节选） */
	resource_list_for_each_entry(window, &bridge->windows) {
		if (resource_type(window->res) != IORESOURCE_MEM)
			continue;

		lo = iova_pfn(iovad, window->res->start - window->offset);
		hi = iova_pfn(iovad, window->res->end - window->offset);
		reserve_iova(iovad, lo, hi);
	}
```

读 `reserved_regions` 时的常见误读是"这些是恒等映射区"。**不是**：这里是
`reserve_iova()`，含义是"IOVA 分配器永不把地址发进这段"，所以设备**看不到**这些地址，
而不是"这些地址按物理意义解释"。被保留的是 host bridge 的 MMIO 窗口（设备本来就不该
DMA 到那里）和 `iommu_dma_get_resv_regions()`（`drivers/iommu/dma-iommu.c:472`）报告的
平台保留区。完整的类型系统在 IO.5。

那个减号 `window->res->start - window->offset` 是 IO.6 的主题。

---

## IO.3 分配器内部：三层结构

`struct iova_domain` 是每个 DMA 域一份（严格说是一个 `IOMMU_DMA_IOVA_COOKIE` 类型的
cookie 一份，见 [Q3 D.5](domains.md#d5-一个反直觉的事实identity-域是全驱动单例)——
identity 域拿不到 cookie）。内部三层：

| 层 | 结构 | 作用 | 源码 |
|---|---|---|---|
| 1 | 红黑树 `rbroot` + `anchor` | 权威账本（已分配区间） | `iova.c:48`-`:57` |
| 2 | `cached_node` / `cached32_node` | 下一次分配的**起点** | `include/linux/iova.h:31-32`、`iova.c:61-68` |
| 3 | `rcaches[]`：per-CPU magazine + depot | 完全绕开 rbtree 的快路径 | `iova.c:565-591` |

### IO.3.1 账本层：一棵树、一个哨兵

```c
/* 来源: drivers/iommu/iova.c:48-57（init_iova_domain 内） */
	iovad->rbroot = RB_ROOT;
	iovad->cached_node = &iovad->anchor.node;
	iovad->cached32_node = &iovad->anchor.node;
	iovad->granule = granule;
	iovad->start_pfn = start_pfn;
	iovad->dma_32bit_pfn = 1UL << (32 - iova_shift(iovad));
	iovad->max32_alloc_size = iovad->dma_32bit_pfn;
	iovad->anchor.pfn_lo = iovad->anchor.pfn_hi = IOVA_ANCHOR;
	rb_link_node(&iovad->anchor.node, NULL, &iovad->rbroot.rb_node);
	rb_insert_color(&iovad->anchor.node, &iovad->rbroot);
```

树里存的是**已分配**区间（不是空闲区间）。`anchor` 是
`pfn_lo = pfn_hi = ~0UL`（`IOVA_ANCHOR`，`iova.c:17`）的哨兵，永远挂在根。
它存在的唯一理由是释放路径要区分"真分配"与哨兵：

```c
/* 来源: drivers/iommu/iova.c:233-237 */
static void free_iova_mem(struct iova *iova)
{
	if (iova->pfn_lo != IOVA_ANCHOR)
		kmem_cache_free(iova_cache, iova);
}
```

分配方向是**从 `limit_pfn` 往低走**（`alloc_iova()` 的注释：
`searching top-down from limit_pfn to iovad->start_pfn`，`iova.c:246-247`），
实际循环在 `__alloc_and_insert_iova_range()` 里用 `rb_prev()` 反向走树
（`iova.c:188-194`）。自上而下 + 起点缓存，短期效果是所有映射挤在高地址，
**这是 IOVA 空间碎片化的根本来源**。

### IO.3.2 起点缓存：为什么是**两个**

```c
/* 来源: drivers/iommu/iova.c:61-68 */
static struct rb_node *
__get_cached_rbnode(struct iova_domain *iovad, unsigned long limit_pfn)
{
	if (limit_pfn <= iovad->dma_32bit_pfn)
		return iovad->cached32_node;

	return iovad->cached_node;
}
```

一个 rbtree，**两条独立的游走起点**：`cached32_node` 服务 32 位窗口内的请求，
`cached_node` 服务全宽度请求。分开的理由是两者不能互相污染 —— 如果只有一条起点，
一次 64 位分配会把起点推到高位，随后所有 32 位请求都得从高位往回爬完整棵树，
"32 位优先"就变成了性能陷阱。

插入与删除时各自维护：

```c
/* 来源: drivers/iommu/iova.c:70-96 */
static void
__cached_rbnode_insert_update(struct iova_domain *iovad, struct iova *new)
{
	if (new->pfn_hi < iovad->dma_32bit_pfn)
		iovad->cached32_node = &new->node;
	else
		iovad->cached_node = &new->node;
}

static void
__cached_rbnode_delete_update(struct iova_domain *iovad, struct iova *free)
{
	struct rb_node *cached_iova;

	cached_iova = to_iova(iovad->cached32_node);
	if (free == cached_iova ||
	    (free->pfn_hi < iovad->dma_32bit_pfn &&
	     free->pfn_lo >= cached_iova->pfn_lo))
		iovad->cached32_node = rb_next(&free->node);

	if (free->pfn_lo < iovad->dma_32bit_pfn)
		iovad->max32_alloc_size = iovad->dma_32bit_pfn;
	...
```

注意 `insert_update` 用的是 `pfn_hi < dma_32bit_pfn` 而 `delete_update` 用的是
`pfn_lo < dma_32bit_pfn`：**跨 4G 边界的区间归到全宽度那条链**（因为它的
`pfn_hi` 不在 32 位窗口里），但释放它时算"曾占用过 32 位空间"。这不是笔误，
而是"`max32_alloc_size` 只在 32 位窗口可能变大时重置"这一语义所需（IO.4.2）。

### IO.3.3 快路径：magazine / depot / 100ms 回收

```c
/* 来源: drivers/iommu/iova.c:559-591 */
/*
 * As kmalloc's buffer size is fixed to power of 2, 127 is chosen to
 * assure size of 'iova_magazine' to be 1024 bytes, so that no memory
 * will be wasted. Since only full magazines are inserted into the depot,
 * we don't need to waste PFN capacity on a separate list head either.
 */
#define IOVA_MAG_SIZE 127

#define IOVA_DEPOT_DELAY msecs_to_jiffies(100)

struct iova_magazine {
	union {
		unsigned long size;
		struct iova_magazine *next;
	};
	unsigned long pfns[IOVA_MAG_SIZE];
};

struct iova_cpu_rcache {
	spinlock_t lock;
	struct iova_magazine *loaded;
	struct iova_magazine *prev;
};

struct iova_rcache {
	spinlock_t lock;
	unsigned int depot_size;
	struct iova_magazine *depot;
	struct iova_cpu_rcache __percpu *cpu_rcaches;
	struct iova_domain *iovad;
	struct delayed_work work;
};
```

三个数字各有来历，都是能直接验证的事实：

| 常量 | 值 | 为什么 | 源码 |
|---|---|---|---|
| `IOVA_MAG_SIZE` | 127 | `8 + 127*8 = 1024`，正好一个 2 的幂，`static_assert` 强制 | `iova.c:565`、`:576` |
| `IOVA_RANGE_CACHE_MAX_SIZE` | 6 | 只缓存 ≤ 2^6 页（256 页 = 1MB）的请求 | `iova.c:19` |
| `IOVA_DEPOT_DELAY` | 100 ms | depot 富余时按此周期归还给 rbtree | `iova.c:567`、`:695-704` |

按**尺寸分桶**：`iova_rcache_get()` 用 `log_size = order_base_2(size)` 选桶，
`log_size >= 6` 直接返回 0 走慢路径（`iova.c:855-865`）。也就是说
**网络收包那种 order-0 的小映射走 per-CPU 无竞争快路径，大块 DMA 走全局 rbtree**。

取用逻辑（`__iova_rcache_get()`，`iova.c:817-848`）是 slab 式的三段：
`loaded` 非空 → 直接 pop；否则 `prev` 非空 → `swap(prev, loaded)`；否则去
`depot` 拿一整本（并把 `loaded` 里剩的丢掉，见 `:836-841`）。
归还（`__iova_rcache_put`）时 `loaded` 满则与 `prev` 交换，`prev` 也满才整体压回
depot；depot 只在"满了的 magazine"时增长（`iova.c:672-690`）。

回收端 `iova_depot_work_func()` 的判据非常省电：

```c
/* 来源: drivers/iommu/iova.c:695-704（iova_depot_work_func 内） */
	spin_lock_irqsave(&rcache->lock, flags);
	if (rcache->depot_size > num_online_cpus())
		mag = iova_depot_pop(rcache);
	spin_unlock_irqrestore(&rcache->lock, flags);

	if (mag) {
		iova_magazine_free_pfns(mag, rcache->iovad);
		iova_magazine_free(mag);
		schedule_delayed_work(&rcache->work, IOVA_DEPOT_DELAY);
	}
```

`depot_size > num_online_cpus()` 的含义是"**每个 CPU 留一本，多出来的才还账**"。
注意 `magazine_free_pfns()` 会把整本 PFN 逐个 `free_iova_fast()` 回 rbtree ——
所以**空闲 IOVA 在高负载后不会立刻回到全局池，最长滞后约 100ms × depot 深度**。
这条直接决定了 `practice/` 里"释放后立刻检查可用空间"为什么可能看不到变化。

`rcache` 用 `__alloc_percpu(sizeof(*cpu_rcache), cache_line_size())` 对齐
（`iova.c:726-727`），并且注册了 CPU 热插拔回调
`cpuhp_state_add_instance_nocalls(CPUHP_IOMMU_IOVA_DEAD, ...)`（`iova.c:745`）——
离线 CPU 的 magazine 会被回收，这一点对"CPU 数很少的微 VM 上 IOVA 碎片更严重"
这个观察是必要解释。

### IO.3.4 粒度：`granule` 不一定等于 4K

`init_iova_domain()` 里 `BUG_ON((granule > PAGE_SIZE) || !is_power_of_2(granule))`
（`iova.c:45`）——
**粒度只能 ≤ PAGE_SIZE**。这与 IO.2 的"IOVA 粒度 = `pgsize_bitmap` 最低位"合并起来
得到一条硬约束：**DMA 域的 IOVA 分配器永远不可能比 CPU 页更大粒度**，即使某个
后端只支持 64K 页。这种情况下 `iommu_map()` 的对齐检查（[Q1 T.3.2](translation.md)）
会先失败，而 IOVA 分配器照旧按 4K 发号。[Q3](domains.md) 说"域的形状和 group 的边界
耦合"，这里是"域的形状和 CPU 页大小耦合"的另一处。

---

## IO.4 "32 位优先"是两个闸门，不是一个

IO.2 列了 IOVA 分配的四条约束，其中"32 位优先"这一条在代码里其实是
**两层独立机制**，混为一谈会看不懂观测结果。

### IO.4.1 闸门一：per-device 一次性闩锁

```c
/* 来源: drivers/iommu/dma-iommu.c:793-803 */
	if (dma_limit > DMA_BIT_MASK(32) && dev->iommu->pci_32bit_workaround) {
		iova = alloc_iova_fast(iovad, iova_len,
				       DMA_BIT_MASK(32) >> shift, false);
		if (iova)
			goto done;

		dev->iommu->pci_32bit_workaround = false;
		dev_notice(dev, "Using %d-bit DMA addresses\n", bits_per(dma_limit));
	}

	iova = alloc_iova_fast(iovad, iova_len, dma_limit >> shift, true);
```

三个特征：

- **作用域是 `dev->iommu`**，即每个设备一份，**不是每个域一份**。同域里另一个设备
  的 32 位优先不受影响。
- **失败即永久关闭**：`pci_32bit_workaround = false` 之后这个设备再也不会尝试
  32 位窗口。所以 `dmesg` 里那行 `Using 48-bit DMA addresses` 只出现一次，
  不代表此后所有映射都在 48 位窗口 —— 但确实代表**这个设备**放弃了 32 位。
- 第一次尝试传 `size_aligned = false`，兜底那次传 `true`
  （`alloc_iova_fast(..., false)` vs `(..., true)`）。差别见 `iova.c:174-175` 的
  `align_mask <<= fls_long(size - 1)` —— **只有走自然对齐分配时才需要按大小对齐**。
  所以 32 位窗口里的分配是"紧凑优先"，64 位窗口里是"对齐优先"。这个不对称会导致
  同一个驱动在 4G 边界附近拿到与远端明显不同的地址形状，是复现"只在特定内存
  容量下出现的设备 bug"的常见根源。

`dma_limit = min_not_zero(dma_limit, dev->bus_dma_limit)`
（`drivers/iommu/dma-iommu.c:777`）与 `force_aperture` 裁剪（`:779-780`）都发生在闸门一之前，
所以 `dma_limit ≤ 4G` 的设备**根本不进这个分支** —— 32 位掩码设备没有"降级"这回事。

### IO.4.2 闸门二：per-domain 高水位线

```c
/* 来源: drivers/iommu/iova.c:178-181 */
	spin_lock_irqsave(&iovad->iova_rbtree_lock, flags);
	if (limit_pfn <= iovad->dma_32bit_pfn &&
			size >= iovad->max32_alloc_size)
		goto iova32_full;
```

以及失败时记账：

```c
/* 来源: drivers/iommu/iova.c:196-206 */
	if (high_pfn < size || new_pfn < low_pfn) {
		if (low_pfn == iovad->start_pfn && retry_pfn < limit_pfn) {
			high_pfn = limit_pfn;
			low_pfn = retry_pfn + 1;
			curr = iova_find_limit(iovad, limit_pfn);
			curr_iova = to_iova(curr);
			goto retry;
		}
		iovad->max32_alloc_size = size;
		goto iova32_full;
	}
```

`max32_alloc_size` 是一条**高水位线**：一旦某个尺寸在 32 位窗口内失败过，此后
"≥ 该尺寸"的请求直接跳出 32 位分支，不再重走整棵树。重置只发生在
`free` 路径：`if (free->pfn_lo < iovad->dma_32bit_pfn)
iovad->max32_alloc_size = iovad->dma_32bit_pfn;`（`iova.c:90-91`）——
**只要有 32 位窗口内的地址被释放，水位线立刻复位**。

两个闸门的分工可以这样记：

| | 作用域 | 触发条件 | 复位条件 | 可见副作用 |
|---|---|---|---|---|
| `pci_32bit_workaround` | 设备 | 32 位窗口分配失败一次 | 永不（驱动生命周期内） | `dev_notice` 一行 |
| `max32_alloc_size` | 域（IOVA 分配器） | 同上 | 任何 32 位地址被 free | 无日志 |

所以：**"设备放弃了 32 位"是永久事实，"32 位窗口暂时紧张"是瞬时状态**。
调试时看到 `Using 48-bit DMA addresses` 与看到大量低地址分配成功，这两件事
完全可能同时成立。

`iova32_full` 只是 `-ENOMEM` 返回（`iova.c:219-221`），外层 `alloc_iova()`
不做二次尝试（`iova.c:263-269`）。**"退到全宽度"这个动作在 dma-iommu 层，
不在 iova 层** —— 上面 IO.4.1 里 `goto done` 之后紧接的那行 `alloc_iova_fast()`
就是全部的兜底逻辑。

---

## IO.5 保留区：谁能往里放东西，谁消费

IO.2 已经用 `iova_reserve_pci_windows()` 举了一个"窗口平移 → IOVA 空洞"
的例子。这里补全整个类型系统与消费方。

### IO.5.1 五种类型，只有一种要求真映射

```c
/* 来源: include/linux/iommu.h:259-274 */
enum iommu_resv_type {
	/* Memory regions which must be mapped 1:1 at all times */
	IOMMU_RESV_DIRECT,
	/*
	 * Memory regions which are advertised to be 1:1 but are
	 * commonly considered relaxable in some conditions,
	 * for instance in device assignment use case (USB, Graphics)
	 */
	IOMMU_RESV_DIRECT_RELAXABLE,
	/* Arbitrary "never map this or give it to a device" address ranges */
	IOMMU_RESV_RESERVED,
	/* Hardware MSI region (untranslated) */
	IOMMU_RESV_MSI,
	/* Software-managed MSI translation window */
	IOMMU_RESV_SW_MSI,
};
```

sysfs 里印出来的字符串与之一一对应，映射表在 `drivers/iommu/iommu.c:82-88`
（`IOMMU_RESV_DIRECT_RELAXABLE` → `"direct-relaxable"`）。
`cat /sys/kernel/iommu_groups/N/reserved_regions` 的第三列就是这些串。
**但这张表里 `IOMMU_RESV_MSI` 与 `IOMMU_RESV_SW_MSI` 都印成 `"msi"`**
（`drivers/iommu/iommu.c:86-87`），也就是说**光看 sysfs 输出无法区分硬件 MSI 门与
软件组装的 MSI 域**—— 排查 MSI 保留区问题时不能靠这一列下结论，得回到
`iommu_get_resv_regions()` 的实现看是哪家后端挂上来的。三家后端的 MSI 保留区差异
在 [Q6](interrupts.md) 展开。

四类消费者，行为完全不同：

| 消费者 | 看哪几种类型 | 做什么 | 源码 |
|---|---|---|---|
| dma-iommu（分配器） | `RESERVED`/`MSI`/`SW_MSI`/设备自己报的 | 建树时 `reserve_iova()`，此后永不分配 | `drivers/iommu/dma-iommu.c:524-531` |
| core 直映射建立 | 只看 `DIRECT` 与 `DIRECT_RELAXABLE` | 真的 `iommu_map()` 上，并置 `require_direct` | `drivers/iommu/iommu.c:1112-1116` |
| 默认域选择 | `DIRECT` → 强制 identity；`_RELAXABLE` 可放弃 | 见 [Q3 D.1](domains.md#d1-决策链粒度是-group不是-device) | `drivers/iommu/iommu.c:2973-2977` |
| VFIO / IOMMUFD | 全部：`RESERVED`/`MSI` 禁止用户态 map，`DIRECT*` 必须转交 | 见 [Q7](userspace.md) | `drivers/vfio/` |

`IOMMU_RESV_DIRECT` 与 `IOMMU_RESV_DIRECT_RELAXABLE` 的区别只有第四列那一条
"直通时能不能放弃"，前三个消费者一视同仁。

### IO.5.2 直映射的建立时机

```c
/* 来源: drivers/iommu/iommu.c:1091-1116（iommu_create_device_direct_mappings 节选） */
	pg_size = domain->pgsize_bitmap ? 1UL << __ffs(domain->pgsize_bitmap) : 0;
	...
	iommu_get_resv_regions(dev, &mappings);

	/* We need to consider overlapping regions for different devices */
	list_for_each_entry(entry, &mappings, list) {
		...
		if (entry->type == IOMMU_RESV_DIRECT)
			dev->iommu->require_direct = 1;

		if ((entry->type != IOMMU_RESV_DIRECT &&
		     entry->type != IOMMU_RESV_DIRECT_RELAXABLE) ||
		    !iommu_is_dma_domain(domain))
			continue;

		start = ALIGN(entry->start, pg_size);
		end   = ALIGN(entry->start + entry->length, pg_size);
```

两点：

1. `pg_size` 又取自 `1UL << __ffs(domain->pgsize_bitmap)` —— 与 [Q1 T.3.2](translation.md)
   的 `min_pagesz` 同源。**保留区边界会被向上/向下取整到最小页粒度**，所以
   sysfs 里看到 reserved region 的起止与设备树/ACPI 报的值差半页，是正常取整。
2. `require_direct = 1` 是**在这里**被置位的，而它的消费点在
   [Q3](domains.md) 的选域逻辑里。也就是说
   **"是否必须 identity"这个决定，取决于直映射建立有没有先跑过一遍** ——
   调用顺序在 `drivers/iommu/iommu.c:2980` 与 `:3015`，两处都在 `iommu_setup_default_domain()`
   内部。

`iommu_get_group_resv_regions()`（`drivers/iommu/iommu.c:839-866`）把 group 内各设备的区间做
**并集去重叠**（`iommu_insert_device_resv_regions()`，`drivers/iommu/iommu.c:825`），
sysfs 打印的就是这个合并结果。所以 `cat .../reserved_regions` 看到的区间
可能比任何单个设备申报的都大 —— 它是 group 级视图。

---

## IO.6 两个"不等号"：phys→dma 平移与 swiotlb

[IO.1](#io1-分岔点这台设备到底走不走-iommu) 的三分支里，`dev->dma_iommu` 为假时走
`dma_direct_map_page()`。这条路常被概括成"DMA 地址就是物理地址"，但它有
两个不等号。

**第一个不等号：CPU 物理地址 ≠ PCIe 总线地址。** `struct resource` 里的 `start` 是
CPU 物理地址，`window->offset` 是这段窗口在 CPU 侧与总线侧之间的平移量。x86 上它恒为
0，所以绝大多数人会以为"DMA 地址就是物理地址"。在有 non-zero offset 的平台上（典型是
ARM SoC 的 PCIe host bridge），同一个 BAR 在 CPU 看来是 `0x1800_0000`，在总线上是
`0x0000_0000`。这个 offset 在 `dma-direct` 路径上由 `phys_to_dma()` 承担：

```c
/* 来源: kernel/dma/direct.h:87-101（节选） */
	phys_addr_t phys = page_to_phys(page) + offset;
	dma_addr_t dma_addr = phys_to_dma(dev, phys);

	if (is_swiotlb_force_bounce(dev)) {
		if (is_pci_p2pdma_page(page))
			return DMA_MAPPING_ERROR;
		return swiotlb_map(dev, phys, size, dir, attrs);
	}

	if (unlikely(!dma_capable(dev, dma_addr, size, true)) ||
	    dma_kmalloc_needs_bounce(dev, size, dir)) {
		if (is_pci_p2pdma_page(page))
			return DMA_MAPPING_ERROR;
		if (is_swiotlb_active(dev))
			return swiotlb_map(dev, phys, size, dir, attrs);
```

**第二个不等号：swiotlb 还能把它整个换掉。** 于是 [Q3 D.4](domains.md#d4-identity-域在硬件上是什么)
那条"identity 域下 DMA 地址 == 物理地址"要收紧成：

> identity 域下**没有 IOMMU 翻译**，但 `dma_addr_t` 仍可能被 ① host bridge 的
> `phys_to_dma()` 平移，② swiotlb 换成 bounce buffer 地址。

`iommu.passthrough=1` 关掉的是**翻译**，不是**地址重解释**。这也是
`CONFIG_IOMMU_DEFAULT_PASSTHROUGH` 在内存加密平台被强制关掉
（`drivers/iommu/iommu.c:204-207`）的理由之一——SWIOTLB 那条路必须有人管理地址。

反过来在 **DMA 域**里，offset 被吸收进 IOVA 分配器的保留逻辑（IO.2 的减号），驱动拿到
的 `dma_addr_t` 是总线地址，`phys_to_dma()` 不参与——这就是"同一个设备在
`passthrough=0/1` 下打印出来的 DMA 地址长得完全不一样"的根源，`practice/` 实验 2
会把这个差值量出来。

`iommu_dma_map_sg()` 开头还有一条几乎无人注意的路径：`iommu_deferred_attach_enabled`
（`drivers/iommu/dma-iommu.c:92`、`:1392-1396`）。它在 `iommu_dma_init()` 里**只为 kdump
内核**打开（`drivers/iommu/dma-iommu.c:1857-1860`），配合 `dev->iommu->attach_deferred`
把 attach 推迟到第一次 `dma_map_*()`（`drivers/iommu/iommu.c:2132-2138`）。目的是崩溃
转储时避免在异常环境里 attach 失败——但顺带说明"attach 一定早于 map"并不是无条件保证。

---

## IO.7 peer/P2P：identity 域下的死结

现在兑现 [README 主流程](README.md#33-主流程里最重要的那条分支) 结尾那句承诺。
带 GPU BAR 页的 `dma_map_sg()` 在 dma-iommu 里会先问一句 p2pdma：

```c
/* 来源: drivers/iommu/dma-iommu.c:1415-1436 */
		if (is_pci_p2pdma_page(sg_page(s))) {
			map = pci_p2pdma_map_segment(&p2pdma_state, dev, s);
			switch (map) {
			case PCI_P2PDMA_MAP_BUS_ADDR:
				/*
				 * iommu_map_sg() will skip this segment as
				 * it is marked as a bus address,
				 * __finalise_sg() will copy the dma address
				 * into the output segment.
				 */
				continue;
			case PCI_P2PDMA_MAP_THRU_HOST_BRIDGE:
				/*
				 * Mapping through host bridge should be
				 * mapped with regular IOVAs, thus we
				 * do nothing here and continue below.
				 */
				break;
			default:
				ret = -EREMOTEIO;
				goto out_restore_sg;
			}
		}
```

三种结果对应三条完全不同的硬件路径，而**判据就是 ACS**：

```c
/* 来源: drivers/pci/p2pdma.c:638-658（节选） */
	if (!acs_cnt) {
		map_type = PCI_P2PDMA_MAP_BUS_ADDR;
		goto done;
	}

	if (verbose) {
		acs_list.buffer[acs_list.len-1] = 0; /* drop final semicolon */
		pci_warn(client, "ACS redirect is set between the client and provider (%s)\n",
			 pci_name(provider));
		pci_warn(client, "to disable ACS redirect for this path, add the kernel parameter: pci=disable_acs_redir=%s\n",
			 acs_list.buffer);
	}
	acs_redirects = true;

map_through_host_bridge:
	if (!cpu_supports_p2pdma() &&
	    !host_bridge_whitelist(provider, client, acs_redirects)) {
		...
		map_type = PCI_P2PDMA_MAP_NOT_SUPPORTED;
```

`acs_cnt` 来自沿路径逐跳调 `pci_bridge_has_acs_redir()`（`drivers/pci/p2pdma.c:388`），
它读的是桥的 `PCI_ACS_CTRL` 里的 `RR | CR | EC` 三位（`:399`）。

| 条件 | 结果 | 设备收到的地址 |
|---|---|---|
| 两设备路径共点且沿途**无** ACS redirect | `BUS_ADDR` | `sg_phys(s) + bus_offset`（`:1026-1027`），**故意不进 IOMMU**：`iommu_map_sg()` 用 `if (sg_dma_is_bus_address(sg)) goto next;`（`drivers/iommu/iommu.c:2647-2648`）跳过它 |
| 沿途**有** ACS redirect | `THRU_HOST_BRIDGE` | 普通 IOVA——由 IOMMU 把 IOVA 翻译成对端 BAR 的物理地址 |
| 两者都不满足（还要过 CPU/白名单这一关，见下方提示框） | `NOT_SUPPORTED` | `iommu_dma_map_sg()` 返回 `-EREMOTEIO`（`:1434`） |

这张表就是"ACS 配置改变 GDR 能力"的内核侧完整机制，它也精确地给出了
`iommu.passthrough` 的位置：

- `BUS_ADDR` 这条路**根本不需要 IOMMU 翻译**（总线地址直接写进 `sg_dma_address`），
  而且它 `goto done` **跳过了** CPU/白名单那一步。这就是"把 GPU 和 CX8 放进同一个
  switch 并关掉 ACS redirect"能成事的内核解释。但注意 identity 域下走的是
  `dma_direct_map_sg()`（`kernel/dma/direct.c:461`，p2pdma 分支从 `:471` 起），
  那条路上没有 IOVA 分配器。
- `THRU_HOST_BRIDGE` 这条路**必须有 IOVA 空间**才能把"对端 BAR"重映射成请求者可用、
  且沿途 root complex 愿意放行的地址。这件事只有 dma-iommu 能做。
  `iommu.passthrough=1` 会让 `dev->dma_iommu=false`，于是这条路直接消失。
- 规范侧的约束是 `intel-vtd.pdf` Section 3.17 (Root-Complex Peer to Peer
  Considerations)：peer 地址解码**只能**基于翻译后的 HPA 做。所以一个被翻译成 peer
  范围的请求，其后续能否被放行由 root complex 的 peer 路径决定，而不是由 IOMMU 决定
  ——这也是为什么"配了 IOMMU 映射"不等于"P2P 就能通"。

> **本节未闭合的问题（写在这里，不当结论用）**：上表第三行的门槛是
> `cpu_supports_p2pdma()`（`drivers/pci/p2pdma.c:413-422`，整个函数体在 `#ifdef
> CONFIG_X86` 里，且只认可 AMD Zen+）或 `host_bridge_whitelist()`（`:518`），后者的
> `pci_p2pdma_whitelist[]`（`:426-447`）**全部是 Intel 芯片组 ID**。也就是说通用
> p2pdma 判定链在 Grace/ARM64 上很可能直接落到 `NOT_SUPPORTED`。这**不代表** GB300 上
> GDR 不通，而是提示 NVIDIA 的 GDR 接口（`nvidia_p2p_get_pages()` 一类）可能完全绕开
> generic p2pdma，自己取 bus address 再手工 `iommu_map()`。**要证实这一点必须读
> nv-p2p 的实现，或用 ftrace 抓 `iommu_map` 的入参看地址落在哪个区间**，不能从本仓库
> 现有材料推断。留作 `practice/` 实验 4 的开放问题。

---

## IO.8 自检问题

1. `dma_map_*()` 返回的地址在什么条件下**等于** CPU 物理地址？
   （IO.1/IO.6：`dma_iommu` 为假、无 `phys_to_dma` 平移、swiotlb 未触发，三者同时成立）
2. `dmesg` 里的 `Using 48-bit DMA addresses` 出现一次，之后这个设备的 IOVA 会回到
   32 位窗口吗？同域的别的设备呢？
   （IO.4.1：不会，`pci_32bit_workaround` 是 per-device 单向闩锁；别的设备不受影响）
3. "32 位窗口紧张"与"设备放弃 32 位"是同一件事吗？各自的作用域与复位条件？
   （IO.4.2：`max32_alloc_size` 是 per-domain 高水位，任何 32 位地址被 free 即复位）
4. `reserved_regions` 里列出的区段，设备是"按恒等方式看到"还是"完全看不到"？
   （IO.2/IO.5：`reserve_iova()` 是"永不发号"，设备看不到；只有 `RESV_DIRECT*`
   才真建恒等映射）
5. 为什么 `THRU_HOST_BRIDGE` 的 P2P 在 `iommu.passthrough=1` 下直接消失？
   （IO.7：这条路必须有 IOVA 空间，而 identity 域下 `dev->dma_iommu=false`，
   没有 IOVA 分配器）
