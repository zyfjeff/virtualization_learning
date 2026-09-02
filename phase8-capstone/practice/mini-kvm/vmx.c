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
 *   VMX 指令的 asm 包装风格        arch/x86/kvm/vmx/vmx_ops.h
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
 * 失败分两档（SDM Vol.3C §31.2 "Conventions"）：VMfailInvalid 置 CF=1/ZF=0，
 * VMfailValid 置 ZF=1/CF=0 并写 VM_INSTRUCTION_ERROR（0x4400）。两档互斥，
 * 所以包装统一用 setna（= ZF|CF）才能同时收到，只看 CF 会漏掉 VMfailValid。
 *
 * 这些包装只处理"VMfail"那一档。§31.3 每条指令的 Operation 伪码开头还有
 * 会**同步 fault** 的前置条件（not in VMX operation / CR0.PE = 0 /
 * RFLAGS.VM = 1 → #UD；CPL > 0 → #GP(0)），setna 收不到它们。那部分靠
 * 结构保证：调用点要么在 mini_cpu_in_vmx_operation() 之后（vcpu.c 运行
 * 循环、mini_vmclear_ipi()），要么在刚 VMXON 成功的同一段临界区里；
 * VMXON/VMXOFF 自己则用 exception table 收口，见 main.c 的
 * mini_cpu_vmxon() / mini_cpu_vmxoff()。
 *
 * 只有一个 mini_vmwrite()，16/32/64 位字段都用它，这是规范允许的：SDM 31.3
 * 的 VMWRITE 写着 "The effective size of the primary source operand ... is
 * always ... 64 bits in 64-bit mode. If the VMCS field specified by the
 * secondary source operand is shorter than this effective operand size, the
 * high bits of the primary source operand are ignored."（反过来说，字段比操作数
 * 长时高位清 0。）字段编码这个第二源操作数也可以是寄存器（Op/En = RM，
 * ModRM:r/m）。
 *
 * KVM 的做法完全一样：vmx_asm2() 展开成 vmwrite %1, %0，操作数搭配是
 * "r"(field) + "rm"(value)（arch/x86/kvm/vmx/vmx_ops.h:205-226），而
 * vmcs_write16()/vmcs_write32() 只是多了 vmcs_check16()/vmcs_check32() 这一层
 * **编译期字段宽度断言**，最后都调同一个 __vmcs_writel() 发 64 位 VMWRITE
 * （:223-243）。也就是说"16 位字段要用 16 位操作数"是想当然，规范没有这条。
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

	/* AT&T 语序：vmwrite <值>, <字段编码>（对照 vmx_ops.h:223-226 的
	 * __vmcs_writel()：vmx_asm2 展开成 vmwrite %1, %0，同样是值在前） */
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
 * "vcpu migration can race with cpu offline"（arch/x86/kvm/vmx/vmx.c:788-789）。
 * 我们照做，只补一条 pr_warn。
 *
 * 另一半修的是 VMRESUME/VMLAUNCH 选择：VMCLEAR 会把 launch state 置为
 * "clear"，所以迁移后的第一次进入必须用 VMLAUNCH。SDM 25.11.3 明说
 * "Since 'migrating' a VMCS from one logical processor to another requires
 * use of VMCLEAR (see Section 25.11.1), which sets the launch state of the
 * VMCS to 'clear', such migration requires the next VM entry to be performed
 * using VMLAUNCH"；忘了清 launched 标志就会用 VMRESUME 进一个 clear 状态的
 * VMCS，得到 VM-instruction error 5 "VMRESUME with non-launched VMCS"
 * （SDM 31.4 Table 31-1）。KVM 也在 __loaded_vmcs_clear() 里清这个标志：
 * VMCLEAR 在 arch/x86/kvm/vmx/vmx.c:793，loaded_vmcs->launched = 0 在 :809。
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

	/*
	 * 次级控制只有一个 MSR，没有 TRUE_* 变体。判据用可直接读到的内核
	 * MSR 编号清单（arch/x86/include/asm/msr-index.h:1182-1200）：TRUE_*
	 * 只有 0x48d-0x490 四个，对应 pin/proc/exit/entry；0x48b 的
	 * PROCBASED_CTLS2 与 0x492 的 CTLS3 都是单一编号。（规范里这一段
	 * 属 Vol.3C 转引的 Appendix A.3，附录不在本仓库这份 PDF 内。）
	 */
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
 * 为什么一定要保证 CR4.VMXE 为 1：规范只在"离开 VMX 操作"时才允许清它
 * —— §24.7 "Once in VMX operation, it is not possible to clear CR4.VMXE"
 * （§24.8 第一条 NOTE 把这些位列为 VMX 操作期间必须为 1 的位："The first
 * processors to support VMX operation require that the following bits be 1 in
 * VMX operation: CR0.PE, CR0.NE, CR0.PG, and CR4.VMXE"）。
 * 而 §27.2.2 对 host-state
 * 的 CR4 只做一件事："The CR4 field must not set any bit to a value not
 * supported in VMX operation" —— 也就是说 VMXE=0 **不会**被进入检查拦下来，
 * 硬件会带着一个"root 模式下 VMXE 为 0"的状态继续跑，那是规范没有定义的领域。
 * 直接把这一位钉住，就不去踩这条边界。
 *
 * 与 KVM 同构：KVM 用 cr4_set_bits(X86_CR4_VMXE) 开启 VMX 能力
 * （arch/x86/kvm/vmx/vmx.c:2837），影子 cpu_tlbstate.cr4 里已含该位，因此直接
 * `cr4 = cr4_read_shadow(); vmcs_writel(HOST_CR4, cr4)`（初始化 :4338-4341，
 * 每次 VM entry 前再刷一次 :7410-7413）。本模块进入 VMX 的路径现在也走
 * cr4_set_bits()（main.c::mini_vmx_enable_one），影子同样带 VMXE，下面这一次
 * `|` 只是保险：万一将来有调用点忘了开，HOST_CR4 仍然是自洽的。
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
	 * §25.5 把 CR0/CR3/CR4 列为 host-state 字段并明说 "processor state is
	 * loaded from these fields on every VM exit"（装载细节见 §28.5.1），
	 * 而"当前进程的 CR3/CR4 会变"是软件事实 —— 所以每次进入前都要刷新。
	 * KVM 把这件事
	 * 放在 vmx_vcpu_run() 的末尾、紧贴 VM-Entry（vmx.c:7397-7413），理由原文：
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

	/*
	 * §25.5 的 CR0/CR3/CR4（CR3/CR4 每次进入再刷新，见上面
	 * mini_vmx_refresh_host_state()）。
	 *
	 * 顺带说明这里曾出现过的 "22.2.3"/"22.2.4" 是哪来的：那是
	 * vmx_set_constant_host_state() 自己的注释（22.2.3 在 vmx.c:4328、
	 * :4335、:4340，22.2.4 在 :4343），引的是二十多年前初代 VMX 规范的
	 * 第 22 章，在 326019-083US 里 Vol.3C 从第 24 章开始，根本没有
	 * §22.x。抄 KVM 的行文可以，抄它的历史编号不行。
	 */
	mini_vmwrite(HOST_CR0, read_cr0());
	mini_vmwrite(HOST_CR3, __read_cr3());
	mini_vmwrite(HOST_CR4, mini_host_cr4());

	/*
	 * §25.5 的段选择子（装载见 §28.5.2，进入检查见 §27.2.3）。
	 * x86_64：DS/ES 置空选择子（对照 vmx.c:4350-4351）
	 */
	mini_vmwrite(HOST_CS_SELECTOR, __KERNEL_CS);
	mini_vmwrite(HOST_DS_SELECTOR, 0);
	mini_vmwrite(HOST_ES_SELECTOR, 0);
	mini_vmwrite(HOST_SS_SELECTOR, __KERNEL_DS);
	mini_vmwrite(HOST_TR_SELECTOR, GDT_ENTRY_TSS * 8);
	/*
	 * FS/GS 选择子必须显式写，即使我们只用基址寻址。
	 *
	 * §25.5 把 "Selector fields (16 bits each) for the segment registers
	 * CS, SS, DS, ES, FS, GS, and TR" 全列进宿主状态区，§28.5.2 也确实在
	 * 每次 VM-Exit 逐个装载选择子；而 §27.2.3 在进入时就检查
	 * "In the selector field for each of CS, SS, DS, ES, FS, GS, and TR,
	 * the RPL (bits 1:0) and the TI flag (bit 2) must be 0"，不满足就是
	 * VM-Entry 检查失败（Table 31-1 错误码 8 "VM entry with invalid
	 * host-state field(s)"）。VMCS 字段在被软件写过之前内容不可依赖，
	 * 所以"这里没写、大概是 0 吧"不能作为正确性依据 —— 唯一的例外是
	 * 我们恰好用了 __GFP_ZERO 的页，但那是实现巧合，不是规范承诺。
	 *
	 * 取 0（空选择子 = 段不可用）是安全的：§28.5.2 规定退出到 IA-32e 模式
	 * 时 "FS and GS ... otherwise, loaded from the base-address field"，
	 * 段可不可用不影响 FS.base/GS.base 的恢复，而宿主内核的 per-CPU 寻址
	 * 要的就是 GS 基址（见下面 refresh 里的 cpu_kernelmode_gs_base）。
	 *
	 * 对照 KVM：init_vmcs() 在调用 vmx_set_constant_host_state() 之前先把
	 * 两者写 0（arch/x86/kvm/vmx/vmx.c:4786-4787），之后
	 * vmx_set_host_fs_gs()（:1258-1275）在同步宿主 FS/GS
	 * 时同样把带 RPL/TI 位的选择子强制改成 0 才写进 VMCS —— 就是在躲
	 * §27.2.3 这条检查。
	 */
	mini_vmwrite(HOST_FS_SELECTOR, 0);
	mini_vmwrite(HOST_GS_SELECTOR, 0);

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
 * IA-32e 进入检查（控制寄存器/EFER 见 §27.3.1.1，段寄存器见 §27.3.1.2）：
 *   - CR0: PG=1 且 PE=1（§27.3.1.1："If the 'IA-32e mode guest' VM-entry
 *     control is 1, bit 31 in the CR0 field ... and bit 5 in the CR4 field
 *     ... must each be 1"）。另外 CR0 字段本身还要过 §24.8 的 fixed 位检查
 *     （见下条 CR4 的说明）；本模块在 insmod 时用 pr_info 打印运行时读到的
 *     IA32_VMX_CR0_FIXED0/1 与 CR4_FIXED0/1（main.c:221-225），上机对照即可
 *   - CR4: PAE=1，**并且必须带 VMXE=1**。别以为非根模式下 CR4.VMXE 该是 0：
 *     §24.8 的 NOTE 写的是 "The first processors to support VMX operation
 *     require that the following bits be 1 in VMX operation: CR0.PE, CR0.NE,
 *     CR0.PG, and CR4.VMXE"，而 "VMX operation" 包含 VMX root 与 non-root 两
 *     种状态；§24.8 正文还有一句 "VM entry or VM exit cannot set any of these
 *     bits to an unsupported value"，§27.3.1.1 则直接对 guest CR4 字段做这条
 *     检查（"The CR4 field must not set any bit to a value not supported in
 *     VMX operation (see Section 24.8)"）。unrestricted guest 只豁免 CR0.PE /
 *     CR0.PG，不豁免 CR4.VMXE。对照 KVM：`KVM_PMODE_VM_CR4_ALWAYS_ON` /
 *     `KVM_RMODE_VM_CR4_ALWAYS_ON` / `KVM_VM_CR4_ALWAYS_ON_UNRESTRICTED_GUEST`
 *     三个常量全都含 X86_CR4_VMXE（vmx.c:156-158），vmx_set_cr4() 无论走哪条
 *     分支都会或上它（vmx.c:3481-3487），最后 `vmcs_writel(GUEST_CR4,
 *     hw_cr4)`（vmx.c:3528）。漏这一位 = VM entry 直接失败，退出原因是 33
 *     "VM-entry failure due to invalid guest state"（§27.8）。
 *     副作用：我们没开 "use CR4 shadows"，所以 guest 读 CR4 会看到 VMXE=1
 *     （真实 KVM 用 CR4_READ_SHADOW 字段把它藏成 guest 认为的值，见
 *     vmx.c:3527）。本项目的 guest 从不读 CR4，可以接受。
 *   - EFER.LMA 必须等于 "IA-32e mode guest" 控制位，且在 CR0.PG=1 时必须与
 *     LME 相同（§27.3.1.1）—— 配合 VM_ENTRY_LOAD_IA32_EFER 写入 0x501
 *   - CS: 必须可用（§27.3.1.2 对 CS 强制 "bits 31:17 ... must all be 0"，
 *     其中 bit16 就是 unusable 位）、Type ∈ {9,11,13,15}。规范只要求
 *     "D/B must be 0 if the guest will be IA-32e mode and the L bit ... is 1"，
 *     **并不强制 L=1**（IA-32e mode guest=1 而 CS.L=0 会以兼容模式开始执行）；
 *     L=1 是我们要跑 64 位 guest 自己选的（AR=0xA09B）
 *   - TR: 必须可用（"Bit 16 (Unusable). The unusable bit must be 0"），且
 *     IA-32e 下 "the Type must be 11 (64-bit busy TSS)"（AR=0x8B）
 *   - 其余段/LDTR 可标记不可用（AR=0x10000，选择子为 0）
 */

#define MINI_GUEST_CR0		(X86_CR0_PE | X86_CR0_MP | X86_CR0_ET | \
				 X86_CR0_NE | X86_CR0_WP | X86_CR0_PG)
/* VMXE 不是笔误：见上面 §24.8 / §27.3.1.1 那条，KVM 同样强制这一位 */
#define MINI_GUEST_CR4		(X86_CR4_PAE | X86_CR4_VMXE)
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
	/*
	 * 这一写其实**不决定** guest 的 DR7：VM-Entry 只在 "load debug
	 * controls"（VM-entry 控制位 2，SDM Table 25-16）为 1 时才从 GUEST_DR7
	 * 装载 DR7（SDM 27.3.2.1），本模块没有开这一位。guest 拿到的是 VM-Exit
	 * 时硬件给宿主的固定值 —— "DR7 is set to 400H"（SDM 28.5.1），于是
	 * 所有局部/全局断言使能位都是 0，配合异常位图里的 #DB 捕获也不会误触
	 * 调试异常。KVM 正是看明白了这一点，初始化里根本不写 GUEST_DR7。
	 * 这里保留一次写入，只为 mini_dump_vmcs() 打出来的值确定可读。
	 */
	mini_vmwrite(GUEST_DR7, 0x400);
	mini_vmwrite(GUEST_INTERRUPTIBILITY_INFO, 0);
	mini_vmwrite(GUEST_PENDING_DBG_EXCEPTIONS, 0);
	mini_vmwrite(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_ACTIVE);
	mini_vmwrite(GUEST_SYSENTER_CS, 0);
	mini_vmwrite(GUEST_SYSENTER_ESP, 0);
	mini_vmwrite(GUEST_SYSENTER_EIP, 0);

	/*
	 * 段寄存器：64 位平坦布局。CS 必须可用；SS/DS/ES/FS/GS/LDTR
	 * 标记不可用（IA-32e 下允许，SDM 27.3.1.2）；TR 必须可用。
	 *
	 * 下面这组 GUEST_*GDTR/IDTR 初值只要过得了 §27.3.1 的检查（limit <
	 * 2^16 即可，base 没有约束），因为 VM entry 装载段与表寄存器是**直接
	 * 从 VMCS 字段读**，不做任何描述符访存（SDM 27.3.2）。但"不需要
	 * 描述符"只到进入 guest 为止：IDT 投递要按门里的 selector 真的去
	 * GDTR 取描述符，所以 guest 自己必须建一张 GDT 并 lgdt —— 见
	 * guest/guest.S 里的 gdt/gdt_ptr。
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
	/*
	 * 同一条规则补齐 LDTR 基址：它也是每次进入都装载的字段。我们的 LDTR
	 * 是空选择子（段不可用，任何穿越它的访问都 #GP），所以这不算修 bug，
	 * 只是把初值钉成确定的。对照 vmx_vcpu_reset() 里 LDTR 那一组
	 * （arch/x86/kvm/vmx/vmx.c:4913-4916），其中 :4914 就是
	 * vmcs_writel(GUEST_LDTR_BASE, 0)。
	 */
	mini_vmwrite(GUEST_LDTR_BASE, 0);

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
	mini_vmwrite(CR3_TARGET_COUNT, 0);
	/*
	 * CR3_TARGET_COUNT=0：不使用 CR3-target 机制（§25.6.7）。
	 * VMCS_LINK_POINTER=INVALID_GPA：字段定义见 §25.4.2，§27.3.1.5 的
	 * 那条检查开头就是 "The following checks apply if the field contains
	 * a value other than FFFFFFFF_FFFFFFFFH" —— 写全 1 正好绕开全部检查，
	 * 因为我们不做嵌套。（KVM 同样写 INVALID_GPA，vmx.c:4739。）
	 */
	mini_vmwrite(VMCS_LINK_POINTER, INVALID_GPA);
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
 * VM 指令失败时的诊断：读 VM_INSTRUCTION_ERROR 并按 SDM §31.4 Table 31-1 解码。
 * 调用点是 VM entry（vcpu.c）与 VMXON（main.c）两条路。
 *
 * 为什么值得专门解码：VM entry 的失败分成两档，两档都**不产生 VM-Exit**，
 * 只是"落到下一条指令"——§27.1 的基本检查（无 current VMCS / shadow VMCS 置
 * CF；被 MOV-SS 阻塞、launch state 与指令不匹配置 ZF 并写错误号）与 §27.2 的
 * 控制域/宿主状态检查（"control is passed to the next instruction, RFLAGS.ZF
 * is set to 1 ... and the VM-instruction error field is loaded with an error
 * number"）。vmx_entry.S 的判据是"VMRESUME/VMLAUNCH 居然返回了"，两档一网
 * 打尽，代价是 CF/ZF 本身不再区分二者 —— 能区分的只有这里的错误号。只有
 * §27.3.1/§27.4 那一档（guest 状态非法、MSR 装载失败）才会装载宿主状态并按
 * §27.8 记录 exit reason，那条路走 vcpu.c 的 bit 31 分支。
 */
void mini_vmx_report_error(const char *what)
{
	u64 err = mini_vmx_instruction_error();
	const char *why = "未知错误号";

	switch (err) {
	case 0:	why = "没有写入错误号——§27.1 第 3/4 条（无 current VMCS 或当前是 shadow VMCS）只置 CF 不写错误号，此时这条 VMREAD 自己也可能 VMfailInvalid"; break;
	case 4:	why = "VMLAUNCH 时 VMCS launch state 不是 clear（§27.1 第 5.b 条）"; break;
	case 5:	why = "VMRESUME 时 VMCS launch state 不是 launched（§27.1 第 5.c 条）"; break;
	case 6:	why = "VMLAUNCH 与 VMRESUME 之间发生过 VMXOFF"; break;
	case 7:	why = "VM entry 的控制域非法（§27.2.1）"; break;
	case 8:	why = "VM entry 的宿主状态域非法（§27.2.2/§27.2.3/§27.2.4）"; break;
	case 12: why = "VMREAD/VMWRITE 访问了不支持的 VMCS 部件"; break;
	case 13: why = "VMWRITE 写了只读的 VMCS 部件"; break;
	case 15: why = "VMXON 在 VMX root operation 中再次执行（§31.3 伪码最后一条）——机器上已有 VMX 用户"; break;
	case 16: why = "VM entry 的 executive-VMCS 指针非法（嵌套相关，本模块不该出现）"; break;
	case 26: why = "VM entry 被 MOV SS 阻塞（§27.1 第 5.a 条）"; break;
	}

	pr_err("mini-kvm: %s 失败, VM_INSTRUCTION_ERROR=%llu：%s\n",
	       what, err, why);
}
