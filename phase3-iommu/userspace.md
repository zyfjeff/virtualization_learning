# Q7：VFIO / IOMMUFD / dma-iommu 三套接口各管什么？为什么需要这么多层？

**短答**：三套接口是三种"消费者形状"，不是三层功能堆叠。内核驱动的 DMA 是
"给我这块缓冲一个设备能访问的地址"——IOVA 由内核选、映射随 `dma_map()` 自动发生；
用户态直通（VMM）是"把**我指定**的一段地址空间映射进设备能看到的地址"——IOVA 由
用户选、页是用户页、必须先 pin。两者最终都收拢到同一对 core 动词：分配域 →
attach 组 → `iommu_map()` / `iommu_unmap()`。VFIO type1 是第一代用户态接口，
IOMMUFD 是第二代：它把"地址空间"和"硬件页表"拆成两个可独立创建的对象（IOAS 与
HWPT），nested 翻译和脏页跟踪因此能挂在对象属性上。

**为什么值得问**：读代码时会撞见三套各自独立的入口——`iommu_dma_map_page()`、
`VFIO_IOMMU_MAP_DMA`、`IOMMU_IOAS_MAP`——它们看起来都在"建映射"。不知道分工，
就会把 dma-iommu 的 IOVA 分配规则（[Q4](iova.md)）误用到直通域上，或者想不通
直通场景为什么要再造一遍保留区处理（[Q6 IR.5](interrupts.md)）。这一篇把三套接口
的域类型、谁选地址、谁管页面一次对齐。

> 源码基线：`/root/code/linux-6.12.93`。VFIO 的用户态视角（container/group/fd、
> 设备直通的完整用例）已在 [第六阶段](../phase6-vfio/README.md) §2 讲过，本文只讲
> "三套接口在 IOMMU 层各做什么"，不重复设备直通的用法。

## 📖 目录

- [U.1 三个消费者，一套动词](#u1-三个消费者一套动词)
- [U.2 dma-iommu：内核驱动的自动路径](#u2-dma-iommu内核驱动的自动路径)
- [U.3 VFIO type1：一次 MAP_DMA 做的全部事情](#u3-vfio-type1一次-map_dma-做的全部事情)
- [U.4 IOMMUFD：把地址空间和页表都变成对象](#u4-iommufd把地址空间和页表都变成对象)
- [U.5 为什么是三层：分工、共存与演进](#u5-为什么是三层分工共存与演进)
- [U.6 自检问题](#u6-自检问题)

---

## U.1 三个消费者，一套动词

域类型枚举是三套接口的第一张分水岭：

```c
/* 来源: include/linux/iommu.h:196-206 */
#define IOMMU_DOMAIN_BLOCKED	(0U)
#define IOMMU_DOMAIN_IDENTITY	(__IOMMU_DOMAIN_PT)
#define IOMMU_DOMAIN_UNMANAGED	(__IOMMU_DOMAIN_PAGING)
#define IOMMU_DOMAIN_DMA	(__IOMMU_DOMAIN_PAGING |	\
				 __IOMMU_DOMAIN_DMA_API)
#define IOMMU_DOMAIN_DMA_FQ	(__IOMMU_DOMAIN_PAGING |	\
				 __IOMMU_DOMAIN_DMA_API |	\
				 __IOMMU_DOMAIN_DMA_FQ)
#define IOMMU_DOMAIN_SVA	(__IOMMU_DOMAIN_SVA)
#define IOMMU_DOMAIN_PLATFORM	(__IOMMU_DOMAIN_PLATFORM)
#define IOMMU_DOMAIN_NESTED	(__IOMMU_DOMAIN_NESTED)
```

注意 `UNMANAGED` 的定义就是 `__IOMMU_DOMAIN_PAGING`（bit 0，"Support for
iommu_map/unmap"，`:165`）——**"无人管理"和"可用 `iommu_map()` 操作"在定义上是
同一件事**。`DMA`/`DMA_FQ` 在它之上多了 `__IOMMU_DOMAIN_DMA_API`（`:166`），
这个位就是"这域是给 DMA API 用的"的标记，判定函数只有三行：

```c
/* 来源: include/linux/iommu.h:235-238 */
static inline bool iommu_is_dma_domain(struct iommu_domain *domain)
{
	return domain->type & __IOMMU_DOMAIN_DMA_API;
}
```

三个消费者与域类型的对应关系：

| 消费者 | 域类型 | 谁选 IOVA | 谁建映射 | 谁管页面生命周期 |
|---|---|---|---|---|
| 内核驱动（DMA API） | `DMA` / `DMA_FQ`（[Q3](domains.md) 默认域） | 内核（IOVA 分配器，[Q4](iova.md)） | `iommu_dma_map_page()` 自动 | 内核页，随缓冲 |
| VFIO type1 | `UNMANAGED` | **用户**（`vfio_iommu_type1_dma_map.iova`） | `VFIO_IOMMU_MAP_DMA` | 用户页，**必须 pin** |
| IOMMUFD | `UNMANAGED`（含 `NESTED`） | **用户**（`iommu_ioas_map.iova`） | `IOMMU_IOAS_MAP` / HWPT | 用户页，iopt_pages 共享 |

三条路最终汇到同一个出口：`iommu_map()` / `iommu_unmap()`（[Q5 I.1](invalidation.md)
讲过的 core 契约），再经 `iommu_domain_ops` 落到后端的 `map_pages`（[Q8](backends.md)
的主题）。**差异全在"汇拢之前"：谁决定地址、谁持有页面、失效由谁触发。**

---

## U.2 dma-iommu：内核驱动的自动路径

这条路的特殊性在于：驱动**不知道**自己在走 IOMMU。它只调 `dma_map_single()`，
DMA API 层看 `dev->dma_iommu`（README §3.2 的总分支）决定走不走本层。这个标志是
设备 probe 完默认域之后才打上的：

```c
/* 来源: drivers/iommu/iommu.c:582-583 */
	if (group->default_domain)
		iommu_setup_dma_ops(dev);
```

```c
/* 来源: drivers/iommu/dma-iommu.c:1747-1756（节选） */
void iommu_setup_dma_ops(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	...
	dev->dma_iommu = iommu_is_dma_domain(domain);
	if (dev->dma_iommu && iommu_dma_init_domain(domain, dev))
		goto out_err;
```

两个判断环环相扣：域不是 `DMA`/`DMA_FQ` 类型（比如默认域是 identity，[Q3](domains.md)）
则 `dev->dma_iommu = false`，驱动走 `dma_direct`，"DMA 地址"就是物理地址；是
DMA 域则接着跑 `iommu_dma_init_domain()`（`drivers/iommu/dma-iommu.c:671`）——
初始化 IOVA 分配器、按 `dma_range_map` 定窗口、挖保留区（[Q4 IO.2](iova.md)、
[Q6 IR.4](interrupts.md) 已展开）。

之后每一次 `dma_map_*()` 就是 [Q4](iova.md) 讲过的那条自动链：分配器发一个
IOVA → `iommu_map()` 建表 → 把 IOVA 还给驱动。驱动从头到尾没有"地址空间"的概念，
这正是这一层存在的意义：**把"设备可见地址"伪装成驱动熟悉的 `dma_addr_t`**。
lazy 模式的 flush queue 也在这一层（[Q5 I.11](invalidation.md)）。

---

## U.3 VFIO type1：一次 MAP_DMA 做的全部事情

用户态直通没有"自动"可言：VMM 知道 guest 的物理布局，要按自己的意思映射。
接口的全部语义都压在一个 ioctl 上。

### U.3.1 入口：一个 type1 文件只有五个 ioctl

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c:2991-3009（节选） */
static long vfio_iommu_type1_ioctl(void *iommu_data,
				   unsigned int cmd, unsigned long arg)
{
	...
	switch (cmd) {
	case VFIO_CHECK_EXTENSION:
		return vfio_iommu_type1_check_extension(iommu, arg);
	case VFIO_IOMMU_GET_INFO:
		return vfio_iommu_type1_get_info(iommu, arg);
	case VFIO_IOMMU_MAP_DMA:
		return vfio_iommu_type1_map_dma(iommu, arg);
	case VFIO_IOMMU_UNMAP_DMA:
		return vfio_iommu_type1_unmap_dma(iommu, arg);
	case VFIO_IOMMU_DIRTY_PAGES:
		return vfio_iommu_type1_dirty_pages(iommu, arg);
	default:
		return -ENOTTY;
	}
}
```

这个文件就是 `/dev/vfio/vfio`（container fd）背后的 IOMMU 驱动。container /
group / device 三层 fd 的组织见 [第六阶段](../phase6-vfio/README.md) §2，这里
只追 MAP_DMA 一条线。

### U.3.2 map 链：pin 在前，map 在后

`VFIO_IOMMU_MAP_DMA` 的调用链是：

```
vfio_iommu_type1_map_dma          (vfio_iommu_type1.c:2831)
  → vfio_dma_do_map               (:1548)   校验标志、查重、建 vfio_dma 记账
    → vfio_pin_map_dma            (:1448)   pin 用户页 + 逐段映射
      → vfio_iommu_map            (:1421)   对容器内每个域执行
        → iommu_map()             (:1427)   ← core 动词，与内核驱动汇合
```

两件事值得停下来：

**第一，为什么要 pin。** 用户页随时可能被换出、迁移或被 munmap；设备 DMA 是
异步的、随时可能到达。不 `pin_user_pages` 住，"映射有效"就是句空话。这是用户态
路径相对内核路径**多出来的全部负担**——内核页不会被换出，所以 dma-iommu 从不
pin。副作用是 pin 账本（`locked_vm` 限制）成了直通内存规模的第一道闸门。

**第二，映射是逐容器内所有域做的。** `vfio_iommu_map()` 里那个
`list_for_each_entry(d, &iommu->domain_list, next)`（`:1426`）说明：一个
container 可以挂多个不兼容的 group（各自一个 `iommu_domain`），同一条
MAP_DMA 要在**每个域里都建一遍同样的映射**。域是哪里来的？
`vfio_iommu_type1_attach_group()`（`:2145`）在 group 加入 container 时：

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c:2137-2142 */
static int vfio_iommu_domain_alloc(struct device *dev, void *data)
{
	struct iommu_domain **domain = data;

	*domain = iommu_paging_domain_alloc(dev);
	return 1; /* Don't iterate */
}
```

`iommu_paging_domain_alloc()`（`drivers/iommu/iommu.c:2043`）分出来的就是
`IOMMU_DOMAIN_UNMANAGED`（`:2048`）。**每个不兼容 group 一个 UNMANAGED 域，
这是 type1 时代"域"的实际形状。** 顺带：UNMANAGED 域不经过
`iommu_dma_init_domain()`，保留区要自己处理——这就是 [Q6 IR.5](interrupts.md)
msi cookie 存在的原因。

### U.3.3 unmap 链：快慢两条路与一个 512 上限

`VFIO_IOMMU_UNMAP_DMA` 走 `vfio_dma_do_unmap()`（`:1270`）→
`vfio_unmap_unpin()`（`:1027`），内部对段做快慢分流：

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c:968-982（节选） */
#define VFIO_IOMMU_TLB_SYNC_MAX		512

static size_t unmap_unpin_fast(struct vfio_domain *domain,
			       struct vfio_dma *dma, dma_addr_t *iova,
			       size_t len, phys_addr_t phys, long *unlocked,
			       struct list_head *unmapped_list,
			       int *unmapped_cnt,
			       struct iommu_iotlb_gather *iotlb_gather)
{
	size_t unmapped = 0;
	struct vfio_regions *entry = kzalloc(sizeof(*entry), GFP_KERNEL);

	if (entry) {
		unmapped = iommu_unmap_fast(domain->domain, *iova, len,
					    iotlb_gather);
```

- **快路径** `unmap_unpin_fast()`（`:970`）：用 `iommu_unmap_fast()`
  （[Q5 I.11](invalidation.md)：只拆表、不当场同步失效），把结果攒进
  `unmapped_list`，攒满 `VFIO_IOMMU_TLB_SYNC_MAX` = **512** 条才统一同步一次。
- **慢路径** `unmap_unpin_slow()`（`:1010`）：直接 `iommu_unmap()`（`:1015`），
  每段同步失效，用于攒不动的场景（分配不到记账条目等）。

攒完之后统一 `iommu_iotlb_sync()`、再 `vfio_unpin_pages_remote()` 解 pin——
**先失效、后放页**，顺序与 [Q5 I.1](invalidation.md) 里"先失效、后释放页表页"
是同一条原理的两个实例：失效没完成前，设备手里还握着旧翻译，物理页不能动。

---

## U.4 IOMMUFD：把地址空间和页表都变成对象

IOMMUFD（`/dev/iommu`）把 type1 里揉在一起的三件事——地址空间、硬件页表、
设备绑定——拆成三类**带 ID 的内核对象**，所有操作都是"创建对象 / 连接对象 /
销毁对象"。

### U.4.1 入口：一张 ioctl 表

```c
/* 来源: drivers/iommu/iommufd/main.c:378-411（节选） */
static const struct iommufd_ioctl_op iommufd_ioctl_ops[] = {
	IOCTL_OP(IOMMU_DESTROY, iommufd_destroy, struct iommu_destroy, id),
	IOCTL_OP(IOMMU_FAULT_QUEUE_ALLOC, iommufd_fault_alloc, ...),
	IOCTL_OP(IOMMU_GET_HW_INFO, iommufd_get_hw_info, ...),
	IOCTL_OP(IOMMU_HWPT_ALLOC, iommufd_hwpt_alloc, struct iommu_hwpt_alloc,
		 __reserved),
	IOCTL_OP(IOMMU_HWPT_GET_DIRTY_BITMAP, iommufd_hwpt_get_dirty_bitmap, ...),
	IOCTL_OP(IOMMU_HWPT_INVALIDATE, iommufd_hwpt_invalidate, ...),
	IOCTL_OP(IOMMU_HWPT_SET_DIRTY_TRACKING, iommufd_hwpt_set_dirty_tracking, ...),
	IOCTL_OP(IOMMU_IOAS_ALLOC, iommufd_ioas_alloc_ioctl, ...),
	IOCTL_OP(IOMMU_IOAS_ALLOW_IOVAS, iommufd_ioas_allow_iovas, ...),
	IOCTL_OP(IOMMU_IOAS_COPY, iommufd_ioas_copy, ...),
	IOCTL_OP(IOMMU_IOAS_IOVA_RANGES, iommufd_ioas_iova_ranges, ...),
	IOCTL_OP(IOMMU_IOAS_MAP, iommufd_ioas_map, struct iommu_ioas_map, iova),
	IOCTL_OP(IOMMU_IOAS_UNMAP, iommufd_ioas_unmap, ...),
	IOCTL_OP(IOMMU_OPTION, iommufd_option, ...),
	IOCTL_OP(IOMMU_VFIO_IOAS, iommufd_vfio_ioas, ...),
	...
};
```

`iommufd_fops_ioctl()`（`drivers/iommu/iommufd/main.c:413`）经
`.unlocked_ioctl`（`:453`）接到 `/dev/iommu` 的文件操作上。命令号定义在
`include/uapi/linux/iommufd.h`：`IOMMU_IOAS_ALLOC`（`:83`）、`IOMMU_IOAS_MAP`
（`:214`）、`IOMMU_HWPT_ALLOC`（`:454`）、`IOMMU_HWPT_SET_DIRTY_TRACKING`（`:589`）。

### U.4.2 IOAS 与 HWPT：一个纯软件、一个纯硬件

**IOAS**（IO Address Space）是纯软件对象：一棵"哪些 IOVA 段已经映射"的账本，
背后是 `io_pagetable`：

```c
/* 来源: drivers/iommu/iommufd/iommufd_private.h:258-263 */
struct iommufd_ioas {
	struct iommufd_object obj;
	struct io_pagetable iopt;
	struct mutex mutex;
	struct list_head hwpt_list;
};
```

`io_pagetable` 里的 `struct xarray domains`（`iommufd_private.h:46`）挂的是
这个地址空间**当前被实例化成的所有硬件域**。注意方向：type1 是"一个域一个
地址空间"；IOMMUFD 是"**一个地址空间可以 1:1 也可以 1:N 地实例化成多个域**"
（`iommufd_private.h:36-39` 注释写明这两种设计都支持）。`IOMMU_IOAS_MAP`
把用户页记进 iopt 的区间树；页在**多个域之间共享**，pin 只做一次。

**HWPT**（Hardware Page Table）是硬件对象：一个真正的 `iommu_domain`。分配
入口 `IOMMU_HWPT_ALLOC` 按 `pt_id` 指向的对象类型分岔
（`drivers/iommu/iommufd/hw_pagetable.c:296-321`）：

- `pt_id` 是 IOAS ⇒ `iommufd_hwpt_paging_alloc()`（`:104`）：分配一个
  **stage-2/普通** 页表域。无后端特定要求时就是老路的
  `iommu_paging_domain_alloc()`（`:151`）；带 flags/user_data 时走
  `ops->domain_alloc_user()`。若请求里带 `IOMMU_HWPT_ALLOC_NEST_PARENT`
  （uapi `:364`），就把这个域标记为可当父表的
  `hwpt_paging->nest_parent`（`:139`）；
- `pt_id` 是一个已标了 `nest_parent` 的 HWPT ⇒ `iommufd_hwpt_nested_alloc()`
  （`:218`）：把用户提供的**第一级页表数据**（`user_data`，Intel 的格式是
  `struct iommu_hwpt_vtd_s1`，uapi `:390-395`，数据类型枚举
  `IOMMU_HWPT_DATA_VTD_S1`，`:404`）交给 `ops->domain_alloc_user()`，产出一个
  `IOMMU_DOMAIN_NESTED` 域。拿到手还有一道硬校验：

```c
/* 来源: drivers/iommu/iommufd/hw_pagetable.c:254-258 */
	if (WARN_ON_ONCE(hwpt->domain->type != IOMMU_DOMAIN_NESTED ||
			 !hwpt->domain->ops->cache_invalidate_user)) {
		rc = -EINVAL;
		goto out_abort;
	}
```

**nested 域必须自带用户态失效接口**（`cache_invalidate_user`）——guest 改自己的
第一级页表时，VMM 要把失效请求转给宿主，这条通路是对象创建时就验过的。这正是
[Q5](invalidation.md) 里"nested 失效最贵"在用户态接口上的落点。

### U.4.3 bind 与 attach：设备进场的两步

`iommufd_device_bind()`（`drivers/iommu/iommufd/device.c:162`）是设备进入
IOMMUFD 的第一步，核心动作是 `iommu_device_claim_dma_owner()`（`:200`）——
宣告"这个设备（组）的 DMA 所有权归这个 ictx"。**bind 不分配、不挂任何域**；
它只是把设备从默认域语境里"认领"出来。

第二步才是挂页表：`iommufd_device_attach()`（`:802`）→
`iommufd_hwpt_attach_device()`（`:369`）→ `iommu_attach_group_handle()`
（`:388`）——把组挂到选定的 `iommu_domain` 上。"认领"与"挂表"分开，是
IOMMUFD 相对 type1（attach group 即分配域）最直观的结构差异：设备可以先被
认领、暂时停在 blocked 状态，等 HWPT 建好再挂上去。

脏页跟踪也按对象挂：`iommufd_hwpt_set_dirty_tracking()`
（`drivers/iommu/iommufd/hw_pagetable.c:361`）只改一个 HWPT；对应的
VFIO 侧老接口是 `VFIO_IOMMU_DIRTY_PAGES`（`vfio_iommu_type1.c:3005`）。
两者服务同一件事——活迁移脏页收集——但粒度一个是"容器所有域"、一个是"单个
硬件页表对象"。

---

## U.5 为什么是三层：分工、共存与演进

### U.5.1 分工表

| 维度 | dma-iommu | VFIO type1 | IOMMUFD |
|---|---|---|---|
| 服务对象 | 内核驱动 | 用户态（VMM） | 用户态（VMM + 其他） |
| 域类型 | `DMA` / `DMA_FQ` | `UNMANAGED` | `UNMANAGED` / `NESTED` |
| IOVA 谁选 | 内核分配器（[Q4](iova.md)） | 用户 | 用户 |
| 页面处理 | 不 pin（内核页） | `pin_user_pages` | pin，且多域共享 |
| 域的形状 | 组默认域，1 组 1 域 | 每不兼容 group 1 域 | 1 地址空间 → N 域 |
| nested 翻译 | 不适用 | 无 | 有（HWPT 父子） |
| 失效触发 | 随 `dma_unmap` / FQ 定时 | 随 UNMAP_DMA | 随 UNMAP + 用户态 `cache_invalidate_user` |
| MSI 保留区 | `iova_reserve_iommu_regions()`（[Q6 IR.4](interrupts.md)） | msi cookie（[Q6 IR.5](interrupts.md)） | 同上（`drivers/iommu/iommufd/device.c:317`） |

### U.5.2 为什么不能只有一套

- **dma-iommu 不能服务用户态**：它的核心假设是"页是内核的、地址由我选"。
  VMM 两样都不满足。
- **type1 做不了 nested 与细粒度硬件控制**：它的域在 attach 时就定死，
  没有"先建父表、再拿用户数据建子表"的对象序列；脏页、故障队列也无处安放。
- **IOMMUFD 不取代 dma-iommu**：内核驱动要的是零认知负担的 `dma_map_*()`，
  不是 ioctl。

所以三层不是历史包袱的堆叠，而是三种消费者形状的投影。真正被取代的只有
一代：**type1 的分配接口**。如今 VFIO 和 IOMMUFD 都用
`iommu_paging_domain_alloc()`（`drivers/iommu/iommu.c:2043`，按**设备**找
ops）；旧的按总线分配的 `iommu_domain_alloc(bus)`（`:2020`）还在，但注释
（`:2016-2019`）已写明 "The iommu ops in bus has been retired. Do not use"，
两家用户态代码都不再调它。

### U.5.3 两代接口的共存桥

IOMMUFD 生来就要接住存量 VFIO 生态，桥在两侧各有一段：

- **VFIO 侧**：`drivers/vfio/iommufd.c`。设备驱动（如 vfio-pci）走
  `vfio_iommufd_physical_bind()`（`:117`）→ `iommufd_device_bind()`（`:122`）
  认领设备；`vfio_iommufd_physical_attach_ioas()`（`:143`）→
  `iommufd_device_attach()`（`:155`）挂地址空间。
- **IOMMUFD 侧**：ioctl 表里的 `IOMMU_VFIO_IOAS`（`drivers/iommu/iommufd/main.c:406-407`）让
  老式 VFIO container 用法映射到 IOAS 对象。

对用户态可见的行为差异因此收敛在对象语义上：同样的"映射"，走
`VFIO_IOMMU_MAP_DMA` 还是 `IOMMU_IOAS_MAP`，底下都是同一棵 iopt 区间树 +
同一对 `iommu_map()`/`iommu_unmap()`。

---

## U.6 自检问题

1. `IOMMU_DOMAIN_UNMANAGED` 和"可以用 `iommu_map()`"为什么是同一件事？
   （U.1：`UNMANAGED` 的定义就是 `__IOMMU_DOMAIN_PAGING` 位，
   `include/linux/iommu.h:198`）
2. 同一个 container 挂了两个不兼容的 group，一条 `VFIO_IOMMU_MAP_DMA`
   会建几份映射？（U.3.2：每个域一份；`vfio_iommu_map()` 遍历
   `domain_list`，`vfio_iommu_type1.c:1426`）
3. VFIO 快路径攒够多少条未同步 unmap 就强制同步？这个上限叫什么？
   （U.3.3：512，`VFIO_IOMMU_TLB_SYNC_MAX`，`vfio_iommu_type1.c:968`）
4. `iommufd_device_bind()` 成功后，设备已经在某个域的页表里了吗？
   （U.4.3：没有；bind 只 `claim_dma_owner`，挂域要等 `iommufd_device_attach`）
5. 为什么 IOMMUFD 创建 nested HWPT 时就要检查 `cache_invalidate_user`？
   （U.4.2：guest 改第一级页表后失效要经 VMM 转给宿主，没有这条通路
   nested 域不可维护；`hw_pagetable.c:254-258`）
6. 旧的 `iommu_domain_alloc(bus)` 与新的 `iommu_paging_domain_alloc(dev)`
   分出来的域类型一样吗？差在哪？（U.5.2：都是 `UNMANAGED`
   ——`drivers/iommu/iommu.c:2029` 与 `:2048`；差别在找 ops 的方式：按总线遍历
   已废弃，按设备直取）
