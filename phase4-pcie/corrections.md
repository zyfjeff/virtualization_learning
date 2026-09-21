# Phase 4 勘误记录

> 本文件记录 phase4 文档审查和修正过程中发现的错误。

---

## 勘误 1：初始版本缺失

**状态**: 待完善

**说明**: Phase 4 为新增内容，目前仅包含基础框架文档（README.md、basics.md、annotations.md）。

**待完善内容**:
- `topology.md`: PCIe 拓扑枚举的详细文档
- `capabilities.md`: PCIe 能力（ACS/ATS/SR-IOV/AER）的详细文档
- `vfio-integration.md`: PCIe 与 VFIO 集成的详细文档
- `practice/`: 动手实验脚本和说明

**修正计划**: 后续版本将补充完整内容。

---

## 勘误 2：源码引用需要验证

**状态**: 待验证

**说明**: `annotations.md` 中的源码引用基于 Linux 6.12.93，但以下函数的行号可能随版本变化：
- `pci_scan_child_bus_extend()`: `drivers/pci/probe.c:3183`
- `pci_scan_slot()`: `drivers/pci/probe.c:2363`
- `pci_acs_flags_enabled()`: `drivers/pci/pci.c:3598`
- `pci_acs_enabled()`: `drivers/pci/pci.c:3624`

**修正建议**: 使用函数名 grep 定位，而非依赖具体行号。

---

## 勘误 3：PCIe 规范版本

**状态**: 需确认

**说明**: 本文档参考 PCIe Base Specification r6.0，但某些特性（如 SR-IOV 1.1、ATS 2.0）可能需要更新版本的规范。

**修正建议**: 在后续版本中明确标注每个特性对应的规范版本。

---

## 勘误 4：ACS quirk 表不完整

**状态**: 待补充

**说明**: `annotations.md` 中提到的 ACS quirk 表（`pci_dev_specific_acs_enabled()`）仅列举了部分设备，实际内核中有更多设备特定的 quirk。

**修正建议**: 补充完整的 ACS quirk 表，或指向内核源码。

---

## 勘误 5：lspci 输出示例过时

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
