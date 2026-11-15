#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 尝试为每个设备显式启用 IOMMU
qemu-system-x86_64 \
  -name kvm-study-explicit \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=none,id=drive0 \
  -device virtio-blk-pci,drive=drive0,iommu=on \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -machine q35,kernel-irqchip=split \
  -device intel-iommu \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,iommu=on \
  -device virtio-9p-pci,fsdev=hostshare,mount_tag=hostshare,iommu=on \
  -fsdev local,id=hostshare,path=../shared,security_model=passthrough \
  -display none \
  -serial file:/tmp/vm-explicit-iommu.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda rw iommu=on"

echo "VM 已启动（显式设备 IOMMU 配置）"
