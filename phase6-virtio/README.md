# Phase 6｜设备虚拟化：从 Virtio Queue 到 vhost

> 数据面为什么要从 QEMU 用户态卸载到内核（vhost）甚至专用用户态进程（vhost-user）？
> 前置：phase0（KVM 框架 / ioeventfd/irqfd）、phase5（中断注入）
> 版本基线：Linux 6.12.93 / QEMU 10.1.0-rc2（vhost-user 新特性核查基于 11.1.0 + DPDK）
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)。

---

## 📋 学习目标

**核心问题**：virtio 把设备模拟标准化了，但每个包都要 VM-Exit + 用户态往返。
数据面能不能绕过 QEMU？vhost（内核）和 vhost-user（专用进程）各卸载了什么、代价是什么？

1. **Virtio Queue 基础**：三环结构（描述符表/Avail/Used）、数据流、Split vs Packed、
   通知抑制（EVENT_IDX）、间接描述符、Fast MMIO、内存屏障同步
2. **vhost 内核态加速**：数据面卸载架构、vhost-net 收发路径、ioeventfd/irqfd 与 KVM 协作
3. **vhost-user 协议**：用户态 vhost、消息流程、DPDK/SPDK 后端
4. **实践**：QEMU 用户态 / vhost-net / vhost-user 性能对比（指针 → phase10）

---

## 📚 前置与衔接

- **前序 phase**：phase0 的 KVM 框架（ioeventfd/irqfd 是 vhost 与 KVM 协作的桥梁）；
  phase5 的中断注入（vhost 直接调 kvm_set_irq）。
- **VMM 经验衔接**：你在 QEMU 里用 `-netdev tap,vhost=on` 开启过 vhost。
  本章讲内核 vhost 线程如何接管数据面、为什么不再回 QEMU。

---

## 📂 本章文件

问题接力（§3.2 规则 6）：

```
README 第一性原理（核心问题：数据面为什么卸载？）
  │ 引出：virtqueue 怎么让驱动和设备无锁协作？
  ▼
virtio-queue.md（三环 + 通知抑制）
  │ 引出：数据面怎么从 QEMU 搬进内核？
  ▼
vhost-architecture.md → vhost-net-datapath.md（架构 + 收发路径）
  │ 引出：内核还不够快怎么办？搬进专用用户态进程？
  ▼
vhost-user-basics.md → protocol-latest.md → new-features-*.md
  │ 链尾：真机吞吐/延迟差多少？
  ▼
practice（vhost-perf-test.sh 取证）── 数字 → phase10 分级
```

| 文件 | 回答什么 | 引出什么 → 去哪 |
|------|---------|----------------|
| `README.md` | 核心问题 + 第一性原理 + 源码路线 | virtqueue 机制 → [virtio-queue.md](virtio-queue.md) |
| [virtio-queue.md](virtio-queue.md) | 三环结构、Kick/Notify、Split/Packed、通知抑制 | 数据面卸载 → [vhost-architecture.md](vhost-architecture.md) |
| [vhost-architecture.md](vhost-architecture.md) | vhost 架构与核心数据结构 | 收发路径 → [vhost-net-datapath.md](vhost-net-datapath.md) |
| [vhost-net-datapath.md](vhost-net-datapath.md) | vhost-net TX/RX 数据路径 | 用户态卸载 → [vhost-user-basics.md](vhost-user-basics.md) |
| [vhost-user-basics.md](vhost-user-basics.md) | vhost-user 消息流程、DPDK 后端 | 协议细节 → [protocol-latest.md](vhost-user-protocol-latest.md) |
| [vhost-user-new-features-factcheck-v2.md](vhost-user-new-features-factcheck-v2.md) | 新特性核查（QEMU 11.1.0 + DPDK） | 场景 → [usecases.md](vhost-user-new-features-usecases.md) |
| [annotations.md](annotations.md) | 问题链精读：vhost 关键路径 | 取证 → [practice/](practice/README.md) |
| [corrections.md](corrections.md) | 勘误 | 蒸馏 → AGENTS.md |

---

## 📖 学习路线

```
第1步: 核心问题 + 第一性原理（本文件）
第2步: virtio-queue.md — 三环怎么无锁协作
第3步: VMM 对比（本文件）— 数据面位置/系统调用/内存访问/中断注入
第4步: 问题链精读 annotations + vhost-architecture/net-datapath
第5步: vhost-user-basics/protocol — 用户态卸载
第6步: practice vhost-perf-test.sh 取证；性能数字 → phase10
第7步: 陷阱勘误；深度自测 → phase7（VFIO 直通，另一条设备虚拟化路线）
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题

virtio 标准化了设备接口（guest 驱动通用），但**用户态后端**每个包要：
Guest kick → VM-Exit → KVM → QEMU 用户态处理 → TAP → 物理网卡。
模式切换 + 系统调用 + 线程调度是每包固定开销，高吞吐下成为瓶颈。

### 不可变约束

| 约束 | 内容 |
|------|------|
| 通知必须跨边界 | guest kick 要通知后端，VM-Exit 省不掉（除非通知抑制） |
| 内存要共享 | 后端要访问 guest 内存（virtqueue + 缓冲），需 GPA→HVA 映射 |
| 中断要回 guest | 后端完成处理要注入中断，经 KVM |
| 标准兼容 | 卸载不能改 virtio 接口（guest 驱动无感） |

### 设计空间与取舍

- **QEMU 用户态后端**：灵活、易调试、支持复杂模拟；但每包模式切换开销。
- **vhost（内核）**：数据面搬进内核线程，kick 经 ioeventfd 直达、中断经 irqfd/kvm_set_irq
  直达，**省掉用户态往返**；代价是只支持标准 virtio 设备、调试难。
- **vhost-user（专用用户态进程）**：数据面搬进 DPDK/SPDK 进程， polled-mode 无中断、
  大页内存；代价是协议复杂、需共享内存设置。
- **取舍本质**：用"通用性/可调试性"换"数据面路径长度"。三者 guest 驱动完全相同。

### 机制全景：三种后端的数据面

```
QEMU 用户态:  Guest → VM-Exit → KVM → QEMU → TAP → NIC   （每包用户态往返）
vhost-net:    Guest → VM-Exit → KVM → vhost 内核线程 → TAP → NIC （内核态闭环）
vhost-user:   Guest → 共享内存 → DPDK 进程（polled）→ NIC  （无 VM-Exit 数据面）
```

详见 [virtio-queue.md](virtio-queue.md)（三环）与 [vhost-architecture.md](vhost-architecture.md)。

---

## 🔍 VMM视角对比

| 方面 | 用户态 virtio (QEMU) | vhost 内核态 | vhost-user |
|------|---------------------|-------------|-----------|
| 数据面位置 | QEMU 用户态线程 | vhost 内核线程 | DPDK/SPDK 进程 |
| 系统调用 | 每包 VM-Exit + ioctl | 0（全程内核态） | 0（polled 共享内存） |
| 内存访问 | mmap guest 内存 | KVM memslot 直接访问 | 共享内存（大页） |
| 中断注入 | ioctl(KVM_INTERRUPT) | 直接 kvm_set_irq() | irqfd / 无中断 |

**为什么这样分工**：数据面路径越短越快，但通用性/可调试性越差。
**量化吞吐/延迟差异见 phase10**（本章不复制数字；旧讲义的 "3 倍/100万 pps" 等无出处推算已登记待审计）。

---

## 📖 问题链精读入口

| 材料 | 入口问题 | 关键引申 | 源码入口 |
|------|---------|---------|---------|
| annotations + vhost-architecture | vhost 怎么接管 virtqueue？ | 为什么用 ioeventfd 而不是 ioctl？ | `vhost_dev_init` — `drivers/vhost/vhost.c` |
| vhost-net-datapath | 一个包怎么从 avail ring 到 NIC？ | handle_tx/handle_rx 怎么批处理？ | `handle_tx`/`handle_rx` — `drivers/vhost/net.c` |
| virtio-queue | 驱动和设备怎么无锁协作？ | 通知抑制为什么能省 VM-Exit？ | 三环结构（virtio spec） |
| vhost-user-basics | 数据面怎么搬进用户态进程？ | 共享内存怎么设置？消息怎么协商？ | vhost-user 协议 |

源码路线：vhost.c（核心）→ net.c（vhost-net）→ vhost 与 KVM 交互
（GPA→HVA via memslot、kvm_set_irq、kvm_vcpu_kick）。

---

## 🔬 实践练习

取证实验（详见 [`practice/README.md`](practice/README.md)，五段式 §3.5）：

| 编号 | 验证的理论 | 预测 | 入口 |
|------|-----------|------|------|
| ex1 | vhost 卸载数据面（VMM 对比） | vhost=on 时存在 vhost 内核线程，数据面不回 QEMU | `practice/vhost-perf-test.sh` |

性能对比（QEMU/vhost-net/vhost-user）的吞吐/延迟数字 → phase10 分级，本章只取证机制。

---

## ⚡ 性能与测量

- 实测结论与分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
  （vhost 对比表为 B 级，带"不同宿主/单队列/无重复"三条限制）
- 测量方法：[`../phase10-performance/measurement.md`](../phase10-performance/measurement.md)
- 本章现状：旧讲义的吞吐/延迟数字（"3 倍"、"100万/300万 pps"、"~5μs/包"、
  亲和性"10-20%"、中断合并"50%"、multi-queue"2-4 倍"）**无本仓实测出处**，
  已登记 corrections 待审计。机制（批处理/亲和性/通知抑制/multi-queue）保留，数字标待实测。

---

## ⚠️ 陷阱与勘误

1. **vhost 未启用**：忘 `vhost=on` → 走 QEMU 用户态后端。检查 `ps aux | grep vhost` 有无内核线程。
2. **vhost 线程未绑亲和性**：线程迁移 → TLB 刷新/缓存污染 → 延迟抖动。`taskset` 绑定。
3. **中断合并未配置**：每包中断 → CPU 占用高。增大队列尺寸 / 通知抑制。
4. **Guest 未启用多队列**：多 vCPU 单 RX 队列 → 单核瓶颈。`mq=on,vectors=2*N+2` + `ethtool -L`。
5. **AGENTS.md #7**：VM 必须 `-enable-kvm -cpu host`，否则 TCG 下所有追踪结论无效。

本章勘误：[`corrections.md`](corrections.md)

---

## ✅ 阶段检验清单

操作性：

- [ ] 解释 vhost 怎么把数据面从 QEMU 卸载到内核
- [ ] 画出 vhost-net TX/RX 数据路径
- [ ] 说明 vhost 怎么访问 guest 内存（memslot）、怎么注入中断（kvm_set_irq）
- [ ] 说明何时用 vhost、何时用 QEMU 用户态

深度自测（答不出 = 没学会）：

- [ ] **反事实**：如果 guest kick 不需要 VM-Exit（通知抑制生效），vhost 相对 QEMU 的优势还剩什么？
- [ ] **不可简化**：vhost-user 为什么不能省掉共享内存设置，直接像 vhost 那样用 memslot？
- [ ] **推导**：从"每包模式切换是固定开销"推出"高吞吐下数据面必须缩短路径"。

---

## 🚀 下一步

- **Phase 7（VFIO 直通）**：另一条设备虚拟化路线——真实设备直接给 guest，对比 virtio 半虚拟化
- **本章进阶**：vhost-user 新特性（factcheck-v2）、DPDK/SPDK 后端

---

## 📚 参考资料

- Linux 源码 `drivers/vhost/vhost.c`、`drivers/vhost/net.c`
- vhost 设计论文 *"vhost: A Virtualization Infrastructure Driver"*
- virtio spec：`virtio-v1.3-csd01.pdf`（仓库内）
