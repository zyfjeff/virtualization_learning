#!/bin/bash
#
# 测量 MSR 访问的 VM-Exit 开销
#

set -e

PID=$(pgrep -f '^qemu-system-x86_64' | head -1)

if [ -z "$PID" ]; then
    echo "✗ 未找到 QEMU 进程"
    exit 1
fi

echo "========================================"
echo "  MSR VM-Exit 开销分析"
echo "========================================"
echo ""
echo "QEMU PID: $PID"
echo ""

# 追踪 kvm_msr 事件 5 秒
echo "追踪 kvm:kvm_msr 事件 (5秒)..."
TRACEFS=/sys/kernel/tracing

echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on
echo > $TRACEFS/set_event
echo nop > $TRACEFS/current_tracer
echo > $TRACEFS/set_event_pid

# 获取所有 vCPU 线程
QEMU_TIDS=$(ls /proc/$PID/task/ 2>/dev/null | tr '\n' ' ')
echo "$QEMU_TIDS" > $TRACEFS/set_event_pid

echo kvm:kvm_msr > $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on
sleep 5
echo 0 > $TRACEFS/tracing_on

echo ""
echo "========================================"
echo "  分析结果"
echo "========================================"
echo ""

# 统计 MSR 访问
TRACE_DATA=$(cat $TRACEFS/trace)
TOTAL=$(echo "$TRACE_DATA" | grep -c "msr_" || echo 0)

echo "总 MSR 访问次数 (5秒): $TOTAL"
echo "平均速率: $(echo "scale=0; $TOTAL / 5" | bc) 次/秒"
echo ""

# 按 MSR 地址统计
echo "MSR 访问热榜:"
echo "  MSR 地址    次数    类型"
echo "  ──────────  ──────  ────"

echo "$TRACE_DATA" | grep "msr_" | \
    sed -E 's/.*msr_(read|write) ([0-9a-f]+) = 0x[0-9a-f]+.*/\1 \2/' | \
    sort | uniq -c | sort -rn | head -10 | \
    while read count rw msr; do
        case "$msr" in
            10)          desc="IA32_TSC (应透传)" ;;
            1b)          desc="IA32_APIC_BASE (拦截)" ;;
            c0000080)    desc="IA32_EFER (拦截)" ;;
            6e0)         desc="IA32_TSC_DEADLINE (拦截)" ;;
            *)           desc="(other)" ;;
        esac
        printf "  0x%-10s %-6d  %s (%s)\n" "$msr" "$count" "$rw" "$desc"
    done

echo ""
echo "========================================"
echo "  结论"
echo "========================================"
echo ""
echo "1. IA32_TSC (0x10) 不应该出现在这里（应该透传）"
echo "   - 如果出现，说明 KVM 配置问题"
echo ""
echo "2. IA32_EFER 和 IA32_APIC_BASE 应该出现（被拦截）"
echo "   - 每次访问触发 VM-Exit，开销约 1500-3000 ns"
echo ""
echo "3. IA32_TSC_DEADLINE (0x6e0) 通常高频出现"
echo "   - Guest 定时器使用，每次 VM-Exit"
echo ""

# 清理
echo > $TRACEFS/set_event
echo nop > $TRACEFS/current_tracer
echo > $TRACEFS/set_event_pid
