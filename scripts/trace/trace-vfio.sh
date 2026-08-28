#!/bin/bash
#
# trace-vfio.sh - 跟踪 VFIO 设备直通操作
#
# 用法: sudo ./trace-vfio.sh [选项]
#   -p PID     跟踪指定的 QEMU 进程 PID
#   -d SECS    跟踪持续时间（默认 10 秒）
#   -a         跟踪所有 VFIO 相关事件（包括 IOMMU）
#   -s         只显示摘要
#   -h         显示帮助
#

set -euo pipefail

TRACEFS=""
PID=""
DURATION=10
ALL_VFIO=false
SUMMARY_ONLY=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "跟踪 VFIO 设备直通操作"
    echo ""
    echo "选项:"
    echo "  -p PID     跟踪指定的 QEMU 进程 PID"
    echo "  -d SECS    跟踪持续时间（默认 10 秒）"
    echo "  -a         跟踪所有 VFIO/IOMMU 事件"
    echo "  -s         只显示摘要"
    echo "  -h         显示帮助"
    echo ""
    echo "示例:"
    echo "  sudo $0 -p 12345 -d 5"
    echo "  sudo $0 -p 12345 -a    # 跟踪所有事件"
    echo "  sudo $0 -p 12345 -s    # 只统计"
    exit 0
}

cleanup() {
    if [ -n "$TRACEFS" ] && [ -d "$TRACEFS" ]; then
        echo 0 > "$TRACEFS/tracing_on" 2>/dev/null || true
        echo > "$TRACEFS/set_event" 2>/dev/null || true
        echo nop > "$TRACEFS/current_tracer" 2>/dev/null || true
        echo > "$TRACEFS/set_ftrace_filter" 2>/dev/null || true
    fi
}

trap cleanup EXIT

while getopts "p:d:ash" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        a) ALL_VFIO=true ;;
        s) SUMMARY_ONLY=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    exit 1
fi

for tfs in /sys/kernel/debug/tracing /sys/kernel/tracing; do
    if [ -d "$tfs" ]; then
        TRACEFS="$tfs"
        break
    fi
done

if [ -z "$TRACEFS" ]; then
    echo -e "${RED}错误: 找不到 tracefs${NC}"
    exit 1
fi

if [ -z "$PID" ]; then
    PID=$(pgrep -f "qemu-system" | head -1 || echo "")
    if [ -n "$PID" ]; then
        echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
    fi
fi

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  VFIO 设备直通跟踪${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "跟踪参数:"
echo "  PID:      ${PID:-所有进程}"
echo "  持续时间: ${DURATION} 秒"
echo "  模式:     ${ALL_VFIO:+全部 VFIO/IOMMU}${SUMMARY_ONLY:+摘要}"
echo ""

# 显示 VFIO 状态
echo -e "${BLUE}--- VFIO 环境状态 ---${NC}"
echo "  VFIO 模块:"
lsmod | grep vfio | while read -r line; do
    echo "    $line"
done
echo ""

echo "  IOMMU 组:"
for grp in /sys/kernel/iommu_groups/*/; do
    if [ -d "$grp" ]; then
        grp_id=$(basename "$grp")
        devices=$(ls "$grp/devices/" 2>/dev/null | tr '\n' ', ' | sed 's/,$//')
        if [ -n "$devices" ]; then
            echo "    Group $grp_id: $devices"
        fi
    fi
done 2>/dev/null || echo "    (无法读取 IOMMU 组)"
echo ""

# 清空之前的跟踪
echo > "$TRACEFS/trace"
echo 0 > "$TRACEFS/tracing_on"
echo > "$TRACEFS/set_event"

# 设置 VFIO 相关事件
echo "设置跟踪事件..."

# VFIO 核心事件
echo vfio:* >> "$TRACEFS/set_event" 2>/dev/null || true

# IOMMU 事件
if [ "$ALL_VFIO" = true ]; then
    echo iommu:* >> "$TRACEFS/set_event" 2>/dev/null || true
fi

# 函数级跟踪
echo function > "$TRACEFS/current_tracer"
FILTER=""
echo vfio_dma_do_map > "$TRACEFS/set_ftrace_filter"
echo vfio_pin_pages_remote >> "$TRACEFS/set_ftrace_filter"
echo vfio_pci_read >> "$TRACEFS/set_ftrace_filter"
echo vfio_pci_write >> "$TRACEFS/set_ftrace_filter"
echo vfio_pci_ioctl >> "$TRACEFS/set_ftrace_filter"
echo vfio_pci_core_mmap >> "$TRACEFS/set_ftrace_filter"
echo kvm_vfio_group_add >> "$TRACEFS/set_ftrace_filter"
echo kvm_vfio_group_del >> "$TRACEFS/set_ftrace_filter"

if [ "$ALL_VFIO" = true ]; then
    echo iommu_map >> "$TRACEFS/set_ftrace_filter"
    echo iommu_unmap >> "$TRACEFS/set_ftrace_filter"
    echo intel_iommu_map >> "$TRACEFS/set_ftrace_filter"
fi

if [ -n "$PID" ]; then
    echo "$PID" > "$TRACEFS/set_event_pid" 2>/dev/null || true
fi

# 开始跟踪
echo "开始跟踪..."
echo 1 > "$TRACEFS/tracing_on"
sleep "$DURATION"
echo 0 > "$TRACEFS/tracing_on"
echo "跟踪完成。"
echo ""

# 分析结果
TRACE_DATA=$(cat "$TRACEFS/trace")

TOTAL_LINES=$(echo "$TRACE_DATA" | wc -l)
DMA_MAP=$(echo "$TRACE_DATA" | grep "vfio_dma_do_map" | wc -l)
DMA_PIN=$(echo "$TRACE_DATA" | grep "vfio_pin_pages" | wc -l)
IOMMU_MAP_COUNT=$(echo "$TRACE_DATA" | grep "iommu_map" | wc -l)
IOMMU_UNMAP_COUNT=$(echo "$TRACE_DATA" | grep "iommu_unmap" | wc -l)
PCI_READ=$(echo "$TRACE_DATA" | grep "vfio_pci_read" | wc -l)
PCI_WRITE=$(echo "$TRACE_DATA" | grep "vfio_pci_write" | wc -l)
PCI_MMAP=$(echo "$TRACE_DATA" | grep "vfio_pci.*mmap" | wc -l)
KVM_VFIO_ADD=$(echo "$TRACE_DATA" | grep "kvm_vfio_group_add" | wc -l)

if [ "$SUMMARY_ONLY" = true ]; then
    echo -e "${BLUE}=== VFIO 操作摘要 ===${NC}"
    echo ""
    echo "  DMA 映射操作:    $DMA_MAP"
    echo "  页面固定:        $DMA_PIN"
    echo "  IOMMU 映射:      $IOMMU_MAP_COUNT"
    echo "  IOMMU 解映射:    $IOMMU_UNMAP_COUNT"
    echo "  PCI 配置读:      $PCI_READ"
    echo "  PCI 配置写:      $PCI_WRITE"
    echo "  PCI MMIO 映射:   $PCI_MMAP"
    echo "  KVM-VFIO 添加:   $KVM_VFIO_ADD"
else
    echo -e "${BLUE}=== VFIO 操作详细分析 ===${NC}"
    echo ""

    echo -e "${BLUE}--- 操作统计 ---${NC}"
    echo "  DMA 映射操作:    $DMA_MAP"
    echo "  页面固定:        $DMA_PIN"
    echo "  IOMMU 映射:      $IOMMU_MAP_COUNT"
    echo "  IOMMU 解映射:    $IOMMU_UNMAP_COUNT"
    echo "  PCI 配置读:      $PCI_READ"
    echo "  PCI 配置写:      $PCI_WRITE"
    echo "  PCI MMIO 映射:   $PCI_MMAP"
    echo "  KVM-VFIO 添加:   $KVM_VFIO_ADD"
    echo ""

    # DMA 映射详情
    if [ "$DMA_MAP" -gt 0 ]; then
        echo -e "${BLUE}--- DMA 映射详情 ---${NC}"
        echo "$TRACE_DATA" | grep "vfio_dma_do_map" | head -20
        echo ""
    fi

    # IOMMU 操作详情
    if [ "$IOMMU_MAP_COUNT" -gt 0 ] && [ "$ALL_VFIO" = true ]; then
        echo -e "${BLUE}--- IOMMU 操作详情 ---${NC}"
        echo "$TRACE_DATA" | grep -E "iommu_(map|unmap)" | head -20
        echo ""
    fi

    # PCI 操作详情
    if [ "$PCI_READ" -gt 0 ] || [ "$PCI_WRITE" -gt 0 ]; then
        echo -e "${BLUE}--- PCI 配置空间访问 ---${NC}"
        echo "$TRACE_DATA" | grep -E "vfio_pci_(read|write)" | head -20
        echo ""
    fi

    # 函数调用详情
    echo -e "${BLUE}--- 函数调用序列 (前 50 条) ---${NC}"
    echo "$TRACE_DATA" | grep -E "vfio_|iommu_|kvm_vfio" | head -50
fi

echo ""
echo -e "${GREEN}完成！${NC}"
echo ""
echo "提示:"
echo "  - 使用 lspci -vvv -s <BDF> 查看设备 MSI-X 配置"
echo "  - 查看 IOMMU 域: dmesg | grep -i iommu"
echo "  - 性能分析: perf record -e iommu:* -p $PID -- sleep $DURATION"
