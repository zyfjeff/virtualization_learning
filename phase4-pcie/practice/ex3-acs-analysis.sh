#!/usr/bin/env bash
# ex3-acs-analysis.sh — 验证 ACS 三层判定（quirk → cap 掩码 → ctl 检查）
#
# 五段式（COURSE_FRAMEWORK §3.5）
#
# 【理论来源】
#   annotations.md §3 pci_acs_flags_enabled()（pci.c:3583）；
#   AGENTS.md 陷阱 #11（quirk 优先 + 未声明算启用 + REQ_ACS_FLAGS=SV|RR|CR|UF）、
#   #13（pci_enable_acs probe 时主动写 ACS Control）。
#   规范：PCIe Spec §6.12（ACS 默认关闭，由软件启用；RCiEP 建议先过 TA）。
#
# 【预测】
#   P1: 有 ACS cap 的设备，Ctl ⊇ (Cap ∩ REQ_ACS_FLAGS)
#       （因为 pci_enable_acs 在 probe 时按 cap 掩码启用了 SV/RR/CR/UF）
#   P2: Intel RCiEP（vendor 8086 + port type RCiEP）即使无 ACS cap，
#       也独占一个 IOMMU group（pci_quirk_rciep_acs 判隔离成立）
#   P3a: 无 ACS 多功能但命中 Intel RCiEP quirk → 各 function 被 quirk 单独隔离（分散 group）
#   P3b: 无 ACS 多功能 Endpoint（非 bridge）且无 quirk → 各 function 同 group
#   （P3a/P3b 合起来验证 AGENTS.md #11：quirk 优先于多功能聚合逻辑。
#    多功能 bridge/root port 每个端口本就是独立隔离域，不适用聚合，故排除）
#
# 用法：sudo ./ex3-acs-analysis.sh
# 只读取证，不写 ACS 寄存器。

set -euo pipefail
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
divider() { echo "════════════════════════════════════════════════════════════════"; }
[[ $EUID -ne 0 ]] && { echo -e "${RED}需要 root：sudo $0${NC}"; exit 1; }
command -v lspci setpci >/dev/null || { echo "安装 pciutils"; exit 1; }
[[ -d /sys/kernel/iommu_groups ]] || { echo -e "${YELLOW}IOMMU 未启用，P2/P3 无法验证${NC}"; }

REQ=$((0x0001 | 0x0004 | 0x0008 | 0x0010))   # SV|RR|CR|UF

divider
echo -e "${GREEN}  ex3 验证 ACS 三层判定${NC}"
divider
echo -e "${CYAN}【理论】${NC} annotations §3 + AGENTS.md #11/#13：quirk 优先 → cap 掩码 → ctl 检查；"
echo "        REQ_ACS_FLAGS=SV|RR|CR|UF；pci_enable_acs probe 时主动启用。"
echo -e "${CYAN}【预测】${NC} P1 Ctl⊇(Cap∩REQ)；P2 Intel RCiEP 无 cap 独占 group；"
echo "        P3a 无 ACS 多功能+quirk→分散；P3b 无 ACS 多功能+无 quirk→同 group。"
echo

# 一次性 dump port type（避免 350 次 lspci -vv）
VV=$(lspci -vv 2>/dev/null || true)
group_of() { # BDF → group 号（无则空）
    local g
    g=$(readlink "/sys/bus/pci/devices/0000:$1/iommu_group" 2>/dev/null || true)
    [[ -n "$g" ]] && basename "$g" || true
}
group_size() { [[ -n "$1" ]] && ls "/sys/kernel/iommu_groups/$1/devices" 2>/dev/null | wc -l || echo 0; }

# ── 扫 ACS extended cap（ID 0x000D），记录 cap/ctl ──
declare -A ACS_OFF ACS_CAP ACS_CTL
while read -r bdf _; do
    off=0x100
    while [[ $off -lt 0x1000 ]]; do
        hdr=$(setpci -s "$bdf" ${off}.l 2>/dev/null || echo 0)
        v=$((16#$hdr)); id=$((v & 0xFFFF)); nxt=$(((v >> 20) & 0xFFF))
        if [[ $id -eq 0x000D ]]; then
            cap=$(setpci -s "$bdf" $(printf '%x' $((16#$off+4))).w 2>/dev/null || echo 0)
            ctl=$(setpci -s "$bdf" $(printf '%x' $((16#$off+6))).w 2>/dev/null || echo 0)
            ACS_OFF[$bdf]=$off; ACS_CAP[$bdf]=$((16#$cap)); ACS_CTL[$bdf]=$((16#$ctl))
            break
        fi
        [[ $nxt -eq 0 ]] && break
        off=$nxt
    done
done < <(lspci 2>/dev/null | awk '{print $1}')

pass=0; fail=0; incon=0
check() { case $4 in
    0) echo -e "  ${GREEN}PASS${NC} $1 $2 — $3"; pass=$((pass+1));;
    1) echo -e "  ${RED}FAIL${NC} $1 $2 — $3"; fail=$((fail+1));;
    *) echo -e "  ${YELLOW}INCONCLUSIVE${NC} $1 $2 — $3"; incon=$((incon+1));; esac; }

# ── P1：Ctl ⊇ (Cap ∩ REQ) ──
echo -e "${CYAN}【对照 P1】${NC} ${#ACS_OFF[@]} 个设备有 ACS cap，检查 Ctl⊇(Cap∩REQ)："
for bdf in "${!ACS_OFF[@]}"; do
    cap=${ACS_CAP[$bdf]}; ctl=${ACS_CTL[$bdf]}
    need=$((cap & REQ)); got=$((ctl & REQ))
    if [[ $((got & need)) -eq $need ]]; then
        check P1 "$bdf" "$(printf 'cap∩REQ=0x%02x ⊆ ctl=0x%02x' "$need" "$got")" 0
    else
        check P1 "$bdf" "$(printf 'cap∩REQ=0x%02x 但 ctl=0x%02x 缺位' "$need" "$got")" 1
    fi
done
echo

# ── P2：Intel RCiEP 无 cap 也独占 group ──
echo -e "${CYAN}【对照 P2】${NC} Intel RCiEP（quirk 层，AGENTS.md #11）："
rciep_checked=0
declare -A QUIRK
while read -r bdf; do
    [[ -z "$bdf" ]] && continue
    vendor=$(setpci -s "$bdf" 0.w 2>/dev/null || echo 0)
    [[ "${vendor,,}" == "8086" ]] || continue
    if [[ -n ${ACS_OFF[$bdf]:-} ]]; then continue; fi   # 有 cap 的不算 quirk 案例
    QUIRK[$bdf]=1
    g=$(group_of "$bdf"); sz=$(group_size "$g")
    if [[ -z "$g" ]]; then check P2 "$bdf" "无 IOMMU group（IOMMU 未启用？）" 2; continue; fi
    rciep_checked=1
    [[ "$sz" -eq 1 ]] && check P2 "$bdf" "无 ACS cap 但独占 group $g（quirk 生效）" 0 \
                      || check P2 "$bdf" "无 ACS cap 却在 ${sz} 成员 group $g" 1
done < <(awk '/^[0-9a-f][0-9a-f]:/{bdf=$1} /Root Complex Integrated Endpoint/{print bdf}' <<< "$VV" | sort -u)
[[ $rciep_checked -eq 0 ]] && check P2 "-" "本机无『无 cap 的 Intel RCiEP』案例" 2
echo

# ── P3a/P3b：无 ACS 多功能设备，按是否命中 quirk 分两类 ──
echo -e "${CYAN}【对照 P3】${NC} 无 ACS 多功能设备（quirk 优先于多功能聚合，AGENTS.md #11）："
p3a=0; p3b=0
declare -A slot_devs
while read -r bdf class _; do
    [[ "$class" == "0604:" ]] && continue        # 多功能 bridge 各端口独立隔离，不适用聚合
    slot=${bdf%.*}
    [[ -n ${ACS_OFF[$bdf]:-} ]] && continue
    slot_devs[$slot]+="$bdf "
done < <(lspci -n 2>/dev/null)
for slot in "${!slot_devs[@]}"; do
    read -ra fns <<< "${slot_devs[$slot]}"
    [[ ${#fns[@]} -ge 2 ]] || continue
    g0=$(group_of "${fns[0]}"); [[ -n "$g0" ]] || continue
    # 该 slot 是否命中 quirk（任一 function 是 Intel RCiEP）
    hit=0; for f in "${fns[@]}"; do [[ -n ${QUIRK[$f]:-} ]] && hit=1; done
    same=1; for f in "${fns[@]}"; do [[ "$(group_of "$f")" == "$g0" ]] || same=0; done
    if [[ $hit -eq 1 ]]; then
        p3a=1
        [[ $same -eq 0 ]] && check P3a "$slot" "命中 quirk → ${#fns[@]} function 分散 group（quirk 优先）" 0 \
                          || check P3a "$slot" "命中 quirk 却同 group" 1
    else
        p3b=1
        [[ $same -eq 1 ]] && check P3b "$slot" "无 quirk → ${#fns[@]} function 同 group $g0" 0 \
                          || check P3b "$slot" "无 quirk 却分散 group" 1
    fi
done
[[ $p3a -eq 0 ]] && check P3a "-" "本机无『无 ACS 多功能+quirk』案例" 2
[[ $p3b -eq 0 ]] && check P3b "-" "本机无『无 ACS 多功能+无 quirk』案例" 2
echo

divider
echo -e "${CYAN}【结论与反馈】${NC}  ${GREEN}PASS=$pass${NC}  ${RED}FAIL=$fail${NC}  ${YELLOW}INCONCLUSIVE=$incon${NC}"
echo "  P1 验证 #13（probe 主动启用）；P2 验证 #11（quirk 优先于 cap）；"
echo "  P3a/P3b 验证 #11（quirk 优先于多功能聚合）——无 ACS 多功能本该同 group，"
echo "  但命中 Intel RCiEP quirk 的会被单独隔离。这就是『quirk 优先』的实证。"
echo "  INCONCLUSIVE 表示本机缺该案例，不是理论被证伪——换硬件或 vIOMMU 环境再验。"
echo -e "  ${CYAN}链尾${NC}：判定结果怎么落成 IOMMU group 边界？→ ex4-iommu-group.sh"
divider
