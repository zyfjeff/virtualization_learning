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
   - vCPU 迁移时只更新 PI Descriptor 的 NDST（Posted IRTE 无 destination，不重写 IRTE）
   - 中断亲和性变更的处理（posted 路径对 IRTE 显式跳过）
   
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

    /* PIR 非空 → 置 ON=1（不是 KVM_REQ_EVENT），让后续通知/同步路径处理 */
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

**注意**：vmx_vcpu_pi_load() **只更新 PI Descriptor 的 NDST**；Posted 格式 IRTE 没有
destination 字段，迁移不需要也不可能"更新 IRTE Dest_ID"。IRTE 改写只发生在路由/绑定
变更时（见 §1.3，AGENTS.md 陷阱 #18）。

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
        new.nv = POSTED_INTR_WAKEUP_VECTOR;  /* 0xf1 */
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
     → 设置 NV = POSTED_INTR_WAKEUP_VECTOR (0xf1)
     → 加入 wakeup_vcpus_on_cpu 列表
  
设备发送中断:
  ② IOMMU 写入 PI Descriptor
     → PIR[vec] = 1
     → 发送通知中断 (向量 = 0xf1) 到 NDST
  
pCPU 收到 wakeup 中断:
  ③ pi_wakeup_handler()
     → 遍历 wakeup_vcpus_on_cpu 列表
     → 如果 ON=1，调用 kvm_vcpu_wake_up()
     → vCPU 被唤醒，重新调度
  
vCPU 重新调度:
  ④ vmx_vcpu_pi_load()
     → 从 wakeup 列表移除
     → 恢复 NV = POSTED_INTR_VECTOR (0xf2)
     → 清除 SN，更新 NDST
```

### 1.3 IRTE 何时才被改写（vCPU 迁移不碰 IRTE）

**问题**：vmx_vcpu_pi_load() 只更新 PI Descriptor 的 NDST，那 IRTE 要不要跟着更新目标 pCPU？

**答案**：**不要，也不能**——Posted 格式 IRTE（VT-d Spec 9.10, Figure 9-10）只含
P/FPD/AVAIL/URG/**IM**/VV/PDA/SID/SQ/SVT，**没有 destination 字段**；通知目标完全由
PI Descriptor 的 NDST 给出，所以 vCPU 迁移对 IOMMU 是零操作。`intel_ir_set_vcpu_affinity()`
是**路由/绑定变更**（irqfd 挂接、KVM_SET_GSI_ROUTING、guest 改 MSI-X 表）时才走的
IRTE 改写路径，与宿主调度无关：

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
        irte_pi.p_vector = vcpu_pi_info->vector;  /* VV = guest 向量（写进 PIR 的位），
                                                   * 不是通知向量；NV 在 PI Descriptor */
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
| 设备直通初始化 / guest 改路由（KVM_SET_GSI_ROUTING、irqfd 挂接、MSI-X 表变更） | `vmx_pi_update_irte()` → `intel_ir_set_vcpu_affinity()` → `modify_irte()` | IRTE: IM(p_pst)、VV(p_vector=guest 向量)、PDA（try_cmpxchg128 原子替换 + qi_flush_iec） |
| vCPU 迁移（宿主调度） | `vmx_vcpu_pi_load()` | **只改 PI Descriptor 的 NDST**；IRTE 不动、无 IEC 刷新 |
| 宿主中断亲和性变更（irqbalance 等） | `intel_ir_set_affinity()` | 对 posted_vcpu 的 IRTE **显式跳过不写**（irq_remapping.c:1176-1177），只更新缓存的 remapped 视图备回退 |

### 1.4 中断亲和性变更

**场景**：用户通过 `/proc/irq/<N>/smp_affinity` 或 `irqbalance` 修改设备中断的目标 pCPU。

**流程**：

```
用户修改亲和性:
  ① echo 2 > /proc/irq/50/smp_affinity  (目标改为 pCPU-1)

内核处理（irq_remapping.c:1217-1240, 1168-1183）:
  ② intel_ir_set_affinity()
     → parent->chip->irq_set_affinity()      (irq 核心层迁移宿主 vector)
     → intel_ir_reconfigure_irte(data, false)
        · 更新**缓存的 remapped 视图** irte_entry 的 vector/dest_id（备将来回退）
        → __intel_ir_reconfigure_irte(data, force_host=false)
           ★ 若 posted_vcpu=1：**直接 return，不写 IRTE、不刷 IEC**
  ③ 若该 IRQ 未切 Posted（Remapped 模式）才会走 modify_irte() 真正改写 IRTE

效果（guest posted 中断）:
  设备中断 → IOMMU 查 Posted IRTE → 写该 vCPU 的 PI Descriptor
  → 通知发往 PI desc.NDST 指向的 pCPU —— 由 vCPU 调度决定，
    与宿主 irq_set_affinity 无关。
```

**源码引用**: `drivers/iommu/intel/irq_remapping.c:1217` / `:1168`

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:1217-1240 / 1168-1183（节选） */
static int
intel_ir_set_affinity(struct irq_data *data, const struct cpumask *mask,
		      bool force)
{
	struct irq_data *parent = data->parent_data;
	struct irq_cfg *cfg = irqd_cfg(data);
	int ret;

	ret = parent->chip->irq_set_affinity(parent, mask, force);
	if (ret < 0 || ret == IRQ_SET_MASK_OK_DONE)
		return ret;

	intel_ir_reconfigure_irte(data, false);
	/*
	 * After this point, all the interrupts will start arriving
	 * at the new destination. So, time to cleanup the previous
	 * vector allocation.
	 */
	vector_schedule_cleanup(cfg);

	return IRQ_SET_MASK_OK_DONE;
}

static void __intel_ir_reconfigure_irte(struct irq_data *irqd, bool force_host)
{
	struct intel_ir_data *ir_data = irqd->chip_data;

	/*
	 * Don't modify IRTEs for IRQs that are being posted to vCPUs if the
	 * host CPU affinity changes.
	 */
	if (ir_data->irq_2_iommu.posted_vcpu && !force_host)
		return;

	ir_data->irq_2_iommu.posted_vcpu = false;

	if (ir_data->irq_2_iommu.posted_msi)
		intel_ir_reconfigure_irte_posted(irqd);
	else
		modify_irte(&ir_data->irq_2_iommu, &ir_data->irte_entry);
}
```

**注意**：对已切 Posted 的 guest 中断，宿主亲和性变更**既不改 IRTE 也不改
PI Descriptor**（通知目的地只随 vCPU 调度在 `vmx_vcpu_pi_load()` 中更新 NDST）。
真正改变 guest 中断投递目标的路径是 **guest 侧**改 MSI/MSI-X 路由 →
`KVM_SET_GSI_ROUTING`/irqfd 更新 → `vmx_pi_update_irte()`。
（本节此前版本虚构了 `irte->destination = cpumask_first(mask)` 代码并把行号标在
`intel_ir_reconfigure_irte_posted()`——宿主 posted-MSI 路径——内部，见 corrections 勘误 10。）

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
   → IOMMU 查找 IRTE（PDA 指向该 vCPU 的 PI Descriptor；IRTE 从未被改写）
   → 读 PI Descriptor 的 NDST = pCPU-3
   → 通知中断发到 pCPU-3 → vCPU 正常处理

④ 没有"再更新 IRTE"的步骤：Posted IRTE 无 destination 字段，
   迁移全程零 modify_irte()/零 IEC 刷新（AGENTS.md 陷阱 #18）
```

**真正的边界情形**：迁移发生在中断已写入 PIR 之后——旧 pCPU 已无人接收通知，
或中断在 NDST 更新前的窗口到达旧 pCPU。内核的处理：
- 被抢占期间 `pi_set_sn()` 抑制无意义通知（vcpu-migration §1.2）；
- `vmx_vcpu_pi_load()` 在更新 NDST/清 SN 后，若 `!pi_is_pir_empty()` 则
  `pi_set_on()`（posted_intr.c:132）重新挂起一次同步，PIR 里的位在下次
  VM-Entry 前由 `vmx_sync_pir_to_irr()` 搬进 IRR——**不丢中断，也不需要动 IRTE**。

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
   → intel_ir_set_affinity()                  (irq_remapping.c:1217)
   → intel_ir_reconfigure_irte(data, false)   ← 只更新缓存的 remapped 视图
   → __intel_ir_reconfigure_irte():
     posted_vcpu=1 且 !force_host → **直接 return，不写 IRTE**（:1176-1177）

③ 后续设备中断
   → IOMMU 查 Posted IRTE → PDA → PI Descriptor → NDST（vCPU 当前 pCPU）
   → 通知仍发到 vCPU 所在 pCPU —— 宿主亲和性变更不改变 guest 中断投递目标
```

**注意**：Posted 格式 IRTE 没有 Dest_ID，"NDST 与 Dest_ID 不一致"这种状态不存在。
投递目标只有 PI Descriptor.NDST 一个来源，由 vCPU 调度维护。若要让 guest 把该中断
换到别的 vCPU/pCPU，走的是 guest 侧改 MSI/MSI-X 路由 → `vmx_pi_update_irte()`。

**AMD AVIC**:

```
修改设备中断亲和性到 pCPU-2:

① echo 4 > /proc/irq/50/smp_affinity

② irq_set_affinity()
   → amd_ir_set_affinity()                    (amd/iommu.c:3946)
   → amd_ir_update_irte → irte_ga_set_affinity():
     if (!guest_mode) { 更新 hi.vector / lo+hi.destination; modify_irte_ga(); }
     guest_mode=1 时**跳过，不写 IRTE**（iommu.c:3483-3491）

③ GA IRTE（guest_mode=1）的目标（doorbell Destination/IsRun）只随
   avic_vcpu_load/put/blocking/unblocking 更新（见 §2），宿主亲和性变更不动它。
```

**对比**：两家对 guest 虚拟化中断的处理原则一致——**宿主 irq 亲和性 ≠ guest 中断投递目标**。
Intel 靠 PI Descriptor.NDST（IRTE 无目标字段），AMD 靠 GA IRTE 的 Destination/IsRun
（由 KVM 在 vCPU 调度时更新）；对非 guest 的 Remapped IRTE，两家才会随亲和性改写 IRTE。

### 3.4 性能影响对比

| 操作 | Intel VT-d PI | AMD AVIC | 说明 |
|------|---------------|----------|------|
| **vCPU Load (无迁移)** | 清 SN, 快速路径 | 更新 Physical APIC table + 遍历 IRTE | AMD 开销更大 |
| **vCPU Load (有迁移)** | **只**更新 NDST（PI desc cmpxchg，不写 IRTE） | 更新 Physical APIC table + 遍历 IRTE | 机制根本不同 |
| **vCPU Put (抢占)** | 设置 SN=1 | 设置 IsRun=0 + 遍历 IRTE | AMD 开销更大 |
| **vCPU 阻塞** | 切换 NV, 加入列表 | 设置 IsRun=0 | Intel 更复杂 |
| **中断亲和性变更（guest posted/GA 时）** | `posted_vcpu` → 跳过不写 IRTE（irq_remapping.c:1176-1177） | `guest_mode=1` → 跳过不写 IRTE（amd/iommu.c:3483） | 两家都不动 guest 中断的 IRTE |
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
# 启用 PI 相关 trace 事件（>> 追加，避免清空已有配置，见 phase10 measurement.md §5）
echo 1 >> /sys/kernel/debug/tracing/events/kvm/kvm_pi_irte_update/enable

# 查看 vCPU 调度
echo 1 >> /sys/kernel/debug/tracing/events/sched/sched_switch/enable

# 运行 VM，触发 vCPU 迁移
# ...

# 查看 trace
cat /sys/kernel/debug/tracing/trace_pipe

# 示例输出（6.12.93 真实格式，trace.h:1103-1105）:
# kvm_pi_irte_update: PI is enabled and being updated for irq 50, vcpu 0, gsi: 0x20, gvec: 0x32, pi_desc_addr: 0x123456000
#   ↑ gvec = guest 向量（写进 IRTE 的 VV）；此事件只在路由/绑定变更时触发，
#     vCPU 迁移不会产生该事件（迁移只改 PI desc NDST，不写 IRTE）
# sched_switch: prev_comm=qemu-system-x86 prev_pid=1234 next_comm=qemu-system-x86 next_pid=1234
```

注：6.12.93 只有 `kvm:kvm_pi_irte_update` 这一个 PI IRTE 事件
（arch/x86/kvm/trace.h:1080）；`kvm_pi_desc_init`/`kvm_pi_set_pir` **不存在**。

**AMD AVIC**:

```bash
# 启用 AVIC 相关 trace 事件（6.12.93 存在的事件，逐个 enable 或用 set_event 追加）
#   kvm_avic_incomplete_ipi / kvm_avic_unaccelerated_access /
#   kvm_avic_ga_log / kvm_avic_kick_vcpu_slowpath / kvm_avic_doorbell
#   (arch/x86/kvm/trace.h:1432-1524)
echo 1 >> /sys/kernel/debug/tracing/events/kvm/kvm_avic_ga_log/enable
cat /sys/kernel/debug/tracing/trace_pipe
```

注：6.12.93 的 AMD IOMMU debugfs（drivers/iommu/amd/debugfs.c）**没有**
`ga_log` 或 `ir_translation_struct` 文件——GA 事件观测走 `kvm:kvm_avic_ga_log`
tracepoint；IRTE 内容观测可挂 `amd_iommu_update_ga`/`modify_irte_ga` kprobe
（注意 AGENTS.md 陷阱 #9 的 .isra 符号验证）。

### 4.2 验证 IRTE 更新

**Intel**:

```bash
# 查看 IRTE
sudo cat /sys/kernel/debug/iommu/intel/ir_translation_struct

# Posted 表头（drivers/iommu/intel/debugfs.c:587 实际格式）:
#  Entry SrcID   PDA_high PDA_low  Vct IRTE_high		IRTE_low
#  0     03:00.0 00000001 23456040 32  0000000123456789	00000000abcdef01
#   Vct = IRTE.VV = guest 向量；**没有 Dest_ID 列**（Posted IRTE 无 destination 字段）

# 修改宿主亲和性
echo 2 > /proc/irq/50/smp_affinity

# 预期：该表项**不变**——posted_vcpu 时 intel_ir_set_affinity 跳过 IRTE 写入
# （irq_remapping.c:1176-1177）；通知目标只随 vCPU 调度更新 PI desc NDST
```

**AMD**:

```bash
# 6.12.93 的 AMD IOMMU debugfs 没有 IRTE 转储文件
# （drivers/iommu/amd/debugfs.c 只建 iommuNN 目录），观测 GA IRTE 更新可挂
# kprobe/ftrace 在 amd_iommu_update_ga（iommu.c:4017）上——取参方案须先按
# AGENTS.md 陷阱 #9 在已知场景做对照验证再采信
echo ga_upd > /sys/kernel/tracing/set_ftrace_filter   # 或 kprobe，注意先清残留

# 修改宿主亲和性
echo 2 > /proc/irq/50/smp_affinity

# 预期：guest_mode=1 的 IRTE 不会被改写——irte_ga_set_affinity() 里有
# `if (!irte->lo.fields_remap.guest_mode)` 门（amd/iommu.c:3483），
# kprobe 也不会因亲和性变更而命中 guest_mode 分支
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
  · IRTE 更新: **迁移不更新 IRTE**；仅路由/绑定变更时由
    vmx_pi_update_irte() → intel_ir_set_vcpu_affinity() 改写
  · 优势: 单个 PI Descriptor，迁移对 IOMMU 零操作
  · 注意点: SN 抑制/PIR 残留由 pi_load 的 pi_set_on() 兜底（posted_intr.c:132）

AMD AVIC:
  · avic_vcpu_load(): 更新 Physical APIC table, 遍历 IRTE 列表
  · avic_vcpu_put(): 设置 IsRun=0
  · IRTE 更新: 立即，在 avic_vcpu_load() 中完成
  · 优势: IRTE 立即更新，确保正确路由
  · 劣势: 遍历 per-vCPU IRTE 列表，开销大
```

### 中断亲和性变更关键要点

```
Intel VT-d PI（guest posted 中断）:
  · intel_ir_set_affinity() 对 posted_vcpu 的 IRTE **跳过不写**（irq_remapping.c:1176-1177）
  · PI.NDST 只随 vCPU 调度更新 → 宿主亲和性变更不改变 guest 中断投递目标
  · 改变 guest 中断目标的路径：guest 改 MSI/MSI-X 路由 → vmx_pi_update_irte()

AMD AVIC（GA/guest_mode IRTE）:
  · irte_ga_set_affinity() 对 guest_mode=1 **跳过不写**（amd/iommu.c:3483-3491）
  · IRTE.Destination/IsRun 只随 avic_vcpu_load/put/blocking 更新
  · 非 guest_mode 的 Remapped IRTE 才会随宿主亲和性改写 Destination
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
