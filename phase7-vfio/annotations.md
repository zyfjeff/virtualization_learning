# Phase 6：源码精读注释 - VFIO 设备直通

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。

---

## 1. VFIO 分层架构

### Q: VFIO 的分层是怎么组织的？

```
用户态 (QEMU)
  │
  ├─ /dev/vfio/vfio         ← 容器 API（版本检查、扩展查询）
  ├─ /dev/vfio/$GROUP_ID    ← 组设备访问（旧 API）
  └─ /dev/vfio/devices/vfio$N ← 设备 fd（新 cdev API）
  │
  │ ioctl / mmap / read / write
  │
VFIO 核心框架 (vfio_main.c)
  │
  ├─ vfio_device            ← 设备抽象
  ├─ vfio_group             ← IOMMU 组
  ├─ vfio_container         ← DMA 容器（旧）
  └─ iommufd                ← 新 IOMMU fd 框架
  │
VFIO 设备驱动 (vfio_pci_core.c)
  │
  ├─ PCI 配置空间访问
  ├─ BAR mmap
  ├─ 中断管理（INTx/MSI/MSI-X）
  └─ 设备复位
  │
IOMMU 驱动 (vfio_iommu_type1.c)
  │
  ├─ DMA 映射/解映射
  ├─ 页面固定
  └─ 脏页跟踪
```

---

## 2. vfio_device：核心设备结构

### Q: `struct vfio_device` 有哪些关键字段？

**文件**: `include/linux/vfio.h:37`

```c
struct vfio_device {
    struct device *dev;                      /* ★ 底层 Linux 设备（pci_dev 等） */
    const struct vfio_device_ops *ops;       /* ★ 设备操作回调 */
    const struct vfio_migration_ops *mig_ops;  /* 迁移操作 */
    const struct vfio_log_ops *log_ops;       /* 脏页日志 */

#if IS_ENABLED(CONFIG_VFIO_GROUP)
    struct vfio_group *group;                /* 所属 IOMMU 组 */
    struct list_head group_next;
    struct list_head iommu_entry;
#endif
    struct vfio_device_set *dev_set;         /* 设备集（迁移协调用） */
    struct list_head dev_set_list;
    unsigned int migration_flags;
    struct kvm *kvm;                         /* ★ 关联的 KVM 实例 */

    /* --- 以下为内部字段，驱动不应直接使用 --- */
    unsigned int index;
    struct device device;                    /* 内嵌设备（kref 管理生命周期） */
    refcount_t refcount;
    unsigned int open_count;                 /* 用户态打开次数 */
    struct completion comp;
    struct iommufd_access *iommufd_access;
    void (*put_kvm)(struct kvm *kvm);
    struct inode *inode;
    struct iommufd_device *iommufd_device;   /* iommufd 设备绑定 */
};
```

### Q: `vfio_device` 的 `kvm` 字段怎么来的？

```c
/* 来源: drivers/vfio/group.c (通过 VFIO_GROUP_SET_CONTAINER 等路径) */
/* 在 irq_bypass 配对时，consumer 和 producer 通过 token 匹配 */
/* token 是同一个 eventfd 上下文指针 */
```

`vfio_device->kvm` 在 VFIO 设备被关联到 KVM 时设置。它主要用于：
1. Posted Interrupts — KVM 需要知道设备的 PI Descriptor
2. 迁移 — KVM 和设备驱动协调脏页

### Q: `vfio_device_ops` 回调表长什么样？

**文件**: `include/linux/vfio.h:109`

```c
struct vfio_device_ops {
    char *name;
    int  (*init)(struct vfio_device *vdev);       /* 初始化私有字段 */
    void (*release)(struct vfio_device *vdev);    /* 释放私有字段 */
    int  (*bind_iommufd)(...);                     /* 绑定 iommufd */
    void (*unbind_iommufd)(...);
    int  (*attach_ioas)(...);                      /* 附加到 IO 地址空间 */
    void (*detach_ioas)(...);
    int  (*open_device)(struct vfio_device *vdev); /* 第一次 fd open */
    void (*close_device)(struct vfio_device *vdev);
    ssize_t (*read)(...);                          /* 读设备（配置空间等） */
    ssize_t (*write)(...);
    long (*ioctl)(...);                            /* ★ 设备特定 ioctl */
    int  (*mmap)(...);                             /* ★ BAR mmap */
    void (*request)(...);                          /* 请求释放设备 */
    int  (*match)(...);
    void (*dma_unmap)(...);                        /* DMA 解映射通知 */
    int  (*device_feature)(...);                   /* VFIO_DEVICE_FEATURE */
};
```

---

## 3. VFIO PCI 回调表

### Q: `vfio_pci_ops` 怎么挂到 VFIO 框架？

**文件**: `drivers/vfio/pci/vfio_pci.c:130`

```c
static const struct vfio_device_ops vfio_pci_ops = {
    .name           = "vfio-pci",
    .init           = vfio_pci_core_init_dev,
    .release        = vfio_pci_core_release_dev,
    .open_device    = vfio_pci_open_device,        /* ★ fd open → enable 设备 */
    .close_device   = vfio_pci_core_close_device,
    .ioctl          = vfio_pci_core_ioctl,          /* ★ 设备 ioctl 分发 */
    .device_feature = vfio_pci_core_ioctl_feature,
    .read           = vfio_pci_core_read,           /* PCI 配置空间读 */
    .write          = vfio_pci_core_write,          /* PCI 配置空间写 */
    .mmap           = vfio_pci_core_mmap,           /* ★ BAR mmap */
    .request        = vfio_pci_core_request,
    .match          = vfio_pci_core_match,
    .bind_iommufd   = vfio_iommufd_physical_bind,
    .unbind_iommufd = vfio_iommufd_physical_unbind,
    .attach_ioas    = vfio_iommufd_physical_attach_ioas,
    .detach_ioas    = vfio_iommufd_physical_detach_ioas,
};
```

### Q: `vfio_pci_probe()` 如何注册设备？

**文件**: `drivers/vfio/pci/vfio_pci.c:149`

```c
static int vfio_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct vfio_pci_core_device *vdev;

    if (vfio_pci_is_denylisted(pdev))
        return -EINVAL;

    /* ★ 分配 vfio_pci_core_device（包含 vfio_device 作为子结构） */
    vdev = vfio_alloc_device(vfio_pci_core_device, vdev, &pdev->dev,
                             &vfio_pci_ops);

    dev_set_drvdata(&pdev->dev, vdev);
    ret = vfio_pci_core_register_device(vdev);  /* ★ 注册到 VFIO 框架 */
    ...
}
```

**关键**：`driver_managed_dma = true`（`drivers/vfio/pci/vfio_pci.c:205`）
声明此驱动自己管理 DMA，probe 时**不**占用 default domain（`owner_cnt` 不变）。
DMA ownership 的实际认领推迟到 `VFIO_GROUP_SET_CONTAINER` 或 `VFIO_GROUP_GET_DEVICE_FD`。

详见 phase6 corrections.md 勘误 1。

---

## 4. vfio_pci_core_enable()：设备启用

### Q: 设备启用时做了什么？

**文件**: `drivers/vfio/pci/vfio_pci_core.c:500`

```c
int vfio_pci_core_enable(struct vfio_pci_core_device *vdev)
{
    struct pci_dev *pdev = vdev->pdev;

    /* ★ Step 1: 清除 Bus Master（不允许初始状态有 DMA） */
    pci_clear_master(pdev);

    /* ★ Step 2: 启用 PCI 设备 */
    ret = pci_enable_device(pdev);

    /* ★ Step 3: 尝试复位（确保设备在已知状态） */
    ret = pci_try_reset_function(pdev);
    vdev->reset_works = !ret;

    /* ★ Step 4: 保存 PCI 配置空间 */
    pci_save_state(pdev);
    vdev->pci_saved_state = pci_store_saved_state(pdev);

    /* ★ Step 5: INTx 处理 */
    if (likely(!nointxmask)) {
        vdev->pci_2_3 = pci_intx_mask_supported(pdev);
    }

    /* ★ Step 6: 读取 MSI-X 信息 */
    msix_pos = pdev->msix_cap;
    if (msix_pos) {
        pci_read_config_word(pdev, msix_pos + PCI_MSIX_FLAGS, &flags);
        pci_read_config_dword(pdev, msix_pos + PCI_MSIX_TABLE, &table);

        vdev->msix_bar = table & PCI_MSIX_TABLE_BIR;      /* ★ MSI-X 表在哪个 BAR */
        vdev->msix_offset = table & PCI_MSIX_TABLE_OFFSET; /* 表内偏移 */
        vdev->msix_size = ((flags & PCI_MSIX_FLAGS_QSIZE) + 1) * 16;
    }

    /* Step 7: VGA 检测 */
    if (!vfio_vga_disabled() && vfio_pci_is_vga(pdev))
        vdev->has_vga = true;

    return 0;
}
```

### Q: 为什么先 `pci_clear_master` 再 `pci_enable_device`？

安全考虑。VFIO 的目标是让设备处于**已知安全状态**后才交给用户空间。
`pci_clear_master` 确保设备在 enable 前不会发起 DMA。DMA 映射要等 QEMU 显式调
`VFIO_IOMMU_MAP_DMA` 后才开始。

---

## 5. vfio_pci_core_mmap()：BAR 映射

### Q: BAR mmap 如何工作？有什么安全限制？

**文件**: `drivers/vfio/pci/vfio_pci_core.c:1752`

```c
int vfio_pci_core_mmap(struct vfio_device *core_vdev, struct vm_area_struct *vma)
{
    struct vfio_pci_core_device *vdev = ...;
    unsigned int index;

    /* ★ 从 mmap offset 提取 region index */
    index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);

    /* 安全检查 */
    if (index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions) return -EINVAL;
    if ((vma->vm_flags & VM_SHARED) == 0) return -EINVAL;    /* 必须共享映射 */
    if (index >= VFIO_PCI_ROM_REGION_INDEX) return -EINVAL;  /* ROM 不可 mmap */
    if (!vdev->bar_mmap_supported[index]) return -EINVAL;    /* 该 BAR 不支持 */

    /* 计算物理地址范围 */
    phys_len = PAGE_ALIGN(pci_resource_len(pdev, index));
    req_len = vma->vm_end - vma->vm_start;
    pgoff = vma->vm_pgoff & ((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);

    /* ★ 映射 BAR 内存 */
    if (!vdev->barmap[index]) {
        ret = pci_request_selected_regions(pdev, 1 << index, "vfio-pci");
        vdev->barmap[index] = pci_iomap(pdev, index, 0);
    }

    /* ★ 设置页面保护：uncached + decrypted */
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

    vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
    vma->vm_ops = &vfio_pci_mmap_ops;

    return 0;
}
```

### Q: MSI-X 表页面能被 mmap 吗？

| 条件 | MSI-X 表 mmap |
|------|--------------|
| 无中断重映射（IR） | ❌ 从 mmap 中剔除，必须通过 read/write 访问 |
| 有中断重映射（IR） | ✅ 整 BAR 可 mmap（`VFIO_REGION_INFO_CAP_MSIX_MAPPABLE`） |

即使有 IR 允许整 BAR mmap，**QEMU 默认仍以 `msix_table_mmio` subregion 拦截 MSI-X 表访问**。
这是因为 QEMU 需要在 Guest 写 MSI-X 表时同步更新 KVM 中断路由。

详见 CLAUDE.md 已知陷阱 #5（VFIO MSI-X 直通流程）。

---

## 6. vfio_pci_core_ioctl()：设备 ioctl 分发

### Q: 设备 fd 支持哪些 ioctl？

**文件**: `drivers/vfio/pci/vfio_pci_core.c:1488`

```c
long vfio_pci_core_ioctl(struct vfio_device *core_vdev, unsigned int cmd,
                         unsigned long arg)
{
    switch (cmd) {
    case VFIO_DEVICE_GET_INFO:            /* 设备基本信息（flag、region 数量等） */
        return vfio_pci_ioctl_get_info(vdev, uarg);
    case VFIO_DEVICE_GET_REGION_INFO:     /* ★ BAR region 信息（地址、大小、flag） */
        return vfio_pci_ioctl_get_region_info(vdev, uarg);
    case VFIO_DEVICE_GET_IRQ_INFO:        /* ★ 中断信息（INTx/MSI/MSI-X 数量） */
        return vfio_pci_ioctl_get_irq_info(vdev, uarg);
    case VFIO_DEVICE_SET_IRQS:            /* ★ 配置中断（武装/触发/屏蔽） */
        return vfio_pci_ioctl_set_irqs(vdev, uarg);
    case VFIO_DEVICE_RESET:               /* 设备复位 */
        return vfio_pci_ioctl_reset(vdev, uarg);
    case VFIO_DEVICE_PCI_HOT_RESET:       /* PCIe 热复位 */
        return vfio_pci_ioctl_pci_hot_reset(vdev, uarg);
    case VFIO_DEVICE_IOEVENTFD:           /* ioeventfd 注册 */
        return vfio_pci_ioctl_ioeventfd(vdev, uarg);
    default:
        return -ENOTTY;
    }
}
```

### Q: `VFIO_DEVICE_SET_IRQS` 的参数结构？

```c
struct vfio_irq_set {
    __u32 argsz;          /* 结构体大小 */
    __u32 flags;          /* ★ 操作标志 */
    __u32 index;          /* 中断索引（0=INTx, 1=MSI, 2=MSI-X） */
    __u32 start;          /* 起始向量号 */
    __u32 count;          /* 向量数量 */
    __u8 data[];          /* 数据（eventfd 或触发数据） */
};
```

flags 组合：

| flags | 含义 |
|-------|------|
| `VFIO_IRQ_SET_DATA_EVENTFD` + `ACTION` | 用 eventfd 武装中断 |
| `VFIO_IRQ_SET_DATA_NONE` + `ACTION_TRIGGER` | 手动触发中断 |
| `VFIO_IRQ_SET_DATA_BOOL` + `ACTION_MASK` | 屏蔽指定向量 |

`ACTION` 值：

| 值 | 含义 |
|----|------|
| `VFIO_IRQ_SET_ACTION_TRIGGER` | 武装（设置触发源） |
| `VFIO_IRQ_SET_ACTION_UNMASK` | 解除屏蔽 |
| `VFIO_IRQ_SET_ACTION_MASK` | 屏蔽 |

**注意**：对 MSI-X 的 mask/unmask 操作，VFIO 内核侧没有直接导出接口（`drivers/vfio/pci/vfio_pci_intrs.c:854-857` 留空为 "XXX Need masking support exported"）。
mask 位只能由 VMM 直接 pwrite 到物理 MSI-X 表。详见 CLAUDE.md 已知陷阱 #17。

---

## 7. VFIO 核心 ioctl 分发

### Q: 设备 fd 的 ioctl 怎么分发到设备驱动？

**文件**: `drivers/vfio/vfio_main.c:1261`

```c
static long vfio_device_fops_unl_ioctl(struct file *filep,
                                       unsigned int cmd, unsigned long arg)
{
    struct vfio_device_file *df = filep->private_data;
    struct vfio_device *device = df->device;

    /* ★ 特殊处理：BIND_IOMMUFD 不需要 access_granted */
    if (cmd == VFIO_DEVICE_BIND_IOMMUFD)
        return vfio_df_ioctl_bind_iommufd(df, uptr);

    /* ★ 安全检查：必须先 open 设备 */
    if (!smp_load_acquire(&df->access_granted))
        return -EINVAL;

    /* cdev-only ioctls（iommufd 路径） */
    if (IS_ENABLED(CONFIG_VFIO_DEVICE_CDEV) && !df->group) {
        switch (cmd) {
        case VFIO_DEVICE_ATTACH_IOMMUFD_PT: ...
        case VFIO_DEVICE_DETACH_IOMMUFD_PT: ...
        }
    }

    switch (cmd) {
    case VFIO_DEVICE_FEATURE:
        ret = vfio_ioctl_device_feature(device, uptr);
        break;
    default:
        /* ★ 其他 ioctl 全部转发给设备驱动 */
        ret = device->ops->ioctl(device, cmd, arg);
        break;
    }
    return ret;
}
```

### Q: `device->ops->ioctl` 对 vfio-pci 就是 `vfio_pci_core_ioctl()`？

对。调用链：

```
vfio_device_fops_unl_ioctl()
  └→ device->ops->ioctl(device, cmd, arg)
      └→ vfio_pci_core_ioctl()
          └→ switch (cmd) ...
```

---

## 8. KVM-VFIO 桥接

### Q: KVM 如何知道哪些 VFIO 设备属于 VM？

**文件**: `virt/kvm/vfio.c:143`

```c
static int kvm_vfio_file_add(struct kvm_device *dev, unsigned int fd)
{
    struct kvm_vfio *kv = dev->private;
    struct kvm_vfio_file *kvf;
    struct file *filp;

    filp = fget(fd);
    if (!kvm_vfio_file_is_valid(filp))
        return -EINVAL;              /* 不是有效的 VFIO fd */

    /* 检查重复 */
    list_for_each_entry(kvf, &kv->file_list, node)
        if (kvf->file == filp)
            return -EEXIST;

    kvf = kzalloc(sizeof(*kvf), GFP_KERNEL_ACCOUNT);
    kvf->file = get_file(filp);
    list_add_tail(&kvf->node, &kv->file_list);

    /* ★ 三件事 */
    kvm_arch_start_assignment(dev->kvm);       /* 1. 递增 assigned_device_count */
    kvm_vfio_file_set_kvm(kvf->file, dev->kvm); /* 2. 设置 VFIO 文件的 kvm 指针 */
    kvm_vfio_update_coherency(dev);             /* 3. 更新 DMA 一致性状态 */

    return 0;
}
```

### Q: `kvm_vfio_file_add` 会触发 IRTE Posted 化吗？

**不会**。这是一个常见误解。

`kvm_vfio_file_add` 只做三件事，没有一件涉及 IRTE：

| 调用 | 作用 |
|------|------|
| `kvm_arch_start_assignment()` | 递增 `assigned_device_count` |
| `kvm_vfio_file_set_kvm()` | 让 VFIO 知道关联的 KVM |
| `kvm_vfio_update_coherency()` | 更新 noncoherent DMA 标志 |

**真正触发 IRTE Posted 化的是 irq_bypass 的 token 配对**：

```
VFIO 侧:  irq_bypass_register_producer()  → token = ctx->trigger (eventfd)
KVM 侧:   irq_bypass_register_consumer()  → token = irqfd->eventfd
                ↓ token 相等
          __connect()  (virt/lib/irqbypass.c:30)
            └→ kvm_arch_irq_bypass_add_producer()  (arch/x86/kvm/x86.c:13665)
                 └→ vmx_pi_update_irte()
                      └→ intel_ir_set_vcpu_affinity()
                           └→ modify_irte()  ← ★ IRTE 写成 Posted 模式
```

详见 phase6 corrections.md 勘误 4。

### Q: `kvm_vfio_update_coherency()` 做什么？

**文件**: `virt/kvm/vfio.c:120`

```c
static void kvm_vfio_update_coherency(struct kvm_device *dev)
{
    struct kvm_vfio *kv = dev->private;
    bool noncoherent = false;

    /* 遍历所有关联的 VFIO 文件 */
    list_for_each_entry(kvf, &kv->file_list, node) {
        if (!kvm_vfio_file_enforced_coherent(kvf->file)) {
            noncoherent = true;
            break;
        }
    }

    if (noncoherent != kv->noncoherent) {
        kv->noncoherent = noncoherent;
        if (kv->noncoherent)
            kvm_arch_register_noncoherent_dma(dev->kvm);   /* 需要软件维护一致性 */
        else
            kvm_arch_unregister_noncoherent_dma(dev->kvm);
    }
}
```

noncoherent DMA = 设备不保证 cache 一致性，KVM 需要在 VM-Exit 时刷新 cache。
Intel VT-d 通常是 coherent 的（硬件保证），某些 ARM 平台可能不是。

---

## 9. DMA 映射：VFIO IOMMU Type 1

### Q: `VFIO_IOMMU_MAP_DMA` 的完整路径？

**文件**: `drivers/vfio/vfio_iommu_type1.c:1548`

```c
static int vfio_dma_do_map(struct vfio_iommu *iommu,
                           struct vfio_iommu_type1_dma_map *map)
{
    dma_addr_t iova = map->iova;
    unsigned long vaddr = map->vaddr;
    size_t size = map->size;
    int prot = 0;

    /* ★ 设置权限 */
    if (map->flags & VFIO_DMA_MAP_FLAG_WRITE) prot |= IOMMU_WRITE;
    if (map->flags & VFIO_DMA_MAP_FLAG_READ)  prot |= IOMMU_READ;

    /* ★ 对齐检查（必须页对齐） */
    pgsize = (size_t)1 << __ffs(iommu->pgsize_bitmap);
    if (!size || (size | iova | vaddr) & (pgsize - 1))
        return -EINVAL;

    /* ★ 检查 IOVA 范围是否空闲 */
    dma = vfio_find_dma(iommu, iova, size);
    if (dma) return -EEXIST;           /* 已存在映射 */

    /* ★ 固定物理页 + 创建 IOMMU 映射 */
    ret = vfio_pin_map_dma(iommu, dma, size);
    /*   ├→ get_user_pages_fast()        ← 固定用户空间物理页 */
    /*   ├→ 创建 vfio_pfn 记录           ← 记录映射的物理页 */
    /*   └→ iommu_map()                  ← IOVA → PFN + 权限 → IOMMU 页表 */

    /* ★ 加入红黑树 */
    vfio_link_dma(iommu, dma);
    return 0;
}
```

### Q: DMA 映射涉及哪些安全约束？

| 约束 | 来源 | 作用 |
|------|------|------|
| 对齐检查 | `size \| iova \| vaddr & (pgsize - 1)` | 防止非对齐映射 |
| 内存锁定限制 | `RLIMIT_MEMLOCK` | 防止用户空间锁定过多内存 |
| IOVA 不重叠 | `vfio_find_dma()` | 防止地址冲突 |
| 页面固定 | `get_user_pages_fast()` | 防止 DMA 目标页被换出 |
| IOMMU 翻译 | `iommu_map()` | 硬件强制隔离 |

---

## 10. IOMMU 组与设备隔离

### Q: IOMMU 组是什么？怎么划分的？

```
IOMMU 组 = 必须一起直通的设备集合

同一组内的设备:
  - 必须一起直通（或都不直通）
  - 共享同一个 IOMMU domain
  - 不能独立隔离

不同组的设备:
  - 可以独立直通
  - 各自有独立的 IOVA 空间
```

### Q: 组划分的关键代码？

**文件**: `drivers/iommu/iommu.c:1543`（简化）

```c
static struct iommu_group *pci_device_group(struct device *dev)
{
    /* ★ Step 1: DMA 别名查找 */
    pci_for_each_dma_alias(pdev, get_pci_alias_or_group, &data);
    if (data.group) return data.group;     /* 找到已有组 */

    /* ★ Step 2: ACS 隔离检查 */
    for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
        if (!bus->self) continue;
        if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
            break;                          /* 上游隔离成立 → 停止 */
        pdev = bus->self;                   /* 隔离不成立 → 把桥拉进同组 */
    }

    /* Step 3: 查找或创建组 */
    ...
}
```

### Q: `REQ_ACS_FLAGS` 包含哪些？

**文件**: `drivers/iommu/iommu.c:1383`

```c
#define REQ_ACS_FLAGS   (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)
```

| 标志 | 含义 | 作用 |
|------|------|------|
| `PCI_ACS_SV` | Source Validation | 验证请求来源 |
| `PCI_ACS_RR` | P2P Request Redirect | 重定向 P2P 请求到上游 |
| `PCI_ACS_CR` | P2P Completion Redirect | 重定向 P2P 完成到上游 |
| `PCI_ACS_UF` | Upstream Forwarding | 上游转发 |

**注意**：`pci_acs_flags_enabled()` 会先按设备声明的 `ACSCap` 掩码：

```c
/* 来源: drivers/pci/pci.c:3597 */
pci_read_config_word(pdev, pos + PCI_ACS_CAP, &cap);
acs_flags &= (cap | PCI_ACS_EC);     /* ★ 没声明的能力不算失败 */
```

只有「Cap 里声明了、Ctl 里没开」才返回 false。详见 CLAUDE.md 已知陷阱 #11。

---

## 11. DMA Ownership 认领时机

### Q: DMA ownership 什么时候被认领？

**文件**: `drivers/vfio/container.c:437`

```c
/* 旧 API（container FD）：在 SET_CONTAINER 时认领 */
ret = iommu_group_claim_dma_owner(group->iommu_group, group);
```

**文件**: `drivers/vfio/group.c:373`（注释）

```c
/* With the container FD the iommu_group_claim_dma_owner() is done
 * during SET_CONTAINER but for IOMMUFd this is done during
 * VFIO_GROUP_GET_DEVICE_FD. */
```

| 路径 | 认领时机 |
|------|---------|
| container FD（旧） | `VFIO_GROUP_SET_CONTAINER` |
| iommufd（新） | `VFIO_GROUP_GET_DEVICE_FD` |

认领时如果同组有其他设备绑在普通驱动上，`owner_cnt > 0` → 返回 `-EPERM`。

详见 phase6 corrections.md 勘误 1。

### Q: 认领时的安全联锁？

**文件**: `drivers/iommu/iommu.c:3184`

```c
static int __iommu_take_dma_ownership(struct iommu_group *group, void *owner)
{
    /* ★ 1. 分配 blocking domain */
    ret = __iommu_group_alloc_blocking_domain(group);

    /* ★ 2. 先切换到 blocking domain（所有 DMA 被拒） */
    ret = __iommu_group_set_domain(group, group->blocking_domain);

    /* ★ 3. 认领 ownership */
    ...

    /* ★ 4. 切换到 VFIO 的 domain */
    ...
}
```

```
时序:
  blocking_domain_attach_dev    ← 先阻断所有 DMA
  iommu_group_claim_dma_owner   ← 认领
  vfio_iommu_type1_attach_group ← 切换到 VFIO domain
```

blocking domain 确保 ownership 转移期间 DMA 窗口始终关闭。
详见 phase6 corrections.md 勘误 3。

---

## 12. 设备 fd 与配置空间访问

### Q: 用户态如何访问 PCI 配置空间？

通过 `read()` / `write()` 系统调用：

```c
/* 来源: drivers/vfio/pci/vfio_pci_core.c */

ssize_t vfio_pci_core_read(struct vfio_device *core_vdev, char __user *buf,
                           size_t count, loff_t *ppos)
{
    /* 根据 *ppos 判断访问区域 */
    if (*ppos < VFIO_PCI_OFFSET_DATA) {
        /* 配置空间访问 → 读 PCI config */
        ret = pci_read_config_byte/word/dword(pdev, ...);
    }
    ...
}
```

**QEMU 侧**：

```c
/* QEMU 通过 device fd 的 read/write 访问配置空间 */
pread(device_fd, buf, size, offset);  /* offset = VFIO_PCI_CONFIG_REGION_INDEX * page + reg */
```

---

## 13. 完整调用链

```
QEMU 设置 VFIO 设备直通的完整流程:

  QEMU                                    KVM                    VFIO / IOMMU
  ────                                    ───                    ────────────

  1. 打开 VFIO 容器
     open("/dev/vfio/vfio") ──────────────────────────────────────▶
       └→ vfio_fops_open()

  2. 检查 API 版本
     ioctl(VFIO_GET_API_VERSION) ─────────────────────────────────▶
       └→ return VFIO_API_VERSION (0)

  3. 获取 VFIO 组
     open("/dev/vfio/$GROUP") ────────────────────────────────────▶
       └→ vfio_group_fops_open()

  4. 关联组到容器
     ioctl(VFIO_GROUP_SET_CONTAINER) ─────────────────────────────▶
       └→ vfio_group_set_container()

  5. 设置 IOMMU 类型
     ioctl(VFIO_SET_IOMMU, TYPE1) ────────────────────────────────▶
       └→ vfio_iommu_type1_attach_group()
           └→ iommu_domain_alloc() + intel_iommu_attach_device()

  6. ★ 关联组到 KVM VM（让 KVM 知道 VFIO 设备）
     ioctl(KVM_DEV_VFIO_FILE_ADD, group_fd) ──▶
       └→ kvm_vfio_file_add()
           ├→ kvm_arch_start_assignment()      ← assigned_device_count++
           ├→ kvm_vfio_file_set_kvm()
           └→ kvm_vfio_update_coherency()

  7. 获取设备 fd
     ioctl(VFIO_GROUP_GET_DEVICE_FD) ─────────────────────────────▶
       └→ vfio_group_get_device_fd()
           └→ vfio_device_open()
               └→ vfio_pci_open_device()
                   └→ vfio_pci_core_enable()    ← 启用设备、读 MSI-X 信息

  8. DMA 映射
     ioctl(VFIO_IOMMU_MAP_DMA, {iova, vaddr, size}) ──────────────▶
       └→ vfio_dma_do_map()
           ├→ get_user_pages_fast()           ← 固定物理页
           └→ iommu_map()                     ← IOVA → PFN

  9. 查询 region 信息
     ioctl(VFIO_DEVICE_GET_REGION_INFO) ──────────────────────────▶
       └→ vfio_pci_ioctl_get_region_info()    ← BAR 地址/大小/flag

  10. 映射 BAR 到 QEMU 地址空间
      mmap(device_fd, bar_offset) ────────────────────────────────▶
        └→ vfio_pci_core_mmap()
            └→ remap_pfn_range()              ← QEMU 直接访问设备 MMIO

  11. 查询中断信息
      ioctl(VFIO_DEVICE_GET_IRQ_INFO, index=2) ──────────────────▶
        └→ 返回 MSI-X 向量数量

  12. 武装中断（eventfd 方式）
      ioctl(VFIO_DEVICE_SET_IRQS, {index=2, DATA_EVENTFD, TRIGGER}) ▶
        └→ vfio_pci_ioctl_set_irqs()
            └→ vfio_msi_enable()
                ├→ pci_alloc_irq_vectors()    ← 分配 MSI-X 向量
                ├→ request_irq()              ← 注册 IRQ handler
                └→ irq_bypass_register_producer()  ← ★ token = eventfd

  13. ★ QEMU 侧 irqfd 配对（Posted Interrupts 的触发点）
      ioctl(KVM_IRQFD, {fd=irqfd, gsi=N}) ──▶
        └→ kvm_irqfd()
            └→ irq_bypass_register_consumer() ← ★ token = irqfd eventfd
                └→ token 匹配 → __connect()
                    └→ kvm_arch_irq_bypass_add_producer()
                        └→ vmx_pi_update_irte()
                            └→ modify_irte()  ← ★ IRTE IM=1 (Posted)

  14. VM 运行
      ioctl(KVM_RUN) ──▶
        └→ vcpu_enter_guest()
            └→ VM-Entry
               Guest 直接访问设备 MMIO（通过 mmap 映射）
               Guest 发起 DMA → IOMMU 翻译 → 物理内存
               设备中断 → IOMMU → PI Descriptor → vCPU（零 VM-Exit）
```
