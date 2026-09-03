#!/bin/bash
# E1: ftrace 诊断 VM 启动失败
# 用法: sudo ./run.sh [--preflight|--dry-run]

set -euo pipefail

TRACEFS=/sys/kernel/debug/tracing
SCRIPT_NAME=$(basename "$0")

# ── 参数解析 ──
PREFLIGHT=false
DRY_RUN=false
for arg in "$@"; do
    case "$arg" in
        --preflight) PREFLIGHT=true ;;
        --dry-run)   DRY_RUN=true ;;
        --help|-h)
            echo "用法: sudo $SCRIPT_NAME [--preflight|--dry-run]"
            echo "  --preflight  检查环境，不执行诊断"
            echo "  --dry-run    模拟诊断流程，不实际跟踪"
            exit 0
            ;;
    esac
done

# ── Preflight ──
preflight() {
    echo "=== 环境检查 ==="

    # 检查 root
    if [ "$(id -u)" -ne 0 ]; then
        echo "✗ 需要 root 权限"
        return 1
    fi
    echo "✓ root 权限"

    # 检查 KVM 模块
    if ! lsmod | grep -q '^kvm_intel\|^kvm_amd'; then
        echo "✗ KVM 模块未加载"
        return 1
    fi
    echo "✓ KVM 模块已加载"

    # 检查 tracefs
    if [ ! -d "$TRACEFS" ]; then
        echo "✗ tracefs 不存在，尝试 mount..."
        mount -t debugfs none /sys/kernel/debug/ || return 1
    fi
    echo "✓ tracefs 可用"

    # 检查 KVM tracepoints
    if [ ! -d "$TRACEFS/events/kvm" ]; then
        echo "✗ KVM tracepoints 不可用"
        return 1
    fi
    local kvm_events
    kvm_events=$(ls "$TRACEFS/events/kvm/" | wc -l)
    echo "✓ KVM tracepoints: $kvm_events 个事件"

    # 检查运行中的 VM
    local qemu_pid
    qemu_pid=$(pgrep -f '^qemu-system-x86_64' | head -1) || true
    if [ -z "$qemu_pid" ]; then
        echo "⚠ 未检测到运行中的 VM（诊断需要 VM 运行）"
        echo "  启动命令: ../../scripts/vm/boot-vm.sh ubuntu --memory 2G --cpus 2"
    else
        echo "✓ 检测到 QEMU PID: $qemu_pid"
    fi

    # 检查 dump_invalid_vmcs
    if [ -f /sys/module/kvm_intel/parameters/dump_invalid_vmcs ]; then
        local val
        val=$(cat /sys/module/kvm_intel/parameters/dump_invalid_vmcs)
        echo "✓ dump_invalid_vmcs 可用 (当前: $val)"
    else
        echo "⚠ dump_invalid_vmcs 不可用（非 Intel 平台或内核版本不支持）"
    fi

    echo ""
    echo "✓ Preflight 通过"
    return 0
}

# ── Dry-run ──
dry_run() {
    echo "=== Dry-run 模式 ==="
    echo "将执行以下步骤（不实际跟踪）："
    echo ""
    echo "1. 启用 ftrace function tracer"
    echo "   跟踪函数: kvm_dev_ioctl, kvm_vm_ioctl, kvm_vcpu_ioctl,"
    echo "             kvm_arch_vcpu_ioctl_run, vcpu_enter_guest, vmx_vcpu_run"
    echo ""
    echo "2. 启用 KVM tracepoints"
    echo "   事件: kvm_entry, kvm_exit, kvm_inj_exception, kvm_userspace_exit"
    echo ""
    echo "3. 收集 5 秒数据"
    echo ""
    echo "4. 分析输出："
    echo "   - ioctl 调用序列"
    echo "   - VM-Entry / VM-Exit 事件"
    echo "   - exit_reason 分布"
    echo "   - 启动失败检查（FAIL_ENTRY / INTERNAL_ERROR）"
    echo ""
    echo "5. 清理 ftrace 状态"
    echo ""
    echo "✓ Dry-run 完成"
}

# ── 清理函数 ──
cleanup() {
    echo ""
    echo "=== 清理 ==="
    echo none > "$TRACEFS/current_tracer" 2>/dev/null || true
    : > "$TRACEFS/set_ftrace_filter" 2>/dev/null || true
    : > "$TRACEFS/set_event" 2>/dev/null || true
    echo 0 > "$TRACEFS/tracing_on" 2>/dev/null || true
    echo 0 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs 2>/dev/null || true
    echo "✓ ftrace 状态已恢复"
}
trap cleanup EXIT

# ── 主流程 ──
main() {
    if $PREFLIGHT; then
        preflight
        exit $?
    fi

    if $DRY_RUN; then
        dry_run
        exit 0
    fi

    # 先做 preflight
    preflight || exit 1

    echo ""
    echo "=== E1: ftrace 诊断 VM 启动失败 ==="
    echo ""

    # Step 1: 启用 ftrace
    echo "Step 1: 启用 ftrace function tracer"
    : > "$TRACEFS/set_event"
    : > "$TRACEFS/set_ftrace_filter"
    echo none > "$TRACEFS/current_tracer"

    echo function > "$TRACEFS/current_tracer"
    {
        echo kvm_dev_ioctl
        echo kvm_create_vm
        echo kvm_vm_ioctl
        echo kvm_vcpu_ioctl
        echo kvm_arch_vcpu_ioctl_run
        echo vcpu_enter_guest
        echo vmx_vcpu_run
    } > "$TRACEFS/set_ftrace_filter"
    echo "✓ function tracer 已启用"

    # Step 2: 启用 tracepoints
    echo ""
    echo "Step 2: 启用 KVM tracepoints"
    echo kvm:kvm_entry >> "$TRACEFS/set_event"
    echo kvm:kvm_exit >> "$TRACEFS/set_event"
    echo kvm:kvm_inj_exception >> "$TRACEFS/set_event"
    echo kvm:kvm_userspace_exit >> "$TRACEFS/set_event"
    echo "✓ tracepoints 已启用"

    # Step 3: 收集数据
    echo ""
    echo "Step 3: 收集数据（5 秒）"
    echo 1 > "$TRACEFS/tracing_on"
    sleep 5
    echo 0 > "$TRACEFS/tracing_on"
    echo "✓ 数据收集完成"

    # Step 4: 分析
    echo ""
    echo "Step 4: 分析 trace 输出"

    echo ""
    echo "--- ioctl 调用序列（前 20 行）---"
    grep -E 'kvm_(dev_|vm_|vcpu_)ioctl' "$TRACEFS/trace" | head -20 || echo "(无)"

    echo ""
    echo "--- VM-Entry / VM-Exit 事件（前 30 行）---"
    grep -E 'kvm_(entry|exit)' "$TRACEFS/trace" | head -30 || echo "(无)"

    echo ""
    echo "--- VM-Exit 原因分布 ---"
    grep 'kvm_exit' "$TRACEFS/trace" | \
        grep -oP 'reason \w+' | sort | uniq -c | sort -rn | head -10 || echo "(无)"

    echo ""
    echo "--- 启动失败检查 ---"
    local fail_count
    fail_count=$(grep 'kvm_userspace_exit' "$TRACEFS/trace" | \
        grep -cE 'FAIL_ENTRY|INTERNAL_ERROR' || true)
    if [ "$fail_count" -gt 0 ]; then
        echo "⚠ 检测到 $fail_count 次启动失败"
        echo "启用 dump_invalid_vmcs 用于下次诊断..."
        echo 1 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs
    else
        echo "✓ 未检测到启动失败"
    fi

    echo ""
    echo "=== 诊断完成 ==="
}

main
