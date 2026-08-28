#!/bin/bash
# 使用 ext4 rootfs 启动 VM（支持完整的 Debian 系统）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"
SHARED_DIR="$PROJECT_ROOT/shared"

# 颜色输出
log_info() { echo -e "\033[0;32m[INFO]\033[0m $1"; }

# 检查镜像
check_images() {
    if [ ! -f "$IMAGES_DIR/bzImage" ]; then
        echo "错误: 内核镜像不存在"
        echo "请先运行: ./build-kernel.sh"
        exit 1
    fi

    if [ ! -f "$IMAGES_DIR/rootfs.ext4" ]; then
        echo "错误: rootfs.ext4 不存在"
        echo "请先运行: ./build-rootfs-ext4.sh"
        exit 1
    fi
}

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - VM Boot (ext4 rootfs)"
    echo "========================================"
    echo ""

    check_images

    log_info "启动 VM..."
    echo ""
    echo "  提示:"
    echo "    - 根文件系统: rootfs.ext4 (Debian)"
    echo "    - 共享目录: $SHARED_DIR → /mnt/shared"
    echo "    - 退出 VM:  poweroff 或 Ctrl-A X"
    echo ""

    cd "$IMAGES_DIR"

    qemu-system-x86_64 \
        -enable-kvm \
        -cpu host \
        -kernel bzImage \
        -drive file=rootfs.ext4,format=raw,if=virtio \
        -append "root=/dev/vda console=ttyS0 rw" \
        -virtfs local,path="$SHARED_DIR",mount_tag=hostshare,security_model=passthrough,id=hostshare \
        -nographic \
        -m 1024 \
        -no-reboot
}

main "$@"
