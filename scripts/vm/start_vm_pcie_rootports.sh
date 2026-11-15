#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 关键：为每个设备创建独立的 PCIe root port
qemu-system-x86_64 \
  -name kvm-study-pcie-rootports \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -machine q35,kernel-irqchip=split \
  -device intel-iommu \
  \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=none,id=drive0 \
  -device pcie-root-port,port=0x10,chassis=1,id=rp1,multifunction=on,addr=0x1.0x0 \
  -device virtio-blk-pci,drive=drive0,bus=rp1,addr=0x0 \
  \
  -device pcie-root-port,port=0x11,chassis=2,id=rp2,addr=0x1.0x1 \
  -device virtio-9p-pci,fsdev=hostshare,mount_tag=hostshare,bus=rp2,addr=0x0 \
  -fsdev local,id=hostshare,path=../shared,security_model=passthrough \
  \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device pcie-root-port,port=0x12,chassis=3,id=rp3,addr=0x1.0x2 \
  -device virtio-net-pci,netdev=net0,bus=rp3,addr=0x0 \
  \
  -display none \
  -serial file:/tmp/vm-pcie-rootports.log \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda rw iommu=on"

echo "VM 已启动（使用 PCIe root port 拓扑）"
