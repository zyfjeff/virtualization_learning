#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 关键：使用 iommu_platform=on 将 virtio 设备放在 IOMMU 后面
qemu-system-x86_64 \
  -name kvm-study-iommu-platform \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=none,id=drive0 \
  -device virtio-blk-pci,drive=drive0,iommu_platform=on \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -machine q35,kernel-irqchip=split \
  -device intel-iommu \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,iommu_platform=on \
  -display none \
  -serial file:/tmp/vm-iommu-platform.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda rw iommu=on"

echo "VM 已启动（使用 iommu_platform）"
