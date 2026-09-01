// SPDX-License-Identifier: GPL-2.0
/*
 * main.c —— mini-kvm 模块入口：能力探测、per-CPU VMXON/VMXOFF、设备注册
 *
 * 【Stage 对应】Stage 1 VMX 基础：把整机带入/带出 VMX 操作模式。
 *
 * 流程对照 Linux 6.12.93 KVM：
 *   模块加载 = 合并了 kvm.ko 与 kvm_intel.ko 的初始化：
 *     1. 读 VMX 能力 MSR                → vmx_hardware_setup() 的 vmcs_config 采集（简化）
 *     2. 每 CPU CR4.VMXE + VMXON        → kvm_cpu_vmxon()          (vmx.c:2833-2851)
 *        CR4.VMXE 预检返回 -EBUSY       → vmx_enable_virtualization_cpu() (vmx.c:2859-2860)
 *     3. 注册 /dev/mini-kvm (misc 设备)  → KVM 注册 /dev/kvm（kvm_main.c kvm_dev_init）
 *   模块卸载 = 相反顺序：
 *     VMXOFF + 清 CR4.VMXE              → kvm_cpu_vmxoff()         (arch/x86/kvm/vmx/vmx.c:743-755)
 *
 * SDM（Vol.3C，Order Number 326019-083US）：
 *   §24.7 Enabling and Entering VMX Operation —— VMXON/VMXOFF 的职责与
 *         IA32_FEATURE_CONTROL 的 lock/bit1/bit2 三门
 *   §24.8 Restrictions on VMX Operation —— 进入 VMX 后 CR4.VMXE 不可清等
 *   §25.11.5 VMXON Region —— 首页 bits 30:0 必须写 VMCS revision identifier
 *         （字段格式本身在 §25.2 "Format of the VMCS Region"）
 *   §31.3 VMX Instructions —— VMXON 的完整 Operation 伪码，含 #UD/#GP(0)/
 *         VMfailInvalid 三档失败条件，是本文件 mini_cpu_vmxon() 的依据
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <asm/io.h>
#include <asm/tlbflush.h>	/* cr4_set_bits()/cr4_clear_bits()（tlbflush.h:41/:51） */
#include <asm/asm.h>		/* _ASM_EXTABLE（asm.h:226-227） */
#include <asm/msr-index.h>
#include <asm/cpufeature.h>
#include <asm/vmx.h>

#include "mini-kvm.h"

struct mini_kvm_global mk_global;

/* per-CPU VMXON 区域与"已 VMXON"标志 */
static DEFINE_PER_CPU(struct page *, mini_vmxon_page);
static DEFINE_PER_CPU(bool, mini_vmxon_done);
static atomic_t vmx_enable_count;

/* mini_rdmsr()/mini_wrmsr() 见 mini-kvm.h */

static inline bool mini_cpu_has_vmx(void)
{
	u32 eax, ebx, ecx, edx;

	asm volatile("cpuid"
		     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		     : "0"(1), "2"(0));
	return ecx & (1u << 5);		/* CPUID.1:ECX.VMX = bit 5 */
}

/*
 * **当前 CPU** 能否安全执行 VMXON —— 必须逐 CPU 问，不能只问一次。
 *
 * CPUID 与 IA32_FEATURE_CONTROL(MSR 0x3A) 都是每个逻辑处理器独立的
 * 状态，SDM §31.3 "VMXON—Enter VMX Operation" 的 Operation 伪码把
 *   "bit 0 (lock bit) of IA32_FEATURE_CONTROL MSR is clear" 和
 *   "outside SMX operation and bit 2 of IA32_FEATURE_CONTROL MSR is clear"
 * 直接列为 #GP(0) 条件（同一条伪码里还有 CPL>0、A20M 模式、CR0/CR4 不是
 * VMX 支持的组合）。在别的 CPU 上读到的值不能替本机作保。
 *
 * 对照 KVM 的两层结构：模块加载时对每个在线 CPU 下发一次
 * smp_call_function_single(cpu, kvm_x86_check_cpu_compat)（x86.c:9828，回调
 * 见 x86.c:9736-9739 → :9733 → __kvm_is_vmx_supported()，
 * vmx.c:2782-2798），而
 * kvm_arch_hardware_enable() 在真正要 VMXON 的那台 CPU 上、执行 VMXON
 * 之前又跑一遍同样的检查（x86.c:12694）。
 */
static bool mini_cpu_vmx_supported(void)
{
	u64 feat;

	if (!mini_cpu_has_vmx()) {
		pr_err("mini-kvm: CPU%d 不支持 VMX (CPUID.1:ECX[5]=0)\n",
		       raw_smp_processor_id());
		return false;
	}

	feat = mini_rdmsr(MSR_IA32_FEAT_CTL);
	if ((feat & (FEAT_CTL_LOCKED | FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX)) !=
	    (FEAT_CTL_LOCKED | FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX)) {
		pr_err("mini-kvm: CPU%d BIOS 未开放 VMX, MSR_IA32_FEAT_CTL=0x%llx\n",
		       raw_smp_processor_id(), feat);
		return false;
	}

	return true;
}

/*
 * 在当前 CPU 上执行 VMXON，把**所有**失败都收敛成返回值。
 *
 * 关键是那条 _ASM_EXTABLE。SDM §31.3 "VMXON—Enter VMX Operation" 的
 * Operation 伪码 + Protected Mode Exceptions 清单列出了这些失败：
 *   #UD    —— 伪码开头共五条：操作数是寄存器、CR0.PE=0、CR4.VMXE=0、
 *             RFLAGS.VM=1、(IA32_EFER.LMA=1 且 CS.L=0)。内核 64 位上下文里
 *             PE=1 / VM=0 / CS.L=1，本函数又用内存操作数、调用者先置好
 *             VMXE，所以这条路理论上走不到。
 *   #GP(0) —— CPL>0、A20M 模式、CR0/CR4 的 fixed 位不符（§24.8）、
 *             IA32_FEATURE_CONTROL 不支持当前模式进入 VMX；
 *             **还包括访问那个内存操作数本身**：有效地址越出 CS/DS/ES/FS/GS
 *             段界限、DS/ES/FS/GS 是不可用段、源操作数落在只执行代码段。
 *   #PF / #SS —— 读操作数时页错误 / 越出 SS 界限。
 * 也就是说 VMXON 的 fault 面比"BIOS 没开 VMX"宽得多，光靠预检收不住。
 * 本回调跑在 IPI 的硬中断上下文里，任何一次 fault 都是宿主当场 die()，
 * 其余 CPU 还会卡在 smp_call_function_single 的等待里 —— 那是整机下线，
 * 不是"模块加载失败"。对照 KVM：kvm_cpu_vmxon()（vmx.c:2833-2851）同样给
 * VMXON 挂了 _ASM_EXTABLE，失败时清掉 CR4.VMXE 再返回 -EFAULT。
 *
 * 顺带纠正一个常见误解：**已经在 VMX root operation 时再执行 VMXON 不
 * #UD**，伪码最后一条是 VMfail("VMXON executed in VMX root operation") =
 * §31.4 Table 31-1 错误号 15。
 * 所以拦"和别的 VMM 互踩"不能指望指令自己报错，必须像下面调用者那样先查
 * CR4.VMXE（对照 vmx_enable_virtualization_cpu()，vmx.c:2859-2860）。
 *
 * 失败标志两档都要收（SDM §31.2）：VMfailInvalid = CF=1/ZF=0；
 * VMfailValid = ZF=1/CF=0 并写 VM_INSTRUCTION_ERROR。§31.2 的 VMfail 伪函数
 * 是"current VMCS 指针有效 ⇒ VMfailValid，否则 VMfailInvalid"，而
 * "已 VMXON 且已 VMPTRLD 过之后再 VMXON"正好落在前者 —— ZF 这一档是真实
 * 存在的，不是防御性冗余。
 */
static int mini_cpu_vmxon(u64 phys)
{
	asm goto("1: vmxon %[ptr]\n\t"
		 "jz  %l[vmfail]\n\t"
		 "jc  %l[vmfail]\n\t"
		 _ASM_EXTABLE(1b, %l[fault])
		 :
		 : [ptr] "m"(phys)
		 : "cc", "memory"
		 : vmfail, fault);

	return 0;

vmfail:
	pr_err("mini-kvm: CPU%d VMXON VMfail（ZF 或 CF 置位）——指针未 4KB 对齐/超出物理地址宽度，或首页 revision 不匹配（SDM §31.3）\n",
	       raw_smp_processor_id());
	/*
	 * VMfailValid 才有错误号可读；没有 current VMCS 时这条 VMREAD 自己也会
	 * VMfailInvalid，mini_vmx_report_error() 会打印 "err=0" 并说明原因。
	 */
	mini_vmx_report_error("VMXON");
	return -EIO;

fault:
	pr_err("mini-kvm: CPU%d VMXON 同步触发 #UD/#GP，已由 exception table 收口（SDM §31.3 的 fault 条件）\n",
	       raw_smp_processor_id());
	return -EIO;
}

/*
 * 读取 VMX 能力 MSR。对照 KVM vmx_hardware_setup() 对 vmcs_config 的
 * 采集（vmx.c），这里只保留 mini-kvm 用到的部分。
 */
int mini_vmx_read_capabilities(void)
{
	u64 basic;

	/*
	 * 这一处只是加载期的早退：跑在 insmod 线程的当前 CPU 上（可能
	 * 迁移），只保证"这台"能问准。权威判定在 mini_vmx_enable_one()
	 * 里 —— 那里是 IPI 回调，跑在真正要执行 VMXON 的那台 CPU 上。
	 */
	if (!mini_cpu_vmx_supported())
		return -ENODEV;

	basic = mini_rdmsr(MSR_IA32_VMX_BASIC);
	mk_global.vmcs_revision_id = (u32)(basic & 0x7fffffffULL);
	mk_global.true_ctls = !!(basic & (1ULL << 55));
	mk_global.ept_vpid_cap = mini_rdmsr(MSR_IA32_VMX_EPT_VPID_CAP);

	/*
	 * EPT 能力校验。对照 vmx_hardware_setup()（vmx.c:8434-8438）：
	 * KVM 用 cpu_has_vmx_ept_4levels() / cpu_has_vmx_ept_mt_wb() /
	 * cpu_has_vmx_invept_global() 决定是否关掉 enable_ept（谓词都是
	 * EPT_VPID_CAP 的位测试，见 capabilities.h:297-344）。
	 *
	 * 与 KVM 的差异：KVM 关掉 EPT 还能退回影子页表继续跑；mini-kvm 只有
	 * EPT 一条路（guest 自己的 Stage-1 页表不是 EPT），所以这里直接拒绝
	 * 加载，而不是悄悄带着一个被硬件忽略的 EPT_POINTER 跑起来。
	 *
	 * all-context INVEPT 同样是硬条件：mini-kvm 唯一的失效点就是运行循环
	 * 里每次重新 VMPTRLD 之后那一发 all-context（vcpu.c 的迁移分支，对照
	 * vmx_vcpu_load_vmcs() 的 KVM_REQ_TLB_FLUSH，vmx.c:1493-1496）。没有
	 * 它就没有任何手段清掉新 pCPU 上残留的 guest-physical 映射 —— KVM
	 * 给出的正是同一条判据。single-context 那型我们完全用不到（KVM 只有
	 * vmx_flush_tlb_current() 一处用它，vmx.c:3244），日志里打印支持与否。
	 */
	if (!(mk_global.ept_vpid_cap & VMX_EPT_PAGE_WALK_4_BIT) ||
	    !(mk_global.ept_vpid_cap & VMX_EPTP_WB_BIT) ||
	    !(mk_global.ept_vpid_cap & VMX_EPT_EXTENT_GLOBAL_BIT)) {
		pr_err("mini-kvm: EPT 能力不足（EPT_VPID_CAP=0x%llx，需 4 级行走 + WB 内存类型 + all-context INVEPT）\n",
		       mk_global.ept_vpid_cap);
		return -ENODEV;
	}

	pr_info("mini-kvm: VMX_BASIC=0x%llx revision=0x%x true_ctls=%d\n",
		basic, mk_global.vmcs_revision_id, mk_global.true_ctls);
	pr_info("mini-kvm: EPT_VPID_CAP=0x%llx INVEPT(context=%d global)\n",
		mk_global.ept_vpid_cap,
		!!(mk_global.ept_vpid_cap & VMX_EPT_EXTENT_CONTEXT_BIT));

	/*
	 * CR0 的 fixed 位在 Appendix A.7、CR4 的在 Appendix A.8（§24.8 里
	 * 这两句是分开的："...consult the VMX capability MSRs
	 * IA32_VMX_CR0_FIXED0 and IA32_VMX_CR0_FIXED1 (see Appendix A.7).
	 * For CR4, ...(see Appendix A.8)"）。它们决定 VMX 下哪些位必须为 1、
	 * 哪些位不能为 1，写进 VMCS 的 host CR0/CR4 由 §27.2.2 的进入检查
	 * 把关、guest 的由 §27.3.1.1 把关。
	 * 打出来作为 vmx.c 里 MINI_GUEST_CR0/CR4 与 mini_host_cr4() 取值的
	 * 实证依据。
	 */
	pr_info("mini-kvm: CR0 FIXED0=0x%08x FIXED1=0x%08x CR4 FIXED0=0x%08x FIXED1=0x%08x\n",
		(u32)mini_rdmsr(MSR_IA32_VMX_CR0_FIXED0),
		(u32)mini_rdmsr(MSR_IA32_VMX_CR0_FIXED1),
		(u32)mini_rdmsr(MSR_IA32_VMX_CR4_FIXED0),
		(u32)mini_rdmsr(MSR_IA32_VMX_CR4_FIXED1));
	return 0;
}

/*
 * 在单个 CPU 上进入 VMX 操作模式（on_each_cpu 回调，IRQ 关闭）。
 *
 * 对照 kvm_cpu_vmxon()（arch/x86/kvm/vmx/vmx.c:2833-2851）+
 * vmx_enable_virtualization_cpu() 的 CR4.VMXE 预检（同文件 :2859-2860）：
 *   - 先在本 CPU 上问一遍 CPUID/IA32_FEATURE_CONTROL（mini_cpu_vmx_supported()），
 *     这是 KVM 在 x86.c:12694 里的顺序：判定和 VMXON 在同一台 CPU 上背靠背；
 *   - CR4.VMXE 已置位说明已有 VMX 用户（典型是忘了卸载的 kvm_intel），
 *     报 -EBUSY 并拒绝，避免与真实 KVM 互踩。
 *
 * CR4.VMXE 必须用 cr4_set_bits()/cr4_clear_bits()（asm/tlbflush.h:41/:51，
 * 底层 cr4_update_irqsoff() 已导出，arch/x86/kernel/cpu/common.c:453-465），
 * **不能裸写 CR4**，理由有两层，都只在真机上才炸：
 *
 * 1. 内核把 CR4 的权威副本记在 per-CPU 影子 cpu_tlbstate.cr4 里，
 *    cr4_update_irqsoff() 是"从影子算新值 → 写回影子 → MOV 到 CR4"
 *    （common.c:455-462）。switch_mm_irqs_off() 每换一次 mm 都会经
 *    cr4_update_pce_mm()（arch/x86/mm/tlb.c:658 → :469-482）动
 *    X86_CR4_PCE。VMXE 只写进真实 CR4 而没进影子的话，下一次 PCE 变化就会
 *    用"没有 VMXE 的影子值"去 MOV CR4 —— 而 §24.8 第一条：
 *    "Any attempt to set one of these bits to an unsupported value while in
 *    VMX operation (including VMX root operation) using any of the CLTS,
 *    LMSW, or MOV CR instructions causes a general-protection exception"
 *    （NOTE 里列出 VMX operation 下必须为 1 的位含 CR4.VMXE）。也就是在进程
 *    切换里当场 #GP(0)，宿主 die()；就算硬件放行，VMX 也被偷偷关掉了。
 * 2. 上面那句 cr4_read_shadow() 预检读的就是这个影子（KVM 同样依赖它，
 *    common.c:467-472 导出 cr4_read_shadow）。裸写会让自己的 VMXE 在影子里
 *    不可见：第二次 insmod 拦不住，kvm_intel 随后加载也拦不住（它查的也是
 *    影子，vmx.c:2859-2860），于是两个 VMXON 用户叠在同一台 CPU 上。
 *
 * KVM 走的正是这条路：cr4_set_bits(X86_CR4_VMXE)（vmx.c:2837）、
 * cr4_clear_bits(X86_CR4_VMXE)（vmx.c:2848、kvm_cpu_vmxoff 里 :749/:753）。
 * 这两个 inline 只在关中断时写 CR4，我们的调用点在 IPI 回调里，满足
 * cr4_update_irqsoff() 的 lockdep_assert_irqs_disabled()。
 */
static void mini_vmx_enable_one(void *info)
{
	int cpu = raw_smp_processor_id();
	struct page *page = per_cpu(mini_vmxon_page, cpu);
	u64 phys = page_to_phys(page);

	if (atomic_read(&mk_global.enable_err))
		return;

	if (!mini_cpu_vmx_supported()) {
		atomic_set(&mk_global.enable_err, -ENODEV);
		return;
	}

	if (cr4_read_shadow() & X86_CR4_VMXE) {
		pr_err("mini-kvm: CPU%d CR4.VMXE 已置位（忘了卸载 kvm 模块?）\n",
		       cpu);
		atomic_set(&mk_global.enable_err, -EBUSY);
		return;
	}

	cr4_set_bits(X86_CR4_VMXE);

	/*
	 * VMXON 的操作数是 VMXON 区域的物理地址（内存操作数，SDM §24.7；
	 * 4KB 对齐与地址宽度约束见 §25.11.5）。mini_cpu_vmxon() 把 VMfail
	 * 和 #UD/#GP 都收敛成返回值 —— 在这个上下文里绝不能让 fault 逃出去。
	 */
	if (mini_cpu_vmxon(phys)) {
		cr4_clear_bits(X86_CR4_VMXE);
		atomic_set(&mk_global.enable_err, -EIO);
		return;
	}

	per_cpu(mini_vmxon_done, cpu) = true;
	atomic_inc(&vmx_enable_count);
}

/*
 * 在单个 CPU 上退出 VMX 操作模式。对照 kvm_cpu_vmxoff()
 * （arch/x86/kvm/vmx/vmx.c:743-755，理由写在它上面的注释 :735-741）：
 * VMXOFF 在 !post-VMXON 时 #UD，而
 * "impossible to atomically track post-VMXON state"，所以 KVM 用
 * _ASM_EXTABLE 吞掉所有 fault，并且无论成败都清 CR4.VMXE。
 *
 * 我们虽然按 per-CPU 标志精确追踪"已 VMXON"，正常路径不会撞 #UD，但这一条
 * 指令没有回头路：真 #UD 就是 rmmod 当场 oops，而 CR4.VMXE 还挂着、VMXON
 * 区域还被占用，模块再也退不干净。收进 exception table 才配得上"卸载一定
 * 要能干净"这个承诺，代价是一行宏。
 */
static void mini_cpu_vmxoff(void)
{
	asm goto("1: vmxoff\n\t"
		 "jz  %l[vmfail]\n\t"
		 _ASM_EXTABLE(1b, %l[fault])
		 :
		 :
		 : "cc", "memory"
		 : vmfail, fault);
	return;

vmfail:
	/*
	 * SDM §31.3 VMXOFF 的 Operation 伪码里，ZF=1 只有一种来源：
	 * "dual-monitor treatment of SMIs and SMM is active" →
	 * VMfail(23)（§31.4 Table 31-1 第 23 项）。这种情况下处理器仍在
	 * VMX 操作模式里，而下面照样会清 CR4.VMXE —— 那是 §24.8 明令禁止的
	 * 状态，只能靠日志发现，所以这里不能静默。
	 */
	pr_warn("mini-kvm: CPU%d VMXOFF VMfail(23)：dual-monitor treatment of SMIs and SMM 生效中\n",
		raw_smp_processor_id());
	return;

fault:
	pr_warn("mini-kvm: CPU%d VMXOFF #UD：本 CPU 其实不在 VMX 操作模式（§31.3）\n",
		raw_smp_processor_id());
}

static void mini_vmx_disable_one(void *info)
{
	int cpu = raw_smp_processor_id();

	if (!per_cpu(mini_vmxon_done, cpu))
		return;

	mini_cpu_vmxoff();

	/*
	 * 必须走 cr4_clear_bits()：真实 CR4 与 per-CPU 影子要一起清，否则影子里
	 * 残留的 VMXE 会让下一次 insmod 误报 -EBUSY。理由与 mini_vmx_enable_one()
	 * 上面那段同源；KVM 的 kvm_cpu_vmxoff() 也是 cr4_clear_bits()
	 * （arch/x86/kvm/vmx/vmx.c:749/:753）。
	 */
	cr4_clear_bits(X86_CR4_VMXE);

	per_cpu(mini_vmxon_done, cpu) = false;
	atomic_dec(&vmx_enable_count);
}

/*
 * 当前 CPU 是否已进入 VMX 操作模式。
 *
 * 为什么需要这个检查：mini_vmx_hardware_enable_all() 用 on_each_cpu()，只
 * 覆盖"加载那一刻在线"的 CPU（区域按 for_each_possible_cpu 分配，VMXON 却
 * 只在在线 CPU 上执行）。若 vCPU 线程后来被调度到一个加载时才离线、之后才
 * 上线的 CPU，那个 CPU 不在 VMX 操作模式里，VMPTRLD/VMLAUNCH 会直接 #UD
 * 打崩宿主。运行循环因此先问这一句，不在 VMX 模式就干净地拒绝进入。
 *
 * 真实 KVM 不需要这个检查，因为它在结构上消除了这种 CPU：
 * kvm_enable_virtualization() 注册 CPU 热插拔状态 CPUHP_AP_KVM_ONLINE
 * （kvm_main.c:5704），回调 kvm_online_cpu() 直接在"CPU 上线"这条路里执行
 * kvm_enable_virtualization_cpu()，失败就中止上线——源码注释写得很直白：
 * "Otherwise running VMs would encounter unrecoverable errors when
 * scheduled to this CPU"（kvm_main.c:5618-5626）；下线则由 kvm_offline_cpu()
 * 负责 VMXOFF（:5638-5642），并用 per-CPU 的 virtualization_enabled 记账
 * （:5630），对应本模块的 mini_vmxon_done。
 */
bool mini_cpu_in_vmx_operation(void)
{
	/*
	 * 调用约定：答案只描述**当前** CPU，所以调用者必须保证"问完到执行那条
	 * VMX 指令"之间不被换下本机，否则判断就失效。现有三个调用点都已满足：
	 * 运行循环（vcpu.c，全程 preempt_disable）、mini_vmclear_ipi()（IPI 回调
	 * 上下文）、mini_invept()（也在运行循环里）。单个 this_cpu_read 本身是
	 * %gs 相对寻址，读到的一定是执行它的那台 CPU 的值。
	 */
	return this_cpu_read(mini_vmxon_done);
}

int mini_vmx_hardware_enable_all(void)
{
	int cpu;

	/*
	 * 为每个可能 CPU 分配 VMXON 区域（一个 4KB 页），并在首页写入
	 * VMCS revision identifier —— §25.11.5 要求 bits 30:0 是 revision id
	 * 且 bit 31 清 0，格式定义在 §25.2；不匹配则 VMXON 直接 VMfailInvalid
	 * （SDM §31.3 VMXON 的 Operation 伪码）。对照 KVM 的 per-CPU
	 * vmxarea（vmx.c 的 alloc_kvm_area()）。
	 */
	for_each_possible_cpu(cpu) {
		struct page *page;

		page = alloc_pages_node(cpu_to_node(cpu),
					GFP_KERNEL | __GFP_ZERO, 0);
		if (!page) {
			int c;

			for_each_possible_cpu(c) {
				if (c == cpu)
					break;
				__free_page(per_cpu(mini_vmxon_page, c));
				per_cpu(mini_vmxon_page, c) = NULL;
			}
			return -ENOMEM;
		}
		*(u32 *)page_address(page) = mk_global.vmcs_revision_id;
		per_cpu(mini_vmxon_page, cpu) = page;
	}

	atomic_set(&mk_global.enable_err, 0);
	atomic_set(&vmx_enable_count, 0);
	on_each_cpu(mini_vmx_enable_one, NULL, 1);

	if (atomic_read(&mk_global.enable_err)) {
		int r = atomic_read(&mk_global.enable_err);

		on_each_cpu(mini_vmx_disable_one, NULL, 1);
		for_each_possible_cpu(cpu) {
			__free_page(per_cpu(mini_vmxon_page, cpu));
			per_cpu(mini_vmxon_page, cpu) = NULL;
		}
		return r;
	}

	mk_global.vmx_enabled = true;
	pr_info("mini-kvm: VMXON 完成，%d/%d 个 CPU 进入 VMX 操作模式\n",
		atomic_read(&vmx_enable_count), num_online_cpus());
	return 0;
}

void mini_vmx_hardware_disable_all(void)
{
	int cpu;

	if (!mk_global.vmx_enabled)
		return;

	on_each_cpu(mini_vmx_disable_one, NULL, 1);

	for_each_possible_cpu(cpu) {
		if (per_cpu(mini_vmxon_page, cpu)) {
			__free_page(per_cpu(mini_vmxon_page, cpu));
			per_cpu(mini_vmxon_page, cpu) = NULL;
		}
	}
	mk_global.vmx_enabled = false;
	pr_info("mini-kvm: VMXOFF 完成，CR4.VMXE 已清除\n");
}

/*
 * 模块加载 / 卸载。
 *
 * 注意加载前提：标准 kvm_intel 必须先卸载（否则 CR4.VMXE 预检 -EBUSY），
 * 且 /dev/kvm 无占用。完整的安全流程见 README 的 SDL 节。
 */
static int __init mini_kvm_init(void)
{
	int r;

	r = mini_vmx_read_capabilities();
	if (r)
		return r;

	r = mini_vmx_hardware_enable_all();
	if (r)
		return r;

	r = mini_vcpu_dev_init();
	if (r) {
		mini_vmx_hardware_disable_all();
		return r;
	}

	pr_info("mini-kvm: 模块已加载, /dev/mini-kvm 就绪\n");
	return 0;
}

static void __exit mini_kvm_exit(void)
{
	mini_vcpu_dev_exit();
	mini_vmx_hardware_disable_all();
	pr_info("mini-kvm: 模块已卸载，宿主恢复标准状态（建议随后 modprobe kvm_intel）\n");
}

module_init(mini_kvm_init);
module_exit(mini_kvm_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kvm-study project");
MODULE_DESCRIPTION("Educational minimal KVM: VMX + EPT + interrupt injection (phase8 capstone)");
