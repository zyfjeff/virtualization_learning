#!/bin/bash
#
# iommu-analysis.sh - IOMMU 性能分析工具
#
# 用法: sudo ./iommu-analysis.sh [选项]
#   -p PID     跟踪指定的 QEMU 进程 PID
#   -d SECS    分析持续时间（默认 30 秒）
#   -g GROUP   分析指定的 IOMMU 组号
#   -s         只显示摘要
#   -h         显示帮助
#
# 功能:
#   - IOMMU 域信息收集
#   - DMA 映射/解映射统计
#   - IOTLB miss 分析
#   - 中断重映射状态
#   - 性能瓶颈定位
#

set -euo pipefail

PID=""
DURATION=30
IOMMU_GROUP=""
SUMMARY_ONLY=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "IOMMU 性能分析工具"
    echo ""
    echo "选项:"
    echo "  -p PID     跟踪指定的 QEMU 进程 PID"
    echo "  -d SECS    分析持续时间（默认 30 秒）"
    echo "  -g GROUP   分析指定的 IOMMU 组号"
    echo "  -s         只显示摘要"
    echo "  -h         显示帮助"
    exit 0
}

while getopts "p:d:g:sh" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        g) IOMMU_GROUP="$OPTARG" ;;
        s) SUMMARY_ONLY=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    exit 1
fi

if [ -z "$PID" ]; then
    PID=$(pgrep -f "qemu-system" | head -1 || echo "")
    if [ -n "$PID" ]; then
        echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
    fi
fi

echo -e "${BLUE}${BOLD}=========================================${NC}"
echo -e "${BLUE}${BOLD}  IOMMU 性能分析${NC}"
echo -e "${BLUE}${BOLD}=========================================${NC}"
echo ""
echo "分析参数:"
echo "  PID:      ${PID:-所有进程}"
echo "  持续时间: ${DURATION} 秒"
echo "  IOMMU 组: ${IOMMU_GROUP:-全部}"
echo ""

# ==========================================
# Part 1: IOMMU 硬件信息
# ==========================================
echo -e "${CYAN}${BOLD}1. IOMMU 硬件信息${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

# 检查 IOMMU 类型
echo "  IOMMU 类型检测:"
if dmesg | grep -qi "DMAR"; then
    echo -e "    ${GREEN}✓ Intel VT-d${NC} 已启用"
    IOMMU_TYPE="intel"
elif dmesg | grep -qi "AMD-Vi"; then
    echo -e "    ${GREEN}✓ AMD-Vi${NC} 已启用"
    IOMMU_TYPE="amd"
else
    echo -e "    ${YELLOW}△ IOMMU 可能未启用${NC}"
    IOMMU_TYPE="unknown"
fi
echo ""

# IOMMU 启用参数
echo "  内核命令行 IOMMU 参数:"
CMDLINE=$(cat /proc/cmdline)
if echo "$CMDLINE" | grep -q "intel_iommu=on"; then
    echo "    intel_iommu=on ✓"
elif echo "$CMDLINE" | grep -q "amd_iommu=on"; then
    echo "    amd_iommu=on ✓"
else
    echo -e "    ${YELLOW}未找到 IOMMU 启用参数${NC}"
fi

if echo "$CMDLINE" | grep -q "iommu=pt"; then
    echo "    iommu=pt ✓ (直通模式)"
fi
echo ""

# DMAR/IVRS 表
echo "  ACPI 表信息:"
if [ -f "/sys/firmware/acpi/tables/DMAR" ]; then
    echo "    DMAR 表存在 ($(stat -c%s /sys/firmware/acpi/tables/DMAR 2>/dev/null || echo '?') bytes)"
elif [ -f "/sys/firmware/acpi/tables/IVRS" ]; then
    echo "    IVRS 表存在 ($(stat -c%s /sys/firmware/acpi/tables/IVRS 2>/dev/null || echo '?') bytes)"
else
    echo "    未找到 DMAR/IVRS 表"
fi
echo ""

# ==========================================
# Part 2: IOMMU 组拓扑
# ==========================================
echo -e "${CYAN}${BOLD}2. IOMMU 组拓扑${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

if [ -n "$IOMMU_GROUP" ]; then
    # 分析指定组
    grp_path="/sys/kernel/iommu_groups/$IOMMU_GROUP"
    if [ -d "$grp_path" ]; then
        echo "  Group $IOMMU_GROUP 设备:"
        for dev in "$grp_path"/devices/*; do
            if [ -d "$dev" ]; then
                bdf=$(basename "$dev")
                desc=$(lspci -s "$bdf" 2>/dev/null | sed "s/^$bdf //")
                driver=$(readlink "$dev/driver" 2>/dev/null | xargs basename 2>/dev/null || echo "none")
                echo "    $bdf  [$driver]  $desc"
            fi
        done
    else
        echo -e "  ${RED}IOMMU 组 $IOMMU_GROUP 不存在${NC}"
    fi
else
    # 显示所有组
    echo "  IOMMU 组列表:"
    for grp in /sys/kernel/iommu_groups/*/; do
        if [ -d "$grp" ]; then
            grp_id=$(basename "$grp")
            devices=$(ls "$grp/devices/" 2>/dev/null)
            if [ -n "$devices" ]; then
                dev_count=$(echo "$devices" | wc -w)
                dev_list=$(echo "$devices" | tr '\n' ' ' | sed 's/ $//')

                # 检查是否有 vfio-pci 绑定的设备
                has_vfio=""
                for dev in $devices; do
                    drv=$(readlink "/sys/bus/pci/devices/$dev/driver" 2>/dev/null | xargs basename 2>/dev/null || echo "")
                    if [ "$drv" = "vfio-pci" ]; then
                        has_vfio="← VFIO"
                    fi
                done

                echo "    Group $grp_id ($dev_count 设备): $dev_list $has_vfio"
            fi
        fi
    done
fi
echo ""

# ==========================================
# Part 3: IOMMU 域信息
# ==========================================
echo -e "${CYAN}${BOLD}3. IOMMU 域信息${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

# Intel IOMMU 域
if [ -d "/sys/kernel/debug/iommu/intel" ]; then
    echo "  Intel IOMMU 域:"
    for iommu_dir in /sys/kernel/debug/iommu/intel/*/; do
        if [ -d "$iommu_dir" ]; then
            iommu_id=$(basename "$iommu_dir")
            echo "    IOMMU $iommu_id:"

            # 域信息
            if [ -f "$iommu_dir/domains" ]; then
                domains=$(cat "$iommu_dir/domains" 2>/dev/null)
                echo "      域: $domains"
            fi
        fi
    done
elif [ -d "/sys/kernel/debug/iommu/amd" ]; then
    echo "  AMD IOMMU 域:"
    for iommu_dir in /sys/kernel/debug/iommu/amd/*/; do
        if [ -d "$iommu_dir" ]; then
            echo "    IOMMU $(basename "$iommu_dir"):"
        fi
    done
else
    echo "  无法读取 IOMMU 域信息 (需要 debugfs)"
fi
echo ""

# ==========================================
# Part 3.5: 组域类型可见性（phase11 实验 1）
# ==========================================
echo -e "${CYAN}${BOLD}3.5 组域类型可见性${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

# 命令行诉求
REQ_TYPE="translated(DMA)"
if echo "$CMDLINE" | grep -qE "iommu=pt|iommu\.passthrough=1"; then
    REQ_TYPE="identity(passthrough)"
elif echo "$CMDLINE" | grep -q "iommu.strict=0"; then
    REQ_TYPE="DMA-FQ(lazy)"
fi
echo "  命令行诉求: $REQ_TYPE"
echo ""

# 每组实际类型与诉求对照
if [ -d /sys/kernel/iommu_groups ] && ls /sys/kernel/iommu_groups/*/type >/dev/null 2>&1; then
    echo "  组实际类型 (sysfs):"
    mismatch=0
    for type_file in /sys/kernel/iommu_groups/*/type; do
        grp_id=$(basename "$(dirname "$type_file")")
        actual=$(cat "$type_file" 2>/dev/null || echo "?")
        flag=""
        case "$REQ_TYPE" in
            identity*) [ "$actual" != "identity" ] && flag=" ← 与诉求不一致" ;;
            DMA-FQ*)   [ "$actual" != "DMA-FQ" ] && flag=" ← 与诉求不一致" ;;
        esac
        if [ -n "$flag" ]; then
            mismatch=$((mismatch + 1))
            echo -e "    Group $grp_id: $actual${YELLOW}$flag${NC}"
        else
            echo "    Group $grp_id: $actual"
        fi
    done
    echo ""
    if [ "$mismatch" -gt 0 ]; then
        echo -e "  ${YELLOW}有 $mismatch 个组的实际类型与命令行诉求不一致${NC}"
        echo "  可能原因: FQ 初始化失败降级 (dma-iommu.c:721-724)"
        echo "            或分配回落 (iommu.c:1637-1639)，查 dmesg 'Falling back'"
    fi
else
    echo "  /sys/kernel/iommu_groups/*/type 不可用 (IOMMU 未启用?)"
fi
echo ""

# ==========================================
# Part 4: DMA 映射分析 (ftrace)
# ==========================================
echo -e "${CYAN}${BOLD}4. DMA 映射性能分析${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"

TRACEFS=""
for tfs in /sys/kernel/debug/tracing /sys/kernel/tracing; do
    if [ -d "$tfs" ]; then
        TRACEFS="$tfs"
        break
    fi
done

if [ -n "$TRACEFS" ]; then
    echo ""
    echo "  收集 DMA 映射数据 (${DURATION}秒)..."

    echo > "$TRACEFS/trace"
    echo 0 > "$TRACEFS/tracing_on"
    echo > "$TRACEFS/set_event"
    echo nop > "$TRACEFS/current_tracer"

    # 设置 IOMMU 和 VFIO 相关事件
    echo iommu_map >> "$TRACEFS/set_event" 2>/dev/null || true
    echo iommu_unmap >> "$TRACEFS/set_event" 2>/dev/null || true
    echo iommu_map_page >> "$TRACEFS/set_event" 2>/dev/null || true

    # 函数跟踪
    echo function > "$TRACEFS/current_tracer"
    echo vfio_dma_do_map > "$TRACEFS/set_ftrace_filter"
    echo vfio_pin_pages_remote >> "$TRACEFS/set_ftrace_filter"
    echo iommu_map >> "$TRACEFS/set_ftrace_filter"
    echo iommu_unmap >> "$TRACEFS/set_ftrace_filter"

    if [ -n "$PID" ]; then
        echo "$PID" > "$TRACEFS/set_event_pid" 2>/dev/null || true
    fi

    echo 1 > "$TRACEFS/tracing_on"
    sleep "$DURATION"
    echo 0 > "$TRACEFS/tracing_on"

    TRACE_DATA=$(cat "$TRACEFS/trace")

    IOMMU_MAP_COUNT=$(echo "$TRACE_DATA" | grep "iommu_map" | grep -v "unmap" | wc -l)
    IOMMU_UNMAP_COUNT=$(echo "$TRACE_DATA" | grep "iommu_unmap" | wc -l)
    DMA_MAP_COUNT=$(echo "$TRACE_DATA" | grep "vfio_dma_do_map" | wc -l)
    DMA_PIN_COUNT=$(echo "$TRACE_DATA" | grep "vfio_pin_pages" | wc -l)

    echo ""
    echo "  DMA 操作统计 (${DURATION}秒):"
    echo "  ────────────────────────────────────"
    printf "  %-25s %d\n" "DMA 映射请求:" "$DMA_MAP_COUNT"
    printf "  %-25s %d\n" "页面固定:" "$DMA_PIN_COUNT"
    printf "  %-25s %d\n" "IOMMU 页表映射:" "$IOMMU_MAP_COUNT"
    printf "  %-25s %d\n" "IOMMU 页表解映射:" "$IOMMU_UNMAP_COUNT"

    if [ "$DMA_MAP_COUNT" -gt 0 ]; then
        RATE=$(echo "scale=1; $DMA_MAP_COUNT / $DURATION" | bc)
        echo "  DMA 映射率: ${RATE}/秒"
    fi

    # 函数耗时分析
    echo ""
    echo "  函数调用耗时分析:"
    echo "  ────────────────────────────────────"

    # 分析 dma_do_map 调用延迟
    if echo "$TRACE_DATA" | grep -q "vfio_dma_do_map"; then
        echo "  vfio_dma_do_map 调用 (前 10):"
        echo "$TRACE_DATA" | grep "vfio_dma_do_map" | head -10
    fi
else
    echo -e "  ${YELLOW}tracefs 不可用，跳过 DMA 分析${NC}"
fi
echo ""

# ==========================================
# Part 4.5: strict/lazy 与失效延迟（phase11 实验 3）
# ==========================================
echo -e "${CYAN}${BOLD}4.5 strict/lazy 与失效延迟${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

if echo "$CMDLINE" | grep -q "iommu.strict=0"; then
    echo "  失效模式: ${YELLOW}lazy (iommu.strict=0, DMA_FQ 攒批全域失效)${NC}"
elif echo "$CMDLINE" | grep -q "iommu.strict=1"; then
    echo "  失效模式: strict (iommu.strict=1, 逐次同步失效)"
else
    echo "  失效模式: 未显式指定 (取各后端默认)"
fi
echo "  在线切换: echo DMA-FQ > /sys/kernel/iommu_groups/<N>/type"
echo "            (DMA→DMA-FQ 唯一可不解绑驱动, iommu.c:3071-3074)"
echo ""

DMAR_LAT=/sys/kernel/debug/iommu/intel/dmar_perf_latency
if [ -f "$DMAR_LAT" ]; then
    enabled=$(cat "$DMAR_LAT" 2>/dev/null | head -1 || true)
    echo "  dmar_perf_latency 直方图 (首行开关状态: ${enabled:-?}):"
    cat "$DMAR_LAT" 2>/dev/null | while read -r line; do
        echo "    $line"
    done
    echo "  开关: echo 1 > $DMAR_LAT  (需 CONFIG_INTEL_IOMMU_DEBUGFS)"
else
    echo "  dmar_perf_latency 不可用 (非 Intel 或未开 CONFIG_INTEL_IOMMU_DEBUGFS)"
fi
echo ""

# ==========================================
# Part 5: 中断重映射状态
# ==========================================
echo -e "${CYAN}${BOLD}5. 中断重映射状态${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

echo "  IRQ Remapping 状态:"
if dmesg | grep -qi "IRQ remapping"; then
    echo -e "    ${GREEN}✓ 中断重映射已启用${NC}"

    # 检查具体状态
    dmesg | grep -i "remapping" | head -5 | while read -r line; do
        echo "    $line"
    done
else
    echo -e "    ${YELLOW}中断重映射可能未启用${NC}"
fi
echo ""

echo "  中断路由表:"
if [ -f "/sys/kernel/debug/kvm" ]; then
    for vm_dir in /sys/kernel/debug/kvm/*/; do
        if [ -d "$vm_dir" ] && [ -f "$vm_dir/irq_routing" ]; then
            echo "    VM $(basename "$vm_dir"):"
            head -10 "$vm_dir/irq_routing" 2>/dev/null | while read -r line; do
                echo "      $line"
            done
        fi
    done 2>/dev/null || true
fi
echo ""

# ==========================================
# Part 6: 性能建议
# ==========================================
echo -e "${CYAN}${BOLD}6. 性能建议${NC}"
echo -e "${CYAN}─────────────────────────────────────────${NC}"
echo ""

echo "  IOMMU 优化检查:"

# 检查是否启用了 pass-through 模式
if echo "$CMDLINE" | grep -q "iommu=pt"; then
    echo -e "    ${GREEN}✓${NC} IOMMU pass-through 模式已启用"
else
    echo -e "    ${YELLOW}△${NC} 建议启用 iommu=pt 减少非直通设备的 IOMMU 开销"
    echo "       在内核命令行添加: iommu=pt"
fi

# 检查 ACS
echo ""
echo "  ACS (Access Control Services) 检查:"
for pcie_dev in /sys/bus/pci/devices/*/; do
    if [ -d "$pcie_dev" ]; then
        bdf=$(basename "$pcie_dev")
        if [ -f "$pcie_dev/acs_ctl" ]; then
            acs_val=$(cat "$pcie_dev/acs_ctl" 2>/dev/null)
            if [ -n "$acs_val" ]; then
                echo "    $bdf: ACS=$acs_val"
            fi
        fi
    fi
done 2>/dev/null | head -10 || echo "    无法检查 ACS 状态"
echo ""

# 大页支持
echo "  大页支持:"
if [ -d "/sys/kernel/mm/hugepages" ]; then
    for hp in /sys/kernel/mm/hugepages/*/; do
        if [ -d "$hp" ]; then
            size=$(basename "$hp")
            free=$(cat "$hp/free_hugepages" 2>/dev/null || echo 0)
            total=$(cat "$hp/nr_hugepages" 2>/dev/null || echo 0)
            echo "    $size: $free/$total 可用"
        fi
    done
else
    echo "    大页不可用"
fi
echo ""

echo -e "${GREEN}${BOLD}分析完成！${NC}"
echo ""
echo "常见优化措施:"
echo "  1. 启用 IOMMU pass-through: iommu=pt intel_iommu=on"
echo "  2. 使用大页内存减少 IOMMU 页表遍历"
echo "  3. 确保 ACS 启用以获得更细粒度的 IOMMU 组"
echo "  4. 启用 Posted Interrupts 减少中断重映射开销"
echo "  5. 使用预映射减少运行时 DMA 映射延迟"
