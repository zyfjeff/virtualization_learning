# Phase 9 勘误

> 本章旧版（`README.md` / `annotations.md` / `practice/*`）在 2026-09-02 重写时发现的
> 错误。每条给出：错在哪、正确信息、源码或实机依据。
> 核查方法：逐条 grep `/root/code/linux-6.12.93/` + 本机 `/sys/module/*/parameters/`
> 实读交叉验证；行号由 `../phase8-capstone/practice/mini-kvm/check-refs.py` 复核。

## A. 参数值与权限错误

### A1 `halt_poll_ns` 默认值写成 400 µs（实为 200 µs）

- **错处**：旧 `README.md:58`、旧 `annotations.md:17`、`:147`。三处均写
  `默认 400000ns = 400μs`。
- **正确**：`KVM_HALT_POLL_NS_DEFAULT = 200000`
  （`arch/x86/include/asm/kvm_host.h:71`），`halt_poll_ns` 初值取该宏
  （`virt/kvm/kvm_main.c:78`）。本机 `/sys/module/kvm/parameters/halt_poll_ns`
  实读 **200000**。
- **扩散面**：同一个错误值另见于 `../phase0-kvm-framework/annotations.md:946`、
  `../phase0-kvm-framework/kvm-framework.md:560`、
  `../phase0-kvm-framework/README.md:517` 与 `:855`、
  `../notes/source-navigation.md:219`、`../notes/debugging-guide.md:416`、
  `../phase10-debugging/annotations.md:307`。**已全仓一并修正**（共 10 处）。

### A2 `halt_poll_ns` 的模块路径写成 `kvm_intel`

- **错处**：旧 `annotations.md:516`
  `cat /sys/module/kvm_intel/parameters/halt_poll_ns`。
- **正确**：该文件**不存在**。`halt_poll_ns` 由 `virt/kvm/kvm_main.c:79` 的
  `module_param` 注册，属 **`kvm`** 模块 →
  `/sys/module/kvm/parameters/halt_poll_ns`（本机实测可读）。
- **判据**：参数属于哪个模块，看它定义在哪个源文件（`kvm_main.c`/`x86.c`/`lapic.c`/
  `mmu.c` → `kvm`；`vmx/vmx.c` → `kvm_intel`），不看它管什么。

### A3 `ple_window_shrink` 默认值与触发条件都错

- **错处**：旧 `README.md:67` 写默认 2；旧 `annotations.md:257-259` 写
  "当 PAUSE 间隔 > `ple_gap` 时（非自旋）→ `shrink_ple_window()` 缩小窗口"。
- **正确**：
  - 默认 `KVM_DEFAULT_PLE_WINDOW_SHRINK = 0`（`arch/x86/kvm/x86.h:75`），
    本机实读 **0**。
  - `ple_gap` **从不参与** shrink 判断。唯一的 shrink 调用点是
    `vmx_vcpu_load()`（`arch/x86/kvm/vmx/vmx.c:1519-1523`），
    条件 `vcpu->scheduled_out && !kvm_pause_in_guest(vcpu->kvm)`。
  - 且 `shrink < 1` 时 `__shrink_ple_window()`（`x86.h:96-107`）**直接返回 base**
    而不是做除法 —— 默认值 0 的语义是"被抢占即回到全局 `ple_window`"。
- **为什么会错**：把 halt-polling 的"`shrink` 是除数"（`kvm_main.c:3689-3706` 里
  确实有 `val /= shrink`）的模型套到了 PLE 上。两者**同名不同实现**。

### A4 `ple_window_max` 默认值

- **错处**：旧 `README.md:68` 写 16384。
- **正确**：`KVM_VMX_DEFAULT_PLE_WINDOW_MAX = UINT_MAX`（`arch/x86/kvm/x86.h:76`），
  本机实读 `4294967295`。源码注释还写明 *"Default is to compute the maximum so we
  can never overflow"*，AMD 侧是 `KVM_SVM_DEFAULT_PLE_WINDOW_MAX = USHRT_MAX`。

### A5 参数权限未标注，导致"echo 改参数"类命令大面积不可执行

- **错处**：旧 `annotations.md` §6 与各章调优建议普遍隐含"直接 `echo` 写参数"。
- **正确**：`ple_*` 五个参数、`ept`/`eptad`/`pml`/`vpid`/`enable_apicv`/
  `preemption_timer`/`tdp_mmu`/`lapic_timer_advance`/`kvmclock_periodic_sync` 全是
  **0444 只读**，`echo` 会失败；能运行期改的是 `halt_poll_ns*`(0644)、
  `eager_page_split`、`nx_huge_pages*`、`flush_on_reuse`、`tsc_tolerance_ppm`、
  `min_timer_period_us`、`pi_inject_timer`。
  完整对照表见 [parameters.md](parameters.md) §8。

## B. 编造的量化数据

### B1 halt-polling 调优"效果"表（旧 `annotations.md:134-153`）

写有"中断延迟降低 ~50%，CPU 占用增加 ~10-20%"、"CPU 利用率提升 ~5-10%，
中断延迟增加 ~200 μs"等四档。**无任何出处**。

本仓自己的实测结论形态完全不同（`../phase8-capstone/practice/README.md` M3）：
flood 负载下 poll-on 与 poll-off 的 RTT 中位数 **165.9 vs 165.7 µs（无差别）**，
代价是 CPU **74.9% vs 58.7%**；空闲负载上窗口根本不会增长。
即该机制的收益是"延迟↔CPU 的交换"，且在很多负载形态下**延迟收益为零** ——
与"降低 50%"不是一回事。

### B2 VM-Exit 减少效果对比表（旧 `annotations.md:270-280`）

五档"无优化 → 全优化 VM-Exit/s"（~500K→~50K 等）。**无出处，且与本仓实测矛盾**：
phase8 M2 实测单 vCPU busy 15 s 约 3.7 万次退出、boot 窗口 q35 的
`IO_INSTRUCTION` 72247 次，本仓**从未测到 10⁵ 量级/秒的稳态退出率**。已删除。

### B3 "~500 ns / ~0 ns" 系列（旧 `annotations.md:310`、`:332`）

"无 A/D 位每次写 ~500 ns"、"有 A/D 位开销 ~0 ns"、"无 PML 每次写 ~500 ns"、
"有 PML 开销 ~0 ns"。既无出处，机理方向也讲错（见 C4）。已删除。

### B4 自适应收敛序列（旧 `annotations.md:116`）

"典型收敛过程（中断间隔 50 μs）：10 μs → 20 μs → 40 μs → 80 μs → 40 μs → 80 μs …"
是**编造的示例轨迹**。真实增长受 `halt_ns < max` 等条件门控
（`kvm_main.c:3872-3874`），且 phase8 M3 的实测轨迹是
`0→10k→20k→40k→80k 四步、0.7 ms 内完成，此后走 no-op 分支`。

## C. 与源码不符的机制描述

### C1 `sched_out_fn()` / `sched_in_fn()` 是杜撰的函数名

旧 `annotations.md:358-370`。真名是 `kvm_sched_out()`（`virt/kvm/kvm_main.c:6388`）
与 `kvm_sched_in()`（`:6375`），经 `kvm_preempt_ops` 注册（`:6515-6516`）。
`check-refs.py --fn-strict` 能抓到这类错。

### C2 `kvm_vcpu_on_spin()` 写成了伪码

旧 `annotations.md:377-398` 是"（简化）"的伪码，没有真实的 `try = 3`、两轮 `pass`、
`last_boosted_vcpu` round-robin、`preempted`/`ready`/`dy_eligible` 判据。
这些细节恰恰决定了实验为什么可能"测不到收益"，已换成真码 + 逐条含义表
（[annotations.md](annotations.md) §1.4）。

### C3 halt-polling 自适应分支的比较对象错

旧 `annotations.md:99-113` 用 `halt_ns <= vcpu->halt_poll_ns` 等表述，
实际比较的是 `max_halt_poll_ns`（`kvm_vcpu_max_halt_poll_ns()`，`kvm_main.c:3787-3803`
起的分支在 `:3862-3877`）。`../phase0-kvm-framework/annotations.md` §9 的写法是对的，
本章旧版是它的失真副本 —— 这正是重复维护的代价，也是本次把 halt-polling
整体移出本章的直接理由。

### C4 `handle_pml_full()` 并不消费 PML buffer

旧 `annotations.md:341` "handle_pml_full() → KVM 消费 PML buffer → 清空 → 继续"。
实际 `handle_pml_full()`（`arch/x86/kvm/vmx/vmx.c:5962`）只做 NMI blocking 处理并
`return 1`，源码注释明确 *"PML buffer already flushed at beginning of VMEXIT.
Nothing to do here"*。真正的 `vmx_flush_pml_buffer()` 在
**每次** VM-Exit 开头无条件调用（`__vmx_handle_exit()`，`:6451-6452`）。
这个错位会让"PML 成本"与"退出次数"对不上账。正确模型见
[annotations.md](annotations.md) §2.4。

### C5 `kvm_pml_full` 的参数

旧 `annotations.md:348` 写 `参数: vcpu_id, full_count`。
实际 `TP_PROTO(unsigned int vcpu_id)`（`arch/x86/kvm/trace.h:964`），只有一个参数。

### C6 "TSC-deadline 到期由硬件比较、零 VM-Exit"

旧 `annotations.md:480-482`。vLAPIC 的 deadline 到期由**宿主 hrtimer**驱动，
要经 VM-Exit 才能注入 —— `../phase7-timer-virt/practice/` Experiment 3 实测到的
正是这条路径（2 ms deadline 实测 2.003–2.010 ms）。KVM 为此还做了提前量补偿
（`adjust_lapic_timer_advance()`，`arch/x86/kvm/lapic.c:1840`）。

### C7 "EPT A/D 位支持"被列为大页启用条件

旧 `annotations.md:298-301`。`__kvm_mmu_max_mapping_level()`
（`arch/x86/kvm/mmu/mmu.c:3138`）的真实闸门是 `disallow_lpage` 计数
（`mmu.c:781-792`）与 `host_pfn_mapping_level`，与 `eptad` 无关。
同时旧版漏掉了最重要的一条：**开脏页跟踪会让新建映射退到 4K**
（`kvm_mmu_hugepage_adjust()`，`mmu.c:3185-3186`）。

### C8 主时钟启用条件

旧 `annotations.md:438-441` 写"Host TSC 是 invariant（`CPUID.80000007:EDX[8]`）"
即为主时钟条件。真实判据是四条与运算：
`host_tsc_clocksource && vcpus_matched && !backwards_tsc_observed &&
!boot_vcpu_runs_old_kvmclock`（`arch/x86/kvm/x86.c:3034-3036`；另一处等价判据
`:2526-2528`）。invariant 是**宿主硬件能力**，既非充分也非必要。

### C9 "vCPU 迁移时更新 TSC_OFFSET"写成了常规步骤

旧 `annotations.md:416`。`adjust_tsc_offset_host()`（`x86.c:2794`）在
`kvm_arch_vcpu_load()` 里的调用点受 `unlikely(vcpu->arch.tsc_offset_adjustment)`
门控（`x86.c:5007-5011`），该字段只在 suspend 等场景被置。
TSC 一致的宿主上正常迁移**不改 offset**；若迁移后观测到变化，是异常信号。

### C10 VPID / APICv 小节的 VMCS 控制位宏名不实

旧 `annotations.md:174-176` 的 `vmcs_write16(VIRTUAL_PROCESSOR_ID, vmx->vpid02)`、
`:196-199` 的 `VIRTUALIZE_APIC_ACCESSES` / `VIRTUALIZE_INTR_DELIVERY` /
`VIRTUALIZE_EOI_EXIT` 三个宏名与真实写法不符（真实宏见
`arch/x86/kvm/vmx/vmx.h` 的 `SECONDARY_EXEC_*`，且 VPID 的赋值路径与 `vpid02`
的用法被旧版简化错了）。这两节整节删除，机制归
`../phase1-vtx-basics/`（VMCS 字段）与 `../phase4-interrupts/`（APICv/PI）。

### C11 旧 §4 没有任何可观测出口，害得重设计初稿抓错统计量

旧 `annotations.md:373-399`（`kvm_vcpu_on_spin()`）只给了三段注释 + 空函数体
（见 C2），**没有指出任何一条能验证"定向让出到底发生了没有"的观测出口**。
本次写 `practice/bench-ple.md` 的验收判据时，因此顺手取了名字最贴切的
debugfs `directed_yield_attempted` / `directed_yield_successful` —— **错的**：

- 这两个计数器唯一的递增点在 `kvm_sched_yield()`（`arch/x86/kvm/x86.c:10026`，
  `:10031` / `:10057`），调用者是 guest 的 `KVM_HC_KICK_CPU`（`:10102-10108`）
  与 `KVM_HC_SCHED_YIELD`（`:10120-10126`）两条 **hypercall**；
- PLE 那条路的 `kvm_vcpu_on_spin()`（`virt/kvm/kvm_main.c:4037-4100`）
  全程只调 `kvm_vcpu_yield_to()`（`:3938`，`yield_to(task,1)` 在 `:3950`）
  并写 `kvm->last_boosted_vcpu`（`:4083-4085`），**一次统计都不加**。

在没开 PV spinlock 的 guest 上（本仓实验内核正是这种状态，见 C12），
PLE 完全正常触发而这两个计数器恒为 0 —— 用它们验收会把**成功**的实验
判成失败。已改写为 [annotations.md](annotations.md) §1.7/§1.8，
并登记进 [measurement.md](measurement.md) §7 陷阱表。

### C12 E1 的触发前提：本仓 guest 内核里 `nopvspin` 压根不存在

设计 PLE 超卖实验时的初稿前提"guest 必须 `nopvspin` 关掉 PV spinlock，
否则等待方走 MSR 写、退出原因变成 `MSR_WRITE`(32)而非 `PAUSE_INSTRUCTION`(40)"
**两处都不成立**，逐条核实结果：

| 初稿说法 | 实际 | 依据 |
|---|---|---|
| 需要 `nopvspin` 才不走 PV spinlock | 本仓实验内核 `# CONFIG_PARAVIRT_SPINLOCKS is not set`（`scripts/images/kernel.config:297`），PV spinlock 整块没编译，`nopvspin` 也随 `kvm_spinlock_init()` 一起不存在（两者都在 `#if … defined(CONFIG_PARAVIRT_SPINLOCKS)` 内：`arch/x86/kernel/kvm.c:1049-1139`、`kernel/locking/qspinlock.c:569-592`）。传这个参数只会成为未知 cmdline 项 | 已核 |
| 踢锁走 MSR 写，退出号 32 | PV 踢锁是 **hypercall**：`kvm_kick_cpu()` → `kvm_hypercall2(KVM_HC_KICK_CPU,…)`（`arch/x86/kernel/kvm.c:1052-1058`），`kvm_hypercall*` 展开成 `vmcall`（`arch/x86/include/asm/kvm_para.h:22`），退出号是 `EXIT_REASON_VMCALL` = **18**（`arch/x86/include/uapi/asm/vmx.h:47`），不是 32 | 已核 |

**真实机制链**（配置关掉 PV spinlock 时为什么 PLE 照样能触发）：
`native_pv_lock_init()` 见到 hypervisor 就 **使能** `virt_spin_lock_key`
（`arch/x86/kernel/paravirt.c:56-60`），而本来会在启动后期把它重新关掉的
`kvm_spinlock_init()`（`arch/x86/kernel/kvm.c:1090`，其中
`static_branch_disable(&virt_spin_lock_key)` 在 `:1136`）没被编译 →
`queued_spin_lock_slowpath()` 走进 `virt_spin_lock()`
（`kernel/locking/qspinlock.c:324` → `arch/x86/include/asm/qspinlock.h:88-110`），
那是一个 TAS + `cpu_relax()`（= `PAUSE`）的 **CPL0 忙等**循环 → PLE 收得到信号。

**结论与做法**：判据不写在文档里，改成实验脚本的**开跑前置检查**——
读 guest 的 `/boot/config-$(uname -r)` 或直接 `dmesg | grep -i "spinlock"`，
按"PV spinlock 开/关"两种状态分别给出正确的准备动作（关着就不用动；
开着才需要 `nopvspin`，而该参数只在 `CONFIG_PARAVIRT_SPINLOCKS=y` 的内核里存在）。
详见 `practice/bench-ple.md` §2。

### C13 本轮自己写进去的一条假"实测"：`set_ftrace_filter` 不是整体校验

写 E3 §4.2 时我记下一条"本机实测：`echo __schedule > set_ftrace_filter` 报
`Invalid argument`，而 `__schedule` 就在 `available_filter_functions` 里"，
并据此写成"整串一次写是**整体校验**的，一个坏名字废掉全部计数"。**两句都不成立**：

- `__schedule` 根本不在那张表里（`grep -c '^__schedule$' available_filter_functions`
  = **0**）。表里只有 `__schedule_bug`，我用的 `grep -c __schedule` 匹配到了它 ——
  又是一次"前缀匹配当精确匹配"的老错，和 C11 的成因同类。
- 真实规则是本机重测三遍、完全可复现的**逐 token 中止**，不是整体校验：

| 一次 write 的内容（都已先 `echo >` 清空） | 报错 | 实际装入 filter |
|---|---|---|
| `"good1 good2"` | 否 | 两个都在 |
| `"good1 bad good2"` | 是 | **只有 good1**，good2 连带丢失（已装入的不回滚） |
| `"bad good1"` | 是 | **一个都没有**，filter 停在 `#### all functions enabled ####` |

源码链：`trace_get_user()` 每次只取一个空白分隔 token
（`kernel/trace/trace.c:1790`）→ `match_records()` 用一个 glob 匹配全量记录、
没命中返回 0（`kernel/trace/ftrace.c:4849-4856`）→ `ftrace_process_regex()`
把 0 变成 `-EINVAL`（`:5682-5684`）→ `ftrace_regex_write()` 见负值 `goto out`
（`:5737-5738`），本次 write 剩余字节不再解析。

**真正该防的是第三行**：空 filter 在 `hash_contains_ip()` 里是**恒匹配**
（`:1513`），统计器于是把整机所有函数都算进去，而我们关心的名字恰好也在"全部"里
→ 打印出的 Hit 看着完全正常，实验已经废了。所以**不能只看返回值**。

顺带两条实测到的拒收原因（都不是"表里有但不接受"）：
`echo function > set_ftrace_filter` 本身无效（`function` 是 `current_tracer` 的值，
不是 filter 的合法内容），而 `bench-ple.sh` 原先正是把它写在批量 filter 的**第一行**；
另外 `available_filter_functions` 把模块函数印成 `kvm_vcpu_on_spin [kvm]`，
照抄整串会让 `[kvm]` 当作第二个名字而报错 —— 判"可跟踪"要用
`^名字( |\[|$)`，`grep -x` 会假阴性（这条 `bench-huge-dirty.md` §6.9 早已写明）。

**处置**：三个脚本的 `start_profile` 全部改成"一个名字一次 `>>` + 收集并打印被拒的
名字"；`stop_profile` 给没命中的名字显式补 `0`，把"没装进去"和"装了但零命中"分开；
删掉 `bench-ple.sh` 里那行 `echo function`。完整写法与实测表放在
`practice/bench-migrate.md` §4.2.1（唯一一份），E2 §4.2 与 `../measurement.md` §7
只留指针。换行分隔的同名串实测行为不同（坏行后面的好行仍装入），机制未追到，
文档里明确写了"别当契约"。

### C14 `kvm:kvm_vcpu_wakeup` 事件数被当成了"有没有阻塞"的判据

E3 §4.2 条件 4 初稿写"每臂 `kvm:kvm_vcpu_wakeup` 事件数与 `halt_exits` 同量级偏小
→ 说明 vCPU 基本没进阻塞路径"。**这条 tracepoint 数量上根本不承担这个判据**：
它在 `kvm_vcpu_halt()` 结尾**无条件**执行（`virt/kvm/kvm_main.c:3880`
`trace_kvm_vcpu_wakeup(halt_ns, waited, vcpu_valid_wakeup(vcpu))`），
轮询成功也发一条，所以**总数 ≈ halt 次数**。区分睡没睡过的是第二个字段
`waited`（`:3837` `waited = kvm_vcpu_block(vcpu)`），打印成 `wait`/`poll`
（`include/trace/events/kvm.h:58-62`，本机 6.8 的 `format` 文件与此一致）。

同一句里还有两处硬伤：PI 常量名写成 `POSTED_WAKEUP`（实为
`POSTED_INTR_WAKEUP_VECTOR`），且把 wakeup 链表分支指到了 `posted_intr.c:74`
—— `:74` 是**快路径守卫**（`nv != WAKEUP && vcpu->cpu == cpu`），
链表分支在 `:91`。

**处置**：条件 4 改为"`wait` 一侧 ≈ 0"，脚本 `collect_events` 拆出
`wakeup_total` / `wakeup_wait` / `wakeup_poll` 三个数并在 `wait` 非 0 时告警。

### C15 E4 初稿把观测窗开在 VM 启动**之后**，等于把 A1 的基线注定测成 0

E4 初稿（`bench-clock-master.sh` 第一版）的顺序是 `boot_vm → sleep → open_win → sleep →
close_win`，判据是"窗内 VM1 的 tid 上有 `masterclock 1`"。**这条窗里天然一条都不会有**：
`kvm_update_master_clock` 只在六个重算入口上产生（`../annotations.md` §3.1.1 的表：
直接重算的 `arch/x86/kvm/x86.c:12844`、`:7024`、`:9504`，与只发
`KVM_REQ_MASTERCLOCK_UPDATE`、等 vCPU 下次进 guest 才消费的 `:2714`、`:2354-2361`、
`:9643`（请求点在 `:9652`）），VM 跑起来之后就静止，不周期性打点。

后果比"读不到数"更糟：A1 会被判成"基线不成立"，而 A3 的判据是**零事件** ——
一个注定为 0 的窗口里读到 0，P2 既没被证实也没被否证，整条实验自洽地空转。

**处置**：所有臂的窗改为**开在动作之前、关在动作之后**（A1 跨 boot、A2 跨切换、
A4 跨 VM2 创建，A5 另开 boot / off-edge / 断言三个窗），并把这条写成
`practice/bench-clock-master.md` §6.8 与脚本头部注释。§4.2 第三条对照（A3 与 A1
窗长相同）正是为了保住这个不对称：A1 能拿到非零，A3 的零才算判据。

### C16 同一条 trace 行里 `offsetmatched` 与 `masterclock` 在两个事件中都不是同一个东西

旧 §5.1 只列了三个事件名（`kvm_update_master_clock` / `kvm_track_tsc` /
`kvm_pvclock_update`），没说任何字段语义，足以让读 trace 的人按字面读两个标签：

- **`offsetmatched`**：在 `kvm_update_master_clock` 里是**布尔**
  （`arch/x86/kvm/x86.c:3042` 传的是 `vcpus_matched`）；在 `kvm_track_tsc` 里是
  **计数** `ka->nr_vcpus_matched_tsc`（`:2540-2542`），**不含基准 vCPU**，
  正确读法是 `offsetmatched + 1 == nr_online`（源码注释就在 `:2521-2528`）。
- **`masterclock`**：`kvm_update_master_clock` 打的是**本次重算的新决定**
  （`ka->use_master_clock` 在 `:3034` 赋值、`:3042` 打印，同一函数体相邻）；
  `kvm_track_tsc` 打的是**翻转前的旧值** —— `kvm_track_tsc_matching()` 只算出一个
  局部变量（`:2526`）并发 `KVM_REQ_MASTERCLOCK_UPDATE`（`:2537-2538`），
  而该字段全树只在 `:3034` 被写。
- **`hostclock`**：符号表只映射 `NONE → "none"`、`TSC → "tsc"`（`trace.h:902-905`），
  切到 hpet 后印的是 `hostclock none`，含义是"不是 TSC 基"，**不是"没有时钟源"**。

**处置**：`practice/bench-clock-master.md` §2.5 逐字段列表并规定"翻边证据只取自
`kvm_update_master_clock`，`kvm_track_tsc` 只用来核对 vCPU 匹配条件"；
脚本 `track_ok()` 按 `+1==nr_online` 判，A2/A3 的 grep 全部锁在
`kvm_update_master_clock:` 之后。与 C11 / C13 / C14 同族：**事件的标签不像它量的是那个东西**。

### C17 扰动预算表把基线定义成"仅 `tracing_on=0`"，而它**不是零开销**

- **错处**：`measurement.md` §4(b) 初稿第一行 —— `nop`（基线，仅 `tracing_on=0`），
  并在同一行把"退出计数漂移"直接写成"0（定义）"。
- **正确**：`tracing_off()` 的 kernel-doc 自己就写明了这一句
  （`kernel/trace/trace.c:1592-1599`）：

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
  1. `echo 0 > tracing_on` → `tracer_tracing_off()`（`kernel/trace/trace.c:1575-1581`）
     → `ring_buffer_record_off()`。**只动了 ring buffer 的记录开关。**
  2. 翻 static key 的是**事件的注册/注销**：开一个事件 `TRACE_REG_REGISTER` →
     `tracepoint_probe_register()`（`kernel/trace/trace_events.c:681`）→ 加第一个 func 时
     `static_key_enable(&tp->key)`（`kernel/tracepoint.c:363`）；移除最后一个 func
     才 `static_key_disable()`（`:419`）。`tracing_on` 不在这条路上。
  3. 于是"事件挂着但 `tracing_on=0`"时每次事件仍然完整走一遍 probe：
     `do_trace_event_raw_event_##call()`（`include/trace/trace_events.h:382-408`）——
     trigger 判断 `:392`、算变长字段偏移 `:395`、`trace_event_buffer_reserve()` `:397`
     （内部 `tracing_gen_ctx_dec()`，`kernel/trace/trace_events.c:657`）——
     直到 `ring_buffer_lock_reserve()` 里 `preempt_disable_notrace()`
     （`kernel/trace/ring_buffer.c:4549`）后在 `record_disabled` 上早退（`:4551-4552`）。
     省掉的只有 `if (!entry) return;`（`include/trace/trace_events.h:400-401`）跳过的
     `TP_fast_assign` 与 `trace_event_buffer_commit()`。

**为什么这条不是抠字眼**：这张表是"每一档 trace 值不值得开"的唯一依据，而它的第一行
本来说的是"什么都不花的起点"。基线含着一笔静默成本，则**所有档位的损失都被系统性低估**，
且 O1（挂着不记）恰好是最容易被误当成"我没在 trace"的部署状态 ——
E1–E4 每一份数据都要按 `bench-observer-cost.md` §5.4 标注档位，标成"基线"其实是这一档。

**处置**：`measurement.md` §4(b) 基线改为 **O0 = 没有任何探针挂着**（`set_event` 空 +
`current_tracer=nop` + 无 bpftrace/perf/kprobe），"事件挂着但 `tracing_on=0`"独立成
**O1** 臂专门量这笔钱；表头同时把"宿主额外 CPU%"改成 **CPU-秒绝对量**并补
"每百万次退出的 vCPU 线程 ns"与 overrun 两列（与 E5 §4.1 / §7 的口径一致）。
E5 的 `apply_arm O1` + §4.2 生效自证按这个定义实现。

---

## D. 命令在 6.12.93 上不可执行

### D1 `lapic_timer_advance_ns` 不是模块参数

- **错处**：旧 `practice/timer-bench.md:120-121`
  `ls /sys/module/kvm/parameters/ | grep lapic_timer_advance` /
  `cat /sys/module/kvm/parameters/lapic_timer_advance_ns`。
- **正确**：6.12.93 只有 `lapic_timer_advance`（`bool`，`arch/x86/kvm/lapic.c:70-71`，
  0444 只读）；`lapic_timer_advance_ns` 是 **per-vCPU debugfs 只读文件**
  （`arch/x86/kvm/debugfs.c:67`）。**手动设固定提前量在 6.12.93 做不到。**
- **这是与 phase7 的直接冲突**：`../phase7-timer-virt/annotations.md:581`
  早已写明"★ 不存在 `lapic_timer_advance_ns` 模块参数"，
  `../phase7-timer-virt/README.md:2388` 还专门列了"常见误解：试图通过
  `echo N > /sys/module/kvm/parameters/lapic_timer_advance_ns` 调优"。
- **根因**：宿主跑 6.8.0-51，那里恰好相反 —— 有 `lapic_timer_advance_ns` 参数
  （本机实读 `-1`）、无 `lapic_timer_advance` bool（本机该路径不存在）。
  照本机命令抄进 6.12.93 文档必错。已在 [measurement.md](measurement.md) §5 第 2 条
  立"参数实存性自检"规则。

### D2 `kvm:kvm_page_fault` 不携带 `level`

- **错处**：旧 `practice/ept-bench.md:85-86`
  "`kvm:kvm_page_fault` 事件携带 `level` 相关上下文，可从输出确认实际建立的映射级别"。
- **正确**：`TRACE_EVENT(kvm_page_fault, ...)`（`arch/x86/kvm/trace.h:402`）的字段是
  `vcpu_id`、`guest_rip`、`fault_address`、`error_code`
  （`TP_printk` 见 `:420`），**没有任何级别/大页字段**。
  确认实际映射级别要走 `/proc/<qemu-pid>/smaps` 的 `AnonHugePages`，
  或对比缺页次数与访问量。已在 `practice/bench-huge-dirty.md` 换成可执行判据。

### D3 `cat /sys/kernel/debug/kvm/*/stats` 不是有效命令（跨章）

6.12.93 的 KVM 统计**每个统计项一个文本文件**，由
`debugfs_create_file(pdesc->name, …)` 逐项注册（`virt/kvm/kvm_main.c:6352`、`:6363`；
per-vCPU 侧 `arch/x86/kvm/debugfs.c:60`）。本机
`ls /sys/kernel/debug/kvm/` 实得 `blocking`、`directed_yield_attempted`、
`directed_yield_successful` … 每项一文件。
`../phase10-debugging/annotations.md` 与 `../phase10-debugging/README.md:134` 有同类
写法，已列入跨章修正。

### D4 `ftrace` 缓冲溢出机制描述（本次写作自查）

`measurement.md` 初稿把 `buffer_percent` 说成"缓冲写满一半就开始丢/覆盖"。**不对**：
它是 `trace_pipe_raw` 阻塞读的唤醒阈值
（`kernel/trace/trace.c:9726` 默认 50；
`Documentation/trace/ftrace.rst:183-195`），与丢事件无关。覆盖丢事件只体现在
per-CPU `buffer_statistics` 的 `overrun` / `commit overrun`
（`kernel/trace/trace.c:8372-8379`）。已改正。

### D5 `measurement.md` 初稿写下一句无出处的"退出率 10⁵–10⁶ 次/秒"

自查删除，改用本仓 phase8 M2 的实测量级（约 3.7 万次/15 s，单 vCPU busy）。
**制定"禁止无出处数字"规则的第一次违反就是我自己**，故记录在此。
另初稿把 cpufreq governor 写成 `powersched`（不存在该档），本机
`scaling_available_governors` 实为
`conservative ondemand userspace powersave performance schedutil`。

### D6 tracefs 里**不存在** `enabled_events`（E5 初稿当作残留探针出口）

- **错处**：`practice/bench-observer-cost.md` 初稿 §2.5/§2.6/§4.2/§6 四处把
  `cat /sys/kernel/tracing/enabled_events` 写成"判断有没有别人在 trace"的出口之一，
  脚本初稿也据此在 preflight 里读它。
- **正确**：这个名字在 6.12.93 里查无实据 ——
  `grep -rn "enabled_events" kernel/trace/ include/linux/ Documentation/trace/` **零命中**；
  本机 6.8.0-51 的 `/sys/kernel/tracing/` 下与"已启用"相关的只有
  `enabled_functions`（`trace_create_file("enabled_functions", TRACE_MODE_READ, …)`
  `kernel/trace/ftrace.c:6986`）与 `function_profile_enabled`（`:6980`）。
  也就是说这是**我凭 `enabled_functions` 类推出来的假文件名**，与 D5 里那个不存在的
  governor 档同类。
- **替代判据（preflight 实际在用的三条）**：
  1. ftrace 事件侧 → `cat set_event` 复读。**只列已启用的事件**，一行一个
     `system:name`，没有 `[+1]` 前缀也没有 `#` 注释头
     （`t_show()` `kernel/trace/trace_events.c:1445-1453` 打 `"%s:%s\n"`；
     `s_next()` `:1413-1423` 只在 `:1421` 往下走带 `EVENT_FILE_FL_ENABLED` 的 file；
     ops 注册 `:2251-2256`）。★ 由此 E5 初稿写在 §4.2 的"`set_event` 里那行为 `[+1]`"
     也是同一族的错 —— `[+1]` 是 `events/<sys>/<ev>/enable` 里**没有**的东西，
     复读判据只能是"整行等值匹配"。
  2. ftrace 函数侧 → `enabled_functions`，列的是**所有挂了 ops 的函数**
     （含 kprobe / ftrace ops / bpf trampoline），本机常态就有 6 个。
     所以**不能"非零即停手"**，只能判"我们要用的那 5 个退出路径函数是否已在里面"
     （在 → O5 的命中数不是本负载造成的 → 停手）。
  3. bpf/perf 侧 → `bpftool -jp prog` 数 tracepoint 型程序 + `bpftool link list` 取**挂点名**
     + `pgrep -x bpftrace` / `pgrep -x perf`。
     ★ 第三条必须看**挂在哪个 tracepoint** 而不是只看有没有：外部 BPF 只要落在 `kvm` 组里，
     O0"无探针"的前提就当场作废（static key 是开着的，见 C17），这是致命项；
     落在 `sched:sched_process_*` 之类的别组只影响 O6 的归因，warn 即可。
     本机实测为后者：`tp_sched_process_fork` / `_exec` / `_exit` 三个。

**处置**：md 四处 `enabled_events` 全部换成上面三条；`[+1]` / "统计 `[+]` 个数"两处判据
改成逐行等值复读与逐事件 `enable` 文件计数（脚本里的 helper `event_on` / `filter_add`
已是这个实现 —— ★ 本文档集约定：`foo()` 这种带空括号的写法只留给内核函数，
shell helper 一律不带括号）。
新增函数级自测：`traceable` 对缺失文件判不可用、`event_on` 不被前后缀子串骗过、
`filter_add` 靠逐名复读而非计数（空 filter 在 `hash_contains_ip()` `kernel/trace/ftrace.c:1513`
里是恒匹配，数个数会被"一个都没装成 + filter 空"骗过）。

### D7 函数 tracer 的行里**没有** `+0x`（E5 的 O5 命中数判据初稿）

- **错处**：`practice/bench-observer-cost.md` 初稿 §4.2/§6.6 把 O5 的命中数写成
  "该臂 `trace` 里 `+0x` 形式的函数行数"，脚本也照抄成 `pat="+0x"`。
- **正确**：`+0x偏移/大小` 这个后缀**默认不印**。`TRACE_DEFAULT_FLAGS`
  （`kernel/trace/trace.c:479-486`）里没有 `TRACE_ITER_SYM_OFFSET` —— 该位只出现在
  符号输出选项掩码 `TRACE_ITER_SYM_MASK`（`kernel/trace/trace.h:1369`）里，
  **不在默认集**；本机 6.8.0-51 实测 `options/sym-offset` 读回 `0`、
  `trace_options` 打印 `nosym-offset`。
  于是 `trace_seq_print_sym()`（`kernel/trace/trace_output.c:364-383`）走 `:374`
  的 `kallsyms_lookup()` 分支（只回名字）而不是 `:372` 的 `sprint_symbol()`
  （`kernel/kallsyms.c:485-489` → `__sprint_symbol()` `:438`，追加在 `:455` `"+%#lx/%#lx"`），
  函数行是**裸函数名**。照初稿写，O5 必然"零命中"→ 该臂永远判 FAILED，
  而失败原因看起来像"filter 没生效"。
- **替代判据**：这一档 events 全关 + 窗首清过缓冲 ⇒ 函数记录是缓冲里**唯一**的写者，
  命中数 = `rec_lines`（全部非表头行）。再挂一条量级核对：`ev_lines / exits ≥ 3`
  （每次退出必中 3 个函数），明显更大说明缓冲里混进了别的东西
  （`TRACE_ITER_PRINTK` 默认开着，驱动里的 `trace_printk()` 会进同一块缓冲）。
- **反向陷阱**：为了拿 `+0x` 而 `echo sym-offset > options` 不解决问题 —— 查找本身
  两边都是 `kallsyms_lookup*`，多的是 `:455` 那次 `"+%#lx/%#lx"` 格式化，量级微不足道；
  真正的问题是它是**全局** `trace_options`，会把同一时刻别的读者的输出格式一起改掉，
  而本实验正是要把记录端与消费端分开算账（§2.5），不该为了格式化方便动全局开关。

**处置**：md §4.2 O5 行与 §6.6 改为"命中数 = 全部非表头行数"；脚本 `pat=""` +
`n_ev=$n_rec`（带注释说明为什么不能用 `+0x`）。

### D8 `/proc/stat` 的 busy 用"总 − idle − iowait"会**把 guest 算两遍**

- **错处**：E5 初稿 §4.1 的"宿主总 CPU"一行写成
  `busy = total − idle − iowait`，等价于把第 10/11 列 `guest`/`guest_nice` 也加进 busy。
- **正确**：guest 时间**已经折进 user/nice**。`account_guest_time()`
  （`kernel/sched/cputime.c:143-159`）在 `:150` 把这段 cputime 加到 `p->utime`，
  又同时记两处 cpustat：`:154-158` 走 `task_group_account_field(CPUTIME_USER/NICE)`
  **和** `cpustat[CPUTIME_GUEST*/GUEST_NICE]`。tick 路径
  `irqtime_account_process_tick()`（`:406`）与 VTIME 路径 `vtime_account_guest()`
  （`:690`）都调它，两种配置下结论一致。`/proc/stat` 首行的字段顺序是
  `user nice system idle iowait irq softirq steal guest guest_nice`
  （`fs/proc/stat.c:128-137`，`$10`/`$11` 就是那两个 guest）。
- **为什么在本实验里致命**：guest 就是被测 VM 的 CPU，而它**恰好随观测档位变化**
  （档位越重、退出越多、宿主侧占比越高、guest 侧越低）。把 guest 重复计入 busy，
  这一列会朝与真实开销**相反**的方向动 —— 看起来"更省的档反而更费宿主 CPU"。
- **正确算法**：`busy = $2+$3+$4+$7+$8+$9`（user nice system irq softirq steal）、
  `idle = $5+$6`（idle iowait）。

**处置**：md §4.1 该行按上式重写并补出处；脚本 `cpu_busy_raw` 已是这个实现，
另加函数级自测：喂冻结的 `/proc/stat`（guest 字段非零）验证 busy 不含 guest、
且"错法"会多算出恰好等于 guest 的量。

### D9 别处**抄了一份参数表**，抄出的四个值全错（跨章）

- **错处**：`../phase10-debugging/annotations.md` §3.1 有一张 kvm / kvm_intel 参数表，
  是本文 [parameters.md](parameters.md) 的副本，且副本已经过期：
  `halt_poll_ns` 写成"默认 400000ns"（真值 200000，`arch/x86/include/asm/kvm_host.h:71`）、
  `nx_huge_pages` 标成 `[bool] 默认 1`（它是 `module_param_cb`，
  `arch/x86/kvm/mmu/mmu.c:87`，只收 `off/force/auto/never`，解析在 `:7259`、
  分支在 `:7268-7284`）、列了一个**不存在**的 `lapic_timer_advance_ns` 模块参数
  （= D1）、`ple_window_shrink` 2 与 `ple_window_max` 16384（= A3 与 A4）。
  同类副本还有两处：`../notes/source-navigation.md` 的"模块参数速查"
  （`halt_poll_ns=400000`、`nx_huge_pages=1`）、`../notes/debugging-guide.md` §4
  （`echo 1 > /sys/module/kvm_intel/parameters/vpid` —— `vpid` 是
  `module_param_named(vpid, enable_vpid, bool, 0444)`，`arch/x86/kvm/vmx/vmx.c:90`，
  运行时写必 `EPERM`）。
- **根因**：默认值与权限被抄了第二份、第三份。抄本不会随重测与换内核更新。

**处置**：三处副本一律**删掉默认值**，只保留调试者真正要问的两件事 ——
**这个参数存不存在**、**运行时能不能改**（列 `stat -c %a` 的判据 + 逐参数的
`module_param` 定义行），默认值统一指向 [parameters.md](parameters.md)。
`notes/debugging-guide.md` §4 另外修了一条独立错误：`taskset -p $QEMU_PID` 改的是
QEMU **主线程**，vCPU 是各自线程（comm 形如 `CPU 0/KVM`），必须按 TID 绑。
预防条款写在 [measurement.md](measurement.md) §5 第 2 条（参数实存性自检）。

### D10 `set_event` 上的 `>`（以及默认的 `tee`）会**静默关掉别人挂的所有事件**

- **错处**：`../examples/bpf-programs/README.md` 三处、`../phase10-debugging/README.md`
  四场景各一处，都写成 `echo evt > .../set_event` 或 `echo evt | sudo tee .../set_event`。
  ★ 登记之后又查出三处同类（本轮一并改掉）：**可执行脚本**
  `../examples/bpf-programs/run-trace-vmexit.sh`（启用一处 + "等效 ftrace"提示五处）、
  `../phase10-debugging/annotations.md`（§1.1 / §1.3 / §1.5 各一条"命令"，§4.3 与 §4.6
  各是事件序列的第一条）、`../phase6-vfio/README.md` 练习 2 第一条。
- **正确**：以写方式打开 `set_event` 时只要带 `O_TRUNC`，
  `ftrace_event_set_open()`（`kernel/trace/trace_events.c:2411`）就在 `:2422-2423`
  调 `ftrace_clear_events()`（`:883`）把**全部**已启用事件清零，然后才处理本次写入。
  而 `tee` 不带 `-a` 时正是 `O_WRONLY|O_CREAT|O_TRUNC` —— 所以管道写法一样会清场。
  ★ **`set_ftrace_filter` 是同一类，机制在另一个文件**：`ftrace_regex_open()`
  （`kernel/trace/ftrace.c:4536`）的 `O_TRUNC` 分支（`:4579-4581`）从**空** hash 起步、
  不拷贝当前 filter，收尾 `ftrace_regex_release()`（`:6438`）用
  `ftrace_hash_move_and_update_ops()`（`:6478-6479`）整体盖回 ops 的 filter_hash。
  所以那里 `>` 是"把 filter **换成**只有这一个名字"，`>>` 才是追加。
- **为什么要登记**：这条不是"命令报错"，而是**命令成功但副作用越界**：
  同宿主上别人（或同一轮实验的上一臂）挂着的探针被悄悄停掉，
  对方下一轮采到 0 事件还以为是负载问题。共享宿主上这是可观测性事故。

**处置**：所有"只想加一个事件"的写法一律 `>>`（`tee` 加 `-a`）；需要清场时**显式**写
`: > set_event`，让动作意图落在纸面上。本章 5 个 bench 脚本本来就是
"`: > set_event` 清场 → 逐个 `>>` 挂回"，与此一致。规则的唯一来源写在
[measurement.md](measurement.md) §5 第 3 条（现已扩成两个 ★：`set_event` 与
`set_ftrace_filter` 各一条），其它章节只给指针。

### D11 `kvm_exit` 的字段与输出格式被写错：没有 `exit_reason_full`，trace 文本里的 reason 是**符号名**

- **错处**（五处，本轮全部改掉）：
  - `../phase10-debugging/annotations.md` §1.1 把参数写成
    "vcpu_id, rip, exit_reason (数字), exit_reason_full (字符串)"；§1.6 的
    `kvm_nested_vmexit` 同样列了 `exit_reason_full` 与 `l1_rsp`；§6.5 那条
    bpftrace 一行命令直接聚合 `args->exit_reason_full` —— **这条跑不起来**，
    bpftrace 找不到该字段。
  - `../examples/bpf-programs/run-trace-vmexit.sh` 的 ftrace 模式汇总用
    `grep -oP 'reason=\K[0-9]+'` 抽原因码 —— **永远抽不到**，所以"汇总统计"
    恒为空表，看着像"没有退出"。
  - 同一脚本的 `EXIT_REASON_NAMES` 表八个码是错的：`EPT_VIOLATION`=24、
    `EPT_MISCONFIG`=25、`PREEMPT_TIMER`=28、`APIC_WRITE`=32、`PML_FULL`=38、
    `XRSTORS`=40、`UMWAIT`=43、`TPAUSE`=44；`EXTERNAL_IRQ`/`INT_WINDOW` 也不是
    内核用的名字。BCC 模式打出来的名字因此整体错位（24 其实是 `VMRESUME`、
    40 其实是 `PAUSE_INSTRUCTION`）。
  - ★ **验收时才扫出来的第四处**：`../examples/bpf-programs/README.md:134` 写
    "追踪点: `kvm:kvm_exit` (reason=24)" 标注在 `handle_ept_violation()` 下面 ——
    两个错叠在一起：`reason=24` 这种形式在 trace 文本里不存在（见下"正确"），
    而 24 也不是 EPT violation（`EPT_VIOLATION`=48，24 是 `VMRESUME`）。
    同一行那条 `handle_ept_violation → kvm_mmu_page_fault → kvm_tdp_page_fault`
    触发链跳过了中间两跳，已按源码补全。
  - ★ **第五处**：`../phase4-interrupts/practice/ex5-on-sn-observe.sh:91` 用
    `grep "kvm_exit" | grep -c "exit_reason=1"` 数外部中断退出 —— 恒为 0，于是
    "外部中断 Exit: 0 次"成了必然结论，看着像 APICv 把中断全吃掉、没有一次
    外部中断退出。已改为 `grep -c "reason EXTERNAL_INTERRUPT"`。
- **正确**（6.12.93）：`TRACE_EVENT_KVM_EXIT` 的字段是 `exit_reason, guest_rip, isa,
  info1, info2, intr_info, error_code, vcpu_id`（`arch/x86/kvm/trace.h:297-331`，
  字段 `:303-310`）；全树 grep `exit_reason_full` **零命中**。`TP_printk` 是
  `reason %s`（`:325-330`），字符串由 `kvm_print_exit_reason()`（`:289-295`）先
  `exit_reason & 0xffff` 查 `VMX_EXIT_REASONS`（`arch/x86/include/uapi/asm/vmx.h:96-158`）、
  再用 `__print_flags()` 附高位标志，而 `VMX_EXIT_REASON_FLAGS` 里**只有**
  `FAILED_VMENTRY`（`:160-161`），其余高位（如 `VMX_EXIT_REASONS_SGX_ENCLAVE_MODE`
  `:30`）按 `trace_print_flags_seq()`（`kernel/trace/trace_output.c:65`）的行为打成十六进制。
  `kvm_nested_vmexit` 是 `TRACE_EVENT_KVM_EXIT(kvm_nested_vmexit)`（`trace.h:679`），
  字段与 `kvm_exit` **完全相同**。原因码取值一律以 `vmx.h:32-95` 为准。
- **为什么要登记**：这条同时踩两种坑 —— 命令**跑不通**（bpftrace 字段不存在）与
  命令**跑通但输出恒空/错位**（grep 抓不到、名字映射错），后者比报错更危险：
  一张"Top 10 退出原因"表看着很专业，数字全对不上号。

**处置**：三处全部按 `trace.h` + `vmx.h` 重写；脚本里的映射表改成整表抄自
`vmx.h:32-95`（与内核字符串表同名，输出可与 trace 文本直接对照），并按内核
`__print_flags` 的语义处理高位标志（宏在 `include/trace/stages/stage3_trace_output.h:67-72`
→ `trace_print_flags_seq()`，`kernel/trace/trace_output.c:65`：命中位打名字并清位 `:74-87`、
剩余位打 `0x%lx` `:89-94`）。脚本里的 `get_exit_name` 与 trace 文本抽取管线都已离线
自测通过（7 个用例 + 合成 trace 行）。顺带修掉同脚本里三处旧的错误触发链
（`vmx_handle_exit()` → 实为 `vmx_vcpu_run()`，见 F 节 VM-Exit 行）与一个死变量。

**本轮自己踩的一次**：写这条勘误时我凭记忆把内核函数名写成 `ftrace_print_flags_seq`
（6.12.93 里没有这个名字，真名 `trace_print_flags_seq()`），是
`../phase8-capstone/practice/mini-kvm/check-refs.py` 的函数名核对抓出来的。教训：**不要
从宏名反推函数名**（`__print_flags` → `ftrace_print_flags_seq` 看着很合理），链路里的每
一跳都 grep 确认过再写。

### D12 `../phase6-vfio/README.md` 练习 2 的三条事件名在内核里**都不存在**

- **错处**：`echo iommu_map / iommu_unmap / vfio_iommu_type1 > set_event`。
- **正确**：`set_event` 收的是 **`system:event`**，iommu 那两个事件的名字就是
  `map` / `unmap`（`include/trace/events/iommu.h:79`、`:103`，`TRACE_SYSTEM iommu`
  在 `:9`）—— 要写 `iommu:map`。宿主只读实测 `/sys/kernel/debug/tracing/events/iommu/`
  下的目录名正是 `map`、`unmap`、`add_device_to_group`、`remove_device_from_group`、
  `attach_device_to_domain`、`io_page_fault`，`events/iommu/map/format` 的 `name:` 也是
  `map`。而 **`vfio_iommu_type1` 根本不是 trace system**：6.12.93 的
  `include/trace/events/` 下没有 `vfio.h`，宿主 `events/` 下也没有 `vfio*` 目录 ——
  它只是模块名（`drivers/vfio/vfio_iommu_type1.c`）。
- **为什么要登记**：三行里没有一行生效，但**都不报错到显眼处**（`>` 写失败只回一句
  shell 重定向错误，很容易被当成"权限问题"忽略），练习照做会得到一份空 trace，
  然后怀疑"直通没有 DMA 映射"。

**处置**：练习 2 重写为「`iommu:map` / `iommu:unmap` 两个事件 + function tracer 抓
VFIO 侧四个函数」：`vfio_dma_do_map`（`vfio_iommu_type1.c:1548`）、`vfio_dma_do_unmap`
（`:1270`）、`vfio_pin_map_dma`（`:1448`）、`vfio_iommu_type1_ioctl`（`:2991`）——
四个名字均在宿主 `available_filter_functions` 里实测存在（模块 `vfio_iommu_type1`）。
写法遵守 D10（显式清场 + 一个名字一次 `>>`）与 `measurement.md` §5 第 3 条，
收尾补上 tracefs 三个出口的清理。

### D13 `|| echo "0"` 兜底与 `set -euo pipefail` + `grep`：两类会让统计静默失真的 shell 写法

这一类不是"引用错源码"，而是**脚本跑得通、数字全错或直接没数字**，跨了四个文件，
所以单独登记一条。

**错误 1：`VAR=$(... | grep -c PATTERN || echo "0")`**

- **错处**：`../phase4-interrupts/practice/ex2-irte-observe.sh:51`、`:70`
  （`REMAPPED_COUNT` / `POSTED_COUNT`）；本轮我自己新写的
  `practice/bench-observer-cost.sh:177-179`（`pgrep -c`）也犯了一遍。
- **正确**：`grep -c` 与 `pgrep -c` 在计数为 0 时**照样打印 `0`**，只是退出码为 1。
  再 `|| echo "0"` 就是**第二次**输出一个 0，命令替换捕到的是两行 `"0\n0"`。
  后果分两种：只用于 `echo` 的，输出多出一行裸 `0`（ex2 就是这种，不致命但脏）；
  用于 `[ "$V" -gt 0 ]` 或 `bc` 的，直接报
  `integer expression expected` / 语法错误。改成 `|| true` 即可。
- **为什么要登记**：这个写法看着像"防御性编程"，很容易被照抄扩散。实测命令：
  `V=$(echo x | grep -c y || echo 0); echo "$V" | wc -l` → `2`。

**错误 1b：同一形状的 `awk` 变体，兜底根本不触发**

- **错处**：`../phase4-interrupts/practice/ex6-vcpu-migration.sh` 三处
  （旧 `:156` `CURRENT_CPU`、`:179` `ORIG_CPU`、`:244` `NEW_CPU`）写的是
  `$(cat /proc/$PID/task/$tid/stat 2>/dev/null | awk '{print $39}' || echo "?")`。
- **正确**：`awk` 读**空输入**时不打印任何东西、但**退出码是 0**，`||` 因此永远不触发，
  变量拿到的是**空串**而不是 `"?"`。这与错误 1 相反：那里是多输出一个 0，这里是
  兜底形同不存在。空串一进 `[ "$ORIG_CPU" -eq 0 ]` 就报
  `integer expression expected`；该判断在 `if` 条件里，`set -e` 不生效，脚本**不会死**，
  但会静默走 else 分支（目标 CPU 恒为 0），只在 stderr 留一行错。
  管用的兜底是 `${VAR:-?}`，不是 `|| echo`。
- **实测**：`V=$(cat /nonexistent | awk '{print $39}' || echo "0"); echo "[$V]"` → `[]`。

**错误 2：`set -euo pipefail` 下 `grep` 无匹配的管线**

- **错处**：`../../scripts/trace/trace-page-fault.sh` 详细分析分支的
  `grep -oP 'address=0x...' | sort | uniq -c | sort -rn | head -20`
  （脚本第 14 行就是 `set -euo pipefail`）。
- **正确**：`grep` 无匹配退出 1，`pipefail` 把它传染给整条管线，`set -e` 随即
  终止脚本。"一次都没抓到"是**正常结果**不是错误，必须显式兜：管线尾部 `|| true`，
  或者把计数包进 `n=$(... || true); printf '%s' "${n:-0}"` 这样的辅助函数。
  另有一个同类：`sort | head -N` 里 `head` 提前关闭管道会给 `sort` 发 SIGPIPE，
  同样让整条管线非零退出。
- **为什么要登记**：这一条比错误 1 更难查 —— 脚本"跑一半就没了"，末尾的统计
  一段都不打印，看着像 trace 是空的。本轮实测：旧版 `trace-page-fault.sh`
  的详细分支**必然**死在这条管线上（因为 `address=` 这个形式在 trace 文本里
  根本不存在，见 D14 错误 2），所以那个分支从来没成功跑完过。

**错误 3：用 `awk` 按空白切 `/proc/<pid>/stat`，对 QEMU vCPU 线程必然数错字段**

- **错处**：同 ex6 那三处的 `awk '{print $39}'` —— 意图是取第 39 项 `processor`
  （线程当前所在物理 CPU），字段号本身**没错**，错在切分方式。
- **正确**：`/proc/<pid>/stat` 的第 2 项是 comm，**外面带括号、里面可以含空格**。
  QEMU 给 vCPU 线程起的名字是 `"CPU %d/KVM"`（QEMU 10.1.0-rc2
  `accel/kvm/kvm-accel-ops.c:70`），展开成 `(CPU 0/KVM)` —— 含一个空格，于是 `awk`
  按空白切分时 comm 占了**两个**字段，其后所有字段号整体**右移 1**：`$39` 读到的是
  第 38 项 `exit_signal`（内核输出顺序 `fs/proc/array.c:641`），而 `processor`
  （`:642` 的 `task_cpu(task)`）落在 `$40`。`exit_signal` 对 QEMU 线程通常是
  `17`(SIGCHLD)，在几十核机器上**看着就是个合理的 CPU 号**，所以错得毫无痕迹。
  正确写法是先贪婪剥掉最后一个 `") "` 再数，剥完 `processor` 是第 37 个字段：

  ```bash
  sed 's/.*) //' "/proc/$PID/task/$TID/stat" | awk '{print $37}'
  ```

- **实测对照**（本机，一个 comm 含两个空格的线程 `Bun Pool 0`，右移 2）：

  | 写法 | 读到 | python 按真实字段号数的真值 |
  |---|---|---|
  | `awk '{print $39}'` | `0` | — |
  | `sed 's/.*) //' \| awk '{print $37}'` | `6` | `6` ✅ |

  comm 不含空格时（PID 1，`systemd`）两种写法都得 `73`，与真值一致 —— 这正是这个
  bug 能长期潜伏的原因：**在非 QEMU 线程上自测是过的**。
- **为什么要登记**：这一条直接毁掉实验的**观测量本身**。ex6 要测的就是"迁移前后
  vCPU 在哪个 pCPU"，而 `CURRENT_CPU` / `ORIG_CPU` / `NEW_CPU` 三个数全是
  `exit_signal`，于是"迁移后 vCPU 所在 CPU: 17"这种输出既不是迁移前也不是迁移后，
  跟 `sched_migrate_task` 事件对不上，做实验的人会去怀疑 tracepoint。
  ★ 同类风险遍布全仓：任何 `awk '{print $N}'` 直接切 `/proc/*/stat` 的地方，
  只要目标线程名可能含空格就有这个问题。

**处置**：`|| echo "0"` / `|| echo 0` / `|| echo "?"` 一共 **15 处**改完 ——
ex2 两处、ex5 四处、`bench-observer-cost.sh` 三处、`trace-page-fault.sh` 三处、
ex6 三处；`trace-page-fault.sh` 抽出 `count_re()` 辅助函数统一兜底，两处
`grep | head` 管线补 `|| true`。ex6 那三处连同错误 3 的字段号问题一起收敛成一个
`task_cpu_of()` 辅助函数（坑只在函数定义处写一遍），取不到值时输出 `?`，
调用方显式判 `?` 再兜底；已按上面的对照表离线自测通过。
`../phase4-interrupts/practice/ex5-on-sn-observe.sh:88-91` 同一批改掉
（该文件另有 D11 第五处的 `exit_reason=1` 问题）。

### D14 `../../scripts/trace/trace-page-fault.sh` 一整组缺陷：两条假"不存在"注释 + 三处 trace 文本形式写错 + `-l` 是空操作

跨章脚本，按 D 节（命令在 6.12.93 上不可执行）的先例登记在这里。这个脚本
**每个统计项都是 0 或者直接死掉**，但一句报错都没有。

**错误 1：两条"在 6.12 中不存在"的注释都是假的**

- **错处**：`# 注: kvm:kvm_mmu_paging_element 和 kvm:kvm_mmu_set_spte 在 6.12 中不存在`、
  `# 注: kvm_mmu_get_page 在 6.12 中不存在`。
- **正确**：`kvm_mmu_paging_element`（`arch/x86/kvm/mmu/mmutrace.h:89-90`）、
  `kvm_mmu_set_spte`（`:334-335`，`TP_printk` 在 `:360`）、`kvm_mmu_get_page`
  （`:158-159`）**都是真实存在的 tracepoint**，宿主
  `/sys/kernel/debug/tracing/events/kvmmmu/` 下三个目录实测都在。它们不在
  `kvm:` 这个 system 下 —— `TRACE_SYSTEM kvmmmu`（`mmutrace.h:9`），要写
  `kvmmmu:kvm_mmu_set_spte`。触发点：`tdp_mmu.c:1059`（TDP MMU 路径）、
  `mmu.c:2949`（shadow 路径）。
- **为什么要登记**：写"不存在"比写错名字更坏 —— 后来人会直接放弃这条观测路径。
  而"页级别分布"这个统计**只能**靠 `kvmmmu:kvm_mmu_set_spte`，因为
  `kvm:kvm_page_fault` 没有 `level` 字段（见 D2）。

**错误 2：三处按 `字段=值` 的形式去 grep trace 文本**

- **错处**：`grep -oP 'address=0x[0-9a-f]+'`、`grep "level=1"` / `"level=2"` / `"level=3"`、
  `grep -c "write"` / `"read"` / `"exec"`。
- **正确**：trace 文本里这些位置是**空格不是等号**，而且没有 write/read/exec 这些词：
  - `kvm_page_fault` 的 `TP_printk` = `"vcpu %u rip 0x%lx address 0x%016llx error_code 0x%llx"`
    （`arch/x86/kvm/trace.h:420-422`）→ 抽 GPA 要按 `address 0x...`。
  - `kvm_mmu_set_spte` 的 `TP_printk` = `"gfn %llx spte %llx (%s%s%s%s) level %d at %llx"`
    （`mmutrace.h:360`）→ 数级别要按 `level 1 ` 这种带尾空格的形式，否则 `level 1`
    会把 `level 10` 之类一起吞掉（本例最大 5，但形式要写对）。
  - 读/写/取指**只能**从 `error_code` 的位解，见错误 3。
- **为什么要登记**：`address=` 那条不只是恒为 0，还经 `pipefail` 把整个详细分支
  打死（D13 错误 2）。与 D11 是同一个病根：**照着 `字段名=值` 的想象写 grep，
  没去读 `TP_printk`**。

**错误 3：缺页类型用"写/取指/其它"三分区，与规范矛盾，而且认错了"读"位**

- **错处**（本轮我自己第一版改法，一并登记）：
  `if (( ec & 0x2 )); then 写; elif (( ec & 0x10 )); then 取指; else 读; fi`。
- **正确**：`kvm_page_fault.error_code` 是 KVM 自己的 **PFERR 字**
  （`arch/x86/include/asm/kvm_host.h:261-273`），不是 EPT 违规的原始 exit
  qualification。VMX 的换算在 `arch/x86/kvm/vmx/common.h:14-25`
  （`__vmx_handle_ept_violation()`）：

  | exit qualification 位 | 宏（`arch/x86/include/asm/vmx.h`） | 换算成 PFERR |
  |---|---|---|
  | bit0 读 | `EPT_VIOLATION_ACC_READ`（`:589`） | `PFERR_USER_MASK` **bit2** |
  | bit1 写 | `EPT_VIOLATION_ACC_WRITE`（`:590`） | `PFERR_WRITE_MASK` bit1 |
  | bit2 取指 | `EPT_VIOLATION_ACC_INSTR`（`:591`） | `PFERR_FETCH_MASK` bit4 |
  | RWX 权限位 | `EPT_VIOLATION_RWX_MASK`（`:592`） | `PFERR_PRESENT_MASK` bit0 |

  两个要点：① **VMX 上"读访问"落在 bit2 `PFERR_USER`**，bit0 的含义是"EPT 表项
  带了 RWX 权限位"而**不是**"页已存在"；② 位**不能当分区**——
  Intel VMX 规范 Table 28-7 的 NOTES 1 写明：开了 EPT accessed/dirty flags 后，
  处理器访问 guest 页表项按写处理，此时 exit qualification 的 **bit0 与 bit1 会同时
  置位**（换算过来就是 bit2 与 bit1 同现）。写成 if/elif/else 会把这类事件静默
  归掉一类；而 `else` 分支还会把 present-only、RSVD(bit3)、PK(bit5)、SGX(bit15)、
  guest-RMP(bit31) 全算成"读"。
- **规范引用**：intel-vmx.pdf, Table 28-7 (Exit Qualification for EPT Violations)
  及其 NOTES 1。
- **跨厂商警告**：SVM 的 NPF 把 `exit_info_1` **原样**当 `error_code` 传下去
  （`arch/x86/kvm/svm/svm.c:2125`，trace 点 `:2139`），那一套位沿用 #PF 错误码语义，
  **bit2 是 U/S（用户态访问）而不是"读"**。同一个脚本在 AMD 机器上跑，
  bit2 那一列不能按读缺页解读。
- **处置**：改成六个位各自独立计数（PRESENT/WRITE/USER/RSVD/FETCH + 低 5 位全空的
  OTHER），输出里明写"各列独立、同一条事件可计入多列、不是分区"。合成 trace 自测：
  8 条覆盖 `0x0/0x1/0x2/0x4/0x6/0x8/0x10/0x20`，其中 `0x6` 正确同时计入 WRITE 与
  USER，六列分别为 1/2/2/1/1/2，退出码 0。

**错误 4：`-l LEVEL` 参数是空操作**

- **错处**：旧版 `case $LEVEL in 1) echo "  过滤: 4KB 页" ;; ...` —— 只打印一行
  说明文字，**从来没往任何 filter 文件里写过东西**，然后统计分支照样报全部级别。
- **正确**：级别过滤只能挂在 `kvmmmu:kvm_mmu_set_spte` 上，它有 `__field(u8, level)`
  （`mmutrace.h:343`，宿主 `events/kvmmmu/kvm_mmu_set_spte/format` 实测
  `field:u8 level`）；`kvm:kvm_page_fault` 没有 level 字段（D2）。取值按
  `enum pg_level`（`arch/x86/include/asm/pgtable_types.h:548-556`）：
  NONE=0 / 4K=1 / 2M=2 / 1G=3 / 512G=4 / 256T=5。写失败要**显式警告**并说明
  "本次统计包含全部级别"，不能静默。

**错误 5：D10 那三个 `>` 写，本脚本一个不落全踩了**

`echo kvm:kvm_page_fault > set_event`、`echo kvm_handle_page_fault > set_ftrace_filter`、
`echo "$PID" > set_event_pid` —— 三个文件都是带 `O_TRUNC` 的写会先清掉**全部**已有
配置（判据分别见 D10 与 `measurement.md` §5 第 3 条）。第三个是本轮新查出来的：
`ftrace_event_set_pid_open()`（`kernel/trace/trace_events.c:2432`）在 `:2442-2444`
对带 `O_TRUNC` 的打开调 `ftrace_clear_event_pids(tr, TRACE_PIDS)`；好在
`event_pid_write()` 开头 `:2167-2168` 是 `if (!cnt) return 0;`，所以**纯 `: >` 截断
就能干净清空**，不会像 `set_event` 那样需要额外收尾。已改成显式 `: >` 清场 + `>>` 追加，
`cleanup()` 里补上本次写的两个 filter 的收场（`kvm_mmu_set_spte/filter` 写 0、
`set_event_pid` 用 `: >`）。

**错误 6：往 `set_ftrace_filter` 写了一个内联掉、没有符号的函数**

- **错处**：`echo __tdp_mmu_set_spte_atomic >> set_ftrace_filter`。
- **正确**：它是 `static inline int __must_check`（`arch/x86/kvm/mmu/tdp_mmu.c:533`），
  被 `:584`、`:609` 就地内联，宿主 `available_filter_functions` 里实测**零命中**；
  同批的 `kvm_handle_page_fault` / `kvm_tdp_page_fault` / `kvm_tdp_mmu_map` /
  `make_spte` 四个实测都在。已改成写之前先在 `available_filter_functions` 里核对，
  不在就打印"跳过 X（可能被内联）"而不是让内核回一句写失败。
- **★ 教训（核对方法本身有坑）**：`available_filter_functions` 的行格式是
  **`symbol [module]`**（宿主实测 `kvm_tdp_page_fault [kvm]`），不是裸符号名。
  用 `grep -c "^fn$"` 去核对会把**五个全部**判成零命中，得出"这脚本一个函数都
  过滤不了"的相反结论；本脚本用的是 `grep -q "^${fn}\b"`，`\b` 正好落在符号名与
  后面那个空格之间，才是对的。

**错误 7：末尾提示里两处会误导**

`trace-cmd record -e kvm:kvm_page_fault -p $PID -- sleep $DURATION` 在没检测到 QEMU
时 `$PID` 为空，`-p` 会把 `--` 当成自己的参数；且要页级别必须再加
`-e kvmmmu:kvm_mmu_set_spte`。`"分析大页效果: 对比 level=1 和 level=2 的数量"`
同错误 2（形式是 `level 1`）。两处已改。

## E. 结构性问题（非单点错误）

### E1 本章的定位与其它章冲突

旧版五块机制（halt-polling / VM-Exit 减少 / EPT 优化 / vCPU 调度 / TSC 同步）
里有三块在别处讲得更深，且都已由别的章实测。保留的结果是同一结论存在三份、
其中一份（本章）没有数据支撑。处置见 [README.md](README.md) 的分工表。

### E2 `README.md:43-51` 的"章节导航"指向不存在的章节

表里列的"1. halt-polling … 5. TSC 同步"并不是五个文件，只是 `annotations.md`
的五个小节。别的章（如 `../phase10-debugging/README.md:45`
"参考 phase9 EPT 优化章节"）按字面去找就会找不到。

### E3 本章内部政策自相矛盾

`practice/README.md:6-8` 声明"删除了所有未经实测的典型值数据"，
而同一目录的 `annotations.md` 通篇是未实测的典型值（见 B 节）。
新规矩统一为：**全章数字只有两种合法来源** —— 本章 `practice/bench/` 原始数据，
或带出处的 [index.md](index.md) 条目（`measurement.md` §8）。

## F. 尚未修正、仅登记的问题

| 项 | 位置 | 状态 |
|---|---|---|
| `perf kvm stat record -p $PID` 丢 vCPU 线程退出 | `../phase10-debugging/README.md` 场景 1 | **已改为 `-a`**（判据 `tools/perf/builtin-kvm.c:1959-1960`），与 `../phase8-capstone/practice/README.md` 的实测结论一致 |
| VM-Exit reason 编号表错 | `../examples/bpf-programs/README.md` §1 速查表 | **本轮才真正改掉**。★ 本行旧版写着"已按 `vmx.h:33-98` 重建"，但文件里当时仍是 `EPT_VIOLATION`=24、`EPT_MISCONFIG`=25、`MSR_WRITE`=48 —— 登记 ≠ 修正，这种"已修"字样必须先核对文件再写。现在的值取自 `arch/x86/include/uapi/asm/vmx.h:32-95`（字符串表 `:96-158`）：`IO_INSTRUCTION`=30、`MSR_READ`=31、`MSR_WRITE`=32、`PAUSE_INSTRUCTION`=40、`EPT_VIOLATION`=48、`EPT_MISCONFIG`=49、`PREEMPTION_TIMER`=52、`PML_FULL`=62。★ 同一张错表在**可执行脚本** `../examples/bpf-programs/run-trace-vmexit.sh` 里还有第二份（`EXIT_REASON_NAMES`，八个码错），本轮已整表重抄并离线自测，见 D11 |
| PI 收益"10-100 倍"无实测 | `../phase4-interrupts/posted-interrupts.md` | 未改（机制部分有规范支撑）。已在 [index.md](index.md) §3 标为 C 级、指向已有脚本 `../phase4-interrupts/practice/ex4-pi-vs-remapped.sh` |
| phase2 的 200 ns / 2-5 µs / 50 ns 等推算值在 `archive/` 多处复制 | `../phase2-mem-virt/` | 未逐处改写，统一在 [index.md](index.md) §3 登记为 C 级 |
| `scripts/shared/` 下 `memtype_test`、`real_hugepage_test` 无对应源码 | `../scripts/shared/` | 未处理。`real_hugepage_test` 本是本章大页实验的现成抓手，缺源码则不可复算 → 列为待补 |
| phase3 的 `dmar_perf_latency` 方法有、数据无 | `../phase3-iommu/practice/README.md` I.15 | 归 phase3 自己补，[index.md](index.md) §3 已登记 |
