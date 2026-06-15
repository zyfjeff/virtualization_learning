# Phase 8：KVM 性能优化深入

> 基于 Linux 6.12.93 源码 | 面向 MicroVM 性能调优专家

---

## 📚 阶段概述

本阶段深入分析 KVM 中所有关键性能优化机制的实现原理，包括：

1. **halt-polling** — 自适应 polling 减少中断延迟
2. **VM-Exit 减少技术** — VPID, APICv, Posted Interrupts, PLE
3. **EPT 性能优化** — 大页映射、访问位跟踪、PML
4. **vCPU 调度** — 抢占通知、vCPU 调度迁移
5. **TSC 同步** — 主时钟、TSC offset 同步

每个机制都从以下维度分析：
- 源码实现（精确行号）
- 性能影响量化
- 可调参数
- 真实 trace events
- **MicroVM 场景下的调优建议**

---

## 🎯 学习要点

### 性能优化的本质
KVM 性能优化的核心是在**延迟**和**吞吐**之间权衡：
- 忙等 (polling) 降低延迟但浪费 CPU
- 阻塞 (blocking) 节省 CPU 但增加延迟
- VM-Exit 减少提升吞吐但可能增加复杂度

### MicroVM 视角的特殊性
MicroVM 场景下：
- VM-Exit 频率更高（最小设备模型 → 更多 MMIO/PIO exit）
- 启动延迟敏感（冷启动场景）
- CPU 利用率敏感（高密度部署）
- 中断延迟敏感（网络存储场景）

---

## 📖 章节导航

| 章节 | 内容 | 关键源码位置 |
|------|------|-------------|
| 1. halt-polling | 自适应 polling 算法 | `virt/kvm/kvm_main.c:3670-3706` |
| 2. VM-Exit 减少 | VPID/APICv/PI/PLE | `arch/x86/kvm/vmx/vmx.c` |
| 3. EPT 优化 | 大页/A-D bit/PML | `arch/x86/kvm/mmu/tdp_mmu.c` |
| 4. vCPU 调度 | 抢占通知/迁移 | `virt/kvm/kvm_main.c:4037-4099` |
| 5. TSC 同步 | 主时钟/offset | `arch/x86/kvm/x86.c` |

---

## 🔧 关键模块参数

```
halt_poll_ns            [uint]   全局 halt-polling 上限 (默认 400000ns = 400μs)
halt_poll_ns_grow       [uint]   增长倍数 (默认 2)
halt_poll_ns_grow_start [uint]   增长起始值 (默认 10000ns = 10μs)
halt_poll_ns_shrink     [uint]   缩小除数 (默认 2)

# VMX 模块 (kvm_intel)
ple_gap                 [uint]   PLE gap 上限 (默认 128)
ple_window              [uint]   PLE 窗口大小 (默认 4096)
ple_window_grow         [uint]   PLE 窗口增长 (默认 2)
ple_window_shrink       [uint]   PLE 窗口缩小 (默认 2)
ple_window_max          [uint]   PLE 窗口最大值 (默认 16384)

ept                     [bool]   启用 EPT (默认 1)
eptad                   [bool]   启用 EPT A/D 位 (默认 1)
pml                     [bool]   启用 PML (默认 1)
```
