#!/bin/bash
# 启动 VM 并自动执行实验

cd /root/code/kvm-study/scripts/vm

echo "启动 VM（带自动执行）..."

# 使用自定义内核参数启动 VM
nohup qemu-system-x86_64 \
  -name kvm-study-auto \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=virtio \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -machine q35,kernel-irqchip=split \
  -device intel-iommu,intremap=on \
  -nographic \
  -serial mon:stdio \
  -append "console=ttyS0 root=/dev/vda1 rw init=/mnt/shared/auto_execute_exp2.sh" \
  > /tmp/vm-auto-exec.log 2>&1 &

VM_PID=$!
echo "VM PID: $VM_PID"
echo ""
echo "VM 正在启动，实验将自动执行..."
echo "日志文件: /tmp/vm-auto-exec.log"
echo "结果文件: /root/code/kvm-study/scripts/shared/exp2_result.txt"
echo ""
echo "等待 VM 启动和实验执行（约 60 秒）..."

# 等待实验完成
for i in {1..60}; do
    if [ -f "../shared/exp2_result.txt" ]; then
        echo "✓ 实验结果已生成（$((i*2))秒）"
        break
    fi
    if ! ps -p $VM_PID > /dev/null 2>&1; then
        echo "✓ VM 已停止"
        break
    fi
    sleep 2
done

echo ""
echo "=== 实验结果 ==="
if [ -f "../shared/exp2_result.txt" ]; then
    cat ../shared/exp2_result.txt
else
    echo "检查结果文件..."
    ls -la ../shared/*.txt 2>/dev/null || echo "未找到结果文件"
    echo ""
    echo "查看 VM 日志："
    tail -50 /tmp/vm-auto-exec.log
fi
