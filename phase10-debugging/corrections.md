# Phase 10 勘误

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

**规则唯一来源**：`../phase9-performance/measurement.md` §5 第 3 条

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
