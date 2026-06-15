// SPDX-License-Identifier: GPL-2.0
/*
 * vmx-info.c - VMX能力检测内核模块 (安全版本)
 *
 * 本模块只读取VMX相关的MSR，报告CPU的虚拟化能力。
 * 不会执行VMXON或VMLAUNCH，因此是安全的。
 *
 * 用法:
 *   make -f Makefile.info
 *   sudo insmod vmx-info.ko
 *   dmesg | tail -30
 *   sudo rmmod vmx-info
 *
 * 对应KVM源码: arch/x86/kvm/vmx/vmx.c::vmx_hardware_setup()
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cpufeature.h>
#include <asm/msr.h>
#include <asm/cpufeature.h>

/* VMX MSR 定义 (Intel SDM Vol.3 Appendix A) */
#define MSR_IA32_VMX_BASIC              0x480
#define MSR_IA32_VMX_PINBASED_CTLS      0x481
#define MSR_IA32_VMX_PROCBASED_CTLS     0x482
#define MSR_IA32_VMX_EXIT_CTLS          0x483
#define MSR_IA32_VMX_ENTRY_CTLS         0x484
#define MSR_IA32_VMX_MISC               0x485
#define MSR_IA32_VMX_CR0_FIXED0         0x486
#define MSR_IA32_VMX_CR0_FIXED1         0x487
#define MSR_IA32_VMX_CR4_FIXED0         0x488
#define MSR_IA32_VMX_CR4_FIXED1         0x489
#define MSR_IA32_VMX_EPT_VPID_CAP     0x48C

MODULE_AUTHOR("KVM Study Project");
MODULE_DESCRIPTION("VMX Capability MSR Reader (Safe - no VMXON)");
MODULE_LICENSE("GPL");

/*
 * 读取MSR并打印其值。
 * 对应KVM源码中setup_vmcs_config()的MSR读取逻辑。
 */
static void read_and_print_msr(u32 msr, const char *name)
{
    u64 val;
    int ret;

    ret = rdmsrl_safe(msr, &val);
    if (ret) {
        pr_info("  %-35s : NOT AVAILABLE (rdmsr failed)\n", name);
        return;
    }

    pr_info("  %-35s : 0x%016llx (low=0x%08x high=0x%08x)\n",
            name, val, (u32)val, (u32)(val >> 32));
}

/*
 * 解析IA32_VMX_BASIC MSR (0x480)
 *
 * 格式 (Intel SDM Vol.3 A.1):
 *   [30:0]  - VMCS revision ID
 *   [31]    - reserved
 *   [44:32] - VMXON region size (bytes)
 *   [48:45] - reserved
 *   [49]    - true controls (如果使用,则用TRUE_CTLS MSR)
 *   [53:50] - reserved
 *   [54]    - VMX functionality (ins/outs for VMX controls)
 *   [63:55] - reserved
 */
static void decode_vmx_basic(void)
{
    u64 val;
    u32 revision_id;
    u32 region_size;
    bool true_controls;

    if (rdmsrl_safe(MSR_IA32_VMX_BASIC, &val)) {
        pr_info("  IA32_VMX_BASIC: NOT AVAILABLE\n");
        return;
    }

    revision_id = val & 0x7FFFFFFF;
    region_size = (val >> 32) & 0x1FFF;
    true_controls = (val >> 55) & 1;

    pr_info("\n  [解码 IA32_VMX_BASIC]\n");
    pr_info("    VMCS Revision ID    : %u (用于VMCS前4字节)\n", revision_id);
    pr_info("    VMXON Region Size   : %u bytes\n", region_size);
    pr_info("    True Controls       : %s\n",
            true_controls ? "YES (使用TRUE_CTLS MSR)" : "NO");
    pr_info("    → KVM使用此revision ID初始化每个VMCS的前4字节\n");
    pr_info("    → 对应代码: setup_vmcs_config() 中的 vmcs_config.revision_id\n");
}

/*
 * 解析IA32_VMX_EPT_VPID_CAP MSR (0x48C)
 *
 * 位定义 (Intel SDM Vol.3 A.10):
 *   [0]  - EPT支持
 *   [1]  - EPT页表walk长度4 (必须)
 *   [5]  - EPT memory type WB
 *   [6]  - EPT超级页 (2MB)
 *   [7]  - EPT超级页 (1GB)
 *   [8]  - INVEPT
 *   [20] - EPT A/D位
 *   [25] - PML (Page Modification Logging)
 *   [26] - VPID支持
 *   [32] - INVVPID individual address
 *   [33] - INVVPID single context
 *   [34] - INVVPID all contexts
 *   [35] - INVVPID single context + global
 */
static void decode_ept_vpid_cap(void)
{
    u64 val;

    if (rdmsrl_safe(MSR_IA32_VMX_EPT_VPID_CAP, &val)) {
        pr_info("  IA32_VMX_EPT_VPID_CAP: NOT AVAILABLE\n");
        return;
    }

    pr_info("\n  [解码 IA32_VMX_EPT_VPID_CAP] = 0x%016llx\n", val);

#define CHECK_BIT(bit, name, kvm_note) \
    pr_info("    [%2d] %-35s : %s  %s\n", bit, name, \
            (val >> bit) & 1 ? "YES" : " NO", kvm_note)

    CHECK_BIT(0,  "EPT supported", "← enable_ept=1的前提");
    CHECK_BIT(1,  "EPT page-walk length 4", "← 4级页表(必须)");
    CHECK_BIT(5,  "EPT memory type WB", "← Write-Back内存类型");
    CHECK_BIT(6,  "EPT 2MB pages", "← 大页支持");
    CHECK_BIT(7,  "EPT 1GB pages", "← 超大页支持");
    CHECK_BIT(8,  "INVEPT supported", "← EPT TLB刷新");
    CHECK_BIT(20, "EPT A/D bits", "← enable_ept_ad_bits=1的前提");
    CHECK_BIT(25, "PML (Page Modification Logging)", "← enable_pml=1");
    CHECK_BIT(26, "VPID supported", "← enable_vpid=1的前提");
    CHECK_BIT(32, "INVVPID individual address", "");
    CHECK_BIT(33, "INVVPID single context", "");
    CHECK_BIT(34, "INVVPID all contexts", "");
    CHECK_BIT(35, "INVVPID single+global", "");

#undef CHECK_BIT

    pr_info("\n  → 对应代码: vmx_hardware_setup() 中的特性检测:\n");
    pr_info("    if (!cpu_has_vmx_ept() || !cpu_has_vmx_ept_4levels() ||\n");
    pr_info("        !cpu_has_vmx_ept_mt_wb() || !cpu_has_vmx_invept_global())\n");
    pr_info("        enable_ept = 0;\n");
}

/*
 * 解析IA32_VMX_PINBASED_CTLS MSR (0x481)
 *
 * [31:0]  = allowed-0: 必须为1的控制位
 * [63:32] = allowed-1: 可以为1的控制位
 *
 * 只有 allowed-1 中为1的位才能被设为1。
 * allowed-0 中为1的位必须为1。
 */
static void decode_pinbased_ctls(void)
{
    u64 val;
    u32 allowed0, allowed1;

    if (rdmsrl_safe(MSR_IA32_VMX_PINBASED_CTLS, &val))
        return;

    allowed0 = (u32)val;
    allowed1 = (u32)(val >> 32);

    pr_info("\n  [解码 IA32_VMX_PINBASED_CTLS]\n");
    pr_info("    allowed-0 (必须为1) : 0x%08x\n", allowed0);
    pr_info("    allowed-1 (可以为1) : 0x%08x\n", allowed1);
    pr_info("    [2] External interrupt exiting : %s\n",
            (allowed1 >> 0) & 1 ? "SUPPORTED" : "NOT SUPPORTED");
    pr_info("    [3] NMI exiting                : %s\n",
            (allowed1 >> 3) & 1 ? "SUPPORTED" : "NOT SUPPORTED");
    pr_info("    [5] Virtual NMIs               : %s  ← enable_vnmi\n",
            (allowed1 >> 5) & 1 ? "SUPPORTED" : "NOT SUPPORTED");
    pr_info("    [6] Activate VMX-preemption    : %s\n",
            (allowed1 >> 6) & 1 ? "SUPPORTED" : "NOT SUPPORTED");
    pr_info("    [7] Process posted interrupts   : %s  ← enable_apicv\n",
            (allowed1 >> 7) & 1 ? "SUPPORTED" : "NOT SUPPORTED");
}

static int __init vmx_info_init(void)
{
    pr_info("============================================\n");
    pr_info("VMX Capability MSR Reader\n");
    pr_info("============================================\n");

    /* 1. 检查CPU是否有VMX */
    if (!boot_cpu_has(X86_FEATURE_VMX)) {
        pr_err("CPU不支持VMX (检查 /proc/cpuinfo 中的 vmx 标志)\n");
        return -ENODEV;
    }
    pr_info("\n✓ CPU支持VMX (X86_FEATURE_VMX)\n");

    /* 其他虚拟化特性 */
    pr_info("  EPT flag in CPUID : %s\n",
            boot_cpu_has(X86_FEATURE_EPT) ? "YES" : "NO");

    /* 2. 读取所有VMX MSR */
    pr_info("\n--- VMX Capability MSRs ---\n");
    read_and_print_msr(MSR_IA32_VMX_BASIC, "IA32_VMX_BASIC (0x480)");
    read_and_print_msr(MSR_IA32_VMX_PINBASED_CTLS, "IA32_VMX_PINBASED_CTLS (0x481)");
    read_and_print_msr(MSR_IA32_VMX_PROCBASED_CTLS, "IA32_VMX_PROCBASED_CTLS (0x482)");
    read_and_print_msr(MSR_IA32_VMX_EXIT_CTLS, "IA32_VMX_EXIT_CTLS (0x483)");
    read_and_print_msr(MSR_IA32_VMX_ENTRY_CTLS, "IA32_VMX_ENTRY_CTLS (0x484)");
    read_and_print_msr(MSR_IA32_VMX_MISC, "IA32_VMX_MISC (0x485)");
    read_and_print_msr(MSR_IA32_VMX_EPT_VPID_CAP, "IA32_VMX_EPT_VPID_CAP (0x48C)");

    /* 3. 解码关键MSR */
    decode_vmx_basic();
    decode_pinbased_ctls();
    decode_ept_vpid_cap();

    pr_info("\n--- CR固定值 ---\n");
    read_and_print_msr(MSR_IA32_VMX_CR0_FIXED0, "IA32_VMX_CR0_FIXED0 (0x486)");
    read_and_print_msr(MSR_IA32_VMX_CR0_FIXED1, "IA32_VMX_CR0_FIXED1 (0x487)");
    read_and_print_msr(MSR_IA32_VMX_CR4_FIXED0, "IA32_VMX_CR4_FIXED0 (0x488)");
    read_and_print_msr(MSR_IA32_VMX_CR4_FIXED1, "IA32_VMX_CR4_FIXED1 (0x489)");

    pr_info("\n============================================\n");
    pr_info("加载成功 (安全模式 - 未执行VMXON)\n");
    pr_info("============================================\n");

    return 0;
}

static void __exit vmx_info_exit(void)
{
    pr_info("vmx-info: 模块卸载\n");
}

module_init(vmx_info_init);
module_exit(vmx_info_exit);
