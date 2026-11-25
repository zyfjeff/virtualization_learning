# Phase 0 勘误记录

> **记录范围（AGENTS.md 瘦身策略）**：只记概念性错误 / 独家复盘 / 待办三类。
> 机械性修复只走 git。修正错误必须同步修正原文。

## 勘误 1：README 交叉引用沿用旧 phase 编号

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置**: `README.md:731`
- **错误**: "Posted Interrupts（phase4 详解）"
- **正确**: phase5。phase4-pcie 插入后中断虚拟化由 phase4 顺延为 phase5
  （现行目录 `phase5-interrupts/`），此处未同步。
- **已同步修正**: `README.md`

## 勘误 2：大页内存收益数字无出处（2026-11-25 登记，待审计）

旧 README「⚡ 性能优化技术」大页节"性能提升 10-20%"无本仓实测出处。
（halt-polling 节已正确指向 phase10 index §1.2，符合单一来源，保留。）

**已做**：README 性能节大页数字标待审计；halt-polling 维持指针。
**待做**：大页收益若要引用，按 measurement.md 实测并汇入 phase10 分级（E2 bench-huge-dirty）。

**状态**: ⚠️ 待审计

## 勘误 3：把 CPUID / MSR 归入"返回用户空间的慢速路径"（概念性错误）

- **发现日期**: 2026-11-25（phase0+1 轻量抽查，事实核查员复核）
- **位置**: `annotations.md` §10「VM-Exit 处理分类」；`kvm-framework.md` §2.1 数据流图；
  `README.md`「设计空间与取舍」；`kvm-framework.md`「混合模式」清单
- **错误**: 旧版把 `CPUID` 与 `MSR_READ/WRITE` 列在"慢速路径（返回用户空间，QEMU 处理）"，
  并给它们配了 `KVM_EXIT_INTERNAL_ERROR`；`kvm-framework.md` 还有"慢速路径VM-Exit处理
  (MMIO/CPUID/IO)"。
- **正确**: 两者都**在内核闭环**，且不产生 `KVM_EXIT_INTERNAL_ERROR`。
  - `arch/x86/kvm/vmx/vmx.c:6103-6107`：
    `[EXIT_REASON_CPUID] = kvm_emulate_cpuid`、
    `[EXIT_REASON_MSR_READ] = kvm_emulate_rdmsr`、`[EXIT_REASON_MSR_WRITE] = kvm_emulate_wrmsr`、
    `[EXIT_REASON_HLT] = kvm_emulate_halt`。
  - `kvm_emulate_cpuid()`（`arch/x86/kvm/cpuid.c:1681-1697`）查 `KVM_SET_CPUID2` 灌进来的
    `vcpu->arch.cpuid_entries`，末尾 `return kvm_skip_emulated_instruction(vcpu)` → 恒 1（重入），
    QEMU 从不"处理 CPUID"。
  - MSR 未命中时只有开了 `KVM_CAP_X86_USER_SPACE_MSR` 才由 `kvm_msr_user_space()`
    （`arch/x86/kvm/x86.c:2032`，调用点 `:2069`/`:2091`）填
    **`KVM_EXIT_X86_RDMSR`/`KVM_EXIT_X86_WRMSR`**；否则经
    `kvm_x86_call(complete_emulated_msr)()`（VMX 绑 `kvm_complete_insn_gp`，
    `arch/x86/kvm/vmx/main.c:157`）注入 #GP。
  - `KVM_EXIT_INTERNAL_ERROR` 只属于"模拟失败/无法继续"：`x86.c:8812`
    （`prepare_emulation_failure_exit()` `:8796`）、`x86.c:11969`（`kvm_task_switch()`）、
    `x86.c:13833`（`kvm_handle_memory_failure()` `:13817`）。
  - 也没有叫 `MMIO` 的 VMX exit reason —— `KVM_EXIT_MMIO` 是 EPT violation 打到未映射 GPA
    时产生的 *exit_reason*。
- **根因**: 把"设备相关 = 要回用户态"错误推广到了 CPUID/MSR；实际边界是**返回值约定**
  （handler 返回 >0 内核重入 / 0 回用户态 / <0 错误），不是 exit reason 类别。
  同一错误与 `phase1-cpu-virt` 的 CPUID 章节自相矛盾，现已统一。
- **已同步修正**: `README.md`、`annotations.md`、`kvm-framework.md`

## 勘误 4：x86 上不存在"异步 vCPU ioctl"，`KVM_SET_SIGNAL_MASK` 不是例外（概念性错误）

- **发现日期**: 2026-11-25
- **位置**: `annotations.md` §3
- **错误**: "异步 ioctl 是什么？不需要持有 vcpu->mutex 的操作，典型的是 `KVM_SET_SIGNAL_MASK`，
  这些操作可以在 vCPU 运行时并发执行。"
- **正确**: `CONFIG_HAVE_KVM_VCPU_ASYNC_IOCTL` 未定义时，
  `kvm_arch_vcpu_async_ioctl()` 是恒返回 `-ENOIOCTLCMD` 的 inline stub
  （`include/linux/kvm_host.h:2377-2387`），因此 §3 代码里
  `if (r != -ENOIOCTLCMD) return r;` 在 x86 上**永远不会提前返回**，
  所有 vCPU ioctl（含 `KVM_RUN`、`KVM_SET_SIGNAL_MASK`）都要过
  `mutex_lock_killable(&vcpu->mutex)`；`KVM_SET_SIGNAL_MASK` 就在同一个 switch 里
  （`virt/kvm/kvm_main.c:4597`）。
  选了这个 Kconfig 的只有 s390 / riscv / mips / loongarch / powerpc
  （如 `arch/s390/kvm/Kconfig:23`），s390 的异步 ioctl 处理 `KVM_S390_IRQ` /
  `KVM_S390_INTERRUPT`（`arch/s390/kvm/kvm-s390.c:5445`）——那才是"不拿锁注入事件"的真实例子。
  x86 的对应需求走 `KVM_INTERRUPT`（`arch/x86/kvm/x86.c:5930`，仍在 mutex 内）与
  VM 级 `KVM_SIGNAL_MSI`（`kvm_main.c:5276`）。
- **已同步修正**: `annotations.md`

## 勘误 5：halt-polling 描述与参数/命名偏差，以及"模式切换减少 50%"（概念性 + 待办）

- **发现日期**: 2026-11-25
- **位置**: `annotations.md` §7；`kvm-framework.md` §2.1、§5.1
- **概念性偏差**:
  1. §7 的 Phase-3 伪代码只给三条分支，**漏了真实第一条**
     `if (!vcpu_valid_wakeup(vcpu)) shrink_halt_poll_ns(vcpu);`（`kvm_main.c:3862-3863`）
     与末尾 `else vcpu->halt_poll_ns = 0;`，并省掉 shrink 的 `vcpu->halt_poll_ns &&`、
     grow 的 `< max_halt_poll_ns` 条件（`kvm_main.c:3858-3878`）。
     **但补这条时也别走偏**：`CONFIG_HAVE_KVM_INVALID_WAKEUPS` 只有 s390 选
     （`arch/s390/kvm/Kconfig:29`），`vcpu->valid_wakeup` 的唯一写者也在 s390
     （`arch/s390/kvm/interrupt.c:1335/1345`）⇒ **x86 上
     `vcpu_valid_wakeup()` 恒 true（`include/linux/kvm_host.h:2361-2364`），
     "伪唤醒砍窗口"在 x86 是死支**。已按此写明，避免被当成 x86 调参依据。
  2. "`max_halt_poll_ns` 是模块参数" 的暗示不成立：它是 `kvm_vcpu_halt()` 里的局部变量，
     取自 `kvm_vcpu_max_halt_poll_ns()`（`kvm_main.c:3787-3802`）——未启用
     `KVM_CAP_HALT_POLL`（写入点 `kvm_main.c:5056`，字段 `kvm_host.h:858`）时就是全局
     `halt_poll_ns`。`/sys/module/kvm/parameters/` 里也确实没有 `max_halt_poll_ns`。
  3. `kvm-framework.md` §5.1 的 `handle_hlt()` → 实际 `kvm_emulate_halt()`
     （`vmx.c:6107` → `x86.c:11421` → `__kvm_emulate_halt()` `x86.c:11393-11413`）；
     "kvm_vcpu_block() → wait_event_interruptible()" → 实际是 **rcuwait + 手写
     `schedule()` 循环**（`kvm_main.c:3733-3758`，判据在 `kvm_vcpu_check_block()`
     `:3708-3726`）；两处都与本章 annotations §9 的 rcuwait 说明冲突，已统一。
- **参数默认值（机械修正，唯一来源仍是 phase10）**:
  `kvm-framework.md` 的"默认 400μs" → **200μs**。
  源码 `arch/x86/include/asm/kvm_host.h:71` `#define KVM_HALT_POLL_NS_DEFAULT 200000`
  （`kvm_main.c:78-79` 使用），宿主实测 `cat /sys/module/kvm/parameters/halt_poll_ns`
  = `200000`。`phase10-performance/parameters.md:82-83` 曾声明"全仓 10 处已修正"，
  本处是漏网；**同时发现 `phase10-performance/README.md:60` 与
  `phase11-debugging/performance-analysis.md:398` 仍写着 400000，属那两个 phase 的遗留，
  不在本次改动范围，需后续处理**。
- **无出处数字（待办）**: `kvm-framework.md` §2.1 的"总开销: 4次模式切换!"与
  "总开销: 2次模式切换! (减少50%)"。切换**次数**是机制描述可以保留，
  但"减少 50%"是把次数比当成开销比，属 `phase1-cpu-virt/corrections.md` 勘误 1
  同类的无出处推算（AGENTS.md 性能单一来源规则）。已改写为
  "省的是 user↔kernel 往返次数，倍数待实测 → phase10"。
- **已同步修正**: `annotations.md`、`kvm-framework.md`
