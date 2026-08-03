#!/bin/bash
# 实验 3: PI 中断追踪
# 使用 ftrace 追踪 Posted Interrupt 的完整投递路径
#
# 知识点:
#   · PI 中断投递的完整路径: 设备 → IOMMU → PI Desc → 通知中断 → Guest
#   · kvm_pi_irte_update: IRTE 更新事件
#   · kvm_apicv_accept_irq: APICv 接受中断事件（PI 模式）
#   · kvm_inj_virq: 虚拟中断注入事件（传统模式）
#
# 参考源码:
#   arch/x86/kvm/vmx/vmx.c - vmx_deliver_posted_interrupt()
#   arch/x86/kvm/vmx/vmx.c - vmx_sync_pir_to_irr()

TRACE_DIR="/sys/kernel/debug/tracing"
DEVICE="0000:4b:00.0"
DURATION=${1:-10}

echo "=========================================="
echo " 实验 3: PI 中断追踪"
echo "=========================================="
echo ""

# 检查前置条件
if [ ! -d "$TRACE_DIR" ]; then
    echo "[错误] ftrace 目录不存在: $TRACE_DIR"
    echo "请确认 debugfs 已挂载: mount -t debugfs none /sys/kernel/debug"
    exit 1
fi

# 检查是否有运行中的 VM
QEMU_PID=$(pgrep -f "qemu-system" 2>/dev/null | head -1 || true)
if [ -z "$QEMU_PID" ]; then
    echo "  [!] 未检测到运行中的 VM"
    echo "      建议先启动 VM: sudo bash setup-vfio-vm.sh start"
    echo "      继续运行（可能无法观察到 PI 事件）..."
    echo ""
fi

echo "  追踪时长: ${DURATION}s"
echo "  测试设备: $DEVICE"
echo ""

# --------------------------------------------------
# 辅助函数: 事件路径转换 (kvm:kvm_entry → kvm/kvm_entry)
# --------------------------------------------------
event_path() {
    echo "$1" | tr ':' '/'
}

# --------------------------------------------------
# 1. 配置 ftrace 事件
# --------------------------------------------------
echo "--- 1. 配置 ftrace 事件 ---"

# 清除之前的追踪
echo 0 > "$TRACE_DIR/tracing_on"
echo > "$TRACE_DIR/trace"

# 启用 PI 相关事件
EVENTS=(
    "kvm:kvm_pi_irte_update"
    "kvm:kvm_apicv_accept_irq"
    "kvm:kvm_inj_virq"
    "kvm:kvm_set_irq"
    "kvm:kvm_msi_set_irq"
    "kvm:kvm_apic_accept_irq"
    "kvm:kvm_ack_irq"
    "kvm:kvm_entry"
    "kvm:kvm_exit"
    "irq:irq_handler_entry"
    "irq:irq_handler_exit"
)

echo "  启用的 trace 事件:"
ENABLED_EVENTS=()
for event in "${EVENTS[@]}"; do
    EPATH=$(event_path "$event")
    EVENT_FILE="$TRACE_DIR/events/$EPATH/enable"
    if [ -f "$EVENT_FILE" ]; then
        echo 1 > "$EVENT_FILE"
        echo "    [✓] $event"
        ENABLED_EVENTS+=("$event")
    else
        echo "    [✗] $event (不存在)"
    fi
done

echo ""

# --------------------------------------------------
# 2. 提示生成负载
# --------------------------------------------------
echo "--- 2. 生成中断负载 ---"
echo ""
echo "  请在 Guest 内生成 I/O 负载（另一个终端）:"
echo "    tmux attach -t pi-test-vm"
echo "    dd if=/dev/vda of=/dev/null bs=4k count=10000"
echo ""
echo "  或者等待 ${DURATION}s 观察自然中断..."
echo ""

# --------------------------------------------------
# 3. 开始追踪
# --------------------------------------------------
echo "--- 3. 开始追踪 (${DURATION}s) ---"
echo ""

echo 1 > "$TRACE_DIR/tracing_on"
sleep "$DURATION"
echo 0 > "$TRACE_DIR/tracing_on"

echo "  追踪完成！"
echo ""

# --------------------------------------------------
# 4. 分析结果
# --------------------------------------------------
echo "--- 4. 追踪结果分析 ---"
echo ""

TRACE_OUTPUT=$(cat "$TRACE_DIR/trace")

# 4.1 统计各类事件数量
echo "  事件统计:"
echo "  ─────────────────────────────────────────"

for event in "${ENABLED_EVENTS[@]}"; do
    EVENT_NAME=$(echo "$event" | cut -d: -f2)
    COUNT=$(echo "$TRACE_OUTPUT" | grep -c " ${EVENT_NAME}:" 2>/dev/null)
    COUNT=${COUNT:-0}
    printf "    %-35s %6d 次\n" "$EVENT_NAME" "$COUNT"
done

echo ""

# 4.2 PI 相关事件详情
echo "  PI 相关事件详情:"
echo "  ─────────────────────────────────────────"

# APICv 接受中断（PI 模式）
APICV_EVENTS=$(echo "$TRACE_OUTPUT" | grep "kvm_apicv_accept_irq" | head -5)
if [ -n "$APICV_EVENTS" ]; then
    echo ""
    echo "  [kvm_apicv_accept_irq] PI 模式中断投递:"
    echo "$APICV_EVENTS" | while IFS= read -r line; do
        echo "    $line"
    done
fi

# 虚拟中断注入（传统模式）
INJ_EVENTS=$(echo "$TRACE_OUTPUT" | grep "kvm_inj_virq" | head -5)
if [ -n "$INJ_EVENTS" ]; then
    echo ""
    echo "  [kvm_inj_virq] 传统模式中断注入:"
    echo "$INJ_EVENTS" | while IFS= read -r line; do
        echo "    $line"
    done
fi

# IRTE 更新
IRTE_EVENTS=$(echo "$TRACE_OUTPUT" | grep "kvm_pi_irte_update" | head -5)
if [ -n "$IRTE_EVENTS" ]; then
    echo ""
    echo "  [kvm_pi_irte_update] IRTE 更新:"
    echo "$IRTE_EVENTS" | while IFS= read -r line; do
        echo "    $line"
    done
fi

if [ -z "$APICV_EVENTS" ] && [ -z "$INJ_EVENTS" ] && [ -z "$IRTE_EVENTS" ]; then
    echo "  未捕获到 PI 相关事件"
    echo "  （请确认 VM 正在运行且有 I/O 负载）"
fi

echo ""

# 4.3 中断向量分布
echo "  中断向量分布 (Top 10):"
echo "  ─────────────────────────────────────────"

# trace 格式: "kvm_apicv_accept_irq: apicid 1 vec 236 (Fixed|edge)"
VECTORS=$(echo "$TRACE_OUTPUT" | grep -oP 'vec \K[0-9]+' 2>/dev/null | sort -n | uniq -c | sort -rn | head -10)
if [ -n "$VECTORS" ]; then
    echo "$VECTORS" | while read count vector; do
        printf "    vector 0x%02x (dec %3d): %6d 次\n" "$vector" "$vector" "$count"
    done
else
    echo "    无数据"
fi

echo ""

# 4.4 VM-Exit 原因分布
echo "  VM-Exit 原因分布 (Top 10):"
echo "  ─────────────────────────────────────────"

# trace 格式: "kvm_exit: vcpu 1 reason MSR_WRITE rip ..."
EXITS=$(echo "$TRACE_OUTPUT" | grep " kvm_exit:" | grep -oP 'reason \K[A-Z_]+' 2>/dev/null | sort | uniq -c | sort -rn | head -10)
if [ -n "$EXITS" ]; then
    echo "$EXITS" | while read count reason; do
        printf "    %-25s: %6d 次\n" "$reason" "$count"
    done
else
    echo "    无数据"
fi

echo ""

# --------------------------------------------------
# 5. PI vs 传统注入对比
# --------------------------------------------------
echo "--- 5. PI vs 传统注入对比 ---"
echo ""

APICV_COUNT=$(echo "$TRACE_OUTPUT" | grep -c " kvm_apicv_accept_irq:" 2>/dev/null)
APICV_COUNT=${APICV_COUNT:-0}
INJ_COUNT=$(echo "$TRACE_OUTPUT" | grep -c " kvm_inj_virq:" 2>/dev/null)
INJ_COUNT=${INJ_COUNT:-0}
EXT_INTR_EXIT=$(echo "$TRACE_OUTPUT" | grep " kvm_exit:" | grep -c "reason EXTERNAL_INTERRUPT" 2>/dev/null)
EXT_INTR_EXIT=${EXT_INTR_EXIT:-0}

echo "  PI 模式中断 (kvm_apicv_accept_irq): $APICV_COUNT 次"
echo "  传统注入 (kvm_inj_virq):            $INJ_COUNT 次"
echo "  EXTERNAL_INTERRUPT VM-Exit:          $EXT_INTR_EXIT 次"
echo ""

if [ "$APICV_COUNT" -gt 0 ]; then
    echo "  分析:"
    echo "    · PI 模式中断通过 vmx_deliver_posted_interrupt() 投递"
    echo "    · 硬件自动处理 PIR→VIRR 同步"
    echo "    · 如果 vCPU 在 Guest 模式，零 VM-Exit"
elif [ "$INJ_COUNT" -gt 0 ]; then
    echo "  分析:"
    echo "    · 当前中断通过传统模式注入（VM-Entry 注入）"
    echo "    · 可能原因: APICv 未启用，或设备未使用 PI 模式"
else
    echo "  分析:"
    echo "    · 未捕获到中断事件"
    echo "    · 请确认 VM 正在运行且有 I/O 负载"
fi

echo ""

# --------------------------------------------------
# 6. 清理
# --------------------------------------------------
echo "--- 6. 清理 ---"

for event in "${ENABLED_EVENTS[@]}"; do
    EPATH=$(event_path "$event")
    EVENT_FILE="$TRACE_DIR/events/$EPATH/enable"
    if [ -f "$EVENT_FILE" ]; then
        echo 0 > "$EVENT_FILE"
    fi
done

echo "  已关闭所有 trace 事件"

echo ""
echo "=========================================="
echo " 实验 3 完成"
echo "=========================================="
echo ""
echo "  思考题:"
echo "  1. 为什么 PI 模式中断不需要 kvm_inj_virq？"
echo "     (提示: PI 通过硬件自动投递，不需要软件注入)"
echo "  2. EXTERNAL_INTERRUPT VM-Exit 中有多少是 PI 通知中断？"
echo "     (提示: PI 通知中断的向量是 0xf7)"
echo "  3. kvm_apicv_accept_irq 和 kvm_inj_virq 的区别是什么？"
echo "     (提示: 前者是 PI 模式，后者是传统模式)"
