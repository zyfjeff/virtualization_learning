# Phase 4: vhost 性能实验

## 实验列表

| 实验 | 文件 | 说明 |
|------|------|------|
| vhost 性能对比 | vhost-perf-test.sh | vhost=on vs off 吞吐/延迟对比 |

## 快速开始

```bash
cd /root/code/kvm-study/phase4-virtio/practice/

# 1. 启动 VM (vhost=on)
sudo bash vhost-perf-test.sh setup

# 2. 在 Guest 中启动 iperf3 server
#    tmux attach -t vhost-perf-vm
#    iperf3 -s

# 3. 运行完整测试
sudo bash vhost-perf-test.sh test

# 4. 查看报告
sudo bash vhost-perf-test.sh report

# 5. 清理
sudo bash vhost-perf-test.sh cleanup
```

## 前置条件

```bash
# 安装 iperf3
apt-get install iperf3

# 确认 TAP 设备可用
ip tuntap list

# 确认内核和 initramfs 存在
ls /root/code/kvm-study/scripts/images/

# 确认内核启用了 virtio-net
grep -E "VIRTIO_NET|NETDEVICES" /root/code/linux-6.12.93/.config
```

## 实际测试结果（2026-08-09）

### 测试环境
```
Host: 128 CPU, Linux 6.8.0-51-generic
Guest: Linux 6.12.93-kvm-study (minimal initramfs + iperf3)
网络: virtio-net-pci + TAP + vhost
```

### TCP 吞吐 (10s, 单流)
```
vhost=on:  20.0 Gbps
vhost=off: 22.1 Gbps
```

### UDP 吞吐 (5s, 1400字节包)
```
vhost=on:  发送 2.39 Gbps (213k pps), 接收 1.30 Gbps (46% loss)
vhost=off: 发送 3.79 Gbps (338k pps), 接收 819 Mbps (78% loss)
```

### 分析
```
1. TCP 吞吐差异不大（已饱和内存带宽）
2. vhost=on 的 UDP 接收丢包率更低（46% vs 78%）
3. 说明 vhost 的内核态处理更稳定
4. 当前环境限制：单队列、Guest 未调优、TAP 成为瓶颈
```

### vhost 优势的真实场景
```
vhost 的优势在以下场景更明显：
  1. 高 pps 小包场景（64 字节包）
  2. 多队列 virtio-net（queues=4/8）
  3. CPU 密集场景（减少上下文切换）
  4. 长时间稳定性测试（减少 QEMU 线程调度抖动）
```

## 关键知识点

```
vhost 性能提升的原因:
  1. 数据面卸载到内核（bypass QEMU 用户态）
  2. ioeventfd/irqfd 机制（bypass 系统调用）
  3. kthread_use_mm() 直接访问 Guest 内存
  4. 批处理优化（减少 sendmsg 调用）
  5. busy polling（降低延迟）

vhost_iotlb 的作用:
  · 用于 Guest 有 vIOMMU 的场景
  · 提供 IOVA→HVA 地址翻译缓存
  · 普通 vhost-net（无 vIOMMU）不需要
```

## 内核配置要求

```
必须启用:
  CONFIG_VIRTIO=y
  CONFIG_VIRTIO_PCI=y
  CONFIG_VIRTIO_NET=y
  CONFIG_NETDEVICES=y
  CONFIG_NET=y
  CONFIG_INET=y

构建 initramfs:
  bash /root/code/kvm-study/scripts/testing/build-rootfs-iperf.sh
```
