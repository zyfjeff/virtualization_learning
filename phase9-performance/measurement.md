# 性能测量的可信性规范

> 本文只回答一个问题：**"这个性能结论能不能信"**。
> 工具怎么用、命令怎么写、症状怎么排查 —— 全部在
> [`../phase10-debugging/`](../phase10-debugging/README.md)，本文一律不重复。
> 参数默认值与权限见 [parameters.md](parameters.md)，本文不写第二份。

---

## 0. 与 phase10 的分工

| | phase10（调试与测试） | 本文（phase9） |
|---|---|---|
| 回答 | "为什么慢？去哪找？" | "这个改动带来多少？这个数可信吗？" |
| 内容 | trace events 目录、`perf kvm` 用法、症状→命令排查树 | 对照组、统计量、扰动预算、噪声控制、归因纪律 |
| 命令语法 | ✅ 权威 | ❌ 只指向 phase10 |

本文里出现的命令**只用于说明测量纪律**（例如"必须先清残留再采样"），
不作为命令手册；要抄命令去 phase10。

---

## 1. 一个性能结论的最小可辩护形态

**缺任何一项，就不准写进本仓库的任何文档。**

| 要素 | 为什么不可省 |
|---|---|
| ① **被测对象与负载**：VM 配置（vCPU/内存/backing/机型）、guest 内核、负载命令与参数量 | 否则结论无法复现，也无法判断适用范围 |
| ② **对照组**：与谁比。必须说明"除了被研究的那个变量，其它都一样" | 只报单点数字不是结论，是数据 |
| ③ **重复次数与统计量**：跑了几次、报的是中位数/均值/分位数中的哪一个、离散度多少 | KVM 的路径方差极大，一次采样的数字基本没有信息量（§2） |
| ④ **观测手段与被测扰动**：用了哪种观测，它自身开销是多少 | 观测本身会改变结果，扰动未量化则结论无效（§4） |
| ⑤ **版本与环境**：宿主内核、guest 内核、源码基线、KVM 确认（非 TCG） | 本仓已实测到宿主 6.8 与基线 6.12.93 的行为差异（`parameters.md` §0(b)） |

本仓库文档集里出现过的**反面教材**（已清理，保留以说明形态）：旧版
`annotations.md` 的"网络密集 10Gbps 无优化 ~500K VM-Exit/s → 全优化 ~50K，减少
~90%"一张表，五项全无。它不是测量结果，是排版出来的错觉。

---

## 2. 重复与统计纪律

### (a) 为什么必须重复

KVM 的一次操作跨越硬件虚拟化、宿主调度、中断投递，路径长度受**与负载无关的事件**
影响（宿主上的其他进程、IRQ 投递时机、THP 的 khugepaged 扫描、节能状态迁移）。
所以测量分布**强烈右偏**：绝大多数样本很快，少数样本极慢。

右偏分布下：

- **均值被长尾拉高**，不代表典型路径 → 报"典型延迟"用**中位数**；
- **max 才是很多场景真正关心的量**（实时性、超时、SLO），但 max 单点不稳定 →
  报 **P99 / P999**，并单独记 max 与它的次数；
- **样本量小则分位数无意义**：n=3 的"P99"就是 max。

### (b) 最低要求

| 场景 | 最少重复 | 报什么 |
|---|---|---|
| 端到端时间（启动、任务完成） | **10 次** | 中位数 + min/max；本仓 phase8 项目4 M1 就是"4 实现 × 10 次取中位数" |
| 单次操作延迟分布（微基准） | 一次长运行、**报直方图** | P50/P99/P999/max + 样本数 |
| 计数型指标（VM-Exit/s、缺页数） | **3 次**，且固定窗口长度 | 三次值 + 波动幅度；波动 >20% 说明有未控制变量，先查再报 |
| 需要重载模块/重启才改的变量（`parameters.md` §8 右列） | 每次开机算 1 个样本，**≥3 次开机** | 同上，并在报告里注明"每次是独立开机" |

### (c) 顺序与漂移

- **不要 A→B→A→B 交替后直接平均**。前一轮的状态会漏到后一轮（PLE 的 per-vCPU
  窗口、THP 的碎片化程度、页缓存冷热都是这样）。
- 正确做法：**随机化顺序**，或至少 `A×n` 与 `B×n` 之间留**静置期**；
  怀疑有残留状态时**必须重启 VM** 再切下一档。
- 报告里写明顺序。`../phase8-capstone/practice/bench-halt-sweep.sh` 是固定升序扫
  窗口，它的结论形态（"窗口只需 ≥ 典型 halt 长度"）之所以稳，是因为该趋势对
  顺序不敏感；换别的变量不保证。

---

## 3. 噪声控制

宿主是 96 线程裸金属（`uname -r` = 6.8.0-51-generic）。下面几项**必须逐项确认，
不能假设** —— 本机实测已处于适合测量的状态（见"实测状态"列），但这是环境事实而非
默认事实，换一台宿主、换一次内核升级都可能变，且每一项都能独立把延迟分布拉长数倍。
确认后按"改了/没改/改回"记录。

| 源 | 查 | 本机（6.8.0-51）实测状态 |
|---|---|---|
| CPU 频率调节 | `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`；可用档看 `scaling_available_governors` | **已是 `performance`**（该宿主可选档为 `conservative / ondemand / userspace / powersave / performance / schedutil`）。换宿主必须重查：`ondemand`/`schedutil`/`powersave` 下同一段代码跨次跑出倍数级差异 |
| 深度睡眠态 | `cat /sys/devices/system/cpu/cpu*/cpuidle/state*/disable`、`cat /sys/module/intel_idle/parameters/max_cstate`、`/proc/cmdline` | **已限制**：cmdline 带 `intel_idle.max_cstate=1`，参数实读亦为 `1` |
| 绑核 | vCPU 线程 `taskset`；对照"1:1 绑核不超卖" vs "cgroup 限幅超卖" | 迁移本身有代价，不绑核等于把迁移代价混进每一次采样（`annotations.md` §1.4） |
| NUMA | `numactl --cpunodebind --membind` | 跨 NUMA 访存差异可测；单节点宿主可跳过，但要记录跳过理由 |
| IRQ 亲和 | `cat /proc/irq/*/smp_affinity`，测量窗口内观察宿主自身中断 | 宿主网卡/NVMe 中断打到被测核会造成长尾 |
| 旁扰进程 | 测量窗口内停 `updatedb`、备份、容器调度 | 尤其 `kswapd`/`khugepaged` 会污染内存类实验 |

**不要为了"干净"去改宿主全局配置后不恢复**。所有调参脚本必须
`trap` 退出恢复，并把原值落盘 —— 本仓惯例见
`../phase8-capstone/practice/bench-halt.sh` 开头记录 `ORIG=` 再恢复的写法。

---

## 4. 观测手段的分辨率与自身扰动

选错手段的典型后果不是"没数据"，而是**观测行为本身造出了你测到的现象**。

### (a) 分辨率量级

从粗到细，先问"我需要哪一档"：

| 手段 | 分辨率 | 适合回答 | 盲区 |
|---|---|---|---|
| debugfs KVM 统计（`/sys/kernel/debug/kvm/<pid>-<vm>/…`，每项一个文件） | 累计计数 | "有没有发生"、"多少次" | 无时间分布；不知道哪一次慢 |
| `perf kvm stat` | 计数 + 每类耗时分布（采样） | "哪类退出占大头" | 采样偏置；低频事件会被漏 |
| ftrace tracepoint（如 `kvm:kvm_exit`） | 单事件 + 时间戳 | "单次路径时序"、"事件顺序" | 高频事件下缓冲会溢出（§4(c)） |
| ftrace function tracer | 函数级，带时间戳 | "慢在哪个函数" | **自身开销与被测函数同量级时读数失真** |
| kprobe / bpftrace | 可取参数、可聚合直方图 | 分布形态 | 取参见 §4(d)；kprobe 插桩本身有 µs 级开销 |
| guest 内自计时（`rdtsc`/`ktime`） | ns 级 | 端到端真实感知延迟 | 只能测 guest 视角，看不到宿主侧 |

**本仓唯一有延迟直方图设计的现成工具**是
`../examples/bpf-programs/trace-irq-latency.bpf`（中断投递全链路）。
需要"分布"而不是"平均"时优先复用它，别用 function tracer 手动减时间戳。

### (b) 扰动预算（待实测）

> **下表数字全部待实测**。E5（`practice/bench-observer-cost.md`）就是为填这张表
> 设计的：同一负载在 **8 档观测强度**（臂号 **O0–O7**）下测吞吐、宿主 CPU 绝对量与
> 退出计数漂移，共 **9 个采样窗** —— 基线在收尾再跑一遍（**O0e**），只用来判漂移
> （§4.3 条件 2），不参与档间比较。
> 在它跑出来之前，本文**不引用任何外部的"perf 开销约 X%"数字** —— 那是别人的
> 硬件、别人的内核，套到我们这台 96 线程宿主上就是编造。

★ **基线的定义**（本轮从源码查出，见 `corrections.md` C17）：**`tracing_on=0` 不是零开销**。
`tracing_off()` 的 kernel-doc 自己写着 "It does not disable any overhead the tracers
themselves may be causing … simply causes all recording to the ring buffers to fail"
（`kernel/trace/trace.c:1592-1599`）。决定开销的是**有没有 probe 挂在 tracepoint 上**
——注册/注销才翻 static key（`kernel/tracepoint.c:363` 启用 / `:419` 禁用），
`tracing_on` 不在这条路上。所以基线必须是"**没有任何探针挂着**"，而"事件挂着但不记录"
单独成一档（下表 O1）。完整机制链路见 `practice/bench-observer-cost.md` §2.1。

| 臂 | 观测档 | 被测吞吐损失 | 宿主额外 CPU（**CPU-秒绝对量**） | 每百万次退出的 vCPU 线程 ns | 退出计数漂移 | overrun |
|---|---|---|---|---|---|---|
| O0 | **真零基线**：`set_event` 空 + `current_tracer=nop` + 无 bpftrace/perf/kprobe 探针 | 0（定义） | 0（定义） | 0（定义） | 0（定义） | 待实测 |
| O1 | `kvm:kvm_exit` 挂着但 `tracing_on=0`（挂着不记） | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| O2 | 单个 tracepoint 记录中：`kvm:kvm_entry` | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| O3 | 单个 tracepoint 记录中：`kvm:kvm_exit`（`TP_fast_assign` 要读 VMCS） | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| O4 | `kvm:*` 全开（本机 91 个事件） | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| O5 | function tracer + 退出路径 5 个函数 | 待实测 | 待实测 | 待实测（**另报每次命中 ns**，成本不挂在退出上） | 待实测 | 待实测 |
| O6 | bpftrace 聚合探针 `t:kvm:kvm_exit{@c=count();}` | 待实测 | 待实测（记录端在 vCPU 线程、消费端在 bpftrace 进程，分开报） | 待实测 | 待实测 | 待实测（bpftrace 走 perf 侧、不写 ftrace 环，**应为 0**，非 NA） |
| O7 | O3 + 常驻 `cat trace_pipe` 读者 | 待实测 | 待实测（另报读者进程 CPU） | 待实测 | 待实测 | 待实测 |

**为什么第二列是 CPU-秒而不是百分比**：本机 96 线程，观测者的绝对开销可能只有
1~2 CPU-s，除以 96 核看着像 0.2% 一样无关紧要，但那 1~2 秒恰好压在 vCPU 线程所在核上
时吞吐就掉了。所以第二列报绝对量，并附"每百万次退出的 vCPU 线程 ns"
（= `Δsum_exec_runtime / exits × 1e6`，落在正确的线程上、精确到 ns）。

**归一化口径有两个，必须同时看**：吞吐掉了 `exits` 也往往跟着掉，所以
`每百万退出 ns` 要用**本臂自己的** `exits`，同时报"每窗绝对增量"。只用一个口径会得出相反结论。

**在这张表填上之前必须遵守的保守规则**：

1. **能用 debugfs 统计量就不要开 tracepoint**（前者是 `this_cpu_inc` 级别的计数，
   后者每次事件要写 ring buffer）。halt-polling 就是典型：`halt_attempted_poll` /
   `halt_successful_poll` 足够回答"poll 命中率"，不必开 `kvm:kvm_halt_poll_ns`。
2. **能不用 function tracer 就不用**。它给每个命中函数插 `mcount` 调用，而 KVM 缺页
   路径的热点函数本身就在 µs 量级 —— 观测开销与被测量同级时，"实验 1 测缺页延迟"
   这类设计会**测到 tracer 自己**。旧版 `practice/ept-bench.md` 实验 1 正是这个形态。
3. **对照组与实验组必须同一观测档位**。"A 开 trace 跑、B 不开 trace 跑，比较耗时"
   是无效对照。
4. **报出观测档**。每一份数据都要说清当时开着什么 trace，否则后续无法判断可比性。

### (c) ring buffer 溢出：静默丢数据

ftrace 的缓冲是**按 CPU 分配的环形缓冲**，写满后**覆盖旧事件**，不报错、不阻塞。
退出事件很容易在采样窗口内把窗口**开头**的数据冲掉 —— 本仓自己的实测量级：
`../phase8-capstone/practice/README.md` 项目4 M2 里，**单 vCPU** busy 15 s 就记到
约 3.7 万次退出（`EXTERNAL_INTERRUPT` 22415 + `IO_INSTRUCTION` 15004），
boot 窗口 q35 的 `IO_INSTRUCTION` 单项 72247 次。这还只是最小负载；
多 vCPU、中断密集或 EPT 抖动场景的量级本仓**未实测**，需要时按
`practice/bench-observer-cost.md`（E5）自己量，不要照搬别处的"每秒百万次"。

失真的表现是"总数"和"分布"同时偏低，而 `trace` 文件里看起来仍然是满满一页数据，
**没有任何地方会打印错误**。

溢出只体现在 per-CPU 统计文件里：

```
/sys/kernel/tracing/per_cpu/<cpu>/buffer_statistics
  entries: <n>            ← 当前缓冲内事件数
  overrun: <n>            ← 被覆盖的事件数
  commit overrun: <n>     ← 提交阶段就失败的事件数
  bytes: <n>
```

（对应 `kernel/trace/trace.c:8372-8379` 的
`ring_buffer_entries_cpu()` / `ring_buffer_overrun_cpu()` /
`ring_buffer_commit_overrun_cpu()` 三项输出。）

**规则：每轮采样后必须逐 CPU 读一次 `overrun` 与 `commit overrun` 并连同数据存档；
任一非零则该轮作废**（要保留则明确标注"计数偏低"）。降低失真的手段按优先级：
缩短采样窗口 → 提高过滤精度（`set_event_pid`、事件 filter）→ 加大
`buffer_size_kb`（★ **每 CPU** 一份，总量 = 该值 × `nproc`，几十上百线程的机器上调大
它要先想清楚内存）→ 换成能就地聚合的 bpftrace 直方图（不往 ring buffer 倒事件流）。

> 顺带澄清一个常见误读：`buffer_percent`
> （`kernel/trace/trace.c:9726` 里默认置 50）**与丢事件无关**。它只作用于
> `trace_pipe_raw` 的**阻塞读**——缓冲填到该比例才唤醒读者，
> `Documentation/trace/ftrace.rst:183-195` 写得很明确（`0`=有数据即唤醒，
> `50`=约一半子缓冲满，`100`=填满才唤醒）。调它不会减少覆盖丢事件，只会改变
> 读端的阻塞行为。

### (d) kprobe 取参不可信的情况

`AGENTS.md` 已知陷阱 9 已固化，这里只重述与测量相关的部分：
对 `.isra` / `.constprop` / `.part` 后缀符号下 kprobe，**寄存器与源码形参的对应关系
不能照原型推断**，且可能有多个同名地址。必须先在"已知答案的场景"做对照验证
（例：抓 IRTE 时先确认 `SID` 字段等于设备 BDF）再采信。

---

## 5. 开跑前自检（每次，不可跳）

按顺序执行，任一项不通过就**停手**，别往下采样。

1. **确认在 KVM 上而不是 TCG**。`ls -l /proc/<qemu-pid>/fd | grep -c kvm` **>0**；
   为 0 说明静默回退到 TCG，此时宿主侧 `kvm:*` tracepoint 零事件，所有实验都会得出
   "没有 VM-Exit"的错误结论（`AGENTS.md` 陷阱 7；`scripts/README.md` §TCG 亦已记录）。
   这是**最容易犯且报错最安静**的一条，放第一位。
2. **版本一致性**：
   - `uname -r` 记下宿主内核；
   - `ls /sys/module/kvm/parameters/ /sys/module/kvm_intel/parameters/` 确认要用的参数
     **真的存在**；
   - 要用 function tracer 时 `grep -w <fn> /sys/kernel/debug/tracing/available_filter_functions`
     确认函数**没被内联、没改名**。
     本仓的现成事故：6.12.93 里 `lapic_timer_advance_ns` **不是**模块参数
     （只有 `lapic_timer_advance` bool），而宿主 6.8 恰好相反
     （`parameters.md` §3、`../phase7-timer-virt/annotations.md:581`）。
     另一处同类：`kvm_mmu_set_spte` 在 6.12 已不存在，
     `../scripts/trace/trace-page-fault.sh` 的注释里已写明。
3. **清残留**：`echo > trace`、`echo nop > current_tracer`、
   `echo > set_ftrace_filter`、`echo > set_event`，并清 `kprobe_events`。
   **`kprobe_events` 与 `current_tracer`/`set_ftrace_filter` 是独立的**，只清前者会被
   上一轮残留的 `function` tracer 淹没（`AGENTS.md` 陷阱 9）。
   ★ 反过来，`set_event` 上的 **`>` 与 `>>` 不等价**：以写方式打开时只要带 `O_TRUNC`，
   `ftrace_event_set_open()`（`kernel/trace/trace_events.c:2411`）在 `:2422-2423`
   就调 `ftrace_clear_events()`（`:883`）把**全部**已启用事件关掉 —— 连 `tee`（默认
   O_TRUNC）也算。所以 `>` 是"清场"动作，会顺手停掉别人挂的探针；只想加一个事件必须
   用 `>>`。本仓脚本一律 `: > set_event` 显式清场、再逐个 `>>` 挂回。
   ★ **`set_ftrace_filter` 同理，只是机制在另一处**：`ftrace_regex_open()`
   （`kernel/trace/ftrace.c:4536`）在 `O_TRUNC` 分支（`:4579-4581`）从**空** hash 起步、
   而不是拷贝当前 filter；收尾时 `ftrace_regex_release()`（`:6438`）用
   `ftrace_hash_move_and_update_ops()`（`:6478-6479`）把它整体盖回 ops 的 filter_hash。
   所以 `echo fn > set_ftrace_filter` 是"把 filter **换成**只有 fn"，`>>` 才是追加；
   想清成"全部函数都开"要显式 `: > set_ftrace_filter`（空 filter = 不过滤，
   这正好是 `practice/bench-migrate.md` §4.2.1 那条"观测面被悄悄放大"的入口，
   改完要回读确认）。
   ★ **第三个同类文件是 `set_event_pid`**（本轮在 `../../scripts/trace/trace-page-fault.sh`
   上查出来的）：`ftrace_event_set_pid_open()`（`kernel/trace/trace_events.c:2432`）在
   `:2442-2444` 对带 `O_TRUNC` 的写打开调 `ftrace_clear_event_pids(tr, TRACE_PIDS)`，
   一样会先清掉**全部**已有 PID。差别在收尾：`event_pid_write()` 开头
   `:2167-2168` 是 `if (!cnt) return 0;`，所以**纯 `: > set_event_pid` 截断就是干净清空**，
   不会像 `set_event` 那样还要额外收尾。规则同前：清场显式写 `: >`，追加一律 `>>`。
4. **参数权限与原值**：写任何 `/sys/module/*/parameters/*` 前 `stat -c %a` 确认是可写
   （`parameters.md` §0(a)：多数性能参数是 0444，`echo` 会失败）。记录原值，
   `trap ... EXIT` 恢复。
5. **debugfs 已挂载**（`/sys/kernel/debug/tracing` 存在）。
6. **溢出计数清零并准备采样后回读**（§4(c)）。

---

## 6. 归因纪律

### (a) 一次只动一个变量 —— 但 KVM 常有"天然纠缠"的变量对

有些变量**不可能**独立变化，必须显式拆开归因，否则结论张冠李戴。已知两组：

| 纠缠对 | 机理 | 怎么拆 |
|---|---|---|
| **脏页日志 × 大页** | 开脏页跟踪会让新建映射退到 4K（`arch/x86/kvm/mmu/mmu.c:3185-3186` 的 `kvm_slot_dirty_track_enabled()` 早退）。所以"迁移期间变慢"里混着**大页塌陷**和**PML/脏记录成本**两项 | 四格矩阵：`{大页,4K} × {脏日志关,开}`，并单独看 `kvm:kvm_pml_full` 次数。设计见 `practice/bench-huge-dirty.md` |
| **PLE × 超卖** | 不超卖时 PLE 根本没有作用对象（无 CPU 可让），单 vCPU 测不出任何东西 —— `../phase8-capstone/practice/README.md` 项目4 已实测定性 | 必须有"1:1 绑核不超卖"的**阴性对照档**，见 `practice/bench-ple.md` |

旧版 `annotations.md` 把"脏页日志开销"直接写成"每次写入触发 EPT Violation ~500ns"，
正是没拆第一组、还把 PML 前后的机理讲反了（有 PML 时**正常写入不退出**）。

### (b) 必须先做的三件事，才能说"是 X 导致了 Y"

1. **阳性对照**：换一个已知会改变结果的设置，确认测量装置真的能看见变化。
   测不到差异时，先怀疑装置，再下"无影响"的结论。
2. **阴性对照**：只改一个无关变量，确认结果不变。用来排除"任何改动都让数字变一点"
   的系统性漂移（开机顺序、温度、后台任务）。
   本仓范例：phase8 M1 用 `tuned` 阴性对照排除了"cmdline 能替代固件信息"的可能。
3. **机制闭合**：数字变化必须能对应到一条源码/硬件路径。
   "设了 X 之后快了 30%" 而说不出少了哪类退出、少了哪个函数的调用次数 ——
   这不算结论，可能是噪声。用 `perf kvm stat` 的退出分布或 debugfs 计数把差值
   **逐项对账**，残差要说明去向。

### (c) "测不到"不等于"没有"

三种成因要区分，报告时必须写清是哪一种：

- **触发条件没满足**（PLE：没有超卖，就没有"该让谁"的问题；或 guest 走 PV spinlock，
  等待方改用 hypercall 踢醒持锁 vCPU 而不是一直 `PAUSE` 自旋，PLE 收不到信号。
  两种情形的排查与判据见 `practice/bench-ple.md`）；
- **观测手段看不见**（`perf kvm stat -p` 丢 vCPU 线程事件，见 §7）；
- **确实没有收益**（本规模下 VPID 对 VMM 透明，phase8 项目4 的结论）。

---

## 7. 已知陷阱清单（本仓实测踩过）

只列与"结论可信性"直接相关的；完整机制在各所属章节。

| 陷阱 | 后果 | 出处 |
|---|---|---|
| QEMU 缺 `-enable-kvm` **静默**回退 TCG | 宿主 `kvm:*` 零事件 → "没有 VM-Exit"的假结论 | `AGENTS.md` 陷阱 7；检测法见 §5 第 1 条 |
| `perf kvm stat record -p $PID` | 只跟踪被包裹进程，**vCPU 线程退出全丢**。源码判据：`tools/perf/builtin-kvm.c:1959-1960` —— `if (target__none(&kvm->opts.target)) … system_wide = true;`，即**只有不给 target 时才自动 system-wide** | 实测结论见 `../phase8-capstone/practice/README.md` "测量陷阱备忘"；`../phase10-debugging/README.md` 场景 1 已按本文改为 `-a` |
| ftrace `function` tracer 残留 | 下一轮被淹没 | `AGENTS.md` 陷阱 9 |
| **`set_ftrace_filter` 一次写多个名字** | 第一个匹配不上的名字**中止本次 write**，其后的名字连带丢失；它在最前面时 filter 停在"全部函数都开"，统计器仍给出看着合理的数字 → 观测面被悄悄放大到整机而不自知。**必须一个名字一次 `>>` 并逐个检查** | 实测表与机制见 `practice/bench-migrate.md` §4.2.1（`trace_get_user`/`match_records`/`ftrace_regex_write` 三处源码链路） |
| ring buffer 溢出静默丢事件 | 计数偏低而不自知 | §4(c) |
| kprobe 打 `.isra` 符号取参错 | 抓到"看似合理"的错参数 | `AGENTS.md` 陷阱 9 |
| 文档命令跨内核版本照抄 | 参数/函数根本不存在 | §5 第 2 条，`parameters.md` §0(b) |
| **统计量的名字不像它量的是那条路** | `directed_yield_attempted/successful` 看着就是 PLE 的定向让出计数，实际只由 guest hypercall 路径 `kvm_sched_yield()` 递增（`arch/x86/kvm/x86.c:10031,10057`），`kvm_vcpu_on_spin()`（`virt/kvm/kvm_main.c:4037`）一次都不加 → 用**成功**的实验得出"机制没跑通" | `annotations.md` §1.8 |
| guest busybox ash 行缓冲 ≈1024 字节 | flood 类负载中途**静默停摆**，后续全部超时 | `../phase8-capstone/practice/README.md` 测量陷阱备忘（`bench-halt.sh` 限 800 字符） |
| QEMU pipe chardev 只打开已存在的 `<path>.in/.out` | 必须先 `mkfifo`，否则卡住 | 同上 |
| 直通设备寄存器布局动态变化 | 按静态布局做换算 → 读出全 1 / 设备收不到映射 | `AGENTS.md` 陷阱 16 |

**测量期间不得触碰正被驱动着的设备的 BAR、不得对疑似设备裸操作 sysfs**
（`AGENTS.md` 陷阱 15 的挂死事故），性能实验同样适用。

---

## 8. 报告规范

每份实测数据落成四件东西，缺一不可：

1. **原始数据目录**：落在 `practice/bench/<name>-<timestamp>/`，含每轮原始输出、
   当时的参数值、`uname -r`、溢出计数。脚本负责生成，不手工整理。
2. **汇总表**：中位数 + 离散度 + 样本数 + 观测档位。
3. **归因段**：差值对到了哪条源码路径（`annotations.md` 的三块之一，或指向别章），
   残差多少。
4. **失效声明**：阳性/阴性对照是否做了、哪些档没测、已知的不可比之处。

引用别处已实测的数字时，**只写指针不写数字**（数字会随别处重测而漂移），
统一登记在 [index.md](index.md)。

**文档里任何数字只有两种合法来源**：本目录 `bench/` 里的原始数据，或带出处的
`index.md` 条目。凭印象、"业界通常认为"、"教科书上说"一律不得出现。
