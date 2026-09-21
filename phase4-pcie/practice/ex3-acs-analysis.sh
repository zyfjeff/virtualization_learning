#!/usr/bin/env bash
# ex3-acs-analysis.sh — ACS（Access Control Services）能力分析
#
# 目标：
#   1. 检查哪些设备支持 ACS
#   2. 分析 ACS Control 寄存器中哪些位已启用
#   3. 分析 ACS 对 IOMMU group 的影响
#   4. 对比有/无 ACS 设备的 group 划分
#
# 连接 annotations.md：
#   - §3「ACS 检查」— pci_acs_flags_enabled() 和 pci_acs_enabled() 的实现
#   - §3「IOMMU group」— iommu_group_get_for_pci_dev() 的算法
#   - basics.md §5 — PCIe 能力链表
#   - CLAUDE.md 已知陷阱 #11 — ACS 判定的 quirk 层
#
# 用法：
#   sudo ./ex3-acs-analysis.sh

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

# ─── ACS 标志定义 ──────────────────────────────────────
# 对应 annotations.md §3 中的 ACS 标志
# 来源: include/uapi/linux/pci_regs.h
declare -A ACS_FLAGS=(
    [SV]="0x0001"   # Source Validation
    [TB]="0x0002"   # Translation Blocking
    [RR]="0x0004"   # P2P Request Redirect
    [CR]="0x0008"   # P2P Completion Redirect
    [UF]="0x0010"   # Upstream Forwarding
    [EC]="0x0020"   # P2P Egress Control
    [DT]="0x0040"   # Direct Translated P2P
)

# REQ_ACS_FLAGS — 内核用于 IOMMU group 判定的标志
# 对应 CLAUDE.md 陷阱 #11: "REQ_ACS_FLAGS 只含 SV/RR/CR/UF"
REQ_ACS_FLAGS=$((0x0001 | 0x0004 | 0x0008 | 0x0010))  # SV + RR + CR + UF

# ─── 检查权限 ───────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}错误: 此脚本需要 root 权限读取 ACS 扩展能力${NC}"
    echo "用法: sudo $0"
    exit 1
fi

divider
echo -e "${GREEN}  ACS（Access Control Services）分析工具${NC}"
echo "  连接 annotations.md §3 + CLAUDE.md 陷阱 #11-13"
divider
echo

# ─── 1. ACS Capability 检查 ────────────────────────────
echo -e "${CYAN}[1] ACS Capability 扫描${NC}"
echo "    对应 annotations.md §3: pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_ACS)"
echo

acs_count=0
declare -A acs_devices

for bdf in $(lspci 2>/dev/null | awk '{print $1}'); do
    # ACS 是 Extended Capability (ID = 0x000D)
    # Extended Capability 从配置空间 0x100 开始
    # 检查 Extended Capability Header
    has_acs=0
    ecap_offset=0x100

    while [[ $ecap_offset -lt 0x1000 ]]; do
        ecap_hdr=$(setpci -s "$bdf" ${ecap_offset}.l 2>/dev/null || echo "00000000")
        ecap_val=$((16#${ecap_hdr}))

        # Extended Capability ID = bits 15:0
        ecap_id=$((ecap_val & 0xFFFF))
        # Next offset = bits 31:20
        ecap_next=$(( (ecap_val >> 20) & 0xFFF ))

        if [[ $ecap_id -eq 0x000D ]]; then
            has_acs=1
            acs_count=$((acs_count + 1))
            acs_devices[$bdf]=$ecap_offset
            break
        fi

        if [[ $ecap_next -eq 0 ]]; then
            break
        fi
        ecap_offset=$ecap_next
    done
done

echo -e "  找到 ${GREEN}${acs_count}${NC} 个设备支持 ACS"
echo

if [[ $acs_count -eq 0 ]]; then
    echo -e "  ${YELLOW}没有设备支持 ACS${NC}"
    echo "  这意味着所有连接在同一 Root Port 下的设备"
    echo "  将被划分到同一个 IOMMU group"
    echo
    echo "  → 对应 CLAUDE.md 陷阱 #11: Intel RCiEP 有 quirk 判定"
    echo "    pci_dev_specific_acs_enabled() 可能通过 quirk 返回隔离成立"
    echo
fi

# ─── 2. ACS Control 寄存器解码 ─────────────────────────
echo -e "${CYAN}[2] ACS Control 寄存器详细解码${NC}"
echo "    对应 annotations.md §3: ACS 标志含义"
echo

for bdf in "${!acs_devices[@]}"; do
    ecap_off="${acs_devices[$bdf]}"
    desc=$(lspci -s "$bdf" 2>/dev/null | cut -d' ' -f2-)

    echo -e "  ${GREEN}${bdf}${NC} — ${desc}"
    echo "    ACS Extended Capability 偏移: 0x${ecap_off}"

    # ACS Capability 结构 (PCIe Spec Section 7.16):
    # Offset 0: Extended Capability Header (32b)
    # Offset 4: ACS Capability Register (16b)
    # Offset 6: ACS Control Register (16b)
    acs_cap_off=$(printf '%x' $((16#${ecap_off} + 4)))
    acs_ctl_off=$(printf '%x' $((16#${ecap_off} + 6)))

    acs_cap=$(setpci -s "$bdf" ${acs_cap_off}.w)
    acs_ctl=$(setpci -s "$bdf" ${acs_ctl_off}.w)
    acs_ctl_val=$((16#${acs_ctl}))

    echo "    ACS Capability (0x${acs_cap_off}): 0x${acs_cap}"
    echo "    ACS Control    (0x${acs_ctl_off}): 0x${acs_ctl}"

    # 解码 ACS Control 位
    echo "    ACS Control 位:"
    all_supported=true
    for flag in SV TB RR CR UF EC DT; do
        flag_val=$((16#${ACS_FLAGS[$flag]}))
        supported="否"
        enabled="否"

        # Capability 中对应位 = 1 表示支持该功能
        # ACS Capability 的 bits 15:1 对应各功能的支持
        acs_cap_val=$((16#${acs_cap}))
        if [[ $((acs_cap_val & flag_val)) -ne 0 ]]; then
            supported="是"
        fi

        # Control 中对应位 = 1 表示启用
        if [[ $((acs_ctl_val & flag_val)) -ne 0 ]]; then
            enabled="是"
        fi

        # 判断是否是 REQ_ACS_FLAGS 的一部分
        is_req="否"
        if [[ $((flag_val & REQ_ACS_FLAGS)) -ne 0 ]]; then
            is_req="是"
        fi

        # 颜色标记
        if [[ "$supported" == "是" && "$enabled" == "是" ]]; then
            color="${GREEN}"
        elif [[ "$supported" == "是" && "$enabled" == "否" ]]; then
            color="${YELLOW}"
        else
            color="${NC}"
        fi

        printf "      %s%-4s${NC} (0x%04x): 支持=%-3s 启用=%-3s REQ_ACS=%-3s\n" \
            "$color" "$flag" "$flag_val" "$supported" "$enabled" "$is_req"

        # 检查 CLAUDE.md 陷阱 #11 的条件
        # "Cap 里有、Ctl 里没开" = 隔离不通过
        if [[ "$supported" == "是" && "$enabled" == "否" && "$is_req" == "是" ]]; then
            echo -e "        ${RED}⚠ 该位在 Cap 中声明但 Ctl 中未启用 → 隔离判定不通过${NC}"
            echo -e "        ${RED}  对应 CLAUDE.md 陷阱 #11: '只有 Cap 里有、Ctl 里没开才算不通过'${NC}"
        fi
    done
    echo
done

# ─── 3. ACS 对 IOMMU Group 的影响 ─────────────────────
echo -e "${CYAN}[3] ACS 对 IOMMU Group 的影响分析${NC}"
echo "    对应 annotations.md §3: iommu_group_get_for_pci_dev() 算法"
echo "    对应 CLAUDE.md 陷阱 #11: quirk 层先于 PCIe 类型判断"
echo

# 列出 IOMMU groups
if [[ -d /sys/kernel/iommu_groups ]]; then
    echo "  当前系统的 IOMMU Group:"
    echo

    for group_dir in /sys/kernel/iommu_groups/*/; do
        group_num=$(basename "$group_dir")
        devices=()

        for dev_link in "$group_dir"devices/*; do
            [[ -e "$dev_link" ]] || continue
            dev_bdf=$(basename "$dev_link")
            desc=$(lspci -s "$dev_bdf" 2>/dev/null | cut -d' ' -f2- || echo "(unknown)")
            devices+=("${dev_bdf}: ${desc}")
        done

        if [[ ${#devices[@]} -gt 0 ]]; then
            echo -e "  ${GREEN}IOMMU Group ${group_num}${NC} (${#devices[@]} 个设备):"
            for dev in "${devices[@]}"; do
                echo "    ${dev}"
            done

            # 分析 group 内的设备是否在同一 Bridge 下
            if [[ ${#devices[@]} -gt 1 ]]; then
                echo -e "    ${YELLOW}→ 多个设备在同一 group: 说明它们之间的 Bridge 没有 ACS 隔离${NC}"
                echo -e "    ${YELLOW}  对应 CLAUDE.md 陷阱 #11: REQ_ACS_FLAGS = SV|RR|CR|UF${NC}"
            fi
            echo
        fi
    done
else
    echo -e "  ${YELLOW}IOMMU 未启用（/sys/kernel/iommu_groups 不存在）${NC}"
    echo "  要启用 IOMMU，在 grub 中添加:"
    echo "    Intel VT-d: intel_iommu=on"
    echo "    AMD-Vi:     amd_iommu=on"
    echo
fi

# ─── 4. Intel RCiEP Quirk 分析 ─────────────────────────
echo -e "${CYAN}[4] Intel RCiEP Quirk 分析${NC}"
echo "    对应 CLAUDE.md 陷阱 #11: pci_quirk_rciep_acs 把所有 Intel RCiEP 判成隔离成立"
echo "    对应 annotations.md §3: pci_acs_enabled() 开头先调 pci_dev_specific_acs_enabled()"
echo

echo "  Root Complex Integrated Endpoint (RCiEP) 设备:"
found_rciep=0
for bdf in $(lspci 2>/dev/null | awk '{print $1}'); do
    port_type=$(lspci -vv -s "$bdf" 2>/dev/null | grep "Port type" || true)
    if echo "$port_type" | grep -qi "root complex integrated endpoint"; then
        desc=$(lspci -s "$bdf" 2>/dev/null | cut -d' ' -f2-)
        vendor=$(setpci -s "$bdf" 0.w)
        echo -e "  ${GREEN}${bdf}${NC} — ${desc}"
        echo "    Vendor ID: 0x${vendor}"
        if [[ "${vendor,,}" == "8086" ]]; then
            echo -e "    ${YELLOW}→ Intel 设备: 命中 pci_quirk_rciep_acs quirk${NC}"
            echo -e "    ${YELLOW}  即使没有 ACS capability，隔离也被判定为成立${NC}"
        fi
        echo "    对应源码: drivers/pci/quirks.c: pci_quirk_rciep_acs()"
        found_rciep=1
    fi
done
if [[ $found_rciep -eq 0 ]]; then
    echo -e "  ${YELLOW}(未找到 RCiEP 设备)${NC}"
fi
echo

# ─── 5. 思考题 ─────────────────────────────────────────
echo -e "${CYAN}[5] 思考题${NC}"
echo
echo "  1. 系统中有哪些设备支持 ACS？它们是什么类型的设备？"
echo "     → 对应 annotations.md §3: ACS 通常在 Downstream Port / Root Port 上"
echo
echo "  2. 对于支持 ACS 的设备，ACS Control 中的 SV/RR/CR/UF 是否全部启用？"
echo "     → 对应 CLAUDE.md 陷阱 #11: REQ_ACS_FLAGS = SV|RR|CR|UF"
echo "       只有这四个位全部启用，设备才算通过隔离检查"
echo
echo "  3. 有没有 Intel RCiEP 设备？它们是否命中了 quirk？"
echo "     → 对应 CLAUDE.md 陷阱 #11: pci_quirk_rciep_acs 把所有 Intel RCiEP"
echo "       判成隔离成立，即使它们没有 ACS capability"
echo
echo "  4. 观察 IOMMU group 的划分，是否有多设备共享一个 group 的情况？"
echo "     这些设备之间的 Bridge 是否缺少 ACS 隔离？"
echo "     → 对应 annotations.md §3: iommu_group_get_for_pci_dev() 的算法"
echo
echo "  5. （进阶）如果需要用 setpci 手动启用 ACS，需要写哪些位？"
echo "     对应 CLAUDE.md 陷阱 #13: pci=disable_acs_redir= 和 pci=config_acs="
echo "     注意 config_acs 的位串是从右往左解析的！"
echo

divider
echo -e "${GREEN}完成！${NC} 连接下一个练习: ./ex4-iommu-group.sh"
divider
