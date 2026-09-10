# Phase 9：源码精读注释 - 性能机制

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。
> 参数默认值与权限的唯一来源在 [parameters.md](parameters.md)，本章不重复。

---

## 1. PLE 与定向让出 —— 超卖场景的自救机制

### Q: PLE 退出是怎么产生的？KVM 有没有设 `PAUSE_EXITING`？

**文件**: `arch/x86/kvm/vmx/vmx.c:5911` — `handle_pause()`

KVM **从不设置** `CPU_BASED_PAUSE_EXITING`。`PAUSE_INSTRUCTION`（退出号 40）
只由硬件的 **Pause-Loop-Exiting** 机制产生 —— 硬件自己统计 PAUSE 间隔与累计时间，
超过窗口才退出：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:5911-5924 */
static int handle_pause(struct kvm_vcpu *vcpu)
{
    if (!kvm_pause_in_guest(vcpu->kvm))
        grow_ple_window(vcpu);          /* ★ PLE 退出 → 放大窗口 */

    /* SDM 25.1.3: PAUSE-loop exiting is ignored if CPL > 0.
     * KVM 从不设 PAUSE_EXITING，所以到达这里的 vCPU 必定 CPL=0 */
    kvm_vcpu_on_spin(vcpu, true);       /* ★ 定向让出 */
    return kvm_skip_emulated_instruction(vcpu);
}
```

### Q: 为什么 SDM 25.1.3 那句话决定了实验设计？

*"PAUSE-loop exiting VM-execution control is ignored if CPL > 0"* ——
**用户态自旋永远不会产生 PLE 退出**。推论：

- `stress-ng --mutex/--futex` 这类用户态争抢**不可能**触发 PLE，只能当阴性对照
- 能触发 PLE 的只有 guest **内核态**（CPL0）自旋

### Q: per-vCPU 窗口怎么变化？唯一的回落点在哪？

**文件**: `arch/x86/kvm/vmx/vmx.c:1417` — `grow_ple_window()`

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:1417-1430 */
static void grow_ple_window(struct kvm_vcpu *vcpu)
{
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

`shrink_ple_window()`（`:1433`）结构相同，但第 4 个参数传的是**全局 `ple_window`**
而不是 `ple_window_max` —— 即 per-vCPU 窗口的**下限就是全局值**（默认 4096）。

### Q: `ple_window_shrink = 0` 的语义是什么？

**文件**: `arch/x86/kvm/x86.h:96` — `__shrink_ple_window()`

```c
/* 来源: arch/x86/kvm/x86.h:96-107 */
static inline unsigned int __shrink_ple_window(unsigned int val,
        unsigned int base, unsigned int modifier, unsigned int min)
{
    if (modifier < 1)                  /* ★ shrink=0 → 直接回 base */
        return base;

    if (modifier < base)
        ret /= modifier;
    else
        ret -= modifier;

    return max(ret, (u64)min);
}
```

`modifier < 1` 早退意味着：默认 `ple_window_shrink = 0`（`arch/x86/kvm/x86.h:75`）
不是"不缩小"，而是**一旦被抢占，窗口直接回到全局 4096**。

### Q: 窗口回落只在什么条件下发生？

**文件**: `arch/x86/kvm/vmx/vmx.c:1519` — `vmx_vcpu_load()`

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:1519-1523 */
void vmx_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
    if (vcpu->scheduled_out && !kvm_pause_in_guest(vcpu->kvm))
        shrink_ple_window(vcpu);       /* ★ 唯一的回落点 */
```

条件 `vcpu->scheduled_out` 由 `kvm_sched_out()` 置位。
旧版本写"PAUSE 间隔 > `ple_gap` 时缩小窗口"—— **源码里没有这回事**，
`ple_gap` 从不参与 shrink 判断。

> **实验后果**：窗口是 per-vCPU 的进程内状态，同一 VM 的第二轮会继承第一轮窗口。
> 要么每档重启 VM，要么在一轮里把采样一次做完。

### Q: 关掉 PLE 有哪两条路？

**文件**: `arch/x86/kvm/vmx/vmx.c:7638` — `vmx_vm_init()`

| 路径 | 机制 | 影响范围 |
|------|------|---------|
| 模块级 | `ple_gap = 0` → `pause_in_guest = true` | 全宿主所有 VM |
| per-VM | `KVM_CAP_X86_DISABLE_EXITS` + `KVM_X86_DISABLE_EXITS_PAUSE` | 只影响本 VM |

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:7638-7641 */
int vmx_vm_init(struct kvm *kvm)
{
    if (!ple_gap)
        kvm->arch.pause_in_guest = true;
```

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:4627-4628 */
    if (kvm_pause_in_guest(vmx->vcpu.kvm))
        exec_control &= ~SECONDARY_EXEC_PAUSE_LOOP_EXITING;
```

两条路实验形态完全不同：前者改一次影响全宿主（需重载模块），后者每个 VM 独立。

### Q: `kvm_vcpu_on_spin()` 到底在挑谁？

**文件**: `virt/kvm/kvm_main.c:4037` — `kvm_vcpu_on_spin()`

PLE 退出后 KVM 的"让出 CPU"不是 `schedule()`，而是**定向让出（directed yield）**：
猜一个"很可能持有锁"的 vCPU，直接跟它换。

```c
/* 来源: virt/kvm/kvm_main.c:4037-4098 */
void kvm_vcpu_on_spin(struct kvm_vcpu *me, bool yield_to_kernel_mode)
{
    last_boosted_vcpu = READ_ONCE(kvm->last_boosted_vcpu);
    kvm_vcpu_set_in_spin_loop(me, true);

    for (pass = 0; pass < 2 && !yielded && try; pass++) {
        kvm_for_each_vcpu(i, vcpu, kvm) {
            if (!pass && i <= last_boosted_vcpu) { ... continue; }
            else if (pass && i > last_boosted_vcpu) break;
            if (!READ_ONCE(vcpu->ready)) continue;
            if (vcpu == me) continue;
            /* ★ 优先让给"退出时在内核态"的 vCPU */
            if (READ_ONCE(vcpu->preempted) && yield_to_kernel_mode &&
                !kvm_arch_dy_has_pending_interrupt(vcpu) &&
                !kvm_arch_vcpu_preempted_in_kernel(vcpu))
                continue;
            if (!kvm_vcpu_eligible_for_directed_yield(vcpu))
                continue;

            yielded = kvm_vcpu_yield_to(vcpu);
            if (yielded > 0) {
                WRITE_ONCE(kvm->last_boosted_vcpu, i);
                break;
            }
        }
    }
    kvm_vcpu_set_in_spin_loop(me, false);
    kvm_vcpu_set_dy_eligible(me, false);   /* ★ 同一次自旋不会被反向挑中 */
}
```

| 机制 | 实验含义 |
|------|---------|
| 两轮 pass，从 `last_boosted_vcpu` 起近似 round-robin | 让出目标是**轮转**的，KVM 不知道锁在哪 |
| `try = 3`，`yielded < 0`（`-ESRCH`）才扣次数 | 返回 0 的失败**不扣**，会一路试完 |
| `yield_to_kernel_mode=true` + `preempted` 过滤 | 优先让给"退出时在内核态"的 vCPU |
| 结尾把 `me` 的 `dy_eligible` 清零 | 同一次自旋循环内**不会**被别的 vCPU 反向挑中 |

### Q: `yield_to()` 里有哪些静默失败的关卡？

**文件**: `kernel/sched/syscalls.c:1468` — `yield_to()`

```c
/* 来源: kernel/sched/syscalls.c:1480-1498 */
        /* 两个 rq 各只有 1 个可运行任务 → 无意义 */
        if (rq->nr_running == 1 && p_rq->nr_running == 1)
            return -ESRCH;                     /* ★ 1:1 绑核必命中 */
        ...
        if (!curr->sched_class->yield_to_task) /* ★ RT 类没有此方法 */
            return 0;
        if (curr->sched_class != p->sched_class)
            return 0;                          /* 两边调度类不同 */
        if (task_on_cpu(p_rq, p) || !task_is_running(p))
            return 0;                          /* 目标正忙/已停 */
```

| 早退 | 什么时候咬到实验 |
|------|----------------|
| `-ESRCH` | **1:1 绑核不超卖时必然命中** → PLE 无事可做 |
| `!yield_to_task` | RT 类没有此方法 → `chrt -f` 提优先级后 directed yield **静默全废** |
| `sched_class` 不同 | 只给部分 vCPU 线程设了实时优先级时命中 |
| `task_on_cpu \|\| !task_is_running` | 窗口过小时 `attempted` 暴涨而 `successful` 上不去 |

### Q: 谁给 PLE 喂信息？三个标志位从哪来？

**文件**: `virt/kvm/kvm_main.c:6388` — `kvm_sched_out()`

```c
/* 来源: virt/kvm/kvm_main.c:6388-6400 */
static void kvm_sched_out(struct preempt_notifier *pn,
                          struct task_struct *next)
{
    struct kvm_vcpu *vcpu = preempt_notifier_to_vcpu(pn);

    WRITE_ONCE(vcpu->scheduled_out, true);    /* ★ 无条件置位 */

    if (task_is_runnable(current) && vcpu->wants_to_run) {
        WRITE_ONCE(vcpu->preempted, true);    /* ★ 门槛：自己被抢占但仍可运行 */
        WRITE_ONCE(vcpu->ready, true);
    }
    kvm_arch_vcpu_put(vcpu);
}
```

`preempted`/`ready` 的门槛是 **`task_is_runnable(current)`** —— 被抢占但**自己已阻塞**
的 vCPU 不进 `ready` 集合，永远不会成为 yield 目标。`scheduled_out` 则无条件置位，
同时是 PLE 窗口回落的开关。

### Q: `directed_yield_attempted` 为什么不能当 PLE 的判据？

**文件**: `arch/x86/kvm/x86.c:10026` — `kvm_sched_yield()`

debugfs 里的 `directed_yield_attempted` / `directed_yield_successful`
（`arch/x86/include/asm/kvm_host.h:1598-1599`）**PLE 一次都不会递增**。

它们唯一的递增点在 `kvm_sched_yield()` 里，调用者是**两条 guest hypercall**：

| Hypercall | 用途 |
|-----------|------|
| `KVM_HC_KICK_CPU` | PV spinlock 等待方踢醒持锁 vCPU |
| `KVM_HC_SCHED_YIELD` | Guest 主动请求调度让出 |

PLE 那条路的 `kvm_vcpu_on_spin()` 全程只调 `kvm_vcpu_yield_to()` 并写
`kvm->last_boosted_vcpu`，**一次统计都不加**。

**后果**：没开 PV spinlock 的 guest 上，`directed_yield_*` 恒为 0 ——
用它做验收会让一个**成功**的 PLE 实验看起来彻底失败。

### Q: PLE 的正确判据是什么？

| 出口 | 位置 | 回答什么 |
|------|------|---------|
| `PAUSE_INSTRUCTION` 退出计数 | 退出号 40 | PLE 到底有没有触发 |
| `kvm:kvm_ple_window_update` | `arch/x86/kvm/trace.h:978` | 窗口轨迹（只在值变了时才打）|
| `kvm_vcpu_on_spin()` 函数计数 | `virt/kvm/kvm_main.c:4037` | PLE 处理程序进了几次 |
| `kvm_vcpu_yield_to()` 函数计数 | `virt/kvm/kvm_main.c:3938` | 真正发起了几次定向让出 |

**判据的与关系**：`PAUSE_INSTRUCTION` 计数随争抢强度上升 **且**
`kvm_vcpu_on_spin`/`kvm_vcpu_yield_to` 有命中 **且** 只在超卖组吞吐改善
—— 三者齐了才能说"PLE 在本场景有效"。

---

## 2. EPT 粒度、PML 与脏页日志

### Q: 大页能不能建，真实闸门是什么？

**文件**: `arch/x86/kvm/mmu/mmu.c:3172` — `kvm_mmu_hugepage_adjust()`

```c
/* 来源: arch/x86/kvm/mmu/mmu.c:3177-3186 */
    fault->huge_page_disallowed = fault->exec &&
                                  fault->nx_huge_page_workaround_enabled;

    if (unlikely(fault->max_level == PG_LEVEL_4K))
        return;

    if (is_error_noslot_pfn(fault->pfn))
        return;

    if (kvm_slot_dirty_track_enabled(slot))    /* ★ 开脏页跟踪 → 一律 4K */
        return;
```

| 条件 | 后果 |
|------|------|
| `fault->max_level == PG_LEVEL_4K` | 上游已算出不允许 |
| pfn 无效 | 退到 4K |
| **`kvm_slot_dirty_track_enabled(slot)`** | **开脏页跟踪 → 新建映射一律 4K** |
| `fault->exec && nx_huge_page_workaround_enabled` | iTLB multihit 缓解，可执行大页被砍 |

真正决定级别上限的 `__kvm_mmu_max_mapping_level()`（`mmu.c:3138`）走
`disallow_lpage` 计数与宿主侧 `host_pfn_mapping_level`，**不涉及 `eptad`**。

### Q: 为什么脏页跟踪与大页收益天然对冲？

`kvm_slot_dirty_track_enabled()` 那条早退的含义：**只要 memslot 开着脏页跟踪，
就不新建大页**；已有大页还要被拆。

"迁移期间性能掉了"这句话里**至少混着三项**：

1. 新建映射退到 4K → 缺页次数上升（最多 512 倍映射数差异）
2. 已有大页被 split → 一次性拆表开销 + EPT TLB 压力
3. 脏页记录本身（PML 或写保护）

### Q: `eager_page_split` 把什么代价提前付了？

**文件**: `arch/x86/kvm/mmu/mmu.c:1336`

```c
/* 来源: arch/x86/kvm/mmu/mmu.c:1336-1341 */
    if (kvm_dirty_log_manual_protect_and_init_set(kvm)) {
        gfn_t start = slot->base_gfn + gfn_offset + __ffs(mask);
        gfn_t end = slot->base_gfn + gfn_offset + __fls(mask);

        if (READ_ONCE(eager_page_split))
            kvm_mmu_try_split_huge_pages(kvm, slot, start, end + 1,
                                         PG_LEVEL_4K);
```

动机：*"immediately try to split huge pages, e.g. so that vCPUs don't get saddled
with the cost of splitting"*。默认 `true`（`arch/x86/kvm/x86.c:193-194`）是
**把拆表成本从 vCPU 运行期挪到脏日志 bitmap 同步时**，代价是同步端变慢、收益是
guest 侧长尾变少。`eager_page_split` 是 **0644**，可运行时调。

### Q: PML 的真实开销模型是什么？旧版讲反了哪？

PML 是硬件行为：开启后，guest 对可写页的**首次写**由硬件把 GPA 追加进 512 条目的
VMCS 缓冲区（`PML_ENTITY_NUM`，`arch/x86/kvm/vmx/vmx.h:336`），缓冲区耗尽才产生
`PML_FULL`（退出号 **62**）。**正常写入不退出**。

关键修正：`handle_pml_full()` 其实**什么都不做**：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:5962-5985 */
static int handle_pml_full(struct kvm_vcpu *vcpu)
{
    trace_kvm_pml_full(vcpu->vcpu_id);

    /* PML buffer already flushed at beginning of VMEXIT.
     * Nothing to do here.., and there's no userspace involvement needed for PML. */
    return 1;                            /* ★ 直接返回，不消费 buffer */
}
```

真正的消费在**每一次 VM-Exit 的最前面**，与退出原因无关：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:6451-6452 */
    if (enable_pml && !is_guest_mode(vcpu))
        vmx_flush_pml_buffer(vcpu);      /* ★ 无条件调用 */
```

`vmx_flush_pml_buffer()`（`:6182`）读 `GUEST_PML_INDEX`，空则直接返回，
否则逐条 `kvm_vcpu_mark_page_dirty()` 并复位索引。

### Q: PML 的三条代价怎么对到实验？

| 项 | 结论 | 依据 |
|----|------|------|
| 稳态写入 | 硬件记账，**不产生退出** | `PML_ENTITY_NUM=512` + 上面两条码 |
| `PML_FULL` 退出频率 | ≈ 脏页速率 / 512 | 缓冲区大小 |
| **每次退出的附加成本** | 只要 PML 开着，**所有** VM-Exit 都多走一遍 flush | `vmx.c:6451-6452` 无条件调用 |
| 嵌套 | L2 期间硬件从不启用 PML | `!is_guest_mode(vcpu)` 守卫 |

旧版写"`handle_pml_full()` → KVM 消费 PML buffer → 清空 → 继续"，
把 flush 的位置安错了。

### Q: `eptad=0` 的真正代价是什么？

关掉 EPT A/D 位不等于"每次写都退出"。KVM 用**软件访问/脏位模拟**，
代价体现为写保护导致的额外退出与 TLB flush。`eptad` 是 **0444 只读**
（`arch/x86/kvm/vmx/vmx.c:106`），做 A/B 必须重载 `kvm_intel`。

---

## 3. 主时钟、TSC offset 与提前量

### Q: 主时钟什么时候才允许开？

**文件**: `arch/x86/kvm/x86.c:3015` — `pvclock_update_vm_gtod_copy()`

```c
/* 来源: arch/x86/kvm/x86.c:3025-3036 */
    host_tsc_clocksource = kvm_get_time_and_clockread(
                            &ka->master_kernel_ns,
                            &ka->master_cycle_now);

    ka->use_master_clock = host_tsc_clocksource && vcpus_matched
                           && !ka->backwards_tsc_observed
                           && !ka->boot_vcpu_runs_old_kvmclock;
```

四个条件缺一不可：

| 条件 | 含义 |
|------|------|
| `host_tsc_clocksource` | 宿主 clocksource 基于 TSC |
| `vcpus_matched` | **所有** vCPU 的 TSC 页匹配 |
| `!backwards_tsc_observed` | 没观察到 TSC 倒退 |
| `!boot_vcpu_runs_old_kvmclock` | 没有 vCPU 跑旧版 kvmclock |

### Q: 主时钟失效是单向的吗？

**文件**: `arch/x86/kvm/x86.c:9687` — `pvclock_gtod_notify()`

```c
/* 来源: arch/x86/kvm/x86.c:9687-9689 */
    if (!gtod_is_based_on_tsc(gtod->clock.vclock_mode) &&
        atomic_read(&kvm_guest_has_master_clock) != 0)
        irq_work_queue(&pvclock_irq_work);
```

**只在"离开 TSC 基"这一侧动作**；换回 `tsc` 时 `gtod_is_based_on_tsc()` 为真，
整条链根本不启动。同时把全局 `kvm_guest_has_master_clock` 清 0。

**开的那条边没有任何自动触发**。重算 `ka->use_master_clock` 的入口只有六个：

| 重算点 | 位置 | 谁能碰它 |
|--------|------|---------|
| `kvm_arch_init_vm()` | `:12803`，调用 `:12844` | 新建 VM |
| `kvm_vm_ioctl_set_clock()` | `:7006`→`:7024` | 迁移恢复 |
| `__kvm_synchronize_tsc()` | `:2670`→`:2714` | vCPU 创建后、guest 写 MSR_IA32_TSC |
| `kvm_write_system_time()` | `:2354-2361` | 新旧 kvmclock MSR 切换 |
| `kvm_hyperv_tsc_notifier()` | `:9483`→`:9504` | Hyper-V |
| `pvclock_gtod_update_fn()` | `:9643` | **只有 TSC→非 TSC 一条边** |

**结论**：宿主 `tsc` 换出去再换回来，已经在跑的 VM 不会恢复；
同期新建的 VM 却是开着的。同一台宿主上两台 VM 的时钟路径就此分叉。

### Q: 主时钟关着时，vCPU 迁移多付什么代价？

**文件**: `arch/x86/kvm/x86.c:5030` — `kvm_arch_vcpu_load()`

```c
/* 来源: arch/x86/kvm/x86.c:5030-5035 */
    /*
     * On a host with synchronized TSC, there is no need to update
     * kvmclock on vcpu->cpu migration
     */
    if (!vcpu->kvm->arch.use_master_clock || vcpu->cpu == -1)
        kvm_make_request(KVM_REQ_GLOBAL_CLOCK_UPDATE, vcpu);
```

后面那半句 `|| vcpu->cpu == -1` 是"首次 load 一定要写一次"；
**前半句才是"迁移 × 主时钟关"的乘积项**。

代价链路：

```
KVM_REQ_GLOBAL_CLOCK_UPDATE 被消费
  └─ kvm_gen_kvmclock_update()         x86.c:3434
       ├─ 本 vCPU 置 KVM_REQ_CLOCK_UPDATE
       └─ schedule_delayed_work(kvmclock_update_work, 100ms)
            └─ kvmclock_update_fn()     :3419
                 对**全部** vCPU 置 KVM_REQ_CLOCK_UPDATE + kvm_vcpu_kick()
```

代价不在那 64 字节 pvclock 页写，而在 **kick**：`kvm_vcpu_kick()` 会读对方 vCPU
的 `mode` 并对正在 guest-mode 的 vCPU 发 IPI。主时钟关掉后，一次迁移的最坏后果是
**整台 VM 的 vCPU 被踢一遍**。

限流粒度是**每 VM 一个 `delayed_work`**，上界是每 VM 每 100 ms 一次全量刷新
（约 10 次/秒），与迁移次数无关。

### Q: vCPU 迁移时会改 TSC offset 吗？

**文件**: `arch/x86/kvm/x86.c:5007` — `kvm_arch_vcpu_load()`

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
之类的场景被置。TSC 一致的宿主上，vCPU 从 pCPU-A 迁到 pCPU-B **不需要改 offset**。

旧版把"迁移时更新 TSC_OFFSET"写成常规步骤，与源码不符。

### Q: vLAPIC 定时器提前量能不能手动设？

**文件**: `arch/x86/kvm/lapic.c:1840` — `adjust_lapic_timer_advance()`

```c
/* 来源: arch/x86/kvm/lapic.c:1840-1851 */
static inline void adjust_lapic_timer_advance(struct kvm_vcpu *vcpu,
                                              s64 advance_expire_delta)
{
    u32 timer_advance_ns = apic->lapic_timer.timer_advance_ns;

    /* Do not adjust for tiny fluctuations or large random spikes. */
    if (abs(advance_expire_delta) > LAPIC_TIMER_ADVANCE_ADJUST_MAX ||
        abs(advance_expire_delta) < LAPIC_TIMER_ADVANCE_ADJUST_MIN)
        return;                              /* ★ 死区：不追噪声，不追尖峰 */
```

6.12.93 的暴露形态：

| 项 | 形态 |
|----|------|
| 开关 | 模块参数 `lapic_timer_advance`（`bool`，`arch/x86/kvm/lapic.c:70-71`），**0444** |
| 当前提前量 | per-vCPU **debugfs 只读文件** `lapic_timer_advance_ns`（`arch/x86/kvm/debugfs.c:67`）|
| 手动设固定值 | **做不到**，只能观测自适应结果 |

> **跨版本陷阱**：宿主若跑 6.8，形态正好相反（有 `lapic_timer_advance_ns` 参数、
> 无 bool）—— 先 `ls` 再写文档。

---

## 4. 观测出口汇总

### Q: 本章三块机制各自该看什么 tracepoint？

| 机制 | 出口 | 位置 |
|------|------|------|
| PLE | `kvm:kvm_ple_window_update` | `arch/x86/kvm/trace.h:978`；触发点 `vmx.c:1428`(grow)/`:1444`(shrink) |
| PML | `kvm:kvm_pml_full` | `arch/x86/kvm/trace.h:963`；**只有 vcpu_id 一个参数**，没有 full_count |
| 主时钟 | `kvm:kvm_update_master_clock` | `arch/x86/kvm/trace.h:906` |
| TSC | `kvm:kvm_track_tsc` | `arch/x86/kvm/trace.h:928` |
| TSC offset | `kvm:kvm_write_tsc_offset` | `arch/x86/kvm/trace.h:879` |
| pvclock 更新 | `kvm:kvm_pvclock_update` | `arch/x86/kvm/trace.h:999`（发出点 `x86.c:3212`）|
| debugfs | `vcpu*/tsc-offset`, `vcpu*/lapic_timer_advance_ns` | `arch/x86/kvm/debugfs.c:60-67` |

命令语法与排查顺序见 `../phase10-debugging/annotations.md`；本章只给
"哪个观测点对应哪条判据"。

---

## 5. 已从本章移出的内容

| 旧版章节 | 现在归 | 原因 |
|---------|--------|------|
| halt-polling 自适应算法 | `../phase0-kvm-framework/annotations.md` §9 | 机制归框架章，实测归毕业章 |
| VPID | `../phase1-cpu-virt/` | VMCS 字段权威 |
| APICv | `../phase4-interrupts/` | 机制在 phase4 讲得更深 |
| Posted Interrupts | `../phase4-interrupts/posted-interrupts.md` | 零退出结论有规范支撑（SDM 30.6）|
| EPT 遍历 | `../phase2-mem-virt/` | 机制章 |
| kvmclock vs TSC-deadline | `../phase7-timer-virt/README.md` | 时钟章 |
| 性能调试命令集 | `../phase10-debugging/` | 命令归调试章 |

**本章保留判据**：某机制是否属于 phase9，看它是不是"只有跨机制权衡视角才看得清"
的东西。PLE = 超卖调度；PML = 迁移与退出的交叉；主时钟 = 调度迁移与时钟的耦合。

---

## 6. 完整调用链

```
PLE 路径:
  Guest CPL0 PAUSE 循环
    │
    ├→ 硬件 PLE 机制检测到 → PAUSE_INSTRUCTION(40) VM-Exit
    │
    └→ handle_pause()                    vmx.c:5907
        ├→ grow_ple_window()             vmx.c:1417
        │   └→ __grow_ple_window()       x86.h:80
        │       └→ vmx->ple_window 几何放大
        │
        └→ kvm_vcpu_on_spin(vcpu, true)  kvm_main.c:4037
            ├→ 两轮 round-robin 遍历 vCPU
            ├→ 过滤: ready / preempted / in_kernel
            └→ kvm_vcpu_yield_to(target) kvm_main.c:3938
                └→ yield_to(task, 1)     sched/syscalls.c:1468
                    └→ 四道早退关卡

PML 路径:
  Guest 写可写页
    │
    ├→ 首次写: 硬件追加 GPA 到 PML buffer（512 条目，无 VM-Exit）
    │
    ├→ buffer 满: PML_FULL(62) VM-Exit
    │   └→ handle_pml_full()             vmx.c:5962
    │       └→ return 1（什么都不做，buffer 已在 VM-Exit 开头 flush）
    │
    └→ 每次 VM-Exit 开头:
        └→ vmx_flush_pml_buffer()        vmx.c:6182
            └→ 读 GUEST_PML_INDEX → 逐条 kvm_vcpu_mark_page_dirty()

主时钟路径:
  pvclock_gtod_notify()                   x86.c:9674
    │
    ├→ gtod_is_based_on_tsc() == false?
    │   ├→ 是: irq_work_queue → pvclock_irq_work_fn()
    │   │   └→ queue_work → pvclock_gtod_update_fn()
    │   │       └→ 全部 VM 置 KVM_REQ_MASTERCLOCK_UPDATE
    │   │       └→ 清 kvm_guest_has_master_clock = 0
    │   └→ 否: 不启动（单向失效）
    │
    └→ 消费: vcpu_enter_guest() 里检查 KVM_REQ_MASTERCLOCK_UPDATE
        └→ kvm_guest_time_update()
            └→ pvclock_update_vm_gtod_copy()
                └→ 四条件: host_tsc_clocksource && vcpus_matched
                           && !backwards && !old_kvmclock
                    ├→ 全满足: ka->use_master_clock = true
                    └→ 任一不满足: ka->use_master_clock = false
```
