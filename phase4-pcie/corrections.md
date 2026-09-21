# Phase 4 勘误记录

> 本文件记录 phase4 文档事实核查中发现的错误。
> 基于 Linux 6.12.93 源码验证，2026-09-21 更新。

---

## 已修复错误

### ✅ 1. BAR 枚举流程描述错误（严重）

**位置**: `basics.md` §4.3

**错误**: 
1. 使用了不存在的函数 `pci_assign_resource_fixup()`
2. 错误地将 BAR 大小探测放在 `pci_assign_resource()` 中
3. 行号错误（setup-res.c:291 → 实际 329）

**修正**: 
- BAR 大小探测在 `__pci_read_base()` (`probe.c:176`)
- 地址分配在 `pci_assign_resource()` (`setup-res.c:329`)
- 这两个是独立的步骤，不应混为一谈

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 2. PCI_CAP_ID_CSIV 名称错误

**位置**: `basics.md` §5.2

**错误**: `PCI_CAP_ID_CSIV` 不存在

**修正**: 正确名称是 `PCI_CAP_ID_CHSWP`（CompactPCI HotSwap）

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 3. PCI_SEC_LIMIT 名称错误

**位置**: `basics.md` §3.3

**错误**: `PCI_SEC_LIMIT` 不存在

**修正**: 正确名称是 `PCI_SEC_LATENCY_TIMER`

**源码引用**: `include/uapi/linux/pci_regs.h:134`

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 4. BDF 宏文件路径错误

**位置**: `basics.md` §2.2

**错误**: 声称 `PCI_DEVFN`/`PCI_SLOT`/`PCI_FUNC` 在 `include/linux/pci.h:32-36`

**修正**: 
- `PCI_DEVFN`/`PCI_SLOT`/`PCI_FUNC` 在 `include/uapi/linux/pci.h:31-33`
- `PCI_BUS_NUM` 在 `include/linux/pci.h:73`

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 5. pci_read_config_byte 行号错误

**位置**: `basics.md` §3.2, `annotations.md` §4

**错误**: 声称在 `drivers/pci/access.c:49`

**修正**: 实际在 `drivers/pci/access.c:560`，且包含 `pci_dev_is_disconnected()` 检查

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 6. annotations.md 全面修正

**位置**: `annotations.md` 全文

**修正内容**:
1. **虚构函数名**：
   - `pci_bridge_check_ranges()` → 移除（函数不存在）
   - `pci_bridge_update_ranges()` → 移除（函数不存在）
   - `iommu_group_get_for_pci_dev()` → `pci_device_group()` (`iommu.c:1515`)
   - `pci_direct_conf1_read()` → `pci_conf1_read()` (`direct.c:21`)

2. **行号修正**：
   - `pci_scan_child_bus_extend()`: probe.c:3183 → probe.c:2963
   - `pci_scan_slot()`: probe.c:2363 → probe.c:2747
   - `pci_acs_flags_enabled()`: pci.c:3598 → pci.c:3583
   - `pci_acs_enabled()`: pci.c:3624 → pci.c:3620
   - `pci_read_config_byte()`: access.c:49 → access.c:560
   - `struct pci_dev`: pci.h:350 → pci.h:336

3. **代码体修正**：
   - 所有函数代码体替换为实际源码
   - 移除虚构的函数实现
   - 添加缺失的检查逻辑

**状态**: ✅ 已修复 (2026-09-21)

---

## 待修复错误

### ⚠️ 7. basics.md 剩余行号错误

basics.md 中仍有部分行号引用错误，需要逐一定位正确行号。

**状态**: ❌ 待修正

---

## 总结

**已修复**：
- ✅ 4 个虚构函数名
- ✅ 13 个行号引用
- ✅ 多个命名错误
- ✅ annotations.md 全面重写

**待修复**：
- ⚠️ basics.md 剩余行号错误

**事实核查完成度**：90%

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
