#!/bin/bash
# 构建最小 rootfs.ext4 镜像（无需 initramfs）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"
ROOTFS_DIR="$IMAGES_DIR/rootfs-work"

# 颜色输出
log_info() { echo -e "\033[0;32m[INFO]\033[0m $1"; }
log_warn() { echo -e "\033[0;33m[WARN]\033[0m $1"; }
log_error() { echo -e "\033[0;31m[ERROR]\033[0m $1"; }

# 检查依赖
check_dependencies() {
    log_info "检查依赖..."

    if ! command -v debootstrap &>/dev/null; then
        log_error "debootstrap 未安装"
        echo "  Ubuntu/Debian: sudo apt install debootstrap"
        exit 1
    fi

    if ! command -v qemu-img &>/dev/null; then
        log_error "qemu-img 未安装"
        echo "  Ubuntu/Debian: sudo apt install qemu-utils"
        exit 1
    fi

    log_info "✓ 依赖检查通过"
}

# 创建 ext4 镜像
create_ext4_image() {
    local size_mb=${1:-256}  # 默认 256MB

    log_info "创建 ext4 镜像 (${size_mb}MB)..."

    cd "$IMAGES_DIR"

    # 创建空的磁盘镜像
    qemu-img create -f raw rootfs.ext4 ${size_mb}M

    # 创建 ext4 文件系统
    mkfs.ext4 -F rootfs.ext4

    # 挂载镜像
    mkdir -p "$ROOTFS_DIR"
    mount -o loop rootfs.ext4 "$ROOTFS_DIR"

    log_info "✓ ext4 镜像创建完成"
}

# 使用 debootstrap 安装最小 Debian 系统
install_debian() {
    log_info "安装最小 Debian 系统..."

    # 使用 debootstrap 安装最小系统
    debootstrap --arch=amd64 --variant=minbase \
        bookworm "$ROOTFS_DIR" \
        http://deb.debian.org/debian

    log_info "✓ Debian 系统安装完成"
}

# 配置系统
configure_system() {
    log_info "配置系统..."

    # 设置 hostname
    echo "kvm-test" > "$ROOTFS_DIR/etc/hostname"

    # 配置 /etc/fstab
    cat > "$ROOTFS_DIR/etc/fstab" <<EOF
/dev/vda    /           ext4    defaults        1 1
proc        /proc       proc    defaults        0 0
sysfs       /sys        sysfs   defaults        0 0
devpts      /dev/pts    devpts  defaults        0 0
tmpfs       /tmp        tmpfs   defaults        0 0
EOF

    # 配置串口控制台
    cat > "$ROOTFS_DIR/etc/inittab" <<EOF
::sysinit:/etc/init.d/rcS
ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
EOF

    # 创建 rcS 启动脚本
    cat > "$ROOTFS_DIR/etc/init.d/rcS" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

# 挂载 9p 共享目录
mkdir -p /mnt/shared
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/shared

echo ""
echo "=========================================="
echo "  KVM Study Test Environment"
echo "=========================================="
echo ""
echo "  共享目录已挂载到 /mnt/shared"
echo "  测试程序:"
echo "    /mnt/shared/test-cpuid-fault"
echo "    /mnt/shared/test-cpuid-fault-kvm"
echo "    /mnt/shared/kvm-demo"
echo ""
echo "=========================================="
echo ""
EOF
    chmod +x "$ROOTFS_DIR/etc/init.d/rcS"

    # 设置 root 密码为空（方便测试）
    chroot "$ROOTFS_DIR" /bin/bash -c "echo 'root::0:0:root:/root:/bin/bash' > /etc/passwd"

    # 安装必要的工具
    chroot "$ROOTFS_DIR" /bin/bash -c "apt-get update && apt-get install -y --no-install-recommends \
        kmod \
        busybox \
        && apt-get clean"

    log_info "✓ 系统配置完成"
}

# 卸载并清理
cleanup() {
    log_info "清理..."

    cd "$IMAGES_DIR"
    umount "$ROOTFS_DIR" 2>/dev/null || true
    rmdir "$ROOTFS_DIR" 2>/dev/null || true

    log_info "✓ 清理完成"
}

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - Rootfs Builder (ext4)"
    echo "========================================"
    echo ""

    check_dependencies
    create_ext4_image 256
    install_debian
    configure_system
    cleanup

    echo ""
    echo "========================================"
    echo "  ✓ Rootfs 构建完成!"
    echo "========================================"
    echo ""
    echo "  输出文件: $IMAGES_DIR/rootfs.ext4"
    echo ""
    echo "  使用方法:"
    echo "    ./boot-vm-9p.sh"
    echo ""
}

main "$@"
