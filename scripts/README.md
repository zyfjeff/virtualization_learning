# 统一测试环境构建指南

## 概述

本项目现在采用统一的测试环境构建方式，所有实验使用同一个基础镜像，避免重复构建和配置。

## 快速开始

### 1. 构建统一镜像

```bash
# 构建 Ubuntu 基础镜像（推荐）
sudo ./build-rootfs-ubuntu.sh

# 或构建 All-in-One 镜像（包含所有工具）
sudo ./build-rootfs-allinone.sh
```

### 2. 启动 VM

```bash
# 使用默认配置启动
./boot-vm-unified.sh ubuntu

# 使用自定义配置启动
./boot-vm-unified.sh ubuntu --memory 4G --cpus 4 --queues 4

# 使用 user 网络（无需 root 权限创建 TAP）
./boot-vm-unified.sh ubuntu --net user

# 调试模式
./boot-vm-unified.sh ubuntu --debug
```

## 镜像类型

### Ubuntu 镜像（推荐）

**特点：**
- 基于 Ubuntu 22.04 LTS
- 包含完整的 apt 包管理
- 预装所有测试工具
- 适合长期测试和开发

**预装工具：**
- 网络：iperf3, ethtool, ip, ping, hping3, tcpdump
- 系统：lspci, numactl, stress-ng
- 性能：perf, bpftrace, sysstat
- 调试：strace, gdb

**构建命令：**
```bash
sudo ./build-rootfs-ubuntu.sh
```

**输出文件：**
- `../images/initramfs-ubuntu.img` - initramfs 镜像
- `../images/disk-ubuntu.img` - 磁盘镜像（10G）

### All-in-One 镜像

**特点：**
- 基于 busybox
- 复制宿主机的测试工具
- 体积较小
- 适合快速测试

**构建命令：**
```bash
sudo ./build-rootfs-allinone.sh
```

**输出文件：**
- `../images/initramfs-allinone.img` - initramfs 镜像
- `../images/disk-allinone.img` - 磁盘镜像（10G）

## 启动参数说明

### 基本参数

```bash
./boot-vm-unified.sh [镜像类型] [选项]
```

**镜像类型：**
- `ubuntu` - Ubuntu 基础系统（默认）
- `allinone` - All-in-One 系统
- `minimal` - 最小化系统（旧版本）

**选项：**
- `--memory <size>` - 内存大小（默认 2G）
- `--cpus <num>` - CPU 数量（默认 2）
- `--queues <num>` - virtio-net 队列数（默认 1）
- `--net <type>` - 网络类型：tap, user, none（默认 tap）
- `--gui` - 启用图形界面
- `--debug` - 启用调试模式

### 使用示例

#### 1. 标准测试环境

```bash
# 4G 内存，4 CPU，4 队列
./boot-vm-unified.sh ubuntu --memory 4G --cpus 4 --queues 4
```

#### 2. 快速测试（无需 root）

```bash
# 使用 user 网络，无需创建 TAP 设备
./boot-vm-unified.sh ubuntu --net user
```

#### 3. 性能测试

```bash
# 高性能配置
./boot-vm-unified.sh ubuntu --memory 8G --cpus 8 --queues 8
```

#### 4. 调试模式

```bash
# 启用 GDB 调试
./boot-vm-unified.sh ubuntu --debug

# 在另一个终端连接 GDB
gdb
(gdb) target remote :1234
```

## 在 VM 内测试

### 网络性能测试

```bash
# 启动 iperf3 server
run-network-test

# 或手动启动
iperf3 -s

# 在另一个终端运行 client
iperf3 -c <server_ip> -t 30 -P 4
```

### 压力测试

```bash
# 运行完整压力测试
run-stress-test

# 或单独测试
stress-ng --cpu 4 --timeout 60s
stress-ng --vm 2 --vm-bytes 1G --timeout 60s
stress-ng --hdd 2 --timeout 60s
```

### Virtio 调优

```bash
# 自动调优
tune-virtio

# 或手动调优
# 查看当前配置
ethtool -g eth0

# 调整队列大小
ethtool -G eth0 rx 1024 tx 1024

# 启用中断合并
ethtool -C eth0 rx-usecs 50 rx-frames 64

# 查看统计
ethtool -S eth0
```

### 性能分析

```bash
# perf 采样
perf record -g -a sleep 30
perf report

# bpftrace 追踪
bpftrace -e 'kprobe:vhost_net_buf_add { @count++; }'

# strace 跟踪
strace -c -p <pid>
```

## 快捷命令

Ubuntu 镜像提供以下快捷命令：

- `run-network-test [server_ip]` - 网络性能测试
- `run-stress-test` - 完整压力测试
- `tune-virtio` - Virtio 自动调优

## 网络配置

### TAP 网络（推荐）

TAP 网络提供最佳性能，需要 root 权限创建 TAP 设备。

```bash
# 启动脚本会自动创建 TAP 设备
./boot-vm-unified.sh ubuntu

# 手动创建 TAP 设备
sudo ip tuntap add tap0 mode tap
sudo ip link set tap0 up
```

### User 网络

User 网络不需要 root 权限，但性能较低。

```bash
# 使用 user 网络
./boot-vm-unified.sh ubuntu --net user

# SSH 端口转发
ssh -p 2222 root@localhost
```

## 文件结构

```
scripts/testing/
├── build-rootfs-ubuntu.sh      # Ubuntu 镜像构建（推荐）
├── build-rootfs-allinone.sh    # All-in-One 镜像构建
├── build-rootfs.sh             # 基础镜像构建（旧版本）
├── build-rootfs-simple.sh      # 最小化镜像构建（旧版本）
├── build-rootfs-ext4.sh        # ext4 镜像构建（旧版本）
├── build-kernel.sh             # 内核构建
├── boot-vm-unified.sh          # 统一启动脚本（推荐）
├── boot-vm.sh                  # 基础启动脚本（旧版本）
├── boot-vm-ext4.sh             # ext4 启动脚本（旧版本）
├── boot-vm-9p.sh               # 9p 启动脚本（旧版本）
├── test-in-vm.sh               # VM 内测试脚本
├── README-UNIFIED.md           # 本文档
└── README.md                   # 原始文档
```

## 迁移指南

### 从旧脚本迁移

如果你之前使用旧版本的脚本，请按以下步骤迁移：

1. **构建新镜像**
   ```bash
   sudo ./build-rootfs-ubuntu.sh
   ```

2. **使用新启动脚本**
   ```bash
   # 旧方式
   ./boot-vm.sh
   
   # 新方式
   ./boot-vm-unified.sh ubuntu
   ```

3. **删除旧文件（可选）**
   ```bash
   rm -f boot-vm.sh boot-vm-ext4.sh boot-vm-9p.sh
   rm -f build-rootfs.sh build-rootfs-simple.sh build-rootfs-ext4.sh
   ```

## 故障排查

### 1. 无法创建 TAP 设备

**问题：** `RTNETLINK answers: Operation not permitted`

**解决：**
```bash
# 使用 sudo
sudo ./boot-vm-unified.sh ubuntu

# 或使用 user 网络
./boot-vm-unified.sh ubuntu --net user
```

### 2. 内核不存在

**问题：** `内核不存在: /root/code/linux-6.12.93/arch/x86_64/boot/bzImage`

**解决：**
```bash
# 构建内核
./build-kernel.sh
```

### 3. 镜像不存在

**问题：** `initramfs 不存在`

**解决：**
```bash
# 构建镜像
sudo ./build-rootfs-ubuntu.sh
```

### 4. 网络不通

**问题：** VM 内无法访问网络

**解决：**
```bash
# 检查 TAP 设备
ip link show tap0

# 配置 IP（如果需要）
sudo ip addr add 192.168.100.1/24 dev tap0

# 在 VM 内配置 DHCP
udhcpc -i eth0
```

## 性能优化建议

### 1. 使用多队列

```bash
# 启动时指定队列数
./boot-vm-unified.sh ubuntu --queues 4

# 在 VM 内调优
ethtool -G eth0 rx 1024 tx 1024
```

### 2. 启用中断合并

```bash
# 在 VM 内配置
ethtool -C eth0 rx-usecs 50 rx-frames 64
```

### 3. 使用大页内存

```bash
# 启动时启用大页
./boot-vm-unified.sh ubuntu --memory 4G --machine q35,memory-backend=mem0
```

## 参考资料

- [Phase 4 实验文档](../../phase4-virtio/README.md)
- [Virtio 规范](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)
- [QEMU 文档](https://www.qemu.org/docs/master/)

## 更新日志

### 2026-08-13
- 创建统一的测试环境构建脚本
- 支持 Ubuntu 基础镜像
- 支持 All-in-One 镜像
- 统一启动脚本
- 预装所有测试工具
