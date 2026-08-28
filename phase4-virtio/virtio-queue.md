# Virtio Queue 深度解析

> Phase 4 深度主题 | 从 README.md 拆出，正文内容未作改动

---

## 🔧 Virtio Queue 基础

### 核心概念

Virtio Queue 是 virtio 规范定义的设备与驱动之间的通信机制。它使用**环形缓冲区（ring buffer）**实现无锁的高效数据传输。

```
关键特性:
├── 无锁设计: 通过内存屏障和原子操作实现
├── 异步处理: 驱动和设备可以独立操作
├── 批量处理: 一次可以提交多个请求
└── 双向通信: avail ring (驱动→设备) + used ring (设备→驱动)
```

### 核心数据结构

Virtio Queue 由三个核心部分组成：

```
┌─ Virtio Queue 结构 ───────────────────────────────────────┐
│                                                            │
│  Guest 内存空间 (GPA)                                      │
│  ┌─────────────────────────────────────────────────────┐  │
│  │                                                     │  │
│  │  ┌─────────────────┐                               │  │
│  │  │ Descriptor Table│  描述符表 (16 bytes/entry)    │  │
│  │  │  [0][1][2]...   │  每个描述符指向一块内存       │  │
│  │  └─────────────────┘                               │  │
│  │         ↓                                          │  │
│  │  ┌─────────────────┐                               │  │
│  │  │   Avail Ring    │  可用环 (驱动→设备)           │  │
│  │  │  [idx][ring]    │  驱动告诉设备"哪些描述符可用" │  │
│  │  └─────────────────┘                               │  │
│  │         ↓                                          │  │
│  │  ┌─────────────────┐                               │  │
│  │  │   Used Ring     │  已用环 (设备→驱动)           │  │
│  │  │  [idx][ring]    │  设备告诉驱动"哪些描述符已处理"│  │
│  │  └─────────────────┘                               │  │
│  │                                                     │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                            │
│  关键指针:                                                 │
│  ├── avail.idx: 驱动递增, 设备读取                        │
│  ├── used.idx: 设备递增, 驱动读取                         │
│  └── desc.next: 描述符链式连接                            │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### Descriptor Table (描述符表)

描述符表是 virtio queue 的核心，每个描述符描述一块内存区域。

```c
/* include/uapi/linux/virtio_ring.h */

struct vring_desc {
    __u64 addr;      /* 物理地址 (Guest 物理地址 GPA) */
    __u32 len;       /* 长度 */
    __u16 flags;     /* 标志位 */
    __u16 next;      /* 下一个描述符索引 (如果 flags 有 NEXT 标志) */
};

/* 标志位定义 */
#define VRING_DESC_F_NEXT     1  /* 有下一个描述符 (链式) */
#define VRING_DESC_F_WRITE    2  /* 设备可写 (否则只读) */
#define VRING_DESC_F_INDIRECT 4  /* 指向间接描述符表 */
```

```
描述符结构 (16 bytes):
┌─────────────────────────────────────────────────────────┐
│  addr (8 bytes)   │  len (4 bytes)  │ flags (2) │ next  │
│  Guest 物理地址   │  缓冲区长度     │ 标志位    │ 下一个│
└─────────────────────────────────────────────────────────┘

使用场景:
├── 简单场景: 单个描述符指向一块连续内存
├── 链式描述符: NEXT 标志链接多个描述符 (scatter-gather)
└── 间接描述符: INDIRECT 标志指向另一个描述符表 (减少 avail ring 占用)
```

### Avail Ring (可用环)

驱动通过 avail ring 告诉设备"哪些描述符准备好了"。

```c
/* include/uapi/linux/virtio_ring.h */

struct vring_avail {
    __u16 flags;     /* 标志位 */
    __u16 idx;       /* 索引 (递增) */
    __u16 ring[];    /* 描述符索引数组 */
};

/* 实际布局 (对齐后) */
struct vring {
    unsigned int num;           /* 队列大小 */
    struct vring_desc *desc;    /* 描述符表 */
    struct vring_avail *avail;  /* 可用环 */
    struct vring_used *used;    /* 已用环 */
};
```

```
Avail Ring 结构:
┌─────────────────────────────────────────────────────────┐
│  flags (2 bytes)  │  idx (2 bytes)  │ ring[0..num-1]    │
│  标志位           │  当前索引       │ 描述符索引数组    │
└─────────────────────────────────────────────────────────┘

数据流 (驱动 → 设备):
  1. 驱动准备描述符 (填充 desc table)
  2. 驱动将描述符索引写入 avail.ring[idx % num]
  3. 驱动递增 avail.idx
  4. 驱动发送 kick 通知设备
  5. 设备读取 avail.ring 和对应的描述符
```

### Used Ring (已用环)

设备通过 used ring 告诉驱动"哪些描述符已经处理完了"。

```c
/* include/uapi/linux/virtio_ring.h */

struct vring_used_elem {
    __u32 id;      /* 描述符链的起始索引 */
    __u32 len;     /* 设备实际写入的长度 (对于 WRITE 描述符) */
};

struct vring_used {
    __u16 flags;     /* 标志位 */
    __u16 idx;       /* 索引 (递增) */
    struct vring_used_elem ring[];  /* 已处理的描述符 */
};
```

```
Used Ring 结构:
┌─────────────────────────────────────────────────────────────────┐
│  flags (2 bytes)  │  idx (2 bytes)  │ ring[0..num-1]            │
│  标志位           │  当前索引       │ {id, len} 数组            │
└─────────────────────────────────────────────────────────────────┘

数据流 (设备 → 驱动):
  1. 设备处理完描述符链
  2. 设备将 {id, len} 写入 used.ring[idx % num]
  3. 设备递增 used.idx
  4. 设备发送 interrupt 通知驱动
  5. 驱动读取 used.ring 并释放描述符
```

### Kick/Notify 机制

Kick 和 Notify 是 virtio 的中断机制，用于通知对方"有新的数据"。

```
┌─ Kick/Notify 机制 ──────────────────────────────────────────┐
│                                                               │
│  驱动 → 设备 (Kick):                                         │
│    · 驱动写 avail ring 后, 写设备寄存器                      │
│    · 触发 VM-Exit (IO/MMIO 访问)                             │
│    · KVM 处理 VM-Exit, 通知设备 (vhost 或 QEMU)              │
│                                                               │
│  设备 → 驱动 (Notify/Interrupt):                             │
│    · 设备写 used ring 后, 发送中断                           │
│    · vhost: 直接调用 kvm_set_irq() 注入中断                  │
│    · QEMU: 通过 eventfd 通知 KVM 注入中断                   │
│                                                               │
│  关键优化:                                                     │
│    · Event Index: 避免不必要的 kick/interrupt                │
│    · 通过 avail_event 和 used_event 精确控制通知时机         │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### 完整数据流示例 (网络包发送)

```
┌─ TX 路径 (Guest 发送网络包) ───────────────────────────────┐
│                                                              │
│  Guest 侧:                                                  │
│    1. virtio-net 驱动准备网络包                              │
│    2. 分配描述符:                                            │
│       desc[0]: 包头 (14 bytes)                              │
│       desc[1]: 包体 (1500 bytes)                            │
│       desc[0].next = 1, desc[0].flags = NEXT                │
│    3. 写入 avail ring:                                       │
│       avail.ring[avail.idx % num] = 0                       │
│       avail.idx++                                           │
│    4. Kick 设备 (写寄存器, 触发 VM-Exit)                    │
│                                                              │
│  Host 侧 (vhost):                                           │
│    5. vhost 线程被唤醒                                       │
│    6. 读取 avail ring:                                       │
│       desc_idx = avail.ring[old_idx % num]                  │
│    7. 读取描述符链:                                          │
│       desc[0] → desc[1] (通过 next 链接)                    │
│    8. 从 Guest 内存读取数据 (GPA→HVA→读取)                 │
│    9. 发送到 TAP 设备 (sock_sendmsg)                        │
│    10. 写入 used ring:                                       │
│        used.ring[used.idx % num] = {id: 0, len: 0}         │
│        used.idx++                                          │
│    11. 注入中断到 Guest (kvm_set_irq)                       │
│                                                              │
│  Guest 侧 (完成):                                            │
│    12. 收到中断, 读取 used ring                              │
│    13. 释放描述符, 可以重用                                  │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 队列大小和对齐

```
队列大小 (num):
  · 通常是 2 的幂次 (128, 256, 512, 1024)
  · 由设备能力决定 (VIRTIO_NET_F_MRG_RXBUF 等)
  · 越大: 可以缓冲更多请求, 但占用更多内存
  · 越小: 内存占用少, 但可能频繁阻塞

内存对齐:
  · 描述符表: 16 字节对齐
  · Avail ring: 2 字节对齐
  · Used ring: 4 字节对齐
  · 整个 vring: 按页对齐 (4KB)
```

### 关键优化技术

```
┌─ 优化技术 ──────────────────────────────────────────────────┐
│                                                               │
│  1. 间接描述符 (Indirect Descriptors):                       │
│     · 一个描述符指向另一个描述符表                           │
│     · 减少 avail ring 的占用                                │
│     · 适合 scatter-gather I/O                               │
│                                                               │
│  2. Event Index:                                              │
│     · 精确控制通知时机                                      │
│     · 避免不必要的 kick/interrupt                           │
│     · 通过 avail_event 和 used_event 实现                   │
│                                                               │
│  3. 批量处理:                                                 │
│     · 一次 kick 可以提交多个描述符                          │
│     · 一次 interrupt 可以通知多个完成                        │
│     · 减少通知开销                                          │
│                                                               │
│  4. 内存屏障:                                                 │
│     · virtio_mb() / virtio_rmb() / virtio_wmb()             │
│     · 确保内存访问顺序正确                                  │
│     · 多核环境下至关重要                                    │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### 高级特性详解

#### Queue Number vs Queue Size

```
Queue Number (队列数量):
  · 一个 Virtio 设备可以有多个队列
  · 例如: virtio-net 可以有多对 TX/RX 队列
  · 每个队列独立运作，支持多队列并行处理
  · 提高并发性能，充分利用多核 CPU

Queue Size (队列大小):
  · 单个队列中可以容纳的描述符数量
  · 必须是 2 的幂次 (128, 256, 512, 1024, 2048)
  · 由设备能力和驱动协商决定
  · 影响内存占用和性能

关系:
  · Queue Number 决定并行度
  · Queue Size 决定单个队列的容量
  · 总描述符数 = Queue Number × Queue Size
```

#### Queue 大小计算

```
一个 Virtio Queue 的内存布局:

┌─ Split Queue (传统模式) ────────────────────────────────────┐
│                                                               │
│  1. 描述符表 (Descriptor Table):                             │
│     大小 = 16 × queue_size bytes                             │
│     每个描述符 16 bytes                                       │
│                                                               │
│  2. 可用描述符区域 (Available Ring):                         │
│     大小 = 6 + 2 × queue_size bytes                          │
│     · flags (2 bytes)                                        │
│     · idx (2 bytes)                                          │
│     · ring[queue_size] (2 × queue_size bytes)               │
│     · avail_event (2 bytes, 可选)                           │
│                                                               │
│  3. 已用描述符区域 (Used Ring):                              │
│     大小 = 6 + 8 × queue_size bytes                          │
│     · flags (2 bytes)                                        │
│     · idx (2 bytes)                                          │
│     · ring[queue_size] (8 × queue_size bytes)               │
│     · used_event (2 bytes, 可选)                            │
│                                                               │
│  总大小 = (16 + 2 + 8) × queue_size + 12 bytes              │
│         = 26 × queue_size + 12 bytes                        │
│                                                               │
│  示例 (queue_size=256):                                      │
│  · 描述符表: 16 × 256 = 4096 bytes                          │
│  · Available: 6 + 2×256 = 518 bytes                         │
│  · Used: 6 + 8×256 = 2054 bytes                             │
│  · 总计: 6668 bytes                                          │
│                                                               │
└───────────────────────────────────────────────────────────────┘

┌─ Packed Queue (Virtio 1.1+) ───────────────────────────────┐
│                                                               │
│  只有一个描述符区域:                                         │
│  · 大小 = 16 × queue_size bytes                             │
│  · 每个描述符包含 avail/used 标志位                         │
│                                                               │
│  加上两个事件结构:                                           │
│  · driver event: 4 bytes                                    │
│  · device event: 4 bytes                                    │
│                                                               │
│  总大小 = 16 × queue_size + 8 bytes                         │
│                                                               │
│  优势:                                                       │
│  · 内存占用更少 (减少约 40%)                               │
│  · 更好的缓存局部性                                         │
│  · 更高的性能                                               │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

#### Split vs Packed Queue

```
┌─ Split Queue (Virtio 1.0) ──────────────────────────────────┐
│                                                               │
│  结构:                                                       │
│  · 三个独立的内存区域                                        │
│  · Descriptor Table + Available Ring + Used Ring             │
│                                                               │
│  工作流程:                                                   │
│  · 驱动写入 avail ring，设备读取                           │
│  · 设备写入 used ring，驱动读取                            │
│  · 通过 idx 字段追踪位置                                   │
│                                                               │
│  优点:                                                       │
│  · 实现简单                                                 │
│  · 广泛支持                                                 │
│                                                               │
│  缺点:                                                       │
│  · 内存占用较大                                             │
│  · 需要多次内存访问                                         │
│  · 缓存不友好                                               │
│                                                               │
└───────────────────────────────────────────────────────────────┘

┌─ Packed Queue (Virtio 1.1+) ───────────────────────────────┐
│                                                               │
│  结构:                                                       │
│  · 只有一个描述符区域                                        │
│  · 每个描述符包含 avail 和 used 标志位                      │
│                                                               │
│  工作流程:                                                   │
│  · 驱动和设备共享同一个描述符环                             │
│  · 通过 wrap counter 区分新旧描述符                         │
│  · 描述符中的 AVAIL/USED 标志位表示状态                     │
│                                                               │
│  状态判断:                                                   │
│  · 驱动判断描述符可用: avail == wrap_counter                │
│  · 设备判断描述符可用: avail != used && avail == wrap       │
│                                                               │
│  Wrap Counter:                                               │
│  · 初始值为 1                                                │
│  · 每当索引溢出时取反                                       │
│  · 用于区分新旧描述符                                       │
│                                                               │
│  优点:                                                       │
│  · 内存占用减少 40%                                         │
│  · 更好的缓存局部性                                         │
│  · 减少内存访问次数                                         │
│  · 性能提升 10-20%                                          │
│                                                               │
│  缺点:                                                       │
│  · 实现复杂                                                 │
│  · 需要 Virtio 1.1+ 支持                                    │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

#### 通知抑制机制 (Notification Suppression)

```
问题:
  · 频繁的 kick/interrupt 会严重影响性能
  · 设备正在处理请求时，不需要驱动频繁 kick
  · 驱动正在处理完成时，不需要设备频繁 interrupt

解决方案: VIRTIO_RING_F_EVENT_IDX

┌─ Available Buffer 通知抑制 ────────────────────────────────┐
│                                                               │
│  场景: 设备正在消费描述符，不希望被驱动频繁 kick           │
│                                                               │
│  机制:                                                       │
│  · 设备在 used ring 末尾写入 avail_event                   │
│  · avail_event 记录设备下次需要通知的 avail idx            │
│                                                               │
│  驱动侧逻辑:                                               │
│  1. 驱动准备写入新的描述符到 avail ring                    │
│  2. 读取 avail_event 的值                                  │
│  3. 判断是否需要 kick:                                     │
│     · if (new_idx >= avail_event) → 需要 kick              │
│     · else → 不需要 kick                                   │
│                                                               │
│  设备侧逻辑:                                               │
│  1. 消费一个描述符后，next_avail++                        │
│  2. 更新 avail_event = next_avail                         │
│  3. 继续处理下一个描述符                                   │
│                                                               │
│  效果:                                                       │
│  · 设备处理完当前批次后才会被通知                          │
│  · 减少了 kick 次数                                         │
│  · 提高了批处理效率                                         │
│                                                               │
└───────────────────────────────────────────────────────────────┘

┌─ Used Buffer 通知抑制 ─────────────────────────────────────┐
│                                                               │
│  场景: 驱动正在处理完成的描述符，不希望被设备频繁 interrupt │
│                                                               │
│  机制:                                                       │
│  · 驱动在 avail ring 末尾写入 used_event                   │
│  · used_event 记录驱动下次需要通知的 used idx              │
│                                                               │
│  设备侧逻辑:                                               │
│  1. 设备写入 used ring 后                                  │
│  2. 读取 used_event 的值                                   │
│  3. 判断是否需要 interrupt:                                │
│     · if (new_idx >= used_event) → 需要 interrupt          │
│     · else → 不需要 interrupt                              │
│                                                               │
│  驱动侧逻辑:                                               │
│  1. 处理一个 used 描述符后，next_used++                   │
│  2. 更新 used_event = next_used                           │
│  3. 继续处理下一个 used 描述符                             │
│                                                               │
│  效果:                                                       │
│  · 驱动处理完当前批次后才会被中断                          │
│  · 减少了 interrupt 次数                                    │
│  · 提高了批处理效率                                         │
│                                                               │
└───────────────────────────────────────────────────────────────┘

代码示例:

// 驱动侧判断是否需要 kick
static bool virtqueue_kick_prepare_split(struct virtqueue *_vq)
{
    struct vring_virtqueue *vq = to_vvq(_vq);
    u16 new, old;
    bool needs_kick;

    if (vq->event) {
        // 使用 EVENT_IDX 机制
        needs_kick = vring_need_event(
            vring_avail_event(&vq->split.vring),
            new, old);
    } else {
        // 使用 VRING_USED_F_NO_NOTIFY 标志
        needs_kick = !(vq->split.vring.used->flags &
                       VRING_USED_F_NO_NOTIFY);
    }
    return needs_kick;
}

// 判断是否需要通知的辅助函数
static inline int vring_need_event(__u16 event_idx, __u16 new_idx, __u16 old)
{
    return (__u16)(new_idx - event_idx - 1) < (__u16)(new_idx - old);
}
```

#### VIRTIO_F_NOTIFICATION_DATA

```
作用:
  · 某些设备需要知道队列中有多少可用数据
  · 用于提高效率或调试
  · 避免访问内存中的 virtqueue

机制:
  · 当驱动 kick 设备时，除了写入 queue index
  · 还会把 avail_idx 和 wrap_counter (packed mode) 写入通知寄存器

实现:

// 计算通知数据
u32 vring_notification_data(struct virtqueue *_vq)
{
    struct vring_virtqueue *vq = to_vvq(_vq);
    u16 next;

    if (vq->packed_ring) {
        // Packed mode: 包含 avail_idx 和 wrap_counter
        next = (vq->packed.next_avail_idx &
                ~(-(1 << VRING_PACKED_EVENT_F_WRAP_CTR))) |
               vq->packed.avail_wrap_counter <<
                VRING_PACKED_EVENT_F_WRAP_CTR;
    } else {
        // Split mode: 只包含 avail_idx
        next = vq->split.avail_idx_shadow;
    }

    return next << 16 | _vq->index;
}

// 设备侧接收通知
static bool vm_notify_with_data(struct virtqueue *vq)
{
    struct virtio_mmio_device *vm_dev = to_virtio_mmio_device(vq);
    u32 data = vring_notification_data(vq);

    // 写入通知数据到寄存器
    writel(data, vm_dev->base + VIRTIO_MMIO_QUEUE_NOTIFY);
    return true;
}

优势:
  · 设备无需访问内存即可知道队列状态
  · 减少内存访问，提高性能
  · 便于调试和监控
```

#### Fast MMIO 优化

```
问题:
  · 每次 MMIO 访问都会触发 VM-Exit
  · 需要 KVM 模拟指令执行，性能较差

解决方案: KVM IOEventFD + Fast MMIO Bus

机制:
  · 设备注册 IOEventFD 时指定地址和数据匹配
  · KVM 将匹配的 MMIO 访问放入快速总线 (KVM_FAST_MMIO_BUS)
  · 当 Guest 触发 EPT_MISCONFIG 时:
    1. 先检查 KVM_FAST_MMIO_BUS 是否有匹配
    2. 如果有，直接完成写入，跳过指令模拟
    3. 如果没有，才走正常的指令模拟流程

实现:

// 注册 IOEventFD
struct kvm_ioeventfd {
    __u64 addr;           // MMIO 地址
    __u32 len;            // 数据长度
    __u64 datamatch;      // 数据匹配值
    __u32 flags;          // 标志位
    // ...
};

// KVM 内部处理
static int kvm_assign_ioeventfd(struct kvm *kvm, struct kvm_ioeventfd *args)
{
    // 如果 len=0，放入快速总线
    if (!args->len && bus_idx == KVM_MMIO_BUS) {
        ret = kvm_assign_ioeventfd_idx(kvm, KVM_FAST_MMIO_BUS, args);
    }
    // ...
}

// 处理 EPT_MISCONFIG
static int handle_ept_misconfig(struct kvm_vcpu *vcpu)
{
    gpa_t gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);
    
    // 先检查快速总线
    if (!is_guest_mode(vcpu) &&
        !kvm_io_bus_write(vcpu, KVM_FAST_MMIO_BUS, gpa, 0, NULL)) {
        trace_kvm_fast_mmio(gpa);
        return kvm_skip_emulated_instruction(vcpu);
    }
    
    // 走正常模拟流程
    return kvm_mmu_page_fault(vcpu, gpa, PFERR_RSVD_MASK, NULL, 0);
}

限制:
  · Virtio-mmio 协议本身的限制
  · 同一地址绑定多个 eventfd 时无法使用 fast-mmio
  · 需要特定条件下的优化
```

#### 同步机制与原子性保证

```
问题:
  · Host 和 Guest 并发访问 virtqueue
  · 需要保证内存访问的原子性和一致性
  · 避免数据竞争和状态不一致

解决方案: volatile + memory barriers

┌─ Volatile 的使用 ──────────────────────────────────────────┐
│                                                               │
│  作用:                                                       │
│  · 防止编译器优化                                            │
│  · 确保每次访问都从内存读取，而不是使用缓存值               │
│                                                               │
│  示例:                                                       │
│  volatile u32 *avail_idx = &vring->avail->idx;              │
│  u32 idx = *avail_idx;  // 每次都从内存读取                 │
│                                                               │
└───────────────────────────────────────────────────────────────┘

┌─ Memory Barriers ──────────────────────────────────────────┐
│                                                               │
│  作用:                                                       │
│  · 保证内存访问的顺序                                        │
│  · 防止 CPU 重排序                                          │
│  · 确保 Host 和 Guest 看到一致的视图                        │
│                                                               │
│  类型:                                                       │
│  · smp_wmb(): 写内存屏障                                    │
│    - 保证之前的写操作在之后的写操作之前完成                  │
│    - 用于: 先写数据，再更新索引                             │
│                                                               │
│  · smp_rmb(): 读内存屏障                                    │
│    - 保证之前的读操作在之后的读操作之前完成                  │
│    - 用于: 先读索引，再读数据                               │
│                                                               │
│  · smp_mb(): 全屏障                                          │
│    - 保证之前的所有操作在之后的所有操作之前完成              │
│    - 用于: 复杂的同步场景                                   │
│                                                               │
│  · virtio_wmb/rmb/mb(): Virtio 封装                         │
│    - 根据 weak_barriers 参数选择强/弱屏障                   │
│    - 提供跨平台抽象                                         │
│                                                               │
└───────────────────────────────────────────────────────────────┘

┌─ 设备侧同步示例 ──────────────────────────────────────────┐
│                                                               │
│  // 设备写入 used ring                                      │
│  void add_used(struct vring_virtqueue *vq,                  │
│                struct vring_used_elem *elem)                 │
│  {                                                           │
│      // 1. 写入数据到 used ring                             │
│      vq->vring.used->ring[idx] = *elem;                    │
│                                                               │
│      // 2. 写屏障，确保数据写入完成                         │
│      smp_wmb();                                              │
│                                                               │
│      // 3. 更新 idx                                         │
│      vq->vring.used->idx++;                                │
│                                                               │
│      // 4. 发送中断通知驱动                                 │
│      notify_guest();                                         │
│  }                                                           │
│                                                               │
│  为什么需要 smp_wmb()?                                       │
│  · 如果 CPU 重排序，可能先更新 idx，再写入数据             │
│  · 驱动看到新的 idx，但数据还没写入                        │
│  · 导致驱动读取到旧数据，状态不一致                        │
│  · smp_wmb() 确保数据先写入，idx 后更新                    │
│                                                               │
└───────────────────────────────────────────────────────────────┘

┌─ 驱动侧同步示例 ──────────────────────────────────────────┐
│                                                               │
│  // 驱动读取 used ring                                      │
│  void process_used(struct vring_virtqueue *vq)             │
│  {                                                           │
│      // 1. 读取 idx                                         │
│      u32 idx = vq->vring.used->idx;                       │
│                                                               │
│      // 2. 读屏障，确保 idx 读取完成                        │
│      smp_rmb();                                              │
│                                                               │
│      // 3. 读取数据                                         │
│      struct vring_used_elem elem = vq->vring.used->ring[idx];│
│                                                               │
│      // 4. 处理数据                                         │
│      process(&elem);                                         │
│  }                                                           │
│                                                               │
│  为什么需要 smp_rmb()?                                       │
│  · 如果 CPU 重排序，可能先读取数据，再读取 idx             │
│  · 导致读取到旧数据                                        │
│  · smp_rmb() 确保 idx 先读取，数据后读取                   │
│                                                               │
└───────────────────────────────────────────────────────────────┘

关键原则:
  · 写方: 先写数据，再写索引 (使用 wmb)
  · 读方: 先读索引，再读数据 (使用 rmb)
  · 确保 Host 和 Guest 看到一致的视图
```

---
