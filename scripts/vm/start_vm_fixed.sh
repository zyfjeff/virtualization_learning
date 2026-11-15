#!/bin/bash
cd /root/code/kvm-study/scripts/vm

# 清理旧进程
pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 关键修正：使用 kernel-irqchip=split 配合 intremap=on
qemu-system-x86_64 \
  -name kvm-study-iommu-fixed \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=virtio \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -machine q35,kernel-irqchip=split \
  -device intel-iommu,intremap=on \
  -device virtio-net-pci,netdev=net0,iommu_platform=on \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -display none \
  -serial file:/tmp/vm-fixed.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda1 rw iommu=on"

echo "VM 已启动（修正配置）"
