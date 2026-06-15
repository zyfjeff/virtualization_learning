# 实践项目 3：VFIO 延迟分析

> 目标：分析 VFIO 设备直通的延迟特征和瓶颈

---

## 🎯 项目目标

通过测量 VFIO 设备直通各个环节的延迟，理解 IOMMU DMA 映射、
设备 MMIO 访问、中断投递的性能特征，找出主要瓶颈。

---

## 📋 前置知识

- 第四阶段：VFIO 架构、DMA 映射
- 第三阶段：中断虚拟化 + VT-d IR (Posted Interrupts、IRTE)
- IOMMU 硬件基础

---

## 🔧 实验环境

```bash
# 需要至少一个可直通的 PCIe 设备
# 以网卡为例

# 检查 IOMMU 和 VFIO 状态
dmesg | grep -i iommu
lsmod | grep vfio

# 查看可用的 PCIe 设备
lspci -nn

# 找到设备的 IOMMU 组
DEVICE="0000:03:00.0"  # 替换为你的设备 BDF
readlink /sys/bus/pci/devices/$DEVICE/iommu_group
# 输出: ../../../../kernel/iommu_groups/N

# 解绑原驱动
echo $DEVICE > /sys/bus/pci/devices/$DEVICE/driver/unbind 2>/dev/null

# 绑定到 VFIO
echo "8086 1533" > /sys/bus/pci/drivers/vfio-pci/new_id
# 或使用 vendor:device 绑定
echo $DEVICE > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null

# 启动带直通的 VM
qemu-system-x86_64 \
    -enable-kvm \
    -m 4G \
    -smp 2 \
    -cpu host \
    -device vfio-pci,host=$DEVICE \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &

QEMU_PID=$!
```

---

## 📊 实验步骤

### 步骤 1：DMA 映射延迟

```bash
#!/bin/bash
# trace-dma-map.sh

TRACEFS=/sys/kernel/debug/tracing

# 设置 DMA 映射跟踪
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

# 跟踪 IOMMU 映射操作
echo iommu_map > $TRACEFS/set_event
echo iommu_unmap >> $TRACEFS/set_event
echo iommu_map_page >> $TRACEFS/set_event 2>/dev/null

# 也跟踪函数调用
echo function > $TRACEFS/current_tracer
echo vfio_dma_do_map > $TRACEFS/set_ftrace_filter
echo vfio_pin_pages_remote >> $TRACEFS/set_ftrace_filter
echo iommu_map >> $TRACEFS/set_ftrace_filter

echo 1 > $TRACEFS/tracing_on

# 在虚拟机内执行 DMA 密集操作
# 例如: 网卡收发大量数据包
# ssh vm "iperf3 -c <server> -t 10"
sleep 15

echo 0 > $TRACEFS/tracing_on

# 分析 DMA 映射延迟
echo "=== DMA 映射延迟分析 ==="
cat $TRACEFS/trace | grep "vfio_dma_do_map" | head -20

echo ""
echo "=== 映射次数统计 ==="
echo "iommu_map 次数: $(cat $TRACEFS/trace | grep "iommu_map" | wc -l)"
echo "iommu_unmap 次数: $(cat $TRACEFS/trace | grep "iommu_unmap" | wc -l)"
```

### 步骤 2：IOMMU 页表遍历开销

```bash
#!/bin/bash
# iommu-tlb-analysis.sh

# 使用 perf 分析 IOMMU 相关事件
echo "=== IOMMU TLB 分析 ==="

# 检查可用的 IOMMU perf 事件
perf list | grep -i iommu

# 记录 IOMMU 相关事件
perf record -e intel_iommu:dtlb_walk -a -- sleep 10 2>/dev/null
perf report --stdio

# 如果 dtlb_walk 不可用，使用通用事件
perf stat -e cache-misses,cache-references,cycles \
    -p $QEMU_PID -- sleep 10
```

### 步骤 3：MMIO 访问延迟

```bash
#!/bin/bash
# mmio-latency-test.sh

TRACEFS=/sys/kernel/debug/tracing

echo "=== MMIO 访问延迟分析 ==="

# 跟踪 VFIO 的 mmap 操作
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

echo vfio_pci_mmap > $TRACEFS/set_event 2>/dev/null
echo kvm_vfio > $TRACEFS/set_event 2>/dev/null

echo 1 > $TRACEFS/tracing_on
sleep 10
echo 0 > $TRACEFS/tracing_on

# 分析 MMIO 访问模式
cat $TRACEFS/trace | head -30

# 在虚拟机内直接测量 MMIO 延迟
# 在 Guest 中运行:
# cat << 'GUEST_EOF' > /tmp/mmio-test.c
# #include <stdio.h>
# #include <sys/mman.h>
# #include <fcntl.h>
# #include <time.h>
#
# int main() {
#     int fd = open("/sys/bus/pci/devices/0000:00:05.0/resource0", O_RDWR);
#     void *map = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
#
#     struct timespec start, end;
#     volatile unsigned int *reg = (unsigned int *)map;
#
#     clock_gettime(CLOCK_MONOTONIC, &start);
#     for (int i = 0; i < 100000; i++) {
#         *reg;  // 读取 MMIO 寄存器
#     }
#     clock_gettime(CLOCK_MONOTONIC, &end);
#
#     double ns = (end.tv_sec - start.tv_sec) * 1e9 +
#                 (end.tv_nsec - start.tv_nsec);
#     printf("平均 MMIO 读取延迟: %.0f ns\n", ns / 100000);
#
#     munmap(map, 4096);
#     close(fd);
#     return 0;
# }
# GUEST_EOF
# gcc -O2 -o /tmp/mmio-test /tmp/mmio-test.c
# /tmp/mmio-test
```

### 步骤 4：中断延迟对比

```bash
#!/bin/bash
# irq-latency-compare.sh

TRACEFS=/sys/kernel/debug/tracing

echo "=== 中断延迟对比测试 ==="

# --- 测试 1: 传统模式（无 PI）---
echo "--- 传统中断模式 ---"

echo > $TRACEFS/trace
echo kvm:kvm_inj_virq > $TRACEFS/set_event
echo kvm:kvm_entry >> $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event

echo 1 > $TRACEFS/tracing_on
# ssh vm "iperf3 -c <server> -t 5 -P 4"  # 产生大量中断
sleep 8
echo 0 > $TRACEFS/tracing_on

LEGACY_EXITS=$(cat $TRACEFS/trace | grep "EXTERNAL_INTERRUPT" | wc -l)
echo "传统模式: $LEGACY_EXITS 次外部中断 VM-Exit"

# --- 测试 2: PI 模式 ---
echo "--- Posted Interrupt 模式 ---"

echo > $TRACEFS/trace
echo 1 > $TRACEFS/tracing_on
# ssh vm "iperf3 -c <server> -t 5 -P 4"
sleep 8
echo 0 > $TRACEFS/tracing_on

PI_EXITS=$(cat $TRACEFS/trace | grep "EXTERNAL_INTERRUPT" | wc -l)
echo "PI 模式: $PI_EXITS 次外部中断 VM-Exit"

echo ""
echo "=== 对比结果 ==="
echo "传统模式 VM-Exit: $LEGACY_EXITS"
echo "PI 模式 VM-Exit:  $PI_EXITS"
if [ $LEGACY_EXITS -gt 0 ]; then
    echo "PI 减少 VM-Exit: $(echo "scale=1; (1-$PI_EXITS/$LEGACY_EXITS)*100" | bc)%"
fi
```

### 步骤 5：网络性能基准

```bash
#!/bin/bash
# network-perf-benchmark.sh

echo "=== 网络性能基准测试 ==="

# 确保虚拟机中有 iperf3
# 在 Host 上启动 iperf3 服务器
iperf3 -s -D

# 测试配置
TESTS=(
    "1流 TCP"
    "4流 TCP"
    "8流 TCP"
    "1流 UDP 1Gbps"
    "4流 UDP 1Gbps"
)

for test in "${TESTS[@]}"; do
    echo "--- $test ---"
    # ssh vm "iperf3 -c <host_ip> $test_params -t 10 --json"
    sleep 12
done

# 清理
killall iperf3 2>/dev/null
```

---

## 📈 预期分析结果

### DMA 映射延迟

```
操作                    延迟            说明
──────────────────────  ──────────────  ──────────────────
vfio_dma_do_map (4KB)   ~5-15 μs       包括页面固定+IOMMU映射
vfio_dma_do_map (2MB)   ~10-30 μs      大页映射
vfio_pin_pages (批量)   ~1-3 μs/page   页面固定
iommu_map               ~2-5 μs        IOMMU 页表更新
iommu_unmap             ~1-3 μs        IOMMU 页表移除
```

### MMIO 访问延迟

```
访问类型              延迟            说明
────────────────────  ──────────────  ──────────────────
直接 MMIO 读取        ~200-500 ns    通过 PCIe 总线
直接 MMIO 写入        ~100-300 ns    写入通常更快
MMIO 批量读取         ~50-100 ns/次  连续访问可能有加速
```

### 中断模式对比

```
中断类型              VM-Exit/秒      延迟       说明
────────────────────  ──────────────  ──────────  ──────────
传统模式 (无 PI)      10K-100K       ~2-5 μs    每次中断都 VM-Exit
APICv (无 PI)         10K-50K        ~1-3 μs    部分由硬件处理
PI 模式               0-1K           ~0.5-1 μs  几乎无 VM-Exit
```

---

## 🔍 瓶颈分析

### 常见瓶颈及优化

```
瓶颈                    原因                    优化方向
──────────────────────  ──────────────────────  ──────────────────
DMA 映射开销           页面固定 + IOMMU 更新    预映射、批量映射
IOMMU TLB miss         IOMMU 页表遍历           大页映射、预取
中断频繁 VM-Exit       无 PI 或 PI 配置不当     启用 PI、合并中断
MMIO 延迟              PCIe 总线延迟            使用 BAR 缓存
内存锁定限制           RLIMIT_MEMLOCK           增大 memlock 限制
```

### 使用 perf 定位瓶颈

```bash
# 分析 KVM+VFIO 热点函数
perf record -g -p $QEMU_PID -- sleep 30
perf report --stdio --sort=dso,sym | grep -E "vfio|kvm|iommu" | head -30

# 分析 IOMMU 开销
perf stat -e cycles,instructions,cache-misses \
    -p $QEMU_PID -- sleep 10
```

---

## 📝 报告要求

1. 测量并记录 DMA 映射/解映射的延迟分布
2. 对比传统中断与 PI 模式下的 VM-Exit 数量
3. 分析 MMIO 访问的延迟特征
4. 识别 VFIO 直通的主要性能瓶颈
5. 提出优化建议（DMA 批量映射、大页、PI 等）
