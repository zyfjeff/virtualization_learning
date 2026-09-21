# Phase 5：源码精读注释 - vhost 内核态加速

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。

---

## 1. vhost_dev：设备级控制块

### Q: `struct vhost_dev` 在 6.12.93 里长什么样？

**文件**: `drivers/vhost/vhost.h:174`

```c
struct vhost_dev {
    struct mm_struct *mm;              /* ★ 持有者(QEMU)的内存空间 */
    struct mutex mutex;                /* 设备级互斥锁 */
    struct vhost_virtqueue **vqs;      /* virtqueue 指针数组 */
    int nvqs;                          /* vq 数量 */
    struct eventfd_ctx *log_ctx;       /* 脏页日志 eventfd */
    struct vhost_iotlb *umem;         /* 用户空间内存映射 */
    struct vhost_iotlb *iotlb;        /* IOMMU 页表（有 IOMMU 时替代 umem） */
    spinlock_t iotlb_lock;
    struct list_head read_list;        /* 待读的 IOTLB 消息 */
    struct list_head pending_list;     /* 待处理的 IOTLB 消息 */
    wait_queue_head_t wait;            /* 等待队列 */
    int iov_limit;                     /* 单次 IO 最大 iov 数 */
    int weight;                        /* 每次调度最大包数 */
    int byte_weight;                   /* 每次调度最大字节数 */
    struct xarray worker_xa;           /* ★ 多 worker 线程管理（xarray） */
    bool use_worker;                   /* 是否使用工作线程 */
    bool fork_owner;                   /* true = vhost_task（继承 cgroups 等） */
                                       /* false = kthread（只继承 cgroups） */
    int (*msg_handler)(...);           /* ASID 消息回调 */
};
```

### Q: `worker_xa` 替代了什么旧机制？

旧版（< 5.x）只有一个全局 `work_list`（llist），对应一个 worker kthread。
6.12.93 用 **xarray** 管理多个 worker，每个 vq 可以绑定不同 worker：

| 旧版 | 6.12.93 |
|------|---------|
| 单 `work_list`（llist） | `worker_xa`（xarray），多 worker |
| 唯一 kthread | vhost_task 或 kthread 可选 |
| `use_mm()`/`unuse_mm()` | `kthread_use_mm()`/`kthread_unuse_mm()` |

### Q: `fork_owner` 为什么有两种选择？

```c
/* 来源: drivers/vhost/vhost.h:191 */
/*
 * If fork_owner is true we use vhost_tasks to create
 * the worker so all settings/limits like cgroups, NPROC,
 * scheduler, etc are inherited from the owner.
 * If false, we use kthreads and only attach to the same
 * cgroups as the owner for compat with older kernels.
 */
bool fork_owner;
```

| `fork_owner = true` | `fork_owner = false` |
|---|---|
| `vhost_task` | `kthread` |
| 继承 cgroups + NPROC + 调度策略 | 只附加 cgroups |
| 行为像 fork()，对资源限制可预测 | 兼容旧内核行为 |

默认值由模块参数 `fork_from_owner_default` 控制。

---

## 2. vhost_virtqueue：队列级状态

### Q: `struct vhost_virtqueue` 的核心字段？

**文件**: `drivers/vhost/vhost.h:93`

```c
struct vhost_virtqueue {
    struct vhost_dev *dev;
    struct vhost_worker __rcu *worker;    /* ★ 关联的 worker（RCU 保护） */

    struct mutex mutex;                    /* vq 级互斥锁 */
    unsigned int num;                      /* 队列大小 */

    /* ★ 三环的用户空间地址（不是内核虚拟地址！） */
    vring_desc_t  __user *desc;
    vring_avail_t __user *avail;
    vring_used_t  __user *used;

    struct file *kick;                     /* kick eventfd */
    struct vhost_vring_call call_ctx;      /* ★ call eventfd + irq_bypass producer */
    struct eventfd_ctx *error_ctx;
    struct eventfd_ctx *log_ctx;

    struct vhost_poll poll;                /* kick 通知的 poll 结构 */
    vhost_work_fn_t handle_kick;           /* ★ kick 回调（handle_tx / handle_rx） */

    u16 last_avail_idx;                    /* 上次处理到的 avail 索引 */
    u16 avail_idx;                         /* 缓存的 avail.idx */
    u16 last_used_idx;                     /* 上次写入的 used 索引 */
    u16 used_flags;                        /* used ring flags */

    /* ... iov/iotlb/log 等辅助字段省略 ... */

    u64 acked_features;                    /* 已协商的 feature bits */
    u64 acked_backend_features;            /* 已协商的 backend features */
    u32 busyloop_timeout;                  /* 忙等超时（μs） */
};
```

### Q: `call_ctx` 里为什么同时有 eventfd 和 irq_bypass_producer？

```c
/* 来源: drivers/vhost/vhost.h:87 */
struct vhost_vring_call {
    struct eventfd_ctx *ctx;                   /* 基础：eventfd 信号 */
    struct irq_bypass_producer producer;       /* ★ 优化：irq_bypass 配对 */
};
```

两个层次的中断注入：

| 路径 | 何时使用 | 延迟 |
|------|---------|------|
| `eventfd_signal(ctx)` | 通用路径，所有场景 | ~5μs（回用户空间） |
| `irq_bypass_producer` | VFIO 直通 + Posted Interrupts | ~0.5μs（硬件直投） |

当 QEMU 同时配置了 irqfd + VFIO 直通设备时，irq_bypass 框架把 producer（VFIO 注册）
和 consumer（KVM irqfd 注册）按 token（同一个 eventfd 上下文指针）配对：

```c
/* 来源: virt/lib/irqbypass.c:108 */
if (consumer->token == producer->token) {
    ret = __connect(producer, consumer);
    /* → kvm_arch_irq_bypass_add_producer() → vmx_pi_update_irte() */
    /*   → IRTE 写成 Posted 模式 */
}
```

**注意**：这走的是完全独立于 `KVM_DEV_VFIO_FILE_ADD` 的路径。详见 phase6 corrections.md 勘误 4。

---

## 3. vhost_dev_init()：初始化

### Q: 初始化做了什么？签名和旧版有何不同？

**文件**: `drivers/vhost/vhost.c:579`

```c
void vhost_dev_init(struct vhost_dev *dev,
                    struct vhost_virtqueue **vqs, int nvqs,
                    int iov_limit, int weight, int byte_weight,
                    bool use_worker,
                    int (*msg_handler)(struct vhost_dev *dev, u32 asid,
                                       struct vhost_iotlb_msg *msg))
{
    /* ★ 基础字段 */
    dev->vqs = vqs;
    dev->nvqs = nvqs;
    mutex_init(&dev->mutex);
    dev->iov_limit = iov_limit;
    dev->weight = weight;          /* 每次调度最大包数 */
    dev->byte_weight = byte_weight;
    dev->use_worker = use_worker;
    dev->fork_owner = fork_from_owner_default;

    xa_init_flags(&dev->worker_xa, XA_FLAGS_ALLOC);  /* ★ xarray 管理 worker */

    /* ★ 初始化每个 vq */
    for (i = 0; i < dev->nvqs; ++i) {
        vq = dev->vqs[i];
        vq->dev = dev;
        mutex_init(&vq->mutex);
        vhost_vq_reset(dev, vq);
        if (vq->handle_kick)
            vhost_poll_init(&vq->poll, vq->handle_kick,
                            EPOLLIN, dev, vq);   /* ★ 注册 kick 的 poll 回调 */
    }
}
```

### Q: `vhost_dev_init` 签名变化对比？

| 版本 | 签名 |
|------|------|
| 旧版 (< 6.x) | `(dev, vqs, nvqs, iov_limit, lock_limit, name, use_worker, poll)` |
| **6.12.93** | `(dev, vqs, nvqs, iov_limit, weight, byte_weight, use_worker, msg_handler)` |

变化要点：
- `lock_limit` 和 `name` 被移除
- 新增 `weight`、`byte_weight`（调度权重限制）
- `poll` 回调改为 `msg_handler`（ASID 消息处理）
- `vhost_dev_init` **不创建 worker 线程** — worker 在 `vhost_dev_set_owner()` 时创建

---

## 4. Worker 线程：新旧架构

### Q: 6.12.93 的 worker 架构长什么样？

```
新架构 (6.12.93):

  vhost_dev
    └── worker_xa (xarray)
         ├── worker[0] (vhost_task 或 kthread)
         │    ├── work_list (llist_head)
         │    ├── mutex (序列化 flush)
         │    ├── kcov_handle
         │    └── id, attachment_cnt, killed
         ├── worker[1]
         │    └── ...
         └── worker[N]
              └── ...
    每个 vq 通过 vq->worker (RCU) 指向一个 worker
```

**文件**: `drivers/vhost/vhost.h:39`

```c
struct vhost_worker {
    struct task_struct *kthread_task;     /* kthread 模式的任务 */
    struct vhost_task *vtsk;              /* vhost_task 模式的封装 */
    struct vhost_dev *dev;
    struct mutex mutex;                   /* 序列化 flush */
    struct llist_head work_list;          /* ★ 无锁工作队列 */
    u64 kcov_handle;
    u32 id;
    int attachment_cnt;
    bool killed;
    const struct vhost_worker_ops *ops;
};
```

### Q: vhost_task 模式和 kthread 模式的区别？

**文件**: `drivers/vhost/vhost.c:437`（vhost_task 模式）

```c
static bool vhost_run_work_list(void *data)
{
    struct vhost_worker *worker = data;
    struct llist_node *node;

    node = llist_del_all(&worker->work_list);  /* ★ 原子取走全部工作 */
    if (node) {
        __set_current_state(TASK_RUNNING);
        node = llist_reverse_order(node);       /* 反转 → FIFO 顺序 */
        smp_wmb();
        llist_for_each_entry_safe(work, work_next, node, node) {
            clear_bit(VHOST_WORK_QUEUED, &work->flags);
            work->fn(work);                     /* ★ 执行工作函数 */
            cond_resched();
        }
    }
    return !!node;
}
```

**文件**: `drivers/vhost/vhost.c:400`（kthread 模式）

```c
static int vhost_run_work_kthread_list(void *data)
{
    struct vhost_worker *worker = data;
    kthread_use_mm(dev->mm);                   /* ★ 借用 QEMU 的内存空间 */

    for (;;) {
        set_current_state(TASK_INTERRUPTIBLE);
        if (kthread_should_stop()) break;

        node = llist_del_all(&worker->work_list);
        if (!node)
            schedule();                         /* 无工作 → 睡眠 */

        /* 处理工作（同 vhost_run_work_list） */
        ...
    }

    kthread_unuse_mm(dev->mm);
    return 0;
}
```

| | kthread 模式 | vhost_task 模式 |
|---|---|---|
| 生命周期 | `kthread_create` + `kthread_stop` | `vhost_task_create` + `vhost_task_stop` |
| 内存空间 | `kthread_use_mm()` 显式借用 | `vhost_task` 内部自动管理 |
| 资源继承 | 只继承 cgroups | 继承 cgroups + NPROC + 调度策略 |
| 工作处理 | 自含 for 循环 + schedule | 外层循环由 vhost_task 框架驱动 |

---

## 5. vhost_get_vq_desc()：读取 avail ring

### Q: 从 avail ring 到 iovec，经过了哪些步骤？

**文件**: `drivers/vhost/vhost.c:2786`

```c
int vhost_get_vq_desc(struct vhost_virtqueue *vq,
                      struct iovec iov[], unsigned int iov_size,
                      unsigned int *out_num, unsigned int *in_num,
                      struct vhost_log *log, unsigned int *log_num)
{
    /* ★ Step 1: 检查 avail ring 是否有新描述符 */
    if (vq->avail_idx == vq->last_avail_idx) {
        ret = vhost_get_avail_idx(vq);       /* 从用户空间读 avail.idx */
        if (!ret) return vq->num;            /* 无新描述符 */
    }

    /* ★ Step 2: 读 avail ring 中的描述符索引 */
    vhost_get_avail_head(vq, &ring_head, last_avail_idx);
    head = vhost16_to_cpu(vq, ring_head);

    /* ★ Step 3: 遍历描述符链 */
    i = head;
    do {
        /* 安全检查 */
        if (unlikely(i >= vq->num))   return -EINVAL;
        if (unlikely(++found > vq->num)) return -EINVAL;  /* 循环检测 */

        /* 读描述符（从用户空间拷贝） */
        ret = vhost_get_desc(vq, &desc, i);

        /* 间接描述符 */
        if (desc.flags & VRING_DESC_F_INDIRECT) {
            ret = get_indirect(vq, iov, ...);
            continue;
        }

        /* ★ 地址翻译: Guest 地址 → 宿主用户空间地址 */
        ret = translate_desc(vq, desc.addr, desc.len,
                             iov + iov_count, iov_size - iov_count,
                             access);

        /* 分类: 输出(RO) vs 输入(WO) */
        if (access == VHOST_ACCESS_WO) *in_num += ret;
        else                           *out_num += ret;

    } while ((i = next_desc(vq, &desc)) != -1);

    vq->last_avail_idx++;
    return head;     /* ★ 返回描述符链的头索引，供 vhost_add_used() 使用 */
}
```

### Q: 为什么返回值是 `int` 而非 `unsigned`？

旧版返回 `unsigned`，用 `vq->num` 表示「无描述符」。6.12.93 返回 `int`，可以是负错误码：

| 返回值 | 含义 |
|--------|------|
| `>= 0` | 描述符链头索引 |
| `vq->num` | 无新描述符（仍为非负） |
| `-EFAULT` | 用户空间访问失败 |
| `-EINVAL` | 描述符索引越界或循环 |
| `-EAGAIN` | IOTLB miss，需等待翻译 |
| `-EPERM` | 权限不匹配 |

### Q: `translate_desc()` 做了什么？

**文件**: `drivers/vhost/vhost.c:2624`

```c
static int translate_desc(struct vhost_virtqueue *vq, u64 addr, u32 len,
                          struct iovec iov[], int iov_size, int access)
{
    struct vhost_iotlb *umem = dev->iotlb ? dev->iotlb : dev->umem;

    while ((u64)len > s) {
        /* ★ 在 IOTLB 中查找覆盖 [addr, addr+size) 的映射 */
        map = vhost_iotlb_itree_first(umem, addr, last);
        if (!map || map->start > addr) {
            if (umem != dev->iotlb)
                ret = -EFAULT;        /* umem 模式：直接失败 */
            else
                ret = -EAGAIN;        /* iotlb 模式：触发 IOTLB miss */
            break;
        }

        /* ★ 权限检查 */
        if (!(map->perm & access)) { ret = -EPERM; break; }

        /* 生成 iov：用户空间虚拟地址（HVA） */
        _iov->iov_base = (void __user *)(map->addr + addr - map->start);
        _iov->iov_len  = min(len - s, map->size - addr + map->start);
        ...
    }

    if (ret == -EAGAIN)
        vhost_iotlb_miss(vq, addr, access);  /* 通知 QEMU 更新 IOTLB */
    return ret;
}
```

**关键理解**：vhost 在内核态运行，但访问的是 QEMU 用户空间映射的 Guest 内存。
`translate_desc()` 通过 IOTLB（QEMU 通过 `VHOST_SET_VRING_ADDR` 配置）把 Guest 地址
翻译成宿主的用户空间地址（HVA），然后用 `copy_to_user()` / `copy_from_user()` 访问。

---

## 6. vhost_add_used()：写入 used ring

### Q: 写 used ring 的完整流程？

**文件**: `drivers/vhost/vhost.c:2915`

```c
int vhost_add_used(struct vhost_virtqueue *vq, unsigned int head, int len)
{
    struct vring_used_elem heads = {
        cpu_to_vhost32(vq, head),
        cpu_to_vhost32(vq, len)
    };
    return vhost_add_used_n(vq, &heads, 1);   /* 批量写入的通用入口 */
}
```

**文件**: `drivers/vhost/vhost.c:2926`

```c
static int __vhost_add_used_n(struct vhost_virtqueue *vq,
                              struct vring_used_elem *heads, unsigned count)
{
    start = vq->last_used_idx & (vq->num - 1);
    used = vq->used->ring + start;

    /* ★ 写入 used ring（通过 copy_to_user） */
    if (vhost_put_used(vq, heads, start, count))
        return -EFAULT;

    /* ★ 脏页日志（迁移时需要） */
    if (unlikely(vq->log_used)) {
        smp_wmb();          /* 数据先于日志 */
        log_used(vq, ...);
    }

    /* 更新 last_used_idx */
    old = vq->last_used_idx;
    new = (vq->last_used_idx += count);

    /* 回绕保护：如果 idx 绕过了 signalled_used，标记失效 */
    if (unlikely((u16)(new - vq->signalled_used) < (u16)(new - old)))
        vq->signalled_used_valid = false;

    return 0;
}
```

### Q: `vhost_signal()` 何时触发？

写完 used ring 后，需要通知 Guest。vhost 用 eventfd 通知：

```c
/* 简化逻辑 */
void vhost_signal(struct vhost_dev *dev, struct vhost_virtqueue *vq)
{
    /* 检查 Guest 是否屏蔽了通知 */
    if (vq->used_flags & VRING_USED_F_NO_NOTIFY)
        return;

    /* Event Index 优化：只在需要时通知 */
    if (vhost_has_feature(vq, VIRTIO_RING_F_EVENT_IDX)) {
        if (!vhost_need_event(vhost16_to_cpu(vq, vring_used_event(&vq->vring)),
                              vq->last_used_idx, vq->last_used_idx - count))
            return;
    }

    /* ★ 通过 eventfd 通知（6.12.93 只有 1 个参数） */
    eventfd_signal(vq->call_ctx.ctx);
}
```

---

## 7. vhost-net：数据面

### Q: `struct vhost_net` 包含什么？

**文件**: `drivers/vhost/net.c:133`

```c
struct vhost_net {
    struct vhost_dev dev;                          /* ★ 继承 vhost_dev */
    struct vhost_net_virtqueue vqs[VHOST_NET_VQ_MAX]; /* TX + RX 两个队列 */
    struct vhost_poll poll[VHOST_NET_VQ_MAX];      /* socket 可读通知 */
    unsigned tx_packets;        /* 最近发送的包数 */
    unsigned tx_zcopy_err;      /* 零拷贝失败次数 */
    bool tx_flush;              /* 零拷贝刷新进行中 */
    struct page_frag_cache pf_cache;  /* 页面碎片缓存 */
};
```

### Q: `handle_tx()` 做什么？

**文件**: `drivers/vhost/net.c:945`

```c
static void handle_tx(struct vhost_net *net)
{
    struct vhost_virtqueue *vq = &net->vqs[VHOST_NET_VQ_TX].vq;
    struct socket *sock;

    mutex_lock_nested(&vq->mutex, VHOST_NET_VQ_TX);
    sock = vhost_vq_get_backend(vq);   /* ★ 后端是 TAP socket */
    if (!sock) goto out;

    vhost_disable_notify(&net->dev, vq);  /* 禁用通知，减少 VM-Exit */

    if (vhost_sock_zcopy(sock))
        handle_tx_zerocopy(net, sock);    /* 零拷贝：不拷贝数据 */
    else
        handle_tx_copy(net, sock);        /* 拷贝模式 */

out:
    mutex_unlock(&vq->mutex);
}
```

### Q: TX 路径的两种模式有什么区别？

| 模式 | 触发条件 | 行为 | 性能 |
|------|---------|------|------|
| `handle_tx_copy` | 默认 | `copy_to_iter()` 拷贝到 skb，`sock_sendmsg()` 发送 | 简单可靠 |
| `handle_tx_zerocopy` | `VHOST_NET_USE_VHOST_NET_ZCOPY` + 支持 | 页面直接挂到 skb（`skb_zerocopy_realloc`），需要等待 DMA 完成才能释放 | 高吞吐但复杂 |

### Q: `handle_rx()` 的批处理机制？

**文件**: `drivers/vhost/net.c:1092`

```c
static void handle_rx(struct vhost_net *net)
{
    struct vhost_net_virtqueue *nvq = &net->vqs[VHOST_NET_VQ_RX];
    struct vhost_virtqueue *vq = &nvq->vq;

    /* ★ 从 TAP socket 收包到 vhost_net_buf（批量） */
    vhost_net_buf_produce(nvq);    /* ptr_ring_consume_batched → 批量取 skb */

    /* 逐个分发给 Guest */
    while (!vhost_net_buf_is_empty(&nvq->rxq)) {
        /* 从描述符链获取 buffer */
        head = vhost_get_vq_desc(vq, vq->iov, ...);

        /* 拷贝数据到 Guest buffer */
        skb = vhost_net_buf_consume(&nvq->rxq);
        /* copy_to_iter / zerocopy 到描述符指向的 Guest 内存 */

        /* 写入 used ring */
        vhost_add_used(vq, head, len);

        /* 检查是否达到调度权重限制 */
        if (vhost_exceeds_weight(vq, ++pkts, total_len))
            break;
    }
}
```

---

## 8. vhost ioctl 接口

### Q: vhost 支持哪些 ioctl？

**文件**: `drivers/vhost/vhost.c` — `vhost_vring_ioctl()`

| ioctl | 作用 |
|-------|------|
| `VHOST_GET_FEATURES` | 返回 vhost 支持的 feature bits |
| `VHOST_SET_FEATURES` | 协商 features（Guest 和 vhost 都支持的） |
| `VHOST_SET_OWNER` | 设置持有者（绑定 mm，创建 worker） |
| `VHOST_RESET_OWNER` | 重置持有者 |
| `VHOST_SET_VRING_NUM` | 设置 virtqueue 大小 |
| `VHOST_SET_VRING_ADDR` | ★ 设置 desc/avail/used 地址 + IOTLB |
| `VHOST_SET_VRING_BASE` | 设置 avail/used 索引起点 |
| `VHOST_GET_VRING_BASE` | 获取当前索引（迁移用） |
| `VHOST_SET_VRING_KICK` | ★ 设置 kick eventfd（Guest→vhost 通知） |
| `VHOST_SET_VRING_CALL` | ★ 设置 call eventfd（vhost→Guest 通知） |
| `VHOST_SET_VRING_ERR` | 设置 error eventfd |
| `VHOST_SET_BACKEND_FEATURES` | 协商 backend features（IOTLB、双缓冲等） |

### Q: `VHOST_SET_VRING_KICK` 做了什么？

```c
/* 简化逻辑 */
case VHOST_SET_VRING_KICK:
    /* 获取用户传入的 eventfd */
    fd = ...;
    kick = eventfd_ctx_fdget(fd);

    /* 替换旧的 kick */
    old_kick = vq->kick;
    vq->kick = kick;

    /* 如果设了 kick，启动 poll 监听 */
    if (kick)
        vhost_poll_start(&vq->poll, kick_file);

    /* 释放旧的 */
    if (old_kick)
        eventfd_ctx_put(old_kick);
```

当 Guest 写 avail.idx 触发 VM-Exit → KVM 检测到 ioeventfd 匹配 →
直接唤醒 vhost worker 线程，不需要回 QEMU。

---

## 9. vhost 与 KVM 的协作

### Q: vhost 如何避免 VM-Exit 回 QEMU？

```
传统用户态 virtio（每个包 2 次 VM-Exit）:

  Guest kick → VM-Exit → KVM → QEMU → 处理 → eventfd → VM-Entry

vhost 内核态（0 次 VM-Exit 用于数据处理）:

  Guest kick → VM-Exit → KVM ioeventfd → 唤醒 vhost worker
  vhost worker（内核态）:
    1. vhost_get_vq_desc() — 读描述符
    2. translate_desc() — 地址翻译
    3. 处理数据（handle_tx/handle_rx）
    4. vhost_add_used() — 写 used ring
    5. eventfd_signal(call_ctx) — 通知 Guest
```

### Q: 中断注入的两条路径？

| 路径 | 机制 | 场景 |
|------|------|------|
| eventfd → KVM | `eventfd_signal()` → irqfd → `kvm_set_irq()` | 通用 |
| Posted Interrupts | irq_bypass 配对 → IRTE 写 Posted → 硬件直投 | VFIO 直通 |

**通用路径详解**：

```
vhost 写 used ring 完成
    ↓
eventfd_signal(call_ctx.ctx)
    ↓
irqfd 的 wake_function 被调用
    ↓
kvm_set_irq() → KVM 中断注入
    ↓
Guest 收到中断 → 处理 completed buffer
```

---

## 10. 完整调用链

```
QEMU 设置 vhost-net:
  │
  ├─ open("/dev/vhost-net")
  │   └→ vhost_net_open()
  │      └→ vhost_dev_init(dev, vqs, 2, ...)    ← 初始化 2 个队列(TX+RX)
  │
  ├─ ioctl(VHOST_SET_OWNER)
  │   └→ vhost_dev_set_owner()
  │      └→ 创建 worker 线程（vhost_task 或 kthread）
  │      └→ dev->mm = current->mm               ← 继承 QEMU 的内存空间
  │
  ├─ ioctl(VHOST_SET_FEATURES)
  │   └→ vhost_set_features()
  │      └→ dev->acked_features = features
  │
  ├─ ioctl(VHOST_NET_SET_BACKEND)               ← vhost-net 特有
  │   └→ 关联 TAP socket 到 vq->private_data
  │
  ├─ ioctl(VHOST_SET_VRING_NUM/ADDR/BASE/KICK/CALL)
  │   └→ vhost_vring_ioctl()
  │      └→ 配置每个 vq 的参数
  │
  └─ VM 开始运行
      │
      ├─ Guest 写 avail.idx
      │   └→ VM-Exit → KVM ioeventfd → 唤醒 vhost worker
      │
      ├─ vhost worker 线程:
      │   └→ handle_tx() / handle_rx()           ← vq->handle_kick 回调
      │      ├→ vhost_get_vq_desc()
      │      │   ├→ vhost_get_avail_idx()         ← 读 avail.idx
      │      │   ├→ vhost_get_avail_head()         ← 读 avail ring
      │      │   ├→ vhost_get_desc()               ← 读描述符
      │      │   └→ translate_desc()               ← GPA → HVA
      │      │
      │      ├→ handle_tx_copy() / handle_tx_zerocopy()
      │      │   └→ sock_sendmsg()                 ← 发到 TAP
      │      │
      │      ├→ vhost_add_used()                   ← 写 used ring
      │      │   └→ vhost_put_used()               ← copy_to_user
      │      │
      │      └→ vhost_signal()                     ← 通知 Guest
      │          └→ eventfd_signal(call_ctx)        ← 6.12.93: 无 n 参数
      │              └→ irqfd → kvm_set_irq() → Guest 收到中断
      │
      └─ 循环直到 QEMU 关闭设备
```
