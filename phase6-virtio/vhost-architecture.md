# vhost 架构与核心数据结构

> Phase 6 深度主题 | 2026-11-25 事实核查重写（原版结构体/协作机制虚构，见 corrections.md）
> 源码基线：Linux 6.12.93 `drivers/vhost/`

---

## 📐 vhost 架构

### 整体架构

```
┌─ QEMU 用户态 ────────────────────────────────────────────┐
│  设备控制面（配置/管理）+ vhost 控制面（ioctl VHOST_*）    │
│  virtio 设备（virtqueue 在 guest 内存）                   │
└──────────────────────────┬───────────────────────────────┘
                           │ ioctl(VHOST_*) + eventfd
                           ▼
┌─ vhost 内核模块 ─────────────────────────────────────────┐
│  vhost 核心框架 (vhost.c)：vhost_dev / vhost_virtqueue /  │
│                            vhost_worker（per-device）     │
│  vhost-net (net.c)：网络数据面 + TAP/socket 集成 TX/RX    │
│  vhost-scsi (scsi.c) / vhost-vsock (vsock.c)              │
└──────────────────────────────────────────────────────────┘
```

### vhost 与 KVM 的协作（★ 经 eventfd，不直接调 KVM 符号）

**关键事实**：`grep -rn kvm drivers/vhost/*.c` **零命中**——vhost 不依赖 KVM 符号，
两者通过 **eventfd** 解耦协作：

```
中断（host→guest）：
  vhost_signal()（vhost.c:3036）→ eventfd_signal(vq->call_ctx.ctx)
    → KVM irqfd（virt/kvm/eventfd.c:49-51）→ kvm_set_irq() 注入 guest
  （vhost 只 signal eventfd，kvm_set_irq 是 KVM irqfd 侧调的，不是 vhost 直接调）

通知（guest→host）：
  guest kick → KVM ioeventfd（virt/kvm/eventfd.c）→ eventfd_signal
    → vhost poll 唤醒 → handle_kick（handle_tx_kick/handle_rx_kick）

guest 内存访问：
  走 vhost 自己的 vhost_iotlb，不是 KVM memslot：
  VHOST_SET_MEM_TABLE → vhost_set_memory()（vhost.c:1965）
    → vhost_iotlb_add_range()（vhost.c:2000）
  查表在 translate_desc()（vhost.c:1316/2624）内 vhost_iotlb_itree_first()
```

> QEMU 侧仍负责 KVM_CREATE_VM/VCPU、KVM_SET_USER_MEMORY_REGION、KVM_SET_GSI_ROUTING、
> 注册 irqfd/ioeventfd；vhost 侧只认 eventfd 与自己的 iotlb。详见 [annotations.md](annotations.md) §9。

---

## 🔧 vhost 核心数据结构（真实源码）

### 1. struct vhost_dev — vhost 设备（`drivers/vhost/vhost.h:174`）

```c
/* 来源: drivers/vhost/vhost.h:174-203（节选关键字段） */
struct vhost_dev {
    struct mm_struct *mm;             /* _owner_ 进程内存（非 guest） */
    struct mutex mutex;               /* 设备级锁（不是 control_lock/vq_mutex） */
    struct vhost_virtqueue **vqs;     /* virtqueue 数组 */
    int nvqs;                         /* virtqueue 数量（int，不是 long） */
    struct eventfd_ctx *log_ctx;      /* 脏页日志 eventfd */
    struct vhost_iotlb *umem;         /* 用户内存映射（GPA→HVA） */
    struct vhost_iotlb *iotlb;        /* IOMMU 映射（IOVA→HVA） */
    spinlock_t iotlb_lock;
    struct xarray worker_xa;          /* worker 管理（不是 worker_task 指针） */
    bool use_worker;
    bool fork_owner;                  /* true=vhost_task 继承 owner cgroup */
    int (*msg_handler)(struct vhost_dev *dev, u32 asid,
                       struct vhost_iotlb_msg *msg);
};
```

> ⚠️ 旧版文档写的 `control_lock`/`vq_mutex`/`worker_task`/`log_size`/`features`/`struct kvm *kvm`
> 字段在 6.12.93 **均不存在**（已更正）。vhost_dev 不持有 kvm 指针。

### 2. struct vhost_virtqueue — vhost 虚拟队列（`drivers/vhost/vhost.h:93`）

```c
/* 来源: drivers/vhost/vhost.h:93-163（节选关键字段） */
struct vhost_virtqueue {
    struct vhost_dev *dev;
    struct vhost_worker __rcu *worker;
    struct mutex mutex;
    unsigned int num;                 /* 队列深度 */
    vring_desc_t __user *desc;        /* 描述符表（guest 内存，__user 指针） */
    vring_avail_t __user *avail;      /* avail ring（guest 内存） */
    vring_used_t __user *used;        /* used ring（guest 内存） */
    struct file *kick;                /* guest→host 通知（ioeventfd） */
    struct vhost_vring_call call_ctx; /* host→guest 中断（irqfd，含 ctx） */
    struct eventfd_ctx *error_ctx;
    vhost_work_fn_t handle_kick;      /* kick 处理函数 */
    u16 last_avail_idx;               /* 上次处理的 avail 索引 */
    u16 last_used_idx;                /* 上次写入的 used 索引 */
    u16 signalled_used;               /* 上次通知 guest 的 used 索引 */
    struct iovec iov[UIO_MAXIOV];     /* 数据面 iov（建 iter 用） */
    struct vhost_iotlb *umem;         /* 地址翻译 */
    struct vhost_iotlb *iotlb;
    u64 acked_features;               /* 协商特性（在 vq 上，不是 dev） */
    bool is_le;                       /* ring 字节序 */
    u32 busyloop_timeout;
};
```

> ⚠️ 方向别搞反：`kick`（struct file*）是 **guest→host**；`call_ctx` 是 **host→guest** 中断。
> 旧版把 `call_ctx`/`kick_ctx` 方向写反、且虚构 `int index`/`struct vring desc`/`struct kvm_vcpu *vcpu`
> 字段（已更正）。ring 是 `__user` 指针（guest 内存），不是嵌入的 `struct vring`。

---

## 🔗 相关

- 数据面收发路径：[vhost-net-datapath.md](vhost-net-datapath.md)
- 逐行精读：[annotations.md](annotations.md) §1/§2/§9
- virtqueue 三环结构：[virtio-queue.md](virtio-queue.md)
