# Phase 4 勘误记录

> 本文件记录 phase4 文档审查和修正过程中发现的错误。

---

## 勘误 1：BAR 枚举流程描述错误（严重）

**位置**: `basics.md` §4.3

**错误**: 
1. 使用了不存在的函数 `pci_assign_resource_fixup()`
2. 错误地将 BAR 大小探测放在 `pci_assign_resource()` 中
3. 行号错误（setup-res.c:291 → 实际 329）

**修正**: 
- BAR 大小探测在 `__pci_read_base()` (`probe.c:176`)
- 地址分配在 `pci_assign_resource()` (`setup-res.c:329`)
- 这两个是独立的步骤，不应混为一谈

**状态**: ✅ 已修复

---

## 勘误 2：PCI_CAP_ID_CSIV 名称错误

**位置**: `basics.md` §5.2

**错误**: `PCI_CAP_ID_CSIV` 不存在

**修正**: 正确名称是 `PCI_CAP_ID_CHSWP`（CompactPCI HotSwap）

**状态**: ✅ 已修复

---

## 勘误 3：源码引用行号错误（多处）

**位置**: `basics.md` 和 `annotations.md`

| 文件 | 错误行号 | 正确行号 |
|------|---------|---------|
| `pci_regs.h` BAR defines | 88 | 102-108 |
| `setup-res.c` pci_assign_resource | 291 | 329 |
| `pci_regs.h` ACS defines | 676 | 991-997 |
| `pci.h` BDF macros | 32-36 | uapi/pci.h:31-33 + pci.h:73 |
| `access.c` pci_read_config_byte | 49 | 560 |
| `probe.c` pci_scan_slot | 2363 | 2747 |

**状态**: 部分已修复，待继续修正

---

## 勘误 4：初始版本缺失

**状态**: 待完善

**说明**: Phase 4 为新增内容，目前仅包含基础框架文档（README.md、basics.md、annotations.md）。

**待完善内容**:
- `topology.md`: PCIe 拓扑枚举的详细文档
- `capabilities.md`: PCIe 能力（ACS/ATS/SR-IOV/AER）的详细文档
- `vfio-integration.md`: PCIe 与 VFIO 集成的详细文档
- `practice/`: 动手实验脚本和说明

**修正计划**: 后续版本将补充完整内容。

---

## 勘误 5：PCIe 规范版本

**状态**: 需确认

**说明**: 本文档参考 PCIe Base Specification r6.0，但某些特性（如 SR-IOV 1.1、ATS 2.0）可能需要更新版本的规范。

**修正建议**: 在后续版本中明确标注每个特性对应的规范版本。

---

## 勘误 6：ACS quirk 表不完整

**状态**: 待补充

**说明**: `annotations.md` 中提到的 ACS quirk 表（`pci_dev_specific_acs_enabled()`）仅列举了部分设备，实际内核中有更多设备特定的 quirk。

**修正建议**: 补充完整的 ACS quirk 表，或指向内核源码。

---

## 勘误 7：lspci 输出示例过时

**状态**: 待更新

**说明**: `basics.md` 中的 `lspci` 输出示例基于特定硬件，可能与其他系统不一致。

**修正建议**: 标注示例硬件型号，或提供多个系统的输出示例。

---

## 总结

Phase 4 为新增内容，当前版本为基础框架。后续将：
1. 补充完整的文档（topology.md、capabilities.md、vfio-integration.md）
2. 添加动手实验脚本
3. 验证所有源码引用的准确性
4. 补充更多实际案例和故障诊断
