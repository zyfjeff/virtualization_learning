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
 * 每次真实 VM-Exit 之后与硬件对账一次：上一轮进入排队的事件究竟投递成功了
 * 没有。这是"注入"与"投递"两回事——SDM 27.6.1 里 VM entry 的事件投递发生在
 * 载入 guest 状态与控制字段**之后**，投递本身完全可以半途而废：
 *
 * SDM 28.2.4 列出的"退出发生在事件投递途中"的场景里就有我们踩到的那一条
 * （原文列表）："An EPT violation, EPT misconfiguration, page-modification
 * log-full event, or SPP-related event that occurs during event delivery."
 * 投递自己要做好几次访存——读 IDT 门、按门里的 selector 读 GDT 描述符、往
 * guest 栈上压中断帧、再取处理器的第一条指令——任何一次都能撞进未映射的
 * guest 页。真机量到的是"退出原因 48 且 IDT-vectoring 记着 vector 0x21 正在
 * 投递"；具体是哪一次访存，靠 A/B 定下来：guest 侧只加一张 GDT 与一条
 * `lgdt`、其余一字不改，故障就消失 —— 所以炸的是"按门里的 selector 读 GDT
 * 描述符"那一步（GDTR.base=0 时它读的就是 GPA 0x8，第 0 页当时从没被碰过）。
 * 见 corrections.md J13。
 *
 * 这种退出会把还没投递完的事件写进 IDT-vectoring information：vector 在
 * [7:0]，类型在 [10:8]，bit 11 是 error-code 有效位，bit 31 对这类退出
 * **恒为 1**；其余退出则清 bit 31（SDM 28.2.4 末尾 + Table 25-20 格式）。
 *
 * 于是重投的判据就是这一位。对照 KVM：`__vmx_complete_interrupts()`
 * （arch/x86/kvm/vmx/vmx.c:7111-7163）先无条件清掉 nmi_injected 与
 * exception/interrupt 两个队列（:7121-7124），再只看 `idtv_info_valid`
 * （:7126）；有效时按类型重新排队，外部中断走
 * `kvm_queue_interrupt(vcpu, vector, type == INTR_TYPE_SOFT_INTR)`
 * （:7156-7158），而 `kvm_queue_interrupt()` 的**第一件事就是置
 * `interrupt.injected = true`**（arch/x86/kvm/x86.h:144）。也就是说 KVM 把
 * "硬件替我保管的事件"重新拿回软件侧，下次进入前由
 * `kvm_check_and_inject_events()`（arch/x86/kvm/x86.c:10342）再写一遍 VMCS
 * （x86.c:10386-10387）。
 *
 * mini-kvm 只有一个事件槽（`pending_intr_info`），语义与上面等价。少了这一
 * 步会怎样：运行循环取走 pending 时已经把槽清空，EPT-violation 分支
 * `continue` 回循环头再写 `VM_ENTRY_INTR_INFO_FIELD` 就写成 0 —— 事件被硬件
 * 记着、被软件丢掉，guest 回到被中断的指令继续跑。实测症状是"注入的中断安静
 * 地消失"：串口里没有 `[IRQ 0x21 handled]`，计数器 inj=1、io 却一个没涨。
 */
void mini_vcpu_complete_intr_info(struct mini_kvm_vcpu *vcpu, u32 reason)
{
	u64 idtv = 0;

	vcpu->pending_intr_info = 0;

	mini_vmread(IDT_VECTORING_INFO_FIELD, &idtv);
	if (!(idtv & (1u << 31)))
		return;			/* 普通退出：没有半途而废的事件 */

	if (((idtv >> 8) & 0x7) == INTR_TYPE_EXT_INTR) {
		vcpu->pending_intr_info = INTR_INFO_VALID_MASK |
					  INTR_TYPE_EXT_INTR | (u32)(idtv & 0xff);
		pr_info("mini-kvm: 事件投递途中退出（原因 %u），重投外部中断 vector=0x%llx\n",
			reason, idtv & 0xff);
	}
	/*
	 * 其余类型（NMI / 硬件异常 / INT n）本模块不注入，落到"丢弃"。真实
	 * KVM 在这里对每种类型分别记账（vmx.c:7133-7158），NMI 还要顺带清
	 * NMI 阻塞位（:7135-7141）。
	 */
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
