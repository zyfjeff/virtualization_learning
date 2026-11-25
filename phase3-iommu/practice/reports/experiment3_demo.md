# 实验 3 演示：strict vs lazy 失效模式

## 实验目标

对比 `DMA`（strict 模式）和 `DMA_FQ`（lazy 模式）的失效行为差异，理解：
1. 两种模式的工作原理
2. 性能差异的来源
3. 适用场景

## 理论基础

### Strict 模式（DMA 域）

```c
// 每次 unmap 立即失效
dma_unmap_single(dev, dma_addr, size, dir);
  ↓
iommu_unmap(domain, iova, size);
  ↓
iotlb_gather 收集范围
  ↓
iommu_iotlb_sync(domain, &iotlb_gather);  // ← 立即同步
  ↓
ops->iotlb_sync()  // 发送 IOTLB 失效命令
  ↓
等待硬件完成
```

**特点**：
- ✓ 安全性高：设备立即无法访问已释放的内存
- ✗ 性能差：每次 unmap 都要等待 IOTLB 失效完成
- ✗ 频繁操作：大量小 unmap 会导致大量失效命令

### Lazy 模式（DMA_FQ 域）

```c
// unmap 时只记录，不立即失效
dma_unmap_single(dev, dma_addr, size, dir);
  ↓
iommu_unmap_fast(domain, iova, size, &iotlb_gather);  // ← 快速路径
  ↓
queue_iova(cookie, fq, iova_pfn, size);  // ← 加入 flush queue
  ↓
返回（不等待失效完成）

// 后台定期批量失效
fq_flush_timeout()  // 每 10ms 触发一次
  ↓
fq_flush_iotlb(cookie);
  ↓
ops->flush_iotlb_all()  // ← 一次性失效整个域
```

**特点**：
- ✓ 性能好：批量失效，减少操作次数
- ✓ 低延迟：unmap 立即返回
- ⚠ 安全性略低：失效有延迟（最多 10ms）
- ⚠ 内存占用：需要维护 flush queue

## 源码分析

### 关键代码路径

#### 1. 模式选择

```c
// drivers/iommu/dma-iommu.c:1747-1756
void iommu_setup_dma_ops(struct device *dev)
{
    struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
    
    dev->dma_iommu = iommu_is_dma_domain(domain);
    if (dev->dma_iommu && iommu_dma_init_domain(domain, dev))
        goto out_err;
}

// drivers/iommu/iommu.c:191-219
static int __init iommu_subsys_init(void)
{
    if (!iommu_default_passthrough() && !iommu_dma_strict)
        iommu_def_domain_type = IOMMU_DOMAIN_DMA_FQ;  // ← lazy 模式
    // ...
}
```

#### 2. Strict 路径

```c
// drivers/iommu/dma-iommu.c:835-844
static void iommu_dma_unmap(struct device *dev, dma_addr_t addr, size_t size)
{
    // ...
    if (!static_branch_unlikely(&iommu_deferred_attach_enabled))
        iommu_dma_unmap_page(dev, addr, size, dir, attrs);
}

static void iommu_dma_unmap_page(struct device *dev, dma_addr_t addr, 
                                  size_t size, enum dma_data_direction dir,
                                  unsigned long attrs)
{
    // ...
    iommu_unmap(domain, addr, size);  // ← 标准 unmap
    // 内部会调用 iommu_iotlb_sync()
}
```

#### 3. Lazy 路径

```c
// drivers/iommu/dma-iommu.c:846-854
static void __iommu_dma_unmap(struct device *dev, dma_addr_t addr, size_t size)
{
    struct iommu_domain *domain = iommu_get_dma_domain(dev);
    struct iommu_dma_cookie *cookie = domain->iova_cookie;
    struct iova_domain *iovad = &cookie->iovad;
    
    iommu_iotlb_gather_init(&iotlb_gather);
    iotlb_gather.queued = READ_ONCE(cookie->fq_domain);  // ← lazy 标记
    
    unmapped = iommu_unmap_fast(domain, addr, size, &iotlb_gather);
    // ...
    if (!iotlb_gather.queued)
        iommu_iotlb_sync(domain, &iotlb_gather);  // ← 只有非 lazy 才同步
    
    iommu_dma_free_iova(cookie, addr, size, &iotlb_gather);
}
```

#### 4. Flush Queue 实现

```c
// drivers/iommu/dma-iommu.c:179-245
static void fq_ring_add(struct iommu_dma_fq *fq)
{
    // 环形缓冲区管理
}

static void fq_flush_iotlb(struct iommu_dma_fq_cookie *cookie)
{
    // 批量失效
    iommu_flush_iotlb_all(cookie->domain);
}

// 定时器触发
static void fq_flush_timeout(struct timer_list *t)
{
    // 每 10ms 检查一次
    fq_flush_iotlb(cookie);
    
    // 重新调度
    mod_timer(&cookie->fq_timer, jiffies + msecs_to_jiffies(10));
}
```

## 预期观测结果

### 在物理主机上（所有组都是 identity 模式）

由于物理主机的命令行参数是 `iommu=pt`，所有设备都使用 identity 域：

```bash
$ for g in /sys/kernel/iommu_groups/*/type; do 
    echo "$g: $(cat $g)"
  done
/sys/kernel/iommu_groups/0/type: identity
/sys/kernel/iommu_groups/1/type: identity
...
```

**无法直接对比 strict vs lazy**，因为：
- Identity 域不做地址翻译
- 不需要 IOTLB 失效
- 没有 strict/lazy 的区别

### 在 VM 中（使用 DMA 域）

如果 VM 使用默认配置（非 passthrough），会看到：

```bash
# 查看域类型
$ cat /sys/kernel/iommu_groups/0/type
DMA

# 查看失效策略
$ dmesg | grep "TLB invalidation policy"
[    0.379769] iommu: DMA domain TLB invalidation policy: lazy mode
```

### 性能统计（Intel IOMMU）

```bash
# 启用性能统计
$ echo 1 > /sys/kernel/debug/iommu/intel/dmar_perf_latency

# 查看统计
$ cat /sys/kernel/debug/iommu/intel/dmar_perf_latency
```

**预期输出**：

Strict 模式：
```
IOTLB:    1000 次失效，平均延迟 5μs
DevTLB:    500 次失效，平均延迟 10μs
```

Lazy 模式：
```
IOTLB:     100 次失效（批量），平均延迟 50μs
DevTLB:     50 次失效（批量），平均延迟 100μs
```

**关键观察**：
- Lazy 模式的失效**次数**显著减少（10x）
- 但单次失效的**延迟**增加（因为批量更大）
- 总体性能提升：减少的命令提交开销 > 增加的单次延迟

## 实验步骤（在 VM 中）

### 步骤 1：确认域类型

```bash
# 查看当前域类型
cat /sys/kernel/iommu_groups/0/type

# 期望：DMA 或 DMA-FQ
```

### 步骤 2：切换到不同模式

```bash
# 从 DMA 切换到 DMA-FQ（在线切换）
echo DMA-FQ > /sys/kernel/iommu_groups/0/type

# 验证切换
cat /sys/kernel/iommu_groups/0/type
# 期望：DMA-FQ

# 注意：从 DMA-FQ 切换回 DMA 需要解绑驱动
```

### 步骤 3：启用性能统计

```bash
# 挂载 debugfs（如果未挂载）
mount -t debugfs none /sys/kernel/debug

# 启用 Intel IOMMU 性能统计
echo 1 > /sys/kernel/debug/iommu/intel/dmar_perf_latency
```

### 步骤 4：制造 DMA 活动

```bash
# 磁盘 I/O
dd if=/dev/zero of=/tmp/test bs=1M count=100

# 网络 I/O
iperf3 -c 10.0.2.2 -t 10
```

### 步骤 5：观察性能统计

```bash
# 查看统计
cat /sys/kernel/debug/iommu/intel/dmar_perf_latency

# 对比 strict 和 lazy 模式的差异
```

### 步骤 6：使用 trace 观察

```bash
# 启用 IOMMU trace
cd /sys/kernel/tracing
echo 1 > events/iommu/iotlb_flush/enable
echo 1 > events/iommu/iotlb_flush_all/enable
echo 1 > tracing_on

# 制造 DMA 活动
dd if=/dev/zero of=/tmp/test bs=1M count=10

# 查看 trace
cat trace | grep iommu

# 关闭 trace
echo 0 > tracing_on
```

## 关键洞察

### 1. 为什么 lazy 模式性能更好？

```
Strict 模式：
  unmap1 → 失效命令 → 等待完成 → unmap2 → 失效命令 → 等待完成 → ...
  
  总时间 = N × (失效命令提交时间 + 等待时间)

Lazy 模式：
  unmap1 → unmap2 → ... → unmapN → 批量失效命令 → 等待完成
  
  总时间 = N × unmap时间 + 1 × (失效命令提交时间 + 等待时间)
```

**关键点**：
- 失效命令提交是**同步操作**，需要等待硬件完成
- 批量失效可以将 N 次提交合并为 1 次
- 减少的提交次数 >> 增加的单次延迟

### 2. 为什么不在所有场景使用 lazy？

**安全性考虑**：

```
场景：设备驱动 unmap 一块内存，然后立即将其释放给其他用途

Strict 模式：
  unmap → 立即失效 → 释放内存 → 安全 ✓

Lazy 模式：
  unmap → 加入队列 → 释放内存 → 危险！
  ↓
  在失效完成前（最多 10ms），设备仍可能访问这块内存
  ↓
  如果内存已被重新分配，会导致数据损坏 ✗
```

**结论**：
- Lazy 模式适合：设备不会在 unmap 后立即访问该内存
- Strict 模式适合：需要立即保证安全性的场景

### 3. 如何选择？

| 场景 | 推荐模式 | 原因 |
|------|---------|------|
| 网络收包 | Lazy | 高频小 unmap，性能敏感 |
| 磁盘 I/O | Lazy | 批量操作，性能优先 |
| 安全关键设备 | Strict | 需要立即隔离 |
| VFIO 直通 | Strict | 用户态控制，安全性优先 |

## 实验总结

### 核心要点

1. **Strict vs Lazy 的本质区别**：
   - Strict：每次 unmap 立即失效
   - Lazy：批量失效，减少操作次数

2. **性能差异的来源**：
   - 失效命令是同步操作，需要等待硬件完成
   - Lazy 通过批量处理减少提交次数
   - 总体性能提升 5-10x（取决于工作负载）

3. **安全性的权衡**：
   - Lazy 有最多 10ms 的失效延迟
   - 在延迟期间，设备仍可能访问已 unmap 的内存
   - 需要根据场景选择合适的模式

### 在实际系统中的表现

- **现代 Linux 默认使用 lazy 模式**（`IOMMU_DOMAIN_DMA_FQ`）
- **原因**：大多数场景性能比极致的安全性更重要
- **例外**：VFIO 等安全敏感场景使用 strict 模式

### 调优建议

1. **监控失效次数**：
   ```bash
   cat /sys/kernel/debug/iommu/intel/dmar_perf_latency
   ```

2. **根据工作负载调整**：
   - 高频小 DMA：使用 lazy
   - 低频大 DMA：strict 也够用
   - 安全关键：使用 strict

3. **内核参数**：
   ```bash
   # 强制使用 strict 模式
   iommu.strict=1
   
   # 强制使用 lazy 模式
   iommu.strict=0
   ```

## 参考资料

- Q5 (invalidation.md)：详细的失效机制
- Q3 (domains.md)：域类型和切换
- `drivers/iommu/dma-iommu.c`：实现代码
- `Documentation/admin-guide/kernel-parameters.txt`：内核参数说明
