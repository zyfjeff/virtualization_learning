# 第四阶段源码注释：VFIO 设备直通

> 基于 Linux 6.12.93 源码 | 对应源码树 `drivers/vfio/` 和 `virt/kvm/`

---

## 1. VFIO 核心框架（vfio_main.c）

### 1.1 模块概述

```c
/* 来源: drivers/vfio/vfio_main.c */

/*
 * VFIO - Virtual Function I/O
 *
 * 模块描述:
 * "VFIO - User Level meta-driver for PCI devices"
 *
 * VFIO 是 Linux 中用于安全设备访问的用户态框架。
 * 它提供:
 *   1. 设备发现（通过 sysfs IOMMU 组）
 *   2. 安全的设备配置空间访问
 *   3. DMA 映射/解映射
 *   4. 设备中断管理
 *
 * 核心文件:
 *   /dev/vfio/vfio       - VFIO API 版本检查，容器管理
 *   /dev/vfio/$GROUP_ID  - IOMMU 组访问
 *   (通过组获取设备 fd)   - 设备特定操作
 */
```

### 1.2 核心数据结构

```c
/* 来源: include/linux/vfio.h, drivers/vfio/vfio_main.c */

/*
 * struct vfio_device - VFIO 设备核心结构
 *
 * 每个被 VFIO 管理的物理设备对应一个 vfio_device
 * 由设备特定驱动（如 vfio-pci）注册
 */
struct vfio_device {
    struct device *dev;           /* 底层 Linux 设备 */
    const struct vfio_device_ops *ops;  /* 设备操作回调 */
    struct vfio_group *group;     /* 所属的 VFIO 组 */
    struct vfio_device_set *dev_set;  /* 设备集（迁移用）*/
    struct iommufd_ctx *iommufd_ictx; /* IOMMUFD 上下文 */

    /* 内部状态 */
    struct kref kref;             /* 引用计数 */
    struct rw_semaphore ops_rwsem; /* ops 读写锁 */

    /* 迁移相关 */
    unsigned int migration_flags;
    enum vfio_device_mig_state migration_state;

    /* 设备特定数据跟在结构体后面（container_of 访问）*/
};

/*
 * struct vfio_device_ops - 设备操作回调
 *
 * 由设备特定驱动（vfio-pci 等）提供
 * VFIO 核心通过这些回调与设备交互
 */
struct vfio_device_ops {
    char *name;

    /* 生命周期 */
    int  (*open_device)(struct vfio_device *vdev);
    void (*close_device)(struct vfio_device *vdev);

    /* I/O 操作 */
    ssize_t (*read)(struct vfio_device *vdev, char __user *buf,
                    size_t count, loff_t *ppos);
    ssize_t (*write)(struct vfio_device *vdev, const char __user *buf,
                     size_t count, loff_t *ppos);
    int (*mmap)(struct vfio_device *vdev, struct vm_area_struct *vma);

    /* ioctl - 设备特定操作 */
    long (*ioctl)(struct vfio_device *vdev, unsigned int cmd,
                  unsigned long arg);

    /* 绑定/解绑 */
    int (*bind_iommufd)(struct vfio_device *vdev,
                        struct iommufd_ctx *ictx, u32 *out_device_id);
    void (*unbind_iommufd)(struct vfio_device *vdev,
                           struct iommufd_ctx *ictx);
    int (*attach_ioas)(struct vfio_device *vdev, u32 *pt_id);
    void (*detach_ioas)(struct vfio_device *vdev);

    /* 请求 */
    void (*request)(struct vfio_device *vdev, unsigned int count);
    int (*get_datapfns)(struct vfio_device *vdev, ...);
};
```

### 1.3 VFIO 核心 ioctl 分发

```c
/* 来源: drivers/vfio/vfio_main.c (简化流程) */

/*
 * VFIO 容器级 ioctl 处理
 *
 * /dev/vfio/vfio 是容器设备文件
 * 支持的 ioctl:
 */

/*
 * VFIO_GET_API_VERSION:
 *   返回 VFIO API 版本号 (VFIO_API_VERSION = 0)
 *   用于用户态确认内核支持 VFIO
 *
 * VFIO_CHECK_EXTENSION:
 *   检查是否支持特定扩展
 *   - VFIO_TYPE1_IOMMU: Type 1 IOMMU 支持
 *   - VFIO_TYPE1v2_IOMMU: Type 1 v2（支持页面固定通知）
 *   - VFIO_SPAPR_TCE_IOMMU: SPAPR TCE（PowerPC）
 *   - VFIO_VIRQ_CHAIN: 虚拟中断链
 *
 * VFIO_SET_IOMMU:
 *   设置容器的 IOMMU 驱动
 *   通常在 VFIO_GROUP_SET_CONTAINER 之后调用
 *   参数: IOMMU 类型（如 VFIO_TYPE1_IOMMU）
 */
```

---

## 2. IOMMU Type 1 驱动（vfio_iommu_type1.c）

### 2.1 模块概述

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c */

/*
 * VFIO IOMMU Type 1 驱动
 *
 * 模块描述:
 * "VFIO IOMMU Type 1 for Intel VT-d and AMD-Vi"
 *
 * Type 1 IOMMU 驱动支持:
 *   - Intel VT-d: DMA remapping hardware
 *   - AMD-Vi: AMD IOMMU
 *
 * 核心功能:
 *   1. DMA 映射: 将用户空间虚拟地址映射到 IOMMU IOVA
 *   2. DMA 解映射: 解除 IOVA 到物理页的映射
 *   3. 页面固定: 防止 DMA 目标页面被换出
 *   4. 脏页跟踪: 支持实时迁移中的脏页检测
 */

/*
 * 关键数据结构:
 */

/* DMA 映射描述 */
struct vfio_dma {
    struct rb_node node;          /* DMA 映射的红黑树节点 */
    dma_addr_t iova;              /* I/O 虚拟地址 */
    unsigned long vaddr;          /* 用户空间虚拟地址 */
    size_t size;                  /* 映射大小 */
    int prot;                     /* 保护标志 (IOMMU_READ/WRITE) */
    size_t locked;                /* 已固定的页数 */
    struct task_struct *task;     /* 映射所属进程 */
    struct vfio_pfn *pfn_list;    /* 映射的物理页列表 */
    bool cache_remote;            /* 是否缓存远程映射 */
};

/* 物理页跟踪 */
struct vfio_pfn {
    struct rb_node node;
    unsigned long pfn;            /* 物理页帧号 */
    int prot;                     /* 保护标志 */
    unsigned long vaddr;          /* 对应的用户空间虚拟地址 */
    bool dirty;                   /* 脏页标记 */
    struct page *page;            /* struct page 指针 */
};
```

### 2.2 DMA 映射完整路径

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c */

/*
 * vfio_dma_do_map - DMA 映射核心函数
 *
 * 流程:
 *   1. 解析用户态参数（IOVA, VADDR, 大小）
 *   2. 检查 IOVA 范围是否空闲
 *   3. 固定用户空间物理页
 *   4. 通过 IOMMU 核心创建映射
 *
 * @iommu:     IOMMU 实例
 * @iova:      目标 I/O 虚拟地址
 * @vaddr:     用户空间虚拟地址
 * @size:      映射大小（必须页对齐）
 * @prot:      保护标志 (IOMMU_READ | IOMMU_WRITE)
 * @type:      映射类型
 */

/*
 * DMA 映射完整路径:
 *
 * ioctl(VFIO_IOMMU_MAP_DMA)
 *     │
 *     ▼
 * vfio_iommu_type1_ioctl()
 *     │
 *     ▼
 * vfio_dma_do_map(iommu, &map)
 *     │
 *     ├── 验证参数
 *     │   ├── IOVA 必须页对齐
 *     │   ├── 大小必须 > 0
 *     │   └── VADDR 必须在用户空间有效
 *     │
 *     ├── vfio_find_dma_valid()
 *     │   └── 检查 IOVA 范围是否与已有映射重叠
 *     │       └── 如果重叠 → 返回 -EEXIST
 *     │
 *     ├── vfio_lock_acct()
 *     │   └── 检查内存锁定限制 (RLIMIT_MEMLOCK)
 *     │
 *     ├── vfio_pin_pages_remote()
 *     │   ├── get_user_pages_fast() → 固定物理页
 *     │   ├── 创建 vfio_pfn 记录
 *     │   └── 将 pfn 加入 DMA 的 pfn_list
 *     │
 *     ├── iommu_map()
 *     │   ├── 在 IOMMU 页表中创建 IOVA→PFN 条目
 *     │   ├── 设置权限 (Read/Write)
 *     │   └── 刷新 IOMMU TLB (如果需要)
 *     │
 *     └── 更新 DMA 映射的红黑树
 *         └── rb_insert(&iommu->dma_list, &dma->node)
 */

static int vfio_dma_do_map(struct vfio_iommu *iommu,
                           struct vfio_iommu_type1_dma_map *map)
{
    dma_addr_t iova = map->iova;
    unsigned long vaddr = map->vaddr;
    size_t size = map->size;
    int prot = 0;
    struct vfio_dma *dma;
    int ret;

    /* 设置保护标志 */
    if (map->flags & VFIO_DMA_MAP_FLAG_READ)
        prot |= IOMMU_READ;
    if (map->flags & VFIO_DMA_MAP_FLAG_WRITE)
        prot |= IOMMU_WRITE;

    /* 检查对齐 */
    if (!IS_ALIGNED(iova, PAGE_SIZE) ||
        !IS_ALIGNED(vaddr, PAGE_SIZE) ||
        !IS_ALIGNED(size, PAGE_SIZE))
        return -EINVAL;

    /* 检查是否已存在 */
    if (vfio_find_dma_valid(iommu, iova, size))
        return -EEXIST;

    /* 创建 DMA 映射结构 */
    dma = kzalloc(sizeof(*dma), GFP_KERNEL);
    dma->iova = iova;
    dma->vaddr = vaddr;
    dma->size = size;
    dma->prot = prot;

    /* 固定物理页并创建 IOMMU 映射 */
    ret = vfio_pin_map_dma(iommu, dma, size);
    if (ret) {
        kfree(dma);
        return ret;
    }

    /* 加入红黑树 */
    vfio_link_dma(iommu, dma);
    return 0;
}
```

### 2.3 DMA 映射流程图

```
DMA 映射详细流程:

  用户空间                        内核空间
  ────────                        ────────
                                  ┌──────────────────────────┐
  ioctl(MAP_DMA)                  │ vfio_iommu_type1_ioctl()  │
  { iova, vaddr, size } ───────▶  │                          │
                                  │ vfio_dma_do_map()         │
                                  │  ├── 参数验证             │
                                  │  │   ├── 对齐检查         │
                                  │  │   ├── 重叠检查         │
                                  │  │   └── 权限检查         │
                                  │  │                       │
                                  │  ├── 内存锁定检查         │
                                  │  │   └── RLIMIT_MEMLOCK   │
                                  │  │                       │
                                  │  ├── 固定物理页           │
                                  │  │   ┌─────────────────┐ │
                                  │  │   │ get_user_pages   │ │
                                  │  │   │ _fast()         │ │
                                  │  │   │                 │ │
                                  │  │   │ VADDR → PFN     │ │
                                  │  │   │ (每个 PAGE_SIZE) │ │
                                  │  │   └────────┬────────┘ │
                                  │  │            │          │
                                  │  ├── 创建 IOMMU 映射      │
                                  │  │   ┌────────┴────────┐ │
                                  │  │   │ iommu_map()     │ │
                                  │  │   │                 │ │
                                  │  │   │ IOVA → PFN      │ │
                                  │  │   │ + 权限位        │ │
                                  │  │   │ + TLB 刷新      │ │
                                  │  │   └─────────────────┘ │
                                  │  │                       │
                                  │  └── 记录到红黑树        │
                                  │      dma_list ← dma      │
                                  └──────────────────────────┘

  结果:
    IOVA 空间:   [iova, iova+size) → [PFN0, PFN1, PFN2, ...]
    IOMMU 页表:  已更新
    设备 DMA:    现在可以通过 IOVA 访问这些物理页
```

---

## 3. VFIO PCI 驱动（vfio_pci_core.c）

### 3.1 模块概述

```c
/* 来源: drivers/vfio/pci/vfio_pci_core.c */

/*
 * VFIO PCI 核心驱动
 *
 * 模块描述:
 * "VFIO PCI - User Level meta-driver for PCI devices"
 *
 * 提供 PCI 设备的 VFIO 接口:
 *   - PCI 配置空间访问（包括扩展配置空间）
 *   - PCI BAR 区域的 mmap 映射
 *   - 中断管理（INTx, MSI, MSI-X）
 *   - 设备复位
 *   - SR-IOV 支持
 */

/*
 * VFIO PCI 设备操作回调:
 */
static const struct vfio_device_ops vfio_pci_ops = {
    .name           = "vfio-pci",
    .open_device    = vfio_pci_core_enable,
    .close_device   = vfio_pci_core_disable,
    .ioctl          = vfio_pci_ioctl,
    .read           = vfio_pci_read,
    .write          = vfio_pci_write,
    .mmap           = vfio_pci_core_mmap,
    .request        = vfio_pci_req_trigger,
    .bind_iommufd   = vfio_pci_core_bind_iommufd,
    .unbind_iommufd = vfio_pci_core_unbind_iommufd,
    .attach_ioas    = vfio_pci_core_attach_ioas,
    .detach_ioas    = vfio_pci_core_detach_ioas,
};
```

### 3.2 设备启用流程

```c
/* 来源: drivers/vfio/pci/vfio_pci_core.c (简化) */

/*
 * vfio_pci_core_enable - 启用 PCI 设备直通
 *
 * 当用户态打开设备文件时调用
 */
int vfio_pci_core_enable(struct vfio_device *core_vdev)
{
    struct vfio_pci_core_device *vdev =
        container_of(core_vdev, struct vfio_pci_core_device, vdev);
    struct pci_dev *pdev = vdev->pdev;
    int ret;

    /*
     * Step 1: 重置设备
     * 确保设备在直通前处于已知状态
     */
    ret = pci_reset_function(pdev);

    /*
     * Step 2: 启用设备
     * 启用 PCI 设备的 MMIO 和 Bus Master 能力
     */
    ret = pci_enable_device(pdev);

    /*
     * Step 3: 保存 PCI 配置空间
     * 保存原始配置，用于设备释放时恢复
     */
    vfio_pci_save_config(vdev);

    /*
     * Step 4: 设置 BAR 区域
     * 记录每个 BAR 的基地址和大小
     */
    for (i = 0; i < PCI_STD_NUM_BARS; i++) {
        /* 检查 BAR 类型和大小 */
        /* 记录 MMIO 区域信息 */
    }

    /*
     * Step 5: 配置中断
     * 确定设备支持的中断类型:
     *   - INTx (传统中断)
     *   - MSI (Message Signaled Interrupt)
     *   - MSI-X (Extended MSI)
     */

    /*
     * Step 6: 设置 MMIO 映射
     * 将设备 BAR 区域映射到用户空间
     * 用户态（QEMU）可以直接读写设备 MMIO
     */

    return 0;
}
```

### 3.3 MMIO 映射

```c
/* 来源: drivers/vfio/pci/vfio_pci_core.c (简化) */

/*
 * vfio_pci_core_mmap - 映射设备 MMIO 到用户空间
 *
 * 允许 QEMU 直接访问设备 MMIO 寄存器
 * 无需通过 ioctl，减少内核-用户态切换开销
 *
 * 映射区域:
 *   - PCI BAR 中的 MMIO 区域（非 I/O 端口）
 *   - 通过 mmap 直接暴露给用户态
 */

/*
 * MMIO 映射路径:
 *
 * QEMU mmap() ──▶ vfio_pci_core_mmap()
 *                    │
 *                    ├── 检查 BAR 区域有效性
 *                    ├── 检查权限（不可映射 I/O 端口 BAR）
 *                    │
 *                    └── remap_pfn_range()
 *                        └── 将设备物理地址映射到用户空间 VMA
 *
 * 结果:
 *   QEMU 获得设备 MMIO 的直接映射
 *   可以直接读写设备寄存器（通过指针访问）
 *   硬件 MMIO 事务通过 PCIe 总线到达设备
 */
```

---

## 4. KVM-VFIO 桥接（virt/kvm/vfio.c）

### 4.1 完整源码分析

```c
/* 来源: virt/kvm/vfio.c */

/*
 * KVM VFIO 桥接模块
 *
 * 这是 KVM 和 VFIO 之间的桥接层
 * 允许 KVM 感知 VFIO 管理的设备组
 *
 * 主要用途:
 *   1. Posted Interrupts: KVM 需要知道哪些设备属于 VM，
 *      以便正确配置 IRTE 的 Posted Interrupt 字段
 *   2. DMA 一致性: 确保设备 DMA 与 KVM 内存管理一致
 *   3. 设备安全: 通过 VFIO 组机制保证隔离
 *
 * 实现为 KVM 设备文件: /dev/kvm 的 KVM_CREATE_DEVICE 接口
 * 设备类型: KVM_DEV_TYPE_VFIO
 */

/*
 * KVM VFIO 设备操作:
 */
static struct kvm_device_ops kvm_vfio_ops = {
    .name = "kvm-vfio",
    .create = kvm_vfio_create,
    .destroy = kvm_vfio_destroy,
    .set_attr = kvm_vfio_set_attr,
    .has_attr = kvm_vfio_has_attr,
};

/*
 * 支持的操作属性:
 *
 * KVM_DEV_VFIO_GROUP (属性组):
 *   KVM_DEV_VFIO_GROUP_ADD:    添加 VFIO 组
 *   KVM_DEV_VFIO_GROUP_DEL:    删除 VFIO 组
 *   KVM_DEV_VFIO_GROUP_SET_SPAPR_TCE: 设置 TCE（PowerPC）
 *
 * KVM_DEV_VFIO_FILE (属性组):
 *   KVM_DEV_VFIO_FILE_ADD:     添加 VFIO 文件
 *   KVM_DEV_VFIO_FILE_DEL:     删除 VFIO 文件
 */
```

### 4.2 KVM VFIO 组管理

```c
/* 来源: virt/kvm/vfio.c (简化分析) */

/*
 * kvm_vfio_group 结构:
 *
 * struct kvm_vfio_group {
 *     struct list_head node;     // 链表节点
 *     struct file *file;         // VFIO 组文件描述符
 * };
 *
 * KVM 维护一个 kvm_vfio_group 列表，记录所有关联到 VM 的 VFIO 组
 */

/*
 * kvm_vfio_group_add - 添加 VFIO 组到 KVM VM
 *
 * QEMU 通过 ioctl 调用此函数:
 *   KVM_DEV_VFIO_GROUP_ADD + fd (VFIO 组文件描述符)
 *
 * 流程:
 *   1. 获取 VFIO 组的引用 (vfio_file_iommu_group)
 *   2. 检查是否已经添加（避免重复）
 *   3. 创建 kvm_vfio_group 记录
 *   4. 加入 kvm->vfio_groups 链表
 *   5. 更新 DMA 一致性状态
 */

/*
 * kvm_vfio_group_del - 从 KVM VM 移除 VFIO 组
 *
 * 流程:
 *   1. 在 kvm->vfio_groups 链表中查找匹配的文件
 *   2. 从链表中删除
 *   3. 释放 VFIO 组引用
 *   4. 更新 DMA 一致性状态
 */

/*
 * kvm_vfio_update_coherency - 更新 DMA 一致性
 *
 * 当 VFIO 组列表发生变化时调用
 *
 * 作用:
 *   检查是否有任何 VFIO 组使用非一致性 DMA
 *   如果是，设置 kvm->arch.noncoherent_dma = true
 *   这会影响 KVM 的内存管理策略（如 cache 刷新）
 */
```

### 4.3 KVM-VFIO 交互时序图

```
QEMU 设置设备直通的完整时序:

  QEMU                              KVM                    VFIO
  ────                              ───                    ────
  │                                  │                      │
  │ 1. 打开 VFIO 容器                │                      │
  │ open("/dev/vfio/vfio") ─────────────────────────────────▶│
  │                                  │                      │
  │ 2. 检查 API 版本                 │                      │
  │ ioctl(VFIO_GET_API_VERSION) ────────────────────────────▶│
  │                                  │                      │
  │ 3. 设置 IOMMU 类型               │                      │
  │ ioctl(VFIO_SET_IOMMU, TYPE1) ───────────────────────────▶│
  │                                  │                      │
  │ 4. 获取 VFIO 组                  │                      │
  │ open("/dev/vfio/5") ────────────────────────────────────▶│
  │                                  │                      │
  │ 5. 将组关联到容器                │                      │
  │ ioctl(GROUP_SET_CONTAINER) ─────────────────────────────▶│
  │                                  │                      │
  │ 6. 将组关联到 KVM VM  ←───────── 关键步骤              │
  │ ioctl(KVM_DEV_VFIO_GROUP_ADD,    │                      │
  │       group_fd) ────────────────▶│                      │
  │                                  │── kvm_vfio_group_add │
  │                                  │── 获取 vfio_group    │
  │                                  │── 加入列表           │
  │                                  │                      │
  │ 7. 获取设备 fd                   │                      │
  │ ioctl(GROUP_GET_DEVICE_FD) ─────────────────────────────▶│
  │                                  │                      │
  │ 8. 映射 DMA 区域                 │                      │
  │ ioctl(VFIO_IOMMU_MAP_DMA, ─────────────────────────────▶│
  │       iova, vaddr, size)         │                      │
  │                                  │── iommu_map()        │
  │                                  │                      │
  │ 9. 启用设备                      │                      │
  │ ioctl(DEVICE_OPEN) ─────────────────────────────────────▶│
  │                                  │── vfio_pci_enable()  │
  │                                  │                      │
  │ 10. 映射设备 MMIO                │                      │
  │ mmap(device_fd, bar_offset) ────────────────────────────▶│
  │                                  │── remap_pfn_range()  │
  │                                  │                      │
  │ 11. 配置中断                     │                      │
  │ ioctl(SET_IRQS, MSI-X config) ─────────────────────────▶│
  │                                  │                      │
  │ 12. VM 运行                      │                      │
  │ ioctl(KVM_RUN) ────────────────▶│                      │
  │                                  │── VM-Entry           │
  │                                  │   Guest 直接访问设备  │
  │                                  │   设备 DMA → IOMMU → │
  │                                  │   物理内存           │
```

---

## 5. IOMMU 组与设备隔离

### 5.1 IOMMU 组概念

```
IOMMU 组拓扑:

  PCIe 拓扑:                    IOMMU 组划分:
  ┌─────────────────┐          ┌─────────────────────┐
  │   Root Complex   │          │                     │
  │                  │          │  Group 0:           │
  │  ┌───┐  ┌───┐  │          │   - Root Complex    │
  │  │0:0│  │0:1│  │          │   - 不可分割设备     │
  │  └─┬─┘  └─┬─┘  │          │                     │
  │    │       │    │          │  Group 5:           │
  │  ┌─┴─┐   ┌┴──┐ │          │   - 03:00.0 (NIC)  │
  │  │1:0│   │2:0│ │          │   (可直通)          │
  │  └─┬─┘   └─┬─┘ │          │                     │
  │    │       │    │          │  Group 8:           │
  │  ┌─┴─┐   ┌┴──┐ │          │   - 05:00.0 (GPU)  │
  │  │3:0│   │5:0│ │          │   (可直通)          │
  │  └───┘   └───┘ │          │                     │
  └─────────────────┘          └─────────────────────┘

  规则:
    - 同一组内的设备必须一起直通（或都不直通）
    - 不同组的设备可以独立直通
    - ACS (Access Control Services) 允许分离设备到独立组
    - 如果 ACS 不支持，下游设备可能与上游在同一组
```

### 5.2 IOMMU 域

```
IOMMU 域管理:

  ┌─────────────────────────────────────────────┐
  │           IOMMU Domain                       │
  │                                             │
  │  ┌─────────────┐                            │
  │  │ iommu_domain│                            │
  │  │             │                            │
  │  │ geometry:   │                            │
  │  │  aperture_start                               │
  │  │  aperture_end                                 │
  │  │  force_aperture                               │
  │  │             │                            │
  │  │ paging_ops: │ ← 页表操作回调             │
  │  │  map/unmap  │                            │
  │  │  iotlb_sync │                            │
  │  └─────────────┘                            │
  │                                             │
  │  关联设备:                                    │
  │    device 1 (NIC)  ───┐                     │
  │    device 2 (extra)  ──┤── 共享同一 IOVA 空间│
  │                        │                     │
  │  IOVA 空间:             │                     │
  │    [0x0000 - 0xFFFF]   │                     │
  │    独立于系统物理地址    │                     │
  │    由 IOMMU 翻译到 HPA  │                     │
  └─────────────────────────────────────────────┘
```

---

## 6. 调试技巧

### 6.1 查看 VFIO 内部状态

```bash
# 查看已加载的 VFIO 模块
lsmod | grep vfio

# 查看 VFIO 组
ls -la /dev/vfio/

# 查看 IOMMU 组信息
for g in /sys/kernel/iommu_groups/*; do
    echo "Group $(basename $g):"
    ls $g/devices/
done

# 查看 IOMMU 域
dmesg | grep "iommu: Adding device"

# 查看设备绑定状态
readlink /sys/bus/pci/devices/0000:03:00.0/driver
```

### 6.2 IOMMU 调试

```bash
# 启用 IOMMU 调试日志
echo 1 > /sys/module/vfio_iommu_type1/parameters/unsafe_noiommu_mode 2>/dev/null

# 查看 IOMMU 页表（需要内核调试支持）
cat /sys/kernel/debug/iommu/intel/0/domains

# 检查 DMAR 表
acpidump | grep DMAR
```
