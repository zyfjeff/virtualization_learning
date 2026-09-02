// SPDX-License-Identifier: GPL-2.0
/*
 * device.c —— IO 退出解码与 COM1 串口模拟（Stage 4）
 *
 * IO 退出的 EXIT_QUALIFICATION 解码（SDM Vol.3C §28.2.1
 * "Basic VM-Exit Information" 里的 Table 28-5）：
 *   bits 2:0   访问大小：0/1/3 → 1/2/4 字节
 *   bit 3      方向：0 = OUT（guest 写），1 = IN（guest 读）
 *   bits 31:16 端口号
 * （注意方向位在 bit 3；骨架版曾误写成 bit 6。）
 *
 * RIP 推进：IO 指令退出后 RIP 停在原指令，必须加上
 * VM_EXIT_INSTRUCTION_LEN (0x440c) 再进入（字段定义见 SDM §25.9.4，
 * 适用场景清单见 §28.2.5，其中 IN/OUT 明确在列）。
 *
 * 串口模拟：只做最简的"字节捕获"——OUT 到 0x3f8 的字节依次记进
 * kvm->serial[]（线性缓冲，不是环形：写满 MINI_KVM_SERIAL_SIZE-1 字节后就
 * 丢弃后续输入），MINI_KVM_VM_GET_SERIAL 只回传前 255 字节（NUL 结尾），
 * 每收到一个 '\n' 把最近一行打到 dmesg。
 * 不做 16550A 寄存器组（IIR/LSR/中断）——那是用户态 minivmm
 * （examples/minivmm）的课题；这里对照的是 KVM 内核态的退出分发。
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <asm/vmx.h>

#include "mini-kvm.h"

/*
 * 串口字节捕获。调用点是运行循环的 EXIT_REASON_IO_INSTRUCTION 分支，它在
 * mini_handle_io_exit() 前后显式开了中断（对照 KVM：handle_exit() 也在
 * local_irq_enable() 之后执行，arch/x86/kvm/x86.c:11170 → :11198）。
 *
 * 但这仍然是原子上下文：运行循环从头到尾 preempt_disable()（成对的
 * preempt_enable() 在 mini_vcpu_run_loop() 末尾），所以这里既不能拿
 * sleeping lock 也不能做可阻塞的分配。KVM 在
 * :11170 的 local_irq_enable() 后面紧跟 preempt_enable()，它的退出处理器
 * 因此可以睡；mini-kvm 不行 —— 这也是 ept.c 的按需映射只用 GFP_ATOMIC 的
 * 原因。printk 本身合法：控制台拿不到锁时走 trylock 路径
 * （down_trylock_console_sem()，kernel/printk/printk.c:315-334），记录先进
 * 环形 log buffer，刷新推迟。
 *
 * 写者不持 kvm->lock：唯一写者是运行循环（同一次 KVM_RUN 里由
 * vcpu->mutex 串行化，天然单写者）。MINI_KVM_VM_GET_SERIAL 那把锁只保证多个
 * 读者互斥，挡不住这个写者，所以并发读可能读到撕裂的一帧 —— 教学场景可接受。
 */
static void mini_serial_out(struct mini_kvm *kvm, u8 c)
{
	char line[MINI_KVM_SERIAL_SIZE + 1];
	int start, len, i;

	if (kvm->serial_len < MINI_KVM_SERIAL_SIZE - 1)
		kvm->serial[kvm->serial_len++] = c;

	if (c != '\n')
		return;

	/* 找最近一个完整行并打到 dmesg */
	start = 0;
	for (i = kvm->serial_len - 2; i >= 0; i--) {
		if (kvm->serial[i] == '\n') {
			start = i + 1;
			break;
		}
	}
	len = kvm->serial_len - start;
	memcpy(line, kvm->serial + start, len);
	line[len] = '\0';
	pr_info("mini-kvm guest: %s", line);
}

/*
 * IO 退出处理：解码 → 模拟 → 推进 RIP。返回 0 继续运行。
 */
int mini_handle_io_exit(struct mini_kvm_vcpu *vcpu)
{
	u64 qual, rip, len;
	u32 port, size;
	bool is_in;

	mini_vmread(EXIT_QUALIFICATION, &qual);

	/*
	 * 串操作（REP INS/OUTS，bit 4）先挡掉：KVM 把它交给 x86 模拟器
	 * （handle_io() 的 string 位判定 vmx.c:5408 → :5412-5413），mini-kvm 没有
	 * 模拟器，继续按标量 IO 解码会静默做错（一次退出只搬一个字节，还要重复退出）。
	 */
	if (qual & (1u << 4)) {
		pr_err("mini-kvm: 不支持串 IO 退出 (qual=0x%llx)，交给用户态\n", qual);
		return -EIO;
	}

	size = (u32)(qual & 7) + 1;		/* bits 2:0；合法编码只有 0/1/3，
						 * 与 KVM 同一公式（vmx.c:5416） */
	is_in = qual & (1u << 3);		/* bit 3 = 方向 */
	port = (u32)(qual >> 16) & 0xffff;	/* bits 31:16 */

	if (is_in) {
		/* 教学简化：所有 IN 返回 0（无 16550A 寄存器组） */
		vcpu->regs[0] = 0;
	} else {
		u32 mask = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
		u32 val = (u32)vcpu->regs[0] & mask;	/* OUT 的值在 RAX */

		if (port == MINI_KVM_SERIAL_PORT)
			mini_serial_out(vcpu->kvm, (u8)val);
		else
			pr_info_ratelimited("mini-kvm: 忽略 OUT port=0x%x val=0x%x\n",
					    port, val);
	}

	mini_vmread(VM_EXIT_INSTRUCTION_LEN, &len);
	mini_vmread(GUEST_RIP, &rip);
	mini_vmwrite(GUEST_RIP, rip + len);

	vcpu->n_io_exits++;
	return 0;
}
