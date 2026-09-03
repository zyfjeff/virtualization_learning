# Phase 10：KVM 调试与测试

> 基于 Linux 6.12.93 源码 | 面向 MicroVM 开发者的实战指南

---

## 📚 阶段概述

本阶段提供 KVM 调试和测试的完整参考，包括：

1. **完整 KVM trace events 目录** — 按分类列出所有可用事件及参数
2. **perf kvm stat** — VM-Exit 统计分析
3. **ftrace 高级用法** — 函数跟踪、过滤、组合
4. **debugfs 接口** — KVM 运行时状态查看
5. **KVM selftests** — 内核自带测试框架
6. **bpftrace 脚本集** — 高级性能分析

---

## 🎯 快速导航

| 问题 | 章节 | 工具 |
|------|------|------|
| VM-Exit 太多，原因不明？ | 1 + 2 | trace events + perf kvm stat |
| 中断延迟高？ | 1 + 6 | kvm_entry/kvm_exit + bpftrace |
| 内存性能差？ | 1 + 3 | kvm_page_fault + ftrace function_graph |
| vCPU 调度问题？ | 1 | kvm_vcpu_wakeup + sched:sched_switch |
| TSC 不同步？ | 1 | kvm_track_tsc + kvm_write_tsc_offset |
| 新功能测试？ | 5 | selftests |
| 热路径分析？ | 6 | bpftrace |

---

## 🔍 调试场景速查表

> **下面所有场景共用的两条前提**
>
> 1. **`>` 与 `>>` 在 `set_event` 上不等价**。带 `O_TRUNC` 的写打开会先把**所有**已启用
>    事件清掉（`tee` 默认就是 `O_TRUNC`），所以 `echo evt > set_event` 会顺手关掉别人挂的
>    探针；只想加就用 `>>`，要清空就显式写 `: > set_event`。源码链路见
>    `../phase9-performance/measurement.md` §5 第 3 条。
> 2. **tracefs 是全局状态**，收尾要**四个出口分别清**：`current_tracer`、
>    `set_ftrace_filter`、`set_event`、`tracing_on`（`kprobe_events` 另有其一，共五条），
>    它们互相独立，只清一个不算清干净。另注意 `tracing_on=0` **不等于零开销**
>    （probe 仍注册着，只是不写 buffer）。完整清理与判据见
>    `../phase9-performance/measurement.md` §5 与
>    `../phase9-performance/practice/bench-observer-cost.md` §2.5 / §6。

### 场景 1: VM-Exit 频率过高

```bash
# Step 1: 使用 perf kvm stat 分析 VM-Exit 分布
#   ★ 必须 system-wide（-a）。用 `-p $QEMU_PID` 只跟踪被包裹的那个进程，
#     vCPU 线程的退出**全丢**：tools/perf/builtin-kvm.c:1959-1960 里
#     `if (target__none(&kvm->opts.target)) … system_wide = true;` ——
#     只有**不给 target** 时才自动 system-wide。判据与实测见
#     ../phase9-performance/measurement.md §7
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report

# Step 2: 针对性处理（去处按重写后的章节分工，phase9 已经没有这些机制章）
# 如果是 EPT_VIOLATION 多:
#   → 机制在 ../phase2-mem-virt/（EPT/TDP MMU）；大页 vs 4K 的**代价**怎么量，
#     见 ../phase9-performance/practice/bench-huge-dirty.md（E2）
# 如果是 EXTERNAL_INTERRUPT 多:
#   → APICv / Posted Interrupts 在 ../phase4-interrupts/（含 posted-interrupts.md）；
#     PI 的"零 VM-Exit"是硬件行为，规范依据 SDM 30.6 / VT-d 5.2.5
# 如果是 IO_INSTRUCTION 多:
#   → 参考 phase5 vhost 优化
# 如果是 CPUID 多:
#   → 使用 KVM_SET_CPUID2 预填充 CPUID 缓存
# 如果是 PAUSE 多:
#   → PV 自旋锁（guest 侧 kvm spinlock，见 ../phase0-kvm-framework/）；
#     PLE 参数 ple_gap / ple_window / ple_window_grow / ple_window_shrink /
#     ple_window_max **全部 0444 只读**（arch/x86/kvm/vmx/vmx.c:204-219），
#     运行时改不了，只能 insmod/内核启动参数传 + 重启 VM。
#     机制走读 ../phase9-performance/annotations.md §1，
#     "到底值多少钱"的测量设计 ../phase9-performance/practice/bench-ple.md（E1）
```

### 场景 2: 中断延迟异常

```bash
# Step 1: 跟踪中断路径
echo kvm:kvm_entry >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_inj_virq >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_apic_accept_irq >> /sys/kernel/debug/tracing/set_event

# Step 2: 测量延迟
sudo bpftrace -e '
kprobe:vmx_vcpu_run { @start[tid] = nsecs; }
kretprobe:vmx_vcpu_run {
    @latency = hist(nsecs - @start[tid]);
    delete(@start[tid]);
}
interval:s:5 { print(@latency); clear(@latency); }
'

# Step 3: 检查 APICv/PI 是否启用（两者都是 0444 只读，运行时改不了）
cat /sys/module/kvm_intel/parameters/enable_apicv   # arch/x86/kvm/vmx/vmx.c:114
cat /sys/module/kvm_intel/parameters/enable_ipiv    # arch/x86/kvm/vmx/vmx.c:117
```

### 场景 3: 内存性能差

```bash
# Step 1: 跟踪 EPT 缺页
echo kvm:kvm_page_fault >> /sys/kernel/debug/tracing/set_event

# Step 2: 分析缺页分布
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @gpa[args->fault_address >> 21] = count();
}
interval:s:5 { print(@gpa, 20); clear(@gpa); }
'

# Step 3: 检查是否启用大页
echo always > /sys/kernel/mm/transparent_hugepage/enabled

# Step 4: 大页/脏页开销的测量方法见 phase9-performance/practice/bench-huge-dirty.md（E2）
#   ★ 别照旧版 ept-bench.md 的实验 1 做：那条用 function tracer 跟踪缺页路径，
#     而缺页热点函数本身就在 µs 量级，观测开销与被测量同级 → 测到的是 tracer。
#     E2 改成只用 kvm:kvm_page_fault 事件 + 按 fault_address>>21 去重判级别。
sudo ./scripts/trace/trace-page-fault.sh -p $QEMU_PID -d 10
```

### 场景 4: vCPU 调度抖动

```bash
# Step 1: 跟踪 vCPU 调度
echo sched:sched_switch >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_entry >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event

# Step 2: 检查 vCPU pinning
taskset -c -p $VCPU_TID

# Step 3: 分析抖动
sudo bpftrace -e '
tracepoint:kvm:kvm_exit {
    @exit_time[tid] = nsecs;
}
tracepoint:sched:sched_switch /@exit_time[args->prev_pid]/ {
    $delta = nsecs - @exit_time[args->prev_pid];
    @preempt_us = hist($delta / 1000);
    delete(@exit_time[args->prev_pid]);
}
interval:s:5 { print(@preempt_us); clear(@preempt_us); }
'
```

### 场景 5: TSC 不同步

```bash
# Step 1: 跟踪 TSC 同步
echo kvm:kvm_track_tsc >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_write_tsc_offset >> /sys/kernel/debug/tracing/set_event

# Step 2: 检查主时钟状态
#   ★ `cat /sys/kernel/debug/kvm/*/stats` 是**无效命令**：6.12.93 的 KVM 统计
#     每项一个文本文件（`debugfs_create_file(pdesc->name, …)` 逐项注册，
#     virt/kvm/kvm_main.c:6352），那个叫 `stats` 的文件是**二进制**的头描述符，
#     grep 不出东西；而且统计项里**没有 clock 类**条目。
#     主时钟状态看 tracepoint（Step 1 那两条之外再加这一条）：
echo kvm:kvm_update_master_clock >> /sys/kernel/debug/tracing/set_event
grep kvm_update_master_clock /sys/kernel/debug/tracing/trace | tail
#   行里 masterclock 是**本次重算的新决定**：赋值与打印都在
#   `pvclock_update_vm_gtod_copy()`（arch/x86/kvm/x86.c:3015）里，
#   赋值在 x86.c:3034、`trace_kvm_update_master_clock()` 在 x86.c:3042。
#   ★ 别拿 kvm_track_tsc 那行的 masterclock 判翻转方向 —— 它打的是**翻转前的旧值**：
#   `kvm_track_tsc_matching()`（arch/x86/kvm/x86.c:2515）只算一个**局部变量**
#   （x86.c:2526）再发请求，`ka->use_master_clock` 全树只有 x86.c:3034 一处写。
#   照着读会稳定慢一拍。机制见 ../phase9-performance/annotations.md §3.1.1，
#   完整判据见 ../phase9-performance/practice/bench-clock-master.md（E4）

# Step 3: 验证 invariant TSC
grep "constant_tsc\|tsc_reliable" /proc/cpuinfo
```

### 场景 6: guest 时钟异常（跳变 / 降级）

```bash
# Step 1: 时间跳变检测（Guest 内持续采样）
while true; do date +%s.%N; sleep 0.1; done | \
    awk 'NR>1{d=$1-prev; if(d<0||d>0.2) print "JUMP:", d} {prev=$1}'

# Step 2: clocksource 降级检测（从 kvm-clock 降到 tsc/hpet 即异常信号）
dmesg | grep -i 'clocksource.*changed'

# Step 3: TSC 不稳定警告
dmesg | grep -i 'tsc.*unstable'

# Step 4: 宿主侧查时钟相关 tracepoint（6.12.93 均存在）
ls /sys/kernel/debug/tracing/events/kvm/ | grep -E 'clock|time|tsc'
#   用 `>>` 而不是 `>`，理由见本节开头第 1 条
echo kvm:kvm_track_tsc >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_write_tsc_offset >> /sys/kernel/debug/tracing/set_event
```

方法学指针：

- **测量纪律与扰动预算**（任何对比前先读）：`../phase9-performance/measurement.md`。
- `../phase9-performance/practice/timer-bench.md` 里 guest 侧 clocksource / cyclictest
  的**手法**仍可用，但其 **实验 6（调 `lapic_timer_advance_ns`）在 6.12.93 上根本不可执行**
  —— 该参数已不是模块参数，只剩 `lapic_timer_advance` 这个 bool
  （`arch/x86/kvm/lapic.c:70-71`），提前量变成**每 vCPU 的只读 debugfs 文件**
  （`arch/x86/kvm/debugfs.c:67`）。详见 `../phase9-performance/corrections.md` D1。
- 可执行的继任实验（masterclock 翻转判据）：
  `../phase9-performance/practice/bench-clock-master.md`（E4）。
- 时钟虚拟化机制与结论归 **phase7**，本章只留指针。

---

## 🌳 调试决策树

```
开始调试
    │
    ├── 性能问题？
    │   ├── VM-Exit 太多 → perf kvm stat → 针对性优化
    │   ├── 延迟高 → bpftrace 测量关键路径
    │   ├── CPU 占用高 → perf record 找热点函数
    │   └── 内存慢 → trace page fault, 检查大页
    │
    ├── 功能问题？
    │   ├── 启动失败 → dmesg, 检查 KVM 初始化
    │   ├── 中断丢失 → trace kvm_inj_virq, kvm_ack_irq
    │   └── 设备异常 → trace kvm_mmio, kvm_pio
    │
    ├── 稳定性问题？
    │   ├── 崩溃 → ftrace function_graph 找路径
    │   ├── 死锁 → ftrace 跟踪锁函数
    │   └── 数据损坏 → trace kvm_set_irq, kvm_set_user_memory_region
    │
    └── 测试新功能？
        └── KVM selftests → tools/testing/selftests/kvm/
```

---

## 🔧 工具选择指南

| 场景 | 推荐工具 | 优势 |
|------|---------|------|
| VM-Exit 分布统计 | perf kvm stat | 开箱即用，输出清晰 |
| 函数级调用跟踪 | ftrace function_graph | 显示完整调用栈 |
| 自定义延迟测量 | bpftrace | 脚本化，灵活 |
| 运行时状态查看 | debugfs | 直接读取 KVM 内部状态 |
| 回归测试 | KVM selftests | 自动化，可重复 |
| 实时流式输出 | ftrace trace_pipe | 实时查看事件 |

---

## 🔗 与其他 Phase 的关联

```
本阶段工具在各 phase 中的应用:

phase0-8 的源码学习 → 用 ftrace 验证理解
phase9 性能优化      → 测量纪律与观测扰动预算看 measurement.md，
                       五个独占实验看 practice/：PLE(E1) / 大页与脏页(E2) /
                       vCPU 迁移(E3) / 主时钟(E4) / 观测者成本(E5)；
                       本章提供的是它们的工具
phase11 MicroVM     → 用 selftests 测试 MicroVM 功能
```

---

## ✅ 验证清单

完成本阶段后，确认能回答：

- [ ] 能列出 10 个以上常用的 KVM trace events 及用途
- [ ] 能用 perf kvm stat 分析 VM-Exit 分布并给出优化建议
- [ ] 能用 bpftrace 编写自定义的延迟测量脚本
- [ ] 能使用 ftrace function_graph 跟踪 vcpu_run 的完整路径
- [ ] 能从 debugfs 读取 KVM 运行时统计
- [ ] 能编译并运行 KVM selftests
- [ ] 能根据调试决策树快速定位问题
