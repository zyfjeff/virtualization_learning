# Phase 11 勘误

> 本节收集写作过程中和已有内容里发现的错误。每条给出错误原文、正确信息、源码依据。

---

## C1: `lapic_timer_advance_ns` 不是模块参数

**错误**：文档或教程里写 `echo 1000 > /sys/module/kvm/parameters/lapic_timer_advance_ns`

**正确**：6.12.93 里 `/sys/module/kvm/parameters/` 下**没有** `lapic_timer_advance_ns`。模块参数只有 `lapic_timer_advance`（`bool`，`arch/x86/kvm/lapic.c:70-71`，0444 只读）。`lapic_timer_advance_ns` 是 **per-vCPU 的 debugfs 只读文件**（`arch/x86/kvm/debugfs.c:67`），手动设固定提前量做不到。

**源码依据**：
- 模块参数：`arch/x86/kvm/lapic.c:70-71`
- debugfs 文件：`arch/x86/kvm/debugfs.c:67`

**验证**：
```bash
# 6.12.93 内核
ls /sys/module/kvm/parameters/ | grep lapic
# 输出: lapic_timer_advance  (bool, 没有 _ns 后缀)

# 宿主内核可能是旧版（如 6.8.0-51），有 lapic_timer_advance_ns
# 所以**先 ls 再写文档**
```

---

## C2: `stats` 文件不能 grep

**错误**：`cat /sys/kernel/debug/kvm/<pid>-<vmid>/stats | grep mmu`

**正确**：`stats` 是**二进制格式**，不是文本。6.12.93 的 KVM 统计每项一个文本文件（`debugfs_create_file(pdesc->name, …)` 逐项注册，`virt/kvm/kvm_main.c:6352`），那个叫 `stats` 的文件是二进制头描述符，grep 不出东西。

**源码依据**：`virt/kvm/kvm_main.c:6352`

**验证**：
```bash
# 列出实际的统计文件
ls /sys/kernel/debug/kvm/<pid>-<vmid>/
# 应该看到 mmu_shadow_zapped, mmu_pte_write 等独立文件，不是 stats 里的字段
```

---

## C3: `echo evt > set_event` 会清掉所有事件

**错误**：`echo kvm:kvm_entry > /sys/kernel/debug/tracing/set_event` 以为只是添加一个事件

**正确**：`>` 带 `O_TRUNC` 标志，打开时内核先 `ftrace_clear_events()` 把**全部**已启用事件清掉再处理本次写入（`kernel/trace/trace_events.c:2411` → `:2422-2423`，函数定义 `:883`）。只想追加就用 `>>`，要清空就显式写 `: > set_event`。

**源码依据**：`kernel/trace/trace_events.c:2411→2422-2423`

**规则唯一来源**：`../phase10-performance/measurement.md` §5 第 3 条

**验证**：
```bash
# 错误做法
echo kvm:kvm_entry > set_event
echo kvm:kvm_exit >> set_event
cat set_event
# 输出只有 kvm:kvm_exit，kvm:kvm_entry 被第一次的 > 清掉了

# 正确做法
: > set_event                    # 显式清场
echo kvm:kvm_entry >> set_event  # 用 >> 追加
echo kvm:kvm_exit >> set_event
cat set_event
# 输出两个事件都在
```

---

## C4: `tracing_on=0` 不等于零开销

**错误**：`echo 0 > /sys/kernel/debug/tracing/tracing_on` 后以为 probe 完全停止

**正确**：`tracing_on=0` 只是不写 buffer，probe 仍然注册着，函数入口的 nop 已经被替换成 call 指令，每次调用仍有开销（call + 检查 tracing_on）。要完全停止，必须 `echo none > current_tracer` + `: > set_event` + `: > set_ftrace_filter`。

**源码依据**：`kernel/trace/trace.c` 中 `tracing_on` 的检查点

---

## C5: `perf kvm stat -p $PID` 丢 vCPU 线程

**错误**：`sudo perf kvm stat record -p $QEMU_PID -- sleep 10` 以为能跟踪所有 vCPU

**正确**：`perf kvm stat` 必须 `-a` system-wide。用 `-p $PID` 只跟踪被包裹的那个进程，vCPU 线程的退出**全丢**：`tools/perf/builtin-kvm.c:1959-1960` 里 `if (target__none(&kvm->opts.target)) … system_wide = true;` —— 只有**不给 target** 时才自动 system-wide。

**源码依据**：`tools/perf/builtin-kvm.c:1959-1960`

> ★ **成因更正（见 C19）**：本条原写的"vCPU 线程不在 `-p` 范围"不成立 ——
> vCPU 是 QEMU 进程内的线程，`perf -p` 会扫 `/proc/<pid>/task` 把整个线程组纳入
> thread map（`tools/perf/util/thread_map.c:34-58`）。
> `-a` 仍然是本章的推荐写法（统一 system-wide 口径，避免"没采到"被读成"没有退出"），
> 但理由要按 C19 说，不要引用错误机制。

**验证**：
```bash
# 错误做法
sudo perf kvm stat record -p $QEMU_PID -- sleep 10
# 输出的 VM-Exit 数量远少于实际

# 正确做法
sudo perf kvm stat record -a -- sleep 10
```

---

## C6: `kvm_exit` 的 `exit_reason` 在 trace 文本里是符号名

**错误**：`grep 'exit_reason=1' /sys/kernel/debug/tracing/trace` 或按 `reason=[0-9]` 去 grep

**正确**：trace 文本里打的是**符号名**（`reason MSR_WRITE`），不是数字。`TP_printk` 是 `reason %s`（`arch/x86/kvm/trace.h:325-330`），字符串由 `kvm_print_exit_reason()` 先 `exit_reason & 0xffff` 查 `VMX_EXIT_REASONS`、再把高位标志按 `VMX_EXIT_REASON_FLAGS` 附上（`:289-295`；`arch/x86/include/uapi/asm/vmx.h:96-158`）。想按原因聚合 trace 文本要抓符号名，想拿数字走 BPF 的 `args->exit_reason`。

**源码依据**：`arch/x86/kvm/trace.h:289-295, :325-330`

**验证**：
```bash
# trace 文本里是符号名
cat /sys/kernel/debug/tracing/trace | grep kvm_exit
# 输出: kvm_exit: reason MSR_WRITE ...

# BPF 里是数字
sudo bpftrace -e 'tracepoint:kvm:kvm_exit { printf("%d\n", args->exit_reason); }'
# 输出: 31  (MSR_WRITE 的数字)

# ★ 6.12.93 没有 exit_reason_full 这个字段
```

---

## C7: `nx_huge_pages` 不是布尔

**错误**：`echo 1 > /sys/module/kvm/parameters/nx_huge_pages`

**正确**：`nx_huge_pages` 是 `module_param_cb`，只收 `off/force/auto/never`（解析在 `arch/x86/kvm/mmu/mmu.c:7259`，分支 `:7268-7284`），**不是布尔**。写 `1` 或 `0` 会报 `Invalid argument`。

**源码依据**：`arch/x86/kvm/mmu/mmu.c:7259→7268-7284`

**验证**：
```bash
# 错误做法
echo 1 > /sys/module/kvm/parameters/nx_huge_pages
# bash: echo: write error: Invalid argument

# 正确做法
echo force > /sys/module/kvm/parameters/nx_huge_pages
# 可选值: off, force, auto, never
```

---

## C8: `kvm_track_tsc` 的 `masterclock` 字段是旧值

**错误**：根据 `kvm_track_tsc` trace 里的 `masterclock` 字段判断翻转方向

**正确**：`kvm_track_tsc` 打的是**翻转前的旧值**：`kvm_track_tsc_matching()`（`arch/x86/kvm/x86.c:2515`）只算一个**局部变量**（`x86.c:2526`）再发请求，`ka->use_master_clock` 全树只有 `x86.c:3034` 一处写。照着读会稳定慢一拍。要看本次重算的新决定，用 `kvm_update_master_clock` tracepoint（赋值在 `pvclock_update_vm_gtod_copy()` `x86.c:3034`，打印在 `:3042`）。

**源码依据**：`arch/x86/kvm/x86.c:2515-2526, :3015, :3034, :3042`

**验证**：
```bash
# kvm_track_tsc 的 masterclock 是旧值
echo kvm:kvm_track_tsc >> /sys/kernel/debug/tracing/set_event
cat /sys/kernel/debug/tracing/trace_pipe | grep kvm_track_tsc
# 行里的 masterclock=0/1 是翻转前的值

# kvm_update_master_clock 的 use_master_clock 是新值
echo kvm:kvm_update_master_clock >> /sys/kernel/debug/tracing/set_event
cat /sys/kernel/debug/tracing/trace_pipe | grep kvm_update_master_clock
# 行里的 use_master_clock=0/1 是本次重算的新决定
```

---

## C9: `kvm_vcpu_wakeup` 没有 `vcpu_id` 字段

**错误**：`bpftrace -e 'tracepoint:kvm:kvm_vcpu_wakeup { printf("vcpu %d\n", args->vcpu_id); }'`

**正确**：`kvm_vcpu_wakeup` 的参数只有 `ns, waited (bool), valid (bool)` —— **没有 vcpu_id**，也没有 `runnable/blocking` 这两个字段（字段定义 `include/trace/events/kvm.h:47-51`）。

**源码依据**：`include/trace/events/kvm.h:43, :47-51`

---

## C10: `kvm_pml_full` 没有 `full_count` 字段

**错误**：`bpftrace -e 'tracepoint:kvm:kvm_pml_full { printf("count %d\n", args->full_count); }'`

**正确**：`kvm_pml_full` 的参数**只有 `vcpu_id`**，没有 `full_count`（字段定义 `arch/x86/kvm/trace.h:967-969`，`TP_printk` 是 `"vcpu %d: PML full"`）。

**源码依据**：`arch/x86/kvm/trace.h:963, :967-969`

---

## C11: `kvm_nested_vmexit` 没有 `l1_rsp` 字段

**错误**：以为 `kvm_nested_vmexit` 有 `l1_rsp` 字段表示 L1 处理延迟

**正确**：`kvm_nested_vmexit` 使用 `TRACE_EVENT_KVM_EXIT` 宏（`arch/x86/kvm/trace.h:679`，宏体 `:297-331`），参数与 `kvm_exit` **完全相同**（`exit_reason, guest_rip, isa, info1, info2, intr_info, error_code, vcpu_id`）—— 没有 `exit_reason_full`，也没有 `l1_rsp`。

**源码依据**：`arch/x86/kvm/trace.h:679, :297-331`

---

## C12: 标题与跨 phase 引用沿用旧编号

**发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）

**错误与修正**:

| 位置 | 错误 | 正确 |
|---|---|---|
| `README.md:1` | 标题 "Phase 10" | Phase 11 |
| `README.md:9-15` | 前置 "Phase 1-9"、"perf kvm stat（Phase 9）"、"VM-Exit 机制（Phase 1-4）"、"还没有完成 Phase 9" | Phase 0-10 / Phase 10 / Phase 1-5 / Phase 10 |
| `README.md:93-96` | "(phase4)"、"(phase9 E2)"、"(phase5)" | (phase5)、(phase10 E2)、(phase6) |
| `README.md:253-254` | "phase0-8 源码学习"、"phase9 性能测量" | phase0-9 / phase10 |
| `annotations.md:1` | 标题 "Phase 10" | Phase 11 |
| `index.md:130-140` | 表头 "phase10 中的位置"（实为本章 phase11 的文件）；行内 phase4 中断/phase5 virtio/phase7 时钟/phase8 最小 VMM/phase9 性能 | 表头 "本章（phase11）中的位置"；phase5/phase6/phase8/phase9/phase10 |
| `practice/e2-perf-bottleneck/README.md:157-160` | 同 README 93-96 三处 | 同上 |

**成因**: phase4-pcie 插入后全局编号顺延（中断 4→5、virtio 5→6、VFIO 6→7、
时钟 7→8、capstone 8→9、性能 9→10、调试 10→11），本章未同步。

**已同步修正**: 上表全部文件（补充：`case-studies.md:1`、`index.md:1`、`corrections.md:1` 的 "Phase 10" 标题与
`practice/ex2-perf-bottleneck/README.md:54-55` 的 phase4/phase9 残留本轮一并改正）。

---

## 第二轮核查（2026-11-26，trace 字段 / 机器环境专项）

> 本轮每条都做了**双重核对**：`arch/x86/kvm/trace.h` + `include/trace/events/kvm.h`
> 的 `TP_STRUCT__entry`，以及宿主 `/sys/kernel/debug/tracing/events/kvm/<evt>/format`。
> 宿主跑的是 6.8.0-51，与 6.12.93 源码在这些字段上一致，故结论不受版本差影响。

## C13: VM-Exit 编号表错两个号

**错误**：`annotations.md` 的常用退出号表把 `TRIPLE_FAULT` 写成 3、`MSR_WRITE` 写成 28。

**正确**：`TRIPLE_FAULT = 2`、`MSR_WRITE = 32`；**28 是 `CR_ACCESS`**、3 是 `INIT_SIGNAL`。

**源码依据**：`arch/x86/include/uapi/asm/vmx.h:34`（TRIPLE_FAULT=2）、`:57`（CR_ACCESS=28）、
`:61`（MSR_WRITE=32）；宿主 `events/kvm/kvm_exit/format` 的符号表同。
`vcpu-exit-diagnosis.md` 一直写的是 `[2] handle_triple_fault`，可与本表互证。

---

## C14: 大部分 KVM trace 事件根本没有 `vcpu_id` 字段

**错误**：`annotations.md` 的事件表给 `kvm_inj_virq`、`kvm_inj_exception`、
`kvm_apic_accept_irq`、`kvm_apicv_accept_irq`、`kvm_pv_eoi`、`kvm_msr`、`kvm_cr`、
`kvm_cpuid`、`kvm_mmio`、`kvm_emulate_insn`、`kvm_nested_vmexit_inject`、
`kvm_nested_intr_vmexit` 都列了 `vcpu_id` 参数。

**正确**：这些事件的 `TP_STRUCT__entry` 里**都没有 `vcpu_id`**。
按 vCPU 归因只能用 tracepoint 公共字段 `common_pid`（= 线程 TID）。
中断路径里带 `vcpu_id` 的只有 `kvm_pi_irte_update`（`kvm_set_irq` 也没有）。

**源码依据**：`trace.h:345-349 / :377-382 / :546-551 / :1405+ / :583-589 / :432-437 /
:464-468 / :220-229`；`include/trace/events/kvm.h:213+`；宿主各 `format` 实测一致。

---

## C15: 中断/IOAPIC/PIC 事件的字段名写错

| 事件 | 文档旧写法 | 实际字段 | 证据 |
|---|---|---|---|
| `kvm_inj_virq` | `vcpu_id, irq` | `vector, soft, reinjected` | `trace.h:341-349` |
| `kvm_inj_exception` | `vcpu_id, exception, has_error_code, error_code, payload` | `exception, has_error, error_code, reinjected` | `trace.h:372-382` |
| `kvm_apic_accept_irq` / `kvm_apicv_accept_irq` | `vcpu_id, vector, level, trig_mode` | `apicid, dm, tm, vec` | `trace.h:542-551`、`:1405` |
| `kvm_pi_irte_update` | `... vector ...` | `... **gvec** ...` | `trace.h:1080` |
| `kvm_pv_eoi` | `vcpu_id, pir` | `apicid, vector` | `trace.h:583` |
| `kvm_ack_irq` | `irq_source_id, gsi` | `irqchip, pin` | `kvm.h:183` |
| `kvm_pic_set_irq` | `chip, pin, level` | `chip, pin, elcr, imr, coalesced` | `trace.h:484-494` |
| `kvm_ioapic_set_irq` | `pin, level, remote_irr` | `e, pin, coalesced` | `kvm.h:98` |
| `kvm_mmio` | `vcpu_id, len, gpa, write, data` | `type, len, gpa, val` | `kvm.h:213` |
| `kvm_pio` | `..., rip` | `..., **val**` | `trace.h:161-172` |
| `kvm_unmap_hva_range` / `kvm_age_hva` | `mmu_notifier, start, end` | `start, end` | `kvm.h:441`、`:459` |

`level/edge`、`(Fixed|SMI|…)` 都是 `TP_printk` 现场由 `tm`/`elcr`/`e` 算出的**文本**，不是字段。

---

## C16: 时钟事件的字段名写错

- `kvm_write_tsc_offset`：不是 `vcpu_id, offset`，而是
  `vcpu_id, previous_tsc_offset, next_tsc_offset`（`trace.h:879-888`，
  print fmt `vcpu=%u prev=%llu next=%llu`）。
- `kvm_wait_lapic_expire`：不是 `vcpu_id, guest_tsc, tsc_deadline, dy_nsec`，
  只有 `vcpu_id, delta`（`trace.h:1035-1042`，`delta<0` 打 "early"）。

---

## C17: `kvm_nested_vmenter` 系列字段（含虚构字段）

- `kvm_nested_vmenter` 没有 `vmcs / nested_vmcs / l2_rip / l2_rsp`；
  实际是 `rip, vmcb, nested_rip, int_ctl, event_inj, tdp_enabled, guest_pgd, isa`
  （`trace.h:603-619`；VMX/SVM 共用同一字段名 `vmcb`，print fmt 按 `isa` 印成
  "vmcs:" 或 "vmcb:"，`:632-644`）。
- `kvm_nested_vmenter_failed` 是 `msg`(__string) + `err`，不是 `rip, error`（`:1677`）。
- `kvm_nested_vmexit_inject` 是 `exit_code, exit_info1, exit_info2, exit_int_info,
  exit_int_info_err, isa`，**退出原因字段叫 `exit_code` 不叫 `exit_reason`**（`:684-698`）。
- `kvm_nested_intr_vmexit` 只有 `rip` 一个字段（`:719`）。

与 C11 同源：这一族事件都没有任何 rsp/l1_rsp 字段。

---

## C18: 照抄出来的 grep 模式与 `TP_printk` 不符（静默抓空）

| 位置 | 错误模式 | 实际文本 | 证据 |
|---|---|---|---|
| `annotations.md` §7 | `grep -oP 'msr=0x[0-9a-f]+'`、"分别统计 write=0 / write=1" | `msr_write 48 = 0x63` | `trace.h:446-449` |
| `performance-analysis.md` §6.1/§8 | `grep -oP 'grow=\d+'`（"grow=1 是 GROW"） | `vcpu 0: halt_poll_ns 20000 (grow 10000)` | `kvm.h:359-366` |
| `performance-analysis.md` §7.1 / `case-studies.md` 案例2 | `grep 'use_master_clock 0'`、样例行 `host_clock 0` | `masterclock 0 hostclock tsc offsetmatched 0` | `trace.h:922-925`、`:951-955` |
| `case-studies.md` 案例2 / `annotations.md` §12 | `trace_kvm_userspace_exit(vcpu_id, …)`、样例 `vcpu 0 exit_reason …` | `reason KVM_EXIT_FAIL_ENTRY (9)`，字段只有 `reason`+`errno` | `kvm.h:22-41`、`kvm_main.c:4495` |
| `practice/ex3` Step5 / `vcpu-exit` §3.3 | `grep -oP 'exception=\w+'`、`awk '{print $NF}'` | `#PF (0)`、`#DF`（$NF 会取到 `(0)`） | `trace.h:391-396` |

与 CLAUDE.md 已知陷阱 3 同类：**必须照 `print fmt` 写 grep**，否则得到空结果会误判成"没有事件"。

---

## C19: `perf kvm stat` 的选项与 `-p` 成因

- **`perf kvm stat report` 没有 `--sort` 选项**。可用的是
  `--event / --vcpu / -k|--key {sample|time} / -p|--pid / -f|--force / --stdio`
  （`tools/perf/builtin-kvm.c:1786-1800`）。按延迟排序要写 `--key=time`。
- **`perf kvm stat record` 禁用了 `-g/--call-graph`**
  （`builtin-kvm.c:1766-1767` 置 `PARSE_OPT_DISABLED`；
  `tools/lib/subcmd/parse-options.c:80-81` 对 disabled 选项直接报 "is not usable"）。
  要调用栈改用 `perf record -g -a -e kvm:kvm_exit`。
- **`-p $PID` 不会漏掉 vCPU 线程**：vCPU 是 QEMU 进程内的线程（同一 TGID），
  `perf -p` 的 thread map 由 `thread_map__new_by_pid()` 扫
  `/proc/<pid>/task` 生成（`tools/perf/util/thread_map.c:34-58`），线程在范围内。
  本章推荐 `-a` 的理由是**避免 target/汇总口径问题**，不是"线程不在范围内"。
  （C5 的实操建议不变，这里只更正成因。）

---

## C20: debugfs 里没有名为 `stats` 的文件，`kvm_stat_per_vm()` 不存在

- 6.12.93 全树 `grep '"stats"' virt/kvm/ arch/x86/kvm/` 零命中：
  KVM 统计是**逐项一个文本文件**，注册在
  `kvm_create_vm_debugfs()`（`virt/kvm/kvm_main.c:1092`、`:1107`，per-VM 目录）
  与 `kvm_init_debug()`（`:6352`、`:6363`，全局目录）。
  **没有叫 `kvm_stat_per_vm()` 的函数**（旧引用里的函数名是编的）。
- 二进制 blob 走 `KVM_GET_STATS_FD` ioctl（`include/uapi/linux/kvm.h:1541`），
  返回的是匿名 inode `kvm-vm-stats`（`kvm_main.c:5121-5139`），
  **不是 debugfs 路径下的 `stats` 文件**。宿主实测
  `/sys/kernel/debug/kvm/<pid>-<fd>/` 下无 `stats`，只有 `exits`、
  `halt_successful_poll` 等逐项文件。
- 由此 C2 的"stats 是二进制文件"要按这里收窄：C2 说的"不能 grep"针对的是
  新内核 `KVM_GET_STATS_FD` 的 blob，而非 debugfs 文件名。
- VM 目录名是 `snprintf("%d-%s", task_pid_nr(current), fdname)`（`kvm_main.c:1060`），
  `fdname` 是 **VM 的 fd 号**（`:5499` 取 fd、`:5501` 写入），不是 vmid。

---

## C21: halt-polling 自适应时序图与实现不符

**错误**：`annotations.md` §12 画成"Phase 1 窗口内命中 → `trace_kvm_halt_poll_ns(grow=false,…)`"、
"Phase 3 窗口内唤醒 → grow"。

**正确**：`trace_kvm_halt_poll_ns` **只在 `grow_halt_poll_ns()` / `shrink_halt_poll_ns()`
里发**（`include/trace/events/kvm.h:373-376` 定义两个宏，调用点
`virt/kvm/kvm_main.c:3686` grow、`:3705` shrink）：

- 轮询**在窗口内命中** → `if (halt_ns <= vcpu->halt_poll_ns) ;` 是空分支
  （`kvm_main.c:3865-3866`），**不发任何 halt_poll_ns 事件**；
  该轮只累加 `halt_attempted_poll` / `halt_successful_poll`
  （`kvm_main.c:3771-3774`）。
- `grow` 的条件是"wakeup 落在窗口外但 `halt_ns < max_halt_poll_ns`"。
- `shrink` 对应 `halt_ns > max_halt_poll_ns` 或 `!vcpu_valid_wakeup()`。
- 函数出口无条件发 `trace_kvm_vcpu_wakeup(halt_ns, waited, valid)`（`:3880`）。

---

## C22: `-d kvm` 不是合法的 QEMU 日志项

**错误**：`vcpu-exit-diagnosis.md` §6、`practice/ex3-*` 让人用
`qemu-system-x86_64 ... -D /tmp/qemu.log -d kvm` 看 KVM_EXIT 原因。

**正确**：QEMU 10.1.0-rc2 的 `qemu_log_items[]`（`util/log.c:476-527`）里没有 `kvm`；
未知项使 `qemu_str_to_log_mask()` 返回 0，`system/vl.c:2609-2613` 打印 usage 后
`exit(1)` —— **VM 根本起不来**（本机 QEMU 8.2.2 实测同样 exit 1）。
KVM 退出信息本来就是直接打 **stderr**：
`accel/kvm/kvm-all.c:2858` `"KVM internal error. Suberror: %d"`、
`target/i386/kvm/kvm.c:6098` `"KVM: entry failed, hardware error 0x%" PRIx64`。
要落文件：shell 层 `2> /tmp/qemu.err`，或用 trace event
`-d trace:kvm_run_exit`（`accel/kvm/kvm-all.c:3242`，`trace:` 前缀由
`util/log.c:543-545` 特殊处理）。

---

## C23: `halt_poll_ns` 默认值抄错 + 违反"默认值单一来源"

**错误**：`performance-analysis.md` §6.2 写 `halt_poll_ns` "默认 400000 (400μs)"、
`halt_poll_ns_shrink` "默认 0（一次失手就归零）"。

**正确**：6.12.93 源码里 `halt_poll_ns = KVM_HALT_POLL_NS_DEFAULT`
（`virt/kvm/kvm_main.c:78`），x86 该常量是 **200000 = 200µs**
（`arch/x86/include/asm/kvm_host.h:71`），宿主实测也是 200000；
`halt_poll_ns_shrink` 源码默认是 **2**（`kvm_main.c:92-93`，注释
"Default halves per-vcpu halt_poll_ns"），本机实读的 0 是运行值、不是默认值。
按 CLAUDE.md 的规则，本章不该抄默认值 —— 已改成只给"存在吗 / 运行时可改吗 + file:line"
并指向 `../phase10-performance/parameters.md` §1。

---

## C24: `launch-failures.md` 四处与源码/机器环境不符

1. **`system_state == SYSTEM_HALTING` 不存在**：实际判据是
   `SYSTEM_HALT || SYSTEM_POWER_OFF || SYSTEM_RESTART`
   （`virt/kvm/kvm_main.c:5722-5723`；枚举 `include/linux/kernel.h:183-192`）。
2. **`/proc/sys/kernel/system_state` 不存在**（内核没注册这个 sysctl；本机实测
   `No such file or directory`），诊断步骤已换成 `dmesg` 间接判断。
3. **`dump_vmcs()` 打印标签**：是 `gh_mask=`（不是 `host_mask=`）、
   `CPUBased=/SecondaryExec=/TertiaryExec=`（不是 `ProcBased=/Proc2Based=`）、
   `PinBased=/EntryControls=/ExitControls=`、`ExceptionBitmap=`、
   `VMExit: ... reason=%08x qualification=%016lx`
   —— **不会打印 `EXIT_REASON_*` 符号名**（`arch/x86/kvm/vmx/vmx.c:6272`、`:6366` 区段）。
   样例 `KVM: VMCS invalid, dumping:` / `VM_EXIT_CONTROLS:` / `GUEST_CR0:` 均非真实输出，
   真实首行是 `VMCS %p, last attempted VM-entry on CPU %d`（`:6269`）。
4. **FAIL_ENTRY 的症状不是 `KVM_RUN failed: Argument list too long`**：
   `KVM_EXIT_FAIL_ENTRY` 经 `run->exit_reason` 上报、ioctl 返回 0
   （`vmx.c:6510-6515`），QEMU 打 `KVM: entry failed, hardware error 0x…`
   （`target/i386/kvm/kvm.c:6096-6099`）。`-E2BIG` 在 KVM 里另有出处
   （如 dirty ring 尺寸超限，`kvm_main.c:4970-4972`）。

另外修正了一批 ±1~4 行的行号漂移（`kvm_arch_alloc_vm` 1148、`get_unused_fd_flags` 5499、
`anon_inode_getfile` 5511、create_vcpu 链 4250/4257/4266/4271、`x86.c` 12380/12384/12434、
`vmx.c` PML 7536、`dump_vmcs` 区间 6239-6430），以及 `KVM_MAX_VCPUS`/`KVM_MAX_VCPU_IDS`
的实际位置（`kvm_host.h:49-54` 与 `:65-66`，两组 define 不相邻）。

---

## C25: `case-studies.md` 三处机制结论错

1. **案例1**：KVM 对 memslot 只要求 **PAGE_SIZE** 对齐
   （`virt/kvm/kvm_main.c:2007-2015` 检查 `memory_size` / `guest_phys_addr` /
   `userspace_addr`），未 2MB 对齐不是错误、只会拿不到大页映射；
   且 guest RAM 在 `/proc/<qemu>/maps` 里是**匿名 `rw-p`**，
   `/dev/kvm` 不出现在 maps（实测只有 `anon_inode:kvm-vcpu:<n>` 的 run 结构）。
2. **案例2**：masterclock 翻转与 cpufreq **无关**。判据是
   `nr_vcpus_matched_tsc + 1 == online_vcpus` **且**宿主 vclock_mode 基于 TSC
   （`arch/x86/kvm/x86.c:2521-2528`、`pvclock_update_vm_gtod_copy()` `:3023-3036`）。
   invariant TSC 下 cpufreq 不改 TSC 计数速率，KVM 也没有读 cpufreq 的代码；
   guest 也不会因此切换 clocksource。另：`tsc_adjust`
   （`X86_FEATURE_TSC_ADJUST`，`cpufeatures.h:243`）**不等于**支持 TSC scaling，
   后者是 VMX secondary control（`cpu_has_vmx_tsc_scaling()`，
   `arch/x86/kvm/vmx/capabilities.h:262-266`）。invariant TSC 的 cpuinfo 判据是
   `constant_tsc` + `nonstop_tsc`，`tsc_reliable` 是 quirk 位（本机没有）。
3. **案例3**：**QEMU `kernel-irqchip` 默认是 `on`，不是 `split`**
   （`qemu-options.hx:231` "(default=on)"；未指定时取
   `mc->default_kernel_irqchip_split`，`accel/kvm/kvm-all.c:2784-2786`；
   q35 当前机型设 false，`hw/i386/pc_q35.c:364`，只有 `pc-q35-4.0` 设 true，`:589`）。
   并且 **VT-d Posted Interrupts 只作用于经 IRTE 的直通设备**
   （`kvm_pi_irte_update` 仅在 `arch/x86/kvm/vmx/posted_intr.c:322`、
   `arch/x86/kvm/svm/avic.c:1003` 发出），模拟/vhost virtio 没有 IRTE，
   切 irqchip 模式既不会"打开 PI"，也解释不了 `EXTERNAL_INTERRUPT` 高占比——
   后者是宿主物理中断落在跑 guest 的 pCPU 上，必然先 VM-Exit。
   案例里 `perf kvm stat` 的阈值也与 §2.1 统一（旧写 "< 10%"）。

---

## C26: 文档内部交叉引用失效

`README.md` 与 `index.md` 大量指向 `annotations.md §2/§3.x/§4.x/§5/§6.x` 的条目失效：
`annotations.md` 实际章节是 §1 入口退出、§2 中断、§3 内存、§4 时钟、§5 性能调优、
§6 tracefs 全局状态、§7 CPU 操作、§8 perf kvm stat、§9 debugfs、§10 bpftrace、
§11 嵌套、§12 调用链，**没有 ftrace function_graph / trace_pipe / KVM selftests 章节**。
同时修正：`practice/ex*` 里 `../../scripts/vm/`、`../../phase10-performance/...` 少一层
（应为 `../../../`）；`index.md` 的问题索引 §1/§2/§5/§6 指向改为
`launch-failures §3.1-§3.3`、`vcpu-exit §4.1/§4.2/§5/§6`；
`README.md:36`/`index.md:13` 的"11 条常见错误"改为实际条数。

---

## 总结

| # | 错误 | 正确 | 源码 |
|---|------|------|------|
| C1 | `lapic_timer_advance_ns` 是模块参数 | 不是；是 per-vCPU debugfs 只读文件 | `lapic.c:70-71`, `debugfs.c:67` |
| C2 | `stats` 文件可以 grep | 是二进制格式 | `kvm_main.c:6352` |
| C3 | `echo > set_event` 只加一个 | `>` 会清掉全部事件 | `trace_events.c:2411→2422-2423` |
| C4 | `tracing_on=0` 等于零开销 | probe 仍注册着 | `trace.c` |
| C5 | `perf kvm stat -p` 能跟踪 vCPU | 必须 `-a` system-wide | `builtin-kvm.c:1959-1960` |
| C6 | `exit_reason` 在 trace 里是数字 | trace 里是符号名，BPF 里是数字 | `trace.h:289-295, :325-330` |
| C7 | `nx_huge_pages` 是布尔 | 只收 `off/force/auto/never` | `mmu.c:7259→7268-7284` |
| C8 | `kvm_track_tsc` 的 masterclock 是当前值 | 是翻转前的旧值 | `x86.c:2515-2526` |
| C9 | `kvm_vcpu_wakeup` 有 `vcpu_id` | 没有，只有 `ns, waited, valid` | `kvm.h:43, :47-51` |
| C10 | `kvm_pml_full` 有 `full_count` | 没有，只有 `vcpu_id` | `trace.h:963, :967-969` |
| C11 | `kvm_nested_vmexit` 有 `l1_rsp` | 没有，参数与 `kvm_exit` 相同 | `trace.h:679, :297-331` |
| C12 | 标题/跨 phase 引用沿用旧编号 | Phase 11 / 新编号 | 见 `COURSE_FRAMEWORK.md` §1.2 |
| C13 | `TRIPLE_FAULT=3`、`MSR_WRITE=28` | 2、32（28 是 `CR_ACCESS`） | `vmx.h:34, :57, :61` |
| C14 | 12 个事件都带 `vcpu_id` | 都没有；用 `common_pid` | `trace.h` / `kvm.h` + 宿主 format |
| C15 | 中断/IOAPIC/PIC 字段名（irq/vector/level/pir…） | `vec/gvec/apicid/irqchip/pin/e/…` | 同 C14 各表 |
| C16 | `kvm_write_tsc_offset=offset`、`wait_lapic_expire` 4 字段 | `previous/next_tsc_offset`、只有 `vcpu_id,delta` | `trace.h:879, :1035` |
| C17 | `kvm_nested_vmenter` 有 `vmcs/l2_rsp` 等 | `vmcb/nested_rip/…`；`intr_vmexit` 只有 `rip` | `trace.h:603, :684, :719, :1677` |
| C18 | grep `msr=0x`、`grow=\d+`、`use_master_clock 0`、`exception=\w+` | 照 `TP_printk` 写，否则永远空 | `trace.h:446, :922, :951`、`kvm.h:359` |
| C19 | `report --sort`、`record -g`、"`-p` 丢 vCPU 线程" | `--key`；`-g` 被禁用；`-p` 含全部线程 | `builtin-kvm.c:1786-1800, :1766-1767`、`thread_map.c:34-58` |
| C20 | debugfs 有 `stats` 文件、函数 `kvm_stat_per_vm()` | 逐项文件；真名 `kvm_init_debug`/`kvm_create_vm_debugfs` | `kvm_main.c:1092, :6352, :5121-5139` |
| C21 | 轮询命中会发 `kvm_halt_poll_ns` | 命中不发；grow=窗口外但未超 max | `kvm_main.c:3686, :3705, :3865-3866` |
| C22 | `-d kvm` 可看 KVM 退出 | 非法日志项，QEMU 直接 exit 1 | `util/log.c:476-527`、`system/vl.c:2609-2613` |
| C23 | `halt_poll_ns` 默认 400000、shrink 默认 0 | 200000；源码默认 2 | `kvm_host.h:71`、`kvm_main.c:78, :92-93` |
| C24 | `SYSTEM_HALTING`、`/proc/sys/kernel/system_state`、`host_mask`、"Argument list too long" | 三态枚举；无该 sysctl；`gh_mask`；`KVM: entry failed, hardware error` | `kvm_main.c:5722-5723`、`vmx.c:6272, :6366`、`kvm.c:6096-6099` |
| C25 | memslot 要 2MB 对齐；cpufreq 导致 masterclock 翻转；QEMU 默认 split | 页对齐即可；offset 匹配 + clocksource；默认 on | `kvm_main.c:2007-2015`、`x86.c:2521-2528`、`qemu-options.hx:231` |
| C26 | README/index 指向 annotations 不存在的 §4.x/§6.x/§5 | 按实际 §8/§9/§10 等改正 | `grep -n "^## " annotations.md` |
