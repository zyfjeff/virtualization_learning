# Phase 6 勘误记录

> **记录范围（AGENTS.md 瘦身策略）**：只记概念性错误 / 独家复盘 / 待办三类。
> 机械性修复只走 git。修正错误必须同步修正原文。

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

## 勘误 2：性能数字无出处（2026-11-25 登记，待审计）

旧 README 多处无本仓实测出处的推算数字：
- VMM 对比表："~100万 pps / ~300万 pps（3 倍）"、"~10μs / ~3μs"
- 性能瓶颈分析："~5μs/包 / ~1.6μs/包"、"VM-Exit ~1μs"
- 性能优化节：亲和性"10-20%"、中断合并"50%/20%"、大队列"10%"、multi-queue"2-4 倍"

按 AGENTS.md 性能单一来源规则属机制推算（至多 C 级）。phase10 index 已收录 vhost 对比表
为 B 级并带三条限制（不同宿主/单队列/无重复）。

**已做**：README 性能节改纯指针（→ phase10），VMM 对比表删除吞吐/延迟数字行只留机制，
陷阱节删除"100万ppp"数字；机制描述（批处理/亲和性/通知抑制/multi-queue）保留。
**待做**：若要引用收益数字，按 measurement.md 多队列+直连对端+≥3 次重复实测并分级。

**状态**: ⚠️ 待审计

## 勘误 3：practice README 标题旧编号（2026-11-25 已修）

`practice/README.md:1` "Phase 5: vhost 性能实验" → "Phase 6"。已同步修正。
