// SPDX-License-Identifier: GPL-2.0
/*
 * vcpu.c —— 三层 fd、ioctl、mmap、运行循环（Stage 5）
 *
 * 与真实 KVM 的对照（Linux 6.12.93）：
 *   三层 fd 模型       virt/kvm/kvm_main.c：
 *     /dev/kvm ioctl   → kvm_dev_ioctl()
 *     vm fd ioctl      → kvm_vm_ioctl()
 *     vcpu fd ioctl    → kvm_vcpu_ioctl()
 *   KVM_CREATE_VM      → kvm_dev_ioctl_create_vm / anon_inode_getfile
 *   KVM_CREATE_VCPU    → kvm_vm_ioctl_create_vcpu（vmx_create_vcpu）
 *   SET_USER_MEMORY    → kvm_vm_ioctl_set_memory_region（pin_user_pages）
 *   kvm_run mmap       → kvm_vcpu_mmap
 *   运行循环           → x86.c vcpu_run / vcpu_enter_guest + vmx_vcpu_run
 *
 * 简化点：VM 可以开多个，但每个 VM 只有一个 vCPU、一个 memslot；ioctl 由
 * vcpu->mutex 串行化；不做信号处理（KVM_RUN 里的 X86EMUL/信号检查）、不做
 * 脏页跟踪、不做多架构。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/anon_inodes.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmap_lock.h>
#include <linux/uaccess.h>
#include <linux/preempt.h>
#include <linux/irqflags.h>
#include <linux/kref.h>
#include <asm/vmx.h>
#include <asm/msr-index.h>
#include <asm/processor-flags.h>

#include "mini-kvm.h"

static atomic_t mini_vm_id_counter = ATOMIC_INIT(-1);

/* VM 引用计数（定义见下方 mini_kvm_destroy()） */
static void mini_kvm_get(struct mini_kvm *kvm);
static void mini_kvm_put(struct mini_kvm *kvm);

/*
 * ============================================================================
 * 运行循环（KVM_RUN）
 * ============================================================================
 *
 * 上下文约束：全程 preempt_disable；进入/退出瞬间关中断。
 * 对照 KVM：vcpu_run 外围的 srcu/preempt 处理与 vmx_vcpu_run 的
 * local_irq 窗口（vmenter.S 前后）。
 *
 * 宿主外部中断：PIN_BASED_EXT_INTR_MASK 让所有外部中断退出到
 * mini-kvm。我们没有 VM_EXIT_ACK_INTR_ON_EXIT，中断向量还在 LAPIC
 * IRR 里 pending；打开中断的"瞬间"（blip）让它走宿主 IDT 正常分发。
 * 对照 KVM 的 handle_external_interrupt_irqoff()（vmx.c）——KVM 用
 * gate descriptor 精确重放，这里用最朴素的开关中断窗口。
 */

static int mini_vcpu_run_loop(struct mini_kvm_vcpu *vcpu)
{
	struct kvm_run *run = vcpu->run;
	int ret = 0;

	run->exit_reason = 0;

	preempt_disable();

	/*
	 * "上机"：一次性完成 VMXON 检查、VMCS 迁移、失效与 Host 修补。
	 * 对照 KVM：这些全在 KVM_RUN 进主循环前的一次 vcpu_load() 里做完
	 * （arch/x86/kvm/x86.c:11590 -> virt/kvm/kvm_main.c:205-213 ->
	 * kvm_arch_vcpu_load() x86.c:4982 -> kvm_x86_call(vcpu_load) x86.c:5002
	 * -> vmx_vcpu_load_vmcs()），不是每次进入前重做。
	 *
	 * 只做一次是成立的：下面全程 preempt_disable，本线程不会被换下 CPU，
	 * 所以 loaded_cpu 一旦记成本机就不可能再不等。
	 *
	 * 反过来，**必须**在进循环前做：本函数若干条 continue 路径会带着
	 * 关中断的状态回到循环头（EPT-violation、CPUID、NMI 再注入），而这里
	 * mini_vmcs_clear() 可能对旧 CPU 发一发 smp_call_function_single(wait=1)，
	 * 关中断下调用它会命中 WARN_ON_ONCE(cpu_online(this_cpu) &&
	 * irqs_disabled()) —— 注释写明了原因："Can deadlock when called with
	 * interrupts disabled."（kernel/smp.c:647-653）。此刻中断还是开的。
	 */
	if (!mini_cpu_in_vmx_operation()) {
		pr_err("mini-kvm: CPU%d 未 VMXON，拒绝进入 guest\n",
		       raw_smp_processor_id());
		run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		ret = -EPROTO;
		goto out;
	}

	/*
	 * vCPU 迁移：VMCS 同一时刻只能 active 在一个 CPU 上。
	 * 对照 vmx_vcpu_load_vmcs()（arch/x86/kvm/vmx/vmx.c:1449-1514）
	 * 的三件事，一件都不能少：
	 *   1. loaded_vmcs_clear()（:1457）—— 把 VMCLEAR 投递到**旧**
	 *      CPU 执行，并把 launched 清零（VMCLEAR 会把 launch state
	 *      置成 "clear"，下一次进入必须用 VMLAUNCH，SDM 25.11.3）。
	 *      见 vmx.c 的 mini_vmcs_clear() 注释。
	 *   2. vmcs_load()（:1476）—— 本机 VMPTRLD。
	 *   3. kvm_make_request(KVM_REQ_TLB_FLUSH)（:1493-1496）+
	 *      修补 per-CPU 的 Host 字段（:1502-1510）。
	 */
	if (vcpu->loaded_cpu != raw_smp_processor_id()) {
		int old_cpu = vcpu->loaded_cpu;

		mini_vmcs_clear(vcpu);
		if (mini_vmptrld(vcpu->vmcs_phys)) {
			run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
			ret = -EIO;
			goto out;
		}
		/*
		 * 本分支也是本 vCPU 第一次加载 VMCS 的路径（setup 结束
		 * 时主动卸下），所以这一发 all-context 一次覆盖两种残留：
		 * 新 pCPU 上这个 vCPU 上次的映射，以及 EPT 根页被复用前
		 * 上一个使用者的映射。见 ept.c 的 INVEPT 一节。
		 */
		if (mini_ept_invept_global())
			pr_warn("mini-kvm: CPU%d 上 INVEPT global 失败\n",
				raw_smp_processor_id());
		vcpu->loaded_cpu = raw_smp_processor_id();
		mini_vmx_fixup_host_for_cpu();
		pr_info("mini-kvm: vCPU 上机 CPU%d→CPU%d, VMCS 重新加载\n",
			old_cpu, vcpu->loaded_cpu);
	}

	for (;;) {
		u64 reason64 = 0, intr_info64 = 0, gs_shadow;
		u32 reason;
		u32 intr;
		int r;

		/* 每次进入前刷新会漂移的 Host 字段（CR3/CR4/GS/FS） */
		mini_vmx_refresh_host_state();

		/*
		 * 注入窗口：把排队中的中断写入 VM_ENTRY_INTR_INFO_FIELD。
		 * 外部中断注入要求 IF=1（SDM 27.3.1.4）且无 STI/MOV-SS 阻塞
		 * （SDM 27.3.1.5，NMI 注入同样要求），这里直接满足
		 * （见 interrupt.c 注释）。
		 */
		intr = mini_vcpu_take_intr_info(vcpu);
		if (intr) {
			u64 rflags = 0, ib = 0;

			mini_vmread(GUEST_RFLAGS, &rflags);
			mini_vmwrite(GUEST_RFLAGS, rflags | X86_EFLAGS_IF);
			mini_vmread(GUEST_INTERRUPTIBILITY_INFO, &ib);
			mini_vmwrite(GUEST_INTERRUPTIBILITY_INFO, ib & ~0x3ULL);
			vcpu->n_injected++;
		}
		mini_vmwrite(VM_ENTRY_INTR_INFO_FIELD, intr);

		/*
		 * 保住宿主的 IA32_KERNEL_GS_BASE（"SWAPGS 影子"）。
		 *
		 * 两个事实叠在一起就是宿主级的洞：
		 *   1. VM-Exit 的 host-state 区只恢复 FS.base 与 GS.base，
		 *      不恢复 KERNEL_GS_BASE（SDM 28.5.1 的宿主状态清单里
		 *      只有 "The MSRs FS.base and GS.base are loaded ..."）。
		 *   2. SWAPGS 在 64 位 non-root 下原生执行：SDM 26.1.2 的
		 *      "无条件退出的指令" 清单（CPUID/INVD/XSETBV/INVEPT/
		 *      INVVPID/VMCALL/...）里没有 SWAPGS，VMX 也没有为它
		 *      设任何控制位——只有 RDMSR/WRMSR 访问 0xC0000102 能被
		 *      MSR bitmap 拦住。
		 * 于是 guest 只要执行一条 SWAPGS，宿主的影子 GS 基址就变成
		 * guest 控制的值；宿主下一次 swapgs（系统调用或中断返回）会把
		 * 这个垃圾装进 GS base，per-CPU 寻址当场废掉。
		 *
		 * 对照 KVM：vmx_prepare_switch_to_guest() 保存宿主值并写入
		 * guest 的值（vmx.c:1338-1346），vmx_prepare_switch_to_host()
		 * 读回 guest 的值并恢复宿主值（vmx.c:1358-1390）。
		 *
		 * mini-kvm 只做"保住宿主值"这一半，不需要维护 guest 影子值：
		 * guest 根本碰不到任何 MSR —— "use MSR bitmaps" = 0 时所有
		 * RDMSR/WRMSR 都无条件退出（SDM Table 25-6 bit 28："If the MSR
		 * bitmaps are not used, all executions of the RDMSR and WRMSR
		 * instructions cause VM exits"），而我们的运行循环没有 MSR 退出
		 * 分支，guest 执行 RDMSR/WRMSR 只会以 KVM_EXIT_INTERNAL_ERROR
		 * 收场。唯一拦不住的是 SWAPGS（上面第 2 点），所以只需要在退出
		 * 后把宿主自己的影子值写回去。
		 */
		gs_shadow = mini_rdmsr(MSR_KERNEL_GS_BASE);

		local_irq_disable();
		r = mini_vmx_enter(vcpu, vcpu->launched);
		mini_wrmsr(MSR_KERNEL_GS_BASE, gs_shadow);
		/* 退出时中断仍关闭（硬件按 HOST 状态恢复，默认 IF=0） */

		if (r) {
			mini_vmx_report_error(vcpu->launched ?
					      "VMRESUME" : "VMLAUNCH");
			mini_dump_vmcs("entry failed");
			run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
			local_irq_enable();
			ret = -EIO;
			break;
		}
		if (!vcpu->launched)
			vcpu->launched = true;

		vcpu->n_exits++;
		mini_vmread(VM_EXIT_REASON, &reason64);
		reason = (u32)reason64 & 0xffff;

		/* bit31 = VM-Entry 失败（SDM 25.9.1） */
		if (reason64 & 0x80000000ULL) {
			pr_err("mini-kvm: VM-Entry 失败, exit_reason=0x%llx\n",
			       reason64);
			mini_dump_vmcs("failed vmentry");
			run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
			local_irq_enable();
			ret = -EIO;
			break;
		}

		switch (reason) {
		case EXIT_REASON_EXTERNAL_INTERRUPT:
			/* 让 pending 的宿主中断走宿主 IDT */
			vcpu->n_extint_exits++;
			local_irq_enable();
			local_irq_disable();
			continue;

		case EXIT_REASON_EXCEPTION_NMI:
			mini_vmread(VM_EXIT_INTR_INFO, &intr_info64);
			if ((intr_info64 & 0x700) == INTR_TYPE_NMI_INTR &&
			    (intr_info64 & 0xff) == 2) {
				vcpu->n_nmi_exits++;
				mini_vcpu_reinject_nmi(vcpu);
				continue;
			}
			/* #DB/#UD/#GP/#PF 等（我们在异常位图里捕获的） */
			{
				u64 rip = 0;

				mini_vmread(GUEST_RIP, &rip);
				pr_err("mini-kvm: guest 异常 vector=%llu info=0x%llx rip=0x%llx\n",
				       intr_info64 & 0xff, intr_info64, rip);
			}
			mini_dump_vmcs("guest exception");
			run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
			local_irq_enable();
			ret = -EIO;
			break;

		case EXIT_REASON_IO_INSTRUCTION:
			local_irq_enable();
			r = mini_handle_io_exit(vcpu);
			local_irq_disable();
			if (r) {
				run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
				ret = r;
				local_irq_enable();
				break;
			}
			continue;

		case EXIT_REASON_EPT_VIOLATION:
			/* 行走/映射可能分配页：GFP_ATOMIC，可在关中断下执行 */
			r = mini_ept_handle_violation(vcpu);
			if (r) {
				mini_dump_vmcs("ept violation failed");
				run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
				ret = r;
				local_irq_enable();
				break;
			}
			continue;

		case EXIT_REASON_CPUID: {
			u64 rip = 0, len = 0;

			/* guest 不该执行 CPUID；给个无害结果并跳过 */
			vcpu->regs[0] = 0;
			mini_vmread(GUEST_RIP, &rip);
			mini_vmread(VM_EXIT_INSTRUCTION_LEN, &len);
			mini_vmwrite(GUEST_RIP, rip + len);
			continue;
		}

		case EXIT_REASON_HLT:
			/*
			 * 与 KVM 相同：HLT 退出不推进 RIP，恢复时重执行。
			 * 退出到用户态（KVM_EXIT_HLT），由测试程序决定下一步。
			 */
			vcpu->n_hlt_exits++;
			run->exit_reason = KVM_EXIT_HLT;
			local_irq_enable();
			goto out;

		case EXIT_REASON_TRIPLE_FAULT:
			pr_err("mini-kvm: guest 三重故障!\n");
			mini_dump_vmcs("triple fault");
			run->exit_reason = KVM_EXIT_SHUTDOWN;
			local_irq_enable();
			goto out;

		default:
			pr_err("mini-kvm: 未处理的退出原因 %u (raw 0x%llx)\n",
			       reason, reason64);
			mini_dump_vmcs("unhandled exit");
			run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
			local_irq_enable();
			ret = -EIO;
			break;
		}
		if (ret)
			break;
	}

out:
	preempt_enable();
	pr_debug("mini-kvm: RUN 结束 exits=%llu io=%llu ept=%llu hlt=%llu extint=%llu nmi=%llu inj=%llu\n",
		 vcpu->n_exits, vcpu->n_io_exits, vcpu->n_ept_violations,
		 vcpu->n_hlt_exits, vcpu->n_extint_exits, vcpu->n_nmi_exits,
		 vcpu->n_injected);
	return ret;
}

/*
 * ============================================================================
 * vCPU fd
 * ============================================================================
 */

static long mini_vcpu_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct mini_kvm_vcpu *vcpu = filp->private_data;
	long r;

	/*
	 * 对照 kvm_vcpu_ioctl()：所有同步的 vCPU ioctl 都在 vcpu->mutex 下
	 * 串行化（virt/kvm/kvm_main.c:4468）。锁必须在 preempt_disable 之前
	 * 拿、出运行循环之后再放。
	 */
	if (mutex_lock_killable(&vcpu->mutex))
		return -EINTR;

	switch (cmd) {
	case KVM_RUN:
		r = -EINVAL;
		if (arg)	/* 对照 kvm_main.c:4474-4475：KVM_RUN 不接受参数 */
			break;
		r = mini_vcpu_run_loop(vcpu);
		break;

	case MINI_KVM_VCPU_INJECT_IRQ: {
		int vector;

		if (copy_from_user(&vector, (void __user *)arg, sizeof(vector))) {
			r = -EFAULT;
			break;
		}
		r = mini_vcpu_inject_irq(vcpu, vector);
		break;
	}

	default:
		r = -ENOTTY;
	}

	mutex_unlock(&vcpu->mutex);
	return r;
}

static int mini_vcpu_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct mini_kvm_vcpu *vcpu = filp->private_data;

	if (vma->vm_pgoff != 0)
		return -EINVAL;
	if (vma->vm_end - vma->vm_start != PAGE_SIZE)
		return -EINVAL;

	/* 对照 kvm_vcpu_mmap()：把 kvm_run 页映射给用户态 */
	return remap_pfn_range(vma, vma->vm_start,
			       page_to_pfn(vcpu->run_page),
			       PAGE_SIZE, vma->vm_page_prot);
}

static int mini_vcpu_release(struct inode *inode, struct file *filp)
{
	struct mini_kvm_vcpu *vcpu = filp->private_data;
	struct mini_kvm *kvm = vcpu->kvm;

	/*
	 * 先解除 VM→vCPU 的反向指针：KVM_CREATE_VCPU 用 kvm->vcpu 是否为空
	 * 判断"已有 vCPU"，留一个悬垂指针会让下一次创建走通并双重持有。
	 */
	mutex_lock(&kvm->lock);
	if (kvm->vcpu == vcpu)
		kvm->vcpu = NULL;
	mutex_unlock(&kvm->lock);

	mini_vcpu_vmx_teardown(vcpu);
	if (vcpu->run_page)
		__free_page(vcpu->run_page);
	kfree(vcpu);

	/*
	 * 还掉 vCPU fd 那份 VM 引用。VM fd 先关闭也不会拆掉 VM —— 直到最后
	 * 一个持有者放手。对照 kvm_vcpu_release() → kvm_put_kvm()
	 * （virt/kvm/kvm_main.c:4156-4162）。
	 */
	mini_kvm_put(kvm);
	return 0;
}

static const struct file_operations mini_vcpu_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= mini_vcpu_ioctl,
	.mmap		= mini_vcpu_mmap,
	.release	= mini_vcpu_release,
	.llseek		= noop_llseek,
};

/*
 * ============================================================================
 * VM fd
 * ============================================================================
 */

static int mini_kvm_set_memregion(struct mini_kvm *kvm,
				  void __user *argp)
{
	struct kvm_userspace_memory_region region;
	struct page **pages;
	unsigned long npages;
	long pinned;

	if (copy_from_user(&region, argp, sizeof(region)))
		return -EFAULT;

	/* 简化：单 slot、无标志、必须先注册内存再建 vCPU */
	if (region.slot != 0 || region.flags)
		return -EINVAL;
	if (kvm->slot.valid)
		return -EEXIST;
	if (!region.memory_size ||
	    !PAGE_ALIGNED(region.guest_phys_addr) ||
	    !PAGE_ALIGNED(region.userspace_addr) ||
	    !PAGE_ALIGNED(region.memory_size))
		return -EINVAL;

	npages = region.memory_size >> PAGE_SHIFT;
	pages = kvcalloc(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/*
	 * 钉住用户页（对照 __kvm_set_memory_region 的 pin 路径，
	 * kvm_main.c）。pin_user_pages() 要求持 mmap 读锁。
	 */
	mmap_read_lock(current->mm);
	pinned = pin_user_pages(region.userspace_addr, npages,
				FOLL_WRITE | FOLL_LONGTERM, pages);
	mmap_read_unlock(current->mm);

	if (pinned != (long)npages) {
		pr_err("mini-kvm: pin_user_pages 只钉住 %ld/%lu 页\n",
		       pinned, npages);
		if (pinned > 0)
			unpin_user_pages(pages, pinned);
		kvfree(pages);
		return -EFAULT;
	}

	kvm->slot.base_gpa = region.guest_phys_addr;
	kvm->slot.npages = npages;
	kvm->slot.userspace_addr = region.userspace_addr;
	kvm->slot.pages = pages;
	kvm->slot.valid = true;

	pr_info("mini-kvm: memslot 注册 GPA=0x%llx 大小=%lu 页 (HVA=0x%llx)\n",
		region.guest_phys_addr, npages, region.userspace_addr);
	return 0;
}

static void mini_kvm_unpin_memslot(struct mini_kvm *kvm)
{
	struct mini_kvm_memslot *slot = &kvm->slot;

	if (!slot->valid)
		return;
	unpin_user_pages(slot->pages, slot->npages);
	kvfree(slot->pages);
	slot->pages = NULL;
	slot->valid = false;
}

/*
 * VM 生命周期：kref 归零才拆资源。
 *
 * 为什么必须有引用计数：VM fd 和 vCPU fd 的关闭顺序完全由用户态决定
 * （进程退出时 close 顺序不保证，也可能只关一个），而 vCPU 的
 * vmcs/run 页之外，EPT 页表和 pin 住的用户页都挂在 VM 上。直接在
 * mini_vm_release() 里 kfree(kvm) 会让后关的 vCPU fd 摸到野指针。
 *
 * 对照 KVM：struct kvm 的 refcount_t users_count 由 kvm_get_kvm() /
 * kvm_put_kvm() 增减，归零时走 kvm_destroy_vm()（virt/kvm/kvm_main.c:
 * 1372-1393）。kref_put() 相当于"减一 + 归零回调"，语义相同但把
 * 判零和回调打包了。KVM 另有一个 kvm_put_kvm_no_destroy()
 * （kvm_main.c:1402-1406），它前面的注释（:1395-1401）专门说明"fd 没装上、
 * 引用要还回去，但调用者仍在用这个 kvm"的情形 —— 下面
 * mini_kvm_create_vcpu() 的错误路径就是这个场景，只不过我们确定 VM fd
 * 还持有一份，kref_put 不会归零。
 */
static void mini_kvm_destroy(struct kref *ref)
{
	struct mini_kvm *kvm = container_of(ref, struct mini_kvm, refcount);

	mini_kvm_unpin_memslot(kvm);
	mini_ept_destroy(kvm);
	pr_info("mini-kvm: VM%d 资源释放\n", kvm->vm_id);
	kfree(kvm);
}

static void mini_kvm_get(struct mini_kvm *kvm)
{
	kref_get(&kvm->refcount);
}

static void mini_kvm_put(struct mini_kvm *kvm)
{
	kref_put(&kvm->refcount, mini_kvm_destroy);
}

static int mini_kvm_create_vcpu(struct mini_kvm *kvm, u32 id)
{
	struct mini_kvm_vcpu *vcpu;
	int fd, r;

	if (id != 0)
		return -EINVAL;
	if (!kvm->slot.valid) {
		pr_err("mini-kvm: 请先 KVM_SET_USER_MEMORY_REGION\n");
		return -EINVAL;
	}

	vcpu = kzalloc(sizeof(*vcpu), GFP_KERNEL);
	if (!vcpu)
		return -ENOMEM;

	vcpu->vcpu_id = id;
	vcpu->kvm = kvm;
	vcpu->loaded_cpu = -1;
	/* 必须在 anon_inode_getfd() 把 vCPU 交出去之前就位（kvm_main.c:484） */
	mutex_init(&vcpu->mutex);

	vcpu->run_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!vcpu->run_page) {
		kfree(vcpu);
		return -ENOMEM;
	}
	vcpu->run = page_address(vcpu->run_page);

	r = mini_vcpu_vmx_setup(vcpu);
	if (r) {
		__free_page(vcpu->run_page);
		kfree(vcpu);
		return r;
	}

	/*
	 * 发布顺序：取 VM 引用 → 挂反向指针 → 装 fd。
	 *
	 * anon_inode_getfd() 成功的一刻 vCPU fd 就对外可见，用户态随时可以
	 * close 它并跑 mini_vcpu_release() —— 所以引用必须先于装 fd，且
	 * kvm->vcpu 必须在装 fd 前就位（否则并发/快速关闭会留下悬垂指针）。
	 * 对照 kvm_vm_ioctl_create_vcpu()：在 kvm->lock 下判重，
	 * kvm_get_kvm() 紧接着 create_vcpu_fd()
	 * （virt/kvm/kvm_main.c:4277-4299）。
	 */
	mini_kvm_get(kvm);

	mutex_lock(&kvm->lock);
	if (kvm->vcpu) {
		mutex_unlock(&kvm->lock);
		mini_kvm_put(kvm);	/* VM fd 仍持有一份，不会归零 */
		r = -EEXIST;
		goto err_teardown;
	}
	kvm->vcpu = vcpu;
	fd = anon_inode_getfd("mini-kvm-vcpu", &mini_vcpu_fops, vcpu,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		kvm->vcpu = NULL;
		mutex_unlock(&kvm->lock);
		mini_kvm_put(kvm);
		r = fd;
		goto err_teardown;
	}
	mutex_unlock(&kvm->lock);

	pr_info("mini-kvm: vCPU%d 创建完成 (fd=%d)\n", id, fd);
	return fd;

err_teardown:
	mini_vcpu_vmx_teardown(vcpu);
	__free_page(vcpu->run_page);
	kfree(vcpu);
	return r;
}

static long mini_vm_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct mini_kvm *kvm = filp->private_data;

	switch (cmd) {
	case KVM_CREATE_VCPU:
		return mini_kvm_create_vcpu(kvm, (u32)arg);

	case KVM_SET_USER_MEMORY_REGION:
		return mini_kvm_set_memregion(kvm, (void __user *)arg);

	case MINI_KVM_VM_GET_SERIAL: {
		char buf[256];

		memset(buf, 0, sizeof(buf));
		mutex_lock(&kvm->lock);
		memcpy(buf, kvm->serial, min_t(int, kvm->serial_len, 255));
		mutex_unlock(&kvm->lock);
		if (copy_to_user((void __user *)arg, buf, sizeof(buf)))
			return -EFAULT;
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static int mini_vm_release(struct inode *inode, struct file *filp)
{
	struct mini_kvm *kvm = filp->private_data;

	/*
	 * 只还 VM fd 这一份引用。若 vCPU fd 还开着，VM 必须活着；
	 * 归零时 kref 回调才真正 unpin 用户页 + 拆 EPT。
	 * 对照 kvm_vm_release() → kvm_put_kvm()（virt/kvm/kvm_main.c:1408-1416）。
	 */
	mini_kvm_put(kvm);
	return 0;
}

static const struct file_operations mini_vm_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= mini_vm_ioctl,
	.release	= mini_vm_release,
	.llseek		= noop_llseek,
};

static int mini_kvm_create_vm(void)
{
	struct mini_kvm *kvm;
	int fd, r;

	kvm = kzalloc(sizeof(*kvm), GFP_KERNEL);
	if (!kvm)
		return -ENOMEM;

	mutex_init(&kvm->lock);
	kvm->vm_id = atomic_inc_return(&mini_vm_id_counter);
	/* 计 1：VM fd 自己那一份（vCPU fd 创建时再 kref_get） */
	kref_init(&kvm->refcount);

	r = mini_ept_init(kvm);
	if (r) {
		kfree(kvm);
		return r;
	}

	fd = anon_inode_getfd("mini-kvm-vm", &mini_vm_fops, kvm,
			      O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		mini_ept_destroy(kvm);
		kfree(kvm);
		return fd;
	}

	pr_info("mini-kvm: VM%d 创建完成 (fd=%d)\n", kvm->vm_id, fd);
	return fd;
}

/*
 * ============================================================================
 * /dev/mini-kvm fd
 * ============================================================================
 */

static long mini_kvm_dev_ioctl(struct file *filp, unsigned int cmd,
			       unsigned long arg)
{
	switch (cmd) {
	case KVM_GET_API_VERSION:
		return KVM_API_VERSION;	/* 12，与真实 KVM 同版本握手 */

	case KVM_CHECK_EXTENSION:
		switch (arg) {
		case KVM_CAP_USER_MEMORY:
			return 1;
		default:
			return 0;
		}

	case KVM_GET_VCPU_MMAP_SIZE:
		return PAGE_SIZE;

	case KVM_CREATE_VM:
		return mini_kvm_create_vm();

	default:
		return -ENOTTY;
	}
}

const struct file_operations mini_kvm_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= mini_kvm_dev_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice mini_kvm_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "mini-kvm",
	.fops	= &mini_kvm_fops,
	.mode	= 0666,
};

int mini_vcpu_dev_init(void)
{
	return misc_register(&mini_kvm_misc);
}

void mini_vcpu_dev_exit(void)
{
	misc_deregister(&mini_kvm_misc);
}
