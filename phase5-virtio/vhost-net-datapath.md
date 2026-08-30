# vhost-net 数据路径

> Phase 5 深度主题 | 从 README.md 拆出，正文内容未作改动

---

## 🚀 vhost-net数据路径

### TX路径 (Guest发送网络包)

```
Guest发送网络包:

┌─ Guest ──────────────────────────────────────────────────┐
│  1. 应用调用send()                                        │
│  2. virtio-net驱动准备数据                                │
│  3. 填充avail ring:                                       │
│     desc[0].addr = DMA地址 (GPA)                          │
│     desc[0].len = 包长度                                  │
│     avail.ring[avail.idx] = desc_index                    │
│     avail.idx++                                           │
│  4. 写Queue Notify寄存器 (VM-Exit!)                       │
└────────────────┬─────────────────────────────────────────┘
                 │ VM-Exit: IO_INSTRUCTION
┌────────────────▼─────────────────────────────────────────┐
│  KVM: 处理IO_INSTRUCTION                                  │
│  ├── 识别为virtio kick                                    │
│  ├── 路由到vhost-net内核线程                              │
│  └→ 唤醒vhost_worker线程                                  │
└────────────────┬─────────────────────────────────────────┘
                 │ 内核态函数调用
┌────────────────▼─────────────────────────────────────────┐
│  vhost_worker线程                                         │
│  │                                                        │
│  ▼                                                        │
│  vhost_handle_tx() [vhost/net.c]                          │
│  │                                                        │
│  ├── 读取avail ring:                                      │
│  │   └→ vring_avail_idx() 获取avail.idx                   │
│  │   └→ vring_avail_ring() 获取描述符索引                 │
│  │                                                        │
│  ├── 读取描述符:                                          │
│  │   └→ vring_desc_addr() 获取GPA                         │
│  │   └→ vring_desc_len() 获取长度                         │
│  │                                                        │
│  ├── 访问Guest内存:                                       │
│  │   └→ vhost_get_vq_desc() 读取数据                      │
│  │      └→ GPA→HVA→读数据 (通过KVM memslot)               │
│  │                                                        │
│  ├── 发送到TAP设备:                                       │
│  │   └→ skb = alloc_skb(len)                              │
│  │   └→ copy_from_user(skb->data, guest_data, len)        │
│  │   └→ dev_queue_xmit(skb) → TAP → 物理网卡              │
│  │                                                        │
│  ├── 写入used ring:                                       │
│  │   └→ vring_used_ring_id() = desc_index                 │
│  │   └→ vring_used_ring_len() = written_bytes             │
│  │   └→ vring_used_idx++                                  │
│  │                                                        │
│  └── 通知Guest:                                           │
│      └→ eventfd_signal(vq->kick_ctx)                      │
│         └→ 触发vCPU中断 (直接调用kvm_set_irq!)            │
│                                                            │
└────────────────────────────────────────────────────────────┘
                 │ 内核态
┌────────────────▼─────────────────────────────────────────┐
│  Host内核: TAP设备 → 物理网卡                             │
└──────────────────────────────────────────────────────────┘
```

### RX路径 (Guest接收网络包)

```
Guest接收网络包:

┌─ Host内核 ───────────────────────────────────────────────┐
│  物理网卡收到包                                           │
│    │                                                      │
│    ▼                                                      │
│  TAP设备接收                                              │
│    │                                                      │
│    ▼                                                      │
│  vhost-net RX处理                                         │
│    │                                                      │
│    ▼                                                      │
│  vhost_handle_rx() [vhost/net.c]                          │
│    │                                                      │
│    ├── 检查avail ring是否有可用的RX buffer                │
│    │   └→ 如果没有，延迟处理 (等待Guest补充buffer)        │
│    │                                                      │
│    ├── 从TAP读取包数据                                    │
│    │   └→ len = skb_copy_to_vq(vq, skb)                   │
│    │                                                      │
│    ├── 写入Guest内存:                                     │
│    │   └→ GPA→HVA→写数据 (通过KVM memslot)                │
│    │                                                      │
│    ├── 写入used ring:                                     │
│    │   └→ vring_used_ring_id() = desc_index               │
│    │   └→ vring_used_ring_len() = len                     │
│    │   └→ vring_used_idx++                                │
│    │                                                      │
│    └── 通知Guest:                                         │
│        └→ eventfd_signal(vq->kick_ctx)                    │
│           └→ 触发vCPU中断                                  │
│                                                            │
└────────────────────────────────────────────────────────────┘
                 │ 中断注入
┌────────────────▼─────────────────────────────────────────┐
│  Guest收到中断                                            │
│    │                                                      │
│    ▼                                                      │
│  virtio-net驱动处理                                       │
│    │                                                      │
│    ├── 读取used ring                                      │
│    ├── 回收描述符                                         │
│    └→ 将包数据传递给网络栈                                │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

---
