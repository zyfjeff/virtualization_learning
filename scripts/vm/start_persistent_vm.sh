#!/bin/bash
cd /root/code/kvm-study/scripts/vm

# 使用 screen 或 tmux 在后台运行 VM
# 或者使用 -daemonize 参数（如果 QEMU 支持）

# 方法 1：使用 -daemonize
qemu-system-x86_64 \
  -name kvm-study-vm \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=virtio \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -machine q35 \
  -device intel-iommu \
  -display none \
  -serial file:/tmp/vm-serial.log \
  -monitor unix:/tmp/qemu-monitor.sock,server,nowait \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda1 rw"

echo "VM 已启动（守护进程模式）"
echo "串口日志: /tmp/vm-serial.log"
echo "Monitor: /tmp/qemu-monitor.sock"
