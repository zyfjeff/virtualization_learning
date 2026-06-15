# 实践项目 5：时钟虚拟化性能与精度分析

> 目标：对比不同时钟源的性能、精度和稳定性，理解 KVM 时钟虚拟化的实际表现

---

## 🎯 项目目标

通过实际测试对比 Guest 中各种时钟源（kvm-clock / TSC / HPET / PIT）的性能特征，
测量定时器延迟，验证 TSC 稳定性，深入理解 KVM 时钟虚拟化的工程取舍。

完成本项目后，你应该能够：
1. 量化不同时钟源的性能差异（读取延迟、精度）
2. 用 cyclictest 测量定时器精度和抖动
3. 用 ftrace 追踪定时器相关的 VM-Exit
4. 判断 TSC 是否稳定，以及何时应该切换到 kvm-clock
5. 分析 timer advance 优化效果

---

## 📋 前置知识

- 第五阶段：PIT / APIC Timer / TSC / kvmclock
- TSC-deadline 模式原理
- pvclock 协议

---

## 🔧 实验环境

```bash
# 启动测试虚拟机 (确保启用多种时钟源)
qemu-system-x86_64 \
    -enable-kvm \
    -m 2G \
    -smp 2 \
    -cpu host \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &

QEMU_PID=$!

# 在 Guest 内安装测试工具
# apt install linux-tools-generic trace-cmd
# 或: yum install perf trace-cmd

# 检查 Guest 可用的时钟源
cat /sys/devices/system/clocksource/clocksource0/available_clocksource
# 预期输出: kvm-clock tsc hpet acpi_pm
# (PIT 通常不显示为可用clocksource，但可作为clockevent)

# 检查当前使用的时钟源
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
# 预期: kvm-clock

# 检查 TSC 特征
grep -E "constant_tsc|tsc_deadline|nonstop_tsc" /proc/cpuinfo | head -3
# 应看到: constant_tsc tsc_deadline_timer nonstop_tsc
```

---

## 📊 实验步骤

### 步骤 1：时钟源切换与基本验证

```bash
#!/bin/bash
# clocksource-switch.sh
# 切换时钟源并验证基本功能

echo "=== 可用时钟源 ==="
cat /sys/devices/system/clocksource/clocksource0/available_clocksource

SOURCES="kvm-clock tsc hpet acpi_pm"

for src in $SOURCES; do
    echo ""
    echo "=== 切换到: $src ==="
    echo $src > /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null
    actual=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)
    
    if [ "$actual" = "$src" ]; then
        echo "  ✓ 切换成功: $actual"
    else
        echo "  ✗ 切换失败: 请求=$src 实际=$actual"
        continue
    fi
    
    # 验证时间是否合理 (不应跳变超过1秒)
    echo "  当前时间: $(date)"
    echo "  uptime: $(uptime -p)"
done

# 切回 kvm-clock
echo kvm-clock > /sys/devices/system/clocksource/clocksource0/current_clocksource
```

### 步骤 2：时钟源读取性能基准测试

```bash
#!/bin/bash
# clocksource-benchmark.sh
# 测量不同时钟源的读取延迟

echo "=== 时钟源读取性能基准 ==="
echo "每个时钟源测量 1000 万次读取"
echo ""

# 方法1: 使用 rdtsc 直接测量 (需要编译小工具)
cat > /tmp/clock_bench.c << 'EOF'
#include <stdio.h>
#include <time.h>
#include <stdint.h>

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(int argc, char *argv[]) {
    int iterations = 10000000;
    struct timespec start, end;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iterations; i++) {
        volatile uint64_t tsc = rdtsc();
        (void)tsc;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("RDTSC: %.2f ns/read (%d iterations in %.3f sec)\n",
           elapsed * 1e9 / iterations, iterations, elapsed);
    return 0;
}
EOF

gcc -O2 -o /tmp/clock_bench /tmp/clock_bench.c
/tmp/clock_bench

# 方法2: 通过 clock_gettime 测量不同 clocksource
cat > /tmp/clock_gettime_bench.c << 'EOF'
#include <stdio.h>
#include <time.h>

int main(void) {
    int iterations = 10000000;
    struct timespec start, end, ts;
    
    /* 测试 CLOCK_MONOTONIC (使用当前clocksource) */
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iterations; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("clock_gettime: %.2f ns/call (%d iterations in %.3f sec)\n",
           elapsed * 1e9 / iterations, iterations, elapsed);
    return 0;
}
EOF

gcc -O2 -o /tmp/clock_gettime_bench /tmp/clock_gettime_bench.c

echo ""
for src in kvm-clock tsc hpet acpi_pm; do
    echo $src > /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null
    actual=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)
    if [ "$actual" != "$src" ]; then continue; fi
    printf "%-12s: " "$src"
    /tmp/clock_gettime_bench
done

echo kvm-clock > /sys/devices/system/clocksource/clocksource0/current_clocksource
```

### 步骤 3：定时器精度测试 (cyclictest)

```bash
#!/bin/bash
# cyclictest-compare.sh
# 使用 cyclictest 对比不同时钟源下的定时器精度

echo "=== cyclictest 定时器精度对比 ==="
echo "参数: -t1 -p 80 -n -i 1000 -l 10000"
echo "  -t1      : 1个线程"
echo "  -p 80    : FIFO优先级80"
echo "  -n       : 使用 nanosleep"
echo "  -i 1000  : 1ms间隔"
echo "  -l 10000 : 10000次循环"
echo ""

# 安装 cyclictest (如果未安装)
# apt install rt-tests  或  yum install rt-tests

for src in kvm-clock tsc hpet; do
    echo $src > /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null
    actual=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)
    if [ "$actual" != "$src" ]; then
        echo "  跳过 $src (不可用)"
        continue
    fi
    
    echo "=== $src ==="
    cyclictest -t1 -p 80 -n -i 1000 -l 10000 -q 2>/dev/null | head -5
    echo ""
done

echo kvm-clock > /sys/devices/system/clocksource/clocksource0/current_clocksource
```

### 步骤 4：定时器 VM-Exit 追踪

```bash
#!/bin/bash
# trace-timer-exits.sh
# 追踪定时器相关的 VM-Exit

TRACEFS=/sys/kernel/debug/tracing

# 在Host上执行 (需要root)
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

# 启用 KVM exit 追踪 (过滤定时器相关)
echo kvm:kvm_exit > $TRACEFS/set_event
echo kvm:kvm_entry >> $TRACEFS/set_event

# 按 QEMU PID 过滤 (如果有)
if [ -n "$QEMU_PID" ]; then
    echo "function_graph" > $TRACEFS/current_tracer 2>/dev/null || true
    # 使用 PID 过滤
    echo $QEMU_PID > $TRACEFS/set_event_pid 2>/dev/null || true
fi

echo 1 > $TRACEFS/tracing_on

# 在 Guest 内触发定时活动
# ssh vm "stress --cpu 1 --timeout 5"
sleep 5

echo 0 > $TRACEFS/tracing_on

echo "=== VM-Exit 原因分布 ==="
cat $TRACEFS/trace | grep kvm_exit | \
    sed 's/.*reason //' | awk '{print $1}' | \
    sort | uniq -c | sort -rn | head -15

echo ""
echo "=== 定时器相关 Exit (EXTERNAL_INTERRUPT / PENDING_VIRT_INTR) ==="
cat $TRACEFS/trace | grep -E "EXTERNAL_INTERRUPT|PENDING_VIRT_INTR" | head -20
```

### 步骤 5：TSC 稳定性测试

```bash
#!/bin/bash
# tsc-stability-test.sh
# 检测 TSC 在多个 CPU 间是否同步

echo "=== TSC 稳定性测试 ==="

# 方法1: 内核日志
echo "--- 内核 TSC 检测结果 ---"
dmesg | grep -iE "tsc|clocksource" | tail -20

echo ""
echo "--- TSC 特征检查 ---"
grep -E "constant_tsc|nonstop_tsc|tsc_deadline" /proc/cpuinfo | sort -u

# 方法2: 跨CPU TSC 差异测量
cat > /tmp/tsc_sync_test.c << 'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sched.h>
#include <unistd.h>

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t tsc_per_cpu[64];
    cpu_set_t mask;
    
    printf("在 %d 个 CPU 上测量 TSC:\n", num_cpus);
    
    for (int cpu = 0; cpu < num_cpus && cpu < 64; cpu++) {
        CPU_ZERO(&mask);
        CPU_SET(cpu, &mask);
        sched_setaffinity(0, sizeof(mask), &mask);
        
        /* 多读几次取平均 */
        uint64_t sum = 0;
        for (int i = 0; i < 100; i++) {
            sum += rdtsc();
        }
        tsc_per_cpu[cpu] = sum / 100;
        printf("  CPU %d: TSC = %lu\n", cpu, tsc_per_cpu[cpu]);
    }
    
    /* 计算差异 */
    uint64_t min_tsc = tsc_per_cpu[0], max_tsc = tsc_per_cpu[0];
    for (int i = 1; i < num_cpus; i++) {
        if (tsc_per_cpu[i] < min_tsc) min_tsc = tsc_per_cpu[i];
        if (tsc_per_cpu[i] > max_tsc) max_tsc = tsc_per_cpu[i];
    }
    
    uint64_t diff = max_tsc - min_tsc;
    printf("\nTSC 最大差异: %lu ticks\n", diff);
    
    if (diff < 1000) {
        printf("✓ TSC 同步良好 (< 1000 ticks 差异)\n");
    } else if (diff < 100000) {
        printf("⚠ TSC 有轻微偏差 (%lu ticks), 但通常可接受\n", diff);
    } else {
        printf("✗ TSC 不同步! (%lu ticks 差异), 建议使用 kvm-clock\n", diff);
    }
    
    return 0;
}
EOF

gcc -O2 -o /tmp/tsc_sync_test /tmp/tsc_sync_test.c
/tmp/tsc_sync_test
```

### 步骤 6：Timer Advance 优化验证

```bash
#!/bin/bash
# timer-advance-test.sh
# 验证 KVM timer advance 优化效果

echo "=== Timer Advance 优化验证 ==="

# 查看当前设置
echo "当前参数:"
for f in /sys/module/kvm/parameters/lapic_timer_advance*; do
    [ -f "$f" ] && echo "  $(basename $f) = $(cat $f)"
done

# 方法: 对比有/无 timer advance 的延迟

echo ""
echo "--- 1. 启用 timer advance (默认) ---"
echo 1 > /sys/module/kvm/parameters/lapic_timer_advance 2>/dev/null
echo "  lapic_timer_advance = $(cat /sys/module/kvm/parameters/lapic_timer_advance 2>/dev/null)"

# 运行 cyclictest
echo "  cyclictest 结果:"
cyclictest -t1 -p 80 -n -i 1000 -l 5000 -q 2>/dev/null | head -3

# 方法2: 使用 ftrace 测量实际注入延迟
TRACEFS=/sys/kernel/debug/tracing
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on
echo kvm:kvm_exit > $TRACEFS/set_event
echo kvm:kvm_entry > $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on
sleep 3
echo 0 > $TRACEFS/tracing_on

echo ""
echo "  VM-Exit 统计:"
cat $TRACEFS/trace | grep kvm_exit | wc -l
echo " 次 Exit (3秒内)"
```

---

## 📈 预期分析结果

### 时钟源读取性能 (典型值)

```
时钟源          读取延迟(ns)     VM-Exit    适用场景
──────────────  ──────────────   ────────   ──────────────────
kvm-clock       ~25-40           无         ★ 首选 (精度高、迁移友好)
TSC (rdtsc)     ~15-25           无         高精度时间戳
HPET            ~300-800         每次读写   兼容性后备
acpi_pm         ~500-1200        每次读写   最慢后备
PIT             ~1000+           每次读写   仅用于兼容
```

### cyclictest 精度对比 (典型值)

```
时钟源      Avg Latency   Max Latency    抖动(stddev)
──────────  ────────────  ────────────   ────────────
kvm-clock   2-5 μs        15-30 μs       1-3 μs
tsc         1-3 μs        10-20 μs       0.5-2 μs
hpet        5-15 μs       30-80 μs       3-10 μs
pit         50-200 μs     500-1500 μs    50-200 μs
```

### Timer Advance 效果

```
                  无 advance    有 advance (自动)   改善
────────────────  ──────────   ─────────────────   ────
平均延迟          ~3000 ns      ~1500 ns           ~50%
最大延迟          ~8000 ns      ~4000 ns           ~50%
抖动              ~1000 ns      ~500 ns            ~50%
```

---

## 🔍 深入分析

### 分析1: 为什么 kvm-clock 是首选？

```bash
# 对比 kvm-clock 和 TSC 的特性

echo "=== kvm-clock 优势分析 ==="

echo ""
echo "1. 读取速度对比:"
for src in kvm-clock tsc; do
    echo $src > /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null
    [ "$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)" != "$src" ] && continue
    printf "  %-12s: " "$src"
    /tmp/clock_gettime_bench
done

echo ""
echo "2. 迁移兼容性:"
echo "  TSC: 迁移后需要重新同步 offset, 可能跳变"
echo "  kvm-clock: Host 更新 pvclock 结构即可, Guest 无感知"

echo ""
echo "3. 多CPU一致性:"
echo "  TSC: 不同CPU可能不同步 (即使有 constant_tsc)"
echo "  kvm-clock: 通过 pvti 保证所有 vCPU 时间一致"

echo kvm-clock > /sys/devices/system/clocksource/clocksource0/current_clocksource
```

### 分析2: 何时应该切换时钟源？

```bash
echo "=== 时钟源选择决策树 ==="
echo ""
echo "  Guest 内核版本 ≥ 4.x?"
echo "    ├─ 是 → 默认使用 kvm-clock ✓ (无需切换)"
echo "    └─ 否 → 检查是否支持 kvm-clock"
echo "              ├─ 是 → 使用 kvm-clock"
echo "              └─ 否 → 检查 TSC:"
echo "                        grep constant_tsc /proc/cpuinfo"
echo "                        ├─ 有 → 使用 tsc"
echo "                        └─ 无 → 使用 hpet 或 acpi_pm"
echo ""
echo "  实时应用 (低延迟要求)?"
echo "    ├─ 是 → kvm-clock + TSC-deadline (lapic timer)"
echo "    └─ 否 → 默认即可"
echo ""
echo "  需要 VM 迁移?"
echo "    ├─ 是 → 必须 kvm-clock (TSC 会跳变)"
echo "    └─ 否 → TSC 也可接受"
```

### 分析3: 排查时钟问题

```bash
echo "=== 时钟问题排查清单 ==="

echo ""
echo "1. 时间跳变检测:"
echo "   # 在Guest内持续监控时间"
echo "   while true; do"
echo "       date +%s.%N"
echo "       sleep 0.1"
echo "   done | awk 'NR>1{d=\$1-prev; if(d<0||d>0.2) print \"JUMP:\", d} {prev=\$1}'"

echo ""
echo "2. 时钟源降级检测:"
echo "   dmesg | grep -i 'clocksource.*changed'"
echo "   # 如果从 kvm-clock 降级到 tsc/hpet, 说明有问题"

echo ""
echo "3. TSC 不稳定警告:"
echo "   dmesg | grep -i 'tsc.*unstable'"
echo "   # 如果出现, 说明 TSC 不可靠, Guest 应切换到其他时钟源"

echo ""
echo "4. KVM 时钟相关 tracepoint:"
echo "   ls /sys/kernel/debug/tracing/events/kvm/ | grep -E 'clock|time|tsc'"
```

---

## 📝 报告要求

1. **性能对比表**: 填写实测的各时钟源读取延迟和 cyclictest 结果
2. **TSC 稳定性报告**: 记录 TSC 同步测试结果，判断是否适合用作 clocksource
3. **Timer Advance 效果**: 对比启用/禁用 timer advance 的延迟数据
4. **时钟源选择建议**: 基于测试结果，给出不同场景的时钟源选择建议
5. **异常分析**: 如果测试中遇到时间跳变、降级等异常，分析原因

---

## 📚 延伸阅读

- Phase 5 (phase5-timer-virt/) — 时钟虚拟化的理论背景
- Phase 6 项目2 (ept-performance.md) — EPT 性能分析（对比方法论）
- `Documentation/virt/kvm/clocks.rst` — KVM 时钟文档
- pvclock 协议: `arch/x86/include/uapi/asm/kvm_para.h`
