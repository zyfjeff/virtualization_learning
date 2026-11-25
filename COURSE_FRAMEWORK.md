# 统一课程框架设计（COURSE_FRAMEWORK）

> 本文是**课程内容结构**的唯一来源：所有 phase 复用同一套学习框架。
> AI 协作规范、事实核查流程、全局已知陷阱仍以 `AGENTS.md` 为唯一来源，本文不重复。
>
> 状态：设计稿（2026-11-25）。迁移须按 §8 计划逐 phase 执行，**内容只搬不改**，
> 技术结论变动必须走 AGENTS.md 的事实核查流程。

---

## 1. 背景：现状调研结论

12 个 phase 各自长出了不同的结构，读者每进入一章都要重新摸索"这章怎么学"。
2026-11-25 对全部 phase 的 README 章节骨架、文件清单、practice 组织做了逐一调研：

### 1.1 结构差异

| 维度 | 现状 |
|---|---|
| README 体量 | phase9/10 约 90 行（纯导航）↔ phase8 2914 行、phase7 2213 行（巨石，深度内容混在导航里） |
| 章节骨架 | "前置知识"7 个 phase 有、5 个没有；"VMM视角对比"只有 phase0/5/6 有；"验证清单"只有 phase4/5/6/10 有；学习路径有的叫"推荐阅读顺序"、有的叫"源码阅读路线"、有的没有 |
| 深度内容组织 | phase3 拆成 8 篇问题文档、phase5/6/8 拆成主题文档、phase7/8 大部分仍堆在 README |
| practice 组织 | C 程序+Makefile（phase1/2/7/8）、编号 shell 实验（phase4/5）、实验报告堆（phase3，17 个 md 只有 1 个脚本）、md+sh 成对（phase10）、场景目录（phase11）、手工步骤（phase0） |
| 文件命名 | 大写报告（phase3 `EXPERIMENTS_SUMMARY.md` 等）、大写主题文档（phase4 `ACS_UF_EXPLANATION.md`）、易混淆名（phase8 `vmclock-phase1-analysis.md` 的 "phase1" 指分析阶段而非 phase1-cpu-virt） |
| corrections.md | phase0/1/2/6 缺失 |
| archive/ | phase2 README 声称有 archive/ 但目录不存在；phase4/6 有 |

### 1.2 事实性错误（与框架无关，必须修）

| 位置 | 错误 | 正确 |
|---|---|---|
| `phase6-virtio/README.md:1` | 标题 "Phase 5：设备虚拟化" | Phase 6 |
| `phase10-performance/README.md:1` | 标题 "Phase 9：性能测量" | Phase 10 |
| `phase11-debugging/README.md:1` | 标题 "Phase 10：KVM 运行时调试" | Phase 11 |
| `phase11-debugging/README.md:93` | "APICv / Posted Interrupts (phase4)" | phase5 |
| `phase11-debugging/README.md:94` | "大页配置 (phase9 E2)" | phase10 E2 |
| `phase11-debugging/README.md:96` | "vhost 优化 (phase5)" | phase6 |
| `phase4-pcie/README.md` 本章文件表 | 列出 `topology.md` / `capabilities.md` / `vfio-integration.md` 三个**不存在**的文件；实际存在的 `ACS_UF_EXPLANATION.md` / `ATS_EXPLANATION.md` 未列出 | 按实际文件重写 |
| `phase2-mem-virt/README.md` 目录树 | 声称有 `archive/` | 建目录或删引用 |

成因：phase3（IOMMU）/phase4（PCIe）插入后，旧编号未全局更新。
按 AGENTS.md 要求，修正时同步记入对应 phase 的 `corrections.md`。

### 1.3 单一来源违规嫌疑（迁移时逐条审计）

phase0/5/6 README 的"⚡ 性能优化技术"章节含无出处数字
（如 phase5 的"减少中断相关 VM-Exit 80%""中断延迟降低 50%""吞吐提升 20-30%"）。
按 AGENTS.md 性能单一来源规则：实测数字归所属章节/phase10 index 并分级，
其它章节只给指针；机制推算不得冒充实测。**审计前不预设结论**：
有规范/实测出处的保留并补引用，无出处的标"待实测"或移入 phase10 C/D 级。

---

## 2. 设计原则

1. **读者不变**：面向有用户态 VMM 经验（QEMU/crosvm）的读者。每章必须回答
   "这件事用户态怎么做 → 内核接管了什么 → 边界在哪"，所以 **VMM视角对比是知识类 phase 的必备章节**。
2. **一套学习循环**：所有 phase 用相同的认知顺序组织（§3），读者形成肌肉记忆：
   目标 → 全景 → 对比 → 精读 → 动手 → 纠偏 → 自测。
3. **规范为准、源码为证**：全景章先讲硬件/规范提供了什么，再讲内核如何实现。
   顺序不可颠倒（AGENTS.md 核心原则：硬件优化可能在软件里看不见）。
4. **导航与内容分离**：README 是导航 + 全景 + 路线；逐行源码在 `annotations.md`；
   深度主题在 `<topic>.md`；实验在 `practice/`；勘误在 `corrections.md`。
5. **单一来源**：性能数字 / 模块参数默认值 / 测量方法论 → phase10；
   全局陷阱 → AGENTS.md；章节勘误 → corrections.md。其它位置只给指针。
6. **内容只搬不改**：结构迁移不重写技术结论；发现错误走勘误流程，不借迁移夹带。

---

## 3. 统一学习循环（8 步）

每个知识类 phase 的学习路径按以下顺序展开，README 的"学习路线"章逐步映射到材料：

| 步 | 名称 | 回答的问题 | 材料 |
|---|---|---|---|
| 1 | 定位与目标 | 学完我能回答/做到什么？（可验证） | README 学习目标 |
| 2 | 硬件全景 | 硬件/规范提供了什么机制？ | README 技术全景 + 主题文档 |
| 3 | VMM 对比 | 用户态已 familiar 的部分 vs 内核接管的部分，边界在哪？ | README VMM视角对比 |
| 4 | 源码精读 | 内核怎么实现？关键调用链与"为什么" | annotations.md + 源码阅读路线 |
| 5 | 动手验证 | 怎么观察/测量/复现？ | practice/ 编号实验 |
| 6 | 性能与测量 | 开销多大？怎么测才可信？ | 指针 → phase10（不抄数字） |
| 7 | 陷阱与勘误 | 哪些流行说法是错的？ | corrections.md + AGENTS.md 全局陷阱 |
| 8 | 自测与衔接 | 学会了吗？下一步学什么？ | 验证清单 + 下一步 |

> 步 2→3→4 的顺序是教学设计：先知道硬件能做什么，再对照用户态经验定位内核职责，
> 最后带着问题读源码。跳过步 2 直接读代码正是 AGENTS.md 警告的误判来源
> （如 Posted Interrupts 零 VM-Exit 在 KVM 代码里看不见）。

---

## 4. README 统一骨架

模板见 `templates/phase-template/README.md`。章节**名称与顺序固定**，
emoji 标记沿用项目现有习惯。知识类 phase（0–8）全部 12 节必备；
capstone/横切类按 §6 变体裁剪。

```markdown
# Phase N｜<中文标题>            ← N 必须等于目录编号，杜绝旧编号

> 一句话定位
> 前置：phaseX …
> 版本基线：Linux 6.12.93 / QEMU 10.1.0-rc2（不同则注明）

## 📋 学习目标                   ← 可验证条目："能解释/能画出/能操作…"
## 📚 前置与衔接                 ← 依赖的前序 phase + VMM 经验中已熟悉的概念
## 📂 本章文件                   ← 表：文件 | 内容 | 何时读；只列真实存在的文件
## 📖 学习路线                   ← 编号步骤，映射 §3 八步循环，每步 读什么→想什么→做什么
## 🏗️ 技术全景                   ← 硬件/规范视角概览，≤1 屏；细节放主题文档
## 🔍 VMM视角对比                ← 用户态 vs 内核分工表 + 边界（知识类必备）
## 📖 源码精读入口               ← 关键调用链 + annotations.md 章节映射；不贴大段代码
## 🔬 实践练习                   ← 实验列表表（编号|主题|环境|入口）→ practice/README.md
## ⚡ 性能与测量                 ← 只给指针 → phase10；未实测标"待实测"
## ⚠️ 陷阱与勘误                 ← 本章 top 3–5 陷阱内联 + corrections.md 入口 + 相关全局陷阱编号
## ✅ 阶段检验清单               ← 自测问题 + 操作性验收（能跑通什么算学会）
## 🚀 下一步                     ← 下一 phase + 本章进阶主题
```

体量约束：

- README 目标 **≤ 500 行**（导航 + 全景 + 路线）。
- **> 1000 行必须拆分**：深度内容移入主题文档，README 留概览 + 指针。
  当前需拆：phase8（2914）、phase7（2213）、phase0（1169）、phase5（1104）。
- 500–1000 行建议拆分（当前 phase2 890 行）。
- 拆分是**搬运**：原文段落整体移入主题文档并保留引用，不重写结论。

---

## 5. 文件布局与命名规范

```
phaseN-slug/
├── README.md          # 导航 + 全景 + 学习路线（§4 骨架）
├── annotations.md     # 源码精读：file:line + 为什么（知识类必备）
├── corrections.md     # 勘误（所有 phase 必备，可先建占位）
├── <topic>.md         # 深度主题文档：小写 kebab-case，名词短语
├── archive/           # 已被取代的过程性文档（按需创建）
└── practice/
    ├── README.md      # 实验总览表 + 环境准备 + 运行 + 清理纪律
    ├── exN-<slug>.sh  # 编号实验（shell / C 统一 exN- 前缀）
    ├── exN-<slug>.c
    ├── Makefile       # 有 C 程序时必备，二进制 gitignore
    └── reports/       # 实验报告/记录（与可运行脚本分离）
```

命名规则：

1. **README 标题**：`# Phase N｜<中文标题>`，N = 目录编号。
2. **主题文档**：小写 kebab-case（`acs-upstream-forwarding.md`，不用 `ACS_UF_EXPLANATION.md`）。
   名字里的 "phase/step/stage" 一律写全（`vmclock-analysis-stage1.md`，不用 `vmclock-phase1-analysis.md`）。
3. **实验**：`exN-<slug>`，编号从 1 连续。例外：phase10 的 `bench-*`（E1–E5）
   是跨章引用密集的实验名，**保留**，在 practice/README 里注明 exN ↔ EN 映射。
4. **报告与脚本分离**：实验记录放 `practice/reports/`；被取代的环境状态/过程报告
   放 `phaseN/archive/`。practice/ 根目录只放可运行材料与总览。
5. **改名前先查引用**：`grep -rn "<旧名>" --include="*.md" .`，同步更新全部引用
   （含 AGENTS.md 里的锚点链接），改名与引用更新同一个 commit。
6. **二进制不入库**：可由 Makefile 重建的 ELF/.o/.ko 一律 gitignore（2026-11-25 已清理）。

---

## 6. 三类 phase 的框架变体

统一循环不变，载体不同：

### 6.1 知识类（phase0–8）——完整 8 步

§4 骨架全部 12 节。实践以"观察/验证机制"为主。

### 6.2 毕业建造类（phase9）——项目阶梯变体

README 保留：定位（=学习目标）/ 前置与衔接 / 本章文件 / 项目阶梯（=学习路线）/
环境要求 / 总体验收（=验证清单）/ 下一步。**每个 project 文档内部复用 8 步循环**：
目标 → 设计（硬件全景+VMM对比）→ 实现（源码精读）→ 验收（动手+自测）。
corrections.md 必备（已有，含事故复盘）。

### 6.3 横切类（phase10/11）——方法论/场景变体

- **phase10**：保留 measurement / parameters / index 的单一来源地位。
  README 对齐骨架中的：学习目标 / 前置与衔接 / 本章文件 / 学习路线（读序）/
  实践练习（E1–E5）/ 验证清单。"技术全景/VMM对比"替换为"本章只负责三件事"式的职责声明。
- **phase11**：保留场景驱动导航（快速导航/决策树/工具选择）。README 对齐：
  学习目标 / 前置与衔接 / 本章文件 / 学习路线 / 验证清单。
  场景文档（launch-failures 等）内部结构：症状 → 定位路径 → 根因 → 修复 → 案例。

---

## 7. 逐 phase 差距分析

✓=已有 ✗=缺失 △=有但需改造（改名/拆分/审计）。章节名按 §4 骨架归一化后对比：

| phase | 目标 | 前置 | 文件表 | 路线 | 全景 | VMM对比 | 精读入口 | 实践 | 性能指针 | 陷阱勘误 | 验证 | 下一步 | 其它 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | ✓ | ✗ | ✓ | △源码阅读路线 | ✓ | ✓ | ✓ | △手工步骤 | △数字待审计 | △无corrections | ✗ | ✗ | README 1169 行需拆 |
| 1 | ✓ | ✗ | ✓ | ✓ | ✓硬件理论 | ✗ | ✓ | ✓ | ✗ | ✗无corrections | ✗ | ✗ | |
| 2 | ✓ | ✓ | ✓ | △ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗无corrections | ✗ | ✗ | archive/ 引用悬空；890 行建议拆 |
| 3 | ✓ | ✓ | ✓ | ✓ | ✓8问结构 | ✗ | △ | △报告堆 | ✗ | ✓ | ✗ | ✗ | practice 17 报告需分离归档 |
| 4 | ✓ | ✓ | △悬空链接 | ✓ | ✓ | ✗ | △ | ✓ | ✗ | △ | ✓ | ✗ | 大写文档改名（无外部引用，安全） |
| 5 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | △数字待审计 | △ | ✓ | ✓ | README 1104 行需拆；结构最接近目标 |
| 6 | ✓ | ✗ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | △数字待审计 | ✓ | ✓ | ✗ | 标题旧编号；无corrections |
| 7 | ✓ | ✗ | ✓ | ✗ | △巨石 | ✗ | ✓ | ✓ | ✗ | ✓ | ✗ | ✗ | README 2213 行必须拆 |
| 8 | ✓ | ✓ | ✓ | △ | △巨石 | ✗ | ✓ | ✓ | ✗ | ✓ | ✗ | ✗ | README 2914 行必须拆；vmclock 文档改名 |
| 9 | ✓定位 | ✗ | △ | ✓阶梯 | — | — | — | ✓项目 | — | ✓ | ✓验收 | ✓ | capstone 变体 |
| 10 | ✓ | ✓ | ✓ | ✓ | —职责声明 | — | — | ✓E1–E5 | ✓本体 | ✓ | ✓ | △ | 标题旧编号 |
| 11 | ✓ | ✓ | ✓ | ✓导航 | —场景 | — | — | △eN→exN | ✓指针 | ✓ | ✗ | ✓关联 | 标题旧编号+3处旧交叉引用 |

---

## 8. 迁移计划

**总原则**：一次一个 phase、一个 commit；内容只搬不改；每步完成后
`grep` 验证无悬空链接；技术结论变动单独走勘误流程。

### P0 — 事实性修复（小 commit，先行）

1. phase6/10/11 README 标题编号 + phase11 三处旧交叉引用（§1.2 表）。
2. phase4 本章文件表按实际文件重写。
3. phase2 archive/ 悬空引用处理。
4. 以上记入对应 corrections.md（phase6 先建占位）。

### P1 — 试点（phase4）

选 phase4：体量小（228 行）、骨架最接近、改名无外部引用。
按 §4 补齐 VMM对比 / 陷阱勘误 / 性能指针 / 下一步，主题文档改名
（`ACS_UF_EXPLANATION.md` → `acs-upstream-forwarding.md`、`ATS_EXPLANATION.md` → `ats.md`），
practice 报告（PHASE4_SUMMARY.md）移入 reports/。**试点验收后冻结骨架，再铺开。**

### P2 — 知识类铺开（每 phase 一个 commit）

顺序（先易后难）：phase1 → phase3 → phase6 → phase5 → phase0 → phase2 → phase7 → phase8。

- 补齐缺失章节；缺 corrections.md 的建占位。
- phase3：practice 报告分离（实验记录 → reports/，4 份重复环境状态报告 → archive/）。
- phase0/5/6：性能章节数字审计（§1.3），改"性能与测量（指针）"。
- phase0/5/7/8：README 拆分（>1000 行强制），深度内容移主题文档。
  phase7/8 是重活，各自可再拆成多个 commit（先拆文档、再补章节）。
- phase8：`vmclock-phase1-analysis.md` → `vmclock-analysis-stage1.md`（仅 README 引用）。
- phase11：practice `eN-*` → `exN-*`（8 处内部引用同步）。

### P3 — 横切/毕业类对齐 + 收尾

- phase9/10/11 按 §6 变体对齐 README 骨架。
- 根 README 目录树、AGENTS.md 增加本文指针；本文状态从"设计稿"改"已实施"。
- 全局验收：每个 phase 对照 §7 表逐项打勾；`grep` 全库无悬空文件链接。

### 不做的事

- 不重写技术结论、不借迁移"顺手优化"内容。
- 不改 phase10 bench-* 实验名（跨章引用密集）。
- 不动 PDF 规范文件与 scripts/vm 环境。

---

## 9. 验收标准（单个 phase 对齐的判据）

- [ ] README 标题编号 = 目录编号，§4 骨架章节齐全（按类型裁剪）
- [ ] 本章文件表与磁盘文件一一对应，无悬空链接
- [ ] 学习路线映射 8 步循环，每步有材料入口
- [ ] VMM视角对比存在（知识类）
- [ ] 性能章只有指针，无未分级数字
- [ ] corrections.md 存在；本章已知陷阱有入口
- [ ] practice/ 脚本与报告分离，实验编号连续，README 有总览表
- [ ] 验证清单可操作（"能跑通/能回答"而非"了解"）
- [ ] README ≤ 500 行（>1000 行已拆分）
- [ ] 全库 grep 旧文件名/旧编号零残留
