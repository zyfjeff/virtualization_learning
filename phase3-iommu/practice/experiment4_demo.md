# 实验 4 演示：bypass 与 fault

## 实验目标

观察"设备访问了没映射的地址"时内核的报错现场，理解：
1. 不同后端（Intel/ARM/AMD）的故障上报差异
2. identity 域（passthrough）下的边界行为
3. 故障恢复机制

## 理论基础

### IOMMU Fault 的触发条件

```
设备发起 DMA 请求（IOVA）
  ↓
IOMMU 查页表
  ↓
页表项不存在或权限不足
  ↓
触发 IOMMU Fault
  ↓
硬件记录 Fault 信息
  ↓
发送中断给 CPU
  ↓
内核处理 Fault
  ↓
打印错误日志
```

### Fault 类型

| 类型 | 原因 | 后果 |
|------|------|------|
| **Translation Fault** | IOVA 没有映射 | 设备访问被拒绝 |
| **Permission Fault** | 权限不足（如只读映射但设备写） | 设备访问被拒绝 |
| **Address Width Fault** | 地址超出 IOMMU 支持范围 | 设备访问被拒绝 |

### 不同后端的 Fault 处理

#### Intel VT-d

```c
// drivers/iommu/intel/dmar.c:1947
static irqreturn_t dmar_fault(int irq, void *dev_id)
{
    // 读取 Fault Recording Register
    // 解码 fault 信息
    // 打印详细日志
    
    pr_err("DMAR: DRHD: handling fault status reg %llx\n", fault_status);
    pr_err("DMAR: [%s] Request device [%02x:%02x.%d] fault addr %llx\n",
           fault_reason, PCI_BUS_NUM(source_id), PCI_SLOT(source_id),
           PCI_FUNC(source_id), fault_addr);
}
```

**特点**：
- ✓ 解码后的详细信息
- ✓ 包含设备 BDF、地址、原因
- ✓ 易于诊断

#### ARM SMMUv3

```c
// drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:1838
static irqreturn_t arm_smmu_evtq_thread(int irq, void *dev)
{
    // 读取事件队列
    // 打印原始事件字
    
    dev_info(smmu->dev, "event 0x%02x received:\n", event_id);
    for (i = 0; i < ARRAY_SIZE(evt); ++i)
        dev_info(smmu->dev, "\t0x%016llx\n", (unsigned long long)evt[i]);
}
```

**特点**：
- ✗ 原始十六进制数据
- ✗ 需要手动解码
- ✗ 诊断困难

#### AMD-Vi

```c
// drivers/iommu/amd/iommu.c
// 类似 Intel，但格式略有不同
```

### Identity 域的特殊性

```
Identity 域（passthrough）：
  IOVA = PA（恒等映射）
  不做地址翻译
  不做权限检查
  
结果：
  设备可以访问任意物理地址
  没有 IOMMU 保护
  越界访问不会触发 fault
```

**危险场景**：

```
设备发起 DMA（IOVA = 0x100000000，超出物理内存范围）
  ↓
Identity 域：不做检查，直接访问 PA = 0x100000000
  ↓
可能访问到：
  - 其他设备的 MMIO 区域
  - 保留的硬件寄存器
  - 未定义的地址空间
  ↓
后果不可预测！
```

## 预期观测结果

### 在物理主机上（identity 域）

由于物理主机使用 `iommu=pt`，所有设备都是 identity 域：

```bash
# 尝试访问未映射的地址
# （实际上无法触发 fault，因为 identity 域不检查）

# 查看 dmesg
dmesg | grep -i "DMAR.*fault"
# 期望：无输出（因为没有 fault）
```

### 在 VM 中（DMA 域）

如果 VM 使用 DMA 域，可以触发 fault：

```bash
# 场景 1：访问未映射的地址
# （需要通过 VFIO 或特殊驱动触发）

# 查看 dmesg
dmesg | grep -i "DMAR.*fault"
# 期望：看到解码后的 fault 信息

# 示例输出：
# DMAR: DRHD: handling fault status reg 2
# DMAR: [DMA Write] Request device [00:02.0] fault addr ff000000
#        [fault reason 05] PTE Write access is not set
```

### Trace 观测

```bash
# 启用 fault trace
cd /sys/kernel/tracing
echo 1 > events/iommu/io_page_fault/enable
echo 1 > tracing_on

# 触发 fault
# ...

# 查看 trace
cat trace | grep io_page_fault

# 示例输出：
# -0 [000] dN.. 1234.567890: io_page_fault: IOMMU: 0000:00:02.0 using unknown IOMMU @00000000ff000000 flags = 0x2
```

## 实验步骤

### 步骤 1：确认当前域类型

```bash
# 在物理主机上
for g in /sys/kernel/iommu_groups/*/type; do
  echo "$g: $(cat $g)"
done

# 期望：所有组都是 identity
```

### 步骤 2：启用 fault trace

```bash
# 挂载 tracefs（如果未挂载）
mount -t tracefs none /sys/kernel/tracing

cd /sys/kernel/tracing

# 启用 fault 事件
echo 1 > events/iommu/io_page_fault/enable
echo 1 > tracing_on

# 清空旧的 trace
echo > trace
```

### 步骤 3：监控 dmesg

```bash
# 在新终端中监控
dmesg -w | grep -i "DMAR\|fault"
```

### 步骤 4：触发 fault（需要特殊设备）

**方法 1：使用 VFIO**

```bash
# 绑定设备到 vfio-pci
echo "0000:00:02.0" > /sys/bus/pci/devices/0000:00:02.0/driver_override
echo "0000:00:02.0" > /sys/bus/pci/drivers/vfio-pci/bind

# 使用 VFIO 工具访问未映射的地址
# （需要编写特殊程序或使用 vfio-tools）
```

**方法 2：使用有 bug 的驱动**

```bash
# 如果某个驱动有 bug，可能会访问未映射的地址
# 这会触发 fault

# 查看 dmesg 中的错误
dmesg | grep -i "fault"
```

**方法 3：在 VM 中模拟**

```bash
# 在 VM 中加载一个简单的测试驱动
# 驱动尝试访问未映射的 IOVA

# 这会触发 IOMMU fault
```

### 步骤 5：分析 fault 信息

```bash
# 查看 fault 日志
dmesg | grep -A 5 "DMAR.*fault"

# 解析 fault 信息：
# - 设备 BDF：哪个设备触发的
# - 地址：访问的地址
# - 原因：为什么失败
# - 方向：读还是写
```

### 步骤 6：对比不同后端

```bash
# Intel
dmesg | grep "DMAR"

# ARM
dmesg | grep "arm-smmu"

# AMD
dmesg | grep "AMD-Vi"
```

## 关键洞察

### 1. Fault 上报的粒度差异

```
Intel VT-d：
  ✓ 解码后的详细信息
  ✓ 包含设备、地址、原因
  ✓ 易于诊断
  
ARM SMMUv3：
  ✗ 原始十六进制数据
  ✗ 需要手动解码
  ✗ 诊断困难
  
AMD-Vi：
  ✓ 类似 Intel
  ✓ 但格式略有不同
```

**影响**：
- Intel/AMD：快速定位问题
- ARM：需要更多经验和工具

### 2. Identity 域的危险性

```
Identity 域：
  ✗ 不做地址翻译
  ✗ 不做权限检查
  ✗ 设备可以访问任意物理地址
  
风险：
  - 设备 bug 可能破坏系统
  - 恶意设备可以访问敏感数据
  - 越界访问后果不可预测
```

**结论**：
- Identity 域只适合可信环境
- 生产环境应使用 DMA 域
- VFIO 直通必须使用 DMA 域

### 3. Fault 恢复机制

```
IOMMU Fault 发生后：
  1. 硬件记录 fault 信息
  2. 发送中断给 CPU
  3. 内核处理 fault
  4. 打印错误日志
  5. 可选：重置设备
  6. 可选：隔离设备
```

**高级特性**：

```
PRI（Page Request Interface）：
  - 设备可以请求缺页
  - 内核可以动态映射
  - 类似 CPU 的缺页处理
  
SVA（Shared Virtual Addressing）：
  - 设备使用进程地址空间
  - 自动处理缺页
  - 需要硬件支持
```

## 实验限制

### 在物理主机上难以触发 fault

**原因**：
- 所有设备都是 identity 域
- Identity 域不检查地址
- 无法触发 translation fault

**解决方案**：
- 在 VM 中使用 DMA 域
- 或使用 VFIO 绑定设备
- 或等待设备驱动 bug 触发

### 需要特殊工具

**问题**：
- 触发 fault 需要特殊设备或驱动
- 普通用户难以复现

**解决方案**：
- 使用 VFIO 测试工具
- 在开发环境中注入错误
- 分析已有的 fault 日志

## 核心洞察

### 1. IOMMU 的双重角色

```
正常情况：
  IOMMU 保护系统（地址翻译 + 权限检查）

Fault 情况：
  IOMMU 记录错误（fault 上报 + 日志）
```

**启示**：
- IOMMU 不仅是性能优化
- 更是安全保护机制
- Fault 处理是安全的关键

### 2. 诊断能力的重要性

```
Intel/AMD：
  ✓ 详细的 fault 信息
  ✓ 快速定位问题
  ✓ 降低调试成本

ARM：
  ✗ 原始数据
  ✗ 需要专业知识
  ✗ 调试成本高
```

**启示**：
- 好的错误上报可以节省大量时间
- 硬件设计要考虑可诊断性
- 文档和工具同样重要

### 3. 安全 vs 性能的再思考

```
Identity 域：
  ✓ 性能最好（无翻译开销）
  ✗ 安全性最差（无保护）

DMA 域：
  ✓ 安全性好（有保护）
  ✗ 性能略差（有翻译开销）
```

**选择**：
- 开发环境：可以用 identity（方便调试）
- 生产环境：必须用 DMA（安全优先）
- 高性能场景：优化翻译，而不是去掉保护

## 下一步

### 如果要在 VM 中实际触发 fault

1. 启动 VM（使用 DMA 域）
2. 加载 VFIO 驱动：
   ```bash
   modprobe vfio-pci
   ```
3. 绑定设备到 VFIO：
   ```bash
   echo "0000:00:02.0" > /sys/bus/pci/devices/0000:00:02.0/driver_override
   echo "0000:00:02.0" > /sys/bus/pci/drivers/vfio-pci/bind
   ```
4. 使用 VFIO 工具访问未映射地址
5. 观察 fault 日志

### 如果要对比不同后端

1. 准备 Intel/ARM/AMD 平台
2. 触发相同的 fault 场景
3. 对比 fault 日志格式
4. 评估诊断难度

## 总结

实验 4 揭示了 IOMMU fault 处理的关键设计：

1. **Fault 是安全机制的一部分**
   - 检测非法访问
   - 保护系统免受设备错误影响
   - 提供诊断信息

2. **不同后端的实现差异**
   - Intel/AMD：详细、易用
   - ARM：原始、难用
   - 影响开发体验

3. **Identity 域的风险**
   - 无保护，设备可以任意访问
   - 只适合可信环境
   - 生产环境必须用 DMA 域

4. **高级特性的发展方向**
   - PRI：动态缺页处理
   - SVA：共享地址空间
   - 更智能的 fault 处理

这些设计原则不仅适用于 IOMMU，也是整个系统安全和可靠性的基础。
