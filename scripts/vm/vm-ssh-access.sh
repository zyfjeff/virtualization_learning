#!/bin/bash
# 可靠的 VM SSH 访问脚本

VM_PORT=2222
MAX_RETRIES=10
RETRY_DELAY=5

echo "=== VM SSH 访问工具 ==="
echo ""

# 检查 VM 是否运行
if ! pgrep -f "qemu.*kvm-study" > /dev/null; then
    echo "错误: VM 未运行"
    echo "请先启动 VM: ./boot-vm.sh ubuntu --net user"
    exit 1
fi

echo "✓ VM 进程正在运行"

# 检查端口
if ! ss -tlnp | grep -q ":$VM_PORT"; then
    echo "错误: SSH 端口 $VM_PORT 未监听"
    exit 1
fi

echo "✓ SSH 端口 $VM_PORT 已监听"
echo ""

# 尝试 SSH 连接
echo "尝试 SSH 连接（最多 $MAX_RETRIES 次）..."
for i in $(seq 1 $MAX_RETRIES); do
    echo -n "  尝试 $i/$MAX_RETRIES... "
    
    if timeout 10 ssh -o StrictHostKeyChecking=no \
                     -o UserKnownHostsFile=/dev/null \
                     -o ConnectTimeout=5 \
                     -o BatchMode=yes \
                     -p $VM_PORT root@localhost \
                     "echo '连接成功'; hostname; uname -r" 2>&1 | grep -q "连接成功"; then
        echo "✓ 成功！"
        echo ""
        echo "=== VM 信息 ==="
        ssh -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile=/dev/null \
            -o ConnectTimeout=5 \
            -p $VM_PORT root@localhost \
            "echo '主机名:' \$(hostname)
             echo '内核版本:' \$(uname -r)
             echo '运行时间:' \$(uptime -p)
             echo '内存使用:' \$(free -h | grep Mem | awk '{print \$3\"/\"\$2}')
             echo 'IOMMU 组数:' \$(ls /sys/kernel/iommu_groups/ 2>/dev/null | wc -l)"
        exit 0
    else
        echo "失败（$((i * RETRY_DELAY))秒后重试）"
        sleep $RETRY_DELAY
    fi
done

echo ""
echo "✗ SSH 连接失败"
echo ""
echo "建议："
echo "1. 通过串口控制台访问 VM（直接在运行 VM 的终端操作）"
echo "2. 检查 VM 内的 SSH 服务: systemctl status ssh"
echo "3. 手动启动 SSH: systemctl start ssh"
exit 1
