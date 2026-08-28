#!/bin/bash
# 构建包含 iperf3 的 initramfs
# 用于 vhost 性能测试

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGES_DIR="$SCRIPT_DIR/../images"
ROOTFS_DIR="$IMAGES_DIR/rootfs-iperf"
INITRAMFS="$IMAGES_DIR/initramfs-iperf.img"

log_info() { echo -e "\033[0;32m[INFO]\033[0m $1"; }
log_error() { echo -e "\033[0;31m[ERROR]\033[0m $1"; }

# 检查依赖
check_deps() {
    log_info "检查依赖..."

    if ! command -v busybox &> /dev/null; then
        log_error "busybox 未安装"
        exit 1
    fi

    if ! command -v iperf3 &> /dev/null; then
        log_error "iperf3 未安装"
        exit 1
    fi

    if ! command -v cpio &> /dev/null; then
        log_error "cpio 未安装"
        exit 1
    fi

    log_info "依赖检查通过"
}

# 创建 rootfs 目录结构
create_rootfs() {
    log_info "创建 rootfs 目录结构..."

    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"/{bin,sbin,lib,lib64,usr/bin,usr/lib,proc,sys,dev,etc,tmp,root}

    # 复制 busybox
    cp "$(which busybox)" "$ROOTFS_DIR/bin/"

    # 创建 busybox 符号链接
    local BUSYBOX_APPLETS="sh ls cat echo mount umount ip ifconfig ping nc dd grep awk sed vi ps top free dmesg insmod rmmod lsmod mkdir rm cp mv ln touch chmod chown hostname uname sleep"
    for applet in $BUSYBOX_APPLETS; do
        ln -sf busybox "$ROOTFS_DIR/bin/$applet"
    done

    # 复制 iperf3
    cp "$(which iperf3)" "$ROOTFS_DIR/usr/bin/"

    # 复制 iperf3 依赖的共享库
    log_info "复制 iperf3 依赖库..."
    local LIBS=$(ldd "$(which iperf3)" | grep -oP '/\S+' | sort -u)
    for lib in $LIBS; do
        if [ -f "$lib" ]; then
            # 保持目录结构
            local DEST_DIR="$ROOTFS_DIR$(dirname $lib)"
            mkdir -p "$DEST_DIR"
            cp "$lib" "$DEST_DIR/"
        fi
    done

    # 复制动态链接器
    if [ -f /lib64/ld-linux-x86-64.so.2 ]; then
        mkdir -p "$ROOTFS_DIR/lib64"
        cp /lib64/ld-linux-x86-64.so.2 "$ROOTFS_DIR/lib64/"
    fi

    log_info "rootfs 创建完成"
}

# 创建 init 脚本
create_init() {
    log_info "创建 init 脚本..."

    cat > "$ROOTFS_DIR/init" << 'INIT_EOF'
#!/bin/sh

# 挂载基本文件系统
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

# 显示欢迎信息
echo ""
echo "=========================================="
echo "  vhost 性能测试环境"
echo "=========================================="
echo ""
echo "  Linux $(uname -r)"
echo ""
echo "  可用命令:"
echo "    ip link show          # 查看网络接口"
echo "    ip addr add ...       # 配置 IP"
echo "    iperf3 -s             # 启动 iperf3 server"
echo "    iperf3 -c <ip>        # 运行 iperf3 client"
echo ""
echo "=========================================="
echo ""

# 启动 shell
exec /bin/sh
INIT_EOF

    chmod +x "$ROOTFS_DIR/init"
}

# 打包 initramfs
build_initramfs() {
    log_info "打包 initramfs..."

    cd "$ROOTFS_DIR"
    find . | cpio -o -H newc | gzip > "$INITRAMFS"
    cd "$SCRIPT_DIR"

    local SIZE=$(du -h "$INITRAMFS" | cut -f1)
    log_info "initramfs 创建完成: $INITRAMFS ($SIZE)"
}

# 主逻辑
log_info "开始构建 iperf3 initramfs..."
check_deps
create_rootfs
create_init
build_initramfs

echo ""
echo "=========================================="
echo " 构建完成！"
echo "=========================================="
echo ""
echo "  initramfs: $INITRAMFS"
echo ""
echo "  使用方法:"
echo "    qemu-system-x86_64 -enable-kvm -m 2G -smp 2 \\"
echo "      -kernel <bzImage> \\"
echo "      -initrd $INITRAMFS \\"
echo "      -append 'console=ttyS0 root=/dev/ram0 rdinit=/init' \\"
echo "      -netdev tap,id=net0,ifname=tap0,vhost=on \\"
echo "      -device virtio-net-pci,netdev=net0 \\"
echo "      -nographic"
echo ""
echo "  在 Guest 中:"
echo "    ip link set eth0 up"
echo "    ip addr add 192.168.100.2/24 dev eth0"
echo "    iperf3 -s"
echo ""
