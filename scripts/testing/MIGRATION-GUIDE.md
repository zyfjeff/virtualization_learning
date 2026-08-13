# 测试环境迁移指南

## 变更概述

我们已经整理并统一了测试环境的构建和启动脚本，现在使用统一的方式管理所有测试环境。

## 新方案

### 统一构建脚本

```bash
# 构建 Ubuntu 基础镜像（推荐）
sudo ./build-rootfs-ubuntu.sh

# 或构建 All-in-One 镜像
sudo ./build-rootfs-allinone.sh
```

### 统一启动脚本

```bash
# 使用默认配置启动
./boot-vm-unified.sh ubuntu

# 使用自定义配置
./boot-vm-unified.sh ubuntu --memory 4G --cpus 4 --queues 4

# 使用 user 网络（无需 root）
./boot-vm-unified.sh ubuntu --net user
```

## 旧脚本说明

以下脚本仍然保留，但建议使用新的统一脚本：

| 旧脚本 | 新脚本 | 说明 |
|--------|--------|------|
| `build-rootfs.sh` | `build-rootfs-ubuntu.sh` | 基础 rootfs → Ubuntu rootfs |
| `build-rootfs-simple.sh` | `build-rootfs-allinone.sh` | 简单 rootfs → All-in-One rootfs |
| `build-rootfs-ext4.sh` | `build-rootfs-ubuntu.sh` | ext4 rootfs → Ubuntu rootfs |
| `build-rootfs-iperf.sh` | `build-rootfs-allinone.sh` | iperf rootfs → All-in-One rootfs |
| `boot-vm.sh` | `boot-vm-unified.sh` | 基础启动 → 统一启动 |
| `boot-vm-ext4.sh` | `boot-vm-unified.sh` | ext4 启动 → 统一启动 |
| `boot-vm-9p.sh` | `boot-vm-unified.sh` | 9p 启动 → 统一启动 |

## 迁移步骤

### 1. 构建新镜像

```bash
# 构建 Ubuntu 镜像（推荐）
cd /root/code/kvm-study/scripts/testing
sudo ./build-rootfs-ubuntu.sh
```

### 2. 使用新启动脚本

```bash
# 旧方式
./boot-vm.sh

# 新方式
./boot-vm-unified.sh ubuntu
```

### 3. 清理旧文件（可选）

```bash
# 备份旧脚本
mkdir -p /tmp/old-scripts
mv build-rootfs.sh build-rootfs-simple.sh build-rootfs-ext4.sh build-rootfs-iperf.sh /tmp/old-scripts/
mv boot-vm.sh boot-vm-ext4.sh boot-vm-9p.sh /tmp/old-scripts/

# 或直接删除
rm -f build-rootfs.sh build-rootfs-simple.sh build-rootfs-ext4.sh build-rootfs-iperf.sh
rm -f boot-vm.sh boot-vm-ext4.sh boot-vm-9p.sh
```

## 新特性

### 1. 预装工具

Ubuntu 镜像预装了所有测试工具：

- **网络工具**: iperf3, ethtool, ip, ping, hping3, tcpdump, net-tools
- **系统工具**: lspci, numactl, stress-ng, procps, sysstat
- **性能工具**: perf, bpftrace, strace
- **调试工具**: gdb

### 2. 快捷命令

```bash
# 网络性能测试
run-network-test [server_ip]

# 压力测试
run-stress-test

# Virtio 调优
tune-virtio
```

### 3. 灵活的配置

```bash
# 内存和 CPU
./boot-vm-unified.sh ubuntu --memory 8G --cpus 8

# 多队列
./boot-vm-unified.sh ubuntu --queues 4

# 网络类型
./boot-vm-unified.sh ubuntu --net user  # user 网络
./boot-vm-unified.sh ubuntu --net tap   # TAP 网络（默认）
./boot-vm-unified.sh ubuntu --net none  # 无网络

# 调试模式
./boot-vm-unified.sh ubuntu --debug

# 图形界面
./boot-vm-unified.sh ubuntu --gui
```

## 对比

| 特性 | 旧方案 | 新方案 |
|------|--------|--------|
| 镜像数量 | 4+ 个 | 1 个（统一） |
| 启动脚本 | 3+ 个 | 1 个（统一） |
| 工具安装 | 手动 | 自动 |
| 配置灵活性 | 低 | 高 |
| 维护成本 | 高 | 低 |

## 常见问题

### Q: 旧脚本还能用吗？

A: 可以，旧脚本仍然保留，但建议使用新的统一脚本。

### Q: 如何切换回旧脚本？

A: 直接使用旧脚本名称即可，例如 `./boot-vm.sh`。

### Q: 新镜像和旧镜像有什么区别？

A: 新镜像基于 Ubuntu，包含完整的包管理和所有测试工具。旧镜像基于 busybox，功能有限。

### Q: 能否同时使用新旧脚本？

A: 可以，但建议使用新脚本。新旧脚本使用不同的镜像文件，不会冲突。

## 回滚

如果需要回滚到旧方案：

```bash
# 使用旧脚本
./build-rootfs.sh
./boot-vm.sh
```

## 支持

如有问题，请参考：
- [README-UNIFIED.md](README-UNIFIED.md) - 统一测试环境使用指南
- [README.md](README.md) - 原始文档

---

**更新日期**: 2026-08-13
