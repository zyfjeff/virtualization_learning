# Phase 4｜PCIe 总线与设备直通

> PCIe 是理解设备直通的基石：拓扑、配置空间、关键能力（ACS/ATS/SR-IOV），最终与 VFIO 直通结合。
> 前置：phase3（IOMMU）、phase2（EPT）、phase1（CPU 虚拟化）
> 版本基线：Linux 6.12.93 / QEMU 10.1.0-rc2 | 预计学习时间：2-3 周
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)（统一学习循环 §3）。

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

> 覆盖度说明：SR-IOV / AER 目前只有概览级覆盖（`basics.md` §5），深入文档待补。

---

## 📚 前置与衔接

本阶段假设你已经完成 Phase 1-3，掌握了：
- ✅ IOMMU 基础概念（Phase 3）
- ✅ 内存虚拟化 EPT（Phase 2）
- ✅ CPU 虚拟化基础（Phase 1）

**VMM 经验衔接**：你在 QEMU 里用 `-device` 挂过 PCIe 设备、见过 q35 的虚拟拓扑；
本阶段讲宿主内核如何发现/隔离**物理** PCIe 设备——两套拓扑互不相干，
直通就是把物理设备经 VFIO 插进虚拟拓扑（见「VMM视角对比」）。

建议学习顺序：Phase 3 → Phase 4（本阶段）→ Phase 5（中断）→ Phase 7（VFIO）

---

## 📂 本章文件

| 文件 | 内容 | 何时读 |
|------|------|--------|
| `README.md` | 本文件：技术全景 + 学习路线 | 入口 |
| [`basics.md`](basics.md) | PCIe 硬件基础：拓扑、BDF、配置空间、BAR、能力概览（含 SR-IOV/AER 简介） | 步 2 |
| [`acs-upstream-forwarding.md`](acs-upstream-forwarding.md) | ACS Upstream Forwarding (UF) 深入：UF 与 RR 分工、完整 ACS 配置 | 步 2 深入 |
| [`ats.md`](ats.md) | ATS 深入：设备侧翻译、失效机制、与 IOMMU 协作、启用时序 | 步 2 深入 |
| [`annotations.md`](annotations.md) | 源码精读：设备发现 / Bridge / ACS 检查 / 配置访问 / 数据结构 | 步 4 |
| [`corrections.md`](corrections.md) | 勘误记录 | 步 7 |
| [`practice/`](practice/README.md) | 4 个编号实验（拓扑 / 配置空间 / ACS / IOMMU group） | 步 5 |

> VFIO 集成（IOMMU group 划分、ACS/ATS 直通依赖）的完整讨论在
> [`../phase7-vfio/README.md`](../phase7-vfio/README.md) §1.4–1.5，本章先打好 PCIe 地基。

---

## 📖 学习路线

```
第1步: 定位与目标 — 读本文件「学习目标」
  → 带着问题进入：为什么有些设备不能单独直通？

第2步: 硬件全景 — 读本文件「技术全景」+ basics.md
  → 拓扑（RC/Switch/Endpoint）、BDF、配置空间、BAR
  → 深入：acs-upstream-forwarding.md（UF 与 RR 分工）、ats.md（设备侧翻译）
  → 想：硬件提供了什么隔离/翻译能力？软件怎么用？

第3步: VMM 对比 — 读本文件「VMM视角对比」
  → 想：QEMU 的虚拟拓扑 vs 内核的物理拓扑，边界在哪？

第4步: 源码精读 — annotations.md §1–5
  → 设备发现 → Bridge/Bus Number → ACS 检查 → 配置访问 → 数据结构
  → 目标：读懂每行代码的"为什么"

第5步: 动手验证 — practice/README.md，实验 ex1–ex4
  → 拓扑分析 / 配置空间读写 / ACS 验证 / IOMMU group 分析

第6步: 性能与测量 — 读本文件「性能与测量」
  → 指针 → phase10；本章机制的实测数据现状

第7步: 陷阱与勘误 — 读本文件「陷阱与勘误」+ corrections.md
  → 对照 AGENTS.md 全局陷阱 #10–14（全部与本章相关）

第8步: 自测与衔接 — 本文件「阶段检验清单」
  → 全部通过 → phase5（中断）→ phase7（VFIO 直通）
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

## 🔍 VMM视角对比

> QEMU 侧基线 10.1.0-rc2，内核侧 6.12.93。直通全栈见 phase7。

| 职责 | 用户态（QEMU） | 内核（Linux） | 交接点 |
|------|---------------|--------------|--------|
| PCIe 拓扑 | q35 构造**虚拟**拓扑：host bridge 模拟（`hw/pci-host/q35.c`）+ `-device` 端点；guest `lspci` 看到的是这套 | 枚举**物理**拓扑：`pci_scan_child_bus_extend()`（`drivers/pci/probe.c:2963`，annotations §1） | 两套拓扑互不相干；直通设备以 `vfio-pci` 身份插进虚拟拓扑 |
| 配置空间访问 | 模拟 guest 配置读写（`hw/pci/pci_host.c`）；直通设备的配置访问走 VFIO config region：`vfio_pci_read_config()`（`hw/vfio/pci.c:1366`）/ `vfio_pci_write_config()`（`:1399`） | 物理配置访问：`pci_read_config_byte()`（`drivers/pci/access.c:560`）→ `pci_bus_read_config_byte()`；x86 legacy 路径 `pci_conf1_read()`（`arch/x86/pci/direct.c:21`，annotations §4） | VFIO PCI config region（pread/pwrite） |
| 隔离与 IOMMU group | **不做隔离判定**，只按 group 粒度打开 `/dev/vfio/<group>`（phase7） | ACS 判定 `pci_acs_flags_enabled()`（`drivers/pci/pci.c:3583`，annotations §3），quirk 优先（AGENTS.md #11）；probe 时 `pci_enable_acs()` 主动写 ACS Control（AGENTS.md #13） | IOMMU group = 直通最小粒度 |

---

## 📖 源码精读入口

| 环节 | annotations.md | 关键入口（源码） |
|------|---------------|-----------------|
| 设备发现与枚举 | §1 | `pci_scan_child_bus_extend()` — `drivers/pci/probe.c:2963` |
| Bridge 与 Bus Number | §2 | `PCI_PRIMARY_BUS` / `PCI_SECONDARY_BUS` / `PCI_SUBORDINATE_BUS` — `include/uapi/linux/pci_regs.h:132-134` |
| ACS 检查 | §3 | `pci_acs_flags_enabled()` — `drivers/pci/pci.c:3583`（quirk 层 `pci_dev_specific_acs_enabled()` 见 AGENTS.md #11） |
| 配置访问 | §4 | `pci_read_config_byte()` — `drivers/pci/access.c:560`；`pci_conf1_read()` — `arch/x86/pci/direct.c:21` |
| 关键数据结构 | §5 | `struct pci_dev` — `include/linux/pci.h:336` |

---

## 🔬 实践练习

4 个编号实验（均在宿主执行，需 root + `pciutils`，详见 [`practice/README.md`](practice/README.md)）：

| 编号 | 主题 | 核心问题 | 入口 |
|------|------|---------|------|
| ex1 | PCIe 拓扑分析 | RC/Switch/Endpoint 如何分布？ | `practice/ex1-pcie-topology.sh` |
| ex2 | 配置空间读写 | Type 0/1 区别？BAR 如何解析？ | `practice/ex2-config-space.sh` |
| ex3 | ACS 能力分析 | 哪些设备支持 ACS？如何影响 group？ | `practice/ex3-acs-analysis.sh` |
| ex4 | IOMMU Group 分析 | 设备如何分组？直通约束是什么？ | `practice/ex4-iommu-group.sh` |

实验报告在 `practice/reports/`。

---

## ⚡ 性能与测量

- 跨 phase 实测结论与可信度分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
- 测量方法（重复/噪声/观测者扰动）：[`../phase10-performance/measurement.md`](../phase10-performance/measurement.md)
- 本章现状：**无本仓实测数据**。`ats.md`「性能分析」与 `acs-upstream-forwarding.md`
  「性能影响分析」中的数字（如 "IOMMU 查询减少 80-95%"、"100-500 ns"）是**无出处的机制推算**，
  不得当实测结论引用（已在 corrections.md 登记待审计）。配置访问开销、ATS 命中收益等标**待实测**。

---

## ⚠️ 陷阱与勘误

本章是 AGENTS.md 全局陷阱的"重灾区"，**读文档前先过一遍**：

1. **#10**：`PCI_EXP_TYPE_PCI_BRIDGE` 与 `PCI_EXP_TYPE_PCIE_BRIDGE` 名字与含义正好相反
2. **#11**：`ACSCap` 里的 `-` 不算失败；quirk 判定优先于 PCIe 类型判断
3. **#12**：`pci_enable_acs()` 的 quirk 判据是反的（命中 quirk 反而跳过标准写入）
4. **#13**：ACS 不是只读的；`pci=config_acs=` 标志串**从右往左**解析
5. **#14**：ATS 的 STU 是位移量之差、Page Aligned Request 在 bit5、队列深度 0 有两种身份、Linux 关闭时序与规范推荐不同

- 本章勘误（含 basics.md 行号待修项）：[`corrections.md`](corrections.md)
- ACS/ATS 直通依赖的完整实测讨论：[`../phase7-vfio/README.md`](../phase7-vfio/README.md) §1.5

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

---

## 🚀 下一步

- **Phase 5（中断虚拟化）**：MSI/MSI-X 消息格式与 VT-d IR/Posted Interrupts
- **Phase 6（virtio）**：半虚拟化设备——与直通互为对照
- **Phase 7（VFIO 直通）**：本章内容的直接应用（§1.4 group 划分、§1.5 ACS/ATS 依赖）
- **本章进阶**：SR-IOV / AER 深入文档（待补）

---

## 📚 参考资料

- **PCIe Base Specification**：`pcie-base-spec-r6.0.pdf`（本仓库已包含）
- **Intel VT-d Specification**：`intel-vtd.pdf`（IOMMU 相关章节）
- **Linux Kernel Documentation**：`Documentation/PCI/` 目录
- **lspci 源码**：`pciutils` 项目
