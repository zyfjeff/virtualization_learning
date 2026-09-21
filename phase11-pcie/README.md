# Phase 11：PCIe 总线与设备直通

> 基于 Linux 6.12.93 内核源码 | 预计学习时间：2-3 周
>
> PCIe 是理解设备直通的基石。本阶段从 PCIe 硬件基础出发，深入拓扑结构、配置空间、
> 关键能力（ACS/ATS/SR-IOV），最终与 VFIO 设备直通紧密结合。

---

## 📚 前置知识

本阶段假设你已经完成 Phase 1-10，掌握了：
- ✅ IOMMU 基础概念（Phase 3）
- ✅ 中断虚拟化与 VT-d（Phase 4）
- ✅ VFIO 设备直通基础（Phase 6）

如果你还没有完成 Phase 6，建议先学习 `../phase6-vfio/README.md`。

---

## 📋 学习目标

完成本阶段后，你应该能够：

1. **PCIe 基础**
   - 画出 PCIe 拓扑结构（Root Complex、Switch、Endpoint）
   - 理解 BDF（Bus/Device/Function）寻址机制
   - 解释配置空间的组织结构（Type 0/1/2）
   - 理解 BAR（Base Address Register）的作用和映射方式

2. **PCIe 拓扑与枚举**
   - 描述 PCIe 设备的发现过程
   - 理解 Bridge 的类型和作用（PCIe-to-PCIe、PCIe-to-PCI）
   - 解释 Secondary/Subordinate Bus Number 的作用
   - 用 `lspci` 和 `setpci` 工具探索实际拓扑

3. **关键 PCIe 能力**
   - **ACS（Access Control Services）**：隔离机制、P2P 控制、与 IOMMU group 的关系
   - **ATS（Address Translation Services）**：设备侧 IOTLB、与 IOMMU 的协作
   - **SR-IOV（Single Root I/O Virtualization）**：PF/VF 概念、虚拟化场景
   - **AER（Advanced Error Reporting）**：错误分类、恢复机制

4. **PCIe 与 VFIO 结合**
   - 解释 IOMMU group 如何由 PCIe 拓扑决定
   - 理解 ACS 如何影响设备直通
   - 分析设备直通的拓扑约束（为什么某些设备必须一起直通）
   - 诊断常见的直通失败场景

5. **实战能力**
   - 使用 `lspci -tv` 分析 PCIe 拓扑
   - 使用 `setpci` 读写配置空间
   - 用 ftrace 追踪 PCIe 配置访问
   - 诊断 ACS/ATS 相关的直通问题

---

## 📂 本章文件

| 文件 | 内容 |
|------|------|
| `README.md` | 本文件：技术全景 + 学习路线 |
| [`basics.md`](basics.md) | PCIe 硬件基础：拓扑结构、BDF 寻址、配置空间、BAR |
| [`topology.md`](topology.md) | PCIe 拓扑枚举：设备发现、Bridge、Bus Number 分配 |
| [`capabilities.md`](capabilities.md) | PCIe 能力详解：ACS / ATS / SR-IOV / AER |
| [`vfio-integration.md`](vfio-integration.md) | PCIe 与 VFIO：IOMMU group、ACS 要求、直通约束 |
| `annotations.md` | 源码精读：PCIe 配置访问、ACS 检查、IOMMU group 构造 |
| `practice/` | 动手实验：拓扑分析、配置空间读写、ACS 验证 |
| `corrections.md` | 勘误记录 |

---

## 📖 推荐阅读顺序

```
第1步: README.md (本文件) — 技术全景
  → 理解 PCIe 在虚拟化中的角色
  → 掌握学习路线和关键概念

第2步: basics.md — PCIe 硬件基础
  → 拓扑结构（RC/Switch/Endpoint）
  → BDF 寻址和配置空间
  → BAR 映射机制
  → 目标：能用 lspci 识别设备类型

第3步: topology.md — 拓扑枚举
  → 设备发现过程
  → Bridge 类型和作用
  → Bus Number 分配
  → 目标：能画出完整拓扑树

第4步: capabilities.md — 关键能力
  → ACS：隔离机制和 P2P 控制
  → ATS：设备侧地址转换
  → SR-IOV：虚拟化扩展
  → AER：错误报告
  → 目标：理解每个能力的作用和影响

第5步: vfio-integration.md — VFIO 集成
  → IOMMU group 与拓扑的关系
  → ACS 如何影响直通
  → 直通约束和诊断
  → 目标：能诊断直通失败问题

第6步: annotations.md — 源码精读
  → PCIe 配置访问的内核实现
  → ACS 检查逻辑
  → IOMMU group 构造算法
  → 目标：读懂每行代码的"为什么"

第7步: practice/ — 动手实验
  → 实验 1：拓扑分析与可视化
  → 实验 2：配置空间读写
  → 实验 3：ACS 验证与直通测试
```

---

## 🏗️ 技术全景

### 为什么 PCIe 对虚拟化如此重要？

```
┌─────────────────────────────────────────────────────────────┐
│              PCIe 是设备直通的硬件基础                         │
│                                                              │
│  设备直通的三个关键问题：                                      │
│  1. 隔离性：设备能否独立分配给某个 VM？                        │
│     → 由 PCIe 拓扑 + ACS + IOMMU group 决定                  │
│                                                              │
│  2. 性能：设备访问内存的延迟和带宽？                           │
│     → 由 IOMMU 翻译 + ATS + 拓扑位置决定                     │
│                                                              │
│  3. 中断：设备中断如何投递到 Guest？                           │
│     → 由 MSI-X + VT-d IR + Posted Interrupts 决定            │
│                                                              │
│  理解 PCIe = 理解设备直通的"物理层"                            │
└─────────────────────────────────────────────────────────────┘
```

### PCIe 在虚拟化栈中的位置

```
┌─────────────────────────────────────────────────────────────┐
│  Guest                                                      │
│    ↓ 设备驱动（vfio-pci）                                    │
│  QEMU / VMM                                                 │
│    ↓ VFIO API                                               │
│  Linux VFIO 驱动                                             │
│    ↓ IOMMU API                                              │
│  Linux IOMMU 驱动（Intel VT-d / AMD-Vi）                     │
│    ↓ 硬件                                                   │
│  IOMMU 硬件                                                  │
│    ↓ 地址翻译                                                │
│  PCIe 设备                                                   │
│    ↓ DMA 请求                                                │
│  系统内存                                                    │
└─────────────────────────────────────────────────────────────┘

PCIe 的作用：
- 定义设备的物理连接和拓扑
- 提供配置空间（设备发现、能力协商）
- 定义中断机制（MSI/MSI-X）
- 提供高级能力（ACS/ATS/SR-IOV）
```

### 关键概念速览

| 概念 | 作用 | 虚拟化影响 |
|------|------|-----------|
| **BDF** | Bus/Device/Function 寻址 | 设备标识、拓扑定位 |
| **配置空间** | 设备能力、BAR、中断配置 | 设备发现、能力协商 |
| **BAR** | 内存/IO 映射区域 | 设备内存映射、MMIO |
| **Bridge** | 连接不同 PCIe 段 | 拓扑层次、Bus Number 分配 |
| **ACS** | 访问控制、P2P 隔离 | IOMMU group 划分、直通约束 |
| **ATS** | 设备侧地址缓存 | 减少 IOMMU 翻译开销 |
| **SR-IOV** | 单根多虚拟化 | 设备共享、VF 直通 |
| **AER** | 错误报告与恢复 | 故障诊断、系统稳定性 |

---

## 🔬 实践练习

本阶段包含 3 个动手实验：

### 实验 1：PCIe 拓扑分析与可视化
- 使用 `lspci -tv` 分析系统 PCIe 拓扑
- 绘制拓扑树，标注设备类型和 Bridge
- 识别可独立直通的设备和必须一起直通的组

### 实验 2：配置空间读写
- 使用 `setpci` 读写设备配置空间
- 修改 BAR 基地址（危险操作，需谨慎）
- 启用/禁用设备能力

### 实验 3：ACS 验证与直通测试
- 检查设备的 ACS 能力
- 分析 ACS 对 IOMMU group 的影响
- 尝试直通 ACS 配置不同的设备

---

## 📚 参考资料

- **PCIe Base Specification**：`pcie-base-spec-r6.0.pdf`（本仓库已包含）
- **Intel VT-d Specification**：`intel-vtd.pdf`（IOMMU 相关章节）
- **Linux Kernel Documentation**：`Documentation/PCI/` 目录
- **lspci 源码**：`pciutils` 项目

---

## ✅ 阶段检验清单

完成本阶段后，你应该能够回答以下问题：

- [ ] 画出 PCIe 拓扑结构，标注 Root Complex、Switch、Endpoint
- [ ] 解释 BDF 寻址机制，给定 BDF 定位设备
- [ ] 区分 Type 0/1/2 配置空间的用途
- [ ] 解释 BAR 的作用和映射方式
- [ ] 描述 PCIe 设备的发现过程
- [ ] 区分 PCIe-to-PCIe Bridge 和 PCIe-to-PCI Bridge
- [ ] 解释 Secondary/Subordinate Bus Number 的作用
- [ ] 说明 ACS 的作用和与 IOMMU group 的关系
- [ ] 解释 ATS 的工作原理和性能影响
- [ ] 描述 SR-IOV 的 PF/VF 概念
- [ ] 分析为什么某些设备必须一起直通
- [ ] 诊断 ACS 相关的直通失败问题
- [ ] 使用 `lspci` 和 `setpci` 工具探索 PCIe 拓扑
- [ ] 用 ftrace 追踪 PCIe 配置访问
