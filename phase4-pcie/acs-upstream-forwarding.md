# ACS Upstream Forwarding (UF) 详解

> 基于 PCIe Base Specification r6.0 和实际系统分析
>
> **承接**：[`basics.md`](basics.md) §5 能力概览——你已知道 ACS 管隔离、P2P 流量会绕过 IOMMU。
> **本文回答**：RR 把流量重定向后，UF 凭什么保证它真能到 RC？UF 和 RR 怎么分工？
> **引出**：内核怎么判定"隔离成立"（quirk 为什么优先）→ [`annotations.md`](annotations.md) §3 问题链；
> 真机验证 → [`practice/`](practice/README.md) ex3/ex4。

---

## 🎯 UF 的核心作用

**一句话总结**：UF 确保被 RR 重定向的流量能够真正到达 Root Complex，不会在中途被丢弃。

---

## 🔍 UF 与 RR 的分工

### 工作层级不同

```
PCIe 拓扑层级：

Device A
    ↓
Switch Downstream Port 1  ← RR 在这里工作（拦截 P2P）
    ↓
Switch Internal Fabric
    ↓
Switch Upstream Port      ← UF 在这里工作（确保通过）
    ↓
Root Port                 ← UF 也在这里工作（确保到达 RC）
    ↓
Root Complex (RC)
```

### 各自的职责

| 特性 | RR (Request Redirect) | UF (Upstream Forwarding) |
|------|----------------------|-------------------------|
| **工作位置** | Switch Downstream Ports | Switch Upstream Ports + Root Ports |
| **主要作用** | 主动拦截 P2P 请求，强制改道 | 确保改道后的流量能到达 RC |
| **工作时机** | 请求刚进入 Switch 时 | 请求向上游传播过程中 |
| **类比** | 交通警察（改道） | 路标（保通） |

---

## 💡 为什么需要 UF？

### 场景 1：只有 RR，没有 UF

```
Device A → Port 1 (RR 拦截) → Upstream Port → ???
                                    ↓
                            可能被丢弃！
                            因为 Upstream Port 不知道
                            这个请求是被 RR 重定向的
```

**问题**：请求被 RR 重定向后，可能在 Switch 内部被丢弃或处理不当。

### 场景 2：只有 UF，没有 RR

```
Device A → Port 1 → Switch 内部 P2P 路由 → Port 2 → Device B
               ↓
         P2P 直接完成
         UF 看不到！
```

**问题**：UF 只能处理已经向上游传播的流量，无法拦截 P2P。

### 场景 3：RR + UF（正确配置）

```
Device A → Port 1 (RR 拦截) → Upstream Port (UF 确保通过) → Root Port (UF 确保通过) → RC
                                                                          ↓
                                                              RC 处理后往下游转发
                                                                          ↓
                                                              Port 2 → Device B
```

**优势**：
- ✓ RR 负责拦截和改道
- ✓ UF 负责确保通过
- ✓ 完整的路径，不会丢失
- ✓ IOMMU 能看到所有流量

---

## 🔄 完整的 ACS 配置

### Linux 内核的要求

```c
// drivers/iommu/iommu.c
#define REQ_ACS_FLAGS   (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)
```

**四个标志的作用**：

| 标志 | 作用 | 必要性 |
|------|------|--------|
| **SV** (Source Validation) | 验证请求源地址 | 安全性增强 |
| **RR** (Request Redirect) | 重定向 P2P 请求到 RC | **核心** |
| **CR** (Completion Redirect) | 重定向 P2P 完成到 RC | **核心** |
| **UF** (Upstream Forwarding) | 确保重定向的流量到达 RC | **核心** |

### 为什么 Linux 要求全部启用？

```
完整隔离 = RR + CR + UF

RR：确保请求经过 IOMMU
CR：确保响应经过 IOMMU
UF：确保 RR/CR 重定向的流量不会丢失

三者缺一不可！
```

---

## 📊 性能影响分析

### UF 是否会导致带宽翻倍？

**答案**：**不会！** UF 不会造成循环。

```
错误理解：
  Device A → Port 1 (RR) → RC → Port 2 (UF) → RC → Device B
                                  ↑              ↑
                               第一次到 RC    又被 UF 送回 RC？
                               ✗ 这是循环！

正确理解：
  Device A → Port 1 (RR 拦截) → Upstream Port (UF 确保通过) → RC
                                                              ↓
                                                    RC 处理后往下游转发
                                                              ↓
                                                    Port 2 → Device B
```

**关键点**：
- UF 只在请求**向上游传播**的过程中起作用
- 一旦到达 RC，RC 往下游转发时，UF 不再介入
- **不会造成循环**！

### 实际性能影响

| 场景 | 没有 ACS | 有 ACS (RR+CR+UF) | 性能差异 |
|------|---------|-------------------|---------|
| **P2P 流量** | Switch 内部路由（快速） | 经过 RC（较慢） | ⚠️ 延迟增加 |
| **普通流量** | 经过 RC | 经过 RC | ✓ 无差异 |
| **整体影响** | - | - | P2P 流量少，影响小 |

---

## 🎯 实际应用

### 你的系统配置

```
检查结果显示：
  - 65 个设备启用了完整的 ACS (SV+RR+CR+UF)
  - 主要是 Switch Downstream Ports
  - 这是正确的配置（用于虚拟化隔离）
```

### 为什么云服务器需要完整 ACS？

1. **多租户隔离**：VM 之间不能直接通信
2. **安全性**：所有流量必须经过 IOMMU 检查
3. **地址翻译**：确保 GPA → HPA 正确翻译
4. **合规性**：符合虚拟化安全标准

---

## 💡 关键要点总结

1. **UF 不是循环**：它只在向上游传播时起作用，不会造成请求反复经过 RC
2. **UF 和 RR 互补**：RR 负责拦截，UF 负责保通
3. **完整 ACS = RR + CR + UF**：三者缺一不可
4. **性能影响小**：P2P 流量在实际系统中很少见
5. **安全性优先**：在虚拟化环境中，隔离比性能更重要

---

## 📚 参考资料

- PCIe Base Specification r6.0, Section 6.12 (Access Control Services)
- Linux Kernel: `drivers/iommu/iommu.c` - REQ_ACS_FLAGS 定义
- Linux Kernel: `drivers/pci/pci.c` - ACS 检查逻辑

---

**下一步**：[`annotations.md`](annotations.md) §3（内核 ACS 判定问题链：quirk 优先 / 未声明算启用 / probe 主动写）
→ [`practice/`](practice/README.md) ex3/ex4（真机取证）

**文档版本**: v1.0  
**最后更新**: 2026-11-15  
**状态**: 已完成
