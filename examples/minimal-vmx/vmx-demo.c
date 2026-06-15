// SPDX-License-Identifier: GPL-2.0
/*
 * vmx-demo.c - 最小化VMX演示内核模块 (简化版)
 *
 * 本模块演示 VMXON → VMCLEAR → VMPTRLD → VMCS配置 的完整流程。
 * 不包含 VMLAUNCH (硬件入口)，因为那需要非常精确的Guest状态。
 * 重点在于理解 VMCS 的结构和配置过程。
 *
 * 对应KVM源码:
 *   arch/x86/kvm/vmx/vmx.c::vmx_hardware_setup() - 初始化
 *   arch/x86/kvm/vmx/vmx.c::vmx_vcpu_create()     - VMCS分配
 *
 * 用法:
 *   sudo rmmod kvm_intel kvm       # 先卸载KVM
 *   sudo insmod vmx-demo.ko
 *   dmesg | tail -60
 *   sudo rmmod vmx-demo
 *   sudo modprobe kvm_intel        # 恢复KVM
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/msr.h>
#include <asm/msr-index.h>
#include <asm/cpufeature.h>
#include <asm/desc.h>
#include <asm/processor.h>
#include <asm/io.h>

/* VMX MSR定义 - 已在asm/msr-index.h中定义，这里只补充未定义的 */
/* MSR_IA32_VMX_BASIC = 0x480 等已在内核头文件中定义 */

/* VMCS字段编码 (Intel SDM Vol.3 Appendix B) */
#define GUEST_ES_SELECTOR       0x0800
#define GUEST_CS_SELECTOR       0x0802
#define GUEST_SS_SELECTOR       0x0804
#define GUEST_DS_SELECTOR       0x0806
#define GUEST_FS_SELECTOR       0x0808
#define GUEST_GS_SELECTOR       0x080A
#define GUEST_CS_BASE           0x6808
#define GUEST_CS_LIMIT          0x4802
#define GUEST_CS_AR_BYTES       0x4816
#define GUEST_RIP               0x681E
#define GUEST_RSP               0x681C
#define GUEST_RFLAGS            0x6820
#define GUEST_CR0               0x6800
#define GUEST_CR3               0x6802
#define GUEST_CR4               0x6804
#define HOST_RIP                0x6C16
#define HOST_RSP                0x6C14
#define HOST_CR0                0x6C00
#define HOST_CR3                0x6C02
#define HOST_CR4                0x6C04
#define VM_ENTRY_CONTROLS       0x4012
#define VM_EXIT_CONTROLS        0x400C
#define PIN_BASED_VM_EXEC_CTL   0x4000
#define CPU_BASED_VM_EXEC_CTL   0x4002
#define EXCEPTION_BITMAP        0x4004

MODULE_AUTHOR("KVM Study Project");
MODULE_DESCRIPTION("Minimal VMX Demo - VMCS Configuration");
MODULE_LICENSE("GPL");

static void *vmxon_region;
static void *vmcs_region;
static u64 vmxon_phys;
static u64 vmcs_phys;
static u32 vmcs_revision_id;

/* === VMX指令封装 === */

static inline int do_vmxon(u64 phys)
{
    u8 error;
    asm volatile("vmxon %1; setna %0" : "=g"(error) : "m"(phys) : "cc");
    return error;
}

static inline void do_vmxoff(void)
{
    asm volatile("vmxoff" ::: "cc");
}

static inline int do_vmclear(u64 phys)
{
    u8 error;
    asm volatile("vmclear %1; setna %0" : "=g"(error) : "m"(phys) : "cc");
    return error;
}

static inline int do_vmptrld(u64 phys)
{
    u8 error;
    asm volatile("vmptrld %1; setna %0" : "=g"(error) : "m"(phys) : "cc");
    return error;
}

static inline void do_vmwrite(unsigned long field, unsigned long value)
{
    u8 error;
    asm volatile("vmwrite %2, %1; setna %0"
                 : "=g"(error) : "r"(value), "r"(field) : "cc");
    if (error)
        pr_warn("  vmwrite(0x%lx, 0x%lx) 失败\n", field, value);
}

static inline unsigned long do_vmread(unsigned long field)
{
    unsigned long value;
    asm volatile("vmread %1, %0" : "=r"(value) : "r"(field) : "cc");
    return value;
}

/*
 * 配置Guest状态 - 设置为实模式最小状态
 *
 * 对比KVM: vmx_vcpu_reset() → vmx_set_constant_guest_state()
 *   KVM在这里设置了完整的段寄存器、控制寄存器、RIP等
 *   我们只设置最小化的实模式状态
 */
static void setup_guest_state(void)
{
    pr_info("\n--- 配置 Guest State (实模式) ---\n");

    /* 段选择子 - 全部为0 (实模式典型值) */
    do_vmwrite(GUEST_CS_SELECTOR, 0x0000);
    do_vmwrite(GUEST_CS_BASE, 0x00000000);
    do_vmwrite(GUEST_CS_LIMIT, 0xFFFF);
    do_vmwrite(GUEST_CS_AR_BYTES, 0x0093);  /* Present, Code, RW, type=3 */

    do_vmwrite(GUEST_DS_SELECTOR, 0x0000);
    do_vmwrite(GUEST_ES_SELECTOR, 0x0000);
    do_vmwrite(GUEST_FS_SELECTOR, 0x0000);
    do_vmwrite(GUEST_GS_SELECTOR, 0x0000);
    do_vmwrite(GUEST_SS_SELECTOR, 0x0000);

    /* Guest RIP/RSP/RFLAGS */
    do_vmwrite(GUEST_RIP, 0x1000);
    do_vmwrite(GUEST_RSP, 0x7C00);
    do_vmwrite(GUEST_RFLAGS, 0x2);  /* 只有bit 1为1 (reserved) */

    /* 控制寄存器 - 实模式最小配置 */
    do_vmwrite(GUEST_CR0, 0x00000030);  /* NE=1, ET=1 */
    do_vmwrite(GUEST_CR3, 0);
    do_vmwrite(GUEST_CR4, 0);

    pr_info("  GUEST_RIP    = 0x%lx  (Guest代码入口)\n", do_vmread(GUEST_RIP));
    pr_info("  GUEST_RSP    = 0x%lx  (栈指针)\n", do_vmread(GUEST_RSP));
    pr_info("  GUEST_CR0    = 0x%lx  (NE+ET)\n", do_vmread(GUEST_CR0));
    pr_info("  GUEST_CS.sel = 0x%lx\n", do_vmread(GUEST_CS_SELECTOR));
    pr_info("  GUEST_CS.ar  = 0x%lx  (Present+Code+RW)\n",
            do_vmread(GUEST_CS_AR_BYTES));
}

/*
 * 配置Host状态 - VM-Exit后恢复
 *
 * 对比KVM: vmx_vcpu_run() 中设置 HOST_RSP/HOST_RIP 等
 *   KVM在每次VM-Entry前更新HOST_CR3 (因为内核可能切换PCID)
 */
static void setup_host_state(unsigned long host_rip)
{
    unsigned long cr0, cr4;

    pr_info("\n--- 配置 Host State ---\n");

    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    do_vmwrite(HOST_CR0, cr0);
    do_vmwrite(HOST_CR4, cr4);
    do_vmwrite(HOST_CR3, 0);
    do_vmwrite(HOST_RSP, (unsigned long)vmxon_region + 4096 - 64);
    do_vmwrite(HOST_RIP, host_rip);

    pr_info("  HOST_CR0 = 0x%lx\n", cr0);
    pr_info("  HOST_CR4 = 0x%lx\n", cr4);
    pr_info("  HOST_RSP = 0x%lx\n",
            (unsigned long)vmxon_region + 4096 - 64);
    pr_info("  HOST_RIP = 0x%lx\n", host_rip);
}

/*
 * 配置VM执行控制
 *
 * 对比KVM: setup_vmcs_config()
 *   KVM在这里配置了大量的执行控制位:
 *   - PIN_BASED: 外部中断/NMI/Posted Interrupts
 *   - CPU_BASED: MSR bitmap/IO bitmap/TPR Shadow等
 *   - VM_ENTRY: 进入64位模式/加载EFER等
 *   - VM_EXIT: 离开64位模式/保存EFER等
 */
static void setup_exec_controls(void)
{
    u64 pinbased, procbased, entry_ctls, exit_ctls;

    pr_info("\n--- 配置 VM Execution Controls ---\n");

    /* 读取MSR获取 allowed-0 值 (必须为1的位) */
    rdmsrl_safe(0x481, &pinbased);
    rdmsrl_safe(0x482, &procbased);
    rdmsrl_safe(0x484, &entry_ctls);
    rdmsrl_safe(0x483, &exit_ctls);

    /* 只使用 required bits (allowed-0 的低位32位) */
    do_vmwrite(PIN_BASED_VM_EXEC_CTL, (u32)pinbased);
    do_vmwrite(CPU_BASED_VM_EXEC_CTL, (u32)procbased);
    do_vmwrite(VM_ENTRY_CONTROLS, (u32)entry_ctls);
    do_vmwrite(VM_EXIT_CONTROLS, (u32)exit_ctls);
    do_vmwrite(EXCEPTION_BITMAP, 0);

    pr_info("  PIN_BASED  = 0x%08x  (required: ext_intr=%d, nmi=%d, posted_int=%d)\n",
            (u32)pinbased,
            (u32)pinbased & 1,
            ((u32)pinbased >> 3) & 1,
            ((u32)pinbased >> 7) & 1);
    pr_info("  PROC_BASED = 0x%08x  (required: hlt_exit=%d, int_window=%d)\n",
            (u32)procbased,
            (u32)procbased & 1,
            ((u32)procbased >> 2) & 1);
    pr_info("  ENTRY_CTL  = 0x%08x\n", (u32)entry_ctls);
    pr_info("  EXIT_CTL   = 0x%08x\n", (u32)exit_ctls);
}

/* Host恢复点 - 这是一个占位函数 */
static noinline void vmx_host_resume(void)
{
    /* 如果VMLAUNCH成功且发生VM-Exit，CPU会跳到这里 */
    pr_info("  ← Host恢复点 (VM-Exit后执行到这里)\n");
}

static int __init vmx_demo_init(void)
{
    u64 basic_msr, cr4, cr0_fixed0, cr0_fixed1;
    unsigned long cr4_val;
    int ret;

    pr_info("================================================\n");
    pr_info(" Minimal VMX Demo (简化版)\n");
    pr_info("================================================\n");

    /* 1. 检查VMX */
    if (!boot_cpu_has(X86_FEATURE_VMX)) {
        pr_err("CPU不支持VMX!\n");
        return -ENODEV;
    }
    pr_info("✓ CPU支持VMX\n");

    /* 2. 读取VMCS revision ID */
    rdmsrl_safe(MSR_IA32_VMX_BASIC, &basic_msr);
    vmcs_revision_id = basic_msr & 0x7FFFFFFF;
    pr_info("  VMCS Revision ID = %u\n", vmcs_revision_id);
    pr_info("  VMXON Region Size = %llu bytes\n",
            (basic_msr >> 32) & 0x1FFF);

    /* 3. 启用VMXE (CR4 bit 13) */
    asm volatile("mov %%cr4, %0" : "=r"(cr4_val));
    if (!(cr4_val & (1UL << 13))) {
        cr4_val |= (1UL << 13);
        asm volatile("mov %0, %%cr4" :: "r"(cr4_val));
        pr_info("✓ 启用 CR4.VMXE (bit 13)\n");
    }

    /* 4. 调整CR0满足VMX要求 */
    rdmsrl_safe(MSR_IA32_VMX_CR0_FIXED0, &cr0_fixed0);
    rdmsrl_safe(MSR_IA32_VMX_CR0_FIXED1, &cr0_fixed1);
    {
        unsigned long cur_cr0;
        asm volatile("mov %%cr0, %0" : "=r"(cur_cr0));
        cur_cr0 |= cr0_fixed0;  /* 必须为1的位 */
        cur_cr0 &= cr0_fixed1;  /* 必须为0的位 */
        asm volatile("mov %0, %%cr0" :: "r"(cur_cr0));
    }
    pr_info("✓ 调整 CR0 满足VMX要求\n");

    /* 5. 分配VMXON和VMCS区域 (4K对齐) */
    vmxon_region = (void *)__get_free_page(GFP_KERNEL);
    vmcs_region = (void *)__get_free_page(GFP_KERNEL);
    if (!vmxon_region || !vmcs_region) {
        pr_err("内存分配失败\n");
        return -ENOMEM;
    }
    vmxon_phys = virt_to_phys(vmxon_region);
    vmcs_phys = virt_to_phys(vmcs_region);

    /* 设置VMCS revision ID (前4字节) */
    *(u32 *)vmxon_region = vmcs_revision_id;
    *(u32 *)vmcs_region = vmcs_revision_id;

    pr_info("  VMXON: virt=%p phys=0x%llx\n", vmxon_region, vmxon_phys);
    pr_info("  VMCS:  virt=%p phys=0x%llx\n", vmcs_region, vmcs_phys);

    /* 6. VMXON - 进入VMX Root模式 */
    pr_info("\n--- 执行 VMXON ---\n");
    ret = do_vmxon(vmxon_phys);
    if (ret) {
        pr_err("✗ VMXON 失败! (确保已卸载KVM: rmmod kvm_intel kvm)\n");
        goto err_free;
    }
    pr_info("✓ VMXON 成功 → 现在在 VMX Root Mode\n");

    /* 7. VMCLEAR + VMPTRLD */
    pr_info("\n--- VMCLEAR + VMPTRLD ---\n");
    do_vmclear(vmcs_phys);
    pr_info("✓ VMCLEAR 完成 (VMCS标记为非活跃)\n");

    ret = do_vmptrld(vmcs_phys);
    if (ret) {
        pr_err("✗ VMPTRLD 失败!\n");
        goto err_vmxoff;
    }
    pr_info("✓ VMPTRLD 完成 (VMCS已加载为当前活跃)\n");

    /* 8. 配置VMCS */
    setup_host_state((unsigned long)vmx_host_resume);
    setup_guest_state();
    setup_exec_controls();

    /*
     * 注意: 这里我们不执行VMLAUNCH。
     * 完整的VMLAUNCH需要非常精确的Guest状态配置:
     * - 所有段寄存器的AR字节必须匹配
     * - CR0/CR4的fixed bits必须满足
     * - 需要有效的Guest代码页
     * - 需要配置EPT (如果使用unrestricted guest)
     *
     * 对比KVM: vmx_vcpu_run() → __vmx_vcpu_run() [vmenter.S]
     *   KVM在VM-Entry前做了大量的VMCS字段同步工作。
     *
     * 如果你想尝试VMLAUNCH, 需要:
     * 1. 分配一个物理页作为Guest代码区域
     * 2. 在该页写入简单指令 (如: f4 = hlt)
     * 3. 配置EPT映射该GPA到HPA
     * 4. 设置HOST_RIP指向一个有效的恢复点
     * 5. 执行 asm volatile("vmlaunch")
     */

    pr_info("\n================================================\n");
    pr_info(" VMCS 配置完成 (未执行VMLAUNCH)\n");
    pr_info("================================================\n");
    pr_info("已完成的操作:\n");
    pr_info("  1. VMXON    → 进入 VMX Root Mode\n");
    pr_info("  2. VMCLEAR  → 清除VMCS状态\n");
    pr_info("  3. VMPTRLD  → 加载VMCS为当前活跃\n");
    pr_info("  4. VMCS写入 → Host State, Guest State, Exec Controls\n");
    pr_info("\n对比KVM源码:\n");
    pr_info("  vmx_hardware_setup() → 本步骤1 (VMXON)\n");
    pr_info("  vmx_vcpu_create()    → 本步骤2-3 (VMCS分配)\n");
    pr_info("  vmx_vcpu_reset()     → 本步骤4 (Guest State)\n");
    pr_info("  vmx_vcpu_run()       → 本步骤4+VMLAUNCH (完整路径)\n");
    pr_info("  vmx_handle_exit()    → VM-Exit后分发处理\n");

    /* 9. VMXOFF */
    pr_info("\n--- 执行 VMXOFF ---\n");
    do_vmxoff();
    pr_info("✓ VMXOFF 完成 → 回到正常模式\n");

    free_page((unsigned long)vmcs_region);
    free_page((unsigned long)vmxon_region);
    return 0;

err_vmxoff:
    do_vmxoff();
err_free:
    free_page((unsigned long)vmcs_region);
    free_page((unsigned long)vmxon_region);
    return -EIO;
}

static void __exit vmx_demo_exit(void)
{
    pr_info("vmx-demo: 模块卸载\n");
}

module_init(vmx_demo_init);
module_exit(vmx_demo_exit);
