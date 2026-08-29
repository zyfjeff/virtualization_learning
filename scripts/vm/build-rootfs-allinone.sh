#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# 构建 All-in-One rootfs (包含所有测试工具)
#
# 用法:
#   ./build-rootfs-allinone.sh [输出目录]
#
# 默认输出: ../images/rootfs-allinone.img

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="${1:-$PROJECT_ROOT/images}"
ROOTFS_DIR="$OUTPUT_DIR/rootfs-allinone"

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

    if ! command -v busybox &>/dev/null; then
        log_error "busybox 未安装"
        echo "  Ubuntu/Debian: apt install busybox-static"
        missing=1
    fi

    if ! command -v cpio &>/dev/null; then
        log_error "cpio 未安装"
        echo "  Ubuntu/Debian: apt install cpio"
        missing=1
    fi

    if [ $missing -eq 1 ]; then
        exit 1
    fi

    log_info "✓ 依赖检查通过"
}

# 创建目录结构
create_structure() {
    log_info "创建目录结构..."

    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"/{bin,sbin,usr/bin,usr/sbin}
    mkdir -p "$ROOTFS_DIR"/{proc,sys,dev,tmp,run,var/log}
    mkdir -p "$ROOTFS_DIR"/{etc,root,mnt/shared}
    mkdir -p "$ROOTFS_DIR"/lib/x86_64-linux-gnu
    mkdir -p "$ROOTFS_DIR"/lib64

    log_info "✓ 目录结构创建完成"
}

# 复制基本工具
copy_basic_tools() {
    log_info "复制基本工具..."

    # 复制 busybox
    cp "$(which busybox)" "$ROOTFS_DIR/bin/"
    
    # 创建 busybox 链接
    cd "$ROOTFS_DIR/bin"
    for cmd in sh ash mount umount mkdir echo cat ls pwd cd rm cp mv \
               ln chmod chown grep sed awk head tail wc sort uniq \
               ps kill sleep date hostname uname id whoami; do
        ln -sf busybox $cmd 2>/dev/null || true
    done
    cd - >/dev/null

    log_info "✓ 基本工具复制完成"
}

# 复制网络工具
copy_network_tools() {
    log_info "复制网络工具..."

    # 尝试复制静态编译的工具
    local tools="iperf3 ethtool"
    
    for tool in $tools; do
        if command -v $tool &>/dev/null; then
            # 检查是否是静态编译
            if ldd "$(which $tool)" 2>&1 | grep -q "not a dynamic executable"; then
                cp "$(which $tool)" "$ROOTFS_DIR/usr/bin/"
                log_info "  ✓ $tool (静态)"
            else
                # 动态编译，需要复制库
                cp "$(which $tool)" "$ROOTFS_DIR/usr/bin/"
                log_info "  ✓ $tool (动态，需要库)"
                
                # 复制依赖库
                ldd "$(which $tool)" | grep "=> /" | awk '{print $3}' | \
                while read lib; do
                    if [ -f "$lib" ]; then
                        cp "$lib" "$ROOTFS_DIR/lib/x86_64-linux-gnu/" 2>/dev/null || true
                    fi
                done
            fi
        else
            log_warn "  ✗ $tool 未安装"
        fi
    done

    # 复制 ip 命令（来自 iproute2）
    if command -v ip &>/dev/null; then
        cp "$(which ip)" "$ROOTFS_DIR/sbin/"
        log_info "  ✓ ip"
        
        # 复制依赖库
        ldd "$(which ip)" | grep "=> /" | awk '{print $3}' | \
        while read lib; do
            if [ -f "$lib" ]; then
                cp "$lib" "$ROOTFS_DIR/lib/x86_64-linux-gnu/" 2>/dev/null || true
            fi
        done
    fi

    # 复制 ping（来自 iputils）
    if command -v ping &>/dev/null; then
        cp "$(which ping)" "$ROOTFS_DIR/bin/"
        log_info "  ✓ ping"
        
        # 复制依赖库
        ldd "$(which ping)" | grep "=> /" | awk '{print $3}' | \
        while read lib; do
            if [ -f "$lib" ]; then
                cp "$lib" "$ROOTFS_DIR/lib/x86_64-linux-gnu/" 2>/dev/null || true
            fi
        done
    fi

    log_info "✓ 网络工具复制完成"
}

# 复制系统工具
copy_system_tools() {
    log_info "复制系统工具..."

    local tools="lspci numactl stress-ng"
    
    for tool in $tools; do
        if command -v $tool &>/dev/null; then
            cp "$(which $tool)" "$ROOTFS_DIR/usr/bin/" 2>/dev/null || \
            cp "$(which $tool)" "$ROOTFS_DIR/usr/sbin/" 2>/dev/null || true
            log_info "  ✓ $tool"
            
            # 复制依赖库
            ldd "$(which $tool)" 2>/dev/null | grep "=> /" | awk '{print $3}' | \
            while read lib; do
                if [ -f "$lib" ]; then
                    cp "$lib" "$ROOTFS_DIR/lib/x86_64-linux-gnu/" 2>/dev/null || true
                fi
            done
        else
            log_warn "  ✗ $tool 未安装"
        fi
    done

    log_info "✓ 系统工具复制完成"
}

# 复制动态链接器
copy_ld() {
    log_info "复制动态链接器..."

    if [ -f /lib64/ld-linux-x86-64.so.2 ]; then
        cp /lib64/ld-linux-x86-64.so.2 "$ROOTFS_DIR/lib64/"
        log_info "✓ 动态链接器复制完成"
    else
        log_warn "动态链接器不存在"
    fi
}

# 创建配置文件
create_config() {
    log_info "创建配置文件..."

    # 创建 /etc/passwd
    cat > "$ROOTFS_DIR/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF

    # 创建 /etc/group
    cat > "$ROOTFS_DIR/etc/group" <<'EOF'
root:x:0:
EOF

    # 创建 /etc/hosts
    cat > "$ROOTFS_DIR/etc/hosts" <<'EOF'
127.0.0.1 localhost
::1 localhost
EOF

    # 创建 /etc/resolv.conf
    cat > "$ROOTFS_DIR/etc/resolv.conf" <<'EOF'
nameserver 8.8.8.8
nameserver 8.8.4.4
EOF

    log_info "✓ 配置文件创建完成"
}

# 创建 /init
create_init() {
    log_info "创建 /init..."

    cat > "$ROOTFS_DIR/init" <<'EOF'
#!/bin/sh

# 挂载基本文件系统
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

# 挂载 9p 共享目录（如果可用）
mkdir -p /mnt/shared
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/shared 2>/dev/null || true

# 配置网络（如果有 eth0）
if ip link show eth0 &>/dev/null; then
    ip link set eth0 up
    # 尝试 DHCP
    if command -v udhcpc &>/dev/null; then
        udhcpc -i eth0 -b -p /var/run/udhcpc.pid 2>/dev/null &
    fi
fi

# 显示欢迎信息
echo ""
echo "=========================================="
echo "  KVM Study - All-in-One Test Environment"
echo "=========================================="
echo ""
echo "  Linux $(uname -r)"
echo ""
echo "  可用工具:"
echo "    网络: iperf3, ethtool, ip, ping"
echo "    系统: lspci, numactl, stress-ng"
echo "    基本: sh, ls, cat, mount, etc."
echo ""

if [ -d /mnt/shared ] && [ "$(ls -A /mnt/shared 2>/dev/null)" ]; then
    echo "  共享目录: /mnt/shared"
    echo ""
fi

echo "=========================================="
echo ""

# 启动 shell
exec /bin/sh
EOF

    chmod +x "$ROOTFS_DIR/init"
    log_info "✓ /init 创建完成"
}

# 设置库路径
setup_ldconfig() {
    log_info "设置库路径..."

    # 创建 ldconfig 配置
    cat > "$ROOTFS_DIR/etc/ld.so.conf" <<'EOF'
/lib/x86_64-linux-gnu
/lib64
EOF

    # 如果有 ldconfig，运行它
    if command -v ldconfig &>/dev/null; then
        chroot "$ROOTFS_DIR" /sbin/ldconfig 2>/dev/null || true
    fi

    log_info "✓ 库路径设置完成"
}

# 打包为 initramfs
create_initramfs() {
    log_info "打包为 initramfs..."

    local img="$OUTPUT_DIR/initramfs-allinone.img"

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

# 创建磁盘镜像（可选）
create_disk_image() {
    log_info "创建磁盘镜像..."

    local disk_img="$OUTPUT_DIR/disk-allinone.img"
    
    # 创建 10G 磁盘镜像
    qemu-img create -f qcow2 "$disk_img" 10G 2>/dev/null
    
    local size=$(du -h "$disk_img" | cut -f1)
    log_info "✓ 磁盘镜像创建完成 ($size)"
}

# 显示使用说明
show_usage() {
    echo ""
    echo "=========================================="
    echo "  使用说明"
    echo "=========================================="
    echo ""
    echo "1. 启动 VM:"
    echo "   qemu-system-x86_64 -m 2G -smp 2 \\"
    echo "     -kernel /root/code/linux-6.12.93/arch/x86_64/boot/bzImage \\"
    echo "     -initrd $OUTPUT_DIR/initramfs-allinone.img \\"
    echo "     -drive file=$OUTPUT_DIR/disk-allinone.img,format=qcow2,if=virtio \\"
    echo "     -netdev tap,id=net0,vhost=on \\"
    echo "     -device virtio-net-pci,netdev=net0 \\"
    echo "     -nographic"
    echo ""
    echo "2. 在 VM 内测试:"
    echo "   # 网络测试"
    echo "   iperf3 -s                    # 启动 server"
    echo "   iperf3 -c <server_ip> -t 30  # 运行测试"
    echo ""
    echo "   # 网络配置"
    echo "   ethtool -S eth0              # 查看统计"
    echo "   ethtool -G eth0 rx 1024 tx 1024  # 调整队列"
    echo ""
    echo "   # 性能测试"
    echo "   stress-ng --cpu 4 --timeout 60s  # CPU 压力"
    echo ""
    echo "=========================================="
    echo ""
}

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - All-in-One Rootfs Builder"
    echo "========================================"
    echo ""

    check_dependencies
    create_structure
    copy_basic_tools
    copy_network_tools
    copy_system_tools
    copy_ld
    create_config
    create_init
    setup_ldconfig
    create_initramfs
    create_disk_image

    echo ""
    echo "========================================"
    echo "  ✓ All-in-One Rootfs 构建完成!"
    echo "========================================"
    echo ""
    echo "  输出文件:"
    echo "    - $OUTPUT_DIR/initramfs-allinone.img"
    echo "    - $OUTPUT_DIR/disk-allinone.img"
    echo ""
    
    show_usage
}

main "$@"
