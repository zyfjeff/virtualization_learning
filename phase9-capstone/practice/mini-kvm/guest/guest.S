/* SPDX-License-Identifier: GPL-2.0 */
/*
 * guest.S —— mini-kvm 的 guest：64 位裸机程序（Stage 3-5 的被运行对象）
 *
 * 由 guest.ld 链接到 GPA 0x1000，用户态测试程序把产物 guest.bin 拷入
 * GPA 0x1000 处。内存视图是测试程序搭建的 2MB 大页恒等映射：
 *   PML4 @ GPA 0x6000 → PDPT @ 0x7000 → PD @ 0x8000，PD[0] = 2MB PS 页
 *
 * 执行流程：
 *   1. lgdt 一个 3 项的最小 GDT（空 / 0x08 代码 / 0x10 数据）——中断投递要按
 *        IDT 门里的 selector 真去 GDT 取描述符，没有 GDT 第一次注入就 #GP；
 *      再在 GPA 0x2000 建 256 项中断门 IDT：
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

	/*
	 * GDT 必须先立起来。VM entry 装载段状态是**从 VMCS 的四个字段直接读**
	 * （selector / base / limit / access rights，SDM 27.3.2 "Loading Guest
	 * Non-Register State"），不查任何描述符表；而 IDT 投递要按门里的
	 * selector 真的去 GDTR.base + index*8 取描述符。所以"guest 没有 GDT"
	 * 这件事在只跑顺序代码时完全看不出来 —— 第一次把中断投进去就退化成
	 * #GP：运行循环报出 exit reason 0 + `VM_EXIT_INTR_INFO` 类型 3（硬件异
	 * 常）vector 13，用户态看到 `-EIO`。实测链见 corrections.md J13。
	 *
	 * 描述符取值抄 arch/x86/boot/compressed/head_64.S:611-612 的
	 * __KERNEL_CS / __KERNEL_DS（64 位代码 L=1、D/B=0；数据段 4G 粒度），
	 * 与 VMCS 里 GUEST_CS_AR_BYTES=0xa09b（L=1、D/B=0）一致。
	 */
	lgdt	gdt_ptr(%rip)

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

	/*
	 * GDT 与它的伪描述符。lgdt 的内存操作数里写的是**线性地址**，而测试
	 * 程序是按 guest.bin 的**文件偏移**把这段字节搬到 GPA 0x1000 的，所以
	 * 伪描述符里那个地址要同时成立两件事：链接期地址 == 文件偏移。
	 *
	 * guest.ld 只有一条 `*(.text) *(.rodata...)`，所有 alloc 段共用同一个
	 * "链接地址 = 文件偏移 + 一个页对齐量" 的平移；本例从 0x1000 起、
	 * .text(0x1000) 与 .rodata(0x10a8) 之间没被页对齐撑开，平移量恰好是 0。
	 * 换布局（加 .data、改对齐、把表挪进别的段）就可能不再是 0，所以这条
	 * 每次都要实测，别照记忆断言：
	 *   readelf -S guest/guest.elf | grep -E 'Address|\.text|\.rodata'
	 *   # Address 列与 Offset 列必须逐段相同
	 *
	 * 填错的症状不是"没反应"而是 guest 立刻 #GP：GDTR 指向一个不存在的表，
	 * 第一次中断投递就取不到描述符（就是下面这张表当初整个缺失的同一症状，
	 * 见 phase8-capstone/corrections.md 的 J13）。
	 */
	.balign	8
gdt:
	.quad	0			/* 0x00 空描述符（不可被选中） */
	.quad	0x00af9a000000ffff	/* 0x08 64 位代码段 = IDT 门里的 selector */
	.quad	0x00cf92000000ffff	/* 0x10 数据段 */
gdt_end:

gdt_ptr:
	.word	gdt_end - gdt - 1
	.quad	gdt
