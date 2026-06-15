# 第一阶段：源码精读注释

> 基于 Linux 6.12.93 源码（行号已验证）

## 1. vt_x86_ops - VMX操作回调表

**文件**: `arch/x86/kvm/vmx/main.c:19-162`

这是VMX实现的核心接口表，将KVM x86通用层与VMX硬件实现解耦。

```c
struct kvm_x86_ops vt_x86_ops __initdata = {
    .name = KBUILD_MODNAME,

    // === 硬件管理 ===
    .check_processor_compatibility = vmx_check_processor_compat,
    .hardware_unsetup = vmx_hardware_unsetup,
    .enable_virtualization_cpu = vmx_enable_virtualization_cpu,   // VMXON
    .disable_virtualization_cpu = vmx_disable_virtualization_cpu, // VMXOFF

    // === VM生命周期 ===
    .vm_size = sizeof(struct kvm_vmx),
    .vm_init = vmx_vm_init,
    .vm_destroy = vmx_vm_destroy,

    // === vCPU生命周期 ===
    .vcpu_precreate = vmx_vcpu_precreate,
    .vcpu_create = vmx_vcpu_create,    // ★ 分配VMCS区域
    .vcpu_free = vmx_vcpu_free,
    .vcpu_reset = vmx_vcpu_reset,

    // === vCPU调度 ===
    .prepare_switch_to_guest = vmx_prepare_switch_to_guest,
    .vcpu_load = vmx_vcpu_load,        // vCPU加载到pCPU
    .vcpu_put = vmx_vcpu_put,          // vCPU从pCPU卸载

    // === 寄存器访问 ===
    .get_msr / .set_msr,              // MSR读写
    .get_segment_base / .get_segment / .set_segment,  // 段寄存器
    .get_cpl,                          // 当前特权级
    .set_cr0 / .set_cr4 / .set_efer,  // 控制寄存器
    .get_rflags / .set_rflags,        // 标志寄存器

    // === TLB管理 ===
    .flush_tlb_all = vmx_flush_tlb_all,
    .flush_tlb_current = vmx_flush_tlb_current,
    .flush_tlb_gva = vmx_flush_tlb_gva,
    .flush_tlb_guest = vmx_flush_tlb_guest,  // INVVPID

    // === ★ 核心执行路径 ===
    .vcpu_pre_run = vmx_vcpu_pre_run,
    .vcpu_run = vmx_vcpu_run,          // ★ VM-Entry/Exit主循环
    .handle_exit = vmx_handle_exit,    // ★ VM-Exit处理分发
    .handle_exit_irqoff = vmx_handle_exit_irqoff,

    // === 中断/NMI注入 ===
    .inject_irq = vmx_inject_irq,
    .inject_nmi = vmx_inject_nmi,
    .inject_exception = vmx_inject_exception,
    .cancel_injection = vmx_cancel_injection,
    .interrupt_allowed = vmx_interrupt_allowed,
    .nmi_allowed = vmx_nmi_allowed,
    .enable_irq_window = vmx_enable_irq_window,
    .enable_nmi_window = vmx_enable_nmi_window,

    // === APIC虚拟化 ===
    .set_virtual_apic_mode = vmx_set_virtual_apic_mode,
    .set_apic_access_page_addr = vmx_set_apic_access_page_addr,
    .refresh_apicv_exec_ctrl = vmx_refresh_apicv_exec_ctrl,
    .sync_pir_to_irr = vmx_sync_pir_to_irr,  // ★ Posted Interrupts同步
    .hwapic_irr_update = vmx_hwapic_irr_update,
    .hwapic_isr_update = vmx_hwapic_isr_update,
    .load_eoi_exitmap = vmx_load_eoi_exitmap,

    // === Posted Interrupts ===
    .pi_update_irte = vmx_pi_update_irte,     // PI中断重映射表更新
    .pi_start_assignment = vmx_pi_start_assignment,

    // === MMU ===
    .load_mmu_pgd = vmx_load_mmu_pgd,         // 加载EPT根页表

    // === TSC ===
    .get_l2_tsc_offset / .write_tsc_offset,
    .get_l2_tsc_multiplier / .write_tsc_multiplier,

    // === PML (脏页日志) ===
    .cpu_dirty_log_size = PML_ENTITY_NUM,
    .update_cpu_dirty_logging = vmx_update_cpu_dirty_logging,
};
```

**学习要点**:
- 此表定义了VMX实现的所有操作接口
- KVM x86通用层通过 `kvm_x86_call()` 宏间接调用这些回调
- `__initdata` 表示初始化后可释放（但回调指针保留在runtime_ops中）
- 注意区分初始化时和运行时的操作

---

## 2. vmx_hardware_setup() - 硬件初始化

**文件**: `arch/x86/kvm/vmx/vmx.c:8404-8503`

```c
__init int vmx_hardware_setup(void)
{
    unsigned long host_bndcfgs;
    struct desc_ptr dt;
    int r;

    store_idt(&dt);
    host_idt_base = dt.address;           // 保存Host IDT基址

    vmx_setup_user_return_msrs();          // 配置用户返回MSR列表

    if (setup_vmcs_config(&vmcs_config, &vmx_capability) < 0)
        return -EIO;                       // ★ 读取VMCS配置 (MSR_IA32_VMX_*)

    if (boot_cpu_has(X86_FEATURE_NX))
        kvm_enable_efer_bits(EFER_NX);    // 启用NX位支持

    // === 特性检测和启用 ===

    // VPID: 需要硬件支持VPID + INVVPID指令
    if (!cpu_has_vmx_vpid() || !cpu_has_vmx_invvpid() ||
        !(cpu_has_vmx_invvpid_single() || cpu_has_vmx_invvpid_global()))
        enable_vpid = 0;

    // EPT: 需要4级页表 + WB内存类型 + INVEPT全局
    if (!cpu_has_vmx_ept() ||
        !cpu_has_vmx_ept_4levels() ||
        !cpu_has_vmx_ept_mt_wb() ||
        !cpu_has_vmx_invept_global())
        enable_ept = 0;

    // 无EPT时必须有NX (影子页表需要NX)
    if (!enable_ept && !boot_cpu_has(X86_FEATURE_NX)) {
        pr_err_ratelimited("NX not supported\n");
        return -EOPNOTSUPP;
    }

    // EPT A/D位: 需要硬件支持
    if (!cpu_has_vmx_ept_ad_bits() || !enable_ept)
        enable_ept_ad_bits = 0;

    // Unrestricted Guest: 允许Guest实模式
    if (!cpu_has_vmx_unrestricted_guest() || !enable_ept)
        enable_unrestricted_guest = 0;

    // FlexPriority: TPR Shadow
    if (!cpu_has_vmx_flexpriority())
        flexpriority_enabled = 0;

    // 虚拟NMI
    if (!cpu_has_virtual_nmis())
        enable_vnmi = 0;

    // APICv: 虚拟中断投递
    if (!cpu_has_vmx_apicv())
        enable_apicv = 0;
    if (!enable_apicv)
        vt_x86_ops.sync_pir_to_irr = NULL;  // 关闭PI同步

    // IPI虚拟化 (需要APICv + IPIV)
    if (!enable_apicv || !cpu_has_vmx_ipiv())
        enable_ipiv = false;

    // TSC缩放
    if (cpu_has_vmx_tsc_scaling())
        kvm_caps.has_tsc_control = true;

    // PLE (Pause Loop Exiting)
    if (!cpu_has_vmx_ple()) {
        ple_gap = 0; ple_window = 0; ...
    }

    // Bus Lock检测
    kvm_caps.has_bus_lock_exit = cpu_has_vmx_bus_lock_detection();

    // SPTE加密位掩码 (MKTME)
    vmx_setup_me_spte_mask();

    // 嵌套虚拟化
    if (nested)
        r = nested_vmx_hardware_setup(kvm_vmx_exit_handlers);

    return r;
}
```

**特性检测流程**:
```
vmx_hardware_setup()
  │
  ├─ setup_vmcs_config()         ← 读取VMX capability MSRs
  │   ├─ MSR_IA32_VMX_BASIC       ← VMCS revision, VMXON区域大小
  │   ├─ MSR_IA32_VMX_PINBASED_CTLS ← 引脚控制
  │   ├─ MSR_IA32_VMX_PROCBASED_CTLS ← CPU控制
  │   ├─ MSR_IA32_VMX_EXIT_CTLS   ← Exit控制
  │   ├─ MSR_IA32_VMX_ENTRY_CTLS  ← Entry控制
  │   └─ MSR_IA32_VMX_EPT_VPID_CAP ← EPT/VPID能力
  │
  ├─ 特性启用检测 (按依赖关系)
  │   ├─ EPT → EPT_AD → unrestricted_guest
  │   ├─ VPID → INVVPID
  │   ├─ APICv → IPIv → PI
  │   └─ FlexPriority → TPR Shadow
  │
  └─ 回调表调整 (禁用不支持的功能)
      ├─ sync_pir_to_irr = NULL (无APICv)
      ├─ set_apic_access_page_addr = NULL (无FlexPriority)
      └─ update_cr8_intercept = NULL (无TPR Shadow)
```

---

## 3. vmx_vcpu_run() - vCPU执行主循环

**文件**: `arch/x86/kvm/vmx/vmx.c:7344-7530+`

这是KVM中最关键的函数之一，负责执行VM-Entry并处理VM-Exit后的状态恢复。

```c
fastpath_t vmx_vcpu_run(struct kvm_vcpu *vcpu, u64 run_flags)
{
    bool force_immediate_exit = run_flags & KVM_RUN_FORCE_IMMEDIATE_EXIT;
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    unsigned long cr3, cr4;

    // === 前置检查 ===

    // 1. 检查Guest状态有效性
    if (unlikely(vmx->emulation_required)) {
        // Guest状态无效，需要模拟而非直接进入
        vmx->exit_reason.full = EXIT_REASON_INVALID_STATE;
        vmx->exit_reason.failed_vmentry = 1;
        return EXIT_FASTPATH_NONE;
    }

    // === Trace ===
    trace_kvm_entry(vcpu, force_immediate_exit);

    // === PLE窗口更新 ===
    if (vmx->ple_window_dirty) {
        vmx->ple_window_dirty = false;
        vmcs_write32(PLE_WINDOW, vmx->ple_window);
    }

    // === 同步脏寄存器到VMCS ===
    if (kvm_register_is_dirty(vcpu, VCPU_REGS_RSP))
        vmcs_writel(GUEST_RSP, vcpu->arch.regs[VCPU_REGS_RSP]);
    if (kvm_register_is_dirty(vcpu, VCPU_REGS_RIP))
        vmcs_writel(GUEST_RIP, vcpu->arch.regs[VCPU_REGS_RIP]);
    vcpu->arch.regs_dirty = 0;

    // === 调试寄存器 ===
    if (run_flags & KVM_RUN_LOAD_GUEST_DR6)
        set_debugreg(vcpu->arch.dr6, 6);

    // === Host CR3/CR4 刷新 ===
    // 必须在VM-Entry前完成，因为内核可能切换PCID
    cr3 = __get_current_cr3_fast();
    if (unlikely(cr3 != vmx->loaded_vmcs->host_state.cr3)) {
        vmcs_writel(HOST_CR3, cr3);
        vmx->loaded_vmcs->host_state.cr3 = cr3;
    }
    cr4 = cr4_read_shadow();
    if (unlikely(cr4 != vmx->loaded_vmcs->host_state.cr4)) {
        vmcs_writel(HOST_CR4, cr4);
        vmx->loaded_vmcs->host_state.cr4 = cr4;
    }

    // === 单步调试处理 ===
    if (vcpu->guest_debug & KVM_GUESTDBG_SINGLESTEP)
        vmx_set_interrupt_shadow(vcpu, 0);

    // === XSAVE状态切换 ===
    kvm_load_guest_xsave_state(vcpu);

    // === Intel PT (Processor Trace) ===
    pt_guest_enter(vmx);

    // === 性能计数器 ===
    atomic_switch_perf_msrs(vmx);
    if (intel_pmu_lbr_is_enabled(vcpu))
        vmx_passthrough_lbr_msrs(vcpu);

    // === 抢占定时器 ===
    if (enable_preemption_timer)
        vmx_update_hv_timer(vcpu, force_immediate_exit);
    else if (force_immediate_exit)
        smp_send_reschedule(vcpu->cpu);

    // === 等待LAPIC定时器 ===
    kvm_wait_lapic_expire(vcpu);

    // ═══════════════════════════════════════════════
    // ★ ★ ★  核心: VM-Entry/Exit (汇编实现)  ★ ★ ★
    // ═══════════════════════════════════════════════
    vmx_vcpu_enter_exit(vcpu, __vmx_vcpu_run_flags(vmx));
    //
    // 在此调用内部:
    //   1. 保存Host状态 (或从VMCS恢复)
    //   2. VMENTER指令 → 进入Non-Root模式
    //   3. Guest执行...
    //   4. VM-Exit触发 → 回到Root模式
    //   5. 保存VM-Exit信息到vmx结构
    //
    // 返回后 vmx->exit_reason 已填充

    // === VM-Exit后处理 ===

    // eVMCS同步 (Hyper-V)
    if (kvm_is_using_evmcs()) {
        current_evmcs->hv_clean_fields |= HV_VMX_ENLIGHTENED_CLEAN_FIELD_ALL;
    }

    // DEBUGCTL恢复
    if (vcpu->arch.host_debugctl)
        update_debugctlmsr(vcpu->arch.host_debugctl);

    // === 读取VM-Exit信息 ===
    vmx->exit_qualification = vmcs_readl(EXIT_QUALIFICATION);
    vmx->exit_intr_info = vmcs_read32(VM_EXIT_INTR_INFO);

    // ... (更多状态保存和恢复)

    // === L1D Flush (安全缓解) ===
    // vmx_l1d_flush() - 可选的L1D缓存刷新

    // === 返回exit处理类型 ===
    // EXIT_FASTPATH_NONE        → 需要完整exit处理
    // EXIT_FASTPATH_REENTER_GUEST → 可以直接重新进入
    // EXIT_FASTPATH_RETREAT     → 需要回退RIP后重新进入
}
```

**VM-Entry/Exit 汇编入口**:
```
vmx_vcpu_enter_exit() [vmx/vmx.c]
  └→ __vmx_vcpu_run() [vmenter.S]  ← 汇编实现
       │
       ├─ 保存Host callee-saved寄存器 (rbx, rbp, r12-r15)
       ├─ vmcs_writel(HOST_RSP)    ← Host栈指针
       ├─ vmcs_writel(HOST_RIP)    ← Host返回地址
       │
       ├─ ★ VMENTER 指令
       │     CPU从VMX Root → Non-Root模式
       │     Guest RIP从VMCS加载
       │     Guest状态从VMCS Guest Area加载
       │
       ├─ Guest执行... (时间不定)
       │
       ├─ ★ VM-EXIT 触发
       │     CPU从Non-Root → Root模式
       │     Host RIP从VMCS HOST_RIP加载
       │     Exit信息保存到VMCS Exit Info Fields
       │
       └─ 返回到C代码
```

---

## 4. vmx_handle_exit() - VM-Exit处理

**文件**: `arch/x86/kvm/vmx/vmx.c:6615-6631`

```c
int vmx_handle_exit(struct kvm_vcpu *vcpu, fastpath_t exit_fastpath)
{
    int ret = __vmx_handle_exit(vcpu, exit_fastpath);

    // Bus Lock检测处理
    if (to_vmx(vcpu)->exit_reason.bus_lock_detected) {
        if (ret > 0)
            vcpu->run->exit_reason = KVM_EXIT_X86_BUS_LOCK;
        vcpu->run->flags |= KVM_RUN_X86_BUS_LOCK;
        return 0;  // 退出到用户空间
    }
    return ret;
}
```

**__vmx_handle_exit() 内部分发逻辑**:
```
__vmx_handle_exit()
  │
  ├─ 检查 failed_vmentry (VM-Entry失败)
  │   └→ handle_vmentry_failure()
  │
  ├─ 检查 exit_fastpath (可快速处理)
  │   ├─ EXIT_FASTPATH_REENTER_GUEST → 直接返回
  │   └─ EXIT_FASTPATH_RETREAT → RIP回退后返回
  │
  ├─ exit_reason 分发:
  │   │
  │   ├─ EXIT_REASON_EXCEPTION_NMI       → handle_exception_nmi()
  │   ├─ EXIT_REASON_EXTERNAL_INTERRUPT  → handle_external_interrupt()
  │   ├─ EXIT_REASON_TRIPLE_FAULT        → handle_triple_fault()
  │   ├─ EXIT_REASON_PENDING_INTERRUPT   → handle_interrupt_window()
  │   ├─ EXIT_REASON_PENDING_MCE_NMI     → handle_pending_mce_nmi()
  │   ├─ EXIT_REASON_CPUID               → handle_cpuid()
  │   ├─ EXIT_REASON_HLT                 → handle_halt()
  │   ├─ EXIT_REASON_INVD                → handle_invd()
  │   ├─ EXIT_REASON_INVLPG              → handle_invlpg()
  │   ├─ EXIT_REASON_RDPMC               → handle_rdpmc()
  │   ├─ EXIT_REASON_RDRAND              → handle_rdrand()
  │   ├─ EXIT_REASON_RDSEED              → handle_rdseed()
  │   ├─ EXIT_REASON_VMCALL              → handle_vmcall()
  │   ├─ EXIT_REASON_VMCLEAR             → handle_vmx_insn()
  │   ├─ EXIT_REASON_VMLAUNCH            → handle_vmx_insn()
  │   ├─ EXIT_REASON_VMPTRLD             → handle_vmx_insn()
  │   ├─ EXIT_REASON_VMPTRST             → handle_vmx_insn()
  │   ├─ EXIT_REASON_VMREAD              → handle_vmx_insn()
  │   ├─ EXIT_REASON_VMRESUME            → handle_vmx_insn()
  │   ├─ EXIT_REASON_VMWRITE             → handle_vmx_insn()
  │   ├─ EXIT_REASON_VMXOFF              → handle_vmx_insn()
  │   ├─ EXIT_REASON_VMXON               → handle_vmx_insn()
  │   ├─ EXIT_REASON_CR_ACCESS           → handle_cr()
  │   ├─ EXIT_REASON_DR_ACCESS           → handle_dr()
  │   ├─ EXIT_REASON_IO_INSTRUCTION      → handle_io()
  │   ├─ EXIT_REASON_MSR_WRITE           → handle_write_msr()
  │   ├─ EXIT_REASON_MSR_READ            → handle_read_msr()
  │   ├─ EXIT_REASON_INVALID_STATE       → handle_invalid_guest_state()
  │   ├─ EXIT_REASON_MWAIT_INSTRUCTION   → handle_mwait()
  │   ├─ EXIT_REASON_MONITOR_INSTRUCTION → handle_monitor()
  │   ├─ EXIT_REASON_PAUSE_INSTRUCTION   → handle_pause()
  │   ├─ EXIT_REASON_MCE_DURING_VMENTRY  → handle_mce_during_vmentry()
  │   ├─ EXIT_REASON_TPR_BELOW_THRESHOLD → handle_tpr_below_threshold()
  │   ├─ EXIT_REASON_APIC_ACCESS         → handle_apic_access()
  │   ├─ EXIT_REASON_EPT_VIOLATION       → handle_ept_violation()  ★
  │   ├─ EXIT_REASON_EPT_MISCONFIG      → handle_ept_misconfig()  ★
  │   ├─ EXIT_REASON_INVPCID             → handle_invpcid()
  │   ├─ EXIT_REASON_XSAVES              → handle_xsaves()
  │   ├─ EXIT_REASON_XRSTORS             → handle_xrstors()
  │   ├─ EXIT_REASON_PML_FULL            → handle_pml_full()
  │   ├─ EXIT_REASON_PREEMPTION_TIMER    → handle_preemption_timer()
  │   ├─ EXIT_REASON_BUS_LOCK            → handle_bus_lock()
  │   └─ EXIT_REASON_NOTIFY_VM_EXIT      → handle_notify_vmexit()
  │
  └─ 未知exit_reason → KVM_EXIT_INTERNAL_ERROR
```

---

## 5. struct vcpu_vmx - VMX vCPU扩展结构

**文件**: `arch/x86/kvm/vmx/vmx.h:251-400+`

```c
struct vcpu_vmx {
    struct kvm_vcpu       vcpu;        // ★ 基础vCPU结构 (必须在第一位)
    u8                    fail;        // VM-Entry是否失败
    u8                    x2apic_msr_bitmap_mode;

    bool                  guest_state_loaded;  // Guest状态是否已加载

    // === VM-Exit信息 (缓存) ===
    unsigned long         exit_qualification;  // EXIT_QUALIFICATION
    u32                   exit_intr_info;      // VM_EXIT_INTR_INFO
    u32                   idt_vectoring_info;  // IDT_VECTORING_INFO
    ulong                 rflags;

    // === MSR管理 ===
    struct vmx_uret_msr   guest_uret_msrs[MAX_NR_USER_RETURN_MSRS];
    u64                   spec_ctrl;
    u32                   msr_ia32_umwait_control;

    // === ★ VMCS ===
    struct loaded_vmcs    vmcs01;       // L1 guest的VMCS
    struct loaded_vmcs   *loaded_vmcs;  // 当前活跃的VMCS
                                         // 非嵌套=vmcs01, 嵌套=vmcs02

    struct msr_autoload {
        struct vmx_msrs guest;
        struct vmx_msrs host;
    } msr_autoload;

    struct msr_autostore {
        struct vmx_msrs guest;
    } msr_autostore;

    // === APIC虚拟化 ===
    struct kvm_host_map apic_access_page_map;  // APIC访问页映射
    struct kvm_host_map virtual_apic_map;      // 虚拟APIC页映射

    // === ★ Posted Interrupts ===
    struct pi_desc *pi_desc;          // PI描述符指针
    bool pi_pending;                  // PI挂起标志
    u16 posted_intr_nv;              // PI通知向量

    // === 抢占定时器 ===
    struct hrtimer preemption_timer;

    // === VPID ===
    u16 vpid02;                      // L2的VPID
    u16 last_vpid;                   // 上次使用的VPID

    // === 嵌套虚拟化 ===
    struct nested_vmx { ... } nested;

    // === Intel PT ===
    struct pt_desc pt_desc;

    // === LBR ===
    struct lbr_desc lbr_desc;
};
```

**内存布局关系**:
```
struct vcpu_vmx
├── struct kvm_vcpu vcpu           ← 通用vCPU (所有架构共享)
│   ├── struct kvm_run *run        ← 与用户空间共享的内存
│   ├── struct kvm *kvm            ← 所属VM
│   ├── int vcpu_id                ← vCPU编号
│   ├── struct kvm_vcpu_arch arch  ← 架构相关状态
│   │   ├── struct kvm_mmu *mmu   ← MMU上下文
│   │   ├── u64 regs[...]         ← 通用寄存器缓存
│   │   ├── struct kvm_lapic *apic← 虚拟LAPIC
│   │   └── ...
│   └── ...
├── struct loaded_vmcs vmcs01      ← VMCS区域 (物理页对齐)
│   ├── struct vmcs *vmcs          ← VMCS虚拟地址
│   ├── struct vmcs_host_state host_state
│   └── ...
├── struct pi_desc ...              ← Posted Interrupt描述符
└── ...
```

---

## 6. VMX模块参数

**文件**: `arch/x86/kvm/vmx/vmx.c:89-149`

```c
// 核心模块参数 (可通过 /sys/module/kvm_intel/parameters/ 查看)

bool enable_vpid = 1;              // VPID支持 (避免VM-Entry时flush TLB)
bool enable_vnmi = 1;              // 虚拟NMI支持
bool flexpriority_enabled = 1;     // TPR Shadow (灵活APIC优先级)
bool enable_ept = 1;               // ★ Extended Page Tables
bool enable_unrestricted_guest = 1;// 无限制Guest (实模式/保护模式)
bool enable_ept_ad_bits = 1;       // EPT Accessed/Dirty位
bool emulate_invalid_guest_state = true; // 模拟无效Guest状态
bool fasteoi = 1;                  // 快速EOI处理
bool enable_apicv;                 // ★ APIC虚拟化
bool enable_ipiv = true;           // IPI虚拟化
bool nested = 1;                   // ★ 嵌套虚拟化
bool enable_pml = 1;               // Page Modification Logging (脏页)
bool enable_preemption_timer = 1;  // 抢占定时器
```

---

## 7. vmx_init() - 模块初始化

**文件**: `arch/x86/kvm/vmx/main.c:164-171`

```c
struct kvm_x86_init_ops vt_init_ops __initdata = {
    .hardware_setup = vmx_hardware_setup,  // ★ 硬件初始化回调
    .handle_intel_pt_intr = NULL,
    .runtime_ops = &vt_x86_ops,            // 运行时操作表
    .pmu_ops = &intel_pmu_ops,
};
```

**初始化流程**:
```
module_init() → kvm_init() [kvm_main.c]
  → kvm_arch_hardware_setup() [x86.c]
    → kvm_x86_vendor_init()
      → hardware_setup() = vmx_hardware_setup()  ← 检测CPU特性
    → kvm_x86_check_processor_compatibility()
  → kvm_create_vm() 系列 ioctl 注册
```
