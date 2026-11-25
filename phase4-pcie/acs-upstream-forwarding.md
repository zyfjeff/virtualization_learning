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
PCIe 拓扑层级（控制位所在 Port，依据 PCIe Spec r6.0 §6.12.1）：

Device A
    ↓
Switch Downstream Port 1  ← RR 在这里工作（拦截 P2P 请求改道）
    ↓                      ← UF 也在这里工作：Ingress 收到指向本端口
Switch Internal Fabric        Egress 的 Upstream TLP 时，改向 Upstream 转发
    ↓
Switch Upstream Port        ←（UF 控制位不在此——spec 只在
    ↓                          DSP/Root Port 要求实现 UF）
Root Port                   ← UF 在这里工作（配合 Redirected Request
    ↓                          Validation 把重定向流量交给 TA 处理）
Root Complex (RC)
```

> **勘误（2026-11-25）**：旧版把 UF 画在 "Switch Upstream Port + Root Port"。
> PCIe 6.0 §6.12.1 原文："ACS Upstream Forwarding: must be implemented by **Root
> Ports** if the RC supports Redirected Request Validation; must be implemented by
> **Switch Downstream Ports**"，且行为定义在 DSP："When ACS Upstream Forwarding
> is enabled in a **Switch Downstream Port**, and its Ingress Port receives an
> Upstream Request or Completion TLP targeting the Port's own Egress Port, the
> Port must instead forward the TLP Upstream towards the RC."——UF 字段挂在
> DSP/Root Port 上，不在 Upstream Port；§6.12.2 说的"中间 Switch 必须支持 UF"
> 指的就是这些 Switch 的下游端口。

### 各自的职责

| 特性 | RR (Request Redirect) | UF (Upstream Forwarding) |
|------|----------------------|-------------------------|
| **控制位所在** | Root Ports + Switch Downstream Ports（多功能设备/RCiEP 侧另有 RR 要求） | Root Ports（若 RC 支持 Redirected Request Validation）+ Switch Downstream Ports |
| **主要作用** | 把原本要 P2P 转发的 Request 重定向到上游 | 把 Ingress 收到、指向自身 Egress 的 upstream TLP 强制转发向上游（不使能时处理 undefined） |
| **工作时机** | 请求进入端口、即将 P2P 短路时 | 任何"本可下游短路"的 TLP（含 RR 重定向产物） |
| **类比** | 交通警察（改道） | 同一路口的"禁止就地掉头、一律送上主路"规则 |

---

## 💡 为什么需要 UF？

### 场景 1：只有 RR，没有 UF

```
Device A → Port 1 (RR 拦截, 改道向 Upstream) → 中间端口 → ???
                                    ↓
                     §6.12.2: 若路径上的 Switch/Root Port 不支持 UF，
                     "how such Switches handle Upstream redirected
                      TLPs is undefined" —— 行为未定义（可能丢弃/误投）
```

**问题**：RR 只是把 TLP 推向"Upstream 方向"；从 DSP 到 RC 沿途端口若没有
UF 语义兜底，规范明确说这些重定向 TLP 的处理是 undefined——隔离链在
第一个不支持 UF 的端口处断掉（不是"Upstream Port 有 UF 位"，UF 位挂在
沿途 DSP/Root Port 上）。

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
Device A → Port 1 (RR 拦截 + 本端口 UF 强制上送) → 沿途端口 UF 保通 → Root Port (UF/RRA 交 TA) → RC
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
  Device A → Port 1 (RR 拦截；本端口 UF 把"想去自己 Egress"的流量送上游)
           → 沿途端口 UF 保通 → RC
                                  ↓
                       RC 处理后往下游转发（这是正常路由，
                       不属于任何端口的 UF 语义）
                                  ↓
                       Port 2 → Device B
```

**关键点**：
- UF 只在请求**向上游传播**的过程中起作用（控制位在 DSP/Root Port，见上文勘误）
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
