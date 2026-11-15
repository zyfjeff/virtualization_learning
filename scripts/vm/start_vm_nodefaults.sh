#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 关键：使用 -nodefaults 避免默认设备，然后手动添加所有设备
qemu-system-x86_64 \
  -name kvm-study-nodefaults \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -nodefaults \
  -machine q35,kernel-irqchip=split,accel=kvm \
  -device intel-iommu \
  \
  -chardev stdio,id=serial0 \
  -device isa-serial,chardev=serial0 \
  \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=none,id=drive0 \
  -device pcie-root-port,port=0x10,chassis=1,id=rp1,bus=pcie.0,addr=0x2 \
  -device virtio-blk-pci,drive=drive0,bus=rp1,addr=0x0 \
  \
  -fsdev local,id=hostshare,path=../shared,security_model=passthrough \
  -device pcie-root-port,port=0x11,chassis=2,id=rp2,bus=pcie.0,addr=0x3 \
  -device virtio-9p-pci,fsdev=hostshare,mount_tag=hostshare,bus=rp2,addr=0x0 \
  \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device pcie-root-port,port=0x12,chassis=3,id=rp3,bus=pcie.0,addr=0x4 \
  -device virtio-net-pci,netdev=net0,bus=rp3,addr=0x0 \
  \
  -serial file:/tmp/vm-nodefaults.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda rw iommu=on"

echo "VM 已启动（使用 -nodefaults）"
