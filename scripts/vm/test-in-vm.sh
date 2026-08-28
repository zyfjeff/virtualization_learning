#!/bin/bash
# 在 VM 内测试 CPUID Faulting
# 通过串口发送命令并捕获输出

set -e

echo "=== VM CPUID Faulting 测试 ==="
echo ""

# 启动 VM
cd /root/code/kvm-study/scripts/images
qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -kernel bzImage \
    -initrd initramfs.img \
    -append "console=ttyS0" \
    -nographic \
    -m 512 \
    -no-reboot \
    -serial mon:stdio \
    -display none &

VM_PID=$!
echo "VM PID: $VM_PID"
echo "等待 VM 启动..."

# 等待 VM 启动完成（检测 shell 提示符）
sleep 8

echo ""
echo "=== 测试 1: 检查 CPUID Faulting 支持 ==="
# 注意：这里无法直接通过脚本发送命令到串口
# 需要手动交互或使用 expect 工具

echo ""
echo "VM 已启动！请手动测试以下命令："
echo ""
echo "  # 检查 CPUID Faulting 支持"
echo "  grep cpuid_fault /proc/cpuinfo"
echo ""
echo "  # 运行 CPUID Faulting 测试"
echo "  test-cpuid-fault"
echo ""
echo "  # 检查 KVM 支持"
echo "  ls /dev/kvm"
echo "  dmesg | grep -i kvm"
echo ""
echo "  # 退出 VM"
echo "  Ctrl-A X"
echo ""

# 等待用户按 Ctrl-A X 退出
wait $VM_PID 2>/dev/null || true
