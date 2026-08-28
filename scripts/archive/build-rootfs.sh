#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# 构建最小 rootfs (基于 Busybox)
#
# 用法:
#   ./build-rootfs.sh [输出目录]
#
# 默认输出: ../images/rootfs.img
#
# 依赖:
#   - busybox (静态编译版本)
#   - qemu-system-x86_64 (用于测试)
#
# 对应课程: 所有 Phase 的测试环境

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="${1:-$PROJECT_ROOT/images}"
ROOTFS_DIR="$OUTPUT_DIR/rootfs"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# 检查依赖
check_dependencies() {
    log_info "检查依赖..."

    local missing=0

    if ! command -v busybox &>/dev/null; then
        log_error "busybox 未安装"
        echo "  Ubuntu/Debian: apt install busybox-static"
        echo "  CentOS/RHEL: yum install busybox"
        missing=1
    fi

    if ! command -v cpio &>/dev/null; then
        log_error "cpio 未安装"
        echo "  Ubuntu/Debian: apt install cpio"
        echo "  CentOS/RHEL: yum install cpio"
        missing=1
    fi

    if [ $missing -eq 1 ]; then
        exit 1
    fi

    log_info "✓ 依赖检查通过"
}

# 创建基本目录结构
create_directory_structure() {
    log_info "创建目录结构..."

    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"/{bin,sbin,etc,proc,sys,dev,tmp,root,usr/{bin,sbin,lib}}
    mkdir -p "$ROOTFS_DIR"/etc/init.d
    mkdir -p "$ROOTFS_DIR"/var/{log,run}

    log_info "✓ 目录结构创建完成"
}

# 安装 Busybox
install_busybox() {
    log_info "安装 Busybox..."

    local busybox_path

    # 查找静态编译的 busybox
    if [ -f /bin/busybox ]; then
        busybox_path=/bin/busybox
    elif command -v busybox &>/dev/null; then
        busybox_path=$(which busybox)
    else
        log_error "找不到 busybox"
        exit 1
    fi

    # 检查是否是静态编译
    if file "$busybox_path" | grep -q "statically linked"; then
        log_info "使用静态编译的 busybox: $busybox_path"
    else
        log_warn "busybox 不是静态编译，可能导致问题"
        log_warn "建议安装: apt install busybox-static"
    fi

    # 复制 busybox
    cp "$busybox_path" "$ROOTFS_DIR/bin/busybox"
    chmod +x "$ROOTFS_DIR/bin/busybox"

    # 创建常用命令的符号链接
    local commands="sh ash bash ls cat echo mount umount mkdir rmdir rm cp mv \
                    ln chmod chown grep find sed awk head tail wc sort uniq \
                    ps kill sleep clear reset date hostname id whoami pwd \
                    dmesg insmod rmmod lsmod modprobe ifconfig ip ping \
                    vi more less test expr true false"

    for cmd in $commands; do
        ln -sf busybox "$ROOTFS_DIR/bin/$cmd" 2>/dev/null || true
    done

    log_info "✓ Busybox 安装完成 ($(ls "$ROOTFS_DIR/bin/" | wc -l) 个命令)"
}

# 创建 /init 脚本
create_init_script() {
    log_info "创建 /init 脚本..."

    cat > "$ROOTFS_DIR/init" <<'EOF'
#!/bin/sh

# 挂载基本文件系统
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

# 创建必要的设备节点
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts

# 设置主机名
hostname kvm-test

# 加载 KVM 模块（如果可用）
modprobe kvm 2>/dev/null || true
modprobe kvm_intel 2>/dev/null || true
modprobe kvm_amd 2>/dev/null || true

# 显示欢迎信息
echo ""
echo "=========================================="
echo "  KVM Study Test Environment"
echo "=========================================="
echo ""
echo "  Linux $(uname -r)"
echo "  CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2)"
echo "  VMX: $(grep -c vmx /proc/cpuinfo) CPUs with VMX support"
echo ""
echo "  可用命令:"
echo "    test-cpuid-fault     - CPUID Faulting 测试"
echo "    test-cpuid-fault-kvm - KVM 中的 CPUID Faulting 测试"
echo "    kvm-demo             - KVM API 演示"
echo "    vmx-info             - VMX 能力检测"
echo ""
echo "=========================================="
echo ""

# 启动 shell
exec /bin/sh
EOF

    chmod +x "$ROOTFS_DIR/init"
    log_info "✓ /init 脚本创建完成"
}

# 创建系统配置文件
create_system_config() {
    log_info "创建系统配置文件..."

    # /etc/inittab
    cat > "$ROOTFS_DIR/etc/inittab" <<'EOF'
::sysinit:/etc/init.d/rcS
::respawn:/sbin/getty -L ttyS0 115200 vt100
::respawn:/sbin/getty -L tty1 0 vt100
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
EOF

    # /etc/init.d/rcS
    cat > "$ROOTFS_DIR/etc/init.d/rcS" <<'EOF'
#!/bin/sh

# 挂载基本文件系统
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

# 创建必要的设备节点
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts

# 设置主机名
hostname kvm-test

# 加载 KVM 模块（如果可用）
modprobe kvm 2>/dev/null || true
modprobe kvm_intel 2>/dev/null || true
modprobe kvm_amd 2>/dev/null || true

# 显示欢迎信息
echo ""
echo "=========================================="
echo "  KVM Study Test Environment"
echo "=========================================="
echo ""
echo "  Linux $(uname -r)"
echo "  CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2)"
echo "  VMX: $(grep -c vmx /proc/cpuinfo) CPUs with VMX support"
echo ""
echo "  可用命令:"
echo "    test-cpuid-fault     - CPUID Faulting 测试"
echo "    test-cpuid-fault-kvm - KVM 中的 CPUID Faulting 测试"
echo "    kvm-demo             - KVM API 演示"
echo "    vmx-info             - VMX 能力检测"
echo ""
echo "=========================================="
echo ""
EOF
    chmod +x "$ROOTFS_DIR/etc/init.d/rcS"

    # /etc/passwd
    cat > "$ROOTFS_DIR/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF

    # /etc/group
    cat > "$ROOTFS_DIR/etc/group" <<'EOF'
root:x:0:
EOF

    # /etc/hosts
    cat > "$ROOTFS_DIR/etc/hosts" <<'EOF'
127.0.0.1   localhost kvm-test
EOF

    # /etc/fstab
    cat > "$ROOTFS_DIR/etc/fstab" <<'EOF'
proc    /proc   proc    defaults    0 0
sysfs   /sys    sysfs   defaults    0 0
devtmpfs /dev   devtmpfs defaults   0 0
EOF

    log_info "✓ 系统配置文件创建完成"
}

# 复制测试程序
copy_test_programs() {
    log_info "复制测试程序..."

    # CPUID Faulting 测试
    if [ -f "$PROJECT_ROOT/examples/cpuid-faulting-demo/test-cpuid-fault" ]; then
        cp "$PROJECT_ROOT/examples/cpuid-faulting-demo/test-cpuid-fault" \
           "$ROOTFS_DIR/usr/bin/"
        log_info "  ✓ test-cpuid-fault"
    fi

    if [ -f "$PROJECT_ROOT/examples/cpuid-faulting-demo/test-cpuid-fault-kvm" ]; then
        cp "$PROJECT_ROOT/examples/cpuid-faulting-demo/test-cpuid-fault-kvm" \
           "$ROOTFS_DIR/usr/bin/"
        log_info "  ✓ test-cpuid-fault-kvm"
    fi

    # KVM API 演示
    if [ -f "$PROJECT_ROOT/examples/kvm-api-demo/kvm-demo" ]; then
        cp "$PROJECT_ROOT/examples/kvm-api-demo/kvm-demo" \
           "$ROOTFS_DIR/usr/bin/"
        log_info "  ✓ kvm-demo"
    fi

    # Mini KVM
    if [ -f "$PROJECT_ROOT/examples/mini-kvm/mini-kvm.ko" ]; then
        cp "$PROJECT_ROOT/examples/mini-kvm/mini-kvm.ko" \
           "$ROOTFS_DIR/root/"
        log_info "  ✓ mini-kvm.ko"
    fi

    log_info "✓ 测试程序复制完成"
}

# 创建 initramfs 镜像
create_initramfs() {
    log_info "创建 initramfs 镜像..."

    local initramfs="$OUTPUT_DIR/initramfs.img"

    cd "$ROOTFS_DIR"

    # 打包为 cpio 格式
    find . | cpio -o -H newc 2>/dev/null | gzip > "$initramfs"

    cd - >/dev/null

    local size=$(du -h "$initramfs" | cut -f1)
    log_info "✓ initramfs 创建完成: $initramfs ($size)"
}

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - Rootfs Builder"
    echo "========================================"
    echo ""

    check_dependencies
    create_directory_structure
    install_busybox
    create_init_script
    create_system_config
    copy_test_programs
    create_initramfs

    echo ""
    echo "========================================"
    echo "  ✓ Rootfs 构建完成!"
    echo "========================================"
    echo ""
    echo "  输出文件: $OUTPUT_DIR/initramfs.img"
    echo ""
    echo "  使用方法:"
    echo "    ./boot-vm.sh"
    echo ""
    echo "  或手动启动:"
    echo "    qemu-system-x86_64 \\"
    echo "      -enable-kvm \\"
    echo "      -kernel images/bzImage \\"
    echo "      -initrd images/initramfs.img \\"
    echo "      -append \"console=ttyS0\" \\"
    echo "      -nographic \\"
    echo "      -m 512"
    echo ""
}

main "$@"
