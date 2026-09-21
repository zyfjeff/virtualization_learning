# Phase 4 勘误记录

> 本文件记录 phase4-pcie 文档事实核查中发现的错误。
> 基于 Linux 6.12.93 源码验证，2026-09-21 更新。

---

## 勘误 A：annotations.md 中不存在的函数名（严重）

以下 4 个函数名在 Linux 6.12.93 源码中**不存在**，是虚构的：

### A1. `pci_bridge_check_ranges()` — 不存在

**位置**: `annotations.md` §2, 声称在 `drivers/pci/probe.c:1180`

**实际情况**: 该函数不存在。`probe.c:1180` 附近是 `pci_scan_bridge_extend()` 和 `pci_ea_fixed_busnrs()`。

**修正**: Bridge 资源范围检查的逻辑实际在 `pci_bridge_check_ranges()` 的等价实现在 `drivers/pci/setup-bus.c` 中的 `pci_bridge_check_ranges()`（注意这个文件确实有这个函数，但 `annotations.md` 引用的文件和行号完全错误）。

**状态**: ❌ 待修正

### A2. `pci_bridge_update_ranges()` — 不存在

**位置**: `annotations.md` §2, 声称在 `drivers/pci/probe.c:1243`

**实际情况**: 该函数不存在。`probe.c:1243` 附近是 `pci_ea_fixed_busnrs()`。

**修正**: Subordinate Bus Number 的更新逻辑在 `drivers/pci/probe.c` 中的 `pci_scan_bridge_extend()` 或 `pci_bus_update_busn_res()` 等函数中。需要重写该代码示例。

**状态**: ❌ 待修正

### A3. `iommu_group_get_for_pci_dev()` — 不存在

**位置**: `annotations.md` §3, 声称在 `drivers/iommu/iommu.c:1383`

**实际情况**: 该函数不存在。实际函数名为 **`pci_device_group()`**，位于 `drivers/iommu/iommu.c:1515`。

**修正**:
- 函数名改为 `pci_device_group()`
- 行号改为 `iommu.c:1515`
- 该函数使用 `pci_acs_path_enabled()`（不是 `pci_acs_enabled()`）来向上遍历拓扑
- 代码体需要完全重写

**状态**: ❌ 待修正

### A4. `pci_direct_conf1_read()` — 不存在

**位置**: `annotations.md` §4, 声称在 `arch/x86/pci/direct.c:65`

**实际情况**: 该函数不存在。`pci_direct_conf1` 是一个 `struct pci_raw_ops` 变量，不是函数。实际读函数名为 **`pci_conf1_read()`**，位于 `arch/x86/pci/direct.c:28`。

**修正**:
- 函数名改为 `pci_conf1_read()`
- 行号改为 `direct.c:28`
- 实际代码使用 `PCI_CONF1_ADDRESS` 宏构造地址，不是内联的 `outl()`
- 代码体需要重写

**状态**: ❌ 待修正

---

## 勘误 B：annotations.md 中所有函数行号均错误

**所有 13 个函数/结构体引用的行号全部错误**：

| 函数/结构体 | 文档行号 | 实际行号 | 偏差 |
|---|---|---|---|
| `pci_scan_child_bus_extend()` | `probe.c:3183` | `probe.c:2963` | -220 |
| `pci_scan_slot()` | `probe.c:2363` | `probe.c:2747` | +384 |
| `pci_bridge_check_ranges()` | `probe.c:1180` | **函数不存在** | — |
| `pci_bridge_update_ranges()` | `probe.c:1243` | **函数不存在** | — |
| `pci_acs_flags_enabled()` | `pci.c:3598` | `pci.c:3583` | -15 (可接受) |
| `pci_acs_enabled()` | `pci.c:3624` | `pci.c:3620` | -4 (可接受) |
| `iommu_group_get_for_pci_dev()` | `iommu.c:1383` | **函数不存在**，实际为 `pci_device_group()` @ `iommu.c:1515` | — |
| `pci_read_config_byte()` | `access.c:49` | `access.c:560` | +511 |
| `pci_bus_read_config_byte()` | `access.c:156` | 由宏 `PCI_OP_READ` 生成 @ ~`access.c:56` | — |
| `pci_direct_conf1_read()` | `direct.c:65` | **函数不存在**，实际为 `pci_conf1_read()` @ `direct.c:28` | — |
| `struct pci_dev` | `pci.h:350` | `pci.h:336` | -14 (可接受) |
| `pci_assign_resource()` | `setup-res.c:291` | `setup-res.c:329` | +38 (可接受) |
| `pci_create_root_bus()` | `probe.c:3058` | `probe.c:3113` | +55 |

**状态**: ❌ 待修正 — 需要逐一定位正确行号并更新文档

---

## 勘误 C：annotations.md 中函数代码体不准确

8/13 个函数的代码体与实际源码不符（虚构或过度简化）：

### C1. `pci_scan_child_bus_extend()` — 代码体错误

**文档代码**: 显示 `for (pass = 0; pass < 2; pass++)` 和 `list_for_each_entry()`

**实际代码**: 单次 `for (devfn = 0; devfn < 256; devfn += 8)` 循环 + `for_each_pci_bridge()` 宏

### C2. `pci_scan_slot()` — 代码体完全不符

**文档代码**: 显示 `pci_bus_read_dev_vendor_id()` + `hdr_type & 0x80` 检测

**实际代码**: 使用 `do { } while (fn >= 0)` 循环 + `next_fn()` 函数遍历

### C3. `pci_acs_flags_enabled()` — 代码体错误

**文档代码**: 调用 `pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_ACS)`

**实际代码**: 使用缓存的 `pdev->acs_cap` 字段（在 probe 时已查找并缓存）

### C4. `pci_acs_enabled()` — 代码体错误

**文档代码**: `if (ret != -ENOTTY) return ret;`

**实际代码**: `if (ret >= 0) return ret > 0;`（返回值语义不同）
另外文档缺少 `!pci_is_pcie()` 的检查。

### C5. `iommu_group_get_for_pci_dev()` — 整个函数虚构

**文档代码**: 显示一个使用 `pci_acs_enabled()` 的 while 循环

**实际情况**: 该函数不存在。实际函数 `pci_device_group()` 使用 `pci_acs_path_enabled()`，算法逻辑不同。

### C6. `pci_read_config_byte()` — 代码体不准确

**文档代码**: 简单委托到 `pci_bus_read_config_byte()`

**实际代码**: 包含 `pci_dev_is_disconnected()` 检查

### C7. `pci_conf1_read()`（文档称 `pci_direct_conf1_read()`）— 代码体虚构

**文档代码**: 内联 `outl()` 构造地址

**实际代码**: 使用 `PCI_CONF1_ADDRESS` 宏 + `raw_spin_lock_irqsave()` 保护

### C8. `pci_scan_slot()` — 代码体完全不符（同 C2）

**状态**: ❌ 待修正 — 所有代码体需要替换为实际源码

---

## 勘误 D：struct pci_dev 字段错误

**位置**: `annotations.md` §5

### D1. `busnr` 字段不存在

**文档声称**: `struct pci_dev` 有 `unsigned int busnr` 字段

**实际情况**: `struct pci_dev` 没有 `busnr` 字段。Bus Number 通过 `dev->bus->number` 访问。`busnr` 字段存在于 `struct pci_host_bridge`（`pci.h:584`）。

### D2. `iommu_group` 字段不存在

**文档声称**: `struct pci_dev` 有 `struct iommu_group *iommu_group` 字段

**实际情况**: `struct pci_dev` 在 6.12.93 中没有 `iommu_group` 字段。IOMMU group 通过 `dev->dev.iommu_group` 或 `dev_get_iommu_group()` 访问。

**状态**: ❌ 待修正

---

## 勘误 E：basics.md 中 BAR 枚举流程描述错误（严重）

**位置**: `basics.md` §4.3

### E1. 使用不存在的函数 `pci_assign_resource_fixup()`

**文档代码**: 调用 `pci_assign_resource_fixup(dev, resno)` 来分配地址

**实际情况**: 该函数在 Linux 6.12.93 中不存在。

### E2. BAR 大小探测位置错误

**文档代码**: 在 `pci_assign_resource()` 中写入全 1 并读回大小

**实际情况**:
- BAR 大小探测在 `__pci_read_base()`（`drivers/pci/probe.c:176`）
- 地址分配在 `pci_assign_resource()`（`drivers/pci/setup-res.c:329`）
- 这两个是独立的步骤，不应混为一谈

**修正**: 重写 §4.3，分开描述 BAR sizing 和 BAR assignment 两个阶段。

**状态**: ❌ 待修正（corrections.md 已有记录但未实际修复文档）

---

## 勘误 F：basics.md 中 PCI_CAP_ID_CSIV 名称错误

**位置**: `basics.md` §5.2

**错误**: 列出 `PCI_CAP_ID_CSIV = 0x06`（CompactPCI）

**实际情况**: `PCI_CAP_ID_CSIV` 不存在。正确名称是 `PCI_CAP_ID_CHSWP`（CompactPCI HotSwap），值为 `0x06`。

**源码引用**: `include/uapi/linux/pci_regs.h:216`

```c
#define PCI_CAP_ID_CHSWP        0x06    /* CompactPCI HotSwap */
```

**状态**: ❌ 待修正（corrections.md 已有记录但未实际修复文档）

---

## 勘误 G：basics.md 中 PCI_SEC_LIMIT 名称不存在

**位置**: `basics.md` §3.3

**错误**: 列出 `PCI_SEC_LIMIT = 0x1b`

**实际情况**: `PCI_SEC_LIMIT` 不存在。`0x1b` 对应的是 `PCI_SEC_LATENCY_TIMER`。

**源码引用**: `include/uapi/linux/pci_regs.h:134`

```c
#define PCI_SEC_LATENCY_TIMER   0x1b    /* Secondary bus latency timer */
```

**状态**: ❌ 待修正

---

## 勘误 H：basics.md 中 BDF 宏的文件路径错误

**位置**: `basics.md` §2.2

**错误**: 声称 `PCI_DEVFN`/`PCI_SLOT`/`PCI_FUNC` 在 `include/linux/pci.h:32-36`

**实际情况**: 这些宏在 `include/uapi/linux/pci.h:31-33`。`include/linux/pci.h:73` 也有 `PCI_BUS_NUM` 的定义。

**状态**: ❌ 待修正

---

## 勘误 I：basics.md 中所有行号引用均错误

basics.md 中引用的所有行号与 6.12.93 源码不符：

| 内容 | 文档行号 | 实际行号 |
|---|---|---|
| `pci_regs.h` BAR 类型定义 | 88 | 102-108 |
| `pci_regs.h` Capability ID 定义 | (未标行号) | 211-229 |
| `pci_regs.h` PCIe Capability 结构 | 586 | 474-487 |
| `pci_regs.h` PCI_EXP_TYPE 值 | (未标行号) | 477-485 |
| `pci_regs.h` ACS 标志定义 | 676 | 991-997 |
| `probe.c` pci_create_root_bus | 3058 | 3113 |
| `access.c` pci_read_config_byte | 49 | 560 |
| `setup-res.c` pci_assign_resource | 291 | 329 |
| `pci.h` struct pci_dev | 350 | 336 |

**状态**: ❌ 待修正

---

## 勘误 J：basics.md 中正确的内容

以下内容的值和编码经核实**全部正确**：

- ✅ 所有 BAR 类型十六进制值（`PCI_BASE_ADDRESS_SPACE=0x01` 等）
- ✅ 所有 `PCI_EXP_TYPE_*` 值（ENDPOINT=0x0 到 RC_EC=0xa）
- ✅ 所有 Capability ID 值（PM=0x01 到 AF=0x13，除 CSIV 名称外）
- ✅ 所有 ACS 标志值（SV=0x0001 到 DT=0x0040）
- ✅ BDF 宏的公式定义（文件路径错误，但公式本身正确）
- ✅ 配置空间寄存器偏移值（VENDOR_ID=0x00 等）
- ✅ Bridge Bus Number 偏移（PRIMARY_BUS=0x18 等）
- ✅ `PCI_EXP_FLAGS_*` 位域定义

---

## 勘误 K：README.md 前置知识引用错误

**位置**: `README.md` 前置知识部分

**问题**: README 中写 "本阶段假设你已经完成 Phase 1-10"，但 PCIe 阶段实际是 Phase 4，且前置知识写的是 Phase 3（IOMMU）和 Phase 4（中断虚拟化），这造成了编号混乱。

**实际情况**: 根据 git log，phase 编号经历过重组（commit `4c19f41`），PCIe 现在是 Phase 4。README 中的前置知识引用需要更新为正确的 phase 编号。

**状态**: ❌ 待修正

---

## 总结

### 严重程度分类

| 级别 | 数量 | 说明 |
|---|---|---|
| 🔴 严重 | 4 | 函数名虚构（A1-A4） |
| 🟠 重要 | 5 | 代码体错误（C1-C7）、字段不存在（D1-D2）、BAR 流程错误（E1-E2） |
| 🟡 中等 | 3 | 行号错误（B）、文件路径错误（H）、宏名错误（F、G） |
| 🟢 轻微 | 1 | README 前置知识编号（K） |

### 修正优先级

1. **立即修正**: 虚构函数名（A1-A4）— 这些会误导读者 grep 源码时找不到
2. **尽快修正**: 代码体错误（C1-C8）— 代码示例必须来自实际源码
3. **计划修正**: 行号和文件路径更新（B、H、I）
4. **补充修正**: BAR 枚举重写（E）、README 编号（K）

### 待修正文件清单

- [ ] `annotations.md` — 修正所有函数名、行号、代码体、struct 字段
- [ ] `basics.md` — 修正行号、函数名、BAR 枚举流程、宏名
- [ ] `README.md` — 修正前置知识 phase 编号引用
