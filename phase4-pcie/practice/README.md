# Phase 4 深度实践 — PCIe 总线源码精读配套练习

> 基于 Linux 6.12.93 源码。每个练习连接 `../annotations.md` 的源码精读，
> 要求**先读懂源码，再做实验验证**。
>
> **执行环境**：
> - 所有练习在**宿主侧**执行（分析宿主 PCIe 拓扑和配置空间）
> - 需要 root 权限（读写配置空间、访问 sysfs）

---

## 练习总览

| # | 名称 | 核心问题 | 连接 annotations.md | 执行位置 |
|---|------|---------|-------------------|---------|
| 1 | PCIe 拓扑分析 | 系统的 PCIe 拓扑是什么？RC/Switch/Endpoint 如何分布？ | §1 设备发现 + basics §1-2 | 宿主 |
| 2 | 配置空间读写 | Type 0 和 Type 1 的配置空间有什么区别？BAR 如何解析？ | §4 配置访问 + basics §3-4 | 宿主 |
| 3 | ACS 能力分析 | 哪些设备支持 ACS？ACS 如何影响 IOMMU group？ | §3 ACS 检查 + CLAUDE.md #11 | 宿主 |
| 4 | IOMMU Group 分析 | 设备如何被划分到不同 group？直通约束是什么？ | §3 IOMMU group + CLAUDE.md #11 | 宿主 |

---

## 环境准备

```bash
# 确认 lspci 和 setpci 可用
which lspci setpci
# 如未安装: apt install pciutils

# 确认 IOMMU 已启用
ls /sys/kernel/iommu_groups/
# 如果不存在，在 grub 中添加: intel_iommu=on 或 amd_iommu=on

# 确认有 root 权限
sudo -v
```

---

## Exercise 1: PCIe 拓扑分析

### 目标

完整分析系统的 PCIe 拓扑结构，理解 RC / Switch / Endpoint 的层次关系。

**核心问题**：你的系统有几个 PCIe Root Port？是否有 Switch？设备如何连接到 RC？

### 连接 annotations.md

- §1「PCIe 设备发现与枚举」：`pci_scan_child_bus_extend()` 递归扫描
- §1「pci_scan_slot()」：Function 0 检测 + 多功能设备扫描
- basics.md §1 — 拓扑结构（RC / Switch / Endpoint）
- basics.md §2 — BDF 寻址机制

### 步骤

```bash
sudo ./ex1-pcie-topology.sh
```

脚本会：
1. 使用 `lspci -tv` 绘制拓扑树
2. 统计 Endpoint 和 Bridge 数量
3. 识别 Root Complex 和 Root Port
4. 识别 PCIe Switch 端口
5. 列出完整 BDF 列表
6. 按 Bus 号分组显示设备

### 思考题

1. **你的系统有几个 PCIe Root Port？它们分别连接什么设备？**
   - 对应 basics.md §1.2: Root Complex 至少包含一个 Root Port
   - 读 `pci_create_root_bus()` 了解 Bus 0 的创建

2. **是否有 PCIe Switch？如何从 lspci 输出判断？**
   - 对应 basics.md §1.3: Switch 包含 Upstream + Downstream Port
   - lspci 中 Switch 端口显示为 "PCI bridge"

3. **Bus Number 的分配是否连续？有哪些 Bus 范围是空闲的？**
   - 对应 annotations.md §2: Bridge 的 Secondary/Subordinate Bus
   - Subordinate = 最远的下游 Bus Number

### 预期输出示例

```
[1] PCIe 拓扑树（lspci -tv）
-[0000:00]-+-00.0  Host bridge
           +-01.0-[01]----00.0  VGA compatible controller
           +-14.0  USB controller
           \-1f.0  ISA bridge

[2] 设备类型统计
  Endpoint 数量: 15
  Bridge 数量: 3

[3] Root Complex 与 Root Port 识别
  Bus 0 设备（Root Complex 内部）:
    00:00.0 — Host bridge: Intel ...
    00:01.0 — PCI bridge: Intel ...

  PCIe Root Port:
    00:01.0 — PCI bridge: Intel ...
      Port type: Root Port
```

---

## Exercise 2: 配置空间读写

### 目标

使用 `setpci` 直接读写 PCIe 配置空间，理解 Type 0/1 的差异和 BAR 解析。

**核心问题**：BAR 的类型如何判断？64-bit BAR 占用几个槽位？Bridge 的 Bus Number 字段含义？

### 连接 annotations.md

- §4「PCIe 配置访问实现」：`pci_read_config_*()` 的 IO 端口 / MMIO 两种方式
- §5「关键数据结构」：`struct pci_dev` 的配置空间字段
- basics.md §3 — 配置空间类型（Type 0/1）
- basics.md §4 — BAR 枚举过程

### 步骤

```bash
# 分析指定设备
sudo ./ex2-config-space.sh 01:00.0

# 自动分析一个 Endpoint + 一个 Bridge
sudo ./ex2-config-space.sh
```

脚本会：
1. 读取 Vendor/Device ID
2. 解码 Command/Status 寄存器的每个位
3. 解析 Header Type（Type 0 vs Type 1）
4. 解析所有 BAR（Endpoint）或 Bus Number（Bridge）
5. 查找并解码 PCIe Capability

### 思考题

1. **观察 Endpoint 的 BAR，哪些是 32-bit、哪些是 64-bit？**
   - 64-bit BAR 占用两个 BAR 槽位
   - 对应 basics.md §4.2: `PCI_BASE_ADDRESS_MEM_TYPE_64 = 0x04`

2. **Bridge 的 Secondary Bus 和 Subordinate Bus 是否相同？**
   - 如果不同，说明下游还有 Bridge
   - 对应 annotations.md §2: Subordinate = max(所有下游 Bus)

3. **PCIe Capability 的 Device Type 字段与 `lspci -vv` 的输出一致吗？**
   - 对应 basics.md §5.3: `PCI_EXP_FLAGS_TYPE` 的编码

4. **Command 寄存器的 Bus Master 位（bit 2）是否启用？**
   - 未启用则设备不能发起 DMA
   - 对应 basics.md §4.1: BAR 映射与 DMA 的关系

### 预期输出示例

```
━━━ 目标设备: 01:00.0 ━━━

  设备: VGA compatible controller: NVIDIA ...

  [Vendor ID / Device ID]
    Vendor ID  (0x00): 0x10de
    Device ID  (0x02): 0x1401

  [Command / Status 寄存器]
    Command (0x04): 0x0007
      bit 0: I/O Space — 已启用
      bit 1: Memory Space — 已启用
      bit 2: Bus Master — 已启用

  [BAR 分析（Type 0 Endpoint）]
    BAR0 (0x10): 0xf100000c
      类型: 32-bit Memory, non-prefetchable
      基地址: 0xf1000000
    BAR1 (0x14): 0xd000000c
      类型: 64-bit Memory, prefetchable
      基地址: 0xd0000000
```

---

## Exercise 3: ACS 能力分析

### 目标

分析设备的 ACS（Access Control Services）能力和启用状态，理解 ACS 对 IOMMU group 的影响。

**核心问题**：哪些设备支持 ACS？ACS Control 中的 SV/RR/CR/UF 位是否全部启用？

### 连接 annotations.md

- §3「ACS 检查」：`pci_acs_flags_enabled()` 和 `pci_acs_enabled()` 的实现
- §3「IOMMU group」：`pci_device_group()` 的算法
- basics.md §5 — PCIe 能力链表
- **CLAUDE.md 陷阱 #11** — ACS 判定的 quirk 层

### ⚠️ 重要：ACS 判定的复杂性

ACS 判定比看起来复杂得多。内核的 `pci_acs_enabled()` 流程：

```
1. pci_dev_specific_acs_enabled()    ← quirk 层（最高优先级）
   ├─ 命中 quirk → 直接返回 quirk 结果
   └─ 没命中 → 继续

2. PCIe 类型判断
   ├─ RCiEP (RC_END) → 总是隔离
   ├─ Endpoint → 不提供隔离
   ├─ Downstream/Root Port → 检查 ACS 寄存器
   └─ PCI_BRIDGE/PCIE_BRIDGE → 检查 ACS 寄存器
```

**Intel RCiEP quirk**：`pci_quirk_rciep_acs`（`drivers/pci/quirks.c`）把**所有 Intel RCiEP** 判成隔离成立，即使它们没有 ACS capability。

**REQ_ACS_FLAGS = SV | RR | CR | UF**（不含 TB、EC、DT）。

### 步骤

```bash
sudo ./ex3-acs-analysis.sh
```

脚本会：
1. 扫描所有设备，查找 ACS Extended Capability (ID = 0x000D)
2. 解码 ACS Control 寄存器的每个位
3. 标注哪些位属于 REQ_ACS_FLAGS
4. 分析 ACS 对 IOMMU group 的影响
5. 检测 Intel RCiEP quirk

### 思考题

1. **系统中有哪些设备支持 ACS？它们是什么类型的设备？**
   - ACS 通常在 Downstream Port / Root Port 上
   - 对应 annotations.md §3

2. **对于支持 ACS 的设备，SV/RR/CR/UF 是否全部启用？**
   - 只有这四个位全部启用，才算通过隔离检查
   - 对应 CLAUDE.md 陷阱 #11

3. **有没有 Intel RCiEP 设备？它们是否命中了 quirk？**
   - `pci_quirk_rciep_acs` 把所有 Intel RCiEP 判成隔离成立
   - 对应 CLAUDE.md 陷阱 #11

4. **（进阶）如何通过内核参数控制 ACS？**
   - `pci=disable_acs_redir=` — 移除隔离
   - `pci=config_acs=` — 自定义 ACS 位
   - **注意**：`config_acs` 的位串从右往左解析！
   - 对应 CLAUDE.md 陷阱 #13

---

## Exercise 4: IOMMU Group 分析

### 目标

分析 IOMMU group 的划分，理解为什么某些设备必须一起直通。

**核心问题**：哪些设备共享 IOMMU group？它们之间的 Bridge 是否缺少 ACS 隔离？

### 连接 annotations.md

- §3「IOMMU group」：`pci_device_group()` 的算法
- §2「Bridge 与 Bus Number 分配」：拓扑层次
- CLAUDE.md 陷阱 #11 — ACS 判定的 quirk 层

### 步骤

```bash
sudo ./ex4-iommu-group.sh
```

脚本会：
1. 列出所有 IOMMU group 及其设备
2. 分析每个 group 内设备的父 Bridge
3. 检查父 Bridge 是否有 ACS
4. 诊断多设备共享 group 的原因
5. 分析设备直通可行性
6. 在拓扑树中标注 IOMMU group

### 思考题

1. **系统中有哪些多设备共享的 IOMMU group？**
   - 这些设备在拓扑中的位置关系是什么？
   - 对应 annotations.md §3: 向上遍历直到找到 ACS Bridge

2. **是否有 GPU 和 HDMI Audio 在同一个 group？**
   - 常见现象：GPU 的 HDMI Audio 是同一芯片的不同 Function
   - 它们必须一起直通给同一个 VM

3. **如何通过 ACS 配置来分割 group？**
   - 对父 Bridge 启用 ACS 的 SV/RR/CR/UF 位
   - 对应 CLAUDE.md 陷阱 #11

4. **为什么有些 Root Port 没有 ACS 但设备仍在独立 group？**
   - Root Port 本身提供隔离（不需要 ACS）
   - 对应 annotations.md §3

5. **（进阶）IOMMU group 划分与 ACS quirk 的关系？**
   - `pci_acs_enabled()` 开头先调 `pci_dev_specific_acs_enabled()`
   - 命中 quirk 就直接定论，PCIe 类型判断根本不执行
   - 对应 CLAUDE.md 陷阱 #11

---

## 提交检查清单

完成练习后，确认：

- [ ] 读过 `../annotations.md` 对应章节
- [ ] 查过 Linux 内核源码（文件路径 + 行号）
- [ ] 查过 PCIe Base Spec（章节号）
- [ ] 思考题已回答
- [ ] 输出数据已记录
- [ ] 未重犯 `CLAUDE.md` 已知陷阱

---

## 参考

- `../annotations.md` — 源码精读
- `../basics.md` — PCIe 硬件基础
- `../vfio-integration.md` — PCIe 与 VFIO 的结合
- Intel PCIe Base Spec — `pcie-base-spec-r6.0.pdf`
- Linux 6.12.93 源码 — `drivers/pci/`, `drivers/iommu/`
