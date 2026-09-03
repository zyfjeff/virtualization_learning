# 运行时性能分析

> VM 性能不及预期时，如何用 KVM 侧工具定位瓶颈。

---

## 📋 概述

VM 性能问题通常归为四类：

1. **VM-Exit 过多** — vCPU 频繁退出到宿主，每次退出有 μs 级开销
2. **Exit 处理延迟高** — 单次 VM-Exit 处理时间异常
3. **内存性能差** — EPT 缺页频繁、大页未生效
4. **vCPU 调度抖动** — vCPU 线程在宿主上被抢占

本文按这四类展开诊断方法。

> **测量纪律**：任何性能对比前，先读 `../phase9-performance/measurement.md`。
> 观测者有成本 —— ftrace / perf / bpftrace 本身会改变被测量系统的行为。
> 观测成本的数量级和测量方法详见 `../phase9-performance/practice/bench-observer-cost.md`（E5）。

---

## 1. 诊断流程总览

```
VM 性能差
    │
    ├─ Step 1: perf kvm stat 看 VM-Exit 分布
    │   └─ 哪种 exit_reason 占比异常？
    │
    ├─ Step 2: bpftrace 测 VM-Exit 延迟
    │   └─ 是处理慢还是频率高？
    │
    ├─ Step 3: 针对性深入
    │   ├─ EXTERNAL_INTERRUPT 多 → §5 中断分析
    │   ├─ EPT_VIOLATION 多 → §4 内存分析
    │   ├─ CPUID 多 → §2.3 CPUID 缓存
    │   ├─ IO_INSTRUCTION 多 → §2.4 PIO/vhost
    │   └─ HLT 多 → §6 halt-polling
    │
    └─ Step 4: vCPU 调度分析 → §5 调度抖动
```

---

## 2. VM-Exit 分布分析

### 2.1 perf kvm stat

```bash
# ★ 必须 -a system-wide（见 corrections.md C5）
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report
```

**判读标准**（经验值，具体因工作负载而异）：

| exit_reason | 正常范围 | 异常信号 | 可能原因 |
|-------------|---------|---------|---------|
| EXTERNAL_INTERRUPT | < 20% | > 40% | APICv/PI 未启用，中断风暴 |
| EPT_VIOLATION | < 15% | > 30% | 大页未生效、内存访问分散 |
| CPUID | < 5% | > 10% | VMM 未预填充 CPUID 缓存 |
| IO_INSTRUCTION | < 5% | > 15% | 设备用 PIO 而非 MMIO |
| HLT | 视负载 | 视负载 | halt-polling 效率问题 |
| MSR_WRITE/READ | < 10% | > 20% | 频繁 MSR 访问 |

### 2.2 按 vCPU 分解

```bash
# 按 vCPU 线程分别统计
for tid in $(ls /proc/$(pgrep qemu)/task/); do
    echo "=== vCPU TID $tid ==="
    sudo perf kvm stat record -t $tid -- sleep 5 2>/dev/null
    sudo perf kvm stat report 2>/dev/null | head -8
done
```

如果某个 vCPU 的退出率远高于其他，可能是负载不均或中断集中在一个 vCPU。

### 2.3 CPUID 退出优化

CPUID 退出多说明 guest 频繁执行 CPUID 指令。VMM 应预填充 CPUID 缓存：

```bash
# 检查 CPUID 退出占比
sudo perf kvm stat report | grep CPUID
# 如果 > 10%:

# QEMU 侧：确保 CPUID 缓存已填充
# QEMU 默认会调用 KVM_SET_CPUID2 缓存所有支持的 CPUID 叶
# 如果仍然多，检查是否有自定义 CPUID 过滤

# 内核侧：kvm_emulate_cpuid() arch/x86/kvm/cpuid.c
# KVM 有自己的 CPUID 缓存（vcpu->arch.cpuid_entries）
# 如果缓存 miss，会退出到用户态
```

### 2.4 PIO 退出优化

IO_INSTRUCTION 多说明 guest 频繁使用端口 I/O：

```bash
# 检查哪些端口访问最频繁
sudo bpftrace -e '
tracepoint:kvm:kvm_pio {
    @ports[args->port] = count();
}
interval:s:10 {
    print(@ports, 10);
    clear(@ports);
}
'
```

常见热点端口：
- `0x3f8` / `0x2f8` — 串口（可用 `-serial null` 禁用）
- `0x70` / `0x71` — RTC
- `0xcf8` / `0xcfc` — PCI 配置空间

**优化方向**：
- 串口：如果不需要，用 `-serial null` 禁用
- 网卡：从 `e1000`（PIO）切换到 `virtio-net`（MMIO）
- 存储：从 IDE（PIO）切换到 virtio-blk（MMIO）

---

## 3. VM-Exit 延迟分析

### 3.1 bpftrace 测量 vmx_handle_exit 延迟

```bash
sudo bpftrace -e '
kprobe:vmx_handle_exit { @start[tid] = nsecs; }
kretprobe:vmx_handle_exit {
    if (@start[tid]) {
        @latency_us = hist((nsecs - @start[tid]) / 1000);
        delete(@start[tid]);
    }
}
interval:s:5 {
    printf("=== VM-Exit 处理延迟分布（μs）===\n");
    print(@latency_us);
    clear(@latency_us);
}
'
```

**判读**：
- 90% < 2μs → 正常
- 长尾 > 10μs → 检查锁竞争、内存分配、或特定 exit_reason 处理慢

### 3.2 按 exit_reason 分解延迟

```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_exit {
    @exit_time[tid] = nsecs;
    @exit_reason[tid] = args->exit_reason;
}
kprobe:vmx_handle_exit /@exit_time[tid]/ {
    $delta = nsecs - @exit_time[tid];
    $reason = @exit_reason[tid];
    @latency_by_reason[$reason] = hist($delta / 1000);
    delete(@exit_time[tid]);
    delete(@exit_reason[tid]);
}
interval:s:10 {
    print(@latency_by_reason);
    clear(@latency_by_reason);
}
'
# ★ args->exit_reason 是数字（见 corrections.md C6）
# 需要对照 arch/x86/include/uapi/asm/vmx.h 翻译
```

### 3.3 火焰图分析

```bash
# 记录调用栈
sudo perf kvm stat record -g -a -- sleep 30

# 生成火焰图（需要 FlameGraph 工具）
sudo perf kvm stat report --stdio | head -50

# 或用 perf script + FlameGraph
sudo perf script | ./stackcollapse-perf.pl | ./flamegraph.pl > kvm-exit.svg
```

火焰图可以直观看到哪些函数占据最多时间。

---

## 4. 内存性能分析

### 4.1 EPT 缺页热点

```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @gpa_2m[args->fault_address >> 21] = count();  // 按 2MB 对齐
    @gpa_4k[args->fault_address >> 12] = count();  // 按 4KB 对齐
}
interval:s:10 {
    printf("=== EPT 缺页热点（2MB 粒度，top 20）===\n");
    print(@gpa_2m, 20);
    printf("=== EPT 缺页热点（4KB 粒度，top 20）===\n");
    print(@gpa_4k, 20);
    clear(@gpa_2m);
    clear(@gpa_4k);
}
'
```

**判读**：
- 热点集中在少数 2MB 区域 → 大页可显著减少缺页
- 热点分散在多个 2MB 区域 → 大页效果有限，考虑数据布局优化

### 4.2 缺页级别统计

```bash
# 统计 4K / 2M / 1G 缺页的比例
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    $addr = args->fault_address;
    if ($addr & 0x1fffff)       // 非 2MB 对齐
        @level["4K"] = count();
    else if ($addr & 0x3fffffff) // 非 1GB 对齐
        @level["2M"] = count();
    else
        @level["1G"] = count();
}
interval:s:10 {
    print(@level);
    clear(@level);
}
'
```

理想情况：大部分缺页应为 2M 或 1G 级别。

### 4.3 大页配置检查

```bash
# 宿主侧：检查 THP 配置
cat /sys/kernel/mm/transparent_hugepage/enabled
# 推荐: always 或 madvise

# 检查大页可用性
cat /proc/meminfo | grep -i huge
# HugePages_Total / HugePages_Free / Hugepagesize

# Guest 侧：检查是否使用大页
cat /proc/meminfo | grep -i huge
# 如果 Guest 内核支持 THP，也会显示 HugePages

# 大页/脏页开销的测量方法：phase9-performance/practice/bench-huge-dirty.md（E2）
```

### 4.4 MMU notifier 跟踪

```bash
# 跟踪 mmu_notifier 事件（宿主内存管理通知 KVM 失效映射）
sudo bpftrace -e '
tracepoint:kvm:kvm_unmap_hva_range {
    @unmap_count = count();
    @unmap_ranges = hist(args->end - args->start);
}
tracepoint:kvm:kvm_age_hva {
    @age_count = count();
}
interval:s:10 {
    printf("mmu_notifier unmap: %d\n", @unmap_count);
    print(@unmap_ranges);
    printf("mmu_notifier age: %d\n", @age_count);
    clear(@unmap_count);
    clear(@unmap_ranges);
    clear(@age_count);
    clear(@age_count);
}
'
```

频繁的 mmu_notifier 事件说明宿主内存管理（如 THP compaction、page migration）在干扰 KVM 页表，可能导致性能下降。

---

## 5. 中断与调度分析

### 5.1 中断退出分析

```bash
# 统计 EXTERNAL_INTERRUPT 的频率
sudo bpftrace -e '
tracepoint:kvm:kvm_exit /args->exit_reason == 1/ {  // 1 = EXTERNAL_INTERRUPT
    @ext_int = count();
}
tracepoint:kvm:kvm_exit {
    @total = count();
}
interval:s:5 {
    if (@total > 0)
        printf("EXTERNAL_INTERRUPT: %d (%.1f%%)\n", @ext_int, 100.0 * @ext_int / @total);
    clear(@ext_int);
    clear(@total);
}
'
```

如果 EXTERNAL_INTERRUPT > 40%，考虑：
- 启用 APICv（`enable_apicv=Y`，`arch/x86/kvm/vmx/vmx.c:114`，0444 只读）
- 启用 Posted Interrupts（VT-d IR，`phase4-interrupts/posted-interrupts.md`）
- 检查 `enable_ipiv`（`vmx.c:117`，IPI 虚拟化）

### 5.2 中断注入延迟

```bash
# 测量从中断接受到注入的延迟
sudo bpftrace -e '
tracepoint:kvm:kvm_apic_accept_irq {
    @accept_time[args->vcpu_id, args->vector] = nsecs;
}
tracepoint:kvm:kvm_inj_virq /@accept_time[args->vcpu_id, args->irq]/ {
    $delta = nsecs - @accept_time[args->vcpu_id, args->irq];
    @inject_delay = hist($delta / 1000);  // μs
    delete(@accept_time[args->vcpu_id, args->irq]);
}
interval:s:10 {
    printf("=== 中断注入延迟（μs）===\n");
    print(@inject_delay);
    clear(@inject_delay);
}
'
```

### 5.3 vCPU 调度抖动

```bash
# 测量 vCPU 线程被宿主调度器抢占的时间
sudo bpftrace -e '
tracepoint:kvm:kvm_exit {
    @exit_time[tid] = nsecs;
}
tracepoint:sched:sched_switch /@exit_time[args->prev_pid]/ {
    $delta = nsecs - @exit_time[args->prev_pid];
    @preempt_us = hist($delta / 1000);
    delete(@exit_time[args->prev_pid]);
}
interval:s:10 {
    printf("=== vCPU 被抢占时间分布（μs）===\n");
    print(@preempt_us);
    clear(@preempt_us);
}
'
```

**判读**：
- 大部分 < 10μs → 正常
- 长尾 > 100μs → vCPU 线程被宿主调度器抢占

**优化**：
```bash
# 绑定 vCPU 到物理 CPU
taskset -c 0-3 -p $QEMU_PID

# 或使用 cgroup cpuset
echo 0-3 > /sys/fs/cgroup/cpuset/qemu/cpuset.cpus
```

---

## 6. halt-polling 分析

### 6.1 halt-polling 窗口跟踪

```bash
TRACEFS=/sys/kernel/debug/tracing
: > $TRACEFS/set_event
echo kvm:kvm_halt_poll_ns >> $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on
sleep 10
echo 0 > $TRACEFS/tracing_on

# 分析 GROW vs SHRINK 比例
cat $TRACEFS/trace | grep halt_poll | \
    grep -oP 'grow=\d+' | sort | uniq -c
# grow=1 是 GROW，grow=0 是 SHRINK
```

**判读**：
- GROW 远多于 SHRINK → halt-polling 效果好
- SHRINK 频繁 → vCPU 唤醒延迟高，考虑调整参数

### 6.2 halt-polling 参数

```bash
# 运行时可调（0644）
cat /sys/module/kvm/parameters/halt_poll_ns          # 默认 400000 (400μs)
cat /sys/module/kvm/parameters/halt_poll_ns_grow      # 默认 2
cat /sys/module/kvm/parameters/halt_poll_ns_grow_start # 默认 10000 (10μs)
cat /sys/module/kvm/parameters/halt_poll_ns_shrink    # 默认 0（一次失手就归零）

# 调优建议
echo 800000 > /sys/module/kvm/parameters/halt_poll_ns  # 增大窗口
echo 2 > /sys/module/kvm/parameters/halt_poll_ns_shrink # 收缩而非归零
```

### 6.3 halt-polling 开销测量

halt-polling 本身有开销（vCPU 线程在宿主上空转）。测量方法：

```bash
# 对比启用/禁用 halt-polling 的 VM-Exit 数
echo 0 > /sys/module/kvm/parameters/halt_poll_ns
sudo perf kvm stat record -a -- sleep 10
# 记录禁用时的退出数

echo 400000 > /sys/module/kvm/parameters/halt_poll_ns
sudo perf kvm stat record -a -- sleep 10
# 记录启用时的退出数

# 如果启用后退出数显著减少 → halt-polling 有效
# 如果 CPU 占用明显增加 → polling 成本高于收益
```

完整测量设计见 `../phase9-performance/practice/bench-ple.md`（E1）。

---

## 7. TSC 与时钟分析

### 7.1 TSC 同步状态

```bash
TRACEFS=/sys/kernel/debug/tracing
: > $TRACEFS/set_event
echo kvm:kvm_track_tsc >> $TRACEFS/set_event
echo kvm:kvm_update_master_clock >> $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on
sleep 10
echo 0 > $TRACEFS/tracing_on

# 检查 masterclock 状态
cat $TRACEFS/trace | grep 'update_master_clock'
# use_master_clock=1 表示 masterclock 启用
# use_master_clock=0 表示 masterclock 禁用（可能有 TSC 问题）

# ★ kvm_track_tsc 的 masterclock 字段是旧值（见 corrections.md C8）
# 要看新值，用 kvm_update_master_clock
```

### 7.2 TSC offset 检查

```bash
# 查看各 vCPU 的 TSC offset
for vcpu_dir in /sys/kernel/debug/kvm/*/vcpu*/; do
    offset=$(cat "$vcpu_dir/tsc-offset" 2>/dev/null)
    echo "$(basename $vcpu_dir): tsc-offset=$offset"
done
```

如果各 vCPU 的 offset 差异很大，说明 TSC 不同步。

### 7.3 invariant TSC 检查

```bash
# 宿主侧
grep -o 'constant_tsc\|nonstop_tsc\|tsc_reliable' /proc/cpuinfo | sort -u
# 三个都有 → invariant TSC，KVM 可以启用 masterclock

# Guest 侧
grep -o 'constant_tsc\|nonstop_tsc\|tsc_reliable' /proc/cpuinfo | sort -u
```

### 7.4 时钟跳变检测

```bash
# Guest 内持续采样
while true; do date +%s.%N; sleep 0.01; done | \
    awk 'NR>1{d=$1-prev; if(d<0||d>0.02) print "JUMP:", d, "at", $1} {prev=$1}'
```

正常情况：`d` 应在 0.01±0.005 范围。如果 `d` 偶尔 > 0.1 或 < 0，说明时钟跳变。

---

## 8. 综合诊断脚本

以下脚本自动执行上述分析：

```bash
#!/bin/bash
# kvm-perf-diagnosis.sh — 综合性能诊断
# 用法: sudo ./kvm-perf-diagnosis.sh [duration]

DURATION=${1:-10}
TRACEFS=/sys/kernel/debug/tracing

echo "=== KVM 性能诊断（${DURATION}s）==="

# 1. perf kvm stat
echo ""
echo "--- VM-Exit 分布 ---"
perf kvm stat record -a -- sleep $DURATION 2>/dev/null
perf kvm stat report 2>/dev/null | head -15

# 2. bpftrace 延迟
echo ""
echo "--- VM-Exit 延迟分布（μs）---"
timeout $((DURATION+2)) bpftrace -e '
kprobe:vmx_handle_exit { @s[tid] = nsecs; }
kretprobe:vmx_handle_exit { @l = hist((nsecs - @s[tid]) / 1000); delete(@s[tid]); }
interval:s:'$DURATION' { print(@l); exit(); }
' 2>/dev/null

# 3. EPT 缺页
echo ""
echo "--- EPT 缺页热点（2MB 粒度，top 10）---"
timeout $((DURATION+2)) bpftrace -e '
tracepoint:kvm:kvm_page_fault { @g[args->fault_address >> 21] = count(); }
interval:s:'$DURATION' { print(@g, 10); exit(); }
' 2>/dev/null

# 4. halt-polling
echo ""
echo "--- halt-polling 窗口变化 ---"
: > $TRACEFS/set_event
echo kvm:kvm_halt_poll_ns >> $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on
sleep $DURATION
echo 0 > $TRACEFS/tracing_on
grep halt_poll $TRACEFS/trace | grep -oP 'grow=\d+' | sort | uniq -c

# 5. 清理
: > $TRACEFS/set_event

echo ""
echo "=== 诊断完成 ==="
```

---

## 📚 参考

- KVM trace events 完整目录：`annotations.md` §1
- bpftrace 脚本集：`annotations.md` §6
- halt-polling 机制：`../phase9-performance/annotations.md` §1
- 测量纪律：`../phase9-performance/measurement.md`
- 参数默认值：`../phase9-performance/parameters.md`
- 大页与脏页测量：`../phase9-performance/practice/bench-huge-dirty.md`（E2）
- PLE 测量：`../phase9-performance/practice/bench-ple.md`（E1）
- 观测者成本：`../phase9-performance/practice/bench-observer-cost.md`（E5）
