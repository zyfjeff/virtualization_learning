#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 关键：使用 PCIe root port 将设备放置在 IOMMU 后面
qemu-system-x86_64 \
  -name kvm-study-pcie \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=none,id=drive0 \
  -device virtio-blk-pci,drive=drive0,bus=pcie.0,addr=0x1f \
  -virtfs local,path=../shared,mount_tag=hostshare,security_model=passthrough,id=hostshare \
  -machine q35,kernel-irqchip=split \
  -device intel-iommu,intremap=on \
  -device pcie-root-port,port=0x10,chassis=1,id=pci.1,bus=pcie.0,multifunction=on,addr=0x2 \
  -device pcie-root-port,port=0x11,chassis=2,id=pci.2,bus=pcie.0,addr=0x2.0x1 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0,bus=pci.1,addr=0x0 \
  -display none \
  -serial file:/tmp/vm-pcie.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda1 rw iommu=on"

echo "VM 已启动（PCIe 拓扑配置）"
