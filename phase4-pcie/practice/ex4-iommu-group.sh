#!/usr/bin/env bash
# ex4-iommu-group.sh — IOMMU Group 分析
#
# 目标：
#   1. 列出所有 IOMMU group
#   2. 分析每个 group 包含的设备
#   3. 解释 group 划分的原因（ACS/拓扑）
#   4. 诊断为什么某些设备必须一起直通
#
# 连接 annotations.md：
#   - §3「IOMMU group」— iommu_group_get_for_pci_dev() 的算法
#   - §2「Bridge 与 Bus Number 分配」— 拓扑层次
#   - CLAUDE.md 陷阱 #11 — ACS 判定的 quirk 层
#
# 用法：
#   sudo ./ex4-iommu-group.sh

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

# ─── 检查 IOMMU 是否启用 ───────────────────────────────
if [[ ! -d /sys/kernel/iommu_groups ]]; then
    echo -e "${RED}错误: IOMMU group 目录不存在${NC}"
    echo
    echo "IOMMU 可能未启用。在 grub 中添加以下参数："
    echo "  Intel VT-d: intel_iommu=on iommu=pt"
    echo "  AMD-Vi:     amd_iommu=on iommu=pt"
    echo
    echo "也可以检查 dmesg:"
    echo "  dmesg | grep -i iommu"
    exit 1
fi

divider
echo -e "${GREEN}  IOMMU Group 分析工具${NC}"
echo "  连接 annotations.md §3 + CLAUDE.md 陷阱 #11"
divider
echo

# ─── 1. 列出所有 IOMMU Group ──────────────────────────
echo -e "${CYAN}[1] IOMMU Group 列表${NC}"
echo "    对应 annotations.md §3: iommu_group_get_for_pci_dev()"
echo

# 统计 group 数量
total_groups=$(ls -d /sys/kernel/iommu_groups/*/ 2>/dev/null | wc -l)
total_devices=0

echo -e "  系统共有 ${GREEN}${total_groups}${NC} 个 IOMMU group"
echo

# ─── 2. 详细分析每个 Group ────────────────────────────
echo -e "${CYAN}[2] 详细分析${NC}"
echo

single_dev_groups=0
multi_dev_groups=0

for group_dir in $(ls -d /sys/kernel/iommu_groups/*/ 2>/dev/null | sort -t/ -k5 -n); do
    group_num=$(basename "$group_dir")
    devices=()

    for dev_link in "$group_dir"devices/*; do
        [[ -e "$dev_link" ]] || continue
        dev_bdf=$(basename "$dev_link")
        desc=$(lspci -s "$dev_bdf" 2>/dev/null | cut -d' ' -f2- || echo "(unknown)")
        class=$(lspci -v -s "$dev_bdf" 2>/dev/null | grep "Class:" | head -1 || echo "")
        devices+=("$dev_bdf|$desc|$class")
        total_devices=$((total_devices + 1))
    done

    dev_count=${#devices[@]}

    if [[ $dev_count -eq 1 ]]; then
        single_dev_groups=$((single_dev_groups + 1))
        group_color="${GREEN}"
    else
        multi_dev_groups=$((multi_dev_groups + 1))
        group_color="${YELLOW}"
    fi

    echo -e "  ${group_color}Group ${group_num}${NC} (${dev_count} 个设备)"

    for dev_entry in "${devices[@]}"; do
        IFS='|' read -r bdf desc class <<< "$dev_entry"
        echo "    ${bdf} — ${desc}"

        # 获取设备所在的 Bridge（父设备）
        dev_path="/sys/kernel/iommu_groups/${group_num}/devices/${bdf}"
        if [[ -L "$dev_path" ]]; then
            real_path=$(readlink -f "$dev_path")
            parent_dir=$(dirname "$real_path")
            parent_bdf=$(basename "$parent_dir")
            if [[ "$parent_bdf" != "pci0000:00" && "$parent_bdf" != "0000:00" ]]; then
                parent_desc=$(lspci -s "$parent_bdf" 2>/dev/null | cut -d' ' -f2- || echo "(bridge)")
                echo "      └─ 父 Bridge: ${parent_bdf} — ${parent_desc}"

                # 检查父 Bridge 是否有 ACS
                has_acs=0
                ecap_offset=0x100
                while [[ $ecap_offset -lt 0x1000 ]]; do
                    ecap_hdr=$(setpci -s "$parent_bdf" ${ecap_offset}.l 2>/dev/null || echo "00000000")
                    ecap_val=$((16#${ecap_hdr}))
                    ecap_id=$((ecap_val & 0xFFFF))
                    ecap_next=$(( (ecap_val >> 20) & 0xFFF ))

                    if [[ $ecap_id -eq 0x000D ]]; then
                        has_acs=1
                        break
                    fi

                    if [[ $ecap_next -eq 0 ]]; then
                        break
                    fi
                    ecap_offset=$ecap_next
                done

                if [[ $has_acs -eq 1 ]]; then
                    echo -e "      ${GREEN}✓ 父 Bridge 支持 ACS${NC}"
                else
                    # 检查 quirk
                    port_type=$(lspci -vv -s "$parent_bdf" 2>/dev/null | grep "Port type" || true)
                    if echo "$port_type" | grep -qi "root port"; then
                        echo -e "      ${GREEN}✓ Root Port（总是提供隔离）${NC}"
                    else
                        echo -e "      ${RED}✗ 父 Bridge 不支持 ACS${NC}"
                        echo -e "      ${RED}  → 这是设备被分到同一 group 的原因${NC}"
                        echo -e "      ${RED}  → 对应 CLAUDE.md 陷阱 #11: 没有 ACS = 没有隔离 = 同一 group${NC}"
                    fi
                fi
            fi
        fi
    done
    echo
done

# ─── 3. 统计汇总 ──────────────────────────────────────
echo -e "${CYAN}[3] 统计汇总${NC}"
echo

echo -e "  总设备数:          ${total_devices}"
echo -e "  IOMMU Group 数:    ${total_groups}"
echo -e "  单设备 Group:      ${GREEN}${single_dev_groups}${NC}"
echo -e "  多设备 Group:      ${YELLOW}${multi_dev_groups}${NC}"
echo

if [[ $multi_dev_groups -gt 0 ]]; then
    echo -e "  ${YELLOW}存在多设备共享的 IOMMU group！${NC}"
    echo
    echo "  这意味着这些设备必须一起直通给同一个 VM，无法单独分配。"
    echo
    echo "  原因分析："
    echo "    1. 设备之间的 Bridge 没有启用 ACS 隔离"
    echo "       → 对应 annotations.md §3: pci_acs_enabled() 返回 false"
    echo "    2. 设备可能在同一个 Switch 下游"
    echo "       → Switch 的 Downstream Port 需要 ACS 才能分割 group"
    echo "    3. 可能存在 Intel RCiEP quirk"
    echo "       → 对应 CLAUDE.md 陷阱 #11: pci_quirk_rciep_acs"
    echo
    echo "  解决方案："
    echo "    - 启用 ACS: setpci -s <bridge_bdf> <acs_ctl_offset>.w=<value>"
    echo "    - 使用内核参数: pci=config_acs=<bitmask>"
    echo "      （注意位串从右往左解析！见 CLAUDE.md 陷阱 #13）"
    echo "    - 将设备移到不同的 Root Port 下"
    echo
else
    echo -e "  ${GREEN}所有设备都有独立的 IOMMU group！${NC}"
    echo "  每个设备都可以单独直通给不同的 VM。"
    echo
fi

# ─── 4. 直通可行性分析 ────────────────────────────────
echo -e "${CYAN}[4] 设备直通可行性分析${NC}"
echo

# 查找常见可直通设备（网卡、GPU、存储、NVMe）
echo "  可直通设备（Endpoint, Type 0）:"
for group_dir in /sys/kernel/iommu_groups/*/; do
    group_num=$(basename "$group_dir")

    for dev_link in "$group_dir"devices/*; do
        [[ -e "$dev_link" ]] || continue
        dev_bdf=$(basename "$dev_link")

        # 检查是否是 Endpoint
        ht=$(lspci -v -s "$dev_bdf" 2>/dev/null | grep "Header type" || true)
        if echo "$ht" | grep -q "0"; then
            desc=$(lspci -s "$dev_bdf" 2>/dev/null | cut -d' ' -f2-)

            # 过滤常见设备类型
            if echo "$desc" | grep -qiE "ethernet|network|3d controller|vga|nvme|non-volatile|usb|audio"; then
                dev_count=$(ls "$group_dir"devices/ 2>/dev/null | wc -l)
                if [[ $dev_count -eq 1 ]]; then
                    status="${GREEN}✓ 可单独直通${NC}"
                else
                    status="${YELLOW}⚠ 需与同 group 设备一起直通${NC}"
                fi

                echo "    ${dev_bdf} — ${desc}"
                echo "      Group ${group_num}: ${status}"
            fi
        fi
    done
done
echo

# ─── 5. 拓扑与 Group 关系可视化 ───────────────────────
echo -e "${CYAN}[5] 拓扑与 Group 关系${NC}"
echo "    对应 annotations.md §3: 从设备向上遍历检查 ACS"
echo

echo "  PCIe 拓扑（标注 IOMMU Group）:"
echo

# 为每个设备标注其 IOMMU group
lspci -tv 2>/dev/null | while IFS= read -r line; do
    # 尝试从行中提取 BDF
    if echo "$line" | grep -qE '[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]'; then
        bdf=$(echo "$line" | grep -oE '[0-9a-f]{2}:[0-9a-f]{2}\.[0-9]' | head -1)
        # 查找该设备的 IOMMU group
        group_file=$(find /sys/kernel/iommu_groups/ -name "$bdf" 2>/dev/null | head -1 || true)
        if [[ -n "$group_file" ]]; then
            group_num=$(echo "$group_file" | grep -oE '[0-9]+' | head -1)
            echo -e "${line} ${CYAN}[Group ${group_num}]${NC}"
        else
            echo "$line"
        fi
    else
        echo "$line"
    fi
done
echo

# ─── 6. 思考题 ─────────────────────────────────────────
echo -e "${CYAN}[6] 思考题${NC}"
echo
echo "  1. 系统中有哪些多设备共享的 IOMMU group？"
echo "     这些设备在拓扑中的位置关系是什么？"
echo "     → 对应 annotations.md §3: 向上遍历直到找到 ACS Bridge"
echo
echo "  2. 是否有 GPU 和 HDMI Audio 在同一个 group？"
echo "     这是常见现象：GPU 的 HDMI Audio 是同一芯片的不同 Function"
echo "     它们必须一起直通给同一个 VM"
echo
echo "  3. 如何通过 ACS 配置来分割 group？"
echo "     对父 Bridge 启用 ACS 的 SV/RR/CR/UF 位"
echo "     → 对应 CLAUDE.md 陷阱 #11: REQ_ACS_FLAGS = SV|RR|CR|UF"
echo
echo "  4. （进阶）为什么有些 Root Port 没有 ACS 但设备仍在独立 group？"
echo "     → Root Port 本身提供隔离（不需要 ACS）"
echo "     → 对应 annotations.md §3: Root Port 和 Downstream Port 检查 ACS"
echo "       但 Root Port 的下游设备默认就在独立 group"
echo
echo "  5. （进阶）IOMMU group 的划分与 ACS quirk 的关系？"
echo "     → 对应 CLAUDE.md 陷阱 #11: pci_acs_enabled() 开头先调"
echo "       pci_dev_specific_acs_enabled()，命中 quirk 就直接定论"
echo "       Intel RCiEP 全部命中 pci_quirk_rciep_acs → 隔离成立"
echo

divider
echo -e "${GREEN}完成！${NC}"
echo
echo "  所有练习完成。回顾："
echo "    ex1 — PCIe 拓扑分析"
echo "    ex2 — 配置空间读写"
echo "    ex3 — ACS 能力分析"
echo "    ex4 — IOMMU Group 分析（本练习）"
echo
echo "  连接阅读："
echo "    - annotations.md §3 — ACS 与 IOMMU group 的源码实现"
echo "    - vfio-integration.md — PCIe 与 VFIO 的结合"
echo "    - phase6 的 vfio 直通实践"
divider
