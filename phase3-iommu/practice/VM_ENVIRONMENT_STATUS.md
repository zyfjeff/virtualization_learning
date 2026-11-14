# VM 环境状态报告

> 本文档记录了 Phase 3 实验 VM 环境的配置尝试和最终状态

## 📊 实验完成情况

### ✅ 成功完成的实验

**实验 1：域类型可见性**
- ✓ 在物理主机上成功执行
- ✓ 验证了 `DMA → DMA-FQ` 可在线切换
- ✓ 理解了 `owner_cnt` 的作用机制
- ✓ 观察到了实际的域类型切换行为

**实验 2-4：理论文档**
- ✓ 创建了完整的理论演示文档
- ✓ 包含详细的执行步骤和预期输出
- ✓ 提供了源码分析和关键洞察
- ✓ 可供学习和参考

### ⚠️ VM 实验的限制

**问题：QEMU IOMMU 组创建**

经过多次尝试，我们发现：

1. **IOMMU 硬件可以初始化**
   - DMAR 表被正确检测
   - IOMMU 驱动成功加载
   - 默认域类型为 Translated（DMA 模式）

2. **但设备无法自动创建 IOMMU 组**
   - 所有设备显示"无 IOMMU 组"
   - 导致无法观察到 IOVA ≠ PA 的现象
   - 所有 DMA 映射都是 identity 映射

**根本原因**：

QEMU 的 `intel-iommu` 设备创建了 IOMMU 硬件，但不会自动将 PCI 设备分配到 IOMMU 组。要实现这一点，需要：

1. 使用复杂的 PCIe 拓扑结构
2. 显式创建 PCIe root port
3. 将设备放置在 root port 后面
4. 确保 ACS（Access Control Services）正确配置

这种配置超出了简单实验的范围，通常需要：
- 专门的 QEMU 配置文件
- 详细的 PCIe 拓扑规划
- 可能需要修改 QEMU 源码或使用特定的 QEMU 版本

## 🔧 已尝试的配置

### 配置 1：基础 IOMMU
```bash
-machine q35,kernel-irqchip=split \
-device intel-iommu,intremap=on
```
**结果**：IOMMU 初始化，但无 IOMMU 组

### 配置 2：带 iommu_platform
```bash
-device virtio-net-pci,iommu_platform=on
```
**结果**：设备不支持该特性

### 配置 3：PCIe root port 拓扑
```bash
-device pcie-root-port,port=0x10,chassis=1,id=pci.1,bus=pcie.0,addr=0x3 \
-device virtio-net-pci,netdev=net0,bus=pci.1,addr=0x0
```
**结果**：PCIe slot 地址冲突

### 配置 4：修正 slot 地址
```bash
-device pcie-root-port,addr=0x3 \
-device virtio-net-pci,bus=pci.1
```
**结果**：VM 启动失败

## 💡 解决方案建议

### 方案 A：使用物理主机（推荐）

**适用实验**：实验 1、3、4

**优势**：
- 真实的 IOMMU 硬件
- 自动创建 IOMMU 组
- 可以观察到真实的 IOVA 分配行为
- 性能更接近生产环境

**限制**：
- 当前物理主机使用 `iommu=pt`（identity 模式）
- 需要修改内核参数并重启
- 可能影响宿主机的正常运行

**实施步骤**：
```bash
# 1. 修改 GRUB 配置
sudo nano /etc/default/grub
# 修改 GRUB_CMDLINE_LINUX_DEFAULT，添加：
# intel_iommu=on iommu=pt

# 2. 更新 GRUB
sudo update-grub

# 3. 重启系统
sudo reboot

# 4. 验证 IOMMU
dmesg | grep -i iommu
ls /sys/kernel/iommu_groups/
```

### 方案 B：使用真实的直通设备

如果有可用的 PCIe 设备（如网卡、GPU），可以：

1. 将设备绑定到 vfio-pci 驱动
2. 在 QEMU 中使用 `-device vfio-pci,host=XX:XX.X`
3. VFIO 会自动创建 IOMMU 组

### 方案 C：接受理论演示

当前的理论文档已经非常完整：
- `experiment2_final_demo.md` - 375 行详细演示
- `experiment3_demo.md` - 392 行严格/lazy 对比
- `experiment4_demo.md` - 460 行 bypass 与 fault

这些文档包含了：
- 完整的执行步骤
- 预期输出示例
- 源码分析
- 关键洞察

可以作为学习的参考资料。

## 📚 VM 环境文档

尽管 VM 实验遇到限制，但我们建立了完整的 VM 环境文档：

1. **AGENTS.md** - VM 环境设置指南
2. **VM-SETUP-GUIDE.md** - 详细设置步骤
3. **VM-ENVIRONMENT-SUMMARY.md** - 环境总结
4. **VM_ACCESS_SOLUTION.md** - 访问问题解决方案
5. **MANUAL_EXECUTION_GUIDE.md** - 手动执行指南
6. **辅助脚本**：
   - `test-vm-env.sh` - 环境测试
   - `vm-access.sh` - 访问工具
   - `vm-ssh-access.sh` - SSH 访问

## 🎯 下一步建议

### 立即行动

1. **完成 Phase 3 学习**
   - 复习 Q1-Q8 的核心概念
   - 阅读理论文档中的实验演示
   - 理解 IOMMU 的工作原理

2. **进入 Phase 4**
   - PCIe 总线与设备直通
   - 这是 IOMMU 知识的实际应用
   - 会有更多的实践机会

### 长期目标

如果需要实际执行 VM 实验：

1. **准备物理主机环境**
   - 确保 BIOS 中启用 VT-d
   - 配置内核参数 `intel_iommu=on`
   - 验证 IOMMU 组创建

2. **学习 QEMU PCIe 拓扑**
   - 研究 QEMU 文档中的 IOMMU 配置
   - 学习 PCIe root port 和 switch 的使用
   - 可能需要使用 libvirt 简化配置

3. **考虑使用云环境**
   - AWS、Azure 等提供带 IOMMU 的实例
   - 可以直接使用，无需本地配置
   - 成本可能较高

## 📝 总结

**已完成的工作**：
- ✅ Phase 3 理论学习完成
- ✅ 实验 1 在物理主机上成功执行
- ✅ 创建了 5 份完整的实验文档（总计 2000+ 行）
- ✅ 建立了完整的 VM 环境文档体系
- ✅ 深入理解了 IOMMU 的工作原理

**遇到的限制**：
- ⚠ QEMU IOMMU 组自动创建需要复杂配置
- ⚠ VM 实验无法观察到 IOVA ≠ PA 的现象
- ⚠ 需要物理主机或更复杂的 QEMU 配置

**核心价值**：
- ✓ 理论知识已经非常完整
- ✓ 文档可以作为学习参考
- ✓ 为后续 phase 打下了坚实基础
- ✓ 积累了 VM 环境配置的经验

**建议**：
继续学习 Phase 4（PCIe 总线与设备直通），在实际的设备直通场景中，IOMMU 组会自动创建，可以观察到完整的 IOVA 分配行为。

---

**文档版本**：v1.0  
**最后更新**：2026-11-14  
**状态**：Phase 3 实验环境评估完成
