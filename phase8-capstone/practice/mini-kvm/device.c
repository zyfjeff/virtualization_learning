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
 * 串口模拟：只做最简的"字节捕获"——OUT 到 0x3f8 的字节记入环形文本
 * 缓冲，供 MINI_KVM_VM_GET_SERIAL 读取，并把每个完整行打到 dmesg。
 * 不做 16550A 寄存器组（IIR/LSR/中断）——那是用户态 minivmm
 * （examples/minivmm）的课题；这里对照的是 KVM 内核态的退出分发。
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <asm/vmx.h>

#include "mini-kvm.h"

/*
 * 串口字节捕获。运行在关中断的退出分发路径上，但 printk 走
 * deferred console，允许在此调用。
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
