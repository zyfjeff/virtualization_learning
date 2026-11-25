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

> **测量纪律**：任何性能对比前，先读 `../phase10-performance/measurement.md`。
> 观测者有成本 —— ftrace / perf / bpftrace 本身会改变被测量系统的行为。
> 观测成本的数量级和测量方法详见 `../phase10-performance/practice/bench-observer-cost.md`（E5）。

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
# ★ 用 -a system-wide（见 corrections.md C5；成因见 C19）
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
# ★ perf kvm stat record 显式禁用了 -g/--call-graph
#   （tools/perf/builtin-kvm.c:1766-1767 把它们标成 PARSE_OPT_DISABLED，
#    parse-options 会报 "is not usable"），要调用栈只能用普通 perf record：
sudo perf record -g -a -e kvm:kvm_exit -- sleep 30

# 上一步用的是普通 perf record，所以读数据也用普通 perf 子命令
sudo perf report --stdio | head -50

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

### 4.2 缺页"级别"：不能用 fault_address 对齐推

`kvm_page_fault` **没有 `level` 字段**（`trace.h:406-411` 只有
`vcpu_id / guest_rip / fault_address / error_code`，宿主 format 同）。
用 GPA 是否 2MB/1GB 对齐去"猜级别"是错的：一个 2MB 对齐的 GPA
完全可能因为脏页追踪、NX 拆分、`nx_huge_pages`、rmap 冲突而按 4K 建映射；
反过来非对齐地址的缺页也可能落在已存在的 2MB 大页里。
所以下面这段只能算**"访问是否落在大页边界上"的分布统计**，不是映射级别：

```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    $addr = args->fault_address;
    if ($addr & 0x1fffff)        @align["非 2MB 对齐"] = count();
    else if ($addr & 0x3fffffff) @align["2MB 对齐、非 1GB 对齐"] = count();
    else                         @align["1GB 对齐"] = count();
}
interval:s:10 { print(@align); clear(@align); }
'
```

要**真实映射级别**，走 MMU trace 系统 `kvmmmu:`（不是 `kvm:`）：

```bash
# kvmmmu:kvm_mmu_set_spte 带 u8 level 字段（arch/x86/kvm/mmu/mmutrace.h:334）
sudo bpftrace -e '
tracepoint:kvmmmu:kvm_mmu_set_spte { @level[args->level] = count(); }
interval:s:10 { print(@level); clear(@level); }
'
```

宿主实测 `events/kvmmmu/kvm_mmu_set_spte/format` 字段为
`gfn / spte / sptep / level / r / x / u`，其中 `level` 才是映射级别。
另可看 `/proc/<qemu-pid>/smaps` 的 `AnonHugePages` 判断后端是否真拿到了大页。

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

# 大页/脏页开销的测量方法：phase10-performance/practice/bench-huge-dirty.md（E2）
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
- 启用 Posted Interrupts（VT-d IR，`phase5-interrupts/posted-interrupts.md`）
- 检查 `enable_ipiv`（`vmx.c:117`，IPI 虚拟化）

### 5.2 中断注入延迟

> ★ 这两个事件**都没有 `vcpu_id` 字段**（`kvm_apic_accept_irq` =
> `apicid/dm/tm/vec`，`trace.h:546-551`；`kvm_inj_virq` =
> `vector/soft/reinjected`，`trace.h:345-349`），旧脚本里的
> `args->vcpu_id` / `args->irq` / `args->vector`（对 accept 而言）都会让
> bpftrace 直接报字段不存在。能用来配对的只有向量 + 事件公共字段
> `common_pid`（= 发事件的线程 TID）。

```bash
# 测量从中断接受到注入的延迟（按 TID + 向量配对）
sudo bpftrace -e '
tracepoint:kvm:kvm_apic_accept_irq {
    @accept[pid, args->vec] = nsecs;      // 字段是 vec，不是 vector
}
tracepoint:kvm:kvm_inj_virq /@accept[pid, args->vector]/ {   // 字段是 vector，不是 irq
    $delta = nsecs - @accept[pid, args->vector];
    @inject_delay_us = hist($delta / 1000);
    delete(@accept[pid, args->vector]);
}
interval:s:10 {
    printf("=== 中断注入延迟（μs）===\n");
    print(@inject_delay_us);
    clear(@inject_delay_us);
}
'
```

> 局限：`kvm_apic_accept_irq` 可能由别的线程（例如 ioapic 路由发起方）触发，
> 此时与 `kvm_inj_virq` 的 TID 不同，配对会漏；要精确归因 vCPU，
> 只能结合 `kvm_exit`/`kvm_entry` 的 `vcpu_id` 间接对齐。

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
# trace 文本形如：kvm_halt_poll_ns: vcpu 0: halt_poll_ns 20000 (grow 10000)
#   ↑ TP_printk 是 "vcpu %u: halt_poll_ns %u (%s %u)"，%s 直接印 "grow"/"shrink"
#     （include/trace/events/kvm.h:359-366）——**没有 grow=1 / grow=0 这种键**。
#     旧版 `grep -oP 'grow=\d+'` 永远抓不到东西。
cat $TRACEFS/trace | grep 'kvm_halt_poll_ns' | \
    grep -oP '\((grow|shrink) \d+\)' | awk '{print $1}' | sort | uniq -c
```

**判读**：
- GROW 远多于 SHRINK → halt-polling 效果好
- SHRINK 频繁 → vCPU 唤醒延迟高，考虑调整参数
- ★ 窗口内轮询**命中**时这个 tracepoint 不触发（`kvm_main.c:3865-3866` 是空分支），
  所以"事件数为 0"不等于"没在 poll"，要看 `halt_attempted_poll` /
  `halt_successful_poll` 两个 debugfs 计数（`kvm_main.c:3771-3774`）

### 6.2 halt-polling 参数

参数默认值与权限的唯一来源是 `../phase10-performance/parameters.md` §1，本节只给指针，
不复制数字（历史上这里抄错过一份默认值，见该文件 §1 的勘误）。

本章只需要知道两件事：

| 问题 | 答案 | 证据 |
|---|---|---|
| 这组参数存在吗？ | `halt_poll_ns` / `_grow` / `_grow_start` / `_shrink` 四个都在 `/sys/module/kvm/parameters/` | `virt/kvm/kvm_main.c:78-94` |
| 运行时可改吗？ | 可，四个都是 `0644` | 同上各行 `module_param(..., 0644)` |

```bash
# 读当前值（当前值 ≠ 6.12.93 的默认值，宿主可能是别的内核）
cat /sys/module/kvm/parameters/halt_poll_ns
# 改值（示例；改前先按 phase10 的规矩记录基线）
echo 0 > /sys/module/kvm/parameters/halt_poll_ns        # 关掉 halt-polling
```

> 语义提示（与默认值无关，来自 `kvm_main.c:3689-3706`）：
> `halt_poll_ns_shrink == 0` 时窗口直接被清零；非 0 时按除法收缩，
> 且结果低于 `halt_poll_ns_grow_start` 也归零。

### 6.3 halt-polling 开销测量

halt-polling 本身有开销（vCPU 线程在宿主上空转）。测量方法：

```bash
# 对比启用/禁用 halt-polling 的 VM-Exit 数
echo 0 > /sys/module/kvm/parameters/halt_poll_ns
sudo perf kvm stat record -a -- sleep 10
# 记录禁用时的退出数

echo 200000 > /sys/module/kvm/parameters/halt_poll_ns   # 用一个非 0 窗口重新启用
sudo perf kvm stat record -a -- sleep 10
# 记录启用时的退出数

# 如果启用后退出数显著减少 → halt-polling 有效
# 如果 CPU 占用明显增加 → polling 成本高于收益
```

完整测量设计见 `../phase10-performance/practice/bench-ple.md`（E1）。

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

# 检查 masterclock 状态：trace 文本里没有 "use_master_clock=1" 这种键！
cat $TRACEFS/trace | grep 'update_master_clock'
#   kvm_update_master_clock: masterclock 1 hostclock tsc offsetmatched 1
#   ↑ TP_printk = "masterclock %d hostclock %s offsetmatched %u"（trace.h:922-925）
#     masterclock 1 = 本次重算后启用；0 = 禁用（可能有 TSC 问题）
#     hostclock 是**符号**（none/tsc），不是数字

# ★ kvm_track_tsc 的 masterclock 字段是旧值（见 corrections.md C8），其文本是
#   "vcpu_id %u masterclock %u offsetmatched %u nr_online %u hostclock %s"
#   （trace.h:951-955）——注意 offsetmatched 才是 nr_vcpus_matched_tsc 那一列
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
# ★ 判据别写成"三个都有"。tsc_reliable 是内核 quirk 位
#   （X86_FEATURE_TSC_RELIABLE，arch/x86/include/asm/cpufeatures.h:103），
#   只对少数机器置位——本机 6.8.0-51 实测只有 constant_tsc 与 nonstop_tsc，
#   没有 tsc_reliable，但这台机器确实是 invariant TSC。
#   工程上的 invariant TSC 判据是 constant_tsc + nonstop_tsc
#   （真正的架构位在 CPUID 0x80000007 EDX[8]，/proc/cpuinfo 不单独打印它）。
#   按"三个都有"判会把正常机器误判成时钟不可信。

# Guest 侧（同样只看这两个位）
grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u
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
grep 'kvm_halt_poll_ns' $TRACEFS/trace | \
    grep -oP '\((grow|shrink) \d+\)' | awk '{print $1}' | sort | uniq -c

# 5. 清理
: > $TRACEFS/set_event

echo ""
echo "=== 诊断完成 ==="
```

---

## 📚 参考

- KVM trace events 完整目录：`annotations.md` §1
- bpftrace 脚本集：`annotations.md` §6
- halt-polling 机制：`../phase10-performance/annotations.md` §1
- 测量纪律：`../phase10-performance/measurement.md`
- 参数默认值：`../phase10-performance/parameters.md`
- 大页与脏页测量：`../phase10-performance/practice/bench-huge-dirty.md`（E2）
- PLE 测量：`../phase10-performance/practice/bench-ple.md`（E1）
- 观测者成本：`../phase10-performance/practice/bench-observer-cost.md`（E5）
