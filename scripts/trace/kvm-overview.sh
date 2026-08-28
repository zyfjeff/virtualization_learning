#!/bin/bash
#
# kvm-overview.sh - KVM 综合性能概览
#
# 用法: sudo ./kvm-overview.sh [选项]
#   -p PID     跟踪指定的 QEMU 进程 PID
#   -d SECS    分析持续时间（默认 30 秒）
#   -o FILE    输出文件
#   -h         显示帮助
#
# 功能:
#   - VM-Exit 原因分布
#   - vCPU 时间分析
#   - 内存性能统计
#   - 中断性能统计
#   - 综合性能报告
#

set -euo pipefail

PID=""
DURATION=30
OUTPUT=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "KVM 综合性能概览工具"
    echo ""
    echo "选项:"
    echo "  -p PID     跟踪指定的 QEMU 进程 PID"
    echo "  -d SECS    分析持续时间（默认 30 秒）"
    echo "  -o FILE    输出文件"
    echo "  -h         显示帮助"
    echo ""
    echo "功能:"
    echo "  1. VM-Exit 原因分布"
    echo "  2. vCPU Guest/Host 时间比"
    echo "  3. 缺页处理统计"
    echo "  4. 中断注入统计"
    echo "  5. 综合性能报告"
    exit 0
}

while getopts "p:d:o:h" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        o) OUTPUT="$OPTARG" ;;
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
        echo "请使用 -p 指定 QEMU PID"
        exit 1
    fi
    echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
fi

echo -e "${BLUE}${BOLD}=========================================${NC}"
echo -e "${BLUE}${BOLD}  KVM 综合性能概览${NC}"
echo -e "${BLUE}${BOLD}=========================================${NC}"
echo ""
echo "分析参数:"
echo "  PID:      $PID"
echo "  持续时间: ${DURATION} 秒"
echo ""

# ==========================================
# Part 1: KVM 统计信息
# ==========================================
echo -e "${CYAN}${BOLD}1. KVM 统计信息${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"

# 读取 KVM 统计
if [ -d "/sys/kernel/debug/kvm" ]; then
    echo ""
    echo "  全局统计:"
    for stat in /sys/kernel/debug/kvm/*; do
        if [ -f "$stat" ]; then
            name=$(basename "$stat")
            value=$(cat "$stat" 2>/dev/null || echo "N/A")
            printf "  %-35s %s\n" "$name:" "$value"
        fi
    done
fi
echo ""

# ==========================================
# Part 2: 使用 perf kvm stat
# ==========================================
echo -e "${CYAN}${BOLD}2. perf kvm 统计${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

echo "  收集 perf kvm 数据 (${DURATION}秒)..."
PERF_DATA=$(mktemp /tmp/kvm-perf-XXXXXX.data)

# perf kvm stat record
perf kvm stat record -p "$PID" -o "$PERF_DATA" -- sleep "$DURATION" 2>/dev/null || \
    echo -e "  ${YELLOW}perf kvm stat 不可用，跳过${NC}"

if [ -f "$PERF_DATA" ] && [ -s "$PERF_DATA" ]; then
    echo ""
    echo "  VM-Exit 原因分布:"
    echo "  ────────────────────────────────────"
    perf kvm stat report -i "$PERF_DATA" --stdio 2>/dev/null | head -30
fi

rm -f "$PERF_DATA"
echo ""

# ==========================================
# Part 3: ftrace 事件分析
# ==========================================
echo -e "${CYAN}${BOLD}3. ftrace 事件分析${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"

TRACEFS=""
for tfs in /sys/kernel/debug/tracing /sys/kernel/tracing; do
    if [ -d "$tfs/events/kvm" ]; then
        TRACEFS="$tfs"
        break
    fi
done

if [ -n "$TRACEFS" ]; then
    echo ""
    echo "  收集 ftrace 数据 (${DURATION}秒)..."

    # 清空并设置
    echo > "$TRACEFS/trace"
    echo 0 > "$TRACEFS/tracing_on"
    echo > "$TRACEFS/set_event"
    echo nop > "$TRACEFS/current_tracer"

    # 设置关键事件
    echo kvm:kvm_exit > "$TRACEFS/set_event"
    echo kvm:kvm_entry >> "$TRACEFS/set_event"
    echo kvm:kvm_page_fault >> "$TRACEFS/set_event"
    echo kvm:kvm_inj_virq >> "$TRACEFS/set_event"
    echo kvm:kvm_vcpu_wakeup >> "$TRACEFS/set_event"

    echo "$PID" > "$TRACEFS/set_event_pid" 2>/dev/null || true

    # 开始跟踪
    echo 1 > "$TRACEFS/tracing_on"
    sleep "$DURATION"
    echo 0 > "$TRACEFS/tracing_on"

    TRACE_DATA=$(cat "$TRACEFS/trace")

    # 分析 VM-Exit
    TOTAL_EXIT=$(echo "$TRACE_DATA" | grep "kvm_exit" | wc -l)
    TOTAL_ENTRY=$(echo "$TRACE_DATA" | grep "kvm_entry" | wc -l)
    TOTAL_FAULT=$(echo "$TRACE_DATA" | grep "kvm_page_fault" | wc -l)
    TOTAL_INJ=$(echo "$TRACE_DATA" | grep "kvm_inj_virq" | wc -l)
    TOTAL_WAKE=$(echo "$TRACE_DATA" | grep "kvm_vcpu_wakeup" | wc -l)

    echo ""
    echo "  事件统计 (${DURATION}秒):"
    echo "  ────────────────────────────────"
    printf "  %-25s %d\n" "VM-Exit:" "$TOTAL_EXIT"
    printf "  %-25s %d\n" "VM-Entry:" "$TOTAL_ENTRY"
    printf "  %-25s %d\n" "EPT 缺页:" "$TOTAL_FAULT"
    printf "  %-25s %d\n" "中断注入:" "$TOTAL_INJ"
    printf "  %-25s %d\n" "vCPU 唤醒:" "$TOTAL_WAKE"

    if [ "$TOTAL_EXIT" -gt 0 ]; then
        echo ""
        echo "  退出率: $(echo "scale=1; $TOTAL_EXIT / $DURATION" | bc)/秒"
        echo ""

        echo "  VM-Exit 原因 Top 10:"
        echo "  ────────────────────────────────────"
        echo "$TRACE_DATA" | grep "kvm_exit" | \
            sed 's/.*reason //' | sed 's/ .*//' | \
            sort | uniq -c | sort -rn | head -10 | \
            while read count reason; do
                pct=$(echo "scale=1; $count * 100 / $TOTAL_EXIT" | bc)
                printf "  %-28s %6d (%5s%%)\n" "$reason" "$count" "$pct"
            done
    fi

    if [ "$TOTAL_FAULT" -gt 0 ]; then
        echo ""
        echo "  缺页率: $(echo "scale=1; $TOTAL_FAULT / $DURATION" | bc)/秒"
    fi

    echo ""
else
    echo -e "  ${YELLOW}tracefs 不可用，跳过 ftrace 分析${NC}"
    echo ""
fi

# ==========================================
# Part 4: CPU 使用率分析
# ==========================================
echo -e "${CYAN}${BOLD}4. CPU 使用率分析${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

# 使用 ps 查看 QEMU CPU 使用率
echo "  QEMU 进程 CPU 使用率:"
ps -p "$PID" -o pid,pcpu,pmem,comm --no-headers 2>/dev/null | \
    while read pid cpu mem comm; do
        printf "    PID %-8s CPU: %5s%%  MEM: %5s%%  %s\n" "$pid" "$cpu" "$mem" "$comm"
    done
echo ""

# QEMU 线程分析
echo "  QEMU 线程 CPU 使用率 (Top 5):"
ps -T -p "$PID" -o spid,pcpu,comm --no-headers 2>/dev/null | \
    sort -k2 -rn | head -5 | \
    while read tid cpu comm; do
        printf "    TID %-8s CPU: %5s%%  %s\n" "$tid" "$cpu" "$comm"
    done
echo ""

# ==========================================
# Part 5: 内存使用分析
# ==========================================
echo -e "${CYAN}${BOLD}5. 内存使用分析${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

# 使用 smaps 分析
if [ -f "/proc/$PID/smaps_rollup" ]; then
    echo "  QEMU 内存使用:"
    cat "/proc/$PID/smaps_rollup" | grep -E "Rss|Pss|Shared|Private" | \
        while read line; do
            echo "    $line"
        done
else
    echo "  QEMU 内存使用:"
    cat "/proc/$PID/status" | grep -E "VmRSS|VmSize|VmData|VmStk" | \
        while read line; do
            echo "    $line"
        done
fi
echo ""

# KVM 内存统计
echo "  KVM 内存统计:"
echo "    页面错误 (Host):  $(cat /proc/vmstat 2>/dev/null | grep pgfault | awk '{print $2}')"
echo "    大页分配失败:     $(cat /proc/vmstat 2>/dev/null | grep thp_collapse | awk '{print $2}' || echo 'N/A')"
echo ""

# ==========================================
# Part 6: 性能评估
# ==========================================
echo -e "${CYAN}${BOLD}6. 性能评估${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

if [ "$TOTAL_EXIT" -gt 0 ]; then
    EXIT_RATE=$(echo "scale=0; $TOTAL_EXIT / $DURATION" | bc)

    echo "  VM-Exit 率评估:"
    if [ "$EXIT_RATE" -lt 10000 ]; then
        echo -e "    ${GREEN}✓ 优秀${NC} - VM-Exit 率 ${EXIT_RATE}/秒 (< 10K/s)"
    elif [ "$EXIT_RATE" -lt 50000 ]; then
        echo -e "    ${YELLOW}○ 正常${NC} - VM-Exit 率 ${EXIT_RATE}/秒 (10K-50K/s)"
    elif [ "$EXIT_RATE" -lt 100000 ]; then
        echo -e "    ${YELLOW}△ 偏高${NC} - VM-Exit 率 ${EXIT_RATE}/秒 (50K-100K/s)"
    else
        echo -e "    ${RED}✗ 过高${NC} - VM-Exit 率 ${EXIT_RATE}/秒 (> 100K/s)"
    fi
    echo ""

    # APICv 评估
    EXT_INT=$(echo "$TRACE_DATA" | grep "EXTERNAL_INTERRUPT" | wc -l)
    echo "  APICv/PI 评估:"
    if [ "$EXT_INT" -eq 0 ]; then
        echo -e "    ${GREEN}✓ 优秀${NC} - 无外部中断 VM-Exit（PI 完美工作）"
    elif [ "$EXT_INT" -lt 100 ]; then
        echo -e "    ${GREEN}○ 良好${NC} - 外部中断 VM-Exit 很少"
    else
        echo -e "    ${RED}✗ 需要关注${NC} - 外部中断 VM-Exit: ${EXT_INT} (${DURATION}秒内)"
    fi
    echo ""

    # EPT 评估
    if [ "$TOTAL_FAULT" -gt 0 ]; then
        FAULT_RATE=$(echo "scale=0; $TOTAL_FAULT / $DURATION" | bc)
        echo "  EPT 缺页评估:"
        if [ "$FAULT_RATE" -lt 1000 ]; then
            echo -e "    ${GREEN}✓ 正常${NC} - 缺页率 ${FAULT_RATE}/秒"
        else
            echo -e "    ${YELLOW}△ 偏高${NC} - 缺页率 ${FAULT_RATE}/秒"
            echo "    建议: 考虑使用大页（-mem-prealloc -mem-path /dev/hugepages）"
        fi
        echo ""
    fi
fi

# 输出到文件
if [ -n "$OUTPUT" ]; then
    echo "报告已保存到: $OUTPUT"
fi

echo -e "${GREEN}${BOLD}分析完成！${NC}"
echo ""
echo "进一步优化建议:"
echo "  1. 使用大页内存: -mem-path /dev/hugepages"
echo "  2. 启用 vhost: -netdev tap,vhost=on"
echo "  3. CPU pinning: taskset -c <cpu> qemu..."
echo "  4. 启用 APICv: -cpu host,kvm-pv-eoi=on"
echo "  5. 使用 virtio 设备减少 VM-Exit"
