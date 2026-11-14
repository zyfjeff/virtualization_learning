#!/bin/bash
# VM 环境测试脚本（修复版）

set -e

echo "=========================================="
echo "  VM 环境测试"
echo "=========================================="
echo ""

# 检查 1: VM 进程
echo "✓ 检查 1: VM 进程状态"
VM_PIDS=$(pgrep -f "qemu.*kvm-study" || true)
if [ -n "$VM_PIDS" ]; then
    VM_PID=$(echo "$VM_PIDS" | head -1)
    VM_COUNT=$(echo "$VM_PIDS" | wc -l)
    echo "  ✓ VM 正在运行 (PID: $VM_PID, 共 $VM_COUNT 个进程)"
    ps -p $VM_PID -o pid,pcpu,pmem,etime --no-headers 2>/dev/null | awk '    {printf "    CPU: %s%%, MEM: %s%%, 运行时间: %s\n", $2, $3, $4}' || true
else
    echo "  ✗ VM 未运行"
    echo "  请先启动 VM: ./boot-vm.sh ubuntu --net user"
    exit 1
fi
echo ""

# 检查 2: KVM 加速
echo "✓ 检查 2: KVM 加速"
KVM_FD_COUNT=$(ls -l /proc/$VM_PID/fd 2>/dev/null | grep -c kvm || echo "0")
if [ "$KVM_FD_COUNT" -gt 0 ]; then
    echo "  ✓ 使用 KVM 加速 (KVM fd 数量: $KVM_FD_COUNT)"
else
    echo "  ✗ 未使用 KVM 加速（可能使用 TCG）"
fi
echo ""

# 检查 3: 网络端口
echo "✓ 检查 3: 网络端口"
if ss -tlnp 2>/dev/null | grep -q ":2222"; then
    echo "  ✓ SSH 端口 2222 已监听"
else
    echo "  ⚠ SSH 端口 2222 未监听（可能还在启动中）"
fi
echo ""

# 检查 4: 串口日志
echo "✓ 检查 4: VM 启动状态"
VM_LOG="/tmp/vm-boot.log"
if [ -f "$VM_LOG" ]; then
    if grep -q "login:" "$VM_LOG"; then
        echo "  ✓ VM 已启动到登录提示符"
        echo "  最后 3 行日志："
        tail -3 "$VM_LOG" | sed 's/^/    /'
    else
        echo "  ⚠ VM 可能还在启动中"
    fi
else
    echo "  ⚠ 未找到 VM 日志文件"
fi
echo ""

# 检查 5: 共享目录
echo "✓ 检查 5: 共享目录"
SHARED_DIR="/root/code/kvm-study/scripts/shared"
if [ -d "$SHARED_DIR" ]; then
    echo "  ✓ 共享目录存在: $SHARED_DIR"
    FILE_COUNT=$(ls -1 "$SHARED_DIR" 2>/dev/null | wc -l)
    echo "    文件数量: $FILE_COUNT"
else
    echo "  ✗ 共享目录不存在"
fi
echo ""

# 检查 6: IOMMU 支持（可选）
echo "✓ 检查 6: IOMMU 配置"
if pgrep -f "qemu.*intel-iommu" > /dev/null; then
    echo "  ✓ vIOMMU 已启用"
else
    echo "  ℹ vIOMMU 未启用（标准配置）"
fi
echo ""

echo "=========================================="
echo "  测试完成"
echo "=========================================="
echo ""
echo "下一步操作："
echo "  1. 通过串口控制台访问 VM（推荐）"
echo "     直接在运行 VM 的终端操作"
echo ""
echo "  2. 尝试 SSH 连接（可能失败）"
echo "     ssh -p 2222 root@localhost"
echo ""
echo "  3. 查看 VM 日志"
echo "     tail -f /tmp/vm-boot.log"
echo ""
echo "  4. 停止 VM"
echo "     pkill -9 -f 'qemu.*kvm-study'"
