# Phase 4｜PCIe 总线与设备直通

> PCIe 是理解设备直通的基石：拓扑、配置空间、关键能力（ACS/ATS/SR-IOV），最终与 VFIO 直通结合。
> 前置：phase3（IOMMU）、phase2（EPT）、phase1（CPU 虚拟化）
> 版本基线：Linux 6.12.93 / QEMU 10.1.0-rc2 | 预计学习时间：2-3 周
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)（统一学习循环 §3）。

---

## 📋 学习目标

**核心问题**：为什么有些设备不能单独直通？——隔离粒度不是软件策略，
而是 PCIe 拓扑的物理事实。本章从第一性原理推出这个结论，再读内核怎么把它变成 IOMMU group。

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

文件不是清单，是问题链的接力站——每个文件回答上一站引出的问题，再把自己的问题交给下一站：

```
README 第一性原理（核心问题：隔离粒度为什么是物理事实？）
  │ 引出：拓扑长什么样？BDF/配置空间怎么读？
  ▼
basics.md（硬件词汇表：拓扑/BDF/配置空间/BAR/能力概览）
  │ 引出①：ACS 怎么关掉 P2P 短路？UF 和 RR 什么分工？
  │ 引出②：ATS 省了什么、代价是什么？
  ├──────────────────────────────┐
  ▼                              ▼
acs-upstream-forwarding.md     ats.md
  │ 引出：内核怎么判定隔离？       │ 引出：失效时序谁管？直通怎么用？
  ▼                              ▼
annotations.md §3             phase7 §1.5 + phase3 invalidation.md
  │ 链尾：隔离怎么落成 group 粒度？
  ▼
practice ex1–ex4（真机取证）── 与直觉矛盾？──→ corrections.md + AGENTS.md #10–14
```

| 文件 | 回答什么 | 引出什么 → 去哪 |
|------|---------|----------------|
| `README.md` | 核心问题 + 第一性原理四问 | 拓扑怎么读 → [`basics.md`](basics.md) |
| [`basics.md`](basics.md) | 拓扑/BDF/配置空间/BAR/能力概览（含 SR-IOV/AER 简介） | ACS 隔离细节 → [`acs-upstream-forwarding.md`](acs-upstream-forwarding.md)；ATS 取舍 → [`ats.md`](ats.md) |
| [`acs-upstream-forwarding.md`](acs-upstream-forwarding.md) | UF/RR 分工、完整 ACS 配置 | 内核判定逻辑 → [`annotations.md`](annotations.md) §3；真机验证 → ex3/ex4 |
| [`ats.md`](ats.md) | 设备侧翻译、失效机制、启用时序 | 直通依赖 → [`phase7 §1.5`](../phase7-vfio/README.md)；失效实现 → [`phase3 invalidation.md`](../phase3-iommu/invalidation.md)；陷阱 → AGENTS.md #14 |
| [`annotations.md`](annotations.md) | 问题链精读 §1–5（设备发现/Bridge/ACS/配置访问/数据结构） | 每节链尾 → 对应实验 ex1–ex4 |
| [`practice/`](practice/README.md) | ex1–ex4 取证（每个实验标注验证哪个问题） | 矛盾与待办 → [`corrections.md`](corrections.md) |
| [`corrections.md`](corrections.md) | 概念性错误证据、待审计登记 | 蒸馏 → AGENTS.md 全局陷阱 / 本文件「陷阱与勘误」 |

> VFIO 集成（IOMMU group 划分、ACS/ATS 直通依赖）的完整讨论在
> [`../phase7-vfio/README.md`](../phase7-vfio/README.md) §1.4–1.5，本章先打好 PCIe 地基。

---

## 📖 学习路线

```
第1步: 定位与目标 — 读本文件「学习目标」
  → 带着问题进入：为什么有些设备不能单独直通？

第2步: 第一性原理与硬件全景 — 读本文件「第一性原理与技术全景」+ basics.md
  → 想：I/O 互连的根本矛盾是什么？为什么串行化却保留配置空间？
  → 拓扑（RC/Switch/Endpoint）、BDF、配置空间、BAR
  → 深入：acs-upstream-forwarding.md（UF 与 RR 分工）、ats.md（设备侧翻译）

第3步: VMM 对比 — 读本文件「VMM视角对比」
  → 想：为什么隔离判定必须在内核？QEMU 的虚拟拓扑 vs 内核的物理拓扑，边界在哪？

第4步: 问题链精读 — annotations.md §1–5，先看下方「问题链精读入口」
  → 每节带着入口问题 + 引申问题读代码（为什么步长 8？为什么两遍扫描？为什么 quirk 优先？）
  → 答不出 = 没读懂，回到链上重读

第5步: 动手验证 — practice/README.md，实验 ex1–ex4
  → 每个实验取证问题链中的一个答案（见实践练习表）

第6步: 性能与测量 — 读本文件「性能与测量」
  → 想：这些设计的代价是什么？指针 → phase10；本章实测数据现状

第7步: 陷阱与勘误 — 读本文件「陷阱与勘误」+ corrections.md
  → 对照 AGENTS.md 全局陷阱 #10–14（全部与本章相关）：直觉错在哪一层？

第8步: 自测与衔接 — 本文件「阶段检验清单」（含深度自测）
  → 全部通过 → phase5（中断）→ phase7（VFIO 直通）
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题

I/O 互连要同时满足三个互相拉扯的需求：**带宽随设备演进持续增长**、
**软件模型保持稳定**（OS/驱动生态不能推倒重来）、虚拟化时代追加的
**流量可隔离、可分配**（直通）。PCIe 的每个关键设计都是这三者的折中。

### 不可变约束

| 约束 | 内容 | 依据 |
|------|------|------|
| 物理 | 并行总线频率受 skew/串扰限制，共享总线仲裁不 scale | PCIe Spec Chapter 1（p.98）："parallel bus implementation is replaced by a highly scalable, fully serial interface" |
| 兼容 | PCI 的 load-store 模型与配置空间软件接口必须保留 | Spec Chapter 1："usage model, load-store architecture, and software interfaces, are maintained from PCI Local Bus" |
| 资源 | BDF 的 bus 字段 8 位 → 每 segment 只有 256 个 bus 号，枚举时是稀缺资源 | `drivers/pci/probe.c:2963` 的 available_buses 分配逻辑（annotations §1） |
| 隔离 | P2P 流量在 Switch 内短路、不经过 RC → IOMMU 看不见 | PCIe Spec §6.12（ACS 的控制点定义） |

### 设计空间与取舍

- **物理层**：串行差分点对点 + Switch  fabric +  packetized（TLP），
  按 lane 扩展带宽——用协议复杂度换频率可扩展性（Spec Chapter 1）。
- **发现**：保留配置空间 = 标准化、与驱动无关的枚举接口，
  内核才能用一套统一算法扫描所有设备（annotations §1 的扫描循环）。
- **隔离**：ACS 不做"所有流量强制过 RC"，而是在拓扑里设**控制点**
  （正常路由/阻止/重定向，Spec §6.12）——保留 P2P 的性能选项，
  把"关掉短路"的能力交给软件。且 ACS **默认关闭**、只能由 ACS-aware 软件启用
  → 这就是 `pci_enable_acs()` 在 probe 时主动写寄存器的原因（AGENTS.md #13）。
- **翻译**：每次 DMA 过 IOMMU vs 设备侧缓存翻译（ATS，Spec Chapter 10）——
  省翻译延迟，换来失效协议的往返开销与设备复杂度（`ats.md`；
  Device-TLB 失效为什么是同步的，见 phase3）。

### 机制全景

#### 为什么 PCIe 对虚拟化如此重要？

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

#### PCIe 在虚拟化栈中的位置

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

#### 关键概念速览

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

## 📖 问题链精读入口

> 完整问题链 + 答案在 annotations.md 各节开头；这里给入口与关键引申。
> 读法：先盖住答案，自己顺着问题读代码；答不出再对答案。

| annotations 节 | 入口问题 | 关键引申问题 | 源码入口 |
|---------------|---------|-------------|---------|
| §1 设备发现 | 没有任何驱动，内核怎么知道有什么设备？ | 为什么扫描步长是 8？为什么先给 SR-IOV 预留 bus 号？为什么 bridge 扫两遍（反事实：去掉第一遍会怎样）？ | `pci_scan_child_bus_extend()` — `drivers/pci/probe.c:2963` |
| §2 Bridge/Bus Number | bridge 为什么需要三个 bus 号？ | 256 个 bus 号用完会怎样？hotplug bridge 为什么要留余量？ | `PCI_PRIMARY_BUS` 等 — `include/uapi/linux/pci_regs.h:132-134` |
| §3 ACS 检查 | "有 ACS 能力"为什么 ≠ "已隔离"？ | 为什么 quirk 优先于 PCIe 类型？为什么没声明的能力算已启用？为什么 probe 时主动写（反事实：不写会怎样）？ | `pci_acs_flags_enabled()` — `drivers/pci/pci.c:3583` |
| §4 配置访问 | CF8/CFC 与 ECAM 为什么并存？ | 为什么 CF8/CFC 要持锁（反事实：去掉锁会怎样）？为什么先查 disconnected？ | `pci_read_config_byte()` — `drivers/pci/access.c:560`；`pci_conf1_read()` — `arch/x86/pci/direct.c:21` |
| §5 数据结构 | 拓扑/配置空间/能力在 `pci_dev` 里如何对应？ | 配置读这么贵，为什么缓存 `acs_cap`？（接 §4 链尾） | `struct pci_dev` — `include/linux/pci.h:336` |

---

## 🔬 实践练习

4 个实验串成一条**取证链**（均在宿主物理机执行，需 root + `pciutils`）。
每个按五段式（§3.5）：先押注预测，再跑观察，脚本自动给 PASS/FAIL/INCONCLUSIVE。
详见 [`practice/README.md`](practice/README.md)。

| 编号 | 验证的理论 | 预测（理论成立必然观察到） | 入口 |
|------|-----------|--------------------------|------|
| ex1 | bus 号递归分配（§1-2） | 子树最远 bus == subordinate；secondary 唯一 | `practice/ex1-pcie-topology.sh` |
| ex2 | Type 0/1 + BAR 编码（basics §3-4） | BAR0 bit0 ↔ lspci 区域类型；64 位 BAR 占两槽 | `practice/ex2-config-space.sh` |
| ex3 | ACS 三层判定（§3 + #11/#13） | Ctl⊇(Cap∩REQ)；Intel RCiEP 无 cap 独占 group；quirk 优先 | `practice/ex3-acs-analysis.sh` |
| ex4 | group=最小隔离集（phase3） | 多成员 group 可归因到隔离缺口 | `practice/ex4-iommu-group.sh` |

**链式衔接**：ex1 拓扑就位 → ex2 配置空间承载能力 → ex3 ACS 判定隔离 → ex4 落成 group 边界 → phase7 直通。
ex4 直接回答本章核心问题：本机 157 个设备落在多成员 group，必须整组直通。
实验报告在 `practice/reports/`；实测发现（含 header-type 边界）见 practice/README 与 corrections。

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

- 本章勘误（basics.md 行号与 annotations §1/§3、ats.md 代码体已于
  2026-11-25 二轮修正；待审计项见条目 11/12）：[`corrections.md`](corrections.md)
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

**深度自测**（答不出 = 没学会，回到问题链重读）：

- [ ] **反事实**：如果没有 ACS，Switch 下两个设备 P2P 通信时 IOMMU 为什么看不见？
      这会造成什么隔离漏洞？（Spec §6.12）
- [ ] **反事实**：去掉 `pci_conf1_read()` 的 `pci_config_lock`，两个 CPU 并发读配置空间
      会具体坏在哪一步？（CF8/CFC 两寄存器间接访问）
- [ ] **不可简化**：bridge 两遍扫描的第一遍看似"什么都没做"，为什么不能省？
      （固件已配置的 bridge；`probe.c` 注释）
- [ ] **推导**：合上代码，从"每 segment 256 个 bus 号 + 热插拔随时可能加设备"
      推出枚举算法必须给 hotplug bridge 预留余量。

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
