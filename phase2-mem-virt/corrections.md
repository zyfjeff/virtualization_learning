# Phase 2 勘误记录

> **记录范围（AGENTS.md 瘦身策略）**：只记概念性错误 / 独家复盘 / 待办三类。
> 机械性修复只走 git。修正错误必须同步修正原文。

## 勘误 1：根 README 目录树悬空的 archive/ 引用

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置**: 根 `README.md` 项目结构树 phase2 块
- **错误**: 列出 `└── archive/ ← 已归档的过程性文档`，但 `phase2-mem-virt/archive/`
  目录不存在（`ls` 验证）。
- **正确**: 删除该行。2026-11-25 起不再设 archive/ 目录（git 历史即归档，
  COURSE_FRAMEWORK.md §5）。
- **已同步修正**: 根 `README.md`

## 勘误 2：性能数字无出处（2026-11-25 登记，待审计）

旧 README「⚡ 性能优化技术」与 VMM 对比含无本仓实测出处数字：
大页"10-20%"、EPT A/D"热迁移 20-30%"、脏页"首次迭代下降 50%"、缺页"省 ~2μs"。
按 AGENTS.md 性能单一来源规则属机制推算（至多 C 级）。
（phase10 corrections F 曾登记这些推算值在 archive/ 多处复制；archive/ 已删，git 历史保留。）

**已做**：README 性能节改纯指针（→ phase10 E2 bench-huge-dirty），数字标待审计；
机制（大页减层级/A/D 免软件干预/MMIO 两级缓存/脏页批量）保留。
**待做**：按 measurement.md 实测并汇入 phase10 分级。

**状态**: ⚠️ 待审计

> **2026-11-26 补充**：同类的无出处数字不止旧 README 那四个——
> `ept-violation-handling.md`（"性能提升 10-100 倍"、"10-20%"、"硬件设置 Dirty 位 ~1 ns"、
> "EPT Violation ~200 ns"、"每次 EPT Violation 约 2-5 μs"）与
> `tdp-mmu-concurrency.md`（"无锁 ~200 ns / 有锁 2-5 μs / 提升 10-25 倍"，
> 以及第 9 节同一组数字）也全是机制推算。已在 `README.md` 性能节加统一声明，
> 正文数字待按 measurement.md 实测或改为指针。**在这些数字被实测替换掉之前，
> 不要把本章的 ns/μs 当作可引用结论。**

---

## 勘误 3：EPTP 的位段被写成"级数在 bits 2:0、内存类型在 bits 51:48"

- **发现日期**: 2026-11-26（phase2+3 抽查）
- **位置**: `README.md`（机制全景一节，原第 113 行）、`annotations.md`（EPTP 格式一节，原第 279 行）
- **错误**: `README.md` 原文 "EPTP（VMCS）指向 EPT 根：bits 2:0 级数（0=4 级）、
  bit 6 A/D 启用、bits 51:48 内存类型。"；`annotations.md` 原文把 PWL 标成 "bits 7:3"。
- **正确**（SDM Vol 3C §25.6.11 Table 25-9）：

  | 位 | 含义 |
  |---|---|
  | 2:0 | EPT paging-structure **内存类型**（0=UC、6=WB，其余保留） |
  | 5:3 | **页表级数减 1**（4 级 = 3、5 级 = 4；0/1/2 保留 —— "0=4 级"也是错的） |
  | 6 | 启用 EPT A/D |
  | 7 | supervisor shadow-stack 权限强制 |
  | 11:8 | 保留 |
  | N-1:12 | EPT 根表物理地址（N = 物理地址宽度） |
  | 63:N | 保留 |

  内核同构：`VMX_EPTP_PWL_MASK 0x38ull` / `PWL_4 0x18ull` / `PWL_5 0x20ull` /
  `VMX_EPTP_AD_ENABLE_BIT (1ull << 6)` / `VMX_EPTP_MT_MASK 0x7ull`
  （`arch/x86/include/asm/vmx.h:526-531`，`:530` 的注释直接写
  "The EPTP memtype is encoded in bits 2:0, i.e. doesn't need to be shifted"）；
  `VMX_EPTP_MT_WB = X86_MEMTYPE_WB = 6`（`arch/x86/include/asm/msr-index.h:50`）；
  装配见 `construct_eptp()`（`arch/x86/kvm/vmx/vmx.c:3411-3423`，`eptp |= root_hpa`）。
- **成因**: 把 EPT **叶条目**的布局（MT 在 bits 5:3、地址在 51:12）套到了 EPTP 上。
  EPTP 与 EPT PTE 的内存类型位宽位置**正好不同**：PTE 的 MT 在 5:3，EPTP 的 MT 在 2:0。
- **已同步修正**: `README.md`（改成整张位段表 + 宏引用）、`annotations.md`（重画 + 补宏）

---

## 勘误 4：SPTE 的 A/D 模式软件位被写成 "bit 9-8"

- **发现日期**: 2026-11-26
- **位置**: `ept-violation-handling.md` 8.3 节（原第 799 行）
- **错误**: 原文把 `00/01/10 = AD_ENABLED/AD_DISABLED/AD_WRPROT_ONLY` 标成
  "SPTE bit 9-8（软件位，用于标记 A/D 模式）"，而**同一块里下一段**又写
  "硬件 A/D 位：bit 8 Accessed、bit 9 Dirty" —— 同一对位号给了两种含义，自相矛盾。
- **正确**: 模式位是 **bits 53:52**：`SPTE_TDP_AD_SHIFT = 52`、
  `SPTE_TDP_AD_MASK = (3ULL << 52)`、`ENABLED/DISABLED/WRPROT_ONLY = 0/1/2 << 52`
  （`arch/x86/kvm/mmu/spte.h:32-37`）。bits 8/9 才是硬件 A/D
  （`VMX_EPT_ACCESS_BIT (1ull << 8)`、`VMX_EPT_DIRTY_BIT (1ull << 9)`，
  `arch/x86/include/asm/vmx.h:538-539`）。
  `annotations.md:317/:1015` 一直写的是 52:53，是对的。
- **顺带修正的记法**: 本章原有多处写成 "52:53"、"54:55"（低位在前）。规范惯例是
  `高:低`，已统一为 **53:52**、**55:54**（`SHADOW_ACC_TRACK_SAVED_BITS_SHIFT = 54` +
  R/X 两位 → bits 55:54，`spte.h:69-73`）。
- **已同步修正**: `ept-violation-handling.md` 8.3、`annotations.md`（软件位表、
  EPT 布局图、make_spte 注释共 6 处）

---

## 勘误 5：`make_spte()` 与 `struct kvm_page_fault` 的"源码"其实是伪代码

- **发现日期**: 2026-11-26
- **位置**: `ept-violation-handling.md` 2.9 节、3.1 节、5.3 节
- **错误**:
  1. 2.9 把 `tdp_mmu_map_handle_target_level()` 写成 `new_spte = make_spte(vcpu, sp,
     fault->slot, fault->gfn, fault->pfn, fault->level, fault->spte_ptr, ...)`；
  2. 3.1 的 `struct kvm_page_fault` 列表里写 `const u32 error_code`、`const u8 access`、
     `bool prefault`、`int max_level/req_level/goal_level`、`hpa_t hva`，并标
     `mmu_internal.h:191`；
  3. 5.3 把 `if (fault->prefault) new_spte |= PT_ACCESSED_MASK;` 挂在 `tdp_mmu.c:1017`。
- **正确**:
  - `make_spte()` **返回 `bool wrprot`**，SPTE 经最后一个出参 `u64 *new_spte` 带回
    （`arch/x86/kvm/mmu/spte.c:157-161`、声明 `spte.h:499`）。真实调用是
    `wrprot = make_spte(vcpu, sp, fault->slot, ACC_ALL, iter->gfn, fault->pfn,
    iter->old_spte, fault->prefetch, true, fault->map_writable, &new_spte);`
    （`tdp_mmu.c:1032-1034`）。`fault->level`、`fault->spte_ptr` **都不是该结构体的字段**；
    `tdp_mmu_set_spte_atomic()` 的首参是 `vcpu->kvm`（`tdp_mmu.c:1038`）。
    旧稿还漏了 `if (unlikely(!fault->slot)) new_spte = make_mmio_spte(...)` 这条 MMIO 分支
    （`:1029-1030`）——它恰恰是本章第 7 节 MMIO 机制的入口。
  - `struct kvm_page_fault`（`arch/x86/kvm/mmu/mmu_internal.h:190-250`，**定义在 190 行**）
    真实字段类型是 `const u64 error_code`、`const bool prefetch`、
    `u8 max_level/req_level/goal_level`、`hva_t hva`；**没有 `access` 字段**
    （访问类型派生成 `exec/write/present/rsvd/user` 五个 bool，构造点 `:293-315`）。
  - 预取的 A 位语义是**反的**：`if (!prefetch) spte |= spte_shadow_accessed_mask(spte);`
    （`spte.c:181-183`）——预取的 SPTE **A=0**，硬件第一次真正访问才置 1，
    KVM 靠这个识别"预取了但没用上"。不存在 `fault->prefault` 与那段 `PT_ACCESSED_MASK` 代码。
- **已同步修正**: 三节均改为逐字摘自源码 + 勘误说明；`annotations.md` §6 的 `make_spte()`
  也补回了被省掉的 `out:` 之后两步（access-track 改造、`mark_page_dirty_in_slot()`）。

---

## 勘误 6：IPAT 的语义被写成"忽略 Guest 的 PAT/MTRR"

- **发现日期**: 2026-11-26
- **位置**: `mmio-identification.md` §5.1/§5.2/§5.5/§7.2.2、`ept-violation-handling.md` §9.1/§9.4/§9.7
- **错误**: 原文两处写 "IPAT = 0 考虑 Guest 的 PAT/**MTRR** 设置 / IPAT = 1 忽略 Guest 的
  PAT/**MTRR**"，并把 "Guest 设 `mtrr_add(...)` 不生效" 归因于 IPAT。
- **正确**（SDM Vol 3C §29.3.7.2 原文）：
  - bit6 = 1 ⇒ "the memory type used for the access is the EPT memory type.
    **The PAT memory type is ignored.**"
  - bit6 = 0 ⇒ EPT MT 与 PAT MT 按 Table 12-7 组合，"**using the EPT memory type
    in place of the MTRR memory type**" —— EPT MT 是**顶替** MTRR 的位置。
  - 同节最后一条：**"The MTRRs have no effect on the memory type used for an access to a
    guest-physical address."**
  所以 guest MTRR 在 EPT 下**恒不参与**，与 IPAT 取值无关；能影响 guest 侧类型的只有
  **PAT**（页表 PCD/PAT + `MSR_IA32_PAT`），外加 `CR0.CD`（=1 ⇒ UC）与 `CR0.PG`（=0 ⇒ PAT MT 固定 WB）。
  §29.3.7.1 还补了一条：MTRR 对"取 EPT 分页结构本身"的访问也无影响（那次用 EPTP bits 2:0）。
- **已同步修正**: 上述各节改为"只关 PAT"，并各自加规范原文引用。KVM 侧结论不变
  （`vmx_get_mt_mask()` 三分支 `vmx.c:7679-7692`、MMIO 强制 UC `:7685-7686`、
  IPAT 仅在 `vmx_ignore_guest_pat()` 为真时置 `:7689-7690`）。

---

## 勘误 7：`noncoherent_dma_count` 被当成 debugfs 文件

- **发现日期**: 2026-11-26
- **位置**: `mmio-identification.md` §5.2/§5.6、`ept-violation-handling.md` §9.4/§9.8
- **错误**: 原文教人 `cat /sys/kernel/debug/kvm/*/noncoherent_dma_count` 判断 VM 是否
  有非一致 DMA 设备（进而判断 IPAT）。
- **实际**: 该文件**不存在**。`noncoherent_dma_count` 只是 `struct kvm_arch` 里的
  `atomic_t`（`arch/x86/include/asm/kvm_host.h:1341`），没有任何 debugfs/sysfs 导出。
  实测宿主的 `/sys/kernel/debug/kvm/<pid>-<n>/` 下是
  `exits / pf_taken / pf_fixed / pf_fast / pf_emulate / pf_mmio_spte_created /
  pages_4k / pages_2m / pages_1g / mmu_* / remote_tlb_flush …`
  这些统计项（来自 `arch/x86/kvm/x86.c:232-247` 的 `kvm_vm_stats_desc[]` 等表）。
- **可落地的替代观测**（已写进原文）：
  1. quirk 位：`KVM_X86_QUIRK_IGNORE_GUEST_PAT = (1 << 9)`
     （`arch/x86/include/uapi/asm/kvm.h:444`）；
     `KVM_CHECK_EXTENSION(KVM_CAP_DISABLE_QUIRKS2)` 读支持集（`x86.c:4800-4802`），
     `KVM_ENABLE_CAP(KVM_CAP_DISABLE_QUIRKS2, bit)` 关它（`x86.c:6549-6556`，
     落到 `kvm->arch.disabled_quirks`，判定 `kvm_check_has_quirk()` `x86.h:358-361`）。
  2. 副作用：第一个/最后一个非一致设备注册会 **zap 全部 SPTE**
     （`kvm_noncoherent_dma_assignment_start_or_stop()` `x86.c:13627-13639`），
     表现为 `mmu_shadow_zapped`/`pages_4k` 突变 + 一轮 EPT violation。
  3. 宿主 self-snoop：`grep -o '\bss\b' /proc/cpuinfo`
     （`X86_FEATURE_SELFSNOOP`，`arch/x86/include/asm/cpufeatures.h:54`；
     缺失 ⇒ quirk 被整条摘掉，`vmx.c:8606-8607`，IPAT 恒 0）。
  4. `ls /sys/bus/pci/drivers/vfio-pci/`（真实存在）只能说明"挂了 VFIO"，
     **不等于**非一致——判据是 `IOMMU_CAP_ENFORCE_CACHE_COHERENCY`。
- **另一处顺带纠正**：旧稿把 IPAT 的开关条件写成"无非一致 DMA 设备"一条，
  **漏了 quirk 这一半**（真实是两者相与，`vmx.c:7668-7677`）。

---

## 勘误 8：A/D 三档的"使用场景"张冠李戴

- **发现日期**: 2026-11-26
- **位置**: `annotations.md` AD 三模式一节（原第 375-395 行）
- **错误**: 把 `AD_WRPROT_ONLY` 的场景写成"内存快照 / COW 优化 / 实时迁移优化"，
  又把"L2 Guest 使用 PML"列在 `AD_DISABLED` 名下。
- **正确**:
  - `AD_WRPROT_ONLY` 的**唯一**判据是
    `kvm_x86_ops.cpu_dirty_log_size && sp->role.guest_mode`
    （`arch/x86/kvm/mmu/mmu_internal.h:148-159`），即 **CPU 有 PML**
    （`cpu_dirty_log_size = PML_ENTITY_NUM = 512`，`vmx/main.c:130`、`vmx.h:336`）
    **且这张影子页属于 L2**。源码注释给的理由很具体：PML 记下的 GPA 会是 **L2** 的而不是
    L1 的，所以改成写保护让每次写都 VM-Exit，"which bypasses PML, since writes now
    result in a vmexit"。它与快照/COW/迁移这些**非嵌套**场景无关——那些走
    AD_ENABLED + PML，或 AD_DISABLED + 软件写保护。
  - `AD_DISABLED` 来自 `role.ad_disabled = !kvm_ad_enabled()`（`mmu.c:5494`；
    `kvm_ad_enabled()` = `!!shadow_accessed_mask`，`spte.h:294-297`），根因是
    CPU 无 EPT A/D 能力、或 `kvm_intel.eptad=0`（`vmx.c:105-106`，权限 0444）、
    或不用 EPT（`vmx.c:8446-8447`）。**PML 反而是它的反面**：
    `!enable_ept || !enable_ept_ad_bits || !cpu_has_vmx_pml()` ⇒ `enable_pml = 0`
    （`vmx.c:8525-8529`），PML 依赖 A/D 可用。
  - `SPTE_TDP_AD_ENABLED` 必须为 0 的**硬理由**在 `spte.h:26-27`：PAE 分页把
    bits 62:52 整体保留，"enabled"若占非 0 值就会踩保留位（不只是"省一条 OR"的性能优化）。
- **已同步修正**: `annotations.md` 三模式表 + 使用场景小节 + `mmu_internal.h:148-159`
  与 `tdp_mmu.c:1497-1505`（`tdp_mmu_need_write_protect()`）逐字引用。

---

## 勘误 9（待办）：本章实践脚本仍引用旧的错误结论

`practice/` 里的取证练习若按"bit 9-8 是 A/D 模式位"、"`cat …/noncoherent_dma_count`"
去设计判据，会得到假阴性。改文档时未同步动 `practice/`——**待办**：
下一轮实践复核时按本文件勘误 4/7 校正判据。
