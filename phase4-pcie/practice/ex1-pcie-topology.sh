#!/usr/bin/env bash
# ex1-pcie-topology.sh — PCIe 拓扑分析
#
# 目标：
#   1. 使用 lspci 绘制 PCIe 拓扑树
#   2. 识别 Root Complex、Switch、Endpoint
#   3. 列出所有 BDF 和设备类型
#   4. 连接 annotations.md §1「PCIe 设备发现与枚举」
#
# 连接 annotations.md：
#   - §1 pci_scan_child_bus_extend() — 扫描当前总线和下游 Bridge
#   - §1 pci_scan_slot() — 检测 Function 0 和多功能设备
#   - basics.md §1 — 拓扑结构（RC / Switch / Endpoint）
#   - basics.md §2 — BDF 寻址机制
#
# 用法：
#   sudo ./ex1-pcie-topology.sh
#
# 为什么需要 root：
#   lspci -xxx 需要读取配置空间，普通用户只能看到部分信息

set -euo pipefail

# ─── 颜色定义 ───────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

divider() {
    echo "════════════════════════════════════════════════════════════════"
}

# ─── 检查权限 ───────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo -e "${YELLOW}[警告]${NC} 建议以 root 运行以获取完整配置空间信息"
    echo "  sudo $0"
    echo
fi

divider
echo -e "${GREEN}  PCIe 拓扑分析工具${NC}"
echo "  连接 annotations.md §1 + basics.md §1-2"
divider
echo

# ─── 1. PCIe 拓扑树 ────────────────────────────────────
echo -e "${CYAN}[1] PCIe 拓扑树（lspci -tv）${NC}"
echo "    对应 basics.md §1.1 — RC / Switch / Endpoint 层次结构"
echo

if command -v lspci &>/dev/null; then
    lspci -tv 2>/dev/null || lspci -t 2>/dev/null || echo "  lspci 不支持 -t 选项"
else
    echo -e "  ${RED}错误: lspci 未安装${NC}"
    echo "  安装: apt install pciutils"
fi
echo

# ─── 2. 设备分类统计 ────────────────────────────────────
echo -e "${CYAN}[2] 设备类型统计${NC}"
echo "    对应 basics.md §1.2-1.4 — RC / Bridge / Endpoint"
echo

# 用 lspci -v 解析 Header Type
# Type 0 = Endpoint, Type 1 = Bridge
declare -A type_count
declare -a endpoints=()
declare -a bridges=()

while IFS= read -r line; do
    # 提取 BDF
    bdf=$(echo "$line" | awk '{print $1}')
    [[ -z "$bdf" ]] && continue

    # 获取详细信息
    detail=$(lspci -v -s "$bdf" 2>/dev/null | grep -i "header type" || true)
    if echo "$detail" | grep -q "1"; then
        type_count[Bridge]=$(( ${type_count[Bridge]:-0} + 1 ))
        bridges+=("$bdf")
    else
        type_count[Endpoint]=$(( ${type_count[Endpoint]:-0} + 1 ))
        endpoints+=("$bdf")
    fi
done < <(lspci 2>/dev/null | awk '{print $1}')

echo -e "  ${GREEN}Endpoint 数量:${NC} ${type_count[Endpoint]:-0}"
echo -e "  ${GREEN}Bridge 数量:${NC}   ${type_count[Bridge]:-0}"
echo

# ─── 3. Root Complex 识别 ──────────────────────────────
echo -e "${CYAN}[3] Root Complex 与 Root Port 识别${NC}"
echo "    对应 basics.md §1.2 — Root Complex 是拓扑根节点"
echo "    对应 annotations.md §1 — pci_create_root_bus() 创建 Bus 0"
echo

# Bus 0 上的 Host Bridge 是 Root Complex 的一部分
echo "  Bus 0 设备（Root Complex 内部）:"
lspci 2>/dev/null | grep "^00:" | while IFS= read -r line; do
    echo -e "    ${YELLOW}${line}${NC}"
done
echo

# 查找 PCIe Root Port（Type 1, PCIe Cap 中 type = Root Port）
echo "  PCIe Root Port（RC 的下游端口）:"
found_rp=0
for bdf in $(lspci 2>/dev/null | awk '{print $1}'); do
    # 读取 PCIe Capability 中的 Device/Port Type
    # PCI_EXP_FLAGS @ offset 2 (within PCIe cap), bits 7:4 = type
    pcie_cap=$(lspci -v -s "$bdf" 2>/dev/null | grep "Capabilities:.*PCI Express" || true)
    if [[ -n "$pcie_cap" ]]; then
        # 从 lspci -vv 提取 Port type
        port_type=$(lspci -vv -s "$bdf" 2>/dev/null | grep "Port type" || true)
        if echo "$port_type" | grep -qi "root port"; then
            desc=$(lspci -s "$bdf" 2>/dev/null | cut -d' ' -f2-)
            echo -e "    ${GREEN}${bdf}${NC} — ${desc}"
            echo "      ${port_type}"
            found_rp=1
        fi
    fi
done
if [[ $found_rp -eq 0 ]]; then
    echo -e "    ${YELLOW}(未找到 Root Port，可能是虚拟环境或集成设备)${NC}"
fi
echo

# ─── 4. Switch 识别 ────────────────────────────────────
echo -e "${CYAN}[4] PCIe Switch 识别${NC}"
echo "    对应 basics.md §1.3 — Switch 包含 Upstream + Downstream Port"
echo "    Switch 在 lspci 中显示为 'PCI bridge'"
echo

echo "  PCIe Switch 端口（Upstream/Downstream Port）:"
found_sw=0
for bdf in $(lspci 2>/dev/null | awk '{print $1}'); do
    port_type=$(lspci -vv -s "$bdf" 2>/dev/null | grep "Port type" || true)
    if echo "$port_type" | grep -qiE "upstream|downstream"; then
        desc=$(lspci -s "$bdf" 2>/dev/null | cut -d' ' -f2-)
        echo -e "    ${BLUE}${bdf}${NC} — ${desc}"
        echo "      ${port_type}"
        found_sw=1
    fi
done
if [[ $found_sw -eq 0 ]]; then
    echo -e "    ${YELLOW}(未找到 Switch 端口)${NC}"
fi
echo

# ─── 5. 完整 BDF 列表 ──────────────────────────────────
echo -e "${CYAN}[5] 完整 BDF 列表与设备类型${NC}"
echo "    对应 basics.md §2.1 — BDF = Bus(8b) + Device(5b) + Function(3b)"
echo

printf "  ${GREEN}%-10s %-8s %-8s %-52s${NC}\n" "BDF" "Bus" "Dev.Fn" "设备描述"
echo "  ────────── ──────── ──────── ──────────────────────────────────────────"

lspci 2>/dev/null | while IFS= read -r line; do
    bdf=$(echo "$line" | awk '{print $1}')
    desc=$(echo "$line" | cut -d' ' -f2-)

    # 解析 BDF
    bus=$(echo "$bdf" | cut -d: -f1)
    devfn=$(echo "$bdf" | cut -d: -f2)
    dev=$(echo "$devfn" | cut -d. -f1)
    fn=$(echo "$devfn" | cut -d. -f2)

    printf "  %-10s %-8s %-8s %-52s\n" "$bdf" "$bus" "${dev}.${fn}" "$desc"
done
echo

# ─── 6. 每个 Bus 的设备分布 ────────────────────────────
echo -e "${CYAN}[6] 按 Bus 号分组${NC}"
echo "    对应 annotations.md §2 — Bridge 的 Primary/Secondary/Subordinate Bus"
echo

lspci 2>/dev/null | awk '{print $1}' | cut -d: -f1 | sort -u | while read -r bus; do
    count=$(lspci 2>/dev/null | grep "^${bus}:" | wc -l)
    echo -e "  Bus ${bus}: ${count} 个设备"
    lspci 2>/dev/null | grep "^${bus}:" | while IFS= read -r line; do
        bdf=$(echo "$line" | awk '{print $1}')
        desc=$(echo "$line" | cut -d' ' -f2- | cut -c1-60)
        echo "    ${bdf} — ${desc}"
    done
done
echo

# ─── 7. 关键信息汇总 ──────────────────────────────────
echo -e "${CYAN}[7] 思考题${NC}"
echo
echo "  1. 你的系统有几个 PCIe Root Port？它们分别连接什么设备？"
echo "     → 对应 basics.md §1.2: Root Complex 至少包含一个 Root Port"
echo
echo "  2. 是否有 PCIe Switch？如果有，它有几个 Downstream Port？"
echo "     → 对应 basics.md §1.3: Switch 有一个 Upstream + 多个 Downstream"
echo
echo "  3. 哪些 Bus Number 被分配了？哪些 Bus 范围是空闲的？"
echo "     → 对应 annotations.md §2: Bus Number 由 Bridge 的 Secondary/Subordinate 决定"
echo
echo "  4. 观察拓扑树，哪些设备在同一个 Bridge 下游？"
echo "     → 这决定了它们是否可能在同一个 IOMMU group（见 ex4-iommu-group.sh）"
echo

divider
echo -e "${GREEN}完成！${NC} 连接下一个练习: ./ex2-config-space.sh"
divider
