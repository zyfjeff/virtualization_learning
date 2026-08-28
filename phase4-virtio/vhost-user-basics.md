# vhost-user 协议入门

> Phase 4 深度主题 | 从 README.md 拆出，正文内容未作改动；协议字段速查见 vhost-user-protocol-latest.md

---

## 🔄 vhost-user协议

### 什么是vhost-user？

vhost-user 是 vhost 的用户态实现版本，允许在用户态实现 vhost 后端，而无需编写内核模块。

```
┌─ vhost vs vhost-user ──────────────────────────────────────┐
│                                                               │
│  vhost (内核态):                                             │
│  · 后端实现在内核模块中 (vhost-net.ko)                      │
│  · 通过 ioctl 与 QEMU 通信                                  │
│  · 高性能，但灵活性低                                        │
│  · 需要内核模块开发能力                                      │
│                                                               │
│  vhost-user (用户态):                                        │
│  · 后端实现在用户态进程中                                    │
│  · 通过 Unix domain socket 与 QEMU 通信                     │
│  · 灵活性高，易于开发和调试                                  │
│  · 性能略低于内核态 vhost                                   │
│  · 支持热迁移和动态配置                                      │
│                                                               │
│  典型应用:                                                   │
│  · DPDK vhost-user (高性能网络后端)                         │
│  · SPDK vhost-user (高性能存储后端)                         │
│  · 自定义 vhost-user 后端                                   │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### vhost-user协议架构

```
┌─ vhost-user 架构 ──────────────────────────────────────────┐
│                                                               │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  QEMU (前端)                                          │  │
│  │  ├── 创建 Virtio 设备前端                            │  │
│  │  ├── 配置 virtqueue                                  │  │
│  │  └── 通过 Unix socket 连接后端                       │  │
│  └────────────────────────┬─────────────────────────────┘  │
│                           │ Unix domain socket              │
│                           │ (VHOST_USER 协议消息)          │
│  ┌────────────────────────▼─────────────────────────────┐  │
│  │  vhost-user 后端 (用户态进程)                        │  │
│  │  ├── 监听 Unix socket                               │  │
│  │  ├── 接收 VHOST_USER 消息                           │  │
│  │  ├── 实现设备逻辑 (网络/存储/自定义)                │  │
│  │  └── 直接访问 Guest 内存 (通过 mmap)                │  │
│  └──────────────────────────────────────────────────────┘  │
│                           │                                  │
│                           │ mmap                             │
│  ┌────────────────────────▼─────────────────────────────┐  │
│  │  Guest 内存空间                                       │  │
│  │  ├── virtqueue (描述符表/avail/used ring)            │  │
│  │  └── 数据缓冲区                                      │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### vhost-user协议消息

vhost-user 协议定义了一系列消息用于前端（QEMU）和后端之间的通信。

```
┌─ 核心消息 ─────────────────────────────────────────────────┐
│                                                               │
│  1. VHOST_USER_GET_FEATURES                                 │
│     · 后端报告支持的特性                                    │
│     · 前端根据特性进行协商                                  │
│                                                               │
│  2. VHOST_USER_SET_FEATURES                                 │
│     · 前端设置协商后的特性                                  │
│     · 后端根据特性启用/禁用功能                             │
│                                                               │
│  3. VHOST_USER_SET_MEM_TABLE                                │
│     · 前端传递 Guest 内存区域信息                           │
│     · 后端通过 mmap 映射这些区域                            │
│     · 包含多个内存区域 (memory regions)                     │
│                                                               │
│  4. VHOST_USER_SET_VRING_NUM                                │
│     · 设置 virtqueue 的大小 (描述符数量)                   │
│                                                               │
│  5. VHOST_USER_SET_VRING_ADDR                               │
│     · 设置 virtqueue 的地址信息                             │
│     · 包括描述符表、avail ring、used ring 的 GPA           │
│                                                               │
│  6. VHOST_USER_SET_VRING_BASE                               │
│     · 设置 virtqueue 的起始索引                             │
│     · 用于恢复或迁移场景                                    │
│                                                               │
│  7. VHOST_USER_GET_VRING_BASE                               │
│     · 获取 virtqueue 的当前索引                             │
│     · 用于迁移时保存状态                                    │
│                                                               │
│  8. VHOST_USER_SET_VRING_KICK                               │
│     · 设置 kick eventfd (Guest→后端通知)                   │
│     · 前端写入 eventfd 通知后端处理请求                     │
│                                                               │
│  9. VHOST_USER_SET_VRING_CALL                               │
│     · 设置 call eventfd (后端→前端通知)                    │
│     · 后端写入 eventfd 通知前端处理完成                     │
│                                                               │
│  10. VHOST_USER_SET_VRING_ERR                               │
│      · 设置错误 eventfd                                     │
│      · 后端发生错误时通知前端                               │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### vhost-user消息格式

```c
/* vhost-user 消息头 */
struct VhostUserMsg {
    uint32_t request;      /* 消息类型 (VHOST_USER_*) */
    
#define VHOST_USER_VERSION  1
    uint32_t flags;        /* 标志位 (版本等) */
    uint32_t size;         /* 消息体大小 */
    
    /* 消息体 (根据 request 类型不同) */
    union {
        uint64_t u64;                      /* 单个 64位值 */
        struct vhost_vring_state state;    /* virtqueue 状态 */
        struct vhost_vring_addr addr;      /* virtqueue 地址 */
        struct vhost_user_memory memory;   /* 内存区域信息 */
        struct vhost_user_log log;         /* 日志信息 */
        /* ... 其他类型 ... */
    };
};

/* 示例: VHOST_USER_SET_MEM_TABLE 消息体 */
struct vhost_user_memory {
    uint32_t nregions;     /* 内存区域数量 */
    uint32_t padding;
    struct vhost_user_memory_region regions[0];  /* 可变数组 */
};

struct vhost_user_memory_region {
    uint64_t guest_phys_addr;  /* Guest 物理地址 */
    uint64_t memory_size;      /* 内存大小 */
    uint64_t userspace_addr;   /* 用户态地址 (QEMU侧) */
    uint64_t mmap_offset;      /* mmap 偏移 */
};
```

### vhost-user工作流程

```
┌─ vhost-user 初始化流程 ────────────────────────────────────┐
│                                                               │
│  1. QEMU 启动 vhost-user 后端进程                           │
│     · 通过 Unix domain socket 连接                          │
│                                                               │
│  2. 特性协商                                                │
│     · QEMU: VHOST_USER_GET_FEATURES                        │
│     · 后端: 返回支持的特性                                  │
│     · QEMU: VHOST_USER_SET_FEATURES (协商后的特性)         │
│                                                               │
│  3. 配置内存                                                │
│     · QEMU: VHOST_USER_SET_MEM_TABLE                       │
│     · 后端: mmap 映射 Guest 内存区域                       │
│                                                               │
│  4. 配置 virtqueue (对每个队列重复)                         │
│     · VHOST_USER_SET_VRING_NUM (设置队列大小)              │
│     · VHOST_USER_SET_VRING_ADDR (设置队列地址)             │
│     · VHOST_USER_SET_VRING_BASE (设置起始索引)             │
│     · VHOST_USER_SET_VRING_KICK (设置 kick eventfd)        │
│     · VHOST_USER_SET_VRING_CALL (设置 call eventfd)        │
│                                                               │
│  5. 启动后端处理                                            │
│     · 后端开始监听 kick eventfd                             │
│     · 收到 kick 后处理 virtqueue                            │
│                                                               │
└───────────────────────────────────────────────────────────────┘

┌─ vhost-user 数据路径 ──────────────────────────────────────┐
│                                                               │
│  Guest 发送数据:                                            │
│  1. Guest 驱动填充 avail ring                              │
│  2. Guest 写入 kick eventfd                                │
│  3. 后端收到 kick 通知                                     │
│  4. 后端读取 avail ring                                    │
│  5. 后端处理描述符 (通过 mmap 访问 Guest 内存)           │
│  6. 后端写入 used ring                                     │
│  7. 后端写入 call eventfd                                  │
│  8. QEMU 收到 call 通知                                    │
│  9. QEMU 注入中断到 Guest                                  │
│                                                               │
│  关键点:                                                    │
│  · 全程用户态，无需内核介入                                │
│  · 通过 mmap 直接访问 Guest 内存                           │
│  · 通过 eventfd 进行异步通知                               │
│  · 性能接近内核态 vhost                                   │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### vhost-user后端实现示例 (DPDK)

```c
/* DPDK vhost-user 后端简化示例 */

#include <rte_vhost.h>

/*  virtio-net 设备操作回调 */
static const struct vhost_device_ops virtio_net_device_ops = {
    .new_device =  new_device,      /* 新设备连接 */
    .destroy_device = destroy_device, /* 设备断开 */
    .vring_state_changed = vring_state_changed, /* virtqueue 状态变化 */
    .features_changed = features_changed, /* 特性变化 */
};

/* 新设备连接回调 */
static int
new_device(int vid)
{
    /* 获取 virtqueue 数量 */
    int num_queues = rte_vhost_get_vring_num(vid, 0);
    
    /* 获取 Guest 内存 */
    struct rte_vhost_memory *mem;
    rte_vhost_get_mem_table(vid, &mem);
    
    /* 映射 Guest 内存到用户态 */
    for (int i = 0; i < mem->nregions; i++) {
        void *addr = mmap(NULL, mem->regions[i].size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         mem->regions[i].fd,
                         mem->regions[i].mmap_offset);
        /* 保存映射地址 */
    }
    
    /* 启用设备 */
    rte_vhost_driver_enable_features(vid, ...);
    
    return 0;
}

/* virtqueue 状态变化回调 */
static int
vring_state_changed(int vid, int vring, int enable)
{
    if (enable) {
        /* 启动 vring 处理 */
        start_vring_handler(vid, vring);
    } else {
        /* 停止 vring 处理 */
        stop_vring_handler(vid, vring);
    }
    return 0;
}

/* 主函数 */
int main(int argc, char *argv[])
{
    /* 初始化 DPDK */
    rte_eal_init(argc, argv);
    
    /* 注册 vhost-user 驱动 */
    rte_vhost_driver_register(socket_path, flags);
    
    /* 注册设备操作回调 */
    rte_vhost_driver_callback_register(&virtio_net_device_ops);
    
    /* 启动 vhost-user 驱动 */
    rte_vhost_driver_start(socket_path);
    
    /* 主循环 */
    while (1) {
        rte_epoll_wait(epfd, events, MAX_EVENTS, -1);
        /* 处理事件 */
    }
    
    return 0;
}
```

### vhost-user优势

```
┌─ vhost-user 优势 ──────────────────────────────────────────┐
│                                                               │
│  1. 灵活性高                                                 │
│     · 用户态实现，易于开发和调试                            │
│     · 可以快速迭代和测试                                    │
│     · 支持自定义设备逻辑                                    │
│                                                               │
│  2. 零拷贝优化                                               │
│     · 通过 mmap 直接访问 Guest 内存                         │
│     · 无需数据拷贝                                          │
│     · 性能接近内核态 vhost                                 │
│                                                               │
│  3. 多队列支持                                               │
│     · 支持多 virtqueue 并行处理                             │
│     · 充分利用多核 CPU                                      │
│     · 提高并发性能                                          │
│                                                               │
│  4. 热迁移支持                                               │
│     · 通过 VHOST_USER_GET_VRING_BASE 保存队列状态          │
│     · 通过 VHOST_USER_SET_VRING_BASE 恢复队列状态          │
│     · 支持实时迁移                                          │
│                                                               │
│  5. 动态配置                                                 │
│     · 支持动态添加/移除设备                                │
│     · 支持动态调整队列大小                                  │
│     · 支持特性协商                                          │
│                                                               │
│  6. 生态丰富                                                 │
│     · DPDK vhost-user (网络)                                │
│     · SPDK vhost-user (存储)                                │
│     · 开源社区活跃                                          │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

### vhost-user性能对比

```
┌─ 性能对比 (10G 网络) ─────────────────────────────────────┐
│                                                               │
│  实现方式              吞吐量        延迟       CPU占用    │
│  ────────────────────────────────────────────────────────  │
│  QEMU (用户态)         ~100万 pps    ~50μs      高         │
│  vhost-net (内核态)    ~300万 pps    ~15μs      中         │
│  DPDK vhost-user       ~350万 pps    ~12μs      中         │
│                                                               │
│  分析:                                                       │
│  · DPDK vhost-user 性能略高于 vhost-net                   │
│  · 因为 DPDK 使用了更多优化技术:                          │
│    - 用户态轮询模式                                        │
│    - 零拷贝数据路径                                        │
│    - 批量处理优化                                          │
│    - CPU 亲和性优化                                        │
│                                                               │
│  但是:                                                       │
│  · vhost-net 更简单，不需要用户态进程                      │
│  · vhost-net 更稳定，内核级质量                            │
│  · vhost-net 更易维护，内核统一管理                        │
│                                                               │
└───────────────────────────────────────────────────────────────┘
```

---
