#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# 一键设置 KVM 测试环境
#
# 用法:
#   ./setup.sh
#
# 功能:
#   1. 检查并安装依赖
#   2. 编译最小 Linux 内核
#   3. 构建 rootfs
#   4. 复制测试程序
#   5. 显示使用指南
#
# 对应课程: Phase 0-10 测试环境

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

# 显示 Banner
show_banner() {
    echo ""
    echo "=========================================="
    echo "  KVM Study - Test Environment Setup"
    echo "=========================================="
    echo ""
    echo "  本脚本将自动："
    echo "    1. 检查并安装依赖"
    echo "    2. 编译最小 Linux 内核"
    echo "    3. 构建最小 rootfs"
    echo "    4. 准备测试程序"
    echo ""
    echo "  预计耗时: 10-20 分钟"
    echo ""
    echo "=========================================="
    echo ""
}

# 检查并安装依赖
install_dependencies() {
    log_step "检查依赖..."

    local missing=()

    # 检查编译工具
    for cmd in gcc make bc; do
        if ! command -v $cmd &>/dev/null; then
            missing+=($cmd)
        fi
    done

    # 检查 busybox
    if ! command -v busybox &>/dev/null; then
        missing+=("busybox-static")
    fi

    # 检查 QEMU
    if ! command -v qemu-system-x86_64 &>/dev/null; then
        missing+=("qemu-system-x86")
    fi

    if [ ${#missing[@]} -eq 0 ]; then
        log_info "✓ 所有依赖已安装"
        return
    fi

    log_warn "缺少依赖: ${missing[*]}"
    echo ""

    # 检测包管理器
    if command -v apt-get &>/dev/null; then
        log_info "检测到 apt-get，准备安装..."
        echo ""
        echo "  将要安装:"
        for pkg in "${missing[@]}"; do
            echo "    - $pkg"
        done
        echo ""
        read -p "  是否继续? [Y/n] " -n 1 -r
        echo ""

        if [[ $REPLY =~ ^[Nn]$ ]]; then
            log_error "用户取消安装"
            exit 1
        fi

        # 安装依赖
        case "${missing[*]}" in
            *"gcc"*|*"make"*|*"bc"*)
                sudo apt-get install -y build-essential bc flex bison libssl-dev
                ;;
        esac

        case "${missing[*]}" in
            *"busybox-static"*)
                sudo apt-get install -y busybox-static
                ;;
        esac

        case "${missing[*]}" in
            *"qemu-system-x86"*)
                sudo apt-get install -y qemu-system-x86
                ;;
        esac

    elif command -v yum &>/dev/null; then
        log_info "检测到 yum，准备安装..."
        sudo yum install -y "${missing[@]}"

    elif command -v dnf &>/dev/null; then
        log_info "检测到 dnf，准备安装..."
        sudo dnf install -y "${missing[@]}"

    else
        log_error "未找到包管理器，请手动安装以下依赖:"
        for pkg in "${missing[@]}"; do
            echo "  - $pkg"
        done
        exit 1
    fi

    log_info "✓ 依赖安装完成"
}

# 编译内核
build_kernel() {
    log_step "编译 Linux 内核..."

    if [ ! -x "$SCRIPT_DIR/build-kernel.sh" ]; then
        chmod +x "$SCRIPT_DIR/build-kernel.sh"
    fi

    "$SCRIPT_DIR/build-kernel.sh"
}

# 构建 rootfs
build_rootfs() {
    log_step "构建 rootfs..."

    if [ ! -x "$SCRIPT_DIR/build-rootfs.sh" ]; then
        chmod +x "$SCRIPT_DIR/build-rootfs.sh"
    fi

    "$SCRIPT_DIR/build-rootfs.sh"
}

# 设置脚本权限
setup_permissions() {
    log_step "设置脚本权限..."

    chmod +x "$SCRIPT_DIR"/*.sh

    log_info "✓ 权限设置完成"
}

# 显示完成信息
show_completion() {
    echo ""
    echo "=========================================="
    echo "  ✓ 测试环境构建完成!"
    echo "=========================================="
    echo ""
    echo "  生成的文件:"
    echo "    内核:     $(dirname "$SCRIPT_DIR")/images/bzImage"
    echo "    initramfs: $(dirname "$SCRIPT_DIR")/images/initramfs.img"
    echo ""
    echo "  快速开始:"
    echo "    cd $(dirname "$SCRIPT_DIR")/images"
    echo "    ../scripts/testing/boot-vm.sh"
    echo ""
    echo "  或手动启动:"
    echo "    qemu-system-x86_64 \\"
    echo "      -enable-kvm \\"
    echo "      -kernel $(dirname "$SCRIPT_DIR")/images/bzImage \\"
    echo "      -initrd $(dirname "$SCRIPT_DIR")/images/initramfs.img \\"
    echo "      -append \"console=ttyS0\" \\"
    echo "      -nographic \\"
    echo "      -m 512"
    echo ""
    echo "  虚拟机内测试:"
    echo "    test-cpuid-fault        # CPUID Faulting 测试"
    echo "    test-cpuid-fault-kvm    # KVM 中的 CPUID Faulting"
    echo "    kvm-demo                # KVM API 演示"
    echo ""
    echo "  更多帮助:"
    echo "    cat $(dirname "$SCRIPT_DIR")/scripts/testing/README.md"
    echo ""
    echo "=========================================="
    echo ""
}

# 主函数
main() {
    show_banner

    install_dependencies
    setup_permissions
    build_kernel
    build_rootfs
    show_completion
}

main "$@"
