# 第四阶段源码注释：中断虚拟化 + VT-d中断重映射

> 基于 Linux 6.12.93 源码 | 按照实际数据路径组织

---

## 1. PI描述符 — KVM与VT-d的交汇点

**文件**: `arch/x86/include/asm/posted_intr.h`

```c
/* 来源: arch/x86/include/asm/posted_intr.h */

/*
 * struct pi_desc - Posted Interrupt Descriptor
 *
 * 64字节, 64字节对齐
 * KVM分配(vcpu_vmx.pi_desc), IOMMU通过IRTE的PDA字段找到它
 * 这是KVM层和VT-d层之间的硬件共享数据结构
 */
struct pi_desc {
    union {
        u32 pir[8];         /* Posted interrupt requested (256位) */
        u64 pir64[4];       /* 每个bit对应一个中断向量 */
    };
    union {
        struct {
            u16 notifications;  /* SN(1bit) + ON(1bit) + 保留 */
            u8  nv;             /* Notification Vector (8位) */
            u8  rsvd_2;
            u32 ndst;           /* Notification Destination (APIC ID) */
        };
        u64 control;            /* 控制字 (64位整体，用于原子操作) */
    };
    u32 rsvd[6];               /* 填充到64字节 */
} __aligned(64);

/* 位操作辅助函数 */
static inline bool pi_test_on(struct pi_desc *pi_desc)
{
    return test_bit(POSTED_INTR_ON, (unsigned long *)&pi_desc->control);
}

static inline bool pi_test_and_clear_on(struct pi_desc *pi_desc)
{
    return test_and_clear_bit(POSTED_INTR_ON,
                              (unsigned long *)&pi_desc->control);
}

static inline void pi_set_sn(struct pi_desc *pi_desc)
{
    set_bit(POSTED_INTR_SN, (unsigned long *)&pi_desc->control);
}

static inline bool pi_test_and_set_pir(int vector, struct pi_desc *pi_desc)
{
    return test_and_set_bit(vector, (unsigned long *)pi_desc->pir);
}
```

**要点**:
- `pir[256]` 由 IOMMU 硬件写入（set_bit），KVM 读取并拷贝到 vLAPIC IRR
- `control` 字段用于原子 cmpxchg 操作（因为IOMMU硬件和软件同时访问）
- `nv` = Notification Vector：IOMMU发通知中断的向量号
- `ndst` = pCPU的物理APIC ID：vCPU迁移时需要更新

---

## 2. VT-d层：IRTE与中断重映射

**文件**: `drivers/iommu/intel/irq_remapping.c`

```c
/* 来源: drivers/iommu/intel/irq_remapping.c */

/*
 * struct irq_2_iommu - 每个IRQ对应的IOMMU映射信息
 * 这是Linux IRQ子系统与IOMMU中断重映射的桥梁
 */
struct irq_2_iommu {
    struct intel_iommu *iommu;    /* 关联的IOMMU实例 */
    u16 irte_index;               /* 在IR表中的索引 */
    u16 sub_handle;               /* 子句柄(MSI-X多向量) */
    u8  irte_mask;                /* IRTE掩码 */
    bool posted_msi;              /* ★ PI模式是否启用 */
    bool posted_vcpu;             /* PI投递到vCPU */
};

/*
 * struct intel_ir_data - 中断重映射数据
 * 存储一个中断的IRTE配置，作为irq_cfg的私有数据
 */
struct intel_ir_data {
    struct irq_2_iommu irq_2_iommu;    /* IOMMU映射 */
    struct irte irte_entry;             /* IRTE条目(128位) */
    union {
        struct msi_msg msi_entry;       /* MSI消息(地址+数据) */
    };
};
```

### IRTE 128位格式 (参考 VT-d Spec Section 9.9 / 9.10)

```
Remapped模式 (IM=0, Section 9.9):
低64位:
┌──┬───┬──┬──┬──┬───┬───┬───┬──┬───┬───┬────────┐
│ P│FPD│DM│RH│TM│DLM│AV │RSV│IM│ V │RSV│Dest ID │
│ 1│ 1 │ 1│ 1│ 1│ 3 │ 4 │ 3 │ 1│ 8 │ 8 │   32   │
└──┴───┴──┴──┴──┴───┴───┴───┴──┴───┴───┴────────┘
bit: 0   1  2  3  4  7:5 11:8 14:12 15 23:16 31:24 63:32

高64位:
┌──────┬──┬───┬────────────────────────────────┐
│ SID  │SQ│SVT│ Reserved                       │
│ 16bit│ 2│ 2 │          44 bits               │
└──────┴──┴───┴────────────────────────────────┘
bit: 79:64 81:80 83:82        127:84

Posted模式 (IM=1, Section 9.10):
低64位:
┌──┬───┬──┬───┬──┬───┬──┬───┬───┬──────────────────┐
│ P│FPD│RSV│AV │RSV│URG│IM│ VV│RSV│ PDAL (26 bits) │
│ 1│ 1 │ 6 │ 4 │ 2 │ 1 │ 1│ 8 │14 │                │
└──┴───┴──┴───┴──┴───┴──┴───┴───┴──────────────────┘

高64位:
┌──────┬──┬───┬──────┬─────────────────────────────┐
│ SID  │SQ│SVT│ RSV  │ PDAH (PI描述符地址高位)      │
│ 16bit│ 2│ 2 │ 12bit│          32 bits             │
└──────┴──┴───┴──────┴─────────────────────────────┘

字段说明:
  IM  = IRTE Mode (bit 15): 1=Posted模式, 0=Remapped模式
  VV  = Virtual Vector (bits 23:16): 通知向量
  PDAL = Posted Descriptor Address Low (bits 63:38, 地址bits 31:6)
  PDAH = Posted Descriptor Address High (bits 127:96, 地址bits 63:32)
  
PDA (Posted Descriptor Address) = pi_desc的物理地址(64字节对齐)
  → IOMMU通过PDA找到vCPU的PI描述符
  → 直接写PIR[vector]和ON位
  
注意: 
  · IRTE中的VV会被写入PI Descriptor的NV字段
  · 不要与Remapped模式的DLM (Delivery Mode, bits 7:5)字段混淆
```

### prepare_irte() — 初始化IRTE

```c
/* 来源: drivers/iommu/intel/irq_remapping.c */

static void prepare_irte(struct irte *irte)
{
    memset(irte, 0, sizeof(*irte));
    /*
     * P=0 (不启用), 所有字段清零
     * 等完全配置好后再通过 modify_irte() 激活
     */
}
```

### modify_irte() — 写入IRTE

```c
/* 来源: drivers/iommu/intel/irq_remapping.c */

static int modify_irte(struct irq_2_iommu *irq_iommu,
                       struct irte *irte)
{
    struct intel_iommu *iommu = irq_iommu->iommu;
    int index = irq_iommu->irte_index;

    /*
     * 1. 清除Present位 (防止更新过程中错误投递)
     * 2. 写入新的IRTE内容到IR表
     * 3. qi_flush_iec() → 刷新IOMMU的IRTE缓存
     * 4. 设置Present位 (重新启用)
     *
     * 注意: 每次vCPU迁移都需要更新IRTE的Dest ID
     * 然后刷新IEC (Interrupt Entry Cache)
     */
}
```

---

## 3. KVM层：PIR→IRR同步

**文件**: `arch/x86/kvm/vmx/vmx.c:6912` (vmx_sync_pir_to_irr), `arch/x86/kvm/vmx/vmx.c:6881` (vmx_set_rvi)

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:6912 */

/*
 * vmx_sync_pir_to_irr - 将Posted Interrupt的PIR同步到vLAPIC IRR
 *
 * 调用时机:
 *   - vCPU进入KVM_RUN之前 (vcpu_enter_guest)
 *   - VM-Exit处理过程中需要检查中断
 *   - vCPU从halt状态被唤醒时
 *
 * ★ 这是PI模式的核心：将IOMMU写入的PIR拷贝到vLAPIC IRR
 */
int vmx_sync_pir_to_irr(struct kvm_vcpu *vcpu)
{
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    int max_irr;
    bool got_posted_interrupt;

    if (KVM_BUG_ON(!enable_apicv, vcpu->kvm))
        return -EIO;

    if (pi_test_on(&vmx->pi_desc)) {
        /* ★ ON=1说明IOMMU投递了中断 */
        pi_clear_on(&vmx->pi_desc);
        /*
         * IOMMU可以写PI.ON，所以需要内存屏障
         * 确保先读完PIR再清除ON
         */
        smp_mb__after_atomic();

        /* ★ 将PIR拷贝到vLAPIC的IRR */
        got_posted_interrupt =
            kvm_apic_update_irr(vcpu, vmx->pi_desc.pir, &max_irr);
    } else {
        max_irr = kvm_lapic_find_highest_irr(vcpu);
        got_posted_interrupt = false;
    }

    /*
     * 更新RVI (Requested Virtual Interrupt) → 触发VID
     * VID (Virtual Interrupt Delivery) 使硬件在VM-Entry时
     * 自动评估IRR并注入最高优先级中断
     */
    if (!is_guest_mode(vcpu) && kvm_vcpu_apicv_active(vcpu))
        vmx_set_rvi(max_irr);
    else if (got_posted_interrupt)
        kvm_make_request(KVM_REQ_EVENT, vcpu);

    return max_irr;
}

/*
 * vmx_set_rvi - 更新VMCS的GUEST_INTR_STATUS的RVI字段
 *
 * RVI (Requested Virtual Interrupt) = 最高优先级待处理中断向量
 * 硬件在VM-Entry时比较RVI和PPR:
 *   如果RVI > PPR → 自动注入中断 (零VM-Exit!)
 */
static void vmx_set_rvi(int vector)
{
    u16 status;
    u8 old;

    if (vector == -1)
        vector = 0;

    status = vmcs_read16(GUEST_INTR_STATUS);
    old = (u8)status & 0xff;      /* RVI在低8位 */
    if ((u8)vector != old) {
        status &= ~0xff;
        status |= (u8)vector;
        vmcs_write16(GUEST_INTR_STATUS, status);
    }
}
```

---

## 4. KVM-VT-d桥梁：vmx_pi_update_irte()

**文件**: `arch/x86/kvm/vmx/posted_intr.c:272` (vmx_pi_update_irte), 通过 `vmx/main.c:135` 注册到 kvm_x86_ops

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:272 */

/*
 * vmx_pi_update_irte - 更新PI模式的IRTE
 *
 * 当VFIO设备直通给VM时调用，将vCPU的PI描述符地址写入IRTE
 * 使IOMMU能够直接通过PI机制投递中断给vCPU
 *
 * 调用链:
 *   QEMU: ioctl(KVM_IRQFD) → kvm_vfio_setup_pi_irte()
 *     → kvm_x86_ops.pi_update_irte() → vmx_pi_update_irte()
 *
 * @vcpu: 目标vCPU
 * @gvec: Guest中断向量
 * @girq: Guest中断号(GSIV)
 * @set:  true=设置PI模式, false=清除
 */
void vmx_pi_update_irte(struct kvm_vcpu *vcpu,
                        u32 gvec, u32 girq, bool set)
{
    struct kvm *kvm = vcpu->kvm;
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    struct irq_2_iommu *irq_iommu;

    if (!set) {
        /* 清除PI模式: 恢复传统IRTE */
        return;
    }

    /*
     * ★ 关键步骤:
     * 1. 通过girq找到对应的Linux IRQ和irq_2_iommu
     * 2. 获取IRTE索引
     * 3. 构造PI模式IRTE:
     *    - IM=1 (PI模式)
     *    - PDA = __pa(&vmx->pi_desc) ← vCPU的PI描述符物理地址
     *    - NV  = pi_desc.nv ← 通知向量
     *    - Dest_ID = 当前pCPU的APIC ID
     * 4. modify_irte() 写入IR表
     * 5. 刷新IOMMU IEC缓存
     */

    /*
     * 之后每次vCPU迁移pCPU:
     *   vmx_vcpu_pi_load() 更新 pi_desc.ndst = 新pCPU APIC ID
     *   同时需要更新IRTE的Dest_ID
     *   然后刷新IEC
     */
}
```

---

## 5. vCPU调度时的PI操作

**文件**: `arch/x86/kvm/vmx/posted_intr.c:53` (vmx_vcpu_pi_load), `arch/x86/kvm/vmx/posted_intr.c:196` (vmx_vcpu_pi_put)

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:53 */

/*
 * vmx_vcpu_pi_load - vCPU调度到pCPU时调用
 *
 * 作用:
 *   1. 清除SN位 → 允许IOMMU发送通知中断
 *   2. 更新NDST → 当前pCPU的物理APIC ID
 *
 * ★ 必须使用cmpxchg原子操作，因为IOMMU硬件可能同时写PI.ON
 */
void vmx_vcpu_pi_load(struct kvm_vcpu *vcpu, int cpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    struct pi_desc old, new;
    unsigned int dest;

    if (!enable_apicv || !lapic_in_kernel(vcpu))
        return;

    /* 如果没在wakeup列表上且没迁移，只需清SN */
    if (pi_desc->nv != POSTED_INTR_WAKEUP_VECTOR && vcpu->cpu == cpu) {
        if (pi_test_and_clear_sn(pi_desc))
            goto after_clear_sn;
    }

    /* ★ 原子更新整个control字段 */
    do {
        old.control = READ_ONCE(pi_desc->control);
        new = old;

        new.sn = 0;    /* 允许通知 */

        /* NDST = 当前pCPU的物理APIC ID */
        dest = per_cpu(x86_cpu_to_physical_apicid, cpu);
        new.ndst = dest;

        /* 通知向量 */
        new.nv = POSTED_INTR_VECTOR;

    } while (cmpxchg64(&pi_desc->control,
                       old.control, new.control) != old.control);

after_clear_sn:
    /* 如果PIR非空，同步到IRR */
    if (!pi_is_pir_empty(pi_desc))
        kvm_make_request(KVM_REQ_EVENT, vcpu);
}

/*
 * vmx_vcpu_pi_put - vCPU从pCPU卸载时调用
 *
 * 来源: posted_intr.c:196
 *
 * 作用:
 *   1. 如果vCPU将要阻塞且中断未屏蔽 → 启用wakeup handler
 *   2. 如果vCPU被抢占 → 设置SN位抑制通知
 */
void vmx_vcpu_pi_put(struct kvm_vcpu *vcpu)
{
	struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);

	/* 如果不需要PI唤醒(无直通设备等)，直接返回 */
	if (!vmx_needs_pi_wakeup(vcpu))
		return;

	/*
	 * 如果vCPU将要阻塞且中断未被屏蔽:
	 *   启用wakeup handler
	 *   → 当PI到达时，pi_wakeup_handler()唤醒vCPU
	 *   → 将vCPU加入wakeup_vcpus_on_cpu列表
	 */
	if (kvm_vcpu_is_blocking(vcpu) && !vmx_interrupt_blocked(vcpu))
		pi_enable_wakeup_handler(vcpu);

	/*
	 * 如果vCPU被抢占:
	 *   设置SN位 → 抑制通知中断
	 *   (vCPU不在运行，通知无意义)
	 *   注意: vCPU可能同时被看作blocking和preempted
	 */
	if (vcpu->preempted)
		pi_set_sn(pi_desc);
}
```

---

## 6. 完整中断注入到vLAPIC

**文件**: `arch/x86/kvm/lapic.c`, `virt/kvm/irqchip.c:70` (kvm_set_irq)

```c
/* 来源: virt/kvm/irqchip.c:70 */

/*
 * kvm_set_irq - 设置中断 (从irqfd或KVM_IRQ_LINE调用)
 *
 * 调用链:
 *   QEMU写irqfd → irqfd_inject() → kvm_set_irq()
 *   ioctl(KVM_IRQ_LINE) → kvm_vm_ioctl_irq_line() → kvm_set_irq()
 *
 * → kvm_irq_delivery_to_apic()
 *   → 找到目标vCPU的vLAPIC
 *   → __apic_accept_irq()
 *     → kvm_lapic_set_irr() 或 vmx_deliver_posted_interrupt()
 */
int kvm_set_irq(struct kvm *kvm, int irq_source_id,
                u32 irq, int level, bool line_status)
{
    /*
     * 1. 查IRQ路由表: irq → irq_routing_table → 目标芯片
     * 2. 调用对应芯片的set_irq回调:
     *    - PIC:   kvm_pic_set_irq() → pic_set_irq1()
     *    - IOAPIC: kvm_ioapic_set_irq() → ioapic_service()
     *    - MSI:   kvm_set_msi() → kvm_irq_delivery_to_apic()
     * 3. 最终投递到vLAPIC
     */
}
```

```c
/* 来源: arch/x86/kvm/vmx/vmx.c */

/*
 * vmx_deliver_interrupt - 投递中断到vCPU
 *
 * 两种模式:
 *   1. PI模式: vmx_deliver_posted_interrupt()
 *      直接写PI描述符的PIR[vector]=1
 *      设置ON位，发送通知中断
 *      ★ 无需写vLAPIC IRR，硬件自动同步
 *
 *   2. 传统模式: kvm_lapic_set_irr()
 *      写vLAPIC的IRR寄存器
 *      然后kick vCPU触发KVM_REQ_EVENT
 */
void vmx_deliver_interrupt(struct kvm_lapic *apic, int delivery_mode,
                           int trig_mode, int vector)
{
    struct kvm_vcpu *vcpu = apic->vcpu;

    if (vmx_deliver_posted_interrupt(vcpu, vector)) {
        /* PI投递失败 → 回退到传统模式 */
        kvm_lapic_set_irr(vector, apic);
        kvm_make_request(KVM_REQ_EVENT, vcpu);
        kvm_vcpu_kick(vcpu);
    } else {
        trace_kvm_apicv_accept_irq(vcpu->vcpu_id, delivery_mode,
                                   trig_mode, vector);
    }
}
```

---

## 7. 数据结构关系总图

```
完整数据结构关系:

  QEMU/KVM用户空间                    KVM内核层                      VT-d/IOMMU层
  ═══════════════                    ══════════                      ══════════════

  irqfd/KVM_IRQ_LINE                struct kvm_vcpu                 struct intel_iommu
       │                            ├── arch                        ├── ir_table[]
       │                            │   ├── apic (vLAPIC)          │   ├── irte[0] ──┐
       │                            │   │   ├── regs (IRR/ISR/TMR) │   ├── irte[1]   │
       │                            │   │   └── irr_pending        │   └── irte[N]   │
       ▼                            │   │                          │                 │
  kvm_set_irq() ──────────────▶    │   └── pending_events         │                 │
       │                            │                              │                 │
       │                            ├── (VMX)                      │                 │
       │                            │   └── struct vcpu_vmx        │                 │
       │                            │       ├── pi_desc ──────────────────────────┐  │
       │                            │       │   ├── pir[256] ◄── IOMMU硬件写      │  │
       │                            │       │   ├── on ◄────── IOMMU硬件设1       │  │
       │                            │       │   ├── sn ←── KVM调度时设/清         │  │
       │                            │       │   ├── nv  (通知向量)                 │  │
       │                            │       │   └── ndst (pCPU APIC ID)           │  │
       │                            │       │                                     │  │
       │                            │       └── vcpu                              │  │
       │                            │                                             │  │
       │                            └─────────────────────────────────────────────┘  │
       │                                                                             │
       │  kvm_x86_ops.pi_update_irte() ──────────────────────────────────────────────┘
       │  → vmx_pi_update_irte()
       │     IRTE.PDA = __pa(&vmx→pi_desc)
       │     IRTE.DM = 1 (PI模式)
       │     → modify_irte() → 写入IR表 → qi_flush_iec()
       │
       ▼
  kvm_irq_delivery_to_apic()
       │
       ├──→ vmx_deliver_posted_interrupt()  [PI模式]
       │      pi_test_and_set_pir(vector)
       │      发送通知中断到pCPU
       │
       └──→ kvm_lapic_set_irr()             [传统模式]
              写vLAPIC IRR
              kick vCPU
```

---

## 8. PIC 模拟代码路径 (i8259.c)

```
PIC中断注入完整路径:

irqfd_inject() → kvm_set_irq() → kvm_pic_set_irq()
  │
  ├── pic_set_irq1()                    ← 设置IRR (边沿/电平触发)
  │     边沿: 检测last_irr变化, 只有上升沿才设置irr
  │     电平: level=1时设置irr, level=0时清除irr
  │
  ├── pic_update_irq()                  ← 优先级评估
  │     1. pic_get_irq(slave) → 评估slave PIC
  │     2. 如果有中断: pic_set_irq1(master, IR2, pulse)  ← 级连
  │     3. pic_get_irq(master) → 评估master PIC
  │     4. pic_irq_request() → 设置s->output (模拟INTR引脚)
  │
  └── pic_unlock()                      ← 唤醒vCPU
        kvm_vcpu_kick():
          ├── 睡眠中 → kvm_vcpu_wake_up()
          └── Guest中 → smp_send_reschedule() (IPI kick出)

Guest收到中断:
  kvm_check_and_inject_events()         ← vcpu_enter_guest()前调用
    → kvm_cpu_has_injectable_intr()     ← 检查是否允许注入
    → kvm_cpu_get_interrupt()
      → kvm_cpu_get_extint()
        → kvm_pic_read_irq()            ← 获取最高优先级中断
          → pic_get_irq(master)
          → pic_intack()                ← ISR[irq]=1, IRR[irq]=0
    → kvm_x86_call(inject_irq)()
      → vmx_inject_irq()
        → vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, intr)

Guest关中断时 (IF=0):
  vmx_interrupt_blocked() → true
  kvm_x86_call(enable_irq_window)()
    → exec_controls_setbit(CPU_BASED_INTR_WINDOW_EXITING)
    → Guest执行STI后, 硬件触发VM-Exit
    → KVM再次尝试注入

Guest处理完中断后 (EOI):
  Guest写IO端口0x20 → pic_ioport_write()
    → cmd = val >> 5
    → case 1 (EOI): get_priority(isr) → pic_clear_isr(irq)
    → pic_update_irq() → 检查是否有pending中断
```

---

## 9. IOAPIC 模拟代码路径 (ioapic.c)

```
IOAPIC中断注入完整路径:

Guest写RTE (MMIO):
  ioapic_mmio_write() → ioapic_write_indirect()
    index = (ioregsel - 0x10) >> 1     ← 从寄存器地址反推pin号
    根据 ioregsel 奇偶决定写高/低32位

中断触发:
  irqfd → kvm_set_irq → kvm_ioapic_set_irq → ioapic_set_irq()
    ioapic->irr |= (1 << irq)          ← 设置IRR
    ioapic_service()
      │
      ├── 检查RTE.mask和RTE.remote_irr (level触发EOI阻塞)
      │
      └── RTE → kvm_lapic_irq 转换:
            {vector, dest_id, dest_mode, trig_mode, delivery_mode}
            │
            └── kvm_irq_delivery_to_apic()
                  ├── 快路径: kvm_irq_delivery_to_apic_fast (缓存)
                  └── 慢路径: 遍历所有vCPU匹配dest_id
                        │
                        └── kvm_apic_set_irq → __apic_accept_irq
                              │
                              ├── 设置TMR (触发模式寄存器)
                              │
                              └── kvm_x86_call(deliver_interrupt)()
                                    → vmx_deliver_interrupt()
                                      ├── PI尝试: vmx_deliver_posted_interrupt()
                                      │   PIR[vec]=1, ON=1, 发通知IPI
                                      │   成功 → 零VM-Exit!
                                      └── 回退: kvm_lapic_set_irr()
                                          写vLAPIC IRR + kick

Guest写EOI:
  apic_set_eoi()
    → apic_clear_isr(vector)
    → apic_update_ppr()
    → kvm_ioapic_send_eoi()            ← 通知IOAPIC
      → ioapic->remote_irr &= ~(1 << irq)
      → 检查引脚电平: level且引脚仍高 → 重新投递
```

---

## 10. FAQ 解答

### Q1: 什么是APICv？它包含哪些硬件特性？

APICv (Virtual APIC) 是一组VMCS控制位的集合：
| 特性 | VMCS控制 | 作用 |
|------|---------|------|
| Virtual APIC Page | VIRTUAL_APIC_PAGE_ADDR | Guest读写TPR/IRR/ISR不VM-Exit |
| Virtual Interrupt Delivery | 二级执行控制 bit 37 | 硬件自动评估IRR并注入 |
| EOI Virtualization | 二级执行控制 bit 38 | 硬件自动处理EOI |
| Posted Interrupts | PIN_BASED bit 7 | 外部中断零VM-Exit投递 |
| APIC-register virt | 二级执行控制 bit 39 | 大部分LAPIC寄存器不VM-Exit |

### Q2: 虚拟中断投递一定走Posted Interrupt吗？

**不一定**。PI路径仅在以下条件全部满足时启用：
1. CPU支持APICv (`cpu_has_vmx_apicv()`)
2. `enable_apicv=1`
3. 设备是直通设备 (VFIO)，通过irqfd注册
4. IOMMU支持中断重映射 (IRTE的IM=1)

虚拟设备 (virtio等) 的中断通常走传统路径: `kvm_lapic_set_irr()`

### Q3: IOMMU Interrupt Remapping是必须的吗？

| 场景 | 是否需要IR |
|------|-----------|
| 直通设备 (VFIO) | ★ 必须! 安全+PI支持 |
| 虚拟设备 (virtio) | 不需要 |
| 不使用设备直通 | 不需要 |

### Q4: 直通设备中断如何bypass VMM?

```
传统路径:  设备MSI → Host内核IRQ → KVM软件注入 → VM-Exit (≥1次)
PI路径:    设备MSI → IOMMU(IRTE) → PI.PIR → vCPU (0次VM-Exit!)

关键初始化:
  QEMU: eventfd → irqfd → ioctl(KVM_IRQFD)
  KVM:  kvm_vfio_setup_pi_irte()
        → vmx_pi_update_irte()
          IRTE.PDA = __pa(&vmx→pi_desc)  ← PI描述符物理地址
          IRTE.DM  = 1                     ← PI模式
          IRTE.NV  = POSTED_INTR_VECTOR    ← 通知向量
          → modify_irte() → 写入IR表
```

### Q5: PI到达时vCPU在不同状态怎么处理？

| vCPU状态 | 处理方式 |
|----------|---------|
| Guest Mode运行中 | 硬件自动PIR→IRR, 零VM-Exit, Guest直接处理 |
| Root Mode (KVM中) | 下次VM-Entry前 `vmx_sync_pir_to_irr()` 同步 |
| Halted | PI通知中断 → `pi_wakeup_handler()` → `kvm_vcpu_kick()` → 唤醒 |
| 被调出 | SN=1抑制通知, 调度回来时 `pi_load()` 检查PIR |
| Guest关中断(IF=0) | PIR→IRR同步不受IF影响, 开中断后硬件自动评估 |

### Q6: 如何支持大于256个VCPU？

- xAPIC: APIC ID = LAPIC ID寄存器, 8位, 最多255个(1个留给广播)
- x2APIC: APIC ID = MSR 0x802, 32位, 理论上无限制
- IOAPIC RTE: xAPIC模式dest_id=8位, x2APIC模式扩展到32位
- KVM: `KVM_MAX_VCPU_IDS` 控制上限
- 需要Guest内核和Host都启用x2APIC模式
