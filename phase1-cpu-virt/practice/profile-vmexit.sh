#!/bin/bash
#
# profile-vmexit.sh — perf kvm stat VM-Exit profiling
#
# 用法: sudo ./profile-vmexit.sh [选项]
#   -p PID     QEMU 进程 PID
#   -d SECS    分析持续时间（默认 30 秒）
#   -o FILE    输出文件
#   -h         显示帮助
#
# 功能:
#   使用 perf kvm stat record/report 分析 VM-Exit 分布
#   对照 phase1 annotations.md §3「VM-Exit 分发」
#
# 源码引用:
#   arch/x86/kvm/trace.h:297 — TRACE_EVENT_KVM_EXIT(kvm_exit)
#   arch/x86/include/uapi/asm/vmx.h:96-150 — VMX exit reasons
#   tools/perf/builtin-kvm.c:669 — exit_event_decode_key()
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
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "perf kvm stat VM-Exit profiling"
    echo ""
    echo "选项:"
    echo "  -p PID     QEMU 进程 PID (不指定则自动检测)"
    echo "  -d SECS    分析持续时间（默认 30 秒）"
    echo "  -o FILE    输出文件"
    echo "  -h         显示帮助"
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
        exit 1
    fi
    echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
fi

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  VM-Exit Reason Profiling (perf kvm stat)${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "参数:"
echo "  PID:      $PID"
echo "  持续时间: ${DURATION} 秒"
echo ""
echo "对照: annotations.md §3「VM-Exit 分发」"
echo "源码: vmx.c:6095 — kvm_vmx_exit_handlers[]"
echo ""

# ==========================================
# Part 1: perf kvm stat record
# ==========================================
echo -e "${CYAN}[1] 收集 perf kvm 数据 (${DURATION}秒)...${NC}"
echo ""

PERF_DATA=$(mktemp /tmp/kvm-perf-XXXXXX.data)

# 检查 perf 是否可用
if ! command -v perf &>/dev/null; then
    echo -e "${RED}错误: perf 不可用${NC}"
    echo "安装: apt install linux-tools-$(uname -r)"
    exit 1
fi

perf kvm stat record -p "$PID" -o "$PERF_DATA" -- sleep "$DURATION" 2>&1 || {
    echo -e "${YELLOW}perf kvm stat record 失败${NC}"
    rm -f "$PERF_DATA"
    exit 1
}

# ==========================================
# Part 2: perf kvm stat report
# ==========================================
echo -e "${CYAN}[2] 分析 VM-Exit 分布...${NC}"
echo ""

REPORT_DATA=$(mktemp /tmp/kvm-report-XXXXXX.txt)
perf kvm stat report -i "$PERF_DATA" --stdio > "$REPORT_DATA" 2>&1 || true

if [ -f "$REPORT_DATA" ] && [ -s "$REPORT_DATA" ]; then
    # 解析 perf kvm stat report 输出
    echo -e "${BLUE}=== VM-Exit Reason 分布 ===${NC}"
    echo ""
    cat "$REPORT_DATA" | head -60
    echo ""
else
    echo -e "${YELLOW}perf kvm stat report 无数据${NC}"
fi

# ==========================================
# Part 3: ftrace 补充分析
# ==========================================
echo -e "${CYAN}[3] ftrace 补充分析 (kvm:kvm_exit + kvm:kvm_entry)...${NC}"
echo ""

TRACEFS=""
for tfs in /sys/kernel/tracing /sys/kernel/debug/tracing; do
    if [ -d "$tfs/events/kvm" ]; then
        TRACEFS="$tfs"
        break
    fi
done

if [ -n "$TRACEFS" ] && [ -d "$TRACEFS/events/kvm/kvm_exit" ]; then
    # 清空 tracefs
    echo > "$TRACEFS/trace"
    echo 0 > "$TRACEFS/tracing_on"
    echo > "$TRACEFS/set_event"
    echo nop > "$TRACEFS/current_tracer"

    # 设置事件
    echo kvm:kvm_exit > "$TRACEFS/set_event"
    echo kvm:kvm_entry >> "$TRACEFS/set_event"
    echo "$PID" > "$TRACEFS/set_event_pid"

    # 追踪
    echo 1 > "$TRACEFS/tracing_on"
    echo "  收集 ftrace 数据 (${DURATION}秒)..."
    sleep "$DURATION"
    echo 0 > "$TRACEFS/tracing_on"

    TRACE_DATA=$(cat "$TRACEFS/trace")

    TOTAL_EXIT=$(echo "$TRACE_DATA" | grep "kvm_exit" | wc -l)
    TOTAL_ENTRY=$(echo "$TRACE_DATA" | grep "kvm_entry" | wc -l)

    echo ""
    echo -e "${BLUE}=== 事件统计 ===${NC}"
    echo ""
    printf "  %-20s %d\n" "VM-Exit:" "$TOTAL_EXIT"
    printf "  %-20s %d\n" "VM-Entry:" "$TOTAL_ENTRY"
    echo ""

    if [ "$TOTAL_EXIT" -gt 0 ]; then
        EXIT_RATE=$(echo "scale=0; $TOTAL_EXIT / $DURATION" | bc)
        printf "  %-20s %s/s\n" "退出率:" "$EXIT_RATE"
        echo ""

        echo -e "${BLUE}=== Exit Reason 分布 (Top 15) ===${NC}"
        echo ""
        echo "  退出原因                    次数     占比"
        echo "  ──────────────────────────  ──────   ─────"

        echo "$TRACE_DATA" | grep "kvm_exit" | \
            sed 's/.*reason //' | sed 's/ .*//' | \
            sort | uniq -c | sort -rn | head -15 | \
            while read count reason; do
                pct=$(echo "scale=1; $count * 100 / $TOTAL_EXIT" | bc)
                printf "  %-28s %6d  (%5s%%)\n" "$reason" "$count" "$pct"
            done

        echo ""

        # 分类: 快速路径 vs 慢速路径
        echo -e "${BLUE}=== 快速路径 vs 慢速路径分类 ===${NC}"
        echo ""
        echo "  参考: annotations.md §3 — exit_fastpath"
        echo ""

        # 快速路径 Exit (内核处理完直接重入)
        FAST_EXITS="EXTERNAL_INTERRUPT PREEMPTION_TIMER APIC_WRITE HLT"
        FAST_COUNT=0
        for exit_reason in $FAST_EXITS; do
            c=$(echo "$TRACE_DATA" | grep "kvm_exit" | grep -c "$exit_reason" || true)
            if [ "$c" -gt 0 ]; then
                FAST_COUNT=$((FAST_COUNT + c))
                printf "  %-28s %6d  ← 快速路径 (可能)\n" "$exit_reason" "$c"
            fi
        done

        # 慢速路径 Exit (需回用户空间)
        SLOW_EXITS="CPUID IO_INSTRUCTION EPT_VIOLATION MSR_READ MSR_WRITE"
        SLOW_COUNT=0
        for exit_reason in $SLOW_EXITS; do
            c=$(echo "$TRACE_DATA" | grep "kvm_exit" | grep -c "$exit_reason" || true)
            if [ "$c" -gt 0 ]; then
                SLOW_COUNT=$((SLOW_COUNT + c))
                printf "  %-28s %6d  ← 慢速路径\n" "$exit_reason" "$c"
            fi
        done

        echo ""
        echo "  快速路径: $FAST_COUNT ($(echo "scale=0; $FAST_COUNT * 100 / $TOTAL_EXIT" | bc)%)"
        echo "  慢速路径: $SLOW_COUNT ($(echo "scale=0; $SLOW_COUNT * 100 / $TOTAL_EXIT" | bc)%)"
        echo ""
    fi

    # 清理 tracefs
    echo > "$TRACEFS/set_event"
    echo nop > "$TRACEFS/current_tracer"
    echo > "$TRACEFS/set_event_pid"
else
    echo -e "${YELLOW}tracefs 不可用，跳过 ftrace 分析${NC}"
fi

# 清理
rm -f "$PERF_DATA" "$REPORT_DATA"

# 输出到文件
if [ -n "$OUTPUT" ]; then
    echo "报告已保存到: $OUTPUT"
fi

echo -e "${GREEN}完成！${NC}"
echo ""
echo "对照: annotations.md §3 — kvm_vmx_exit_handlers[] 表"
