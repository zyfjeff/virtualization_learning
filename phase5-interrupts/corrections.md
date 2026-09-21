# Phase 4 勘误

> 基线：Linux 6.12.93。本文档记录 phase4 文档审查过程中发现的事实错误及其修复。

---

## 勘误 1：`struct pi_desc` 定义错误

**位置**: `posted-interrupts.md` §4.1

**错误**：
```c
struct {
    u8  on      : 1,
        sn      : 1,
        rsvd_1  : 6;
    u8  nv;
    u32 ndst;
};
```

**正确**（`arch/x86/include/asm/posted_intr.h:12`）：
```c
struct {
    u16 notifications;  /* ON(bit 0) + SN(bit 1) + 保留 */
    u8  nv;
    u8  rsvd_2;
    u32 ndst;
};
```

**原因**：ON 和 SN 不是独立的 bitfield，而是 `notifications` (u16) 的最低两个 bit。
这样设计是为了支持 `try_cmpxchg64(&pi_desc->control, ...)` 原子操作。

**修复**：采用 `annotations.md` §1 中的正确定义。

---

## 勘误 2：`vmx_pi_update_irte()` 函数签名错误

**位置**: `posted-interrupts.md` §4.2

**错误**：
```c
void vmx_pi_update_irte(struct kvm_vcpu *vcpu,
                        struct kvm_kernel_irq_routing_entry *e,
                        int dest_id)
```

**正确**（`arch/x86/kvm/vmx/posted_intr.c:272`）：
```c
void vmx_pi_update_irte(struct kvm_vcpu *vcpu,
                        u32 gvec, u32 girq, bool set)
```

**原因**：该函数不直接操作 IRTE 字段，而是构建 `struct vcpu_data` 传给 IOMMU 层，
由 `intel_ir_set_vcpu_affinity()` 完成实际的 IRTE 写入。这是 KVM 和 IOMMU 的分层设计。

**修复**：采用 `annotations.md` §5 中的正确版本。

---

## 勘误 3：`vmx_vcpu_pi_load()` 使用了不存在的字段

**位置**: `posted-interrupts.md` §4.4

**错误**：
```c
new.sn = 0;    /* 清除 SN，允许通知 */
```

**正确**（`arch/x86/kvm/vmx/posted_intr.c:53`）：
```c
__pi_clear_sn(&new);    /* 清除 SN（control 的 bit 1） */
```

**原因**：`struct pi_desc` 没有 `sn` 字段（见勘误 1）。SN 是 `notifications` (u16)
的 bit 1。此外，实际源码使用 `try_cmpxchg64` 而非 `cmpxchg64`。

**修复**：采用 `annotations.md` §6 中的正确版本。

---

## 勘误 4：vCPU 迁移时 IRTE 更新描述不完整

**位置**: `posted-interrupts.md` §1.3

**错误**：
> 无需更新 IRTE：IRTE 的 PDA 不变，指向同一个 PI Descriptor

**正确**：vCPU 迁移时，虽然 IRTE 的 PDA（指向 PI Descriptor）不变，但 IRTE 的
**Dest_ID 必须更新**为新的 pCPU，否则通知中断仍会发往旧 pCPU。

**修复**：明确区分"PDA 不变"与"Dest_ID 必须更新"。

---

## 勘误 5："PI 模式零 VM-Exit" 表述过于绝对

**位置**: `posted-interrupts.md` 常见误解澄清

**错误**：
> PI 模式在所有情况下都可以实现零 VM-Exit

**正确**：vCPU 在 Guest 中运行且 SN=0 时，PI 路径零 VM-Exit。但存在旁路情况：
- vCPU 不在运行（halted/blocking）→ `pi_wakeup_handler()` 处理
- SN=1（通知被抑制）→ 恢复后可能走软件路径
- 嵌套虚拟化 → L1 VMM 需要拦截
- 通知向量不匹配 → `handle_external_interrupt_irqoff()` 处理

参见 AGENTS.md 核心原则 #6："正常 PI 路径 0 次 VM-Exit"（关键词：正常）。

**修复**：区分正常路径和旁路情况。

---

## 勘误 6：IRTE 字段命名违规

**位置**: `README.md` 第 775、868 行

**错误**：
```c
irte.NV = POSTED_INTR_VECTOR;   /* 通知向量 */
```

**正确**：
```c
irte.VV = POSTED_INTR_VECTOR;   /* 虚拟向量 (Virtual Vector) */
```

**原因**：根据 VT-d 规范和 AGENTS.md 已知陷阱 #2：
- IRTE 中是 `VV` (Virtual Vector) @ bits 23:16
- PI Descriptor 中才是 `NV` (Notification Vector) @ bits 279:272

**修复**：将 `irte.NV` 改为 `irte.VV`。

---

## 总结

本次审查发现了 6 个严重事实错误，主要问题集中在：
1. **数据结构定义错误**（勘误 1、3）：对 `struct pi_desc` 的理解不准确
2. **函数签名错误**（勘误 2）：未理解 KVM-IOMMU 分层设计
3. **表述不完整**（勘误 4、5）：遗漏了关键的旁路情况
4. **命名规范违规**（勘误 6）：未遵循 VT-d 规范术语

所有错误均已修复，并统一以 `annotations.md` 为准（因为它的源码引用最准确）。
