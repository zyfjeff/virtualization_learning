# E2: perf + bpftrace 定位性能瓶颈

> 场景：VM 运行时 CPU 占用高、响应慢，定位是 VM-Exit 过多、中断延迟、还是内存问题

---

## 🎯 目标

学会使用 perf 和 bpftrace 定位 KVM 性能瓶颈。

**你将学会**：
1. 用 `perf kvm stat` 分析 VM-Exit 分布
2. 用 bpftrace 测量 VM-Exit 处理延迟
3. 用 `kvm_page_fault` tracepoint 分析 EPT 缺页热点
4. 用 `kvm_halt_poll_ns` 跟踪 halt-polling 窗口自适应

---

## 📋 前置条件

```bash
# 需要 bpftrace
which bpftrace || apt install bpftrace

# 启动测试 VM
cd ../../scripts/vm/
./boot-vm.sh ubuntu --memory 2G --cpus 2
```

---

## 🔬 诊断步骤

### Step 1: perf kvm stat 分析 VM-Exit 分布

```bash
# 记录 10 秒的 VM-Exit（★ 必须 -a system-wide）
sudo perf kvm stat record -a -- sleep 10

# 查看分布
sudo perf kvm stat report

# 预期输出:
#  VM-Exit Reason        Count    %
#  ──────────────────    ─────    ───
#  EXTERNAL_INTERRUPT    15234    45.2%
#  EPT_VIOLATION         8456     25.1%
#  CPUID                 3211     9.5%
#  HLT                   1024     3.0%
#  ...
```

**判读**：
- `EXTERNAL_INTERRUPT` > 40% → 考虑启用 APICv / Posted Interrupts（phase4）
- `EPT_VIOLATION` > 20% → 检查大页配置，参考 phase9 E2
- `CPUID` > 10% → VMM 应预填充 CPUID 缓存
- `HLT` 多 → halt-polling 可能低效，检查 `halt_poll_ns` 参数

### Step 2: bpftrace 测量 VM-Exit 处理延迟

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
    printf("=== VM-Exit 处理延迟分布 ===\n");
    print(@latency_us);
    clear(@latency_us);
}
'
```

**预期输出**：
```
=== VM-Exit 处理延迟分布 ===
@latency_us:
[0, 1]        5234 |@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@|
[2, 4]        1523 |@@@@@@@@@@@@@@|
[4, 8]         456 |@@@@|
[8, 16]        123 |@|
[16, 32]        34 |@|
```

**判读**：
- 大部分 < 2μs → 正常
- 长尾 > 10μs → 检查是否有锁竞争或内存分配

### Step 3: EPT 缺页热点分析

```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @gpa[args->fault_address >> 21] = count();  // 按 2MB 对齐
}
interval:s:10 {
    printf("=== EPT 缺页热点（2MB 粒度）===\n");
    print(@gpa, 20);
    clear(@gpa);
}
'
```

**判读**：
- 热点集中在少数 GPA → 可能是特定数据结构频繁访问
- 热点分散 → 考虑大页（`echo always > /sys/kernel/mm/transparent_hugepage/enabled`）

### Step 4: halt-polling 窗口自适应

```bash
echo kvm:kvm_halt_poll_ns >> /sys/kernel/debug/tracing/set_event
echo 1 > /sys/kernel/debug/tracing/tracing_on
sleep 5
echo 0 > /sys/kernel/debug/tracing/tracing_on

cat /sys/kernel/debug/tracing/trace | grep halt_poll | head -20
```

**判读**：
- 频繁 `SHRINK` → vCPU 唤醒延迟高，考虑增大 `halt_poll_ns`
- 频繁 `GROW` → halt-polling 效果好

### Step 5: vCPU 调度抖动

```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_exit { @exit_time[tid] = nsecs; }
tracepoint:sched:sched_switch /@exit_time[args->prev_pid]/ {
    $delta = nsecs - @exit_time[args->prev_pid];
    @preempt_us = hist($delta / 1000);
    delete(@exit_time[args->prev_pid]);
}
interval:s:10 {
    printf("=== vCPU 被抢占时间分布 ===\n");
    print(@preempt_us);
    clear(@preempt_us);
}
'
```

**判读**：
- 大部分 < 10μs → 正常
- 长尾 > 100μs → vCPU 线程被宿主调度器抢占，考虑 `taskset` pinning

---

## 📊 诊断决策树

```
性能差
    │
    ├─ VM-Exit 过多（perf kvm stat）
    │   ├─ EXTERNAL_INTERRUPT > 40% → APICv / Posted Interrupts (phase4)
    │   ├─ EPT_VIOLATION > 20% → 大页配置 (phase9 E2)
    │   ├─ CPUID > 10% → 预填充 CPUID 缓存
    │   └─ IO_INSTRUCTION 多 → vhost 优化 (phase5)
    │
    ├─ Exit 延迟高（bpftrace vmx_handle_exit）
    │   ├─ 大部分 < 2μs → 正常
    │   └─ 长尾 > 10μs → 检查锁竞争、内存分配
    │
    ├─ EPT 缺页热点（bpftrace kvm_page_fault）
    │   ├─ 集中在少数 GPA → 数据结构优化
    │   └─ 分散 → 启用大页
    │
    ├─ halt-polling 低效（kvm_halt_poll_ns trace）
    │   ├─ 频繁 SHRINK → 增大 halt_poll_ns
    │   └─ 频繁 GROW → 效果好，无需调整
    │
    └─ vCPU 调度抖动（bpftrace sched_switch）
        ├─ < 10μs → 正常
        └─ > 100μs → taskset pinning
```

---

## ⚠️ 注意事项

1. **perf kvm stat 必须 -a**：用 `-p $PID` 会丢 vCPU 线程数据
2. **bpftrace 需要 root**：`sudo bpftrace`
3. **tracefs 全局状态**：用 `>>` 追加事件，用 `: >` 清场
4. **性能影响**：bpftrace 开销小，但 function_graph 开销大

---

## 📚 参考资料

- 性能分析: `../../performance-analysis.md` §3
- KVM trace events: `../../annotations.md` §1
- bpftrace 脚本集: `../../annotations.md` §6
- halt-polling 机制: `../../phase9-performance/annotations.md` §1
