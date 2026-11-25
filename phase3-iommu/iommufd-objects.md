# IOMMUFD 对象模型与 VFIO 的关系

> 本文档说明 IOMMUFD 的 IOAS/HWPT 对象模型与传统 domain 的关系，以及在 VFIO 场景中的应用。

---

## 🎯 核心概念对比

### 传统 Domain 模型

```
传统 Domain = 地址空间管理 + 硬件页表翻译
```

**特点**：
- Domain 同时管理 IOVA 映射和硬件页表
- 每个 Domain 独立，无法嵌套
- VFIO type1 直接使用这种模型

### IOMMUFD 对象模型

```
IOAS (IO Address Space)  ← 地址空间管理（软件）
  ↓
HWPT (Hardware Page Table)  ← 硬件页表翻译（= Domain）
```

**特点**：
- **分离关注点**：IOAS 管理映射账本，HWPT 负责实际翻译
- **一对多关系**：一个 IOAS 可以关联多个 HWPT
- **支持嵌套**：Parent HWPT (stage 2) + Nested HWPT (stage 1)

---

## 📊 三者关系

| 概念 | 本质 | 职责 | 对应传统概念 |
|------|------|------|-------------|
| **Group** | 硬件隔离边界 | IOMMU 最小隔离单位 | 不变 |
| **Domain** | 硬件页表 | 执行地址翻译 | 不变 |
| **IOAS** | 软件对象 | 管理 IOVA 映射账本 | Domain 的地址空间管理部分 |
| **HWPT** | 硬件对象 | 实际页表 = Domain | Domain 的硬件页表部分 |

---

## 🔗 嵌套翻译示例

### 传统模型（无法嵌套）

```
Container
  ├─ Domain 0 (独立) → Group 0
  └─ Domain 1 (独立) → Group 1

问题：两个 Domain 完全独立，无法表达嵌套关系
```

### IOMMUFD 模型（支持嵌套）

```
IOAS (软件账本)
  └─ Parent HWPT = nest_parent 的 paging HWPT
       ＝ stage-2 页表：输入 GPA → 输出 HPA
       │
       └─ Nested HWPT（用户态自备 stage-1 表基址）
            ＝ stage-1 页表：输入 进程地址/VA → 输出 GPA

翻译链（一次设备请求）：
  VA/IOVA ──stage-1(Nested)──▶ GPA ──stage-2(Parent)──▶ HPA
```

**勘误（旧稿把整条链下移了一级）**：原来写的是
`Parent HWPT (stage 2, HPA → PA)`、`Nested HWPT (stage 1, GPA → HPA)`、
`翻译链：GPA → (Nested) → HPA → (Parent) → PA`。三处都不对：

| 旧稿 | 正确 | 依据 |
|---|---|---|
| stage-1 是 `GPA → HPA` | stage-1 是 **VA/进程地址 → GPA** | VT-d Rev 4.1 §3.6：first-stage 的输入地址必须是 canonical address（4 级 48 位 / 5 级 57 位），也就是 CPU 那套 64 位页表能表示的地址空间 |
| stage-2 是 `HPA → PA` | stage-2 是 **GPA → HPA** | §3.7：second-stage 输入受 MGAW 检查，输出才是宿主物理地址；**根本不存在 "HPA → PA" 这一层** |
| 链里第一个输入是 GPA | 第一个输入是 VA | §3.8 (Nested)："requests translated through first-stage translation are also subjected to nested second-stage translation" —— first-stage 先跑，其输出再喂给 second-stage |

对象侧的对应关系（内核 6.12.93）：

- `struct iommufd_hwpt_paging` 里有 `nest_parent` 位（`drivers/iommu/iommufd/iommufd_private.h:301-310`），
  由 `IOMMU_HWPT_ALLOC_NEST_PARENT`（`include/uapi/linux/iommufd.h:364`）置上；它是**内核管理的
  那张真实页表**，在 nested 场景承担 **stage-2**。
- `struct iommufd_hwpt_nested` 只有一个 `parent` 指针（`:312-315`）—— 它自己**不带 IOAS**，
  只携带用户态递上来的 stage-1 表信息：`struct iommu_hwpt_vtd_s1 { pgtbl_addr; addr_width; flags; }`
  （`include/uapi/linux/iommufd.h:381-394`，`IOMMU_HWPT_DATA_VTD_S1`）。
- 一个 IOAS 可以挂多个 paging HWPT：`struct iommufd_ioas.hwpt_list`（`:258-263`），
  注释明确 `/* Head at iommufd_ioas::hwpt_list */`。所以"一对多"指的是 **IOAS → paging HWPT**，
  nested HWPT 不在这张表里（它挂 parent，不挂 IOAS）。

### 为什么要分两级

Guest 里跑着自己的 stage-2（Guest 认为的"GPA→HPA"，其实是 L2 的 EPT/二级页表），
宿主侧把这张表**原样交给硬件**做 stage-1，宿主自己的 stage-2 再把 GPA 落到真 HPA。
传统 `iommu_domain` 没有"父域"概念，表达不了这个父子关系，所以才需要 IOMMUFD 把
"页表对象"和"地址空间账本"拆开。

---

## 💡 为什么需要 IOMMUFD？

1. **支持 Nested Translation**：传统 Domain 无法表达父子关系
2. **映射共享**：多个 HWPT 共享同一个 IOAS 的映射，避免重复 pin
3. **细粒度控制**：IOAS 级别管理映射，HWPT 级别管理翻译

---

## 📍 在 VFIO 中的位置

- **VFIO type1**：使用传统 Domain 模型
- **VFIO + IOMMUFD**：使用 IOAS/HWPT 对象模型
- **Nested Translation**：只能通过 IOMMUFD 实现

详细的 VFIO 用法请参考 Phase 7（设备直通）。
