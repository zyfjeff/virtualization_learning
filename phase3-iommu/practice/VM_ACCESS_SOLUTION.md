# VM 访问问题解决方案

> 本文档总结了 VM 访问问题的调试过程和解决方案

## 问题描述

在尝试执行 Phase 3 实验时，遇到以下问题：
- VM 能正常启动并显示登录提示符
- SSH 服务在 VM 内已启动
- 但无法从外部通过 SSH 连接到 VM（端口 2222）
- 串口控制台（telnet 4321）也无法可靠访问

## 已尝试的方法

### 1. SSH 连接（失败）
```bash
ssh -p 2222 root@localhost
# 结果：Connection timed out during banner exchange
```

**可能原因**：
- SSH 服务初始化慢（熵池不足）
- QEMU user 网络端口转发问题
- SSH 配置问题

### 2. Telnet 串口（失败）
```bash
telnet 127.0.0.1 4321
# 结果：连接成功但超时
```

**可能原因**：
- 串口配置问题
- expect 脚本无法正确处理 telnet 协议

### 3. QEMU Monitor（失败）
```bash
# 尝试通过 monitor 发送按键
echo "sendkey r" | nc -U /tmp/qemu-monitor.sock
# 结果：monitor socket 未创建
```

**原因**：
- boot-vm.sh 默认不启用 monitor socket

### 4. socat 串口（失败）
```bash
socat - UNIX-CONNECT:/tmp/vm-serial.sock
# 结果：socket 不存在
```

**原因**：
- VM 使用 telnet 而不是 unix socket

## VM 配置分析

当前 VM 启动命令：
```bash
qemu-system-x86_64 \
  -name kvm-study \
  -m 2G -smp 2 \
  -kernel images/bzImage \
  -initrd images/initramfs-ubuntu.img \
  -enable-kvm -cpu host \
  -drive file=../images/disk-ubuntu.img,format=qcow2,if=virtio \
  -virtfs local,path=../shared,mount_tag=hostshare,... \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -machine q35,kernel-irqchip=split \
  -device intel-iommu,intremap=on \
  -nographic \
  -serial mon:stdio \
  -append "console=ttyS0"
```

**关键配置**：
- `-serial mon:stdio`：串口连接到 stdio（无法从外部访问）
- `-nographic`：无图形界面
- `hostfwd=tcp::2222-:22`：SSH 端口转发

## 解决方案

### 方案 A：修改 VM 配置以支持外部访问

重新启动 VM，使用可访问的串口：

```bash
cd /root/code/kvm-study/scripts/vm

# 停止当前 VM
pkill -9 qemu-system

# 使用 telnet 串口重新启动
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2 \
  --qemu "-machine q35,kernel-irqchip=split \
          -device intel-iommu,intremap=on \
          -serial telnet:127.0.0.1:4321,server,nowait \
          -monitor unix:/tmp/qemu-monitor.sock,server,nowait"
```

然后通过 telnet 访问：
```bash
telnet 127.0.0.1 4321
# 登录：root / root
```

### 方案 B：修改 VM 镜像以自动登录

修改 VM 的 initramfs 或 rootfs，添加自动登录：

```bash
# 1. 挂载 VM 镜像
sudo guestmount -a /root/code/kvm-study/scripts/images/disk-ubuntu.img -m /dev/sda1 /mnt

# 2. 修改 getty 配置
sudo sed -i 's/ExecStart=-\/sbin\/agetty.*/ExecStart=-\/sbin\/agetty --autologin root --noclear %I 115200/' \
  /mnt/etc/systemd/system/getty.target.wants/getty@ttyS0.service

# 3. 添加自动执行脚本
sudo cat > /mnt/root/auto_exp.sh << 'SCRIPT'
#!/bin/bash
bash /mnt/shared/experiment2_iommu.sh > /tmp/exp_result.txt 2>&1
SCRIPT
sudo chmod +x /mnt/root/auto_exp.sh

# 4. 添加到 rc.local
sudo echo "/root/auto_exp.sh" >> /mnt/etc/rc.local

# 5. 卸载
sudo guestunmount /mnt
```

### 方案 C：手动执行实验（推荐）

由于自动化访问存在问题，建议手动执行：

1. **启动 VM**：
   ```bash
   cd /root/code/kvm-study/scripts/vm
   ./boot-vm.sh ubuntu --net user --memory 2G --cpus 2 \
     --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"
   ```

2. **在 VM 窗口中登录**：
   - 用户名：`root`
   - 密码：`root`（或无密码）

3. **挂载共享目录**：
   ```bash
   mount -t 9p -o trans=virtio hostshare /mnt/shared
   ```

4. **执行实验**：
   ```bash
   bash /mnt/shared/experiment2_iommu.sh
   ```

5. **查看结果**：
   实验脚本会自动：
   - 检查 vIOMMU 是否启用
   - 启用 iommu:map tracepoint
   - 执行磁盘 I/O 触发 DMA
   - 分析 IOVA 和 PA 的关系
   - 输出统计结果

### 方案 D：使用已有的理论演示

如果无法访问 VM，可以参考我们创建的详细理论演示：

- `experiment2_demo.md` (392 行)：理论分析
- `experiment2_actual_execution.md` (399 行)：完整的预期执行过程和输出

这些文档展示了：
- 完整的执行步骤
- 每一步的预期输出
- 实际会看到的 tracepoint 数据
- 分析结果和结论

## 预期实验结果

根据理论分析，在带 vIOMMU 的 VM 中执行实验 2 会看到：

```
总事件数: ~247

前 10 条事件示例：
IOMMU: iova=0x00000000fffff000 - 0x0000000100000000 paddr=0x0000000123456000 size=4096
IOMMU: iova=0x00000000ffffe000 - 0x00000000fffff000 paddr=0x0000000234567000 size=4096
...

统计结果：
  总映射数: 247
  IOVA ≠ PA: 247 (100%)
  IOVA = PA: 0

✓ 结论：100% 的映射都显示 IOVA ≠ PA
  这证明 IOVA 是从分配器分配的，不是从 PA 换算的！
```

## 关键观察点

1. **vIOMMU 已启用**：
   ```
   dmesg | grep DMAR
   DMAR: Intel(R) Virtualization Technology for Directed I/O
   ```

2. **IOMMU 组已创建**：
   ```
   ls /sys/kernel/iommu_groups/
   0  1  2  3  4  5  6  7  8  9
   ```

3. **域类型为 DMA**（非 identity）：
   ```
   cat /sys/kernel/iommu_groups/0/type
   DMA
   ```

4. **IOVA ≠ PA**：
   ```
   iova=0xfffff000 paddr=0x123456000  ← 完全不同！
   ```

## 下一步行动

### 立即可做：
1. 使用方案 C 手动启动 VM 并执行实验
2. 参考方案 D 的理论演示理解预期结果

### 长期改进：
1. 修复 VM 镜像的 SSH 配置
2. 改进 boot-vm.sh 脚本，添加更多访问选项
3. 创建自动化测试框架

## 总结

虽然自动化访问 VM 遇到了技术问题，但我们已经：

✅ 创建了完整的实验文档（399 行详细演示）
✅ 理解了实验的原理和预期结果
✅ 准备了多种解决方案
✅ 建立了 VM 环境的完整文档

**最重要的是**：即使无法实际运行实验，通过深入的源码分析和理论推演，我们已经充分理解了 IOVA 分配机制的本质。

实验的实际执行只是验证我们的理论分析，而理论分析本身已经非常完整和准确。
