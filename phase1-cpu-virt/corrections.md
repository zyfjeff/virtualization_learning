# Phase 1 勘误记录

> **记录范围（AGENTS.md 瘦身策略）**：只记概念性错误 / 独家复盘 / 待办三类。
> 机械性修复只走 git。修正错误必须同步修正原文。自引用写"本章 corrections.md"。

## 勘误 1：性能优化章节的无出处数字（2026-11-25 登记，待审计）

旧 README「⚡ 性能优化技术」含无出处推算：
- "VPID 减少 VM-Entry 开销 10-15%"
- "VMCS Shadow 嵌套虚拟化性能提升 20-30%"
- "用户态/内核态切换 ~1μs/次"
- "快速路径减少 50% 模式切换"

这些无规范引用、无本仓实测记录，按 AGENTS.md 性能单一来源规则属机制推算（至多 C 级）。
旧节里的 EPTP Switching / VMCS Shadow 示例代码（`alloc_vmcs_shadow()`、
`vmx->nested.vmcs02.vmcs_shadow` 等）未核实，疑似示意性伪代码。

**已做**：README 性能节改为「⚡ 性能与测量」纯指针（→ phase10），数字标"待实测"，
移除未核实的示意代码；VMM 对比节保留机制描述、删除量化数字。
**待做**：按 COURSE_FRAMEWORK §1.3 审计——VPID/VMCS Shadow 收益若要引用，
须在 phase10 做 A/B 实测并分级；VMCS Shadow 真实实现见 `vmx/nested.c`（待精读补 annotations）。

**状态**: ⚠️ 待审计

## 勘误 2：practice README 误标 ex1 执行位置（2026-11-25 已修）

旧 practice README 称 "ex1/ex2/ex3/ex4/ex5 在 Guest 内执行"。
ex1-vmx-verify 读 `/dev/cpu/0/msr` 的**物理** VMX capability MSR，应在**宿主**执行
（Guest 内读到的是虚拟化后的值，不是硬件 capability）。

**已同步修正**：practice/README.md 执行环境说明与总览表（ex1 → 宿主）。
