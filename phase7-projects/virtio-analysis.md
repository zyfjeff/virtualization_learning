# 实践项目：Virtio 性能与行为分析

> 目标：深入分析 virtio 设备在真实工作负载下的行为，对比不同传输层和加速方案的性能

---

## 🎯 项目目标

1. 追踪 virtio I/O 路径，观察 vring 数据流转
2. 对比 QEMU 用户空间后端 vs vhost 内核后端的性能
3. 分析 virtio-net / virtio-blk 的 VM-Exit 频率
4. 验证 feature 协商结果，理解各功能对性能的影响
5. 对比 split ring 和 packed ring 的性能差异

---

## 📋 前置知识

- 第四阶段：Virtio 架构、vring 结构、vhost 加速
- 理解 PCI 设备配置空间基本概念

---

## 🔧 实验环境

```bash
# 方案 A: virtio-net + QEMU 用户空间后端
qemu-system-x86_64 \
    -enable-kvm -m 2G -smp 2 -cpu host \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &
QEMU_PID=$!

# 方案 B: virtio-net + vhost 加速
qemu-system-x86_64 \
    -enable-kvm -m 2G -smp 2 -cpu host \
    -netdev tap,id=n0,vhost=on -device virtio-net-pci,netdev=n0 \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &
QEMU_PID=$!
```

---

## 📊 实验步骤

### 步骤 1：Virtio 设备信息收集

```bash
#!/bin/bash
# virtio-info.sh
# 收集 Guest 内 virtio 设备信息

echo "=== Virtio 设备信息 ==="

echo ""
echo "--- PCI 设备列表 ---"
lspci | grep -i virtio

echo ""
echo "--- Virtio 设备详情 ---"
for dev in /sys/bus/virtio/devices/virtio*; do
    [ -d "$dev" ] || continue
    echo ""
    echo "设备: $(basename $dev)"
    echo "  驱动: $(readlink $dev/driver 2>/dev/null | xargs basename 2>/dev/null)"
    echo "  状态: $(cat $dev/status 2>/dev/null)"
    
    # Feature 协商结果
    FEATURES=$(cat $dev/features 2>/dev/null)
    DRIVER_FEATURES=$(cat $dev/driver_features 2>/dev/null)
    echo "  设备功能: 0x${FEATURES:-0}"
    echo "  驱动功能: 0x${DRIVER_FEATURES:-0}"
    
    # 队列信息
    echo "  队列:"
    for vq in $dev/virtio*/queues/vq.*; do
        [ -d "$vq" ] || continue
        qname=$(basename $vq)
        echo "    $qname:"
        echo "      大小: $(cat $vq/queue_size 2>/dev/null)"
        echo "      最大大小: $(cat $vq/max_queue_size 2>/dev/null)"
    done
done

echo ""
echo "--- virtio-net 网卡 ---"
ip -br link | grep virtio || echo "(无 virtio 网卡)"
for dev in /sys/class/net/*/device/driver; do
    [ -L "$dev" ] || continue
    drv=$(basename $(readlink $dev))
    iface=$(basename $(dirname $(dirname $dev)))
    echo "  $iface → $drv"
done
```

### 步骤 2：Virtio VM-Exit 追踪

```bash
#!/bin/bash
# virtio-trace.sh
# 追踪 virtio 相关的 VM-Exit

TRACEFS=/sys/kernel/debug/tracing

echo "=== Virtio VM-Exit 追踪 ==="

echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

# 追踪 KVM exit (关注 IO 和中断)
echo kvm:kvm_exit > $TRACEFS/set_event

# 如果有 virtio tracepoint
echo virtio:virtio_config_changed >> $TRACEFS/set_event 2>/dev/null

# 按 QEMU PID 过滤
if [ -n "$QEMU_PID" ]; then
    echo $QEMU_PID > $TRACEFS/set_event_pid 2>/dev/null
fi

echo 1 > $TRACEFS/tracing_on

# 在 Guest 内运行工作负载
echo "追踪 5 秒..."
echo "建议在 Guest 内运行: iperf3 -c server -t 5 (网络) 或 dd (磁盘)"
sleep 5

echo 0 > $TRACEFS/tracing_on

echo ""
echo "=== VM-Exit 原因分布 ==="
cat $TRACEFS/trace | grep kvm_exit | \
    sed 's/.*reason //' | awk '{print $1}' | \
    sort | uniq -c | sort -rn | head -15

echo ""
echo "--- Virtio 相关 Exit 分析 ---"
IO_EXIT=$(cat $TRACEFS/trace | grep kvm_exit | grep -c 'IO_INSTRUCTION')
IRQ_EXIT=$(cat $TRACEFS/trace | grep kvm_exit | grep -c 'EXTERNAL_INTERRUPT')
echo "IO_INSTRUCTION (virtio kick): $IO_EXIT"
echo "EXTERNAL_INTERRUPT (virtio 中断): $IRQ_EXIT"

echo ""
echo "--- 分析 ---"
echo "IO_INSTRUCTION 高频: virtio 队列通知 (kick) 频繁"
echo "  可能优化: 使用 event_idx 减少不必要的 kick"
echo "  或: 切换到 vhost 减少 VM-Exit 开销"
```

### 步骤 3：网络性能对比 (QEMU vs vhost)

```bash
#!/bin/bash
# virtio-net-perf.sh
# 对比 QEMU 和 vhost 的网络性能

echo "=== Virtio-Net 性能对比 ==="

echo ""
echo "--- 测试环境 ---"
echo "Guest: 2 vCPU, 2GB RAM"
echo "Host TAP + iperf3 server"

echo ""
echo "--- 方案 A: QEMU 用户空间后端 ---"
echo "启动参数: -netdev user,id=n0 -device virtio-net-pci,netdev=n0"
echo ""
echo "在 Guest 内运行:"
echo "  iperf3 -c <server_ip> -t 30 -P 1"
echo "  iperf3 -c <server_ip> -t 30 -P 4 (多流)"
echo ""
echo "记录结果:"
echo "  单流带宽: ______ Gbps"
echo "  多流带宽: ______ Gbps"
echo "  CPU 使用率: ______ %"

echo ""
echo "--- 方案 B: vhost-net 加速 ---"
echo "启动参数: -netdev tap,id=n0,vhost=on -device virtio-net-pci,netdev=n0"
echo ""
echo "在 Guest 内运行相同的 iperf3 命令"
echo ""
echo "记录结果:"
echo "  单流带宽: ______ Gbps"
echo "  多流带宽: ______ Gbps"
echo "  CPU 使用率: ______ %"

echo ""
echo "--- 性能提升计算 ---"
echo "带宽提升: vhost / QEMU = _____ 倍"
echo "CPU 降低: (QEMU_CPU - vhost_CPU) / QEMU_CPU = _____ %"

echo ""
echo "--- 分析 ---"
echo "vhost 优势来源:"
echo "  1. 无用户态/内核态切换"
echo "  2. 无额外 socket 层"
echo "  3. 批量处理 (NAPI 风格)"
echo "  4. 零拷贝优化 (sendpage)"
```

### 步骤 4：磁盘 IO 性能测试

```bash
#!/bin/bash
# virtio-blk-perf.sh
# virtio-blk 磁盘性能测试

echo "=== Virtio-Blk 性能测试 ==="

# 在 Guest 内执行

echo "--- 随机读 (4K) ---"
fio --name=randread --ioengine=libaio --direct=1 --bs=4k \
    --iodepth=32 --numjobs=4 --rw=randread --runtime=30 \
    --filename=/dev/vdb --group_reporting 2>/dev/null | tail -5

echo ""
echo "--- 随机写 (4K) ---"
fio --name=randwrite --ioengine=libaio --direct=1 --bs=4k \
    --iodepth=32 --numjobs=4 --rw=randwrite --runtime=30 \
    --filename=/dev/vdb --group_reporting 2>/dev/null | tail -5

echo ""
echo "--- 顺序读 (1M) ---"
fio --name=seqread --ioengine=libaio --direct=1 --bs=1M \
    --iodepth=8 --numjobs=1 --rw=read --runtime=30 \
    --filename=/dev/vdb --group_reporting 2>/dev/null | tail -5

echo ""
echo "--- 分析 ---"
echo "virtio-blk 性能瓶颈:"
echo "  - 小 IO (4K): virtqueue 操作开销 + VM-Exit"
echo "  - 大 IO (1M): 带宽接近原生, 受 TAP/QEMU 处理限制"
echo "  - 优化方向: vhost-blk, io_uring, 多队列"
```

### 步骤 5：Feature 功能对比

```bash
#!/bin/bash
# virtio-feature-test.sh
# 测试不同 feature 对性能的影响

echo "=== Virtio Feature 功能测试 ==="

# 列出常见 virtio-net 特性
echo "--- 启用的特性 ---"
FEATURES=$(cat /sys/bus/virtio/devices/virtio*/features 2>/dev/null)
if [ -n "$FEATURES" ]; then
    FEAT_HEX=$((FEATURES))
    
    check_bit() {
        local bit=$1 name=$2
        if (( FEAT_HEX & (1 << bit) )); then
            echo "  ✓ bit $bit: $name"
        else
            echo "  ✗ bit $bit: $name"
        fi
    }
    
    check_bit 0 "VIRTIO_NET_F_CSUM (校验和卸载)"
    check_bit 5 "VIRTIO_NET_F_MAC (MAC由设备提供)"
    check_bit 11 "VIRTIO_NET_F_HOST_TSO4 (TSOv4)"
    check_bit 12 "VIRTIO_NET_F_HOST_TSO6 (TSOv6)"
    check_bit 15 "VIRTIO_NET_F_MRG_RXBUF (合并RX buffer)"
    check_bit 17 "VIRTIO_NET_F_CTRL_VQ (控制队列)"
    check_bit 20 "VIRTIO_NET_F_MQ (多队列)"
    check_bit 29 "VIRTIO_F_RING_EVENT_IDX (事件索引)"
    check_bit 32 "VIRTIO_F_VERSION_1 (1.0规范)"
fi

echo ""
echo "--- 功能测试 ---"

echo "1. TSO 效果:"
echo "   启用 TSO: 大包发送效率高, 减少包数量"
echo "   禁用: 每次发送都是小包, 增加 virtqueue 压力"
echo "   测试: ethtool -K eth0 tso off/on"

echo ""
echo "2. 多队列效果:"
echo "   启用: 多核并行处理, 减少锁竞争"
echo "   测试: ethtool -L eth0 combined <N>"
echo "   查看: ethtool -l eth0"
```

---

## 📈 预期结果

### 网络性能典型值

```
方案                带宽(Gbps)    PPS(万/秒)    CPU(%)
─────────────────  ──────────   ──────────   ──────
QEMU user           1-3          50-100       80-100
vhost-net           5-10         200-400      30-50
VFIO 直通           10-25+       1000+        10-20
```

### Virtio VM-Exit 典型分布

```
Exit 原因              占比      来源
───────────────────  ────────  ──────────────────
IO_INSTRUCTION       30-50%    virtio kick (队列通知)
EXTERNAL_INTERRUPT   20-40%    virtio 完成中断
EPT_VIOLATION        10-20%    内存访问
HLT                   5-15%    Guest 空闲
其他                  5-10%    TSC, MSR, 等
```

---

## 📝 报告要求

1. **设备信息报告**: 记录 Guest 中 virtio 设备的 feature 协商结果
2. **VM-Exit 分布**: 统计 virtio 操作产生的 VM-Exit 类型和频率
3. **性能对比表**: 填写 QEMU vs vhost vs VFIO 的带宽/PPS/CPU 数据
4. **Feature 影响**: 分析 TSO/多队列/event_idx 对性能的影响
5. **优化建议**: 基于测试结果，给出 virtio 部署优化建议

---

## 📚 延伸阅读

- Phase 4 (phase4-virtio/) — Virtio 架构和源码
- Phase 3 (phase3-interrupts/) — 中断路径 (virtio 完成中断)
- `drivers/virtio/virtio_ring.c` — vring 实现
- `drivers/vhost/net.c` — vhost-net 实现
