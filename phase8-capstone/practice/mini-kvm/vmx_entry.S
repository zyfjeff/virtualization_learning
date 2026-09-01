/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmx_entry.S —— VMLAUNCH/VMRESUME 进入 guest 与 VM-Exit 返回路径
 *
 * 【Stage 对应】Stage 1（VMX 基础）的核心：真正的世界切换汇编。
 *
 * 结构逐段对照 KVM 的 __vmx_vcpu_run()
 * （arch/x86/kvm/vmx/vmenter.S:79-305，Linux 6.12.93）：
 *   1. 入口处把被调用者保存寄存器和 vcpu 指针压栈（VM-Exit 后要救命）
 *   2. 写 HOST_RSP = 当前栈顶（每次进入都刷新）
 *   3. 从 vcpu->regs[] 装载 guest 寄存器，RAX 最后（杀掉指针）
 *   4. 依据 launched 标志选 VMRESUME / VMLAUNCH（bt 置 CF，mov 不影响）
 *   5. VM-Exit 时硬件跳转到 HOST_RIP = mini_vmx_vmexit，RSP = HOST_RSP，
 *      于是从栈上恢复 vcpu 指针，把 guest 寄存器存回 regs[]
 *   6. 全部 GPR 清零，防止 guest 值被推测使用（KVM 的 paranoid 措施）
 *
 * C 原型（mini-kvm.h）：
 *   int mini_vmx_enter(struct mini_kvm_vcpu *vcpu, int launched);
 *   返回 0 = 正常 VM-Exit；1 = VMLAUNCH/VMRESUME 失败（错误码见
 *   VM_INSTRUCTION_ERROR，由 C 侧读取）
 *
 * 布局约定：vcpu->regs 必须是 struct mini_kvm_vcpu 的第一个成员
 * （见 mini-kvm.h 的布局注释），顺序同 struct kvm_regs。
 */

#include <linux/linkage.h>
#include <asm/frame.h>
#include <asm/ibt.h>
#include <asm/unwind_hints.h>

#define HOST_RSP_ENCODING	0x6c14	/* VMCS 字段 HOST_RSP（asm/vmx.h 枚举） */

/* vcpu->regs[] 偏移（regs 是 vcpu 首成员） */
#define REG_RAX		 0
#define REG_RCX		 8
#define REG_RDX		16
#define REG_RBX		24
/* regs[4]=RSP(32) 不用：guest RSP 保存在 VMCS GUEST_RSP */
#define REG_RBP		40
#define REG_RSI		48
#define REG_RDI		56
#define REG_R8		64
#define REG_R9		72
#define REG_R10		80
#define REG_R11		88
#define REG_R12		96
#define REG_R13		104
#define REG_R14		112
#define REG_R15		120

/*
 * 栈布局。HOST_RSP 写在 pushed launched 那一格，所以 VM-Exit 落地的瞬间
 * RSP 就是这张表的 [rsp]（低地址在上）：
 *   [rsp   ] launched 标志
 *   [rsp+ 8] vcpu 指针
 *   [rsp+16] rbx
 *   [rsp+24] r12
 *   [rsp+32] r13
 *   [rsp+40] r14
 *   [rsp+48] r15
 *   [rsp+56] 保存的 rbp
 *   [rsp+64] 返回地址
 *
 * VM-Entry 失败（VMLAUNCH/VMRESUME 本身返回）落在同一个 RSP 上，
 * 而且此时寄存器里装的是刚才为 guest 加载的值 —— 所以失败路径必须和退出
 * 路径走同一段收栈代码。KVM 正是这么做的：vmenter.S 的 .Lvmfail 只把返回值
 * 设成 1 就 jmp .Lclear_regs（arch/x86/kvm/vmx/vmenter.S:300-303 → :227）。
 *
 * 判据是"指令居然返回了"，不是"CF=1"：SDM Vol.3C §27.1 里"无 current VMCS /
 * 当前是 shadow VMCS"置 CF，被 MOV-SS 阻塞与 launch state 不匹配置 ZF，
 * §27.2 的控制域/宿主状态检查也置 ZF。三档都不产生 VM-Exit、都不装载宿主
 * 状态，一律 "control passes to the next instruction"，所以紧跟在
 * VMRESUME/VMLAUNCH 后面的 jmp .Lvmfail 能把它们全收到。
 */

	.section .noinstr.text, "ax"

SYM_FUNC_START(mini_vmx_enter)
	push %rbp
	mov  %rsp, %rbp
	push %r15
	push %r14
	push %r13
	push %r12
	push %rbx

	/* 保存 vcpu 指针与 launched 标志到栈上（退出时靠它们恢复） */
	push %rdi			/* vcpu */
	push %rsi			/* launched */

	/*
	 * HOST_RSP = 当前栈顶。VMWRITE 的字段编码必须是寄存器操作数，
	 * 不能是立即数（SDM 31.3）。此值每次进入都刷新，对照
	 * vmx_update_host_rsp()（vmx.c，__vmx_vcpu_run 内联调用）。
	 */
	mov  %rsp, %rax
	mov  $HOST_RSP_ENCODING, %rcx
	vmwrite %rax, %rcx

	/* launched → CF（bt 置标志；随后的 mov 序列不动标志位） */
	mov  (%rsp), %ebx
	bt   $0, %ebx

	/* vcpu 指针进 RAX，regs[] 就在偏移 0 */
	mov  8(%rsp), %rax

	/* 装载 guest 寄存器（对照 vmenter.S:144-162，RSP 经 VMCS） */
	mov  REG_RCX(%rax), %rcx
	mov  REG_RDX(%rax), %rdx
	mov  REG_RBX(%rax), %rbx	/* 覆盖 rbx，CF 不受影响 */
	mov  REG_RBP(%rax), %rbp
	mov  REG_RSI(%rax), %rsi
	mov  REG_RDI(%rax), %rdi
	mov  REG_R8 (%rax), %r8
	mov  REG_R9 (%rax), %r9
	mov  REG_R10(%rax), %r10
	mov  REG_R11(%rax), %r11
	mov  REG_R12(%rax), %r12
	mov  REG_R13(%rax), %r13
	mov  REG_R14(%rax), %r14
	mov  REG_R15(%rax), %r15

	/* 最后装载 guest RAX——杀掉 vcpu 指针（vmenter.S:161-162 同款） */
	mov  REG_RAX(%rax), %rax

	/* 检查 CF：launched=1 走 VMRESUME，否则 VMLAUNCH */
	jnc  .Lvmlaunch

	UNWIND_HINT_SAVE

.Lvmresume:
	vmresume
	jmp  .Lvmfail

.Lvmlaunch:
	vmlaunch
	jmp  .Lvmfail

/*
 * VM-Exit 着陆点：VMCS 的 HOST_RIP 指向这里（在 vmx.c 里写入）。
 * 硬件已完成宿主状态重载（CR0/CR3/CR4/段/GS base/RSP=HOST_RSP，SDM 28.5），
 * 中断默认关闭（PIN 控制未设 ACK）。
 */
SYM_INNER_LABEL_ALIGN(mini_vmx_vmexit, SYM_L_GLOBAL)
	UNWIND_HINT_RESTORE
	ENDBR

	/* 暂存 guest RAX，再取回 vcpu 指针 */
	push %rax
	mov  8(%rsp), %rax		/* [guest_rax, launched, vcpu, ...] */

	/* 保存全部 guest 寄存器（对照 vmenter.S:205-222） */
	pop  REG_RAX(%rax)
	mov  %rcx, REG_RCX(%rax)
	mov  %rdx, REG_RDX(%rax)
	mov  %rbx, REG_RBX(%rax)
	mov  %rbp, REG_RBP(%rax)
	mov  %rsi, REG_RSI(%rax)
	mov  %rdi, REG_RDI(%rax)
	mov  %r8,  REG_R8 (%rax)
	mov  %r9,  REG_R9 (%rax)
	mov  %r10, REG_R10(%rax)
	mov  %r11, REG_R11(%rax)
	mov  %r12, REG_R12(%rax)
	mov  %r13, REG_R13(%rax)
	mov  %r14, REG_R14(%rax)
	mov  %r15, REG_R15(%rax)

	/* 返回值：0 = 正常 VM-Exit（对照 vmenter.S:225 的 xor %ebx,%ebx） */
	xor  %ebx, %ebx
	jmp  .Lrestore_host

/*
 * 收栈。到这里两条路径已经合并，RSP 都指向 launched 那一格：
 *   - VM-Exit：上面的 pop REG_RAX 已经把暂存的 guest RAX 那一格释放掉；
 *   - VM-Entry 失败：根本没有 push 过。
 */
.Lrestore_host:
	add  $16, %rsp			/* 丢弃 launched + vcpu 两格 */

	/*
	 * 清零 GPR，防止 guest 值被推测使用，连后面要从栈上取回的那些也一样
	 * （对照 vmenter.S:227-255 的 .Lclear_regs：先清，再 pop）。
	 * RSP 与 RBX 是 KVM 明确豁免的两个：RSP 由硬件在 VM-Exit 时恢复
	 * （"RSP is restored by hardware during VM-Exit"），RBX 此刻装的是
	 * 返回值 0/1。RBP 不在豁免之列——它是 guest 的值，必须清掉，
	 * 宿主自己的 rbp 稍后从栈上 pop 回来。
	 */
	xor  %eax, %eax
	xor  %ecx, %ecx
	xor  %edx, %edx
	xor  %ebp, %ebp
	xor  %esi, %esi
	xor  %edi, %edi
	xor  %r8d, %r8d
	xor  %r9d, %r9d
	xor  %r10d, %r10d
	xor  %r11d, %r11d
	xor  %r12d, %r12d
	xor  %r13d, %r13d
	xor  %r14d, %r14d
	xor  %r15d, %r15d

	mov  %ebx, %eax			/* 返回值放进 RAX */

	/* 严格按 push 的逆序恢复被调用者保存寄存器，最后才是 rbp + RET */
	pop  %rbx
	pop  %r12
	pop  %r13
	pop  %r14
	pop  %r15
	pop  %rbp
	RET

.Lvmfail:
	/*
	 * VMLAUNCH/VMRESUME 失败（§27.1/§27.2 三档之任一，见上面栈布局注释）：
	 * 没进 guest，但寄存器刚才已经装载成 guest 的值，不可信也不回写
	 * regs[]；RSP 就是 HOST_RSP。是 §27.1 还是 §27.2 造成的，由 C 侧解码
	 * VM_INSTRUCTION_ERROR 区分（vmx.c 的 mini_vmx_report_error()）。
	 * 和 KVM 一样只改返回值，然后并入上面那段收栈（vmenter.S:300-303）。
	 */
	mov  $1, %ebx
	jmp  .Lrestore_host

SYM_FUNC_END(mini_vmx_enter)
