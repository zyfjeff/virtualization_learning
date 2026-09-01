/* SPDX-License-Identifier: GPL-2.0 */
/*
 * guest.S —— mini-kvm 的 guest：64 位裸机程序（Stage 3-5 的被运行对象）
 *
 * 由 guest.ld 链接到 GPA 0x1000，用户态测试程序把产物 guest.bin 拷入
 * GPA 0x1000 处。内存视图是测试程序搭建的 2MB 大页恒等映射：
 *   PML4 @ GPA 0x6000 → PDPT @ 0x7000 → PD @ 0x8000，PD[0] = 2MB PS 页
 *
 * 执行流程：
 *   1. 在 GPA 0x2000 建 256 项中断门 IDT：
 *        全部指向 irq_stub（直接 iretq）；
 *        第 0x21 项指向 irq_handler（打印消息）。
 *   2. lidt，经 COM1 (0x3f8) 输出 "Hello from Mini-KVM Guest!"。
 *   3. sti；hlt 循环等中断。
 *
 * 当 mini-kvm 经 VM_ENTRY_INTR_INFO_FIELD 注入 vector 0x21 的外部中断
 * 时，CPU 转向 irq_handler，打印 "[IRQ 0x21 handled]"，iretq 回到
 * hlt 重新停机 → 用户态再次收到 KVM_EXIT_HLT，检查串口缓冲验收。
 *
 * 约束（与模块侧设计配套，见 interrupt.c / stages/stage3-interrupt.md）：
 *   - 中断注入前模块会强制 RFLAGS.IF=1；我们自己也执行 sti。
 *   - 不碰 MSR：mini-kvm 未启用 "use MSR bitmaps"，此时所有 RDMSR/WRMSR
 *     都无条件 VM 退出（SDM Table 25-6 bit 28），而运行循环没有 MSR 退出
 *     分支，guest 一执行就以 KVM_EXIT_INTERNAL_ERROR 收场。
 *   - 不依赖 GS / 不执行 swapgs：模块只负责在退出后把宿主的
 *     IA32_KERNEL_GS_BASE 恢复原值（见 vcpu.c 的 gs_shadow），guest 自己的
 *     GS.base 与影子值都不做保存。
 */

#define SERIAL_PORT	0x3f8
#define IDT_BASE	0x2000
#define IDT_ENTRIES	256
#define TEST_VECTOR	0x21

	.section .text
	.code64

	.globl _start
_start:
	cld

	/* 256 项全部指向 irq_stub */
	lea	irq_stub(%rip), %rsi
	mov	$IDT_BASE, %rdi
	mov	$IDT_ENTRIES, %ecx
	call	fill_idt_entries

	/* 第 0x21 项改指向 irq_handler */
	lea	irq_handler(%rip), %rsi
	mov	$(IDT_BASE + TEST_VECTOR * 16), %rdi
	mov	$1, %ecx
	call	fill_idt_entries

	lidt	idt_ptr(%rip)

	lea	msg(%rip), %rsi
	call	print

	sti
.Lhalt_loop:
	hlt			/* VM-Exit (HLT) → mini-kvm 运行循环 */
	jmp	.Lhalt_loop

/*
 * fill_idt_entries：向 %rdi 连续写 %ecx 个 64 位中断门，目标 = %rsi。
 *
 * IDT 门描述符 16 字节。这个格式的权威规范章节不在本仓库这份
 * intel-vmx.pdf 里（该 PDF 只含 Vol.3C，第 24-33 章），本地无法复核，所以
 * 这里改用可直接读到的 Linux 结构体对表：`struct gate_struct`
 * （arch/x86/include/asm/desc_defs.h:134-143）与 `struct idt_bits`（同文件
 * :119-125）。两者拼出的字节布局与下面写的偏移一一对应：
 *   [15:0]   offset 15:0
 *   [31:16]  selector（guest CS = 0x8）
 *   [39:32]  IST = 0（idt_bits 的 ist:3 + zero:5 低字节）
 *   [47:40]  type/attr = 0x8E（present, DPL0, 64 位中断门）
 *   [63:48]  offset 31:16
 *   [95:64]  offset 63:32
 *   [127:96] 保留（0）
 *
 * 代码都在 4GB 以内，offset 63:32 恒为 0。
 */
fill_idt_entries:
	mov	%rsi, %rax
.Lfill:
	mov	%ax, 0(%rdi)
	movw	$0x8, 2(%rdi)
	movb	$0, 4(%rdi)
	movb	$0x8e, 5(%rdi)
	mov	%rax, %rdx
	shr	$16, %rdx
	mov	%dx, 6(%rdi)
	movl	$0, 8(%rdi)
	movl	$0, 12(%rdi)
	add	$16, %rdi
	loop	.Lfill
	ret

/*
 * 逐字节 OUT 到 COM1（mini-kvm 捕获为串口输出）。
 *
 * 端口必须经 %dx 传递：OUT 的立即数形式端口字段只有 8 位（`out %al,imm8` =
 * E6 ib，实测编码 `e6 f8`；`out %al,%dx` = EE，实测）。写
 * `out %al, $0x3f8` 时 GNU as 会报 `Warning: 0x3f8 shortened to 0xf8` 但
 * **照样生成指令**，objdump 复核得到 `e6 f8   out %al,$0xf8` —— 警告容易被
 * 忽略，而 guest 输出的端口根本不是 COM1，串口捕获恒为空，症状看起来像
 * "guest 没跑起来"。
 */
print:
	push	%rdx
	mov	$SERIAL_PORT, %edx
.Lprint_loop:
	lodsb
	test	%al, %al
	jz	.Lprint_done
	out	%al, %dx
	jmp	.Lprint_loop
.Lprint_done:
	pop	%rdx
	ret

/* vector 0x21 的正式处理：打印并返回被中断的 hlt 循环 */
irq_handler:
	lea	irqmsg(%rip), %rsi
	call	print
	iretq

/* 其余向量的兜底：直接返回 */
irq_stub:
	iretq

	.section .rodata
msg:
	.asciz	"Hello from Mini-KVM Guest!\n"
irqmsg:
	.asciz	"[IRQ 0x21 handled]\n"

idt_ptr:
	.word	IDT_ENTRIES * 16 - 1	/* limit */
	.quad	IDT_BASE		/* base（GPA，恒等映射下可直接用） */
