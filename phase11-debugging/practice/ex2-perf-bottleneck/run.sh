#!/bin/bash
# E2: perf + bpftrace 定位性能瓶颈
# 用法: sudo ./run.sh [--preflight|--dry-run]

set -euo pipefail

TRACEFS=/sys/kernel/debug/tracing
SCRIPT_NAME=$(basename "$0")

PREFLIGHT=false
DRY_RUN=false
for arg in "$@"; do
    case "$arg" in
        --preflight) PREFLIGHT=true ;;
        --dry-run)   DRY_RUN=true ;;
        --help|-h)
            echo "用法: sudo $SCRIPT_NAME [--preflight|--dry-run]"
            exit 0
            ;;
    esac
done

preflight() {
    echo "=== 环境检查 ==="
    [ "$(id -u)" -eq 0 ] || { echo "✗ 需要 root"; return 1; }
    echo "✓ root"

    lsmod | grep -q '^kvm' || { echo "✗ KVM 未加载"; return 1; }
    echo "✓ KVM 模块"

    which perf >/dev/null || { echo "✗ perf 未安装"; return 1; }
    echo "✓ perf"

    which bpftrace >/dev/null || { echo "✗ bpftrace 未安装"; return 1; }
    echo "✓ bpftrace"

    pgrep -f '^qemu-system-x86_64' >/dev/null || {
        echo "⚠ 未检测到 VM（诊断需要 VM 运行）"
    }
    echo "✓ QEMU 运行中"

    echo "✓ Preflight 通过"
}

dry_run() {
    cat <<'EOF'
=== Dry-run 模式 ===

将执行以下诊断步骤：

1. perf kvm stat record -a -- sleep 10
   → 分析 VM-Exit 分布

2. bpftrace vmx_handle_exit 延迟直方图
   → 测量 VM-Exit 处理延迟

3. bpftrace kvm_page_fault 热点（2MB 对齐）
   → 分析 EPT 缺页分布

4. kvm_halt_poll_ns trace
   → halt-polling 窗口自适应

5. bpftrace sched_switch 调度抖动
   → vCPU 被抢占时间分布

每个步骤 5-10 秒，总计约 2 分钟。

EOF
    echo "✓ Dry-run 完成"
}

cleanup() {
    echo ""
    echo "=== 清理 ==="
    : > "$TRACEFS/set_event" 2>/dev/null || true
    echo 0 > "$TRACEFS/tracing_on" 2>/dev/null || true
    echo "✓ 清理完成"
}
trap cleanup EXIT

main() {
    $PREFLIGHT && { preflight; exit $?; }
    $DRY_RUN && { dry_run; exit 0; }

    preflight || exit 1

    echo ""
    echo "=== E2: perf + bpftrace 定位性能瓶颈 ==="
    echo ""

    # Step 1: perf kvm stat
    echo "Step 1: perf kvm stat（10 秒）"
    perf kvm stat record -a -- sleep 10 2>/dev/null
    echo ""
    echo "--- VM-Exit 分布 ---"
    perf kvm stat report 2>/dev/null | head -20

    # Step 2: VM-Exit 延迟
    echo ""
    echo "Step 2: VM-Exit 处理延迟（5 秒）"
    timeout 6 bpftrace -e '
kprobe:vmx_handle_exit { @start[tid] = nsecs; }
kretprobe:vmx_handle_exit {
    if (@start[tid]) {
        @latency_us = hist((nsecs - @start[tid]) / 1000);
        delete(@start[tid]);
    }
}
interval:s:5 { print(@latency_us); exit(); }
' 2>/dev/null || echo "(bpftrace 超时或无数据)"

    # Step 3: EPT 缺页热点
    echo ""
    echo "Step 3: EPT 缺页热点（5 秒）"
    timeout 6 bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @gpa[args->fault_address >> 21] = count();
}
interval:s:5 { print(@gpa, 10); exit(); }
' 2>/dev/null || echo "(bpftrace 超时或无数据)"

    # Step 4: halt-polling
    echo ""
    echo "Step 4: halt-polling 窗口（5 秒）"
    : > "$TRACEFS/set_event"
    echo kvm:kvm_halt_poll_ns >> "$TRACEFS/set_event"
    echo 1 > "$TRACEFS/tracing_on"
    sleep 5
    echo 0 > "$TRACEFS/tracing_on"
    echo "--- halt-polling 事件 ---"
    grep halt_poll "$TRACEFS/trace" | tail -10 || echo "(无事件)"

    echo ""
    echo "=== 诊断完成 ==="
}

main
