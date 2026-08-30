#!/bin/bash
# 实验 4: PI vs Remapped 性能对比
# 对比 PI 模式和 Remapped 模式的中断处理性能
#
# 知识点:
#   · PI 模式: 零 VM-Exit，硬件自动处理
#   · Remapped 模式: 需要 Host 内核处理，有 VM-Exit
#   · 通过禁用/启用 APICv 来切换模式
#
# 参考源码:
#   arch/x86/kvm/vmx/vmx.c - vmx_deliver_interrupt()
#   arch/x86/kvm/vmx/vmx.c - vmx_deliver_posted_interrupt()

set -e

echo "=========================================="
echo " 实验 4: PI vs Remapped 性能对比"
echo "=========================================="
echo ""

DEVICE="0000:4b:00.0"
DURATION=${1:-10}

# --------------------------------------------------
# 前置检查
# --------------------------------------------------
echo "--- 前置检查 ---"

APICV_FILE="/sys/module/kvm_intel/parameters/enable_apicv"
if [ ! -f "$APICV_FILE" ]; then
    echo "[错误] 无法访问 APICv 参数文件"
    exit 1
fi

CURRENT_APICV=$(cat "$APICV_FILE")
echo "  当前 APICv 状态: $CURRENT_APICV"
echo "  测试时长: ${DURATION}s"
echo ""

# --------------------------------------------------
# 函数: 收集性能数据
# --------------------------------------------------
collect_perf_data() {
    local MODE=$1
    local LABEL=$2

    echo "  [$LABEL] 收集数据中 (${DURATION}s)..."

    # 记录开始时的中断计数
    local START_IRQS=$(cat /proc/interrupts | grep "$DEVICE" | awk '{sum=0; for(i=2;i<NF;i++) sum+=$i; print sum}')

    # 使用 perf kvm stat 收集 VM-Exit 数据
    local PERF_OUTPUT=$(perf kvm stat record -a sleep "$DURATION" 2>&1 || true)
    local PERF_REPORT=$(perf kvm stat report 2>&1 || true)

    # 记录结束时的中断计数
    local END_IRQS=$(cat /proc/interrupts | grep "$DEVICE" | awk '{sum=0; for(i=2;i<NF;i++) sum+=$i; print sum}')

    local IRQ_RATE=$(( (END_IRQS - START_IRQS) / DURATION ))

    echo ""
    echo "  [$LABEL] 结果:"
    echo "    中断速率: ~$IRQ_RATE 次/秒"
    echo ""

    # 提取关键 VM-Exit 统计
    if [ -n "$PERF_REPORT" ]; then
        echo "    VM-Exit 统计:"
        echo "$PERF_REPORT" | grep -E "EXTERNAL_INTERRUPT|Total" | head -5 | while read line; do
            echo "      $line"
        done
    fi

    echo ""

    # 保存结果
    echo "$LABEL|$IRQ_RATE" >> /tmp/pi-perf-results.txt
}

# --------------------------------------------------
# 测试 1: 当前模式 (APICv + PI)
# --------------------------------------------------
echo "--- 测试 1: 当前模式 (APICv=$CURRENT_APICV) ---"
echo ""

rm -f /tmp/pi-perf-results.txt

if [ "$CURRENT_APICV" = "Y" ] || [ "$CURRENT_APICV" = "1" ]; then
    collect_perf_data "PI" "PI 模式 (APICv 启用)"
else
    collect_perf_data "Remapped" "Remapped 模式 (APICv 禁用)"
fi

# --------------------------------------------------
# 测试 2: 切换模式
# --------------------------------------------------
echo "--- 测试 2: 切换模式 ---"
echo ""

if [ "$CURRENT_APICV" = "Y" ] || [ "$CURRENT_APICV" = "1" ]; then
    echo "  禁用 APICv (切换到 Remapped 模式)..."
    echo 0 > "$APICV_FILE" 2>/dev/null || {
        echo "  [警告] 无法动态修改 APICv 参数"
        echo "  需要重新加载模块: modprobe -r kvm_intel && modprobe kvm_intel enable_apicv=0"
        echo ""
        echo "  跳过模式切换测试"
        echo ""
    }

    NEW_APICV=$(cat "$APICV_FILE" 2>/dev/null || echo "unknown")
    if [ "$NEW_APICV" = "N" ] || [ "$NEW_APICV" = "0" ]; then
        echo "  APICv 已禁用"
        collect_perf_data "Remapped" "Remapped 模式 (APICv 禁用)"

        # 恢复
        echo "  恢复 APICv..."
        echo 1 > "$APICV_FILE" 2>/dev/null || true
    fi
else
    echo "  启用 APICv (切换到 PI 模式)..."
    echo 1 > "$APICV_FILE" 2>/dev/null || {
        echo "  [警告] 无法动态修改 APICv 参数"
        echo "  需要重新加载模块: modprobe -r kvm_intel && modprobe kvm_intel enable_apicv=1"
        echo ""
        echo "  跳过模式切换测试"
        echo ""
    }

    NEW_APICV=$(cat "$APICV_FILE" 2>/dev/null || echo "unknown")
    if [ "$NEW_APICV" = "Y" ] || [ "$NEW_APICV" = "1" ]; then
        echo "  APICv 已启用"
        collect_perf_data "PI" "PI 模式 (APICv 启用)"

        # 恢复
        echo "  恢复 APICv..."
        echo 0 > "$APICV_FILE" 2>/dev/null || true
    fi
fi

# --------------------------------------------------
# 结果对比
# --------------------------------------------------
echo "--- 结果对比 ---"
echo ""

if [ -f /tmp/pi-perf-results.txt ]; then
    echo "  ┌──────────────┬──────────────┐"
    echo "  │  模式         │  中断速率     │"
    echo "  ├──────────────┼──────────────┤"
    while IFS='|' read mode rate; do
        printf "  │  %-12s │  %10s  │\n" "$mode" "$rate/s"
    done < /tmp/pi-perf-results.txt
    echo "  └──────────────┴──────────────┘"
fi

echo ""
echo "  分析要点:"
echo "    · PI 模式下，中断通过硬件自动处理"
echo "    · Remapped 模式下，中断需要 Host 内核处理"
echo "    · PI 模式应该有更低的 VM-Exit 次数"
echo "    · PI 模式应该有更低的延迟"
echo ""
echo "  注意:"
echo "    · 如果中断负载很低，差异可能不明显"
echo "    · 建议在高 I/O 负载下测试 (如 fio 随机读写)"
echo "    · 动态修改 APICv 参数可能不支持，需要重新加载模块"

echo ""
echo "=========================================="
echo " 实验 4 完成"
echo "=========================================="
echo ""
echo "  下一步: sudo bash ex5-on-sn-observe.sh"

# 清理
rm -f /tmp/pi-perf-results.txt
