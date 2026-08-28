# 实践项目 4：中断路径跟踪

> 目标：完整跟踪外部设备中断从硬件到 Guest 的路径

---

## 🎯 项目目标

通过跟踪工具记录一个外部设备中断从物理硬件触发到 Guest 中断处理
函数执行的完整路径，验证 APICv 和 Posted Interrupts 的工作机制。

---

## 📋 前置知识

- 第三阶段：中断虚拟化 + VT-d中断重映射 (合并版)
  - vLAPIC、APICv、Posted Interrupts
  - IRTE、中断重映射、PI模式IRTE
  - vmx_pi_update_irte() 桥梁函数
- x86 中断处理基础

---

## 🔧 实验环境

```bash
# 启动 VM（配置 APICv 和 PI）
qemu-system-x86_64 \
    -enable-kvm \
    -m 2G \
    -smp 2 \
    -cpu host,+kvm-pv-eoi \
    -device vfio-pci,host=0000:03:00.0 \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &

QEMU_PID=$!

# 检查 APICv 是否启用
# 在 QEMU monitor 中:
#   info kvm    (查看 APICv 状态)
#   info lapic  (查看 LAPIC 状态)
```

---

## 📊 实验步骤

### 步骤 1：基础中断跟踪

```bash
#!/bin/bash
# trace-irq-basic.sh

TRACEFS=/sys/kernel/debug/tracing

# 设置中断跟踪
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

# 跟踪关键中断事件
echo kvm:kvm_entry > $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_inj_virq >> $TRACEFS/set_event
echo kvm:kvm_ack_irq >> $TRACEFS/set_event

# 跟踪 VMX 相关事件 (仅在启用嵌套虚拟化时有意义)
echo kvm:kvm_nested_vmenter >> $TRACEFS/set_event 2>/dev/null
echo kvm:kvm_nested_vmexit >> $TRACEFS/set_event 2>/dev/null

echo 1 > $TRACEFS/tracing_on

# 在虚拟机内产生中断
# 方法 1: 使用直通网卡
# ssh vm "ping -c 100 <target>"

# 方法 2: 使用 virtio 设备
# ssh vm "iperf3 -c <server> -t 5"

# 方法 3: 直接在 VM 内生成软件中断
# ssh vm "kill -SIGUSR1 <pid>"

sleep 5
echo 0 > $TRACEFS/tracing_on

# 显示中断注入序列
echo "=== 中断注入序列 ==="
cat $TRACEFS/trace | grep -E "kvm_inj_virq|kvm_exit.*EXTERNAL" | head -30
```

### 步骤 2：Posted Interrupt 验证

```bash
#!/bin/bash
# verify-pi.sh

TRACEFS=/sys/kernel/debug/tracing

echo "=== Posted Interrupt 验证 ==="

# 方法 1: 通过 VM-Exit 数量间接验证
echo "--- 方法 1: VM-Exit 统计 ---"

echo > $TRACEFS/trace
echo kvm:kvm_exit > $TRACEFS/set_event

echo 1 > $TRACEFS/tracing_on

# 产生中断（使用直通设备）
# ssh vm "iperf3 -c <server> -t 5 -P 8"
sleep 8
echo 0 > $TRACEFS/tracing_on

# 统计外部中断 VM-Exit
EXT_EXIT=$(cat $TRACEFS/trace | grep "EXTERNAL_INTERRUPT" | wc -l)
TOTAL_EXIT=$(cat $TRACEFS/trace | grep "kvm_exit" | wc -l)

echo "总 VM-Exit: $TOTAL_EXIT"
echo "外部中断 VM-Exit: $EXT_EXIT"
echo "外部中断占比: $(echo "scale=1; $EXT_EXIT/$TOTAL_EXIT*100" | bc)%"

# 如果 PI 正常工作，外部中断 VM-Exit 应该很少

# 方法 2: 直接跟踪 PI 事件（如果内核支持）
echo ""
echo "--- 方法 2: PI 事件跟踪 ---"

echo > $TRACEFS/trace
# 某些内核版本有 PI 相关的 tracepoint
echo kvm_pi_notification > $TRACEFS/set_event 2>/dev/null
echo kvm_pi >> $TRACEFS/set_event 2>/dev/null

echo 1 > $TRACEFS/tracing_on
sleep 5
echo 0 > $TRACEFS/tracing_on

cat $TRACEFS/trace | head -20
```

### 步骤 3：APICv 功能验证

```bash
#!/bin/bash
# verify-apicv.sh

TRACEFS=/sys/kernel/debug/tracing

echo "=== APICv 功能验证 ==="

# APICv 的特征:
# 1. LAPIC 寄存器读写不触发 VM-Exit
# 2. TPR 更新由硬件处理
# 3. EOI 由硬件处理

# 方法 1: 通过 VM-Exit 原因推断
echo > $TRACEFS/trace
echo kvm:kvm_exit > $TRACEFS/set_event

echo 1 > $TRACEFS/tracing_on
# 在 VM 内执行 LAPIC 密集操作
# ssh vm "cat /proc/interrupts > /dev/null"  # 触发 LAPIC 读取
# ssh vm "for i in \$(seq 1 1000); do cat /proc/interrupts > /dev/null; done"
sleep 5
echo 0 > $TRACEFS/tracing_on

# 分析退出原因
echo "VM-Exit 原因分布:"
cat $TRACEFS/trace | grep "kvm_exit" | \
    sed 's/.*reason //' | sed 's/ .*//' | sort | uniq -c | sort -rn | head -10

# 如果 APICv 正常:
# - 不应该有 APIC_ACCESS 退出
# - 不应该有 APIC_WRITE 退出

# 方法 2: 检查 MSR 状态
echo ""
echo "APICv MSR 检查:"
# 通过 rdmsr 检查 APICv 相关 MSR
# rdmsr 0x48  # IA32_VMX_PROCBASED_CTLS2
# 检查 bit 40 (Virtualize APIC accesses)
```

### 步骤 4：完整中断路径时间线

```bash
#!/bin/bash
# full-irq-timeline.sh

TRACEFS=/sys/kernel/debug/tracing

echo "=== 完整中断路径时间线 ==="

# 设置全面跟踪
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

# 所有中断相关事件
echo kvm:kvm_entry > $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_inj_virq >> $TRACEFS/set_event
echo kvm:kvm_ack_irq >> $TRACEFS/set_event

# 调度事件（用于分析 vCPU 调度）
echo sched:sched_switch >> $TRACEFS/set_event

# IRQ 事件
echo irq:irq_handler_entry >> $TRACEFS/set_event
echo irq:irq_handler_exit >> $TRACEFS/set_event
echo irq:softirq_entry >> $TRACEFS/set_event
echo irq:softirq_exit >> $TRACEFS/set_event

echo 1 > $TRACEFS/tracing_on

# 产生一个可追踪的中断事件
# ssh vm "ping -c 1 <gateway>"  # 网络中断
sleep 3
echo 0 > $TRACEFS/tracing_on

# 提取完整的中断时间线
echo "=== 中断时间线 ==="
cat $TRACEFS/trace | \
    grep -E "kvm_entry|kvm_exit|kvm_inj|irq_handler|softirq" | \
    head -50

# 分析单个中断的完整路径
echo ""
echo "=== 单次中断路径示例 ==="
cat $TRACEFS/trace | \
    grep -E "kvm_exit.*EXTERNAL|kvm_inj_virq|irq_handler_entry" | \
    head -10
```

### 步骤 5：中断延迟测量

```bash
#!/bin/bash
# irq-latency-measure.sh

echo "=== 中断延迟测量 ==="

# 方法: 在 Host 和 Guest 中分别打时间戳

# Host 端: 使用 cyclictest 风格的测量
# 在 Guest 中运行 cyclictest
# ssh vm "cyclictest -t1 -p 80 -n -i 1000 -l 10000 -q" > /tmp/cyclictest.txt

# 分析结果
echo "中断延迟统计:"
# cat /tmp/cyclictest.txt | head -5

# 方法 2: 使用 perf kvm 统计
echo ""
echo "=== perf kvm 中断统计 ==="
perf kvm stat record -p $QEMU_PID -- sleep 10
perf kvm stat report

# 方法 3: 使用 trace-cmd
echo ""
echo "=== trace-cmd 分析 ==="
trace-cmd record -e kvm:kvm_exit -e kvm:kvm_entry -e kvm:kvm_inj_virq \
    -p $QEMU_PID -- sleep 5
trace-cmd report | grep -E "EXTERNAL_INTERRUPT|inj_virq" | head -20
```

---

## 📈 预期分析结果

### 中断路径时间线（传统模式）

```
时间线 (μs)       事件                          位置
────────────────  ────────────────────────────  ──────────
0.0               设备产生 MSI                   硬件
0.1               MSI 到达 IOMMU                IOMMU
0.3               IRTE 重映射                    IOMMU
0.5               中断到达 pCPU LAPIC           pCPU
0.7               Host 中断处理                  Host 内核
1.0               KVM 处理 (kvm_handle_irq)     KVM
1.5               VM-Exit (EXTERNAL_INTERRUPT)  VMX
2.0               中断注入到 vLAPIC              KVM
2.5               VM-Entry                       VMX
3.0               Guest 中断处理                 Guest 内核

总延迟: ~3 μs (传统模式)
```

### 中断路径时间线（PI 模式）

```
时间线 (μs)       事件                          位置
────────────────  ────────────────────────────  ──────────
0.0               设备产生 MSI                   硬件
0.1               MSI 到达 IOMMU                IOMMU
0.3               IRTE 重映射 (IM=1, PI模式)    IOMMU
0.5               写入 PI desc PIR[vec]=1       内存
0.6               发送通知中断到 pCPU            IOMMU
0.8               pCPU 收到通知中断             pCPU
1.0               [如果vCPU运行] PIR→IRR        硬件自动
1.5               Guest 中断处理                 Guest 内核

总延迟: ~1.5 μs (PI 模式, vCPU运行中)
无 VM-Exit!
```

### VM-Exit 分布对比

```
中断模式              外部中断 VM-Exit    总 VM-Exit     外部占比
────────────────────  ──────────────────  ────────────   ────────
传统模式              ~10000/s           ~30000/s       ~33%
APICv (无PI)          ~5000/s            ~25000/s       ~20%
PI 模式               ~0-100/s           ~20000/s       <1%
```

---

## 🔍 深入分析

### 分析中断合并效果

```bash
# 查看中断合并配置
cat /sys/bus/pci/devices/0000:03:00.0/msi_bus

# 在 VM 内查看中断合并
# cat /sys/class/net/eth0/queues/rx-0/irq_affinity
# ethtool -c eth0  # 查看中断合并参数
```

### 分析 vCPU halt 状态下的 PI 行为

```bash
# 在 VM 内执行 hlt
# ssh vm "sleep 100"  # 空载时 vCPU 进入 halt

# 跟踪 halt/wakeup
echo kvm:kvm_vcpu_wakeup > /sys/kernel/debug/tracing/set_event
echo kvm:kvm_pv_eoi >> /sys/kernel/debug/tracing/set_event

echo 1 > /sys/kernel/debug/tracing/tracing_on
sleep 3
echo 0 > /sys/kernel/debug/tracing/tracing_on

# 观察 PI 唤醒 halted vCPU 的过程
cat /sys/kernel/debug/tracing/trace
```

---

## 📝 报告要求

1. 画出完整的中断路径时间线（从设备 MSI 到 Guest ISR）
2. 对比传统模式与 PI 模式的 VM-Exit 数量和延迟
3. 验证 APICv 是否正常工作（通过分析 VM-Exit 原因）
4. 分析 vCPU halt 状态下的 PI 唤醒机制
5. 提出中断虚拟化优化建议
