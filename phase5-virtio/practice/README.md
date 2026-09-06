# Phase 5: vhost 性能实验

## 实验列表

| 实验 | 文件 | 说明 |
|------|------|------|
| vhost 性能对比 | vhost-perf-test.sh | vhost=on vs off 吞吐/延迟对比 |

## 快速开始

```bash
cd phase5-virtio/practice/

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
ls ../../scripts/images/

# 确认内核启用了 virtio-net
grep -E "VIRTIO_NET|NETDEVICES" .config

# Guest 内: 确认 feature 协商结果（sysfs 属性 features
# 定义于 drivers/virtio/virtio.c:62；位含义对照
# include/uapi/linux/virtio_net.h 的 VIRTIO_NET_F_*）
cat /sys/bus/virtio/devices/virtio0/features
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
  bash ../../scripts/vm/build-rootfs-iperf.sh
```

---

> 以下练习原先内嵌在 `../README.md` 中，现统一收口到本目录。

## 🧪 Virtio Queue 实战练习

### 实验 1: 生产环境性能基准测试与调优

**目标**：在实际生产负载下测试 virtio 性能，并进行调优

```bash
# 场景 1: 高并发网络场景（模拟 Web 服务器）

# 1. 启动 VM，配置多队列 virtio-net
qemu-system-x86_64 -m 8G -smp 8 \
  -netdev tap,id=net0,vhost=on,queues=4 \
  -device virtio-net-pci,netdev=net0,mq=on,vectors=10 \
  -drive file=disk.img,format=qcow2,if=virtio \
  -nographic

# 2. 在 Guest 内安装测试工具
apt-get install iperf3 nginx wrk

# 3. 测试网络性能（多流并发）
# 启动 iperf3 server
iperf3 -s

# 在 Host 上运行多流测试
iperf3 -c <guest_ip> -P 8 -t 60 -R

# 4. 测试 Web 服务器性能
# 在 Guest 内启动 nginx
nginx

# 在 Host 上运行 wrk 测试
wrk -t8 -c400 -d60s http://<guest_ip>/

# 5. 性能调优
# 查看当前队列配置
ethtool -l eth0

# 调整队列大小
ethtool -G eth0 rx 1024 tx 1024

# 调整 ring buffer
ethtool -G eth0 rx 2048 tx 2048

# 重新测试，对比性能提升
```

**分析要点**：
- 多队列对高并发场景的影响
- 队列大小与性能的关系
- 如何根据负载特征调优

---

### 实验 2: NUMA-aware Virtio 配置与优化

**目标**：理解 NUMA 架构对 virtio 性能的影响，并进行优化

```bash
# 1. 检查 Host NUMA 拓扑
numactl --hardware
numactl --show

# 2. 启动 VM，绑定到特定 NUMA node
numactl --cpunodebind=0 --membind=0 \
  qemu-system-x86_64 -m 8G -smp 8 \
  -netdev tap,id=net0,vhost=on \
  -device virtio-net-pci,netdev=net0 \
  -drive file=disk.img,format=qcow2,if=virtio \
  -nographic

# 3. 在 Guest 内测试性能
iperf3 -c <server_ip> -t 60

# 4. 检查 vhost 线程的 NUMA 亲和性
ps -eo pid,psr,comm | grep vhost
numactl --show

# 5. 优化：将 vhost 线程绑定到正确的 NUMA node
# 查找 vhost 线程 PID
VHOST_PID=$(ps -eo pid,comm | grep vhost | awk '{print $1}')

# 绑定到 VM 所在的 NUMA node
taskset -c 0-7 $VHOST_PID

# 6. 重新测试，对比性能差异
```

**分析要点**：
- NUMA 跨 node 访问的性能损失
- vhost 线程的 NUMA 亲和性
- 如何正确配置 NUMA-aware 的 VM

---

### 实验 3: Virtio 中断风暴排查与优化

**目标**：模拟和排查生产环境中的中断风暴问题

```bash
# 1. 启动 VM，配置 virtio-net
qemu-system-x86_64 -m 4G -smp 4 \
  -netdev tap,id=net0,vhost=on \
  -device virtio-net-pci,netdev=net0 \
  -nographic

# 2. 在 Host 上监控中断情况
watch -n 1 'cat /proc/interrupts | grep -E "vhost|virtio"'

# 3. 在 Guest 内模拟高负载（产生大量小包）
# 使用 ping flood
ping -f -s 64 <target_ip>

# 或使用 hping3
hping3 -S --flood -p 80 <target_ip>

# 4. 观察中断数量激增
# 在 Host 上观察
watch -n 0.1 'cat /proc/interrupts | grep vhost'

# 5. 使用 perf 分析中断热点
perf record -g -a -e irq:irq_handler_entry sleep 10
perf report

# 6. 优化：启用中断合并（Interrupt Coalescing）
# 在 QEMU 启动时添加参数
-device virtio-net-pci,netdev=net0,

# 或使用 ethtool 调整
ethtool -C eth0 rx-usecs 50 rx-frames 64

# 7. 重新测试，对比中断数量
```

**分析要点**：
- 如何识别中断风暴
- 中断合并（Interrupt Coalescing）的原理和配置
- 延迟 vs 吞吐的权衡

---

### 实验 4: Virtio 设备热插拔与迁移测试

**目标**：测试 virtio 设备的热插拔和 live migration

```bash
# 场景 1: Virtio-net 热插拔

# 1. 启动 VM（不带网卡）
qemu-system-x86_64 -m 4G -smp 2 \
  -drive file=disk.img,format=qcow2,if=virtio \
  -nographic -monitor telnet::4545,server,nowait

# 2. 连接到 QEMU monitor
telnet localhost 4545

# 3. 热添加 virtio-net 设备
netdev_add tap,id=net0,vhost=on
device_add virtio-net-pci,netdev=net0,id=net0

# 4. 在 Guest 内验证
ip link show
# 应该看到新添加的网卡

# 5. 热移除设备
device_del net0
netdev_del net0

# 场景 2: Live Migration with Virtio

# 1. 启动源 VM
qemu-system-x86_64 -m 4G -smp 2 \
  -netdev tap,id=net0,vhost=on \
  -device virtio-net-pci,netdev=net0 \
  -drive file=disk.img,format=qcow2,if=virtio \
  -incoming tcp:0:4444 \
  -nographic

# 2. 在 Guest 内运行持续的网络负载
iperf3 -c <server_ip> -t 300 &

# 3. 启动目标 VM（用于接收迁移）
qemu-system-x86_64 -m 4G -smp 2 \
  -netdev tap,id=net0,vhost=on \
  -device virtio-net-pci,netdev=net0 \
  -drive file=disk.img,format=qcow2,if=virtio \
  -incoming tcp:0:4444 \
  -nographic

# 4. 在源 VM monitor 中发起迁移
migrate -d tcp:target_host:4444

# 5. 监控迁移过程
info migrate

# 6. 验证迁移后 virtio 设备状态
ip link show
ethtool -S eth0
```

**分析要点**：
- 热插拔的实现机制
- Live migration 中 virtio 设备的状态保存和恢复
- 迁移过程中的性能影响

---

### 实验 5: Virtio 性能分析与瓶颈定位

**目标**：使用高级工具进行 virtio 性能分析和瓶颈定位

```bash
# 1. 启动 VM，配置 virtio-net
qemu-system-x86_64 -m 8G -smp 8 \
  -netdev tap,id=net0,vhost=on \
  -device virtio-net-pci,netdev=net0 \
  -nographic

# 2. 使用 bpftrace 追踪 virtio 数据路径
bpftrace -e '
kprobe:vhost_net_buf_add {
    @start[tid] = nsecs;
}

kretprobe:vhost_net_buf_add {
    $duration = nsecs - @start[tid];
    @latency = hist($duration);
}

kprobe:vhost_add_used {
    @used_count = count();
}
'

# 3. 在 Guest 内运行网络负载
iperf3 -c <server_ip> -P 4 -t 60

# 4. 使用 perf 进行火焰图分析
perf record -F 99 -a -g -- sleep 30
perf script | ./stackcollapse-perf.pl | ./flamegraph.pl > flame.svg

# 5. 使用 SystemTap 追踪 virtio queue 操作
stap -e '
probe kernel.function("vhost_get_vq_desc") {
    println("vhost_get_vq_desc called, pid=", pid());
}

probe kernel.function("vhost_add_used") {
    println("vhost_add_used called, pid=", pid());
}
'

# 6. 使用 strace 分析 QEMU 系统调用
strace -c -p <qemu_pid>

# 7. 分析瓶颈
# - 是 vhost 线程 CPU 瓶颈？
# - 是 virtio queue 锁竞争？
# - 是内存拷贝开销？
# - 是中断处理开销？
```

**分析要点**：
- 如何使用 bpftrace/perf/SystemTap 进行性能分析
- 如何识别 virtio 数据路径的瓶颈
- 如何根据分析结果进行优化

---

### 实验 6: 自定义 Virtio 后端开发

**目标**：开发一个简单的自定义 virtio 后端，理解 virtio 协议

```c
/* 简化的 virtio-blk 后端示例 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/virtio_blk.h>
#include <linux/virtio_ring.h>

struct virtio_blk_dev {
    int fd;
    void *virtqueue_mem;
    struct vring_virtqueue *vq;
    char *disk_image;
};

int main() {
    struct virtio_blk_dev dev;
    
    /* 1. 打开磁盘镜像 */
    dev.disk_image = mmap_disk_image("disk.img");
    
    /* 2. 初始化 virtqueue */
    dev.virtqueue_mem = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    /* 3. 处理 virtio 请求 */
    while (1) {
        struct virtio_blk_outhdr *hdr;
        struct iovec iov[8];
        int num_iov;
        
        /* 从 avail ring 获取请求 */
        int head = get_request_from_avail(dev.vq, &hdr, iov, &num_iov);
        
        /* 处理请求 */
        switch (hdr->type) {
            case VIRTIO_BLK_T_IN:  // 读请求
                handle_read_request(dev.disk_image, iov, num_iov);
                break;
            case VIRTIO_BLK_T_OUT: // 写请求
                handle_write_request(dev.disk_image, iov, num_iov);
                break;
        }
        
        /* 将完成的请求放入 used ring */
        put_completed_request(dev.vq, head, 0);
        
        /* 通知前端 */
        notify_guest(dev.vq);
    }
    
    return 0;
}
```

**实现要点**：
- 理解 virtio ring 的数据结构
- 实现 avail/used ring 的处理逻辑
- 实现 virtio-blk 协议
- 实现通知机制

---

## 🎯 生产环境最佳实践

### 1. Virtio-net 性能调优清单

```bash
# 网络性能调优

# 1. 启用多队列
-device virtio-net-pci,mq=on,vectors=$((2*N+2))
# N = vCPU 数量

# 2. 调整队列大小
ethtool -G eth0 rx 1024 tx 1024

# 3. 启用中断合并
ethtool -C eth0 rx-usecs 50 rx-frames 64

# 4. 启用 GRO/GSO
ethtool -K eth0 gro on gso on tso on

# 5. NUMA 绑定
numactl --cpunodebind=0 --membind=0 qemu-system-x86_64 ...

# 6. 启用 vhost
-netdev tap,vhost=on
```

### 2. Virtio-blk 性能调优清单

```bash
# 存储性能调优

# 1. 使用 iothread
-object iothread,id=iothread0
-device virtio-blk-pci,drive=drive0,iothread=iothread0

# 2. 启用多队列
-device virtio-blk-pci,num-queues=4

# 3. 调整队列深度
-device virtio-blk-pci,queue-size=256

# 4. 启用 cache 模式
-drive file=disk.img,cache=writeback,discard=unmap

# 5. 使用 native AIO
-drive file=disk.img,aio=native
```

### 3. 监控与告警

```bash
# 监控 virtio 性能指标

# 1. 监控 virtio-net
watch -n 1 'ethtool -S eth0 | grep -E "rx_packets|tx_packets|rx_errors"'

# 2. 监控 virtio-blk
iostat -x 1 | grep vda

# 3. 监控 vhost 线程
top -H -p $(pgrep vhost)

# 4. 设置告警阈值
# - virtio-net: rx_errors > 0
# - virtio-blk: await > 10ms
# - vhost CPU: > 80%
```

---

---

---

## 📊 vhost 对比练习

### 练习1：对比QEMU vs vhost性能

```bash
# 测试1: QEMU用户态后端
qemu-system-x86_64 -m 2G \
  -netdev tap,id=net0 \
  -device virtio-net-pci,netdev=net0 \
  ...

# Guest内运行iperf3
iperf3 -c <server_ip> -t 30

# 测试2: vhost-net后端
qemu-system-x86_64 -m 2G \
  -netdev tap,id=net0,vhost=on \
  -device virtio-net-pci,netdev=net0 \
  ...

# Guest内运行iperf3
iperf3 -c <server_ip> -t 30

# 对比结果
# 预期: vhost比QEMU快3倍
```

### 练习2：vhost线程亲和性

```bash
# 查找vhost线程
ps aux | grep vhost

# 默认: vhost线程可能在多个pCPU间迁移
top -p <vhost_pid>

# 绑定到pCPU 1
taskset -p 0x2 <vhost_pid>

# 再次运行iperf3
iperf3 -c <server_ip> -t 30

# 对比性能
# 预期: 性能提升10-20%
```

### 练习3：跟踪vhost数据路径

```bash
# 启用vhost tracepoints
echo 1 > /sys/kernel/debug/tracing/events/vhost/enable

# 运行iperf3
iperf3 -c <server_ip> -t 10

# 查看trace
cat /sys/kernel/debug/tracing/trace_pipe | grep vhost

# 观察到的事件:
# vhost_tx: vq=0x... len=1514
# vhost_rx: vq=0x... len=1514
```

### 练习4：多队列测试

```bash
# QEMU启用multi-queue (4个vCPU)
qemu-system-x86_64 -smp 4 \
  -netdev tap,id=net0,vhost=on \
  -device virtio-net-pci,netdev=net0,mq=on,vectors=10 \
  ...

# Guest内启用多队列
ethtool -L eth0 combined 4

# 运行iperf3
iperf3 -c <server_ip> -t 30 -P 4  # 4个并发连接

# 观察多核扩展
# 预期: 吞吐提升2-4倍
```

---
