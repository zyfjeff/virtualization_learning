#!/bin/bash
#
# trace-msr-access.sh — 宿主侧 ftrace MSR 访问追踪
#
# 用法: sudo ./trace-msr-access.sh [选项]
#   -p PID     QEMU 进程 PID
#   -d SECS    追踪持续时间（默认 10 秒）
#   -s         只显示摘要统计
#   -h         显示帮助
#
# 功能:
#   启用 kvm:kvm_msr trace event，收集 MSR 读/写统计
#   对照 phase1 annotations.md §5 的 MSR Bitmap 分析
#
# 源码引用:
#   arch/x86/kvm/trace.h:428 — TRACE_EVENT(kvm_msr)
#   arch/x86/kvm/vmx/vmx.c:171 — vmx_possible_passthrough_msrs[]
#
# 注意:
#   1. tracefs 是全局状态: 写入 set_event 时不带 -a 会清掉所有已有事件
#      (kernel/trace/trace_events.c:2422)
#   2. 本脚本在 cleanup 时用 >> 追加清空，避免污染下一轮
#   3. kvm:kvm_msr 只记录被 KVM 拦截的 MSR 访问（触发 VM-Exit 的），
#      透传的 MSR 不经过 KVM 模拟，不会产生 trace event
#

set -euo pipefail

TRACEFS=""
PID=""
DURATION=10
SUMMARY_ONLY=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "宿主侧 ftrace MSR 访问追踪"
    echo ""
    echo "选项:"
    echo "  -p PID     QEMU 进程 PID (不指定则自动检测)"
    echo "  -d SECS    追踪持续时间（默认 10 秒）"
    echo "  -s         只显示摘要统计"
    echo "  -h         显示帮助"
    exit 0
}

cleanup() {
    if [ -n "$TRACEFS" ] && [ -d "$TRACEFS" ]; then
        echo 0 > "$TRACEFS/tracing_on" 2>/dev/null || true
        echo > "$TRACEFS/set_event" 2>/dev/null || true
        echo nop > "$TRACEFS/current_tracer" 2>/dev/null || true
        echo > "$TRACEFS/set_ftrace_filter" 2>/dev/null || true
        echo > "$TRACEFS/set_event_pid" 2>/dev/null || true
    fi
}

trap cleanup EXIT

while getopts "p:d:sh" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        s) SUMMARY_ONLY=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    exit 1
fi

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

if [ ! -d "$TRACEFS/events/kvm/kvm_msr" ]; then
    echo -e "${RED}错误: kvm:kvm_msr tracepoint 不可用${NC}"
    exit 1
fi

if [ -z "$PID" ]; then
    PID=$(pgrep -f "qemu-system" | head -1 || echo "")
    if [ -z "$PID" ]; then
        echo -e "${YELLOW}警告: 未找到 QEMU 进程，将追踪所有 KVM MSR 访问${NC}"
    else
        echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
    fi
fi

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  MSR 访问追踪 (ftrace)${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "参数:"
echo "  PID:      ${PID:-所有 KVM}"
echo "  持续时间: ${DURATION} 秒"
echo ""
echo "注意: kvm:kvm_msr 只记录被 KVM 拦截的 MSR"
echo "      透传的 MSR (如 IA32_TSC) 不触发 VM-Exit, 不会出现在 trace 中"
echo ""

# 清空 tracefs 状态
echo > "$TRACEFS/trace"
echo 0 > "$TRACEFS/tracing_on"
echo > "$TRACEFS/set_event"
echo nop > "$TRACEFS/current_tracer"

# 设置 MSR 事件
echo kvm:kvm_msr > "$TRACEFS/set_event"

# PID 过滤 - 必须包括所有 vCPU 线程，不仅仅是主线程
# KVM tracepoint 在 vCPU 线程中触发，不是 QEMU 主线程
if [ -n "$PID" ]; then
    QEMU_TIDS=$(ls /proc/$PID/task/ 2>/dev/null | tr '\n' ' ' | sed 's/ $//')
    if [ -n "$QEMU_TIDS" ]; then
        echo "$QEMU_TIDS" > "$TRACEFS/set_event_pid"
        TID_COUNT=$(echo "$QEMU_TIDS" | wc -w)
        echo "追踪 $TID_COUNT 个线程 (主线程 + vCPU 线程)"
    else
        echo -e "${YELLOW}警告: 无法获取线程列表，将追踪所有 KVM MSR 访问${NC}"
    fi
fi

# 开始追踪
echo "开始追踪..."
echo 1 > "$TRACEFS/tracing_on"
sleep "$DURATION"
echo 0 > "$TRACEFS/tracing_on"
echo "追踪完成。"
echo ""

# 分析结果
TRACE_DATA=$(cat "$TRACEFS/trace")

# 统计总数 - 使用 || true 防止 grep 找不到匹配时脚本退出
TOTAL=$(echo "$TRACE_DATA" | grep -c "msr_" || true)
READS=$(echo "$TRACE_DATA" | grep -c "msr_read" || true)
WRITES=$(echo "$TRACE_DATA" | grep -c "msr_write" || true)
EXCEPTIONS=$(echo "$TRACE_DATA" | grep -c "#GP" || true)

echo -e "${BLUE}=== MSR 访问统计 (${DURATION}秒) ===${NC}"
echo ""
echo "总计:     $TOTAL 次"
echo "  读:     $READS 次"
echo "  写:     $WRITES 次"
echo "  异常:   $EXCEPTIONS 次 (#GP)"
echo ""

if [ "$TOTAL" -gt 0 ]; then
    echo "  速率:   $(echo "scale=0; $TOTAL / $DURATION" | bc) 次/秒"
    echo ""
fi

# 按 MSR 地址统计 (Top 20)
echo -e "${BLUE}--- MSR 访问热榜 (Top 20) ---${NC}"
echo ""
echo "  MSR 地址           次数    读/写   说明"
echo "  ────────────────   ─────   ────   ────────────────────"

echo "$TRACE_DATA" | grep "msr_" | \
    sed -E 's/.*msr_(read|write) ([0-9a-f]+) = 0x[0-9a-f]+.*/\1 \2/' | \
    sort | uniq -c | sort -rn | head -20 | \
    while read count rw msr; do
        # 标注常见 MSR
        case "$msr" in
            10)          desc="IA32_TSC (不应出现=透传)" ;;
            1b)          desc="IA32_APIC_BASE" ;;
            3b)          desc="IA32_TSC_ADJUST" ;;
            48)          desc="IA32_SPEC_CTRL" ;;
            49)          desc="IA32_PRED_CMD" ;;
            8b)          desc="IA32_BIOS_SIGN_ID" ;;
            fe)          desc="IA32_MTRRCAP" ;;
            10b)         desc="IA32_FLUSH_CMD" ;;
            174)         desc="IA32_SYSENTER_CS" ;;
            175)         desc="IA32_SYSENTER_ESP" ;;
            176)         desc="IA32_SYSENTER_EIP" ;;
            1a4)         desc="IA32_PRIMASK" ;;
            1a6)         desc="IA32_SECPLMASK" ;;
            1a9)         desc="IA32_TERTPLMASK" ;;
            6e0)         desc="IA32_TSC_DEADLINE (定时器)" ;;
            c0000080)    desc="IA32_EFER" ;;
            c0000081)    desc="IA32_STAR" ;;
            c0000082)    desc="IA32_LSTAR" ;;
            c0000083)    desc="IA32_CSTAR" ;;
            c0000084)    desc="IA32_FMASK" ;;
            c0000100)    desc="MSR_FS_BASE" ;;
            c0000101)    desc="MSR_GS_BASE" ;;
            c0000102)    desc="MSR_KERNEL_GS_BASE" ;;
            *)           desc="(unknown)" ;;
        esac
        printf "  0x%-16s %-5d %-5s  %s\n" "$msr" "$count" "$rw" "$desc"
    done

echo ""

# 分析透传 vs 拦截
echo -e "${BLUE}--- 透传 vs 拦截分析 ---${NC}"
echo ""
echo "  KVM 透传的 MSR (不出现在 trace 中):"
echo "    IA32_TSC (0x10)           — 只读, 直接读取物理 MSR"
echo "    MSR_FS_BASE (0xC0000100)  — 上下文切换时直接写"
echo "    MSR_GS_BASE (0xC0000101)  — 上下文切换时直接写"
echo "    MSR_KERNEL_GS_BASE        — swapgs 恢复"
echo "    IA32_SYSENTER_CS/ESP/EIP  — syscall 入口"
echo "    C-state residency MSRs    — 只读计数器"
echo ""
echo "  KVM 拦截的 MSR (出现在 trace 中):"
echo "    IA32_EFER (0xC0000080)    — 控制长模式, Guest 写必须拦截"
echo "    IA32_APIC_BASE (0x1B)     — 控制 APIC 模式"
echo "    IA32_STAR/LSTAR/CSTAR     — syscall 入口"
echo "    IA32_CR_PAT               — 页表内存类型"
echo ""

echo -e "${YELLOW}对照: annotations.md §5, vmx.c:171 — vmx_possible_passthrough_msrs[]${NC}"
echo ""
echo -e "${GREEN}完成！${NC}"
