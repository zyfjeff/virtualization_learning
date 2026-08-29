#!/bin/bash
# 构建极简 rootfs（仅用于 KVM 测试）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"
ROOTFS_DIR="$IMAGES_DIR/rootfs"

# 颜色输出
log_info() { echo -e "\033[0;32m[INFO]\033[0m $1"; }
log_warn() { echo -e "\033[0;33m[WARN]\033[0m $1"; }
log_error() { echo -e "\033[0;31m[ERROR]\033[0m $1"; }

# 检查依赖
check_dependencies() {
    log_info "检查依赖..."

    if ! command -v busybox &>/dev/null; then
        log_error "busybox 未安装"
        echo "  Ubuntu/Debian: sudo apt install busybox-static"
        exit 1
    fi

    if ! command -v cpio &>/dev/null; then
        log_error "cpio 未安装"
        echo "  Ubuntu/Debian: sudo apt install cpio"
        exit 1
    fi

    log_info "✓ 依赖检查通过"
}

# 创建最小目录结构
create_minimal_structure() {
    log_info "创建最小目录结构..."

    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"/{bin,sbin,proc,sys,dev,tmp,mnt/shared}

    # 复制静态编译的 busybox
    cp "$(which busybox)" "$ROOTFS_DIR/bin/"
    cd "$ROOTFS_DIR/bin"
    ln -sf busybox sh
    ln -sf busybox mount
    ln -sf busybox umount
    ln -sf busybox mkdir
    ln -sf busybox echo
    ln -sf busybox cat
    ln -sf busybox ls
    cd - >/dev/null

    log_info "✓ 目录结构创建完成"
}

# 创建极简 /init
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

# 显示欢迎信息
echo ""
echo "=========================================="
echo "  KVM Study Test Environment"
echo "=========================================="
echo ""
echo "  Linux $(uname -r)"
echo ""

if [ -d /mnt/shared ] && [ "$(ls -A /mnt/shared 2>/dev/null)" ]; then
    echo "  共享目录: /mnt/shared"
    echo "  Phase 1 实战练习:"
    ls -1 /mnt/shared/ex1-* /mnt/shared/ex2-* /mnt/shared/ex3-* /mnt/shared/ex5-* 2>/dev/null | grep -v '\.c$' | sed 's|^|    |'
    echo ""
fi

echo "  可用命令: sh, mount, ls, cat, echo..."
echo ""
echo "=========================================="
echo ""

# 启动 shell
exec /bin/sh
EOF

    chmod +x "$ROOTFS_DIR/init"
    log_info "✓ /init 创建完成"
}

# 打包为 initramfs
create_initramfs() {
    log_info "打包为 initramfs..."

    local img="$IMAGES_DIR/initramfs.img"

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

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - Minimal Rootfs Builder"
    echo "========================================"
    echo ""

    check_dependencies
    create_minimal_structure
    create_init
    create_initramfs

    echo ""
    echo "========================================"
    echo "  ✓ Rootfs 构建完成!"
    echo "========================================"
    echo ""
    echo "  输出: $IMAGES_DIR/initramfs.img"
    echo ""
    echo "  使用方法:"
    echo "    ./boot-vm.sh"
    echo ""
}

main "$@"
