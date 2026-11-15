#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 使用 pc 机器类型而不是 q35
qemu-system-x86_64 \
  -name kvm-study-pc \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=virtio \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -machine pc,accel=kvm \
  -device intel-iommu \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -display none \
  -serial file:/tmp/vm-pc-machine.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda rw iommu=on pci=assign-busses"

echo "VM 已启动（使用 pc 机器类型）"
