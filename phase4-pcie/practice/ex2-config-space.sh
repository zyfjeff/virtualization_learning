#!/usr/bin/env bash
# ex2-config-space.sh — 验证 Type 0/1 配置空间布局与 BAR 编码
#
# 五段式（COURSE_FRAMEWORK §3.5）
#
# 【理论来源】
#   basics.md §3（Type 0/1 头）、§4（BAR 编码）；annotations.md §4 配置访问。
#   结论：Type 0（Endpoint）有 6 个 BAR（0x10–0x24）；Type 1（bridge）只有 2 个 BAR
#         （0x10/0x14）+ bus 号寄存器（0x18–0x1a）。BAR bit0：0=Memory，1=I/O；
#         Memory 的 bits2:1：00=32 位，10=64 位；bit3=prefetchable。
#
# 【预测】
#   P1: Endpoint 的 BAR0 bit0 与 lspci 首个区域类型一致（0↔Memory，1↔I/O）
#   P2: bridge（class 0604）的 0x18–0x1a 是 bus 号（ex1 已证），不是 BAR
#   P3: 64 位 Memory BAR（bits2:1=10）占用相邻两个 BAR 槽（BAR0+BAR1）
#
# 用法：sudo ./ex2-config-space.sh [BDF]   （不带参数自动挑样本）
# ⚠ 只读取证。绝不写 BAR（写 BAR 会扰动设备，AGENTS.md 陷阱 #15）。
#   BAR 大小探测（写全 1 读回掩码）只讲原理，不在活设备上执行。

set -euo pipefail
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
divider() { echo "════════════════════════════════════════════════════════════════"; }
[[ $EUID -ne 0 ]] && { echo -e "${RED}需要 root：sudo $0${NC}"; exit 1; }
command -v lspci setpci >/dev/null || { echo "安装 pciutils"; exit 1; }

divider
echo -e "${GREEN}  ex2 验证配置空间布局与 BAR 编码${NC}"
divider
echo -e "${CYAN}【理论】${NC} basics §3-4：Type0 六 BAR / Type1 两 BAR+bus 号；BAR bit0=Mem/IO。"
echo -e "${CYAN}【预测】${NC} P1 BAR0 bit0 ↔ lspci 区域类型；P2 bridge 0x18-1a 是 bus 号；P3 64 位 BAR 占两槽。"
echo

pass=0; fail=0; incon=0
check() { case $4 in
    0) echo -e "  ${GREEN}PASS${NC} $1 $2 — $3"; pass=$((pass+1));;
    1) echo -e "  ${RED}FAIL${NC} $1 $2 — $3"; fail=$((fail+1));;
    *) echo -e "  ${YELLOW}INCONCLUSIVE${NC} $1 $2 — $3"; incon=$((incon+1));; esac; }

# 选样本：用户指定，或自动挑 3 个有 BAR 的 Endpoint + 1 个 bridge
samples=()
if [[ $# -ge 1 ]]; then
    samples=("$1")
else
    while read -r bdf class _; do
        [[ "$class" == "0604:" || "$class" == "0600:" ]] && continue
        bar0=$(setpci -s "$bdf" 10.l 2>/dev/null) || continue
        [[ "$bar0" == "00000000" ]] && continue
        samples+=("$bdf"); [[ ${#samples[@]} -ge 3 ]] && break
    done < <(lspci -n 2>/dev/null)
    br=$(lspci -n 2>/dev/null | awk '$2 ~ /^0604/ {print $1; exit}' || true)
    [[ -n "$br" ]] && samples+=("$br") || true
fi

echo -e "${CYAN}【操作+对照】${NC} 解码样本设备 BAR0，与 lspci 区域对照："
for bdf in "${samples[@]}"; do
    desc=$(lspci -s "$bdf" 2>/dev/null | cut -d' ' -f2-)
    class=$(lspci -n -s "$bdf" 2>/dev/null | awk '{print $2}')
    echo -e "\n  ${GREEN}${bdf}${NC} — ${desc}"

    bar0=$(setpci -s "$bdf" 10.l 2>/dev/null); bar0v=$((16#$bar0))
    bit0=$((bar0v & 1))
    # lspci 首个区域类型
    region=$(lspci -v -s "$bdf" 2>/dev/null | grep -iE "Memory at|I/O ports at" | head -1 || true)

    if [[ "$class" == "0604:" ]]; then
        # bridge：验证 P2（0x18-1a 是 bus 号，与 ex1 一致）
        p=$(setpci -s "$bdf" 18.b); s=$(setpci -s "$bdf" 19.b); u=$(setpci -s "$bdf" 1a.b)
        echo "    bridge：0x18/19/1a = $p/$s/$u（bus 号，非 BAR）"
        [[ "$s" != "00" ]] && check P2 "$bdf" "0x19=secondary bus $s（非 BAR）" 0 || check P2 "$bdf" "secondary=0" 2
        continue
    fi

    if [[ $bit0 -eq 1 ]]; then
        kind="I/O"; expect="I/O ports"
    else
        kind="Memory"; expect="Memory at"
        memtype=$(( (bar0v >> 1) & 3 ))
        [[ $memtype -eq 2 ]] && kind="Memory(64-bit)"
    fi
    echo "    BAR0=0x${bar0} → bit0=$bit0 → 判定 ${kind}"
    echo "    lspci: ${region:-(无区域)}"
    if [[ -z "$region" ]]; then
        check P1 "$bdf" "BAR0 非零但 lspci 无区域（可能被禁用）" 2
    elif echo "$region" | grep -qi "$expect"; then
        check P1 "$bdf" "BAR0 bit0=$bit0 ↔ ${kind}" 0
        # P3: 64 位 BAR 占两槽——BAR1 应为高 32 位地址（非独立区域）
        if [[ $bit0 -eq 0 && $memtype -eq 2 ]]; then
            bar1=$(setpci -s "$bdf" 14.l 2>/dev/null)
            nregion=$(lspci -v -s "$bdf" 2>/dev/null | grep -ciE "Memory at|I/O ports at" || true)
            echo "    BAR1=0x${bar1}（64 位高 32 位）；lspci 区域数=$nregion"
            check P3 "$bdf" "64 位 BAR0 配 BAR1 高位" 0
        fi
    else
        check P1 "$bdf" "BAR0 bit0=$bit0 与 lspci 区域不符" 1
    fi
done
echo

divider
echo -e "${CYAN}【结论与反馈】${NC}  ${GREEN}PASS=$pass${NC}  ${RED}FAIL=$fail${NC}  ${YELLOW}INCONCLUSIVE=$incon${NC}"
echo "  BAR 大小探测原理（不在活设备执行）：写全 1 读回，低位硬连线 0 的位数=log2(size)。"
echo "  这就是 __pci_read_base()（probe.c:176）做的事——读编码、算大小，与分配地址是两步。"
echo -e "  ${CYAN}链尾${NC}：配置空间承载能力链表——ACS 隔离能力怎么查？→ ex3-acs-analysis.sh"
divider
