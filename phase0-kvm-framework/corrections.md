# Phase 0 勘误记录

> 记录本章文档中发现并修正的错误。格式遵循 AGENTS.md：说明错误、给出正确信息与引用。
> 全局性陷阱记入 AGENTS.md「已知陷阱」，此处只记本章特有的。

## 勘误 1：README 交叉引用沿用旧 phase 编号

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置**: `README.md:731`
- **错误**: "Posted Interrupts（phase4 详解）"
- **正确**: phase5。phase4-pcie 插入后中断虚拟化由 phase4 顺延为 phase5
  （现行目录 `phase5-interrupts/`），此处未同步。
- **已同步修正**: `README.md`
