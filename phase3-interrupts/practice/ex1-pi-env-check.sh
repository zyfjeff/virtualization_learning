#!/bin/bash
# 实验 1: PI 环境检查
# 检查系统是否支持 Posted Interrupts，以及当前的配置状态
#
# 知识点:
#   · APICv 是 PI 的前提条件
#   · "process posted interrupts" VM-execution control
#   · PI 需要 IOMMU 中断重映射支持
#
# 参考源码:
#   arch/x86/kvm/vmx/capabilities.h - cpu_has_vmx_apicv()
#   arch/x86/kvm/vmx/vmx.c - vmx_hardware_setup()

set -e

echo "=========================================="
echo " 实验 1: PI 环境检查"
echo "=========================================="
echo ""

# 颜色定义
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass() { echo -e "  ${GREEN}[✓]${NC} $1"; }
fail() { echo -e "  ${RED}[✗]${NC} $1"; }
info() { echo -e "  ${YELLOW}[i]${NC} $1"; }

# --------------------------------------------------
# 1. CPU 虚拟化支持
# --------------------------------------------------
echo "--- 1. CPU 虚拟化支持 ---"

if grep -q "vmx" /proc/cpuinfo; then
    pass "CPU 支持 VMX (Intel VT-x)"
else
    fail "CPU 不支持 VMX"
    exit 1
fi

if grep -q "ept" /proc/cpuinfo; then
    pass "CPU 支持 EPT"
else
    fail "CPU 不支持 EPT"
fi

if grep -q "vpid" /proc/cpuinfo; then
    pass "CPU 支持 VPID"
else
    info "CPU 不支持 VPID（可选）"
fi

echo ""

# --------------------------------------------------
# 2. APICv 支持
# --------------------------------------------------
echo "--- 2. APICv 支持 ---"

# 检查 CPU 是否支持 APICv 相关特性
APICV_FEATURES=("tpr_shadow" "flexpriority" "vapic" "posted_intr")
for feat in "${APICV_FEATURES[@]}"; do
    if grep -q "$feat" /proc/cpuinfo; then
        pass "CPU 支持 $feat"
    else
        fail "CPU 不支持 $feat"
    fi
done

# 检查 KVM 模块参数
if [ -f /sys/module/kvm_intel/parameters/enable_apicv ]; then
    APICV_STATUS=$(cat /sys/module/kvm_intel/parameters/enable_apicv)
    if [ "$APICV_STATUS" = "Y" ] || [ "$APICV_STATUS" = "1" ]; then
        pass "kvm_intel enable_apicv = $APICV_STATUS (已启用)"
    else
        fail "kvm_intel enable_apicv = $APICV_STATUS (未启用!)"
        info "启用方法: modprobe -r kvm_intel && modprobe kvm_intel enable_apicv=1"
    fi
else
    fail "无法读取 kvm_intel 参数"
fi

echo ""

# --------------------------------------------------
# 3. IOMMU 和中断重映射
# --------------------------------------------------
echo "--- 3. IOMMU 和中断重映射 ---"

# 检查 IOMMU 是否启用
if dmesg | grep -qi "DMAR.*IOMMU"; then
    pass "IOMMU (DMAR) 已初始化"
else
    fail "IOMMU 未初始化"
    info "启用方法: 内核参数添加 intel_iommu=on"
fi

# 检查中断重映射
if dmesg | grep -qi "DMAR-IR\|interrupt remapping.*enabled\|IRQ remapping.*enabled"; then
    pass "中断重映射 (IR) 已启用"
else
    fail "中断重映射未启用"
    info "启用方法: 内核参数添加 intel_iommu=on,irq_remap=on"
fi

# 检查 Posted Interrupts 支持
if dmesg | grep -qi "posted.*interrupt\|PI.*support"; then
    pass "Posted Interrupts 已启用"
else
    info "未检测到明确的 PI 启用消息（可能正常）"
fi

echo ""

# --------------------------------------------------
# 4. KVM 模块状态
# --------------------------------------------------
echo "--- 4. KVM 模块状态 ---"

if lsmod | grep -q "kvm_intel"; then
    pass "kvm_intel 模块已加载"

    # 显示关键参数
    echo "  kvm_intel 关键参数:"
    for param in enable_apicv enable_vpid enable_ept enable_shadow_vmcs \
                 nested enable_unrestricted_guest; do
        if [ -f "/sys/module/kvm_intel/parameters/$param" ]; then
            val=$(cat "/sys/module/kvm_intel/parameters/$param")
            info "  $param = $val"
        fi
    done
else
    fail "kvm_intel 模块未加载"
fi

echo ""

# --------------------------------------------------
# 5. 测试设备检查
# --------------------------------------------------
echo "--- 5. 测试设备检查 (0000:4b:00.0) ---"

DEVICE="0000:4b:00.0"

if lspci -s "$DEVICE" > /dev/null 2>&1; then
    DEVICE_NAME=$(lspci -s "$DEVICE" | sed 's/^[^ ]* //')
    pass "设备存在: $DEVICE_NAME"

    # 检查 MSI-X 支持
    if lspci -s "$DEVICE" -vvv 2>/dev/null | grep -q "MSI-X"; then
        MSIX_COUNT=$(lspci -s "$DEVICE" -vvv 2>/dev/null | grep "MSI-X" | grep -oP 'Count=\K[0-9]+')
        pass "设备支持 MSI-X (Count=$MSIX_COUNT)"
    elif lspci -s "$DEVICE" -vvv 2>/dev/null | grep -q "MSI"; then
        pass "设备支持 MSI"
    else
        fail "设备不支持 MSI/MSI-X"
    fi

    # 检查 IOMMU group
    IOMMU_GROUP=$(basename $(readlink /sys/bus/pci/devices/$DEVICE/iommu_group 2>/dev/null) 2>/dev/null)
    if [ -n "$IOMMU_GROUP" ]; then
        pass "设备在 IOMMU group $IOMMU_GROUP"
    else
        fail "设备不在任何 IOMMU group 中"
    fi

    # 检查中断重映射
    if cat /proc/interrupts | grep -q "IR-.*$DEVICE"; then
        pass "设备中断使用了中断重映射 (IR- 前缀)"
        IRQ_COUNT=$(cat /proc/interrupts | grep "IR-.*$DEVICE" | wc -l)
        info "  设备有 $IRQ_COUNT 个 IRQ 向量"
    else
        info "设备中断未使用中断重映射"
    fi

    # 显示当前中断分布
    echo ""
    echo "  当前中断分布:"
    cat /proc/interrupts | grep "$DEVICE" | while read line; do
        IRQ=$(echo "$line" | awk '{print $1}' | tr -d ':')
        NAME=$(echo "$line" | awk '{print $NF}')
        # 统计非零中断的 CPU
        ACTIVE_CPUS=$(echo "$line" | awk '{for(i=2;i<NF;i++) if($i>0) printf "%d ", i-2}')
        TOTAL=$(echo "$line" | awk '{sum=0; for(i=2;i<NF;i++) sum+=$i; print sum}')
        info "  IRQ $IRQ ($NAME): 总计 $TOTAL 次, 活跃 CPU: $ACTIVE_CPUS"
    done
else
    fail "设备 $DEVICE 不存在"
fi

echo ""

# --------------------------------------------------
# 6. debugfs 检查
# --------------------------------------------------
echo "--- 6. debugfs 检查 ---"

if mount | grep -q debugfs; then
    pass "debugfs 已挂载"

    if [ -d /sys/kernel/debug/iommu/intel ]; then
        pass "IOMMU debugfs 目录存在"

        if [ -f /sys/kernel/debug/iommu/intel/ir_translation_struct ]; then
            pass "IRTE 表可访问"
        else
            fail "IRTE 表不可访问"
        fi
    else
        fail "IOMMU debugfs 目录不存在"
    fi
else
    fail "debugfs 未挂载"
    info "挂载方法: mount -t debugfs none /sys/kernel/debug"
fi

echo ""

# --------------------------------------------------
# 7. 总结
# --------------------------------------------------
echo "=========================================="
echo " 检查总结"
echo "=========================================="
echo ""

# 判断 PI 是否可用
APICV=$(cat /sys/module/kvm_intel/parameters/enable_apicv 2>/dev/null)
IR_ENABLED=$(dmesg | grep -ci "DMAR-IR\|interrupt remapping.*enabled\|IRQ remapping.*enabled" 2>/dev/null)

if [ "$APICV" = "Y" ] || [ "$APICV" = "1" ]; then
    if [ "$IR_ENABLED" -gt 0 ]; then
        pass "PI 环境就绪！可以进行后续实验。"
        echo ""
        echo "  下一步: sudo bash ex2-irte-observe.sh"
    else
        fail "中断重映射未启用，PI 不可用"
        info "请检查内核参数: intel_iommu=on,irq_remap=on"
    fi
else
    fail "APICv 未启用，PI 不可用"
    info "启用方法: modprobe -r kvm_intel && modprobe kvm_intel enable_apicv=1"
fi

echo ""
echo "=========================================="
echo " 实验 1 完成"
echo "=========================================="
