# Phase 10：KVM 运行时调试与诊断

> 基于 Linux 6.12.93 源码 | 面向 KVM 运行时诊断的场景指南

---

## 📋 阶段概述

本阶段聚焦 **KVM 内核侧的运行时调试** —— 当 VMM 拉起失败、vCPU 异常退出、或运行时性能不及预期时，如何用 ftrace / perf / bpftrace / debugfs 定位根因。

> **定位**：本章是 **场景驱动的诊断手册**，不是工具参考。工具细节（trace events 完整目录、bpftrace 脚本集）在 `annotations.md`。

---

## 📂 本章文件

| 文件 | 内容 | 何时阅读 |
|------|------|---------|
| `README.md` | 本文件：场景导航 + 快速参考 | 入口 |
| [`annotations.md`](annotations.md) | KVM trace events 完整目录 + bpftrace 脚本集 | 需要具体 tracepoint 参数或脚本时 |
| [`corrections.md`](corrections.md) | 11 条常见错误（模块参数、trace 字段、tracefs 用法） | **写脚本前先扫一遍** |
| [`launch-failures.md`](launch-failures.md) | VM 启动失败诊断 | KVM_CREATE_VM/VCPU/RUN 失败 |
| [`vcpu-exit-diagnosis.md`](vcpu-exit-diagnosis.md) | vCPU 异常退出诊断 | VM 运行中崩溃、vCPU stopped |
| [`performance-analysis.md`](performance-analysis.md) | 运行时性能瓶颈定位 | VM 性能不及预期 |
| [`case-studies.md`](case-studies.md) | 3 个端到端诊断案例 | 完整诊断流程示例 |
| [`index.md`](index.md) | 主题索引 | 按问题/工具快速定位 |
| [`practice/`](practice/README.md) | 3 个场景驱动实验 | 动手练习 |

---

## 📖 学习目标

完成本阶段后，你应该能够：

1. 用 ftrace function tracer 跟踪 KVM ioctl 链路（`kvm_dev_ioctl` → `kvm_vm_ioctl` → `kvm_vcpu_ioctl`）
2. 根据 `KVM_EXIT_FAIL_ENTRY` / `KVM_EXIT_INTERNAL_ERROR` / `KVM_EXIT_SHUTDOWN` 定位启动失败根因
3. 使用 `dump_invalid_vmcs` 模块参数导出 VMCS 状态
4. 分析 `kvm_exit` tracepoint 的 `exit_reason`，判断 VM-Exit 分布是否异常
5. 用 `kvm_inj_exception` 跟踪异常注入链路，诊断 triple fault
6. 用 `perf kvm stat` 分析 VM-Exit 分布并给出优化建议
7. 用 bpftrace 测量 VM-Exit 处理延迟
8. 用 `kvm_page_fault` tracepoint 分析 EPT 缺页热点
9. 用 `kvm_halt_poll_ns` 跟踪 halt-polling 窗口自适应
10. 区分 `>` 与 `>>` 在 `set_event` 上的行为差异（见 `corrections.md` C3）
11. 知道 `kvm_exit` 的 `exit_reason` 在 trace 文本里是符号名、在 BPF 里是数字（见 `corrections.md` C6）
12. 知道 `perf kvm stat` 必须 `-a` system-wide，否则丢 vCPU 线程数据（见 `corrections.md` C5）

---

## 🎯 快速导航

| 问题 | 章节 | 工具 |
|------|------|------|
| VM 启不起来？ | `launch-failures.md` | ftrace + dump_invalid_vmcs |
| vCPU 异常退出？ | `vcpu-exit-diagnosis.md` | `kvm_exit` + `kvm_inj_exception` |
| VM-Exit 太多？ | `performance-analysis.md` §2 | `perf kvm stat` |
| 中断延迟高？ | `performance-analysis.md` §5 | bpftrace + `kvm_entry/exit` |
| 内存性能差？ | `performance-analysis.md` §4 | bpftrace `kvm_page_fault` |
| vCPU 调度抖动？ | `performance-analysis.md` §5 | bpftrace `sched_switch` |
| TSC 不同步？ | `performance-analysis.md` §7 | `kvm_track_tsc` |
| halt-polling 低效？ | `performance-analysis.md` §6 | `kvm_halt_poll_ns` |
| Triple fault？ | `vcpu-exit-diagnosis.md` §3 | `kvm_inj_exception` |

---

## 🔍 调试场景速查

> **共用前提**：
>
> 1. **`>` 与 `>>` 在 `set_event` 上不等价**。`>` 带 `O_TRUNC` 会先清掉所有已启用事件（`kernel/trace/trace_events.c:2411→2422-2423`）。只想追加用 `>>`，要清空显式写 `: > set_event`。详见 `corrections.md` C3。
> 2. **tracefs 是全局状态**，收尾要分别清：`current_tracer`、`set_ftrace_filter`、`set_event`、`tracing_on`。`tracing_on=0` **不等于零开销**。详见 `corrections.md` C4。

### 场景 1: VM-Exit 频率过高

```bash
# ★ 必须 -a system-wide（见 corrections.md C5）
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report

# 针对性处理（详细分析见 performance-analysis.md §2）
# EXTERNAL_INTERRUPT 多 → APICv / Posted Interrupts (phase4)
# EPT_VIOLATION 多 → 大页配置 (phase9 E2)
# CPUID 多 → VMM 预填充 CPUID 缓存
# IO_INSTRUCTION 多 → vhost 优化 (phase5)
```

### 场景 2: 中断延迟异常

```bash
# 跟踪中断路径
echo kvm:kvm_entry >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_inj_virq >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_apic_accept_irq >> /sys/kernel/debug/tracing/set_event

# 测量延迟（详细脚本见 annotations.md §6.1）
sudo bpftrace -e '
kprobe:vmx_handle_exit { @start[tid] = nsecs; }
kretprobe:vmx_handle_exit {
    @latency = hist(nsecs - @start[tid]);
    delete(@start[tid]);
}
interval:s:5 { print(@latency); clear(@latency); }
'

# 检查 APICv/PI（都是 0444 只读）
cat /sys/module/kvm_intel/parameters/enable_apicv   # vmx.c:114
cat /sys/module/kvm_intel/parameters/enable_ipiv    # vmx.c:117
```

### 场景 3: 内存性能差

```bash
# 跟踪 EPT 缺页
echo kvm:kvm_page_fault >> /sys/kernel/debug/tracing/set_event

# 分析缺页分布（按 2MB 对齐）
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @gpa[args->fault_address >> 21] = count();
}
interval:s:5 { print(@gpa, 20); clear(@gpa); }
'

# 大页/脏页测量：phase9-performance/practice/bench-huge-dirty.md（E2）
```

### 场景 4: vCPU 调度抖动

```bash
echo sched:sched_switch >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_entry >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event

# 分析抢占（详细脚本见 performance-analysis.md §5.3）
sudo bpftrace -e '
tracepoint:kvm:kvm_exit { @exit_time[tid] = nsecs; }
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
echo kvm:kvm_track_tsc >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_write_tsc_offset >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_update_master_clock >> /sys/kernel/debug/tracing/set_event

# ★ kvm_track_tsc 的 masterclock 字段是翻转前的旧值（见 corrections.md C8）
# ★ 看新值用 kvm_update_master_clock

# 验证 invariant TSC
grep "constant_tsc\|tsc_reliable" /proc/cpuinfo
```

### 场景 6: 启动失败

```bash
# 启用 VMCS dump
echo 1 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs

# ftrace 跟踪 ioctl 链路
TRACEFS=/sys/kernel/debug/tracing
: > $TRACEFS/set_event
echo function > $TRACEFS/current_tracer
{ echo kvm_dev_ioctl; echo kvm_create_vm; echo kvm_vm_ioctl; echo kvm_vcpu_ioctl;
  echo kvm_arch_vcpu_ioctl_run; echo vcpu_enter_guest; echo vmx_vcpu_run;
} > $TRACEFS/set_ftrace_filter
echo 1 > $TRACEFS/tracing_on

# 启动 VM 后查看
cat $TRACEFS/trace | grep -E 'kvm_(dev_|vm_|vcpu_)ioctl'
dmesg | grep -A30 'VMCS'   # 如果 dump_invalid_vmcs 启用

# 详细诊断：launch-failures.md
```

---

## 🌳 调试决策树

```
开始调试
    │
    ├─ VM 启不起来？
    │   ├─ /dev/kvm 不存在 → 检查 KVM 模块 + BIOS VT-x
    │   ├─ KVM_CREATE_VM 失败 → launch-failures.md §1
    │   ├─ KVM_CREATE_VCPU 失败 → launch-failures.md §2
    │   └─ KVM_RUN 失败
    │       ├─ FAIL_ENTRY → dump_invalid_vmcs → launch-failures.md §3.1
    │       ├─ INTERNAL_ERROR → vcpu-exit-diagnosis.md §2
    │       └─ SHUTDOWN → vcpu-exit-diagnosis.md §3
    │
    ├─ vCPU 运行中停止？
    │   ├─ KVM_EXIT_SHUTDOWN → vcpu-exit-diagnosis.md §3 (triple fault)
    │   ├─ KVM_EXIT_INTERNAL_ERROR → vcpu-exit-diagnosis.md §2
    │   └─ 高频 VM-Exit → performance-analysis.md
    │
    ├─ 性能问题？
    │   ├─ VM-Exit 太多 → perf kvm stat → performance-analysis.md §2
    │   ├─ 延迟高 → bpftrace vmx_handle_exit → performance-analysis.md §3
    │   ├─ 内存慢 → bpftrace kvm_page_fault → performance-analysis.md §4
    │   ├─ 中断延迟 → bpftrace kvm_inj_virq → performance-analysis.md §5
    │   └─ 调度抖动 → bpftrace sched_switch → performance-analysis.md §5.3
    │
    ├─ 时钟问题？
    │   ├─ 时间跳变 → kvm_update_master_clock → performance-analysis.md §7
    │   └─ TSC 不同步 → kvm_track_tsc → performance-analysis.md §7
    │
    └─ 测试新功能？
        └─ KVM selftests → annotations.md §5
```

---

## 🔧 工具选择指南

| 场景 | 推荐工具 | 优势 | 文档 |
|------|---------|------|------|
| VM-Exit 分布统计 | `perf kvm stat` | 开箱即用，输出清晰 | `annotations.md` §2 |
| 函数级调用跟踪 | ftrace function_graph | 显示完整调用栈 | `annotations.md` §4.2 |
| 自定义延迟测量 | bpftrace | 脚本化，灵活 | `annotations.md` §6 |
| 运行时状态查看 | debugfs | 直接读取 KVM 内部状态 | `annotations.md` §3 |
| VMCS 状态导出 | `dump_invalid_vmcs` | VM-Entry 失败时的关键信息 | `launch-failures.md` §4 |
| 异常注入跟踪 | `kvm:kvm_inj_exception` | triple fault 诊断 | `vcpu-exit-diagnosis.md` §5 |
| 回归测试 | KVM selftests | 自动化，可重复 | `annotations.md` §5 |
| 实时流式输出 | ftrace trace_pipe | 实时查看事件 | `annotations.md` §4.5 |

---

## 🔗 与其他 Phase 的关联

```
本阶段工具在各 phase 中的应用:

phase0-8 源码学习    → 用 ftrace 验证理解
phase9 性能测量      → 测量纪律与观测成本看 phase9-performance/measurement.md
                       本章提供场景诊断的工具和方法论
phase11 MicroVM     → 用 selftests 测试 MicroVM 功能

跨 phase 指针（不复制内容）：
- 模块参数默认值      → phase9-performance/parameters.md
- halt-polling 机制   → phase9-performance/annotations.md §1
- PLE 测量            → phase9-performance/practice/bench-ple.md (E1)
- 大页/脏页测量       → phase9-performance/practice/bench-huge-dirty.md (E2)
- 观测者成本          → phase9-performance/practice/bench-observer-cost.md (E5)
- APICv/PI 机制      → phase4-interrupts/posted-interrupts.md
- 时钟虚拟化机制     → phase7-timer-virt/
```

---

## ✅ 验证清单

完成本阶段后，确认能回答：

- [ ] 能列出 10 个以上常用的 KVM trace events 及用途
- [ ] 能用 `perf kvm stat` 分析 VM-Exit 分布并给出优化建议
- [ ] 能用 bpftrace 编写自定义的延迟测量脚本
- [ ] 能用 ftrace function_graph 跟踪 `vcpu_run` 的完整路径
- [ ] 能从 debugfs 读取 KVM 运行时统计
- [ ] 能编译并运行 KVM selftests
- [ ] 能根据调试决策树快速定位问题
- [ ] 知道 `>` 与 `>>` 在 `set_event` 上的差异（C3）
- [ ] 知道 `kvm_exit` 的 `exit_reason` 在 trace 里是符号名（C6）
- [ ] 知道 `perf kvm stat` 必须 `-a` system-wide（C5）
- [ ] 知道 `dump_invalid_vmcs` 默认关闭，需要手动启用
- [ ] 能区分 `KVM_EXIT_FAIL_ENTRY` / `INTERNAL_ERROR` / `SHUTDOWN` 的诊断路径

---

## 📖 推荐阅读顺序

1. **入门**：读本页 → 了解整体结构
2. **避坑**：扫 `corrections.md` → 避免常见错误
3. **动手**：做 `practice/e1-ftrace-diagnosis` → 练习 ftrace 诊断
4. **深入**：按需求选读 `launch-failures.md` / `vcpu-exit-diagnosis.md` / `performance-analysis.md`
5. **综合**：读 `case-studies.md` → 端到端案例
6. **参考**：`annotations.md` → trace events 完整目录 + bpftrace 脚本集
