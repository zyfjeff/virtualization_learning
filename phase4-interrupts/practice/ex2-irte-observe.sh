#!/bin/bash
# 实验 2: IRTE 观察
# 查看 IOMMU 的 IRTE 表，观察 Posted 模式和 Remapped 模式的 IRTE
#
# 知识点:
#   · IRTE 是 IOMMU 中断重映射表的核心数据结构
#   · IM=0: Remapped 模式（生成新的 MSI）
#   · IM=1: Posted 模式（写入 PI Descriptor）
#   · PDA 字段指向 PI Descriptor 的物理地址
#
# 参考源码:
#   drivers/iommu/intel/debugfs.c - ir_translation_struct_show()
#   include/linux/dmar.h - struct irte

set -e

echo "=========================================="
echo " 实验 2: IRTE 观察"
echo "=========================================="
echo ""

DEVICE="0000:4b:00.0"
IRTE_FILE="/sys/kernel/debug/iommu/intel/ir_translation_struct"

# 检查前置条件
if [ ! -f "$IRTE_FILE" ]; then
    echo "[错误] IRTE 文件不存在: $IRTE_FILE"
    echo "请确认 debugfs 已挂载且 IOMMU 已启用"
    exit 1
fi

# --------------------------------------------------
# 1. 查看完整的 IRTE 表
# --------------------------------------------------
echo "--- 1. 完整 IRTE 表 ---"
echo ""
cat "$IRTE_FILE"
echo ""

# --------------------------------------------------
# 2. 分析 Remapped 模式 IRTE
# --------------------------------------------------
echo "--- 2. Remapped 模式 IRTE (IM=0) ---"
echo ""

REMAPPED_SECTION=$(cat "$IRTE_FILE" | sed -n '/Remapped Interrupt supported/,/^\*\*\*\*/p')
if [ -n "$REMAPPED_SECTION" ]; then
    echo "$REMAPPED_SECTION"
    echo ""

    REMAPPED_COUNT=$(echo "$REMAPPED_SECTION" | grep -c "^ *[0-9]" 2>/dev/null || echo "0")
    echo "  Remapped 模式 IRTE 数量: $REMAPPED_COUNT"
else
    echo "  未找到 Remapped 模式 IRTE"
fi

echo ""

# --------------------------------------------------
# 3. 分析 Posted 模式 IRTE
# --------------------------------------------------
echo "--- 3. Posted 模式 IRTE (IM=1) ---"
echo ""

POSTED_SECTION=$(cat "$IRTE_FILE" | sed -n '/Posted Interrupt supported/,/^\*\*\*\*/p')
if [ -n "$POSTED_SECTION" ]; then
    echo "$POSTED_SECTION"
    echo ""

    POSTED_COUNT=$(echo "$POSTED_SECTION" | grep -c "^ *[0-9]" 2>/dev/null || echo "0")
    echo "  Posted 模式 IRTE 数量: $POSTED_COUNT"
else
    echo "  未找到 Posted 模式 IRTE"
    echo "  （可能需要运行中的 VM 且有直通设备）"
fi

echo ""

# --------------------------------------------------
# 4. 查找测试设备的 IRTE
# --------------------------------------------------
echo "--- 4. 测试设备 $DEVICE 的 IRTE ---"
echo ""

# 将 BDF 格式转换为 lspci 格式
BDF_SHORT=$(echo "$DEVICE" | sed 's/^0000://')
BDF_BUS=$(echo "$BDF_SHORT" | cut -d: -f1)
BDF_DEV=$(echo "$BDF_SHORT" | cut -d: -f2 | cut -d. -f1)
BDF_FUNC=$(echo "$BDF_SHORT" | cut -d. -f2)

echo "  搜索 BDF: $BDF_BUS:$BDF_DEV.$BDF_FUNC"
echo ""

# 在 IRTE 表中搜索设备
DEVICE_IRTES=$(cat "$IRTE_FILE" | grep "$BDF_BUS:$BDF_DEV\.$BDF_FUNC" 2>/dev/null || true)

if [ -n "$DEVICE_IRTES" ]; then
    echo "  找到以下 IRTE:"
    echo "$DEVICE_IRTES"
    echo ""
    echo "  字段解读:"
    echo "    Entry   = IRTE 索引 (Handle)"
    echo "    SrcID   = 设备 BDF (Bus:Device.Function)"
    echo "    PDA     = PI Descriptor 物理地址 (Posted 模式)"
    echo "    Vct     = 通知向量 (通常为 0xf7)"
else
    echo "  未找到设备的 IRTE"
    echo "  （设备可能未启用 MSI/MSI-X，或中断未使用 IR）"
fi

echo ""

# --------------------------------------------------
# 5. IRTE 字段解读练习
# --------------------------------------------------
echo "--- 5. IRTE 字段解读练习 ---"
echo ""
echo "  请观察上面的 IRTE 表，回答以下问题:"
echo ""
echo "  Q1: 有多少个 Remapped 模式的 IRTE？有多少个 Posted 模式的？"
echo "  Q2: Posted 模式 IRTE 的 Vct 字段是什么值？(提示: 通常是 0xf7)"
echo "  Q3: PDA_high 和 PDA_low 组合起来是什么地址？"
echo "  Q4: 同一个设备 (相同 SrcID) 有多少个 IRTE？为什么？"
echo ""
echo "  提示: 每个 MSI-X 向量对应一个 IRTE"
echo "  提示: Posted 模式的 Vct 是通知向量 (POSTED_INTR_VECTOR = 0xf7)"
echo "  提示: PDA = (PDA_high << 32) | (PDA_low << 6)，因为 PI Descriptor 64 字节对齐"

echo ""

# --------------------------------------------------
# 6. 对比分析
# --------------------------------------------------
echo "--- 6. Remapped vs Posted 对比 ---"
echo ""
echo "  ┌─────────────┬──────────────────┬──────────────────┐"
echo "  │   字段       │  Remapped (IM=0) │  Posted (IM=1)   │"
echo "  ├─────────────┼──────────────────┼──────────────────┤"
echo "  │  Vector      │  物理 vector     │  VV (guest vec)  │"
echo "  │  Dest        │  目标 pCPU       │  通过 PDA 间接   │"
echo "  │  PDA         │  无              │  PI Desc 地址    │"
echo "  │  处理方式    │  生成新 MSI      │  写 PI Desc      │"
echo "  │  VM-Exit     │  需要            │  零 VM-Exit      │"
echo "  └─────────────┴──────────────────┴──────────────────┘"

echo ""
echo "=========================================="
echo " 实验 2 完成"
echo "=========================================="
echo ""
echo "  下一步: sudo bash ex3-pi-trace.sh"
