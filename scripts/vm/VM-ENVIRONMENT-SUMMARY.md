# VM 环境设置总结

> 本文档记录了 KVM 学习项目的 VM 环境设置过程和最佳实践。

## 📋 概述

VM 环境是所有 phase 实验的基础。本文档总结了：
- VM 环境的配置和启动方法
- 访问 VM 的多种方式
- 常见问题及解决方案
- 辅助工具脚本

## ✅ 当前状态

### 环境验证（2026-11-14）

```
✓ VM 进程运行正常 (PID: 158679)
✓ KVM 硬件加速启用 (8 个 KVM fd)
✓ SSH 端口 2222 已监听
✓ VM 已启动到登录提示符
✓ 共享目录可用 (23 个文件)
✓ vIOMMU 已启用（用于 IOMMU 实验）
```

### 已知问题

1. **SSH 连接不稳定**
   - 症状：SSH 连接超时或 banner exchange 失败
   - 原因：VM 内 SSH 服务初始化慢，熵池不足
   - 解决方案：**使用串口控制台**（推荐）

2. **串口控制台访问**
   - 优点：可靠、即时、可看到完整启动日志
   - 缺点：前台运行，占用终端
   - 最佳实践：使用后台运行 + 日志查看

## 🚀 快速开始

### 1. 启动标准 VM

```bash
cd scripts/vm

# 方式 1：前台运行（推荐用于调试）
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2

# 方式 2：后台运行（推荐用于实验）
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2 > /tmp/vm.log 2>&1 &
tail -f /tmp/vm.log  # 查看启动进度
```

### 2. 启动带 vIOMMU 的 VM

```bash
# IOMMU 实验（phase3）
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2 \
  --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"

# 完整 IOMMU 实验（包含 caching-mode）
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2 \
  --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on"
```

### 3. 访问 VM

#### 方式 A：串口控制台（推荐）

直接在运行 VM 的终端操作：

```
Ubuntu 22.04 LTS kvm-study ttyS0

kvm-study login: root
root@kvm-study:~#
```

退出 VM：`Ctrl-A X`

#### 方式 B：SSH（不推荐）

```bash
ssh -p 2222 root@localhost
# 如果超时，等待 60 秒后重试
```

### 4. 验证 VM 环境

```bash
# 运行环境测试脚本
./test-vm-env.sh

# 预期输出：
# ✓ VM 进程运行正常
# ✓ KVM 硬件加速启用
# ✓ SSH 端口已监听
# ✓ VM 已启动到登录提示符
# ✓ 共享目录可用
```

## 📚 文档更新

### CLAUDE.md / AGENTS.md

已更新 `AGENTS.md`（通过 `CLAUDE.md` 符号链接访问），新增内容：

1. **快速开始**：3 步启动 VM
2. **VM 启动配置表**：5 种常见场景的命令
3. **VM 访问方式**：3 种方式的详细说明
4. **VM 管理脚本**：常用管理命令
5. **共享目录**：使用方法
6. **IOMMU 实验验证**：验证步骤
7. **常见问题**：5 个常见问题及解决方案
8. **KVM 加速确认**：验证方法

### 辅助脚本

创建了以下辅助脚本：

1. **`test-vm-env.sh`**
   - 功能：全面测试 VM 环境
   - 检查：进程、KVM、网络、启动状态、共享目录、IOMMU
   - 用法：`./test-vm-env.sh`

2. **`vm-access.sh`**
   - 功能：VM 访问和管理
   - 命令：serial, ssh, status, stop
   - 用法：`./vm-access.sh status`

3. **`VM-SETUP-GUIDE.md`**
   - 功能：详细的 VM 设置指南
   - 内容：启动、访问、故障排除、高级配置

## 🔧 常用命令速查

### 启动 VM

```bash
# 标准 VM
./boot-vm.sh ubuntu --net user

# 带 vIOMMU
./boot-vm.sh ubuntu --net user \
  --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"

# 后台运行
./boot-vm.sh ubuntu --net user > /tmp/vm.log 2>&1 &
```

### 管理 VM

```bash
# 查看状态
ps aux | grep qemu | grep kvm-study

# 查看端口
ss -tlnp | grep 2222

# 查看日志
tail -f /tmp/vm.log

# 停止 VM
pkill -9 -f "qemu.*kvm-study"
```

### 访问 VM

```bash
# 串口（推荐）
# 直接在运行 VM 的终端操作

# SSH（可能失败）
ssh -p 2222 root@localhost
```

### 验证环境

```bash
# 在 host 上
./test-vm-env.sh

# 在 VM 内
dmesg | grep -i "DMAR\|IOMMU"
ls /sys/kernel/iommu_groups/
```

## 📊 实验对照表

| Phase | 实验内容 | VM 配置 | 关键参数 |
|-------|---------|---------|---------|
| Phase 1 | CPU 虚拟化 | 标准 VM | `-cpu host` |
| Phase 2 | 内存虚拟化 (EPT) | 标准 VM | `-enable-kvm` |
| Phase 3 | IOMMU | vIOMMU VM | `-device intel-iommu,intremap=on` |
| Phase 4 | PCIe | 标准 VM | `-machine q35` |
| Phase 5 | 中断虚拟化 | 标准 VM | `-cpu host` |
| Phase 6 | Virtio | 标准 VM | `virtio-*` 设备 |
| Phase 7 | VFIO | 直通 VM | 需要物理设备 |
| Phase 8 | 时钟虚拟化 | 标准 VM | `-cpu host` |

## 🎯 最佳实践

### 1. 优先使用串口控制台

**原因**：
- 立即可用，无需等待 SSH 服务
- 可以看到完整的启动日志
- 更稳定，不受网络配置影响

**方法**：
- 前台运行 VM，直接在终端操作
- 后台运行时，通过日志查看状态

### 2. 使用后台运行模式

**原因**：
- 不占用终端
- 可以同时执行其他命令
- 便于自动化

**方法**：
```bash
./boot-vm.sh ubuntu --net user > /tmp/vm.log 2>&1 &
tail -f /tmp/vm.log  # 监控启动
```

### 3. 使用辅助脚本

**原因**：
- 自动化常见操作
- 减少手动错误
- 提高效率

**推荐脚本**：
- `test-vm-env.sh`：环境验证
- `vm-access.sh`：VM 管理

### 4. 共享目录使用

**原因**：
- 方便在 host 和 VM 之间传输文件
- 无需网络配置
- 自动挂载

**方法**：
```bash
# Host
echo "data" > scripts/shared/file.txt

# VM
cat /mnt/shared/file.txt
```

## ⚠️ 注意事项

1. **SSH 不可靠**
   - 不要依赖 SSH 进行关键操作
   - 始终准备好使用串口控制台

2. **VM 启动时间**
   - 首次启动可能需要 30-60 秒
   - 通过日志确认启动完成

3. **资源消耗**
   - 每个 VM 占用约 2GB 内存
   - 及时停止不用的 VM

4. **端口冲突**
   - 确保端口 2222 未被占用
   - 多个 VM 需要不同的端口

## 📖 相关文档

- `scripts/README.md`：脚本总体说明
- `scripts/vm/VM-SETUP-GUIDE.md`：详细设置指南
- `CLAUDE.md` → `AGENTS.md`：项目协作规范
- 各 phase 的 `practice/README.md`：具体实验说明

## 🔄 更新日志

- **2026-11-14**：完成 VM 环境设置，创建辅助脚本，更新 CLAUDE.md
- **2026-11-14**：验证 vIOMMU 配置，测试 IOMMU 实验环境
- **2026-11-14**：记录 SSH 访问问题，确定串口控制台为推荐方式

---

**维护者**：KVM Study Project  
**最后更新**：2026-11-14  
**状态**：✅ 已验证，可用于实验
