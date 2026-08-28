#!/bin/bash
#
# trace-irq-inject.sh - 跟踪 KVM 中断注入路径
#
# 用法: sudo ./trace-irq-inject.sh [选项]
#   -p PID     跟踪指定的 QEMU 进程 PID
#   -d SECS    跟踪持续时间（默认 10 秒）
#   -v         详细模式（显示完整路径）
#   -s         只显示摘要
#   -h         显示帮助
#

set -euo pipefail

TRACEFS=""
PID=""
DURATION=10
VERBOSE=false
SUMMARY_ONLY=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "跟踪 KVM 中断注入路径"
    echo ""
    echo "选项:"
    echo "  -p PID     跟踪指定的 QEMU 进程 PID"
    echo "  -d SECS    跟踪持续时间（默认 10 秒）"
    echo "  -v         详细模式（显示完整中断路径）"
    echo "  -s         只显示摘要"
    echo "  -h         显示帮助"
    echo ""
    echo "示例:"
    echo "  sudo $0 -p 12345 -d 5"
    echo "  sudo $0 -p 12345 -v    # 详细模式"
    echo "  sudo $0 -p 12345 -s    # 只统计"
    exit 0
}

cleanup() {
    if [ -n "$TRACEFS" ] && [ -d "$TRACEFS" ]; then
        echo 0 > "$TRACEFS/tracing_on" 2>/dev/null || true
        echo > "$TRACEFS/set_event" 2>/dev/null || true
        echo nop > "$TRACEFS/current_tracer" 2>/dev/null || true
        echo > "$TRACEFS/set_ftrace_filter" 2>/dev/null || true
    fi
}

trap cleanup EXIT

while getopts "p:d:vsh" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        v) VERBOSE=true ;;
        s) SUMMARY_ONLY=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    exit 1
fi

for tfs in /sys/kernel/debug/tracing /sys/kernel/tracing; do
    if [ -d "$tfs/events/kvm" ]; then
        TRACEFS="$tfs"
        break
    fi
done

if [ -z "$TRACEFS" ]; then
    echo -e "${RED}错误: 找不到 tracefs${NC}"
    exit 1
fi

if [ -z "$PID" ]; then
    PID=$(pgrep -f "qemu-system" | head -1 || echo "")
    if [ -n "$PID" ]; then
        echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
    fi
fi

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  KVM 中断注入路径跟踪${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "跟踪参数:"
echo "  PID:      ${PID:-所有 KVM}"
echo "  持续时间: ${DURATION} 秒"
echo "  模式:     ${VERBOSE:+详细}${SUMMARY_ONLY:+摘要}"
echo ""

# 清空之前的跟踪
echo > "$TRACEFS/trace"
echo 0 > "$TRACEFS/tracing_on"

# 设置中断相关事件
echo kvm:kvm_entry > "$TRACEFS/set_event"
echo kvm:kvm_exit >> "$TRACEFS/set_event"
echo kvm:kvm_inj_virq >> "$TRACEFS/set_event"
echo kvm:kvm_ack_irq >> "$TRACEFS/set_event"

# 添加 IRQ 事件
echo irq:irq_handler_entry >> "$TRACEFS/set_event" 2>/dev/null || true
echo irq:irq_handler_exit >> "$TRACEFS/set_event" 2>/dev/null || true

# 添加调度事件用于分析
if [ "$VERBOSE" = true ]; then
    echo sched:sched_switch >> "$TRACEFS/set_event" 2>/dev/null || true
fi

if [ -n "$PID" ]; then
    echo "$PID" > "$TRACEFS/set_event_pid" 2>/dev/null || true
fi

# 开始跟踪
echo "开始跟踪..."
echo 1 > "$TRACEFS/tracing_on"
sleep "$DURATION"
echo 0 > "$TRACEFS/tracing_on"
echo "跟踪完成。"
echo ""

# 分析结果
TRACE_DATA=$(cat "$TRACEFS/trace")

if [ "$SUMMARY_ONLY" = true ]; then
    echo -e "${BLUE}=== 中断注入摘要 ===${NC}"
    echo ""

    # 统计 VM-Exit 原因
    echo -e "${CYAN}VM-Exit 原因分布:${NC}"
    echo "  原因                         次数"
    echo "  ──────────────────────────── ────────"

    echo "$TRACE_DATA" | grep "kvm_exit" | \
        sed 's/.*reason //' | sed 's/ .*//' | \
        sort | uniq -c | sort -rn | head -10 | \
        while read count reason; do
            printf "  %-30s %d\n" "$reason" "$count"
        done

    echo ""

    # 统计中断注入
    TOTAL_INJ=$(echo "$TRACE_DATA" | grep "kvm_inj_virq" | wc -l)
    TOTAL_EXT=$(echo "$TRACE_DATA" | grep "EXTERNAL_INTERRUPT" | wc -l)
    TOTAL_EXIT=$(echo "$TRACE_DATA" | grep "kvm_exit" | wc -l)

    echo -e "${CYAN}中断统计:${NC}"
    echo "  总 VM-Exit:         $TOTAL_EXIT"
    echo "  外部中断 VM-Exit:   $TOTAL_EXT"
    echo "  中断注入次数:       $TOTAL_INJ"

    if [ "$TOTAL_EXIT" -gt 0 ]; then
        echo ""
        EXT_PCT=$(echo "scale=1; $TOTAL_EXT * 100 / $TOTAL_EXIT" | bc)
        echo "  外部中断占比: ${EXT_PCT}%"
        echo ""

        # 推断 PI 效果
        echo -e "${CYAN}Posted Interrupt 效果推断:${NC}"
        if [ "$TOTAL_EXT" -lt 100 ]; then
            echo "  外部中断 VM-Exit 很少 → PI 可能正在工作 ✓"
        elif [ "$TOTAL_EXT" -gt 1000 ]; then
            echo "  外部中断 VM-Exit 很多 → PI 可能未启用或未生效 ✗"
        else
            echo "  外部中断 VM-Exit 数量中等 → 部分 PI 工作"
        fi
    fi

else
    # 详细分析
    echo -e "${BLUE}=== 中断注入详细分析 ===${NC}"
    echo ""

    TOTAL_EXIT=$(echo "$TRACE_DATA" | grep "kvm_exit" | wc -l)
    TOTAL_EXT=$(echo "$TRACE_DATA" | grep "EXTERNAL_INTERRUPT" | wc -l)
    TOTAL_INJ=$(echo "$TRACE_DATA" | grep "kvm_inj_virq" | wc -l)

    echo "总 VM-Exit: $TOTAL_EXIT"
    echo "外部中断 VM-Exit: $TOTAL_EXT"
    echo "中断注入次数: $TOTAL_INJ"
    echo ""

    # VM-Exit 分布
    echo -e "${BLUE}--- VM-Exit 原因分布 ---${NC}"
    echo "$TRACE_DATA" | grep "kvm_exit" | \
        sed 's/.*reason //' | sed 's/ .*//' | \
        sort | uniq -c | sort -rn | head -15
    echo ""

    # 中断注入详情
    if [ "$TOTAL_INJ" -gt 0 ]; then
        echo -e "${BLUE}--- 中断注入向量分布 ---${NC}"
        echo "$TRACE_DATA" | grep "kvm_inj_virq" | \
            grep -oP 'vector \d+' | \
            sort | uniq -c | sort -rn | head -20
        echo ""
    fi

    # 完整中断路径示例
    if [ "$VERBOSE" = true ]; then
        echo -e "${BLUE}--- 完整中断路径示例 (前 20 个事件) ---${NC}"
        echo ""
        echo "时间              事件"
        echo "────────────────  ──────────────────────────"

        echo "$TRACE_DATA" | \
            grep -E "kvm_exit.*EXTERNAL|kvm_inj_virq|irq_handler|kvm_entry" | \
            head -20 | \
            while IFS= read -r line; do
                # 提取时间戳和事件类型
                ts=$(echo "$line" | grep -oP '\d+\.\d+' | head -1)
                event=$(echo "$line" | grep -oP '\w+:\w+' | head -1)
                detail=$(echo "$line" | sed 's/.*kvm_/kvm_/' | head -c 60)

                if echo "$line" | grep -q "EXTERNAL_INTERRUPT"; then
                    echo -e "  ${ts}  ${YELLOW}VM-Exit (外部中断)${NC}"
                elif echo "$line" | grep -q "kvm_inj_virq"; then
                    echo -e "  ${ts}  ${GREEN}中断注入 → vLAPIC${NC}"
                elif echo "$line" | grep -q "kvm_entry"; then
                    echo -e "  ${ts}  ${CYAN}VM-Entry (恢复Guest)${NC}"
                elif echo "$line" | grep -q "irq_handler_entry"; then
                    echo -e "  ${ts}  ${RED}Host 中断处理${NC}"
                else
                    echo "  ${ts}  ${detail}"
                fi
            done
        echo ""
    fi

    # PI 效果分析
    echo -e "${BLUE}--- Posted Interrupt 效果分析 ---${NC}"
    if [ "$TOTAL_EXIT" -gt 0 ]; then
        EXT_PCT=$(echo "scale=1; $TOTAL_EXT * 100 / $TOTAL_EXIT" | bc)
        echo "  外部中断 VM-Exit 占比: ${EXT_PCT}%"
        echo ""

        if [ "$TOTAL_EXT" -eq 0 ]; then
            echo "  结论: PI 正在完美工作（零外部中断 VM-Exit）"
            echo "  所有外部中断通过 PIR 直接投递到 vCPU"
        elif (( $(echo "$EXT_PCT < 1" | bc -l) )); then
            echo "  结论: PI 效果显著，外部中断极少触发 VM-Exit"
        elif (( $(echo "$EXT_PCT < 10" | bc -l) )); then
            echo "  结论: PI 部分工作，仍有少量外部中断 VM-Exit"
            echo "  可能原因: vCPU halt 状态下的唤醒需要 VM-Exit"
        else
            echo "  结论: PI 可能未启用或未生效"
            echo "  建议检查: - APICv 是否在 CPU 特性中启用"
            echo "           - IRTE 是否配置了 PI 模式"
            echo "           - 设备是否使用 MSI-X"
        fi
    fi
fi

echo ""
echo -e "${GREEN}完成！${NC}"
echo ""
echo "提示:"
echo "  - PI 模式下，外部中断 VM-Exit 应该极少或为零"
echo "  - 检查 /sys/kernel/debug/kvm/ 下的统计信息"
echo "  - 使用 trace-cmd 保存数据: trace-cmd record -e kvm -p $PID -- sleep $DURATION"
