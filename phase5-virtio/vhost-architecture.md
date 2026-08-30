# vhost 架构与核心数据结构

> Phase 5 深度主题 | 从 README.md 拆出，正文内容未作改动

---

## 📐 vhost架构

### 整体架构

```
┌─ QEMU用户态 ─────────────────────────────────────────────┐
│                                                           │
│  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐   │
│  │ 设备控制面  │  │ vhost控制面   │  │ virtio设备   │   │
│  │ (配置/管理) │  │ (ioctl调用)   │  │ (virtqueue)  │   │
│  └──────┬──────┘  └──────┬───────┘  └──────┬───────┘   │
│         │                │                  │            │
│         └────────────────┼──────────────────┘            │
│                          │                               │
│              ioctl(VHOST_*)                               │
│                          │                               │
└──────────────────────────┼───────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────┐
│              vhost内核模块                                 │
│                                                           │
│  ┌────────────────────────────────────────────────────┐ │
│  │  vhost核心框架 (vhost.c)                           │ │
│  │  ├── vhost_dev: vhost设备结构                      │ │
│  │  ├── vhost_virtqueue: vhost虚拟队列                │ │
│  │  └── vhost_worker: vhost工作线程                   │ │
│  └────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌────────────────────────────────────────────────────┐ │
│  │  vhost-net (net.c)                                 │ │
│  │  ├── 网络数据面处理                                │ │
│  │  ├── TAP设备集成                                   │ │
│  │  └── TX/RX路径                                     │ │
│  └────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌────────────────────────────────────────────────────┐ │
│  │  vhost-scsi (scsi.c)                               │ │
│  │  ├── SCSI命令处理                                  │ │
│  │  └── 块设备集成                                    │ │
│  └────────────────────────────────────────────────────┘ │
│                                                           │
│  ┌────────────────────────────────────────────────────┐ │
│  │  vhost-vsock (vsock.c)                             │ │
│  │  └── VM间通信                                      │ │
│  └────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

### vhost与KVM的协作

```
vhost与KVM协作:

┌─ QEMU ───────────────────────────────────────────────────┐
│  1. 创建VM: ioctl(KVM_CREATE_VM)                        │
│  2. 创建vCPU: ioctl(KVM_CREATE_VCPU)                    │
│  3. 创建vhost-net: ioctl(VHOST_NET_SET_BACKEND)         │
│  4. 关联vhost到VM: ioctl(KVM_SET_USER_MEMORY_REGION)    │
│  5. 设置中断路由: ioctl(KVM_SET_GSI_ROUTING)            │
└──────────────────────────────────────────────────────────┘

┌─ KVM ────────────────────────────────────────────────────┐
│  ├── struct kvm: VM实例                                  │
│  ├── struct kvm_vcpu: 虚拟CPU                            │
│  └── struct kvm_memory_slot: 内存slot                    │
└──────────────────────────────────────────────────────────┘
         │
         │ vhost访问KVM资源
         │
┌─ vhost ──────────────────────────────────────────────────┐
│  ├── 访问Guest内存: 通过KVM的memslot (GPA→HVA→HPA)     │
│  ├── 注入中断: 直接调用kvm_set_irq() (无需ioctl!)       │
│  ├── 唤醒vCPU: 直接调用kvm_vcpu_kick()                  │
│  └→ 全程内核态，无系统调用!                              │
└──────────────────────────────────────────────────────────┘
```

---

## 🔧 vhost核心数据结构

### 1. struct vhost_dev - vhost设备

```c
/* 来源: drivers/vhost/vhost.h */

struct vhost_dev {
    struct mm_struct *mm;             /* 用户空间内存映射 */
    struct mutex control_lock;        /* 控制面锁 */
    struct mutex vq_mutex;            /* virtqueue锁 */
    
    /* === virtqueue管理 === */
    long nvqs;                        /* virtqueue数量 */
    struct vhost_virtqueue **vqs;     /* virtqueue数组 */
    
    /* === 工作线程 === */
    struct vhost_worker *worker;      /* vhost工作线程 */
    struct task_struct *worker_task;  /* 工作线程task_struct */
    
    /* === 日志 === */
    struct vhost_log *log;            /* 脏页日志 */
    u64 log_size;                     /* 日志大小 */
    
    /* === 特性 === */
    u64 features;                     /* 协商的特性 */
    
    /* === KVM关联 === */
    struct kvm *kvm;                  /* 关联的KVM VM */
    
    /* ... 省略其他字段 ... */
};
```

### 2. struct vhost_virtqueue - vhost虚拟队列

```c
/* 来源: drivers/vhost/vhost.h */

struct vhost_virtqueue {
    struct vhost_dev *dev;            /* 所属vhost设备 */
    
    /* === virtqueue索引 === */
    int index;                        /* virtqueue索引 (0=RX, 1=TX) */
    
    /* === virtio ring === */
    struct vring desc;                /* 描述符表 (Guest内存) */
    struct vring avail;               /* avail ring (Guest内存) */
    struct vring used;                /* used ring (Guest内存) */
    
    /* === 状态 === */
    u16 last_avail_idx;               /* 上次处理的avail索引 */
    u16 last_used_idx;                /* 上次写入的used索引 */
    bool signalled_used;              /* 是否已通知Guest */
    
    /* === 通知 === */
    struct eventfd_ctx *call_ctx;     /* Guest→Host通知 (kick) */
    struct eventfd_ctx *kick_ctx;     /* Host→Guest通知 (中断) */
    
    /* === 工作队列 === */
    struct llist_head work_list;      /* 待处理工作列表 */
    
    /* === KVM中断注入 === */
    struct kvm_vcpu *vcpu;            /* 关联的vCPU (用于中断注入) */
    
    /* ... 省略其他字段 ... */
};
```

---
