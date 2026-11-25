# vCPU 迁移与中断亲和性变更

> Phase 5 深度主题 | vCPU 调度、迁移、亲和性变更场景下的中断虚拟化机制
>
> 前置阅读：
> - [posted-interrupts.md](posted-interrupts.md) — Intel VT-d PI 基础
> - [amd-avic.md](amd-avic.md) — AMD AVIC 基础
> - [msi-affinity-migration.md](msi-affinity-migration.md) — 设备 MSI 亲和性迁移

---

## 📋 学习目标

理解 vCPU 调度、迁移、亲和性变更场景下，中断虚拟化机制的处理逻辑：

1. **Intel VT-d PI**：
   - vCPU 调度 (load/put) 时 PI Descriptor 的更新
   - vCPU 迁移时 NDST 和 IRTE Dest_ID 的更新
   - 中断亲和性变更的处理
   
2. **AMD AVIC**：
   - vCPU 调度时 Physical APIC table 和 IRTE 的更新
   - vCPU 迁移时 Destination 和 IsRun 的更新
   - GA Log 在调度决策中的作用

3. **对比分析**：
   - Intel vs AMD 的实现差异
   - 性能影响和优化策略

---

## 🎯 第一部分：Intel VT-d PI 的 vCPU 调度

### 1.1 vCPU Load (调度入)

**源码引用**: `arch/x86/kvm/vmx/posted_intr.c:53`

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:53 */
void vmx_vcpu_pi_load(struct kvm_vcpu *vcpu, int cpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    struct pi_desc old, new;
    unsigned long flags;
    unsigned int dest;

    if (!enable_apicv || !lapic_in_kernel(vcpu))
        return;

    /* 快速路径：没迁移 + 不在 wakeup 列表 → 只需清 SN */
    if (pi_desc->nv != POSTED_INTR_WAKEUP_VECTOR && vcpu->cpu == cpu) {
        if (pi_test_and_clear_sn(pi_desc))
            goto after_clear_sn;
        return;  /* 无需更新 NDST，vCPU 在同一 pCPU */
    }

    local_irq_save(flags);

    /* 慢路径：可能迁移了，或者从 wakeup 列表恢复 */
    
    /* 如果 vCPU 之前阻塞在 wakeup 列表，移除它 */
    if (pi_desc->nv == POSTED_INTR_WAKEUP_VECTOR) {
        raw_spin_lock(&per_cpu(wakeup_vcpus_on_cpu_lock, vcpu->cpu));
        list_del(&vmx->pi_wakeup_list);
        raw_spin_unlock(&per_cpu(wakeup_vcpus_on_cpu_lock, vcpu->cpu));
    }

    /* 更新 NDST = 当前 pCPU 的物理 APIC ID */
    dest = cpu_physical_id(cpu);
    if (!x2apic_mode)
        dest = (dest << 8) & 0xFF00;

    /* 原子更新整个 control 字段（NDST + NV + ON + SN） */
    old.control = READ_ONCE(pi_desc->control);
    do {
        new.control = old.control;
        __pi_clear_sn(&new);      /* 清除 SN */
        new.ndst = dest;           /* 更新 NDST */
        new.nv = POSTED_INTR_VECTOR;  /* 恢复通知向量 */
    } while (pi_try_set_control(pi_desc, &old.control, new.control));

    local_irq_restore(flags);

after_clear_sn:
    /* 内存屏障，确保在读取 PIR 之前清除 SN */
    smp_mb__after_atomic();

    /* 如果 PIR 非空，触发 KVM_REQ_EVENT 让 vCPU 处理 */
    if (!pi_is_pir_empty(pi_desc))
        pi_set_on(pi_desc);
}
```

**关键操作**：

| 步骤 | 操作 | 目的 |
|------|------|------|
| 1 | 清除 SN 位 | 允许接收通知中断 |
| 2 | 更新 NDST | 通知中断发送到正确的 pCPU |
| 3 | 恢复 NV = POSTED_INTR_VECTOR | 从 wakeup 向量切换回正常通知向量 |
| 4 | 原子更新 control 字段 | 防止 IOMMU 读到半更新状态 |
| 5 | 检查 PIR 非空 | 如果有 pending 中断，触发处理 |

**注意**：vmx_vcpu_pi_load() **只更新 PI Descriptor 的 NDST**，**不更新 IRTE 的 Dest_ID**。IRTE 的更新由 IOMMU 层在需要时完成（见 §1.3）。

### 1.2 vCPU Put (调度出)

**源码引用**: `arch/x86/kvm/vmx/posted_intr.c:196`

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:196 */
void vmx_vcpu_pi_put(struct kvm_vcpu *vcpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);

    if (!vmx_needs_pi_wakeup(vcpu))
        return;

    /* 如果 vCPU 将被阻塞且中断未被屏蔽，启用 wakeup handler */
    if (kvm_vcpu_is_blocking(vcpu) && !vmx_interrupt_blocked(vcpu))
        pi_enable_wakeup_handler(vcpu);

    /* 如果 vCPU 被抢占（不是主动阻塞），设置 SN 位抑制通知 */
    if (vcpu->preempted)
        pi_set_sn(pi_desc);
}
```

**关键逻辑**：

| 场景 | 操作 | 目的 |
|------|------|------|
| vCPU 阻塞 (blocking) | 启用 wakeup handler | 使用 WAKEUP_VECTOR 唤醒 vCPU |
| vCPU 被抢占 (preempted) | 设置 SN=1 | 抑制非紧急中断通知 |
| vCPU 正常调度出 | 无特殊操作 | - |

**Wakeup Handler 机制**：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:146 */
static void pi_enable_wakeup_handler(struct kvm_vcpu *vcpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    struct pi_desc old, new;

    local_irq_save(flags);

    /* 将 vCPU 加入当前 pCPU 的 wakeup 列表 */
    raw_spin_lock(&per_cpu(wakeup_vcpus_on_cpu_lock, vcpu->cpu));
    list_add_tail(&vmx->pi_wakeup_list,
                  &per_cpu(wakeup_vcpus_on_cpu, vcpu->cpu));
    raw_spin_unlock(&per_cpu(wakeup_vcpus_on_cpu_lock, vcpu->cpu));

    /* 切换通知向量为 WAKEUP_VECTOR */
    old.control = READ_ONCE(pi_desc->control);
    do {
        new.control = old.control;
        new.nv = POSTED_INTR_WAKEUP_VECTOR;  /* 0xf6 */
    } while (pi_try_set_control(pi_desc, &old.control, new.control));

    /* 如果 ON=1，发送 wakeup IPI 给自己 */
    if (pi_test_on(&new))
        __apic_send_IPI_self(POSTED_INTR_WAKEUP_VECTOR);

    local_irq_restore(flags);
}
```

**Wakeup 流程**：

```
vCPU 阻塞 (HLT):
  ① vmx_vcpu_pi_put()
     → pi_enable_wakeup_handler()
     → 设置 NV = POSTED_INTR_WAKEUP_VECTOR (0xf6)
     → 加入 wakeup_vcpus_on_cpu 列表
  
设备发送中断:
  ② IOMMU 写入 PI Descriptor
     → PIR[vec] = 1
     → 发送通知中断 (向量 = 0xf6) 到 NDST
  
pCPU 收到 wakeup 中断:
  ③ pi_wakeup_handler()
     → 遍历 wakeup_vcpus_on_cpu 列表
     → 如果 ON=1，调用 kvm_vcpu_wake_up()
     → vCPU 被唤醒，重新调度
  
vCPU 重新调度:
  ④ vmx_vcpu_pi_load()
     → 从 wakeup 列表移除
     → 恢复 NV = POSTED_INTR_VECTOR (0xf7)
     → 清除 SN，更新 NDST
```

### 1.3 vCPU 迁移时的 IRTE 更新

**问题**：vmx_vcpu_pi_load() 只更新 PI Descriptor 的 NDST，但 IRTE 的 Dest_ID 字段也需要更新，否则通知中断仍会发往旧 pCPU。

**答案**：IRTE 的 Dest_ID 由 IOMMU 层在 `intel_ir_set_vcpu_affinity()` 中更新。

**源码引用**: `drivers/iommu/intel/irq_remapping.c:1248`

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:1248 */
static int intel_ir_set_vcpu_affinity(struct irq_data *data, void *info)
{
    struct intel_ir_data *ir_data = data->chip_data;
    struct vcpu_data *vcpu_pi_info = info;

    /* 如果 info=NULL，回到 remapped 模式 */
    if (!vcpu_pi_info) {
        __intel_ir_reconfigure_irte(data, true);
    } else {
        struct irte irte_pi;

        /* 从 remapped entry 复制，修改 posted 模式字段 */
        memset(&irte_pi, 0, sizeof(irte_pi));
        dmar_copy_shared_irte(&irte_pi, &ir_data->irte_entry);

        /* 更新 posted 模式字段 */
        irte_pi.p_pst = 1;                    /* IM=1 */
        irte_pi.p_urgent = 0;
        irte_pi.p_vector = vcpu_pi_info->vector;  /* VV (通知向量) */
        irte_pi.pda_l = (vcpu_pi_info->pi_desc_addr >>
                        (32 - PDA_LOW_BIT)) & ~(-1UL << PDA_LOW_BIT);
        irte_pi.pda_h = (vcpu_pi_info->pi_desc_addr >> 32) &
                        ~(-1UL << PDA_HIGH_BIT);

        ir_data->irq_2_iommu.posted_vcpu = true;
        modify_irte(&ir_data->irq_2_iommu, &irte_pi);  /* 写入 IRTE */
    }

    return 0;
}
```

**调用时机**：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:272 */
int vmx_pi_update_irte(struct kvm *kvm, unsigned int host_irq,
                       uint32_t guest_irq, bool set)
{
    /* ... */
    
    /* 构建 vCPU 信息，传给 IOMMU 层 */
    vcpu_info.pi_desc_addr = __pa(vcpu_to_pi_desc(vcpu));
    vcpu_info.vector = irq.vector;

    /* 调用 IOMMU 层更新 IRTE */
    ret = irq_set_vcpu_affinity(host_irq, &vcpu_info);
    
    /* ... */
}
```

**触发场景**：

| 场景 | 触发函数 | 更新内容 |
|------|----------|----------|
| 设备直通初始化 | `vmx_pi_update_irte()` | IRTE: PDA, VV, Dest_ID |
| vCPU 迁移 | `kvm_arch_vcpu_load()` → `kvm_arch_irq_bypass_add_consumer()` | IRTE: Dest_ID |
| 中断亲和性变更 | `irq_set_affinity()` | IRTE: Dest_ID |

**注意**：IRTE 的 Dest_ID 字段在 `modify_irte()` 中自动更新为当前 pCPU 的 APIC ID（通过 `ir_data->irq_2_iommu` 中的缓存值）。

### 1.4 中断亲和性变更

**场景**：用户通过 `/proc/irq/<N>/smp_affinity` 或 `irqbalance` 修改设备中断的目标 pCPU。

**流程**：

```
用户修改亲和性:
  ① echo 2 > /proc/irq/50/smp_affinity  (目标改为 pCPU-1)
  
内核处理:
  ② irq_set_affinity(irq_data, cpumask_of(1))
     → intel_ir_set_affinity()
     → 更新 IRTE.Dest_ID = pCPU-1 的 APIC ID
     → modify_irte() 写入 IOMMU
     → 刷新 IOMMU 缓存
  
效果:
  ③ 后续设备中断
     → IOMMU 查找 IRTE
     → 发送通知中断到 pCPU-1 (NDST 已由 PI Descriptor 指定)
     → pCPU-1 处理中断
```

**源码引用**: `drivers/iommu/intel/irq_remapping.c:1156`

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:1156 (简化) */
static int intel_ir_set_affinity(struct irq_data *data,
                                 const struct cpumask *mask,
                                 bool force)
{
    struct intel_ir_data *ir_data = data->chip_data;
    struct irte *irte = &ir_data->irte_entry;

    /* 更新目标 CPU */
    irte->destination = cpumask_first(mask);  /* 简化：假设单目标 */

    /* 写入 IRTE */
    modify_irte(&ir_data->irq_2_iommu, irte);

    return IRQ_SET_MASK_OK_DONE;
}
```

**注意**：亲和性变更**只更新 IRTE 的 Dest_ID**，不修改 PI Descriptor 的 NDST。NDST 由 vCPU 调度时更新。

---

## 🎯 第二部分：AMD AVIC 的 vCPU 调度

### 2.1 vCPU Load (调度入)

**源码引用**: `arch/x86/kvm/svm/avic.c:1075`

```c
/* 来源: arch/x86/kvm/svm/avic.c:1075 */
void avic_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
    u64 entry;
    int h_physical_id = kvm_cpu_get_apicid(cpu);
    struct vcpu_svm *svm = to_svm(vcpu);
    unsigned long flags;

    lockdep_assert_preemption_disabled();

    if (WARN_ON(h_physical_id & ~AVIC_PHYSICAL_ID_ENTRY_HOST_PHYSICAL_ID_MASK))
        return;

    /* 如果 vCPU 是阻塞后调度入，无需更新 */
    if (kvm_vcpu_is_blocking(vcpu))
        return;

    /* 获取 per-vCPU 的 IR 列表锁 */
    spin_lock_irqsave(&svm->ir_list_lock, flags);

    entry = READ_ONCE(*(svm->avic_physical_id_cache));
    WARN_ON_ONCE(entry & AVIC_PHYSICAL_ID_ENTRY_IS_RUNNING_MASK);

    /* 更新 Physical APIC table entry */
    entry &= ~AVIC_PHYSICAL_ID_ENTRY_HOST_PHYSICAL_ID_MASK;
    entry |= (h_physical_id & AVIC_PHYSICAL_ID_ENTRY_HOST_PHYSICAL_ID_MASK);
    entry |= AVIC_PHYSICAL_ID_ENTRY_IS_RUNNING_MASK;  /* IsRunning = 1 */

    WRITE_ONCE(*(svm->avic_physical_id_cache), entry);
    
    /* 更新 IOMMU IRTE 的 Destination 和 IsRun */
    avic_update_iommu_vcpu_affinity(vcpu, h_physical_id, true);

    spin_unlock_irqrestore(&svm->ir_list_lock, flags);
}
```

**关键操作**：

| 步骤 | 操作 | 目的 |
|------|------|------|
| 1 | 更新 Physical APIC table | 设置 Host Physical ID 和 IsRunning=1 |
| 2 | 更新 IOMMU IRTE | 设置 Destination 和 IsRun=1 |
| 3 | 原子写入 | 防止 IOMMU 读到半更新状态 |

### 2.2 vCPU Put (调度出)

**源码引用**: `arch/x86/kvm/svm/avic.c:1119`

```c
/* 来源: arch/x86/kvm/svm/avic.c:1119 */
void avic_vcpu_put(struct kvm_vcpu *vcpu)
{
    u64 entry;
    struct vcpu_svm *svm = to_svm(vcpu);
    unsigned long flags;

    lockdep_assert_preemption_disabled();

    entry = READ_ONCE(*(svm->avic_physical_id_cache));

    /* 如果 IsRunning 已经是 0，无需操作 */
    if (!(entry & AVIC_PHYSICAL_ID_ENTRY_IS_RUNNING_MASK))
        return;

    spin_lock_irqsave(&svm->ir_list_lock, flags);

    /* 更新 IOMMU IRTE：设置 IsRun=0 */
    avic_update_iommu_vcpu_affinity(vcpu, -1, 0);

    /* 清除 Physical APIC table 的 IsRunning */
    entry &= ~AVIC_PHYSICAL_ID_ENTRY_IS_RUNNING_MASK;
    WRITE_ONCE(*(svm->avic_physical_id_cache), entry);

    spin_unlock_irqrestore(&svm->ir_list_lock, flags);
}
```

**关键操作**：

| 步骤 | 操作 | 目的 |
|------|------|------|
| 1 | 更新 IOMMU IRTE | 设置 IsRun=0 |
| 2 | 清除 Physical APIC table | 设置 IsRunning=0 |

### 2.3 IOMMU IRTE 更新

**源码引用**: `arch/x86/kvm/svm/avic.c:1049`

```c
/* 来源: arch/x86/kvm/svm/avic.c:1049 */
static inline int
avic_update_iommu_vcpu_affinity(struct kvm_vcpu *vcpu, int cpu, bool r)
{
    int ret = 0;
    struct amd_svm_iommu_ir *ir;
    struct vcpu_svm *svm = to_svm(vcpu);

    lockdep_assert_held(&svm->ir_list_lock);

    if (!kvm_arch_has_assigned_device(vcpu->kvm))
        return 0;

    if (list_empty(&svm->ir_list))
        return 0;

    /* 遍历 vCPU 的所有 IRTE，逐一更新 */
    list_for_each_entry(ir, &svm->ir_list, node) {
        ret = amd_iommu_update_ga(cpu, r, ir->data);
        if (ret)
            return ret;
    }
    return 0;
}
```

**源码引用**: `drivers/iommu/amd/iommu.c:4017`

```c
/* 来源: drivers/iommu/amd/iommu.c:4017 */
int amd_iommu_update_ga(int cpu, bool is_run, void *data)
{
    struct amd_ir_data *ir_data = (struct amd_ir_data *)data;
    struct irte_ga *entry = (struct irte_ga *) ir_data->entry;

    if (!AMD_IOMMU_GUEST_IR_VAPIC(amd_iommu_guest_ir) ||
        !entry || !entry->lo.fields_vapic.guest_mode)
        return 0;

    if (!ir_data->iommu)
        return -ENODEV;

    /* 更新 Destination (如果 cpu >= 0) */
    if (cpu >= 0) {
        entry->lo.fields_vapic.destination =
                            APICID_TO_IRTE_DEST_LO(cpu);
        entry->hi.fields.destination =
                            APICID_TO_IRTE_DEST_HI(cpu);
    }
    
    /* 更新 IsRun */
    entry->lo.fields_vapic.is_run = is_run;

    /* 写入 IRTE */
    return __modify_irte_ga(ir_data->iommu, ir_data->irq_2_irte.devid,
                            ir_data->irq_2_irte.index, entry);
}
```

**关键逻辑**：

| 场景 | cpu 参数 | is_run 参数 | 更新内容 |
|------|----------|-------------|----------|
| vCPU Load | h_physical_id | true | Destination + IsRun=1 |
| vCPU Put | -1 (不更新 Destination) | false | IsRun=0 |
| vCPU 迁移 | new_h_physical_id | true | Destination + IsRun=1 |

**注意**：vCPU Put 时**不更新 Destination**，只设置 IsRun=0。这样在 vCPU 迁移后，如果 IsRun=0，IOMMU 不会发送 doorbell，而是写入 GA Log。

### 2.4 GA Log 的作用

**源码引用**: `drivers/iommu/amd/iommu.c` (GA Log 处理)

当 IsRun=0 时，IOMMU 写入 GA Log 而不是发送 doorbell：

```
设备发送中断:
  ① IOMMU 查找 IRTE (GuestMode=1)
  ② 原子设置 vIRR[Vector]=1 (在 vAPIC backing page)
  ③ 检查 IsRun:
     如果 IsRun=1: 发送 doorbell 给 Destination
     如果 IsRun=0:
        → 检查 GALogIntr
        → 如果 GALogIntr=1: 写入 GA Log entry
           Entry = { GATag, DeviceID }
        → 不发送 doorbell
  
软件处理 GA Log:
  ④ KVM 定期检查 GA Log (或通过中断)
  ⑤ 读取 GA Log entry，获取 GATag
  ⑥ 根据 GATag 找到目标 vCPU
  ⑦ 调度 vCPU (设置 vCPU 为运行状态)
  
vCPU 调度后:
  ⑧ avic_vcpu_load()
     → 设置 IsRun=1, 更新 Destination
  ⑨ vCPU 运行时，直接处理 vIRR 中的中断
     → 零 VM-Exit (vIRR 已经设置)
```

**GA Log 的优势**：

1. **避免不必要的 doorbell**：vCPU 未运行时，不发送 doorbell
2. **批量处理**：多个中断可以合并到一次调度
3. **调度决策**：VMM 知道哪个 vCPU 有 pending 中断

---

## 🎯 第三部分：Intel vs AMD 对比

### 3.1 vCPU 调度机制对比

| 方面 | Intel VT-d PI | AMD AVIC |
|------|---------------|----------|
| **运行状态标识** | 无 (使用 SN 位抑制) | IsRun 位 (IRTE) |
| **vCPU Load** | 清 SN, 更新 NDST | 设置 IsRun=1, 更新 Destination |
| **vCPU Put** | 设置 SN=1 (如果被抢占) | 设置 IsRun=0 |
| **vCPU 阻塞** | 切换 NV=WAKEUP_VECTOR | IsRun=0, 写入 GA Log |
| **唤醒机制** | pi_wakeup_handler() | GA Log + 软件调度 |
| **更新粒度** | 单个 PI Descriptor | 遍历所有 IRTE (per-vCPU 列表) |

### 3.2 vCPU 迁移对比

**Intel VT-d PI**:

```
vCPU 从 pCPU-0 迁移到 pCPU-3:

① 调度器切换 vCPU 到 pCPU-3

② vmx_vcpu_pi_load(vcpu, cpu=3)
   → 更新 PI.NDST = pCPU-3 的 APIC ID
   → 清除 SN
   → 恢复 NV = POSTED_INTR_VECTOR

③ 设备发送中断
   → IOMMU 查找 IRTE (Dest_ID 仍指向 pCPU-0)
   → 发送通知中断到 pCPU-0 (旧 NDST)
   → pCPU-0 收到中断，但 vCPU 已不在
   → 触发 VM-Exit? (见下方说明)

④ IRTE 更新 (延迟或立即)
   → 某个时机调用 vmx_pi_update_irte()
   → intel_ir_set_vcpu_affinity()
   → 更新 IRTE.Dest_ID = pCPU-3
   → 后续中断正确路由到 pCPU-3
```

**问题**：在步骤 ③，IRTE 的 Dest_ID 可能还未更新，导致通知中断发往旧 pCPU。

**解决**：Intel 使用 SN 位抑制通知，或者依赖硬件的 posted-interrupt delivery 机制（见 SDM 30.6）。

**AMD AVIC**:

```
vCPU 从 pCPU-0 迁移到 pCPU-3:

① 调度器切换 vCPU 到 pCPU-3

② avic_vcpu_load(vcpu, cpu=3)
   → 更新 Physical APIC table: Host Physical ID = pCPU-3, IsRunning=1
   → 遍历 vCPU 的 ir_list
   → 对每个 IRTE: 更新 Destination = pCPU-3, IsRun=1
   → 原子写入 IOMMU

③ 设备发送中断
   → IOMMU 查找 IRTE (Destination = pCPU-3, IsRun=1)
   → 发送 doorbell 到 pCPU-3
   → pCPU-3 处理中断
```

**优势**：AMD 在 vCPU Load 时**立即更新所有 IRTE**，确保后续中断正确路由。

### 3.3 中断亲和性变更对比

**Intel VT-d PI**:

```
修改设备中断亲和性到 pCPU-2:

① echo 4 > /proc/irq/50/smp_affinity

② irq_set_affinity()
   → intel_ir_set_affinity()
   → 更新 IRTE.Dest_ID = pCPU-2 的 APIC ID
   → modify_irte() 写入 IOMMU

③ 后续设备中断
   → IOMMU 发送通知中断到 pCPU-2 (IRTE.Dest_ID)
   → PI Descriptor 的 NDST 未变 (仍指向 vCPU 当前 pCPU)
   → pCPU-2 收到通知中断
   → 硬件特殊处理 (SDM 30.6)
   → 如果 vCPU 在运行: PIR→VIRR, 零 VM-Exit
   → 如果 vCPU 不在运行: 触发 VM-Exit
```

**注意**：PI Descriptor 的 NDST 和 IRTE 的 Dest_ID 可以不一致。NDST 由 vCPU 调度更新，Dest_ID 由亲和性变更更新。

**AMD AVIC**:

```
修改设备中断亲和性到 pCPU-2:

① echo 4 > /proc/irq/50/smp_affinity

② irq_set_affinity()
   → amd_ir_set_affinity()
   → 更新 IRTE.Destination = pCPU-2 的 APIC ID
   → __modify_irte_ga() 写入 IOMMU

③ 后续设备中断
   → IOMMU 查找 IRTE (Destination = pCPU-2)
   → 如果 IsRun=1: 发送 doorbell 到 pCPU-2
   → pCPU-2 处理中断
```

**对比**：两者逻辑类似，都是更新 IRTE 的目标地址。

### 3.4 性能影响对比

| 操作 | Intel VT-d PI | AMD AVIC | 说明 |
|------|---------------|----------|------|
| **vCPU Load (无迁移)** | 清 SN, 快速路径 | 更新 Physical APIC table + 遍历 IRTE | AMD 开销更大 |
| **vCPU Load (有迁移)** | 更新 NDST + 遍历 IRTE | 更新 Physical APIC table + 遍历 IRTE | 类似 |
| **vCPU Put (抢占)** | 设置 SN=1 | 设置 IsRun=0 + 遍历 IRTE | AMD 开销更大 |
| **vCPU 阻塞** | 切换 NV, 加入列表 | 设置 IsRun=0 | Intel 更复杂 |
| **中断亲和性变更** | 更新 IRTE.Dest_ID | 更新 IRTE.Destination | 类似 |
| **GA Log 处理** | 无 | 遍历 IRTE 列表 | AMD 额外开销 |

**关键差异**：

1. **Intel**：单个 PI Descriptor，更新开销小
2. **AMD**：per-vCPU IRTE 列表，遍历更新开销大
3. **Intel**：使用 SN 位抑制，不需要遍历
4. **AMD**：使用 IsRun 位，需要遍历更新

---

## 🎯 第四部分：实践与调试

### 4.1 追踪 vCPU 迁移

**Intel VT-d PI**:

```bash
# 启用 PI 相关 trace 事件
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_pi_irte_update/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_pi_desc_init/enable

# 查看 vCPU 调度
echo 1 > /sys/kernel/debug/tracing/events/sched/sched_switch/enable

# 运行 VM，触发 vCPU 迁移
# ...

# 查看 trace
cat /sys/kernel/debug/tracing/trace_pipe

# 示例输出:
# kvm_pi_irte_update: vcpu=0 host_irq=50 guest_irq=32 vector=0xf7 pi_desc_addr=0x123456000
# sched_switch: prev_comm=qemu-system-x86 prev_pid=1234 next_comm=qemu-system-x86 next_pid=1234
```

**AMD AVIC**:

```bash
# 启用 AVIC 相关 trace 事件
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_avic_* /enable

# 查看 GA Log (如果支持)
cat /sys/kernel/debug/iommu/amd/ga_log

# 查看 IOMMU IRTE
cat /sys/kernel/debug/iommu/amd/ir_translation_struct
```

### 4.2 验证 IRTE 更新

**Intel**:

```bash
# 查看 IRTE
sudo cat /sys/kernel/debug/iommu/intel/ir_translation_struct

# 输出示例:
# Posted Interrupt supported on IOMMU: dmar0
#  Entry SrcID   PDA_high PDA_low  Vct Dest_ID
#  0     03:00.0 00000001 23456040 f7  00000003  (Dest_ID = pCPU-3)

# 修改亲和性
echo 2 > /proc/irq/50/smp_affinity

# 再次查看，Dest_ID 应该变为 pCPU-2
```

**AMD**:

```bash
# 查看 IRTE
sudo cat /sys/kernel/debug/iommu/amd/ir_translation_struct

# 输出示例:
# Guest Virtual APIC enabled
#  Entry SrcID   GVAPTR    Vector Destination IsRun
#  0     03:00.0 0x12345000 0x42   0x03        1   (Destination = pCPU-3, IsRun=1)

# 修改亲和性
echo 2 > /proc/irq/50/smp_affinity

# 再次查看，Destination 应该变为 pCPU-2
```

### 4.3 性能测试

```bash
# 测试 vCPU 迁移延迟
perf stat -e kvm:kvm_exit,kvm:kvm_entry \
    -e kvm:kvm_pi_irte_update \
    -- ./run_vm.sh

# 测试中断亲和性变更延迟
perf stat -e irq:irq_set_affinity \
    -- bash -c 'echo 2 > /proc/irq/50/smp_affinity'
```

---

## 📊 总结

### vCPU 迁移关键要点

```
Intel VT-d PI:
  · vmx_vcpu_pi_load(): 更新 PI.NDST, 清除 SN
  · vmx_vcpu_pi_put(): 设置 SN (如果被抢占)
  · IRTE 更新: 延迟或立即，由 intel_ir_set_vcpu_affinity() 完成
  · 优势: 单个 PI Descriptor，更新开销小
  · 劣势: PI.NDST 和 IRTE.Dest_ID 可能不一致

AMD AVIC:
  · avic_vcpu_load(): 更新 Physical APIC table, 遍历 IRTE 列表
  · avic_vcpu_put(): 设置 IsRun=0
  · IRTE 更新: 立即，在 avic_vcpu_load() 中完成
  · 优势: IRTE 立即更新，确保正确路由
  · 劣势: 遍历 per-vCPU IRTE 列表，开销大
```

### 中断亲和性变更关键要点

```
Intel VT-d PI:
  · 更新 IRTE.Dest_ID
  · PI.NDST 不变 (由 vCPU 调度更新)
  · 通知中断可能发往旧 pCPU，依赖硬件处理

AMD AVIC:
  · 更新 IRTE.Destination
  · Physical APIC table 不变 (由 vCPU 调度更新)
  · doorbell 发往新 pCPU
```

### 性能优化建议

```
1. 减少 vCPU 迁移频率
   · 使用 CPU pinning (taskset, cgroups)
   · 禁用 irqbalance 或配置为静态模式

2. 批量更新 IRTE
   · AMD: 合并多个 IRTE 更新，减少 IOMMU 写入

3. 使用 SN/IsRun 位
   · Intel: vCPU 被抢占时设置 SN=1，避免无谓通知
   · AMD: vCPU 阻塞时设置 IsRun=0，使用 GA Log

4. 监控 GA Log (AMD)
   · 频繁写入 GA Log 说明 vCPU 调度不及时
   · 考虑调整调度策略或 CPU 亲和性
```

---

**参考源码**:
- 内核: `arch/x86/kvm/vmx/posted_intr.c`, `arch/x86/kvm/svm/avic.c`
- IOMMU: `drivers/iommu/intel/irq_remapping.c`, `drivers/iommu/amd/iommu.c`
- 规范: `intel-vtd.pdf` Section 5.2.3, 9.10, 9.11; `amd-virtualization.pdf` Section 2.2.5.2, 2.7
