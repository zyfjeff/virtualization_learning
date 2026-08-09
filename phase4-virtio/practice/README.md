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
```

## 预期结果

```
vhost=on:
  · UDP 吞吐: ~300 万 pps
  · TCP 吞吐: ~20-40 Gbps
  · CPU 占用: 较低

vhost=off:
  · UDP 吞吐: ~100 万 pps
  · TCP 吞吐: ~10-20 Gbps
  · CPU 占用: 较高

性能提升: 2-3 倍
```

## 关键知识点

```
vhost 性能提升的原因:
  1. 数据面卸载到内核（bypass QEMU 用户态）
  2. ioeventfd/irqfd 机制（bypass 系统调用）
  3. kthread_use_mm() 直接访问 Guest 内存
  4. 批处理优化（减少 sendmsg 调用）
  5. busy polling（降低延迟）
```
