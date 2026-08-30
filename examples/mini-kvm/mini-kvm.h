/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mini-kvm.h - 简化版 KVM 内部头文件
 *
 * 教学项目：循序渐进实现一个简化版 KVM
 * 对应课程：Phase 0-11
 *
 * 本文件定义 mini-kvm 的核心数据结构和接口
 */

#ifndef _MINI_KVM_H
#define _MINI_KVM_H

#include <linux/types.h>
#include <linux/kvm.h>
#include <linux/mutex.h>
#include <linux/list.h>

/*
 * ============================================================================
 * 配置常量
 * ============================================================================
 */
#define MINI_KVM_MAX_VCPUS      4        /* 最大 vCPU 数 */
#define MINI_KVM_MAX_MEMSLOTS   8        /* 最大内存槽数 */
#define MINI_KVM_GUEST_STACK_SIZE (64 * 1024)  /* Guest 栈大小 */

/* VM-Exit 处理返回值 */
#define MINI_KVM_EXIT_RESUME_GUEST   1   /* 重新进入 Guest */
#define MINI_KVM_EXIT_TO_USERSPACE   0   /* 返回用户空间 */
#define MINI_KVM_EXIT_ERROR         -1   /* 错误 */

/* 串口端口 (标准 COM1) */
#define MINI_KVM_SERIAL_PORT      0x3f8

/*
 * ============================================================================
 * EPT 相关定义 (Stage 2)
 * ============================================================================
 */

/* EPT 页表条目位定义 */
#define EPT_PTE_READ        (1ULL << 0)
#define EPT_PTE_WRITE       (1ULL << 1)
#define EPT_PTE_EXEC        (1ULL << 2)
#define EPT_PTE_MEM_WB      (6ULL << 3)   /* Write-Back 内存类型 */
#define EPT_PTE_LARGE_PAGE  (1ULL << 7)   /* 2MB 大页 */
#define EPT_PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* EPT 页表层级 */
#define EPT_LEVEL_PML4      4
#define EPT_LEVEL_PDPT      3
#define EPT_LEVEL_PD        2
#define EPT_LEVEL_PT        1

#define EPT_PTRS_PER_PTE    512
#define EPT_PT_SHIFT(level) (((level) - 1) * 9 + 12)

/*
 * ============================================================================
 * VMX 相关定义 (Stage 1)
 * ============================================================================
 */

/* VMCS 编码 (Intel SDM Appendix B) */
#define VMCS_GUEST_RIP          0x681E
#define VMCS_GUEST_RSP          0x681C
#define VMCS_GUEST_CR0          0x6800
#define VMCS_GUEST_CR3          0x6802
#define VMCS_GUEST_CR4          0x6804
#define VMCS_GUEST_RFLAGS       0x6820
#define VMCS_GUEST_CS_SEL       0x0802
#define VMCS_GUEST_CS_BASE      0x6808
#define VMCS_GUEST_CS_LIMIT     0x4802
#define VMCS_GUEST_CS_AR        0x4816
#define VMCS_GUEST_DS_SEL       0x0806
#define VMCS_GUEST_DS_BASE      0x680C
#define VMCS_GUEST_DS_LIMIT     0x4806
#define VMCS_GUEST_DS_AR        0x4818
#define VMCS_GUEST_ES_SEL       0x0800
#define VMCS_GUEST_ES_BASE      0x6806
#define VMCS_GUEST_ES_LIMIT     0x4800
#define VMCS_GUEST_ES_AR        0x4814
#define VMCS_GUEST_SS_SEL       0x0804
#define VMCS_GUEST_SS_BASE      0x680A
#define VMCS_GUEST_SS_LIMIT     0x4804
#define VMCS_GUEST_SS_AR        0x481A
#define VMCS_GUEST_FS_SEL       0x0808
#define VMCS_GUEST_FS_BASE      0x680E
#define VMCS_GUEST_FS_LIMIT     0x4808
#define VMCS_GUEST_FS_AR        0x481C
#define VMCS_GUEST_GS_SEL       0x080A
#define VMCS_GUEST_GS_BASE      0x6810
#define VMCS_GUEST_GS_LIMIT     0x480A
#define VMCS_GUEST_GS_AR        0x481E
#define VMCS_GUEST_LDTR_SEL     0x080C
#define VMCS_GUEST_LDTR_BASE    0x6812
#define VMCS_GUEST_LDTR_LIMIT   0x480C
#define VMCS_GUEST_LDTR_AR      0x4820
#define VMCS_GUEST_TR_SEL       0x080E
#define VMCS_GUEST_TR_BASE      0x6814
#define VMCS_GUEST_TR_LIMIT     0x480E
#define VMCS_GUEST_TR_AR        0x4822
#define VMCS_GUEST_GDTR_BASE    0x6816
#define VMCS_GUEST_GDTR_LIMIT   0x4810
#define VMCS_GUEST_IDTR_BASE    0x6818
#define VMCS_GUEST_IDTR_LIMIT   0x4812
#define VMCS_GUEST_IA32_EFER    0x2806
#define VMCS_GUEST_ACTIVITY     0x4826
#define VMCS_GUEST_INTERRUPTIBILITY 0x4824

#define VMCS_HOST_RIP           0x6C16
#define VMCS_HOST_RSP           0x6C14
#define VMCS_HOST_CR0           0x6C00
#define VMCS_HOST_CR3           0x6C02
#define VMCS_HOST_CR4           0x6C04
#define VMCS_HOST_CS_SEL        0x0C02
#define VMCS_HOST_DS_SEL        0x0C06
#define VMCS_HOST_ES_SEL        0x0C00
#define VMCS_HOST_SS_SEL        0x0C04
#define VMCS_HOST_FS_SEL        0x0C08
#define VMCS_HOST_GS_SEL        0x0C0A
#define VMCS_HOST_TR_SEL        0x0C0E
#define VMCS_HOST_FS_BASE       0x6C06
#define VMCS_HOST_GS_BASE       0x6C08
#define VMCS_HOST_TR_BASE       0x6C0C
#define VMCS_HOST_GDTR_BASE     0x6C0E
#define VMCS_HOST_IDTR_BASE     0x6C10

/* Primary Processor-Based VM-Execution Controls */
#define VMCS_CTRL_CPU_BASED             0x4002
#define CPU_BASED_HLT_EXITING           (1 << 7)
#define CPU_BASED_IO_EXITING            (1 << 25)
#define CPU_BASED_USE_MSR_BITMAPS       (1 << 28)
#define CPU_BASED_ACTIVATE_SECONDARY    (1 << 31)

/* Secondary Processor-Based VM-Execution Controls */
#define VMCS_CTRL_CPU_BASED2            0x401E
#define CPU_BASED2_ENABLE_EPT           (1 << 1)
#define CPU_BASED2_RDTSCP               (1 << 3)
#define CPU_BASED2_ENABLE_VPID          (1 << 5)

/* VM-Exit Controls */
#define VMCS_CTRL_EXIT                  0x400C
#define VM_EXIT_HOST_ADDR_SPACE_SIZE    (1 << 9)   /* 64-bit host */

/* VM-Entry Controls */
#define VMCS_CTRL_ENTRY                 0x4012
#define VM_ENTRY_IA32E_MODE             (1 << 9)   /* 64-bit guest */

/* EPT Pointer */
#define VMCS_CTRL_EPT_POINTER           0x201A

/* VM-Exit Information Fields */
#define VMCS_EXIT_REASON                0x4402
#define VMCS_EXIT_QUALIFICATION         0x6400

/* VM-Exit Reasons (Intel SDM Appendix C) */
#define EXIT_REASON_EXCEPTION_NMI       0
#define EXIT_REASON_EXTERNAL_INTERRUPT  1
#define EXIT_REASON_TRIPLE_FAULT        2
#define EXIT_REASON_CPUID               10
#define EXIT_REASON_HLT                 12
#define EXIT_REASON_INVD                13
#define EXIT_REASON_INVLPG              14
#define EXIT_REASON_RDPMC               15
#define EXIT_REASON_RDTSC               16
#define EXIT_REASON_VMCALL              18
#define EXIT_REASON_CR_ACCESS           28
#define EXIT_REASON_IO_INSTRUCTION      30
#define EXIT_REASON_MSR_READ            31
#define EXIT_REASON_MSR_WRITE           32
#define EXIT_REASON_EPT_VIOLATION       48
#define EXIT_REASON_EPT_MISCONFIG       49
#define EXIT_REASON_XSAVE               55
#define EXIT_REASON_XRSTORS             63

/*
 * ============================================================================
 * 核心数据结构
 * ============================================================================
 */

/* EPT 页表页 (4KB, 包含 512 个条目) */
struct mini_kvm_ept_page {
    u64 entries[EPT_PTRS_PER_PTE];
} __aligned(4096);

/* EPT 内存上下文 */
struct mini_kvm_ept {
    struct mini_kvm_ept_page *pml4;   /* EPT 根页表 (PML4) */
    u64 eptp;                          /* EPT Pointer (写入 VMCS) */
};

/* 内存槽 (Guest 物理内存区域) */
struct mini_kvm_memslot {
    u64 gpa;                           /* Guest 物理地址起始 */
    u64 size;                          /* 区域大小 */
    void *hva;                         /* Host 虚拟地址 */
    u64 hpa;                           /* Host 物理地址 */
};

/* vCPU 架构状态 (VMX 相关) */
struct mini_kvm_vcpu_arch {
    /* VMX 状态 */
    void *vmcs_virt;                   /* VMCS 虚拟地址 */
    u64 vmcs_phys;                     /* VMCS 物理地址 */
    void *vmxon_virt;                  /* VMXON 区域虚拟地址 */
    u64 vmxon_phys;                    /* VMXON 区域物理地址 */

    /* 通用寄存器缓存 */
    u64 regs[16];                      /* RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, ... */

    /* Guest 状态 */
    u64 rip;
    u64 rsp;
    u64 rflags;
    u64 cr0, cr3, cr4;
    u64 efer;

    /* Host 上下文 (用于 VM-Exit 恢复) */
    u64 host_rip;                      /* Host 返回地址 */
    u64 host_rsp;                      /* Host 栈指针 */
};

/* vCPU 结构 */
struct mini_kvm_vcpu {
    int vcpu_id;
    struct mini_kvm *kvm;              /* 所属 VM */
    struct mini_kvm_vcpu_arch arch;    /* 架构相关状态 */

    /* 运行状态 */
    bool running;
    int exit_reason;
    u64 exit_qualification;

    /* 统计 */
    u64 num_exits;
    u64 num_ept_violations;
    u64 num_io_exits;
    u64 num_hlt_exits;
};

/* VM 结构 */
struct mini_kvm {
    int vm_id;
    struct mutex lock;                 /* VM 级锁 */

    /* vCPU */
    struct mini_kvm_vcpu *vcpus[MINI_KVM_MAX_VCPUS];
    int nr_vcpus;

    /* 内存槽 */
    struct mini_kvm_memslot memslots[MINI_KVM_MAX_MEMSLOTS];
    int nr_memslots;

    /* EPT 上下文 */
    struct mini_kvm_ept ept;

    /* 设备状态 (Stage 4) */
    char serial_buffer[256];
    int serial_pos;
};

/* 全局状态 */
struct mini_kvm_global {
    bool vmx_enabled;
    u64 vmx_basic;                     /* MSR_IA32_VMX_BASIC */
    u64 vmx_cr0_fixed0;
    u64 vmx_cr0_fixed1;
    u64 vmx_cr4_fixed0;
    u64 vmx_cr4_fixed1;
    u32 vmcs_revision_id;
    int device_major;
};

extern struct mini_kvm_global mini_kvm_global;

/*
 * ============================================================================
 * 函数声明 (按 Stage 组织)
 * ============================================================================
 */

/* Stage 1: VMX 操作 */
int mini_kvm_vmx_init(void);
void mini_kvm_vmx_exit(void);
int mini_kvm_vcpu_vmx_setup(struct mini_kvm_vcpu *vcpu);
void mini_kvm_vcpu_vmx_teardown(struct mini_kvm_vcpu *vcpu);
int mini_kvm_vcpu_run(struct mini_kvm_vcpu *vcpu);

/* Stage 2: EPT 内存虚拟化 */
int mini_kvm_ept_init(struct mini_kvm *kvm);
void mini_kvm_ept_destroy(struct mini_kvm *kvm);
int mini_kvm_ept_map_page(struct mini_kvm *kvm, u64 gpa, u64 hpa);
int mini_kvm_ept_handle_violation(struct mini_kvm_vcpu *vcpu);

/* Stage 3: 中断处理 */
int mini_kvm_inject_irq(struct mini_kvm_vcpu *vcpu, int vector);
void mini_kvm_handle_interrupt(struct mini_kvm_vcpu *vcpu);

/* Stage 4: 设备模拟 */
int mini_kvm_handle_io(struct mini_kvm_vcpu *vcpu, u16 port, bool is_write,
                       u32 size, u32 value);

/* Stage 5: 运行循环 */
int mini_kvm_vcpu_run_loop(struct mini_kvm_vcpu *vcpu);
int mini_kvm_handle_exit(struct mini_kvm_vcpu *vcpu);

#endif /* _MINI_KVM_H */
