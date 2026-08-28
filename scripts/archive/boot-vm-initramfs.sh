#!/bin/bash
# 启动 VM（支持 9p 共享目录）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"
SHARED_DIR="$PROJECT_ROOT/shared"

# 颜色输出
log_info() { echo -e "\033[0;32m[INFO]\033[0m $1"; }

# 准备共享目录
prepare_shared_dir() {
    log_info "准备共享目录..."
    mkdir -p "$SHARED_DIR"

    # 复制 Phase 1 练习程序到共享目录
    cp -v "$PROJECT_ROOT/phase1-vtx-basics/practice/ex1-vmx-verify" "$SHARED_DIR/" 2>/dev/null || true
    cp -v "$PROJECT_ROOT/phase1-vtx-basics/practice/ex2-cpuid-fault" "$SHARED_DIR/" 2>/dev/null || true
    cp -v "$PROJECT_ROOT/phase1-vtx-basics/practice/ex3-msr-test" "$SHARED_DIR/" 2>/dev/null || true
    cp -v "$PROJECT_ROOT/phase1-vtx-basics/practice/ex5-vmexit-overhead" "$SHARED_DIR/" 2>/dev/null || true

    log_info "✓ 共享目录准备完成: $SHARED_DIR"
}

# 检查镜像
check_images() {
    if [ ! -f "$IMAGES_DIR/bzImage" ]; then
        echo "错误: 内核镜像不存在"
        echo "请先运行: ./build-kernel.sh"
        exit 1
    fi

    if [ ! -f "$IMAGES_DIR/initramfs.img" ]; then
        echo "错误: initramfs 不存在"
        echo "请先运行: ./build-rootfs-simple.sh"
        exit 1
    fi
}

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - VM Boot"
    echo "========================================"
    echo ""

    check_images
    prepare_shared_dir

    log_info "启动 VM..."
    echo ""
    echo "  提示:"
    echo "    - 共享目录: $SHARED_DIR → /mnt/shared"
    echo "    - 测试程序: /mnt/shared/test-*"
    echo "    - 退出 VM:  Ctrl-A X"
    echo ""

    cd "$IMAGES_DIR"

    qemu-system-x86_64 \
        -enable-kvm \
        -cpu host \
        -kernel bzImage \
        -initrd initramfs.img \
        -append "console=ttyS0" \
        -virtfs local,path="$SHARED_DIR",mount_tag=hostshare,security_model=passthrough,id=hostshare \
        -nographic \
        -m 512 \
        -no-reboot
}

main "$@"
