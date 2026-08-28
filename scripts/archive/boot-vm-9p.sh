#!/bin/bash
# 使用 9p 共享目录启动 VM（无需 initramfs）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"
SHARED_DIR="$PROJECT_ROOT/shared"

# 确保共享目录存在
mkdir -p "$SHARED_DIR"

# 复制测试程序到共享目录
log_info() { echo -e "\033[0;32m[INFO]\033[0m $1"; }

log_info "准备共享目录..."
cp -v "$PROJECT_ROOT/examples/cpuid-faulting-demo/test-cpuid-fault" "$SHARED_DIR/" 2>/dev/null || true
cp -v "$PROJECT_ROOT/examples/cpuid-faulting-demo/test-cpuid-fault-kvm" "$SHARED_DIR/" 2>/dev/null || true
cp -v "$PROJECT_ROOT/examples/kvm-api-demo/kvm-demo" "$SHARED_DIR/" 2>/dev/null || true

log_info "启动 VM（使用 9p 共享目录）..."

cd "$IMAGES_DIR"

qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -kernel bzImage \
    -append "root=/dev/vda console=ttyS0 rw" \
    -drive file=rootfs.ext4,format=raw,if=virtio \
    -virtfs local,path="$SHARED_DIR",mount_tag=hostshare,security_model=passthrough,id=hostshare \
    -nographic \
    -m 512 \
    -no-reboot
