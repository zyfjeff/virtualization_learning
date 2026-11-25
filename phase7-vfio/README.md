# Phase 7｜VFIO 设备直通

> 设备直通怎么既接近裸机性能又保证隔离？VFIO 怎么把物理设备安全地交给 guest？
> 前置：phase3（IOMMU/group）、phase4（PCIe/ACS/ATS）、phase5（MSI-X 中断）
> 版本基线：Linux 6.12.93 / QEMU 10.1.0-rc2
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)。

---

## 📋 学习目标

**核心问题**：设备模拟每次 I/O 都 VM-Exit（慢），直通让设备直接 DMA（快）但设备能访问
任意物理内存（不安全）。VFIO 怎么靠 IOMMU 把"快"和"隔离"同时拿到？

1. 理解 VFIO 分层架构（容器/组/设备）与 Type1/IOMMUFD 后端
2. 掌握 IOMMU group 划分（直通粒度）与 ACS/ATS 依赖
3. 走通 DMA 映射路径与 KVM-VFIO 桥接
4. 理解 MSI-X 中断直通（Remapped→Posted）
5. 能诊断直通失败（group 不可分、ACS 缺失）

---

## 📚 前置与衔接

- **前序 phase**：phase3 的 IOMMU/group/domain（VFIO 消费 core 的分组）、
  phase4 的 PCIe/ACS/ATS（隔离的物理基础）、phase5 的 MSI-X/IRTE（中断直通）。
- **VMM 经验衔接**：你在 QEMU 用 `-device vfio-pci,host=...` 直通过设备。
  本章讲内核 VFIO 怎么把设备 fd 交给 QEMU、DMA 怎么映射、中断怎么直达。

---

## 📂 本章文件

问题接力（§3.2 规则 6）：

```
README 第一性原理（核心问题：快与隔离怎么兼得？）
  │ 引出：直通粒度为什么是 group？
  ▼
group-division.md（§1.4 group 划分）
  │ 引出：ACS/ATS 能力本身是什么？
  ▼
acs-ats.md（§1.5 ACS 与 ATS）
  │ 引出：DMA 怎么映射？中断怎么直通？
  ▼
README §2 DMA 映射 + §3 KVM-VFIO 桥接 ── 取证 → practice
```

| 文件 | 回答什么 | 引出什么 → 去哪 |
|------|---------|----------------|
| `README.md` | 核心问题 + 第一性原理 + 架构 + DMA/桥接 | group 粒度 → [group-division.md](group-division.md) |
| [group-division.md](group-division.md) | ★ §1.4 IOMMU group 怎么划（直通粒度） | ACS/ATS → [acs-ats.md](acs-ats.md) |
| [acs-ats.md](acs-ats.md) | ★ §1.5 ACS（隔离）与 ATS（翻译）正交两件事 | DMA → 本文件 §2 |
| [annotations.md](annotations.md) | 问题链精读：VFIO 关键路径 | 取证 → [practice/](practice/README.md) |
| [corrections.md](corrections.md) | 勘误（含实测事故复盘 F/G 节） | 蒸馏 → AGENTS.md #15/#16 |

---

## 📖 学习路线

```
第1步: 核心问题 + 第一性原理 + 架构（本文件）
第2步: group-division.md — 直通粒度为什么是 group
第3步: acs-ats.md — ACS/ATS 能力（AGENTS.md #10–14）
第4步: 本文件 §2 DMA 映射 + §3 KVM-VFIO 桥接（MSI-X 直通）
第5步: 问题链精读 annotations；practice 取证
第6步: 性能指针 → phase10；陷阱勘误；深度自测 → phase8
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题

设备模拟（virtio）每次 I/O 都 VM-Exit→QEMU→物理设备，安全但慢。直通让设备直接 DMA
到物理内存，接近裸机，但设备若不限制能 DMA 到任意地址（其他 guest/宿主）→ 不安全。

### 不可变约束

| 约束 | 内容 |
|------|------|
| 设备自主 DMA | 直通设备自己发 DMA，不经 VMM 逐次审批 |
| 隔离 | 设备只能访问分配给该 guest 的内存 |
| 硬件翻译 | DMA 地址翻译要对设备透明 |

### 设计空间与取舍

- **设备模拟**：VMM 审批每次 I/O → 安全但慢。
- **直通 + IOMMU**：设备直接 DMA，IOMMU 硬件把 IOVA 翻译成 HPA 并限制范围 →
  快（接近裸机）且隔离（IOMMU 保证）。代价：需 IOMMU 硬件、设备独占（不能多 VM 共享）、
  直通粒度受 group 限制。
- **取舍本质**：用 IOMMU 硬件隔离换"设备自主 DMA 的安全"。VFIO 是这条路的用户态接口。

### 机制全景：VFIO 分层

```
用户态 QEMU：/dev/vfio/vfio（容器）+ /dev/vfio/$GROUP（组）
  ioctl: GROUP_SET_CONTAINER / GET_DEVICE_FD / IOMMU_MAP_DMA / DEVICE_RESET
VFIO 核心 (vfio_main.c)：vfio_device / vfio_group / vfio_container
VFIO 设备驱动 (vfio_pci_core.c)：read/write 配置空间、mmap MMIO、irq（INTx/MSI/MSI-X）
IOMMU 驱动 (vfio_iommu_type1.c)：DMA 映射/域管理/页表 → IOMMU core（intel/amd）
```

**Type1 vs IOMMUFD**：Type1（`/dev/vfio/vfio`，Container→Domain）vs
IOMMUFD（`/dev/iommu`，IOAS→HWPT，原生嵌套翻译）。详见
[phase3 iommufd-objects.md](../phase3-iommu/iommufd-objects.md)。本章以 Type1 讲基础。

---

## 🔍 VMM视角对比

| 职责 | 用户态（QEMU） | 内核（VFIO + IOMMU） |
|------|---------------|---------------------|
| 设备访问 | 打开 group/device fd，mmap BAR | vfio_pci 提供 fd/region |
| DMA 映射 | VFIO_IOMMU_MAP_DMA（GPA→IOVA） | vfio_iommu_type1 建页表 |
| 中断 | VFIO_DEVICE_SET_IRQS | 设 IRTE（→ phase5 Posted） |
| 隔离 | 不直接管 | IOMMU group + 页表限制 |

**为什么这样分工**：QEMU 管"给 guest 什么设备/内存"，内核管"硬件怎么隔离/翻译"。
MSI-X 直通 QEMU 始终中间协调（AGENTS.md #5）。

---

## 📖 问题链精读入口

| 材料 | 入口问题 | 关键引申 | 源码入口 |
|------|---------|---------|---------|
| group-division | 组为什么不是 VFIO 划的？ | probe 时 core 怎么定组？ | `pci_device_group` |
| acs-ats | ACS 和 ATS 为什么正交？ | quirk 为什么优先？ | `pci_acs_enabled` |
| README §2 | DMA 映射怎么建页表？ | IOVA 怎么分配？ | `vfio_dma_do_map` |
| README §3 | KVM 怎么和 VFIO 协作？ | MSI-X 怎么从 Remapped 变 Posted？ | `kvm_vfio_file_add` |
| annotations | VFIO 关键路径逐行 | （各节问题链） | `drivers/vfio/` |

---

## 🏗️ VFIO 架构总览（核心概念）

### 1.4 核心概念

| 概念 | 说明 |
|------|------|
| **VFIO 容器** | 最高层抽象，对应 `/dev/vfio/vfio`，管理 IOMMU 域 |
| **VFIO 组** | IOMMU 组的用户态接口，对应 `/dev/vfio/$GROUP_ID` |
| **VFIO 设备** | 具体设备的接口，通过组的 `DEVICE_FD` ioctl 获取 |
| **IOMMU 域** | 一组设备的 DMA 地址空间，由 IOMMU 硬件隔离 |
| **IOMMU 组** | IOMMU 拓扑中不可分割的设备集合 |
| **DMA 映射** | 将 Guest 物理地址映射到设备可见的 IOVA |
| **MSI-X** | 设备的中断机制，通过 MSIX 表配置 |

### 1.4 IOMMU 组是怎么划出来的

> 本节详情已拆至 **[group-division.md](group-division.md)**（内容未改）。
> 要点：组不是 VFIO 划的，划分在设备 probe 的 IOMMU core 里，VFIO 只消费既成结果。
> 真机复现命令与逐跳 ACS 分析见该文档。

### 1.5 ACS 与 ATS：直通依赖的两个 PCIe 能力

> 本节详情已拆至 **[acs-ats.md](acs-ats.md)**（内容未改）。
> 要点：ACS（隔离）与 ATS（地址翻译）是正交两件事，常被混为一谈。
> 初版桥类型常量/group 101 成因/Root Port ACS 校验条件的修正见 [corrections.md](corrections.md) 勘误 5。
> 相关全局陷阱：AGENTS.md #10–14。

---
## 🔍 DMA 映射详解

### 2.1 DMA 地址翻译

```
┌──────────────────────────────────────────────────────────────────┐
│                DMA 地址翻译流程                                   │
│                                                                  │
│  Guest 中:                                                       │
│    设备驱动发起 DMA:                                              │
│      DMA 地址 = GPA (Guest 物理地址)                             │
│                                                                  │
│  IOMMU 翻译:                                                     │
│    ┌──────────┐    IOVA     ┌──────────────┐    HPA             │
│    │  设备    │ ──────────▶ │   IOMMU      │ ─────────▶ ┌─────┐ │
│    │ (PCIe)  │  DMA请求    │  页表翻译     │  物理地址   │ 内存 │ │
│    └──────────┘             └──────────────┘             └─────┘ │
│                                                                  │
│  两层映射:                                                        │
│    Guest: GPA → IOVA (由 QEMU/KVM 在 IOMMU 中设置)              │
│    设备:  看到的是 IOVA (设备认为自己 DMA 到 GPA)                 │
│                                                                  │
│  或者直接使用 GPA 作为 IOVA:                                      │
│    Guest GPA = IOVA (当 Guest 不使用虚拟 IOMMU 时)               │
│    KVM 将 GPA 映射到 HPA 到 IOMMU 页表中                         │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 DMA 映射路径

> 2026-11-25 核查：本图早期版本写的 `vfio_find_dma_valid()` / `get_user_pages_fast()`
> 在 6.12.93 里都不存在，已按真实调用链改写（见 [corrections.md](corrections.md) 勘误 12）。

```
用户态 (QEMU) DMA 映射流程:

  ioctl(VFIO_IOMMU_MAP_DMA)
      │
      ▼
  vfio_iommu_type1.c: vfio_dma_do_map()
      │
      ├── 验证参数（IOVA, 大小, 用户空间 VADDR）
      │
      ▼
  vfio_find_dma(iommu, iova, size) — 查 IOVA 区间是否冲突
      │                                （vfio_iommu_type1.c:1589）
      ├── 重复映射 / vaddr_invalid 处理 → 返回错误（:1590-1608）
      ▼
  vfio_iommu_iova_dma_valid() — 落在允许的 IOVA 区间内？（:1615）
      │
      ▼
  vfio_pin_pages_remote() — 固定用户空间页面
      │                    （vfio_iommu_type1.c:597）
      ├── vaddr_get_pfns() → pin_user_pages_remote(mm, vaddr, npages,
      │       flags | FOLL_LONGTERM, ...)   ← :551 / :563
      │   ★ 不是 get_user_pages_fast()：本文件里 get_user_pages 0 命中。
      │     FOLL_PIN（而非 FOLL_GET）+ FOLL_LONGTERM 才是"防迁移/防回收"的依据
      ├── 防止页面被换出
      │
      ▼
  iommu_map() — 写入 IOMMU 页表
      │
      ├── IOVA → PFN 映射
      ├── 权限设置 (Read/Write)
      │
      ▼
  IOMMU 硬件生效
      │
      └── 设备 DMA 现在可以通过 IOMMU 访问这些物理页
```

---

## 🔗 KVM-VFIO 桥接

### 3.1 KVM 如何与 VFIO 交互

```
┌──────────────────────────────────────────────────────────────────┐
│                  KVM-VFIO 桥接架构                                │
│                                                                  │
│  ┌──────────────────┐              ┌──────────────────┐          │
│  │    QEMU          │              │    KVM           │          │
│  │                  │              │                  │          │
│  │  /dev/vfio/5     │              │  /dev/kvm        │          │
│  │  (设备文件)      │              │  (KVM 设备文件)  │          │
│  └────────┬─────────┘              └────────┬─────────┘          │
│           │                                  │                    │
│           │ KVM_DEV_VFIO_GROUP               │                    │
│           │ (将 VFIO 组关联到 KVM VM)        │                    │
│           └──────────────┬───────────────────┘                    │
│                          │                                        │
│                          ▼                                        │
│              ┌───────────────────────┐                            │
│              │  virt/kvm/vfio.c     │                            │
│              │  kvm_vfio 模块       │                            │
│              │                      │                            │
│              │  功能:               │                            │
│              │  - 管理 VFIO 组列表  │                            │
│              │  - DMA coherency     │                            │
│              │  - 中断路由关联      │                            │
│              │  - 设备生命周期管理  │                            │
│              └───────────────────────┘                            │
│                                                                  │
│  关键交互点:                                                      │
│    1. QEMU 将 VFIO 设备/组的 fd 传给 KVM (KVM_DEV_VFIO_FILE_ADD)  │
│    2. KVM 取引用，并递增 assigned_device_count                    │
│    3. 根据设备是否 DMA coherent，注册/注销 noncoherent DMA        │
│                                                                  │
│  注意: IRTE 改成 Posted 模式**不走这条路**。                      │
│  组列表只提供 PI 的前提条件之一(assigned_device_count > 0)，       │
│  真正的触发是 irq_bypass 的 token 配对，见 3.3 节。               │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 KVM VFIO 操作

```c
/* 来源: virt/kvm/vfio.c */

/*
 * KVM VFIO 桥接支持的操作:
 *
 * KVM_DEV_VFIO_GROUP_ADD:
 *   将 VFIO 组添加到 KVM VM
 *   - 获取 vfio_group 引用
 *   - 加入 kvm->vfio_groups 列表
 *   - 用于后续 PI 配置
 *
 * KVM_DEV_VFIO_GROUP_DEL:
 *   从 KVM VM 移除 VFIO 组
 *   - 释放 vfio_group 引用
 *   - 从列表中删除
 *
 * KVM_DEV_VFIO_GROUP_SET_SPAPR_TCE:
 *   (PowerPC 特有) 设置 TCE 表
 *
 * KVM_DEV_VFIO_FILE_ADD / DEL:
 *   与 GROUP_ADD/DEL 类似，但使用文件描述符
 */
```

### 3.3 MSI-X 中断直通：从 Remapped 到 Posted

这是 phase5（中断虚拟化）与 phase7（VFIO，本阶段）真正接壤的地方。要点是**职责分工**：

| 阶段 | 谁做 | IRTE 状态 |
|---|---|---|
| `VFIO_DEVICE_SET_IRQS` 启用 MSI-X | VFIO | Remapped（`IM=0`），中断打进宿主 |
| irqfd 注册 + token 配对 | irq_bypass | 不变 |
| `__connect()` 回调进 KVM | KVM | 改写为 Posted（`IM=1`），硬件直投 vCPU |

**VFIO 自己永远建不出 Posted IRTE** —— 它不知道 vCPU 的 PI Descriptor 在哪。
Posted 化只能由 KVM 发起，而 KVM 要发起就必须先和 VFIO 对上。

#### 接缝：eventfd 指针当 token

VFIO 在装配每个向量的最后一步注册 producer：

```c
/* 来源: drivers/vfio/pci/vfio_pci_intrs.c:515 */
	ctx->producer.token = trigger;
	ctx->producer.irq = irq;
	ret = irq_bypass_register_producer(&ctx->producer);
```

KVM 在建立 irqfd 时注册 consumer：

```c
/* 来源: virt/kvm/eventfd.c:444 */
		irqfd->consumer.token = (void *)irqfd->eventfd;
		irqfd->consumer.add_producer = kvm_arch_irq_bypass_add_producer;
		irqfd->consumer.del_producer = kvm_arch_irq_bypass_del_producer;
		irqfd->consumer.stop = kvm_arch_irq_bypass_stop;
		irqfd->consumer.start = kvm_arch_irq_bypass_start;
		ret = irq_bypass_register_consumer(&irqfd->consumer);
```

两个 `token` 都是**同一个 eventfd 上下文的指针** —— QEMU 把同一个 eventfd 分别交给
`VFIO_DEVICE_SET_IRQS` 和 `KVM_IRQFD`，内核两侧便自动认亲。注册**无先后要求**，
producer 和 consumer 两侧都会扫描对面的链表（`virt/lib/irqbypass.c:108` 与 `:204`）。

#### 配对成功后的调用链

```
irq_bypass_register_{producer,consumer}()
  └─ __connect()                                  virt/lib/irqbypass.c:30
       └─ cons->add_producer()
            = kvm_arch_irq_bypass_add_producer()   arch/x86/kvm/x86.c:13665
                 ├─ kvm_arch_start_assignment()    :13673
                 └─ pi_update_irte()
                      = vmx_pi_update_irte()       arch/x86/kvm/vmx/posted_intr.c:272
                           └─ irq_set_vcpu_affinity()
                                = intel_ir_set_vcpu_affinity()
                                                   drivers/iommu/intel/irq_remapping.c:1248
                                     └─ modify_irte()   ← IRTE 落盘为 Posted
```

KVM 提供的两个关键信息就是 Posted IRTE 里 VFIO 拿不到的部分：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:319 */
		vcpu_info.pi_desc_addr = __pa(vcpu_to_pi_desc(vcpu));
		vcpu_info.vector = irq.vector;
```

`pi_desc_addr` 变成 IRTE 的 `PDAL`/`PDAH`，`vector` 变成 `VV`（Virtual Vector）。

#### 不是所有中断都会被 Posted 化

`vmx_pi_update_irte()` 先查四个全局前提：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:135 */
static bool vmx_can_use_vtd_pi(struct kvm *kvm)
{
	return irqchip_in_kernel(kvm) && enable_apicv &&
		kvm_arch_has_assigned_device(kvm) &&
		irq_remapping_cap(IRQ_POSTING_CAP);
}
```

再逐条路由表项筛，只有单目标 vCPU 的普通中断能过：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:314 */
		kvm_set_msi_irq(kvm, e, &irq);
		if (!kvm_intr_is_single_vcpu(kvm, &irq, &vcpu) ||
		    !kvm_irq_is_postable(&irq))
			continue;
```

被筛掉的（多播、广播、多目标低优先级）会走 `posted_intr.c:338` 的
`irq_set_vcpu_affinity(host_irq, NULL)` **退回 Remapped 模式**。
所以一台机器上同一个设备的不同向量，完全可能一部分 Posted、一部分 Remapped。

##### 两种不同的 Posted Interrupt：VT-d PI vs KVM-side PI

> ⚠️ **容易混淆的点**：`vmx_can_use_vtd_pi()` 中有 `has_assigned_device` 条件，
> 容易让人以为"没有直通设备就不能用 Posted Interrupt"。这是**错误**的理解。

PI Descriptor 机制有**两种触发源**，共享同一套底层硬件（PIR→IRR 自动同步），但入口不同：

| | **VT-d PI** (IOMMU 路径) | **KVM-side PI** (APICv 路径) |
|---|---|---|
| **谁写 PI Descriptor** | IOMMU 硬件直接写 | KVM 软件写 (`vmx_deliver_posted_interrupt`) |
| **触发源** | 直通设备 MSI → IOMMU 拦截 | 任何中断源 → KVM APIC 模拟 |
| **调用入口** | `vmx_pi_update_irte()` | `vmx_deliver_interrupt()` |
| **需要 `has_assigned_device`** | ✅ 是（没有直通设备就没有 MSI 给 IOMMU） | ❌ 否 |
| **需要的条件** | `vmx_can_use_vtd_pi()` 四条件全满足 | `apicv_active` |

```
中断来源 A: 直通设备 (VT-d PI)
  设备 MSI → IOMMU → PI Descriptor ← IOMMU 硬件直接写
                    ↑
                    需要 vmx_can_use_vtd_pi() = true

中断来源 B: 模拟设备 (KVM-side PI)
  QEMU → kvm_set_irq() → APIC 模拟 → deliver_interrupt()
                                        → vmx_deliver_posted_interrupt()
                                        → PI Descriptor ← KVM 软件写
                                          ↑
                                          只需 apicv_active = true

中断来源 C: vCPU 间 IPI (KVM-side PI)
  kvm_apic_send_ipi() → deliver_interrupt() → vmx_deliver_posted_interrupt()
```

**`vmx_deliver_interrupt()`** 注册为 `kvm_x86_ops.deliver_interrupt`，由
`lapic.c:1352` 的 `kvm_x86_call(deliver_interrupt)()` 调用。这意味着 **KVM 侧的
PI Descriptor 机制对所有中断源都可用**——包括模拟设备、IPI 等，不限于直通设备。

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:4299 */
void vmx_deliver_interrupt(struct kvm_lapic *apic, int delivery_mode,
                           int trig_mode, int vector)
{
    if (vmx_deliver_posted_interrupt(vcpu, vector)) {
        /* Posted 失败 → 退回传统路径 */
        kvm_lapic_set_irr(vector, apic);
        kvm_make_request(KVM_REQ_EVENT, vcpu);
        kvm_vcpu_kick(vcpu);
    }
}
```

`vmx_deliver_posted_interrupt()` 只检查 `apicv_active`，**不检查 `has_assigned_device`**。
所以即使纯模拟 VM，只要 APICv 开启，中断也能走 PI Descriptor 路径减少 VM-Exit。

**总结**：`has_assigned_device` 只影响 **IOMMU 直接 posted**（VT-d PI 路径），
不影响 **KVM 自己写 PI Descriptor**（KVM-side PI 路径）。两者共享 PI Descriptor
结构和 PIR→IRR 硬件同步机制，但触发源和门控条件不同。

#### 实测：一个向量的 IRTE 前后对比

在 `0000:4b:00.0` 上抓 `modify_irte` 的原始 128 位（完整过程见
[practice/README.md](practice/README.md) 练习3）：

| | `pst` (spec **IM**) | 向量 | 目标信息 |
|---|---|---|---|
| VFIO 建好时 | **0** = Remapped | `0x21` = 33 | `dest_id` = 宿主 APIC ID |
| KVM 改写后 | **1** = Posted | `VV` = 33 | `PDAL=0x2e5210b` `PDAH=0x29` → PI Desc `0x29b94842c0` |

`SID` 两次都是 `0x4b00`，即设备 BDF `4b:00.0`，可用来校验探针取参是否正确。
从 KVM 注册 consumer 到 Posted IRTE 落盘实测约 53 µs。

**Posted 化之后宿主彻底看不到这个中断了**：Guest 内压 80 MB 只读 I/O，
Guest 侧中断计数 +629，而宿主 `/proc/interrupts` 上对应的三个 IRQ 计数**全程为 0** ——
`vfio_msihandler()` 根本没被调用，也就没有 `eventfd_signal()`，更没有 VM-Exit。

> ⚠️ 别把它和 `CONFIG_X86_POSTED_MSI` 搞混。后者（6.11+）是**宿主自己**的
> posted MSI 优化，与 Guest 无关，走的是 `prepare_irte_posted()`
> （`irq_remapping.c:1111`）+ `posted_msi_supported()` 门控。
> 本节讲的是 KVM 的 Guest VT-d PI，走 `intel_ir_set_vcpu_affinity()`，两条路互不相干。

---

## 📖 源码阅读路线

### 推荐阅读顺序

```
┌─────────────────────────────────────────────────────────────┐
│                    源码阅读路线                                │
│                                                             │
│  Step 1: VFIO 核心框架                                       │
│  ├── drivers/vfio/vfio_main.c       ← VFIO 核心            │
│  │   ├── vfio_group 管理                                    │
│  │   ├── vfio_device 管理                                   │
│  │   └── ioctl 分发                                          │
│  └── include/linux/vfio.h           ← 接口定义              │
│                                                             │
│  Step 2: IOMMU Type 1 驱动                                   │
│  ├── drivers/vfio/vfio_iommu_type1.c ← DMA 映射核心        │
│  │   ├── vfio_dma_do_map()                                   │
│  │   ├── vfio_pin_pages_remote()                             │
│  │   └── iommu_map()                                         │
│  └── drivers/iommu/iommu.c          ← IOMMU 核心           │
│                                                             │
│  Step 3: VFIO PCI 驱动                                       │
│  ├── drivers/vfio/pci/vfio_pci_core.c ← PCI 设备直通       │
│  │   ├── 配置空间模拟                                         │
│  │   ├── MMIO 区域映射                                        │
│  │   └── 中断管理 (INTx/MSI/MSI-X)                          │
│  └── drivers/vfio/pci/vfio_pci_priv.h                       │
│                                                             │
│  Step 4: KVM-VFIO 桥接                                       │
│  └── virt/kvm/vfio.c                ← KVM VFIO 桥接        │
│      ├── kvm_vfio_group 管理                                 │
│      └── kvm_dev_vfio_ops                                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 关键函数索引

| 函数名 | 文件 | 作用 |
|--------|------|------|
| `pci_device_group()` | `drivers/iommu/iommu.c:1515` | PCI 设备的组划分主逻辑，四步判定 |
| `pci_acs_path_enabled()` | `drivers/pci/pci.c:3693` | 检查整条上游路径的 ACS，任一跳不满足即 false |
| `pci_acs_enabled()` | `drivers/pci/pci.c:3620` | 单设备 ACS 判定：先查 quirk（`:3624`），再按 PCIe 类型分类 |
| `pci_dev_specific_acs_enabled()` | `drivers/pci/quirks.c:5234` | ACS quirk 表分发，三态返回；`pci_quirk_rciep_acs()` 匹配所有 Intel RCiEP |
| `get_pci_function_alias_group()` | `drivers/iommu/iommu.c:1391` | 归并同 slot 上未隔离的多功能 function |
| `pci_acs_init()` | `drivers/pci/pci.c:3717` | probe 时找 ACS extended capability 并尝试启用 |
| `pci_enable_acs()` | `drivers/pci/pci.c:1075` | 写 ACS Control：quirk 优先，**返回 0 才跳过标准写入** |
| `pci_std_enable_acs()` | `drivers/pci/pci.c:1052` | 标准写入 SV/RR/CR/UF；`TB` 只给外部/不可信设备或 `pci=noats` |
| `pci_request_acs()` | `drivers/pci/pci.c:943` | 置位「需要 ACS」，由 IOMMU 探测时调用（Intel：`dmar.c:935`） |
| `pci_dev_specific_enable_acs()` | `drivers/pci/quirks.c:5438` | ACS 启用 quirk 表分发，命中返回 0 |
| `pci_ats_supported()` | `drivers/pci/ats.c:41` | 只看 `ats_cap` 与 `untrusted`，**不查 ACS** |
| `pci_enable_ats()` | `drivers/pci/ats.c:90` | 写 ATS Control 的 `E` 与 `STU`；VF 只校验不新写 |
| `pci_ats_page_aligned()` | `drivers/pci/ats.c:193` | 读 `PCI_ATS_CAP` bit5，Intel 开 ATS 的硬前提 |
| `pci_ats_queue_depth()` | `drivers/pci/ats.c:168` | 读 Invalidate Queue Depth，`0` 含义见规范 |
| `iommu_enable_pci_caps()` | `drivers/iommu/intel/iommu.c:1287` | Intel 侧 ATS 启用门控（三合一判断在 `:1295-1297`） |
| `iommu_disable_pci_caps()` | `drivers/iommu/intel/iommu.c:1300` | 清 `E` 位；与规范 §4.5.2 的顺序差异见 1.5.6 |
| `pci_enable_pasid()` | `drivers/pci/ats.c:395` | 前置 `pci_acs_path_enabled(RR+UF)` 判断在 `ats.c:419` |
| `cache_tag_flush_devtlb_psi()` | `drivers/iommu/intel/cache.c:390` | 把 Device-TLB 失效排进 qi batch |
| `iommu_group_claim_dma_owner()` | `drivers/iommu/iommu.c:3214` | 认领组的 DMA ownership，组内有普通驱动则 `-EPERM`（判断在 `:3222`） |
| `vfio_file_iommu_group()` | `drivers/vfio/group.c:839` | 获取文件的 IOMMU 组（声明在 `include/linux/vfio.h:300`；★ 不在 `vfio_main.c`） |
| `vfio_dma_do_map()` | `vfio_iommu_type1.c` | DMA 映射操作 |
| `vfio_pin_pages_remote()` | `vfio_iommu_type1.c` | 固定用户页面 |
| `vfio_pci_core_enable()` | `vfio_pci_core.c` | 启用 PCI 设备直通 |
| `vfio_pci_core_mmap()` | `drivers/vfio/pci/vfio_pci_core.c:1752` | 映射设备 MMIO（ops 挂在 `drivers/vfio/pci/vfio_pci.c:140`；★ 6.12.93 里没有 `vfio_pci_mmap` 这个函数，同前缀的只有 `vfio_pci_mmap_ops` `vfio_pci_core.c:1745` 与 `vfio_pci_mmap_{page,huge}_fault` `vfio_pci_core.c:1740`/`:1685`） |
| `kvm_vfio_file_add()` | `virt/kvm/vfio.c:143` | 把 VFIO **file** 加进 KVM（dispatch `virt/kvm/vfio.c:277`；★ 不叫 `kvm_vfio_group_add` —— 6.12 起属性名是 `KVM_DEV_VFIO_FILE_ADD`，`KVM_DEV_VFIO_GROUP_ADD` 只是 uapi 兼容别名，`include/uapi/linux/kvm.h:1133`、`:1136-1140`） |
| `kvm_vfio_update_coherency()` | `virt/kvm/vfio.c` | 更新 DMA 一致性 |
| `vfio_pci_set_msi_trigger()` | `vfio_pci_intrs.c` | `SET_IRQS` 的 MSI/MSI-X 入口 |
| `vfio_msi_set_vector_signal()` | `vfio_pci_intrs.c:447` | 单向量装配：`request_irq` + 注册 producer |
| `vfio_msihandler()` | `vfio_pci_intrs.c:373` | 宿主中断处理，仅 Remapped 模式下执行 |
| `irq_bypass_register_producer()` | `virt/lib/irqbypass.c:84` | VFIO 侧注册，按 token 找 consumer |
| `irq_bypass_register_consumer()` | `virt/lib/irqbypass.c:179` | KVM 侧注册，按 token 找 producer |
| `kvm_arch_irq_bypass_add_producer()` | `arch/x86/kvm/x86.c:13665` | 配对成功回调，转发到 PI 更新 |
| `vmx_pi_update_irte()` | `vmx/posted_intr.c:272` | 判定可否 Posted，取 PI Desc 地址 |
| `intel_ir_set_vcpu_affinity()` | `intel/irq_remapping.c:1248` | 把 IRTE 实际改写为 Posted 格式 |

---

## 🔬 实践练习

> 下面是手工验证步骤。带实测输出的可运行实验（ownership 认领时机、IOVA→HPA 映射验证、
> MSI-X 中断直通与 IRTE Posted 化）见 [practice/README.md](practice/README.md)。

### 练习 1：基本设备直通

```bash
# 1. 检查 IOMMU 是否启用
dmesg | grep -i iommu
# 应看到类似: DMAR: IOMMU enabled

# 2. 检查 VFIO 模块是否加载
lsmod | grep vfio

# 3. 加载 VFIO 模块（如果未加载）
modprobe vfio
modprobe vfio-pci
modprobe vfio_iommu_type1

# 4. 查看设备的 IOMMU 组
readlink /sys/bus/pci/devices/0000:03:00.0/iommu_group
# 输出类似: ../../../../kernel/iommu_groups/5

# 5. 绑定设备到 VFIO 驱动
echo "8086 1533" > /sys/bus/pci/drivers/vfio-pci/new_id
# 或使用 vendor:device ID

# 6. 在 QEMU 中使用设备直通
# qemu-system-x86_64 ... \
#   -device vfio-pci,host=0000:03:00.0
```

### 练习 2：DMA 映射跟踪

```bash
# 跟踪 IOMMU DMA 映射操作
# ★ set_event 里写的是 **system:event**：iommu 的事件名就是 `map` / `unmap`
#   （include/trace/events/iommu.h:79、:103；宿主 `events/iommu/` 下的目录名同名）。
#   `iommu_map` 这种拼法在内核里不存在，写进去直接失败。
# ★ 清场要**显式**：带 O_TRUNC 的写（`echo x > file`、不带 -a 的 tee）会先把
#   **全部**已启用事件清掉，只想加事件一律 `>>`
#   （../phase10-performance/measurement.md §5 第 3 条）。
: > /sys/kernel/debug/tracing/set_event
echo iommu:map   >> /sys/kernel/debug/tracing/set_event
echo iommu:unmap >> /sys/kernel/debug/tracing/set_event

# ★ `vfio_iommu_type1` **不是 trace system**：6.12.93 的 include/trace/events/ 下
#   没有 vfio.h，宿主 events/ 里也没有 vfio 目录 —— 那一行以前是写不进去的。
#   要看 VFIO 侧的 DMA map/unmap，走 function tracer（四个函数名均在
#   drivers/vfio/vfio_iommu_type1.c，宿主 available_filter_functions 实测存在）：
echo function > /sys/kernel/debug/tracing/current_tracer
: > /sys/kernel/debug/tracing/set_ftrace_filter
# 一个名字一次 `>>`：一次写多个时，第一个匹配不上的会中止本次 write，后面的连带丢失
echo vfio_dma_do_map        >> /sys/kernel/debug/tracing/set_ftrace_filter  # :1548
echo vfio_dma_do_unmap      >> /sys/kernel/debug/tracing/set_ftrace_filter  # :1270
echo vfio_pin_map_dma       >> /sys/kernel/debug/tracing/set_ftrace_filter  # :1448
echo vfio_iommu_type1_ioctl >> /sys/kernel/debug/tracing/set_ftrace_filter  # :2991

echo 1 > /sys/kernel/debug/tracing/tracing_on

# 启动带设备直通的虚拟机
# 然后在 trace 中观察 DMA 映射活动

echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace

# 收尾：tracefs 是全局状态，四个出口分别清
echo nop > /sys/kernel/debug/tracing/current_tracer
: > /sys/kernel/debug/tracing/set_ftrace_filter
: > /sys/kernel/debug/tracing/set_event
```

### 练习 3：分析 IOMMU 域

```bash
# 查看 IOMMU 域信息
ls /sys/kernel/iommu_groups/

# 查看特定组的设备
ls /sys/kernel/iommu_groups/5/devices/

# 查看 IOMMU 设备信息
dmesg | grep -E "iommu|DMAR|AMD-Vi"

# 查看 DMAR 表
cat /sys/firmware/acpi/tables/DMAR 2>/dev/null | hexdump -C | head
```

> 拿到本机的分组结果后，对照 [1.4 IOMMU 组是怎么划出来的](#14-iommu-组是怎么划出来的)
> 逐条推导：哪些组是因为 PCIe-to-PCI 桥而并的，哪些是同 slot 多功能设备，
> 哪些设备独占一组、以及它上游每一跳的 ACS 状况。

### 练习 4：性能测试

```bash
# 使用 iperf3 测试直通网卡性能
# 在 Host 上:
iperf3 -s

# 在 Guest 上:
iperf3 -c <host_ip> -t 30

# 对比:
# 1. 纯模拟网卡 (virtio) 的吞吐量
# 2. VFIO 直通的吞吐量
# 3. 裸机（无虚拟化）的吞吐量

# 使用 perf 分析 IOMMU 开销
perf record -e intel_iommu:dtlb_walk -a -- sleep 10
```

### 练习 5：中断分析

```bash
# 1. 查看直通设备的宿主 IRQ（名字形如 vfio-msix[0](0000:4b:00.0)）
grep vfio /proc/interrupts
cat /sys/bus/pci/devices/0000:4b:00.0/msi_irqs

# 2. 判断有没有走中断重映射（IR- 前缀）
for n in $(ls /sys/bus/pci/devices/0000:4b:00.0/msi_irqs); do
  echo "irq $n -> $(cat /sys/kernel/irq/$n/chip_name)"
done

# 3. 观察 KVM 侧的 IRTE 更新（这个 tracepoint 真实存在，
#    定义在 arch/x86/kvm/trace.h:1080）
sudo bash -c 'cd /sys/kernel/tracing
echo 1 > events/kvm/kvm_pi_irte_update/enable
echo 1 > tracing_on'
# 启动带直通设备的 VM，然后看 trace
sudo cat /sys/kernel/tracing/trace | grep kvm_pi_irte_update
```

> 注意：内核里**没有** `vfio_irq_set` tracepoint（`vfio_irq_set` 只是
> `include/uapi/linux/vfio.h:584` 的一个结构体名），本仓早期版本写错过。
> 要观测 VFIO 侧的装配过程只能下 kprobe，做法见
> [practice/README.md](practice/README.md) 练习3。

**判读要点**：Posted 模式生效后，第 1 步里那些 IRQ 的计数应当**始终为 0**。
计数在涨说明中断仍走 Remapped 路径，排查见 [陷阱4](#陷阱4中断停在-remapped-模式性能达不到预期)。

---

## 🔧 常见问题排查

### 设备直通失败

```bash
# 1. 检查 IOMMU 启用状态
cat /proc/cmdline | grep -E "intel_iommu|amd_iommu"
# 需要: intel_iommu=on 或 amd_iommu=on

# 2. 检查 ACS (Access Control Services)
# ACS 确保设备间 DMA 隔离
setpci -s 00:1c.0 ECAP_ACS+6.b  # 读取 ACS 能力

# 3. 检查 IOMMU 组完整性
# 如果多个设备在同一组且不能分离，可能无法直通单个设备

# 4. 检查 dmesg 错误
dmesg | grep -i vfio
dmesg | grep -i iommu
```

---

## 📚 参考资料

- Linux kernel: `Documentation/driver-api/vfio.rst`
- Intel SDM Vol 3, Chapter 10: Advanced Programmable Interrupt Controller
- Intel VT-d Specification: IOMMU 硬件细节
- KVM Forum talks on VFIO and device passthrough
- 论文: *"VFIO: The Virtual Function I/O Framework"*

---

## ✅ 阶段检验清单

- [ ] VFIO 容器、组、设备三者的关系是什么？
- [ ] IOMMU 组为什么有时包含多个设备？如何处理？
- [ ] ACS 七个能力位分别管什么？为什么 `ACSCap` 里的 `-` 不算隔离失败？
- [ ] 内核在哪些时机会**写** ACS Control？`pci=config_acs=` 的标志串该怎么读？
- [ ] ATS 开启后 `AT=10b` 的请求硬件还做哪些检查？为什么直通时 `unmap` 会变慢？
- [ ] `vfio_dma_do_map()` 的完整路径是什么？从 ioctl 到 IOMMU 页表更新
- [ ] KVM-VFIO 桥接的作用是什么？为什么 KVM 需要知道 VFIO 组？
- [ ] 直通设备的 MSI-X 中断是如何路由到 Guest 的？
- [ ] 设备直通的 DMA 一致性如何保证？

深度自测（答不出 = 没学会）：

- [ ] **反事实**：如果没有 IOMMU，直通设备直接 DMA 会怎样？为什么"快"和"隔离"必须靠 IOMMU 兼得？
- [ ] **不可简化**：为什么直通粒度必须是 group 而不能按单设备？强行拆组会破坏什么？
- [ ] **推导**：从"设备自主 DMA 不经 VMM 审批"推出"必须有硬件（IOMMU）限制 DMA 范围"。

---

## 🚀 下一步

- **Phase 8（时钟虚拟化）**：直通设备的时钟/TSC 处理
- **本章进阶**：IOMMUFD 嵌套翻译（phase3）、MSI-X Posted 实测（phase5 ex4）

---

## ⚡ 性能与测量

- 实测结论与分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
- 测量方法：[`../phase10-performance/measurement.md`](../phase10-performance/measurement.md)
- 本章现状：旧讲义"IOTLB 缓存 DMA 负载 20-30%""DMA 批处理 10-15%"**无本仓实测出处**，
  且示例代码（`iommu_enable_iotlb_cache`/`VTD_CAP_IOTLB_CACHE`）未核实疑似伪代码，
  已移除并登记 corrections 待审计。机制（IOTLB 缓存/批处理/中断亲和性）保留为定性描述。
- 中断 Remapped vs Posted 开销：见 [acs-ats.md](acs-ats.md) 与 phase5 ex4（待实测分级）。

---

## ⚠️ 常见陷阱

### 陷阱1：IOMMU组包含多个设备

**场景**：无法直通单个设备

**症状**：绑定 `vfio-pci` **会成功**，但随后 `VFIO_GROUP_SET_CONTAINER` 返回 `-EPERM`；
`VFIO_GROUP_GET_STATUS` 中 `VIABLE` 标志为 0。

**原因**：IOMMU组包含多个设备，组内仍有设备绑在普通驱动上，DMA ownership 无法认领。

`vfio-pci` 声明了 `.driver_managed_dma = true`（`drivers/vfio/pci/vfio_pci.c:205`），
probe 阶段刻意不动 `owner_cnt`，所以绑定不会失败。认领发生在 container 附加时
（`drivers/vfio/container.c:437`），此时才检查：

```c
/* 来源: drivers/iommu/iommu.c:3214 —— iommu_group_claim_dma_owner() */
	if (group->owner_cnt) {
		ret = -EPERM;
		goto unlock_out;
	}
```

**解决**：
```bash
# 列出组内全部成员
ls /sys/kernel/iommu_groups/<N>/devices/

# 组内所有设备都必须绑到 vfio-pci（或处于 unbound），然后整组一起直通
qemu-system-x86_64 ... \
  -device vfio-pci,host=0000:03:00.0 \
  -device vfio-pci,host=0000:03:00.1  # 同组的其他设备
```

> 详见 [corrections.md](corrections.md) 勘误 1，含 kprobe 实测的完整接管时序。

### 陷阱2：ACS未启用

**场景**：设备与上游桥被划入同一个 IOMMU group，无法单独直通

**症状**：`/sys/kernel/iommu_groups/<N>/devices/` 里除目标设备外还有上游桥

**原因**：上游链路某一跳不满足 `REQ_ACS_FLAGS`，内核认为无法保证 peer-to-peer DMA 隔离，
于是把桥拉进同一组：

```c
/* 来源: drivers/iommu/iommu.c:1547-1550 —— pci_device_group() 的 ACS 上溯循环（循环头在 :1543） */
	if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
		break;

	pdev = bus->self;          /* 隔离不成立 → 把桥拉进来 */
```

**排查**：
```bash
# 看上游链路每一跳的 PCIe 类型与 ACS 能力
lspci -vvv -s 49:01.0 | grep -A2 "Access Control Services"
```

**注意**：要分清 ACS 的**能力位**（`ACSCap`）与**控制位**（`ACSCtl`）。能力位是硬件属性，
**没有任何内核参数能变出一块硬件没有的能力**；控制位是可写的，而且只要内核检测到 IOMMU，
`pci_request_acs()` 就会在设备 probe 时把 `SV`/`RR`/`CR`/`UF` 全部置上 —— 也就是说
"忘了开 ACS" 这个前提在上游内核里基本不成立。写控制位时 `caps->ctrl |= (caps->cap & ...)`
会被 `ACSCap` 掩掉（`pci.c:1055`），所以 `ACSCtl` 里显示 `-` 的位，对应 `ACSCap` 里一定也是 `-`
—— **缺的是能力，不是开关**（见 [acs-ats.md](acs-ats.md) §1.5.2）。上游确实存在两个能改控制位的参数
`pci=config_acs=` 与 `pci=disable_acs_redir=`，但它们的作用都是**进一步关掉**隔离，
解决不了「组太大」。社区流传的 `pcie_acs_override=downstream,multifunction` 来自第三方补丁，
**不在上游内核中**（6.12.93 的 `drivers/pci/` 与 `kernel-parameters.txt` 均无此参数），
在原生内核上写了会被静默忽略；`pci=noaer` 关的是 AER，与 ACS 无关。
硬件不提供隔离时唯一正规解法是整组一起直通 —— 强行拆组会真实破坏 DMA 隔离。

> 单功能设备"没有 ACS 能力"反而算隔离成立，这一点极易误判，
> 详见 [corrections.md](corrections.md) 勘误 2。
>
> 完整的划分规则（四步判定、`pci_acs_enabled()` 的三类处理、一个 switch 拆出 30+ 组的实测）
> 见 [1.4 IOMMU 组是怎么划出来的](#14-iommu-组是怎么划出来的)。

### 陷阱3：DMA一致性未处理

**场景**：Guest和Host看到不一致的数据

**症状**：Guest读取到过时的数据

**原因**：DMA一致性未处理

**解决**：
```c
// KVM-VFIO桥接中自动处理
kvm_vfio_update_coherency(kvm, vfio_group);
// 确保DMA操作使用正确的内存类型
```

### 陷阱4：中断停在 Remapped 模式，性能达不到预期

**场景**：直通设备中断能到 Guest，但延迟偏高、宿主 CPU 有可观的中断开销

**症状**：宿主 `/proc/interrupts` 上该设备的 IRQ 计数**在涨**。
Posted 模式下这些计数应当**始终为 0**，因为 `vfio_msihandler()` 不会被调用。

```bash
# 找出设备的宿主 IRQ 号，观察计数是否增长
cat /sys/bus/pci/devices/0000:4b:00.0/msi_irqs
watch -n1 'grep -E "^ *(112|113|114):" /proc/interrupts'
```

**原因**：IRTE 没有被改写成 Posted 模式（`IM=0`），中断仍然先进宿主、再经 eventfd
唤醒 irqfd，多绕一整圈。常见诱因：

1. `enable_apicv=N`，或硬件不支持 IRQ posting；
2. 该向量的目标不是单个 vCPU（多播/广播/多目标低优先级），被
   `posted_intr.c:315` 主动排除，这是**设计如此，不是故障**；
3. Guest 里做了 irqbalance，把向量的 affinity 改成多 CPU 掩码。

**排查**：
```bash
cat /sys/module/kvm_intel/parameters/enable_apicv    # 需要 Y
dmesg | grep -i "posting\|Posted-Interrupt"

# 区分 Remapped / Posted 需要看 IRTE 原始位，chip_name 只能告诉你有没有重映射
cat /sys/kernel/irq/112/chip_name                   # IR- 前缀 = 有重映射
```

`/sys/kernel/debug/kvm/` 下**没有** `irq_routing` 这个文件（本仓早期版本写错过）。
可靠的观测手段是 `kvm_pi_irte_update` tracepoint 或对 `modify_irte` 下 kprobe。

> 完整机制见 [3.3 节](#33-msi-x-中断直通从-remapped-到-posted)，
> 含实测数据的动手实验见 [practice/README.md](practice/README.md) 练习3。
