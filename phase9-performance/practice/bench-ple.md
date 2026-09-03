# E1 · PLE 超卖实验：定向让出到底值多少钱

> 这是本章**独占**的实验。`../phase8-capstone/practice/README.md:490` 在结论里
> 明确把这件事推给了本章：
> "**PLE**：单 vCPU、无超卖自旋，测不出；需要多 vCPU 超卖实验。"
> 机制与源码走读在 [`../annotations.md`](../annotations.md) §1，参数与权限在
> [`../parameters.md`](../parameters.md)，本文只管**怎么测出可信的数字**。
>
> **本轮不上机**：所有格子标"待实测"，脚本齐备。

---

## 1. 要回答的问题

四个可判定的是非题，不要写成"PLE 有优化效果吗"这种无法回答的大问句：

| # | 问题 | 判据（最小可辩护形式） |
|---|---|---|
| Q1 | 本环境下 PLE 到底触发了没有？ | 争抢组的 `PAUSE_INSTRUCTION`（退出号 40）计数显著高于不争抢组，且随 `hold_loops`/线程数单调上升 |
| Q2 | 触发之后，宿主真的做了定向让出吗？ | `kvm_vcpu_on_spin` 与 `kvm_vcpu_yield_to` 的函数命中数 > 0，且比值（yield 尝试/on_spin 次数）说得清 |
| Q3 | 让出换来的是收益还是纯开销？ | **只有超卖组** guest 吞吐（`completed/s`）改善；1:1 组关闭 PLE 反而更快或持平 |
| Q4 | 窗口大小有多敏感？ | 极小 / 默认 / 极大 / 关闭 四档的吞吐差值 > 观测噪声（噪声由 [`../measurement.md`](../measurement.md) §4 的扰动预算给出） |

三条 Q1/Q2/Q3 是**与关系**，缺一都不能说"PLE 有效"（[`../annotations.md`](../annotations.md) §1.7 末尾）。

---

## 2. 前置检查：不满足就别开跑

这一节是这个实验最容易翻车的地方，**每一条都是可判定的**，脚本
`bench-ple.sh --preflight` 会逐条打印。

### 2.1 guest 的锁等待必须走 CPL0 的 `PAUSE`

**为什么不能用 `stress-ng --mutex/--futex`**：`"PAUSE-loop exiting"` 这个
VM-execution 控制在 **CPL > 0 时被硬件忽略** ——
`arch/x86/kvm/vmx/vmx.c:5916-5921` 的注释原文引 Intel SDM vol3 ch-25.1.3，
并据此推出"既然 KVM 从不单独置 `PAUSE_EXITING`、只在支持时置 PLE，那么
拿到 PAUSE 退出时 vCPU 必定在 CPL=0"。用户态自旋锁的 `PAUSE` 循环在 ring 3，
**根本产生不了退出**，只能当阴性对照。

所以需要 [`ple-load/`](ple-load/) —— N 个绑核内核线程抢一把 `spinlock_t`，
等待方停在 guest 内核的忙等循环里。

**必须先确认等待方到底停在哪个循环**，这决定要不要动 guest 命令行：

```
guest 内核 CONFIG_PARAVIRT_SPINLOCKS
  ├─ 未开（本仓 scripts/images/kernel.config:297 就是 "is not set"）
  │    → kvm_spinlock_init() 整个没编译（arch/x86/kernel/kvm.c:1049-1139）
  │    → native_pv_lock_init() 见到 hypervisor 就使能 virt_spin_lock_key
  │      （arch/x86/kernel/paravirt.c:56-60），没人再关掉它
  │    → queued_spin_lock_slowpath() 走 virt_spin_lock()
  │      （kernel/locking/qspinlock.c:324 → arch/x86/include/asm/qspinlock.h:88-110）
  │      = TAS + cpu_relax() 的忙等，cpu_relax() 就是 PAUSE
  │      （arch/x86/include/asm/vdso/processor.h:11-20，"rep; nop"）
  │    → ★ 什么都不用改，PLE 收得到信号
  │
  └─ 已开（发行版内核通常 =y）
       → pv_ops.lock.kick = kvm_kick_cpu（arch/x86/kernel/kvm.c:1128），
         等待方不再死转 PAUSE，而是 hypercall 踢醒持锁 vCPU
       → 需要 nopvspin（该 early_param 只在 CONFIG_PARAVIRT_SPINLOCKS=y 时存在，
         kernel/locking/qspinlock.c:586-592）
```

> **踩过的坑**：`nopvspin` 对本仓的实验内核**不存在**，传上去只会变成一个未知
> cmdline 项；反过来，PV 踢锁的退出原因也不是 `MSR_WRITE`(32) ——
> `kvm_kick_cpu()` 走 `kvm_hypercall2(KVM_HC_KICK_CPU,…)`
> （`arch/x86/kernel/kvm.c:1052-1058`），`kvm_hypercall*` 展开成 `vmcall`
> （`arch/x86/include/asm/kvm_para.h:22`），退出号是 `EXIT_REASON_VMCALL` = **18**
> （`arch/x86/include/uapi/asm/vmx.h:47`）。完整记录见
> [`../corrections.md`](../corrections.md) C12。

guest 内自查（脚本会跑这三条并落盘）：

```bash
uname -r
zcat /proc/config.gz 2>/dev/null | grep PARAVIRT_SPINLOCKS \
    || grep PARAVIRT_SPINLOCKS /boot/config-$(uname -r) 2>/dev/null \
    || echo "无 config 副本：用 dmesg 判"
dmesg | grep -iE "PV spinlock|spinlocks (en|dis)abled"
```

### 2.2 别拿 `directed_yield_*` 当判据

debugfs 那两个名字最贴切的统计 **不属于 PLE 路径**，恒为 0。
详见 [`../annotations.md`](../annotations.md) §1.8 与
[`../corrections.md`](../corrections.md) C11。用 `kvm_vcpu_yield_to()` 函数计数替代。

### 2.3 超卖必须是"抢同一条 runqueue"，不是带宽限流

| 手段 | 机制 | 用不用 |
|---|---|---|
| cgroup v2 `cpuset.cpus` | 把 16 个 vCPU 线程**限制在 8 个物理核**上 → 真正 2:1 抢 runqueue，出现" runnable 但没在跑"的兄弟 vCPU —— 正是 `kvm_vcpu_on_spin()` 要挑的对象 | ✅ 主手段 |
| cgroup v2 `cpu.max` | CFS 带宽配额，到周期边界**整组 throttle**。定向让出解决不了"整个组没时间了" | ❌ 只作为额外的第二种压制，不与之混用 |
| `taskset -c 0-15` | 16 线程对 16 个物理核，1:1 | ✅ 不超卖对照组 |

本机拓扑已核实（`/sys/devices/system/cpu/cpu*/topology/`）：Xeon 8163，
2 路 × 24 核 × 2 线程 = 96 逻辑 CPU；**SMT 兄弟是 `N` 与 `N+48`**，
所以 `0-7` / `0-15` 都是**互不相同的物理核**，不会掺进 SMT 共享管线的效应。
NUMA node0 = `0-23,48-71`，因此两组都要同时写 `cpuset.mems=0`，
否则对照组和实验组的内存落点不同，又是一处污染。

### 2.4 只读参数：`ple_*` 全是 0444

`ple_gap` / `ple_window` / `ple_window_grow` / `ple_window_shrink` /
`ple_window_max` 全部 `module_param(..., 0444)`（`arch/x86/kvm/vmx/vmx.c:204` 等），
`echo` 改不动，只能**重载 `kvm_intel`** 或写内核 cmdline。
后果（[`../measurement.md`](../measurement.md) §2 要求）：**每档至少 3 次完整重启 VM**。

有一条**不用重载**的替代路，但它不干净：

```
-per-VM 关 PLE 的两条路
├── ★ 干净：ple_gap=0（重载模块）
│     vmx_vm_init() → if (!ple_gap) kvm->arch.pause_in_guest = true   vmx.c:7639-7640
│     → 只清 SECONDARY_EXEC_PAUSE_LOOP_EXITING                        vmx.c:4627-4628
│     影响面：只有 PLE。
│
└── 省事但**混淆**：QEMU -overcommit cpu-pm=on
      kvm_vm_enable_disable_exits() 只在 enable_cpu_pm 时调用
        （qemu-10.1.0-rc2 target/i386/kvm/kvm.c:3321-3322；cpu-pm 解析在 system/vl.c:1891）
      它一次性传 MWAIT|HLT|PAUSE|CSTATE 四个位
        （同文件 :3120-3132）→ KVM 分别置四个 *_in_guest（x86.c:6595-6600,6606-6610）
      影响面：PLE **加上** HLT/MWAIT/CSTATE 退出全没了。
      → 只能当"这些退出合计值多少钱"的粗对照，不能单独归因给 PLE。
      （变量纠缠规则见 ../measurement.md §6）
      注意 QEMU 10.1.0-rc2 与 11.1.0 该逻辑一致（11.1.0 的
      kvm_vm_enable_disable_exits 在 target/i386/kvm/kvm.c:3287，调用点 :3533）。
```

---

## 3. 实验矩阵

自变量两两正交，**每次只动一个**，其余固定为默认。

| 臂 | 超卖 | PLE | 说明 |
|---|---|---|---|
| A0 | 1:1（`cpuset 0-15`） | 默认（`ple_window=4096`） | 基线：没有"该让谁"的问题 |
| A1 | 2:1（`cpuset 0-7`） | 默认 | 主实验组 |
| A2 | 2:1 | **关**（`ple_gap=0`，重载） | 机制闭合：A1 vs A2 才是 PLE 的净效应 |
| A3 | 2:1 | 窗口极小（`ple_window=128`） | 过早触发 → 让出过多 |
| A4 | 2:1 | 窗口极大（`ple_window=$((1<<24))`） | 几乎不触发 → 应退化到接近 A2 |
| A5 | 1:1 | 关（`ple_gap=0`） | 阴性对照：不超卖时关不关应无差 |

**争抢强度扫描**（回答 Q1 的单调性）：`nr_threads ∈ {4, 8, 16}` ×
`hold_loops ∈ {500, 2000, 8000}`，在 A1/A2 两臂各跑。

`hold_loops` 的物理含义是"持锁多久"。**不要猜它的绝对时长**，先标定：

```
标定方法：guest 内单线程（nr_threads=1）跑，completed/s 的倒数 ≈ 单次临界区
成本（含进出开销）。取 hold_loops=0 作差，得到纯持锁时长。
再和 ple_window 的量纲比较 —— 见 §4 的"窗口单位"警告。
```

---

## 4. 观测点与判据

| 观测量 | 怎么取 | 注意 |
|---|---|---|
| guest 吞吐 | `cat /sys/module/ple_load/parameters/completed` 两次作差 / 时间 | 只读计数（`ple-load/ple_load.c` 里 `module_param_cb(..., 0444)`）；**每档都要等 2 s 预热再采样** |
| `PAUSE_INSTRUCTION`(40) 计数 | `scripts/trace/trace-vmexit.sh`，或 `perf kvm stat -a` | `perf kvm stat` **必须 `-a`**（`../measurement.md` §7） |
| `kvm_vcpu_on_spin` / `kvm_vcpu_yield_to` 命中 | ftrace `function` tracer + `set_ftrace_filter` | 两者在本机 6.8.0-51 的 `available_filter_functions` 中实测**存在**（带 `[kvm]` 后缀）。下 kprobe 取参前按 `AGENTS.md` 陷阱 9 先确认没被内联/改名 |
| PLE 窗口轨迹 | `kvm:kvm_ple_window_update` tracepoint（`arch/x86/kvm/trace.h:978`，参数 `vcpu_id, new, old`） | `grow_ple_window`/`shrink_ple_window` 在本机 `available_filter_functions` 中**查不到**（已内联）→ 窗口变化**只有这一条出口**，且"值没变就不打"，用它计数会低估触发次数 |
| 宿主 CPU% | `/proc/<qemu>/stat` 的 utime+stime 差 | 见 §5 |
| 定向让出成功与否 | `kvm_vcpu_yield_to()` 返回值（>0 成功）；kprobe ret 阶段 | `yield_to()` 有四个提前返回 0/-ESRCH 的口子，`kernel/sched/syscalls.c:1483-1495` |

**★ 窗口单位**：`ple_gap`/`ple_window` 是 VMCS 里的原始字段
（`vmx.c:4774` `vmcs_write32(PLE_GAP, ple_gap)`），单位是处理器的
pause-loop-exiting 计数单位，**不是纳秒也不是微秒**。
把它和 `hold_loops` 换算来换算去必然出错 —— 所以本实验的自变量是
**这两个原始值本身**，结论只在同一台机器上成立，换机器必须重标定。

---

## 5. 执行

```bash
cd phase9-performance/practice
./bench-ple.sh --preflight          # 只做 §2 的检查，不改任何状态
./bench-ple.sh --arm A1 --repeat 10 --dry-run   # 打印将执行的完整命令
sudo ./bench-ple.sh --all --repeat 3            # 全矩阵，需 root
```

`--all` 里涉及 `modprobe -r kvm_intel` 的臂（A2/A3/A4/A5）**默认拒绝执行**，
必须再加 `--allow-reload`；脚本会先确认宿主上没有其它 VM 在跑
（`ls /proc/*/fd | grep -c /dev/kvm`），失败就停下而不是把别人的 VM 拆了。

---

## 6. 已知坑与排查顺序

按顺序排，别跳。

1. **完全没有 `PAUSE_INSTRUCTION` 退出** → 依次查：
   (a) 是不是走了 TCG？`ls -l /proc/<pid>/fd | grep -c kvm` 必须 >0
   （`AGENTS.md` 陷阱 7）；
   (b) guest 的锁等待停在哪个循环？回 §2.1；
   (c) `cat /sys/module/kvm_intel/parameters/ple_gap` 是否为 0；
   (d) QEMU 命令行里有没有 `cpu-pm=on`（`-overcommit`）；
   (e) 争抢是不是太弱，等待方根本没跨过窗口 —— 加大 `hold_loops` 再看。
2. **`PAUSE` 退出有，但 `kvm_vcpu_yield_to` 命中 0** →
   on_spin 找不到目标：`kvm_vcpu_on_spin()` 的筛选条件
   （`virt/kvm/kvm_main.c:4056-4080`）要求目标 `ready`、非自身、
   非 blocking、且（`yield_to_kernel_mode` 时）不是"用户态被抢占"。
   超卖没做够（A0 臂就是这样）时找不到合适目标属于**正常**。
3. **`yield_to()` 返回 0 而不是成功** → 四个提前返回里最常见两个：
   `!curr->sched_class->yield_to_task`（**RT 类没有这个钩子**，
   `.yield_to_task` 只在 fair/ext 类里 —— `kernel/sched/fair.c:13722`、
   `kernel/sched/ext.c:4158`）与 `curr->sched_class != p->sched_class`
   （`kernel/sched/syscalls.c:1489-1495`）。**若用 `chrt` 把 vCPU 线程提成 SCHED_FIFO，
   定向让出会静默失效**，实验结论会整个反过来。本实验禁止对 vCPU 线程改调度类。
4. **每轮结果单调地越跑越慢/越快** → PLE 窗口是 **per-vCPU 且只增不减地
   在 `handle_pause` 里长**（`vmx.c:1417-1431`），回落只发生在
   `vmx_vcpu_load()` 里 `vcpu->scheduled_out && !kvm_pause_in_guest()` 时
   （`vmx.c:1519-1523`，**唯一**触发点）。**同一进程内跨档比较一定被上一档污染**
   → 每档必须重启 VM。
5. **`ple_window_shrink` 的想象**：它默认 **0**（`arch/x86/kvm/x86.h:75`），
   0 表示直接回到基值而不是做除法；它**不**由"PAUSE 间隔 > ple_gap"触发
   （旧文档写的这条是错的，见 `../corrections.md` A3）。
6. **观测自身在吃 CPU** → 本文同时开了 tracepoint 与 function tracer。
   先跑 [`bench-observer-cost.md`](bench-observer-cost.md) 拿到扰动预算，
   再决定这个观测档位下多大的差值才算数。

---

## 7. 结果

**待实测**。跑完回填三处：
本节表格 → [`../index.md`](../index.md) 对应条目（按可信度评级）→
原始数据留 `bench/ple-<timestamp>/`。

| 臂 | 重复 | completed/s（中位 [P25,P75]） | PAUSE 退出/s | on_spin | yield_to 成功 | 宿主 CPU% |
|---|---|---|---|---|---|---|
| A0 | | 待实测 | | | | |
| A1 | | 待实测 | | | | |
| A2 | | 待实测 | | | | |
| A3 | | 待实测 | | | | |
| A4 | | 待实测 | | | | |
| A5 | | 待实测 | | | | |
