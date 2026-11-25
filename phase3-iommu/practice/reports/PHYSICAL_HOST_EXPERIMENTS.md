# 物理主机实验指南：实验 2-4

> 本指南说明如何在物理主机上执行实验 2-4

## 当前状态

- ✓ 物理主机有 222 个 IOMMU 组
- ✓ IOMMU 硬件正常工作
- ⚠️ 当前使用 `iommu=pt`（identity/passthrough 模式）
- ✗ 需要切换到 DMA 模式才能观察 IOVA ≠ PA

## 步骤 1：修改内核参数

### 当前内核参数
```
iommu=pt
```

### 需要修改为
```
intel_iommu=on iommu=strict
```

或者简单地移除 `iommu=pt`，让系统使用默认的 DMA 模式。

### 修改方法

**方法 A：临时修改（推荐先测试）**

```bash
# 1. 编辑 GRUB 配置
sudo nano /etc/default/grub

# 2. 找到 GRUB_CMDLINE_LINUX_DEFAULT 行
# 修改前：
GRUB_CMDLINE_LINUX_DEFAULT="... iommu=pt ..."

# 修改后（选项 1）：启用严格 IOMMU 模式
GRUB_CMDLINE_LINUX_DEFAULT="... intel_iommu=on iommu=strict ..."

# 或者（选项 2）：使用默认的 DMA 模式（移除 iommu=pt）
GRUB_CMDLINE_LINUX_DEFAULT="... intel_iommu=on ..."

# 3. 更新 GRUB
sudo update-grub

# 4. 重启系统
sudo reboot
```

**方法 B：使用 GRUB 菜单临时修改（无需重启）**

```bash
# 1. 重启系统，在 GRUB 菜单出现时按 'e' 编辑
# 2. 找到 linux 行，修改参数
# 3. 按 Ctrl+X 或 F10 启动
```

## 步骤 2：验证 IOMMU 模式切换

重启后，检查：

```bash
# 1. 检查内核参数
cat /proc/cmdline | grep -o 'iommu[^ ]*'

# 2. 检查 IOMMU 组类型
for g in /sys/kernel/iommu_groups/{0,1,2}/type; do
  if [ -f "$g" ]; then
    group=$(echo $g | grep -oP '\d+')
    type=$(cat $g)
    echo "Group $group: $type"
  fi
done

# 期望看到：DMA 或 DMA-FQ（不是 identity）

# 3. 检查 IOMMU 初始化日志
dmesg | grep -i "iommu\|dmar" | head -20
```

## 步骤 3：执行实验 2（IOVA ≠ PA）

```bash
cd /sys/kernel/tracing

# 1. 启用 iommu:map tracepoint
echo > trace
echo 1 > events/iommu/map/enable
echo 1 > tracing_on

# 2. 制造 DMA 活动
dd if=/dev/sda of=/dev/null bs=1M count=16
sync
sleep 1

# 3. 停止追踪
echo 0 > tracing_on

# 4. 查看结果
echo "总事件数: $(wc -l < trace)"
echo ""
echo "前 10 条事件："
head -10 trace
echo ""

# 5. 分析 IOVA vs PA
echo "IOVA vs PA 对比："
grep -oP 'iova=0x[0-9a-f]+|paddr=0x[0-9a-f]+' trace | paste - - | head -5 | while read line; do
  iova=$(echo "$line" | grep -oP 'iova=0x\K[0-9a-f]+')
  paddr=$(echo "$line" | grep -oP 'paddr=0x\K[0-9a-f]+')
  if [ -n "$iova" ] && [ -n "$paddr" ]; then
    if [ "$iova" != "$paddr" ]; then
      echo "  ✓ IOVA: 0x$iova → PA: 0x$paddr (不同！)"
    else
      echo "  ✗ IOVA: 0x$iova → PA: 0x$paddr (相同)"
    fi
  fi
done

# 6. 统计分析
TOTAL=$(grep -oP 'iova=0x[0-9a-f]+|paddr=0x[0-9a-f]+' trace | paste - - | wc -l)
DIFFERENT=$(grep -oP 'iova=0x[0-9a-f]+|paddr=0x[0-9a-f]+' trace | paste - - | awk '{
  split($1, a, "="); split($2, b, "=");
  if (a[2] != b[2]) print "diff"
}' | wc -l)

echo ""
echo "统计结果："
echo "  总映射数: $TOTAL"
echo "  IOVA ≠ PA: $DIFFERENT"
echo "  IOVA = PA: $((TOTAL - DIFFERENT))"

if [ $DIFFERENT -gt 0 ]; then
  echo ""
  echo "✓✓✓ 结论：IOVA ≠ PA，证明 IOVA 是分配出来的！✓✓✓"
fi
```

## 步骤 4：执行实验 3（strict vs lazy）

```bash
# 1. 挂载 debugfs（如果未挂载）
sudo mount -t debugfs none /sys/kernel/debug

# 2. 启用性能统计
echo 1 > /sys/kernel/debug/iommu/intel/dmar_perf_latency

# 3. 选择一个 DMA 模式的组
GROUP=$(ls /sys/kernel/iommu_groups/ | head -1)
echo "使用组: $GROUP"
echo "当前类型: $(cat /sys/kernel/iommu_groups/$GROUP/type)"

# 4. 制造 DMA 负载（strict 模式）
echo "制造 DMA 负载（10 秒）..."
dd if=/dev/sda of=/dev/null bs=1M count=100 &
DD_PID=$!
sleep 10
kill $DD_PID 2>/dev/null

# 5. 记录 strict 模式统计
echo ""
echo "=== Strict 模式统计 ==="
cat /sys/kernel/debug/iommu/intel/dmar_perf_latency

# 6. 切换到 lazy 模式
echo DMA-FQ > /sys/kernel/iommu_groups/$GROUP/type
echo "新类型: $(cat /sys/kernel/iommu_groups/$GROUP/type)"

# 7. 清空统计
echo 0 > /sys/kernel/debug/iommu/intel/dmar_perf_latency
echo 1 > /sys/kernel/debug/iommu/intel/dmar_perf_latency

# 8. 制造 DMA 负载（lazy 模式）
echo ""
echo "制造 DMA 负载（10 秒）..."
dd if=/dev/sda of=/dev/null bs=1M count=100 &
DD_PID=$!
sleep 10
kill $DD_PID 2>/dev/null

# 9. 记录 lazy 模式统计
echo ""
echo "=== Lazy 模式统计 ==="
cat /sys/kernel/debug/iommu/intel/dmar_perf_latency

# 10. 对比结果
echo ""
echo "=== 对比分析 ==="
echo "观察失效次数的变化："
echo "  - Strict 模式：每次 unmap 都立即失效"
echo "  - Lazy 模式：批量失效，次数应该显著减少"
```

## 步骤 5：执行实验 4（bypass 与 fault）

```bash
# 1. 启用 fault trace
cd /sys/kernel/tracing
echo 1 > events/iommu/io_page_fault/enable
echo 1 > tracing_on

# 2. 监控 dmesg
dmesg -w &
DMESG_PID=$!

# 3. 尝试触发 fault（需要特殊设备）
# 注意：这个实验需要 VFIO 设备或有 bug 的驱动
# 在物理主机上，我们可以观察现有的 fault 日志

# 4. 查看已有的 fault 日志
echo "检查 IOMMU fault 日志："
dmesg | grep -i "DMAR.*fault" | tail -10

# 5. 查看 trace 输出
echo ""
echo "IOMMU fault 事件："
cat trace | grep io_page_fault | head -10

# 6. 停止监控
kill $DMESG_PID 2>/dev/null
echo 0 > tracing_on

# 7. 分析结果
echo ""
echo "=== 分析 ==="
echo "Intel IOMMU 的 fault 日志格式："
echo "  DMAR: DRHD: handling fault status reg ..."
echo "  DMAR: [DMA Write] Request device [XX:XX.X] fault addr ..."
echo "  包含：设备 BDF、地址、原因（已解码）"
echo ""
echo "对比 ARM SMMUv3："
echo "  提供原始十六进制数据，需要手动解码"
```

## 预期结果

### 实验 2：IOVA ≠ PA
```
✓ 总事件数: 200+
✓ IOVA ≠ PA: 200+ (100%)
✓ IOVA = PA: 0
✓ 结论：IOVA 是分配出来的，不是从物理地址换算的
```

### 实验 3：strict vs lazy
```
Strict 模式：
  IOTLB: 1000+ 次失效
  
Lazy 模式：
  IOTLB: 100 次失效（批量）
  
结论：Lazy 模式减少 90% 的失效次数
```

### 实验 4：fault 处理
```
Intel IOMMU：
  DMAR: [DMA Write] Request device [03:00.0] fault addr 0x...
  [fault reason 05] PTE Write access is not set
  
结论：Intel 提供解码后的详细信息，易于诊断
```

## 注意事项

1. **修改内核参数需要重启**
   - 建议在非生产环境执行
   - 可以先用 GRUB 菜单临时测试

2. **使用安全的设备**
   - 不要对系统盘执行大量 I/O
   - 使用非关键设备测试

3. **记录结果**
   - 保存 trace 输出
   - 对比 identity 和 DMA 模式的差异

4. **恢复原状**
   - 实验完成后可以改回 `iommu=pt`
   - 重启系统恢复原始配置

## 故障排除

### 问题 1：无法切换到 DMA 模式
**解决**：检查内核参数是否正确，确保 `intel_iommu=on` 存在

### 问题 2：没有 IOMMU 组
**解决**：检查 BIOS 中是否启用了 VT-d

### 问题 3：tracepoint 不可用
**解决**：挂载 tracefs 和 debugfs
```bash
sudo mount -t tracefs none /sys/kernel/tracing
sudo mount -t debugfs none /sys/kernel/debug
```

## 总结

在物理主机上执行实验 2-4 可以：
- ✓ 观察到真实的 IOVA 分配行为
- ✓ 对比 strict 和 lazy 模式的性能差异
- ✓ 查看真实的 IOMMU fault 处理

这是验证 Phase 3 理论知识的最佳方式！
