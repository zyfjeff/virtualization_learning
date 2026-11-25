# vhost-net 数据路径

> Phase 6 深度主题 | 2026-11-25 事实核查重写
> 源码基线：Linux 6.12.93 `drivers/vhost/net.c`、`drivers/vhost/vhost.c`、`virt/kvm/eventfd.c`
>
> ⚠️ 本节初版（从旧 README 拆出、正文未作改动）用的是多轮重写之前的模型：
> 函数名 `vhost_handle_tx/vhost_handle_rx`、字段 `vq->kick_ctx`、发送方式
> `alloc_skb + copy_from_user + dev_queue_xmit`、以及"KVM 处理 IO_INSTRUCTION 识别 virtio kick"
> 在 6.12.93 里**都不存在**。修正记录见 [corrections.md](corrections.md) 勘误 4。

---

## 1. kick 怎么进内核：ioeventfd，不是 I/O 模拟

Guest 通知设备的动作是**写 notify 寄存器**（virtio-pci modern 是写 notify BAR 上的 2 字节，
virtio-mmio 是写 `VIRTIO_MMIO_QUEUE_NOTIFY`）。QEMU 提前把这个地址注册成 ioeventfd
（`KVM_IOEVENTFD`，fd 就是 vhost 的 `vq->kick`），于是：

```
Guest 写 notify 寄存器
  → VM-Exit
  → KVM 在 MMIO/PIO bus 上命中 ioeventfd
  → eventfd_signal(kick)              ← 不回 QEMU，不模拟指令
  → vhost 的等待队列被唤醒
```

`len = 0` 且地址在 `KVM_MMIO_BUS` 上的 ioeventfd 会被**额外**挂到 `KVM_FAST_MMIO_BUS`
（见 [virtio-queue.md](virtio-queue.md) §fast-mmio）：

```c
/* 来源: virt/kvm/eventfd.c:981-988 —— kvm_assign_ioeventfd() */
	/* When length is ignored, MMIO is also put on a separate bus, for
	 * faster lookups.
	 */
	if (!args->len && bus_idx == KVM_MMIO_BUS) {
		ret = kvm_assign_ioeventfd_idx(kvm, KVM_FAST_MMIO_BUS, args);
		if (ret < 0)
			goto fast_fail;
	}
```

vhost 侧接收 kick 的是 `vq->poll`：`VHOST_SET_VRING_KICK` 设置 `vq->kick` 后开始轮询该 fd：

```c
/* 来源: drivers/vhost/vhost.c:2257-2258 —— vhost_vring_ioctl() */
	if (pollstart && vq->handle_kick)
		r = vhost_poll_start(&vq->poll, vq->kick);
```

eventfd 被 signal 时，`vhost_poll_wakeup()` 直接排入工作（或不用 worker 时同步执行）：

```c
/* 来源: drivers/vhost/vhost.c:172-187 —— vhost_poll_wakeup() */
static int vhost_poll_wakeup(wait_queue_entry_t *wait, unsigned mode, int sync,
			     void *key)
{
	struct vhost_poll *poll = container_of(wait, struct vhost_poll, wait);
	struct vhost_work *work = &poll->work;

	if (!(key_to_poll(key) & poll->mask))
		return 0;

	if (!poll->dev->use_worker)
		work->fn(work);
	else
		vhost_poll_queue(poll);

	return 0;
}
```

`work->fn` 就是 `vq->handle_kick`，vhost-net 在 `vhost_net_open()` 里把它设成
`handle_tx_kick` / `handle_rx_kick`，二者再调 `handle_tx()` / `handle_rx()`
（`drivers/vhost/net.c:1238-1254`）。

**卸载的收益就在这里**：数据面路径上 QEMU 不再参与，vhost worker 线程在内核里完成
读 avail → 收发 → 写 used → 通知 guest 的全部动作。

---

## 2. TX 路径：一个包怎么从 avail ring 到 TAP

```
handle_tx(net)                     net.c:945
  ├─ vhost_vq_get_backend(vq)      ← 后端是 TAP socket（struct socket *）
  ├─ vq_meta_prefetch(vq)          ← 预取 desc/avail/used 三处的 iotlb 映射
  ├─ vhost_disable_notify()        ← 处理期间不再收 kick（配合 batch）
  ├─ vhost_net_disable_vq()
  └─ vhost_sock_zcopy(sock) ?      net.c:346（模块参数 experimental_zcopytx + SOCK_ZEROCOPY）
       ├─ yes → handle_tx_zerocopy()   net.c:836
       └─ no  → handle_tx_copy()        net.c:746
```

### 2.1 `handle_tx_copy()`：真正发一个包

**源码引用**: `drivers/vhost/net.c:746-834`

```c
static void handle_tx_copy(struct vhost_net *net, struct socket *sock)
{
	struct vhost_net_virtqueue *nvq = &net->vqs[VHOST_NET_VQ_TX];
	struct vhost_virtqueue *vq = &nvq->vq;
	...
	size_t len, total_len = 0;
	bool sock_can_batch = (sock->sk->sk_sndbuf == INT_MAX);

	do {
		if (nvq->done_idx == VHOST_NET_BATCH)
			vhost_tx_batch(net, nvq, sock, &msg);

		head = get_tx_bufs(net, nvq, &msg, &out, &in, &len,
				   &busyloop_intr);
		...
		err = sock->ops->sendmsg(sock, &msg, len);      /* :816 */
		...
		vq->heads[nvq->done_idx].id = cpu_to_vhost32(vq, head);   /* :828 */
		vq->heads[nvq->done_idx].len = 0;
		++nvq->done_idx;
	} while (likely(!vhost_exceeds_weight(vq, ++sent_pkts, total_len)));

	vhost_tx_batch(net, nvq, sock, &msg);
}
```

要点：

| 步骤 | 代码 | 做什么 |
|---|---|---|
| 取描述符链 | `get_tx_bufs()` `net.c:628` → `vhost_net_tx_get_vq_desc()` `net.c:579` | `vhost_get_vq_desc()` 读 avail/desc，填充 `vq->iov`（**HVA**） |
| 建 iter | `init_iov_iter()` `net.c:616-626` | 跳过 virtio-net hdr，把 guest 缓冲直接做成 `iov_iter` |
| 发送 | `sock->ops->sendmsg()` `net.c:816` | 交给 TAP socket；拷贝发生在 tap/协议层 |
| 记账 | `vq->heads[nvq->done_idx]` `net.c:828-830` | 攒到 `VHOST_NET_BATCH` 才批量写 used |

`init_iov_iter()` 是"vhost 不用 skb 手工搬运"的关键——数据源就是 guest 内存的 iovec：

```c
/* 来源: drivers/vhost/net.c:616-626 —— init_iov_iter() */
static size_t init_iov_iter(struct vhost_virtqueue *vq, struct iov_iter *iter,
			    size_t hdr_size, int out)
{
	/* Skip header. TODO: support TSO. */
	size_t len = iov_length(vq->iov, out);

	iov_iter_init(iter, ITER_SOURCE, vq->iov, out, len);
	iov_iter_advance(iter, hdr_size);

	return iov_iter_count(iter);
}
```

> **vhost-net 不调用 `dev_queue_xmit()`，也不自己 `alloc_skb()`**。
> TX 的出口是 socket（`sock->ops->sendmsg`，`net.c:816`）；`net.c` 里 `dev_queue_xmit` 0 命中。
> 早期版本把它写成"skb 直发 TAP 网卡"是错的。
> 6.12 还多了一条 XDP 批量路径：`sock_can_batch` 时先 `vhost_net_build_xdp()`（`net.c:793`），
> 装不下才回落到单包 `sendmsg`。

### 2.2 两种 TX 模式的真实判据

| 模式 | 判据（源码） | 行为 |
|---|---|---|
| `handle_tx_copy` | `!vhost_sock_zcopy(sock)`（`net.c:965`） | 数据经 `sendmsg` 的 iter 拷进 tap |
| `handle_tx_zerocopy` | `vhost_sock_zcopy(sock)`（`net.c:962`） | 页挂 ubuf，等 DMA 完成再回收 used |

```c
/* 来源: drivers/vhost/net.c:346-350 —— vhost_sock_zcopy() */
static bool vhost_sock_zcopy(struct socket *sock)
{
	return unlikely(experimental_zcopytx) &&
		sock_flag(sock->sk, SOCK_ZEROCOPY);
}
```

开关是**模块参数 `experimental_zcopytx`**（`net.c:38` 默认 0，`:39` 权限 0444）
**且** socket 带 `SOCK_ZEROCOPY`。不存在 `VHOST_NET_USE_VHOST_NET_ZCOPY` 这个宏
（全内核 0 命中）。

---

## 3. RX 路径：包怎么进 guest buffer

**源码引用**: `drivers/vhost/net.c:1092-1236`（`handle_rx`）

与 TX 相反，RX 的驱动方是"TAP socket 上有数据可读"：`vhost_net_enable_vq()`
把 `poll` 挂在 **socket 的等待队列**上，可读时唤醒 `handle_rx_net()`（`net.c:1263`）：

```c
/* 来源: drivers/vhost/net.c:442-455 —— vhost_net_enable_vq() */
static int vhost_net_enable_vq(struct vhost_net *n,
				struct vhost_virtqueue *vq)
{
	struct vhost_net_virtqueue *nvq =
		container_of(vq, struct vhost_net_virtqueue, vq);
	struct vhost_poll *poll = n->poll + (nvq - n->vqs);
	struct socket *sock;

	sock = vhost_vq_get_backend(vq);
	if (!sock)
		return 0;

	return vhost_poll_start(poll, sock->file);
}
```

`handle_rx()` 的主循环（节选，全部为实际源码行）：

```c
/* 来源: drivers/vhost/net.c:1138-1185（节选） */
	do {
		sock_len = vhost_net_rx_peek_head_len(net, sock->sk,
						      &busyloop_intr);   /* :1139 */
		if (!sock_len)
			break;
		sock_len += sock_hlen;
		vhost_len = sock_len + vhost_hlen;
		headcount = get_rx_bufs(vq, vq->heads + nvq->done_idx,
					vhost_len, &in, vq_log, &log,
					likely(mergeable) ? UIO_MAXIOV : 1); /* :1145 */
		...
		iov_iter_init(&msg.msg_iter, ITER_DEST, vq->iov, in, vhost_len); /* :1177 */
		...
		err = sock->ops->recvmsg(sock, &msg,
					 vhost_len, MSG_DONTWAIT | MSG_TRUNC);   /* :1185 */
```

| 步骤 | 代码 | 说明 |
|---|---|---|
| 看有没有包 | `vhost_net_rx_peek_head_len()` `net.c:992` → `vhost_net_buf_peek()` `net.c:209` | 批量预取用 `ptr_ring_consume_batched()`（`net.c:181`），不是每包一次 |
| 备 guest buffer | `get_rx_bufs()` | 一次可能占用多个描述符（`VIRTIO_NET_F_MRG_RXBUF`） |
| 收数据 | `sock->ops->recvmsg()` `net.c:1185` | iter 方向是 `ITER_DEST`，目标就是 guest 的 `vq->iov` |
| 补 hdr / num_buffers | `copy_to_iter(&hdr,...)` `net.c:1198`、`copy_to_iter(&num_buffers,...)` `net.c:1214` | 只有这两处是 vhost 自己的拷贝，且写的是 header 字段 |
| 攒批写 used | `nvq->done_idx += headcount` `net.c:1220` → `vhost_net_signal_used()` `net.c:1222` | 超过权重就收工：`vhost_exceeds_weight()` `net.c:1227` |

> **循环条件是"权重/包数"**（`net.c:1227`），不是"rxq 非空"；
> `vhost_net_buf_produce()` 只在 `vhost_net_buf_peek()`（`net.c:216`）里被调用，
> `handle_rx()` 取包用的是 `vhost_net_buf_consume()`（`net.c:1167`）。

---

## 4. used ring 写回 + 中断注入

`vhost_net_signal_used()` 一次把整批 heads 写进 used ring 并通知 guest：

```c
/* 来源: drivers/vhost/net.c:457-467 —— vhost_net_signal_used() */
static void vhost_net_signal_used(struct vhost_net_virtqueue *nvq)
{
	struct vhost_virtqueue *vq = &nvq->vq;
	struct vhost_dev *dev = vq->dev;

	if (!nvq->done_idx)
		return;

	vhost_add_used_and_signal_n(dev, vq, vq->heads, nvq->done_idx);
	nvq->done_idx = 0;
}
```

```c
/* 来源: drivers/vhost/vhost.c:3055-3061 —— vhost_add_used_and_signal_n() */
void vhost_add_used_and_signal_n(struct vhost_dev *dev,
				 struct vhost_virtqueue *vq,
				 struct vring_used_elem *heads, unsigned count)
{
	vhost_add_used_n(vq, heads, count);
	vhost_signal(dev, vq);
}
```

而"通知 guest"**不是**调 KVM 接口，就是 signal 一个 eventfd：

```c
/* 来源: drivers/vhost/vhost.c:3035-3041 —— vhost_signal() */
/* This actually signals the guest, using eventfd. */
void vhost_signal(struct vhost_dev *dev, struct vhost_virtqueue *vq)
{
	/* Signal the Guest tell them we used something up. */
	if (vq->call_ctx.ctx && vhost_notify(dev, vq))
		eventfd_signal(vq->call_ctx.ctx);
}
```

`vhost_notify()` 负责 event index / `VRING_USED_F_NO_NOTIFY` 的抑制判断
（定义 `vhost.c:2998`，末行 `return vring_need_event(vhost16_to_cpu(vq, event), new, old);`
在 `vhost.c:3032`）。

字段名是 `vq->call_ctx.ctx`，**不存在 `vq->kick_ctx`**（`drivers/vhost/vhost.h` 0 命中）：
`struct file *kick`（`vhost.h:104`）是 guest→host 方向，
`struct vhost_vring_call call_ctx`（`vhost.h:105`，内含 `eventfd_ctx *ctx` + `irq_bypass_producer`）
才是 host→guest 的中断方向。

调用 `kvm_set_irq()` 的是 KVM 的 irqfd，不是 vhost：

```c
/* 来源: virt/kvm/eventfd.c:41-56 —— irqfd_inject() */
static void
irqfd_inject(struct work_struct *work)
{
	struct kvm_kernel_irqfd *irqfd =
		container_of(work, struct kvm_kernel_irqfd, inject);
	struct kvm *kvm = irqfd->kvm;

	if (!irqfd->resampler) {
		kvm_set_irq(kvm, KVM_USERSPACE_IRQ_SOURCE_ID, irqfd->gsi, 1,
				false);
		kvm_set_irq(kvm, KVM_USERSPACE_IRQ_SOURCE_ID, irqfd->gsi, 0,
				false);
	} else
		kvm_set_irq(kvm, KVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID,
			    irqfd->gsi, 1, false);
}
```

> `grep -rn "kvm" drivers/vhost/*.c` → **0 命中**：vhost 模块不引用任何 KVM 符号，
> 两侧唯一的耦合点就是"同一个 eventfd"（QEMU 用 `KVM_IRQFD` 把它绑成 gsi 的中断源）。
> vCPU 的唤醒同样发生在 KVM 侧的中断注入路径里，不在 vhost 里。

完整链条：

```
vhost_add_used_n()            写 used ring + idx
  └→ vhost_signal()           eventfd_signal(vq->call_ctx.ctx)
       └→ KVM irqfd 被唤醒     virt/kvm/eventfd.c:272 irqfd_update() / :41 irqfd_inject()
            └→ kvm_set_irq()  拉高再拉低该 gsi
                 └→ APICv / Posted Interrupt 或 VM-Exit 注入 → Guest 处理 used ring
```

---

## 5. 批处理与调度权重（为什么单流也能跑满）

| 机制 | 代码 | 作用 |
|---|---|---|
| 攒够再写 used | `VHOST_NET_BATCH`（`net.c:767`、`:1221`） | 一次 `vhost_add_used_n()` 写多个 entry，减少 used ring 上的原子更新与通知次数 |
| 权重上限 | `vhost_exceeds_weight()`（`net.c:831`、`:1227`） | 按包数 `weight` / 字节数 `byte_weight` 让出 CPU，防止一个 vq 饿死别人 |
| 抑制 kick | `vhost_disable_notify()` / `vhost_enable_notify()`（`net.c:959`、`:779-782`） | 处理期间 guest 的新增描述符不必再触发一次 eventfd |
| 空队列时重新挂 | `vhost_net_enable_vq()`（`net.c:799`、`:820`、`:1232`） | 没活干了才重新允许通知，避免忙等 |
| busyloop 补偿 | `busyloop_intr` → `vhost_poll_queue()`（`net.c:777-778`、`:1153-1154`） | 抑制窗口内漏掉的 kick 由 vhost 自己补排一次 |

---

## 6. 与 QEMU 用户态后端的差异（一张表）

| 环节 | QEMU 用户态 virtio | vhost-net |
|---|---|---|
| guest kick | VM-Exit →（可能回 QEMU 模拟指令） | VM-Exit → KVM ioeventfd → `eventfd_signal(vq->kick)` |
| 读 avail/desc | QEMU 线程 `pread` guest 内存 | `vhost_get_vq_desc()` + `vq->umem`(vhost_iotlb) 翻译 |
| 收发 | QEMU 走 tap fd 的 `read()/write()` | `sock->ops->sendmsg()` / `recvmsg()`（`net.c:816/:1185`） |
| 写 used | QEMU 写 guest 内存 | `vhost_add_used_n()` |
| 注入中断 | QEMU → `KVM_SIGNAL_MSI`/irqfd | `eventfd_signal(vq->call_ctx.ctx)` → **KVM** irqfd → `kvm_set_irq()` |
| 谁在跑 | QEMU vCPU/IO 线程 | vhost worker（`vhost_task` 或 kthread，见 annotations.md §4） |

机制差异一句话：**vhost 缩短的是"数据面在内核里闭环"这条路径，中断与 kick 仍然靠同两个 eventfd
和 KVM 对接**；要连"中断也不进 KVM"都省掉，那是 phase7 的 VT-d Posted Interrupts（硬件直投）。
