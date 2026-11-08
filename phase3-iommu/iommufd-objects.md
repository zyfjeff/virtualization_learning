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
IOAS (统一管理)
  ├─ Parent HWPT (stage 2, HPA → PA)
  └─ Nested HWPT (stage 1, GPA → HPA)
       └─ Group

翻译链：GPA → (Nested) → HPA → (Parent) → PA
```

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
