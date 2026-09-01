# Stage 5: 运行循环

> 对应课程 Phase 0（KVM 框架）+ Phase 9（性能优化）
>
> 关键源码: `arch/x86/kvm/x86.c::vcpu_run()`（:11343）
>           `arch/x86/kvm/x86.c::vcpu_enter_guest()`（:10777）
>           `arch/x86/kvm/vmx/vmx.c::vmx_exit_handlers_fastpath()`（:7264）
>           `virt/kvm/kvm_main.c::kvm_vcpu_halt()`（:3811）

---

## 🎯 阶段目标

把前四个阶段拼成一个能跑的执行体，并说清楚**哪些事循环必须自己做、哪些是
KVM 有而 mini-kvm 故意没有的**：

- 一次 `KVM_RUN`  ioctl 在内核里到底转了多少圈
- 进入前的准备工作为什么只属于循环外（以及搞错顺序会死锁在哪）
- VM-Exit 之后"留在内核重进"和"回用户态"的分界
- 为什么 mini-kvm 全程关抢占，而 KVM 只在进出那一小段关

## 📖 核心概念

### KVM 是两层，mini-kvm 是一层

```
KVM:  kvm_arch_vcpu_ioctl_run()
        └→ vcpu_run()                          x86.c:11343   ← 外层，可睡眠
             for (;;) {
               kvm_vcpu_running() ? vcpu_enter_guest() : vcpu_block()
               r <= 0            → break，回用户态
               KVM_REQ_UNBLOCK / pending timer / irq-window / 信号
             }
        └→ vcpu_enter_guest()                  x86.c:10777   ← 内层，跑一圈事件
             KVM_REQ_* 全部消费 → inject_pending_event() → static_call(vcpu_run)
             preempt_disable()（:10979）… VM-Entry … VM-Exit …（:11171 才 enable）

mini-kvm: mini_vcpu_run_ioctl()
        └→ mini_vcpu_run_loop()                vcpu.c:62     ← 一层 for(;;)
             全程 preempt_disable（:69）；要回用户态就 goto out
```

外层与内层的分界在 KVM 里是"**要不要让出 CPU**"：`vcpu_block()` /
`kvm_vcpu_halt()` 会 `schedule()`，`xfer_to_guest_mode_handle_work()` 会处理
信号。mini-kvm 里这三件事都不存在，所以一层循环够用。

### 快路径 ≠ "不回用户态"

老文档常把 KVM 的 `EXIT_FASTPATH_*` 说成"VM-Exit 优化的总称"，其实 6.12.93
里它的口径窄得很：`vmx_exit_handlers_fastpath()`（`vmx/vmx.c:7264-7285`）只
认 `MSR_WRITE`、`PREEMPTION_TIMER`、`HLT` 三种，其余一律 `EXIT_FASTPATH_NONE`
回到 `vcpu_enter_guest()` 的正常路径。真正决定"这次退出用户态看不看得到"的是
`vcpu_run()` 的 `r <= 0` 那个判断，而不是 fastpath。

mini-kvm 的分界更直白：**`continue` = 留在内核重进；`goto out` = 回用户态**。

| VM-Exit 原因 | mini-kvm 的处理 | 去留 |
|---|---|---|
| `EXTERNAL_INTERRUPT`(1) | 不读 `VM_EXIT_INTR_INFO`（ack-on-exit=0，该字段无效），`local_irq_enable(); local_irq_disable();` 让宿主 IDT 消费 pending 中断 | `continue` |
| `EXCEPTION_NMI`(0) + type=2/vector=2 | `mini_vcpu_reinject_nmi()` 转注给 guest（Stage 3） | `continue` |
| `EXCEPTION_NMI` 的其他（#DB/#UD/#GP/#PF） | 打印 RIP + `VM_EXIT_INTR_INFO` + `mini_dump_vmcs()` | `KVM_EXIT_INTERNAL_ERROR`，回用户态 |
| `IO_INSTRUCTION`(30) | `mini_handle_io_exit()`：串口 0x3f8 收字节，其余忽略；RIP += `VM_EXIT_INSTRUCTION_LEN` | `continue`（串 IO / 解码失败 → `-EIO` 回用户态） |
| `EPT_VIOLATION`(48) | `mini_ept_handle_violation()` 按需映射，硬件重放 | `continue`（GPA 不在 memslot → `-EFAULT`） |
| `CPUID`(10) | `regs[0] = 0` + RIP += 长度（guest 不该执行它） | `continue` |
| `HLT`(12) | `run->exit_reason = KVM_EXIT_HLT` | `goto out`，回用户态 |
| `TRIPLE_FAULT`(2) | 打印 + `mini_dump_vmcs()` | `KVM_EXIT_SHUTDOWN`，回用户态 |
| 其他 | 打印未处理原因 + `mini_dump_vmcs()` | `KVM_EXIT_INTERNAL_ERROR`，回用户态 |

括号里的 basic exit reason 编号取自
`arch/x86/include/uapi/asm/vmx.h:32-74`；`EXCEPTION_NMI` = 0 与
`EXTERNAL_INTERRUPT` = 1 挨着，是最容易记反的一对。

`VM_EXIT_REASON` 的 bit31 是 "entry failure"，在进入 switch 之前就单独拦掉了
（`vcpu.c:229-267`）—— 混进 switch 会看到一个"reason 号但字段全不可信"的
假退出。

## 🔧 实现（`vcpu.c::mini_vcpu_run_loop()`）

### 1. 上机：一次性，且必须在循环外

```c
/* 来源: phase8-capstone/practice/mini-kvm/vcpu.c:69-130（注释与错误分支略） */
preempt_disable();

if (!mini_cpu_in_vmx_operation()) { ... KVM_EXIT_INTERNAL_ERROR / -EPROTO ... }

if (vcpu->loaded_cpu != raw_smp_processor_id()) {
	int old_cpu = vcpu->loaded_cpu;

	mini_vmcs_clear(vcpu);			/* 投递到旧 CPU 上 VMCLEAR */
	if (mini_vmptrld(vcpu->vmcs_phys)) { ... }
	if (mini_ept_invept_global())		/* all-context INVEPT */
		pr_warn("mini-kvm: CPU%d 上 INVEPT global 失败\n", ...);
	vcpu->loaded_cpu = raw_smp_processor_id();
	mini_vmx_fixup_host_for_cpu();
	pr_info("mini-kvm: vCPU 上机 CPU%d→CPU%d, VMCS 重新加载\n", ...);
}

for (;;) {
	...
```

对照 KVM：这些全在进主循环前的一次 `vcpu_load()` 里做完
（`arch/x86/kvm/x86.c:11590` → `virt/kvm/kvm_main.c:205-213` →
`kvm_arch_vcpu_load()` `x86.c:4982` → `kvm_x86_call(vcpu_load)` `x86.c:5002` →
`vmx_vcpu_load_vmcs()`），不是每次进入前重做。

**顺序错了会死锁**，而且是硬死锁：`mini_vmcs_clear()` 在旧 CPU 不是本机时
走 `smp_call_function_single(cpu, …, wait=1)`，而 `kernel/smp.c:647-653` 的
注释写明了 "`Can deadlock when called with interrupts disabled.`"。运行循环
里有好几条 `continue` 路径是带着关中断状态回到循环头的（EPT violation、
CPUID、NMI 再注入），所以这段迁移只能放在 `for (;;)` 之前 —— 那里中断还开着。

### 2. 循环头：每次进入前只刷"会漂移"的东西

```c
/* 来源: phase8-capstone/practice/mini-kvm/vcpu.c:138-157（注入窗口的注释略） */
/* 每次进入前刷新会漂移的 Host 字段（CR3/CR4/GS/FS） */
mini_vmx_refresh_host_state();

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

Host 的 CR3/CR4/GS base 每次都要重取：这个线程可能已经被调度到别的 CPU
（per-CPU 的 CR3/CR4 值不同），也可能刚被 `switch_mm` 换了地址空间。
VMCS 里的 Host 字段是**上一次写进去的快照**，不会跟着宿主变。

紧接着是那段"关中断进、关中断出"的窗口：

```c
/* 来源: phase8-capstone/practice/mini-kvm/vcpu.c:188-193 */
gs_shadow = mini_rdmsr(MSR_KERNEL_GS_BASE);

local_irq_disable();
r = mini_vmx_enter(vcpu, vcpu->launched);
mini_wrmsr(MSR_KERNEL_GS_BASE, gs_shadow);
/* 退出时中断仍关闭（硬件按 HOST 状态恢复，默认 IF=0） */
```

`mini_vmx_enter()` 是 `vmx_entry.S` 里那套手写世界切换，返回值 0 = 真的进过
guest，非 0 = VM-Entry 检查失败。失败后立即 `mini_vmx_report_error()` 解
`VM_INSTRUCTION_ERROR` + `mini_dump_vmcs()`（`vcpu.c:202-210`），别指望
switch 还能救它。

`launched` 标志只在第一次成功后置位（`vcpu.c:211-212`）：VMCS 的 launch state
是 "clear" 时执行 VMRESUME 会得到 VM-instruction error 5（SDM 31.4
Table 31-1），而每次迁移里的 `VMCLEAR` 都会把它打回 "clear"
（SDM 25.11.3），所以 `mini_vmcs_clear()` 必须同时清掉 `vcpu->launched`。

### 3. 收尾

`goto out` / `break` 之后只有一句 `preempt_enable()` 和一行 `pr_debug` 统计
（`vcpu.c:366-369`）：

```
mini-kvm: RUN 结束 exits=%llu io=%llu ept=%llu hlt=%llu extint=%llu nmi=%llu inj=%llu
```

计数器是 vCPU 级、全程累加、跨 `KVM_RUN` 不清零 —— 想看单次 RUN 的构成，
得自己对比两行的差值。

## ⚡ mini-kvm 没有的性能手段

### 全程 preempt_disable

KVM 只在 `vcpu_enter_guest()` 的 `preempt_disable()`（`x86.c:10979`）到
`preempt_enable()`（`x86.c:11171`）之间关抢占，覆盖的就是 VM-Entry/Exit 那
一小段；`vcpu_run()` 外层是完全可以被抢占、可以睡眠的。

mini-kvm 的 `preempt_disable()` 在循环**外面**（`vcpu.c:69`），一整个
`KVM_RUN` 都不让出 CPU。原因很实际：VMCS 同一时刻只能 active 在一个 CPU 上，
只要线程被换下 CPU，下一次进入前就得重做 VMCLEAR/VMPTRLD/INVEPT；把它钉死
最省事。代价是 guest 跑多久，这颗 CPU 就被独占多久 —— RCU stall、watchdog
`dmesg` 告警都会照来，因为调度器根本拿不走这颗 CPU。**这是 mini-kvm 和
真实 VMM 最本质的运行模型差异**，不是"再优化一下就有"的差距。

### 没有 halt-polling

KVM 的 HLT 不会立刻回用户态：`kvm_vcpu_halt()`（`kvm_main.c:3811`）先在
`vcpu->halt_poll_ns` 纳秒内忙等 `kvm_vcpu_check_block()`，等不到才
`schedule()`。轮询窗口自适应：成功唤醒就 `grow_halt_poll_ns()`（`:3670`），
等满就 `shrink_halt_poll_ns()`（`:3689`），上限由模块参数
`halt_poll_ns`（`:79`，还有 `halt_poll_ns_grow` / `_grow_start` / `_shrink`
`:84/:89/:94`）或 per-VM 的 `KVM_CAP_HALT_POLL` 决定（`kvm_vcpu_max_halt_poll_ns()`，
`:3787-3803`）。命中与否的统计在 `update_halt_poll_stats()`（`:3765`）里，
`perf kvm stats` / `kvm_vcpu_stats` 的 `halt_successful_poll` /
`halt_attempted_poll` 就是它。

mini-kvm 的 HLT 直接把 `KVM_EXIT_HLT` 写进共享的 `kvm_run` 页并
`return`，唤醒开销由用户态承担 —— 教学 guest 的 HLT 本来就是用例终点，没有
优化价值。真要加，正确的位置是这个 `case EXIT_REASON_HLT` 分支，而且要先把
"全程 preempt_disable" 解决掉，否则忙等会让那颗 CPU 彻底失联。

### 没有 exit 快速路径

`vmx_exit_handlers_fastpath()` 那一档（`vmx/vmx.c:7264-7285`：
`MSR_WRITE` / `PREEMPTION_TIMER` / `HLT`）mini-kvm 一个都没有 —— 我们的
分发就是一个 `switch`，处理完统一回到循环头。省下的一次
`preempt_enable`/`local_irq_enable` 往返，在这个规模上不是瓶颈。

## 🔑 关键差异: mini-kvm vs 真实 KVM

| 特性 | mini-kvm | 真实 KVM |
|------|----------|---------|
| 循环层数 | 一层 `for(;;)`（`vcpu.c:132`） | `vcpu_run()` + `vcpu_enter_guest()` 两层 |
| 让出 CPU | 不让，全程 `preempt_disable` | 外层可抢占可睡眠 |
| 请求机制 | 无 | `KVM_REQ_*` 位图，在 `vcpu_enter_guest()` 开头消费 |
| 事件注入 | 单个 `pending_intr_info`（Stage 3） | 异常/中断/NMI/异常影子 完整队列 + 窗口退出 |
| HLT | 立即回用户态 | `kvm_vcpu_halt()`：polling → `schedule()` |
| 信号 | 不检查（`KVM_RUN` 期间不可中断） | `__xfer_to_guest_mode_work_pending()` → `xfer_to_guest_mode_handle_work()` |
| exit 快路径 | 无 | `vmx_exit_handlers_fastpath()`（窄口径，见上） |
| 统计 | 7 个自增计数器 + 一行 pr_debug | `kvm_vcpu_stat` 上百项 + tracepoint + `kvm:kvm_exit` |
| vCPU 数 | 1（第二个 `-EEXIST`） | 多 vCPU + APICv + posted interrupt |

## 🧪 实验验证

完整走一遍（上机前的宿主安全检查见 `../README.md` 第 4 节）：

```bash
make && make guest && make user
sudo insmod mini-kvm.ko
sudo ./test-mini-kvm
```

test-mini-kvm 的九步验收（`test-mini-kvm.c:9-19`）全部通过时的关键几行：

```
[信息] ✓ 退出原因 = KVM_EXIT_HLT (5)
[信息] 串口缓冲: Hello from Mini-KVM Guest!
[信息] ✓ guest 欢迎语 验证通过
[信息] ✓ 中断注入处理 验证通过
  全部通过: VMX 进入/退出 + EPT 按需映射 +
  IO 模拟 + 中断注入均工作正常!
```

想看到每次退出的分布，打开 dynamic debug 再跑：

```bash
echo -n 'module mini_kvm +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
sudo dmesg | grep 'RUN 结束'
```

异常路径的验证办法（不需要改代码）：把 guest 镜像里那条 `OUT` 的端口改成
别的值，串口捕获会变空、dmesg 里出现
`mini-kvm: 忽略 OUT port=0x…`；把 guest 页表的某一级故意指到 memslot 之外，
会看到 `EPT violation GPA=0x… 不在 memslot 内` 加一行 `mini_dump_vmcs()` 的
字段表 —— 这就是"未处理退出的样子"，比读代码直观。

## 📝 检查清单

- [ ] 说出一次 `KVM_RUN` 里 mini-kvm 会转多少圈、什么情况下才返回
- [ ] 解释迁移为什么必须在 `for (;;)` 之外，以及 `smp_call_function_single` 的约束（`kernel/smp.c:647-653`）
- [ ] 说明 Host CR3/CR4/GS 为什么要每次进入前刷新
- [ ] 讲清 `launched` 与 VMCLEAR/launch state 的关系（SDM 25.11.3、31.4 Table 31-1）
- [ ] 说出 KVM 的 fastpath 到底覆盖哪几种退出，为什么它不等于"不回用户态"
- [ ] 说明 mini-kvm 全程关抢占的代价，以及为什么它不是"再优化一下"能补上的

## 🎉 毕业检查

五个 Stage 都做完后，你应该能回答：**从用户态一句 `ioctl(KVM_RUN)` 到 guest
里一条 `OUT` 落到宿主的 `printk`，中间经过了几次特权级切换、几次地址空间
翻译、几次 VMCS 读写？** 如果每一跳都能指出具体代码行，Phase 8 就算过了。

## 🔗 扩展方向

按"加一行就有效果"到"要重设计"排序：

1. **MSR 退出处理** —— 运行循环加一个 `EXIT_REASON_MSR_READ/WRITE` 分支 +
   一张白名单表；开启 "use MSR bitmaps"（`asm/vmx.h`，SDM Table 25-6 bit 28）
   才能只拦想拦的 MSR。
2. **第二个串口 / 端口白名单** —— `device.c` 现在只认 0x3f8，其余一律忽略。
3. **`kvm_run` 结构化 IO 退出** —— 按 `KVM_EXIT_IO` 的 `port/size/count/
   data_offset` 约定填，把设备模型搬到用户态（这才像 QEMU/crosvm 的用法）。
4. **多 vCPU** —— EPT 根共享 + 每 vCPU 一份 VMCS；然后必须解决"全程
   preempt_disable"和 `local_irq_disable` 的粒度。
5. **VPID + 大页 EPT + halt-polling** —— 性能项，前提是先有第 4 步的调度模型。
6. **Posted Interrupts** —— 需要 vLAPIC、PI descriptor、`KVM_CAP_X86_PI` 一
   整套，属于 Phase 4 的正式内容，不适合塞进 mini-kvm。
