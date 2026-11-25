# Phase 1 勘误记录

> **记录范围（AGENTS.md 瘦身策略）**：只记概念性错误 / 独家复盘 / 待办三类。
> 机械性修复只走 git。修正错误必须同步修正原文。自引用写"本章 corrections.md"。

## 勘误 1：性能优化章节的无出处数字（2026-11-25 登记，待审计）

旧 README「⚡ 性能优化技术」含无出处推算：
- "VPID 减少 VM-Entry 开销 10-15%"
- "VMCS Shadow 嵌套虚拟化性能提升 20-30%"
- "用户态/内核态切换 ~1μs/次"
- "快速路径减少 50% 模式切换"

这些无规范引用、无本仓实测记录，按 AGENTS.md 性能单一来源规则属机制推算（至多 C 级）。
旧节里的 EPTP Switching / VMCS Shadow 示例代码（`alloc_vmcs_shadow()`、
`vmx->nested.vmcs02.vmcs_shadow` 等）未核实，疑似示意性伪代码。

**已做**：README 性能节改为「⚡ 性能与测量」纯指针（→ phase10），数字标"待实测"，
移除未核实的示意代码；VMM 对比节保留机制描述、删除量化数字。
**待做**：按 COURSE_FRAMEWORK §1.3 审计——VPID/VMCS Shadow 收益若要引用，
须在 phase10 做 A/B 实测并分级；VMCS Shadow 真实实现见 `vmx/nested.c`（待精读补 annotations）。

**状态**: ⚠️ 待审计

## 勘误 2：practice README 误标 ex1 执行位置（2026-11-25 已修）

旧 practice README 称 "ex1/ex2/ex3/ex4/ex5 在 Guest 内执行"。
ex1-vmx-verify 读 `/dev/cpu/0/msr` 的**物理** VMX capability MSR，应在**宿主**执行
（Guest 内读到的是虚拟化后的值，不是硬件 capability）。

**已同步修正**：practice/README.md 执行环境说明与总览表（ex1 → 宿主）。

## 勘误 3：MSR Bitmap 布局的尺寸与偏移整体差 8 倍（高危，概念性错误）

- **发现日期**: 2026-11-25（phase0+1 轻量抽查，事实核查员复核）
- **位置**: `cpu-virtualization.md` §2.1「MSR Bitmap 机制」
- **错误**: 旧版写 "4KB = 4096 bits"，四张图各 **1024 bits**，偏移 **0x000 / 0x080 / 0x100 /
  0x180**。这是把"低位区覆盖 8192 个 MSR"错当成"8192 bit = 1024 bit"，
  于是偏移也按 128 Byte 递增。
- **正确**: 四张**连续位图各 1 KByte（8192 bit）**，偏移
  **0x000 读低位 / 0x400 读高位 / 0x800 写低位 / 0xC00 写高位**（整页 4096 Byte = 32768 bit）。
  - 规范：intel-vmx.pdf（Vol.3C, 326019-083US）**Section 25.6.9 "MSR-Bitmap Address"**——
    "the 64-bit physical address of four contiguous MSR bitmaps, which are **each 1-KByte
    in size**"，并逐个标注 "located at the MSR-bitmap address **plus 1024 / plus 2048 /
    plus 3072**"；每图 "one bit for each MSR address in the range 00000000H to 00001FFFH"
    （8192 个 MSR ⇒ 8192 bit = 1024 Byte）。
  - 源码：`arch/x86/kvm/vmx/vmx.h:464-470` 注释直接给区间——
    "low 用 bytes 0-0x3ff 读、0x800-0xbff 写；high 用 0x400-0x7ff 读、0xc00-0xfff 写"；
    `vmx.h:472-486` 宏以 `base = 0x0`(read) / `0x800`(write)，高位再加 `0x400`。
  - 交叉印证：`vmx.c:4079-4080` 的 `read_idx = 0x800/64 = 32`、
    `write_idx = 32 + 0x800/8 = 288` ⇒ 字节 0x100 与 0x900，只有"每图 1KB"能自洽；
    `alloc_loaded_vmcs()`（`vmx.c:2963`）把整页 memset 成 0xff（先全拦截再打洞）。
- **影响**: 任何按旧偏移去解 /dev/cpu 或 vmcs dump 里拦截位的做法都会读错字节，
  practice `ex2-cpuid-fault.c` + `trace-msr-access.sh` 的拦截位核对首当其冲。
- **已同步修正**: `cpu-virtualization.md` §2.1（含新增源码/规范引用与 x2APIC 下标推导）

## 勘误 4：一批与 Linux 6.12.93 实际代码不符的符号/机制表述（概念性）

均为 `grep` 本树即可定论的偏差，逐条已改原文。

1. **`handle_cpuid()` 不存在**，`annotations.md` §3 分发表的 CPUID 行写的就是这个符号
   （全树只命中 `arch/x86/coco/tdx/tdx.c:425` 的 TDX VE 处理，与 VMX exit 表无关）。
   实际 `[EXIT_REASON_CPUID] = kvm_emulate_cpuid`（`vmx.c:6103`，定义 `cpuid.c:1681`）。
   并且该行旧版标为"**用户空间**"：**CPUID 恒在内核闭环**（返回
   `kvm_skip_emulated_instruction()` → 1），MSR 同理（未命中才走
   `KVM_EXIT_X86_RDMSR/WRMSR`，`x86.c:2032/2069/2091`，或 `kvm_complete_insn_gp` 注入 #GP，
   `vmx/main.c:157`）；`KVM_EXIT_INTERNAL_ERROR` 与这两类无关。
   旧表还列了不存在的 `MMIO` exit reason（`KVM_EXIT_MMIO` 是 EPT violation 的产物）。
2. **`cpu_has_vmx_apicv()` 的判据被写错**：旧表述"APICv 要求 虚拟中断投递 + EOI 虚拟化 +
   MSR 位图"。实际是 `cpu_has_vmx_apic_register_virt()`(Secondary bit 8) &&
   `cpu_has_vmx_virtual_intr_delivery()`(Secondary bit 9) &&
   `cpu_has_vmx_posted_intr()`(**Pin-Based** bit 7) —— `capabilities.h:274-279` +
   `capabilities.h:92-95`，`VMX_FEATURE_POSTED_INTR = (0*32+7)`（`vmxfeatures.h:21`）。
   顺带：旧 §5 的 capability MSR 表把 Posted Interrupts 归给 CTLS2 ——
   "Process posted interrupts" 是 **pin-based** 控制位（Table 25-5 bit 7）。
   `cpu_has_vmx_ipiv()` 读的是 **Tertiary** 控制（`capabilities.h:281-284`），
   `cpu_has_vmx_flexpriority()` = TPR Shadow && Virtualize APIC accesses
   （`capabilities.h:286-290`）。
3. **模块参数名 `apicv` / `ipiv` 不存在**：`vmx.c:114` / `:117` 用的是
   `module_param(enable_apicv, ...)` / `module_param(enable_ipiv, ...)` ⇒
   参数名与变量同名。宿主实测 `/sys/module/kvm_intel/parameters/` 只有
   `enable_apicv`(Y)、`enable_ipiv`(N)，权限 444。另注意 `enable_apicv` 变量本体在
   `x86.c:229-230`（kvm.ko），kvm-intel.ko 只是把已导出符号注册成参数；
   SVM 侧**没有**重复注册（全树仅此一处 `module_param(...enable_apicv...)`）。
   表里补了每个参数的注册行号（`vmx.c:89-125`）。
4. **模块加载链写错**：旧图 `module_init(kvm_intel) → kvm_init(&vt_init_ops) ← kvm_main.c →
   kvm_arch_hardware_setup()`。实际 `module_init(vmx_init)`（`vmx.c:8699`，函数 `:8638`）→
   `kvm_x86_vendor_init(&vt_init_ops)`（调用 `vmx.c:8651`，定义 `x86.c:9741`）→
   `ops->hardware_setup()`（`x86.c:9821`）→ `kvm_ops_update()`（`x86.c:9825`，内含
   `memcpy(&kvm_x86_ops, ...)` `x86.c:9700`）；`kvm_init()`（`kvm_main.c:6476`，签名
   `(vcpu_size, vcpu_align, struct module *)`，见 `kvm_host.h:1058`）是 vmx_init 之后
   单独调用（`vmx.c:8686`）才注册 `/dev/kvm`（`misc_register(&kvm_dev)` `kvm_main.c:6534`）。
   **6.12.93 已无 `kvm_arch_hardware_setup()`**；"开硬件"改由
   `enable_virt_at_load`（`kvm_main.c:5583-5584`）或 `kvm_create_vm()` →
   `kvm_enable_virtualization()`（`kvm_main.c:1228`/`:5693`）→
   `kvm_arch_enable_virtualization_cpu()`（`x86.c:12682`）→
   `vmx_enable_virtualization_cpu()`（`vmx.c:2853`）。
   同一节旧说法"`__initdata` 是为了嵌套虚拟化需要切换回调表"也不成立：
   嵌套用子表 `nested_ops`（`vmx/main.c:133` → `nested.c:7310`），不换 `kvm_x86_ops`。
5. **`vcpu_vmx` 字段表两处错**：`preemption_timer` 不在 `vcpu_vmx`，
   它在 `struct nested_vmx`（`vmx.h:210`，L0 给 L2 软件模拟抢占定时器用）；
   `vpid` 是 `int`（`vmx.h:317`），`u16` 的是 `nested.vpid02/last_vpid`（`vmx.h:231-232`）。
6. **`exit_qualification` 不是 `vmx_vcpu_run()` 里 `vmcs_read32` 读的**：
   6.12 走惰性 `vmx_get_exit_qual()`（`vmx.h:708-716`，`vmcs_readl`，靠
   `VCPU_EXREG_EXIT_INFO_1` 记账），`exit_reason` 才是
   `vmx_vcpu_enter_exit()` 里 `vmcs_read32(VM_EXIT_REASON)`（`vmx.c:7326`）。
   `EXIT_QUALIFICATION` 是宽字段，按 32 位读本身是错的类型。
7. **`MSR_MISC_FEATURES_ENABLES` 是 0x140，不是 0x161**（`arch/x86/include/asm/msr-index.h:1040`，
   bit0 `:1042-1043`）；`MSR_PLATFORM_INFO` = 0xCE bit31 ✓（`:96-98`）。
8. **x2APIC MSR 透传方向写偏**：旧表述"APICv 启用时部分 x2APIC MSR（ICR 除外）透传"。
   实际 `vmx_update_msr_bitmap_x2apic()`（`vmx.c:4072`）先"读=除 TMCCT 外全透传、
   **写=全拦截**"（`:4110-4114`），再打洞：TPR(RW, 只需 virtualize x2APIC mode)、
   EOI(仅写)、SELF_IPI(仅写)、**ICR 只有 `enable_ipiv` 时才透传**（`:4120-4129`）；
   KVM 只动态改 0x800-0x83F（`:4104-4105`）。旧文的 "通过 VIRTUAL_APIC_PAGE 硬件直接处理"
   属于 APIC-access page 那条 **MMIO** 路径，不是 MSR 路径。
9. **SDM 引用号与"引文"**：旧版引 "SDM 25.1.2" 和一句 `"CPUID instruction (always;
   see Section 25.2)"`。本仓 spec/intel-vmx.pdf（Vol.3C 326019-083US）里 25.1 是
   VMCS Overview、**没有 25.1.2**，该字符串全文检索不存在。正确出处
   **§26.1.2 "Instructions That Cause VM Exits Unconditionally"**：
   "The following instructions cause VM exits when they are executed in VMX non-root
   operation: CPUID, GETSEC, INVD, and XSETBV."；优先级依据 **§26.1.1**
   "…it is assumed that the instruction does not incur a fault that takes priority over a
   VM exit."（README/practice/ex4 的 25.1.2 也一并改为 26.1.2）。
   结论本身不变：不存在 CPUID exiting 控制位，Secondary bit 12 = Enable INVPCID ✓。
- **已同步修正**: `cpu-virtualization.md`、`annotations.md`、`README.md`、
  `practice/README.md`、`practice/ex4-cpuid-analysis.c`

## 勘误 5：待核实 / 待实测（登记，不改结论）

- **CPUID Faulting 的性能收益**：`cpu-virtualization.md` §1.1.1.2 旧有
  "~1-2 μs vs ~0.1 μs / ★ 性能提升 10-20 倍 ★"，无本仓实测、无规范出处，
  已删除并改为"方向性结论 + 待实测"（与本章勘误 1 同类）。
  要引用须按 `../phase10-performance/measurement.md` 实测并汇入 phase10 分级。
- **CPUID Faulting 的 CPUID 枚举位**：旧文写 "CPUID leaf 7, sub-leaf **0**, ECX bit 3"。
  本仓 spec/ 只有 Vol.3C（系统编程），CPUID 指令位定义在 **Vol.2A**，无法核对；
  Linux 的判定路径只用 `MSR_PLATFORM_INFO` bit31（`arch/x86/kernel/cpu/intel.c:524-532`），
  `X86_FEATURE_CPUID_FAULT` 放在自定义 scattered word 7（`cpufeatures.h:193`），
  QEMU 10.1.0-rc2 `target/i386/cpu.h` 也没定义该位 ⇒ **标"待核实（需 Vol.2A）"**。
- **VM-entry/exit MSR load/store 列表的硬件容量**：`MAX_NR_LOADSTORE_MSRS = 8` 是
  KVM 数组上限（`vmx.h:28`），硬件上限要查 `IA32_VMX_MISC`（规范附录 A.6，本 PDF 未含）
  ⇒ **待核实**。已在文中区分"KVM 上限"与"硬件上限"，并注明 KVM 的这套列表实际只服务
  嵌套（`nested.c:2309-2310 / 2621-2622 / 3255-3258`），L1 侧走的是
  uret MSR（`vmx.c:8366-8372`）与专用 VMCS 控制位/状态字段。
