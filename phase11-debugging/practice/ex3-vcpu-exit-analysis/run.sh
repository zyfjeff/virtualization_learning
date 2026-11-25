#!/bin/bash
# E3: vCPU 异常退出分析
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

    # 检查 QEMU 日志
    if [ -f /tmp/qemu.log ]; then
        echo "✓ QEMU 日志: /tmp/qemu.log ($(wc -l < /tmp/qemu.log) 行)"
    else
        echo "⚠ /tmp/qemu.log 不存在"
        echo "  启动 VM 时加: -D /tmp/qemu.log -d kvm"
    fi

    pgrep -f '^qemu-system-x86_64' >/dev/null || {
        echo "⚠ 未检测到 VM"
    }

    echo "✓ Preflight 通过"
}

dry_run() {
    cat <<'EOF'
=== Dry-run 模式 ===

将执行以下步骤：

1. 检查 QEMU 日志（/tmp/qemu.log）
   → 查找 KVM_EXIT_* 原因

2. ftrace 跟踪退出事件（10 秒）
   → kvm_exit, kvm_inj_exception, kvm_inj_virq, kvm_userspace_exit

3. 分析退出模式
   → 退出原因分布、异常注入、triple fault

4. bpftrace 退出频率统计
   → 按 exit_reason 聚合

5. 诊断高频退出
   → EPT_VIOLATION / EXTERNAL_INTERRUPT / IO_INSTRUCTION

总计约 15 秒。

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
    echo "=== E3: vCPU 异常退出分析 ==="
    echo ""

    # Step 1: QEMU 日志
    echo "Step 1: 检查 QEMU 日志"
    if [ -f /tmp/qemu.log ]; then
        echo "--- KVM_EXIT 原因 ---"
        grep -E 'KVM_EXIT_|kvm_exit' /tmp/qemu.log | tail -10 || echo "(无)"
        echo ""
        echo "--- 内部错误 ---"
        grep -i 'internal error\|kvm internal' /tmp/qemu.log || echo "(无)"
    else
        echo "⚠ /tmp/qemu.log 不存在，跳过"
    fi

    # Step 2: ftrace
    echo ""
    echo "Step 2: ftrace 跟踪（10 秒）"
    : > "$TRACEFS/set_event"
    echo kvm:kvm_exit >> "$TRACEFS/set_event"
    echo kvm:kvm_inj_exception >> "$TRACEFS/set_event"
    echo kvm:kvm_inj_virq >> "$TRACEFS/set_event"
    echo kvm:kvm_userspace_exit >> "$TRACEFS/set_event"
    echo 1 > "$TRACEFS/tracing_on"
    sleep 10
    echo 0 > "$TRACEFS/tracing_on"

    # Step 3: 分析
    echo ""
    echo "Step 3: 分析退出模式"
    echo "--- 退出原因分布 ---"
    grep 'kvm_exit' "$TRACEFS/trace" | \
        grep -oP 'reason \w+' | sort | uniq -c | sort -rn | head -15 || echo "(无)"

    echo ""
    echo "--- 异常注入 ---"
    grep 'kvm_inj_exception' "$TRACEFS/trace" | tail -10 || echo "(无)"

    echo ""
    echo "--- Triple fault 检查 ---"
    local inj_count
    inj_count=$(grep -c 'kvm_inj_exception' "$TRACEFS/trace" || true)
    echo "异常注入次数: $inj_count"
    if [ "$inj_count" -gt 0 ]; then
        echo "异常分布:"
        grep 'kvm_inj_exception' "$TRACEFS/trace" | \
            grep -oP 'exception=\w+' | sort | uniq -c | sort -rn
    fi

    # Step 4: bpftrace
    echo ""
    echo "Step 4: bpftrace 退出频率（5 秒）"
    timeout 6 bpftrace -e '
tracepoint:kvm:kvm_exit {
    @exits[args->exit_reason] = count();
    @total = count();
}
interval:s:5 {
    printf("Total: %d\n", @total);
    print(@exits, 10);
    exit();
}
' 2>/dev/null || echo "(bpftrace 超时或无数据)"

    echo ""
    echo "=== 诊断完成 ==="
}

main
