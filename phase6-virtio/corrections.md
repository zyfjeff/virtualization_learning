# Phase 6 勘误记录

> **记录范围（AGENTS.md 瘦身策略）**：只记概念性错误 / 独家复盘 / 待办三类。
> 机械性修复只走 git。修正错误必须同步修正原文。

## 勘误 1：标题与交叉引用沿用旧 phase 编号

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置与修正**:
  | 位置 | 错误 | 正确 |
  |---|---|---|
  | `README.md:1` | 标题 "Phase 5：设备虚拟化" | Phase 6 |
  | `README.md:155` | "Phase 5 的正文按主题拆分" | Phase 6 |
  | `annotations.md:1` | 标题 "Phase 5：源码精读注释" | Phase 6 |
  | `annotations.md:142` | "详见 phase6 corrections.md 勘误 4" | phase7。该勘误（IRTE Posted 化不由 KVM-VFIO 组列表触发）在 `../phase7-vfio/corrections.md` 勘误 4，已核对存在 |
- **成因**: phase4-pcie 插入后 virtio 由 phase5 顺延为 phase6，VFIO 由 phase6
  顺延为 phase7，本文未同步。
- **已同步修正**: `README.md`、`annotations.md`

## 勘误 2：性能数字无出处（2026-11-25 登记，待审计）

旧 README 多处无本仓实测出处的推算数字：
- VMM 对比表："~100万 pps / ~300万 pps（3 倍）"、"~10μs / ~3μs"
- 性能瓶颈分析："~5μs/包 / ~1.6μs/包"、"VM-Exit ~1μs"
- 性能优化节：亲和性"10-20%"、中断合并"50%/20%"、大队列"10%"、multi-queue"2-4 倍"

按 AGENTS.md 性能单一来源规则属机制推算（至多 C 级）。phase10 index 已收录 vhost 对比表
为 B 级并带三条限制（不同宿主/单队列/无重复）。

**已做**：README 性能节改纯指针（→ phase10），VMM 对比表删除吞吐/延迟数字行只留机制，
陷阱节删除"100万ppp"数字；机制描述（批处理/亲和性/通知抑制/multi-queue）保留。
**待做**：若要引用收益数字，按 measurement.md 多队列+直连对端+≥3 次重复实测并分级。

**状态**: ⚠️ 待审计

## 勘误 3：practice README 标题旧编号（2026-11-25 已修）

`practice/README.md:1` "Phase 5: vhost 性能实验" → "Phase 6"。已同步修正。

## 勘误 4：vhost 架构 / 数据面两份文档停留在多轮重写之前（2026-11-25 已修）

**发现方式**：本轮抽查 `file:line` + 符号存在性（`grep -w` 全树计数），不是读出来的疑问。

**错误与依据**：

| 原文（旧） | 实际（Linux 6.12.93） | 证据 |
|---|---|---|
| `struct vhost_dev` 含 `control_lock`/`vq_mutex`/`worker_task`/`log_size`/`u64 features`/`struct kvm *kvm` | 六个字段**全部不存在**；真实成员是 `mutex`/`nvqs`(int)/`worker_xa`/`fork_owner` | `drivers/vhost/vhost.h:174-203`；旧名 grep 计数 0 |
| `struct vhost_virtqueue` 含 `int index`/`struct vring desc`/`kick_ctx`/`struct kvm_vcpu *vcpu` | 均不存在；三环是 `__user` 指针（`vring_desc_t __user *desc`），通知字段是 `struct file *kick` + `struct vhost_vring_call call_ctx` | `vhost.h:93-163`（`:100-105`） |
| 把 `call_ctx` 注为"Guest→Host(kick)"、`kick_ctx` 注为"Host→Guest(中断)" | **方向写反**：kick = guest→host，call = host→guest | `vhost.h:104-105` + `vhost.c:3035-3041` |
| `vhost_handle_tx()` / `vhost_handle_rx()` | 无此符号；真实是 `handle_tx`/`handle_rx` | `net.c:945`、`:1092`；两树 0 命中 |
| `eventfd_signal(vq->kick_ctx)` | `eventfd_signal(vq->call_ctx.ctx)` | `vhost.c:3040` |
| TX = `alloc_skb`+`copy_from_user`+`dev_queue_xmit` | 实为 `iov_iter_init(ITER_SOURCE, vq->iov,…)` + `sock->ops->sendmsg()`；`dev_queue_xmit` 在 `net.c` 0 命中 | `net.c:616-626`、`:816` |
| RX 用 `skb_copy_to_vq()`、`while (!vhost_net_buf_is_empty())` | 无该符号；实为 `get_rx_bufs()` + `sock->ops->recvmsg(ITER_DEST)`，循环条件是 `vhost_exceeds_weight()` | `net.c:1145`、`:1177/:1185`、`:1227` |
| `vring_avail_idx()/vring_desc_addr()/vring_used_ring_id()` | 全部 0 命中（3.x 之前的旧 vhost 访问器） | `include/linux/virtio_ring.h`、`drivers/vhost/` |
| "KVM 处理 IO_INSTRUCTION → 识别为 virtio kick → 路由到 vhost 线程" | guest kick 走 **ioeventfd**（`len=0` 的 MMIO 还会进 `KVM_FAST_MMIO_BUS`），KVM 不认识 virtio | `virt/kvm/eventfd.c:981-988`、`arch/x86/kvm/vmx/vmx.c:5815-5834` |

**已同步修正**：`vhost-architecture.md`（整体重写）、`vhost-net-datapath.md`（整体重写）、
`annotations.md`（zerocopy 门控行 + `handle_rx` 代码块）。

**教训**：从旧 README 拆出时标了"正文内容未作改动"的文档 = **未经本仓核查的存量**，
必须按符号存在性（而非"读起来通顺"）复验。判断某符号是否还在，用
`grep -rw <name> <tree> | wc -l`，不要凭命名规律推断。

## 勘误 5：virtio-queue.md 的 split ring 尾部字段与 fast-mmio 前提（2026-11-25 已修）

1. **avail/used ring 尾部字段写反**（原 `:303`/`:310`）。正确：used ring 尾部是
   `avail_event`，avail ring 尾部是 `used_event`。
   **源码引用**: `include/uapi/linux/virtio_ring.h:194-197`（注释 "We publish the used event
   index at the end of the available ring, and vice versa"）
   **规范引用**: virtio-v1.3-csd01.pdf §2.7.6（`struct virtq_avail` 以 `le16 used_event` 结尾）、
   §2.7.8（`struct virtq_used` 以 `le16 avail_event` 结尾）
2. **fast-mmio 的前提是"不带 datamatch"**，原文写"注册 IOEventFD 时指定地址和数据匹配"。
   `virt/kvm/eventfd.c:973-975` 明确 `len==0` 与 `KVM_IOEVENTFD_FLAG_DATAMATCH` 互斥（`-EINVAL`），
   `:984` 只在 `!args->len` 时挂 fast bus。VMM 侧印证：
   **QEMU 10.1.0-rc2** `hw/virtio/virtio-pci.c:344-345`（modern：size `0`、match_data `false`）
   vs `:347`（modern PIO：size 2、true）/`:352`（legacy：size 2、true）/
   `hw/virtio/virtio-mmio.c:52-53`（size 4、true）—— 后三者**都进不了** fast bus。
   ⇒ "virtio-mmio 没有 fast-mmio"的原因不是协议，而是 QEMU 注册方式（共用单地址 + datamatch）。
3. **packed ring 驱动侧判据写反**（原 `:380`）。规范 §2.8.1：驱动**写** AVAIL=wrap/USED=!wrap
   标记可用；"AVAIL 与 USED 不等→可用，相等→已用"。驱动实际检查的是"已用"：
   `drivers/virtio/virtio_ring.c:1648-1658` `is_used_desc_packed()` 返回
   `avail == used && used == used_wrap_counter`。
4. **虚构的同步示例代码**（原 `:653-703`）：`add_used()`/`process_used()`/`notify_guest()`
   在 `drivers/virtio/` 全为 0 命中；`vq->vring.used` 不是成员（是 `vq->split.vring`，
   union 定义见 `virtio_ring.c:154` 起）；`used->idx` 是 `__virtio16` 不是 `u32`
   （`virtio_ring.h:133`）。已换成真实源码（设备侧 `vhost.c:2960-2995` 的
   `smp_wmb()`+`vhost_put_used_idx()`；驱动侧 `virtio_ring.c:817-852` 的
   `more_used_split()`+`virtio_rmb()`）。
5. **布局总量漏了对齐**：`26n+12` 只是三段之和；实际申请量由
   `vring_size(num, align)`（`virtio_ring.h:209-214`）决定，`align = VIRTIO_PCI_VRING_ALIGN
   = 4096`（`include/uapi/linux/virtio_pci.h:93`）⇒ num=256 时 **10246** 而非 6668。
6. `struct kvm_ioeventfd` 引文缺 `__s32 fd` 且字段顺序错（真实顺序 datamatch/addr/len/fd/flags/pad，
   `include/uapi/linux/kvm.h:612-619`）。
7. packed 优势行的 "40%/10-20%" 无实测出处 → 改为定性 + 指向 phase10（并入勘误 2 的待审计）。

## 勘误 6：vhost-user 的内存插槽默认值与特性归属（2026-11-25 已修）

| 原文 | 实际 | 证据 |
|---|---|---|
| "默认最多 **509** 个内存区域" | 默认 **8**；QEMU 上限 **512**；509 是 **libvhost-user 后端**的上限 | QEMU 11.1.0 `hw/virtio/vhost-user.c:44/:49/:2618/:2636`；`subprojects/libvhost-user/libvhost-user.h:39` |
| 特性 22 名 `VHOST_USER_PROTOCOL_F_SHMEM_MAP`，"仅文档定义/需调查" | 真名 `VHOST_USER_PROTOCOL_F_SHMEM`（`SHMEM_MAP` 是**消息**名）；前端+libvhost-user **都已实现** | `include/hw/virtio/vhost-user.h:38`、`hw/virtio/vhost-user.c:3395`、`libvhost-user.c:1643/:1680` |
| 特性 20/21 "已实现（libvhost-user 和 …）" | libvhost-user **只有保留注释**，枚举无该值、无处理 | `libvhost-user.h:73/:74`；`GET_VRING_BASE_INFLIGHT`/`GPA_ADDRESSES` 在 `subprojects/libvhost-user/` 仅命中注释行 |
| SHMEM_MAP/UNMAP 门控写成 `F_SHARED_OBJECT` | 门控是 `F_SHMEM`；`F_SHARED_OBJECT` 门控的是 `vu_add/rm_shared_object()` | `libvhost-user.c:1662/:1695` vs `:1618/:1636` |
| `if (vhost_user_has_protocol_feature(...RESET_DEVICE))` | 漏 `!`：未协商则 `return -ENOSYS` | `hw/virtio/vhost-user.c:1674-1677`（函数头 `:1663`） |
| "QEMU 启动 vhost-user 后端进程" | QEMU **不** fork/exec 后端，只经 chardev 连接；且 client/server 角色不固定 | `hw/virtio/vhost-user.c` 中 `g_spawn|fork()|execvp|posix_spawn` 0 命中（10.1.0-rc2 与 11.1.0 皆然）；`docs/interop/vhost-user.rst:38-39` |
| 特性 13-22 统称"新特性" | 相对基线 **10.1.0-rc2 只有 20/21/22 是新增**（基线止于 `DEVICE_STATE = 19`），且 helper `vhost_user_has_protocol_feature()` 本身 11.1.0 才有（基线 0 命中） | `qemu-10.1.0-rc2/include/hw/virtio/vhost-user.h:34-35` |

**成因**：拿后端库的常量当协议默认值；拿"搜不到符号"当"未实现"（符号名写错 + 版本树选错）。
**教训**：核查"某特性有没有实现"之前，先在**规范原文**里确认符号的准确拼写与归属
（特性名 vs 消息名 vs 后端库私有宏），再分别 grep 前端树、后端库、DPDK 三处。
