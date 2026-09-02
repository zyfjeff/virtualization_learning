# Stage 3: 中断处理

> 对应课程 Phase 4: Interrupts (APIC, Posted Interrupts)
>
> 关键源码: `arch/x86/kvm/lapic.c`
>           `arch/x86/kvm/vmx/posted_intr.c`
>           `arch/x86/kvm/vmx/vmx.c::vmx_inject_irq()` / `vmx_do_nmi_irqoff()`

---

## 🎯 阶段目标

打通一条最小可用的事件注入通路：

- 用户态怎么请求"给这个 vCPU 注入 vector N"
- 注入信息要写进哪个 VMCS 字段、格式是什么
- 硬件对注入有哪些**进入检查**，不满足会怎样
- 宿主自己的外部中断/NMI 在 VMX 下走哪条路（以及 mini-kvm 与 KVM 的分歧）

## 📖 核心概念

### 注入通道只有一个字段

VM-Entry 注入事件全靠 **VM-entry interruption-information field**（32 位，
编码 `0x00004016`，`arch/x86/include/asm/vmx.h`）：

```
Table 25-17. Format of the VM-Entry Interruption-Information Field（SDM 25.8.3）
7:0    vector
10:8   类型：0=外部中断 1=保留 2=NMI 3=硬件异常 4=软件中断(INT n)
       5=特权软件异常(INT1) 6=软件异常(INT3/INTO) 7=其他事件
11     deliver error code
30:12  保留，必须为 0
31     valid
```

配套还有 **VM-entry exception error code**（仅 bit31 与 bit11 同时为 1 时有效）
和 **VM-entry instruction length**（仅注入 type 4/5/6 时用来决定压栈的 RIP）。
两条容易忽略的硬件事实：

- **每次 VM-Exit 都会清掉 bit31**（SDM 25.8.3："The valid bit in this field
  is cleared on every VM exit"）。所以注入是"一次性"的，退出后必须重新写。
- 类型 1 是保留值，注入 type 7（other event）在不支持 monitor trap flag 的
  处理器上是保留值 —— 都在 **SDM 27.2.1.3** 的进入检查里，违反就是
  VM-Entry failure。

### 三道门槛（都在 VM-Entry 检查里，不是"建议"）

| 检查 | 规范位置 | 内容 |
|------|---------|------|
| 字段自洽 | SDM 27.2.1.3 | type=NMI ⇒ vector 必须为 2；type=硬件异常 ⇒ vector ≤ 31；保留位 30:12 必须为 0 |
| `RFLAGS.IF` | SDM 27.3.1.4 | "The IF flag (RFLAGS[bit 9]) must be 1 if the valid bit … is 1 and the interruption type … is **external interrupt**" |
| 阻塞状态 | SDM 27.3.1.5 | bit0（STI 阻塞）与 bit1（MOV-SS 阻塞）在注入 type 0 **或 type 2（NMI）** 时都必须为 0 |

第三条最容易被漏：很多人以为只有外部中断受限，规范里 NMI 注入同样要求
这两位为 0。反过来，`bit3`（NMI 阻塞）只有在 "virtual NMIs" = 1 时才对注入
有约束（SDM 27.3.1.5 的 NOTE 明说 virtual-NMI 控制为 0 时不要求 bit3 = 0）——
mini-kvm 没开 virtual NMIs，所以对 bit3 的处理纯属策略。

任何一条不满足都是 **VM-Entry failure**（不是"这次不注入"），运行循环会看到
`VM_EXIT_REASON` 的 bit31。

### 宿主的中断从哪来

mini-kvm **没有**开 "acknowledge interrupt on exit"（VM-exit 控制 **bit 15**）。
SDM 28.1 写得很直接：

> "An external interrupt does not acknowledge the interrupt controller and the
> interrupt remains pending, unless the 'acknowledge interrupt on exit'
> VM-exit control is 1."

所以 `EXIT_REASON_EXTERNAL_INTERRUPT` 退出时中断还挂在 LAPIC 上，
`VM_EXIT_INTR_INFO` 是**无效**的（SDM 28.2.2 明确 ack=0 时该字段 bit31 清零、
其余 undefined）。运行循环因此什么都不读，只开一扇中断窗口让宿主 IDT 把这个
pending 中断就地消费掉，然后重新进入 guest：

```c
/* 来源: vcpu.c:285-303（EXIT_REASON_EXTERNAL_INTERRUPT 分支） */
local_irq_enable();
vcpu->n_extint_exits++;		/* 这条指令是给中断影子留的出口 */
local_irq_disable();
```

**sti 与 cli 之间必须夹一条指令。** 处理器执行完 STI 之后的那条指令才认可
pending 的可屏蔽中断（这条"中断影子"规则定义在 Vol.3A 的 RFLAGS/STI 说明里，
本仓库这份 PDF 只有 Vol.3C，给不出可核对的章节号，故只引下面的源码），所以
背靠背的 `sti; cli` 等于刚把门推开又关上，一个中断都收不到；向量还留在
LAPIC IRR，下一次 VM entry 立刻又以原因 1 退出 —— 于是 VM-Exit 风暴：guest
不再推进，宿主这个 CPU 的 tick 永远进不来（soft lockup / RCU stall）。症状
是"模块把机器拖死了"，而代码里一行错都没有，静态审查最容易放过去。

KVM 同一处放的正是 `local_irq_enable(); ++vcpu->stat.exits;
local_irq_disable();`，理由写在它自己的注释里
（`arch/x86/kvm/x86.c:11149-11158`）：

> "An instruction is required after local_irq_enable() to fully unblock
> interrupts on processors that implement an interrupt shadow, the stat.exits
> increment will do nicely."

至于 ack-on-exit 这个控制位本身，KVM 走的是相反的路：`VM_EXIT_ACK_INTR_ON_EXIT` 在
`__KVM_REQUIRED_VMX_VM_EXIT_CONTROLS` 里（`arch/x86/kvm/vmx/vmx.h:515-517`），
是**必需位**。既然向量已经被硬件从 LAPIC 取走了，KVM 必须自己把它交付出去，
于是在 root 模式直接跳进宿主 IDT 的对应门：
`handle_external_interrupt_irqoff()` → `vmx_do_interrupt_irqoff(gate_offset(host_idt_base + vector))`
（`vmx/vmx.c:7013-7030`）。

## 🔧 实现（`interrupt.c` + 运行循环）

### 1. 排队，不碰 VMCS（`interrupt.c::mini_vcpu_inject_irq()`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/interrupt.c:55-71 */
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
```

三个要点：

- **只登记到 `pending_intr_info`，不写 VMCS**。ioctl 可能落在任何 CPU 上，
  而那个 CPU 未必 `VMPTRLD` 了这个 VMCS —— VMREAD/VMWRITE 会直接失败。
  真正写 VMCS 的时机只有一个：运行循环即将进入 guest 之前。
- `vector < 32` 是**软件策略**（0-31 是异常保留区），不是硬件要求；
  硬件对 type=0 的 vector 没有范围检查。
- `INTR_TYPE_EXT_INTR` 定义成 `EVENT_TYPE_EXTINT << 8`
  （`arch/x86/include/asm/vmx.h:400`），已经是 bits 10:8 的位置，不要再乘。
- 一次只排队一个：注入信息的 valid 位一次只能带一个事件，硬件也没有
  "注入队列"这回事（KVM 的队列在 vLAPIC 的 PIRR/IRR 位图里）。

### 2. 注入窗口（`vcpu.c::mini_vcpu_run_loop()`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/vcpu.c:152-162 */
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
```

注意最后一行**无条件**执行：没东西可注入时写入 0，正好把上一轮的残留清掉
（虽然 VM-Exit 已经清过 valid 位，显式写 0 让状态可依赖）。清阻塞位用
`~0x3`，正好对应 SDM 27.3.1.5 的 bit0/bit1；SMI(bit2)/NMI(bit3)/enclave(bit4)
都不能乱动 —— bit2 在非 SMM 下必须为 0，但那是 guest 状态本身的正确性问题，
不该由 VMM 在注入路径上顺手改。

KVM 不做这种"改写 guest RFLAGS"的事。它判定能不能注入用的是同一组条件
（`__vmx_interrupt_blocked()`，`vmx/vmx.c:5076-5081`：`!IF` 或
interruptibility 的 STI|MOV_SS），不能注入就**等**：`enable_irq_window` 打开
"interrupt-window exiting"，guest 一开中断就退出，再由
`handle_interrupt_window()`（`vmx/vmx.c:5658`）继续投递；开了 APICv 时
向量放在 RVI 里由硬件评估（`vmx_set_rvi()`，`vmx/vmx.c:6881`）。
注入动作本身在 `vmx_inject_irq()`（`vmx/vmx.c:4958-4984`），最后一句
`vmx_clear_hlt(vcpu)`（`vmx/vmx.c:1817-1828`）才是 KVM 处理"guest 在 HLT
状态下收中断"的手法：改 **activity state**，而不是改 RFLAGS。
（SDM 27.3.1.5 允许 HLT 状态下注入外部中断与 NMI，所以这一步对进入检查
不是必需的，KVM 是为了让 guest 真的从 hlt 醒过来。）

mini-kvm 直接强制 IF=1 + 清 bit0/bit1，对我们的教学 guest 语义等价：
guest 本来就 `sti` 之后 `hlt` 等注入，IF 早就是 1，阻塞位早就是 0。

### 3. Guest 侧（`guest/guest.S`）

guest 在 GPA 0x2000 建 256 项 16 字节中断门（type/attr = `0x8E`，
present + DPL0 + 64 位中断门），全部指向 `irq_stub`（直接 `iretq`），
只有第 `0x21` 项指向 `irq_handler`；填 IDT 之前还要先 `lgdt` 装一张 3 项
GDT（空 / 64 位代码段 / 数据段，`guest/guest.S:176-184`）：

```asm
/* 来源: phase8-capstone/practice/mini-kvm/guest/guest.S:41-79（注释略） */
	lgdt	gdt_ptr(%rip)

	lea	irq_stub(%rip), %rsi
	mov	$IDT_BASE, %rdi
	mov	$IDT_ENTRIES, %ecx
	call	fill_idt_entries

	lea	irq_handler(%rip), %rsi
	mov	$(IDT_BASE + TEST_VECTOR * 16), %rdi
	mov	$1, %ecx
	call	fill_idt_entries

	lidt	idt_ptr(%rip)
	...
	sti
.Lhalt_loop:
	hlt			/* VM-Exit (HLT) → mini-kvm 运行循环 */
	jmp	.Lhalt_loop
```

**为什么非要有这张 GDT**：VM entry 装载 guest 段状态是**从 VMCS 的四个字段直接
读**（selector / base / limit / access rights，SDM 27.3.2），CPU 不查任何描述符
表；而 IDT 投递要按门里的 selector 真的去 `GDTR.base + index*8` 取描述符。所以
"guest 没有 GDT"在只跑顺序代码时**完全看不出来**，第一次把中断投进去才炸 —— 本
模块第一次上机就是这条：GDTR.base=0，读 selector 0x8 的描述符就是读 GPA 0x8，投递
途中 EPT violation；第 0 页被按需映射补上之后，从那里读到的又不是合法描述符，于是
同一轮投递改成 #GP（exit reason 0 + `VM_EXIT_INTR_INFO` 类型 3 / vector 13），用户
态拿到 `-EIO`。完整链条见下文第 5 节与 corrections.md J13。

`hlt` 触发 HLT exiting → 运行循环把 `KVM_EXIT_HLT` 交给用户态并**返回**。
下一次 `KVM_RUN` 带着排队的 `0x21` 进入，硬件在 VM-Entry 时把中断"当作
刚刚发生"投给 guest，CPU 走 IDT 门进 `irq_handler`，打印后 `iretq` 回到
`hlt` 的下一条继续停机 —— 于是用户态第二次拿到 `KVM_EXIT_HLT`。

### 4. NMI：mini-kvm 与 KVM 在这里是分歧

`PIN_BASED_NMI_EXITING` = 1 时，宿主的 NMI 会以 **退出原因 0（EXCEPTION_NMI）
+ `VM_EXIT_INTR_INFO` type=2 / vector=2** 出现（SDM 28.2.2）。运行循环
（`vcpu.c:305-312`）认出这个组合后调 `mini_vcpu_reinject_nmi()`，把 NMI
转注给 guest：

```c
/* 来源: phase8-capstone/practice/mini-kvm/interrupt.c:151-165 */
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
```

**别把这段当成 KVM 的做法。** KVM 认为这种 NMI 属于**宿主**：在 root 模式
直接跳进宿主 IDT 的 NMI 门把它消费掉（`vmx_vcpu_enter_exit()` 里
`vmx_do_nmi_irqoff()`，`vmx/vmx.c:7330-7338`；声明 `:6978`），随后
`handle_exception_nmi()` 见到 `is_nmi()` 直接 `return 1`，理由是"NMIs are
handled by vmx_vcpu_enter_exit()"（`vmx/vmx.c:5225-5231`）。KVM 注入给 guest
的 NMI 另有来源：用户态 `KVM_NMI` ioctl（`x86.c:5193-5197` → `kvm_inject_nmi()`
→ `KVM_REQ_NMI` → `process_nmi()`），以及"上一轮已注入但 guest 还没消费"的
重投（`x86.c:10381-10388`）。

mini-kvm 转注给 guest 的取舍：教学 guest 是本模块唯一的执行体，这样能让
guest 的 NMI 路径可观测；代价是**宿主自己的 NMI 被抢走**，需要宿主 NMI
（MCE 打印、NMI watchdog）的场景里这是错的。`bit3` 那一发放弃也只是策略
（我们没开 virtual NMIs，硬件并不要求 bit3 = 0），真实 KVM 会记账后择机重注
（`vmx_set_nmi_mask()` / `vmx_nmi_blocked()`，`vmx/vmx.c:5028-5062`）。

### 5. 注入 ≠ 投递：VM-Exit 之后与硬件对账

第 2 节写完 `VM_ENTRY_INTR_INFO_FIELD` 就算"注入了"，读代码时很容易顺着得出"这一
轮 guest 必然收到了中断"。规范不是这么说的，**投递是 entry 流程里一段还会访存的
动作**：

- SDM 27.6.1（Vectored-Event Injection）：事件投递发生在 VM entry **载入 guest 状态
  与控制字段之后**。投递要做好几次访存——读 IDT 门描述符、按门里的 selector 走
  GDTR 取段描述符、往 guest 栈上压中断帧、再取处理器的第一条指令。
- 每一次访存都可能撞进未映射的 guest 页。SDM 28.2.4 的原文列表里就有这一条：
  *"An EPT violation, EPT misconfiguration, page-modification log-full event, or
  SPP-related event that occurs during event delivery."*

这类退出会额外留下一条记录：**IDT-vectoring information**（编码 `0x00004408`，
`arch/x86/include/asm/vmx.h:314`）。格式在 Table 25-20：vector 在 [7:0]、类型在
[10:8]、bit11 是 error-code 有效位；而 **bit31 对"投递途中退出"恒为 1、对其余退出
恒为 0**（SDM 28.2.4 末尾）。换句话说，硬件把没投完的事件替你记着，等你拿回去。

mini-kvm 少了这一步会怎样：事件槽 `pending_intr_info` 在循环头就被
`mini_vcpu_take_intr_info()`（`vcpu.c:152`）清空了，EPT-violation 分支 `continue`
回循环头时 `VM_ENTRY_INTR_INFO_FIELD` 被写成 0 —— **事件被硬件记着、被软件丢掉**，
guest 回到被中断的指令继续跑。第一次上机量到的症状就是它的样子：串口里没有
`[IRQ 0x21 handled]`，计数器 `inj=1` 而 `io` 一个没涨，全程没有任何报错。

修法照 KVM 的形状：`__vmx_complete_interrupts()`（`arch/x86/kvm/vmx/vmx.c:7111-7163`）
先**无条件**清掉 `nmi_injected` 与 exception/interrupt 两个队列（`:7121-7124`），
再只看 `idtv_info_valid`（`:7126`）；有效时按类型重新排队，外部中断走
`kvm_queue_interrupt()`（`:7156-7158`），而它**第一件事就是置
`interrupt.injected = true`**（`arch/x86/kvm/x86.h:144`）—— 把硬件保管的事件重新
拿回软件侧，下次进入前由 `kvm_check_and_inject_events()`（`x86.c:10342`）再写一遍
VMCS（`x86.c:10386-10387`）。

```c
/* 来源: phase8-capstone/practice/mini-kvm/interrupt.c:122-143（注释略） */
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
}
```

调用点放在分发 switch **之前**（`vcpu.c:282`，理由在 `vcpu.c:274-281`）：对账的对象
是"上一次 VM entry 有没有投完"，与这一次退出的是什么原因无关，放在 switch 里就只
覆盖了其中一条分支；而 `KVM_EXIT_INTERNAL_ERROR`/`KVM_EXIT_HLT` 那几条 `goto out`
回用户态的路径同样可能带着一个没投完的事件（用户态下一次 `KVM_RUN` 该重投它）。

两条边界，都是读 KVM 得到的：

- 本模块只重投外部中断。NMI 与硬件异常也会走这条路（KVM 在
  `vmx/vmx.c:7133-7158` 按类型分别记账，NMI 还要顺带清 NMI 阻塞位
  `:7135-7141`），mini-kvm 不注入这两类，所以落到"丢弃"。
- 重投的是**同一个 vector**，不是重新问一遍中断控制器。别以为"KVM 有 LAPIC，丢了
  下次会从 IRR 再评估一遍"——KVM 在**取走向量的那一刻就 ack 了**：
  `kvm_cpu_get_interrupt()`（`arch/x86/kvm/irq.c:139`）拿到 vector 后立刻调
  `kvm_apic_ack_interrupt()`（`:147`），后者清 IRR（`lapic.c:3015`）并置 ISR
  （`:3030`）。所以事件一旦丢了，KVM 也只剩 `interrupt.injected` 这一份软件记录可
  言，跟 mini-kvm 一样只能靠 IDT-vectoring 兜住。真正的差别是 KVM 还有 ISR/EOI
  账本（guest 的 EOI 有对应位可清），而我们的 vector 是用户态一次性递交的。

本节按"追加为第 5 节"的方式插入，前三节的编号不变。

## 🔑 关键差异: mini-kvm vs 真实 KVM

| 特性 | mini-kvm | 真实 KVM |
|------|----------|---------|
| 中断控制器 | 无（用户态直接指定 vector） | vLAPIC + PIC + IOAPIC，`kvm_irq_delivery_to_apic()`（`arch/x86/kvm/irq_comm.c:47`） |
| 排队结构 | 单个 `pending_intr_info` | IRR/PIR 位图 + RVI/SVI |
| 投递对账 | 每次真实退出后查 IDT-vectoring bit31，放回唯一事件槽（`interrupt.c:122-143`） | `__vmx_complete_interrupts()` 按类型重排（`vmx.c:7111-7163`）；保存/恢复 vCPU 状态时还会再排一次（`x86.c:12096-12099` + `x86.h:156` `kvm_event_needs_reinjection()`） |
| 等窗口 | 无：注入前强改 IF 与 interruptibility | interrupt-window / NMI-window exiting |
| ack-on-exit | 关（中断留在 LAPIC，靠开/关中断的窗口 + 中间那条指令交付宿主，见上「宿主的中断从哪来」） | `__KVM_REQUIRED_VMX_VM_EXIT_CONTROLS` 必需位，KVM 自己跳宿主 IDT（`vmx.c:7013-7030`） |
| 宿主 NMI | 转注给 guest（分歧点，见上） | 宿主自己消费（`vmx_do_nmi_irqoff()`） |
| NMI 阻塞 | 只看 bit3，放弃即丢 | virtual NMIs（`enable_vnmi`）或软件 `soft_vnmi_blocked` |
| Posted Interrupts | 无 | `posted_intr.c`：PIR→VIRR、RVI 由硬件评估，正常路径 0 次 VM-Exit（SDM 30.6） |
| 异常捕获 | `EXCEPTION_BITMAP` = #DB/#UD/#GP/#PF（本模块 `vmx.c:724-725`） | 按需 + 软件模拟（`handle_exception_nmi()`） |

## 🧪 实验验证

```bash
sudo ./test-mini-kvm
# --- 注入 vector 0x21 并第二次 KVM_RUN ---
# [信息] 串口缓冲: Hello from Mini-KVM Guest!
#          [IRQ 0x21 handled]
# [信息] ✓ 中断注入处理 验证通过
```

第一次 RUN 的验收行是 `[信息] ✓ 退出原因 = KVM_EXIT_HLT (5)`
（`KVM_EXIT_HLT = 5`，`include/uapi/linux/kvm.h:151`）。串口缓冲是
`MINI_KVM_VM_GET_SERIAL` 一次性读出的**累计**内容，所以第二次读到的是两行。

内核侧的统计行走 `pr_debug`（`vcpu.c:394-397`），要先打开 dynamic debug：

```bash
echo -n 'module mini_kvm +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
sudo dmesg | grep mini-kvm | tail
# mini-kvm: 排队注入外部中断 vector=0x21
# mini-kvm: RUN 结束 exits=… io=46 ept=… hlt=2 extint=… nmi=0 inj=1
```

`exits`/`ept`/`extint` 取决于按需映射命中数与宿主中断，不必对齐；能对上账的是
后三个：计数器全程累加且从不归零，`io=46` = 欢迎语 27 字节 + 中断消息 19 字节
（逐字节 `OUT`，每字节一次退出），`hlt=2` = 两次停机，`inj=1` = 这一次注入。

这两行是 2026-09-02 真机跑出来的（`exits` 与 `io`/`hlt`/`inj` 的差值就是账）：

```
mini-kvm: RUN 结束 exits=90  io=27 ept=6 hlt=1 extint=56 nmi=0 inj=0
mini-kvm: RUN 结束 exits=110 io=46 ept=6 hlt=2 extint=56 nmi=0 inj=1
```

`extint=56` 用的是一个**故意空转**的 guest 变体（标准 guest 一上来就 `hlt`，几
乎不会撞上宿主中断，这条路径就没被测到 —— 见 corrections.md J13）；第二轮
`io` 正好 +19，就是 `[IRQ 0x21 handled]\n` 的字节数，说明事件确实走到了
`irq_handler`。

`[IRQ 0x21 handled]` 说明 guest 真的走了 IDT 第 0x21 项
（`irq_handler`），而不是兜底的 `irq_stub` —— 后者只会静默 `iretq`，串口什么
都不会多。想验证注入失败的样子：换个 guest 没专门处理的 vector，串口只会多
一个空行都算幸运；换 `< 32` 会被内核侧 `-EINVAL` 拒绝（`interrupt.c:59-63`）。

## 📝 检查清单

- [ ] 说出注入事件用到的**唯一**VMCS 字段及其 5 个字段位段
- [ ] 复述三道进入检查分别在哪一节（27.2.1.3 / 27.3.1.4 / 27.3.1.5）
- [ ] 解释为什么 NMI 注入也要清 interruptibility 的 bit0/bit1
- [ ] 为什么 `mini_vcpu_inject_irq()` 不直接 VMWRITE
- [ ] ack-on-exit = 0 时 `VM_EXIT_INTR_INFO` 是什么状态，KVM 为什么必须开它
- [ ] mini-kvm 处理宿主 NMI 的方式与 KVM 有什么风险差异
- [ ] "注入"与"投递"差在哪一步？投递途中退出时硬件在哪个字段的哪一位留下记录，
      软件该拿它怎么办（第 5 节）
- [ ] VM entry 根本不查描述符表，guest 为什么还必须有 GDT？少了它会以什么形式暴
      露（第 3 节）

## 🔗 下一步

Stage 4: 设备模拟（串口）
