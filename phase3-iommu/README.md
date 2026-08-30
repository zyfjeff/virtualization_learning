# 第3阶段：IOMMU 层

> 基于 Linux 6.12.93 内核源码 | 预计学习时间：2-3 周
>
> **前置依赖**:
> - 第二阶段（内存虚拟化 EPT）：本阶段"框架"一节直接用 EPT 做类比，没读过会失去最大的一个杠杆
> - 不需要先读第六阶段；**反过来，本阶段是第六阶段的地基**
>
> **建议阅读顺序：phase2 → phase3（本章）→ phase4 → phase6。**

---

## 📋 学习目标

前几阶段里，IOMMU 一直是以"结论"的形式出现的：phase4 说"直通设备的中断要经 IRTE"，
phase6 说"ACS 决定 group 边界，group 决定能否单独直通"。这些都对，但它们背后有一整层
**内核怎么建域、怎么翻译、怎么失效**的机制，此前没有系统讲过。本阶段补的就是这一层。

本阶段**不按机制铺陈，而是先立框架、走一遍主流程，然后围绕主流程冒出的 8 个问题逐个
展开**。读完你应当能：

1. 把 IOMMU 和 CPU 的 MMU/EPT 放进同一个心智模型，说清两者相同与不同
2. 走通"一次设备 DMA 从 `dma_map()` 到硬件翻译出物理地址"的完整主流程
3. 对主流程里每一处"为什么是这样而不是那样"给出源码级的回答
4. 准确解释 `iommu.passthrough` 改了什么——它是正确性开关，不只是性能开关
5. 对照 VT-d / ARM SMMUv3 / AMD-Vi 三个后端，知道同一份 core 语义各自落到什么硬件动作

---

## 📂 本章文件

本阶段的主干是本文件的"框架 → 概念 → 主流程 → 问题"四段；每个问题对应一篇深入文档。

| 文件 | 对应问题 | 内容 |
|------|------|------|
| `README.md` | — | 本文件：框架 + 概念 + 主流程 + 8 问清单 |
| [`translation.md`](translation.md) | Q1 | 硬件怎么把设备的 DMA 地址翻译成物理地址 |
| [`group.md`](group.md) | Q2 | 为什么直通必须"整组一起"，组怎么划出来 |
| [`domains.md`](domains.md) | Q3 | `iommu.passthrough` 到底改了什么 |
| [`iova.md`](iova.md) | Q4 | 驱动拿到的"DMA 地址"为什么不是物理地址 |
| [`invalidation.md`](invalidation.md) | Q5 | unmap 之后，硬件是不是立刻就不用旧翻译了 |
| [`interrupts.md`](interrupts.md) | Q6 | 直通设备的中断怎么穿过 IOMMU |
| [`userspace.md`](userspace.md) | Q7 | VFIO / IOMMUFD / dma-iommu 三套接口各管什么 |
| [`backends.md`](backends.md) | Q8 | Intel / ARM / AMD 怎么对上同一套软件抽象 |
| `annotations.md` | — | 源码精读：probe→选域→attach→map→unmap→失效 完整调用链走读 |
| `practice/` | — | 4 个实验：域类型可见性 / 总线地址≠物理地址 / 失效代价 / bypass fault |
| `corrections.md` | — | 勘误：实测发现本阶段或既有文档结论有误时记录 |

---

## ⚠️ 规范来源说明

本仓库的规范资料与本阶段相关的有五份：`intel-vmx.pdf`、`intel-vtd.pdf`（Intel® VT-d™
Architecture Specification, **Rev 4.1**, Order Number D51397-016）、
`virtio-v1.3-csd01.pdf`、`arm-smmuv3.pdf`（Arm SMMUv3 架构规范）、
`pcie-base-spec-r6.0.pdf`（PCIe Base Specification r6.0）。**唯一缺的是 AMD-Vi 规范**
（AMD I/O Virtualization Technology Specification）。

因此本阶段的引用规则是（各篇文档开头的"规范可用性声明"与此一致）：

- **VT-d 相关**：以 `intel-vtd.pdf` 章节号 + 表号/图号为准，源码为辅。
- **SMMUv3 相关**：`arm-smmuv3.pdf` 在仓库内可用，但文中**命令格式与字段一律优先按
  Linux 代码与注释陈述**并标注 `path:line`，规范用于核对字段语义。
- **AMD-Vi 相关**：无规范，只陈述 `drivers/iommu/amd/` 代码事实；凡只能靠代码说话的
  地方会明确写"代码如此，规范语义未核实"。
- **PCIe/ACS/ATS 相关**：`pcie-base-spec-r6.0.pdf` 可用；VT-d 把部分语义（如 Device-TLB
  失效的 S 位）**委托**给 PCIe/ATS 规范，本阶段只给位运算事实并标出委托边界。
  ACS/ATS 的判定语义已在第六阶段 §1.5 以源码为据核过，本阶段不重复，只做交叉链接。
- 术语统一采用 Intel 规范写法：**First-Stage Translation / Second-Stage Translation**
  （Rev 4.1 用 *stage* 不用 *level*）。

---

# 一、框架：IOMMU 是什么

## 1.1 一句话

**IOMMU 是设备侧的 MMU。** CPU 访问内存要过 MMU，设备发起 DMA 也要过 IOMMU；两者干的
是同一件事——把一个"虚拟"地址翻译成物理地址——只是发起方、输入地址空间和缺页后果不同。

把第二阶段学过的 EPT 直接搬过来，对应关系几乎是一一的：

```
┌────────────────────────────────────────────────────────────────────┐
│                    CPU 侧                          设备侧           │
│                                                                     │
│  谁发起访问        load/store                     设备 DMA          │
│  输入的地址        GVA / GPA                      IOVA              │
│  输出的地址        HPA                            HPA               │
│                                                                     │
│  第一层翻译        CR3 指向的进程页表             第一级翻译 (3.6)   │
│                    GVA → GPA                      IOVA → GPA        │
│  第二层翻译        EPT / NPT                      第二级翻译 (3.7)   │
│                    GPA → HPA                      (G)PA → HPA       │
│                                                                     │
│  翻译结构根指针    VMCS.EPTP                      context entry 的   │
│                                                  SSPTPTR 字段        │
│  缓存              VPID-tagged TLB                IOTLB / Device-TLB│
│  缓存不一致的代价  INVEPT / INVVPID               失效队列 + 等待    │
│  缺页时谁来补      硬件 VM-Exit → KVM 改 EPT      硬件记 fault →     │
│                                                   驱动 unmap/retry  │
└────────────────────────────────────────────────────────────────────┘
```

**规范引用**: `intel-vtd.pdf`, Section 3.6 (First-Stage Translation), 3.7
(Second-Stage Translation), Figure 3-5。

## 1.2 类比的价值在最后一行

CPU 侧缺页是"廉价"的：硬件 VM-Exit 到 KVM，KVM 补一个 EPT entry 再 VM-Entry，代价几微秒、
路径成熟。**IOMMU 侧缺页在历史上是"致命"的**——设备收不到"请软件补页"这种信号，它只会
看到一个 completion abort 或超时。所以 IOMMU 页表必须在 DMA 发起**之前**就建好，这就是
`dma_map*()` 存在的全部理由，也是后面 Q5"失效为什么是同步的"的根源。

（例外是 PRI/PRQ：少数硬件能把缺页变成可恢复事件，见 [Q5](invalidation.md) 末尾。）

## 1.3 IOMMU 多出来的三个维度

类比到这里为止。有三件事 CPU MMU 完全没有对应物，它们是后面所有困难的来源，也是 8 个问题
里大部分问题的种子：

**(1) 输入不止一个：RID/SID 参与"选表"**
CPU 的翻译入口是 `CR3`，一个进程一套表。IOMMU 的入口是**请求者身份**：PCIe TLP 自带
Requester ID（bus:devfn），VT-d 用它索引 root entry 和 context entry，SMMUv3 叫
StreamID 并索引 Stream Table。所以"同一个 IOVA 在两个设备手里翻译成不同物理地址"是
**正常行为**——这一条直接引出 Q1（怎么选表）和 Q2（为什么设备要分组）。

**(2) 管理粒度是"设备"，而且是 `struct device`**
CPU 页表挂在 `mm_struct` 上，由 `fork/exit` 驱动；IOMMU 域挂在 `struct device` 上，由
总线的 add/remove 通知驱动。设备的热插拔、驱动绑定、电源管理都会触及地址空间，于是必须
有东西回答"这个设备的 DMA 允许看到哪些地址"——那就是 IOMMU group（Q2）和默认域（Q3）。

**(3) 翻译结果会被设备缓存，于是失效变成同步问题**
CPU 的 TLB 全在芯片内部，`invlpg`/`mov cr3` 的时序设计者是自己。PCIe 设备可以用 ATS 把
翻译结果缓存到**设备自己的 Device-TLB** 里——缓存副本跑到芯片外面、一条异步链路的另一端。
"我改了页表"和"设备看到的页表变了"之间没有硬件保证的次序，这就是 Q5 的全部。

---

# 二、概念：四个核心对象

读源码前先认识四个对象，后面所有函数都围绕它们转：

| 对象 | 定义位置 | 一句话职责 | 生命周期 |
|---|---|---|---|
| `iommu_ops` | `include/linux/iommu.h:559` | 一个 IOMMU **驱动实例**的方法表 | 驱动 probe 时注册，每硬件单元一份 |
| `iommu_domain` | `include/linux/iommu.h:208` | 一套地址空间（页表 + 属性） | 由默认域分配或用户（VFIO/IOMMUFD）分配 |
| `iommu_group` | `drivers/iommu/iommu.c:47` | **不可分割的域绑定单位**：这组设备必须共享同一个域 | 引用计数，随最后一个成员消失 |
| `dev_iommu` | `include/linux/iommu.h:732` | 挂在 `dev->iommu` 上的每设备私数据（fwspec、group 指针） | `iommu_deinit_device()` 释放 |

四条关系记住即可：

- **`iommu_ops` 是"驱动能做什么"**：map/unmap/attach/选组都由它的方法表分派到具体后端。
- **`iommu_group` 是"隔离边界"**：同一组的设备要么一起直通、要么一起不直通（Q2）。
- **`iommu_domain` 是"一张地址空间"**：一组设备共享一个域；域里装的是页表。
- **`dev_iommu` 是"设备自己的小档案"**：记录这个设备属于哪个组、有哪些 PASID 能力。

`iommu_ops` 里有几个字段是理解本阶段的钥匙——它们**不是函数**，而是指向**静态
`iommu_domain`** 的指针：

```c
/* 来源: drivers/iommu/intel/iommu.c:4644-4647 */
const struct iommu_ops intel_iommu_ops = {
	.blocked_domain		= &blocking_domain,
	.release_domain		= &blocking_domain,
	.identity_domain	= &identity_domain,
```

`__iommu_domain_alloc()` 遇到 IDENTITY/BLOCKED 时**直接返回这个静态对象、不分配新内存**
（`drivers/iommu/iommu.c:1947-1950`）。这个"单例"细节是 Q3 的关键。

---

# 三、主流程：一次设备 DMA 的完整旅程

这是全阶段的主干。先走通这条"快乐路径"，8 个问题就都挂在它的各个节点上。

## 3.1 分层图

```
┌─────────────────────────────────────────────────────────────────────┐
│  驱动 / 设备驱动                                          (消费者)   │
│    dma_map_single()  dma_map_sg()  dma_alloc_coherent()              │
└───────────────────────────────┬─────────────────────────────────────┘
                                │
┌───────────────────────────────▼─────────────────────────────────────┐
│  DMA API 分发       kernel/dma/mapping.c:155  dma_map_page_attrs()   │
│    if (dma_map_direct(dev, ops))            → dma_direct_map_page()  │
│    else if (use_dma_iommu(dev))             → iommu_dma_map_page()   │
│    else                                     → ops->map_page()        │
│    use_dma_iommu(dev) 就是 return dev->dma_iommu                     │
│         (include/linux/iommu-dma.h:13-16)                            │
└───────────────────────────────┬─────────────────────────────────────┘
                    ┌───────────┴────────────┐
┌───────────────────▼──────────┐ ┌───────────▼──────────────────────┐
│  dma-iommu                   │ │  dma-direct / swiotlb            │
│  drivers/iommu/dma-iommu.c   │ │  （identity 域下走这条，见 Q3）    │
│  IOVA 分配器 + iommu_map()   │ └──────────────────────────────────┘
│  flush queue（lazy unmap）   │
└───────────────────┬──────────┘
┌───────────────────▼──────────────────────────────────────────────────┐
│  IOMMU core        drivers/iommu/iommu.c                              │
│    iommu_group / iommu_domain / 默认域选择 / iommu_map() 插页表        │
│    → ops->attach_dev / ops->map_pages / ops->flush_iotlb_all          │
└───────────────────┬──────────────────────────────────────────────────┘
┌───────────────────▼──────────────────────────────────────────────────┐
│  硬件驱动      intel/iommu.c   arm/arm-smmu-v3/   amd/iommu.c         │
│    建表：context entry / PASID table entry / STE / device table       │
│    失效：VT-d Qi 队列 / SMMUv3 CMDQ / AMD-Vi 命令缓冲                 │
└──────────────────────────────────────────────────────────────────────┘
```

## 3.2 沿主流程走一遍

**① 驱动要 DMA，调 `dma_map_*()`。** 它手里只有 `struct page` / 内核虚拟地址。

**② DMA API 分发。** `dma_map_page_attrs()` 看 `dev->dma_iommu`：为真就走
`iommu_dma_map_page()`（进 IOMMU），为假走 `dma_direct_map_page()`（直通，"DMA 地址"就是
物理地址）。**这条分支决定了一个设备到底走不走 IOMMU**，而它取决于默认域类型（Q3）。

**③ IOVA 分配。** 走 IOMMU 时，dma-iommu 先从 IOVA 分配器挑一个空闲的总线地址（IOVA），
它**不是**从物理地址换算出来的——这是一个独立的地址空间（Q4）。

**④ 建表。** `iommu_map()` 把 `IOVA → phys` 插进该域的页表（Q1 讲的页表格式）。注意：
**这一步发生在设备发 DMA 之前**，这正是 1.2 节"缺页致命"的工程后果。

**⑤ 设备发起 DMA。** 设备拿着 ③ 得到的 IOVA 作为目标地址发 PCIe 请求，请求里带自己的
Requester ID。

**⑥ 硬件翻译。** IOMMU 用 RID 选表（Q1），walk 页表，把 IOVA 翻译成 HPA，访问内存。
翻译结果可能被缓存进 IOTLB / Device-TLB。

**⑦ 驱动 DMA 完成，调 `dma_unmap_*()`。** 删页表 + **同步失效**缓存（Q5）。到这一步，
"设备还能不能用旧翻译"这件事才被彻底关掉。

## 3.3 主流程里最重要的那条分支

图里那条三分支是整个 IOMMU 层的"总开关"：`dev->dma_iommu` 为假时，驱动拿到的"DMA 地址"
就是物理地址本身，**没有任何一层能构造出"设备 A 眼里的设备 B"**。这条线直接决定了
GPUDirect RDMA 这类 peer 场景能不能成立（Q4 末尾展开）。

---

# 四、围绕主流程冒出的 8 个问题

主流程走通后，每一处"为什么是这样"都值得追问。下面 8 个问题就是本阶段的目录——每个问题
一篇文档，按"问题 → 为什么值得问 → 源码级回答"展开。

### Q1. 硬件怎么把设备的 DMA 地址翻译成物理地址？
两级翻译、RID 怎么选到那张表、页表长什么样、粒度怎么定。这是 IOMMU 的"机制本体"。
→ [`translation.md`](translation.md)

### Q2. 为什么直通必须"整组一起"？组是怎么划出来的？
一个设备想单独直通，内核却说"它和隔壁那设备在一个组里，必须一起"。组的边界由什么决定、
core 怎么构造它。判定规则在 phase6 §1.4/§1.5，构造在本阶段。
→ [`group.md`](group.md)

### Q3. `iommu.passthrough` 到底改了什么？为什么是正确性开关？
它常被说成"开/关 IOMMU"——这是错的，错的方向足以让人排查时完全找错地方。默认域怎么选、
identity 域在硬件上是什么。
→ [`domains.md`](domains.md)

### Q4. 驱动拿到的"DMA 地址"为什么不是 CPU 物理地址？
IOVA 是分配出来的、不是换算出来的；保留区、32 位闸门、以及为什么 identity 域下没法表达
"设备 A 看设备 B"（peer/P2P 的死结）。
→ [`iova.md`](iova.md)

### Q5. `iommu_unmap()` 之后，硬件是不是立刻就不用旧翻译了？
不是。缓存副本可能躺在芯片外的 Device-TLB 里，所以 unmap 必须同步等失效完成。失效队列的
代价从哪来、strict 与 lazy 差在哪。
→ [`invalidation.md`](invalidation.md)

### Q6. 直通设备的中断怎么穿过 IOMMU？MSI 地址走不走普通翻译？
中断重映射（IR）和 DMA 重映射是**两套独立机制**；MSI 地址要么走固定保留窗口、要么由软件
单独建一段映射，取决于后端。
→ [`interrupts.md`](interrupts.md)

### Q7. VFIO / IOMMUFD / dma-iommu 三套接口各管什么？为什么需要这么多层？
同样是"给用户态/驱动暴露 DMA"，为什么内核养了三套接口？它们的域类型、谁管页表、怎么分工。
→ [`userspace.md`](userspace.md)

### Q8. Intel / ARM / AMD 三家硬件，怎么对上同一套软件抽象？
同一份 `iommu_ops` 语义，VT-d 走自己的页表、ARM/AMD 走 io-pgtable 库；启动链、表结构、
identity 的硬件表达各不相同。
→ [`backends.md`](backends.md)

---

# 五、怎么读这个阶段

**第一遍（半天）**：只读本文件的"框架 → 概念 → 主流程 → 8 问"，不点进任何深入文档。目标
是能在脑子里画出 3.2 节那条旅程，并知道 8 个问题各自挂在哪一步。

**第二遍（按需）**：带着具体问题点进对应文档。每篇文档都以"问题 + 为什么值得问"开头，
可以独立读；但建议顺序是 Q1 → Q3 → Q4 → Q5（机制主线），再读 Q2、Q6、Q7、Q8（边界与
对照）。

**第三遍（动手）**：做 `practice/` 里的 4 个实验，用 `scripts/trace/iommu-analysis.sh`
观测。实验前提是按 [AGENTS.md 陷阱 7](../AGENTS.md) 用 `scripts/vm/boot-vm.sh` 启动 VM
（默认已带 `-enable-kvm -cpu host`）。

---

# 📚 参考资料

- **Linux 内核源码** `/root/code/linux-6.12.93/`：`drivers/iommu/`（core + 三后端）、
  `kernel/dma/`（DMA API）、`include/linux/iommu.h`。
- **`intel-vtd.pdf`**（VT-d Rev 4.1）：§3.6/3.7 两级翻译、§4 地址翻译、§6 失效、§9 页表项。
- **第六阶段** [`../phase6-vfio/README.md`](../phase6-vfio/README.md)：group 判定（§1.4）、
  ACS/ATS（§1.5）、DMA 映射（§2）、MSI-X 直通（§3.3）。
- **第四阶段** [`../phase4-interrupts/README.md`](../phase4-interrupts/README.md)：IRTE、
  Posted Interrupt、MSI 地址格式。
- **第二阶段** [`../phase2-mem-virt/README.md`](../phase2-mem-virt/README.md)：EPT 两级翻译，
  本阶段类比的基础。

---

# ⚠️ 常见陷阱（本阶段层面）

1. **`iommu.passthrough` 不是"开关 IOMMU"**：它改的是默认域类型，见 [Q3](domains.md)。
2. **identity 域下"设备 A 看设备 B"无法表达**：peer/P2P 地址没有对应机制，见 [Q4](iova.md)。
3. **unmap ≠ 设备立刻看不到旧翻译**：必须等失效完成，且 Device-TLB 在链路另一端，
   见 [Q5](invalidation.md)。
4. **IR 和 DMA 重映射是两套**：直通中断不经过普通页表翻译，见 [Q6](interrupts.md)。
5. **sysfs 的 `type` 可能不是命令行想要的**：默认域会在运行时降级/切换，见 [Q3](domains.md)。
