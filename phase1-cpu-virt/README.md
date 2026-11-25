# Phase 1｜Intel VT-x 与 CPU 虚拟化

> Guest 如何既接近原生速度执行，又让 VMM 保留控制权？VT-x 用 root/non-root + VMCS 给出答案。
> 前置：phase0（KVM 框架 / KVM_RUN ioctl）
> 版本基线：Linux 6.12.93 / QEMU 10.1.0-rc2 | Intel SDM Vol.3 Ch 23-29
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)。

---

## 📋 学习目标

**核心问题**：Guest 代码要跑得接近原生快（不能逐条模拟），但 VMM 必须保留控制权
（隔离、资源管理）。VT-x 怎么调和这对矛盾？哪些事件交给硬件自动 trap、哪些要软件补？

完成本阶段后，你应该能够：

1. 理解 VMX Root/Non-root 模式切换机制
2. 掌握 VMCS 结构和关键控制字段
3. 追踪从 ioctl(KVM_RUN) 到 VMENTER 的完整路径
4. 理解 VM-Exit 的分类和处理流程
5. 熟悉 KVM 核心数据结构和回调表
6. 掌握 CPUID 虚拟化机制（CPUID 总是 exit + CPUID Faulting 补充）
7. 理解 MSR Bitmap 和 MSR 虚拟化策略
8. 列举触发 VM-Exit 的指令及 KVM 处理方式
9. 理解 vCPU 唤醒机制：immediate_exit vs 信号，竞态陷阱，三种 VMM 的设计权衡

---

## 📚 前置与衔接

- **前序 phase**：phase0 的 KVM 框架（`kvm_vcpu_ioctl` → `vcpu_run` → `kvm_x86_ops`）。
  本章展开 `kvm_x86_ops` 的 VMX 实现层。
- **VMM 经验衔接**：你在 QEMU 里调 `ioctl(KVM_RUN)` 后阻塞、返回时处理 `kvm_run->exit_reason`。
  本章讲内核侧 `vmx_vcpu_run()` 如何在 root/non-root 间切换、为什么多数 VM-Exit 不必回用户态。

---

## 📂 本章文件

文件是问题链的接力站（§3.2 规则 6）：

```
README 第一性原理（核心问题：速度 vs 控制权怎么调和？）
  │ 引出：硬件 trap 了哪些？VMCS 怎么控制粒度？
  ▼
annotations.md §1-8（VMX 分层 / Entry-Exit 汇编 / Exit 分发 / 特性检测）
  │ 引出：CPUID/MSR/指令这三大信息接口硬件不全自动虚拟化，软件怎么补？
  ▼
cpu-virtualization.md（CPUID 总 exit / MSR Bitmap 逐 MSR / 指令拦截 / vCPU 唤醒）
  │ 链尾：这些机制在真机上长什么样？
  ▼
practice ex1–ex5（取证）── 与直觉矛盾？──→ corrections.md + AGENTS.md #7
```

| 文件 | 回答什么 | 引出什么 → 去哪 |
|------|---------|----------------|
| `README.md` | 核心问题 + 第一性原理 + VMCS 全景 | 硬件 trap 粒度 → [`annotations.md`](annotations.md) |
| [`annotations.md`](annotations.md) | 问题链精读 §1-8（VMX 实现层） | 三大信息接口软件补充 → [`cpu-virtualization.md`](cpu-virtualization.md) |
| [`cpu-virtualization.md`](cpu-virtualization.md) | CPUID / MSR / 指令虚拟化 / vCPU 唤醒 | 真机取证 → [`practice/`](practice/README.md) ex1–ex5 |
| [`practice/`](practice/README.md) | 5 个编号实验（标注验证哪条理论） | 矛盾与待办 → [`corrections.md`](corrections.md) |
| [`corrections.md`](corrections.md) | 概念性错误证据、待审计 | 蒸馏 → AGENTS.md / 本文件陷阱节 |

---

## 📖 学习路线

```
第1步: 定位与目标 — 核心问题：速度 vs 控制权
第2步: 第一性原理与硬件全景 — 本文件「第一性原理与技术全景」
  → 想：为什么 trap-and-emulate 需要硬件？VMCS 控制什么？
第3步: VMM 对比 — 本文件「VMM视角对比」
  → 想：为什么多数 VM-Exit 内核态处理就够了？
第4步: 问题链精读 — annotations.md §1-8，先看下方「问题链精读入口」
  → 顺着引申问题读：Exit 怎么分发？特性怎么按依赖裁剪？
第5步: 软件补充 — cpu-virtualization.md
  → CPUID 为什么总 exit？MSR Bitmap 为什么逐 MSR？vCPU 唤醒竞态
第6步: 动手验证 — practice ex1–ex5（每个取证一条理论）
第7步: 性能与测量 — 指针 → phase10
第8步: 陷阱与勘误 + 自测 — corrections + AGENTS.md #7；深度自测
  → 通过 → phase2（内存虚拟化）
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题

Guest 要执行**真实指令**（接近原生速度），不能逐条模拟；但 VMM 必须能拦截
特权/敏感操作以维持隔离与资源管理。纯 trap-and-emulate 在 x86 上行不通——
有些敏感指令在 ring3 不 trap（经典虚拟化漏洞）。

### 不可变约束

| 约束 | 内容 | 依据 |
|------|------|------|
| 硬件漏洞 | x86 部分敏感指令非特权级执行不 trap，纯软件 trap-and-emulate 不完备 | Intel SDM Vol.3 Ch 23（VMX 动机） |
| 向后兼容 | 不能要求改 Guest OS（区别于半虚拟化） | VT-x 设计目标 |
| 性能 | 模式切换有成本，不能每条指令都 exit | VMCS 控制粒度 |

### 设计空间与取舍

- **全模拟**（TCG）：每条指令翻译，慢但完备 → 对照基线。
- **半虚拟化**：改 Guest 用 hypercall，快但不兼容。
- **硬件辅助 trap-and-emulate（VT-x）**：新增 root/non-root 模式，Guest 在 non-root
  原生执行，**只有 VMCS 配置的事件**才 VM-Exit 回 root。用硬件换速度+控制权的平衡。
- **软件补充**：CPUID/MSR/特权指令三大信息接口硬件不全自动虚拟化
  → CPUID 无条件 exit（SDM 26.1.2）、MSR Bitmap 逐 MSR 粒度、VM-Execution Controls 配指令拦截
  （详见 [`cpu-virtualization.md`](cpu-virtualization.md)）。

### 机制全景：VMX 模式与 VMCS

```
┌──────────────────────────────────────────────────┐
│  VMX Root Mode (VMM)        VMX Non-Root (Guest)  │
│  执行 VMM 代码              执行 Guest 代码        │
│  管理 VMCS                  受 VMCS 控制           │
│  处理 VM-Exit             VMENTER ──► 进入 Guest   │
│                          VM-Exit ◄── 配置事件触发  │
└──────────────────────────────────────────────────┘
```

VMCS 四区（`arch/x86/include/asm/vmx.h`）：

- **Guest State Area**：CR0/CR3/CR4、RSP/RIP/RFLAGS、段寄存器、EFER —— VM-Entry 加载
- **Host State Area**：CR0/CR3/CR4、RSP/RIP —— VM-Exit 恢复
- **VM-Execution Controls**：PIN/CPU_BASED、EXCEPTION_BITMAP、EPT_POINTER、VPID —— 控制哪些事件 exit
- **VM-Exit Information**：EXIT_REASON、EXIT_QUALIFICATION、GUEST_PHYSICAL_ADDRESS —— exit 现场

> VMCS 完整字段布局见旧版讲义存档（git 历史）；关键控制字段在 annotations §4/§5 精读。

---

## 🔍 VMM视角对比

| 方面 | 用户态 VMM (QEMU) | KVM 内核态 |
|------|------------------|-----------|
| VMCS 管理 | ioctl(KVM_SET_SREGS) 等间接设置 | 直接 `vmcs_write()`，零系统调用 |
| VM-Entry/Exit | ioctl(KVM_RUN) 返回用户态 | `vmx_vcpu_run()` 内核态循环 |
| VM-Exit 处理 | 每次回用户态 | 简单 exit（HLT/外部中断）内核态直接处理 |
| 寄存器同步 | ioctl 读/设 | 直接 vmcs_read/vmcs_write |

**为什么这样分工**：模式切换有成本（root↔non-root + 用户↔内核）。把能在内核态闭环的
VM-Exit 留在内核（快速路径），只把需要设备模拟的 exit 回 QEMU（慢速路径）——
这是"速度 vs 控制权"权衡在 exit 处理上的延伸。量化开销见 phase10（本章不复制数字）。

---

## 📖 问题链精读入口

> 完整链 + 答案在 annotations.md 各节；这里给入口与关键引申。

| annotations 节 | 入口问题 | 关键引申问题 | 源码入口 |
|---------------|---------|-------------|---------|
| §1 VMX 架构分层 | KVM 怎么把通用层和 VMX 硬件接起来？ | 为什么用回调表而不是直接调用？ | `vt_x86_ops` — `vmx/main.c` |
| §2 Entry/Exit 汇编 | VMENTER 在哪发生？ | 为什么用汇编而不是 C？ | `vmx_vcpu_run` — `vmx/vmx.c` |
| §3 VM-Exit 分发 | exit 怎么找到处理函数？ | 快速路径 vs 慢速路径怎么分？Bus lock 为什么回用户态？ | `vmx_handle_exit` — `vmx.c:6615` |
| §4 vcpu_vmx 结构 | vmcs01 和 loaded_vmcs 什么关系？ | 嵌套时为什么要多个 VMCS？ | `struct vcpu_vmx` — `vmx/vmx.h` |
| §5 特性检测 | 硬件支持的特性怎么按依赖裁剪？ | 为什么 EPT 要四个条件全满足？ | `vmx_hardware_setup` — `vmx.c:8404` |
| §6-8 参数/初始化/数据结构 | 模块参数怎么映射到特性开关？ | （接 §5） | `vmx.c` 模块参数 |

---

## 🔬 实践练习

5 个实验串成取证链（详见 [`practice/README.md`](practice/README.md)，五段式 §3.5）：

| 编号 | 验证的理论 | 预测 | 入口 |
|------|-----------|------|------|
| ex1 | VMX capability MSR 编码（§5） | 7 个 MSR 的 allowed-0/1 解出特性依赖链 | `practice/ex1-vmx-verify.c` |
| ex2 | MSR Bitmap 逐 MSR 拦截（cpu-virt §2） | 透传 MSR 无 kvm_msr trace，拦截的有 | `practice/ex2-cpuid-fault.c` + `trace-msr-access.sh` |
| ex3 | VM-Exit 快/慢路径（§3） | 不同负载 exit reason 分布不同 | `practice/ex3-msr-test.c` + `profile-vmexit.sh` |
| ex4 | CPUID 总 exit（cpu-virt §1，SDM 26.1.2） | Guest CPUID 必产生 kvm_cpuid trace | `practice/ex4-cpuid-analysis.c` |
| ex5 | halt-polling 自适应（phase0 §7） | 四档 poll 参数改变 halt 延迟/CPU 占用 | `practice/ex5-vmexit-overhead.c` + `trace-vcpu-sched.sh` |

---

## ⚡ 性能与测量

- 跨 phase 实测结论与分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
- 测量方法：[`../phase10-performance/measurement.md`](../phase10-performance/measurement.md)
- 模块参数（存在吗/运行时可改吗）：[`../phase10-performance/parameters.md`](../phase10-performance/parameters.md)
- 本章现状：VPID 收益、VMCS Shadow 收益、VM-Exit 往返开销**无本仓实测**。
  旧讲义的 "VPID 10-15%"、"VMCS Shadow 20-30%"、"~1μs/次" 是无出处推算，
  已登记 corrections 待审计，不得当实测引用。VM-Exit 微基准见 phase10 §2.2（B 级）。

---

## ⚠️ 陷阱与勘误

1. **CPUID 不是"可拦截可不拦截"**：CPUID 在 non-root **无条件** VM-Exit（SDM 26.1.2），
   不存在 "CPUID exiting" 控制位（Secondary Exec bit12 是 INVPCID）。详见 cpu-virtualization.md §1.1。
2. **AGENTS.md #7**：启动 VM 必须 `-enable-kvm -cpu host`，否则静默回退 TCG，
   所有 KVM 追踪实验得出"没有 VM-Exit"的错误结论。
3. **tracefs 全局状态**（AGENTS.md 硬约束 2）：`set_event`/`set_ftrace_filter`/`set_event_pid`
   带 O_TRUNC 的写清掉全部配置；加用 `>>`，清场显式写。

- 本章勘误：[`corrections.md`](corrections.md)

---

## ✅ 阶段检验清单

操作性：

- [ ] `vt_x86_ops` 关键回调及调用时机？
- [ ] `vmx_hardware_setup()` 检测哪些特性、怎么禁用不支持的？
- [ ] `vmx_vcpu_run()` 完整流程？VMENTER 在哪？
- [ ] `vmx_handle_exit()` 怎么按 exit_reason 分发？
- [ ] `vmcs01` 和 `loaded_vmcs` 关系？Guest/Host State 何时加载？
- [ ] EPT Pointer 在 VMCS 怎么配置？与 kvm->arch.eptp 关系？

深度自测（答不出 = 没学会）：

- [ ] **反事实**：如果没有 VMX（只有 ring3），为什么纯 trap-and-emulate 虚拟化 x86 不完备？
- [ ] **不可简化**：MSR Bitmap 为什么不能省成"全拦截"或"全透传"一个开关？
- [ ] **推导**：从"模式切换有成本"推出"为什么多数 VM-Exit 要留在内核态快速路径"。

---

## 🚀 下一步

- **Phase 2（内存虚拟化）**：EPT——VT-x 解决 CPU，EPT 解决内存地址翻译
- **本章进阶**：嵌套虚拟化（vmcs02/VMCS Shadow）、vCPU 唤醒竞态（cpu-virtualization.md）
