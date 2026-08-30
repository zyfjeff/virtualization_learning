#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# 编译最小 Linux 内核（用于 KVM 学习）
#
# 用法:
#   ./build-kernel.sh [内核源码目录]
#
# 默认使用: /root/code/linux-6.12.93
#
# 输出:
#   ../images/bzImage - 内核镜像
#   ../images/System.map - 符号表（用于调试）
#
# 对应课程: Phase 0-11 所有测试

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
KERNEL_SRC="${1:-/root/code/linux-6.12.93}"
OUTPUT_DIR="$PROJECT_ROOT/images"
CONFIG_FILE="$SCRIPT_DIR/kernel-config"

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

    if ! command -v make &>/dev/null; then
        log_error "make 未安装"
        echo "  Ubuntu/Debian: apt install build-essential"
        echo "  CentOS/RHEL: yum install make"
        missing=1
    fi

    if ! command -v gcc &>/dev/null; then
        log_error "gcc 未安装"
        echo "  Ubuntu/Debian: apt install gcc"
        echo "  CentOS/RHEL: yum install gcc"
        missing=1
    fi

    if ! command -v bc &>/dev/null; then
        log_error "bc 未安装"
        echo "  Ubuntu/Debian: apt install bc"
        echo "  CentOS/RHEL: yum install bc"
        missing=1
    fi

    if ! command -v flex &>/dev/null; then
        log_warn "flex 未安装（可能导致编译失败）"
        echo "  Ubuntu/Debian: apt install flex"
    fi

    if ! command -v bison &>/dev/null; then
        log_warn "bison 未安装（可能导致编译失败）"
        echo "  Ubuntu/Debian: apt install bison"
    fi

    if [ $missing -eq 1 ]; then
        exit 1
    fi

    log_info "✓ 依赖检查通过"
}

# 检查内核源码
check_kernel_source() {
    log_info "检查内核源码..."

    if [ ! -d "$KERNEL_SRC" ]; then
        log_error "内核源码目录不存在: $KERNEL_SRC"
        echo "  请指定正确的内核源码目录:"
        echo "  ./build-kernel.sh /path/to/linux-source"
        exit 1
    fi

    if [ ! -f "$KERNEL_SRC/Makefile" ]; then
        log_error "不是有效的内核源码目录: $KERNEL_SRC"
        exit 1
    fi

    local kernel_version=$(grep -E "^VERSION = " "$KERNEL_SRC/Makefile" | awk '{print $3}')
    local patchlevel=$(grep -E "^PATCHLEVEL = " "$KERNEL_SRC/Makefile" | awk '{print $3}')
    local sublevel=$(grep -E "^SUBLEVEL = " "$KERNEL_SRC/Makefile" | awk '{print $3}')

    log_info "✓ 内核源码: Linux $kernel_version.$patchlevel.$sublevel"
}

# 配置内核
configure_kernel() {
    log_info "配置内核..."

    cd "$KERNEL_SRC"

    # 检查配置文件
    if [ ! -f "$CONFIG_FILE" ]; then
        log_error "配置文件不存在: $CONFIG_FILE"
        exit 1
    fi

    # 复制配置文件
    log_step "复制配置文件..."
    cp "$CONFIG_FILE" .config

    # 运行 olddefconfig（使用默认值填充新选项）
    log_step "运行 make olddefconfig..."
    make olddefconfig >/dev/null 2>&1

    log_info "✓ 内核配置完成"
}

# 编译内核
compile_kernel() {
    log_info "编译内核..."

    cd "$KERNEL_SRC"

    local nproc=$(nproc)
    log_step "使用 $nproc 个线程编译..."
    log_warn "编译可能需要 5-15 分钟，请耐心等待..."

    # 编译 bzImage
    if ! make -j"$nproc" bzImage 2>&1 | tail -20; then
        log_error "内核编译失败"
        exit 1
    fi

    log_info "✓ 内核编译完成"
}

# 安装内核镜像
install_kernel() {
    log_info "安装内核镜像..."

    mkdir -p "$OUTPUT_DIR"

    cd "$KERNEL_SRC"

    # 复制内核镜像
    if [ -f arch/x86/boot/bzImage ]; then
        cp arch/x86/boot/bzImage "$OUTPUT_DIR/"
        log_info "✓ bzImage → $OUTPUT_DIR/bzImage"
    else
        log_error "bzImage 不存在"
        exit 1
    fi

    # 复制 System.map（用于调试）
    if [ -f System.map ]; then
        cp System.map "$OUTPUT_DIR/"
        log_info "✓ System.map → $OUTPUT_DIR/System.map"
    fi

    # 复制 .config（用于参考）
    if [ -f .config ]; then
        cp .config "$OUTPUT_DIR/kernel.config"
        log_info "✓ .config → $OUTPUT_DIR/kernel.config"
    fi

    # 显示内核大小
    local kernel_size=$(du -h "$OUTPUT_DIR/bzImage" | cut -f1)
    log_info "✓ 内核大小: $kernel_size"
}

# 显示编译信息
show_info() {
    echo ""
    echo "========================================"
    echo "  ✓ 内核编译完成!"
    echo "========================================"
    echo ""
    echo "  输出文件:"
    echo "    内核镜像: $OUTPUT_DIR/bzImage"
    echo "    符号表:   $OUTPUT_DIR/System.map"
    echo "    配置文件: $OUTPUT_DIR/kernel.config"
    echo ""
    echo "  下一步:"
    echo "    1. 构建 rootfs:"
    echo "       sudo ./build-rootfs-ubuntu.sh     # 推荐"
    echo "       sudo ./build-rootfs-minimal.sh    # 或最小化（busybox，秒级）"
    echo ""
    echo "    2. 启动虚拟机:"
    echo "       ./boot-vm.sh ubuntu"
    echo ""
    echo "  或手动启动:"
    echo "    qemu-system-x86_64 \\"
    echo "      -enable-kvm \\"
    echo "      -kernel $OUTPUT_DIR/bzImage \\"
    echo "      -initrd $OUTPUT_DIR/initramfs.img \\"
    echo "      -append \"console=ttyS0\" \\"
    echo "      -nographic \\"
    echo "      -m 512"
    echo ""
}

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - Kernel Builder"
    echo "========================================"
    echo ""

    check_dependencies
    check_kernel_source
    configure_kernel
    compile_kernel
    install_kernel
    show_info
}

main "$@"
