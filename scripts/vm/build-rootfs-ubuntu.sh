#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# 构建基于 Ubuntu 的 rootfs (包含所有测试工具)
#
# 用法:
#   ./build-rootfs-ubuntu.sh [输出目录] [Ubuntu 版本]
#
# 默认输出: ../images/rootfs-ubuntu.img
# 默认版本: jammy (22.04 LTS)
#
# 依赖:
#   - debootstrap
#   - qemu-system-x86_64 (用于测试)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="${1:-$PROJECT_ROOT/images}"
UBUNTU_VERSION="${2:-jammy}"
ROOTFS_DIR="$OUTPUT_DIR/rootfs-ubuntu"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }

# 检查依赖
check_dependencies() {
    log_info "检查依赖..."

    local missing=0

    if ! command -v debootstrap &>/dev/null; then
        log_error "debootstrap 未安装"
        echo "  Ubuntu/Debian: sudo apt install debootstrap"
        missing=1
    fi

    if ! command -v qemu-system-x86_64 &>/dev/null; then
        log_error "qemu-system-x86_64 未安装"
        echo "  Ubuntu/Debian: sudo apt install qemu-system-x86"
        missing=1
    fi

    if [ $missing -eq 1 ]; then
        exit 1
    fi

    log_info "✓ 依赖检查通过"
}

# 使用 debootstrap 构建基础系统
build_base_system() {
    log_info "使用 debootstrap 构建 Ubuntu $UBUNTU_VERSION 基础系统..."

    rm -rf "$ROOTFS_DIR"
    
    # 使用 debootstrap 构建最小系统
    debootstrap --arch=amd64 --variant=minbase \
        "$UBUNTU_VERSION" "$ROOTFS_DIR" \
        http://archive.ubuntu.com/ubuntu/

    log_info "✓ 基础系统构建完成"
}

# 配置系统
configure_system() {
    log_info "配置系统..."

    # 配置 hostname
    echo "kvm-study" > "$ROOTFS_DIR/etc/hostname"

    # 配置 hosts
    cat > "$ROOTFS_DIR/etc/hosts" <<EOF
127.0.0.1 localhost kvm-study
::1 localhost ip6-localhost ip6-loopback
EOF

    # 配置网络（使用 DHCP）
    mkdir -p "$ROOTFS_DIR/etc/netplan"
    cat > "$ROOTFS_DIR/etc/netplan/01-dhcp.yaml" <<EOF
network:
  version: 2
  renderer: networkd
  ethernets:
    eth0:
      dhcp4: true
      optional: true
EOF

    # 配置 root 密码（空密码，方便测试）
    chroot "$ROOTFS_DIR" /usr/sbin/usermod -p '' root

    # 配置 serial console
    mkdir -p "$ROOTFS_DIR/etc/systemd/system"
    cat > "$ROOTFS_DIR/etc/systemd/system/serial-getty@ttyS0.service" <<EOF
[Unit]
Description=Serial Console getty
After=systemd-user-sessions.service

[Service]
ExecStart=/sbin/agetty -8 115200 ttyS0
Restart=always
RestartSec=0
StandardInput=tty
StandardOutput=tty

[Install]
WantedBy=getty.target
EOF

    # 启用 serial console
    chroot "$ROOTFS_DIR" systemctl enable serial-getty@ttyS0.service

    # 配置默认 target
    chroot "$ROOTFS_DIR" systemctl set-default multi-user.target

    log_info "✓ 系统配置完成"
}

# 安装测试工具
install_test_tools() {
    log_info "安装测试工具..."

    # 挂载必要的文件系统
    mount --bind /dev "$ROOTFS_DIR/dev" || true
    mount --bind /proc "$ROOTFS_DIR/proc" || true
    mount --bind /sys "$ROOTFS_DIR/sys" || true

    # 安装测试工具
    chroot "$ROOTFS_DIR" apt-get update
    chroot "$ROOTFS_DIR" apt-get install -y --no-install-recommends \
        iperf3 \
        ethtool \
        iproute2 \
        iputils-ping \
        pciutils \
        numactl \
        stress-ng \
        hping3 \
        tcpdump \
        net-tools \
        procps \
        sysstat \
        strace \
        linux-tools-generic \
        bpftrace

    # 清理
    chroot "$ROOTFS_DIR" apt-get clean
    chroot "$ROOTFS_DIR" rm -rf /var/lib/apt/lists/*

    # 卸载文件系统
    umount "$ROOTFS_DIR/dev" || true
    umount "$ROOTFS_DIR/proc" || true
    umount "$ROOTFS_DIR/sys" || true

    log_info "✓ 测试工具安装完成"
}

# 创建自定义启动脚本
create_custom_scripts() {
    log_info "创建自定义脚本..."

    # 创建测试脚本
    cat > "$ROOTFS_DIR/usr/local/bin/run-network-test" <<'EOF'
#!/bin/bash
# 网络性能测试脚本

echo "=========================================="
echo "  网络性能测试"
echo "=========================================="
echo ""

# 检查网络接口
echo "网络接口:"
ip link show
echo ""

# 测试 ethtool
echo "网卡信息:"
ethtool eth0 2>/dev/null || echo "ethtool 不可用"
echo ""

# 测试 iperf3 (client mode)
if [ -n "$1" ]; then
    echo "运行 iperf3 测试到 $1..."
    iperf3 -c "$1" -t 30 -P 4
else
    echo "启动 iperf3 server..."
    echo "运行: iperf3 -c <server_ip> -t 30"
    iperf3 -s
fi
EOF

    chmod +x "$ROOTFS_DIR/usr/local/bin/run-network-test"

    # 创建压力测试脚本
    cat > "$ROOTFS_DIR/usr/local/bin/run-stress-test" <<'EOF'
#!/bin/bash
# 压力测试脚本

echo "=========================================="
echo "  压力测试"
echo "=========================================="
echo ""

# CPU 压力
echo "CPU 压力测试 (60s)..."
stress-ng --cpu 4 --timeout 60s
echo ""

# 内存压力
echo "内存压力测试 (60s)..."
stress-ng --vm 2 --vm-bytes 1G --timeout 60s
echo ""

# I/O 压力
echo "I/O 压力测试 (60s)..."
stress-ng --hdd 2 --timeout 60s
echo ""

echo "测试完成"
EOF

    chmod +x "$ROOTFS_DIR/usr/local/bin/run-stress-test"

    # 创建 virtio 调优脚本
    cat > "$ROOTFS_DIR/usr/local/bin/tune-virtio" <<'EOF'
#!/bin/bash
# Virtio 调优脚本

echo "=========================================="
echo "  Virtio 调优"
echo "=========================================="
echo ""

# 查看当前队列大小
echo "当前队列配置:"
ethtool -g eth0 2>/dev/null || echo "ethtool 不可用"
echo ""

# 调整队列大小
echo "调整队列大小到 1024..."
ethtool -G eth0 rx 1024 tx 1024 2>/dev/null || echo "调整失败"
echo ""

# 启用中断合并
echo "配置中断合并..."
ethtool -C eth0 rx-usecs 50 rx-frames 64 2>/dev/null || echo "配置失败"
echo ""

# 查看统计
echo "网卡统计:"
ethtool -S eth0 2>/dev/null | head -20
echo ""

echo "调优完成"
EOF

    chmod +x "$ROOTFS_DIR/usr/local/bin/tune-virtio"

    log_info "✓ 自定义脚本创建完成"
}

# 创建启动信息
create_motd() {
    log_info "创建启动信息..."

    cat > "$ROOTFS_DIR/etc/motd" <<'EOF'

==========================================
  KVM Study - Ubuntu Test Environment
==========================================

  可用工具:
    网络: iperf3, ethtool, ip, ping, hping3, tcpdump
    系统: lspci, numactl, stress-ng, strace
    性能: perf, bpftrace, sysstat
    
  快捷命令:
    run-network-test [server_ip]  # 网络性能测试
    run-stress-test               # 压力测试
    tune-virtio                   # Virtio 调优

==========================================

EOF

    log_info "✓ 启动信息创建完成"
}

# 打包为磁盘镜像
create_disk_image() {
    log_info "打包为磁盘镜像..."

    local disk_img="$OUTPUT_DIR/disk-ubuntu.img"
    
    # 创建 10G 磁盘镜像
    qemu-img create -f qcow2 "$disk_img" 10G
    
    # 创建临时目录
    local mnt_dir=$(mktemp -d)
    
    # 挂载镜像
    modprobe nbd max_part=8 || true
    qemu-nbd --connect=/dev/nbd0 "$disk_img"
    mkfs.ext4 /dev/nbd0
    mount /dev/nbd0 "$mnt_dir"
    
    # 复制 rootfs
    cp -a "$ROOTFS_DIR"/* "$mnt_dir"/
    
    # 安装 GRUB
    cat > "$mnt_dir/boot/grub/grub.cfg" <<EOF
set timeout=0
set default=0

menuentry "KVM Study Ubuntu" {
    linux /boot/vmlinuz root=/dev/vda ro console=ttyS0
    initrd /boot/initrd.img
}
EOF

    # 复制内核和 initrd
    cp /boot/vmlinuz-$(uname -r) "$mnt_dir/boot/vmlinuz" 2>/dev/null || \
        cp /boot/vmlinuz "$mnt_dir/boot/vmlinuz" 2>/dev/null || true
    cp /boot/initrd.img-$(uname -r) "$mnt_dir/boot/initrd.img" 2>/dev/null || \
        cp /boot/initrd.img "$mnt_dir/boot/initrd.img" 2>/dev/null || true

    # 卸载
    umount "$mnt_dir"
    qemu-nbd --disconnect /dev/nbd0
    rmdir "$mnt_dir"
    
    local size=$(du -h "$disk_img" | cut -f1)
    log_info "✓ 磁盘镜像创建完成 ($size)"
}

# 打包为 initramfs（更简单的方式）
create_initramfs() {
    log_info "打包为 initramfs..."

    local img="$OUTPUT_DIR/initramfs-ubuntu.img"

    cd "$ROOTFS_DIR"
    # set -e 不覆盖管道中间命令，缺 pipefail 时 cpio/gzip 失败会写出截断镜像而构建仍报成功
    local rc=0
    ( set -o pipefail; find . | cpio -o -H newc 2>/dev/null | gzip > "$img" ) || rc=$?
    cd - >/dev/null

    if [ "$rc" -ne 0 ] || ! gzip -t "$img" 2>/dev/null; then
        rm -f "$img"
        log_error "initramfs 打包失败（rc=$rc）或 gzip -t 校验未通过，已删除损坏产物"
        exit 1
    fi

    local size=$(du -h "$img" | cut -f1)
    log_info "✓ initramfs 创建完成 ($size)"
}

# 显示使用说明
show_usage() {
    echo ""
    echo "=========================================="
    echo "  使用说明"
    echo "=========================================="
    echo ""
    echo "1. 启动 VM (使用 initramfs):"
    echo "   qemu-system-x86_64 -m 2G -smp 2 \\"
    echo "     -kernel /root/code/linux-6.12.93/arch/x86_64/boot/bzImage \\"
    echo "     -initrd $OUTPUT_DIR/initramfs-ubuntu.img \\"
    echo "     -drive file=$OUTPUT_DIR/disk-ubuntu.img,format=qcow2,if=virtio \\"
    echo "     -netdev tap,id=net0,vhost=on \\"
    echo "     -device virtio-net-pci,netdev=net0 \\"
    echo "     -nographic"
    echo ""
    echo "2. 在 VM 内测试:"
    echo "   # 网络测试"
    echo "   run-network-test              # 启动 server"
    echo "   run-network-test <server_ip>  # 运行 client"
    echo ""
    echo "   # 压力测试"
    echo "   run-stress-test"
    echo ""
    echo "   # Virtio 调优"
    echo "   tune-virtio"
    echo ""
    echo "3. 手动测试:"
    echo "   # 网络性能"
    echo "   iperf3 -s                     # server"
    echo "   iperf3 -c <ip> -t 30 -P 4    # client"
    echo ""
    echo "   # 网络配置"
    echo "   ethtool -S eth0               # 统计"
    echo "   ethtool -G eth0 rx 1024 tx 1024  # 队列"
    echo "   ethtool -C eth0 rx-usecs 50   # 中断合并"
    echo ""
    echo "   # 性能分析"
    echo "   perf record -g -a sleep 30    # perf 采样"
    echo "   bpftrace -e 'kprobe:vhost_net_buf_add { @count++; }'"
    echo ""
    echo "=========================================="
    echo ""
}

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - Ubuntu Rootfs Builder"
    echo "========================================"
    echo ""

    check_dependencies
    build_base_system
    configure_system
    install_test_tools
    create_custom_scripts
    create_motd
    create_initramfs
    create_disk_image

    echo ""
    echo "========================================"
    echo "  ✓ Ubuntu Rootfs 构建完成!"
    echo "========================================"
    echo ""
    echo "  输出文件:"
    echo "    - $OUTPUT_DIR/initramfs-ubuntu.img"
    echo "    - $OUTPUT_DIR/disk-ubuntu.img"
    echo ""
    
    show_usage
}

main "$@"
