/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mini-kvm.h - 简化版 KVM 内核模块：核心数据结构与接口
 *
 * 教学项目：Phase 8 毕业建造 · KVM 内核侧
 *   构建目标 = 正在运行的内核（/lib/modules/$(uname -r)/build）
 *   参考实现 = Linux 6.12.93 KVM（引用格式：文件:行号）
 *
 * 与真实 KVM 的对应关系：
 *   struct mini_kvm       ←→ struct kvm        (include/linux/kvm_host.h)
 *   struct mini_kvm_vcpu  ←→ struct kvm_vcpu + struct vcpu_vmx
 *                                              (arch/x86/kvm/vmx/vmx.h)
 *   struct mini_kvm_memslot ←→ struct kvm_memory_slot (include/linux/kvm_host.h)
 */

#ifndef _MINI_KVM_H
#define _MINI_KVM_H

#include <linux/types.h>
#include <linux/kvm.h>
#include <linux/kvm_types.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/kref.h>

/*
 * ============================================================================
 * 常量
 * ============================================================================
 */

#define MINI_KVM_SERIAL_SIZE	512	/* 串口捕获缓冲区大小 */

/*
 * 原始 rdmsr / wrmsr。
 *
 * 为什么不直接用内核接口：native_read_msr()/native_write_msr() 不在构建目标
 * 内核的 Module.symvers 里（模块拿不到，见 Makefile 顶部关于构建目标的说明），
 * 只能自己内联指令。
 * 前提：所有调用点的 MSR 都已由前面的 CPUID / VMX 能力检查保证存在，
 * 因此不会 #GP。
 */
static inline u64 mini_rdmsr(u32 msr)
{
	u32 lo, hi;

	asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((u64)hi << 32) | lo;
}

static inline void mini_wrmsr(u32 msr, u64 val)
{
	u32 lo = (u32)val, hi = (u32)(val >> 32);

	asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

/*
 * Guest 入口状态（简化设计：固定值，不由用户态配置）。
 * Guest 是一个 64 位裸机程序：
 *   代码   @ GPA 0x1000   （guest/guest.S 链接地址）
 *   栈顶   @ GPA 0x100000 （1MB 处向下生长）
 *   页表根 @ GPA 0x6000   （用户态测试程序负责搭建：
 *                           PML4@0x6000 → PDPT@0x7000 → PD@0x8000，
 *                           PD[0] 为 2MB 大页覆盖 GPA 0-2MB）
 * 对应关系：RIP/RSP 写入 VMCS GUEST_RIP/GUEST_RSP；CR3 写入 GUEST_CR3。
 */
#define MINI_KVM_GUEST_RIP	0x1000ULL
#define MINI_KVM_GUEST_RSP	0x100000ULL
#define MINI_KVM_GUEST_CR3	0x6000ULL

/* 模拟的串口端口（标准 PC COM1） */
#define MINI_KVM_SERIAL_PORT	0x3f8

/* EPT 叶条目权限位（SDM §29.3.2 的 Table 29-7 "EPT Page-Table Entry that
 * Maps a 4-KByte Page"；非叶条目见 Table 29-1/29-2/29-4/29-6，bits 2:0 同义） */
#define EPT_PTE_READ		(1ULL << 0)
#define EPT_PTE_WRITE		(1ULL << 1)
#define EPT_PTE_EXEC		(1ULL << 2)

/*
 * ============================================================================
 * mini-kvm 私有 ioctl（magic 'M'）
 * ============================================================================
 *
 * 真实 KVM 用统一的 KVM_* ioctl（include/uapi/linux/kvm.h）。mini-kvm 在
 * 复用大部分 KVM_* ioctl（API_VERSION/CHECK_EXTENSION/CREATE_VM/
 * CREATE_VCPU/SET_USER_MEMORY_REGION/RUN/GET_VCPU_MMAP_SIZE）之外，
 * 只新增两个私有命令：
 */
#define MINI_KVM_IOC_MAGIC		'M'

/* VM fd 上：读取串口捕获缓冲区（NUL 结尾字符串） */
#define MINI_KVM_VM_GET_SERIAL		_IOR(MINI_KVM_IOC_MAGIC, 0x01, char[256])

/* vCPU fd 上：请求注入一个外部中断（参数 = vector，如 0x21） */
#define MINI_KVM_VCPU_INJECT_IRQ	_IOW(MINI_KVM_IOC_MAGIC, 0x02, int)

/*
 * ============================================================================
 * 全局状态（模块级）
 * ============================================================================
 */

struct mini_kvm_global {
	bool vmx_enabled;		/* 全机 VMX 已开启（所有在线 CPU 已 VMXON） */
	u32 vmcs_revision_id;		/* IA32_VMX_BASIC[30:0]，写入每个 VMCS/VMXON 区首页 */
	bool true_ctls;			/* IA32_VMX_BASIC[55]：TRUE_* 控制 MSR 可用
					 * （内核同名宏 VMX_BASIC_TRUE_CTLS，
					 * arch/x86/include/asm/vmx.h:134） */
	u64 ept_vpid_cap;		/* IA32_VMX_EPT_VPID_CAP。位定义在规范
					 * 附录 A.10（不在本仓库这份 Vol.3C PDF
					 * 里），本模块用到的位都按内核
					 * arch/x86/kvm/vmx/capabilities.h 的
					 * cpu_has_vmx_* 谓词核对；
					 * EPTP 各字段的用法见 §25.6.11） */
	atomic_t enable_err;		/* per-CPU VMXON 阶段的第一个错误码 */
};

extern struct mini_kvm_global mk_global;

/*
 * ============================================================================
 * 内存虚拟化（Stage 2）
 * ============================================================================
 */

/*
 * 内存槽：等价于 KVM 的 struct kvm_memory_slot 的最小版本。
 * KVM_SET_USER_MEMORY_REGION 时 pin_user_pages() 钉住用户页，
 * EPT violation 时经 pages[] 数组把 GPA 翻译成 HPA。
 */
struct mini_kvm_memslot {
	bool valid;
	u64 base_gpa;			/* Guest 物理地址起始（页对齐） */
	u64 npages;			/* 页数 */
	u64 userspace_addr;		/* 用户态 HVA 起始 */
	struct page **pages;		/* pin_user_pages() 结果，长度 = npages */
};

/*
 * EPT 上下文：等价于 KVM 的 kvm_mmu / EPT 页表（真实 KVM 用 TDP MMU，
 * arch/x86/kvm/mmu/tdp_mmu.c；mini-kvm 手写 4 级页表）。
 * tracked[] 记录行走过程中分配的中间页表页，销毁时统一释放。
 */
#define MINI_KVM_EPT_TRACK_MAX	1024

struct mini_kvm_ept {
	struct page *pml4;		/* EPT 根（PML4），EPTP 指向它 */
	u64 eptp;			/* 写入 VMCS EPT_POINTER 的值 */
	struct page *tracked[MINI_KVM_EPT_TRACK_MAX];
	int nr_tracked;
};

/*
 * ============================================================================
 * vCPU
 * ============================================================================
 */

/*
 * struct mini_kvm_vcpu - 等价于 KVM 的 kvm_vcpu + vcpu_vmx 最小合并版
 *
 * 布局注意：regs[16] 必须是第一个成员——vmx_entry.S 的进入/退出汇编
 * 直接按固定偏移读写它。顺序与 struct kvm_regs 一致：
 *   regs[0]=RAX regs[1]=RCX regs[2]=RDX regs[3]=RBX regs[4]=RSP(不用)
 *   regs[5]=RBP regs[6]=RSI regs[7]=RDI regs[8..15]=R8-R15
 * （guest RSP 保存在 VMCS GUEST_RSP，不经 regs[] 传递，与 KVM 相同：
 *  vmenter.S 的寄存器装载序列同样跳过 RSP。）
 */
struct mini_kvm_vcpu {
	u64 regs[16];

	int vcpu_id;
	struct mini_kvm *kvm;

	/*
	 * 串行化同一 vCPU fd 上的 ioctl，对应 KVM 的 vcpu->mutex
	 * （mutex_init 见 virt/kvm/kvm_main.c:484，加锁见 kvm_vcpu_ioctl()
	 * 的 mutex_lock(&vcpu->mutex)，kvm_main.c:4468）。
	 *
	 * 不是可有可无的"整洁性"锁：没有它，两个线程可以同时 ioctl 同一个
	 * vCPU fd，各自在不同 CPU 上 VMPTRLD 同一份 VMCS。SDM 25.11.1 对这一
	 * 句的措辞是硬性："No VMCS should ever be active on more than one
	 * logical processor … A VMCS that is made active on more than one
	 * logical processor may become corrupted"。除此之外两个线程还会同时
	 * 读写 vcpu->regs[] / pending_intr_info / kvm_run 页。
	 */
	struct mutex mutex;

	/* VMCS（每 vCPU 一个物理页，首页写 revision_id） */
	struct page *vmcs_page;
	u64 vmcs_phys;
	bool launched;			/* VMLAUNCH 成功后置位，此后用 VMRESUME */
	int loaded_cpu;			/* 当前持有 VMPTRLD 的 CPU（-1 = 无） */

	/* kvm_run 共享页（vcpu fd 的 mmap 目标） */
	struct page *run_page;
	struct kvm_run *run;

	/*
	 * 中断注入：非 0 时在下次进入前写入 VM_ENTRY_INTR_INFO_FIELD。
	 * 位格式见 SDM 25.8.3 / 27.3.1.4：
	 *   [7:0]=vector [10:8]=类型(0=外部中断) [31]=valid
	 *
	 * 这一格同时承担 KVM 里 `vcpu->arch.interrupt.injected` 的职责：运行
	 * 循环把它取走写进 VMCS 就算"已注入"，但注入不等于投递完成——事件在
	 * 投递途中引发 VM-Exit 时（SDM 28.2.4），硬件会把该事件记进
	 * IDT-vectoring information，退出路径据此把它放回这一格重投。
	 */
	u32 pending_intr_info;

	/* 统计（dmesg 与调试用） */
	u64 n_exits;
	u64 n_io_exits;
	u64 n_ept_violations;
	u64 n_hlt_exits;
	u64 n_extint_exits;
	u64 n_nmi_exits;
	u64 n_injected;
};

/*
 * ============================================================================
 * VM
 * ============================================================================
 */

struct mini_kvm {
	int vm_id;
	struct mutex lock;

	/*
	 * 引用计数：VM fd 持有 1 份，每个 vCPU fd 再持有 1 份。
	 * 必要性和真实 KVM 一样 —— vCPU fd 的生命周期独立于 VM fd，
	 * 用户态可以先 close(vm_fd) 再 close(vcpu_fd)，而 vcpu->kvm、
	 * EPT 页表、pin 住的用户页全都挂在 VM 上。
	 * 对照 struct kvm 的 users_count（include/linux/kvm_host.h:826）与
	 * kvm_get_kvm()/kvm_put_kvm()（virt/kvm/kvm_main.c:1372-1393）；
	 * KVM 在 kvm_vm_ioctl_create_vcpu() 里正是先 kvm_get_kvm()
	 * 再装 fd（kvm_main.c:4296-4297）。
	 */
	struct kref refcount;

	struct mini_kvm_memslot slot;	/* 单 slot 简化 */
	struct mini_kvm_ept ept;

	/* 串口捕获缓冲区（Stage 4）：OUT 到 0x3f8 的字节收集于此 */
	char serial[MINI_KVM_SERIAL_SIZE];
	int serial_len;

	struct mini_kvm_vcpu *vcpu;	/* 单 vCPU 简化 */
};

/*
 * ============================================================================
 * 函数声明（按源文件组织）
 * ============================================================================
 */

/* main.c —— 模块入口、能力探测、per-CPU VMXON/VMXOFF */
int mini_vmx_read_capabilities(void);
int mini_vmx_hardware_enable_all(void);
void mini_vmx_hardware_disable_all(void);
bool mini_cpu_in_vmx_operation(void);

/* vmx.c —— VMX 指令包装、控制域协商、VMCS 初始化 */
int mini_vmread(u32 field, u64 *value);
int mini_vmwrite(u32 field, u64 value);
int mini_vmptrld(u64 phys);
int mini_vmclear(u64 phys);
void mini_vmcs_clear(struct mini_kvm_vcpu *vcpu);	/* 投递到 loaded_cpu 的 VMCLEAR + 清 launched */
u32 mini_vmx_adjust_control(u32 desired, u32 msr_index);
int mini_vcpu_vmx_setup(struct mini_kvm_vcpu *vcpu);
void mini_vcpu_vmx_teardown(struct mini_kvm_vcpu *vcpu);
void mini_vmx_refresh_host_state(void);		/* 每次进入前调用 */
void mini_vmx_fixup_host_for_cpu(void);		/* 迁移 CPU 时调用 */
void mini_dump_vmcs(const char *when);
void mini_vmx_report_error(const char *what);

/* vmx_entry.S —— VMLAUNCH/VMRESUME 进入与 VM-Exit 返回 */
int mini_vmx_enter(struct mini_kvm_vcpu *vcpu, int launched);
/* mini_vmx_vmexit：HOST_RIP 目标（汇编内全局符号，勿从 C 调用） */

/* ept.c —— EPT 建立与按需映射（Stage 2） */
int mini_ept_init(struct mini_kvm *kvm);
void mini_ept_destroy(struct mini_kvm *kvm);
int mini_ept_handle_violation(struct mini_kvm_vcpu *vcpu);
int mini_ept_invept_global(void);	/* all-context INVEPT；运行循环每次 VMPTRLD 后一次 */

/* interrupt.c —— 中断注入（Stage 3） */
int mini_vcpu_inject_irq(struct mini_kvm_vcpu *vcpu, int vector);
u32 mini_vcpu_take_intr_info(struct mini_kvm_vcpu *vcpu);
void mini_vcpu_complete_intr_info(struct mini_kvm_vcpu *vcpu, u32 reason);
void mini_vcpu_reinject_nmi(struct mini_kvm_vcpu *vcpu);

/* device.c —— IO 退出解码 + 串口模拟（Stage 4） */
int mini_handle_io_exit(struct mini_kvm_vcpu *vcpu);

/* vcpu.c —— 三层 fd、ioctl、mmap、运行循环（Stage 5） */
extern const struct file_operations mini_kvm_fops;
int mini_vcpu_dev_init(void);
void mini_vcpu_dev_exit(void);

#endif /* _MINI_KVM_H */
