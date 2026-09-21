# Phase 4：源码精读注释 - 中断虚拟化 + VT-d 中断重映射

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。
> **命名约定**：VT-d 规范术语（IM / VV / NV）用于硬件描述；内核源码字段名（`pst` / `p_vector`）
> 用于代码引用。两者映射关系见 §2。

---

## 1. PI 描述符 — KVM 与 VT-d 的硬件交汇

### Q: Posted Interrupt Descriptor 长什么样？谁写哪些字段？

**文件**: `arch/x86/include/asm/posted_intr.h:12` — `struct pi_desc`

```c
/* 来源: arch/x86/include/asm/posted_intr.h:12 */
struct pi_desc {
    union {
        u32 pir[8];         /* ★ 256-bit PIR 位图，IOMMU 硬件写 */
        u64 pir64[4];
    };
    union {
        struct {
            u16 notifications;  /* ON(bit 0) + SN(bit 1) + 保留 */
            u8  nv;             /* Notification Vector */
            u8  rsvd_2;
            u32 ndst;           /* 目标 pCPU 的物理 APIC ID */
        };
        u64 control;            /* ★ 8 字节，cmpxchg64 原子操作用 */
    };
    u32 rsvd[6];               /* 填充到 64 字节 */
} __aligned(64);
```

**`notifications` 是 u16，不是独立 bitfield** — ON 和 SN 是它的最低两个 bit：

| Bit | 名称 | 含义 |
|-----|------|------|
| 0 | ON (Outstanding Notification) | 有中断 pending 在 PIR 中，需要同步 |
| 1 | SN (Suppress Notification) | 抑制通知中断（vCPU 被抢占/blocking 时置位） |

**为什么没有 `sn`/`on` 位域？** 因为 ON/SN 操作需要原子性保障。`struct pi_desc` 用
`u64 control` 联合，允许 `try_cmpxchg64(&pi_desc->control, ...)` 一次性原子更新整个
control 字段（含 NDST + NV + ON + SN），防止 IOMMU 在修改中间读到半更新状态。

辅助函数通过 `set_bit`/`test_bit` 操作 `control` 的对应位：

```c
/* 来源: arch/x86/include/asm/posted_intr.h */
#define POSTED_INTR_ON  0                     /* control 的 bit 0 */
#define POSTED_INTR_SN  1                     /* control 的 bit 1 */

static inline bool pi_test_on(struct pi_desc *pi_desc)           /* :74 */
{ return test_bit(POSTED_INTR_ON, (unsigned long *)&pi_desc->control); }

static inline bool pi_test_and_clear_on(struct pi_desc *pi_desc) /* :34 */
{ return test_and_clear_bit(POSTED_INTR_ON,
                            (unsigned long *)&pi_desc->control); }

static inline void pi_set_sn(struct pi_desc *pi_desc)            /* :54 */
{ set_bit(POSTED_INTR_SN, (unsigned long *)&pi_desc->control); }

/* 非原子版本 — 仅用于 cmpxchg 循环内部（已持有 old 值） */
static inline void __pi_clear_sn(struct pi_desc *pi_desc)        /* :90 */
{ pi_desc->notifications &= ~BIT(POSTED_INTR_SN); }
```

### Q: 硬件怎么通过这个结构投递中断？

1. IOMMU 收到设备 MSI，查 IRTE 得到 PDA 地址 → 找到 vCPU 的 `pi_desc`
2. 写 `pir[vector/32] |= (1 << (vector%32))` — 标记该向量 pending
3. 设 `ON = 1`（atomic set_bit）

**之后行为取决于 vCPU 状态**：

| 条件 | 行为 | VM-Exit? |
|------|------|----------|
| vCPU 在 Guest 且 SN=0 | 硬件在 VM-Entry 前自动 PIR→VIRR | ★ **零 VM-Exit** |
| vCPU 在 Guest 且 SN=1 | 不投递，等 SN 清除后处理 | 延迟 |
| vCPU 不在 Guest | IOMMU 发通知中断到 NDST 指向的 pCPU | 需软件处理 |

**为什么 64 字节对齐？** 确保整个结构在一个缓存行内，IOMMU 和 CPU 对 `control` 字段的
原子操作不会跨缓存行。`__aligned(64)` 是缓存行对齐，**不是物理页对齐**。

### Q: 为什么 `control` 要设计成 u64 联合？

`nv`、`ndst`、`notifications`（含 ON/SN）合在一个 `u64` 里。当 vCPU 迁移到不同 pCPU 时，
KVM 需要同时更新 NDST 和清除 SN。如果分开写，IOMMU 可能在中间读到不一致的状态。
`try_cmpxchg64` 保证一次原子操作完成所有修改。

---

## 2. VT-d IRTE — 内核源码表示

### Q: 内核里 `struct irte` 怎么表示 Posted 和 Remapped 两种格式？

**文件**: `include/linux/dmar.h:201` — `struct irte`

VT-d 规范定义了 128-bit IRTE 的两种格式（Remapped / Posted）。内核用**重叠 union** 提供三种视图：

```c
/* 来源: include/linux/dmar.h:201（简化，省略保留位） */
struct irte {
    union {
        struct {                    /* 共享视图 */
            __u64 present   : 1,    /* [0]   */
                  fpd       : 1,    /* [1]   */
                  /* ... */
                  pst       : 1,    /* ★ [15] — 两种格式共享该位 */
                  vector    : 8,    /* [23:16] */
            /* ... */
        };
        struct {                    /* Remapped 视图 (pst=0) */
            /* ... dlm, dest_mode, destination_id ... */
            __u64 r_res0    : 4;    /* [12:15] — pst 在该视图中是保留位的一部分 */
            __u64 r_vector  : 8;    /* [23:16] */
            /* ... */
        };
        struct {                    /* ★ Posted 视图 (pst=1) */
            __u64 p_present : 1,    /* [0]   */
                  p_fpd     : 1,    /* [1]   */
                  p_res0    : 6,    /* [7:2] */
                  p_avail   : 4,    /* [11:8] */
                  p_res1    : 2,    /* [13:12] */
                  p_urgent  : 1,    /* [14]  */
                  p_pst     : 1,    /* ★ [15] — IRTE Mode */
                  p_vector  : 8,    /* ★ [23:16] — Virtual Vector (VV) */
                  p_res2    : 14,   /* [37:24] */
                  pda_l     : 26;   /* ★ [63:38] — PDA 低 26 位 */
            __u64 p_sid     : 16,   /* [79:64] */
                  p_sq      : 2,    /* [81:80] */
                  p_svt     : 2,    /* [83:82] */
                  p_res3    : 12,   /* [95:84] */
                  pda_h     : 32;   /* ★ [127:96] — PDA 高 32 位 */
        };
        __u64 raw[2];
    };
};
```

### Q: 规范术语 vs 内核字段名对照？

| 规范名 | 内核字段 | 位置 | 说明 |
|--------|---------|------|------|
| IM (IRTE Mode) | `pst` / `p_pst` | bit 15 | **0=Remapped, 1=Posted** |
| VV (Virtual Vector) | `p_vector` / `vector` | bits 23:16 | IRTE 中的通知向量 |
| NV (Notification Vector) | — | PI Desc `nv` | PI 描述符中，值与 VV 相同 |
| PDAL | `pda_l` | bits 63:38 | PDA 低 26 位（地址 bits 31:6） |
| PDAH | `pda_h` | bits 127:96 | PDA 高 32 位（地址 bits 63:32） |
| URG | `p_urgent` | bit 14 | Posted 模式紧急位 |

**⚠️ 已知陷阱**（CLAUDE.md #1）：区分 Remapped/Posted 的是 `IM` (bit 15)，**不是 `DM`**。
Remapped 格式里 `DM` (Destination Mode, bit 2) 和 `DLM` (Delivery Mode, bits 7:5) 是
两个完全不同的字段。在 Posted 上下文中写 `DM=1` 表示 PI 模式是**错误的**。

**⚠️ 已知陷阱**（CLAUDE.md #2）：IRTE 里该字段叫 `VV` (Virtual Vector)；PI Descriptor
里叫 `NV` (Notification Vector)。值相同，命名不同，别混用。

### Q: PDA 的地址对齐和位范围？

PDA 总 58 位（`pda_l` 26 位 + `pda_h` 32 位），64 字节对齐：

```
pda_l [bits 63:38] → 地址 bits 31:6   (26 bits)
pda_h [bits 127:96] → 地址 bits 63:32  (32 bits)
合计 58 位，低 6 位隐式为 0（64 字节对齐）

来源: include/linux/dmar.h:286-287
#define PDA_LOW_BIT  26
#define PDA_HIGH_BIT 32
```

---

## 3. `irq_2_iommu` — 两条 Posted 路径的分水岭

### Q: `struct irq_2_iommu` 里有两个 `posted` 标志，分别是什么？

**文件**: `drivers/iommu/intel/irq_remapping.c:43` — `struct irq_2_iommu`

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:43 */
struct irq_2_iommu {
    struct intel_iommu *iommu;
    u16 irte_index;
    u16 sub_handle;
    u8  irte_mask;
    bool posted_msi;      /* ★ line 48 — 宿主自身 MSI 合并 */
    bool posted_vcpu;     /* ★ line 49 — KVM Guest PI 投递到 vCPU */
};
```

**⚠️ 已知陷阱**（CLAUDE.md #8）：这两个是**完全独立的路径**，都叫 "posted" 但用途不同：

| 路径 | 标志 | 触发条件 | PDA 指向 | 作用 |
|------|------|---------|---------|------|
| 宿主 Posted MSI | `posted_msi` | `CONFIG_X86_POSTED_MSI` + `posted_msi_supported()` | 宿主 per-CPU PI descriptor | 宿主自身的 MSI 合并优化，与 Guest 无关 |
| Guest VT-d PI | `posted_vcpu` | `intel_ir_set_vcpu_affinity()` | vCPU 的 PI descriptor | KVM 直通设备中断直接投递给 vCPU |

`prepare_irte_posted()`（`irq_remapping.c:1111`）走宿主路径；`intel_ir_set_vcpu_affinity()`
（`:1248`）走 Guest 路径。两者在 alloc 阶段分别设置各自的标志，互不干扰。

### Q: `struct intel_ir_data` 怎么组织？

**文件**: `drivers/iommu/intel/irq_remapping.c:54`

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:54 */
struct intel_ir_data {
    struct irq_2_iommu irq_2_iommu;  /* IOMMU 映射 + posted 标志 */
    struct irte irte_entry;           /* 128-bit IRTE */
    struct msi_msg msi_entry;         /* MSI 消息（地址+数据） */
};
```

这个结构作为 `irq_cfg` 的私有数据，贯穿 Linux IRQ 子系统和 IOMMU 中断重映射。

---

## 4. PIR→IRR 同步 — `vmx_sync_pir_to_irr()`

### Q: IOMMU 写入的 PIR 怎么到达 vLAPIC 的 IRR？

**文件**: `arch/x86/kvm/vmx/vmx.c:6912` — `vmx_sync_pir_to_irr()`

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:6912 */
int vmx_sync_pir_to_irr(struct kvm_vcpu *vcpu)
{
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    int max_irr;
    bool got_posted_interrupt;

    if (KVM_BUG_ON(!enable_apicv, vcpu->kvm))
        return -EIO;

    if (pi_test_on(&vmx->pi_desc)) {                  /* ★ ON=1? */
        pi_clear_on(&vmx->pi_desc);                   /* 清 ON */
        smp_mb__after_atomic();                       /* ★ 屏障 */

        /* 读 PIR → 写入 vLAPIC IRR，返回最高优先级向量 */
        got_posted_interrupt =
            kvm_apic_update_irr(vcpu, vmx->pi_desc.pir, &max_irr);
    } else {
        max_irr = kvm_lapic_find_highest_irr(vcpu);
        got_posted_interrupt = false;
    }

    /* ★ 更新 RVI → 触发 VID（Virtual Interrupt Delivery） */
    if (!is_guest_mode(vcpu) && kvm_vcpu_apicv_active(vcpu))
        vmx_set_rvi(max_irr);                         /* line 6951 */
    else if (got_posted_interrupt)
        kvm_make_request(KVM_REQ_EVENT, vcpu);

    return max_irr;
}
```

### Q: 为什么清 ON 之后需要 `smp_mb__after_atomic()`？

**执行顺序必须是：① 读 PIR → ② 清 ON → ③ 屏障**

```
KVM:                    IOMMU:
① 读 PIR（全部拷贝）
② 清 ON=0
③ smp_mb()
                        ④ 写 PIR[新向量]
                        ⑤ 看到 ON=0 → 发通知中断
```

屏障保证 ② 对 IOMMU 可见时，① 已经完成。否则 IOMMU 可能在 KVM 还在读 PIR 时
就重设 ON=1，后续 KVM 看到 ON=1 再次清除但漏掉新的 PIR 写入。

**实际上**：`pi_clear_on` 本身是原子操作（`test_and_clear_bit`），带隐式屏障。
显式的 `smp_mb__after_atomic()` 是防御性编程，确保在所有架构上顺序正确。

### Q: `vmx_set_rvi()` 做了什么？

**文件**: `arch/x86/kvm/vmx/vmx.c:6881` — `vmx_set_rvi()`

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:6881 */
static void vmx_set_rvi(int vector)
{
    u16 status;
    u8  old;

    if (vector == -1)
        vector = 0;

    status = vmcs_read16(GUEST_INTR_STATUS);     /* line 6889 */
    old = (u8)status & 0xff;                     /* RVI = 低 8 位 */
    if ((u8)vector != old) {
        status &= ~0xff;
        status |= (u8)vector;
        vmcs_write16(GUEST_INTR_STATUS, status); /* line 6894 */
    }
}
```

**RVI (Requested Virtual Interrupt)** = 最高优先级待处理中断向量。VMCS 的
`GUEST_INTR_STATUS` 是 u16：低 8 位 RVI，高 8 位 SVI（Servicing Virtual Interrupt）。
写 RVI 时保持 SVI 不变（`status &= ~0xff` 只清低 8 位）。

**为什么写 RVI？** 配合 VID（Virtual Interrupt Delivery）：VM-Entry 时硬件比较
RVI 和 VPPR（虚拟 PPR）。RVI > VPPR → 硬件自动注入中断到 Guest，**零 VM-Exit**。

---

## 5. KVM-VT-d 桥梁 — IRTE Posted 模式设置

### Q: 直通设备的 IRTE 怎么切换到 Posted 模式？

**文件**: `arch/x86/kvm/vmx/posted_intr.c:272` — `vmx_pi_update_irte()`

**注意**：`vmx_pi_update_irte()` **不直接操作 IRTE 字段**。它构建 `struct vcpu_data`
传给 IOMMU 层，由 `intel_ir_set_vcpu_affinity()` 完成实际的 IRTE 写入。

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:272（简化） */
void vmx_pi_update_irte(struct kvm_vcpu *vcpu,
                        u32 gvec, u32 girq, bool set)
{
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    struct vcpu_data vcpu_info;

    if (!set)
        return;

    /* ★ 构建 vCPU 信息，传给 IOMMU 层 */
    vcpu_info.pi_desc_addr = __pa(&vmx->pi_desc);  /* PI 描述符物理地址 */
    vcpu_info.vector = vmx->pi_desc.nv;             /* 通知向量 */

    host_irq = kvm_irq_to_host_irq(girq);
    irq_set_vcpu_affinity(host_irq, &vcpu_info);    /* → IOMMU 层 */
}
```

### Q: IOMMU 层怎么写 IRTE？

**文件**: `drivers/iommu/intel/irq_remapping.c:1248` — `intel_ir_set_vcpu_affinity()`

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:1270（节选） */
/* intel_ir_set_vcpu_affinity() 内部 */

irte_pi.p_pst     = 1;        /* ★ IM=1 → Posted 模式（内核字段名 p_pst） */
irte_pi.p_urgent  = 0;
irte_pi.p_vector  = vcpu_pi_info->vector;    /* 通知向量 = PI desc 的 nv */
irte_pi.pda_l = (vcpu_pi_info->pi_desc_addr >> (32 - PDA_LOW_BIT))
                & ~(-1UL << PDA_LOW_BIT);
irte_pi.pda_h = (vcpu_pi_info->pi_desc_addr >> 32)
                & ~(-1UL << PDA_HIGH_BIT);

modify_irte(&data->irq_2_iommu, &irte_pi);   /* line 1279 → 写入 IR 表 */
```

### Q: `modify_irte()` 的更新协议？

**文件**: `drivers/iommu/intel/irq_remapping.c:156` — `modify_irte()`

```
1. 清 Present 位（防止更新过程中错误投递）
2. 写入新 IRTE 到内存中的 IR 表
3. qi_flush_iec() → 刷新 IOMMU 的 IEC（Interrupt Entry Cache）
4. 设 Present 位（重新启用）
```

**每次 vCPU 迁移 pCPU 都需要更新 IRTE 的 destination 并刷新 IEC**。这是 PI 的
运维开销 — 中断投递本身零 VM-Exit，但 vCPU 迁移有 IOMMU 侧的缓存失效成本。

---

## 6. vCPU 调度时的 PI 操作

### Q: vCPU 加载到 pCPU 时 PI 描述符怎么更新？

**文件**: `arch/x86/kvm/vmx/posted_intr.c:53` — `vmx_vcpu_pi_load()`

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:53（简化） */
void vmx_vcpu_pi_load(struct kvm_vcpu *vcpu, int cpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);
    struct pi_desc old, new;
    unsigned int dest;

    /* 快速路径：没迁移 + 不在 wakeup 列表 → 只需清 SN */
    if (pi_desc->nv != POSTED_INTR_WAKEUP_VECTOR && vcpu->cpu == cpu) {
        if (pi_test_and_clear_sn(pi_desc))
            goto after_clear_sn;
    }

    /* ★ 慢路径：try_cmpxchg64 原子更新整个 control 字段 */
    old.control = READ_ONCE(pi_desc->control);
    do {
        new.control = old.control;
        new.ndst = per_cpu(x86_cpu_to_physical_apicid, cpu);  /* 新 pCPU APIC ID */
        __pi_clear_sn(&new);                      /* 清 SN → 允许通知 */
        new.nv = POSTED_INTR_VECTOR;              /* 通知向量 */
    } while (pi_try_set_control(pi_desc, &old.control, new.control));
                                              /* 内部: try_cmpxchg64 (line 47) */

after_clear_sn:
    if (!pi_is_pir_empty(pi_desc))
        kvm_make_request(KVM_REQ_EVENT, vcpu);  /* PIR 有数据 → 同步 */
}
```

**为什么用 `__pi_clear_sn(&new)` 而不是 `new.sn = 0`？** 因为 `struct pi_desc` 没有
`sn` 位域。SN 是 `notifications` (u16) 的 bit 1。`__pi_clear_sn()` 做
`pi_desc->notifications &= ~BIT(POSTED_INTR_SN)`，在 cmpxchg 循环内部操作 `new`
的副本，最终由 `try_cmpxchg64` 原子提交。

### Q: vCPU 从 pCPU 卸载时呢？

**文件**: `arch/x86/kvm/vmx/posted_intr.c:196` — `vmx_vcpu_pi_put()`

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:196 */
void vmx_vcpu_pi_put(struct kvm_vcpu *vcpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);

    if (!vmx_needs_pi_wakeup(vcpu))
        return;

    /* vCPU 要阻塞 + 中断未被屏蔽 → 启用 wakeup handler */
    if (kvm_vcpu_is_blocking(vcpu) && !vmx_interrupt_blocked(vcpu))
        pi_enable_wakeup_handler(vcpu);     /* NV 切到 WAKEUP_VECTOR */

    /* vCPU 被抢占 → 设 SN 抑制通知（vCPU 不在运行，通知无意义） */
    if (vcpu->preempted)
        pi_set_sn(pi_desc);                 /* line 212 */
}
```

**两个场景总结**：

| 场景 | 操作 | 原因 |
|------|------|------|
| vCPU blocking | NV → WAKEUP_VECTOR | 通知中断走特殊唤醒路径 |
| vCPU preempted | SN = 1 | 抑制无意义的通知，调度回来后 `pi_load` 清除 |

---

## 7. 完整中断投递路径

### Q: PI 投递的完整代码路径？

**文件**: `arch/x86/kvm/vmx/vmx.c:4269` — `vmx_deliver_posted_interrupt()`

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:4269 */
static int vmx_deliver_posted_interrupt(struct kvm_vcpu *vcpu, int vector)
{
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    int r;

    /* 嵌套虚拟化：先尝试投递给 L1 VMM */
    r = vmx_deliver_nested_posted_interrupt(vcpu, vector);  /* line 4274 */
    if (!r) return 0;

    if (!vcpu->arch.apic->apicv_active)                     /* line 4279 */
        return -1;                              /* APICv 未激活 → 触发回退 */

    if (pi_test_and_set_pir(vector, &vmx->pi_desc))         /* line 4282 */
        return 0;                               /* PIR 位已设 → 通知已 pending */

    if (pi_test_and_set_on(&vmx->pi_desc))                  /* line 4286 */
        return 0;                               /* ON 已设 → 通知已 pending */

    /* ★ 发送通知中断 */
    kvm_vcpu_trigger_posted_interrupt(vcpu, POSTED_INTR_VECTOR); /* line 4295 */
    return 0;
}
```

**`pi_test_and_set_pir`/`pi_test_and_set_on` 返回值的含义**：返回 **旧值**。
返回 1（true）意味着该位在操作前**已经是 1** — 之前的投递已经设好了，不需要
再发通知 IPI。这是避免冗余 IPI 的优化。

### Q: PI 投递和传统投递的入口在哪？

**文件**: `arch/x86/kvm/vmx/vmx.c:4299` — `vmx_deliver_interrupt()`

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:4299 */
void vmx_deliver_interrupt(struct kvm_lapic *apic, int delivery_mode,
                           int trig_mode, int vector)
{
    struct kvm_vcpu *vcpu = apic->vcpu;

    if (vmx_deliver_posted_interrupt(vcpu, vector)) {
        /* ★ PI 投递失败（返回 -1，APICv 未激活） → 回退传统路径 */
        kvm_lapic_set_irr(vector, apic);
        kvm_make_request(KVM_REQ_EVENT, vcpu);
        kvm_vcpu_kick(vcpu);
    } else {
        trace_kvm_apicv_accept_irq(vcpu->vcpu_id, delivery_mode,
                                   trig_mode, vector);
    }
}
```

### Q: 两条路径对比？

```
═══ PI 路径（直通设备 via VT-d IR） ═══

  设备 MSI → IOMMU IR → IRTE(IM=1, PDA) → pi_desc.pir[vec]=1, ON=1
    │
    ├── vCPU 在 Guest + SN=0:
    │   硬件 VM-Entry 前自动 PIR→VIRR → Guest 直接处理（零 VM-Exit!）
    │
    └── vCPU 不在 Guest:
        IOMMU 发通知 IPI → 宿主 IRQ handler
        → kvm_vcpu_kick() 唤醒 vCPU 线程
        → 下次进 Guest 前 vmx_sync_pir_to_irr() 同步


═══ 传统路径（虚拟设备 via vLAPIC） ═══

  irqfd/IOAPIC → kvm_set_irq() → kvm_irq_delivery_to_apic()
    → __apic_accept_irq() → vmx_deliver_interrupt()
      ├── PI 尝试: vmx_deliver_posted_interrupt()
      │   成功 → PIR[vec]=1, ON=1, 发通知 IPI
      └── 回退 (返回 -1): kvm_lapic_set_irr() + kick
```

---

## 8. 核心原则：PI 零 VM-Exit 与旁路处理

### Q: `handle_external_interrupt_irqoff()` 是 PI 的正常路径吗？

**★ 不是！** 这是 CLAUDE.md 强调的核心原则。

**文件**: `arch/x86/kvm/vmx/vmx.c:7013` — `handle_external_interrupt_irqoff()`

**正常 PI 路径（vCPU 在 Guest，SN=0）**：

```
设备 MSI → IOMMU → PI PIR[vec]=1, ON=1
  → 硬件在 VM-Entry 前自动完成：
    1. 清 ON
    2. PIR → VIRR（按优先级合并）
    3. 更新 RVI
    4. 评估中断可注入性
  → Guest 直接收到中断
  → ★ 零 VM-Exit，延迟 < 1μs
```

Intel SDM Section 30.6 原文："without transferring control to the VMM"。
**KVM 代码根本不执行**。

**`handle_external_interrupt_irqoff()` 只处理旁路情况**：

| 旁路场景 | 原因 |
|---------|------|
| 通知向量不匹配 | PI notification vector ≠ 外部中断向量 |
| PI 未启用 | APICv 被关闭 |
| 嵌套虚拟化 | L1 VMM 需要拦截 |
| vCPU 迁移中 | PI desc 目标 pCPU 已变 |

### Q: APICv 包含哪些硬件特性？

| 特性 | VMCS 控制位 | 作用 |
|------|-----------|------|
| Virtual APIC Page | `VIRTUAL_APIC_PAGE_ADDR` | Guest 读 TPR/IRR/ISR 不 VM-Exit |
| Virtual Interrupt Delivery (VID) | 二级执行控制 bit 37 | 硬件自动评估 IRR 并注入 |
| EOI Virtualization | 二级执行控制 bit 38 | 硬件自动处理 EOI |
| Posted Interrupts | `PIN_BASED` bit 7 | 外部中断零 VM-Exit 投递 |
| APIC-register Virt | 二级执行控制 bit 39 | LAPIC 寄存器访问不 VM-Exit |

依赖链：`Posted Interrupts → APICv → Virtual Interrupt Delivery + EOI Virt`

### Q: 虚拟中断投递一定走 PI 吗？

**不一定**。PI 路径需要以下条件全部满足：

1. CPU 支持 APICv（`cpu_has_vmx_apicv()`）
2. `enable_apicv=1`（模块参数，0444 只读）
3. vCPU 的 `apicv_active` 被设置（直通设备通过 `irqfd` 注册后才会）
4. IOMMU 中断重映射已配置 Posted 模式（`p_pst = 1`）

**虚拟设备（virtio 等）的中断通常走传统路径**：`kvm_lapic_set_irr()` 直接写 vLAPIC IRR。

---

## 9. PIC / IOAPIC 模拟关键路径

### Q: PIC 中断注入的完整路径？

**文件**: `arch/x86/kvm/i8259.c`

```
irqfd_inject() → kvm_set_irq() → kvm_pic_set_irq()
  │
  ├── pic_set_irq1()                    ← 设置 IRR（边沿/电平触发）
  ├── pic_update_irq()                  ← 优先级评估 + 级联
  │   └── pic_irq_request()             ← 设 s->output（模拟 INTR 引脚）
  │
  └── pic_unlock() → kvm_vcpu_kick()   ← 唤醒 vCPU

Guest 收到中断:
  kvm_check_and_inject_events()         ← vcpu_enter_guest() 前
    → kvm_cpu_get_interrupt()
      → kvm_pic_read_irq()              ← 取最高优先级中断
      → pic_intack()                    ← ISR=1, IRR=0
    → vmx_inject_irq()
      → vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, intr)

Guest 关中断 (IF=0):
  vmx_interrupt_blocked() → true
  → 开中断窗口退出：Guest 执行 STI 后触发 VM-Exit → KVM 重尝试注入

Guest EOI:
  Guest 写 0x20 → pic_ioport_write() → pic_clear_isr() → pic_update_irq()
```

### Q: IOAPIC 中断注入的完整路径？

**文件**: `arch/x86/kvm/ioapic.c`

```
irqfd → kvm_set_irq() → kvm_ioapic_set_irq() → ioapic_service()
  │
  ├── 检查 RTE.mask 和 RTE.remote_irr
  ├── RTE → kvm_lapic_irq 转换
  └── kvm_irq_delivery_to_apic()
        └── __apic_accept_irq()
              └── vmx_deliver_interrupt()
                    ├── PI 尝试: vmx_deliver_posted_interrupt()
                    │   成功 → 零 VM-Exit!
                    └── 回退: kvm_lapic_set_irr() + kick

Guest EOI:
  apic_set_eoi() → kvm_ioapic_send_eoi()
    → remote_irr &= ~(1 << irq)
    → 电平触发 + 引脚仍高 → 重新投递
```

---

## 10. 完整调用链

```
═══════════════════ PI 路径（直通设备中断） ═══════════════════

  设备 MSI
    │
    ▼
  IOMMU (VT-d)
    ├→ 查 IR 表 → IRTE[p_pst=1, pda_l/pda_h, p_vector]
    │
    ├→ 通过 PDA 找到 vCPU 的 pi_desc（物理地址）
    │   ├→ pir[vector/32] |= (1 << vector%32)     ← 标记 pending
    │   └→ test_and_set_bit(POSTED_INTR_ON, &control)  ← 设 ON
    │
    ├── vCPU 在 Guest + SN=0:
    │   └→ 硬件自动: 清ON → PIR→VIRR → 更新RVI → 评估注入
    │      ★ Guest 直接收到中断，零 VM-Exit
    │
    └── vCPU 不在 Guest:
        └→ 发通知 IPI 到 pi_desc.ndst（pCPU）
           └→ handle_external_interrupt_irqoff()（旁路场景）
              或 pi_wakeup_handler()（vCPU halted）
              └→ kvm_vcpu_kick() → 唤醒 vCPU 线程
                 └→ vmx_sync_pir_to_irr() 同步 PIR→IRR


═══════════════════ 传统路径（虚拟设备中断） ═══════════════════

  QEMU (用户空间)
    ├→ eventfd 触发
    │
    ▼
  KVM (内核)
    ├→ irqfd_inject() → kvm_set_irq()         (irqchip.c:70)
    │   ├→ 查 irq_routing_table
    │   ├─→ PIC:    kvm_pic_set_irq()          (i8259.c)
    │   ├─→ IOAPIC: kvm_ioapic_set_irq()       (ioapic.c)
    │   └─→ MSI:    kvm_set_msi()
    │
    ├→ kvm_irq_delivery_to_apic()
    │   └→ __apic_accept_irq()
    │       └→ vmx_deliver_interrupt()          (vmx.c:4299)
    │           ├→ vmx_deliver_posted_interrupt() (vmx.c:4269)
    │           │   ├→ pi_test_and_set_pir()     ← PIR[vec]=1
    │           │   ├→ pi_test_and_set_on()      ← ON=1
    │           │   └→ kvm_vcpu_trigger_posted_interrupt()
    │           │       └→ 发 IPI 到目标 pCPU
    │           └→ (失败时回退) kvm_lapic_set_irr() + kick


═══════════════════ IRTE Posted 模式设置 ═══════════════════

  QEMU: ioctl(KVM_IRQFD)
    │
    ▼
  KVM:
    vmx_pi_update_irte()              (posted_intr.c:272)
      ├→ vcpu_info.pi_desc_addr = __pa(&vmx->pi_desc)
      ├→ vcpu_info.vector = pi_desc.nv
      └→ irq_set_vcpu_affinity()     (irq_remapping.c)
          └→ intel_ir_set_vcpu_affinity()  (irq_remapping.c:1248)
              ├→ irte_pi.p_pst = 1          ★ IM=1 (Posted 模式)
              ├→ irte_pi.p_vector = vector
              ├→ irte_pi.pda_l / pda_h = 物理地址
              └→ modify_irte()              (irq_remapping.c:156)
                  ├→ 清 Present 位
                  ├→ 写新 IRTE
                  ├→ qi_flush_iec()         ★ 刷新 IEC
                  └→ 设 Present 位


═══════════════════ vCPU 调度时的 PI 维护 ═══════════════════

  vcpu_load() → vmx_vcpu_pi_load()    (posted_intr.c:53)
    ├→ 快速路径: pi_test_and_clear_sn()
    └→ 慢路径: try_cmpxchg64(&pi_desc->control, ...)
        ├→ new.ndst = 当前 pCPU APIC ID
        ├→ __pi_clear_sn(&new)
        ├→ new.nv = POSTED_INTR_VECTOR
        └→ PIR 非空 → KVM_REQ_EVENT

  vcpu_put() → vmx_vcpu_pi_put()      (posted_intr.c:196)
    ├→ blocking + 中断未屏蔽:
    │   └→ pi_enable_wakeup_handler() → NV = WAKEUP_VECTOR
    └→ preempted:
        └→ pi_set_sn() → SN=1
```
