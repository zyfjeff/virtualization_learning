# 源码精读：phase9 独占的三个机制块

> 基于 **Linux 6.12.93**。本章只精读**别处不owns**的三块：
> PLE/directed yield 与调度联动、EPT 粒度与 PML、主时钟与 TSC offset。
> 参数默认值与权限在 [parameters.md](parameters.md)，本章不重复。
>
> **为什么不讲其它**：halt-polling 算法、VPID、APICv、Posted Interrupt 机制、
> 时钟源与 EPT 缺页流程都在更合适的章节，见文末 §4 的对照表。
> 旧版本章的对应段落已删除，删除原因逐条记录在 [corrections.md](corrections.md)。

---

## 1. PLE 与 directed yield —— 超卖场景的唯一自救机制

**为什么只有这里讲**：全仓库唯一涉及 `kvm_vcpu_on_spin()` / `yield_to()` 这条链的
文档。`../phase8-capstone/practice/README.md` 项目4 已定性确认
"单 vCPU、无超卖自旋，PLE 测不出；需要多 vCPU 超卖实验"，该实验就是本章的
`practice/bench-ple.md`。

### 1.1 入口：`handle_pause()`

PLE 不是 KVM "拦截 PAUSE 指令"。KVM **从不设置** `CPU_BASED_PAUSE_EXITING`；
`PAUSE_INSTRUCTION`(退出号 40) 只由硬件的 **Pause-Loop-Exiting** 机制产生 ——
硬件自己统计 PAUSE 间隔与累计时间，超过窗口才退出。所以 `handle_pause()` 的注释
第一句就是"我们没开 PAUSE exiting"：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:5907-5924 (Linux 6.12.93) */

/*
 * Indicate a busy-waiting vcpu in spinlock. We do not enable the PAUSE
 * exiting, so only get here on cpu with PAUSE-Loop-Exiting.
 */
static int handle_pause(struct kvm_vcpu *vcpu)
{
	if (!kvm_pause_in_guest(vcpu->kvm))
		grow_ple_window(vcpu);

	/*
	 * Intel sdm vol3 ch-25.1.3 says: The "PAUSE-loop exiting"
	 * VM-execution control is ignored if CPL > 0. OTOH, KVM
	 * never set PAUSE_EXITING and just set PLE if supported,
	 * so the vcpu must be CPL=0 if it gets a PAUSE exit.
	 */
	kvm_vcpu_on_spin(vcpu, true);
	return kvm_skip_emulated_instruction(vcpu);
}
```

**注释里那句 SDM 25.1.3 是本章实验设计的地基**：
*"PAUSE-loop exiting VM-execution control is ignored if CPL > 0"*。
即**用户态自旋永远不会产生 PLE 退出**。推论直接决定 `bench-ple.md` 的负载选型：
`stress-ng --mutex/--futex/--switch` 这类用户态争抢**不可能**触发 PLE，
只能当阴性对照；能触发的只有 guest **内核态**（CPL0）自旋。

### 1.2 per-vCPU 窗口只增不减，回落点只有一个

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:1417-1430 */
static void grow_ple_window(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	unsigned int old = vmx->ple_window;

	vmx->ple_window = __grow_ple_window(old, ple_window,
					    ple_window_grow,
					    ple_window_max);

	if (vmx->ple_window != old) {
		vmx->ple_window_dirty = true;
		trace_kvm_ple_window_update(vcpu->vcpu_id,
					    vmx->ple_window, old);
	}
}
```

`shrink_ple_window()`（`:1433-1446`）结构相同，差别只在第 4 个实参：
grow 传的是 `ple_window_max`，shrink 传的是**全局 `ple_window`** —— 即
per-vCPU 窗口的**下限就是全局值**，不可能低于它。

放大/缩小的算术在 `arch/x86/kvm/x86.h:80-107`：

```c
/* 来源: arch/x86/kvm/x86.h:80-94 */
static inline unsigned int __grow_ple_window(unsigned int val,
		unsigned int base, unsigned int modifier, unsigned int max)
{
	u64 ret = val;

	if (modifier < 1)
		return base;

	if (modifier < base)
		ret *= modifier;
	else
		ret += modifier;

	return min(ret, (u64)max);
}
```

`__shrink_ple_window()`（`:96-107`）同构。**两处 `modifier < 1` 早退值得注意**：
`ple_window_grow` 或 `ple_window_shrink` 设成 0 不是"不增长/不缩小"，而是
**直接把窗口重置为 base**。默认 `ple_window_shrink = 0`
（[parameters.md](parameters.md) §2）就是这个含义 —— 一旦被抢占，窗口**直接回到全局
4096**，不是除以 2。

**唯一的回落点**在 `vmx_vcpu_load()`：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:1519-1523 */
void vmx_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	if (vcpu->scheduled_out && !kvm_pause_in_guest(vcpu->kvm))
		shrink_ple_window(vcpu);
```

条件 `vcpu->scheduled_out` 由 `kvm_sched_out()` 置位（见 §1.5）。
旧版本文写"PAUSE 间隔 > `ple_gap` 时 `shrink_ple_window()` 缩小窗口"——
**源码里没有这回事**，`ple_gap` 从不参与 shrink 判断（见 `corrections.md`）。

> **实验后果**：窗口是 per-vCPU 的**进程内状态**，同一 VM 的第二轮会继承第一轮的
> 窗口。要么每档重启 VM，要么在一轮里把采样一次做完。

### 1.3 关掉 PLE 的两条路

模块级：`ple_gap = 0` 在 **VM 创建时**落到 `pause_in_guest`，

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:7638-7641 */
int vmx_vm_init(struct kvm *kvm)
{
	if (!ple_gap)
		kvm->arch.pause_in_guest = true;
```

再在每次构造执行控制时清掉硬件位：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:4627-4628 */
	if (kvm_pause_in_guest(vmx->vcpu.kvm))
		exec_control &= ~SECONDARY_EXEC_PAUSE_LOOP_EXITING;
```

per-VM 级（做对照实验更实用）：`KVM_CAP_X86_DISABLE_EXITS` +
`KVM_X86_DISABLE_EXITS_PAUSE`，同样落到 `pause_in_guest`，但**不用重载模块**。
两条路的实验形态完全不同：前者改一次影响全宿主，后者每个 VM 独立。

### 1.4 `kvm_vcpu_on_spin()`：它到底在挑谁

PLE 退出后 KVM 的"让出 CPU"不是 `schedule()`，而是**定向让出（directed yield）**：
猜一个"很可能持有我等的那把锁"的 vCPU，直接跟它换。真实实现：

```c
/* 来源: virt/kvm/kvm_main.c:4037-4098 (Linux 6.12.93) */
void kvm_vcpu_on_spin(struct kvm_vcpu *me, bool yield_to_kernel_mode)
{
	...
	last_boosted_vcpu = READ_ONCE(kvm->last_boosted_vcpu);
	kvm_vcpu_set_in_spin_loop(me, true);
	/*
	 * We boost the priority of a VCPU that is runnable but not
	 * currently running, because it got preempted by something
	 * else and called schedule in __vcpu_run.  Hopefully that
	 * VCPU is holding the lock that we need and will release it.
	 * We approximate round-robin by starting at the last boosted VCPU.
	 */
	for (pass = 0; pass < 2 && !yielded && try; pass++) {
		kvm_for_each_vcpu(i, vcpu, kvm) {
			if (!pass && i <= last_boosted_vcpu) { ... continue; }
			else if (pass && i > last_boosted_vcpu) break;
			if (!READ_ONCE(vcpu->ready)) continue;
			if (vcpu == me) continue;
			if (kvm_vcpu_is_blocking(vcpu) && !vcpu_dy_runnable(vcpu))
				continue;
			/* 有 pending 中断的 vCPU 视作 in-kernel */
			if (READ_ONCE(vcpu->preempted) && yield_to_kernel_mode &&
			    !kvm_arch_dy_has_pending_interrupt(vcpu) &&
			    !kvm_arch_vcpu_preempted_in_kernel(vcpu))
				continue;
			if (!kvm_vcpu_eligible_for_directed_yield(vcpu))
				continue;

			yielded = kvm_vcpu_yield_to(vcpu);
			if (yielded > 0) { WRITE_ONCE(kvm->last_boosted_vcpu, i); break; }
			else if (yielded < 0) { if (!--try) break; }
		}
	}
	kvm_vcpu_set_in_spin_loop(me, false);
	/* Ensure vcpu is not eligible during next spinloop */
	kvm_vcpu_set_dy_eligible(me, false);
}
```

要点（每一处都对应实验里一个可观测行为）：

| 机制 | 位置 | 实验含义 |
|---|---|---|
| 两轮 pass，从 `last_boosted_vcpu` 起近似 round-robin | `:4056-4058` | 让出目标是**轮转**的，不是"找真正的持锁者"—— KVM 不知道锁在哪 |
| `try = 3`，`yielded < 0` 才扣次数 | `:4044`、`:4087-4090` | 只有 `-ESRCH` 才消耗尝试次数；返回 0 的失败**不扣**，会一路试完 |
| `yield_to_kernel_mode=true` + `preempted` 过滤 | `:4076-4079` | 优先让给"退出时在内核态"的 vCPU。`handle_pause` 传的就是 `true` |
| 结尾把 `me` 的 dy_eligible 清零 | `:4096` | 同一个自旋循环内**不会**被别的 vCPU 反向挑中 |

### 1.5 让出最终交给调度器 —— 四条早退决定实验成败

```c
/* 来源: virt/kvm/kvm_main.c:3938-3954 */
int kvm_vcpu_yield_to(struct kvm_vcpu *target)
{
	...
	if (!task)
		return ret;
	ret = yield_to(task, 1);
	put_task_struct(task);
	return ret;
}
```

`kernel/sched/syscalls.c:1468` 的 `yield_to()` 里有**四道会静默失败的关卡**，
它们是 `bench-ple.md` 判据的直接来源：

```c
/* 来源: kernel/sched/syscalls.c:1480-1498 (Linux 6.12.93) */
		/*
		 * If we're the only runnable task on the rq and target rq also
		 * has only one task, there's absolutely no point in yielding.
		 */
		if (rq->nr_running == 1 && p_rq->nr_running == 1)
			return -ESRCH;                     /* :1483-1484 */
		...
		if (!curr->sched_class->yield_to_task)     /* :1489 */
			return 0;
		if (curr->sched_class != p->sched_class)   /* :1492 */
			return 0;
		if (task_on_cpu(p_rq, p) || !task_is_running(p))  /* :1495 */
			return 0;
		yielded = curr->sched_class->yield_to_task(rq, p);
```

| 早退 | 条件 | 什么时候会咬到你的实验 |
|---|---|---|
| `-ESRCH` | 两个 rq 各只有 1 个可运行任务 | **1:1 绑核不超卖时必然命中** → 这正是"PLE 在无超卖环境下无事可做"的源码解释，也是阴性对照该看的返回值 |
| `!curr->sched_class->yield_to_task` | 当前任务所属调度类**没实现** `yield_to_task` | `.yield_to_task` 只有 CFS（`kernel/sched/fair.c:13722` → `yield_to_task_fair`）与 sched-ext（`kernel/sched/ext.c:4158`）提供。**RT 类没有** → 用 `chrt -f` 把 vCPU 线程提成 RT 后，directed yield **静默全废**，而 PLE 退出照样发生，看起来就像"PLE 无效" |
| `curr->sched_class != p->sched_class` | 两边不同调度类 | 只给部分 vCPU 线程设了实时优先级时命中 |
| `task_on_cpu \|\| !task_is_running` | 目标正在跑 / 已不在运行 | 窗口设得**过小**时 `attempted` 暴涨而 `successful` 上不去，主因就在这条 |

### 1.6 三个标志位与抢占通知：谁给 PLE 喂信息

`vcpu->scheduled_out` / `preempted` / `ready` 都是 preempt notifier 维护的，
真名是 `kvm_sched_in()` / `kvm_sched_out()`（注册于 `virt/kvm/kvm_main.c:6515-6516`）：

```c
/* 来源: virt/kvm/kvm_main.c:6388-6400 */
static void kvm_sched_out(struct preempt_notifier *pn,
			  struct task_struct *next)
{
	struct kvm_vcpu *vcpu = preempt_notifier_to_vcpu(pn);

	WRITE_ONCE(vcpu->scheduled_out, true);

	if (task_is_runnable(current) && vcpu->wants_to_run) {
		WRITE_ONCE(vcpu->preempted, true);
		WRITE_ONCE(vcpu->ready, true);
	}
	kvm_arch_vcpu_put(vcpu);
	...
```

注意 `preempted`/`ready` 的门槛是 **`task_is_runnable(current)`** ——
被抢占但**自己已阻塞**的 vCPU 不进 `ready` 集合，因而永远不会成为 yield 目标。
`scheduled_out` 则无条件置位，它同时是 §1.2 里 PLE 窗口回落的开关。

`kvm_arch_vcpu_put()` → `vmx_vcpu_pi_put()`（`arch/x86/kvm/vmx/posted_intr.c:196`），
`vmx_vcpu_load()` → `vmx_vcpu_pi_load()`（`:53`，调用点 `arch/x86/kvm/vmx/vmx.c:1527`）。
这两者把 vCPU 的 Posted 描述符 `SN`（suppressed notification）与 `NDST` 目标 CPU
更新好 —— **这是 PLE/调度与 Posted Interrupt 的交界**：一次 directed yield 会顺带
产生 PI 描述符改写和一次通知向量 IPI。PI 的机制本身见
`../phase4-interrupts/posted-interrupts.md`；本章只管它被调度牵动的部分。

（旧版本文在此处写的 `sched_out_fn()` / `sched_in_fn()` **不是真实函数名**，
见 `corrections.md`。）

### 1.7 观测出口

| 出口 | 位置 | 回答什么 |
|---|---|---|
| `PAUSE_INSTRUCTION` 退出计数 | 退出号 **40**，`arch/x86/include/uapi/asm/vmx.h:67` | PLE 到底有没有触发 |
| `kvm:kvm_ple_window_update` | `arch/x86/kvm/trace.h:978`；触发点 `vmx.c:1428`（grow）/`:1444`（shrink） | 窗口轨迹；参数 `vcpu_id, new, old`；**只在值真的变了时才打** |
| `kvm_vcpu_on_spin()` 函数计数 | `virt/kvm/kvm_main.c:4037`；本机 6.8.0-51 `available_filter_functions` 实测**可跟踪** | PLE 处理程序进了几次 |
| `kvm_vcpu_yield_to()` 函数计数 | `virt/kvm/kvm_main.c:3938`，`yield_to(task, 1)` 在 `:3950`；实测**可跟踪** | 真正发起了几次定向让出（含返回值，>0 为成功） |
| guest 侧临界区完成吞吐 | 自建负载（`practice/ple-load/`） | 最终收益 |

### 1.8 一个会把人带偏的统计量：`directed_yield_*` 不属于 PLE 路径

debugfs 里名字最贴切的两个统计 **`directed_yield_attempted` /
`directed_yield_successful`**（`arch/x86/include/asm/kvm_host.h:1598-1599`，
描述符 `arch/x86/kvm/x86.c:288-289`）**PLE 一次都不会递增**。

它们唯一的递增点在 `kvm_sched_yield()` 里（`arch/x86/kvm/x86.c:10026`，
`:10031` attempted、`:10057` successful），而调用它的是**两条 guest hypercall**：

- `KVM_HC_KICK_CPU`（`x86.c:10102-10108`，门控 `KVM_FEATURE_PV_UNHALT`）——
  **PV spinlock** 的等待方踢醒持锁 vCPU；
- `KVM_HC_SCHED_YIELD`（`x86.c:10120-10126`，门控 `KVM_FEATURE_PV_SCHED_YIELD`）。

也就是说这两个统计衡量的是 guest **主动告诉宿主"请让出"**的那条路，
而 PLE 是宿主**单方面从 PAUSE 流里抢回控制权**的那条路：
`kvm_vcpu_on_spin()`（`kvm_main.c:4037-4100`）全程只调 `kvm_vcpu_yield_to()`
并在成功时写 `kvm->last_boosted_vcpu`（`:4083-4085`），一次统计都不加。

**为什么值得单独写一节**：这两个名字太像了，拿它当 PLE 的判据是极自然的误读；
而在**没开 PV spinlock** 的 guest 上（下一章会说明本仓的实验内核正是这种状态，
PLE 照样能触发），`directed_yield_*` 恒为 0 —— 用它做验收会让一个**成功**的
PLE 实验看起来彻底失败。判据必须换成上表里的四个出口。

**判据的与关系**：`PAUSE_INSTRUCTION` 计数随争抢强度上升 **且**
`kvm_vcpu_on_spin`/`kvm_vcpu_yield_to` 有命中 **且** 只在超卖组吞吐改善
—— 三者齐了才能说"PLE 在本场景有效"。只满足前两条说明机制跑通了但没收益；
只满足第三条说明收益来自别处。

---

## 2. EPT 粒度、PML 与脏页日志

**为什么只有这里讲**：`../phase2-mem-virt/` 讲 EPT/TDP MMU **机制**（怎么建表、
怎么缺页处理），本章讲的是"**粒度和脏页跟踪各自值多少钱、代价落在哪**"——
而这两者在源码里是**纠缠**的，必须放在一起才说得清。

### 2.1 大页能不能建：真实闸门与 A/D 位无关

决定映射级别的调用链：

```
kvm_tdp_page_fault → kvm_mmu_hugepage_adjust()   mmu.c:3172
                   → __kvm_mmu_max_mapping_level()  mmu.c:3138
```

`kvm_mmu_hugepage_adjust()` 的早退顺序就是**门条件清单**
（`arch/x86/kvm/mmu/mmu.c:3172-3196`）：

```c
/* 来源: arch/x86/kvm/mmu/mmu.c:3177-3186 */
	fault->huge_page_disallowed = fault->exec && fault->nx_huge_page_workaround_enabled;

	if (unlikely(fault->max_level == PG_LEVEL_4K))
		return;

	if (is_error_noslot_pfn(fault->pfn))
		return;

	if (kvm_slot_dirty_track_enabled(slot))
		return;
```

| 条件 | 后果 |
|---|---|
| `fault->max_level == PG_LEVEL_4K` | 上游已经算出不允许，直接 4K |
| pfn 无效 | 4K |
| **`kvm_slot_dirty_track_enabled(slot)`** | **开脏页跟踪 → 新建映射一律 4K**（见 §2.2） |
| `fault->exec && nx_huge_page_workaround_enabled` | `huge_page_disallowed` 置位 → iTLB multihit 缓解，可执行大页被砍 |

真正决定级别上限的 `__kvm_mmu_max_mapping_level()`（`mmu.c:3138`）走
`disallow_lpage` 计数（`update_gfn_disallow_lpage_count`，`mmu.c:781-792`）
与宿主侧 `host_pfn_mapping_level`，**不涉及 `eptad`**。
旧版本文写"EPT A/D 位支持"是启用条件之一，无源码依据（`corrections.md`）。

### 2.2 脏页跟踪与大页收益天然对冲

`:3185-3186` 那两行的含义是：**只要 memslot 开着脏页跟踪，就不新建大页**；
已有大页还要被拆（§2.3）。helper 在 `include/linux/kvm_host.h:628`。

所以"迁移期间性能掉了"这句话里**至少混着三项**：

1. 新建映射退到 4K → 缺页次数上升（最多 512 倍映射数差异）；
2. 已有大页被 split → 一次性拆表开销 + 之后 EPT TLB 压力；
3. 脏页记录本身（写保护或 PML）→ 见 §2.4。

**测量必须拆开归因**，否则结论张冠李戴。拆法见 `practice/bench-huge-dirty.md`
（四格矩阵）与 `measurement.md` §6(a)。

### 2.3 `eager_page_split`：把拆表代价提前付

```c
/* 来源: arch/x86/kvm/mmu/mmu.c:1336-1341 */
	if (kvm_dirty_log_manual_protect_and_init_set(kvm)) {
		gfn_t start = slot->base_gfn + gfn_offset + __ffs(mask);
		gfn_t end = slot->base_gfn + gfn_offset + __fls(mask);

		if (READ_ONCE(eager_page_split))
			kvm_mmu_try_split_huge_pages(kvm, slot, start, end + 1, PG_LEVEL_4K);
```

上方注释（`mmu.c:1328-1330`）写明了动机：
*"immediately try to split huge pages, e.g. so that vCPUs don't get saddled with
the cost of splitting"*。即默认 `true`（`arch/x86/kvm/x86.c:193-194`）是
**把拆表成本从 vCPU 运行期挪到脏日志 bitmap 同步时**，代价是同步端变慢、收益是
guest 侧长尾变少。拆表实现在
`kvm_tdp_mmu_try_split_huge_pages()`（`arch/x86/kvm/mmu/tdp_mmu.c:1479`）。

这是"迁移期 guest 卡顿"与"迁移带宽/收敛"之间一个**可运行时调**的权衡旋钮
（0644），适合做单独一档扫描。

### 2.4 PML 的真实开销模型 —— 旧版讲反了

PML 是硬件行为：开启后，guest 对可写页的**首次写**由硬件把该页 GPA 追加进
一个 512 条目的 VMCS 缓冲区（`PML_ENTITY_NUM`，`arch/x86/kvm/vmx/vmx.h:336`），
缓冲区耗尽才产生 `PML_FULL`（退出号 **62**，`arch/x86/include/uapi/asm/vmx.h:88`）
退出。**正常写入不退出**。

关键修正：`handle_pml_full()` 其实**什么都不做**：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:5962-5985 */
static int handle_pml_full(struct kvm_vcpu *vcpu)
{
	unsigned long exit_qualification;

	trace_kvm_pml_full(vcpu->vcpu_id);

	exit_qualification = vmx_get_exit_qual(vcpu);

	/* PML buffer FULL happened while executing iret from NMI ... */
	if (!(to_vmx(vcpu)->idt_vectoring_info & VECTORING_INFO_VALID_MASK) &&
			enable_vnmi &&
			(exit_qualification & INTR_INFO_UNBLOCK_NMI))
		vmcs_set_bits(GUEST_INTERRUPTIBILITY_INFO,
				GUEST_INTR_STATE_NMI);

	/*
	 * PML buffer already flushed at beginning of VMEXIT. Nothing to do
	 * here.., and there's no userspace involvement needed for PML.
	 */
	return 1;
}
```

真正的消费在**每一次 VM-Exit 的最前面**，与退出原因无关：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:6451-6452 */
	if (enable_pml && !is_guest_mode(vcpu))
		vmx_flush_pml_buffer(vcpu);
```

`vmx_flush_pml_buffer()`（`:6182-6208`）读 `GUEST_PML_INDEX`，空则直接返回
（`pml_idx == PML_ENTITY_NUM - 1`），否则逐条 `kvm_vcpu_mark_page_dirty()` 并复位索引。

由此得到 PML 的正确代价模型，三条都要在实验里对上：

| 项 | 结论 | 依据 |
|---|---|---|
| 稳态写入 | 硬件记账，**不产生退出** | `PML_ENTITY_NUM=512` + 上面两条码 |
| `PML_FULL` 退出频率 | ≈ 脏页速率 / 512 | 缓冲区大小 |
| **每次退出的附加成本** | 只要 PML 开着，**所有** VM-Exit 都多走一遍 flush（可能只是读一个 VMCS 字段就早退，也可能遍历上百条） | `vmx.c:6451-6452` 无条件调用 |
| 嵌套 | L2 期间硬件从不启用 PML | `!is_guest_mode(vcpu)` 守卫 + `:6468` 注释"PML is never enabled when running L2" |

旧版本文写"`handle_pml_full()` → KVM 消费 PML buffer → 清空 → 继续"，
把 flush 的位置安错了 —— 这会让"退出次数"与"PML 成本"的对账对不上。

**观测出口**：`kvm:kvm_pml_full`（`trace.h:963`，
`TP_PROTO(unsigned int vcpu_id)` `:964` —— 只有一个参数，旧版写的
"vcpu_id, full_count"不存在）；`PML_FULL` 退出计数；
脏日志 bitmap 同步耗时（用户态侧）。

### 2.5 注意 `eptad=0` 的真正代价

关掉 EPT A/D 位不等于"每次写都退出"。KVM 用**软件访问/脏位模拟**，
代价体现为写保护导致的额外退出与 TLB flush，具体流程属
`../phase2-mem-virt/` 的范围。本章只强调：`eptad` 是 **0444 只读**
（`arch/x86/kvm/vmx/vmx.c:106`），做这个 A/B 必须重载 `kvm_intel`，
每个数据点一次开机。

---

## 3. 主时钟、TSC offset 与提前量

**为什么只有这里讲**：`../phase7-timer-virt/` 讲时钟**虚拟化机制**
（clocksource vs clockevent、kvmclock/pvclock、TSC-deadline 语义），
本章讲的是**主时钟什么时候才允许开、TSC offset 在 vCPU 迁移时怎么变**——
一个跨"调度 + 时钟"的判据问题。

### 3.1 主时钟的启用判据是源码里那几个条件

`kvm_track_tsc_matching()`（`arch/x86/kvm/x86.c:2515`）里的条件：

```c
/* 来源: arch/x86/kvm/x86.c:2521-2528 */
	/*
	 * To use the masterclock, the host clocksource must be based on TSC
	 * and all vCPUs must have matching TSCs.  Note, the count for matching
	 * vCPUs doesn't include the reference vCPU, hence "+1".
	 */
	bool use_master_clock = (ka->nr_vcpus_matched_tsc + 1 ==
				 atomic_read(&vcpu->kvm->online_vcpus)) &&
				gtod_is_based_on_tsc(gtod->clock.vclock_mode);
```

真正的赋值在 `pvclock_update_vm_gtod_copy()`（`arch/x86/kvm/x86.c:3015`）里
（`arch/x86/kvm/x86.c:3025-3036`）：

```c
/* 来源: arch/x86/kvm/x86.c:3025-3036 */
	/*
	 * If the host uses TSC clock, then passthrough TSC as stable
	 * to the guest.
	 */
	host_tsc_clocksource = kvm_get_time_and_clockread(
					&ka->master_kernel_ns,
					&ka->master_cycle_now);

	ka->use_master_clock = host_tsc_clocksource && vcpus_matched
				&& !ka->backwards_tsc_observed
				&& !ka->boot_vcpu_runs_old_kvmclock;
```

四个条件缺一不可：宿主 clocksource 基于 TSC、**所有** vCPU 的 TSC 页匹配、
没观察到 TSC 倒退、没有 vCPU 跑旧版 kvmclock。

> 旧版本文把启用条件写成"Host TSC 是 invariant（`CPUID.80000007:EDX[8]`）"——
> 那是**宿主的硬件能力**，既非充分也非必要（宿主 invariant 但 clocksource 选了
> HPET，`host_tsc_clocksource` 就是 0，主时钟不开）。判据一律以上面四个为准，
> 验证实验见 [practice/bench-clock-master.md](practice/bench-clock-master.md)。

#### 3.1.1 失效是单向的：换走会立刻关，换回不会自己开

**关的那条边**由 timekeeping 的 pvclock gtod notifier 驱动，链路每一跳都是异步的：

```
echo hpet > /sys/devices/system/clocksource/clocksource0/current_clocksource
  └─ current_clocksource_store()   kernel/time/clocksource.c:1401
       └─ clocksource_select() → __clocksource_select()   kernel/time/clocksource.c:1069
            └─ timekeeping_notify()             kernel/time/timekeeping.c:1531
                 └─ stop_machine(change_clocksource)      kernel/time/timekeeping.c:1537
                      └─ （回调本体 change_clocksource()  kernel/time/timekeeping.c:1479）
                           └─ timekeeping_update(tk, …| TK_CLOCK_WAS_SET)  kernel/time/timekeeping.c:1509
                                └─ update_pvclock_gtod()   kernel/time/timekeeping.c:666
                                     └─ （notifier 链  kernel/time/timekeeping.c:568-570）
                                          └─ KVM pvclock_gtod_notify()  arch/x86/kvm/x86.c:9674
                                               └─ irq_work_queue()      arch/x86/kvm/x86.c:9689
                                                    └─ pvclock_irq_work_fn()  arch/x86/kvm/x86.c:9664
                                                         └─ queue_work(system_long_wq) → pvclock_gtod_update_fn()  arch/x86/kvm/x86.c:9643
                                                              遍历 vm_list 全部 vCPU 下
                                                              KVM_REQ_MASTERCLOCK_UPDATE，并清全局标志
```

判据在 `x86.c:9687-9689`，它带一个**单向**条件：

```c
/* 来源: arch/x86/kvm/x86.c:9687-9689 */
	if (!gtod_is_based_on_tsc(gtod->clock.vclock_mode) &&
	    atomic_read(&kvm_guest_has_master_clock) != 0)
		irq_work_queue(&pvclock_irq_work);
```

- **只在"离开 TSC 基"这一侧动作**；换回 `tsc` 时 `gtod_is_based_on_tsc()` 为真，
  整条链根本不启动。
- 它同时把全局 `kvm_guest_has_master_clock`（`x86.c:2414`，置位 `:3039`、
  清零 `:9653`）清 0，所以重复的 notifier 也不会再吵醒 VM。
- 请求只是**置位**，真正重算发生在每个 vCPU 下一次 `vcpu_enter_guest()`
  消费 `KVM_REQ_MASTERCLOCK_UPDATE` 时（`x86.c:10809-10810`）。
  一个睡着不动的 vCPU 不会消费它 —— 采样期间 guest 必须有负载。

**开的那条边没有任何自动触发**。重算 `ka->use_master_clock` 的入口只有六个，
可达路径全列在这里：

| 重算点 | 位置 | 谁能碰它 |
|---|---|---|
| `kvm_arch_init_vm()` | `arch/x86/kvm/x86.c:12803`，调用 `:12844` | **新建 VM**（只影响新 VM） |
| `kvm_vm_ioctl_set_clock()` | `:7006`，调用 `:7024` | 用户态 `KVM_SET_CLOCK`（QEMU 只在迁移恢复路径下发） |
| `__kvm_synchronize_tsc()` | `:2670`→`:2714` | vCPU 创建后的 `kvm_arch_vcpu_postcreate()`（`:12463`→`:12470`）、guest 写 `MSR_IA32_TSC`（`kvm_set_msr_common()` `:3938-3940`）、`kvm_arch_tsc_set_attr()`（`:5757`→`:5784`） |
| `kvm_write_system_time()` | `:2354-2361` | **仅当** `boot_vcpu_runs_old_kvmclock` 真翻转（guest 在新旧 kvmclock MSR 之间换） |
| `kvm_hyperv_tsc_notifier()` | `:9483`→`:9504` | Hyper-V TSC page 变化，需要 Hyper-V guest |
| `pvclock_gtod_update_fn()` | `:9643` | 见上，**只有 TSC→非 TSC 一条边** |

最后一行之外还要排除一个看着像的钩子：`KVM_REQ_MASTERCLOCK_UPDATE` 也出现在
`kvm_arch_enable_virtualization_cpu()`（`:12682`）里，但它在
`if (backwards_tsc)` 分支内（`:12754-12762`），是 S4 恢复后补 TSC 倒退用的，
**CPU 热插拔 / 上下线不会**让主时钟重算。

所以本章的真实结论是一句不太好接受的话：**宿主 `tsc` 换出去再换回来，
已经在跑的 VM 的快路径不会恢复；而同期新建的 VM 却是开着的**。
同一台宿主上两台 VM 的时钟路径就此分叉，直到老 VM 自己重启或被上面某条路径碰一下。

#### 3.1.2 主时钟关着时，每次 vCPU 迁移多付一笔 pvclock 写账

`kvm_arch_vcpu_load()` 里的迁移分支（`x86.c:5014`）有一句直白的因果：

```c
/* 来源: arch/x86/kvm/x86.c:5030-5035 */
		/*
		 * On a host with synchronized TSC, there is no need to update
		 * kvmclock on vcpu->cpu migration
		 */
		if (!vcpu->kvm->arch.use_master_clock || vcpu->cpu == -1)
			kvm_make_request(KVM_REQ_GLOBAL_CLOCK_UPDATE, vcpu);
```

后面那半句 `|| vcpu->cpu == -1` 是"首次 load 一定要写一次"，与主时钟无关；
**前半句才是"迁移 × 主时钟关"的乘积项**：

```
KVM_REQ_GLOBAL_CLOCK_UPDATE 被消费（x86.c:10811-10812）
  └─ kvm_gen_kvmclock_update()              x86.c:3434
       ├─ 本 vCPU 置 KVM_REQ_CLOCK_UPDATE   :3438
       └─ schedule_delayed_work(kvmclock_update_work, KVMCLOCK_UPDATE_DELAY)  :3439
            KVMCLOCK_UPDATE_DELAY = msecs_to_jiffies(100)   :3417
            └─ kvmclock_update_fn()         :3419
                 对**全部** vCPU 置 KVM_REQ_CLOCK_UPDATE + kvm_vcpu_kick()  :3428-3430
```

代价不在那 64 字节的 pvclock 页写（`kvm_setup_guest_pvclock()` `:3161`，
`:3200` 的 `memcpy` + `:3209` 的 `kvm_gpc_mark_dirty_in_slot`），而在
**kick**：`kvm_vcpu_kick()` 走 `kvm_make_all_cpus_request()`
（`virt/kvm/kvm_main.c:311`），会读对方 vCPU 的 `mode` 并对正在 guest-mode
的 vCPU 发 IPI / 触发重调度。也就是说主时钟关掉之后，一次迁移的最坏后果是
**整台 VM 的 vCPU 被踢一遍**。源码自己就写了要限流：

```c
/* 来源: arch/x86/kvm/x86.c:3404-3414（注释） */
 * So in those cases, request a kvmclock update for all vcpus.
 * We need to rate-limit these requests though, as they can
 * considerably slow guests that have a large number of vcpus.
```

限流粒度是**每 VM 一个 `delayed_work`**，`schedule_delayed_work()` 对已挂起的
同一 work 不会重复排队 → 上界是每 VM 每 100 ms 一次全量刷新（约 10 次/秒），
与迁移次数无关。**这条上界就是 E4 的 A5 臂的判据来源**：观测出口
`kvm:kvm_pvclock_update`（`arch/x86/kvm/trace.h:999`，发出点 `x86.c:3212`）
的次数应当**饱和在 `10 × vCPU 数 / 秒` 附近**，而不是随注入次数线性增长；
线性增长说明限流没生效，饱和在 0 说明主时钟其实没关（§3.1.1）。

### 3.2 TSC offset 在 vCPU 迁移时的真实行为

`Guest TSC = Host TSC × multiplier + offset`（支持 TSC scaling 时）或
`+ offset`（不支持时）。offset 的调整入口：

| 函数 | 位置 | 触发场景 |
|---|---|---|
| `adjust_tsc_offset_guest()` | `arch/x86/kvm/x86.c:2787` | guest 侧读写 TSC、pvclock update 等 |
| `adjust_tsc_offset_host()` | `arch/x86/kvm/x86.c:2794`（内部调 `:2800` 的 guest 版） | **宿主 TSC 基准变化** |

`adjust_tsc_offset_host()` 的实际调用点在 `kvm_arch_vcpu_load()`
（`arch/x86/kvm/x86.c:4982`）里的 `:5007-5011`，用于**外部检测到的 TSC 调整**
（源码注释：*"Apply any externally detected TSC adjustments (due to suspend)"*）：

```c
/* 来源: arch/x86/kvm/x86.c:5007-5011 */
	/* Apply any externally detected TSC adjustments (due to suspend) */
	if (unlikely(vcpu->arch.tsc_offset_adjustment)) {
		adjust_tsc_offset_host(vcpu, vcpu->arch.tsc_offset_adjustment);
		vcpu->arch.tsc_offset_adjustment = 0;
		kvm_make_request(KVM_REQ_CLOCK_UPDATE, vcpu);
	}
```

**这条链在正常迁移时是空转的** —— `tsc_offset_adjustment` 只在 suspend/resume
之类的场景被置。也就是说：**在 TSC 一致的宿主上，vCPU 从 pCPU-A 迁到 pCPU-B
不需要改 offset**（这正是 x86 上 TSC 调整"本应罕见"的设计意图，也是主时钟
`vcpus_matched` 条件在守的东西）。旧版本章 §4.3 把"迁移时更新 TSC_OFFSET"写成
常规步骤，与源码不符（`corrections.md`）。

真要做迁移实验，正确预期是：**迁移后 offset 不变**；若变了，说明踩到了
`tsc_offset_adjustment` 或 scaling ratio 路径，那是**异常信号**而不是正常代价。

### 3.3 vLAPIC 定时器提前量：自适应，且不能手动设

`lapic_timer` 的到期是**宿主 hrtimer**，不是硬件比较 —— 所以
"TSC-deadline 到期零 VM-Exit"是错的（`../phase7-timer-virt/practice/` 的
Experiment 3 实测到的正是那次退出）。为抵消"宿主定时器到期 → 注入回 guest"
的滞后，KVM 提前 `timer_advance_ns` 触发，并按实测误差自动调节：

```c
/* 来源: arch/x86/kvm/lapic.c:1840-1851 */
static inline void adjust_lapic_timer_advance(struct kvm_vcpu *vcpu,
					      s64 advance_expire_delta)
{
	struct kvm_lapic *apic = vcpu->arch.apic;
	u32 timer_advance_ns = apic->lapic_timer.timer_advance_ns;
	u64 ns;

	/* Do not adjust for tiny fluctuations or large random spikes. */
	if (abs(advance_expire_delta) > LAPIC_TIMER_ADVANCE_ADJUST_MAX ||
	    abs(advance_expire_delta) < LAPIC_TIMER_ADVANCE_ADJUST_MIN)
		return;
```

两个死区常量就是它的调节纪律：**太小的抖动不理会**（否则会追逐噪声），
**太大的尖峰不理会**（否则一次偶发就把提前量拉飞）。

6.12.93 的暴露形态（**旧版文档这里是错的，且与 phase7 直接冲突**）：

| 项 | 形态 |
|---|---|
| 开关 | 模块参数 `lapic_timer_advance`（`bool`，`arch/x86/kvm/lapic.c:70-71`），**0444 只读** |
| 当前提前量 | per-vCPU **debugfs 只读文件** `lapic_timer_advance_ns`（`arch/x86/kvm/debugfs.c:67`） |
| 手动设固定值 | **做不到**，只能观测自适应结果 |

宿主若跑 6.8，形态正好相反（有 `lapic_timer_advance_ns` 参数、无 bool）——
`measurement.md` §5.2 的版本自检就是为这类坑准备的。

### 3.4 观测出口

| 出口 | 位置 |
|---|---|
| `kvm:kvm_update_master_clock` | `arch/x86/kvm/trace.h:906` |
| `kvm:kvm_track_tsc` | `arch/x86/kvm/trace.h:928` |
| `kvm:kvm_write_tsc_offset` | `arch/x86/kvm/trace.h:879` |
| `kvm:kvm_pvclock_update` | `arch/x86/kvm/trace.h:999`（发出点 `arch/x86/kvm/x86.c:3212`）—— §3.1.2 那笔"迁移 × 主时钟关"的唯一出口 |
| debugfs `vcpu*/tsc-offset`、`vcpu*/lapic_timer_advance_ns` | `arch/x86/kvm/debugfs.c:60-67` |

命令语法与排查顺序见 `../phase10-debugging/annotations.md`；
本章只给"哪个观测点对应哪条判据"。

---

## 4. 已从本章移出的内容 → 去处对照

| 旧版章节 | 现在归 | 原因 |
|---|---|---|
| §1 halt-polling 自适应算法 | `../phase0-kvm-framework/annotations.md` §9（`:824-950`，`kvm_vcpu_halt()` 完整走读）；调参实测 `../phase8-capstone/practice/README.md` M3；参数表 [parameters.md](parameters.md) §1 | 机制归框架章，实测归毕业章，本章不该有第三份 |
| §2.1 VPID | `../phase1-vtx-basics/`（VMCS 字段权威） | 旧版三个控制位宏名不实 |
| §2.2 APICv | `../phase4-interrupts/` | 旧版宏名与"~500 ns/次"均无据 |
| §2.3 Posted Interrupts | `../phase4-interrupts/posted-interrupts.md`；零退出结论有规范支撑（SDM 30.6 / VT-d 5.2.5），但收益倍数无实测 → [index.md](index.md) §3 | 机制在 phase4 讲得更深 |
| §3.1 4 级/3 级/2 级 EPT 遍历 | `../phase2-mem-virt/` | 机制章 |
| §5.1 kvmclock vs TSC-deadline | `../phase7-timer-virt/README.md` | 时钟章 |
| §6 性能调试命令集 | `../phase10-debugging/` | 命令归调试章，见 [measurement.md](measurement.md) §0 的分工 |

**本章保留判据**：某机制是否属于 phase9，看它是不是"**只有跨机制权衡视角才看得清**"
的东西。PLE 的价值 = 超卖调度；PML 的成本 = 迁移与退出的交叉；主时钟 = 调度迁移与时钟
的耦合。三者都在章节边界上，别处确实没有归属。
