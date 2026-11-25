# Phase 6 勘误记录

> 记录本章文档中发现并修正的错误。格式遵循 AGENTS.md：说明错误、给出正确信息与引用。
> 全局性陷阱记入 AGENTS.md「已知陷阱」，此处只记本章特有的。

## 勘误 1：标题与交叉引用沿用旧 phase 编号

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置与修正**:
  | 位置 | 错误 | 正确 |
  |---|---|---|
  | `README.md:1` | 标题 "Phase 5：设备虚拟化" | Phase 6 |
  | `README.md:155` | "Phase 5 的正文按主题拆分" | Phase 6 |
  | `annotations.md:1` | 标题 "Phase 5：源码精读注释" | Phase 6 |
  | `annotations.md:142` | "详见 phase6 corrections.md 勘误 4" | phase7。该勘误（IRTE Posted 化不由 KVM-VFIO 组列表触发）在 `../phase7-vfio/corrections.md` 勘误 4，已核对存在 |
- **成因**: phase4-pcie 插入后 virtio 由 phase5 顺延为 phase6，VFIO 由 phase6
  顺延为 phase7，本文未同步。
- **已同步修正**: `README.md`、`annotations.md`
