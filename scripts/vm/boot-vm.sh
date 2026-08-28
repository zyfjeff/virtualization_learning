#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# 统一的 VM 启动脚本
#
# 用法:
#   ./boot-vm-unified.sh [镜像类型] [选项]
#
# 镜像类型:
#   ubuntu    - Ubuntu 基础系统（推荐）
#   allinone  - All-in-One 系统
#   minimal   - 最小化系统
#
# 选项:
#   --memory <size>    - 内存大小（默认 2G）
#   --cpus <num>       - CPU 数量（默认 2）
#   --queues <num>     - virtio-net 队列数（默认 1）
#   --net <type>       - 网络类型: tap, user, none（默认 tap）
#   --gui              - 启用图形界面
#   --debug            - 启用调试模式
#
# 示例:
#   ./boot-vm-unified.sh ubuntu --memory 4G --cpus 4 --queues 4
#   ./boot-vm-unified.sh allinone --net user
#   ./boot-vm-unified.sh minimal --debug

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"

# 默认参数
IMAGE_TYPE="ubuntu"
MEMORY="2G"
CPUS="2"
QUEUES="1"
NET_TYPE="tap"
GUI=false
DEBUG=false

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        ubuntu|allinone|minimal)
            IMAGE_TYPE="$1"
            shift
            ;;
        --memory)
            MEMORY="$2"
            shift 2
            ;;
        --cpus)
            CPUS="$2"
            shift 2
            ;;
        --queues)
            QUEUES="$2"
            shift 2
            ;;
        --net)
            NET_TYPE="$2"
            shift 2
            ;;
        --gui)
            GUI=true
            shift
            ;;
        --debug)
            DEBUG=true
            shift
            ;;
        -h|--help)
            echo "用法: $0 [镜像类型] [选项]"
            echo ""
            echo "镜像类型:"
            echo "  ubuntu    - Ubuntu 基础系统（推荐）"
            echo "  allinone  - All-in-One 系统"
            echo "  minimal   - 最小化系统"
            echo ""
            echo "选项:"
            echo "  --memory <size>    - 内存大小（默认 2G）"
            echo "  --cpus <num>       - CPU 数量（默认 2）"
            echo "  --queues <num>     - virtio-net 队列数（默认 1）"
            echo "  --net <type>       - 网络类型: tap, user, none（默认 tap）"
            echo "  --gui              - 启用图形界面"
            echo "  --debug            - 启用调试模式"
            echo ""
            echo "示例:"
            echo "  $0 ubuntu --memory 4G --cpus 4 --queues 4"
            echo "  $0 allinone --net user"
            echo "  $0 minimal --debug"
            exit 0
            ;;
        *)
            log_error "未知参数: $1"
            exit 1
            ;;
    esac
done

# 设置内核路径
KERNEL="/root/code/linux-6.12.93/arch/x86_64/boot/bzImage"
if [ ! -f "$KERNEL" ]; then
    log_error "内核不存在: $KERNEL"
    log_error "请先运行: ./build-kernel.sh"
    exit 1
fi

# 根据镜像类型设置路径
case $IMAGE_TYPE in
    ubuntu)
        INITRD="$IMAGES_DIR/initramfs-ubuntu.img"
        DISK="$IMAGES_DIR/disk-ubuntu.img"
        ;;
    allinone)
        INITRD="$IMAGES_DIR/initramfs-allinone.img"
        DISK="$IMAGES_DIR/disk-allinone.img"
        ;;
    minimal)
        INITRD="$IMAGES_DIR/initramfs.img"
        DISK="$IMAGES_DIR/disk.img"
        ;;
esac

# 检查镜像是否存在
if [ ! -f "$INITRD" ]; then
    log_error "initramfs 不存在: $INITRD"
    log_error "请先运行: ./build-rootfs-$IMAGE_TYPE.sh"
    exit 1
fi

# 构建 QEMU 命令
QEMU_CMD="qemu-system-x86_64"
QEMU_CMD="$QEMU_CMD -m $MEMORY"
QEMU_CMD="$QEMU_CMD -smp $CPUS"
QEMU_CMD="$QEMU_CMD -kernel $KERNEL"
QEMU_CMD="$QEMU_CMD -initrd $INITRD"

# 添加磁盘（如果存在）
if [ -f "$DISK" ]; then
    QEMU_CMD="$QEMU_CMD -drive file=$DISK,format=qcow2,if=virtio"
fi

# 配置网络
case $NET_TYPE in
    tap)
        # 创建 TAP 设备
        TAP_NAME="tap0"
        if ! ip link show "$TAP_NAME" &>/dev/null; then
            log_info "创建 TAP 设备: $TAP_NAME"
            ip tuntap add "$TAP_NAME" mode tap
            ip link set "$TAP_NAME" up
        fi
        
        # 配置 virtio-net
        VECTORS=$((2 * QUEUES + 2))
        QEMU_CMD="$QEMU_CMD -netdev tap,id=net0,ifname=$TAP_NAME,vhost=on,queues=$QUEUES"
        QEMU_CMD="$QEMU_CMD -device virtio-net-pci,netdev=net0,mq=on,vectors=$VECTORS"
        ;;
    user)
        QEMU_CMD="$QEMU_CMD -netdev user,id=net0,hostfwd=tcp::2222-:22"
        QEMU_CMD="$QEMU_CMD -device virtio-net-pci,netdev=net0"
        ;;
    none)
        # 无网络
        ;;
esac

# 配置显示
if [ "$GUI" = true ]; then
    QEMU_CMD="$QEMU_CMD -display gtk"
else
    QEMU_CMD="$QEMU_CMD -nographic -serial mon:stdio"
fi

# 调试模式
if [ "$DEBUG" = true ]; then
    QEMU_CMD="$QEMU_CMD -d guest_errors,unimp"
    QEMU_CMD="$QEMU_CMD -s -S"  # 启用 GDB server，等待调试器
fi

# 显示启动信息
echo ""
echo "=========================================="
echo "  KVM Study - VM 启动"
echo "=========================================="
echo ""
echo "  镜像类型: $IMAGE_TYPE"
echo "  内存: $MEMORY"
echo "  CPU: $CPUS"
echo "  网络: $NET_TYPE"
if [ "$NET_TYPE" = "tap" ]; then
    echo "  队列: $QUEUES"
fi
echo ""
echo "=========================================="
echo ""

# 启动 VM
log_info "启动 VM..."
eval "$QEMU_CMD"
