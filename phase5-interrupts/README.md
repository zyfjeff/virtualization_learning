# Phase 5｜中断虚拟化与 VT-d 中断重映射

> 设备中断怎么投递到 guest？为什么 Posted Interrupts 能做到零 VM-Exit？
> 前置：phase3（IOMMU/IRTE）、phase4（PCIe MSI-X）、phase1（VMX/APICv）
> 版本基线：Linux 6.12.93 | intel-vtd.pdf Rev 4.1 / intel-vmx.pdf SDM / amd-virtualization.pdf
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)。

---

## 📋 学习目标

**核心问题**：传统中断注入每次都要 VM-Exit（KVM 拦截→注入→VM-Entry）。
Posted Interrupts 凭什么让设备中断**不经过 VMM** 直达 guest？硬件做了什么软件看不见的事？

1. 理解 APICv / Posted Interrupts 的硬件机制与零 VM-Exit 原理
2. 掌握 IRTE 两种格式（Remapped / Posted）与 PI Descriptor 字段
3. 走通 `vmx_pi_update_irte()` 桥梁函数与 PIR→IRR 同步
4. 理解 vCPU 调度/迁移/亲和性变更时 PI/IRTE 状态变化
5. 对照 AMD AVIC 与 Intel VT-d PI 的异同
6. 掌握 MSI 地址格式与亲和性迁移路径差异

---

## 📚 前置与衔接

- **前序 phase**：phase3 的 IOMMU/IRTE（中断重映射是 IOMMU 的独立机制）、
  phase4 的 MSI-X（设备中断消息）、phase1 的 VMX/APICv。
- **VMM 经验衔接**：你在 QEMU 里用 `KVM_SET_GSI_ROUTING` + `VFIO_DEVICE_SET_IRQS`
  配置过直通中断。本章讲内核侧 IRTE 怎么从 Remapped 变 Posted、PI Descriptor 怎么挂到 vCPU。

---

## 📂 本章文件

问题接力（§3.2 规则 6）：

```
README 第一性原理（核心问题：中断注入为什么能零 VM-Exit？）
  │ 引出：IRTE/PI Descriptor 长什么样？硬件怎么处理？
  ▼
posted-interrupts.md（Intel PI 系统深入）+ annotations.md（PI 描述符/IRTE/PIR→IRR 精读）
  │ 引出：vCPU 调度/迁移时 PI 状态怎么变？
  ▼
vcpu-migration-affinity.md + msi-affinity-migration.md
  │ 引出：AMD 怎么做同一件事？
  ▼
amd-avic.md（AVIC vs Intel PI）── 取证 → practice ex1–ex6
```

| 文件 | 回答什么 | 引出什么 → 去哪 |
|------|---------|----------------|
| `README.md` | 核心问题 + 第一性原理 + 全景 | PI 机制细节 → [posted-interrupts.md](posted-interrupts.md) |
| [posted-interrupts.md](posted-interrupts.md) | ★ PI Descriptor / IRTE 格式 / 零 VM-Exit / KVM 实现 | 调度迁移 → [vcpu-migration-affinity.md](vcpu-migration-affinity.md) |
| [annotations.md](annotations.md) | 问题链精读：pi_desc / IRTE / PIR→IRR / 桥梁函数 | 取证 → [practice/](practice/README.md) |
| [vcpu-migration-affinity.md](vcpu-migration-affinity.md) | ★ vCPU 调度/迁移/亲和性变更的 PI/AVIC 处理 | MSI 迁移 → [msi-affinity-migration.md](msi-affinity-migration.md) |
| [msi-affinity-migration.md](msi-affinity-migration.md) | ★ MSI 地址格式与亲和性迁移（Remapped vs Posted） | AMD 对照 → [amd-avic.md](amd-avic.md) |
| [amd-avic.md](amd-avic.md) | ★ AMD AVIC：Guest Virtual APIC / IRTE GuestMode / GA Log | 失中断分析 → [amd-avic-lost-interrupt-analysis.md](amd-avic-lost-interrupt-analysis.md) |
| [corrections.md](corrections.md) | 勘误 | 蒸馏 → AGENTS.md |

---

## 📖 学习路线

```
第1步: 核心问题 + 第一性原理（本文件）
第2步: posted-interrupts.md — PI Descriptor / IRTE / 零 VM-Exit
第3步: VMM 对比（本文件）— QEMU 协调 vs 内核 IRTE vs 硬件自动处理
第4步: 问题链精读 annotations（pi_desc/IRTE/PIR→IRR/桥梁函数）
第5步: vcpu-migration-affinity.md + msi-affinity-migration.md — 调度迁移
第6步: amd-avic.md — AMD 对照
第7步: practice ex1–ex6 取证；性能指针 → phase10
第8步: 陷阱勘误（AGENTS.md #1–9 / #18 / #19）；深度自测 → phase6
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题

设备中断要送达 guest。传统路径：设备 MSI → IOMMU 重映射 → 宿主中断 → KVM 拦截
（VM-Exit）→ 注入虚拟中断 → VM-Entry。**每次中断一次 VM-Exit 往返**，高中断率下开销巨大。

### 不可变约束

| 约束 | 内容 | 依据 |
|------|------|------|
| 中断要隔离 | 设备中断不能直接打到任意 pCPU，须经重映射 | VT-d IR |
| 注入有成本 | VM-Exit/Entry 往返数千周期 | SDM |
| 软件看不见硬件优化 | 硬件自动处理的路径 KVM 代码不执行 | AGENTS.md 核心原则 |

### 设计空间与取舍

- **Remapped 模式**：IOMMU 把设备 MSI 重映射成宿主中断，KVM 拦截注入 → 每次 VM-Exit。
- **Posted 模式**：IOMMU 把中断信息写进 vCPU 的 **PI Descriptor**（PIR 位图），
  发一个专用通知向量；**硬件**自动清 ON→PIR→VIRR→评估中断，**不产生 VM-Exit**
  （SDM 30.6 "without transferring control to the VMM"）。
- **取舍**：Posted 用硬件复杂度换零 VM-Exit；但 vCPU 被抢占/迁移时要软件维护 PI 状态
  （SN 位、NDST——都在 **PI Descriptor** 里；Posted 格式 IRTE 无 destination 字段，
  迁移不写 IRTE、不刷 IEC，AGENTS.md 陷阱 #18）——这是 vcpu-migration-affinity.md 的全部。

### 机制全景：传统 vs Posted

```
传统:  设备 MSI → IOMMU(Remapped) → 宿主 IRQ → KVM VM-Exit → 注入 → VM-Entry
Posted: 设备 MSI → IOMMU(Posted) → 写 PI Descriptor.PIR → 通知向量
        → 硬件自动 PIR→VIRR→投递（0 次 VM-Exit，KVM 代码不执行）
```

> ⚠️ **核心原则**（AGENTS.md）：只看 `handle_external_interrupt_irqoff` 会误以为通知中断
> 总触发 VM-Exit。SDM 30.6 明确：process posted interrupts=1 且向量匹配时硬件自动完成，
> **不 VM-Exit**。KVM 那段代码只处理向量不匹配/PI 未启用/嵌套/迁移等旁路。

IRTE 两种格式与 PI Descriptor 字段详见 [posted-interrupts.md](posted-interrupts.md) 与
[annotations.md](annotations.md)；术语用 Intel 规范写法（IM/VV/PDA，见陷阱节）。

---

## 🔍 VMM视角对比

| 职责 | 用户态（QEMU） | 内核（KVM + IOMMU） | 硬件 |
|------|---------------|--------------------|----|
| MSI-X 表写 | 拦截 guest 写（msix_table_mmio_write） | KVM_SET_GSI_ROUTING + VFIO_DEVICE_SET_IRQS → 设 IRTE | — |
| IRTE 模式 | 不直接管 | intel_ir_set_vcpu_affinity() 设 Posted | — |
| 中断投递 | — | 旁路情况软件处理 | **正常 Posted 路径硬件自动，0 VM-Exit** |

**为什么这样分工**：QEMU 始终在中间协调 MSI-X 配置（AGENTS.md #5），但正常中断路径
硬件闭环——VMM 和 KVM 都不参与。这是"软件看不见硬件优化"的典型。

---

## 📖 问题链精读入口

| 材料 | 入口问题 | 关键引申 | 源码入口 |
|------|---------|---------|---------|
| annotations（pi_desc/IRTE） | PI Descriptor 为什么 64 字节对齐？ | PIR/ON/SN 各干什么？ | `struct pi_desc` |
| annotations（桥梁函数） | IRTE 怎么从 Remapped 变 Posted？ | 为什么由 KVM-VFIO 触发而非组列表？ | `vmx_pi_update_irte()` |
| annotations（PIR→IRR） | 硬件投递后软件还要做什么？ | vmx_sync_pir_to_irr 何时调？ | `vmx_sync_pir_to_irr()` |
| vcpu-migration-affinity | vCPU 迁移中断为什么丢？ | NDST/SN 怎么更新？IRTE 何时才需要重写？ | `vmx_vcpu_load/put` |
| amd-avic | AMD 怎么实现同一目标？ | GA Log 为什么存在？IsRun vs SN？ | `avic.c` |

---

## 🔬 实践练习

6 个取证实验（详见 [`practice/README.md`](practice/README.md)，五段式 §3.5）：

| 编号 | 验证的理论 | 预测 | 入口 |
|------|-----------|------|------|
| ex1 | PI 环境支持（APICv+IR） | 模块参数 enable_apicv=Y，IOMMU intremap=on | `ex1-pi-env-check.sh` |
| ex2 | IRTE Posted 格式（IM=1） | 直通设备 IRTE 的 pst 位=1，PDA 指向 vCPU pi_desc | `ex2-irte-observe.sh` |
| ex3 | 零 VM-Exit（SDM 30.6） | Posted 路径通知中断不产生 kvm_exit | `ex3-pi-trace.sh` |
| ex4 | Posted vs Remapped 开销 | Posted 中断延迟/exit 数低于 Remapped | `ex4-pi-vs-remapped.sh` |
| ex5 | SN 位（vCPU 抢占） | vCPU 阻塞时 SN=1，避免无谓通知 | `ex5-on-sn-observe.sh` |
| ex6 | 迁移只改 PI desc | vCPU 迁移后 PI Descriptor NDST 更新，**IRTE 不变**（无 posted IRTE 写入/IEC 刷新） | `ex6-vcpu-migration.sh` |

---

## ⚡ 性能与测量

- 实测结论与分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
- 测量方法：[`../phase10-performance/measurement.md`](../phase10-performance/measurement.md)
- 本章现状：旧讲义"减少 VM-Exit 80%""中断延迟降低 50%""吞吐 3-5 倍/20-30%"
  **无本仓实测出处**，登记 corrections 待审计。"正常 Posted 路径 0 VM-Exit、延迟<1μs"
  有规范支撑（SDM 30.6 / VT-d 5.2.5，AGENTS.md #6）属机制结论。ex4 可实测但需分级。

---

## ⚠️ 陷阱与勘误

**本章是 AGENTS.md 全局陷阱 #1–9 / #18 / #19 的密集区，读前必过**：

1. **#1 IRTE 模式位是 IM（bit15）不是 DM**：Remapped IM=0、Posted IM=1。
   内核里叫 `pst`/`p_pst`（`include/linux/dmar.h:212/240`）。Remapped 格式的 DM（bit2）
   是 Destination Mode、DLM（bits7:5）是 Delivery Mode，别混。
2. **#2 向量字段命名**：IRTE 里是 VV（bits23:16）= 记入 PIR 的 **guest 向量**，
   PI Descriptor 里才是 NV（bits279:272）= 宿主**通知向量**（0xf2/0xf1）——命名与值都不同。
3. **#3 向量空间**：无"全局共享 256 vector"，每 CPU 独立 LAPIC，唯一性是 (CPU_ID, vector)。
4. **#4 PDA 位范围**：PDAL bits63:38、PDAH bits127:96，共 58 位，64 字节对齐。
5. **#5 VFIO MSI-X 直通**：QEMU 始终中间协调（msix_table_mmio_write → KVM_SET_GSI_ROUTING
   + VFIO_DEVICE_SET_IRQS → VFIO 设 IRTE）。
6. **#6 PI 零 VM-Exit**：正常路径 0 次 VM-Exit（SDM 30.6）。
7. **#8 CONFIG_X86_POSTED_MSI ≠ KVM PI**：前者是宿主 MSI 合并，后者是 guest VT-d PI，
   `irq_2_iommu` 里 posted_msi 与 posted_vcpu 是两个标志。
8. **#9 .isra/.constprop 符号 kprobe 取参不可信**：抓 IRTE 先验证 SID=设备 BDF。
9. **#18 Posted IRTE 无 destination 字段**：vCPU 迁移只更新 PI Descriptor 的 NDST
   （`vmx_vcpu_pi_load` cmpxchg），不写 IRTE、不刷 IEC；宿主亲和性变更对
   `posted_vcpu` 的 IRTE 显式跳过（`irq_remapping.c:1176-1177`）。
10. **#19 向量常量**：`POSTED_INTR_VECTOR=0xf2`、`POSTED_INTR_WAKEUP_VECTOR=0xf1`
    （irq_vectors.h:87-88），不是 0xf7/0xf6。

本章勘误：[`corrections.md`](corrections.md)

---

## ✅ 阶段检验清单

操作性：

- [ ] 能画出传统 vs Posted 中断路径
- [ ] 能解码 IRTE 两种格式与 PI Descriptor 字段（用规范术语）
- [ ] 能走通 vmx_pi_update_irte() 与 PIR→IRR 同步
- [ ] 能跑通 ex2/ex3 观察 IRTE 与零 VM-Exit

深度自测（答不出 = 没学会）：

- [ ] **反事实**：如果 Posted 通知向量不匹配（或 PI 未启用），中断路径会怎样退化？KVM 哪段代码接管？
- [ ] **不可简化**：PI Descriptor 的 SN 位为什么不能省？vCPU 被抢占时没有它会怎样？
- [ ] **推导**：从"VM-Exit 往返数千周期"推出"高中断率设备必须 Posted"。

---

## 🚀 下一步

- **Phase 6（virtio）**：半虚拟化设备的中断（ioeventfd/irqfd）对比直通
- **本章进阶**：AMD AVIC 失中断分析、嵌套虚拟化 PI
