# Phase 2｜内存虚拟化（EPT / TDP MMU）

> Guest 以为自己拥有物理内存，宿主要隔离/超卖——地址翻译怎么让 guest 无感又宿主可控？
> 前置：phase1（VT-x/VMCS）、phase0（memslot）
> 版本基线：Linux 6.12.93 | Intel SDM Vol.3 Ch 28（EPT）
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)。

---

## 📋 学习目标

**核心问题**：guest 页表把 GVA 翻译成 GPA，但 GPA 不是真物理地址。宿主怎么加第二层
GPA→HPA 翻译，既让 guest 页表自治（改动不 trap）、又能隔离/超卖/跟踪脏页？

1. 画出 GPA→HPA 完整翻译路径
2. 读懂 `spte.h` 每个 SPTE 位定义
3. 用 ftrace 跟踪一次 EPT Violation 处理
4. 理解 TDP MMU 根页面管理与并发机制

---

## 📚 前置与衔接

- **前序 phase**：phase1 的 VMCS（EPTP 是 VMCS 字段）、phase0 的 memslot（GPA→HVA）。
- **VMM 经验衔接**：你在 QEMU 里 mmap guest 内存、设 memslot。本章讲内核怎么把
  memslot 变成 EPT 页表、缺页时怎么补。

---

## 📂 本章文件

问题接力（§3.2 规则 6）：

```
README 第一性原理（核心问题：第二层翻译怎么让 guest 自治又宿主可控？）
  │ 引出：EPT 页表怎么建？SPTE 长什么样？
  ▼
ept-establishment-path.md + annotations.md（SPTE/make_spte/建表路径）
  │ 引出：缺页了怎么处理？
  ▼
ept-violation-handling.md
  │ 引出：多 vCPU 并发改页表怎么不冲突？MMIO 怎么识别？
  ▼
tdp-mmu-concurrency.md + mmio-identification.md ── 取证 → practice
```

| 文件 | 回答什么 | 引出什么 → 去哪 |
|------|---------|----------------|
| `README.md` | 核心问题 + 第一性原理 + EPT 全景 | 建表/SPTE → [annotations.md](annotations.md) |
| [annotations.md](annotations.md) | 问题链精读：SPTE 位 / 缺页入口 / make_spte / 原子更新 | 建表路径 → [ept-establishment-path.md](ept-establishment-path.md) |
| [ept-establishment-path.md](ept-establishment-path.md) | ★ mmap 到 EPT entry 的调用链 | 缺页处理 → [ept-violation-handling.md](ept-violation-handling.md) |
| [ept-violation-handling.md](ept-violation-handling.md) | ★ EPT Violation 完整流程 | 并发 → [tdp-mmu-concurrency.md](tdp-mmu-concurrency.md) |
| [tdp-mmu-concurrency.md](tdp-mmu-concurrency.md) | ★ TDP MMU 并发与根页面管理 | MMIO → [mmio-identification.md](mmio-identification.md) |
| [mmio-identification.md](mmio-identification.md) | ★ MMIO 识别与 IPAT/内存类型 | 取证 → [practice/](practice/README.md) |
| [corrections.md](corrections.md) | 勘误 | 蒸馏 → AGENTS.md |

---

## 📖 学习路线

```
第1步: 核心问题 + 第一性原理 + EPT 全景（本文件）
第2步: VMM 对比（本文件）— 为什么 EPT 必须内核管
第3步: 问题链精读 annotations（SPTE/make_spte）+ ept-establishment-path
第4步: ept-violation-handling — 缺页处理
第5步: tdp-mmu-concurrency + mmio-identification — 并发与 MMIO
第6步: practice 取证；性能指针 → phase10
第7步: 陷阱勘误；深度自测 → phase3（IOMMU 是设备侧的 EPT）
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题

guest OS 用自己的页表把 GVA→GPA，它以为 GPA 就是物理地址。但宿主要隔离多个 guest、
超卖内存、跟踪脏页——必须掌握 GPA→HPA 的最终翻译。怎么加这第二层而不改 guest？

### 不可变约束

| 约束 | 内容 |
|------|------|
| 向后兼容 | guest OS 页表不能改（它以为自己在裸机） |
| 性能 | guest 页表改动频繁，不能每次都 trap |
| 隔离/超卖 | 宿主必须控制最终物理映射 |

### 设计空间与取舍

- **影子页表（软件）**：KVM 维护合并 GVA→HPA 的影子表，guest 页表改动全 trap 同步。
  正确但维护贵（每次 guest 改 PT 都 VM-Exit）。
- **EPT（硬件）**：硬件做第二层 GPA→HPA，guest 页表自治（改动不 trap），
  只有 EPT 缺页/权限违规才 VM-Exit。用硬件换"guest 自治 + 按需 trap"。
- **取舍**：EPT 省了 guest PT 同步开销，代价是 EPT violation 处理 + TLB 失效
  （INVEPT/VPID）+ 两级 walk 的访存放大（大页缓解）。倍数关系是确定的，绝对次数
  取决于计数口径，见下。

### 机制全景：二级翻译

```
GVA → [Guest PT, guest CR3] → GPA → [EPT, VMCS.EPTP] → HPA
```

4 级 guest 页表 × 4 级 EPT、无大页、无缓存命中时的访存放大：guest 侧每读一条
分页结构项（PML4E/PDPE/PDE/PTE，共 4 次），本身是一次 GPA 访问，需要 EPT 走 4 级
（4 次）再取目标（1 次）＝ 5 次，4 级共 20 次；最后访问目标页本身再 4 + 1 = 5 次。
合计 **25 次**（若不计最终那次数据访问本身，则为 24 次 —— 流传的"最多 24 次"是这个
口径；本仓库 `intel-vmx.pdf` 是 Vol 3C（ch24–33），其中并无该数字，属机制推算）。
**待实测**：实际倍数由 EPT/VPID 缓存命中率决定，
量化见 [phase10](../phase10-performance/index.md)，本章不复制数字。

EPT walk 失败 → VM-Exit（EPT Violation, reason 48）：

| 场景 | 触发 | KVM 处理 |
|------|------|---------|
| 缺页 | 叶条目 Present=0 | 分配物理页，建 SPTE |
| 权限违规 | 访问类型与权限不符 | 脏页/访问位更新 |
| Misconfiguration | 保留位组合（W+X 无 R） | **MMIO 机制**：故意设无效组合，handle_ept_misconfig 识别模拟 |

EPTP（VMCS 字段，SDM Vol 3C §25.6.11 Table 25-9）的位段，**内存类型在最低位，不是地址**：

| 位 | 含义 | 内核宏 |
|---|---|---|
| 2:0 | EPT paging-structure 内存类型：0 = UC、6 = WB，其余保留 | `VMX_EPTP_MT_MASK 0x7ull`（`vmx.h:531`，注释"encoded in bits 2:0, doesn't need to be shifted"）、`VMX_EPTP_MT_WB = X86_MEMTYPE_WB = 6`（`msr-index.h:50`） |
| 5:3 | **页表级数减 1**：4 级 = `3`、5 级 = `4`（0/1/2 保留） | `VMX_EPTP_PWL_MASK 0x38ull`、`PWL_4 0x18ull`、`PWL_5 0x20ull`（`vmx.h:526-528`） |
| 6 | 启用 EPT A/D 标志 | `VMX_EPTP_AD_ENABLE_BIT (1ull << 6)`（`vmx.h:529`） |
| 7 | supervisor shadow-stack 访问权限强制 | — |
| 11:8 | 保留 | — |
| N-1:12 | EPT 根表（PML4/PML5）的物理地址，N = 物理地址宽度 | `eptp |= root_hpa;`（`construct_eptp()`，`vmx.c:3411-3423`） |
| 63:N | 保留 | — |

SPTE 位定义、make_spte、原子更新的逐行精读见 [annotations.md](annotations.md)。

---

## 🔍 VMM视角对比

| 方面 | 用户态 VMM (QEMU) | KVM 内核态 |
|------|------------------|-----------|
| 内存分配 | malloc/mmap host 内存 | memslot 映射 guest 内存 |
| EPT 管理 | 无法直接操作 | 直接管理 EPT 页表 |
| 页错误处理 | 无法参与 | 内核态直接处理 EPT Violation |
| 并发 | 无 | 多 vCPU 并发缺页（cmpxchg） |
| 大页 | 显式配置 | 自动检测 2MB/1GB |
| 脏页跟踪 | 无法实现 | SPTE 软件位自动跟踪 |

**为什么 EPT 必须内核管**：缺页处理要快（不回用户态）、要并发（多 vCPU）、
要高级特性（大页/脏页/A/D）。量化开销见 phase10（本章不复制数字）。

---

## 📖 问题链精读入口

| 材料 | 入口问题 | 关键引申 | 源码入口 |
|------|---------|---------|---------|
| annotations（SPTE） | SPTE 哪些是硬件位、哪些是 KVM 软件位？ | 为什么软件位要塞进硬件保留位？ | `spte.h` |
| annotations（make_spte） | 一个 SPTE 怎么造出来？ | 大页/脏页/MMIO 怎么影响位？ | `make_spte()` |
| ept-violation-handling | 缺页从 VM-Exit 到映射完成经历什么？ | 为什么用 cmpxchg 原子更新？ | `kvm_tdp_page_fault` |
| tdp-mmu-concurrency | 多 vCPU 并发改页表怎么不冲突？ | 根页面怎么管理？ | TDP MMU |
| mmio-identification | KVM 怎么区分 RAM 和 MMIO？ | IPAT 位怎么控制内存类型？ | `handle_ept_misconfig` |

---

## 🔬 实践练习

取证实验（详见 [`practice/README.md`](practice/README.md)，五段式 §3.5）：
4 个 C 练习（ept_violation_demo / memtype_analysis / memtest / concurrent_test）
+ 2 个 EPT A/D 诊断工具（check-ept-ad / read-ept-ad-msr）。

| 编号 | 验证的理论 | 预测 |
|------|-----------|------|
| ept_violation_demo | 缺页建表（ept-violation-handling） | 首次访问触发 EPT violation，后续不触发 |
| concurrent_test | 并发原子更新（tdp-mmu-concurrency） | 多 vCPU 并发缺页无 SPTE 损坏 |
| memtype_analysis | MMIO/IPAT（mmio-identification） | MMIO 区走 misconfig 模拟 |
| check-ept-ad | EPT A/D 硬件支持（§5） | capability MSR 解出 A/D 位 |

---

## ⚡ 性能与测量

> **本章所有正文里的 ns/μs/倍数一律是机制推算，不是实测**。包括
> `ept-violation-handling.md`（"10-100 倍"、"10-20%"、"~200 ns"、"~1 ns"、"2-5 μs"）与
> `tdp-mmu-concurrency.md`（同组数字）。按 AGENTS.md 的性能单一来源规则，
> 需要数字请指向 [phase10](../phase10-performance/index.md)；本章只留机制与口径。
> 已登记在 [`corrections.md`](corrections.md) 勘误 2。

- 实测结论与分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
- 测量方法：[`../phase10-performance/measurement.md`](../phase10-performance/measurement.md)
- 本章现状：旧讲义"大页 10-20%""EPT A/D 热迁移 20-30%""脏页首次迭代下降 50%"
  "缺页省 ~2μs"**无本仓实测出处**，登记 corrections 待审计。
  大页/脏页实测见 phase10 E2（bench-huge-dirty）。机制（大页减层级/A/D 免软件干预/
  MMIO 两级缓存/脏页批量）保留，数字标待实测。

---

## ⚠️ 陷阱与勘误

1. **memslot 配置错误**：GPA 范围/对齐不对 → 缺页异常。
2. **大页对齐**：2MB 需 GPA % 0x200000 == 0，1GB 需 % 0x40000000 == 0。
3. **MMIO 误判**：misconfig 是 KVM 故意设的无效 SPTE 组合，不是真硬件错误。
4. **AGENTS.md #7**：VM 必须 `-enable-kvm -cpu host`。

本章勘误：[`corrections.md`](corrections.md)

---

## ✅ 阶段检验清单

操作性：

- [ ] 能画出 GVA→GPA→HPA 二级翻译
- [ ] 能解码 SPTE 硬件位/软件位
- [ ] 能用 ftrace 跟踪 EPT Violation
- [ ] 能说明 TDP MMU 并发机制

深度自测（答不出 = 没学会）：

- [ ] **反事实**：如果用影子页表代替 EPT，guest 每次改页表会发生什么？开销差在哪？
- [ ] **不可简化**：MMIO 为什么故意设"无效"SPTE 组合，而不是单独建一张 MMIO 表？
- [ ] **推导**：从"guest 页表自治 + 按需 trap"推出"EPT 缺页才 VM-Exit，改 PT 不 exit"。

---

## 🚀 下一步

- **Phase 3（IOMMU）**：设备侧的"EPT"——DMA 地址翻译，类比本章
- **本章进阶**：TDP MMU 并发、MMIO/IPAT 内存类型
