#!/bin/bash
#
# trace-vcpu-sched.sh — vCPU 调度与 Halt-Polling 追踪
#
# 用法: sudo ./trace-vcpu-sched.sh [选项]
#   -p PID     QEMU 进程 PID
#   -d SECS    追踪持续时间（默认 10 秒）
#   -h         显示帮助
#
# 功能:
#   1. 用 perf sched 追踪 vCPU 线程调度
#   2. 用 ftrace kvm_vcpu_wakeup 追踪 halt-polling 行为
#   3. 调整 halt_poll_ns 参数对比
#
# 源码引用:
#   virt/kvm/kvm_main.c:3811 — kvm_vcpu_halt()
#   virt/kvm/kvm_main.c:78   — halt_poll_ns 参数
#   include/trace/events/kvm.h:43 — kvm_vcpu_wakeup trace event
#
# 连接: phase0 annotations.md §7「halt-polling」
#

set -euo pipefail

PID=""
DURATION=10

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "vCPU 调度与 Halt-Polling 追踪"
    echo ""
    echo "选项:"
    echo "  -p PID     QEMU 进程 PID (不指定则自动检测)"
    echo "  -d SECS    追踪持续时间（默认 10 秒）"
    echo "  -h         显示帮助"
    exit 0
}

while getopts "p:d:h" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    exit 1
fi

if [ -z "$PID" ]; then
    PID=$(pgrep -f "qemu-system" | head -1 || echo "")
    if [ -z "$PID" ]; then
        echo -e "${RED}错误: 未找到运行中的 QEMU 进程${NC}"
        exit 1
    fi
    echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
fi

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  vCPU 调度与 Halt-Polling 追踪${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "参数:"
echo "  PID:      $PID"
echo "  持续时间: ${DURATION} 秒"
echo ""
echo "对照: phase0 annotations.md §7「halt-polling」"
echo "源码: virt/kvm/kvm_main.c:3811 — kvm_vcpu_halt()"
echo ""

# ==========================================
# Part 1: halt-polling 参数
# ==========================================
echo -e "${CYAN}[1] 当前 halt-polling 参数${NC}"
echo ""

PARAMS_DIR="/sys/module/kvm/parameters"
if [ -d "$PARAMS_DIR" ]; then
    for p in halt_poll_ns halt_poll_ns_grow halt_poll_ns_grow_start halt_poll_ns_shrink; do
        if [ -f "$PARAMS_DIR/$p" ]; then
            val=$(cat "$PARAMS_DIR/$p" 2>/dev/null || echo "N/A")
            printf "  %-30s %s\n" "$p:" "$val"
        fi
    done
else
    echo -e "${YELLOW}KVM 参数目录不可用${NC}"
fi
echo ""

# ==========================================
# Part 2: ftrace kvm_vcpu_wakeup 追踪
# ==========================================
echo -e "${CYAN}[2] ftrace halt-polling 追踪 (${DURATION}秒)...${NC}"
echo ""

TRACEFS=""
for tfs in /sys/kernel/tracing /sys/kernel/debug/tracing; do
    if [ -d "$tfs/events/kvm" ]; then
        TRACEFS="$tfs"
        break
    fi
done

if [ -z "$TRACEFS" ] || [ ! -d "$TRACEFS/events/kvm/kvm_vcpu_wakeup" ]; then
    echo -e "${RED}kvm_vcpu_wakeup tracepoint 不可用${NC}"
    exit 1
fi

# 清空 tracefs
echo > "$TRACEFS/trace"
echo 0 > "$TRACEFS/tracing_on"
echo > "$TRACEFS/set_event"
echo nop > "$TRACEFS/current_tracer"
echo > "$TRACEFS/set_event_pid"

# 设置事件
echo kvm:kvm_vcpu_wakeup > "$TRACEFS/set_event"

# PID 过滤 - 必须包括所有 vCPU 线程
QEMU_TIDS=$(ls /proc/$PID/task/ 2>/dev/null | tr '\n' ' ' | sed 's/ $//')
if [ -n "$QEMU_TIDS" ]; then
    echo "$QEMU_TIDS" > "$TRACEFS/set_event_pid"
    TID_COUNT=$(echo "$QEMU_TIDS" | wc -w)
    echo "追踪 $TID_COUNT 个线程"
else
    echo -e "${YELLOW}警告: 无法获取线程列表${NC}"
fi

# 追踪
echo 1 > "$TRACEFS/tracing_on"
sleep "$DURATION"
echo 0 > "$TRACEFS/tracing_on"

TRACE_DATA=$(cat "$TRACEFS/trace")

# 分析 wakeup 事件
# 实际格式: "wait time X ns, polling valid" 或 "poll time X ns, polling invalid"
# waited=true 时输出 "wait time"，waited=false 时输出 "poll time"
POLL_COUNT=$(echo "$TRACE_DATA" | grep -c "poll time.*polling valid" || true)
WAIT_COUNT=$(echo "$TRACE_DATA" | grep -c "wait time" || true)
INVALID_COUNT=$(echo "$TRACE_DATA" | grep -c "polling invalid" || true)
TOTAL_WAKE=$((POLL_COUNT + WAIT_COUNT + INVALID_COUNT))

echo -e "${BLUE}=== Halt-Polling 统计 (${DURATION}秒) ===${NC}"
echo ""
echo "  总唤醒次数: $TOTAL_WAKE"
echo ""
printf "  %-30s %d\n" "Poll (忙等成功):" "$POLL_COUNT"
printf "  %-30s %d\n" "Wait (真正阻塞):" "$WAIT_COUNT"
printf "  %-30s %d\n" "Invalid (无效唤醒):" "$INVALID_COUNT"
echo ""

if [ "$TOTAL_WAKE" -gt 0 ]; then
    POLL_PCT=$(echo "scale=1; $POLL_COUNT * 100 / $TOTAL_WAKE" | bc 2>/dev/null || echo "0")
    WAIT_PCT=$(echo "scale=1; $WAIT_COUNT * 100 / $TOTAL_WAKE" | bc 2>/dev/null || echo "0")
    printf "  Poll 占比: %s%%\n" "$POLL_PCT"
    printf "  Wait 占比: %s%%\n" "$WAIT_PCT"
    echo ""

    WAKE_RATE=$(echo "scale=0; $TOTAL_WAKE / $DURATION" | bc)
    printf "  唤醒率: %s/s\n" "$WAKE_RATE"
fi
echo ""

# 分析等待时间分布
echo -e "${BLUE}=== 等待时间分布 ===${NC}"
echo ""
echo "  参考: phase0 annotations.md §7 — halt-polling 三阶段"
echo ""

# 提取等待时间（格式: "wait time 3992257 ns"）
echo "$TRACE_DATA" | grep "kvm_vcpu_wakeup" | \
    grep -oP 'wait time \K[0-9]+' | \
    sort -n | \
    awk '
    BEGIN { n=0; sum=0; min=999999999999; max=0; }
    {
        vals[n] = $1;
        sum += $1;
        if ($1 < min) min = $1;
        if ($1 > max) max = $1;
        n++;
    }
    END {
        if (n == 0) { print "  无数据"; exit; }
        avg = sum / n;
        median = vals[int(n/2)];
        p95 = vals[int(n*0.95)];
        p99 = vals[int(n*0.99)];

        printf "  样本数: %d\n", n;
        printf "  最小值: %.1f μs\n", min/1000;
        printf "  最大值: %.1f μs\n", max/1000;
        printf "  平均值: %.1f μs\n", avg/1000;
        printf "  中位数: %.1f μs\n", median/1000;
        printf "  P95:    %.1f μs\n", p95/1000;
        printf "  P99:    %.1f μs\n", p99/1000;
    }'
echo ""

# 清理 tracefs
echo > "$TRACEFS/set_event"
echo nop > "$TRACEFS/current_tracer"
echo > "$TRACEFS/set_event_pid"

# ==========================================
# Part 3: halt_poll_ns 对比建议
# ==========================================
echo -e "${CYAN}[3] halt_poll_ns 调优对比建议${NC}"
echo ""
echo "  默认值: 200000 ns (200 μs)"
echo "  源码: virt/kvm/kvm_main.c:78"
echo "  参考: phase0 README.md「halt-polling 调优」"
echo ""
echo "  四档对比实验:"
echo ""
echo "    # 保存原值"
echo "    ORIG=\$(cat /sys/module/kvm/parameters/halt_poll_ns)"
echo ""
echo "    # 四档对比"
echo "    for v in 0 200000 400000 1000000; do"
echo "        echo \$v > /sys/module/kvm/parameters/halt_poll_ns"
echo "        # 运行工作负载，测量延迟和 CPU 占用"
echo "    done"
echo ""
echo "    # 恢复原值"
echo "    echo \$ORIG > /sys/module/kvm/parameters/halt_poll_ns"
echo ""
echo "  关键结论 (phase0 README.md):"
echo "    - idle 场景: 零收益，纯浪费 CPU"
echo "    - flood 场景: 买不到延迟，反而多付 CPU"
echo "    - 收益曲线在「窗口刚够盖住典型 halt」处就饱和"
echo "    - 唤醒源随机且大概率落在窗口内时才有收益"
echo ""

# ==========================================
# Part 4: 算法分析
# ==========================================
echo -e "${CYAN}[4] halt-polling 算法 (kvm_vcpu_halt)${NC}"
echo ""
echo "  源码: virt/kvm/kvm_main.c:3811"
echo ""
echo "  ┌─────────────────────────────────────────────┐"
echo "  │ Phase 1: 忙等 (不释放 CPU)                  │"
echo "  │   do {                                      │"
echo "  │     kvm_vcpu_check_block(vcpu);             │"
echo "  │     cpu_relax();                            │"
echo "  │   } while (cur < stop);                     │"
echo "  │   → 有事件立即恢复 (goto out)                │"
echo "  │                                             │"
echo "  │ Phase 2: 真正阻塞 (释放 CPU)                 │"
echo "  │   kvm_vcpu_block(vcpu);                     │"
echo "  │   → schedule() 让出 CPU                     │"
echo "  │                                             │"
echo "  │ Phase 3: 自适应调整                          │"
echo "  │   if halt_ns <= window: 保持 (poll 成功)     │"
echo "  │   if halt_ns > max:     shrink (poll 浪费)   │"
echo "  │   if halt_ns < max:     grow (窗口太小)      │"
echo "  └─────────────────────────────────────────────┘"
echo ""
echo "  参数:"
echo "    halt_poll_ns           = 200000 ns (窗口上限)"
echo "    halt_poll_ns_grow      = 2 (乘数)"
echo "    halt_poll_ns_grow_start = 10000 ns (起始值)"
echo "    halt_poll_ns_shrink    = 2 (除数)"
echo ""

echo -e "${GREEN}完成！${NC}"
