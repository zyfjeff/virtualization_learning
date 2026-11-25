# Phase 2 勘误记录

> **记录范围（AGENTS.md 瘦身策略）**：只记概念性错误 / 独家复盘 / 待办三类。
> 机械性修复只走 git。修正错误必须同步修正原文。

## 勘误 1：根 README 目录树悬空的 archive/ 引用

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置**: 根 `README.md` 项目结构树 phase2 块
- **错误**: 列出 `└── archive/ ← 已归档的过程性文档`，但 `phase2-mem-virt/archive/`
  目录不存在（`ls` 验证）。
- **正确**: 删除该行。2026-11-25 起不再设 archive/ 目录（git 历史即归档，
  COURSE_FRAMEWORK.md §5）。
- **已同步修正**: 根 `README.md`

## 勘误 2：性能数字无出处（2026-11-25 登记，待审计）

旧 README「⚡ 性能优化技术」与 VMM 对比含无本仓实测出处数字：
大页"10-20%"、EPT A/D"热迁移 20-30%"、脏页"首次迭代下降 50%"、缺页"省 ~2μs"。
按 AGENTS.md 性能单一来源规则属机制推算（至多 C 级）。
（phase10 corrections F 曾登记这些推算值在 archive/ 多处复制；archive/ 已删，git 历史保留。）

**已做**：README 性能节改纯指针（→ phase10 E2 bench-huge-dirty），数字标待审计；
机制（大页减层级/A/D 免软件干预/MMIO 两级缓存/脏页批量）保留。
**待做**：按 measurement.md 实测并汇入 phase10 分级。

**状态**: ⚠️ 待审计
