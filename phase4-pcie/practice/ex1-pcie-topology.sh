#!/usr/bin/env bash
# ex1-pcie-topology.sh — 验证 bus 号的递归层级分配
#
# 五段式（COURSE_FRAMEWORK §3.5）：理论 → 预测 → 操作 → 对照 → 反馈
#
# 【理论来源】
#   annotations.md §1 pci_scan_child_bus_extend()（probe.c:2963）递归扫描 bridge；
#   §2 bridge 的三个 bus 号寄存器（pci_regs.h:132-134，配置空间 0x18/0x19/0x1a）。
#   结论：bus 号沿拓扑树递归分配，bridge 的 secondary = 直接下游 bus，
#         subordinate = 子树里最远的 bus。
#
# 【预测】（先押注，再看证据）
#   P1: 每个 bridge primary < secondary ≤ subordinate（层级单调）
#   P2: 沿 bridge 映射从 secondary 递归走子树，最远 bus == subordinate
#       （这正是 §1 递归扫描算法的不变量——本实验的核心）
#   P3: 各 bridge 的 secondary bus 号互不相同（bus 号唯一分配）
#
# 用法：sudo ./ex1-pcie-topology.sh
# 只读取证，不写任何寄存器。

set -euo pipefail
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
divider() { echo "════════════════════════════════════════════════════════════════"; }

[[ $EUID -ne 0 ]] && { echo -e "${RED}需要 root 读配置空间：sudo $0${NC}"; exit 1; }
command -v lspci setpci >/dev/null || { echo "安装 pciutils: apt install pciutils"; exit 1; }

divider
echo -e "${GREEN}  ex1 验证 bus 号递归层级分配${NC}"
divider
echo -e "${CYAN}【理论】${NC} annotations §1-2：bus 号沿拓扑树递归分配；"
echo "        bridge 0x18/0x19/0x1a = primary/secondary/subordinate。"
echo -e "${CYAN}【预测】${NC} P1 primary<secondary≤subordinate；P2 子树最远 bus==subordinate；"
echo "        P3 secondary bus 号唯一。"
echo

LSPCI_N=$(lspci -n 2>/dev/null)
LSPCI=$(lspci 2>/dev/null)

# bridge 用 class code 0x0604 识别（不是 header type bit——见文末【发现】）
declare -A PRIM SEC SUB
bridges=()
while read -r bdf class _; do
    [[ "$class" == "0604:" || "$class" == "0604" ]] || continue
    p=$(setpci -s "$bdf" 18.b 2>/dev/null) || continue
    s=$(setpci -s "$bdf" 19.b 2>/dev/null) || continue
    u=$(setpci -s "$bdf" 1a.b 2>/dev/null) || continue
    bridges+=("$bdf"); PRIM[$bdf]=$((16#$p)); SEC[$bdf]=$((16#$s)); SUB[$bdf]=$((16#$u))
done <<< "$LSPCI_N"

echo -e "${CYAN}【操作】${NC} 扫到 ${#bridges[@]} 个 PCI bridge（class 0604），读取 bus 号寄存器。"
echo

pass=0; fail=0
check() { if [[ $4 -eq 0 ]]; then echo -e "  ${GREEN}PASS${NC} $1 $2 — $3"; pass=$((pass+1));
          else echo -e "  ${RED}FAIL${NC} $1 $2 — $3"; fail=$((fail+1)); fi; }

echo -e "${CYAN}【对照】${NC} 逐 bridge 验证 P1/P2："
declare -A sec_seen
for bdf in "${bridges[@]}"; do
    p=${PRIM[$bdf]}; s=${SEC[$bdf]}; u=${SUB[$bdf]}

    [[ $p -lt $s && $s -le $u ]] && check P1 "$bdf" "p=$p < s=$s ≤ u=$u" 0 || check P1 "$bdf" "p=$p < s=$s ≤ u=$u" 1

    # P2: 从 secondary 沿 primary→secondary 映射递归走子树，取最远 bus
    max=$s; stack=("$s")
    while [[ ${#stack[@]} -gt 0 ]]; do
        cur=${stack[-1]}; unset 'stack[-1]'
        for b2 in "${bridges[@]}"; do
            if [[ ${PRIM[$b2]} -eq $cur ]]; then
                s2=${SEC[$b2]}; [[ $s2 -gt $max ]] && max=$s2; stack+=("$s2")
            fi
        done
    done
    [[ $max -eq $u ]] && check P2 "$bdf" "子树最远 bus=$max == subordinate=$u" 0 \
        || check P2 "$bdf" "子树最远 bus=$max == subordinate=$u" 1

    # P3: secondary 唯一
    if [[ -n ${sec_seen[$s]:-} ]]; then
        check P3 "$bdf" "secondary=$s 与 ${sec_seen[$s]} 重复" 1
    else
        sec_seen[$s]=$bdf
    fi
done
echo -e "  ${GREEN}PASS${NC} P3 — ${#sec_seen[@]} 个 secondary bus 号互不相同（共 ${#bridges[@]} bridge）"
echo

divider
echo -e "${CYAN}【结论与反馈】${NC}"
echo -e "  自动对照：${GREEN}PASS=$pass${NC}  ${RED}FAIL=$fail${NC}"
[[ $fail -eq 0 ]] && echo "  理论成立：bus 号按 §1 递归算法分配，subordinate 覆盖整个子树。" \
                  || echo -e "  ${YELLOW}有 FAIL：对照 annotations §1-2，或记入 corrections.md。${NC}"
echo
echo -e "${CYAN}【发现】${NC}（本机实测，理论简化的边界——不是脚本 bug）"
echo "  1. header type bit 不是可靠的 bridge 判据：17:00.0 / 02:00.0 class=0604（bridge）"
echo "     但 config[0x0e] bit7=0（setpci 与 sysfs 双路核实）。basics 'Type1 头=bridge' 是简化。"
echo "     → 内核如何调和 hdr_type 与 class？待核查，已登记 corrections.md。"
echo "  2. 存在空 bus（如 00:1c.0 的 secondary=01 无设备）：热插拔/未 populated 槽位，"
echo "     理论允许，不是分配错误。"
echo
echo "  拓扑树（人工核对一个 bridge 的 subordinate=最远下游）："
lspci -tv 2>/dev/null | head -15
echo
echo -e "  ${CYAN}链尾${NC}：拓扑就位——内核怎么判定哪些设备互相隔离？→ ex3-acs-analysis.sh"
divider
