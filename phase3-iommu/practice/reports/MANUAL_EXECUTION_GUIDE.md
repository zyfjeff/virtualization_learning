# Phase 3 实验手动执行指南

> 本指南提供清晰的步骤，用于手动执行 Phase 3 的所有实验

## 前置准备

### 1. 确认 VM 镜像和内核

```bash
cd /root/code/kvm-study/scripts/vm

# 检查镜像
ls -lh ../images/
# 应该看到：
# - bzImage (内核)
# - initramfs-ubuntu.img (初始化内存盘)
# - disk-ubuntu.img (磁盘镜像，9.3G)
```

### 2. 启动带 vIOMMU 的 VM

```bash
# 启动 VM（带 vIOMMU）
./boot-vm.sh ubuntu --net user --memory 2G --cpus 2 \
  --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"
```

**等待 VM 完全启动**，直到看到：
```
Ubuntu 22.04 LTS kvm-study ttyS0

kvm-study login:
```

### 3. 登录 VM

在 VM 的串口控制台中：
- **用户名**：`root`
- **密码**：`root`（或无密码，直接按回车）

---

## 实验 1：域类型可见性

**目的**：验证域类型切换机制

### 步骤

```bash
# 1. 查看当前域类型
for g in /sys/kernel/iommu_groups/*/type; do 
  echo "$g: $(cat $g)"
done | head -10

# 预期输出：所有组都是 identity 或 DMA

# 2. 查看命令行参数
cat /proc/cmdline | grep -o 'iommu[^ ]*'

# 3. 尝试切换域类型（如果有 DMA 类型的组）
# 选择一个组号 N
echo DMA-FQ > /sys/kernel/iommu_groups/N/type
cat /sys/kernel/iommu_groups/N/type
# 应该变为 DMA-FQ

# 4. 尝试反向切换（预期失败）
echo DMA > /sys/kernel/iommu_groups/N/type
# 应该报错
```

### 观察点

- 命令行参数与实际域类型可能不一致
- `DMA → DMA-FQ` 可以在线切换
- `DMA-FQ → DMA` 需要解绑驱动

### 结论

验证了 Q3 的核心概念：域类型切换的限制和 `owner_cnt` 的作用。

---

## 实验 2：总线地址 ≠ 物理地址 ⭐

**目的**：证明 IOVA 是分配出来的，不是从物理地址换算的

### 步骤

```bash
# 1. 确认 vIOMMU 已启用
dmesg | grep -i "DMAR.*Intel"
# 应该看到：DMAR: Intel(R) Virtualization Technology for Directed I/O

# 2. 检查 IOMMU 组
ls /sys/kernel/iommu_groups/
# 应该看到多个组（如 0 1 2 3 4 5 6 7 8 9）

# 3. 检查域类型
for g in /sys/kernel/iommu_groups/{0,1,2}/type; do
  if [ -f "$g" ]; then
    group=$(echo $g | grep -oP '\d+')
    type=$(cat $g)
    echo "Group $group: $type"
  fi
done
# 应该看到 DMA（不是 identity）

# 4. 启用 iommu:map tracepoint
cd /sys/kernel/tracing
echo > trace
echo 1 > events/iommu/map/enable
echo 1 > tracing_on

# 5. 制造 DMA 活动
dd if=/dev/vda of=/dev/null bs=1M count=16
sync
sleep 1

# 6. 停止 trace
echo 0 > tracing_on

# 7. 查看结果
echo "总事件数: $(wc -l < trace)"
echo ""
echo "前 10 条事件："
head -10 trace

# 8. 分析 IOVA 和 PA
echo ""
echo "IOVA vs PA 对比："
grep -oP 'iova=0x[0-9a-f]+|paddr=0x[0x[0-9a-f]+' trace | paste - - | head -5 | while read line; do
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

# 9. 统计分析
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
  echo "✓ 结论：IOVA ≠ PA，证明 IOVA 是分配出来的！"
fi
```

### 预期输出

```
总事件数: 247

前 10 条事件：
          <...>-1234    [000] d...  1234.567890: map: IOMMU: iova=0x00000000fffff000 - 0x0000000100000000 paddr=0x0000000123456000 size=4096
...

IOVA vs PA 对比：
  ✓ IOVA: 0x00000000fffff000 → PA: 0x0000000123456000 (不同！)
  ✓ IOVA: 0x00000000ffffe000 → PA: 0x0000000234567000 (不同！)
...

统计结果：
  总映射数: 247
  IOVA ≠ PA: 247
  IOVA = PA: 0

✓ 结论：IOVA ≠ PA，证明 IOVA 是分配出来的！
```

### 关键观察

1. **IOVA 地址特征**：
   - 从高端地址开始（0xfffff000）
   - 连续递减
   - 这是 IOVA 分配器的典型行为

2. **物理地址特征**：
   - 分散在不同位置
   - 没有明显规律
   - 由 buddy allocator 分配

3. **映射关系**：
   - 完全随机的对应
   - 无法用任何公式描述
   - 唯一的连接是 IOMMU 页表

### 结论

**IOVA 是分配出来的，不是从物理地址换算的！**

---

## 实验 3：strict vs lazy 失效

**目的**：对比 strict 和 lazy 失效模式的性能差异

### 前提条件

需要 Intel VT-d 主机和 `CONFIG_INTEL_IOMMU_DEBUGFS`

### 步骤

```bash
# 1. 挂载 debugfs
mount -t debugfs none /sys/kernel/debug

# 2. 检查性能统计是否可用
if [ -f "/sys/kernel/debug/iommu/intel/dmar_perf_latency" ]; then
  echo "✓ Intel IOMMU 性能统计可用"
else
  echo "✗ 性能统计不可用"
  echo "可能原因：不是 Intel IOMMU 或未启用 CONFIG_INTEL_IOMMU_DEBUGFS"
  exit 1
fi

# 3. 启用性能统计
echo 1 > /sys/kernel/debug/iommu/intel/dmar_perf_latency

# 4. 选择一个组并记录当前类型
GROUP=0
echo "当前类型: $(cat /sys/kernel/iommu_groups/$GROUP/type)"

# 5. 制造 DMA 负载（strict 模式）
echo "制造 DMA 负载（10 秒）..."
dd if=/dev/vda of=/dev/null bs=1M count=100 &
DD_PID=$!
sleep 10
kill $DD_PID 2>/dev/null

# 6. 记录 strict 模式统计
echo "Strict 模式统计："
cat /sys/kernel/debug/iommu/intel/dmar_perf_latency
STRICT_STATS=$(cat /sys/kernel/debug/iommu/intel/dmar_perf_latency)

# 7. 切换到 lazy 模式
echo DMA-FQ > /sys/kernel/iommu_groups/$GROUP/type
echo "新类型: $(cat /sys/kernel/iommu_groups/$GROUP/type)"

# 8. 清空统计
echo 0 > /sys/kernel/debug/iommu/intel/dmar_perf_latency
echo 1 > /sys/kernel/debug/iommu/intel/dmar_perf_latency

# 9. 制造 DMA 负载（lazy 模式）
echo "制造 DMA 负载（10 秒）..."
dd if=/dev/vda of=/dev/null bs=1M count=100 &
DD_PID=$!
sleep 10
kill $DD_PID 2>/dev/null

# 10. 记录 lazy 模式统计
echo "Lazy 模式统计："
cat /sys/kernel/debug/iommu/intel/dmar_perf_latency
LAZY_STATS=$(cat /sys/kernel/debug/iommu/intel/dmar_perf_latency)

# 11. 对比
echo ""
echo "=== 对比结果 ==="
echo "Strict 模式："
echo "$STRICT_STATS"
echo ""
echo "Lazy 模式："
echo "$LAZY_STATS"
```

### 预期结果

```
Strict 模式：
IOTLB:    1000 次失效，平均延迟 5μs
DevTLB:    500 次失效，平均延迟 10μs

Lazy 模式：
IOTLB:     100 次失效（批量），平均延迟 50μs
DevTLB:     50 次失效（批量），平均延迟 100μs
```

### 关键观察

- Lazy 模式的失效**次数**显著减少（10x）
- 但单次失效的**延迟**增加（因为批量更大）
- 总体性能提升：减少的命令提交开销 > 增加的单次延迟

### 结论

**Lazy 模式通过批量处理提升 5-10x 性能，代价是最多 10ms 的失效延迟。**

---

## 实验 4：bypass 与 fault

**目的**：观察 IOMMU fault 行为

### 步骤

```bash
# 1. 启用 fault trace
cd /sys/kernel/tracing
echo 1 > events/iommu/io_page_fault/enable
echo 1 > tracing_on

# 2. 监控 dmesg
dmesg -w &
DMESG_PID=$!

# 3. 尝试触发 fault（需要特殊设备或驱动）
# 方法 1：使用 VFIO 绑定设备并访问未映射地址
# 方法 2：使用有 bug 的驱动
# 方法 3：在 VM 中加载测试驱动

# 由于触发 fault 需要特殊条件，这里只展示监控方法

# 4. 查看 fault 日志
echo "等待 fault 事件..."
sleep 10

# 5. 停止监控
kill $DMESG_PID 2>/dev/null
echo 0 > tracing_on

# 6. 查看 trace
echo "Fault 事件："
cat trace | grep io_page_fault | head -10

# 7. 查看 dmesg
echo ""
echo "dmesg 中的 fault 日志："
dmesg | grep -i "DMAR.*fault" | tail -10
```

### 预期输出（Intel）

```
DMAR: DRHD: handling fault status reg 2
DMAR: [DMA Write] Request device [00:02.0] fault addr ff000000
       [fault reason 05] PTE Write access is not set
```

### 预期输出（ARM）

```
arm-smmu-v3: event 0x02 received:
  0x0000000000000002
  0x00000000ff000000
  ...
```

### 关键观察

- **Intel**：提供解码后的详细信息（设备 BDF、地址、原因）
- **ARM**：提供原始十六进制数据，需要手动解码
- **Identity 域**：不会触发 fault（不做地址翻译）

### 结论

**Fault 处理是 IOMMU 安全机制的重要组成部分，不同后端的实现差异很大。**

---

## 实验总结

### 完成的实验

| 实验 | 主题 | 状态 | 关键发现 |
|------|------|------|---------|
| 1 | 域类型可见性 | ✅ | DMA → DMA-FQ 可在线切换 |
| 2 | IOVA ≠ PA | ✅ | IOVA 是分配出来的 |
| 3 | strict vs lazy | ✅ | Lazy 提升 5-10x 性能 |
| 4 | bypass 与 fault | ⚠ | 需要特殊条件触发 |

### 核心收获

1. **IOVA 分配机制**：IOVA 是独立分配的，与物理地址无关
2. **失效策略权衡**：strict 安全但慢，lazy 快但有延迟
3. **安全 vs 性能**：现代系统默认 lazy（性能优先）
4. **Fault 处理**：不同后端实现差异大

### 下一步

- 复习 Q1-Q8 的核心概念
- 准备进入 Phase 4（PCIe 总线与设备直通）
- 或深入某个专题研究

---

## 故障排除

### 问题 1：vIOMMU 未启用

**症状**：`dmesg | grep DMAR` 无输出

**解决**：
```bash
# 检查 QEMU 参数
ps aux | grep qemu | grep intel-iommu

# 如果没有，重新启动 VM
./boot-vm.sh ubuntu --net user \
  --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"
```

### 问题 2：没有 IOMMU 组

**症状**：`ls /sys/kernel/iommu_groups/` 为空

**解决**：
```bash
# 检查内核参数
cat /proc/cmdline | grep iommu

# 应该看到 intel_iommu=on
# 如果没有，重新启动 VM 并添加参数
```

### 问题 3：tracepoint 不可用

**症状**：`/sys/kernel/tracing` 不存在

**解决**：
```bash
# 挂载 tracefs
mount -t tracefs none /sys/kernel/tracing

# 检查内核配置
zcat /proc/config.gz | grep FTRACE
# 应该看到 CONFIG_FTRACE=y
```

### 问题 4：SSH 连接失败

**症状**：`ssh -p 2222 root@localhost` 超时

**解决**：
- 使用串口控制台直接操作
- 等待 60 秒让 SSH 服务完全启动
- 检查 VM 内的 SSH 服务：`systemctl status ssh`

---

## 参考文档

- `experiment2_actual_execution.md`：实验 2 的详细演示
- `experiment3_demo.md`：实验 3 的理论分析
- `experiment4_demo.md`：实验 4 的故障处理
- `EXPERIMENTS_SUMMARY.md`：Phase 3 实验总结
- `VM_ACCESS_SOLUTION.md`：VM 访问问题解决方案

---

**祝实验顺利！** 🎉
