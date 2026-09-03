# E5 · 观测开销自校准：每一档 trace 到底吃掉多少吞吐、CPU 和计数

> 本文是 **phase9 的"仪器校准"实验**：不研究 KVM 的某个机制，而是量出
> [`../measurement.md`](../measurement.md) §4(b) 那张"扰动预算表"里的每一个数字，
> 并回答"我挂上这一档观测，代价是多少、代价落在谁头上"。
> 驱动器：[`bench-observer-cost.sh`](bench-observer-cost.sh)。
> 上游依赖：`../measurement.md` §4/§6；负载与观测出口的通用前置检查见
> [`bench-migrate.md`](bench-migrate.md) §2.4/§2.5 与 §4.2.1，本文**不重复**。

本轮（按项目决定）**不上机**，§7 的数字全部标"待实测"。但本轮仍然产出三条非实测结论：

* **§2.1 的基线定义错误**（`measurement.md` §4(b) 把基线写成"仅 `tracing_on=0`"）
  —— 从源码查出，回写 `corrections.md` C17；
* **§2.5/§6.2 里我写的一个不存在的 tracefs 文件 `enabled_events`** —— 回写 D6；
* **驱动器已做函数级自测**（`bench-observer-cost.sh` 的读数/复读 helper，49 项断言），
  自测 + 逐条复核抓到**六处**会静默产出错数的真缺陷：`scan_trace` 把注释表头算进
  `rec_lines`、`sched_all` 只读到部分线程时输出部分和而非 NA、`/proc/<tid>/stat`
  剥 comm 用了贪婪匹配、`/proc/stat` 的 busy 把 guest 时间重复计入、O5 的命中模式
  `+0x` 在 `sym-offset` 默认关掉时必然零命中、相对路径的 `--out`/`--report-from`
  被脚本开头的 `cd` 改到脚本目录下解析。全部见 §6.12。

---

## 1. 要回答的问题

| # | 问题 | 为什么必须有数 |
|---|---|---|
| Q1 | 同一负载在 8 档观测强度下，吞吐各掉多少？ | `../measurement.md` §4(b) 的**吞吐损失**列；E3 §4.2 条件 2 的阈值就是它 |
| Q2 | 宿主为观测多花了多少 CPU？多花的 CPU 落在**谁**的上下文里？ | **宿主额外 CPU（CPU-秒）**列。落在 vCPU 线程里 = 直接从退出路径偷时间；落在读者进程里 = 只是抢核 |
| Q3 | 观测会不会改变"退出次数"本身？ | **退出计数漂移**列。若退出率随档位变化，则"每次退出多少 ns"这类归一化数字全部失真 |
| Q4 | `tracing_on=0` 是不是零开销？ | 见 §2.1，**答案已知：不是**。这一臂量化"不是"的量 |
| Q5 | "记录"与"消费（打印）"是不是两笔独立的钱？ | 决定采样策略：写窗口可以短，读文件的时间花在哪 |
| Q6 | 同样每秒上万次的两个事件，成本差多少？ | `kvm_entry` 与 `kvm_exit` 都在退出路径上，但 `TP_fast_assign` 干的事不一样（§2.4） |

**产出被谁用**：① `../measurement.md` §4(b) 表；② 后续任何实验选档位时的依据；
③ E1–E4 每份数据都要按 §5.4 报出当时档位，本表让那个标注有数值含义。

---

## 2. 前置检查

### 2.1 ★ 基线必须先重定义：`tracing_on=0` 不是零开销

旧版 `../measurement.md` §4(b) 把基线写成"nop（基线，仅 `tracing_on=0`）"。
这句**错**，而且是 upstream 自己在注释里写明的错（`kernel/trace/trace.c:1592-1599`，
`tracing_off()` 的 kernel-doc）：

```c
/**
 * tracing_off - turn off tracing buffers
 *
 * This function stops the tracing buffers from recording data.
 * It does not disable any overhead the tracers themselves may
 * be causing. This function simply causes all recording to
 * the ring buffers to fail.
 */
```

链路核对（6.12.93）：

1. `echo 0 > tracing_on` → `tracer_tracing_off()` → `ring_buffer_record_off()`
   （`kernel/trace/trace.c:1575-1581`）。**只动了 ring buffer 的记录开关。**
2. 事件的注册/注销才控制 tracepoint 的 static key：开一个事件 = `TRACE_REG_REGISTER`
   → `tracepoint_probe_register()`（`kernel/trace/trace_events.c:681`）→
   加第一个 func 时 `static_key_enable(&tp->key)`（`kernel/tracepoint.c:363`）；
   移除最后一个 func 时才 `static_key_disable()`（`:419`）。
   **`tracing_on` 不在这条路上。**
3. 于是"事件挂着但 `tracing_on=0`"时，每次事件仍然完整走一遍 probe：
   `do_trace_event_raw_event_##call()`（`include/trace/trace_events.h:382-408`）
   —— trigger 判断 `:392`、算变长字段偏移 `:395`、`trace_event_buffer_reserve()`
   `:397`（内部还要 `tracing_gen_ctx_dec()`，`kernel/trace/trace_events.c:657`）——
   直到 `ring_buffer_lock_reserve()` 开头 `preempt_disable_notrace()`（
   `kernel/trace/ring_buffer.c:4549`）后在 `record_disabled` 上早退（`:4551-4552`）。
4. 省掉的只有两段：`if (!entry) return;`（`include/trace/trace_events.h:400-401`）
   跳过 `TP_fast_assign` 与 `trace_event_buffer_commit()`。

**结论（不依赖实测）**：挂着不记 > 真零；记录中 > 挂着不记。所以本实验的基线 O0
必须是**"没有任何 probe 挂在 tracepoint 上"**（`set_event` 空 + `current_tracer=nop`
+ 无 bpftrace/perf 探针），而"挂着不记"单独成一臂（O1）。
已回写 `../corrections.md` C17，`../measurement.md` §4(b) 表头同步改了。

> 顺带一条：**带 filter 的事件反而多干一段活**。若该 `trace_event_file` 带
> `EVENT_FILE_FL_SOFT_DISABLED|FILTERED`，`trace_event_buffer_lock_reserve()`
> 会先 `preempt_disable_notrace()` + `this_cpu_inc_return(trace_buffered_event_cnt)`
> 走 per-CPU 临时缓冲（`kernel/trace/trace.c:2692-2744`）。E5 默认不挂 filter；
> 谁要用 `set_event` + filter 降开销，得知道这笔反向的账（要量化就加一臂）。

### 2.2 debugfs 统计：必须挑 **per-VM** 那一份，全局同名文件会串台

`../measurement.md` §4(a) 把 debugfs 统计当"最便宜的档"，E5 要用它当**退出次数真值**，
所以必须先把位置钉死。6.12.93 里**同一个名字有两份**：

| 路径 | 建它的代码 | 含义 |
|---|---|---|
| `/sys/kernel/debug/kvm/<pid>-<fd>/exits` | `kvm_create_vm_debugfs()` `virt/kvm/kvm_main.c:1047`，vCPU 级统计在 `:1107`（`stat_fops_per_vm`） | **这台 VM** 的全部 vCPU 之和（`kvm_get_stat_per_vcpu()` `:6135-6145`，无锁、纯求和） |
| `/sys/kernel/debug/kvm/exits` | `kvm_init_debug()` `virt/kvm/kvm_main.c:6363` | **全机所有 VM** 之和 |

本机 6.8.0-51 实测：没有 VM 在跑时，`/sys/kernel/debug/kvm/` 根下直接就是
`exits`、`fpu_reload`、`blocking`… 一整批文件（读值 0）—— 也就是全局那一份。
所以 `bench-migrate.md` §2.5 的"谁可读用谁"对 E3 够用，**对 E5 不够**：
E5 一律优先 per-VM 路径，退化到全局路径时必须打印警告，且 preflight 数一遍
`/dev/kvm` fd 与 debugfs 里的 VM 目录数，**不止一台 VM 就停手**
（否则第二列"宿主额外 CPU%"和第三列"退出计数漂移"都被别人污染）。

### 2.3 function tracer 的 filter：三个坑见 E3，本文只补一条

`set_ftrace_filter` 的三种静默失效（裸名、坏名吞后名、**空 filter = 全开**）
已在 [`bench-migrate.md`](bench-migrate.md) §4.2.1 实测并给了机制行号，
E5 的 `traceable` / 逐名字 `>>` / 复读计数完全沿用那套。

补的一条：**开关 tracer 本身是一批代码补丁，不能算进采样窗**。
写 filter 会走 `ftrace_hash_rec_enable_modify()`（`kernel/trace/ftrace.c:1949`）
→ `ftrace_run_update_code()`（`:2954`）逐 record 改指令；x86 侧是
`text_poke_bp()`（`arch/x86/kernel/ftrace.c:189`）与
`text_poke_queue()`+`text_poke_finish()`（`:250-253`）。
本机 `available_filter_functions` 有 65904 个函数，空 filter 全开时这是几万处改写。
→ 脚本把"设档位"与"开窗"严格分开，档位在 `WARM_S` 稳定后才开计时窗。

### 2.4 单事件这一档为什么选两个事件（O2 与 O3）

`kvm:kvm_entry` 与 `kvm:kvm_exit` 都是**每次 VM-entry/exit 各一条**，频率同量级，
但记录代价不同，因为 `TP_fast_assign` 干的活不同：

| 事件 | `TP_fast_assign` 实际做的事 | 出处 |
|---|---|---|
| `kvm_entry` | `vcpu_id` + `kvm_rip_read(vcpu)`（读寄存器缓存）+ 一个 bool | `TRACE_EVENT(kvm_entry, …)` `arch/x86/kvm/trace.h:17-35`；`kvm_rip_read()` = `kvm_register_read_raw(vcpu, VCPU_REGS_RIP)`，`arch/x86/kvm/kvm_cache_regs.h:116-119` |
| `kvm_exit` | 上面这些 + `kvm_x86_call(get_exit_info)(…)` → **`vmcs_readl(EXIT_QUALIFICATION)`、`vmcs_read32(VM_EXIT_INTR_INFO)`、必要时 `vmcs_read32(VM_EXIT_INTR_ERROR_CODE)`** | `TRACE_EVENT_KVM_EXIT` 宏 `arch/x86/kvm/trace.h:297-335`；`vmx_get_exit_info()`（`arch/x86/kvm/vmx/vmx.c:6153-6172`）→ `vmx.h:708-716`、`:718-726` |

★ 所以 `kvm_exit` 这一档**不是"只读内存"的旁观**：它在 `vmx_vcpu_run()` 里
（调用点 `arch/x86/kvm/vmx/vmx.c:7489`，函数起于 `:7344`）就替 exit handler 把
`VCPU_EXREG_EXIT_INFO_1/2` 读出来并 `kvm_register_test_and_mark_available()`
标记为可用。对被 KVM 自己**本来不读**这些字段的快路径退出而言，这是凭空多出来的
VMCS 读。O2/O3 之差就是这笔钱的量。（这条只说机制，量级待实测。）

反过来说，`TP_printk` 里的 `kvm_print_exit_reason()`（`arch/x86/kvm/trace.h:289-295`，
`__print_symbolic` + `__print_flags` 两个表扫描）**完全不在写端**：打印函数是
`trace_raw_output_##call()`（`include/trace/trace_events.h:188-208`，注册为 `.trace`），
只在读端 `print_trace_line()`（`kernel/trace/trace.c:4346-4393`）里被调。
这就是 Q5 的出处：**记录与消费是两笔账**，O7 专门量消费侧。

### 2.5 bpftrace 走的是另一个 probe，`set_event` 里看不见它

本机 `bpftrace v0.24.0-4accb077`，`timeout … bpftrace -e 't:kvm:kvm_exit { @=count(); }'`
实测输出 `Attached 1 probe`（无 VM 在跑也能挂上，0 事件）。

机制（6.12.93）：perf/bpf 侧注册的是 `TRACE_REG_PERF_REGISTER` →
`tracepoint_probe_register(call->tp, call->class->perf_probe, call)`
（`kernel/trace/trace_events.c:691-696`）—— **同一个 tracepoint，另一个函数**。
所以：

* bpftrace 一臂**不需要** `set_event`，`cat set_event` 会显示空 →
  用 `set_event` 判断"有没有人在 trace"会漏掉它。★ tracefs 里**没有**
  `enabled_events` 这个文件（6.12.93 全树零命中，本机也没有；`corrections.md` D6），
  preflight 因此查的是另外三条：`set_event` 复读（事件侧）、
  `enabled_functions`（ftrace ops 侧，含 kprobe/bpf trampoline）、
  `bpftool -jp prog` + `bpftool link list` + 进程表（bpf/perf 侧）。
  第三条必须看**挂在哪个 tracepoint**：外部 BPF 只要落在 `kvm` 组里，O0"无探针"的前提
  当场作废（static key 开着，§2.1）→ 停手；落在别组（本机实测是
  `sched_process_fork/exec/exit` 三个）只影响 O6 的归因 → 打印名字后继续。
* 它的 probe 体 `perf_trace_##call()`（`include/trace/perf.h:16-56`）与 ftrace 那条
  早退逻辑不同：`:34-37` 只在"既没有 BPF 程序也没有 perf 事件"时才早退，
  否则 `:43` `perf_trace_buf_alloc()`、`:47` `perf_fetch_caller_regs()`、
  **`:49-51` 无条件跑完 `TP_fast_assign`**、`:53` 提交。
  也就是**聚合型的 bpftrace 探针照样填充整条记录**，只是不往 ftrace ring buffer 写。
* `perf_trace_run_bpf_submit()`（`kernel/events/core.c:10535-10550`）：
  `:10542` `if (!trace_call_bpf(call, raw_data) || hlist_empty(head)) return;`
  —— BPF 程序返回 0 就不进 `perf_tp_event()`，样本不拷到用户态环形缓冲。
  `trace_call_bpf()`（`kernel/trace/bpf_trace.c:110-150`）每次事件都要
  `__this_cpu_inc_return(bpf_prog_active)` 递归检查（`:114`）+ `bpf_prog_run_array()`
  （`:145`）。**bpftrace 的聚合探针到底返不返回 0，本文不断言，交给 O6 实测。**
* `perf_trace_buf_alloc()`（`kernel/trace/trace_event_perf.c:398-423`）：取递归上下文
  + per-CPU 缓冲 + 一次 `memset`。

### 2.6 负载与独占

* 负载固定 `ple_load.ko workload=1`（每线程私有缓冲区、无锁）。不用 `workload=0`
  的理由见 [`bench-migrate.md`](bench-migrate.md) §3.1，另加一条 E5 特有的：
  锁争抢吞吐对宿主时序极敏感，档位一动、争抢窗口就动，Q1 无法归因到观测本身。
* 采样窗内宿主必须**只有这一台 VM、只有本脚本在 trace**。preflight 查的出口与严重程度
  （★ `enabled_events` 不存在，别照抄；`corrections.md` D6）：

  | 出口 | 判据 | 致命 / 告警 |
  |---|---|---|
  | `/proc/*/fd` 里 `/dev/kvm` 引用 | 数出**别的** VM | 致命（`../measurement.md` §5）。`--i-know-other-vms-exist` 只能降级成告警，此时**全局那份 debugfs 统计、O5 的 function 命中、`/proc/stat` 的宿主总 CPU 三处全部串台**（§2.2、§3.1）；只有 per-VM `exits` 与 guest 侧 `completed` 仍然干净 |
  | `current_tracer` / `tracing_on` / `function_profile_enabled` | 必须分别是 `nop` / 未开 / 0 | 致命（残留会让 O0 不是基线） |
  | `set_event` 复读 | 必须为空 | 致命 |
  | `set_ftrace_filter` | 必须为空 | 致命 |
  | `enabled_functions` | 本机**常态就有 6 条**（kprobe、bpf trampoline 都在里面），所以**不看条数**；只看我们那 5 个退出路径函数是否已在其中 | 命中 → 致命（O5 的命中数不是本负载造成的）；否则只打印前 3 条备查 |
  | tracepoint 型 BPF（`bpftool`） | 看**挂点名**与 `kvm` 组求交集 | 交集非空 → 致命（O0 前提破）；挂在别组 → 告警并打印名字 |
  | `trace_pipe` 读者（`/proc/*/fd`） | 数引用 | 致命（消费侧成本会被别人混进来） |
  | `bpftrace` / `perf` 进程 | `pgrep -x` | 致命（它们走同一个 tracepoint 的另一个 probe，§2.5） |

---

## 3. 实验矩阵

一臂 = 一次独立采样窗（默认 `--sample-s 20`）。档位（`--arms` 可挑子集，默认全选）：

| 臂 | 这一档开着什么 | 回答 |
|---|---|---|
| **O0** | 真零：`set_event` 空、`current_tracer=nop`、无外部探针（先跑一次，末了再跑一次 O0e） | Q1 基线 / Q3 基线 / 漂移对照 |
| **O1** | `set_event` 里挂 `kvm:kvm_exit`，但 `tracing_on=0`（挂着不记） | **Q4**：与 O0 之差 = 静默成本 |
| **O2** | 单个 tracepoint 记录中：`kvm:kvm_entry` | Q1/Q6：最省的 `TP_fast_assign` |
| **O3** | 单个 tracepoint 记录中：`kvm:kvm_exit` | Q1/Q6：多读 VMCS 的那一档 |
| **O4** | `kvm:*` 全开（本机 91 个事件目录） | Q1：整组打开的常见用法 |
| **O5** | `function` tracer + `set_ftrace_filter` 5 个退出路径函数 | Q1/Q2：表里"function tracer（5 个函数）"那行 |
| **O6** | bpftrace 聚合探针 `t:kvm:kvm_exit { @c=count(); }`（`set_event` 保持空） | Q1/Q2：聚合型 eBPF |
| **O7** | O3 之上再挂一个常驻 `cat trace_pipe > /dev/null` | **Q5**：消费侧独立成本 |

### 3.1 为什么 O5 是 5 个函数

`measurement.md` §4(b) 原来就写了"5 个函数"，沿用。选的是**每次退出必然命中**的三个 +
两个只在缺页/APIC 路径上命中的，这样"命中次数差异"本身也成了观测点：

```
vcpu_enter_guest   vmx_vcpu_run   vmx_handle_exit   handle_ept_violation   kvm_mmu_page_fault
```

本机 6.8.0-51 `available_filter_functions` 实测这 5 个**全部可跟踪**（各 1 条命中）。
preflight 逐个 `traceable` 复核，缺哪个就报错并要求换名字 ——
**函数名跨版本会变，不许照抄**（`../measurement.md` §5 第 2 条）。

★ 这五个函数是**宿主全局**的：`function` tracer 会记录宿主上任何进程调用它们的路径，
所以"只有本脚本在 trace + 只有一台 VM"两个前提缺一不可（§2.6）。

### 3.2 为什么顺序固定、O0 跑两遍

档位之间不可交换：O4 关掉后 `set_event` 里残留的 enable 位、O5 的补丁状态、
O6 的 BPF 程序卸载都要时间。所以：
`O0 → O1 → … → O7 → O0e`，**每臂结束立刻把 ftrace 恢复到"什么都没开"再进下一臂**；
O0e 用来判漂移（§4.3 条件 2）。默认单轮；`--repeat N` 跑 N 轮**交错**
（第 k 轮所有臂跑完再进第 k+1 轮），报中位数与极差。

### 3.3 为什么不用"全系统所有事件"

那会打开 `sched`/`irq`/`exceptions` 等每秒几万到几十万的事件，ring buffer 必溢出，
测到的第一件事是"丢了"，不是"多贵"（`../measurement.md` §4(c)）。
E5 的 O4 只做 `kvm:*`，就是"研究 KVM 时最可能开的一档"。

---

## 4. 观测点与判据

### 4.1 三个因变量的取法（全部与 trace 无关，档位切换不影响取数）

| 量 | 怎么取 | 出处 / 注意 |
|---|---|---|
| 吞吐 `completed/s` | 窗口两端各读一次 guest 内 `/sys/module/ple_load/parameters/completed`（经串口），差 / 实测窗长 | 参数只读且 `0444`（`ple-load/ple_load.c:48-51`、`completed` 于 `:134-143`）；读它是 guest 的一次 syscall，不进宿主账。★ 时间戳取在"发读命令之前"（t0）与"发读命令之后"（t1），两端各自的串口往返延迟都落在 `[t0,t1]` 之外，否则窗长被高估、`completed/s` 被低估 |
| 退出次数 `exits` | 同两端读 **per-VM** debugfs `exits`，取差值（§2.2） | `++vcpu->stat.exits` 无条件累加：`arch/x86/kvm/x86.c:11094`（快路径循环内）与 `:11157`（慢路径），二者互斥 → 每次退出恰好计一次 |
| vCPU 线程 CPU | 同两端读四个 vCPU 线程 `/proc/<qemu-pid>/task/<tid>/schedstat` 第 1 列 `sum_exec_runtime`（ns）求和，取差值 | `proc_pid_schedstat()` `fs/proc/base.c:511-522`（三列 = `sum_exec_runtime run_delay pcount`）；★ 观测的**记录**成本就发生在这个线程上下文里，所以这一列直接吃得到。**不做端点差值以外的累加** —— 窗内 tick 只用来核对单调性与"有没有掉核"（落 `tick-<O#>.tsv`），不参与派生量 |
| vCPU 线程排队 | 同上第 2 列 `run_delay`（ns）差值 | 用来判"是不是只是被抢核"而非"路径变长"。★ 本机 6.8 实测纯忙循环这一列为 **0**，只有真被抢过核的线程才有值 → preflight 现场报告，整列为 0 时这一项按 NA 处理，不许当成"没有被抢" |
| 线程 `stime` | 同两端读 `/proc/<pid>/task/<tid>/stat` 的 stime 列（clock ticks）求和取差 | `fs/proc/array.c:604-605`；与 `sum_exec_runtime` 互相核对，量级差太多说明读数被截。**两个必须绕开的坑**：① 第 2 列 `comm` 实测是 `CPU 0/KVM`，**含空格** → 不能按空白直接数下标，先剥掉第一个 `") "` 之前的部分再取第 13 个字段；② 剥离必须用**最短**匹配（`${line#*) }`），贪婪的 `##*) ` 会切到行内最后一个 `") "`，而第 28 列是可执行文件路径，路径里带 `") "` 就把 stime 静默截错（procps 同样按第一个 `)` 切）。★ **任一线程读不到就整行报 NA**：部分求和会系统性低估 Δ，而低估不会像 NA 那样被打出来 |
| 宿主总 CPU | 同两端读 `/proc/stat` 首行，`busy = $2+$3+$4+$7+$8+$9`（user nice system irq softirq steal）、`idle = $5+$6`（idle iowait） | 字段顺序 `fs/proc/stat.c:128-137`。★ **不能加 `$10/$11`（guest / guest_nice）**：guest 时间已经折进 user/nice 里（`account_guest_time()` `kernel/sched/cputime.c:143-159`：`:150` 加进 `p->utime`、`:154-158` 同时 `task_group_account_field(CPUTIME_USER/NICE)` 与 `cpustat[CPUTIME_GUEST*]`；tick 路径 `irqtime_account_process_tick()` `:406` 与 VTIME 路径 `vtime_account_guest()` `:690` 都调它，两条配置都得同一个结论），再加一次就是把整台 VM 的 CPU 重复计入 —— 而本实验里 VM 的 CPU 恰好随档位变化，这一项会假性暴涨。全机 96 线程聚合，必须报 **CPU-秒绝对量**、不能报百分比（§6.4） |
| 消费侧 CPU | O6/O7 里那个**消费者进程自己**的 `/proc/<pid>/schedstat` 第 1 列，同样两端取差 | 用来把成本劈成"记录端 vs 消费端"。★ O7 的 PID 必须是 `cat` 本人：`( ulimit …; exec cat … ) &` 用 `exec` 把子 shell 换成 `cat`，`$!` 才等于真正的读者；否则量到的是一个几乎不耗 CPU 的外壳，消费成本全部漏计 |

派生量（脚本按上表原始值直接算，`report()` 里每臂先对 `--repeat` 取中位数）：

* `吞吐损失% = (O0 中位 − 本臂) / O0 中位 × 100`
* `额外宿主 CPU-s = (本臂 busy_jif − O0 busy_jif) / USER_HZ`
* **`每百万次退出的 vCPU 线程 ns` = (Δsum_exec_runtime − O0 的) / (本臂 exits/s × 本臂窗长) × 1e6**（主归一化口径）
* `退出率漂移% = (本臂 exits/s − O0 exits/s) / O0 exits/s × 100`
* O5 另报 `每次命中 ns = Δsum_exec_runtime / 本臂 trace 命中行数`（§6.6）

### 4.2 每臂必须先自证"这一档真的生效"

不生效的臂**不是"代价为 0"，是"没有数据"**（`../measurement.md` §6 归因纪律）。
`rec_lines` = 该臂窗口结束时 `trace` 里**非注释头**的行数（`scan_trace` 的第二个返回值），
`ev_lines` = 命中该臂特征串的行数。

| 臂 | 生效证据 | 通过条件（不满足即 FAILED，派生量不采） |
|---|---|---|
| O1 | `set_event` 复读里**有一行裸 `kvm:kvm_exit`**；`tracing_on` 读回 0 | `rec_lines == 0` **且** `overrun == 0` —— 挂着但确实一条没记 |
| O2/O3 | `ev_lines`（`kvm_entry:` / `kvm_exit:` 行数）> 0，且与同窗 `exits` 同量级（§4.3 条件 3） | `ev_lines > 0`；为 0 的第一嫌疑是走了 TCG（陷阱 7） |
| O4 | 逐目录 `cat events/kvm/*/enable` 数出值为 `1` 的个数，必须**等于**开始前 `ls -d events/kvm/*/` 的目录数（本机 91/91，落盘 `kvm_events_enabled=`）；再要 `ev_lines > 0` | 两者都成立；装不满说明有事件挂不上，"整组打开"这一档的定义不成立 |
| O5 | `current_tracer` 读回 `function`；`set_ftrace_filter` 复读里 **5 个名字逐条命中**；命中数 > 0 | 三条全成立。**★ 命中数不能按 `+0x` 数**：`TRACE_DEFAULT_FLAGS`（`kernel/trace/trace.c:479-486`）里**没有** `TRACE_ITER_SYM_OFFSET`（本机 `options/sym-offset` 实测 `0`），`trace_seq_print_sym()`（`kernel/trace/trace_output.c:364-383`）因此走 `kallsyms_lookup()` 而不是 `sprint_symbol()`，函数行只印**裸函数名**、不带 `+0x偏移/大小`。本档 events 全关 + 窗首清过缓冲 ⇒ 函数记录是缓冲里唯一的写者，所以命中数 = `rec_lines`。量级核对：`ev_lines / exits` 应当 **≥ 3**（每次退出必中 3 个函数）且没有大到离谱，否则缓冲里混进了别的东西（例如某驱动调 `trace_printk()`，`TRACE_ITER_PRINTK` 默认开着）。**必须按名字核对 filter、不能数条目**：`print_rec()`（`kernel/trace/ftrace.c:4303-4320`）给每个名字追加 `" [module]"` 后缀，且 filter 为空时打印 `#### all functions enabled ####` 并在 `hash_contains_ip()`（`:1513`）里当"全匹配" |
| O6 | bpftrace 的 `@c` 在窗口结束前打印且 > 0；同时 `set_event` 复读**仍为空**（§2.5 的"看不见"必须现场成立，落盘 `set_event_during_bpftrace=`） | `@c > 0` |
| O7 | O3 的全部条件 + 读者确实收到行（`pipe-O7.txt` 里 `ev_lines > 0`）；**且捕获未撞顶** | `ev_lines > 0` 且 `pipe_bytes < 上限`（§6.13） |
| O0/O0e | 四项残留全为 0：`set_event` 复读为空、`current_tracer=nop`、`tracing_on=0`、无 bpftrace/perf/trace_pipe 读者；另加**无外部 BPF 程序挂在 `kvm` 组**（`bpftool link list` 的挂点名与 `ls events/kvm` 求交集，§2.5） | `rec_lines == 0` **且** `overrun == 0` —— 基线窗内连一条记录都不该有 |

★ `set_event` 的格式（决定了上第一行为什么这么写）：seq_file 只输出**已启用**的事件，
一行一个裸 `system:name`，没有 `[+1]` 也没有 `#` 头 ——
`t_show()` `kernel/trace/trace_events.c:1445-1453`、`s_next()` `:1413-1423`
（启用判据在 `:1421`）、ops 表 `:2251-2256`。所以核对方式是
`set_event 复读里有没有这一行`（脚本 `event_on` 用 `grep -xF`），
不是"看那一行是不是 `[+1]`"。

### 4.3 三条硬性对照（不是判据，是"这轮能不能要"）

1. **ring buffer 溢出必须为 0**：每臂窗口结束逐 CPU 读 `per_cpu/cpu*/stats`，
   `overrun` 与 `commit overrun` 求和；非 0 → 该臂作废（`../measurement.md` §4(c)）。
   同时 `grep -c '\[LOST'` —— 读端确实会打一行 `CPU:%d [LOST %lu EVENTS]`
   （`kernel/trace/trace.c:4352-4358`，计数来自 `ring_buffer_iter_dropped()` `:3515-3527`），
   但它**只是输出里的一行**，不会让任何系统调用失败，所以两个都要看。
2. **漂移对照**：`|O0e − O0| < 1/3 × min(相邻臂间差)`，**两边都是 `completed/s` 的绝对量**。
   不满足 → 整轮作废（机器在漂，档间差读不出来）。默认按 `completed/s` 判。
   ★ 别把左边写成百分比：`|Δ|/O0`（无量纲）去比 `1/3 × 臂间差`（有量纲）在数值上几乎永远
   成立，这条判据就形同虚设 —— 本文初稿正是这么写的，脚本按绝对量实现后才发现。
   本机 96 线程、独占 + 固定负载下 O0 与 O0e 的差应当远小于任何相邻臂差；
   若机器本身在漂（别的租户、温度降频），这一条先炸，后面的档间差全部不可引用。
3. **计数交叉核对**：O3 窗内 `kvm_exit:` 行数 vs 同窗 per-VM `exits` 增量。
   * `trace 条数 ≤ exits 增量` 是**允许**的：`vmx->fail` 那条旁路在
     `trace_kvm_exit()` 之前就 return（`arch/x86/kvm/vmx/vmx.c:7483-7484`），
     但这条退出照样被 `arch/x86/kvm/x86.c:11157` 计数。正常机器上 `vmx->fail`
     只在 VM-entry 硬失败时出现，所以**差额超过 1% 就按丢事件处理**
     （对照 1 漏网），不要自我安慰成"本来就不等"。
   * `trace 条数 > exits 增量` **一定是归属错了**：要么 per-VM 路径退化成了
     全局聚合文件（§2.2，混进别的 VM），要么该臂的窗与 `exits` 采样没对齐。
   ★ 这一条是"退出计数漂移"那一列的可信度前提：真值取自 debugfs，不取自 trace。

### 4.4 预注册预期（先写预期，再测；测反了要回写 §6）

| 预期 | 依据 |
|---|---|
| O1 的损失 < O2 的损失，但 **O1 > O0 可测** | §2.1（`TP_fast_assign`/commit 省了，probe + reserve 早退没省） |
| O3 > O2 | §2.4：`kvm_exit` 多两三次 `vmcs_read*` |
| O4 ≫ O3 | `kvm:*` 91 个事件都挂着 → 每个相关 tracepoint 的 static key 都开；且 `kvm_page_fault`/`kvm_mmu_*` 在缺页路径上会命中 |
| O5 的损失大但**与退出次数不成比例**（按命中次数计） | `function_trace_call()`（`kernel/trace/trace_functions.c:179-204`）每次命中都跑，成本挂在函数上而不是 VM-exit 上 → 用 `exits` 归一化会失真，必须同时报"每命中成本" |
| O6 的**记录端**成本落在 vCPU 线程，消费端（bpftrace 用户态）另计 | §2.5 |
| O7 与 O3 的 `Δsum_exec_runtime` 接近，但 O7 多出一笔读者进程 CPU 与宿主总 CPU | §2.4 打印侧只在读端 |
| 所有臂 `exits/s` 漂移 < 吞吐损失 | 观测改变的是耗时，PLE/中断驱动的退出**次数**由 guest 行为决定；若漂移大到与损失同量级，说明负载形态本身被扰动，全部归一化数字重算 |

---

## 5. 执行

```bash
./bench-observer-cost.sh --preflight                 # 只读，不碰系统
./bench-observer-cost.sh --all --dry-run             # 打印整条时间线
sudo ./bench-observer-cost.sh --arms O0,O2,O3 --sample-s 20
sudo ./bench-observer-cost.sh --all --repeat 3
```

* 参数：`--arms <列表>`、`--all`、`--sample-s <秒>`、`--warm-s <秒>`、`--repeat <N>`、
  `--vcpu <N>`、`--priv-kb <N>`、`--tick-s <秒>`、`--buf-kb <N>`、
  `--funcs "5 个名字"`、`--kernel <bzImage>`、`--out <目录>`、
  `--report-from <目录>`、`--i-know-other-vms-exist`、`--preflight`、`--dry-run`。
  ★ **没有 `--event`**：`kvm_entry` / `kvm_exit` 是常量（脚本 `EVENT_ENTRY`/`EVENT_EXIT`），
  换事件名等于换实验定义，不做成命令行参数。
* `--buf-kb` 是**每 CPU** 的（`buffer_size_kb` 语义），总量 = 该值 × `nproc`。
  本机 96 线程 × 默认 8192 kB = **768 MiB**，preflight 会拿它和 `MemTotal` 的 1/4 比，
  超了就提示调小（这一写会走 `tracing_resize_ring_buffer()`，真金白银按 CPU 分配内存）。
  ★ 别指望内核替你兜着：`buffer_size_kb` 没有这种上限，写它只挡 `0`
  （`tracing_entries_write()` `kernel/trace/trace.c:6797-6801` "must have at least
  1 entry" → 其余一律按 CPU 去 `tracing_resize_ring_buffer()`）；会拒绝过大的
  是**另一个**文件 `buffer_subbuf_size_kb`，按 order>7 返回 `-EINVAL`
  （`kernel/trace/trace.c:9170-9172` "limit between 1 and 128 system pages"）。
  ★ 缓冲开大只改变**溢出概率**，不改变记录成本 —— 跨臂必须同一值，
  否则 `overrun` 列不可比（§4.3 条件 1）。
* 时间预算：**8 档观测强度 = 9 个采样窗**（含收尾基线 O0e），
  9 × (warm 3s + sample 20s) ≈ 207 s，加档间恢复、每臂读 `trace`
  与逐 CPU `stats`、串口往返 → 默认单轮约 **3~4 分钟**；`--repeat 3` 约 9~11 分钟。
  VM **只起一次**，档位切换不动 VM（动了就不是同一负载）。
* guest 侧计数器怎么读：QEMU 用 `-serial pipe:<前缀>`（脚本自己 `mkfifo` 两个管道），
  读值 = 往串口写一条 `cat /sys/module/ple_load/parameters/completed` 再从
  `serial-<tag>.log` 里等回显。不用 ssh/9p 轮询，是为了**不在窗口里引入网络栈的耗时**。
* 落盘：`bench/observer-cost-<ts>/`（`--out` 可改；相对路径按**调用者 cwd** 展开）
  * `params.txt` —— 本轮全部参数（含 `--funcs` 实参），事后判断可比性就看它
  * `arm-<O#>.txt` —— 该臂一行全部原始读数（`rt_ns`/`busy_jif`/`ev_lines`/…）
    + `stat_path=`/`kind=`（`exits` 取自 per-VM 还是全局）+ 该臂专属字段
  * `win-<O#>.txt` —— 该臂 `trace` 快照，**上限 2 MB**（`WIN_MAX`），全量计数另在
    `arm-<O#>.txt` 里 —— 快照是截断的、计数不是
  * `bufstats-<O#>.txt` —— 逐 CPU `per_cpu/cpu*/stats` 原文（`entries`/`overrun`/
    `commit overrun`），§4.3 条件 1 的证据
  * `tick-<O#>.tsv` —— 窗内每 `--tick-s` 一行：时间戳 + `snap_all` 的 7 列
    （`rt rd stime busy idle exits cons`），**只用于核对单调性与掉核**，
    不参与派生量（§4.1）
  * `bpftrace-<O#>.txt` / `bpftrace-<O#>.pid`、`reader-<O#>.pid`、`pipe-O7.txt`
    （O7 读者落盘，受 `ulimit -f` 硬上限约束，§6.13）
  * `qemu.err`、`serial-<tag>.log`、`ser-<tag>.in` / `ser-<tag>.out`（两条 fifo）
  * `fingerprint-start.txt` / `fingerprint-end.txt` —— 外部状态摘要；`--dry-run`
    下两者必须逐字节相同（驱动器实测：dry-run 不产生任何写）
  * `orig-ftrace.txt` —— 开跑前读到的原值快照，`restore_ftrace` 的回写依据
  * `report.err` —— 汇总 awk 的 stderr（有它说明 `summary.tsv` 格式出问题）
  * `summary.tsv` —— **一行一臂一轮**，14 列（tab 分隔，无表头）：
    `1 臂 / 2 实测窗长 s / 3 completed_per_s / 4 exits_per_s / 5 rt_ns(Δsum_exec_runtime)
    / 6 busy_jif / 7 run_delay_ns / 8 stime_ticks / 9 cons_rt_ns / 10 ev_lines
    / 11 overrun / 12 lost_lines / 13 rec_lines / 14 stat kind`
* **退出恢复**：`trap cleanup EXIT INT TERM`。`cleanup()` 依次停采样器、杀侧路进程
  （bpftrace / trace_pipe 读者）、关 VM、`clear_ftrace` + `restore_ftrace` —— 后者把
  `function_profile_enabled`、`current_tracer`、`set_ftrace_filter`、`set_event`、
  `tracing_on`、`buffer_size_kb`、`trace_clock` **逐项写回 `orig-ftrace.txt` 里的原值**
  （`set_event`/filter 是逐条重放，不是简单清空）。★ 两个不碰系统的出口：
  `--report-from` 立刻 `return`（只重算已有数据），`--dry-run` 全程只打印。

### 5.4 报数模板（任何用了 trace 的实验都要照此标注）

```
档位：O3（单事件 kvm:kvm_exit 记录中）｜buffer_size_kb=8192/_cpu｜窗长 20s
本轮 overrun 合计=0，[LOST 行数=0
```

---

## 6. 已知坑

1. **`tracing_on` 与 `set_event` 是两回事**。清场必须两件都写；只看
   `tracing_on=0` 会以为"没人在 trace"，其实 probe 还挂着（§2.1）。
2. **bpftrace / perf 不写 `set_event`**，preflight 只查 `set_event` 会漏（§2.5）。
   ★ tracefs **没有 `enabled_events` 这个文件**（本文初稿写过，登记在
   `../corrections.md` D6）。驱动器实际查三处：`set_event` 复读（ftrace 侧事件）、
   `enabled_functions`（ftrace ops 残留 —— 本机常态就有 6 个函数挂着，**不能按"非零即停"**，
   只有当我们的 5 个目标函数出现在里面才判致命）、`bpftool -jp prog` +
   `bpftool link list` + `pgrep -x bpftrace/perf`（bpf/perf 侧）。判据挂在
   **"外部探针是否落在 `kvm` 组"**上：落了 O0 就不是真零基线，必须停手；
   没落（本机实测只有 `sched_process_fork/exec/exit` 三个）只记录不阻塞。
3. **同名 debugfs 统计有两份**，全局那份把所有 VM 加在一起（§2.2）。E3 的
   "谁可读用谁"在这里会把别人 VM 的退出算进真值。
4. **`/proc/stat` 是 96 线程求和**，"宿主额外 CPU%" 分母是 96 核 → 观测者的
   绝对量可能只有 1~2 CPU-s，百分比看着像 0.2% 一样无关紧要，但那 1~2 秒
   恰好压在 vCPU 线程所在核上时吞吐就掉了。所以第二列必须报 **CPU-s 绝对量**，
   并附 `Δsum_exec_runtime`（精确到 ns、且落在正确的线程上）。
5. **分母会跟着动**：吞吐掉了，`exits` 也往往掉。所以每百万退出的 ns 要用
   **本臂自己的** `exits`，并同时报"每窗绝对增量"，两个口径一起看，
   只用一个会得出相反结论。
6. **O5 的成本不挂在退出上**。5 个函数里 `vcpu_enter_guest`/`vmx_vcpu_run`/
   `vmx_handle_exit` 每次退出各命中一次，另两个只在缺页路径命中 →
   "每退出成本"会随负载的缺页密度变化。必须同时打印每次命中的成本
   （命中数 = 该臂 `trace` 的**全部非表头行数**，**不是** `+0x` 行数 ——
   `sym-offset` 默认关着，函数行里没有 `+0x`，见 §4.2 O5 行）。
7. **别在窗口中间改档位**。`ftrace_run_update_code()` 的一批 `text_poke`
   会把几毫秒的改写记进 `sum_exec_runtime`（§2.3）→ 那臂作废。
8. **`cat trace` 不是免费的**：它把整块缓冲格式化打印（读端 `print_trace_line()`
   → 每行 `TP_printk` 展开，`kvm_exit` 还要扫两张 reason 表）。O7 就是为它设的。
   ★ 驱动器自己的取数顺序因此是：**先 `tracing_on=0` 停记录**（缓冲不再增长），
   再一次 `scan_trace` 同时"写受限快照 + 数行数 + 数 `[LOST`"，统计全在快照上做；
   这一切落在计时窗**之外**（t1 已取），所以读端成本不会记进任何臂的账。
   人工做实验时同理：`grep -c` 之前先把缓冲固定住，别对活的 `trace` 反复读。
9. **AGENTS.md 陷阱 7**：VM 必须以 `-enable-kvm -cpu host` 起，
   `verify_kvm()` 数 `/dev/kvm` fd，为 0 直接停手 —— TCG 下所有 `kvm:*` 事件为 0，
   每一档都"看起来零开销"。
10. **AGENTS.md 陷阱 9**：本实验**不下 kprobe**，也不碰 `kprobe_events`；
    清理函数按 `current_tracer` / `set_ftrace_filter` / `set_event` / `tracing_on`
    四项独立清（清一项不够）。
11. **哪些是读源码查出来的、哪些是实测查出来的**：`tracing_on=0` 不是零开销基线
    （`../corrections.md` C17）、tracefs 里根本没有 `enabled_events`（同 `D6`）
    —— 这两条**都来自读 6.12.93 源码，不是本机实测**（本机跑的是 6.8.0-51）。
    凡是"跨版本会变"的结论，preflight 都现场复核一遍，不靠文档兜底。
12. **函数级自测抓到的六处真缺陷**（驱动器写完之后，把每个读数/复读 helper
    喂进冻结输入跑一遍断言，49 项全绿才认可；这些 bug 每一个都会**静默**产出错数）：
    * `scan_trace` 把注释表头也算进 `rec_lines` → O0/O1 的"基线窗内零记录"判据
      永远不成立，两臂会被误判 FAILED。
    * `sched_all` 在某个 vCPU 线程读不到时**继续求和** → Δ 被系统性低估，
      表现为"这一档更便宜"。现在少读到一个就整行 NA（§4.1）。
    * `/proc/<tid>/stat` 用贪婪 `##*) ` 剥 comm → 路径里出现 `") "` 时 stime 截错。
    * `/proc/stat` 的 busy 最初按"总 − idle − iowait"算，把 `$10/$11` 的 guest
      又加了一遍 → 这一列会随 VM 负载假性暴涨（§4.1）。
    * O5 的命中模式写成 `+0x`，而 `sym-offset` 默认关（§4.2）→ 该臂必然零命中。
    * `--out` / `--report-from` 传相对路径时被脚本开头的 `cd "$(dirname "$0")"`
      改到脚本目录下解析 → 重算汇总会莫名其妙报"文件不存在"。现在按调用者 cwd 展开。
13. **O7 的落盘必须有硬上限，且撞顶 = 该臂作废**：`trace_pipe` 的产出 =
    退出率 × 平均行长。行长由 `kvm_exit` 自己的 `TP_printk` 决定 ——
    `vcpu %u reason %s%s%s rip 0x%lx info1 0x%016llx info2 0x%016llx intr_info 0x%08x
    error_code 0x%08x`：两个 `%016llx` 就占 34 B（含前缀），加 reason 名、rip、
    每条记录固定前缀（comm + pid + `[cpu]` + flags + 时间戳，`print_trace_line()`），
    **单条约 200 B 量级**（首轮实测后用 `pipe-O7.txt` 的均长复核这个推算）。
    退出率**只能用 O0 臂自己测出的 `exits/s`**（本仓没有跨负载的现成值，
    `../measurement.md` §8 禁止引用外部数字），两者相乘再乘 `--sample-s` 就是
    需求上限。默认 `PIPE_CAP_MB=256`（常量，**没有命令行开关**）对应
    "20 s × ~6e4 exits/s × ~200 B" 的容量。
    `ulimit -f`（512 B 块）到点由内核发 `SIGXFSZ` 杀掉 `cat`。
    ★ 撞顶不是"数据少一点"—— 读者后半窗**不在场**，消费侧成本（`cons_rt_ns`）
    只覆盖部分窗，O7 − O3 的差值不可比，所以该臂直接判 FAILED。

---

## 7. 结果（待实测）

| 臂 | 观测档 | 吞吐损失% | 额外宿主 CPU-s | 每百万退出 Δsum_exec_runtime(ns) | 退出率漂移% | overrun |
|---|---|---|---|---|---|---|
| O0  | 真零基线 | 0（定义） | 0（定义） | 0（定义） | 0（定义） | 待实测（判据要求 =0） |
| O0e | 真零基线（收尾复跑） | 待实测（漂移，须 < 1/3 × 最小臂间差，绝对量） | 待实测 | 待实测 | 待实测 | 待实测 |
| O1  | 挂着不记 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测（应 =0） |
| O2  | 单事件 kvm:kvm_entry | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| O3  | 单事件 kvm:kvm_exit | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| O4  | kvm:* 整组 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| O5  | function tracer（5 函数） | 待实测 | 待实测 | 待实测（**另报每次命中 ns**，§6.6） | 待实测 | 待实测 |
| O6  | bpftrace 聚合探针 | 待实测（另报 bpftrace 进程 CPU-s） | 待实测 | 待实测 | 待实测 | 待实测（ftrace 侧不记，应 =0） |
| O7  | O3 + 常驻 trace_pipe 读者 | 待实测（另报读者进程 CPU-s） | 待实测 | 待实测 | 待实测 | 待实测（★ 读者在排空缓冲，读 `stats` 时窗已结束，见 §6.13） |

填完后同步三处：① `../measurement.md` §4(b) 表；② E3 §4.2 条件 2 的阈值
（"M2 与 M0 的差值须大于扰动预算"里的"预算"就是这张表的最小非零档）；
③ `../index.md` 的本仓实测数据索引。
