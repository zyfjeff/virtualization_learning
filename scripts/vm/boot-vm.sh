#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# 实验 VM 启动脚本
#
# 默认启用 KVM 硬件加速（-enable-kvm -cpu host）。这是必须的：
#   - 宿主侧 kvm:* tracepoint 只在走 KVM 时才会产生事件
#   - guest 内可见 VMX 依赖 -cpu host（phase1 实验前提）
# 需要对比纯软件模拟时用 --tcg 显式回退。
#
# 用法:
#   ./boot-vm.sh [镜像类型] [选项]
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
#   --debug            - 启用调试模式（GDB server，等待调试器）
#   --tcg              - 回退到 TCG 纯软件模拟（不用 KVM）
#   --qemu "<args>"    - 透传额外参数给 qemu
#
# 示例:
#   ./boot-vm.sh ubuntu --memory 4G --cpus 4 --queues 4
#   ./boot-vm.sh allinone --net user
#   ./boot-vm.sh minimal --debug
#   ./boot-vm.sh allinone --qemu "-machine q35"

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"
SHARED_DIR="$PROJECT_ROOT/shared"

# 默认参数
IMAGE_TYPE="ubuntu"
MEMORY="2G"
CPUS="2"
QUEUES="1"
NET_TYPE="tap"
GUI=false
DEBUG=false
ACCEL="kvm"
EXTRA_QEMU_ARGS=()

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

usage() {
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
    echo "  --debug            - 启用调试模式（GDB server，等待调试器）"
    echo "  --tcg              - 回退到 TCG 纯软件模拟（不用 KVM）"
    echo "  --qemu \"<args>\"    - 透传额外参数给 qemu"
    echo ""
    echo "示例:"
    echo "  $0 ubuntu --memory 4G --cpus 4 --queues 4"
    echo "  $0 allinone --net user"
    echo "  $0 minimal --debug"
    echo "  $0 allinone --qemu \"-machine q35\""
}

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
        --tcg)
            ACCEL="tcg"
            shift
            ;;
        --qemu)
            # shellcheck disable=SC2206  # 有意做词拆分，把一串参数展开成多个
            EXTRA_QEMU_ARGS+=($2)
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            log_error "未知参数: $1"
            echo ""
            usage
            exit 1
            ;;
    esac
done

# 检查加速器可用性
check_accel() {
    if [ "$ACCEL" = "tcg" ]; then
        log_warn "使用 TCG 纯软件模拟：宿主侧 kvm:* tracepoint 不会有任何事件"
        return
    fi

    if [ ! -e /dev/kvm ]; then
        log_error "/dev/kvm 不存在，无法启用 KVM 加速"
        log_error "  加载模块: modprobe kvm_intel   （AMD: modprobe kvm_amd）"
        log_error "  确认虚拟化已在 BIOS 中开启"
        log_error "  只想跑纯软件模拟: $0 $IMAGE_TYPE --tcg"
        exit 1
    fi

    if [ ! -w /dev/kvm ]; then
        log_error "/dev/kvm 不可写，当前用户无权使用 KVM"
        log_error "  用 root 运行，或把用户加入 kvm 组: usermod -aG kvm \$USER"
        exit 1
    fi

    # guest 内可见 VMX 还需要宿主开启嵌套虚拟化
    local nested_param="/sys/module/kvm_intel/parameters/nested"
    if [ -r "$nested_param" ]; then
        local nested
        nested=$(cat "$nested_param")
        if [ "$nested" != "Y" ] && [ "$nested" != "1" ]; then
            log_warn "嵌套虚拟化未开启（$nested_param = $nested）"
            log_warn "  guest 内看不到 VMX，phase1 的 VT-x 实验无法进行"
            log_warn "  开启方式: modprobe -r kvm_intel && modprobe kvm_intel nested=1"
        fi
    fi
}

check_accel

# 设置内核路径（使用 build-kernel.sh 输出的 bzImage）
KERNEL="$IMAGES_DIR/bzImage"
if [ ! -f "$KERNEL" ]; then
    log_error "内核不存在: $KERNEL"
    log_error "请先运行: $SCRIPT_DIR/build-kernel.sh"
    exit 1
fi

# 根据镜像类型设置路径
case $IMAGE_TYPE in
    ubuntu)
        INITRD="$IMAGES_DIR/initramfs-ubuntu.img"
        DISK="$IMAGES_DIR/disk-ubuntu.img"
        BUILDER="build-rootfs-ubuntu.sh"
        ;;
    allinone)
        INITRD="$IMAGES_DIR/initramfs-allinone.img"
        DISK="$IMAGES_DIR/disk-allinone.img"
        BUILDER="build-rootfs-allinone.sh"
        ;;
    minimal)
        INITRD="$IMAGES_DIR/initramfs.img"
        DISK="$IMAGES_DIR/disk.img"
        BUILDER="build-rootfs-minimal.sh"
        ;;
esac

# 检查镜像是否存在
if [ ! -f "$INITRD" ]; then
    log_error "initramfs 不存在: $INITRD"
    log_error "请先运行: sudo $SCRIPT_DIR/$BUILDER"
    exit 1
fi

# 构建 QEMU 命令
QEMU_ARGS=(
    qemu-system-x86_64
    -m "$MEMORY"
    -smp "$CPUS"
    -kernel "$KERNEL"
    -initrd "$INITRD"
)

# 加速器与 CPU 模型
if [ "$ACCEL" = "kvm" ]; then
    QEMU_ARGS+=(-enable-kvm -cpu host)
fi

# 添加磁盘（如果存在）
if [ -f "$DISK" ]; then
    QEMU_ARGS+=(-drive "file=$DISK,format=qcow2,if=virtio")
fi

# 共享目录：rootfs 的 /init 会把 mount_tag=hostshare 挂到 guest 的 /mnt/shared
mkdir -p "$SHARED_DIR"
QEMU_ARGS+=(-virtfs "local,path=$SHARED_DIR,mount_tag=hostshare,security_model=passthrough,id=hostshare")

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
        QEMU_ARGS+=(-netdev "tap,id=net0,ifname=$TAP_NAME,vhost=on,queues=$QUEUES")
        QEMU_ARGS+=(-device "virtio-net-pci,netdev=net0,mq=on,vectors=$VECTORS")
        ;;
    user)
        QEMU_ARGS+=(-netdev "user,id=net0,hostfwd=tcp::2222-:22")
        QEMU_ARGS+=(-device virtio-net-pci,netdev=net0)
        ;;
    none)
        # 无网络
        ;;
    *)
        log_error "未知网络类型: $NET_TYPE（可选 tap / user / none）"
        exit 1
        ;;
esac

# 配置显示。串口模式必须给内核 console=ttyS0，否则串口上看不到 guest 输出
if [ "$GUI" = true ]; then
    QEMU_ARGS+=(-display gtk)
else
    QEMU_ARGS+=(-nographic -serial mon:stdio -append "console=ttyS0")
fi

# 调试模式
if [ "$DEBUG" = true ]; then
    QEMU_ARGS+=(-d guest_errors,unimp)
    QEMU_ARGS+=(-s -S)  # 启用 GDB server，等待调试器
fi

# 透传的额外参数放最后，便于覆盖前面的默认值
if [ ${#EXTRA_QEMU_ARGS[@]} -gt 0 ]; then
    QEMU_ARGS+=("${EXTRA_QEMU_ARGS[@]}")
fi

# 显示启动信息
echo ""
echo "=========================================="
echo "  KVM Study - 实验 VM 启动"
echo "=========================================="
echo ""
echo "  镜像类型: $IMAGE_TYPE"
echo "  加速器:   $ACCEL"
echo "  内存:     $MEMORY"
echo "  CPU:      $CPUS"
echo "  网络:     $NET_TYPE"
if [ "$NET_TYPE" = "tap" ]; then
    echo "  队列:     $QUEUES"
fi
echo "  共享目录: $SHARED_DIR → guest /mnt/shared"
if [ "$GUI" != true ]; then
    echo ""
    echo "  退出 VM:  Ctrl-A X"
fi
if [ "$DEBUG" = true ]; then
    echo ""
    echo "  GDB:      target remote :1234（VM 已暂停，等待连接）"
fi
echo ""
echo "=========================================="
echo ""

# 启动 VM
log_info "启动 VM..."
exec "${QEMU_ARGS[@]}"
