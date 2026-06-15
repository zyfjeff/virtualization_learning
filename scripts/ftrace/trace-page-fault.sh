#!/bin/bash
#
# trace-page-fault.sh - 跟踪 KVM EPT 缺页处理
#
# 用法: sudo ./trace-page-fault.sh [选项]
#   -p PID     跟踪指定的 QEMU 进程 PID
#   -d SECS    跟踪持续时间（默认 10 秒）
#   -l LEVEL   过滤页表级别 (1=4K, 2=2M, 3=1G)
#   -a         显示全部详细信息
#   -s         只显示摘要
#   -h         显示帮助
#

set -euo pipefail

TRACEFS=""
PID=""
DURATION=10
LEVEL=""
ALL_DETAILS=false
SUMMARY_ONLY=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "跟踪 KVM EPT 缺页处理过程"
    echo ""
    echo "选项:"
    echo "  -p PID     跟踪指定的 QEMU 进程 PID"
    echo "  -d SECS    跟踪持续时间（默认 10 秒）"
    echo "  -l LEVEL   过滤页表级别 (1=4K, 2=2M, 3=1G)"
    echo "  -a         显示全部详细信息"
    echo "  -s         只显示摘要"
    echo "  -h         显示帮助"
    echo ""
    echo "示例:"
    echo "  sudo $0 -p 12345 -d 5"
    echo "  sudo $0 -p 12345 -l 2    # 只跟踪 2MB 大页"
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
while getopts "p:d:l:ash" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        l) LEVEL="$OPTARG" ;;
        a) ALL_DETAILS=true ;;
        s) SUMMARY_ONLY=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    exit 1
fi

# 查找 tracefs
for tfs in /sys/kernel/tracing /sys/kernel/debug/tracing; do
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
echo -e "${BLUE}  KVM EPT 缺页跟踪${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "跟踪参数:"
echo "  PID:      ${PID:-所有 KVM}"
echo "  持续时间: ${DURATION} 秒"
echo "  页级别:   ${LEVEL:-全部}"
echo ""

# 清空之前的跟踪
echo > "$TRACEFS/trace"
echo 0 > "$TRACEFS/tracing_on"

# 设置缺页相关事件
echo kvm:kvm_page_fault > "$TRACEFS/set_event"
echo kvm:kvm_mmu_paging_element >> "$TRACEFS/set_event" 2>/dev/null || true
echo kvm:kvm_mmu_set_spte >> "$TRACEFS/set_event" 2>/dev/null || true

# 如果指定了页级别过滤
if [ -n "$LEVEL" ]; then
    case $LEVEL in
        1) echo "  过滤: 4KB 页" ;;
        2) echo "  过滤: 2MB 大页" ;;
        3) echo "  过滤: 1GB 大页" ;;
    esac
fi

# 设置 PID 过滤
if [ -n "$PID" ]; then
    echo "$PID" > "$TRACEFS/set_event_pid" 2>/dev/null || true
fi

# 如果需要详细函数跟踪
if [ "$ALL_DETAILS" = true ]; then
    echo ""
    echo "启用函数级跟踪..."
    echo function > "$TRACEFS/current_tracer"
    echo kvm_handle_page_fault > "$TRACEFS/set_ftrace_filter"
    echo kvm_tdp_page_fault >> "$TRACEFS/set_ftrace_filter"
    echo kvm_tdp_mmu_map >> "$TRACEFS/set_ftrace_filter"
    echo make_spte >> "$TRACEFS/set_ftrace_filter"
    echo tdp_mmu_set_spte_atomic >> "$TRACEFS/set_ftrace_filter"
    echo kvm_mmu_get_page >> "$TRACEFS/set_ftrace_filter"
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
    # 摘要统计
    echo -e "${BLUE}=== EPT 缺页统计 ===${NC}"
    echo ""

    TOTAL_FAULTS=$(echo "$TRACE_DATA" | grep "kvm_page_fault" | wc -l)
    echo "总缺页次数: $TOTAL_FAULTS"
    echo ""

    if [ "$TOTAL_FAULTS" -gt 0 ]; then
        RATE=$(echo "scale=1; $TOTAL_FAULTS / $DURATION" | bc)
        echo "平均缺页率: ${RATE}/秒"
        echo ""

        # 分析缺页类型
        echo "缺页类型分布:"
        echo "  写缺页: $(echo "$TRACE_DATA" | grep "kvm_page_fault" | grep -c "write" || echo 0)"
        echo "  读缺页: $(echo "$TRACE_DATA" | grep "kvm_page_fault" | grep -c "read" || echo 0)"
        echo "  执行缺页: $(echo "$TRACE_DATA" | grep "kvm_page_fault" | grep -c "exec" || echo 0)"
        echo ""

        # 分析 SPTE 操作
        SPTE_OPS=$(echo "$TRACE_DATA" | grep "kvm_mmu_set_spte" | wc -l)
        echo "SPTE 操作次数: $SPTE_OPS"
    fi

else
    # 详细分析
    echo -e "${BLUE}=== EPT 缺页详细分析 ===${NC}"
    echo ""

    TOTAL_FAULTS=$(echo "$TRACE_DATA" | grep "kvm_page_fault" | wc -l)
    echo "总缺页次数: $TOTAL_FAULTS"

    if [ "$TOTAL_FAULTS" -gt 0 ]; then
        RATE=$(echo "scale=1; $TOTAL_FAULTS / $DURATION" | bc)
        echo "平均缺页率: ${RATE}/秒"
        echo ""

        # 分析缺页的 GPA 分布
        echo -e "${BLUE}--- GPA 地址分布 (前 20 个热门区域) ---${NC}"
        echo "$TRACE_DATA" | grep "kvm_page_fault" | \
            grep -oP 'address=0x[0-9a-f]+' | sort | uniq -c | \
            sort -rn | head -20
        echo ""

        # 分析页级别
        echo -e "${BLUE}--- 页级别分布 ---${NC}"
        echo "  4KB 页映射: $(echo "$TRACE_DATA" | grep "level=1" | wc -l)"
        echo "  2MB 大页映射: $(echo "$TRACE_DATA" | grep "level=2" | wc -l)"
        echo "  1GB 大页映射: $(echo "$TRACE_DATA" | grep "level=3" | wc -l)"
        echo ""

        # SPTE 分析
        echo -e "${BLUE}--- SPTE 操作 ---${NC}"
        SPTE_OPS=$(echo "$TRACE_DATA" | grep "kvm_mmu_set_spte" | wc -l)
        echo "  SPTE 设置次数: $SPTE_OPS"
        echo ""

        # 显示前 30 个缺页事件
        echo -e "${BLUE}--- 缺页事件示例 (前 30 条) ---${NC}"
        echo "$TRACE_DATA" | grep "kvm_page_fault" | head -30
        echo ""

        # 如果启用了函数跟踪，显示函数调用
        if [ "$ALL_DETAILS" = true ]; then
            echo -e "${BLUE}--- 函数调用跟踪 (前 50 条) ---${NC}"
            echo "$TRACE_DATA" | grep -E "kvm_tdp_mmu_map|make_spte|tdp_mmu_set" | head -50
        fi
    else
        echo -e "${YELLOW}未观察到缺页事件。${NC}"
        echo "可能原因:"
        echo "  - 虚拟机未运行"
        echo "  - 内存已全部映射"
        echo "  - 跟踪时间太短"
    fi
fi

echo ""
echo -e "${GREEN}完成！${NC}"
echo ""
echo "提示:"
echo "  - 使用 trace-cmd 可以保存完整数据:"
echo "    trace-cmd record -e kvm:kvm_page_fault -p $PID -- sleep $DURATION"
echo "  - 分析大页效果: 对比 level=1 和 level=2 的数量"
echo "  - 跟踪脏页: 观察 kvm_mmu_set_spte 中 W 位的变化"
