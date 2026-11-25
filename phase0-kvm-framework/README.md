# Phase 0｜KVM 框架层

> 为什么 CPU/内存虚拟化在内核、设备模拟在用户态？KVM 怎么调和硬件访问需求与隔离？
> 前置：无（本章是地基）| 版本基线：Linux 6.12.93 / QEMU 10.1.0-rc2
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)。所有后续 phase 建立在本框架上。

---

## 📋 学习目标

**核心问题**：guest 要用硬件虚拟化（VMX 指令需 ring0），但 VMM 要隔离、设备模拟要灵活。
KVM 为什么把 CPU/内存虚拟化放内核、设备模拟留用户态？这条边界怎么划？

1. 画出 `ioctl(KVM_RUN)` 到 `VMENTER` 的完整调用链
2. 解释 `struct kvm` / `struct kvm_vcpu` 关键字段
3. 理解 KVM 并发模型（vCPU 线程、MMU 锁、irq_lock）
4. 掌握 halt-polling 机制（调优结论 → phase10）
5. 分析 memslot 管理与 GPA→HVA→HPA 转换
6. 对比用户态 VMM 与 KVM 内核态实现

---

## 📚 前置与衔接

- **前序 phase**：无。本章是地基。
- **VMM 经验衔接**：你熟悉 QEMU 的 `ioctl(KVM_CREATE_VM/VCPU/RUN)`、mmap kvm_run、
  设备模拟线程。本章讲内核侧收到这些 ioctl 后做什么、为什么这么分层。

---

## 📂 本章文件

问题接力（§3.2 规则 6）：

```
README 第一性原理（核心问题：内核/用户态边界怎么划？）
  │ 引出：ioctl 进来后内核怎么走到 VMENTER？
  ▼
annotations.md（kvm_dev_ioctl → vcpu_enter_guest 全链路 + halt-polling）
  │ 引出：用户态 VMM 和内核到底怎么分工？
  ▼
kvm-framework.md（VMM 视角深度对比：设计/数据流/并发/内存）
  │ 链尾：真机怎么观察这条路径？
  ▼
practice（手工步骤取证）
```

| 文件 | 回答什么 | 引出什么 → 去哪 |
|------|---------|----------------|
| `README.md` | 核心问题 + 第一性原理 + 架构 | ioctl 全链路 → [annotations.md](annotations.md) |
| [annotations.md](annotations.md) | 问题链精读：ioctl→VM/vCPU 创建→运行主循环→halt-polling | VMM 分工 → [kvm-framework.md](kvm-framework.md) |
| [kvm-framework.md](kvm-framework.md) | ★ VMM 视角深度对比：设计差异/数据流/并发/内存 | 取证 → [practice/](practice/README.md) |
| [practice/](practice/README.md) | VM 生命周期/vCPU 调度/memslot（手工步骤） | 矛盾 → [corrections.md](corrections.md) |
| [corrections.md](corrections.md) | 勘误 | 蒸馏 → AGENTS.md |

---

## 📖 学习路线

```
第1步: 核心问题 + 第一性原理 + 架构图（本文件）
第2步: VMM 对比（本文件 + kvm-framework.md）— 边界怎么划
第3步: 问题链精读 annotations §1-7 — ioctl→vcpu_enter_guest→halt-polling
第4步: kvm-framework.md — 并发模型/内存管理深度对比
第5步: practice 手工取证；halt-polling 调优结论 → phase10
第6步: 陷阱勘误；深度自测 → phase1（VT-x 展开 VMX 层）
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题

guest 要执行硬件虚拟化指令（VMXON/VMLAUNCH 需 ring0），但 VMM 本身要被隔离、
设备模拟要灵活易开发。纯用户态 VMM 碰不了 ring0；纯内核设备模拟又不灵活。

### 不可变约束

| 约束 | 内容 |
|------|------|
| 硬件特权 | VMX/EPT 指令需 ring0，用户态碰不了 |
| 隔离 | guest 之间、guest 与宿主之间要隔离 |
| 灵活性 | 设备模型千变万化，用户态开发调试方便 |
| 性能 | VM-Exit 处理不能每次都回用户态（模式切换贵） |

### 设计空间与取舍

- **全用户态**（TCG）：纯模拟，灵活但慢，无硬件加速。
- **全内核**：设备模拟进内核，快但不灵活、难调试。
- **split（KVM）**：内核模块（kvm.ko + kvm-intel.ko）做 CPU/内存虚拟化（需 ring0 + 性能关键），
  QEMU 用户态做设备模拟（灵活）。边界是 `/dev/kvm` ioctl。
- **取舍本质**：硬件访问需求 + 性能关键 → 内核；设备模型灵活性 → 用户态。
  简单 VM-Exit（HLT/外部中断）内核快速路径闭环，复杂的（MMIO/CPUID 设备相关）才回 QEMU。

### 机制全景：三层架构

```
用户态 VMM (QEMU/crosvm)：设备模拟 + 内存管理(mmap+ioctl) + vCPU 线程(ioctl KVM_RUN)
        │ ioctl(KVM_*)
KVM 框架层 (kvm_main.c)：ioctl 分发 + VM/vCPU 生命周期 + memslot/rmap
        │
架构相关层 (x86.c)：vCPU 运行主循环 + 中断注入框架 + MSR/IO 拦截
        │
VMX/SVM 实现层 (vmx.c)：VMCS 管理 + VM-Entry/Exit + Exit 处理
        │
硬件 VT-x/AMD-V
```

ioctl 全链路、VM-Exit 分发、数据结构、并发模型的逐函数精读见
[annotations.md](annotations.md)；VMM 视角深度对比见 [kvm-framework.md](kvm-framework.md)。

---

## 🔍 VMM视角对比

| 方面 | 用户态 VMM (QEMU) | KVM 内核态 |
|------|------------------|-----------|
| VMCS 管理 | ioctl(KVM_SET_SREGS) 间接 | 直接 vmcs_write()，零开销 |
| VM-Exit 处理 | ioctl 返回用户态，上下文切换 | 内核态直接处理，快速路径无切换 |
| 内存映射 | mmap 共享 + ioctl memslot | EPT 直接映射，并发页错误 |
| 中断注入 | ioctl(KVM_INTERRUPT) | 直接写 VMCS VM_ENTRY_INTR_INFO |

**关键洞察**：KVM 设计目标是**最小化 VM-Exit 处理开销**——简单 exit 内核闭环，
复杂 exit 才回 QEMU。深度对比（数据流/并发/内存）见 [kvm-framework.md](kvm-framework.md)。

---

## 📖 问题链精读入口

| annotations 节 | 入口问题 | 关键引申 | 源码入口 |
|---------------|---------|---------|---------|
| §1 ioctl 入口 | /dev/kvm 怎么分发 ioctl？ | 为什么三级 fd（kvm/vm/vcpu）？ | `kvm_dev_ioctl` — `virt/kvm/kvm_main.c` |
| §2-3 创建 VM/vCPU | VM 和 vCPU 怎么建？ | memslot 何时设？ | `kvm_vm_ioctl_create_vcpu` |
| §4-5 运行主循环 | KVM_RUN 怎么走到 VMENTER？ | 为什么内核态循环而不每次回用户态？ | `vcpu_enter_guest` — `arch/x86/kvm/x86.c` |
| §6 可运行性 | vCPU 什么时候能跑？ | 中断/事件怎么挡住运行？ | `kvm_arch_vcpu_runnable` |
| §7 halt-polling | HLT 后为什么先轮询？ | 窗口多大收益饱和？（→ phase10） | `kvm_vcpu_halt` |

---

## 🔬 实践练习

手工步骤取证（详见 [`practice/README.md`](practice/README.md)，五段式 §3.5）：
VM 生命周期 / vCPU 调度观测 / memslot 管理 / 性能对比。
用 `scripts/vm/boot-vm.sh` + ftrace（`kvm_entry`/`kvm_exit`）观测调用链。

---

## ⚡ 性能与测量

- 实测结论与分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
- 参数默认值与权限：[`../phase10-performance/parameters.md`](../phase10-performance/parameters.md)
- **halt-polling**：本仓已实测，结论与"按负载调窗口"的流传说法方向相反
  （空闲零收益、flood 买不到延迟反多付 CPU、窗口刚够盖住典型 halt 即饱和）——
  数值/样本/条件唯一来源 phase10 index §1.2，此处只指针。
- 大页内存"性能提升 10-20%"无本仓实测出处，登记 corrections 待审计。

---

## ⚠️ 陷阱与勘误

1. **AGENTS.md #7**：VM 必须 `-enable-kvm -cpu host`，否则静默 TCG，追踪结论无效。
2. **tracefs 全局状态**（AGENTS.md 硬约束 2）：`set_event`/`set_ftrace_filter`/`set_event_pid`
   带 O_TRUNC 的写清掉全部配置；加用 `>>`，清场显式写。
3. **halt-polling 调优**：别信"按负载类型调窗口"的流传说法，本仓实测方向相反（→ phase10）。

本章勘误：[`corrections.md`](corrections.md)

---

## ✅ 阶段检验清单

操作性：

- [ ] 能画出 ioctl(KVM_RUN)→VMENTER 调用链
- [ ] 能解释 struct kvm / kvm_vcpu 关键字段
- [ ] 能说明 memslot 与 GPA→HVA→HPA 转换
- [ ] 能用 ftrace 观测 kvm_entry/kvm_exit

深度自测（答不出 = 没学会）：

- [ ] **反事实**：如果设备模拟也放内核，会失去什么？为什么 QEMU 留用户态？
- [ ] **不可简化**：为什么简单 VM-Exit 不能也回用户态处理（统一路径更简单）？
- [ ] **推导**：从"VMX 需 ring0 + 模式切换贵"推出"CPU 虚拟化必须内核 + 快速路径闭环"。

---

## 🚀 下一步

- **Phase 1（VT-x）**：展开 VMX 实现层（vmx.c）——本章的架构相关层往下
- **本章进阶**：并发模型（SRCU/mmu_lock）、内存管理深度对比（kvm-framework.md）
