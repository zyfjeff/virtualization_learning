#!/bin/bash
#
# trace-vmexit.sh - 跟踪 KVM VM-Exit 事件
#
# 用法: sudo ./trace-vmexit.sh [选项]
#   -p PID     跟踪指定的 QEMU 进程 PID
#   -d SECS    跟踪持续时间（默认 10 秒）
#   -o FILE    输出文件（默认打印到终端）
#   -s         只显示摘要统计
#   -h         显示帮助
#
# 示例:
#   sudo ./trace-vmexit.sh -p 12345 -d 5
#   sudo ./trace-vmexit.sh -p 12345 -o /tmp/vmexit.log
#   sudo ./trace-vmexit.sh -p 12345 -s
#

set -euo pipefail

TRACEFS=""
OUTPUT=""
PID=""
DURATION=10
SUMMARY_ONLY=false

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "选项:"
    echo "  -p PID     跟踪指定的 QEMU 进程 PID"
    echo "  -d SECS    跟踪持续时间（默认 10 秒）"
    echo "  -o FILE    输出文件（默认打印到终端）"
    echo "  -s         只显示摘要统计"
    echo "  -h         显示帮助"
    echo ""
    echo "示例:"
    echo "  sudo $0 -p 12345 -d 5"
    echo "  sudo $0 -p 12345 -s"
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

# 解析参数
while getopts "p:d:o:sh" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        o) OUTPUT="$OPTARG" ;;
        s) SUMMARY_ONLY=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

# 检查 root 权限
if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    exit 1
fi

# 检查 tracefs
for tfs in /sys/kernel/tracing /sys/kernel/debug/tracing; do
    if [ -d "$tfs/events/kvm" ]; then
        TRACEFS="$tfs"
        break
    fi
done

if [ -z "$TRACEFS" ]; then
    echo -e "${RED}错误: 找不到 tracefs，请确保 debugfs/tracefs 已挂载${NC}"
    echo "  mount -t tracefs none /sys/kernel/tracing"
    exit 1
fi

# 检查 KVM 事件是否可用
if [ ! -d "$TRACEFS/events/kvm/kvm_exit" ]; then
    echo -e "${RED}错误: KVM tracepoint 不可用，请确保 KVM 模块已加载${NC}"
    exit 1
fi

# 如果未指定 PID，尝试找到 QEMU 进程
if [ -z "$PID" ]; then
    PID=$(pgrep -f "qemu-system" | head -1 || echo "")
    if [ -z "$PID" ]; then
        echo -e "${YELLOW}警告: 未找到 QEMU 进程，将跟踪所有 KVM 事件${NC}"
        PID=""
    else
        echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
    fi
fi

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  KVM VM-Exit 跟踪${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "跟踪参数:"
echo "  PID:      ${PID:-所有 KVM}"
echo "  持续时间: ${DURATION} 秒"
echo "  输出:     ${OUTPUT:-终端}"
echo ""

# 清空之前的跟踪
echo > "$TRACEFS/trace"
echo 0 > "$TRACEFS/tracing_on"

# 设置事件
echo kvm:kvm_exit > "$TRACEFS/set_event"
echo kvm:kvm_entry >> "$TRACEFS/set_event"

# 设置 PID 过滤（如果指定了 PID）
if [ -n "$PID" ]; then
    echo "$PID" > "$TRACEFS/set_event_pid" 2>/dev/null || true
fi

# 开始跟踪
echo "开始跟踪..."
echo 1 > "$TRACEFS/tracing_on"

# 等待指定时间
sleep "$DURATION"

# 停止跟踪
echo 0 > "$TRACEFS/tracing_on"
echo "跟踪完成。"
echo ""

# 输出结果
output_result() {
    if [ -n "$OUTPUT" ]; then
        cat > "$OUTPUT"
    else
        cat
    fi
}

if [ "$SUMMARY_ONLY" = true ]; then
    # 只显示摘要统计
    echo -e "${BLUE}=== VM-Exit 原因统计 ===${NC}"
    echo ""

    # 统计各种退出原因
    TRACE_DATA=$(cat "$TRACEFS/trace")

    echo "退出原因                    次数"
    echo "──────────────────────────  ────────"

    echo "$TRACE_DATA" | grep "kvm_exit" | \
        sed 's/.*reason //' | sed 's/ .*//' | \
        sort | uniq -c | sort -rn | \
        while read count reason; do
            printf "%-28s  %d\n" "$reason" "$count"
        done | output_result

    echo ""

    # 统计总数
    TOTAL=$(echo "$TRACE_DATA" | grep "kvm_exit" | wc -l)
    ENTRY=$(echo "$TRACE_DATA" | grep "kvm_entry" | wc -l)

    echo "总计:"
    echo "  VM-Exit:  $TOTAL"
    echo "  VM-Entry: $ENTRY"

    if [ "$TOTAL" -gt 0 ]; then
        RATE=$(echo "scale=1; $TOTAL / $DURATION" | bc)
        echo "  平均退出率: ${RATE}/秒"
    fi

else
    # 显示完整跟踪结果
    echo -e "${BLUE}=== 完整 VM-Exit 跟踪 ===${NC}"
    echo ""

    TRACE_DATA=$(cat "$TRACEFS/trace")

    # 先显示摘要
    echo -e "${BLUE}--- 摘要统计 ---${NC}"
    echo ""
    echo "退出原因                    次数"
    echo "──────────────────────────  ────────"

    echo "$TRACE_DATA" | grep "kvm_exit" | \
        sed 's/.*reason //' | sed 's/ .*//' | \
        sort | uniq -c | sort -rn | head -15 | \
        while read count reason; do
            printf "%-28s  %d\n" "$reason" "$count"
        done

    TOTAL=$(echo "$TRACE_DATA" | grep "kvm_exit" | wc -l)
    echo ""
    echo "总 VM-Exit: $TOTAL"
    echo ""

    # 显示详细跟踪（限制行数）
    echo -e "${BLUE}--- 详细跟踪（前 100 条）---${NC}"
    echo ""

    echo "$TRACE_DATA" | \
        grep -E "kvm_exit|kvm_entry" | \
        head -100 | output_result

    echo ""
    echo -e "${YELLOW}提示: 完整数据在 $TRACEFS/trace${NC}"
    echo "使用 trace-cmd 可以导出完整数据:"
    echo "  trace-cmd record -e kvm:kvm_exit -e kvm:kvm_entry -p $PID -- sleep $DURATION"
    echo "  trace-cmd report"
fi

echo ""
echo -e "${GREEN}完成！${NC}"
