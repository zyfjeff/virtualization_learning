# Phase 0 勘误记录

> **记录范围（AGENTS.md 瘦身策略）**：只记概念性错误 / 独家复盘 / 待办三类。
> 机械性修复只走 git。修正错误必须同步修正原文。

## 勘误 1：README 交叉引用沿用旧 phase 编号

- **发现日期**: 2026-11-25（统一课程框架 P0 事实修复，见 `../COURSE_FRAMEWORK.md` §1.2）
- **位置**: `README.md:731`
- **错误**: "Posted Interrupts（phase4 详解）"
- **正确**: phase5。phase4-pcie 插入后中断虚拟化由 phase4 顺延为 phase5
  （现行目录 `phase5-interrupts/`），此处未同步。
- **已同步修正**: `README.md`

## 勘误 2：大页内存收益数字无出处（2026-11-25 登记，待审计）

旧 README「⚡ 性能优化技术」大页节"性能提升 10-20%"无本仓实测出处。
（halt-polling 节已正确指向 phase10 index §1.2，符合单一来源，保留。）

**已做**：README 性能节大页数字标待审计；halt-polling 维持指针。
**待做**：大页收益若要引用，按 measurement.md 实测并汇入 phase10 分级（E2 bench-huge-dirty）。

**状态**: ⚠️ 待审计
