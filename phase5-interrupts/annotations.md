# Phase 5：源码精读注释 - 中断虚拟化 + VT-d 中断重映射

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。
> **命名约定**：VT-d 规范术语（IM / VV / NV）用于硬件描述；内核源码字段名（`pst` / `p_vector`）
> 用于代码引用。两者映射关系见 §2。
>
> **体例：问题链**（COURSE_FRAMEWORK §3.2）。节序沿中断路径：
> PI 描述符 → IRTE → 两条 Posted 分水岭 → PIR→IRR → 桥梁函数 → 调度 → 投递 → 零 VM-Exit 核心原则。

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
| vCPU 在 Guest 且 SN=0 | guest 态收到通知中断（向量=NV）时，硬件自动完成 PIR→VIRR→RVI（SDM 30.6，posted-interrupt processing 由中断到达触发，不是 VM-Entry 时机） | ★ **零 VM-Exit** |
| vCPU 在 Guest 且 SN=1 | 非紧急中断不发送通知；紧急中断（URG=1）不受 SN 抑制 | 延迟 |
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
| VV (Virtual Vector) | `p_vector` / `vector` | bits 23:16 | **guest 向量**——"the vector associated with the interrupt requests posted through this IRTE"（Spec 9.10, Figure 9-10），即写入 PIR 的位索引 |
| NV (Notification Vector) | — | PI Desc `nv`（bits 279:272） | **宿主物理通知向量**——"the physical vector used for the notification event"（Spec 9.11）；KVM 中为 POSTED_INTR_VECTOR(0xf2)/WAKEUP(0xf1)。**与 VV 值不同** |
| PDAL | `pda_l` | bits 63:38 | PDA 低 26 位（地址 bits 31:6） |
| PDAH | `pda_h` | bits 127:96 | PDA 高 32 位（地址 bits 63:32） |
| URG | `p_urgent` | bit 14 | Posted 模式紧急位 |

**⚠️ 已知陷阱**（CLAUDE.md #1）：区分 Remapped/Posted 的是 `IM` (bit 15)，**不是 `DM`**。
Remapped 格式里 `DM` (Destination Mode, bit 2) 和 `DLM` (Delivery Mode, bits 7:5) 是
两个完全不同的字段。在 Posted 上下文中写 `DM=1` 表示 PI 模式是**错误的**。

**⚠️ 已知陷阱**（CLAUDE.md #2）：IRTE 里该字段叫 `VV` (Virtual Vector)，PI Descriptor
里叫 `NV` (Notification Vector)——**命名与值都不同**：VV 是记录进 PIR 的 guest 向量
（`posted_intr.c` 中 `vcpu_info.vector = irq.vector`，来自 MSI 路由项），NV 是宿主侧
通知 IPI 的物理向量（0xf2/0xf1）。别把 VV 写成"通知向量"。

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

**执行顺序必须是：① 清 ON → ② 屏障 → ③ 读 PIR**（与上方代码一致，`pi_clear_on`
在 `kvm_apic_update_irr` 之前）

```
KVM:                    IOMMU:
① 清 ON=0
② smp_mb()
③ 读 PIR（全部拷贝）    ④ 写 PIR[新向量] + 设 ON=1
                        ⑤ 看到 ON=0 → 发通知中断
```

为什么不能先读 PIR 再清 ON：若顺序反过来，IOMMU 可能在"KVM 读完 PIR、还没清 ON"
的窗口里写入新位并发现 ON=1 而**不发通知**，随后 KVM 清掉 ON——这个中断就丢了。
现在的顺序保证：清 ON 之后到达的任何 PIR 写入都伴随一次新的 ON 0→1 转换，
IOMMU 必然发通知；而 KVM 清 ON 与读 PIR 之间的屏障保证"ON 被 IOMMU 观察到为 0 时，
KVM 尚未开始读"这一交错不会漏位。内核注释（vmx.c:6924-6926）明确：
"IOMMU can write to PID.ON, so the barrier matters even on UP"——它是配对所需的
真屏障（x86 上恰因 `clear_bit` 已是锁定指令而退化为编译器屏障），**不是防御性摆设**。

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

真实签名与主体（节选自 6.12.93 源码，循环骨架保留）：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:272（节选，省略多路由项循环体注释） */
int vmx_pi_update_irte(struct kvm *kvm, unsigned int host_irq,
		       uint32_t guest_irq, bool set)
{
	/* ...取 irq_routing，检查 guest_irq 是否有路由... */

	hlist_for_each_entry(e, &irq_rt->map[guest_irq], link) {
		if (e->type != KVM_IRQ_ROUTING_MSI)
			continue;

		kvm_set_msi_irq(kvm, e, &irq);
		if (!kvm_intr_is_single_vcpu(kvm, &irq, &vcpu) ||
		    !kvm_irq_is_postable(&irq))
			continue;

		vcpu_info.pi_desc_addr = __pa(vcpu_to_pi_desc(vcpu));
		vcpu_info.vector = irq.vector;      /* ★ guest 向量（写进 PIR 的位），
		                                       不是 PI desc 的 nv */

		if (!set)
			continue;

		enable_remapped_mode = false;
		ret = irq_set_vcpu_affinity(host_irq, &vcpu_info); /* → IOMMU 层 */
	}

	if (enable_remapped_mode)
		ret = irq_set_vcpu_affinity(host_irq, NULL); /* ★ 退回 Remapped 模式 */
	/* ... */
}
```

要点：① `host_irq` 是入参，调用方为 `kvm_arch_irq_bypass_add/del_producer` 与
`kvm_arch_update_irqfd_routing`（x86.c:13678/13708/13723 → `pi_update_irte` static call）；
函数内部**没有** `kvm_irq_to_host_irq()` 这个转换函数（内核不存在该符号）；
② 传给 IOMMU 的 `vector` 是 **guest 向量** `irq.vector`；③ `set=false` 不是简单
return——当没有任何可投递的路由项时走 `irq_set_vcpu_affinity(host_irq, NULL)`
把 IRTE 切回 Remapped。

### Q: IOMMU 层怎么写 IRTE？

**文件**: `drivers/iommu/intel/irq_remapping.c:1248` — `intel_ir_set_vcpu_affinity()`

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:1269-1279（节选） */

irte_pi.p_pst     = 1;        /* ★ IM=1 → Posted 模式（内核字段名 p_pst） */
irte_pi.p_urgent  = 0;
irte_pi.p_vector  = vcpu_pi_info->vector;  /* ★ VV = guest 向量（KVM 传入的
                                            *    irq.vector），不是通知向量！
                                            *    通知向量 NV 在 PI Descriptor 里 */
irte_pi.pda_l = (vcpu_pi_info->pi_desc_addr >> (32 - PDA_LOW_BIT))
                & ~(-1UL << PDA_LOW_BIT);
irte_pi.pda_h = (vcpu_pi_info->pi_desc_addr >> 32)
                & ~(-1UL << PDA_HIGH_BIT);

ir_data->irq_2_iommu.posted_vcpu = true;    /* :1278 */
modify_irte(&ir_data->irq_2_iommu, &irte_pi); /* :1279 → 写入 IR 表 */
```

### Q: `modify_irte()` 的更新协议？

**文件**: `drivers/iommu/intel/irq_remapping.c:156` — `modify_irte()`

**不存在"清 Present → 写 → 设 Present"的四步协议**。Posted IRTE 用 128 位原子比较交换
一次性替换，避免中间态（6.12.93 实际代码）：

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:174-189（节选） */
	if ((irte->pst == 1) || (irte_modified->pst == 1)) {
		/*
		 * We use cmpxchg16 to atomically update the 128-bit IRTE, ...
		 */
		u128 old = irte->irte;
		WARN_ON(!try_cmpxchg128(&irte->irte, &old, irte_modified->irte));
	} else {
		WRITE_ONCE(irte->low, irte_modified->low);
		WRITE_ONCE(irte->high, irte_modified->high);
	}
	__iommu_flush_cache(iommu, irte, sizeof(*irte));

	rc = qi_flush_iec(iommu, index, 0);
```

（index = `irte_index + sub_handle`，全程持 `irq_2_ir_lock`；非 posted 情况是两次
WRITE_ONCE，同样没有先清 Present。）

**注意：vCPU 迁移（宿主调度）不会走到这里**——Posted 格式 IRTE 没有 destination
字段（VT-d Spec 9.10, Figure 9-10 仅含 P/FPD/AVAIL/URG/IM/VV/PDA/SID/SQ/SVT），
通知目标是 PI Descriptor 的 NDST，迁移只由 `vmx_vcpu_pi_load()` 以
`try_cmpxchg64` 更新 NDST，**不写 IRTE、不刷 IEC**。`modify_irte()` 的调用时机是
路由/绑定变更（`intel_ir_set_vcpu_affinity`、回退 Remapped、宿主 posted-MSI 亲和性
变更等）。宿主中断亲和性变更对 guest posted IRTE 更是显式跳过：
`__intel_ir_reconfigure_irte()` 开头 `if (ir_data->irq_2_iommu.posted_vcpu && !force_host) return;`
（irq_remapping.c:1173-1176）。（AGENTS.md 陷阱 #18）

---

## 6. vCPU 调度时的 PI 操作

### Q: vCPU 加载到 pCPU 时 PI 描述符怎么更新？

**文件**: `arch/x86/kvm/vmx/posted_intr.c:53` — `vmx_vcpu_pi_load()`

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:53-133（节选，省略 wakeup 链表摘除与注释） */
void vmx_vcpu_pi_load(struct kvm_vcpu *vcpu, int cpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);
    struct pi_desc old, new;
    unsigned int dest;

    if (!enable_apicv || !lapic_in_kernel(vcpu))
        return;

    /* 快速路径：没迁移 + 不在 wakeup 列表 → 只需清 SN */
    if (pi_desc->nv != POSTED_INTR_WAKEUP_VECTOR && vcpu->cpu == cpu) {
        if (pi_test_and_clear_sn(pi_desc))
            goto after_clear_sn;
        return;                       /* 没清到 SN：什么都不做 */
    }

    /* （此处：若之前挂在 wakeup 列表，先从旧 pCPU 链表摘除，:91-95） */

    dest = cpu_physical_id(cpu);      /* :97 = per_cpu(x86_cpu_to_apicid, cpu),
                                       *   arch/x86/include/asm/smp.h:129 */
    if (!x2apic_mode)
        dest = (dest << 8) & 0xFF00;  /* :99 xAPIC：APIC ID 放在 NDST 高字节 */

    /* ★ 慢路径：try_cmpxchg64 原子更新整个 control 字段 */
    old.control = READ_ONCE(pi_desc->control);
    do {
        new.control = old.control;
        new.ndst = dest;                              /* 新 pCPU 物理 APIC ID */
        __pi_clear_sn(&new);                          /* 清 SN → 允许通知 */
        new.nv = POSTED_INTR_VECTOR;                  /* 恢复通知向量 0xf2 */
    } while (pi_try_set_control(pi_desc, &old.control, new.control));
                                              /* 内部: try_cmpxchg64 (line 47) */

after_clear_sn:
    /* Clear SN before reading the bitmap...（内核注释 :123-127） */
    smp_mb__after_atomic();             /* :129 */
    if (!pi_is_pir_empty(pi_desc))
        pi_set_on(pi_desc);             /* :132 ★ 置 ON=1，不是 KVM_REQ_EVENT！
                                         *   让后续通知/同步路径把 PIR 搬进 IRR */
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
    │   guest 态收到通知中断时硬件自动 PIR→VIRR（SDM 30.6）→ Guest 直接处理（零 VM-Exit!）
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

**问题链**（入口来自第一性原理："中断注入为什么能零 VM-Exit？"）：

1. 入口：`handle_external_interrupt_irqoff()` 是 PI 的正常路径吗？→ **不是**
2. 引申：正常 Posted 路径在哪？→ **硬件**自动清 ON→PIR→VIRR→评估（SDM 30.6），
   KVM 代码根本不执行
3. 引申（反事实）：那这段 KVM 代码处理什么？→ 向量不匹配 / PI 未启用 / 嵌套 / vCPU 迁移
   等**旁路**情况
4. 链尾：旁路里 vCPU 迁移怎么维护 PI 状态？→ §6 + vcpu-migration-affinity.md；
   真机验证零 VM-Exit → 实验 ex3

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
| Virtual Interrupt Delivery (VID) | 二级执行控制 **bit 9**（`SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY`=0x200，`VMX_FEATURE_VIRT_INTR_DELIVERY`=2*32+9，vmxfeatures.h:71） | 硬件自动评估 IRR 并注入 |
| EOI Virtualization | **不是独立控制位**：随 VID 生效，由 VMCS 的 EOI-exit bitmap 决定哪些向量不产生 MSR-write VM-Exit（SDM 30.1.2） | 硬件自动处理 EOI |
| Posted Interrupts | `PIN_BASED` bit 7（`VMX_FEATURE_POSTED_INTR`=0*32+7，vmxfeatures.h:21） | 外部中断零 VM-Exit 投递 |
| APIC-register Virt | 二级执行控制 **bit 8**（`SECONDARY_EXEC_APIC_REGISTER_VIRT`=0x100，vmxfeatures.h:70） | LAPIC 寄存器访问不 VM-Exit |

依赖链：`Posted Interrupts → APICv → Virtual Interrupt Delivery + EOI Virt`

### Q: 虚拟中断投递一定走 PI 吗？

**不一定**。PI 路径需要以下条件全部满足：

1. CPU 支持 APICv（`cpu_has_vmx_apicv()`）
2. `enable_apicv=1`（模块参数，0444 只读；宿主实测 `/sys/module/kvm_intel/parameters/enable_apicv`）
3. vCPU 的 `apicv_active` 已置位——由 enable_apicv 且无 inhibit 位决定（vCPU 创建/apicv
   状态变更时设置），**与"直通设备是否注册 irqfd"无关**；反过来，有直通设备但平台不支持
   PI 才是 inhibit 来源之一
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
      ├→ vcpu_info.pi_desc_addr = __pa(vcpu_to_pi_desc(vcpu))
      ├→ vcpu_info.vector = irq.vector      ★ guest 向量（非 pi_desc.nv）
      └→ irq_set_vcpu_affinity()     (irq_remapping.c)
          └→ intel_ir_set_vcpu_affinity()  (irq_remapping.c:1248)
              ├→ irte_pi.p_pst = 1          ★ IM=1 (Posted 模式)
              ├→ irte_pi.p_vector = vector  （VV = guest 向量）
              ├→ irte_pi.pda_l / pda_h = 物理地址
              └→ modify_irte()              (irq_remapping.c:156)
                  ├→ try_cmpxchg128 原子替换 128 位 IRTE（pst 相关路径）
                  ├→ __iommu_flush_cache()
                  └→ qi_flush_iec()         ★ 刷新 IEC


═══════════════════ vCPU 调度时的 PI 维护 ═══════════════════

  vcpu_load() → vmx_vcpu_pi_load()    (posted_intr.c:53)
    ├→ 快速路径: pi_test_and_clear_sn()（未迁移且非 wakeup → 清 SN 后返回）
    └→ 慢路径: try_cmpxchg64(&pi_desc->control, ...)
        ├→ new.ndst = cpu_physical_id(cpu)（xAPIC 时 (dest<<8)&0xFF00）
        ├→ __pi_clear_sn(&new)
        ├→ new.nv = POSTED_INTR_VECTOR
        └→ after_clear_sn: smp_mb(); PIR 非空 → pi_set_on()   ★ 不是 KVM_REQ_EVENT
        （★ 只改 PI Descriptor，IRTE 不动——Posted IRTE 无 destination 字段）

  vcpu_put() → vmx_vcpu_pi_put()      (posted_intr.c:196)
    ├→ blocking + 中断未屏蔽:
    │   └→ pi_enable_wakeup_handler() → NV = WAKEUP_VECTOR
    └→ preempted:
        └→ pi_set_sn() → SN=1
```
