// SPDX-License-Identifier: GPL-2.0
/*
 * interrupt.c —— 中断注入（Stage 3）
 *
 * 注入通道：VM_ENTRY_INTR_INFO_FIELD（SDM 25.8.3 / 27.3.1.4）：
 *   [7:0]   vector
 *   [10:8]  类型（0 = 外部中断，2 = NMI）
 *   [11]    error-code 有效位（外部中断不置）
 *   [31]    valid
 *
 * 注入门槛分两处：RFLAGS.IF 的检查在 SDM 27.3.1.4（"The IF flag
 * (RFLAGS[bit 9]) must be 1 if the valid bit ... is 1 and the
 * interruption type ... is external interrupt"）；interruptibility 的
 * bit0（STI 阻塞）/bit1（MOV SS 阻塞）的检查在 SDM 27.3.1.5，对
 * type 0（外部中断）与 type 2（NMI）**同样**要求两位都为 0。
 * 任一不满足都是 VM-Entry 检查失败。
 *
 * 另有一条在字段本身：type 为 NMI 时 vector 必须是 2（SDM 27.2.1.3）。
 *
 * 与真实 KVM 的差异（教学简化，见 stages/stage3-interrupt.md）：
 *   - KVM 用 RVI/中断窗口退出（interrupt-window exiting）排队等窗口，
 *     不改 guest 的 RFLAGS：能不能注入由 __vmx_interrupt_blocked()
 *     判定（vmx.c:5076-5081，测的就是 IF 与 interruptibility 的
 *     STI/MOV-SS 两位），等不到就 enable_irq_window 让硬件在
 *     guest 开中断的那一刻退出（handle_interrupt_window()，
 *     vmx.c:5658；APICv 走 RVI：vmx_set_rvi()，vmx.c:6881）。
 *   - mini-kvm 在 RUN 循环应用注入时直接强制 IF=1 并清阻塞位。
 *     对我们的 64 位教学 guest 语义等价（guest 本来就要开中断等注入）。
 *
 * NMI：宿主收到的 NMI 会在 PIN_BASED_NMI_EXITING 下退出到 mini-kvm
 * （退出原因 0，VM_EXIT_INTR_INFO 的 type=2 / vector=2，SDM 28.2.2）。
 *
 * 这里 mini-kvm 与 KVM 是**分歧**而不是照抄：KVM 认为这种 NMI 属于宿主，
 * 在 root 模式直接跳进宿主 IDT 的 NMI 门把它消费掉
 * （vmx.c:7330-7338 调 vmx_do_nmi_irqoff()），所以 handle_exception_nmi()
 * 见到 is_nmi() 直接 return 1（vmx.c:5225-5231）。KVM 注入给 guest 的 NMI
 * 另有来源：用户态 KVM_NMI ioctl（x86.c:5193-5197 → kvm_inject_nmi()）
 * 以及"已注入但 guest 还没消费"的重投（x86.c:10381-10388）。
 *
 * mini-kvm 把这类 NMI 转注给 guest：教学 guest 是本模块唯一的执行体，
 * 而且这样能让 guest 的 NMI 处理路径可观测。代价是宿主自己的 NMI 被抢走，
 * 真需要宿主 NMI（比如 MCE 打印、perf nmi watchdog）时这是错的。
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <asm/vmx.h>

#include "mini-kvm.h"

/*
 * 用户态请求注入外部中断。只登记到 pending，不碰 VMCS——
 * VMCS 可能不在当前 CPU 上，真正的写入在 RUN 循环里做。
 */
int mini_vcpu_inject_irq(struct mini_kvm_vcpu *vcpu, int vector)
{
	if (vector < 0 || vector > 255)
		return -EINVAL;
	if (vector < 32) {
		pr_warn("mini-kvm: 拒绝注入保留向量 %d（0-31 为异常保留区）\n",
			vector);
		return -EINVAL;
	}
	if (vcpu->pending_intr_info)
		return -EBUSY;	/* 已有一个待注入，先 RUN 消费掉 */

	vcpu->pending_intr_info = INTR_INFO_VALID_MASK |
				  INTR_TYPE_EXT_INTR | (u32)vector;
	pr_info("mini-kvm: 排队注入外部中断 vector=0x%x\n", vector);
	return 0;
}

/*
 * 运行循环在每次进入前取走待注入项（无则返回 0）。
 * NMI 再注入也走同一通道（由运行循环登记）。
 */
u32 mini_vcpu_take_intr_info(struct mini_kvm_vcpu *vcpu)
{
	u32 info = vcpu->pending_intr_info;

	vcpu->pending_intr_info = 0;
	return info;
}

/*
 * 登记一次 NMI 再注入（运行循环在处理退出原因 0 / vector 2 时调用）。
 * 若 guest 正处于 NMI 阻塞中（interruptibility bit3），本轮放弃——
 * guest 已在自己的 NMI 处理路径里，教学场景下可接受（真实 KVM 会
 * 记账后择机重注）。
 */
void mini_vcpu_reinject_nmi(struct mini_kvm_vcpu *vcpu)
{
	u64 ib = 0;

	mini_vmread(GUEST_INTERRUPTIBILITY_INFO, &ib);
	if (ib & (1u << 3)) {
		pr_info_ratelimited("mini-kvm: guest NMI 阻塞中，放弃本次 NMI 再注入\n");
		return;
	}
	if (vcpu->pending_intr_info)
		return;

	vcpu->pending_intr_info = INTR_INFO_VALID_MASK |
				  INTR_TYPE_NMI_INTR | 2;
}
