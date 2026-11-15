#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 修正：使用不同的 PCIe 地址，避免冲突
qemu-system-x86_64 \
  -name kvm-study-pcie-fixed \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=none,id=drive0 \
  -device virtio-blk-pci,drive=drive0,bus=pcie.0,addr=0x4 \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -machine q35,kernel-irqchip=split \
  -device intel-iommu,intremap=on \
  -device pcie-root-port,port=0x10,chassis=1,id=pci.1,bus=pcie.0,multifunction=on,addr=0x2 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,bus=pci.1,addr=0x0 \
  -display none \
  -serial file:/tmp/vm-pcie-fixed.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda1 rw iommu=on"

echo "VM 已启动（修正 PCIe 地址）"
