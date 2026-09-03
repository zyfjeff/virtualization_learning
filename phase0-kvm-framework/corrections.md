# Phase 0 勘误记录

## 修正1：HLT 处理流程描述错误

**日期**: 2026-09-03  
**发现者**: 用户反馈  
**影响范围**: README.md, kvm-framework.md

### 错误描述

文档中多处描述 HLT 会导致 `ioctl` 返回 `KVM_EXIT_HLT` 给用户态，由 QEMU 调用 `select/poll` 等待中断。这是**错误的**。

### 正确描述

**默认情况下（`lapic_in_kernel()` 返回 true）**：
1. Guest 执行 HLT → VM-Exit
2. KVM 在 `kvm_emulate_halt()` 中设置 `vcpu->arch.mp_state = KVM_MP_STATE_HALTED`
3. 返回 1，**不返回用户态**
4. `vcpu_run()` 循环检测到 vCPU 不可运行，调用 `vcpu_block()` → `kvm_vcpu_halt()`
5. 在内核态执行 halt-polling + 阻塞等待中断
6. 中断到达后唤醒，继续运行

**只有特殊情况**（如 QEMU 使用 `-kernel-irqchip off` 导致 `lapic_in_kernel()` 返回 false）：
- HLT 才会设置 `exit_reason = KVM_EXIT_HLT` 并返回用户态

### 源码依据

```c
// arch/x86/kvm/x86.c
static int __kvm_emulate_halt(struct kvm_vcpu *vcpu, int state, int reason)
{
    ++vcpu->stat.halt_exits;
    if (lapic_in_kernel(vcpu)) {  // ← 默认返回 true
        if (kvm_vcpu_has_events(vcpu))
            vcpu->arch.pv.pv_unhalted = false;
        else
            vcpu->arch.mp_state = state;
        return 1;  // ← 返回1，继续在内核态处理
    } else {
        vcpu->run->exit_reason = reason;  // ← 只有这个分支才返回用户态
        return 0;
    }
}

// arch/x86/kvm/lapic.h
static inline bool lapic_in_kernel(struct kvm_vcpu *vcpu)
{
    if (static_branch_unlikely(&kvm_has_noapic_vcpu))
        return vcpu->arch.apic;
    return true;  // ← 默认情况返回 true
}
```

### 修正内容

1. **README.md**:
   - 第527行：修改"VMM视角对比"，说明默认情况下 HLT 在内核态处理
   - 第451行：修改 `vcpu_run()` 流程图中的注释，说明 HLT 默认不返回用户空间
   - 第666行：修改对比表格，说明只有特殊配置才会返回 `KVM_EXIT_HLT`

2. **kvm-framework.md**:
   - 第87-128行：修改"用户态VMM"流程图，在 `KVM_EXIT_HLT` 处添加注释说明
   - 第515-538行：修改"halt-polling机制"对比图的标题，明确标注是"特殊配置"

### 教训

描述 KVM 行为时必须区分：
- **默认配置**：绝大多数用户使用的场景
- **特殊配置**：需要显式启用/禁用的场景

不能将特殊配置的行为当作默认行为描述，否则会误导读者对 KVM 性能特性的理解。
