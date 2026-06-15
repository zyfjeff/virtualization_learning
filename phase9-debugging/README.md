# Phase 9：KVM 调试与测试

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

### 场景 1: VM-Exit 频率过高

```bash
# Step 1: 使用 perf kvm stat 分析 VM-Exit 分布
sudo perf kvm stat record -p $QEMU_PID -- sleep 10
sudo perf kvm stat report

# Step 2: 针对性处理
# 如果是 EPT_VIOLATION 多:
#   → 参考 phase8 EPT 优化章节，启用大页、A/D 位
# 如果是 EXTERNAL_INTERRUPT 多:
#   → 参考 phase8 APICv/Posted Interrupts
# 如果是 IO_INSTRUCTION 多:
#   → 参考 phase4 vhost 优化
# 如果是 CPUID 多:
#   → 使用 KVM_SET_CPUID2 预填充 CPUID 缓存
# 如果是 PAUSE 多:
#   → 调整 PLE 参数或启用 PV 自旋锁
```

### 场景 2: 中断延迟异常

```bash
# Step 1: 跟踪中断路径
echo kvm:kvm_entry > /sys/kernel/debug/tracing/set_event
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

# Step 3: 检查 APICv/PI 是否启用
cat /sys/module/kvm_intel/parameters/enable_apicv
cat /sys/module/kvm_intel/parameters/enable_apicv
```

### 场景 3: 内存性能差

```bash
# Step 1: 跟踪 EPT 缺页
echo kvm:kvm_page_fault > /sys/kernel/debug/tracing/set_event

# Step 2: 分析缺页分布
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @gpa[args->fault_address >> 21] = count();
}
interval:s:5 { print(@gpa, 20); clear(@gpa); }
'

# Step 3: 检查是否启用大页
echo always > /sys/kernel/mm/transparent_hugepage/enabled

# Step 4: 使用 phase7 项目 2 的测试脚本
sudo ./scripts/ftrace/trace-page-fault.sh -p $QEMU_PID -d 10
```

### 场景 4: vCPU 调度抖动

```bash
# Step 1: 跟踪 vCPU 调度
echo sched:sched_switch > /sys/kernel/debug/tracing/set_event
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
echo kvm:kvm_track_tsc > /sys/kernel/debug/tracing/set_event
echo kvm:kvm_write_tsc_offset >> /sys/kernel/debug/tracing/set_event

# Step 2: 检查主时钟状态
cat /sys/kernel/debug/kvm/*/stats | grep -i clock

# Step 3: 验证 invariant TSC
grep "constant_tsc\|tsc_reliable" /proc/cpuinfo
```

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

phase0-7 的源码学习 → 用 ftrace 验证理解
phase8 性能优化      → 用 perf/bpftrace 测量优化效果
phase10 MicroVM     → 用 selftests 测试 MicroVM 功能
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
