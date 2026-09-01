// SPDX-License-Identifier: GPL-2.0
/*
 * vmx.c —— VMX 指令包装、控制域协商、VMCS 全量初始化
 *
 * 【Stage 对应】Stage 1（VMX 基础）的 VMCS 部分 + Stage 2（EPT）的控制位。
 *
 * 参考实现（Linux 6.12.93）：
 *   adjust_vmx_controls()          arch/x86/kvm/vmx/vmx.c:2563-2579  控制域协商
 *   vmx_set_constant_host_state()  arch/x86/kvm/vmx/vmx.c:4320-4385  Host 状态写入配方
 *   init_vmcs()                    arch/x86/kvm/vmx/vmx.c:4728-      控制域/杂项字段
 *   vmx_vcpu_load_vmcs()           arch/x86/kvm/vmx/vmx.c:1449-1514  迁移 CPU 时的 Host 字段修补
 *   VMX 指令的 asm 包装风格        arch/x86/kvm/vmx/ops.h
 *
 * 控制域 MSR 语义：
 *   低 32 位 = 必须为 1 的位；高 32 位 = 允许为 1 的位
 *   desired = (desired & high) | low
 *
 * 本机实证（CPU0，pread /dev/cpu/0/msr；不是内存里的印象值）：
 *   IA32_VMX_BASIC           0x480 = 0x00da040000000004  bit55=1 → 有 TRUE 组
 *   IA32_VMX_PROCBASED_CTLS  0x482 must1=0x0401e172 can1=0xfff9fffe
 *   IA32_VMX_TRUE_PROCBASED  0x48e must1=0x04006172 can1=0xfff9fffe
 *
 * 两组只差 bits 15/16（CR3-load / CR3-store exiting）。SDM 在 Table 25-6 之后
 * 写得很明确：
 *   "The first processors to support the virtual-machine extensions supported
 *    only the 1-settings of bits 1, 4-6, 8, 13-16, and 26. The VMX capability
 *    MSR IA32_VMX_PROCBASED_CTLS will always report that these bits must be 1.
 *    Logical processors that support the 0-settings of any of these bits will
 *    support the VMX capability MSR IA32_VMX_TRUE_PROCBASED_CTLS MSR, and
 *    software should consult this MSR..."
 *   （intel-vmx.pdf, Vol.3C Table 25-6 后的说明段；Appendix A.3.2）
 * 本机走 TRUE 组（mini_compute_controls() 按 mk_global.true_ctls 选择）不只是
 * "更省事"：legacy 组会把 CR3-access exiting 强制打开，而本模块的运行循环没有
 * EXIT_REASON_CR_ACCESS 分支，guest 一旦 MOV to CR3 就会被判成未处理退出。
 * KVM 用的是 legacy 组（setup_vmcs_config()，vmx.c:2621-2624），它有完整的
 * handle_cr 分支（vmx.c:6101）兜住这类退出。
 *
 * 顺带：上面两个 legacy must1 值就是内核里的具名常量
 * CPU_BASED_ALWAYSON_WITHOUT_TRUE_MSR / PIN_BASED_ALWAYSON_WITHOUT_TRUE_MSR
 * （arch/x86/include/asm/vmx.h:51 与 :95）。
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <asm/msr-index.h>
#include <asm/segment.h>
#include <asm/processor.h>
#include <asm/desc.h>
#include <asm/special_insns.h>
#include <asm/vmx.h>
#include <asm/io.h>

#include "mini-kvm.h"

/*
 * ============================================================================
 * VMX 指令包装
 * ============================================================================
 *
 * 所有 VMX 指令失败时置 ZF/CF（SDM Vol.3 31.2）；setna = ZF|CF。
 * 具体错误码需另读 VM_INSTRUCTION_ERROR（0x4400）。
 *
 * 注意：6.8 未导出 KVM 的 vmcs_read/vmcs_write（它们在模块里也依赖
 * 当前 CPU 已 VMPTRLD 的 VMCS），mini-kvm 用自己的包装，语义相同。
 */

int mini_vmread(u32 field, u64 *value)
{
	u8 err;
	u64 v;

	asm volatile("vmread %2, %1\n\t"
		     "setna %0"
		     : "=qm"(err), "=r"(v)
		     : "r"((u64)field) : "cc");
	if (err)
		return -EIO;
	*value = v;
	return 0;
}

int mini_vmwrite(u32 field, u64 value)
{
	u8 err;

	/* AT&T 语序：vmwrite <值>, <字段编码>（对照 ops.h 的 vmwrite()） */
	asm volatile("vmwrite %2, %1\n\t"
		     "setna %0"
		     : "=qm"(err)
		     : "r"((u64)field), "rm"(value) : "cc");
	return err ? -EIO : 0;
}

int mini_vmptrld(u64 phys)
{
	u8 err;

	asm volatile("vmptrld %1\n\t"
		     "setna %0"
		     : "=qm"(err) : "m"(phys) : "cc");
	return err ? -EIO : 0;
}

int mini_vmclear(u64 phys)
{
	u8 err;

	asm volatile("vmclear %1\n\t"
		     "setna %0"
		     : "=qm"(err) : "m"(phys) : "cc");
	return err ? -EIO : 0;
}

/* 读 VM_INSTRUCTION_ERROR，失败诊断用 */
static u64 mini_vmx_instruction_error(void)
{
	u64 v = 0;

	mini_vmread(VM_INSTRUCTION_ERROR, &v);
	return v;
}

struct mini_vmclear_args {
	u64 phys;
	int err;
};

/* smp_call_function_single 回调：在 VMCS 当前所属的 CPU 上 VMCLEAR */
static void mini_vmclear_ipi(void *info)
{
	struct mini_vmclear_args *a = info;

	/*
	 * 本模块没有 CPU 热插拔通知器：一个"加载后才上线"的 CPU 没有
	 * VMXON，在那里执行 VMCLEAR 会 #UD 直接打崩宿主，必须先问一句。
	 */
	if (!mini_cpu_in_vmx_operation()) {
		a->err = -ENODEV;
		return;
	}
	a->err = mini_vmclear(a->phys);
}

/*
 * mini_vmcs_clear - 在"最后 VMPTRLD 过这个 VMCS 的 CPU"上执行 VMCLEAR
 *
 * 必须在旧 CPU 上做，而不是在当前（新）CPU 上做。SDM 25.11.1 的理由有半句
 * 常被忽略（原文）：
 *   "the first logical processor should execute VMCLEAR for the VMCS (to make
 *    it inactive on that logical processor and to ensure that all VMCS data
 *    are in memory) before the other logical processor executes VMPTRLD"
 * 即 VMCLEAR 还负责把可能留在处理器内部的 VMCS 数据回写到内存（25.11.1
 * 后面明确："a logical processor may maintain some VMCS data of an active
 * VMCS on the processor and not in the VMCS region"）。在新 CPU 上补一发
 * VMCLEAR 只改了内存里的状态，旧 CPU 的片上副本仍然有效 —— 于是同一个
 * VMCS 可能"在多个逻辑处理器上 active"，规范说这样"may become corrupted"。
 *
 * 对照 KVM：loaded_vmcs_clear()（arch/x86/kvm/vmx/vmx.c:812-819）把回调
 * 投递到 loaded_vmcs->cpu（旧 CPU）执行 —— smp_call_function_single(cpu,
 * __loaded_vmcs_clear, loaded_vmcs, 1)。
 * 旧 CPU 已经离线时，generic_exec_single() 根本不执行回调、直接返回 -ENXIO
 * （kernel/smp.c:439-441）。KVM 对这个返回值也是忽略的，因为它在
 * __loaded_vmcs_clear() 开头就写明了同一个竞争 ——
 * "vcpu migration can race with cpu offline"（vmx.c:788-789）。
 * 我们照做，只补一条 pr_warn。
 *
 * 另一半修的是 VMRESUME/VMLAUNCH 选择：VMCLEAR 会把 launch state 置为
 * "clear"，所以迁移后的第一次进入必须用 VMLAUNCH。SDM 25.11.3 明说
 * "Since 'migrating' a VMCS from one logical processor to another requires
 * use of VMCLEAR (see Section 25.11.1), which sets the launch state of the
 * VMCS to 'clear', such migration requires the next VM entry to be performed
 * using VMLAUNCH"；忘了清 launched 标志就会用 VMRESUME 进一个 clear 状态的
 * VMCS，得到 VM-instruction error 5 "VMRESUME with non-launched VMCS"
 * （SDM 31.4 Table 31-1）。KVM 同样在 __loaded_vmcs_clear() 里紧跟
 * vmcs_clear() 写 loaded_vmcs->launched = 0（vmx.c:809）。
 */
void mini_vmcs_clear(struct mini_kvm_vcpu *vcpu)
{
	struct mini_vmclear_args a = { .phys = vcpu->vmcs_phys, .err = 0 };
	int cpu = vcpu->loaded_cpu, r;

	/* 无论 VMCLEAR 落在哪里，launch state 都必须当成 clear */
	vcpu->launched = false;

	if (cpu < 0)
		return;

	/*
	 * 判断"在不在本机"和真正执行 VMCLEAR 必须是同一个不可抢占的窗口：
	 * 两步之间被换下 CPU 的话，VMCLEAR 就落在了一台从未 VMPTRLD 过这个
	 * VMCS 的机器上，§25.11.1 要的那份"片上 VMCS 数据回写"依然没发生。
	 * KVM 的 loaded_vmcs_clear() 只在 vcpu_load() 的 get_cpu() 窗口里被调用
	 * （vmx.c:1457 在 vmx_vcpu_load_vmcs() 内），天然满足这个约束。
	 */
	preempt_disable();
	if (cpu == raw_smp_processor_id()) {
		a.err = mini_vmclear(a.phys);
	} else {
		r = smp_call_function_single(cpu, mini_vmclear_ipi, &a, 1);
		if (r)
			a.err = r;	/* 旧 CPU 不在线：回调根本没跑 */
	}
	preempt_enable();

	if (a.err)
		pr_warn("mini-kvm: CPU%d 上的 VMCLEAR 未完成 (err=%d)，VMCS 片上状态可能未回写\n",
			cpu, a.err);

	vcpu->loaded_cpu = -1;
}

/*
 * ============================================================================
 * 控制域协商
 * ============================================================================
 */

/*
 * 对照 adjust_vmx_controls()（vmx.c:2563-2579）：
 *   低 32 位 = 必须为 1 的位 → 强制置位
 *   高 32 位 = 允许为 1 的位 → 其余清掉
 * 简化：不做"最小要求位缺失则报错"（教学模块直接采用协商结果）。
 */
u32 mini_vmx_adjust_control(u32 desired, u32 msr_index)
{
	u64 msr = mini_rdmsr(msr_index);
	u32 must_be_one = (u32)msr;
	u32 can_be_one = (u32)(msr >> 32);

	desired &= can_be_one;
	desired |= must_be_one;
	return desired;
}

/* 协商后的五组控制域（VMCS 初始化时计算一次） */
struct mini_vmcs_controls {
	u32 pin_based;
	u32 cpu_based;
	u32 secondary;
	u32 exit;
	u32 entry;
};

static int mini_compute_controls(struct mini_vmcs_controls *c)
{
	bool t = mk_global.true_ctls;

	c->pin_based = mini_vmx_adjust_control(
		PIN_BASED_EXT_INTR_MASK | PIN_BASED_NMI_EXITING,
		t ? MSR_IA32_VMX_TRUE_PINBASED_CTLS
		  : MSR_IA32_VMX_PINBASED_CTLS);

	c->cpu_based = mini_vmx_adjust_control(
		CPU_BASED_HLT_EXITING |
		CPU_BASED_UNCOND_IO_EXITING |
		CPU_BASED_ACTIVATE_SECONDARY_CONTROLS,
		t ? MSR_IA32_VMX_TRUE_PROCBASED_CTLS
		  : MSR_IA32_VMX_PROCBASED_CTLS);

	/* 次级控制只有一个 MSR，没有 TRUE_* 变体（SDM Appendix A.3.3） */
	c->secondary = mini_vmx_adjust_control(
		SECONDARY_EXEC_ENABLE_EPT,
		MSR_IA32_VMX_PROCBASED_CTLS2);

	c->exit = mini_vmx_adjust_control(
		VM_EXIT_HOST_ADDR_SPACE_SIZE | VM_EXIT_LOAD_IA32_EFER,
		t ? MSR_IA32_VMX_TRUE_EXIT_CTLS
		  : MSR_IA32_VMX_EXIT_CTLS);

	c->entry = mini_vmx_adjust_control(
		VM_ENTRY_IA32E_MODE | VM_ENTRY_LOAD_IA32_EFER,
		t ? MSR_IA32_VMX_TRUE_ENTRY_CTLS
		  : MSR_IA32_VMX_ENTRY_CTLS);

	/*
	 * 协商结果必须回读校验：adjust 只会"按硬件能力削位"，不支持时
	 * 想要的位会静默消失。KVM 的 cpu_has_vmx_ept() 正是检查协商后的
	 * cpu_based_2nd_exec_ctrl（capabilities.h:145-149），而
	 * vmx_hardware_setup() 用它 + 4 级行走 + WB + INVEPT 决定是否
	 * 关掉 enable_ept（vmx.c:8434-8438）。
	 * mini-kvm 没有"不用 EPT 也能跑"的第二条路（guest 页表和 EPT 是
	 * 两套不同的东西），所以这里直接拒绝创建 VM/vCPU。
	 */
	if (!(c->cpu_based & CPU_BASED_ACTIVATE_SECONDARY_CONTROLS) ||
	    !(c->secondary & SECONDARY_EXEC_ENABLE_EPT)) {
		pr_err("mini-kvm: 硬件不支持 EPT（cpu=%08x sec=%08x），mini-kvm 必须依赖 EPT\n",
		       c->cpu_based, c->secondary);
		return -EOPNOTSUPP;
	}

	return 0;
}

/*
 * ============================================================================
 * Host 状态字段
 * ============================================================================
 *
 * 配方逐项对照 vmx_set_constant_host_state()（vmx.c:4320-4385）与
 * vmx_vcpu_load_vmcs()（vmx.c:1457-1514）。
 *
 * VM-Exit 时处理器用这些字段重建宿主执行环境（SDM 28.5），任何字段写错
 * 都可能直接宕机——尤其 HOST_GS_BASE（x86_64 内核用 %gs 寻址 per-CPU
 * 数据，见 cpu_kernelmode_gs_base()，asm/processor.h）。
 */

static u64 host_idt_base;

/*
 * 写入 VMCS 的 host CR4。
 *
 * 为什么一定要或上 CR4.VMXE：规范只在"离开 VMX 操作"时才允许清它
 * —— SDM 1.5 "Once in VMX operation, it is not possible to clear CR4.VMXE"
 * （§23.7 把 CR4.VMXE 列为 VMX 操作期间被固定的位）。而 §27.2.2 对 host-state
 * 的 CR4 只做一件事："The CR4 field must not set any bit to a value not
 * supported in VMX operation" —— 也就是说 VMXE=0 **不会**被进入检查拦下来，
 * 硬件会带着一个"root 模式下 VMXE 为 0"的状态继续跑，那是规范没有定义的领域。
 * 直接把这一位钉住，就不去踩这条边界。
 *
 * 差异点：KVM 用 cr4_set_bits(X86_CR4_VMXE) 开启 VMX 能力
 * （vmx.c:2837），cr4 shadow 里已含该位，因此可直接
 * cr4 = cr4_read_shadow(); vmcs_writel(HOST_CR4, cr4)（vmx.c:4339-4341）。
 * mini-kvm 只能裸写 CR4 —— cr4_set_bits()/cr4_clear_bits() 都没有
 * 导出给模块（见 6.8 的 Module.symvers，只有 cr4_read_shadow 导出）
 * —— shadow 里拿不到 VMXE，必须自己补上。
 */
static u64 mini_host_cr4(void)
{
	return cr4_read_shadow() | X86_CR4_VMXE;
}

/* vmx_entry.S 里的 VM-Exit 着陆点（全局符号，HOST_RIP 的目标） */
extern void mini_vmx_vmexit(void);

/* 每 CPU / 每次进入都要刷新的 Host 字段（vcpu.c 运行循环调用） */
void mini_vmx_refresh_host_state(void)
{
	int cpu = raw_smp_processor_id();

	/*
	 * 22.2.3：CR3/CR4 可能随线程与上下文变化，每次进入前刷新。KVM 把这件事
	 * 放在 vmx_vcpu_run() 的末尾、紧贴 VM-Entry（vmx.c:7396-7412），理由原文：
	 *   "This must be done immediately prior to VM-Enter, as the kernel may
	 *    load a new ASID (PCID) any time it switches back to the current->mm,
	 *    which can occur in KVM context when switching to a temporary mm to
	 *    patch kernel code, e.g. if KVM toggles a static key while handling
	 *    a VM-Exit."
	 * 我们的刷新点在循环头，离 VMRESUME 只剩几次 VMCS 访问 + 一次 RDMSR，
	 * 而且运行循环全程 preempt_disable（不会切 mm），已经足够"紧贴"。
	 */
	mini_vmwrite(HOST_CR3, __read_cr3());
	mini_vmwrite(HOST_CR4, mini_host_cr4());

	/*
	 * %gs 基址：内核态恒为 CPU entry area 的固定偏移。
	 * 对照 vmx_prepare_switch_to_guest()（vmx.c:1332 取
	 * gs_base = cpu_kernelmode_gs_base(cpu)，:1354 传给
	 * vmx_set_host_fs_gs()），实际写 HOST_GS_BASE 在该函数的
	 * vmx.c:1281。
	 */
	mini_vmwrite(HOST_GS_BASE, (u64)cpu_kernelmode_gs_base(cpu));

	/* FS 基址：当前任务的值（guest 不动 FS，退出时硬件按此恢复） */
	mini_vmwrite(HOST_FS_BASE, mini_rdmsr(MSR_FS_BASE));
}

/* 迁移到新 CPU 时修补的 Host 字段（Linux 用 per-CPU 的 TSS/GDT） */
void mini_vmx_fixup_host_for_cpu(void)
{
	int cpu = raw_smp_processor_id();

	/* 对照 vmx_vcpu_load_vmcs()，vmx.c:1502-1510 */
	mini_vmwrite(HOST_TR_BASE,
		     (u64)&get_cpu_entry_area(cpu)->tss.x86_tss);
	mini_vmwrite(HOST_GDTR_BASE, (u64)get_current_gdt_ro());

	if (IS_ENABLED(CONFIG_IA32_EMULATION))
		mini_vmwrite(HOST_IA32_SYSENTER_ESP,
			     (u64)(cpu_entry_stack(cpu) + 1));
}

static void mini_vmx_set_constant_host_state(void)
{
	struct desc_ptr idt;
	u32 lo;

	/* 22.2.3：CR0/CR3/CR4（CR3/CR4 每次进入再刷新） */
	mini_vmwrite(HOST_CR0, read_cr0());
	mini_vmwrite(HOST_CR3, __read_cr3());
	mini_vmwrite(HOST_CR4, mini_host_cr4());

	/* 22.2.4：段选择子。x86_64：DS/ES 置空选择子（vmx.c:4350-4351） */
	mini_vmwrite(HOST_CS_SELECTOR, __KERNEL_CS);
	mini_vmwrite(HOST_DS_SELECTOR, 0);
	mini_vmwrite(HOST_ES_SELECTOR, 0);
	mini_vmwrite(HOST_SS_SELECTOR, __KERNEL_DS);
	mini_vmwrite(HOST_TR_SELECTOR, GDT_ENTRY_TSS * 8);

	/* IDT 基址：全机一份（KVM 在 vmx_hardware_setup 里同样只取一次） */
	store_idt(&idt);
	host_idt_base = idt.address;
	mini_vmwrite(HOST_IDTR_BASE, host_idt_base);

	/* SYSENTER：CS/EIP 取宿主 MSR；ESP 是 per-CPU 入口栈（见上） */
	lo = (u32)mini_rdmsr(MSR_IA32_SYSENTER_CS);
	mini_vmwrite(HOST_IA32_SYSENTER_CS, lo);
	mini_vmwrite(HOST_IA32_SYSENTER_EIP,
		     mini_rdmsr(MSR_IA32_SYSENTER_EIP));
	if (!IS_ENABLED(CONFIG_IA32_EMULATION))
		mini_vmwrite(HOST_IA32_SYSENTER_ESP, 0);

	/* 宿主 EFER：配合 VM_EXIT_LOAD_IA32_EFER 从 VMCS 加载 */
	mini_vmwrite(HOST_IA32_EFER, mini_rdmsr(MSR_EFER));

	/* TR/GDT/SYSENTER_ESP 的 per-CPU 部分 */
	mini_vmx_fixup_host_for_cpu();

	/* FS/GS 基址（每次进入再刷新） */
	mini_vmwrite(HOST_FS_BASE, mini_rdmsr(MSR_FS_BASE));
	mini_vmwrite(HOST_GS_BASE,
		     (u64)cpu_kernelmode_gs_base(raw_smp_processor_id()));

	/*
	 * HOST_RIP = VM-Exit 着陆点（vmx_entry.S 内的全局符号），
	 * 对照 vmx.c:4361：vmcs_writel(HOST_RIP, (unsigned long)vmx_vmexit)。
	 * HOST_RSP 则由 mini_vmx_enter() 汇编在每次进入时写当前栈顶。
	 */
	mini_vmwrite(HOST_RIP, (unsigned long)mini_vmx_vmexit);
}

/*
 * ============================================================================
 * Guest 初始状态
 * ============================================================================
 *
 * Guest 从 64 位（IA-32e）模式直接开始执行，这是与真实 KVM 的最大教学
 * 简化（KVM 的 vCPU 从实模式复位状态起步，vmx_vcpu_reset() vmx.c:4883-）。
 *
 * IA-32e 进入检查（SDM 27.3.1.2）要求：
 *   - CR0: PE=1, PG=1（本 CPU FIXED0=0x80000021 ⊆ 0x80010033，本机实证）
 *   - CR4: PAE=1, VMXE=0
 *   - EFER.LMA=1（配合 VM_ENTRY_LOAD_IA32_EFER 写入 0x501）
 *   - CS: L=1, D=0（AR=0xA09B）
 *   - TR 必须可用，类型为 64 位忙 TSS（AR=0x8B）
 *   - 其余段/LDTR 可标记不可用（AR=0x10000）
 */

#define MINI_GUEST_CR0		(X86_CR0_PE | X86_CR0_MP | X86_CR0_ET | \
				 X86_CR0_NE | X86_CR0_WP | X86_CR0_PG)
#define MINI_GUEST_CR4		X86_CR4_PAE
#define MINI_GUEST_EFER		(EFER_SCE | EFER_LME | EFER_LMA)

#define MINI_SEG_AR_LDT_UNUSABLE	0x10000
#define MINI_SEG_AR_CS64	0xA09B	/* P=1 L=1 S=1 type=0xB，G=1 */
#define MINI_SEG_AR_TSS64	0x8B	/* P=1 type=0xB（64 位忙 TSS） */

static void mini_vmx_set_guest_state(void)
{
	/* 入口寄存器（常量见 mini-kvm.h 注释） */
	mini_vmwrite(GUEST_RIP, MINI_KVM_GUEST_RIP);
	mini_vmwrite(GUEST_RSP, MINI_KVM_GUEST_RSP);
	mini_vmwrite(GUEST_CR3, MINI_KVM_GUEST_CR3);
	mini_vmwrite(GUEST_RFLAGS, X86_EFLAGS_FIXED);	/* 0x2 */

	mini_vmwrite(GUEST_CR0, MINI_GUEST_CR0);
	mini_vmwrite(GUEST_CR4, MINI_GUEST_CR4);
	mini_vmwrite(GUEST_IA32_EFER, MINI_GUEST_EFER);
	mini_vmwrite(GUEST_DR7, 0x400);			/* SDM 27.2 */
	mini_vmwrite(GUEST_INTERRUPTIBILITY_INFO, 0);
	mini_vmwrite(GUEST_PENDING_DBG_EXCEPTIONS, 0);
	mini_vmwrite(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_ACTIVE);
	mini_vmwrite(GUEST_SYSENTER_CS, 0);
	mini_vmwrite(GUEST_SYSENTER_ESP, 0);
	mini_vmwrite(GUEST_SYSENTER_EIP, 0);

	/*
	 * 段寄存器：64 位平坦布局。CS 必须可用；SS/DS/ES/FS/GS/LDTR
	 * 标记不可用（IA-32e 下允许，SDM 27.3.1.2）；TR 必须可用。
	 * Guest 自己会执行 lidt 装载 IDT（GDTR 则完全不用，无远跳转）。
	 */
	mini_vmwrite(GUEST_CS_SELECTOR, 0x8);
	mini_vmwrite(GUEST_CS_BASE, 0);
	mini_vmwrite(GUEST_CS_LIMIT, 0xffffffff);
	mini_vmwrite(GUEST_CS_AR_BYTES, MINI_SEG_AR_CS64);

	mini_vmwrite(GUEST_SS_SELECTOR, 0);
	mini_vmwrite(GUEST_SS_AR_BYTES, MINI_SEG_AR_LDT_UNUSABLE);
	mini_vmwrite(GUEST_DS_SELECTOR, 0);
	mini_vmwrite(GUEST_DS_AR_BYTES, MINI_SEG_AR_LDT_UNUSABLE);
	mini_vmwrite(GUEST_ES_SELECTOR, 0);
	mini_vmwrite(GUEST_ES_AR_BYTES, MINI_SEG_AR_LDT_UNUSABLE);
	mini_vmwrite(GUEST_FS_SELECTOR, 0);
	mini_vmwrite(GUEST_FS_AR_BYTES, MINI_SEG_AR_LDT_UNUSABLE);
	mini_vmwrite(GUEST_GS_SELECTOR, 0);
	mini_vmwrite(GUEST_GS_AR_BYTES, MINI_SEG_AR_LDT_UNUSABLE);
	mini_vmwrite(GUEST_LDTR_SELECTOR, 0);
	mini_vmwrite(GUEST_LDTR_AR_BYTES, MINI_SEG_AR_LDT_UNUSABLE);

	/*
	 * FS/GS 的 base 是独立的 64 位字段，选择子为空也照样参与寻址
	 * （IA-32e 下 %fs/%gs 用 base 而非选择子）。VMCLEAR 之后 VMCS 字段
	 * 的初值不可依赖，必须显式清零，否则 guest 一旦碰到 %fs/%gs 就会
	 * 走到一个随机的 GPA 上。
	 */
	mini_vmwrite(GUEST_FS_BASE, 0);
	mini_vmwrite(GUEST_GS_BASE, 0);

	mini_vmwrite(GUEST_TR_SELECTOR, 0x10);
	mini_vmwrite(GUEST_TR_BASE, 0);
	mini_vmwrite(GUEST_TR_LIMIT, 0x67);	/* 64 位 TSS 大小 */
	mini_vmwrite(GUEST_TR_AR_BYTES, MINI_SEG_AR_TSS64);

	mini_vmwrite(GUEST_GDTR_BASE, 0);
	mini_vmwrite(GUEST_GDTR_LIMIT, 0xffff);
	mini_vmwrite(GUEST_IDTR_BASE, 0);	/* guest.S 会 lidt 到 0x2000 */
	mini_vmwrite(GUEST_IDTR_LIMIT, 0xffff);
}

/*
 * ============================================================================
 * 调试：dump 关键 VMCS 字段
 * ============================================================================
 */

static const u32 dump_fields[] = {
	PIN_BASED_VM_EXEC_CONTROL, CPU_BASED_VM_EXEC_CONTROL,
	SECONDARY_VM_EXEC_CONTROL, VM_EXIT_CONTROLS, VM_ENTRY_CONTROLS,
	EXCEPTION_BITMAP, EPT_POINTER, VMCS_LINK_POINTER,
	GUEST_RIP, GUEST_RSP, GUEST_CR0, GUEST_CR3, GUEST_CR4,
	GUEST_IA32_EFER, GUEST_RFLAGS, GUEST_CS_SELECTOR, GUEST_CS_AR_BYTES,
	GUEST_TR_AR_BYTES, GUEST_ACTIVITY_STATE, GUEST_INTERRUPTIBILITY_INFO,
	HOST_RIP, HOST_RSP, HOST_CR0, HOST_CR3, HOST_CR4,
	HOST_CS_SELECTOR, HOST_SS_SELECTOR, HOST_TR_SELECTOR,
	HOST_TR_BASE, HOST_GDTR_BASE, HOST_IDTR_BASE,
	HOST_GS_BASE, HOST_IA32_SYSENTER_ESP, HOST_IA32_EFER,
};

void mini_dump_vmcs(const char *when)
{
	int i;

	pr_info("mini-kvm: ===== VMCS dump (%s) =====\n", when);
	for (i = 0; i < ARRAY_SIZE(dump_fields); i++) {
		u64 v = 0;

		if (!mini_vmread(dump_fields[i], &v))
			pr_info("mini-kvm:   field 0x%04x = 0x%016llx\n",
				dump_fields[i], v);
	}
}

/*
 * ============================================================================
 * vCPU VMCS 建立 / 拆除
 * ============================================================================
 */

int mini_vcpu_vmx_setup(struct mini_kvm_vcpu *vcpu)
{
	struct mini_vmcs_controls c;
	struct page *page;
	int r;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;
	vcpu->vmcs_page = page;
	vcpu->vmcs_phys = page_to_phys(page);

	/* VMCS 首页写 revision id（对照 alloc_vmcs_cpu()，vmx.c:2902-2923） */
	*(u32 *)page_address(page) = mk_global.vmcs_revision_id;

	r = mini_vmclear(vcpu->vmcs_phys);	/* 对照 vmcs_clear() */
	if (r)
		goto err_free;

	r = mini_vmptrld(vcpu->vmcs_phys);
	if (r)
		goto err_free;
	vcpu->loaded_cpu = raw_smp_processor_id();

	r = mini_compute_controls(&c);
	if (r)
		goto err_clear;

	/* ---- 控制域 ---- */
	mini_vmwrite(PIN_BASED_VM_EXEC_CONTROL, c.pin_based);
	mini_vmwrite(CPU_BASED_VM_EXEC_CONTROL, c.cpu_based);
	mini_vmwrite(SECONDARY_VM_EXEC_CONTROL, c.secondary);
	mini_vmwrite(VM_EXIT_CONTROLS, c.exit);
	mini_vmwrite(VM_ENTRY_CONTROLS, c.entry);

	/*
	 * 异常位图：捕获 #DB/#UD/#GP/#PF，出错时能看到退出信息而不是
	 * 无声地三重故障（对照 init_vmcs() 的杂项字段，vmx.c:4739-4784）。
	 */
	mini_vmwrite(EXCEPTION_BITMAP,
		     (1u << 1) | (1u << 6) | (1u << 13) | (1u << 14));
	mini_vmwrite(PAGE_FAULT_ERROR_CODE_MASK, 0);
	mini_vmwrite(PAGE_FAULT_ERROR_CODE_MATCH, 0);
	mini_vmwrite(CR3_TARGET_COUNT, 0);			/* 22.2.1 */
	mini_vmwrite(VMCS_LINK_POINTER, INVALID_GPA);		/* 22.3.1.5 */
	mini_vmwrite(TPR_THRESHOLD, 0);

	/* EPTP：WB 内存类型 + 4 级行走（SDM 25.6.11；值由 ept.c 最终填写） */
	mini_vmwrite(EPT_POINTER, vcpu->kvm->ept.eptp);

	/* ---- Guest / Host 状态 ---- */
	mini_vmx_set_guest_state();
	mini_vmx_set_constant_host_state();

	/*
	 * 初始化完就把它从当前 CPU 上卸下（launch state 也回到 "clear"）。
	 * 为什么要卸：唯一"必然会重新加载 VMCS"的时机就是运行循环里的迁移
	 * 分支，而那次 VMPTRLD 之后会紧跟一次 all-context INVEPT —— 这条失效
	 * 必须发生在真正要跑这个 guest 的 CPU 上才有效。建立 VMCS 的线程随时
	 * 可能被调度走，所以不能把"第一次加载"留在初始化里。
	 * KVM 的分工正好一样：alloc_vmcs_cpu() 只做 vmcs_clear() 做初始化
	 * （vmx.c:2902-2923），真正的 vmcs_load() 在 vmx_vcpu_load_vmcs()
	 * 里、即 vCPU 上机时发生（vmx.c:1473-1476）。
	 */
	mini_vmcs_clear(vcpu);

	pr_info("mini-kvm: VMCS 初始化完成 (vcpu=%d rev=0x%x pin=%08x cpu=%08x sec=%08x exit=%08x entry=%08x)\n",
		vcpu->vcpu_id, mk_global.vmcs_revision_id,
		c.pin_based, c.cpu_based, c.secondary, c.exit, c.entry);
	return 0;

err_clear:
	/*
	 * 建立过程没有 preempt_disable，线程可能已经换到别的 CPU，
	 * 所以这里不能就地 VMCLEAR，要交给 mini_vmcs_clear() 投递回 loaded_cpu。
	 */
	mini_vmcs_clear(vcpu);
err_free:
	__free_page(page);
	vcpu->vmcs_page = NULL;
	return r;
}

void mini_vcpu_vmx_teardown(struct mini_kvm_vcpu *vcpu)
{
	if (!vcpu->vmcs_page)
		return;

	/*
	 * VMCLEAR 必须落在最后持有该 VMCS 的那个 CPU 上（见 mini_vmcs_clear()
	 * 里 SDM 25.11.1 的引文）。原实现在本 CPU 不等于 loaded_cpu 时只打印
	 * 警告就把 VMCS 页 free 掉，等于把一份可能仍 active 的 VMCS 留在旧 CPU
	 * 上 —— 之后同一物理页被伙伴系统复用、又被别的 vCPU 当 VMCS 用时，
	 * 旧 CPU 的片上副本会回写污染这块内存。
	 */
	mini_vmcs_clear(vcpu);

	__free_page(vcpu->vmcs_page);
	vcpu->vmcs_page = NULL;
}

/*
 * 进入失败时的诊断：读 VM_INSTRUCTION_ERROR（SDM 31.4 错误码表）。
 */
void mini_vmx_report_error(const char *what)
{
	pr_err("mini-kvm: %s 失败, VM_INSTRUCTION_ERROR=%llu\n",
	       what, mini_vmx_instruction_error());
}
