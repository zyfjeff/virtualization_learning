# VM 环境设置指南

## 快速开始

### 1. 启动标准 VM（无 vIOMMU）
```bash
cd scripts/vm
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2
```

### 2. 启动带 vIOMMU 的 VM（用于 IOMMU 实验）
```bash
cd scripts/vm
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2 \
  --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"
```

### 3. 启动带 vIOMMU + caching-mode 的 VM（用于完整 IOMMU 实验）
```bash
cd scripts/vm
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2 \
  --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on"
```

## VM 访问方式

### 方式 1：串口控制台（推荐用于调试）
VM 启动后，直接使用终端交互：
- 登录：`root`
- 密码：`root`（或无密码，取决于镜像配置）
- 退出 VM：`Ctrl-A X`

### 方式 2：SSH（推荐用于自动化）
```bash
# 使用 user 网络模式时，SSH 端口转发到 2222
ssh -p 2222 root@localhost

# 如果 SSH 连接超时，等待 30 秒后重试
# SSH 服务可能需要时间完全启动
```

### 方式 3：后台运行 + 串口访问
```bash
# 后台启动 VM
./boot-vm.sh ubuntu --net user > /tmp/vm.log 2>&1 &

# 查看 VM 状态
tail -f /tmp/vm.log

# 使用辅助脚本
./vm-access.sh status   # 查看状态
./vm-access.sh stop     # 停止 VM
```

## 常见问题解决

### 问题 1：SSH 连接超时
**症状**：`Connection timed out during banner exchange`

**原因**：
- SSH 服务还在初始化
- 熵池不足（虚拟机常见问题）

**解决方案**：
```bash
# 1. 等待 30-60 秒让 SSH 完全启动
sleep 30

# 2. 通过串口登录并手动启动 SSH
# 在串口控制台中：
systemctl restart ssh
systemctl status ssh

# 3. 检查 SSH 配置
cat /etc/ssh/sshd_config | grep -E "PermitRootLogin|PasswordAuthentication"
```

### 问题 2：无法通过串口访问
**症状**：VM 启动后无法交互

**原因**：
- 使用了 `-nographic` 但没有正确配置串口
- 内核命令行缺少 `console=ttyS0`

**解决方案**：
```bash
# 确保启动参数包含串口控制台
./boot-vm.sh ubuntu --net user \
  --qemu "-append 'console=ttyS0 root=/dev/vda1 rw'"
```

### 问题 3：vIOMMU 设备无法识别
**症状**：guest 内看不到 IOMMU 组

**原因**：
- 缺少 `-machine q35` 参数
- 内核未启用 IOMMU 支持

**解决方案**：
```bash
# 使用正确的 machine 类型
./boot-vm.sh ubuntu --net user \
  --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"

# 在 guest 内验证
dmesg | grep -i dmar
ls /sys/kernel/iommu_groups/
```

## VM 镜像说明

| 镜像类型 | 用途 | 启动命令 |
|---------|------|---------|
| `ubuntu` | 通用 Ubuntu 系统 | `./boot-vm.sh ubuntu` |
| `allinone` | 包含所有工具 | `./boot-vm.sh allinone` |
| `minimal` | 最小化系统 | `./boot-vm.sh minimal` |

## 网络配置

### user 网络模式（推荐用于实验）
- 优点：简单，自带端口转发
- SSH 端口：2222 → guest 22
- 缺点：性能较低，无法从外部访问

### tap 网络模式（推荐用于性能测试）
- 优点：性能高，可配置桥接
- 需要 root 权限创建 tap 设备
- 可配置静态 IP

```bash
# 使用 tap 网络
./boot-vm.sh ubuntu --net tap --queues 4

# 在 guest 内配置网络
ip addr add 192.168.100.10/24 dev eth0
ip route add default via 192.168.100.1
```

## 共享目录

VM 自动挂载共享目录到 `/mnt/shared`：
```bash
# 在 host 上
echo "test" > scripts/shared/test.txt

# 在 guest 内
cat /mnt/shared/test.txt
```

## 实验环境验证

### 验证 KVM 加速
```bash
# 在 host 上
ls -l /proc/$(pgrep -f 'qemu.*kvm-study')/fd | grep -c kvm
# 应该 > 0，表示使用 KVM 加速
```

### 验证 vIOMMU
```bash
# 在 guest 内
dmesg | grep -i "DMAR\|IOMMU"
ls /sys/kernel/iommu_groups/
cat /sys/kernel/iommu_groups/*/type
```

### 验证网络
```bash
# 在 guest 内
ip addr show
ping -c 3 10.0.2.2  # QEMU user 网络的网关
```

## 故障排除检查清单

- [ ] VM 进程是否在运行？`ps aux | grep qemu`
- [ ] 端口 2222 是否监听？`ss -tlnp | grep 2222`
- [ ] 串口是否有输出？查看启动日志
- [ ] SSH 服务是否启动？通过串口检查 `systemctl status ssh`
- [ ] 防火墙是否阻止？检查 `iptables -L`
- [ ] 内核参数是否正确？检查 `/proc/cmdline`

## 高级配置

### 自定义内核启动参数
```bash
./boot-vm.sh ubuntu --net user \
  --qemu "-append 'console=ttyS0 root=/dev/vda1 rw iommu=pt'"
```

### 多 CPU 和大内存
```bash
./boot-vm.sh ubuntu --net user --memory 8G --cpus 8 --queues 8
```

### 调试模式
```bash
./boot-vm.sh ubuntu --net user --debug
# 会启动 GDB server，可以用 gdb 连接
```

## 参考资料

- QEMU 文档：https://www.qemu.org/docs/master/
- KVM 文档：https://www.linux-kvm.org/page/Documents
- 项目 scripts/README.md：详细的脚本说明
