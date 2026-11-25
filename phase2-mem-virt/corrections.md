# Phase 2 勘误记录

> 记录本章文档中发现并修正的错误。格式遵循 AGENTS.md：说明错误、给出正确信息与引用。
> 全局性陷阱记入 AGENTS.md「已知陷阱」，此处只记本章特有的。

## 勘误 1：根 README 目录树悬空的 archive/ 引用

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置**: 根 `README.md` 项目结构树 phase2 块
- **错误**: 列出 `└── archive/ ← 已归档的过程性文档`，但 `phase2-mem-virt/archive/`
  目录不存在（`ls` 验证）。
- **正确**: 删除该行。2026-11-25 起不再设 archive/ 目录（git 历史即归档，
  COURSE_FRAMEWORK.md §5）。
- **已同步修正**: 根 `README.md`
