#!/bin/bash
# vhost-net 性能对比实验
#
# 对比 vhost=on vs vhost=off 的网络性能差异
#
# 测试指标:
#   · UDP 吞吐量 (pps)
#   · TCP 吞吐量 (Gbps)
#   · CPU 占用率
#
# 使用方法:
#   sudo bash vhost-perf-test.sh [setup|test|cleanup|report]
#
# 前置条件:
#   · 已安装 qemu-system-x86_64
#   · 已安装 iperf3
#   · 已创建 TAP 设备 (tap0)
#   · 已有可用的内核和 initramfs

set -e

ACTION=${1:-help}
DURATION=${2:-30}

# 路径配置
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KVM_STUDY_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
IMAGES_DIR="$KVM_STUDY_ROOT/scripts/images"
KERNEL="$IMAGES_DIR/bzImage"
INITRD="$IMAGES_DIR/initramfs.img"
FALLBACK_KERNEL="/root/code/linux-6.12.93/arch/x86_64/boot/bzImage"

# VM 配置
VM_NAME="vhost-perf-vm"
VM_PID_FILE="/tmp/${VM_NAME}.pid"
VM_LOG="/tmp/${VM_NAME}.log"
GUEST_IP="192.168.100.2"
HOST_IP="192.168.100.1"
TAP_DEV="tap0"

# 结果文件
RESULT_FILE="/tmp/vhost-perf-results.txt"

# --------------------------------------------------
# 辅助函数
# --------------------------------------------------

log_info() { echo -e "\033[0;32m[INFO]\033[0m $1"; }
log_warn() { echo -e "\033[0;33m[WARN]\033[0m $1"; }
log_error() { echo -e "\033[0;31m[ERROR]\033[0m $1"; }

check_prerequisites() {
    log_info "检查前置条件..."

    # 检查 QEMU
    if ! command -v qemu-system-x86_64 &> /dev/null; then
        log_error "QEMU 未安装"
        return 1
    fi

    # 检查 iperf3
    if ! command -v iperf3 &> /dev/null; then
        log_error "iperf3 未安装 (apt-get install iperf3)"
        return 1
    fi

    # 检查内核
    if [ -f "$KERNEL" ]; then
        KERNEL_PATH="$KERNEL"
    elif [ -f "$FALLBACK_KERNEL" ]; then
        KERNEL_PATH="$FALLBACK_KERNEL"
    else
        log_error "内核不存在"
        return 1
    fi

    # 检查 initramfs
    if [ ! -f "$INITRD" ]; then
        log_error "initramfs 不存在: $INITRD"
        log_info "运行: bash $KVM_STUDY_ROOT/scripts/vm/build-rootfs-minimal.sh"
        return 1
    fi

    # 检查 TAP 设备
    if ! ip link show "$TAP_DEV" &> /dev/null; then
        log_warn "TAP 设备 $TAP_DEV 不存在，创建中..."
        ip tuntap add dev "$TAP_DEV" mode tap
        ip addr add "$HOST_IP/24" dev "$TAP_DEV"
        ip link set "$TAP_DEV" up
    fi

    log_info "前置条件检查通过"
    return 0
}

# --------------------------------------------------
# setup: 启动 VM
# --------------------------------------------------

start_vm() {
    local VHOST_MODE=$1  # on or off

    log_info "启动 VM (vhost=$VHOST_MODE)..."

    # 停止已有的 VM
    stop_vm 2>/dev/null || true

    # 构建 QEMU 命令
    local NETDEV_OPTS
    if [ "$VHOST_MODE" = "on" ]; then
        NETDEV_OPTS="-netdev tap,id=net0,ifname=$TAP_DEV,script=no,downscript=no,vhost=on"
    else
        NETDEV_OPTS="-netdev tap,id=net0,ifname=$TAP_DEV,script=no,downscript=no,vhost=off"
    fi

    # 使用 tmux 运行
    if command -v tmux &> /dev/null; then
        tmux kill-session -t "$VM_NAME" 2>/dev/null || true
        tmux new-session -d -s "$VM_NAME" \
            "qemu-system-x86_64 \
            -enable-kvm -cpu host -m 2048 -smp 2 \
            -kernel $KERNEL_PATH \
            -initrd $INITRD \
            -append 'console=ttyS0 root=/dev/ram0 rdinit=/init ip=$GUEST_IP:::$HOST_IP:guest:eth0:off' \
            $NETDEV_OPTS \
            -device virtio-net-pci,netdev=net0 \
            -nographic -no-reboot"

        sleep 3

        # 获取 PID
        local VM_PID=$(tmux list-panes -t "$VM_NAME" -F '#{pane_pid}' 2>/dev/null | head -1)
        VM_PID=$(pgrep -P "$VM_PID" -f "qemu-system" 2>/dev/null | head -1 || echo "$VM_PID")
        echo "$VM_PID" > "$VM_PID_FILE"

        log_info "VM 已启动 (tmux: $VM_NAME, PID=$VM_PID)"
        log_info "进入 VM: tmux attach -t $VM_NAME"
    else
        log_error "tmux 未安装，无法启动 VM"
        return 1
    fi
}

stop_vm() {
    if command -v tmux &> /dev/null; then
        if tmux has-session -t "$VM_NAME" 2>/dev/null; then
            tmux kill-session -t "$VM_NAME" 2>/dev/null || true
            sleep 1
        fi
    fi

    if [ -f "$VM_PID_FILE" ]; then
        local VM_PID=$(cat "$VM_PID_FILE")
        kill "$VM_PID" 2>/dev/null || true
        rm -f "$VM_PID_FILE"
    fi

    log_info "VM 已停止"
}

# --------------------------------------------------
# test: 运行性能测试
# --------------------------------------------------

run_test() {
    local VHOST_MODE=$1
    local TEST_TYPE=$2  # tcp or udp

    log_info "运行测试: vhost=$VHOST_MODE, type=$TEST_TYPE"

    # 等待 Guest 启动
    log_info "等待 Guest 网络就绪..."
    local RETRIES=30
    while [ $RETRIES -gt 0 ]; do
        if ping -c 1 -W 1 "$GUEST_IP" &> /dev/null; then
            log_info "Guest 网络就绪"
            break
        fi
        sleep 1
        RETRIES=$((RETRIES - 1))
    done

    if [ $RETRIES -eq 0 ]; then
        log_error "Guest 网络未就绪"
        return 1
    fi

    # 在 Guest 中启动 iperf3 server
    log_info "在 Guest 中启动 iperf3 server..."
    # 注意: 这需要通过 tmux send-keys 或 SSH
    # 这里简化处理，假设 Guest 已经运行 iperf3 -s

    # 运行 iperf3 client
    log_info "运行 iperf3 client (${DURATION}s)..."

    local IPERF_OPTS
    if [ "$TEST_TYPE" = "udp" ]; then
        IPERF_OPTS="-u -b 10G -l 1400"
    else
        IPERF_OPTS="-P 4"
    fi

    local RESULT=$(iperf3 -c "$GUEST_IP" -t "$DURATION" $IPERF_OPTS 2>&1)

    # 提取关键指标
    local THROUGHPUT=$(echo "$RESULT" | grep -E "sender|receiver" | tail -1 | awk '{print $(NF-2), $(NF-1)}')
    local CPU_USAGE=$(top -bn1 | grep "Cpu(s)" | awk '{print $2}')

    log_info "结果: 吞吐=$THROUGHPUT, CPU=$CPU_USAGE%"

    # 保存结果
    echo "vhost=$VHOST_MODE type=$TEST_TYPE throughput=$THROUGHPUT cpu=$CPU_USAGE%" >> "$RESULT_FILE"
}

# --------------------------------------------------
# report: 生成报告
# --------------------------------------------------

generate_report() {
    log_info "生成性能报告..."

    if [ ! -f "$RESULT_FILE" ]; then
        log_error "结果文件不存在: $RESULT_FILE"
        return 1
    fi

    echo ""
    echo "=========================================="
    echo " vhost-net 性能对比报告"
    echo "=========================================="
    echo ""
    echo "测试时长: ${DURATION}s"
    echo "Guest IP: $GUEST_IP"
    echo ""

    # 按 vhost 模式分组显示
    echo "--- vhost=on ---"
    grep "vhost=on" "$RESULT_FILE" | while read line; do
        echo "  $line"
    done

    echo ""
    echo "--- vhost=off ---"
    grep "vhost=off" "$RESULT_FILE" | while read line; do
        echo "  $line"
    done

    echo ""
    echo "=========================================="
    echo " 分析"
    echo "=========================================="
    echo ""
    echo "预期结果:"
    echo "  · vhost=on: UDP ~300万 pps, TCP ~20-40 Gbps"
    echo "  · vhost=off: UDP ~100万 pps, TCP ~10-20 Gbps"
    echo "  · vhost=on 应该有 2-3 倍的吞吐提升"
    echo ""
    echo "原因:"
    echo "  · vhost 将数据面从 QEMU 卸载到内核"
    echo "  · 通过 ioeventfd/irqfd bypass QEMU"
    echo "  · 无用户态/内核态切换开销"
    echo "  · 批处理优化减少系统调用次数"
}

# --------------------------------------------------
# cleanup: 清理环境
# --------------------------------------------------

cleanup() {
    log_info "清理环境..."

    stop_vm 2>/dev/null || true
    rm -f "$RESULT_FILE"
    rm -f "$VM_LOG"

    log_info "清理完成"
}

# --------------------------------------------------
# 主逻辑
# --------------------------------------------------

case "$ACTION" in
    setup)
        check_prerequisites || exit 1
        start_vm "on"
        echo ""
        log_info "VM 已启动，请在 Guest 中运行: iperf3 -s"
        log_info "然后运行: sudo bash $0 test"
        ;;
    test)
        check_prerequisites || exit 1
        rm -f "$RESULT_FILE"

        # 测试 vhost=on
        start_vm "on"
        sleep 5
        run_test "on" "tcp"
        run_test "on" "udp"
        stop_vm

        # 测试 vhost=off
        start_vm "off"
        sleep 5
        run_test "off" "tcp"
        run_test "off" "udp"
        stop_vm

        # 生成报告
        generate_report
        ;;
    cleanup)
        cleanup
        ;;
    report)
        generate_report
        ;;
    *)
        echo "用法: sudo bash $0 {setup|test|cleanup|report} [duration]"
        echo ""
        echo "  setup   - 启动 VM (vhost=on)"
        echo "  test    - 运行完整性能测试 (vhost=on vs off)"
        echo "  cleanup - 清理环境"
        echo "  report  - 查看测试报告"
        echo ""
        echo "  duration: 测试时长（秒），默认 30"
        ;;
esac
