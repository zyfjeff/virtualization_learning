# Phase 3：源码精读注释 - IOMMU 子系统

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。

---

## 1. 核心数据结构

### Q: IOMMU 子系统有几个关键结构体？

| 结构体 | 位置 | 作用 |
|--------|------|------|
| `struct iommu_group` | `drivers/iommu/iommu.c:47` | 设备组（共享地址空间的设备集合） |
| `struct iommu_domain` | `include/linux/iommu.h:208` | 地址翻译域（页表 + 操作集） |
| `struct iommu_ops` | `include/linux/iommu.h:559` | IOMMU 驱动回调表（per-IOMMU） |
| `struct iommu_domain_ops` | `include/linux/iommu.h:642` | 域级回调表（per-domain） |

**注意**：`struct iommu_group` 定义在 `.c` 文件里，**不在头文件中**！外部只能通过
`iommu_group` 指针操作，不能直接访问字段。

### Q: `struct iommu_group` 的关键字段？

**文件**: `drivers/iommu/iommu.c:47`

```c
struct iommu_group {
    struct kobject kobj;
    struct kobject *devices_kobj;
    struct list_head devices;                /* ★ 组内设备链表 */
    struct xarray pasid_array;               /* PASID → domain 映射 */
    struct mutex mutex;                      /* ★ 保护组级操作 */
    void *iommu_data;                        /* 后端私有数据 */
    char *name;
    int id;                                  /* sysfs 里看到的组号 */
    struct iommu_domain *default_domain;     /* ★ 默认域 */
    struct iommu_domain *blocking_domain;    /* 阻塞域 */
    struct iommu_domain *domain;             /* ★ 当前活跃域 */
    unsigned int owner_cnt;                  /* VFIO 占用计数 */
    void *owner;                             /* VFIO 持有者 */
};
```

**`default_domain` vs `domain`**：`default_domain` 是系统启动时自动选的域（DMA 或
IDENTITY）；`domain` 是当前实际挂载的域。VFIO 接管后 `domain` 会被替换成用户态
创建的域。

### Q: `struct iommu_domain` 的关键字段？

**文件**: `include/linux/iommu.h:208`

```c
struct iommu_domain {
    unsigned type;                            /* ★ IOMMU_DOMAIN_DMA 等 */
    const struct iommu_domain_ops *ops;       /* ★ 域操作回调 */
    const struct iommu_ops *owner;            /* 哪个 IOMMU 驱动创建的 */
    unsigned long pgsize_bitmap;              /* ★ 支持的页大小位图 */
    struct iommu_domain_geometry geometry;    /* 地址范围约束 */
    struct iommu_dma_cookie *iova_cookie;     /* IOVA 分配器 */
};
```

### Q: `iommu_domain_ops` 里有哪些关键回调？

**文件**: `include/linux/iommu.h:642`

```c
struct iommu_domain_ops {
    int (*attach_dev)(struct iommu_domain *domain, struct device *dev);
    int (*map_pages)(struct iommu_domain *domain, unsigned long iova,
                     phys_addr_t paddr, size_t pgsize, size_t pgcount,
                     int prot, gfp_t gfp, size_t *mapped);
    size_t (*unmap_pages)(struct iommu_domain *domain, unsigned long iova,
                          size_t pgsize, size_t pgcount,
                          struct iommu_iotlb_gather *iotlb_gather);
    void (*flush_iotlb_all)(struct iommu_domain *domain);
    int (*iotlb_sync_map)(struct iommu_domain *domain, unsigned long iova,
                          size_t size);           /* ★ map 后的同步钩子 */
    void (*iotlb_sync)(struct iommu_domain *domain,
                       struct iommu_iotlb_gather *gather); /* ★ unmap 后同步 */
    phys_addr_t (*iova_to_phys)(struct iommu_domain *domain, dma_addr_t iova);
    void (*free)(struct iommu_domain *domain);
};
```

**为什么分两层 ops？** `iommu_ops` 是 IOMMU 硬件驱动级的（probe 设备、分域、选组）；
`iommu_domain_ops` 是域级的（attach/map/unmap/flush）。一个 IOMMU 驱动可以创建
多个域，每个域的 ops 相同但实例数据不同。

### Q: `IOMMU_DOMAIN_*` 类型有哪些？

**文件**: `include/linux/iommu.h:196-206`

| 类型 | 值 | 含义 |
|------|----|------|
| `IOMMU_DOMAIN_BLOCKED` | `0` | 所有 DMA 被阻断 |
| `IOMMU_DOMAIN_IDENTITY` | `__IOMMU_DOMAIN_PT` | DMA 地址 = 物理地址 |
| `IOMMU_DOMAIN_UNMANAGED` | `__IOMMU_DOMAIN_PAGING` | 用户态管理页表（VFIO） |
| `IOMMU_DOMAIN_DMA` | `PAGING \| DMA_API` | 内核 DMA API 用 |
| `IOMMU_DOMAIN_DMA_FQ` | `DMA \| DMA_FQ` | DMA + 批量失效队列 |
| `IOMMU_DOMAIN_SVA` | `__IOMMU_DOMAIN_SVA` | 共享进程地址空间 |
| `IOMMU_DOMAIN_NESTED` | `__IOMMU_DOMAIN_NESTED` | 嵌套翻译（IOMMUFD） |

---

## 2. 设备 probe：`iommu_probe_device()`

### Q: 设备进入 IOMMU 子系统时发生了什么？

**文件**: `drivers/iommu/iommu.c:600` — `iommu_probe_device()`
**文件**: `drivers/iommu/iommu.c:513` — `__iommu_probe_device()`

```c
static int __iommu_probe_device(struct device *dev, struct list_head *group_list)
{
    /* ★ 幂等检查：已在组里则跳过 */
    if (dev->iommu_group)
        return 0;

    /* 1. 初始化设备 IOMMU 档案 */
    ret = iommu_init_device(dev, ops);       /* iommu.c:402 */

    /* 2. 分配到组（ops->device_group 决定分组） */
    group = dev->iommu_group;
    gdev = iommu_group_alloc_device(group, dev);

    /* ★ 3. 先入组，再选域！顺序不能换 */
    list_add_tail(&gdev->list, &group->devices);

    if (group->default_domain)
        iommu_create_device_direct_mappings(group->default_domain, dev);
    if (group->domain) {
        /* 组已有域 → 直接挂上 */
        ret = __iommu_device_set_domain(group, dev, group->domain, 0);
    } else if (!group->default_domain && !group_list) {
        /* ★ 组首次有成员 → 选默认域 */
        ret = iommu_setup_default_domain(group, 0);
    }
    ...
    /* 4. 安装 DMA ops */
    if (group->default_domain)
        iommu_setup_dma_ops(dev);
}
```

### Q: 为什么 `list_add_tail` 必须在 `iommu_setup_default_domain` 之前？

选域函数 `iommu_get_default_domain_type()`（`iommu.c:1726`）遍历 `group->devices`
链表的每个成员投票。如果设备没入链表，投票就不包含它的意见 → 可能选错域类型。

注释也明确写了：`"The gdev must be in the list before calling iommu_setup_default_domain()"`

---

## 3. 默认域选择：投票 + 四级回落

### Q: 默认域的类型怎么确定？

**文件**: `drivers/iommu/iommu.c:1726` — `iommu_get_default_domain_type()`

```c
static int iommu_get_default_domain_type(struct iommu_group *group,
                                         int target_type)
{
    int driver_type = 0;

    /* ★ 第一步：逐成员投票 */
    for_each_group_device(group, gdev) {
        driver_type = iommu_get_def_domain_type(group, gdev->dev, driver_type);
        /* 记录 untrusted 设备（雷电/USB4 外接） */
        if (dev_is_pci(gdev->dev) && to_pci_dev(gdev->dev)->untrusted)
            untrusted = gdev->dev;
    }

    /* ★ 第二步：安全策略覆盖 */

    /* 无 dma-iommu → 强制 IDENTITY */
    if (!IS_ENABLED(CONFIG_IOMMU_DMA)) {
        if (!driver_type) driver_type = IOMMU_DOMAIN_IDENTITY;
    }

    /* untrusted 设备 → 强制 DMA（安全隔离） */
    if (untrusted) {
        if (driver_type && driver_type != IOMMU_DOMAIN_DMA)
            return -1;     /* 冲突 → 拒绝 probe */
        driver_type = IOMMU_DOMAIN_DMA;
    }

    /* 有 target_type → 必须与投票一致 */
    if (target_type) {
        if (driver_type && target_type != driver_type)
            return -1;
        return target_type;
    }
    return driver_type;    /* 0 = 用全局默认 */
}
```

### Q: 域分配时的四级回落顺序？

**文件**: `drivers/iommu/iommu.c:1605` — `iommu_group_alloc_default_domain()`

```c
iommu_group_alloc_default_domain(struct iommu_group *group, int req_type)
{
    /* Level 1: 后端遗留的静态默认域（legacy 驱动） */
    if (ops->default_domain)
        return ops->default_domain;

    /* Level 2: 请求的类型 */
    if (req_type)
        return __iommu_group_alloc_default_domain(group, req_type);

    /* Level 3: 全局默认（iommu.passthrough 改的） */
    dom = __iommu_group_alloc_default_domain(group, iommu_def_domain_type);
    if (!IS_ERR(dom))
        return dom;

    /* Level 4: 回落到 DMA + 告警 */
    dom = __iommu_group_alloc_default_domain(group, IOMMU_DOMAIN_DMA);
    pr_warn("Failed to allocate default IOMMU domain of type %u ... "
            "- Falling back to IOMMU_DOMAIN_DMA", ...);
    return dom;
}
```

看到开机日志里那条 "Falling back to IOMMU_DOMAIN_DMA"，说明 `iommu_def_domain_type`
请求的类型在这里分配失败了。

### Q: `iommu_setup_default_domain()` 里建域后先做什么？

**文件**: `drivers/iommu/iommu.c:2950`

```c
static int iommu_setup_default_domain(struct iommu_group *group, int target_type)
{
    req_type = iommu_get_default_domain_type(group, target_type);
    dom = iommu_group_alloc_default_domain(group, req_type);

    /* ★ 先建直通映射（固件保留区），再挂域！ */
    for_each_group_device(group, gdev)
        iommu_create_device_direct_mappings(dom, gdev->dev);

    /* 提前设 default_domain（__iommu_device_set_domain 要用） */
    group->default_domain = dom;

    if (!group->domain) {
        /* 首次 attach → 不允许失败 */
        ret = __iommu_group_set_domain_internal(
            group, dom, IOMMU_SET_DOMAIN_MUST_SUCCEED);
    } else {
        ret = __iommu_group_set_domain(group, dom);
    }
}
```

**为什么 `IOMMU_RESV_DIRECT` 必须在 attach 之前建？** 这些区域是固件还在用的
（比如 ACPI 表），设备挂到新域的瞬间如果没映射，固件的 DMA 就断了。

---

## 4. attach：把域装到组上

### Q: `__iommu_group_set_domain_internal()` 做什么？

**文件**: `drivers/iommu/iommu.c:2305`

```c
static int __iommu_group_set_domain_internal(struct iommu_group *group,
                                             struct iommu_domain *new_domain,
                                             unsigned int flags)
{
    /* ★ 逐设备调 attach_dev() */
    for_each_group_device(group, gdev) {
        ret = __iommu_device_set_domain(group, gdev->dev, new_domain, flags);
        if (ret) {
            if (flags & IOMMU_SET_DOMAIN_MUST_SUCCEED)
                continue;      /* 首次 attach → 继续尝试其他设备 */
            goto err_revert;   /* 后续 attach → 回滚 */
        }
    }
    group->domain = new_domain;
    return result;
}
```

**attach 在硬件上写什么？** 取决于域类型：
- DMA 域：写页表根指针（VT-d 的 context entry）
- IDENTITY 域：每家不同的恒等表达（bypass）
- BLOCKED 域：阻断所有 DMA

最终落到 `ops->attach_dev()`（Intel: `intel_iommu_attach_device()` 在
`drivers/iommu/intel/iommu.c:3676`）。

---

## 5. map：`dma_map_page()` → `iommu_map()`

### Q: 内核驱动发起 DMA 映射时的完整路径？

```
驱动: dma_map_page(dev, page, offset, size, dir)
  │
  ▼
DMA API 分发（README §3.2）
  │
  ▼
iommu_dma_map_page()          [dma-iommu.c:1163]
  │
  ├── 1. IOVA 分配（iovad）
  ├── 2. 可能需要 SWIOTLB bounce buffer
  │
  ▼
iommu_map()                   [iommu.c:2510]
  │
  ├── __iommu_map()           [iommu.c:2447]
  │     └── ops->map_pages()  ← Intel: intel_iommu_map_pages()
  │                               (iommu.c:3726)
  │
  └── ops->iotlb_sync_map()   ← caching-mode 硬件需要
```

### Q: `iommu_map()` 的事务性保证？

**文件**: `drivers/iommu/iommu.c:2510`

```c
int iommu_map(struct iommu_domain *domain, unsigned long iova,
              phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
    ret = __iommu_map(domain, iova, paddr, size, prot, gfp);
    if (ret == 0 && ops->iotlb_sync_map) {
        ret = ops->iotlb_sync_map(domain, iova, size);
        if (ret)
            goto out_err;
    }
    return ret;

out_err:
    /* ★ 失败即回滚：撤销已建的映射 */
    iommu_unmap(domain, iova, size);
    return ret;
}
```

**事务性由 core 保证**，后端不用管。`__iommu_map()` 按 `iommu_pgsize()` 逐段循环
（`iommu.c:2486` 调用 `ops->map_pages()`），中途失败则 `iommu_unmap()` 清掉已建部分。

### Q: `iotlb_sync_map` 是什么？

caching-mode 的 IOMMU（Intel VT-d 典型）连 map 后都需要失效 IOTLB —— 因为硬件可能
缓存了 "该地址无翻译" 的负缓存。`ops->iotlb_sync_map()` 就是给这种硬件的钩子。

Intel 的实现是 `intel_iommu_iotlb_sync_map()`（`drivers/iommu/intel/iommu.c`
`default_domain_ops` 里 `:4669`）。

---

## 6. unmap 与失效同步

### Q: `iommu_unmap()` 怎么保证失效完成？

**文件**: `drivers/iommu/iommu.c:2594`

```c
size_t iommu_unmap(struct iommu_domain *domain,
                   unsigned long iova, size_t size)
{
    struct iommu_iotlb_gather iotlb_gather;

    iommu_iotlb_gather_init(&iotlb_gather);

    /* ★ 1. 逐段拆页表，累积失效范围 */
    ret = __iommu_unmap(domain, iova, size, &iotlb_gather);

    /* ★ 2. 同步等到硬件确认失效完成 */
    iommu_iotlb_sync(domain, &iotlb_gather);

    return ret;
}
```

**`iommu_iotlb_gather` 是什么？** 批量失效的收集器：

**文件**: `include/linux/iommu.h:345`

```c
struct iommu_iotlb_gather {
    unsigned long    start;      /* 失效范围起点 */
    unsigned long    end;        /* 失效范围终点 */
    size_t           pgsize;     /* 页大小 */
    struct list_head freelist;   /* 释放的页表页链表 */
    bool             queued;     /* 是否有待失效的范围 */
};
```

`__iommu_unmap()`（`iommu.c:2540`）逐段调 `ops->unmap_pages()`，每段把范围
累积进 `gather`。最后 `iommu_iotlb_sync()` 一次性失效 → 减少 IOTLB flush 次数。

**返回时失效一定完成了吗？** 是的。`iommu_iotlb_sync()` 等到所有缓存（IOTLB +
paging-structure cache + Device-TLB）确认失效后才返回。这是 unmap 的**同步语义**。

---

## 7. 运行期换域：sysfs `type`

### Q: 怎么在运行期改组的默认域类型？

**文件**: `drivers/iommu/iommu.c:3047` — `iommu_group_store_type()`

```bash
# 用法（需 root）
echo identity > /sys/kernel/iommu_groups/<id>/type
echo DMA > /sys/kernel/iommu_groups/<id>/type
echo DMA-FQ > /sys/kernel/iommu_groups/<id>/type
echo auto > /sys/kernel/iommu_groups/<id>/type
```

```c
static ssize_t iommu_group_store_type(struct iommu_group *group,
                                      const char *buf, size_t count)
{
    /* 解析类型字符串 */
    if (sysfs_streq(buf, "identity"))       req_type = IOMMU_DOMAIN_IDENTITY;
    else if (sysfs_streq(buf, "DMA"))       req_type = IOMMU_DOMAIN_DMA;
    else if (sysfs_streq(buf, "DMA-FQ"))    req_type = IOMMU_DOMAIN_DMA_FQ;
    else if (sysfs_streq(buf, "auto"))      req_type = 0;

    /* ★ 唯一在线切换：DMA → DMA-FQ（不用解绑驱动） */
    if (req_type == IOMMU_DOMAIN_DMA_FQ &&
        group->default_domain->type == IOMMU_DOMAIN_DMA) {
        ret = iommu_dma_init_fq(group->default_domain);  /* dma-iommu.c:336 */
        group->default_domain->type = IOMMU_DOMAIN_DMA_FQ;
        goto out_unlock;
    }

    /* 其余方向：必须解绑所有驱动 */
    if (list_empty(&group->devices) || group->owner_cnt) {
        ret = -EPERM;
        goto out_unlock;
    }

    ret = iommu_setup_default_domain(group, req_type);
}
```

**`DMA → DMA-FQ` 是唯一在线切换** —— 同一个域对象上长出 flush queue，不需要拆域
重建。其余方向都要先解绑组内所有驱动。

### 为什么 DMA → DMA-FQ 是唯一可以在线切换的？

**设计动机**：

```
DMA 域        → 立即失效（unmap 后立即 flush IOTLB）
DMA-FQ 域     → 批量失效（unmap 后放入 flush queue，延迟 flush）

两者的区别只在失效策略，域对象本身完全相同！
所以可以在线切换：只需初始化 flush queue，不需要重建域。

其他方向为什么不行？
  DMA → IDENTITY: 需要删除所有页表（DMA 有页表，IDENTITY 没有）
  IDENTITY → DMA: 需要创建页表（IDENTITY 没有页表）
  DMA → BLOCKED:  需要切换到"全部阻断"的域
  
这些都需要销毁旧域、创建新域，必须解绑所有驱动才能安全操作。
```

**实际场景**：
- 系统启动时默认用 DMA（立即失效，安全性高）
- 运行中发现性能瓶颈（频繁 unmap 导致 IOTLB flush 过多）
- 在线切换到 DMA-FQ（批量失效，性能更好）
- 不需要重启 VM 或解绑驱动

---

## 8. Intel VT-d 后端实现

### Q: `intel_iommu_ops` 长什么样？

**文件**: `drivers/iommu/intel/iommu.c:4644`

```c
const struct iommu_ops intel_iommu_ops = {
    .blocked_domain      = &blocking_domain,
    .identity_domain     = &identity_domain,
    .capable             = intel_iommu_capable,
    .domain_alloc        = intel_iommu_domain_alloc,
    .domain_alloc_user   = intel_iommu_domain_alloc_user,
    .probe_device        = intel_iommu_probe_device,
    .release_device      = intel_iommu_release_device,
    .device_group        = intel_iommu_device_group,
    .def_domain_type     = device_def_domain_type,
    .pgsize_bitmap       = SZ_4K,              /* ★ 只支持 4KB */
    .default_domain_ops  = &(const struct iommu_domain_ops) {
        .attach_dev      = intel_iommu_attach_device,   /* :3676 */
        .map_pages       = intel_iommu_map_pages,       /* :3726 */
        .unmap_pages     = intel_iommu_unmap_pages,
        .iotlb_sync_map  = intel_iommu_iotlb_sync_map,
        .flush_iotlb_all = intel_flush_iotlb_all,
        .iotlb_sync      = intel_iommu_tlb_sync,
        .iova_to_phys    = intel_iommu_iova_to_phys,
        .free            = intel_iommu_domain_free,
    },
};
```

**`pgsize_bitmap = SZ_4K`**：VT-d 后端只支持 4KB 页。大页（2M/1G）由 `domain_alloc`
时根据硬件能力决定，但对外协商的 `pgsize_bitmap` 始终是 4K。

### Q: Intel attach 在硬件上写什么？

**文件**: `drivers/iommu/intel/iommu.c:3676` — `intel_iommu_attach_device()`

attach 把域的页表根指针写进 VT-d 的 **Context Entry**。Context Entry 是 per-BDF 的
（每个设备 function 一个），包含：
- Domain ID（用于 IOTLB tag）
- 地址翻译模式（legacy / scalable / PT）
- 页表根地址（DAG 指针）

### 为什么 Context Entry 要区分不同的翻译模式？

**设计动机**：

```
VT-d 支持三种翻译模式（Translation Type）：

1. CONTEXT_TT_MULTI_LEVEL (legacy)
   - 传统的两级翻译：First-stage + Second-stage
   - 适用于大多数设备
   - 页表结构：root → context → page table

2. CONTEXT_TT_DEV_IOTLB
   - 启用 Device-TLB（ATS）
   - 设备可以缓存翻译结果
   - 需要在 Context Entry 中设置特殊标志

3. CONTEXT_TT_PASS_THROUGH (PT)
   - 直通模式（identity）
   - 不进行地址翻译
   - 用于 iommu.passthrough=1 的场景

为什么要区分？
  - 不同设备的能力不同（有的支持 ATS，有的不支持）
  - 不同的使用场景（DMA 映射 vs identity 直通）
  - 性能优化（Device-TLB 可以减少 IOTLB miss）

KVM 如何选择？
  - 根据域类型（DMA/IDENTITY）和设备能力（ATS 支持）
  - 在 intel_iommu_attach_device() 中决定
```

**实际影响**：
- 翻译模式影响 IOTLB 的 tag 方式（Domain ID + PASID）
- 影响失效的粒度（device-level vs page-level）
- 影响性能（Device-TLB 命中率高时性能更好）

---

## 9. trace 事件

### Q: 有哪些 IOMMU trace 事件可用于调试？

**文件**: `include/trace/events/iommu.h`

| 事件 | 行 | 参数 | 用途 |
|------|-----|------|------|
| `map` | 79 | `(iova, paddr, size)` | 观察 IOVA→PADDR 映射 |
| `unmap` | 103 | `(iova, size, unmapped_size)` | 观察解映射 |
| `io_page_fault` | 153 | `(dev, iova, flags)` | 观察设备缺页 |
| `attach_device_to_domain` | 72 | `(dev)` | 观察 attach 事件 |
| `add_device_to_group` | 39 | `(group_id, dev)` | 观察设备分组 |

**注意**：`trace_map` 同时打出 **iova 和 paddr** —— 这是证明"总线地址 ≠ 物理地址"
的关键证据。`trace_unmap` 的 `unmapped_size` 与请求 `size` 不等时说明中途失败。

### Q: `report_iommu_fault()` 和 `iommu_report_device_fault()` 的区别？

**文件**: `drivers/iommu/iommu.c:2700` — `report_iommu_fault()`
**文件**: `drivers/iommu/io-pgfault.c:214` — `iommu_report_device_fault()`

| 函数 | 用途 | trace 点 |
|------|------|---------|
| `report_iommu_fault()` | 域内 fault handler 回调 + `trace_io_page_fault` | `iommu:io_page_fault` |
| `iommu_report_device_fault()` | IOPF（可恢复缺页）框架入口 | — |

`report_iommu_fault()` 是**不可恢复**缺页的最终报告点（设备访问了没映射的地址）；
`iommu_report_device_fault()` 是**可恢复**缺页（PRI/PRQ）的入口 —— 驱动可以响应页面
请求。

---

## 10. 完整调用链

```
设备出现在总线
  │
  ▼  A.1
iommu_probe_device()             [iommu.c:600]
  └→ __iommu_probe_device()      [iommu.c:513]
     ├→ iommu_init_device()      [iommu.c:402]
     │   └→ ops->probe_device()  ← Intel: intel_iommu_probe_device()
     ├→ ops->device_group()      ← Intel: intel_iommu_device_group()
     ├→ list_add_tail()          ← ★ 先入组
     │
     ▼  A.2（组首次有成员时）
  iommu_setup_default_domain()   [iommu.c:2950]
     ├→ 类型投票 (1726)：驱动意见 → 冲突时 IDENTITY 胜 → untrusted 强制 DMA
     ├→ 分配回落 (1605)：ops 静态 → 要求类型 → iommu_def_domain_type → DMA + pr_warn
     ├→ RESV_DIRECT 预映射       ← ★ 固件保留区先建
     │
     ▼  A.3
  __iommu_group_set_domain_internal() [iommu.c:2305]
     └→ ops->attach_dev()        ← Intel: intel_iommu_attach_device() (:3676)
                                   写 VT-d Context Entry
     │
     ▼  A.1 收尾
  iommu_setup_dma_ops()          ← dev->dma_iommu 挂上
  │
  │   驱动运行期：
  ▼  A.4
  dma_map_page() → iommu_dma_map_page() [dma-iommu.c:1163]
     └→ iommu_map()              [iommu.c:2510]
        ├→ __iommu_map()         [iommu.c:2447]
        │   └→ ops->map_pages()  ← Intel: intel_iommu_map_pages() (:3726)
        │       写 VT-d DMA PTE (try_cmpxchg64)
        ├→ trace_map()           ← include/trace/events/iommu.h:79
        └→ ops->iotlb_sync_map() ← caching-mode 硬件需要
  │
  ▼  A.5
  dma_unmap_page() → iommu_unmap() [iommu.c:2594]
     ├→ __iommu_unmap()          [iommu.c:2540]
     │   └→ ops->unmap_pages() + gather 累积
     ├→ iommu_iotlb_sync()       ← ★ 同步等硬件确认失效
     └→ trace_unmap()            ← include/trace/events/iommu.h:103
  │
  │   fault 方向：
  ▼
  设备访问未映射地址
     └→ report_iommu_fault()     [iommu.c:2700]
        └→ trace_io_page_fault() ← include/trace/events/iommu.h:153
  可恢复缺页 (PRI/PRQ)：
     └→ iommu_report_device_fault() [io-pgfault.c:214]
```

---

## 11. 关键数据结构关系

```
struct iommu_ops (per-IOMMU 硬件)
  ├── probe_device / device_group / domain_alloc
  └── default_domain_ops → struct iommu_domain_ops
        ├── attach_dev / map_pages / unmap_pages
        └── iotlb_sync / iotlb_sync_map / flush_iotlb_all

struct iommu_group (per-隔离组)
  ├── devices[]          ← 组内设备链表
  ├── default_domain     ← 系统启动时选的
  ├── domain             ← 当前活跃的（VFIO 可替换）
  └── mutex              ← 保护组级操作

struct iommu_domain (per-翻译域)
  ├── type               ← IOMMU_DOMAIN_DMA / IDENTITY / ...
  ├── ops                ← iommu_domain_ops 回调
  ├── pgsize_bitmap      ← 支持的页大小
  ├── iova_cookie        ← IOVA 分配器
  └── owner              ← 创建它的 iommu_ops

Intel VT-d:
  intel_iommu_ops
    ├── .default_domain_ops.attach_dev = intel_iommu_attach_device()
    │     └→ 写 VT-d Context Entry（domain ID + 页表根指针）
    ├── .default_domain_ops.map_pages = intel_iommu_map_pages()
    │     └→ 写 DMA PTE (try_cmpxchg64)
    └── .pgsize_bitmap = SZ_4K
```
