#!/bin/bash
cd /root/code/kvm-study/scripts/vm

pkill -9 qemu-system 2>/dev/null || true
sleep 2

# 精确的 PCIe 拓扑：
# - 使用 pcie-root-port 创建设备槽位
# - 将每个设备放在独立的 root port 后面
# - 使用正确的地址分配避免冲突

qemu-system-x86_64 \
  -name kvm-study-precise \
  -m 2G -smp 2 \
  -kernel ../images/bzImage \
  -initrd ../images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -nodefaults \
  -machine q35,kernel-irqchip=split,accel=kvm \
  \
  -device intel-iommu \
  \
  -device isa-serial,chardev=serial0 \
  -chardev file,id=serial0,path=/tmp/vm-precise-serial.log \
  \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=none,id=drive0 \
  -device pcie-root-port,port=0x10,chassis=1,id=pci.1,bus=pcie.0,addr=0x1.0x0,multifunction=on \
  -device virtio-blk-pci,drive=drive0,bus=pci.1,addr=0x0 \
  \
  -fsdev local,id=hostshare,path=../shared,security_model=passthrough \
  -device pcie-root-port,port=0x11,chassis=2,id=pci.2,bus=pcie.0,addr=0x1.0x1 \
  -device virtio-9p-pci,fsdev=hostshare,mount_tag=hostshare,bus=pci.2,addr=0x0 \
  \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device pcie-root-port,port=0x12,chassis=3,id=pci.3,bus=pcie.0,addr=0x1.0x2 \
  -device virtio-net-pci,netdev=net0,bus=pci.3,addr=0x0 \
  \
  -device pcie-root-port,port=0x13,chassis=4,id=pci.4,bus=pcie.0,addr=0x1.0x3 \
  -device virtio-vga,bus=pci.4,addr=0x0 \
  \
  -daemonize \
  -append "console=ttyS0 root=/dev/vda rw iommu=on pci=realloc"

echo "VM 已启动（精确 PCIe 拓扑）"
