// SPDX-License-Identifier: GPL-2.0
/*
 * ept.c —— EPT 建立与按需映射（Stage 2）
 *
 * 与真实 KVM 的对照：KVM 6.12 用 TDP MMU（arch/x86/kvm/mmu/tdp_mmu.c）
 * 管理 EPT/影子页表；这里手写 4 级 EPT 页表，叶页一律 4KB。
 *
 * EPTP 格式（SDM 25.6.11，Table 25-9）：
 *   [2:0]   EPT paging-structure memory type —— 访问页表本身用的类型，WB=6
 *   [5:3]   EPT page-walk length - 1（4 级 = 3）
 *   [6]     是否启用 accessed/dirty flags（这里为 0）
 *   [51:12] PML4 物理页帧
 *
 * 叶条目与非叶条目的位布局不同，这是最容易写错的地方：
 *   非叶（Table 29-2 PML4E / 29-4 PDPTE / 29-6 PDE）：bits 7:3 是
 *     "Reserved (must be 0)" —— 中间层没有内存类型字段，只能写
 *     RWX(2:0) + 下一级页表物理地址。
 *   叶  （Table 29-7 的 4KB PTE；大页见 29-3 / 29-5）：bits 5:3 = 该页的
 *     EPT memory type（6=WB），bit 6 = "ignore PAT"。
 *
 * 本模块不置 bit 6（ignore PAT），有效类型仍是 WB，推导链如下：
 *   §29.3.7.2 —— bit 6 = 0 时，有效内存类型 = "the combination of the EPT
 *   memory type and the PAT memory type specified in Table 12-7 in Section
 *   12.5.2.2, using the EPT memory type in place of the MTRR memory type"
 *   （Table 12-7 不在本仓库这份 PDF 里 —— 它只含 Vol.3C，所以这里只按
 *   §29.3.7.2 的转述使用）；同节最后一句："The MTRRs have no effect on the
 *   memory type used for an access to a guest-physical address."
 *   测试程序建的 guest Stage-1 是 2MB 页且 PCD/PAT 位为 0（test-mini-kvm.c
 *   的 pd[0]=0x83）→ PAT 索引 0；模块未开 "load IA32_PAT"，非根模式下
 *   IA32_CR_PAT 还是宿主的值，Linux 启动时会把它打印出来，本机
 *   `dmesg | grep 'x86/PAT'` → "x86/PAT: Configuration [0-7]: WB WC UC- UC
 *   WB WP UC- WT"，即 PA0 = WB。WB × WB = WB。
 *   对照 KVM：只有 KVM_X86_QUIRK_IGNORE_GUEST_PAT 生效（且无非一致 DMA）
 *   时才置 VMX_EPT_IPAT_BIT，默认路径同样只写 WB<<3
 *   （vmx_get_mt_mask()，arch/x86/kvm/vmx/vmx.c:7679-7693）。
 *
 * 两个新手最易踩的坑（本项目骨架版曾犯）：
 *   1. EPT violation 的 GPA 来自 VMCS 的 GUEST_PHYSICAL_ADDRESS (0x2400)；
 *      EXIT_QUALIFICATION (0x6400) 存的是访问性质（读/写/取指、是否
 *      行走期错误），不含地址（SDM 28.2.1）。
 *   2. 首次映射无需 INVEPT。SDM 29.4.3.4 原话：
 *      "Because a logical processor does not cache any information derived
 *       from EPT paging-structure entries that are not present (see Section
 *       29.3.2) or misconfigured (see Section 29.3.3.1), it is not necessary
 *       to execute INVEPT following modification of an EPT paging-structure
 *       entry that had been not present or misconfigured."
 *      KVM 确实不在"建映射"这一侧发失效，但它另有三个发失效的时机：
 *      拆/降条目、拿到新的 EPT 根、vCPU 换 pCPU（vmx_flush_tlb_all()
 *      在 enable_ept 时下发 INVEPT global，arch/x86/kvm/vmx/vmx.c:3204-3216）。
 *      本文件下面的 INVEPT 一节按后两条给 mini-kvm 补齐。
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <asm/page.h>
#include <asm/io.h>
#include <asm/vmx.h>

#include "mini-kvm.h"

/* RWX 位在叶与非叶条目里都是 bits 2:0（Table 29-2 / 29-7） */
#define EPT_ENTRY_RWX	(EPT_PTE_READ | EPT_PTE_WRITE | EPT_PTE_EXEC)
/* 内存类型只在叶条目的 bits 5:3（Table 29-7）；非叶条目 bits 7:3 必须为 0 */
#define EPT_MEMTYPE_WB	(6ULL << 3)
#define EPT_PTE_ADDR_MASK	0x000FFFFFFFFFF000ULL

/*
 * ============================================================================
 * INVEPT
 * ============================================================================
 *
 * 先划清不需要失效的一侧：建映射。本文件头注释第 2 条引了 SDM 29.4.3.4 ——
 * 处理器不缓存 not-present / misconfigured 的 EPT 条目，所以 EPT violation
 * 里把叶子从"不存在"写成"存在"之后直接重进即可，不发 INVEPT。
 *
 * 需要失效的一侧是"同一个 EPTP 值下曾经存在过的映射可能还留在某台 CPU 的
 * TLB 里"。KVM 分两个时机处理：
 *   (a) 拿到新的 EPT 根页之后：kvm_mmu_load() 对新根发 flush_tlb_current，
 *       EPT 下即 single-context INVEPT（vmx_flush_tlb_current()，
 *       arch/x86/kvm/vmx/vmx.c:3243-3245 → ept_sync_context()）。理由写在
 *       mmu.c:5780-5787 —— 新根"来历未知"，页是刚从伙伴系统拿回来的。
 *   (b) vCPU 换 pCPU 之后：vmx_vcpu_load_vmcs() 下
 *       kvm_make_request(KVM_REQ_TLB_FLUSH, vcpu)（vmx.c:1493-1496，注释
 *       "the new pCPU may have stale TLB entries from its previous
 *       association with the vCPU"），该请求由 kvm_vcpu_flush_tlb_all()
 *       消费（arch/x86/kvm/x86.c:10828-10829）→ vmx_flush_tlb_all() →
 *       ept_sync_global()，即 all-context。
 *
 * mini-kvm 只用一个点覆盖这两条：**每次重新 VMPTRLD 之后**（vcpu.c 的迁移
 * 分支）。之所以够用，是因为本模块保证"任何一个跑过这个 guest 的 CPU 都必然
 * 先做过一次 VMPTRLD"—— mini_vcpu_vmx_setup() 初始化完就主动把 VMCS 卸下
 * （见该函数末尾的注释），第一次 KVM_RUN 也会走迁移分支。
 * KVM 那两个时机的粒度本来也不同：(a) 正好有新根的精确 EPTP，用
 * single-context 就够；(b) 只知道"这台 CPU 上可能残留过什么"，只能
 * all-context。mini-kvm 的失效点和 (b) 在同一处（刚做完 VMPTRLD 的那台
 * CPU），并且也要覆盖"上一个用过同一个 EPTP 值的人"留下的条目，所以统一
 * 用 all-context。
 *
 * 前置条件（§31.3 INVEPT 的 Operation）：
 *   not in VMX operation → #UD；non-root → VM exit；CPL > 0 → #GP(0)。
 * 没有"当前 VMCS 的 enable EPT 必须为 1"这类要求 —— Operation 里只有 type 1
 * 会校验 descriptor 里的 EPTP 是否合法（"IF VM entry with the 'enable EPT'
 * VM-execution control set to 1 would fail due to the EPTP value THEN
 * VMfail(Invalid operand to INVEPT/INVVPID)"），type 2 直接 "Invalidate
 * mappings associated with all EPTPs; VMsucceed"。
 * 但**不 VMXON 就执行会 #UD 打崩宿主**：本模块没有 CPU 热插拔通知器，一个
 * "加载之后才上线"的 CPU 不在 VMX 操作模式里，所以先问
 * mini_cpu_in_vmx_operation()。KVM 有通知器保证每个在线 CPU 都 VMXON
 * （kvm_main.c:5618-5626），才敢直接下发。
 */
static int mini_invept(u64 extent, u64 eptp, u64 gpa)
{
	struct { u64 eptp, gpa; } operand = { eptp, gpa };	/* Figure 31-1 */
	u8 err;

	if (!mini_cpu_in_vmx_operation())
		return -ENODEV;

	/* AT&T 语序：invept <descriptor(内存)>, <type(寄存器)>，对照 vmx_asm2 */
	asm volatile("invept %1, %2\n\t"
		     "setna %0"
		     : "=qm"(err)
		     : "m"(operand), "r"(extent)
		     : "cc");
	return err ? -EIO : 0;
}

/* 对照 ept_sync_global() → __invept(VMX_EPT_EXTENT_GLOBAL, 0, 0)
 * （arch/x86/kvm/vmx/vmx_ops.h:356-359）：global 的 descriptor 内容无意义 */
int mini_ept_invept_global(void)
{
	return mini_invept(VMX_EPT_EXTENT_GLOBAL, 0, 0);
}

int mini_ept_init(struct mini_kvm *kvm)
{
	struct page *pml4;

	pml4 = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!pml4)
		return -ENOMEM;

	kvm->ept.pml4 = pml4;
	kvm->ept.nr_tracked = 0;
	/*
	 * 对照 KVM 的 construct_eptp()（arch/x86/kvm/vmx/vmx.c:3411-3423）：
	 * WB + 4 级行走；不开 VMX_EPTP_AD_ENABLE_BIT（bit 6 的 A/D flags），
	 * 所以 Table 29-2/29-7 里的 accessed 位字段被硬件忽略。
	 *
	 * 这里不给"新根"发 INVEPT —— 失效统一挪到运行循环重新 VMPTRLD 之后
	 * （见本文件 INVEPT 一节）。KVM 的同类失效也在 vCPU 上下文里发
	 * （kvm_mmu_load()，arch/x86/kvm/mmu/mmu.c:5780-5787），不在分配点发
	 * 是因为分配点未必就是将来跑这个 guest 的那台 CPU。
	 */
	kvm->ept.eptp = page_to_phys(pml4) | VMX_EPTP_MT_WB | VMX_EPTP_PWL_4;
	return 0;
}

void mini_ept_destroy(struct mini_kvm *kvm)
{
	struct mini_kvm_ept *ept = &kvm->ept;
	int i;

	/*
	 * 这里不发 INVEPT。KVM 的规矩是"在分配新根时失效，从而拆根时可以
	 * 不失效"（arch/x86/kvm/mmu/mmu.c:5780-5787），mini-kvm 同理，只是把
	 * 失效点定在每次重新 VMPTRLD 之后（见本文件 INVEPT 一节）。
	 * KVM 拆根时另外多出来的一步是 call_rcu() 等一个宽限期才把页交回
	 * 伙伴系统（tdp_mmu.c:68 的 tdp_mmu_free_sp_rcu_callback()，由 :91
	 * 与 :422 两处 call_rcu() 调用），因为 TDP MMU 有无锁行走者；
	 * mini-kvm 的 EPT 只在持有 vCPU mutex 的运行循环里被改，而 kref 归零
	 * 时不可能还有 vCPU 在跑，所以直接 __free_page()。
	 */
	for (i = 0; i < ept->nr_tracked; i++)
		__free_page(ept->tracked[i]);
	ept->nr_tracked = 0;

	if (ept->pml4) {
		__free_page(ept->pml4);
		ept->pml4 = NULL;
	}
}

/*
 * 4 级行走，返回叶条目指针。alloc=1 时按需分配中间层。
 *
 * 注意分配标志：本函数会在 EPT violation 处理路径上被调用，
 * 而运行循环处于 preempt_disable + 关中断上下文，只能 GFP_ATOMIC。
 */
static u64 *mini_ept_walk(struct mini_kvm_ept *ept, u64 gpa, bool alloc)
{
	static const int shifts[4] = {39, 30, 21, 12};
	u64 *table = (u64 *)page_address(ept->pml4);
	int level;

	for (level = 0; level < 4; level++) {
		int idx = (gpa >> shifts[level]) & 511;
		u64 *entry = &table[idx];

		if (level == 3)
			return entry;

		/*
		 * SDM 29.3.2：bits 2:0 全为 0 即 "not present"。本模块只会写
		 * RWX=111，所以测 R 位就足够。
		 */
		if (!(*entry & EPT_PTE_READ)) {
			struct page *np;

			if (!alloc)
				return NULL;
			if (ept->nr_tracked >= MINI_KVM_EPT_TRACK_MAX)
				return NULL;
			np = alloc_page(GFP_ATOMIC | __GFP_ZERO);
			if (!np)
				return NULL;
			ept->tracked[ept->nr_tracked++] = np;
			/* 非叶条目：bits 7:3 保留必须为 0（Table 29-2/29-4/29-6） */
			*entry = page_to_phys(np) | EPT_ENTRY_RWX;
		}
		table = (u64 *)page_address(pfn_to_page(
				(*entry & EPT_PTE_ADDR_MASK) >> PAGE_SHIFT));
	}
	return NULL;	/* 不可达 */
}

/*
 * EPT violation 处理：GPA → memslot → 钉住页的 HPA → 写叶条目。
 * 返回 0 表示已映射，运行循环应重新进入（硬件自动重放导致缺页的访问）。
 */
int mini_ept_handle_violation(struct mini_kvm_vcpu *vcpu)
{
	struct mini_kvm *kvm = vcpu->kvm;
	struct mini_kvm_memslot *slot = &kvm->slot;
	u64 gpa, *entry;
	unsigned long idx;

	mini_vmread(GUEST_PHYSICAL_ADDRESS, &gpa);
	gpa &= PAGE_MASK;

	if (!slot->valid || gpa < slot->base_gpa ||
	    gpa >= slot->base_gpa + (slot->npages << PAGE_SHIFT)) {
		pr_err("mini-kvm: EPT violation GPA=0x%llx 不在 memslot 内 (slot base=0x%llx npages=%llu)\n",
		       gpa, slot->base_gpa, slot->npages);
		return -EFAULT;
	}

	idx = (gpa - slot->base_gpa) >> PAGE_SHIFT;

	entry = mini_ept_walk(&kvm->ept, gpa, true);
	if (!entry) {
		pr_err("mini-kvm: EPT 行走失败 (GPA=0x%llx)\n", gpa);
		return -ENOMEM;
	}

	*entry = page_to_phys(slot->pages[idx]) | EPT_ENTRY_RWX | EPT_MEMTYPE_WB;
	vcpu->n_ept_violations++;
	return 0;
}
