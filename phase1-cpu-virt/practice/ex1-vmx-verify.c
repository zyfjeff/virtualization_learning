/*
 * Exercise 1: VMX Capability 深度解码
 *
 * 目标: 完整解码 KVM 读取的所有 VMX capability MSR，理解特性依赖链
 * 连接: ../annotations.md §5「VMX 特性检测」
 *
 * 读取 7 个 VMX MSR:
 *   IA32_VMX_BASIC          (0x480)
 *   IA32_VMX_PINBASED_CTLS  (0x481)
 *   IA32_VMX_PROCBASED_CTLS (0x482)
 *   IA32_VMX_EXIT_CTLS      (0x483)
 *   IA32_VMX_ENTRY_CTLS     (0x484)
 *   IA32_VMX_PROCBASED_CTLS2(0x48B)
 *   IA32_VMX_EPT_VPID_CAP   (0x48C)
 *
 * 每个 MSR 的 allowed-0 (must-be-1) 和 allowed-1 (can-be-1) 都会解码
 *
 * 源码引用:
 *   arch/x86/kvm/vmx/vmx.c:2590 — setup_vmcs_config()
 *   arch/x86/kvm/vmx/vmx.c:2563 — adjust_vmx_controls()
 *   arch/x86/include/asm/vmx.h:27-122 — 控制位定义
 *
 * 规范引用:
 *   Intel SDM Vol 3, Appendix A (VMX Capability Reporting)
 *   Intel SDM Vol 3, Section 24.6 (VMCS Layout)
 *
 * 注意: 需要加载 msr 模块 (modprobe msr)
 */

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

/* ================================================================
 *  IA32_VMX_BASIC (0x480) 字段
 *  参考: Intel SDM Vol 3, Appendix A.1
 * ================================================================ */
static void decode_vmx_basic(uint64_t val)
{
    printf("  VMCS revision ID:        %u\n", (uint32_t)(val & 0x7FFFFFFF));
    printf("  VMCS size:               %u bytes\n",
           (uint32_t)((val >> 32) & 0x1FFF));
    printf("  32-bit VMX physical addresses: %s\n",
           (val & (1ULL << 31)) ? "no (50-bit)" : "yes (limited)");
    printf("  Dual-monitor treatment:  %s\n",
           (val & (1ULL << 49)) ? "supported" : "not supported");
    printf("  Memory type for VMCS:    ");
    switch ((val >> 50) & 0xF) {
    case 0: printf("0 (UC)\n"); break;
    case 6: printf("6 (WB)\n"); break;
    default: printf("%lu (reserved)\n", (val >> 50) & 0xF);
    }
    printf("  Ins/Outs VMX-info:       %u\n", (uint32_t)((val >> 54) & 0xF));
    printf("  True-CTLS MSRs:          %s\n",
           (val & (1ULL << 55)) ? "supported (0x48D-0x490)" : "not supported");

    /*
     * 关键: 如果 bit 55 = 1, CPU 提供 true-CTLS MSR (0x48D-0x490),
     * 允许更细粒度的控制位设置。KVM 优先使用 true-CTLS (见 vmx.c:2590)。
     */
}

/* ================================================================
 *  IA32_VMX_PINBASED_CTLS (0x481) / TRUE 版本 (0x48D)
 *  参考: Intel SDM Vol 3, Appendix A.3.1, Figure A-2
 *  源码: arch/x86/include/asm/vmx.h:88-92
 * ================================================================ */
static void decode_pinbased(uint64_t val)
{
    uint32_t allowed0 = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t allowed1 = (uint32_t)((val >> 32) & 0xFFFFFFFF);

    printf("  allowed-0 (must-be-1):  0x%08x\n", allowed0);
    printf("  allowed-1 (can-be-1):   0x%08x\n", allowed1);

    struct { int bit; const char *name; const char *desc; } bits[] = {
        { 0, "EXT_INTR_EXITING",      "External-interrupt exiting" },
        { 3, "NMI_EXITING",           "NMI exiting" },
        { 5, "VIRTUAL_NMIS",          "Virtual NMIs (vnmi)" },
        { 6, "VMX_PREEMPTION_TIMER",  "Activate VMX-preemption timer" },
        { 7, "POSTED_INTERRUPTS",     "Process posted interrupts" },
    };

    printf("\n  Bit  Req   Opt  Name\n");
    printf("  ───  ───   ───  ────────────────────────────────────\n");
    for (size_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        uint32_t mask = 1U << bits[i].bit;
        const char *req = (allowed0 & mask) ? "MUST" : "    ";
        const char *opt = (allowed1 & mask) ? "can " : "    ";
        printf("  %2d   %s   %s   %s — %s\n",
               bits[i].bit, req, opt, bits[i].name, bits[i].desc);
    }

    /*
     * 依赖链 (annotations.md §5):
     *   Posted Interrupts 依赖 APICv (VIRTUAL_NMIS + VPID + EPT 等)
     *   VMX_PREEMPTION_TIMER 用于 halt-polling 的 hypervisor timer
     *
     * KVM 默认启用: EXT_INTR + NMI_EXITING (必须)
     * KVM 可选启用: VIRTUAL_NMIS, PREEMPTION_TIMER, POSTED_INTR
     */
}

/* ================================================================
 *  IA32_VMX_PROCBASED_CTLS (0x482) / TRUE 版本 (0x48E)
 *  参考: Intel SDM Vol 3, Appendix A.3.2, Figure A-3
 *  源码: arch/x86/include/asm/vmx.h:27-48
 * ================================================================ */
static void decode_procbased(uint64_t val)
{
    uint32_t allowed0 = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t allowed1 = (uint32_t)((val >> 32) & 0xFFFFFFFF);

    printf("  allowed-0 (must-be-1):  0x%08x\n", allowed0);
    printf("  allowed-1 (can-be-1):   0x%08x\n", allowed1);

    struct { int bit; const char *name; } bits[] = {
        {  2, "INTR_WINDOW_EXITING"     },
        {  3, "USE_TSC_OFFSETTING"      },
        {  7, "HLT_EXITING"             },
        {  9, "INVLPG_EXITING"          },
        { 10, "MWAIT_EXITING"           },
        { 11, "RDPMC_EXITING"           },
        { 12, "RDTSC_EXITING"           },
        { 15, "CR3_LOAD_EXITING"        },
        { 16, "CR3_STORE_EXITING"       },
        { 17, "TERTIARY_CONTROLS"       },
        { 19, "CR8_LOAD_EXITING"        },
        { 20, "CR8_STORE_EXITING"       },
        { 21, "TPR_SHADOW (VIRTUAL_TPR)"},
        { 22, "NMI_WINDOW_EXITING"      },
        { 23, "MOV_DR_EXITING"          },
        { 24, "UNCOND_IO_EXITING"       },
        { 25, "USE_IO_BITMAPS"          },
        { 27, "MONITOR_TRAP_FLAG"       },
        { 28, "USE_MSR_BITMAPS"         },
        { 29, "MONITOR_EXITING"         },
        { 30, "PAUSE_EXITING"           },
        { 31, "SECONDARY_CONTROLS"      },
    };

    printf("\n  Bit  Req   Opt  Name\n");
    printf("  ───  ───   ───  ────────────────────────────────\n");
    for (size_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        uint32_t mask = 1U << bits[i].bit;
        const char *req = (allowed0 & mask) ? "MUST" : "    ";
        const char *opt = (allowed1 & mask) ? "can " : "    ";
        printf("  %2d   %s   %s   %s\n",
               bits[i].bit, req, opt, bits[i].name);
    }

    /*
     * KVM 必须的位 (KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL):
     *   HLT_EXITING (7),  USE_MSR_BITMAPS (28),
     *   SECONDARY_CONTROLS (31)
     *
     * KVM 可选的位 (KVM_OPTIONAL_VMX_CPU_BASED_VM_EXEC_CONTROL):
     *   INTR_WINDOW (2), TSC_OFFSET (3), CR8_LOAD/STORE (19,20),
     *   TPR_SHADOW (21), NMI_WINDOW (22), USE_IO_BITMAPS (25),
     *   MONITOR_TRAP_FLAG (27), MONITOR_EXITING (29), PAUSE_EXITING (30)
     *
     * 注意: CPU_BASED_ALWAYSON_WITHOUT_TRUE_MSR = 0x0401e172
     *   没有 true-CTLS MSR 时, 这些位必须为 1
     */
}

/* ================================================================
 *  IA32_VMX_EXIT_CTLS (0x483) / TRUE 版本 (0x48F)
 *  参考: Intel SDM Vol 3, Appendix A.4, Figure A-4
 *  源码: arch/x86/include/asm/vmx.h:97-109
 * ================================================================ */
static void decode_exit_ctls(uint64_t val)
{
    uint32_t allowed0 = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t allowed1 = (uint32_t)((val >> 32) & 0xFFFFFFFF);

    printf("  allowed-0 (must-be-1):  0x%08x\n", allowed0);
    printf("  allowed-1 (can-be-1):   0x%08x\n", allowed1);

    struct { int bit; const char *name; } bits[] = {
        {  2, "SAVE_DEBUG_CONTROLS"     },
        {  9, "HOST_ADDR_SPACE_SIZE"    },
        { 12, "LOAD_IA32_PERF_GLOBAL_CTRL" },
        { 15, "ACK_INTR_ON_EXIT"        },
        { 18, "SAVE_IA32_PAT"           },
        { 19, "LOAD_IA32_PAT"           },
        { 20, "SAVE_IA32_EFER"          },
        { 21, "LOAD_IA32_EFER"          },
        { 22, "SAVE_VMX_PREEMPTION_TIMER"},
        { 23, "CLEAR_BNDCFGS"           },
        { 24, "PT_CONCEAL_PIP"          },
        { 25, "CLEAR_IA32_RTIT_CTL"     },
    };

    printf("\n  Bit  Req   Opt  Name\n");
    printf("  ───  ───   ───  ─────────────────────────────────\n");
    for (size_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        uint32_t mask = 1U << bits[i].bit;
        const char *req = (allowed0 & mask) ? "MUST" : "    ";
        const char *opt = (allowed1 & mask) ? "can " : "    ";
        printf("  %2d   %s   %s   %s\n",
               bits[i].bit, req, opt, bits[i].name);
    }

    /*
     * KVM 必须: HOST_ADDR_SPACE_SIZE (9) — 64-bit Host
     * KVM 可选: ACK_INTR_ON_EXIT (15), LOAD/SAVE EFER/PAT,
     *           LOAD_PERF_GLOBAL_CTRL (12)
     *
     * 注意: VM_EXIT_ALWAYSON_WITHOUT_TRUE_MSR = 0x00036dff
     *   没有 true-CTLS 时这些位必须为 1
     */
}

/* ================================================================
 *  IA32_VMX_ENTRY_CTLS (0x484) / TRUE 版本 (0x490)
 *  参考: Intel SDM Vol 3, Appendix A.5, Figure A-5
 *  源码: arch/x86/include/asm/vmx.h:111-122
 * ================================================================ */
static void decode_entry_ctls(uint64_t val)
{
    uint32_t allowed0 = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t allowed1 = (uint32_t)((val >> 32) & 0xFFFFFFFF);

    printf("  allowed-0 (must-be-1):  0x%08x\n", allowed0);
    printf("  allowed-1 (can-be-1):   0x%08x\n", allowed1);

    struct { int bit; const char *name; } bits[] = {
        {  2, "LOAD_DEBUG_CONTROLS"     },
        {  9, "IA32E_MODE"              },
        { 10, "SMM"                     },
        { 11, "DEACT_DUAL_MONITOR"      },
        { 13, "LOAD_IA32_PERF_GLOBAL_CTRL" },
        { 14, "LOAD_IA32_PAT"           },
        { 15, "LOAD_IA32_EFER"          },
        { 16, "LOAD_BNDCFGS"            },
        { 17, "PT_CONCEAL_PIP"          },
        { 18, "LOAD_IA32_RTIT_CTL"      },
    };

    printf("\n  Bit  Req   Opt  Name\n");
    printf("  ───  ───   ───  ─────────────────────────────\n");
    for (size_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        uint32_t mask = 1U << bits[i].bit;
        const char *req = (allowed0 & mask) ? "MUST" : "    ";
        const char *opt = (allowed1 & mask) ? "can " : "    ";
        printf("  %2d   %s   %s   %s\n",
               bits[i].bit, req, opt, bits[i].name);
    }

    /*
     * KVM 必须: IA32E_MODE (9) — 64-bit Guest
     * KVM 可选: LOAD_EFER (15), LOAD_PAT (14),
     *           LOAD_PERF_GLOBAL_CTRL (13), LOAD_BNDCFGS (16)
     *
     * 注意: VM_ENTRY_ALWAYSON_WITHOUT_TRUE_MSR = 0x000011ff
     */
}

/* ================================================================
 *  IA32_VMX_PROCBASED_CTLS2 (0x48B)
 *  参考: Intel SDM Vol 3, Appendix A.3.3, Figure A-6
 *  源码: arch/x86/include/asm/vmx.h:56-81
 * ================================================================ */
static void decode_procbased2(uint64_t val)
{
    uint32_t allowed0 = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t allowed1 = (uint32_t)((val >> 32) & 0xFFFFFFFF);

    printf("  allowed-0 (must-be-1):  0x%08x\n", allowed0);
    printf("  allowed-1 (can-be-1):   0x%08x\n", allowed1);

    struct { int bit; const char *name; } bits[] = {
        {  0, "VIRT_APIC_ACCESSES"      },
        {  1, "ENABLE_EPT"              },
        {  2, "DESC_EXITING"            },
        {  3, "ENABLE_RDTSCP"           },
        {  4, "VIRTUAL_X2APIC_MODE"     },
        {  5, "ENABLE_VPID"             },
        {  6, "WBINVD_EXITING"          },
        {  7, "UNRESTRICTED_GUEST"      },
        {  8, "APIC_REGISTER_VIRT"      },
        {  9, "VIRTUAL_INTR_DELIVERY"   },
        { 10, "PAUSE_LOOP_EXITING"      },
        { 11, "RDRAND_EXITING"          },
        { 12, "ENABLE_INVPCID"          },
        { 13, "ENABLE_VMFUNC"           },
        { 14, "SHADOW_VMCS"             },
        { 15, "ENCLS_EXITING"           },
        { 16, "RDSEED_EXITING"          },
        { 17, "ENABLE_PML"              },
        { 18, "EPT_VIOLATION_VE"        },
        { 19, "PT_CONCEAL_VMX"          },
        { 20, "ENABLE_XSAVES"           },
        { 22, "MODE_BASED_EPT_EXEC"     },
        { 24, "PT_USE_GPA"             },
        { 25, "TSC_SCALING"             },
        { 26, "ENABLE_USR_WAIT_PAUSE"   },
        { 28, "BUS_LOCK_DETECTION"      },
        { 30, "NOTIFY_VM_EXITING"       },
    };

    printf("\n  Bit  Req   Opt  Name\n");
    printf("  ───  ───   ───  ───────────────────────────────────\n");
    for (size_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        uint32_t mask = 1U << bits[i].bit;
        const char *req = (allowed0 & mask) ? "MUST" : "    ";
        const char *opt = (allowed1 & mask) ? "can " : "    ";
        printf("  %2d   %s   %s   %s\n",
               bits[i].bit, req, opt, bits[i].name);
    }

    /*
     * 依赖链 (annotations.md §5):
     *   ENABLE_EPT (1) 要求 4-level + WB + INVEPT global
     *     └→ UNRESTRICTED_GUEST (7) 要求 ENABLE_EPT
     *
     *   VIRT_APIC_ACCESSES (0) 是 TPR shadow 的二级版本
     *     └→ APIC_REGISTER_VIRT (8) 要求 VIRT_APIC_ACCESSES
     *     └→ VIRTUAL_INTR_DELIVERY (9) 要求 VIRT_APIC_ACCESSES
     *
     *   ENABLE_VPID (5) 要求 INVVPID
     *
     *   ENABLE_PML (17) 要求 ENABLE_EPT
     */
}

/* ================================================================
 *  IA32_VMX_EPT_VPID_CAP (0x48C)
 *  参考: Intel SDM Vol 3, Appendix A.10
 *  源码: arch/x86/include/asm/vmxfeatures.h:24-30
 * ================================================================ */
static void decode_ept_vpid_cap(uint64_t val)
{
    printf("  Value: 0x%016lx\n\n", (unsigned long)val);

    /* EPT 能力 (bits 0-19) */
    printf("  === EPT Capabilities ===\n");
    struct { int bit; const char *name; const char *note; } ept[] = {
        {  0, "Execute-only EPT translations",     "4-level execute-only" },
        {  6, "4-level page walk for EPT",         "KVM 必须" },
        {  7, "Paging-write EPT (dirty tracking)", "EPT A/D bits" },
        {  8, "Paging-write EPT for GPA access",   "EPT accessed bit" },
        { 10, "EPT page-walk logging (PML)",       "4-level" },
        { 16, "2MB EPT pages",                      "" },
        { 17, "1GB EPT pages",                      "" },
        { 20, "INVEPT instruction supported",       "" },
        { 21, "INVEPT single-context",              "" },
        { 22, "INVEPT all-context",                 "" },
        { 23, "INVEPT single-context-nointr",       "preserve indirect" },
        { 24, "INVEPT single-context-global",       "w/ global invalid." },
        { 25, "5-level EPT page walk",              "" },
    };

    for (size_t i = 0; i < sizeof(ept)/sizeof(ept[0]); i++) {
        printf("  bit %2d: %s %s %s\n",
               ept[i].bit, (val & (1ULL << ept[i].bit)) ? "Y" : "N",
               ept[i].name, ept[i].note);
    }

    /* VPID 能力 (bits 32-47) */
    printf("\n  === VPID Capabilities ===\n");
    struct { int bit; const char *name; } vpid[] = {
        { 32, "INVVPID instruction supported"   },
        { 33, "INVVPID individual-address"       },
        { 34, "INVVPID single-context"           },
        { 35, "INVVPID all-context"              },
        { 36, "INVVPID single-context-retaining-globals" },
    };

    for (size_t i = 0; i < sizeof(vpid)/sizeof(vpid[0]); i++) {
        printf("  bit %2d: %s %s\n",
               vpid[i].bit, (val & (1ULL << vpid[i].bit)) ? "Y" : "N",
               vpid[i].name);
    }

    /*
     * KVM EPT 启用条件 (vmx.c:8414):
     *   cpu_has_vmx_ept()         → bit 1 (execute-only, 用于 shadow VMCS)
     *     实际上: 要求 bit 6 (4-level) + bit 14 (WB) + bit 22 (INVEPT all)
     *   cpu_has_vmx_ept_4levels() → bit 6
     *   cpu_has_vmx_ept_mt_wb()   → bit 14 (write-back memory type)
     *   cpu_has_vmx_invept_global()→ bit 24 or 22 (all-context)
     *
     * KVM VPID 启用条件:
     *   cpu_has_vmx_invvpid()      → bit 32
     *     且 (bit 33 或 bit 35) 必须有一个支持
     */
}

/* ================================================================
 *  MSR 读取辅助函数
 * ================================================================ */
static int read_msr(int fd, uint32_t msr, uint64_t *value)
{
    off_t offset = msr;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
        return -1;
    if (read(fd, value, 8) != 8)
        return -1;
    return 0;
}

static void print_separator(void)
{
    printf("  ──────────────────────────────────────────────────\n");
}

int main(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint64_t val;

    printf("================================================================\n");
    printf("  Exercise 1: VMX Capability 深度解码\n");
    printf("  连接: ../annotations.md §5「VMX 特性检测」\n");
    printf("  源码: arch/x86/kvm/vmx/vmx.c:2590 — setup_vmcs_config()\n");
    printf("================================================================\n\n");

    /* Step 1: CPUID leaf 1 — 检查 VMX 支持 */
    printf("[1] CPUID leaf 1 — 检查 VMX 支持\n");
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    printf("  ECX = 0x%08x\n", ecx);

    if (ecx & (1 << 5)) {
        printf("  ✓ VMX (VT-x) supported (ECX.VMX = 1)\n");
    } else {
        printf("  ✗ VMX not supported — 无法继续\n");
        return 1;
    }
    if (ecx & (1 << 7))
        printf("  ✓ TPR Shadow (FlexPriority) supported (ECX bit 7)\n");
    printf("\n");

    /* Step 2: CPUID leaf 7 — 检查 CPUID Faulting */
    printf("[2] CPUID leaf 7 subleaf 0 — 检查 CPUID Faulting\n");
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(7), "c"(0));
    if (ecx & (1 << 3))
        printf("  ✓ CPUID Faulting supported (ECX bit 3)\n");
    else
        printf("  ✗ CPUID Faulting not supported\n");
    printf("\n");

    /* Step 3: 打开 MSR 设备 */
    int msr_fd = open("/dev/cpu/0/msr", O_RDONLY);
    if (msr_fd < 0) {
        printf("✗ 无法打开 /dev/cpu/0/msr\n");
        printf("  提示: modprobe msr\n");
        return 1;
    }

    /* Step 4: IA32_VMX_BASIC (0x480) */
    printf("[3] IA32_VMX_BASIC (MSR 0x480)\n");
    printf("  源码: vmx.c:2648 — 首先读取\n");
    print_separator();
    if (read_msr(msr_fd, 0x480, &val) == 0) {
        printf("  Raw value: 0x%016lx\n", (unsigned long)val);
        decode_vmx_basic(val);
    } else {
        printf("  ✗ 读取失败\n");
    }
    printf("\n");

    /* Step 5: IA32_VMX_PINBASED_CTLS (0x481) */
    printf("[4] IA32_VMX_PINBASED_CTLS (MSR 0x481)\n");
    printf("  源码: vmx.c:2690 — adjust_vmx_controls()\n");
    printf("  规范: SDM Vol 3, Appendix A.3.1\n");
    print_separator();
    if (read_msr(msr_fd, 0x481, &val) == 0) {
        decode_pinbased(val);
    } else {
        printf("  ✗ 读取失败\n");
    }
    printf("\n");

    /* Step 6: IA32_VMX_PROCBASED_CTLS (0x482) */
    printf("[5] IA32_VMX_PROCBASED_CTLS (MSR 0x482)\n");
    printf("  源码: vmx.c:2621 — adjust_vmx_controls()\n");
    printf("  规范: SDM Vol 3, Appendix A.3.2\n");
    print_separator();
    if (read_msr(msr_fd, 0x482, &val) == 0) {
        decode_procbased(val);
    } else {
        printf("  ✗ 读取失败\n");
    }
    printf("\n");

    /* Step 7: IA32_VMX_EXIT_CTLS (0x483) */
    printf("[6] IA32_VMX_EXIT_CTLS (MSR 0x483)\n");
    printf("  源码: vmx.c:2675\n");
    printf("  规范: SDM Vol 3, Appendix A.4\n");
    print_separator();
    if (read_msr(msr_fd, 0x483, &val) == 0) {
        decode_exit_ctls(val);
    } else {
        printf("  ✗ 读取失败\n");
    }
    printf("\n");

    /* Step 8: IA32_VMX_ENTRY_CTLS (0x484) */
    printf("[7] IA32_VMX_ENTRY_CTLS (MSR 0x484)\n");
    printf("  源码: vmx.c:2680\n");
    printf("  规范: SDM Vol 3, Appendix A.5\n");
    print_separator();
    if (read_msr(msr_fd, 0x484, &val) == 0) {
        decode_entry_ctls(val);
    } else {
        printf("  ✗ 读取失败\n");
    }
    printf("\n");

    /* Step 9: IA32_VMX_PROCBASED_CTLS2 (0x48B) */
    printf("[8] IA32_VMX_PROCBASED_CTLS2 (MSR 0x48B)\n");
    printf("  源码: vmx.c:2627 — 二级控制\n");
    printf("  规范: SDM Vol 3, Appendix A.3.3\n");
    print_separator();
    if (read_msr(msr_fd, 0x48B, &val) == 0) {
        decode_procbased2(val);
    } else {
        printf("  ✗ 读取失败\n");
    }
    printf("\n");

    /* Step 10: IA32_VMX_EPT_VPID_CAP (0x48C) */
    printf("[9] IA32_VMX_EPT_VPID_CAP (MSR 0x48C)\n");
    printf("  源码: vmx.c:2648 — rdmsr_safe()\n");
    printf("  规范: SDM Vol 3, Appendix A.10\n");
    print_separator();
    if (read_msr(msr_fd, 0x48C, &val) == 0) {
        decode_ept_vpid_cap(val);
    } else {
        printf("  ✗ 读取失败\n");
    }
    printf("\n");

    close(msr_fd);

    /* Summary */
    printf("================================================================\n");
    printf("  特性依赖链总结\n");
    printf("  参考: annotations.md §5\n");
    printf("================================================================\n\n");

    printf("  硬件 capability MSR\n");
    printf("      │\n");
    printf("      ├─ EPT (PROCBASED2 bit 1)\n");
    printf("      │   要求: EPT_VPID_CAP bit 6 (4-level) + bit 14 (WB)\n");
    printf("      │         + bit 22 or 24 (INVEPT all-context)\n");
    printf("      │   └─ EPT_AD (EPT_VPID_CAP bit 7)\n");
    printf("      │       └─ unrestricted_guest (PROCBASED2 bit 7)\n");
    printf("      │           要求: EPT enabled\n");
    printf("      │\n");
    printf("      ├─ VPID (PROCBASED2 bit 5)\n");
    printf("      │   要求: EPT_VPID_CAP bit 32 + (bit 33 or bit 35)\n");
    printf("      │\n");
    printf("      ├─ APICv\n");
    printf("      │   要求: VIRTUAL_TPR (PROCBASED bit 21) +\n");
    printf("      │         VIRT_APIC_ACCESSES (PROCBASED2 bit 0) +\n");
    printf("      │         APIC_REGISTER_VIRT (PROCBASED2 bit 8) +\n");
    printf("      │         VIRTUAL_INTR_DELIVERY (PROCBASED2 bit 9)\n");
    printf("      │   └─ Posted Interrupts (PINBASED bit 7)\n");
    printf("      │       要求: APICv enabled\n");
    printf("      │\n");
    printf("      └─ FlexPriority\n");
    printf("          要求: VIRTUAL_TPR (PROCBASED bit 21)\n");
    printf("\n");

    printf("================================================================\n");
    printf("  ✓ VMX Capability 深度解码完成\n");
    printf("================================================================\n");

    return 0;
}
