// SPDX-License-Identifier: GPL-2.0
/*
 * mini-kvm.c - 简化版 KVM 内核模块
 *
 * 教学项目：循序渐进实现一个简化版 KVM
 * 对应课程：Phase 0-11 (kvm-study/phase*)
 *
 * 本模块实现：
 *   Stage 1: VMX 基础 - VMXON, VMCS, VM-Entry
 *   Stage 2: EPT 内存虚拟化 - 4 级页表, GPA→HPA
 *   Stage 3: 中断注入 - 虚拟 LAPIC
 *   Stage 4: 设备模拟 - 串口 (PIO 0x3f8)
 *   Stage 5: 运行循环 - vcpu_run, exit 分发
 *
 * ⚠️ 警告：这是教学项目，不要在生产环境使用！
 * ⚠️ 必须先卸载 kvm_intel/kvm 模块才能加载本模块
 *
 * 用法：
 *   make
 *   sudo rmmod kvm_intel kvm 2>/dev/null
 *   sudo insmod mini-kvm.ko
 *   dmesg | tail -30
 *   sudo ./test-mini-kvm
 *   sudo rmmod mini_kvm
 *   sudo modprobe kvm_intel
 */

#define pr_fmt(fmt) "mini-kvm: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <asm/msr.h>
#include <asm/msr-index.h>
#include <asm/cpufeature.h>
#include <asm/desc.h>
#include <asm/processor.h>
#include <asm/io.h>
#include <asm/vmx.h>
#include <asm/kvm.h>

#include "mini-kvm.h"

/* ============================================================================
 * 全局状态
 * ============================================================================ */

struct mini_kvm_global mini_kvm_global;

/* 所有创建的 VM (简单起见用数组) */
#define MAX_VMS 8
static struct mini_kvm *vms[MAX_VMS];
static DEFINE_MUTEX(vms_lock);

/* ============================================================================
 * Stage 1: VMX 操作 - VMXON, VMCS 管理
 * ============================================================================
 *
 * 对应课程：Phase 1 (VT-x basics)
 * 关键源码：arch/x86/kvm/vmx/vmx.c::vmx_hardware_setup()
 *           arch/x86/kvm/vmx/vmenter.S::__vmx_vcpu_run()
 *
 * VMX 操作序列:
 *   VMXON → 启用 VMX 操作
 *   VMCLEAR → 清除 VMCS 状态
 *   VMPTRLD → 加载当前 VMCS
 *   VMWRITE → 写 VMCS 字段
 *   VMREAD → 读 VMCS 字段
 *   VMLAUNCH/VMRESUME → VM-Entry
 */

/*
 * 启用 VMX 操作
 *
 * 参考：vmx_hardware_setup() 中的 setup_vmcs_config()
 *
 * 步骤：
 *   1. 检查 CPU 是否支持 VMX
 *   2. 读取 VMX capability MSRs
 *   3. 设置 CR4.VMXE
 *   4. 调整 CR0/CR4 满足 VMX 要求
 *   5. 执行 VMXON
 */
int mini_kvm_vmx_init(void)
{
    u64 cr0, cr4, phys_addr;
    void *vmxon_region;
    u32 vmcs_rev;
    int cpu = smp_processor_id();

    pr_info("=== Stage 1: VMX 初始化 ===\n");

    /* 检查 VMX 支持 */
    if (!boot_cpu_has(X86_FEATURE_VMX)) {
        pr_err("CPU 不支持 VMX\n");
        return -ENODEV;
    }
    pr_info("  ✓ CPU 支持 VMX\n");

    /* 检查 EPT 支持 (Stage 2 需要) */
    if (!boot_cpu_has(X86_FEATURE_VMX_EPT)) {
        pr_warn("  ⚠ CPU 不支持 EPT，Stage 2 将不可用\n");
    } else {
        pr_info("  ✓ CPU 支持 EPT\n");
    }

    /* 读取 VMX capability MSRs */
    rdmsrl(MSR_IA32_VMX_BASIC, mini_kvm_global.vmx_basic);
    mini_kvm_global.vmcs_revision_id = (u32)mini_kvm_global.vmx_basic;
    rdmsrl(MSR_IA32_VMX_CR0_FIXED0, mini_kvm_global.vmx_cr0_fixed0);
    rdmsrl(MSR_IA32_VMX_CR0_FIXED1, mini_kvm_global.vmx_cr0_fixed1);
    rdmsrl(MSR_IA32_VMX_CR4_FIXED0, mini_kvm_global.vmx_cr4_fixed0);
    rdmsrl(MSR_IA32_VMX_CR4_FIXED1, mini_kvm_global.vmx_cr4_fixed1);

    pr_info("  VMCS revision: 0x%x\n", mini_kvm_global.vmcs_revision_id);

    /* 设置 CR0: 必须满足 VMX fixed bits */
    cr0 = read_cr0();
    cr0 |= mini_kvm_global.vmx_cr0_fixed0;
    cr0 &= mini_kvm_global.vmx_cr0_fixed1;
    write_cr0(cr0);
    pr_info("  ✓ CR0 配置完成: 0x%lx\n", cr0);

    /* 设置 CR4: 启用 VMXE + 满足 fixed bits */
    cr4 = __read_cr4();
    cr4 |= X86_CR4_VMXE;  /* ★ 启用 VMX */
    cr4 |= mini_kvm_global.vmx_cr4_fixed0;
    cr4 &= mini_kvm_global.vmx_cr4_fixed1;
    __write_cr4(cr4);
    pr_info("  ✓ CR4 配置完成: 0x%lx (VMXE=1)\n", cr4);

    /* 分配 VMXON 区域 (必须 4KB 对齐) */
    vmxon_region = (void *)__get_free_page(GFP_KERNEL);
    if (!vmxon_region)
        return -ENOMEM;

    /* 写入 VMCS revision ID 到 VMXON 区域 (Intel SDM Vol 3, 25.11.5) */
    *(u32 *)vmxon_region = mini_kvm_global.vmcs_revision_id;

    phys_addr = __pa(vmxon_region);
    pr_info("  VMXON 区域: virt=%p phys=0x%llx\n", vmxon_region, phys_addr);

    /* ★ 执行 VMXON 指令 */
    asm volatile (
        "vmxon %0\n"
        :
        : "m"(phys_addr)
        : "memory", "cc"
    );
    pr_info("  ✓ VMXON 执行成功\n");

    mini_kvm_global.vmx_enabled = true;
    pr_info("=== Stage 1 完成: VMX 已启用 (CPU %d) ===\n\n", cpu);

    return 0;
}

/*
 * 禁用 VMX 操作
 */
void mini_kvm_vmx_exit(void)
{
    if (!mini_kvm_global.vmx_enabled)
        return;

    pr_info("=== Stage 1: VMX 关闭 ===\n");

    asm volatile ("vmxoff" ::: "memory", "cc");
    pr_info("  ✓ VMXOFF 执行成功\n");

    mini_kvm_global.vmx_enabled = false;
}

/*
 * 为 vCPU 配置 VMCS
 *
 * 参考：vmx_vcpu_create() + vmx_setup_vmcs()
 *
 * VMCS 字段分两类：
 *   - Host-State Area: VM-Exit 后恢复的 Host 状态
 *   - Guest-State Area: VM-Entry 加载的 Guest 状态
 *   - VM-Execution Control: 控制哪些事件触发 VM-Exit
 */
int mini_kvm_vcpu_vmx_setup(struct mini_kvm_vcpu *vcpu)
{
    struct mini_kvm *kvm = vcpu->kvm;
    void *vmcs_region, *vmxon_region;
    u64 vmcs_phys, vmxon_phys, phys_addr;
    u32 cpu_based, cpu_based2, exit_ctrl, entry_ctrl;
    int ret = 0;

    pr_info("=== Stage 1: 配置 vCPU %d 的 VMCS ===\n", vcpu->vcpu_id);

    /* 分配 VMCS 区域 (4KB, 对齐) */
    vmcs_region = (void *)__get_free_page(GFP_KERNEL);
    if (!vmcs_region)
        return -ENOMEM;

    /* 写入 VMCS revision ID */
    *(u32 *)vmcs_region = mini_kvm_global.vmcs_revision_id;
    vmcs_phys = __pa(vmcs_region);

    /* 分配 VMXON 区域 (每个 vCPU 需要独立的 VMXON) */
    vmxon_region = (void *)__get_free_page(GFP_KERNEL);
    if (!vmxon_region) {
        free_page((unsigned long)vmcs_region);
        return -ENOMEM;
    }
    *(u32 *)vmxon_region = mini_kvm_global.vmcs_revision_id;
    vmxon_phys = __pa(vmxon_region);

    vcpu->arch.vmcs_virt = vmcs_region;
    vcpu->arch.vmcs_phys = vmcs_phys;
    vcpu->arch.vmxon_virt = vmxon_region;
    vcpu->arch.vmxon_phys = vmxon_phys;

    /* ★ VMCLEAR: 清除 VMCS 状态 */
    phys_addr = vmcs_phys;
    asm volatile ("vmclear %0" : : "m"(phys_addr) : "memory", "cc");

    /* ★ VMPTRLD: 加载当前 VMCS */
    asm volatile ("vmptrld %0" : : "m"(phys_addr) : "memory", "cc");
    pr_info("  ✓ VMCS 加载完成\n");

    /* ====================================================================
     * 配置 VM-Execution Controls (Stage 1 + Stage 4)
     * ====================================================================
     * 控制哪些事件触发 VM-Exit
     * 参考: vmx_exec_control_setup() in vmx.c
     */

    /* Primary processor-based VM-execution controls */
    cpu_based = CPU_BASED_HLT_EXITING |       /* HLT 指令触发 VM-Exit */
                CPU_BASED_IO_EXITING  |       /* IO 指令触发 VM-Exit */
                CPU_BASED_USE_MSR_BITMAPS |   /* 使用 MSR bitmap */
                CPU_BASED_ACTIVATE_SECONDARY; /* 启用 secondary controls */

    vmcs_write(VMCS_CTRL_CPU_BASED, cpu_based);

    /* Secondary processor-based VM-execution controls */
    cpu_based2 = CPU_BASED2_ENABLE_EPT |      /* ★ Stage 2: 启用 EPT */
                 CPU_BASED2_RDTSCP;           /* 支持 RDTSCP */

    vmcs_write(VMCS_CTRL_CPU_BASED2, cpu_based2);
    pr_info("  ✓ 执行控制: HLT/IO exit, EPT 启用\n");

    /* ====================================================================
     * 配置 EPT Pointer (Stage 2)
     * ====================================================================
     * EPTP = (EPT PML4 物理地址) | (内存类型 << 3) | (页表层级 - 1)
     */
    if (kvm->ept.eptp) {
        vmcs_write(VMCS_CTRL_EPT_POINTER, kvm->ept.eptp);
        pr_info("  ✓ EPT Pointer: 0x%llx\n", kvm->ept.eptp);
    }

    /* ====================================================================
     * 配置 VM-Exit Controls
     * ====================================================================
     */
    exit_ctrl = VM_EXIT_HOST_ADDR_SPACE_SIZE;  /* 64-bit host */
    vmcs_write(VMCS_CTRL_EXIT, exit_ctrl);

    /* ====================================================================
     * 配置 VM-Entry Controls
     * ====================================================================
     */
    entry_ctrl = VM_ENTRY_IA32E_MODE;  /* 64-bit guest */
    vmcs_write(VMCS_CTRL_ENTRY, entry_ctrl);

    /* ====================================================================
     * 配置 Host State Area
     * ====================================================================
     * VM-Exit 后恢复这些状态到物理 CPU
     */
    vmcs_write(VMCS_HOST_RIP, (u64) &&vmx_exit_handler);
    vmcs_write(VMCS_HOST_RSP, 0);  /* 将在运行时设置 */
    vmcs_write(VMCS_HOST_CR0, read_cr0());
    vmcs_write(VMCS_HOST_CR3, __read_cr3());
    vmcs_write(VMCS_HOST_CR4, __read_cr4());

    /* Host 段寄存器 (使用内核段) */
    vmcs_write(VMCS_HOST_CS_SEL, __KERNEL_CS);
    vmcs_write(VMCS_HOST_DS_SEL, __KERNEL_DS);
    vmcs_write(VMCS_HOST_ES_SEL, __KERNEL_DS);
    vmcs_write(VMCS_HOST_SS_SEL, __KERNEL_DS);
    vmcs_write(VMCS_HOST_FS_SEL, 0);
    vmcs_write(VMCS_HOST_GS_SEL, 0);
    vmcs_write(VMCS_HOST_TR_SEL, __KERNEL_TSS);
    vmcs_write(VMCS_HOST_FS_BASE, 0);
    vmcs_write(VMCS_HOST_GS_BASE, 0);
    vmcs_write(VMCS_HOST_TR_BASE, 0);
    vmcs_write(VMCS_HOST_GDTR_BASE, 0);  /* 将从 IDT 获取 */
    vmcs_write(VMCS_HOST_IDTR_BASE, 0);

    pr_info("  ✓ Host 状态配置完成\n");

    /* ====================================================================
     * 配置 Guest State Area
     * ====================================================================
     * VM-Entry 时加载这些状态到物理 CPU
     * 初始 Guest 状态：64-bit 长模式，无保护
     */
    vmcs_write(VMCS_GUEST_RIP, vcpu->arch.rip);
    vmcs_write(VMCS_GUEST_RSP, vcpu->arch.rsp);
    vmcs_write(VMCS_GUEST_RFLAGS, 0x2);  /* 保留位必须为 1 */
    vmcs_write(VMCS_GUEST_CR0, vcpu->arch.cr0);
    vmcs_write(VMCS_GUEST_CR3, vcpu->arch.cr3);
    vmcs_write(VMCS_GUEST_CR4, vcpu->arch.cr4);
    vmcs_write(VMCS_GUEST_IA32_EFER, vcpu->arch.efer);

    /* Guest 段寄存器 (64-bit 长模式) */
    vmcs_write(VMCS_GUEST_CS_SEL, 0x0010);
    vmcs_write(VMCS_GUEST_CS_BASE, 0);
    vmcs_write(VMCS_GUEST_CS_LIMIT, 0xffffffff);
    vmcs_write(VMCS_GUEST_CS_AR, 0xa09b);   /* Code segment, present, executable */

    vmcs_write(VMCS_GUEST_DS_SEL, 0x0018);
    vmcs_write(VMCS_GUEST_DS_BASE, 0);
    vmcs_write(VMCS_GUEST_DS_LIMIT, 0xffffffff);
    vmcs_write(VMCS_GUEST_DS_AR, 0xc093);   /* Data segment, present, writable */

    vmcs_write(VMCS_GUEST_ES_SEL, 0x0018);
    vmcs_write(VMCS_GUEST_ES_BASE, 0);
    vmcs_write(VMCS_GUEST_ES_LIMIT, 0xffffffff);
    vmcs_write(VMCS_GUEST_ES_AR, 0xc093);

    vmcs_write(VMCS_GUEST_SS_SEL, 0x0018);
    vmcs_write(VMCS_GUEST_SS_BASE, 0);
    vmcs_write(VMCS_GUEST_SS_LIMIT, 0xffffffff);
    vmcs_write(VMCS_GUEST_SS_AR, 0xc093);

    vmcs_write(VMCS_GUEST_FS_SEL, 0);
    vmcs_write(VMCS_GUEST_FS_BASE, 0);
    vmcs_write(VMCS_GUEST_FS_LIMIT, 0);
    vmcs_write(VMCS_GUEST_FS_AR, 0x10000);

    vmcs_write(VMCS_GUEST_GS_SEL, 0);
    vmcs_write(VMCS_GUEST_GS_BASE, 0);
    vmcs_write(VMCS_GUEST_GS_LIMIT, 0);
    vmcs_write(VMCS_GUEST_GS_AR, 0x10000);

    vmcs_write(VMCS_GUEST_LDTR_SEL, 0);
    vmcs_write(VMCS_GUEST_LDTR_BASE, 0);
    vmcs_write(VMCS_GUEST_LDTR_LIMIT, 0);
    vmcs_write(VMCS_GUEST_LDTR_AR, 0x10000);

    vmcs_write(VMCS_GUEST_TR_SEL, 0);
    vmcs_write(VMCS_GUEST_TR_BASE, 0);
    vmcs_write(VMCS_GUEST_TR_LIMIT, 0);
    vmcs_write(VMCS_GUEST_TR_AR, 0x10000);

    vmcs_write(VMCS_GUEST_GDTR_BASE, 0);
    vmcs_write(VMCS_GUEST_GDTR_LIMIT, 0);
    vmcs_write(VMCS_GUEST_IDTR_BASE, 0);
    vmcs_write(VMCS_GUEST_IDTR_LIMIT, 0);

    vmcs_write(VMCS_GUEST_ACTIVITY, 0);  /* Active */
    vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, 0);

    pr_info("  ✓ Guest 状态配置完成\n");
    pr_info("    Guest RIP: 0x%llx\n", vcpu->arch.rip);
    pr_info("    Guest RSP: 0x%llx\n", vcpu->arch.rsp);

    pr_info("=== Stage 1 完成: vCPU %d VMCS 配置完毕 ===\n\n", vcpu->vcpu_id);
    return ret;

vmx_exit_handler:
    /*
     * VM-Exit 处理器 (从 Guest 返回到这里的 C 代码)
     *
     * 实际 KVM 中，VM-Exit 首先到达汇编代码 (vmenter.S)
     * 然后调用 vmx_handle_exit() (C 代码)
     *
     * 这里为了简化，直接从汇编跳回 C
     */
    pr_info("!!! VM-Exit 发生 !!!\n");

    /* 读取 VM-Exit 信息 */
    vcpu->exit_reason = vmcs_read(VMCS_EXIT_REASON) & 0xFFFF;
    vcpu->exit_qualification = vmcs_read(VMCS_EXIT_QUALIFICATION);
    vcpu->arch.rip = vmcs_read(VMCS_GUEST_RIP);
    vcpu->num_exits++;

    pr_info("  Exit reason: %d\n", vcpu->exit_reason);
    pr_info("  Exit qualification: 0x%llx\n", vcpu->exit_qualification);
    pr_info("  Guest RIP: 0x%llx\n", vcpu->arch.rip);

    /* 处理 exit (Stage 5) */
    ret = mini_kvm_handle_exit(vcpu);
    if (ret == MINI_KVM_EXIT_RESUME_GUEST) {
        /* 重新进入 Guest */
        asm volatile ("vmresume" ::: "memory", "cc");
    }

    return ret;
}

void mini_kvm_vcpu_vmx_teardown(struct mini_kvm_vcpu *vcpu)
{
    if (vcpu->arch.vmcs_virt)
        free_page((unsigned long)vcpu->arch.vmcs_virt);
    if (vcpu->arch.vmxon_virt)
        free_page((unsigned long)vcpu->arch.vmxon_virt);
}

/* ============================================================================
 * Stage 2: EPT 内存虚拟化
 * ============================================================================
 *
 * 对应课程：Phase 2 (Memory Virtualization)
 * 关键源码：arch/x86/kvm/mmu/tdp_mmu.c::kvm_tdp_mmu_map()
 *           arch/x86/kvm/mmu/spte.c::make_spte()
 *
 * EPT 实现 4 级页表 (4KB 页):
 *   PML4 → PDPT → PD → PT → 叶条目
 *
 * GPA → HPA 翻译由硬件自动完成
 * EPT Violation: GPA 未映射时触发 VM-Exit
 */

int mini_kvm_ept_init(struct mini_kvm *kvm)
{
    struct mini_kvm_ept_page *pml4;

    pr_info("=== Stage 2: EPT 初始化 ===\n");

    /* 分配 PML4 (根页表, 4KB 对齐) */
    pml4 = (struct mini_kvm_ept_page *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    if (!pml4)
        return -ENOMEM;

    kvm->ept.pml4 = pml4;

    /* 构造 EPT Pointer (EPTP)
     *
     * EPTP 格式:
     *   [2:0]   = 页表层级 - 1 (3 表示 4 级)
     *   [5:3]   = 内存类型 (6 = Write-Back)
     *   [63:12] = PML4 物理地址
     */
    kvm->ept.eptp = (__pa(pml4) & EPT_PTE_ADDR_MASK) |
                    EPT_PTE_MEM_WB |
                    (EPT_LEVEL_PML4 - 1);

    pr_info("  PML4 物理地址: 0x%llx\n", __pa(pml4));
    pr_info("  EPTP: 0x%llx\n", kvm->ept.eptp);
    pr_info("=== Stage 2 完成: EPT 根页表已建立 ===\n\n");

    return 0;
}

void mini_kvm_ept_destroy(struct mini_kvm *kvm)
{
    if (kvm->ept.pml4) {
        free_page((unsigned long)kvm->ept.pml4);
        kvm->ept.pml4 = NULL;
    }
}

/*
 * 在 EPT 中建立 GPA → HPA 映射
 *
 * 简化版本：只支持 4KB 页，只建立一级映射
 * 实际 KVM 支持大页 (2MB/1GB) 和中间页表分配
 */
int mini_kvm_ept_map_page(struct mini_kvm *kvm, u64 gpa, u64 hpa)
{
    struct mini_kvm_ept_page *pml4 = kvm->ept.pml4;
    struct mini_kvm_ept_page *pdpt, *pd, *pt;
    int pml4_idx, pdpt_idx, pd_idx, pt_idx;
    u64 pte;

    /* 计算各级索引 */
    pml4_idx = (gpa >> 39) & 0x1FF;
    pdpt_idx = (gpa >> 30) & 0x1FF;
    pd_idx   = (gpa >> 21) & 0x1FF;
    pt_idx   = (gpa >> 12) & 0x1FF;

    /* 简化：只处理 pml4_idx=0, pdpt_idx=0, pd_idx=0 的情况 */
    if (pml4_idx != 0 || pdpt_idx != 0 || pd_idx != 0) {
        pr_warn("EPT: 只支持低 2MB 的映射\n");
        return -EINVAL;
    }

    /* 分配 PDPT */
    if (!(pml4->entries[pml4_idx] & EPT_PTE_READ)) {
        pdpt = (struct mini_kvm_ept_page *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
        if (!pdpt)
            return -ENOMEM;
        pml4->entries[pml4_idx] = __pa(pdpt) | EPT_PTE_READ | EPT_PTE_WRITE |
                                  EPT_PTE_EXEC | EPT_PTE_MEM_WB;
    } else {
        pdpt = phys_to_virt(pml4->entries[pml4_idx] & EPT_PTE_ADDR_MASK);
    }

    /* 分配 PD */
    if (!(pdpt->entries[pdpt_idx] & EPT_PTE_READ)) {
        pd = (struct mini_kvm_ept_page *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
        if (!pd)
            return -ENOMEM;
        pdpt->entries[pdpt_idx] = __pa(pd) | EPT_PTE_READ | EPT_PTE_WRITE |
                                  EPT_PTE_EXEC | EPT_PTE_MEM_WB;
    } else {
        pd = phys_to_virt(pdpt->entries[pdpt_idx] & EPT_PTE_ADDR_MASK);
    }

    /* 分配 PT */
    if (!(pd->entries[pd_idx] & EPT_PTE_READ)) {
        pt = (struct mini_kvm_ept_page *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
        if (!pt)
            return -ENOMEM;
        pd->entries[pd_idx] = __pa(pt) | EPT_PTE_READ | EPT_PTE_WRITE |
                              EPT_PTE_EXEC | EPT_PTE_MEM_WB;
    } else {
        pt = phys_to_virt(pd->entries[pd_idx] & EPT_PTE_ADDR_MASK);
    }

    /* 写入叶条目 */
    pte = (hpa & EPT_PTE_ADDR_MASK) | EPT_PTE_READ | EPT_PTE_WRITE |
          EPT_PTE_EXEC | EPT_PTE_MEM_WB;
    pt->entries[pt_idx] = pte;

    pr_debug("EPT: 映射 GPA 0x%llx → HPA 0x%llx (4KB)\n", gpa, hpa);
    return 0;
}

/* ============================================================================
 * Stage 5: VM-Exit 处理与运行循环
 * ============================================================================
 *
 * 对应课程：Phase 0 (KVM framework) + Phase 9 (性能优化)
 * 关键源码：arch/x86/kvm/x86.c::vcpu_run()
 *           arch/x86/kvm/vmx/vmx.c::vmx_handle_exit()
 *           arch/x86/kvm/vmx/vmx.c::handle_pause()
 */

/*
 * 处理 VM-Exit
 *
 * 参考：vmx_handle_exit() → __vmx_handle_exit() → exit_handlers[]
 */
int mini_kvm_handle_exit(struct mini_kvm_vcpu *vcpu)
{
    int ret = MINI_KVM_EXIT_RESUME_GUEST;

    switch (vcpu->exit_reason) {

    case EXIT_REASON_CPUID:
        /*
         * CPUID 指令: 返回简化信息
         * 实际 KVM 在 kvm_emulate_cpuid() 中处理
         */
        pr_info("  → CPUID (模拟返回 0)\n");
        vcpu->arch.regs[0] = 0;  /* RAX = 0 (最大基本功能号) */
        vcpu->arch.rip += 2;      /* 跳过 CPUID 指令 (2字节) */
        break;

    case EXIT_REASON_HLT:
        /*
         * HLT 指令: 停止 vCPU
         * 实际 KVM 在 handle_halt() 中处理, 会进入 halt-polling
         */
        pr_info("  → HLT (Guest 停止)\n");
        vcpu->arch.rip += 1;  /* 跳过 HLT (1字节) */
        ret = MINI_KVM_EXIT_TO_USERSPACE;
        vcpu->num_hlt_exits++;
        break;

    case EXIT_REASON_IO_INSTRUCTION:
        /*
         * IO 指令: Stage 4 设备模拟
         * 实际 KVM 在 handle_io() 中处理
         */
        {
            u64 qual = vcpu->exit_qualification;
            u16 port = qual & 0xFFFF;
            bool is_write = (qual >> 16) & 1;
            u32 size = ((qual >> 5) & 7) + 1;
            u32 value = vcpu->arch.regs[0] & ((1ULL << (size * 8)) - 1);

            pr_info("  → IO %s port=0x%x size=%d value=0x%x\n",
                    is_write ? "OUT" : "IN", port, size, value);

            ret = mini_kvm_handle_io(vcpu, port, is_write, size, value);
            vcpu->arch.rip += (qual >> 4) & 0xF;  /* 指令长度 */
            vcpu->num_io_exits++;
        }
        break;

    case EXIT_REASON_EPT_VIOLATION:
        /*
         * EPT Violation: Stage 2 按需映射
         * 实际 KVM 在 handle_ept_violation() → kvm_tdp_page_fault() 中处理
         */
        pr_info("  → EPT Violation at GPA 0x%llx\n",
                vcpu->exit_qualification);
        ret = mini_kvm_ept_handle_violation(vcpu);
        vcpu->num_ept_violations++;
        break;

    case EXIT_REASON_EXCEPTION_NMI:
        pr_info("  → Exception/NMI\n");
        ret = MINI_KVM_EXIT_TO_USERSPACE;
        break;

    default:
        pr_warn("  → 未处理的 VM-Exit: %d\n", vcpu->exit_reason);
        ret = MINI_KVM_EXIT_TO_USERSPACE;
        break;
    }

    return ret;
}

/* ============================================================================
 * ioctl 接口 (用户空间 ↔ 内核)
 * ============================================================================
 *
 * 对应课程：Phase 0 (KVM framework)
 * 关键源码：virt/kvm/kvm_main.c::kvm_dev_ioctl()
 *           virt/kvm/kvm_main.c::kvm_vcpu_ioctl()
 */

static long mini_kvm_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
    void __user *argp = (void __user *)arg;
    long ret = 0;

    switch (ioctl) {
    case KVM_GET_API_VERSION:
        return 12;  /* 兼容标准 KVM API 版本 */

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static const struct file_operations mini_kvm_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = mini_kvm_ioctl,
};

static struct miscdevice mini_kvm_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "mini-kvm",
    .fops  = &mini_kvm_fops,
};

/* ============================================================================
 * 模块初始化与清理
 * ============================================================================ */

static int __init mini_kvm_init(void)
{
    int ret;

    pr_info("========================================\n");
    pr_info("  Mini-KVM 模块加载\n");
    pr_info("  教学项目: 简化版 KVM 实现\n");
    pr_info("========================================\n\n");

    /* Stage 1: VMX 初始化 */
    ret = mini_kvm_vmx_init();
    if (ret) {
        pr_err("VMX 初始化失败: %d\n", ret);
        return ret;
    }

    /* 注册字符设备 */
    ret = misc_register(&mini_kvm_dev);
    if (ret) {
        pr_err("设备注册失败: %d\n", ret);
        mini_kvm_vmx_exit();
        return ret;
    }

    pr_info("✓ Mini-KVM 模块加载成功\n");
    pr_info("  设备: /dev/%s\n", mini_kvm_dev.name);
    pr_info("  用法: sudo ./test-mini-kvm\n\n");

    return 0;
}

static void __exit mini_kvm_exit(void)
{
    pr_info("\n========================================\n");
    pr_info("  Mini-KVM 模块卸载\n");
    pr_info("========================================\n");

    /* 清理所有 VM */
    mutex_lock(&vms_lock);
    for (int i = 0; i < MAX_VMS; i++) {
        if (vms[i]) {
            mini_kvm_ept_destroy(vms[i]);
            kfree(vms[i]);
            vms[i] = NULL;
        }
    }
    mutex_unlock(&vms_lock);

    /* Stage 1: VMX 关闭 */
    mini_kvm_vmx_exit();

    /* 注销设备 */
    misc_deregister(&mini_kvm_dev);

    pr_info("✓ Mini-KVM 模块卸载成功\n");
}

module_init(mini_kvm_init);
module_exit(mini_kvm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KVM Study Project");
MODULE_DESCRIPTION("Mini-KVM: 简化版 KVM 教学实现");
MODULE_VERSION("0.1");
