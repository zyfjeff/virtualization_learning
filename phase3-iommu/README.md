# Phase 3｜IOMMU 层

> IOMMU 是设备侧的 MMU。本章补上"内核怎么建域、怎么翻译、怎么失效"这一整层机制。
> 前置：phase2（EPT 两级翻译——本章最大的类比杠杆）
> 版本基线：Linux 6.12.93 | VT-d Rev 4.1 / SMMUv3 / AMD-Vi（规范可用性见文末）
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)。本章是问题驱动结构的范例。

---

## 📋 学习目标

**核心问题**：设备 DMA 的地址翻译，和 CPU 的 MMU/EPT 干的是同一件事，
为什么 IOMMU 多出"选表、分组、同步失效"三个 CPU 侧没有的难题？

其它阶段里 IOMMU 一直以"结论"出现（phase5"中断经 IRTE"、phase7"ACS 决定 group"）。
本章补结论背后的机制。读完应当能：

1. 把 IOMMU 和 CPU 的 MMU/EPT 放进同一心智模型，说清相同与不同
2. 走通"一次设备 DMA 从 `dma_map()` 到硬件翻译出物理地址"的完整主流程
3. 对主流程每一处"为什么是这样而不是那样"给出源码级回答
4. 准确解释 `iommu.passthrough` 改了什么——它是正确性开关，不只是性能开关
5. 对照 VT-d / SMMUv3 / AMD-Vi 三后端，知道同一份 core 语义各落到什么硬件动作

---

## 📚 前置与衔接

- **前序 phase**：phase2 的 EPT 两级翻译（GPA→HPA）、SPTE、TDP MMU 根页面管理。
  本章"框架"节直接用 EPT 做类比，没读过会失去最大的杠杆。
- **VMM 经验衔接**：你在 QEMU 里用 VFIO 把设备 DMA 映射进 guest（`VFIO_IOMMU_MAP_DMA`）。
  本章讲内核侧 IOVA 分配、域选择、失效——Q7 专门对照 VFIO/IOMMUFD/dma-iommu 三套接口。
- **本章是 phase7 的地基**（不需要先读 phase7）。建议顺序：phase2 → phase3 → phase4 → phase7。

---

## 📂 本章文件

主干是"框架 → 概念 → 主流程 → 8 问"；每个问题对应一篇深入文档（问题接力，§3.2 规则 6）：

| 文件 | 对应问题 | 回答什么 |
|------|------|------|
| `README.md` | — | 框架 + 概念 + 主流程 + 8 问清单 |
| [`translation.md`](translation.md) | Q1 | 硬件怎么把 DMA 地址翻译成物理地址 |
| [`group.md`](group.md) | Q2 | 为什么直通必须整组、组怎么划 |
| [`domains.md`](domains.md) | Q3 | `iommu.passthrough` 到底改了什么 |
| [`iova.md`](iova.md) | Q4 | DMA 地址为什么不是物理地址 |
| [`invalidation.md`](invalidation.md) | Q5 | unmap 后硬件是否立刻不用旧翻译 |
| [`interrupts.md`](interrupts.md) | Q6 | 直通中断怎么穿过 IOMMU |
| [`userspace.md`](userspace.md) | Q7 | VFIO/IOMMUFD/dma-iommu 各管什么 |
| [`iommufd-objects.md`](iommufd-objects.md) | — | IOMMUFD 对象模型（IOAS/HWPT） |
| [`backends.md`](backends.md) | Q8 | Intel/ARM/AMD 怎么对上同一套抽象 |
| [`annotations.md`](annotations.md) | — | 问题链精读：probe→选域→attach→map→unmap→失效 |
| [`practice/`](practice/README.md) | — | 4 个取证实验 |
| [`corrections.md`](corrections.md) | — | 勘误 |

---

## 📖 学习路线

```
第1步: 框架 + 概念 + 主流程（本文件）— 能在脑中画出 DMA 旅程
第2步: annotations.md — probe→选域→attach→map→unmap 调用链问题链
第3步: 深入文档（问题驱动）— 机制主线 Q1→Q3→Q4→Q5；边界对照 Q2/Q6/Q7/Q8
第4步: practice ex1–ex4 — 取证（域类型/总线地址≠物理/失效代价/bypass fault）
第5步: 性能指针 → phase10；陷阱勘误；深度自测 → phase4
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题与类比

**IOMMU 是设备侧的 MMU。** CPU 访问内存过 MMU，设备 DMA 过 IOMMU——都是把"虚拟"地址
翻译成物理地址，只是发起方、输入地址空间、缺页后果不同。EPT 几乎一一对应：

```
                CPU 侧                          设备侧
谁发起          load/store                      设备 DMA
输入地址        GVA / GPA                       IOVA
输出地址        HPA                             HPA
第一层翻译      CR3 进程页表 GVA→GPA            第一级翻译 IOVA→GPA (VT-d 3.6)
第二层翻译      EPT/NPT GPA→HPA                 第二级翻译 (G)PA→HPA (3.7)
根指针          VMCS.EPTP                       context entry SSPTPTR
缓存            VPID-tagged TLB                 IOTLB / Device-TLB
失效            INVEPT / INVVPID                失效队列 + 等待
缺页谁补        硬件 VM-Exit → KVM 改 EPT       硬件记 fault → 驱动 unmap/retry
```

**规范引用**: `intel-vtd.pdf` §3.6/3.7, Figure 3-5。

### 不可变约束：类比的价值在最后一行

CPU 侧缺页"廉价"（VM-Exit 补 EPT，几微秒）。**IOMMU 侧缺页历史上"致命"**——
设备收不到"请软件补页"信号，只看到 completion abort/超时。所以 IOMMU 页表必须在
DMA 发起**之前**建好——这就是 `dma_map*()` 存在的全部理由，也是 Q5"失效为什么同步"的根源。
（例外：PRI/PRQ 把缺页变可恢复事件，见 [Q5](invalidation.md) 末尾。）

### 设计空间：IOMMU 多出的三个维度（8 问的种子）

1. **输入不止一个：RID/SID 参与选表**。CPU 入口是 CR3（一进程一套表）；IOMMU 入口是
   请求者身份（PCIe RID / SMMU StreamID）。同一 IOVA 在两个设备手里翻译成不同物理地址
   是**正常行为** → 引出 Q1（选表）、Q2（分组）。
2. **管理粒度是 `struct device`**。CPU 页表挂 mm_struct（fork/exit 驱动）；IOMMU 域挂
   device（总线 add/remove 驱动）。热插拔/绑定/电源都触及地址空间 → Q2 group、Q3 默认域。
3. **翻译结果被设备缓存 → 失效是同步问题**。CPU TLB 在芯片内；ATS 把翻译缓存到芯片外
   Device-TLB。"我改页表"和"设备看到变了"无硬件保证次序 → Q5 的全部。

### 四个核心对象

| 对象 | 定义位置 | 职责 | 生命周期 |
|---|---|---|---|
| `iommu_ops` | `include/linux/iommu.h:559` | 驱动实例方法表 | probe 注册，每硬件单元一份 |
| `iommu_domain` | `include/linux/iommu.h:208` | 一套地址空间（页表+属性） | 默认域或用户分配 |
| `iommu_group` | `drivers/iommu/iommu.c:47` | 不可分割的域绑定单位 | 引用计数 |
| `dev_iommu` | `include/linux/iommu.h:732` | 每设备私数据（fwspec/group） | `iommu_deinit_device()` 释放 |

钥匙：`iommu_ops` 的 `blocked_domain`/`identity_domain` 是指向**静态 domain** 的指针
（`intel/iommu.c:4644`），`__iommu_domain_alloc()` 遇 IDENTITY/BLOCKED 直接返回单例不分配
（`iommu.c:1947`）——Q3 的关键。

### 主流程：一次设备 DMA 的完整旅程

```
驱动 dma_map_*()
  → DMA API 分发 (kernel/dma/mapping.c:155)：dev->dma_iommu ? iommu_dma_map_page : dma_direct
  → dma-iommu：IOVA 分配 + iommu_map() 插页表（flush queue lazy unmap）
  → IOMMU core：group/domain/默认域选择 → ops->attach_dev/map_pages/flush_iotlb_all
  → 硬件驱动：建表（context entry/STE/device table）+ 失效（VT-d Qi/SMMU CMDQ/AMD 命令缓冲）
```

七步：① 驱动调 dma_map → ② 分发（dev->dma_iommu 决定走不走 IOMMU，Q3）→
③ IOVA 分配（独立地址空间，非换算，Q4）→ ④ 建表（**DMA 之前**，缺页致命的后果）→
⑤ 设备带 RID 发 DMA → ⑥ 硬件 RID 选表 walk 翻译（Q1，结果缓存）→
⑦ dma_unmap 删表 + **同步失效**（Q5）。

**最重要的分支**：`dev->dma_iommu` 为假时 DMA 地址就是物理地址，没有任何层能构造
"设备 A 眼里的设备 B"——决定 GPUDirect RDMA 等 peer 场景能否成立（Q4 末尾）。

---

## 🔍 VMM视角对比

| 接口 | 谁用 | 管什么 | 详见 |
|------|------|--------|------|
| VFIO type1 | QEMU 直通 | 用户态管 IOVA→HPA 映射，内核管页表 | [Q7](userspace.md) |
| IOMMUFD | 新 VMM | IOAS/HWPT 对象模型，更细粒度 | [Q7](userspace.md) + [iommufd-objects.md](iommufd-objects.md) |
| dma-iommu | 内核驱动 | 驱动无感，IOVA 分配器 + 自动 map | [Q4](iova.md) |

**为什么三套**：消费者不同（用户态 VMM vs 内核驱动）、谁管页表不同、域类型不同。
QEMU 经 VFIO/IOMMUFD 把 guest 内存映射成设备 IOVA 空间——这正是 phase7 直通的 DMA 地基。

---

## 📖 问题链精读入口

主流程走通后，每处"为什么"值得追问。**8 问就是本章的问题链**（每问一篇文档，
按"问题 → 为什么值得问 → 源码级回答"展开）；annotations.md 是调用链走读。

| 问 | 入口问题 | 关键引申 | 文档 |
|---|---------|---------|------|
| Q1 | 硬件怎么翻译 DMA 地址？ | RID 怎么选表？两级翻译粒度？ | [translation.md](translation.md) |
| Q2 | 为什么直通必须整组？ | 组边界由什么决定？core 怎么构造？ | [group.md](group.md) |
| Q3 | passthrough 改了什么？ | 为什么是正确性开关？identity 硬件上是什么？ | [domains.md](domains.md) |
| Q4 | DMA 地址为什么不是物理地址？ | IOVA 分配 vs 换算？peer/P2P 死结？ | [iova.md](iova.md) |
| Q5 | unmap 后立刻不用旧翻译吗？ | Device-TLB 在链路另一端？strict vs lazy？ | [invalidation.md](invalidation.md) |
| Q6 | 中断怎么穿过 IOMMU？ | IR 和 DMA 重映射为什么两套？ | [interrupts.md](interrupts.md) |
| Q7 | 为什么三套接口？ | 域类型/谁管页表/分工？ | [userspace.md](userspace.md) |
| Q8 | 三家硬件怎么对上同一抽象？ | VT-d 自有页表 vs io-pgtable 库？ | [backends.md](backends.md) |

annotations.md 调用链：probe → 选域 → attach → map → unmap → 失效（各节点带问题链）。

---

## 🔬 实践练习

4 个取证实验（详见 [`practice/README.md`](practice/README.md)，五段式 §3.5）：

| 编号 | 验证的理论 | 预测 | 入口 |
|------|-----------|------|------|
| ex1 | 默认域类型可见性（Q3） | sysfs type 反映默认域，passthrough 改变它 | practice（域类型观测） |
| ex2 | IOVA ≠ 物理地址（Q4） | dma_map 返回的总线地址与 phys 不同 | practice（总线地址对照） |
| ex3 | 失效同步代价（Q5） | strict 比 lazy unmap 慢（失效等待） | practice（strict vs lazy） |
| ex4 | bypass 触发 fault（Q1/Q3） | 未映射 IOVA 的 DMA 记 fault | practice（bypass fault） |

实验报告在 `practice/reports/`；观测脚本 `scripts/trace/iommu-analysis.sh`。

---

## ⚡ 性能与测量

- 实测结论与分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
- 测量方法：[`../phase10-performance/measurement.md`](../phase10-performance/measurement.md)
- 本章现状：strict vs lazy 失效代价（ex3）、`dmar_perf_latency` 方法有数据无
  （已登记 phase10 index §3）。失效开销标**待实测**，不复制数字。

---

## ⚠️ 陷阱与勘误

1. **`iommu.passthrough` 不是"开关 IOMMU"**：改的是默认域类型（[Q3](domains.md)）。
2. **identity 域下"设备 A 看设备 B"无法表达**：peer/P2P 无对应机制（[Q4](iova.md)）。
3. **unmap ≠ 设备立刻看不到旧翻译**：必须等失效完成，Device-TLB 在链路另一端（[Q5](invalidation.md)）。
4. **IR 和 DMA 重映射是两套**：直通中断不经普通页表翻译（[Q6](interrupts.md)）。
5. **sysfs `type` 可能不是命令行想要的**：默认域运行时降级/切换（[Q3](domains.md)）。

本章勘误：[`corrections.md`](corrections.md)

---

## ✅ 阶段检验清单

操作性：

- [ ] 能画出 §主流程 的 DMA 七步旅程
- [ ] 能解释 RID 选表与 CR3 选表的异同
- [ ] 能走通 annotations 的 probe→attach→map→unmap 调用链

深度自测（答不出 = 没学会）：

- [ ] **反事实**：如果 IOMMU 缺页能像 CPU 一样廉价恢复，`dma_map` 还需要预先建表吗？Q5 同步失效还会是必需吗？
- [ ] **不可简化**：为什么 IOMMU 不能只用"一进程一套表"（CR3 模型），非要 RID 选表 + group？
- [ ] **推导**：从"设备缓存翻译到芯片外"推出"unmap 必须同步等失效"。

---

## 🚀 下一步

- **Phase 4（PCIe）**：group 判定的物理基础（ACS/拓扑）
- **Phase 7（VFIO）**：本章是地基——DMA 映射/MSI-X 直通直接用本章机制
- **本章进阶**：IOMMUFD 对象模型、PRI/PRQ 可恢复缺页

---

## 📚 参考资料

- Linux 源码 `drivers/iommu/`（core + 三后端）、`kernel/dma/`、`include/linux/iommu.h`
- `intel-vtd.pdf`（VT-d Rev 4.1）§3.6/3.7 两级翻译、§4 翻译、§6 失效、§9 页表项
- [phase7](../phase7-vfio/README.md)（group 判定 §1.4、ACS/ATS §1.5、DMA §2、MSI-X §3.3）
- [phase5](../phase5-interrupts/README.md)（IRTE/Posted Interrupt）、[phase2](../phase2-mem-virt/README.md)（EPT 类比基础）

---

## 附：规范来源说明

仓库相关规范五份：`intel-vmx.pdf`、`intel-vtd.pdf`（Rev 4.1, D51397-016）、
`virtio-v1.3-csd01.pdf`、`arm-smmuv3.pdf`、`pcie-base-spec-r6.0.pdf`。**唯一缺 AMD-Vi 规范**。
引用规则：VT-d 以章节/表/图号为准源码为辅；SMMUv3 命令格式优先按 Linux 代码标 path:line；
AMD-Vi 只陈述代码事实并标"规范语义未核实"；PCIe/ACS/ATS 判定语义已在 phase7 §1.5 核过，
本章只交叉链接。术语用 Intel 写法 **First-Stage / Second-Stage Translation**（Rev 4.1 用 stage 不用 level）。
