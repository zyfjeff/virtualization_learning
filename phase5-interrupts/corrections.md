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

## 勘误 2：`vmx_pi_update_irte()` 函数签名错误（2026-11-25 二次更正）

**位置**: `posted-interrupts.md` §4.2

**错误**：
```c
void vmx_pi_update_irte(struct kvm_vcpu *vcpu,
                        struct kvm_kernel_irq_routing_entry *e,
                        int dest_id)
```

**正确**（`arch/x86/kvm/vmx/posted_intr.c:272`）：
```c
int vmx_pi_update_irte(struct kvm *kvm, unsigned int host_irq,
		       uint32_t guest_irq, bool set)
```

> ⚠️ **本条此前给出的"正确"签名 `void vmx_pi_update_irte(struct kvm_vcpu *vcpu,
> u32 gvec, u32 girq, bool set)` 本身是错的**（返回类型、形参全部不符），并按它
> "修正"了 annotations.md §5 与 posted-interrupts.md §4.2。2026-11-25 已按真实源码
> 重写两处代码块（同时删除虚构的 `kvm_irq_to_host_irq()`——内核无此符号）。

**原因**：该函数不直接操作 IRTE 字段，而是构建 `struct vcpu_data` 传给 IOMMU 层，
由 `intel_ir_set_vcpu_affinity()` 完成实际的 IRTE 写入。这是 KVM 和 IOMMU 的分层设计。
注意 `vcpu_info.vector = irq.vector` 是 **guest 向量**（见勘误 6 的更正）。

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
（2026-11-25 补充：当时"采用"的 annotations §6 版本自身还带着另外两个错误——
`per_cpu(x86_cpu_to_physical_apicid, cpu)`（内核无此符号，真实为
`cpu_physical_id()` + `!x2apic_mode` 时的 `(dest<<8)&0xFF00`，posted_intr.c:97-99）
与 `after_clear_sn` 处的 `kvm_make_request(KVM_REQ_EVENT)`（真实为
`smp_mb__after_atomic(); if (!pi_is_pir_empty) pi_set_on();`，:129-132）。
两处已按源码重写，见勘误 12。）

---

## 勘误 4：~~vCPU 迁移时 IRTE Dest_ID 必须更新~~（2026-11-25 撤销，本条勘误本身是错的）

**原"错误"**：
> 无需更新 IRTE：IRTE 的 PDA 不变，指向同一个 PI Descriptor

——这**本来就是对的方向**，只是表述不完整。本条此前"修正"为"IRTE 的 Dest_ID 必须更新"，
**该结论错误**，已从 README/posted-interrupts/annotations/vcpu-migration/msi-affinity/
amd-avic 全面撤销并改写。

**正确**（VT-d Spec 9.10, Figure 9-10 + 内核源码）：
- Posted 格式 IRTE 的字段只有 P/FPD/AVAIL/URG/**IM**/VV/PDA/SID/SQ/SVT，
  **不存在 destination/Dest_ID 字段**（Dest_ID 只在 Remapped 格式 bits 63:32）。
- 通知目标 = PI Descriptor 的 **NDST**（bits 319:288，属于 PID 不属于 IRTE）。
- vCPU 迁移：`vmx_vcpu_pi_load()`（posted_intr.c:53）用 `try_cmpxchg64` 更新 NDST；
  **不写 IRTE、不刷 IEC**。
- 反证（内核明确允许不一致存在）：`__intel_ir_reconfigure_irte()` 对
  `posted_vcpu` 的 IRQ 在宿主亲和性变更时**直接 return，不修改 IRTE**
  （irq_remapping.c:1176-1177）——若 IRTE 需要跟随目标 pCPU，这里不可能跳过。
- 已蒸馏为 AGENTS.md 全局陷阱 **#18**。

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

## 勘误 6：IRTE 字段命名违规（2026-11-25 二次更正：字段名改对了，赋值仍是错的）

**位置**: 旧 `README.md` 第 775、868 行

**错误**：
```c
irte.NV = POSTED_INTR_VECTOR;   /* 通知向量 */
```

**本条此前的"正确"**：
```c
irte.VV = POSTED_INTR_VECTOR;   /* 虚拟向量 (Virtual Vector) */
```

> ⚠️ 改名 `NV→VV` 符合规范，但**赋值仍是错的**。VT-d Spec 9.10 (Figure 9-10)：
> "VV: Virtual Vector — This 8-bit field contains **the vector associated with the
> interrupt requests posted through this IRTE**"——即记入 PIR 的 **guest 向量**。
> 通知向量 NV 在 PI Descriptor（bits 279:272，"the physical vector used for the
> notification event"），KVM 中为 POSTED_INTR_VECTOR(0xf2)/WAKEUP(0xf1)。
> **两者值不同。** 源码印证：`vmx_pi_update_irte()` 传 `vcpu_info.vector =
> irq.vector`（guest 向量，posted_intr.c:317），`intel_ir_set_vcpu_affinity()` 写入
> `irte_pi.p_vector`（irq_remapping.c:1273）。
> AGENTS.md 陷阱 #2 的"值相同"表述已于 2026-11-25 同步更正。

**修复**：已全面改写 phase5 六处"VV=通知向量/irte.p_vector=POSTED_INTR_VECTOR"
的表述（annotations §2/§5、posted-interrupts §2.2/§2.4/§3.1/§4.2/总结、
vcpu-migration §1.3、msi-affinity 模式3、amd-avic 对比表）。

---

## 勘误 7：annotations.md 标题沿用旧 phase 编号

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置**: `annotations.md:1`
- **错误**: 标题 "Phase 4：源码精读注释 - 中断虚拟化 + VT-d 中断重映射"
- **正确**: Phase 5。phase4-pcie 插入后中断虚拟化由 phase4 顺延为 phase5。
- **已同步修正**: `annotations.md`

---

## 勘误 8：README 陷阱节"检查 DM 位"应为 IM（2026-11-25 已修）

旧 README 陷阱 2 的检查注释写"检查 DM 位是否为 1"，但同节代码用 `irte.IM = 1`。
区分 Remapped/Posted 的是 **IM（IRTE Mode, bit15）**，不是 DM（AGENTS.md 全局陷阱 #1）。
内核里该位叫 `pst`/`p_pst`（`include/linux/dmar.h:212/240`）。

**已同步修正**：README 重写后陷阱节统一用 IM/VV/PDA 规范术语，并列出 #1–9 全局陷阱入口。

## 勘误 9：性能优化节无出处数字（2026-11-25 登记，待审计）

旧 README「⚡ 性能优化技术」含"减少中断相关 VM-Exit 80%""中断延迟降低 50%"
"高吞吐性能提升 3-5 倍""吞吐提升 20-30%"等无本仓实测出处数字。
按 AGENTS.md 性能单一来源规则属机制推算（至多 C 级）。
（"正常 Posted 路径 0 VM-Exit、延迟<1μs" 有 SDM 30.6/VT-d 5.2.5 支撑，属机制结论，保留。）

**已做**：README 性能节改纯指针（→ phase10），数字标待实测；ex4-pi-vs-remapped.sh 可实测但需分级。
**待做**：按 measurement.md 实测 Posted vs Remapped 并汇入 phase10 index 分级。

**状态**: ⚠️ 待审计

---

## 勘误 10：vcpu-migration-affinity.md §1.4 "intel_ir_set_affinity" 代码块虚构

**位置**: `vcpu-migration-affinity.md` §1.4（另 §1.3 触发场景表）

**错误**：标注"来源: irq_remapping.c:1156 (简化)"，函数体写
`irte->destination = cpumask_first(mask); modify_irte(...)`。

**核实**：:1156 位于 `intel_ir_reconfigure_irte_posted()`（宿主 posted-MSI 路径），
`intel_ir_set_affinity()` 实际在 :1217；`struct irte` 没有 `destination` 字段
（remapped 视图叫 `dest_id`）。真实行为：`intel_ir_reconfigure_irte()` 只更新
**缓存的 remapped 视图**（:1197-1198），随后 `__intel_ir_reconfigure_irte()` 对
`posted_vcpu && !force_host` **直接 return**（:1176-1177）——宿主亲和性变更
不会改写 guest posted 中断的 IRTE。§1.3 表中 "vCPU 迁移 → kvm_arch_vcpu_load →
kvm_arch_irq_bypass_add_consumer → IRTE: Dest_ID" 的调用链同样不存在
（bypass add_producer 只在 irqfd 挂接时被调，与迁移无关）。

**修复**：§1.3/§1.4 已按真实源码重写；触发场景表已更正。

---

## 勘误 11：annotations.md §5/§10 modify_irte "四步协议"不存在

**位置**: `annotations.md` §5（"1.清 Present → 2.写 → 3.qi_flush_iec → 4.设 Present"）

**核实**：`modify_irte()`（irq_remapping.c:156-192）真实协议：持 `irq_2_ir_lock`，
`index = irte_index + sub_handle`；旧/新任一 `pst==1` 时
`try_cmpxchg128()` 原子替换整个 128 位（:178-182），否则两次 `WRITE_ONCE`
（:183-186）；然后 `__iommu_flush_cache()`（:187）+ 一次 `qi_flush_iec()`（:189）。
没有"先清 Present 再设 Present"。

**修复**：§5 与 §10 已按真实代码改写。

---

## 勘误 12：annotations.md §6 / posted-interrupts.md §4.4 pi_load 代码块失实

见勘误 3 的 2026-11-25 补充。两处代码块已替换为 `posted_intr.c:53-133` 的真实
节选（含 `cpu_physical_id`+xAPIC 移位、快速路径漏掉的 `return;`、
`after_clear_sn` 的 `pi_set_on`）。vcpu-migration-affinity.md §1.1 原本就是对的，
两处现已一致。

---

## 勘误 13：annotations.md §4 屏障顺序讲反

**错误**："执行顺序必须是 ①读 PIR → ②清 ON → ③屏障"——与同节代码块
（`pi_clear_on → smp_mb__after_atomic → kvm_apic_update_irr`）矛盾，且按此顺序
会丢中断（IOMMU 在"读完未清"窗口置位并见 ON=1 而不发通知）。

**正确**：①清 ON → ②屏障 → ③读 PIR；内核注释（vmx.c:6924-6926）"the barrier
matters even on UP" 说明它是配对所需的真屏障而非防御性摆设。已改写。

---

## 勘误 14：annotations.md §8 APICv 控制位表位号错误

**错误**：VID=二级控制 bit37、EOI Virt=bit38、APIC-register Virt=bit39。

**核实**（arch/x86/include/asm/vmxfeatures.h）：`VMX_FEATURE_VIRT_INTR_DELIVERY`
= 2*32+**9**（secondary bit 9，0x200）；`VMX_FEATURE_APIC_REGISTER_VIRT`
= 2*32+**8**（bit 8，0x100）；**EOI virtualization 没有独立控制位**
（vmxfeatures.h 无对应宏；它随 VID 生效，由 VMCS EOI-exit bitmap 决定哪些向量
不产生 MSR-write exit，SDM 30.1.2）。Posted Interrupts = PIN_BASED bit 7 ✓ 正确。

**修复**：表格已改写并附宏名/行号。

---

## 勘误 15：posted-interrupts.md §3.1 把宿主 posted-MSI 当成 guest PI 初始化路径

**错误**："QEMU 启用 VFIO 设备 → pci_alloc_irq_vectors → intel_irq_remapping_prepare_irte
→ prepare_irte_posted() 设置 IM=1；irte.p_vector = POSTED_INTR_VECTOR"。

**核实**：`prepare_irte_posted()`（irq_remapping.c:1111）仅由
`posted_msi_supported()` 门控调用（:1377，置 `posted_msi=1`），属宿主
CONFIG_X86_POSTED_MSI 路径（AGENTS.md 陷阱 **#8**）；guest 的 IRTE 在分配时是
Remapped（`prepare_irte()` :1091/:1359），QEMU 完成路由/irqfd 后经
`vmx_pi_update_irte → intel_ir_set_vcpu_affinity` 才切 Posted 并填 PDA/VV，
且 VV=guest 向量（勘误 6）。§3.1 已重写为两阶段流程。

---

## 勘误 16：POSTED_INTR_VECTOR/WAKEUP 常量错误（2026-11-25 批量修）

`posted-interrupts.md`/`msi-affinity-migration.md`/`vcpu-migration-affinity.md`/
`practice/ex2-irte-observe.sh`/`ex3-pi-trace.sh` 曾写 0xf7/0xf6；6.12.93 实际为
`POSTED_INTR_VECTOR=0xf2`、`POSTED_INTR_WAKEUP_VECTOR=0xf1`
（arch/x86/include/asm/irq_vectors.h:87-88）。已批量更正并蒸馏为 AGENTS.md 陷阱 **#19**。

---

## 勘误 17：不存在的 trace 事件 / debugfs 文件

- `kvm_pi_desc_init`、`kvm_pi_set_pir`：trace.h 中不存在（PI 相关只有
  `kvm_pi_irte_update`，trace.h:1080）。已从 posted-interrupts.md §5.2、
  vcpu-migration-affinity.md §4.1 移除，示例输出改为真实 `TP_printk` 格式。
- AMD debugfs `ga_log`/`ir_translation_struct`：6.12.93 的
  `drivers/iommu/amd/debugfs.c` 只创建 `iommuNN` 目录，无这两个文件；
  GA 事件观测改用 `kvm:kvm_avic_ga_log`（trace.h:1486）。
- Intel debugfs `ir_translation_struct` 存在（intel/debugfs.c:610/764），但
  Posted 表头为 `Entry SrcID PDA_high PDA_low Vct IRTE_high IRTE_low`
  （:587），**没有 Dest_ID 列**；两处示例已按真实格式改写。

---

## 勘误 18：amd-avic.md 三处位域/时序错误

- §3.1 "基础 IRTE (Table 20)"：旧图 Vector 71:64 / DM=7 / RqEoi=6，且标题写
  "32-bit"却画 128-bit。规范 Table 20 + 内核 `union irte`
  （amd_iommu_types.h:982-994）：Vector **23:16**、bit7=**GuestMode(恒0)**、
  **DM=6、RqEoi=5**；71:64 属 128-bit GA 表内 remap 条目（Table 22）。已重写。
- §4.1 vAPIC backing page：旧图 vIRR 为"0x200 起连续 32 字节"，与同节公式
  `byte offset=(V/32)<<4`（16 字节步距）矛盾；真实为 0x200/0x210/.../0x270
  八个 32-bit 寄存器（跨至 0x273）。已重画。
- §1/§6 把 Intel posted-interrupt processing 说成"VM-Exit 时/VM-Entry 时"处理，
  并在对比表写 "IRTE.DestID"：正常路径**不产生 VM-Exit**（SDM 30.6），Posted
  IRTE 无 destination（陷阱 #18）。已改写。

---

## 总结

第一轮审查发现 6 个事实错误（勘误 1-6），当时"统一以 `annotations.md` 为准"。

**2026-11-25 第二轮事实核查**（依据 Linux 6.12.93 + VT-d Spec 9.10/9.11 +
SDM 30.6 + amd spec Table 20-23）推翻了其中两条"修复"并新增登记（勘误 10-18），
主要问题集中在：

1. **系统性概念错误（第一轮修复反而放大）**
   - "vCPU 迁移要更新 IRTE Dest_ID"（勘误 4 撤销；Posted IRTE 无 destination，
     迁移只改 PI desc NDST）——曾同时存在于 6 个文件
   - "IRTE 的 VV = 通知向量、值与 NV 相同"（勘误 6 更正；VV = guest 向量）
   两者已蒸馏为 AGENTS.md 陷阱 **#18/#19**（#2 同步改写）。
2. **"简化"代码块偏离真实源码**（勘误 2、3、11、12）：虚构签名/不存在的
   per-cpu 符号/不存在的调用链——本轮全部替换为带行号的真实节选，
   符合 AGENTS.md"代码示例必须摘自实际源码"。
3. **时序/机制表述错误**（勘误 13、14、15、18）：屏障顺序讲反、
   APICv 控制位位号错、宿主 posted-MSI 与 guest PI 混淆（陷阱 #8）、
   amd-avic 位域与"通知触发 VM-Exit"式误解（陷阱 #6 反面）。
4. **观测手段失实**（勘误 16、17）：向量常量 0xf7/0xf6 → 0xf2/0xf1、
   不存在的 trace 事件与 debugfs 文件。

教训：**"行号准确的文档"不等于"代码块可信"**——annotations 的 file:line
全部能对上函数头，但代码体是凭记忆重写的。本轮所有代码块均已与
`/root/code/linux-6.12.93` 逐行比对。
