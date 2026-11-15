#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 使用 AMD IOMMU 而不是 Intel IOMMU
qemu-system-x86_64 \
  -name kvm-study-amd-iommu \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=virtio \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -machine q35 \
  -device amd-iommu \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -display none \
  -serial file:/tmp/vm-amd-iommu.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda rw iommu=on"

echo "VM 已启动（使用 AMD IOMMU）"
