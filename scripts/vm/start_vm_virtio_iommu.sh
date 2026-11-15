#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 使用 virtio-iommu 而不是 intel-iommu
qemu-system-x86_64 \
  -name kvm-study-virtio-iommu \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -nodefaults \
  -machine q35,accel=kvm \
  \
  -device virtio-iommu-pci,id=virtio_iommu \
  \
  -device isa-serial,chardev=serial0 \
  -chardev file,id=serial0,path=/tmp/vm-virtio-iommu-serial.log \
  \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=none,id=drive0 \
  -device virtio-blk-pci,drive=drive0,iommu_device=virtio_iommu \
  \
  -fsdev local,id=hostshare,path=../shared,security_model=passthrough \
  -device virtio-9p-pci,fsdev=hostshare,mount_tag=hostshare,iommu_device=virtio_iommu \
  \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,iommu_device=virtio_iommu \
  \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda rw"

echo "VM 已启动（使用 virtio-iommu）"
