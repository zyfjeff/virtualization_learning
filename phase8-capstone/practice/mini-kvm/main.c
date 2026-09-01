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
 *     VMXOFF + 清 CR4.VMXE              → kvm_cpu_vmxoff()         (vmx.c:743-755)
 *
 * SDM：Vol.3 23.7 (VMXON)、23.8 (VMXOFF)、
 *      23.6 (VMXON region 首页写 VMCS revision identifier)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <asm/io.h>
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
 * 读取 VMX 能力 MSR。对照 KVM vmx_hardware_setup() 对 vmcs_config 的
 * 采集（vmx.c），这里只保留 mini-kvm 用到的部分。
 */
int mini_vmx_read_capabilities(void)
{
	u64 basic, feat;

	if (!mini_cpu_has_vmx()) {
		pr_err("mini-kvm: CPU 不支持 VMX (CPUID.1:ECX[5]=0)\n");
		return -ENODEV;
	}

	/*
	 * MSR_IA32_FEAT_CTL 预检，对照 __kvm_is_vmx_supported()
	 * (vmx.c:2782-2795)：必须已锁定且允许 SMX 外使用 VMX，
	 * 否则 VMXON 会 #GP（BIOS 未开放）。
	 */
	feat = mini_rdmsr(MSR_IA32_FEAT_CTL);
	if ((feat & (FEAT_CTL_LOCKED | FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX)) !=
	    (FEAT_CTL_LOCKED | FEAT_CTL_VMX_ENABLED_OUTSIDE_SMX)) {
		pr_err("mini-kvm: BIOS 未开放 VMX, MSR_IA32_FEAT_CTL=0x%llx\n",
		       feat);
		return -ENODEV;
	}

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
	 * CR0/CR4 的 fixed 位（SDM Appendix A.8）决定 VMX 下哪些位必须为 1、
	 * 哪些位不能为 1，写进 VMCS 的 guest 与 host CR0/CR4 都受它约束。
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
 * 对照 kvm_cpu_vmxon()（vmx.c:2833-2851）+ vmx_enable_virtualization_cpu()
 * 的 CR4.VMXE 预检（vmx.c:2859-2860）：
 *   - CR4.VMXE 已置位说明已有 VMX 用户（典型是忘了卸载的 kvm_intel），
 *     报 -EBUSY 并拒绝，避免与真实 KVM 互踩。
 *
 * 注意：cr4_set_bits()/cr4_clear_bits() 没有导出给模块，这里直接读写
 * CR4；IPI 上下文关中断，本 CPU 上的读-改-写是安全的。
 */
static void mini_vmx_enable_one(void *info)
{
	int cpu = raw_smp_processor_id();
	struct page *page = per_cpu(mini_vmxon_page, cpu);
	u64 phys = page_to_phys(page);
	u64 cr4;
	u8 err;

	if (atomic_read(&mk_global.enable_err))
		return;

	if (cr4_read_shadow() & X86_CR4_VMXE) {
		pr_err("mini-kvm: CPU%d CR4.VMXE 已置位（忘了卸载 kvm 模块?）\n",
		       cpu);
		atomic_set(&mk_global.enable_err, -EBUSY);
		return;
	}

	asm volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= X86_CR4_VMXE;
	asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

	/* VMXON：操作数是 VMXON 区域的物理地址（内存操作数），见 SDM 23.7 */
	asm volatile("vmxon %1; setna %0" : "=qm"(err) : "m"(phys) : "cc");
	if (err) {
		pr_err("mini-kvm: CPU%d VMXON 失败 (ZF/CF 置位)\n", cpu);
		cr4 &= ~X86_CR4_VMXE;
		asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
		atomic_set(&mk_global.enable_err, -EIO);
		return;
	}

	per_cpu(mini_vmxon_done, cpu) = true;
	atomic_inc(&vmx_enable_count);
}

/*
 * 在单个 CPU 上退出 VMX 操作模式。对照 kvm_cpu_vmxoff()（vmx.c:743-755）：
 * 无论 VMXOFF 成功与否都清 CR4.VMXE。
 *
 * 我们按 per-CPU 标志精确追踪"已 VMXON"，不会在非 VMX 模式执行 VMXOFF
 * （那会 #UD；KVM 用 _ASM_EXTABLE 吞异常是因为它可能在 NMI 上下文盲调）。
 */
static void mini_vmx_disable_one(void *info)
{
	int cpu = raw_smp_processor_id();
	u64 cr4;
	u8 err;

	if (!per_cpu(mini_vmxon_done, cpu))
		return;

	asm volatile("vmxoff; setna %0" : "=qm"(err) :: "cc", "memory");
	if (err)
		pr_warn("mini-kvm: CPU%d VMXOFF 失败 (ZF/CF 置位)\n", cpu);

	asm volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 &= ~X86_CR4_VMXE;
	asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

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
	 * VMCS revision identifier（SDM 23.6）。对照 KVM 的 per-CPU
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
