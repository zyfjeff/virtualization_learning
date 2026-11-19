# AMD AVIC 丢中断问题分析

> 分析 AMD AVIC 在硬件层面和软件层面可能存在的丢中断问题

---

## 📋 问题背景

用户报告：**如果 vIRR 里已经有未处理的中断，硬件就不会发 GA Log**。

这可能导致丢中断的场景：
1. vCPU 正在 blocking
2. 第一个中断到达，设置 vIRR[vec1]=1，写入 GA Log
3. KVM 还没来得及处理 GA Log
4. 第二个中断到达（相同或不同向量），设置 vIRR[vec2]=1
5. 因为 vIRR 已经有 pending 中断，硬件不再写入 GA Log
6. KVM 处理第一个 GA Log entry，唤醒 vCPU
7. vCPU 运行，处理 vec1
8. 如果 vec2 的 GA Log 没有写入，vec2 可能被遗漏？

---

## 🔍 规范分析

### AMD IOMMU 规范关于 GA Log 的描述

**规范引用**: amd-virtualization.pdf, Section 2.2.5.2, Section 2.7

```
步骤 9 (GuestMode=1 时的处理):
  1. 计算 vIRR 位偏移
  2. 原子设置 vIRR[Vector]=1
  3. 读取 IRTE
  4. 如果 IsRun=0 且 GALogIntr=1，创建 GA Log entry
  5. 如果 IsRun=1，发送 doorbell
```

**关键点**:
- 规范**没有**说 "如果 vIRR 位已经是 1 就不写 GA Log"
- 原子 OR 操作：即使位已经是 1，操作仍然成功
- GA Log 写入应该总是发生（如果 IsRun=0 且 GALogIntr=1）

### 可能的硬件行为（推测）

如果用户观察到 "IRR 有未处理中断时不写 GA Log"，可能是：

1. **硬件优化**: IOMMU 检查 vIRR 位，如果已经是 1，跳过 GA Log 写入
   - 理由：既然 vIRR 已经有 pending 中断，vCPU 应该很快会被唤醒
   - 风险：如果 vCPU 没有被唤醒（GA Log 丢失），中断会延迟

2. **GA Log 去重**: 对于同一个 vCPU，GA Log 只保留一个 entry
   - 多个中断到达时，只写入第一个 GA Log entry
   - 后续中断只更新 vIRR，不重复写 GA Log
   - 这实际上是**正确的行为**，因为 vCPU 被唤醒后会处理所有 pending 中断

---

## 🐛 潜在问题场景分析

### 场景 1: GA Log 去重（正常行为）

```
时间线:
────────────────────────────────────────────────────────────────────

t1: vCPU blocking, IsRun=0, vIRR=0
t2: 设备 A 发中断 vec1
    → IOMMU: vIRR[vec1]=1, GA Log entry #1 (GATag=X)
t3: 设备 B 发中断 vec2 (相同 GATag)
    → IOMMU: vIRR[vec2]=1, GA Log entry #2? (可能跳过)
t4: KVM 处理 GA Log entry #1
    → avic_ga_log_notifier(GATag=X)
    → kvm_vcpu_wake_up(vcpu)
t5: vCPU 被唤醒，运行
    → VM-Entry 评估 vIRR
    → 处理 vec1 和 vec2

结论: 如果 GA Log 去重，这是正确的行为！
      vCPU 被唤醒后会处理所有 pending 中断。
```

### 场景 2: GA Log 丢失（潜在问题）

```
时间线:
────────────────────────────────────────────────────────────────────

t1: vCPU blocking, IsRun=0, vIRR=0
t2: 设备 A 发中断 vec1
    → IOMMU: vIRR[vec1]=1, GA Log entry #1
t3: KVM 处理 GA Log entry #1
    → kvm_vcpu_wake_up(vcpu)
    → vCPU 进入 runnable 状态但还没运行
t4: vCPU 被调度，但还没进入 Guest (在 KVM 内核态)
    → 此时 vIRR[vec1]=1 但 vCPU 还没 VM-Entry
t5: 设备 B 发中断 vec2
    → IOMMU: vIRR[vec2]=1
    → 如果 IsRun 还是 0 (vCPU 还没进入 non-root mode)
    → 应该写 GA Log entry #2
    → 但如果硬件检查 vIRR != 0 就跳过，GA Log 丢失
t6: vCPU 进入 Guest
    → 处理 vec1
    → 如果 vec2 没有 GA Log，但 vIRR[vec2]=1 已经设置
    → vCPU 会继续处理 vec2 (因为 vIRR 已设置)

结论: 即使 GA Log 丢失，只要 vIRR 正确设置，中断不会丢失。
```

### 场景 3: Race Condition（真正的问题）

```
时间线:
────────────────────────────────────────────────────────────────────

t1: vCPU 正在运行, IsRun=1
t2: vCPU 执行 HLT 指令, 触发 VM-Exit
t3: KVM 处理 HLT VM-Exit
t4: KVM 调用 avic_vcpu_blocking()
t5: avic_vcpu_put() 设置 IsRun=0
    → 更新 IRTE: is_run=0
    → 清除 Physical ID entry 的 IS_RUNNING_MASK
t6: 在 t4-t5 之间，设备发中断
    → IsRun 还是 1 (还没更新)
    → IOMMU 发送 doorbell
    → 但 vCPU 即将 blocking，doorbell 可能丢失！

Linux 代码中的防护 (arch/x86/kvm/svm/avic.c:1206-1217):
────────────────────────────────────────────────────────────────────
/*
 * Any IRQs that arrive before IsRunning=0 will not cause an
 * incomplete IPI vmexit on the source, therefore vIRR will also
 * be checked by kvm_vcpu_check_block() before blocking.
 *
 * The memory barrier implicit in set_current_state orders writing
 * IsRunning=0 before reading the vIRR.
 */
avic_vcpu_put(vcpu);  // 先设置 IsRun=0
────────────────────────────────────────────────────────────────────

t7: KVM 调用 kvm_vcpu_check_block()
    → 检查 kvm_arch_vcpu_runnable()
    → 检查 kvm_apic_has_interrupt()
    → 读取 vIRR (包括 t6 设置的中断)
    → 如果有中断，不 blocking，返回运行

结论: Linux 代码已经处理了这个 race condition！
      通过先设置 IsRun=0，再检查 vIRR。
```

### 场景 4: Errata #1235（硬件 bug）

```
Linux 代码注释 (arch/x86/kvm/svm/avic.c:1214-1216):
────────────────────────────────────────────────────────────────────
/*
 * The processor needs a matching memory barrier on interrupt delivery
 * between writing IRR and reading IsRunning; the lack of this barrier
 * might be the cause of errata #1235).
 */
────────────────────────────────────────────────────────────────────

含义:
  · IOMMU 写入 vIRR
  · IOMMU 读取 IsRun
  · 如果这两个操作之间没有内存屏障
  · 可能看到旧的 IsRun 值
  · 导致错误的行为 (doorbell vs GA Log 选择错误)

可能的症状:
  · 中断到达时，IsRun 应该是 0，但 IOMMU 看到 1
  · IOMMU 发送 doorbell 而不是写 GA Log
  · 但 vCPU 即将 blocking，doorbell 丢失
  · 中断丢失！

这是硬件 bug，软件无法完全规避。
Linux 的防护: 尽量确保 IsRun 更新的顺序正确。
```

---

## 🔬 代码分析

### KVM 中的防护措施

**1. Blocking 前检查 vIRR**

```c
// arch/x86/kvm/svm/avic.c:1200-1217
void avic_vcpu_blocking(struct kvm_vcpu *vcpu)
{
    // ...
    avic_vcpu_put(vcpu);  // 先设置 IsRun=0
}

// virt/kvm/kvm_main.c:3748
for (;;) {
    set_current_state(TASK_INTERRUPTIBLE);
    if (kvm_vcpu_check_block(vcpu) < 0)  // 检查 vIRR
        break;
    waited = true;
    schedule();
}
```

**2. 检查 vIRR 的实现**

```c
// arch/x86/kvm/x86.c:11267
int kvm_arch_vcpu_runnable(struct kvm_vcpu *vcpu)
{
    return kvm_vcpu_running(vcpu) || kvm_vcpu_has_events(vcpu);
}

// arch/x86/kvm/x86.c
static bool kvm_vcpu_has_events(struct kvm_vcpu *vcpu)
{
    // ...
    if (kvm_arch_interrupt_allowed(vcpu) && kvm_cpu_has_interrupt(vcpu))
        return true;
    // ...
}

// arch/x86/kvm/irq.c
int kvm_cpu_has_interrupt(struct kvm_vcpu *v)
{
    // ...
    return kvm_apic_has_interrupt(v) != -1;  // 检查 LAPIC IRR
}

// arch/x86/kvm/lapic.c
int kvm_apic_has_interrupt(struct kvm_vcpu *vcpu)
{
    // ...
    return apic_has_interrupt_for_ppr(apic, ppr);
}

// arch/x86/kvm/lapic.c
static int apic_has_interrupt_for_ppr(struct kvm_lapic *apic, u32 ppr)
{
    int highest_irr;
    if (kvm_x86_ops.sync_pir_to_irr)
        highest_irr = kvm_x86_call(sync_pir_to_irr)(apic->vcpu);
    else
        highest_irr = apic_find_highest_irr(apic);  // 读取 vIRR
    // ...
}
```

**3. GA Log 处理**

```c
// arch/x86/kvm/svm/avic.c:186
int avic_ga_log_notifier(u32 ga_tag)
{
    // 解析 GATag 找到 vCPU
    u32 vm_id = AVIC_GATAG_TO_VMID(ga_tag);
    u32 vcpu_id = AVIC_GATAG_TO_VCPUID(ga_tag);
    
    // 查找 vCPU
    vcpu = kvm_get_vcpu_by_id(&kvm_svm->kvm, vcpu_id);
    
    // 唤醒 vCPU
    if (vcpu)
        kvm_vcpu_wake_up(vcpu);
    
    return 0;
}
```

---

## ⚠️ 潜在问题总结

### 问题 1: GA Log 去重（可能是设计）

**现象**: 如果 vIRR 已有 pending 中断，新中断不写 GA Log

**影响**: 
- 如果去重是正确的（同一 vCPU 只保留一个 GA Log entry），则无问题
- vCPU 被唤醒后会处理所有 pending 中断

**验证方法**:
- 测试场景：vCPU blocking，连续发送多个中断
- 观察：是否只有一个 GA Log entry，但所有中断都被处理

### 问题 2: Errata #1235（硬件 bug）

**现象**: IOMMU 在写入 vIRR 和读取 IsRun 之间没有内存屏障

**影响**:
- 可能看到旧的 IsRun 值
- 导致 doorbell/GA Log 选择错误
- 可能丢失中断

**缓解措施**:
- Linux 代码尽量确保 IsRun 更新顺序正确
- 无法完全规避硬件 bug

### 问题 3: Race Condition (IsRun 更新时序)

**现象**: vCPU blocking 过程中，IsRun 从 1 变为 0

**影响**:
- 在这个窗口期到达的中断可能选择错误的路径
- doorbell 被发送，但 vCPU 即将 blocking

**缓解措施**:
- Linux 代码先设置 IsRun=0，再检查 vIRR
- 内存屏障确保顺序正确

---

## 🧪 测试建议

### 测试 1: 验证 GA Log 去重行为

```bash
# 在 Guest 中运行
# 1. 停止 vCPU (HLT)
# 2. 从 Host 向直通设备发送多个中断
# 3. 观察 GA Log 中有多少 entry
# 4. 验证所有中断是否都被处理

# 预期结果:
# - GA Log 可能只有一个 entry (去重)
# - 但所有中断都应该被 Guest 处理
```

### 测试 2: 验证 Race Condition 防护

```bash
# 高并发测试
# 1. 多个 vCPU 同时 blocking/unblocking
# 2. 大量中断到达
# 3. 观察是否有中断丢失

# 工具:
# - ftrace 追踪 avic_vcpu_blocking/unblocking
# - perf 统计中断数量
```

### 测试 3: 检查 vIRR 同步

```bash
# 验证 kvm_apic_has_interrupt() 能正确读取 vIRR
# 1. 设置 vIRR 位 (通过 IOMMU)
# 2. 在 blocking 前检查 kvm_vcpu_check_block()
# 3. 验证是否检测到 pending 中断
```

---

## 📚 结论

1. **GA Log 去重可能是正常行为**
   - 同一 vCPU 的多个中断只需要一个 GA Log entry
   - vCPU 被唤醒后会处理所有 pending 中断
   - 这不是丢中断，而是优化

2. **真正的风险是 Errata #1235**
   - 硬件内存屏障问题
   - 可能导致 doorbell/GA Log 选择错误
   - Linux 代码已尽量防护，但无法完全规避

3. **Race Condition 已被处理**
   - Linux 代码先设置 IsRun=0，再检查 vIRR
   - 内存屏障确保顺序正确

4. **需要进一步验证**
   - 用户提到的 "IRR 有未处理中断时不写 GA Log" 需要验证
   - 如果是去重行为，则无问题
   - 如果是真正的丢失，需要找到 workaround

---

## 🔗 参考资料

**规范引用**:
- amd-virtualization.pdf, Section 2.2.5.2 (IRTE GuestMode=1)
- amd-virtualization.pdf, Section 2.7 (GA Logging)

**源码引用**:
- arch/x86/kvm/svm/avic.c:186 (avic_ga_log_notifier)
- arch/x86/kvm/svm/avic.c:1200 (avic_vcpu_blocking)
- drivers/iommu/amd/iommu.c:1031 (iommu_poll_ga_log)
