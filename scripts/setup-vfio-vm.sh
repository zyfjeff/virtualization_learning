#!/bin/bash
# PI 实验环境搭建：启动带设备直通的 VM
#
# 本脚本将设备 0000:4b:00.0 通过 VFIO 直通给 VM，
# 用于观察 Posted Interrupts 的完整工作流程。
#
# 使用方法:
#   sudo bash setup-vfio-vm.sh build   # 构建内核和 rootfs（首次需要）
#   sudo bash setup-vfio-vm.sh start   # 启动 VM
#   sudo bash setup-vfio-vm.sh stop    # 停止 VM
#   sudo bash setup-vfio-vm.sh status  # 查看状态

set -e

DEVICE="0000:4b:00.0"
VM_NAME="pi-test-vm"
PID_FILE="/tmp/${VM_NAME}.pid"

# 项目路径（脚本位于 scripts/ 目录）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KVM_STUDY_ROOT="$(dirname "$SCRIPT_DIR")"  # scripts/ 的父目录 = 项目根目录
TESTING_DIR="$SCRIPT_DIR/testing"
IMAGES_DIR="$SCRIPT_DIR/images"

# 内核和 initramfs 路径
KERNEL="$IMAGES_DIR/bzImage"
INITRD="$IMAGES_DIR/initramfs.img"

# 备选内核路径（已编译的内核）
FALLBACK_KERNEL="/root/code/linux-6.12.93/arch/x86_64/boot/bzImage"

ACTION=${1:-status}

# --------------------------------------------------
# 函数定义
# --------------------------------------------------

build_images() {
    echo "=========================================="
    echo " 构建内核和 rootfs"
    echo "=========================================="
    echo ""

    mkdir -p "$IMAGES_DIR"

    # 检查内核
    if [ -f "$KERNEL" ]; then
        echo "  [✓] 内核已存在: $KERNEL"
    elif [ -f "$FALLBACK_KERNEL" ]; then
        echo "  [i] 使用已编译的内核: $FALLBACK_KERNEL"
        cp "$FALLBACK_KERNEL" "$KERNEL"
        echo "  [✓] 已复制到: $KERNEL"
    else
        echo "  [i] 构建内核..."
        echo "  运行: bash $TESTING_DIR/build-kernel.sh"
        echo ""
        echo "  或者手动复制已编译的内核:"
        echo "    cp /root/code/linux-6.12.93/arch/x86_64/boot/bzImage $KERNEL"
        echo ""
        return 1
    fi

    # 检查 initramfs
    if [ -f "$INITRD" ]; then
        echo "  [✓] initramfs 已存在: $INITRD"
    else
        echo "  [i] 构建 initramfs..."
        if [ -f "$TESTING_DIR/build-rootfs-simple.sh" ]; then
            bash "$TESTING_DIR/build-rootfs-simple.sh"
            # build-rootfs-simple.sh 输出到 images/ 目录
            if [ -f "$IMAGES_DIR/initramfs.img" ]; then
                echo "  [✓] initramfs 已构建: $INITRD"
            else
                echo "  [!] 构建完成但文件不在预期位置，请检查 $IMAGES_DIR/"
                ls "$IMAGES_DIR/" 2>/dev/null
            fi
        else
            echo "  [错误] 构建脚本不存在: $TESTING_DIR/build-rootfs-simple.sh"
            return 1
        fi
    fi

    echo ""
    echo "  构建完成！"
    echo ""
}

check_prerequisites() {
    echo "--- 前置检查 ---"

    # 检查 QEMU
    if ! command -v qemu-system-x86_64 &> /dev/null; then
        echo "[错误] QEMU 未安装"
        exit 1
    fi
    echo "  [✓] QEMU 已安装"

    # 检查内核
    if [ -f "$KERNEL" ]; then
        echo "  [✓] 内核: $KERNEL"
    elif [ -f "$FALLBACK_KERNEL" ]; then
        echo "  [i] 使用备选内核: $FALLBACK_KERNEL"
        KERNEL="$FALLBACK_KERNEL"
    else
        echo "[错误] 内核不存在"
        echo "  运行: sudo bash $0 build"
        exit 1
    fi

    # 检查 initramfs
    if [ -f "$INITRD" ]; then
        echo "  [✓] initramfs: $INITRD"
    else
        echo "[错误] initramfs 不存在: $INITRD"
        echo "  运行: sudo bash $0 build"
        exit 1
    fi

    # 检查设备
    if ! lspci -s "$DEVICE" &> /dev/null; then
        echo "[错误] 设备不存在: $DEVICE"
        exit 1
    fi
    echo "  [✓] 设备: $(lspci -s $DEVICE | sed 's/^[^ ]* //')"

    # 检查设备是否正在被其他 VM 使用
    CURRENT_DRIVER=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/driver 2>/dev/null) 2>/dev/null || echo "none")
    if [ "$CURRENT_DRIVER" = "virtio-pci" ]; then
        echo ""
        echo "  [!] 警告: 设备当前使用 virtio-pci 驱动"
        echo "      这可能意味着设备正在被另一个 VM 使用"
        echo "      解绑设备会导致该 VM 失去磁盘！"
        echo ""
        read -p "  是否继续？(y/N) " -n 1 -r
        echo ""
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            echo "  已取消"
            exit 0
        fi
    fi

    # 检查 IOMMU
    if ! dmesg | grep -qi "DMAR-IR"; then
        echo "[错误] 中断重映射未启用"
        exit 1
    fi
    echo "  [✓] 中断重映射已启用"

    # 检查 vfio-pci 模块
    if ! lsmod | grep -q "vfio_pci"; then
        echo "  [i] 加载 vfio-pci 模块..."
        modprobe vfio-pci
    fi
    echo "  [✓] vfio-pci 模块已加载"

    echo ""
}

bind_vfio() {
    echo "--- 绑定设备到 vfio-pci ---"

    CURRENT_DRIVER=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/driver 2>/dev/null) 2>/dev/null || echo "none")

    if [ "$CURRENT_DRIVER" = "vfio-pci" ]; then
        echo "  [✓] 设备已绑定到 vfio-pci"
        return
    fi

    echo "  当前驱动: $CURRENT_DRIVER"

    # 解绑当前驱动
    if [ "$CURRENT_DRIVER" != "none" ]; then
        echo "  解绑 $CURRENT_DRIVER..."
        echo "$DEVICE" > /sys/bus/pci/devices/$DEVICE/driver/unbind 2>/dev/null || true
    fi

    # 设置 driver_override
    echo "vfio-pci" > /sys/bus/pci/devices/$DEVICE/driver_override

    # 触发驱动探测
    echo "$DEVICE" > /sys/bus/pci/drivers_probe 2>/dev/null || true

    # 验证
    sleep 1
    NEW_DRIVER=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/driver 2>/dev/null) 2>/dev/null || echo "none")

    if [ "$NEW_DRIVER" = "vfio-pci" ]; then
        echo "  [✓] 设备已绑定到 vfio-pci"
    else
        echo "  [✗] 绑定失败，当前驱动: $NEW_DRIVER"
        echo "  尝试手动绑定..."
        echo "$DEVICE" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
        NEW_DRIVER=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/driver 2>/dev/null) 2>/dev/null || echo "none")
        if [ "$NEW_DRIVER" = "vfio-pci" ]; then
            echo "  [✓] 手动绑定成功"
        else
            echo "  [✗] 绑定失败"
            exit 1
        fi
    fi

    echo ""
}

unbind_vfio() {
    echo "--- 恢复设备驱动 ---"

    CURRENT_DRIVER=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/driver 2>/dev/null) 2>/dev/null || echo "none")

    if [ "$CURRENT_DRIVER" = "vfio-pci" ]; then
        echo "  解绑 vfio-pci..."
        echo "$DEVICE" > /sys/bus/pci/devices/$DEVICE/driver/unbind 2>/dev/null || true
    fi

    # 清除 driver_override
    echo "" > /sys/bus/pci/devices/$DEVICE/driver_override 2>/dev/null || true

    # 重新绑定原始驱动
    echo "$DEVICE" > /sys/bus/pci/drivers_probe 2>/dev/null || true

    sleep 1
    NEW_DRIVER=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/driver 2>/dev/null) 2>/dev/null || echo "none")
    echo "  当前驱动: $NEW_DRIVER"
    echo ""
}

start_vm() {
    echo "--- 启动 VM ---"

    # 检查是否已在运行
    if [ -f "$PID_FILE" ]; then
        OLD_PID=$(cat "$PID_FILE")
        if kill -0 "$OLD_PID" 2>/dev/null; then
            echo "  [i] VM 已在运行 (PID=$OLD_PID)"
            return
        fi
    fi

    # 绑定设备到 vfio-pci
    bind_vfio

    # 获取 IOMMU group
    IOMMU_GROUP=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/iommu_group 2>/dev/null) 2>/dev/null)
    echo "  IOMMU group: $IOMMU_GROUP"

    # 启动 QEMU
    echo "  启动 QEMU (设备直通: $DEVICE)..."
    echo "  内核: $KERNEL"
    echo "  initrd: $INITRD"
    echo ""

    # 注意: -nographic 不能和 -daemonize 一起使用
    # 使用 tmux 运行，方便交互进入 Guest
    VM_LOG="/tmp/${VM_NAME}.log"

    QEMU_CMD="qemu-system-x86_64 \
        -enable-kvm \
        -cpu host \
        -m 1024 \
        -smp 2 \
        -kernel $KERNEL \
        -initrd $INITRD \
        -append 'console=ttyS0 root=/dev/ram0 rdinit=/init' \
        -device vfio-pci,host=$DEVICE \
        -nographic \
        -no-reboot"

    if command -v tmux &> /dev/null; then
        # 使用 tmux 运行，可以 attach 进入 Guest
        tmux kill-session -t "$VM_NAME" 2>/dev/null || true
        tmux new-session -d -s "$VM_NAME" "$QEMU_CMD"
        sleep 3

        # 获取 tmux 中 QEMU 的 PID
        VM_PID=$(tmux list-panes -t "$VM_NAME" -F '#{pane_pid}' 2>/dev/null | head -1)
        # pane_pid 是 shell 的 PID，需要找子进程
        VM_PID=$(pgrep -P "$VM_PID" -f "qemu-system" 2>/dev/null | head -1 || echo "$VM_PID")
        echo "$VM_PID" > "$PID_FILE"

        echo "  [✓] VM 已启动 (tmux session: $VM_NAME)"
        echo ""
        echo "  ★ 进入 Guest:  tmux attach -t $VM_NAME"
        echo "  脱离 Guest:    Ctrl+B 然后按 D"
        echo "  停止 VM:       sudo bash $0 stop"
        echo ""
        echo "  在 Guest 内部测试:"
        echo "    lspci                          # 查看直通设备"
        echo "    cat /proc/interrupts           # 查看中断"
        echo "    dd if=/dev/vda of=/dev/null bs=4k count=1000  # 生成 I/O"
    else
        # 没有 tmux，使用 nohup 后台运行
        echo "  [i] tmux 未安装，使用后台模式"
        echo "  安装 tmux: apt-get install tmux"
        echo ""

        nohup bash -c "$QEMU_CMD" > "$VM_LOG" 2>&1 &
        VM_PID=$!
        echo "$VM_PID" > "$PID_FILE"

        sleep 2

        if kill -0 "$VM_PID" 2>/dev/null; then
            echo "  [✓] VM 已启动 (PID=$VM_PID)"
            echo ""
            echo "  串口日志: tail -f $VM_LOG"
            echo "  (后台模式无法交互输入，建议安装 tmux)"
        else
            echo "  [✗] VM 进程已退出，查看日志:"
            echo "    cat $VM_LOG"
            unbind_vfio
            exit 1
        fi
    fi

    echo ""
}

stop_vm() {
    echo "--- 停止 VM ---"

    # 停止 tmux session
    if command -v tmux &> /dev/null; then
        if tmux has-session -t "$VM_NAME" 2>/dev/null; then
            echo "  停止 tmux session: $VM_NAME"
            tmux kill-session -t "$VM_NAME" 2>/dev/null || true
            sleep 1
        fi
    fi

    # 停止进程
    if [ -f "$PID_FILE" ]; then
        VM_PID=$(cat "$PID_FILE")
        if kill -0 "$VM_PID" 2>/dev/null; then
            echo "  停止 VM (PID=$VM_PID)..."
            kill "$VM_PID" 2>/dev/null || true
            sleep 2
            kill -9 "$VM_PID" 2>/dev/null || true
        fi
        rm -f "$PID_FILE"
        echo "  [✓] VM 已停止"
    else
        echo "  [i] VM 未在运行"
    fi

    # 恢复设备驱动
    unbind_vfio

    echo ""
}

show_status() {
    echo "--- 当前状态 ---"
    echo ""

    # VM 状态
    if [ -f "$PID_FILE" ]; then
        VM_PID=$(cat "$PID_FILE")
        if kill -0 "$VM_PID" 2>/dev/null; then
            echo "  VM: 运行中 (PID=$VM_PID)"
        else
            echo "  VM: 未运行 (PID 文件过期)"
        fi
    else
        echo "  VM: 未运行"
    fi

    # 设备状态
    CURRENT_DRIVER=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/driver 2>/dev/null) 2>/dev/null || echo "none")
    echo "  设备 $DEVICE 驱动: $CURRENT_DRIVER"

    # 镜像状态
    if [ -f "$KERNEL" ]; then
        echo "  内核: $KERNEL [存在]"
    elif [ -f "$FALLBACK_KERNEL" ]; then
        echo "  内核: $FALLBACK_KERNEL [备选]"
    else
        echo "  内核: [不存在，运行 build]"
    fi

    if [ -f "$INITRD" ]; then
        echo "  initrd: $INITRD [存在]"
    else
        echo "  initrd: [不存在，运行 build]"
    fi

    # 中断状态
    echo ""
    echo "  设备中断:"
    cat /proc/interrupts | grep "$DEVICE" | while read line; do
        IRQ=$(echo "$line" | awk '{print $1}' | tr -d ':')
        NAME=$(echo "$line" | awk '{print $NF}')
        TOTAL=$(echo "$line" | awk '{sum=0; for(i=2;i<NF;i++) sum+=$i; print sum}')
        echo "    IRQ $IRQ ($NAME): $TOTAL 次"
    done

    echo ""
}

# --------------------------------------------------
# 主逻辑
# --------------------------------------------------

case "$ACTION" in
    build)
        build_images
        ;;
    start)
        check_prerequisites
        start_vm
        echo "=========================================="
        echo " VM 已启动，可以运行 PI 实验："
        echo "   sudo bash ex3-pi-trace.sh"
        echo "   sudo bash ex4-pi-vs-remapped.sh"
        echo "=========================================="
        ;;
    stop)
        stop_vm
        ;;
    status)
        show_status
        ;;
    *)
        echo "用法: sudo bash $0 {build|start|stop|status}"
        echo ""
        echo "  build   - 构建内核和 rootfs（首次需要）"
        echo "  start   - 绑定设备到 vfio-pci 并启动 VM"
        echo "  stop    - 停止 VM 并恢复设备驱动"
        echo "  status  - 查看当前状态"
        ;;
esac
