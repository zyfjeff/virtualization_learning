#!/usr/bin/env bash
# ex2-config-space.sh — PCIe 配置空间读写
#
# 目标：
#   1. 使用 setpci 读取 Vendor/Device ID
#   2. 读取 BAR 值并解析类型
#   3. 读取 PCIe Capability
#   4. 对比 Type 0（Endpoint）和 Type 1（Bridge）的差异
#
# 连接 annotations.md：
#   - §4「PCIe 配置访问实现」— pci_read_config_*() 的实现
#   - §5「关键数据结构」— struct pci_dev 中的配置空间字段
#   - basics.md §3 — 配置空间类型（Type 0/1）
#   - basics.md §4 — BAR 的作用和枚举
#
# 用法：
#   sudo ./ex2-config-space.sh [BDF]
#   如果不指定 BDF，则分析系统中第一个 Endpoint 和第一个 Bridge

set -euo pipefail

# ─── 颜色定义 ───────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

divider() {
    echo "════════════════════════════════════════════════════════════════"
}

# ─── 检查工具 ───────────────────────────────────────────
for tool in setpci lspci; do
    if ! command -v "$tool" &>/dev/null; then
        echo -e "${RED}错误: $tool 未安装${NC}"
        echo "安装: apt install pciutils"
        exit 1
    fi
done

# ─── 参数处理 ───────────────────────────────────────────
if [[ $# -ge 1 ]]; then
    TARGET_BDF="$1"
    echo -e "${CYAN}分析指定设备: ${TARGET_BDF}${NC}"
else
    # 自动选择一个 Endpoint 和一个 Bridge
    ENDPOINT_BDF=$(lspci 2>/dev/null | head -1 | awk '{print $1}')
    BRIDGE_BDF=""
    for bdf in $(lspci 2>/dev/null | awk '{print $1}'); do
        ht=$(lspci -v -s "$bdf" 2>/dev/null | grep "Header type" || true)
        if echo "$ht" | grep -q "1"; then
            BRIDGE_BDF="$bdf"
            break
        fi
    done
    TARGET_BDF="$ENDPOINT_BDF"
fi

divider
echo -e "${GREEN}  PCIe 配置空间分析工具${NC}"
echo "  连接 annotations.md §4 + basics.md §3-4"
divider
echo

# ─── 函数：解析配置空间 ────────────────────────────────
analyze_device() {
    local bdf="$1"
    local label="$2"

    echo -e "${CYAN}━━━ ${label}: ${bdf} ━━━${NC}"
    echo

    # 设备描述
    desc=$(lspci -s "$bdf" 2>/dev/null | cut -d' ' -f2-)
    echo -e "  设备: ${GREEN}${desc}${NC}"
    echo

    # ─── Vendor/Device ID ──────────────────────────
    echo -e "  ${YELLOW}[Vendor ID / Device ID]${NC}"
    echo "    对应 basics.md §3.3: PCI_VENDOR_ID @ 0x00 (16b), PCI_DEVICE_ID @ 0x02 (16b)"

    # 读取偏移 0x00 的 32 位值（低 16 位 = Vendor ID, 高 16 位 = Device ID）
    vid_did=$(setpci -s "$bdf" 0.w 2>/dev/null || echo "FAIL")
    vid=$(setpci -s "$bdf" 0.w 2>/dev/null || echo "0000")

    # 注意：setpci 的字节序 — 0.w 读出的是 little-endian 16 位
    # Vendor ID 在低地址 0x00-0x01
    vendor_id=$(setpci -s "$bdf" 0.w)
    device_id=$(setpci -s "$bdf" 2.w)

    echo "    Vendor ID  (0x00): 0x${vendor_id}"
    echo "    Device ID  (0x02): 0x${device_id}"
    echo

    # ─── Command / Status ─────────────────────────
    echo -e "  ${YELLOW}[Command / Status 寄存器]${NC}"
    echo "    对应 basics.md §3.3: PCI_COMMAND @ 0x04, PCI_STATUS @ 0x06"

    command_reg=$(setpci -s "$bdf" 4.w)
    status_reg=$(setpci -s "$bdf" 6.w)

    echo "    Command (0x04): 0x${command_reg}"
    # 解码 Command 位
    cmd_val=$((16#${command_reg}))
    [[ $((cmd_val & 0x01)) -ne 0 ]] && echo "      bit 0: I/O Space — 已启用"
    [[ $((cmd_val & 0x02)) -ne 0 ]] && echo "      bit 1: Memory Space — 已启用"
    [[ $((cmd_val & 0x04)) -ne 0 ]] && echo "      bit 2: Bus Master — 已启用"
    [[ $((cmd_val & 0x08)) -ne 0 ]] && echo "      bit 3: Special Cycles — 已启用"
    [[ $((cmd_val & 0x10)) -ne 0 ]] && echo "      bit 4: Memory Write & Invalidate — 已启用"
    [[ $((cmd_val & 0x20)) -ne 0 ]] && echo "      bit 5: VGA Palette Snoop — 已启用"
    [[ $((cmd_val & 0x40)) -ne 0 ]] && echo "      bit 6: Parity Error Response — 已启用"
    [[ $((cmd_val & 0x80)) -ne 0 ]] && echo "      bit 7: SERR# Enable — 已启用"

    echo "    Status  (0x06): 0x${status_reg}"
    echo

    # ─── Header Type ──────────────────────────────
    echo -e "  ${YELLOW}[Header Type]${NC}"
    ht_raw=$(setpci -s "$bdf" e.b)
    ht_val=$((16#${ht_raw}))
    ht_type=$((ht_val & 0x7f))
    ht_multi=$(( (ht_val >> 7) & 0x01 ))

    echo "    原始值: 0x${ht_raw}"
    echo "    Type: ${ht_type} ($([ $ht_type -eq 0 ] && echo 'Endpoint' || echo 'Bridge'))"
    echo "    多功能: $([ $ht_multi -eq 1 ] && echo '是' || echo '否') (bit 7)"
    echo

    # ─── BAR 分析 ─────────────────────────────────
    if [[ $ht_type -eq 0 ]]; then
        echo -e "  ${YELLOW}[BAR 分析（Type 0 Endpoint）]${NC}"
        echo "    对应 basics.md §4 — BAR 用于设备请求内存/IO 映射"
        echo

        for i in 0 1 2 3 4 5; do
            offset=$(printf "%x" $((0x10 + i * 4)))
            bar_val=$(setpci -s "$bdf" ${offset}.l)
            bar_num=$((16#${bar_val}))

            if [[ $bar_num -eq 0 ]]; then
                echo "    BAR${i} (0x${offset}): 0x${bar_val} — 未使用"
                continue
            fi

            # 解析 BAR 类型
            if [[ $((bar_num & 0x01)) -eq 1 ]]; then
                echo "    BAR${i} (0x${offset}): 0x${bar_val} — IO 空间"
                echo "      IO 基地址: 0x$(printf '%x' $((bar_num & ~0x03)))"
            else
                # Memory BAR
                mem_type=$(( (bar_num >> 1) & 0x03 ))
                prefetchable=$(( (bar_num >> 3) & 0x01 ))
                addr_32=$((bar_num & ~0x0f))

                case $mem_type in
                    0) type_str="32-bit"
                       addr=$(printf '%x' $((addr_32 & 0xFFFFFFFF)))
                       ;;
                    2) type_str="32-bit (below 1MB, legacy)"
                       addr=$(printf '%x' $((addr_32 & 0xFFFFFFFF)))
                       ;;
                    4) type_str="64-bit"
                       # 读取高 32 位
                       offset_hi=$(printf "%x" $((0x10 + (i+1) * 4)))
                       bar_hi=$(setpci -s "$bdf" ${offset_hi}.l)
                       addr=$(printf '%x%08x' $((16#${bar_hi})) $((addr_32 & ~0x0f)))
                       ;;
                    *) type_str="reserved"
                       addr="????????"
                       ;;
                esac

                pf_str=$([ $prefetchable -eq 1 ] && echo "prefetchable" || echo "non-prefetchable")
                echo "    BAR${i} (0x${offset}): 0x${bar_val}"
                echo "      类型: ${type_str} Memory, ${pf_str}"
                echo "      基地址: 0x${addr}"

                # 64-bit BAR 占用两个槽位
                if [[ $mem_type -eq 4 ]]; then
                    i=$((i + 1))
                fi
            fi
        done
        echo
    else
        echo -e "  ${YELLOW}[Bridge 特有寄存器（Type 1）]${NC}"
        echo "    对应 basics.md §3.3: PCI_PRIMARY_BUS/SECONDARY_BUS/SUBORDINATE_BUS"
        echo

        # Primary/Secondary/Subordinate Bus Number
        primary=$(setpci -s "$bdf" 18.b)
        secondary=$(setpci -s "$bdf" 19.b)
        subordinate=$(setpci -s "$bdf" 1a.b)
        sec_limit=$(setpci -s "$bdf" 1b.b)

        echo "    Primary Bus   (0x18): 0x${primary} ($((16#${primary})))"
        echo "    Secondary Bus (0x19): 0x${secondary} ($((16#${secondary})))"
        echo "    Subordinate   (0x1a): 0x${subordinate} ($((16#${subordinate})))"
        echo "    Sec Limit     (0x1b): 0x${sec_limit}"
        echo

        echo "    含义:"
        echo "      上游总线: Bus $((16#${primary}))"
        echo "      下游总线: Bus $((16#${secondary})) - Bus $((16#${subordinate}))"
        echo "      对应 annotations.md §2: Bridge 的 Bus Number 分配"
        echo

        # Bridge 的 Memory Base/Limit
        echo "    Bridge 内存窗口:"
        mem_base=$(setpci -s "$bdf" 20.l)
        mem_limit=$(setpci -s "$bdf" 24.l)
        echo "      Memory Base  (0x20): 0x${mem_base}"
        echo "      Memory Limit (0x24): 0x${mem_limit}"
        echo
    fi

    # ─── PCIe Capability ──────────────────────────
    echo -e "  ${YELLOW}[PCIe Capability]${NC}"
    echo "    对应 basics.md §5.3 — PCIe Capability (ID=0x10) 结构"

    # 找到 PCIe Capability 偏移
    pcie_cap_offset=""
    cap_ptr=$(setpci -s "$bdf" 34.b)
    while [[ $((16#${cap_ptr})) -ne 0 ]]; do
        cap_id=$(setpci -s "$bdf" ${cap_ptr}.b)
        if [[ $((16#${cap_id})) -eq 0x10 ]]; then
            pcie_cap_offset="$cap_ptr"
            break
        fi
        cap_ptr=$(setpci -s "$bdf" $(printf '%x' $((16#${cap_ptr} + 1))).b)
    done

    if [[ -n "$pcie_cap_offset" ]]; then
        echo "    PCIe Capability 偏移: 0x${pcie_cap_offset}"

        # 读取 PCI_EXP_FLAGS (offset + 2)
        flags_offset=$(printf '%x' $((16#${pcie_cap_offset} + 2)))
        flags=$(setpci -s "$bdf" ${flags_offset}.w)
        flags_val=$((16#${flags}))

        version=$((flags_val & 0x000f))
        dev_type=$(( (flags_val >> 4) & 0x0f ))
        slot_impl=$(( (flags_val >> 8) & 0x01 ))
        irq_num=$(( (flags_val >> 9) & 0x1f ))

        echo "    PCI_EXP_FLAGS: 0x${flags}"
        echo "      Version:      ${version}"
        echo "      Device Type:  ${dev_type}"

        # 解码设备类型
        case $dev_type in
            0) echo "                   = Endpoint" ;;
            1) echo "                   = Legacy Endpoint" ;;
            4) echo "                   = Root Port" ;;
            5) echo "                   = Upstream Port" ;;
            6) echo "                   = Downstream Port" ;;
            7) echo "                   = PCIe to PCI/PCI-X Bridge" ;;
            8) echo "                   = PCI/PCI-X to PCIe Bridge" ;;
            9) echo "                   = Root Complex Integrated Endpoint" ;;
            10) echo "                   = Root Complex Event Collector" ;;
            *) echo "                   = Unknown (${dev_type})" ;;
        esac

        echo "      Slot:         $([ $slot_impl -eq 1 ] && echo 'Implemented' || echo 'Not implemented')"
        echo "      IRQ Number:   ${irq_num}"
    else
        echo -e "    ${YELLOW}(未找到 PCIe Capability)${NC}"
    fi
    echo
    divider
    echo
}

# ─── 主流程 ─────────────────────────────────────────────
echo -e "${CYAN}[1] 分析指定设备${NC}"
echo
analyze_device "$TARGET_BDF" "目标设备"

# 如果没指定 BDF，再分析一个 Bridge 做对比
if [[ $# -eq 0 && -n "${BRIDGE_BDF:-}" && "$BRIDGE_BDF" != "$TARGET_BDF" ]]; then
    echo -e "${CYAN}[2] 对比分析：Bridge（Type 1）${NC}"
    echo
    analyze_device "$BRIDGE_BDF" "Bridge"

    echo -e "${CYAN}[3] Type 0 vs Type 1 对比总结${NC}"
    echo "    对应 basics.md §3.1 — 配置空间类型"
    echo
    echo "    ┌─────────────────┬────────────────────┬────────────────────┐"
    echo "    │ 特性            │ Type 0 (Endpoint)  │ Type 1 (Bridge)    │"
    echo "    ├─────────────────┼────────────────────┼────────────────────┤"
    echo "    │ BAR 数量        │ 6 个               │ 2 个 (Memory/IO)   │"
    echo "    │ Header 大小     │ 64 字节            │ 64 字节            │"
    echo "    │ 特有寄存器      │ Subsystem ID       │ Primary/Sec/Subord │"
    echo "    │ 配置空间大小    │ 256B (legacy)      │ 256B (legacy)      │"
    echo "    │                 │ 4KB  (PCIe)        │ 4KB  (PCIe)        │"
    echo "    └─────────────────┴────────────────────┴────────────────────┘"
    echo
fi

# ─── 思考题 ─────────────────────────────────────────────
echo -e "${CYAN}思考题${NC}"
echo
echo "  1. 观察 Endpoint 的 BAR，哪些是 32-bit、哪些是 64-bit？"
echo "     64-bit BAR 占用几个 BAR 槽位？"
echo "     → 对应 basics.md §4.2: 64-bit BAR 占用两个槽位"
echo
echo "  2. Bridge 的 Secondary Bus 和 Subordinate Bus 是否相同？"
echo "     如果不同，说明 Bridge 下游还有其他 Bridge"
echo "     → 对应 annotations.md §2: Subordinate = 最远的下游 Bus"
echo
echo "  3. PCIe Capability 的 Device Type 字段与 lspci -vv 的输出一致吗？"
echo "     → 对应 basics.md §5.3: PCI_EXP_FLAGS_TYPE 的编码"
echo
echo "  4. Command 寄存器的 Bus Master 位（bit 2）是否启用？"
echo "     如果未启用，设备不能发起 DMA 读写"
echo "     → 对应 basics.md §4.1: BAR 映射与 DMA 的关系"
echo

divider
echo -e "${GREEN}完成！${NC} 连接下一个练习: ./ex3-acs-analysis.sh"
divider
