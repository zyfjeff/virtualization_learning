#!/bin/bash
# 实验 5: ON/SN 行为观察
# 观察 PI Descriptor 中 ON 和 SN 字段的行为
#
# 知识点:
#   · ON (Outstanding Notification): 防止重复发送通知中断
#   · SN (Suppress Notification): 抑制非紧急中断的通知
#   · 通知发送公式: X = ((ON == 0) & (URG | (SN == 0)))
#
# 参考源码:
#   arch/x86/kvm/vmx/posted_intr.c - vmx_vcpu_pi_load/put()
#   arch/x86/include/asm/posted_intr.h - pi_test_and_set_on()

set -e

echo "=========================================="
echo " 实验 5: ON/SN 行为观察"
echo "=========================================="
echo ""

TRACE_DIR="/sys/kernel/debug/tracing"
DURATION=${1:-10}

# --------------------------------------------------
# 1. 观察通知中断合并 (ON 字段的作用)
# --------------------------------------------------
echo "--- 1. 通知中断合并 (ON 字段) ---"
echo ""
echo "  原理:"
echo "    · 第 1 个中断: ON=0 → 设置 ON=1，发送通知"
echo "    · 第 2-N 个中断: ON=1 → 只写 PIR，不发通知"
echo "    · CPU 处理通知: 清除 ON=0，同步 PIR→VIRR"
echo ""
echo "  观察方法:"
echo "    · 追踪 kvm_posted_intr_ipis 计数器"
echo "    · 对比通知中断数量和实际中断数量"
echo ""

# 检查 /proc/interrupts 中的 PI 通知中断
echo "  当前 PI 通知中断统计:"
if cat /proc/interrupts | grep -q "kvm_posted_intr"; then
    cat /proc/interrupts | grep "kvm_posted_intr" | while read line; do
        NAME=$(echo "$line" | awk '{print $NF}')
        TOTAL=$(echo "$line" | awk '{sum=0; for(i=2;i<NF;i++) sum+=$i; print sum}')
        echo "    $NAME: 总计 $TOTAL 次"
    done
else
    echo "    未找到 kvm_posted_intr 中断（可能需要运行中的 VM）"
fi

echo ""

# --------------------------------------------------
# 2. 追踪 ON 相关事件
# --------------------------------------------------
echo "--- 2. 追踪 PI 通知事件 (${DURATION}s) ---"
echo ""

# 启用相关 trace 事件
EVENTS=(
    "kvm:kvm_apicv_accept_irq"
    "kvm:kvm_pi_irte_update"
    "kvm:kvm_entry"
    "kvm:kvm_exit"
)

echo 0 > "$TRACE_DIR/tracing_on"
echo > "$TRACE_DIR/trace"

for event in "${EVENTS[@]}"; do
    EVENT_FILE="$TRACE_DIR/events/$event/enable"
    if [ -f "$EVENT_FILE" ]; then
        echo 1 > "$EVENT_FILE"
    fi
done

echo "  请在另一个终端生成 I/O 负载..."
echo "  等待 ${DURATION}s..."
echo ""

echo 1 > "$TRACE_DIR/tracing_on"
sleep "$DURATION"
echo 0 > "$TRACE_DIR/tracing_on"

TRACE_OUTPUT=$(cat "$TRACE_DIR/trace")

# 分析结果
APICV_COUNT=$(echo "$TRACE_OUTPUT" | grep -c "kvm_apicv_accept_irq" 2>/dev/null || echo "0")
ENTRY_COUNT=$(echo "$TRACE_OUTPUT" | grep -c "kvm_entry" 2>/dev/null || echo "0")
EXIT_COUNT=$(echo "$TRACE_OUTPUT" | grep -c "kvm_exit" 2>/dev/null || echo "0")
EXT_INTR_COUNT=$(echo "$TRACE_OUTPUT" | grep "kvm_exit" | grep -c "exit_reason=1" 2>/dev/null || echo "0")

echo "  追踪结果:"
echo "    APICv 中断投递: $APICV_COUNT 次"
echo "    VM-Entry:        $ENTRY_COUNT 次"
echo "    VM-Exit:         $EXIT_COUNT 次"
echo "    外部中断 Exit:   $EXT_INTR_COUNT 次"
echo ""

if [ "$APICV_COUNT" -gt 0 ]; then
    RATIO=$(echo "scale=2; $APICV_COUNT / ($EXT_INTR_COUNT + 1)" | bc 2>/dev/null || echo "N/A")
    echo "  分析:"
    echo "    · APICv 中断 / 外部中断 Exit = $RATIO"
    echo "    · 如果比值 > 1，说明多个中断合并到一个通知中"
    echo "    · 这就是 ON 字段的作用：防止重复通知"
fi

echo ""

# 清理
for event in "${EVENTS[@]}"; do
    EVENT_FILE="$TRACE_DIR/events/$event/enable"
    if [ -f "$EVENT_FILE" ]; then
        echo 0 > "$EVENT_FILE"
    fi
done

# --------------------------------------------------
# 3. SN 字段观察
# --------------------------------------------------
echo "--- 3. SN 字段观察 ---"
echo ""
echo "  SN (Suppress Notification) 的作用:"
echo "    · SN=1 时，非紧急中断不发送通知"
echo "    · 紧急中断 (URG=1) 不受 SN 影响"
echo ""
echo "  SN 的设置时机:"
echo "    · vCPU 被抢占时: vmx_vcpu_pi_put() 设置 SN=1"
echo "    · vCPU 恢复运行时: vmx_vcpu_pi_load() 清除 SN=0"
echo ""
echo "  观察方法:"
echo "    · 使用 bpftrace 追踪 pi_set_sn 和 pi_clear_sn"
echo "    · 或者观察 vCPU 调度时的 PI 行为"
echo ""

# 检查是否有 bpftrace
if command -v bpftrace &> /dev/null; then
    echo "  检测到 bpftrace，可以运行以下命令追踪 SN 行为:"
    echo ""
    echo "    sudo bpftrace -e '"
    echo "      kprobe:pi_set_sn {"
    echo "        printf(\"SN set: cpu=%d\\n\", cpu);"
    echo "      }"
    echo "      kprobe:pi_test_and_clear_sn {"
    echo "        printf(\"SN cleared: cpu=%d\\n\", cpu);"
    echo "      }"
    echo "    '"
    echo ""
else
    echo "  bpftrace 未安装，跳过 SN 追踪"
    echo "  安装方法: apt-get install bpftrace"
fi

echo ""

# --------------------------------------------------
# 4. 总结
# --------------------------------------------------
echo "--- 4. 总结 ---"
echo ""
echo "  ON 字段的核心价值:"
echo "    · 合并多个中断到一个通知中"
echo "    · 减少通知中断的数量"
echo "    · 提高高频中断场景的性能"
echo ""
echo "  SN 字段的核心价值:"
echo "    · 在 vCPU 调度时抑制非紧急通知"
echo "    · 避免 vCPU 被不必要地唤醒"
echo "    · 紧急中断不受影响"
echo ""
echo "  通知发送公式:"
echo "    X = ((ON == 0) & (URG | (SN == 0)))"
echo ""
echo "    · ON=1: 不发通知（已有 pending 通知）"
echo "    · URG=1: 总是发通知（紧急中断优先）"
echo "    · SN=1: 不发通知（抑制非紧急中断）"

echo ""
echo "=========================================="
echo " 实验 5 完成"
echo "=========================================="
echo ""
echo "  下一步: sudo bash ex6-vcpu-migration.sh"
