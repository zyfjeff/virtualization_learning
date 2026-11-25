# ATS (Address Translation Services) 详解

> 基于 PCIe Base Specification r6.0 和 Intel VT-d 规范
>
> **承接**：[`basics.md`](basics.md) §5 能力概览——你已知道 ATS 是设备侧地址缓存。
> **本文回答**：缓存省了什么？失效时付出什么？启用/关闭时序为什么有坑？
> **引出**：直通场景的 ATS 依赖 → [`phase7 README §1.5`](../phase7-vfio/README.md)；
> Device-TLB 失效的内核实现 → [`phase3 invalidation.md`](../phase3-iommu/invalidation.md)；
> 命名/时序陷阱 → AGENTS.md 全局陷阱 #14。
> ⚠️ 本文「性能分析」数字是无出处推算（见 [`corrections.md`](corrections.md) 勘误 11），不得当实测引用。

---

## 🎯 ATS 的核心概念

**一句话总结**：ATS 让设备能够缓存 IOMMU 的翻译结果，减少重复翻译的开销。

---

## 🔍 ATS 的工作原理

### 传统流程（无 ATS）

```
Device 发起 DMA (IOVA = 0x1000)
    ↓
IOMMU 接收请求
    ↓
IOMMU 查页表：IOVA 0x1000 → PA 0x2000
    ↓
IOMMU 返回 PA 0x2000
    ↓
Device 访问物理内存 0x2000
    ↓
下次 Device 再次访问 IOVA 0x1000
    ↓
重复上述流程（每次都经过 IOMMU）
```

### ATS 优化流程

```
Device 发起 DMA (IOVA = 0x1000)
    ↓
Device 检查自己的 Device TLB (ATS 缓存)
    ↓
┌─ Hit: 找到缓存的翻译结果
│   IOVA 0x1000 → PA 0x2000
│   ↓
│   直接访问物理内存 0x2000
│   ✓ 不经过 IOMMU，延迟低
│
└─ Miss: 缓存中没有
    ↓
    Device 发送 Translation Request (TR) 给 IOMMU
    ↓
    IOMMU 查页表：IOVA 0x1000 → PA 0x2000
    ↓
    IOMMU 返回 Translation Completion (TC)
    ↓
    Device 缓存翻译结果到 Device TLB
    ↓
    Device 访问物理内存 0x2000
    ↓
    下次访问 IOVA 0x1000
    ↓
    Device TLB Hit，直接访问 ✓
```

---

## 💡 ATS 的关键机制

### 1. Translation Request (TR)

设备向 IOMMU 请求地址翻译：

```
Device → IOMMU: "请翻译 IOVA 0x1000"
    ↓
IOMMU → Device: "翻译结果：PA 0x2000"
    ↓
Device 缓存到 Device TLB
```

**特点**：
- TR 是 PCIe TLP（Transaction Layer Packet）
- 使用特殊的 Requester ID
- IOMMU 必须支持 ATS 才能响应

### 2. Device TLB (DTLB)

设备内部的翻译缓存：

```
┌─────────────────────────────────────┐
│  Device TLB (ATS 缓存)             │
├─────────────────────────────────────┤
│  IOVA        │  PA          │ 权限 │
│  0x1000      │  0x2000      │  RW  │
│  0x2000      │  0x3000      │  R   │
│  0x3000      │  0x4000      │  RW  │
│  ...         │  ...         │  ... │
└─────────────────────────────────────┘

特点：
  - 容量有限（通常 64-512 条目）
  - associative 查找（类似 CPU TLB）
  - 需要失效机制
```

### 3. Invalidation（失效机制）

当 IOMMU 页表更新时，必须通知设备失效缓存：

```
IOMMU 页表更新（例如 unmap IOVA 0x1000）
    ↓
IOMMU 发送 Invalidation Request 给 Device
    ↓
Device 收到请求
    ↓
Device 删除 Device TLB 中 IOVA 0x1000 的条目
    ↓
Device 发送 Invalidation Completion 给 IOMMU
    ↓
IOMMU 确认失效完成
```

**关键**：
- 失效是**同步**的（必须等待设备确认）
- 失效有延迟（跨越 PCIe 总线）
- 这是 ATS 的主要复杂性来源

---

## 🔗 ATS 与 IOMMU 的关系

### 协作模型

```
┌──────────────────────────────────────────────────────┐
│                    系统架构                            │
│                                                       │
│  ┌─────────┐                                         │
│  │ Device  │ ←──── Device TLB (ATS 缓存)             │
│  │   A     │         ↓ (miss)                        │
│  └────┬────┘         ↓                               │
│       │                ↓                              │
│       │         ┌──────────┐                         │
│       └────────→│  IOMMU   │ ←── 主页表              │
│                 │          │                         │
│                 └────┬─────┘                         │
│                      │                               │
│                      ↓                               │
│                ┌──────────┐                         │
│                │  Memory  │                         │
│                └──────────┘                         │
│                                                       │
│  协作流程：                                          │
│  1. Device 先查 Device TLB                          │
│  2. Miss 时请求 IOMMU 翻译                          │
│  3. IOMMU 查主页表                                  │
│  4. Device 缓存翻译结果                             │
│  5. 后续访问直接使用缓存                            │
└──────────────────────────────────────────────────────┘
```

### 与 Phase 3 的联系

在 Phase 3 中，我们学习了：
- IOMMU 的 IOTLB（IOMMU 内部缓存）
- 失效同步机制（strict vs lazy）
- Device-TLB 失效的代价

**ATS 引入了第三层缓存**：
```
CPU TLB      → 缓存 VA → PA 翻译
IOMMU IOTLB  → 缓存 IOVA → PA 翻译
Device TLB   → 缓存 IOVA → PA 翻译（ATS）
```

**失效链条**：
```
IOMMU 页表更新
    ↓
失效 IOTLB（IOMMU 内部）
    ↓
失效 Device TLB（ATS，跨越 PCIe 总线）← 这是 ATS 的代价
```

---

## 📊 ATS 的性能分析

> ⚠️ **本节数字是无出处的机制推算**（非本仓实测，亦未核对规范出处），
> 不得当实测结论引用。2026-11-25 已在 `corrections.md` 登记待审计，
> 分级规则见 `../phase10-performance/index.md`。

### 优势

1. **减少 IOMMU 压力**
   ```
   无 ATS：每次 DMA 都经过 IOMMU
   有 ATS：大部分 DMA 在设备侧完成翻译
   
   IOMMU 查询次数：减少 80-95%
   ```

2. **降低延迟**
   ```
   无 ATS：Device → IOMMU → Memory
           延迟：100-500 ns
   
   有 ATS (Hit)：Device → Memory
           延迟：10-50 ns
           
   性能提升：5-10x（对于频繁访问相同地址的场景）
   ```

3. **提升带宽**
   ```
   IOMMU 不再是瓶颈
   设备可以更充分地利用 PCIe 带宽
   ```

### 代价

1. **失效延迟**
   ```
   当 IOMMU 页表更新时：
     必须发送 Invalidation Request 给设备
     等待设备确认
     跨越 PCIe 总线的延迟：1-10 μs
     
   这就是为什么 Phase 3 中 Device-TLB 失效是同步的
   ```

2. **复杂性**
   ```
   - 设备必须实现 ATS 逻辑
   - 必须处理 Invalidation Request
   - 必须管理 Device TLB
   - 错误处理更复杂
   ```

3. **缓存一致性问题**
   ```
   如果失效机制出错：
     Device TLB 中可能有过期的翻译
     导致设备访问错误的物理地址
     可能造成数据损坏或安全漏洞
   ```

---

## 🎯 ATS 在虚拟化中的应用

### 场景 1：VM 设备直通（VFIO）

```
Guest VM
    ↓
虚拟设备（Guest 认为的）
    ↓
VFIO 驱动
    ↓
IOMMU（翻译 Guest IOVA → Host PA）
    ↓
物理设备
```

**ATS 的作用**：
- 物理设备可以缓存 Guest IOVA → Host PA 的翻译
- 减少 IOMMU 查询次数
- 提升直通设备的性能

**挑战**：
- Guest 修改页表时，必须失效 Device TLB
- 需要 Guest 和 Host 协作
- 复杂性显著增加

### 场景 2：SR-IOV

```
Physical Function (PF)
    ↓
Virtual Function 1 (VF1) → VM1
Virtual Function 2 (VF2) → VM2
Virtual Function 3 (VF3) → VM3
```

**ATS 的作用**：
- 每个 VF 可以有独立的 Device TLB
- 不同 VM 的翻译缓存相互隔离
- 提升多 VM 场景的性能

### 场景 3：GPUDirect RDMA

```
GPU → NVMe（直接数据传输）
```

**ATS 的作用**：
- GPU 和 NVMe 都可以缓存翻译结果
- 减少 P2P 传输的翻译开销
- 提升 GPU 直接访问存储的性能

---

## 🔧 ATS 的配置和启用

### 设备侧

```c
// PCIe 配置空间中的 ATS Capability
struct ats_capability {
    u16 capability_id;     // ATS Capability ID
    u16 next_pointer;      // 下一个 capability
    u16 control;           // ATS 控制寄存器
    // bit 15: ATS Enable
    // bit 14: Invalidate Queue Depth
    // bits 13-0: 保留
};

// 启用 ATS
pci_write_config_word(dev, ats_control_offset, ATS_ENABLE);
```

### IOMMU 侧

```c
// Intel VT-d 中的 ATS 支持
// DMAR 表中声明 ATS 能力
// IOMMU 驱动处理 Translation Request
// IOMMU 驱动发送 Invalidation Request

// Linux 内核中的 ATS 支持
// drivers/iommu/intel/ats.c
int intel_enable_ats(struct pci_dev *dev, int queue_depth)
{
    // 分配 ATS 队列
    // 启用设备的 ATS 能力
    // 注册 Invalidation 处理
}
```

---

## 📈 ATS 的性能测量

### 测试场景

```
场景 1：顺序读写（大量连续地址）
  无 ATS：每次都要查 IOMMU
  有 ATS：第一次查 IOMMU，后续命中缓存
  
  预期：ATS 提升 5-10x

场景 2：随机读写（地址分散）
  无 ATS：每次都要查 IOMMU
  有 ATS：缓存命中率低
  
  预期：ATS 提升 1-2x

场景 3：频繁 unmap/map
  无 ATS：直接操作 IOMMU 页表
  有 ATS：每次都要失效 Device TLB
  
  预期：ATS 可能降低性能（失效开销）
```

### 实际测量方法

```bash
# 使用 perf 测量 IOMMU 查询次数
perf stat -e iommu:map,iommu:unmap ./benchmark

# 使用 ftrace 跟踪 ATS 事件
echo 1 > /sys/kernel/tracing/events/amd_iommu/ats/enable
./benchmark
cat /sys/kernel/tracing/trace

# 测量延迟
fio --ioengine=libaio --direct=1 --rw=randread \
    --bs=4k --iodepth=64 --numjobs=4
```

---

## ⚠️ ATS 的注意事项

### 1. 硬件支持

**不是所有设备都支持 ATS**：
- 需要设备硬件实现 ATS 逻辑
- 需要 IOMMU 支持 ATS
- 需要 PCIe 根复合体支持

**检查方法**：
```bash
lspci -vvv | grep ATS
# 如果输出包含 "ATS: Enable+" 则支持
```

### 2. 安全性考虑

**ATS 的安全风险**：
- Device TLB 可能缓存错误的翻译
- 如果失效机制有 bug，可能导致设备访问错误的内存
- 在虚拟化环境中，可能导致 VM 之间的隔离被破坏

**缓解措施**：
- 严格的失效协议
- 定期的缓存清理
- IOMMU 的访问权限检查（即使 ATS 命中也要检查）

### 3. 调试困难

**ATS 相关的问题难以调试**：
- Device TLB 的状态不可见
- 失效请求可能丢失或延迟
- 缓存一致性问题难以复现

**调试工具**：
- IOMMU 的 fault 日志
- PCIe 协议分析仪
- ftrace 跟踪

---

## 💡 关键要点总结

1. **ATS 的本质**：设备侧的翻译缓存，减少 IOMMU 查询
2. **工作机制**：Translation Request → 缓存 → 直接使用
3. **核心挑战**：失效机制必须正确且高效
4. **性能收益**：频繁访问相同地址时，提升 5-10x
5. **适用场景**：高性能存储、网络设备、GPU 直通
6. **与 Phase 3 的联系**：ATS 引入了第三层缓存，增加了失效的复杂性

---

## 🎯 下一步学习

1. **深入理解失效机制**：
   - Invalidation Request/Completion 的 PCIe TLP 格式
   - 失效队列的管理
   - 同步失效的延迟分析

2. **性能优化**：
   - ATS 缓存大小调优
   - 失效批处理优化
   - 预取策略

3. **虚拟化场景**：
   - VFIO 中的 ATS 支持
   - SR-IOV 与 ATS 的结合
   - 嵌套虚拟化中的 ATS

4. **实际测量**：
   - 在不同工作负载下测量 ATS 的性能收益
   - 分析失效开销
   - 对比有/无 ATS 的延迟和吞吐量

---

## 📚 参考资料

- PCIe Base Specification r6.0, Section 10 (ATS)
- Intel VT-d Specification, Section 1.4 (ATS)
- Linux Kernel: `drivers/iommu/intel/ats.c`
- "Address Translation Services (ATS) in PCIe", PCI-SIG

---

**下一步**：[`phase7 README §1.5`](../phase7-vfio/README.md)（ACS/ATS 直通依赖的实测讨论）
＋ [`phase3 invalidation.md`](../phase3-iommu/invalidation.md)（Device-TLB 失效实现）；
读前过一遍 AGENTS.md #14（STU/bit5/队列深度/关闭时序四个坑）

**文档版本**: v1.0  
**最后更新**: 2026-11-15  
**状态**: 已完成
