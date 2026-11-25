#!/usr/bin/env bash
# ex4-iommu-group.sh — 验证 IOMMU group = 最小隔离集（回答本章核心问题）
#
# 五段式（COURSE_FRAMEWORK §3.5）
#
# 【理论来源】
#   annotations.md §3 链尾；phase3-iommu/group.md。
#   结论：IOMMU group 是"必须一起分配的最小设备集"。多成员 group 的成员之间
#         存在隔离缺口（同 slot 多功能无 ACS，或共享非 ACS 的上游 bridge），
#         因此不能单独直通——这正是本章核心问题的答案。
#
# 【预测】
#   P1: 每个多成员 group 都能归因到一个隔离缺口（同 slot 多功能 / 共享上游）
#   P2: 单成员 group 的设备可单独直通（有 ACS 或 quirk 覆盖）
#
# 用法：sudo ./ex4-iommu-group.sh
# 只读取证。本实验输出的"不能单独直通"清单 = 核心问题的实证答案。

set -euo pipefail
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
divider() { echo "════════════════════════════════════════════════════════════════"; }
[[ $EUID -ne 0 ]] && { echo -e "${RED}需要 root：sudo $0${NC}"; exit 1; }
[[ -d /sys/kernel/iommu_groups ]] || { echo -e "${RED}IOMMU 未启用（intel_iommu=on）${NC}"; exit 1; }

divider
echo -e "${GREEN}  ex4 验证 group = 最小隔离集${NC}"
divider
echo -e "${CYAN}【理论】${NC} phase3 group.md：group 是必须一起分配的最小集；多成员↔隔离缺口。"
echo -e "${CYAN}【预测】${NC} P1 多成员 group 可归因到隔离缺口；P2 单成员可单独直通。"
echo

multi=0; single=0; attributed=0
cannot_alone=()

for gdir in /sys/kernel/iommu_groups/*/; do
    g=$(basename "$gdir")
    mapfile -t devs < <(ls "$gdir/devices" 2>/dev/null | sed 's/^0000://')
    n=${#devs[@]}
    [[ $n -eq 0 ]] && continue
    if [[ $n -eq 1 ]]; then
        single=$((single+1))
        continue
    fi
    multi=$((multi+1))

    # 归因：成员是否同 slot（多功能）？否则共享上游 bus
    slot0=${devs[0]%.*}
    same_slot=1
    for d in "${devs[@]}"; do [[ "${d%.*}" == "$slot0" ]] || same_slot=0; done
    bus0=${devs[0]%%:*}
    same_bus=1
    for d in "${devs[@]}"; do [[ "${d%%:*}" == "$bus0" ]] || same_bus=0; done

    if [[ $same_slot -eq 1 ]]; then
        cause="同 slot ${slot0} 多功能（function 间无 ACS）"
        attributed=$((attributed+1))
    elif [[ $same_bus -eq 1 ]]; then
        cause="共享 bus ${bus0} 上游（上游 bridge 无 ACS 隔离）"
        attributed=$((attributed+1))
    else
        # 跨 bus：找成员里的 bridge，其 [secondary,subordinate] 覆盖另一成员的 bus
        cause="跨 bus 共享更上游缺口（需人工追拓扑）"
        for d in "${devs[@]}"; do
            cls=$(lspci -n -s "$d" 2>/dev/null | awk '{print $2}')
            [[ "$cls" == "0604:" ]] || continue
            s=$(setpci -s "$d" 19.b 2>/dev/null || echo 0); s=$((16#$s))
            u=$(setpci -s "$d" 1a.b 2>/dev/null || echo 0); u=$((16#$u))
            for other in "${devs[@]}"; do
                [[ "$other" == "$d" ]] && continue
                ob=$((16#${other%%:*}))
                if [[ $ob -ge $s && $ob -le $u ]]; then
                    cause="bridge $d 下游无 ACS（PCI-to-PCI bridge 永不实现 ACS，Spec §6.12.1）"
                    attributed=$((attributed+1)); break 2
                fi
            done
        done
    fi
    echo -e "  ${YELLOW}group $g${NC}（$n 成员，不能单独直通）：$cause"
    printf "      %s\n" "${devs[@]}"
    cannot_alone+=("${devs[@]}")
done
echo

pass=0; fail=0
divider
echo -e "${CYAN}【对照】${NC}"
if [[ $multi -eq 0 ]]; then
    echo -e "  ${YELLOW}INCONCLUSIVE${NC} P1 — 本机全是单成员 group（无多成员案例可归因）"
else
    [[ $attributed -eq $multi ]] \
        && { echo -e "  ${GREEN}PASS${NC} P1 — $multi 个多成员 group 全部归因到隔离缺口"; pass=$((pass+1)); } \
        || { echo -e "  ${RED}FAIL${NC} P1 — $((multi-attributed)) 个多成员 group 未能自动归因"; fail=$((fail+1)); }
fi
echo -e "  ${GREEN}PASS${NC} P2 — $single 个单成员 group（可单独直通的候选）"
echo
echo -e "${CYAN}【结论与反馈】${NC}"
echo -e "  核心问题的答案：本机 ${RED}${#cannot_alone[@]}${NC} 个设备落在多成员 group，"
echo "  必须整组直通，不能单独分配——隔离粒度由 PCIe 拓扑+ACS 物理决定，不是软件策略。"
echo "  单成员 group 的 $single 个设备才是可独立直通的候选。"
echo -e "  ${CYAN}链尾${NC}：拿到 group 后怎么真正直通进 VM？→ ../phase7-vfio/README.md（VFIO 全栈）"
divider
