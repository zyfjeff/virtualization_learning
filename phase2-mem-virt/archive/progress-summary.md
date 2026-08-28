# Phase 2 学习进度总结

> 内存虚拟化（EPT）核心概念与实现

---

## 已完成内容

### ✅ 关卡 1: EPT 硬件原理

**核心概念**：
- 两级地址翻译：GVA → GPA → HPA
- EPT 页表结构（4 级：PML4 → PDPT → PD → PT）
- EPTP（EPT Pointer）：VMCS 中指向 EPT 根页面
- EPT Violation 类型：缺页、权限违规、Misconfiguration

**文档**：
- `README.md` - 第 1 章：EPT 硬件原理

**关键要点**：
```
Guest Virtual Address (GVA)
    ↓ (Guest Page Table, 4 级)
Guest Physical Address (GPA)
    ↓ (EPT, 4 级)
Host Physical Address (HPA)

总计：4 × 4 = 16 次内存访问（不使用大页）
```

---

### ✅ 关卡 2: SPTE 格式与内存类型

**核心概念**：
- SPTE（Shadow Page Table Entry）：KVM 软件位 + 硬件位
- 权限分离：硬件位 vs 软件位
- 内存类型：WB（Write-Back）vs UC（Uncacheable）
- MMIO 识别机制：pfn_valid → PageReserved → e820 表

**文档**：
- `README.md` - 第 2 章：SPTE 格式详解（2.1 - 2.4）
- `mmio-identification.md` - MMIO 识别与处理机制（完整）

**关键要点**：

#### SPTE 位布局
```
  63 62-12  11  10  9  8  7  6  5  4  3  2  1  0
 ┌───┬─────┬───┬───┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
 │HW │ PFN │ X │ W │ R│IG│MM│MM│MM│MM│PC│A │D │P │
 │大 │     │ │ │ │ │U │U │U │U │D │D │E │C │I │R │
 │页 │     │ │ │ │ │X │W │R │X │I │R ││C │C │T │E │
 └───┴─────┴───┴───┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
```

#### 内存类型决策
```c
vmx_get_mt_mask(vcpu, gfn, is_mmio)
    ├─ is_mmio = true  → UC (Uncacheable) - 设备内存
    └─ is_mmio = false → WB (Write-Back)  - 普通 RAM
```

#### MMIO 识别流程
```
kvm_is_mmio_pfn(pfn)
    ├─ pfn_valid(pfn) = true
    │   ├─ is_zero_pfn(pfn)? → 不是 MMIO
    │   ├─ PageReserved(pfn)? → 可能是 MMIO
    │   └─ pat_pfn_immune_to_uc_mtrr(pfn)? → 检查 PAT
    │
    └─ pfn_valid(pfn) = false
        └─ e820__mapped_raw_any(pfn)? → 检查 e820 表
```

**实践程序**：
- `practice/memtype_analysis.c` - 内存类型处理分析

---

### ✅ 关卡 3: EPT Violation 处理流程

**核心概念**：
- 完整调用链（10 个步骤）
- 锁机制：mmu_lock（spinlock）+ rcu_read_lock
- 性能优化：快速路径、大页、预取
- 脏页跟踪与写保护机制
- 硬件 A/D 位与 PML（Page Modification Logging）

**文档**：
- `ept-violation-handling.md` - EPT Violation 处理流程详解
  - 第 1-7 章：EPT Violation 处理流程
  - 第 8 章：脏页跟踪与写保护机制（新增）

**完整调用链**：
```
1. vmx_handle_exit()                    ← 硬件触发 VM-Exit
2. handle_ept_violation()               ← 读取 GPA 和 exit_qualification
3. __vmx_handle_ept_violation()         ← 构造 error_code
4. kvm_mmu_page_fault()                 ← 检查 MMIO
5. kvm_mmu_do_page_fault()              ← 初始化 fault 结构体
6. kvm_tdp_page_fault()                 ← TDP 模式
7. direct_page_fault()                  ← 分配物理页，获取锁
8. kvm_tdp_mmu_map()                    ← 遍历 EPT 页表
9. tdp_mmu_map_handle_target_level()    ← 构建 SPTE，原子写入
10. VM-Resume                           ← Guest 继续执行
```

**关键函数**：

| 函数 | 文件 | 作用 |
|------|------|------|
| `handle_ept_violation()` | `vmx/vmx.c:5782` | 读取 VMCS，调用下一层 |
| `__vmx_handle_ept_violation()` | `vmx/common.h:9` | 解析 exit_qualification |
| `kvm_mmu_page_fault()` | `mmu/mmu.c:6106` | 检查 MMIO，调用页错误处理 |
| `kvm_mmu_do_page_fault()` | `mmu/mmu_internal.h:293` | 初始化 fault，调用 TDP |
| `direct_page_fault()` | `mmu/mmu.c:4575` | 快速路径，分配页面，获取锁 |
| `kvm_tdp_mmu_map()` | `mmu/tdp_mmu.c:1104` | 遍历 EPT，创建页表 |
| `tdp_mmu_map_handle_target_level()` | `mmu/tdp_mmu.c:1017` | 构建 SPTE，原子写入 |
| `make_spte()` | `mmu/spte.c:211` | 构建 SPTE 值（包含内存类型） |
| `vmx_get_mt_mask()` | `vmx/vmx.c:7679` | 动态计算内存类型 |
| `kvm_is_mmio_pfn()` | `mmu/spte.c:108` | 判断是否是 MMIO |
| `mark_page_dirty_in_slot()` | `kvm_main.c:3604` | 记录脏页到 dirty_bitmap |
| `vmx_flush_pml_buffer()` | `vmx/vmx.c:6182` | 批量处理 PML buffer |
| `spte_clear_dirty()` | `mmu/mmu.c:1233` | 清除 SPTE 的 Dirty 位 |
| `spte_write_protect()` | `mmu/mmu.c:1205` | 移除 SPTE 写权限 |

**实践程序**：
- `practice/ept_violation_demo.c` - EPT Violation 处理流程可视化

---

### ✅ 关卡 4: 脏页跟踪与写保护机制

**核心概念**：
- 两种脏页跟踪模式：硬件 A/D 位 vs 软件写保护
- PML（Page Modification Logging）批量处理
- 脏页记录机制：dirty_bitmap vs 遍历 SPTE
- KVM_GET_DIRTY_LOG 完整流程
- 热迁移增量同步优化

**文档**：
- `ept-violation-handling.md` - 第 8 章：脏页跟踪与写保护机制

**硬件 A/D 位流程**：
```
Guest 写入页面
    ↓
CPU 自动设置 Dirty 位（bit 9）
    ↓
同时写入 PML buffer
    ↓
无 VM-Exit！
```

**软件写保护流程**：
```
开启脏页跟踪 → 移除所有 SPTE 写权限
    ↓
Guest 写入 → EPT Violation → VM-Exit
    ↓
恢复写权限 + mark_page_dirty_in_slot()
    ↓
VM-Resume
```

**关键函数**：
- `mark_page_dirty_in_slot()`：记录脏页到 dirty_bitmap
- `vmx_flush_pml_buffer()`：批量处理 PML buffer
- `spte_clear_dirty()`：清除 SPTE 的 Dirty 位
- `spte_write_protect()`：移除 SPTE 写权限

---

### ✅ 关卡 5: TDP MMU 并发机制

**核心概念**：
- 两种锁机制：mmu_lock + rcu_read_lock
- 原子操作：cmpxchg（比较并交换）
- 快速页错误路径（Fast Page Fault）
- RCU（Read-Copy-Update）无锁读取
- 多 vCPU 并发修改的正确性保证

**文档**：
- `tdp-mmu-concurrency.md` - TDP MMU 并发机制详解

**并发场景**：
```
vCPU 0: 访问 GPA 0x10000
vCPU 1: 访问 GPA 0x20000
vCPU 2: 访问 GPA 0x30000
    ↓
并发执行 kvm_tdp_mmu_map()
    ↓
使用 cmpxchg 原子更新 SPTE
    ↓
无冲突，完全并发
```

**关键函数**：
- `tdp_mmu_set_spte_atomic()`：原子更新 SPTE
- `try_cmpxchg64()`：原子比较并交换
- `fast_page_fault()`：快速页错误路径
- `fast_pf_fix_direct_spte()`：无锁修复 SPTE

**性能对比**：
| 场景 | 延迟 | 说明 |
|------|------|------|
| 快速路径（无锁） | ~200 ns | 权限修复 |
| 慢速路径（有锁） | 2-5 μs | 页表修改 |
| RCU 读取 | ~50 ns | 页表遍历 |

---

## 核心知识点总结

### 1. EPT 两级地址翻译

```
GVA → (Guest PT) → GPA → (EPT) → HPA
```

**性能优化**：
- 使用大页（2M/1G）减少页表层级
- EPT TLB 缓存减少内存访问
- 预取提前建立映射

### 2. SPTE 软件位复用

KVM 在 SPTE 中混合了硬件和软件信息：
- **硬件位**：Present、Writable、User、PFN、内存类型
- **软件位**：MMU-present、MMU-writable、A/D bits

**为什么可以复用？**
- 4KB 页对齐，低位（bit 0-11）可用于软件
- 硬件只关心高位（PFN + 权限）

### 3. 内存类型动态决策

```c
vmx_get_mt_mask(vcpu, gfn, is_mmio)
    ├─ MMIO → UC (Uncacheable)
    └─ RAM  → WB (Write-Back)
```

**为什么 MMIO 必须用 UC？**
- 防止缓存导致设备状态不同步
- 避免 Machine Check 错误
- 确保精确的访问时序

### 4. MMIO 识别三层机制

```
kvm_is_mmio_pfn(pfn)
    ├─ pfn_valid() → 有 struct page？
    ├─ PageReserved() → 是保留页？
    └─ e820 表 → 是 RAM 类型？
```

**GPU BAR 识别**：
- 地址：0xF0000000+（高地址区域）
- 没有 struct page（pfn_valid = false）
- e820 标记为 reserved（不是 RAM）
- 识别为 MMIO → UC 内存类型

### 5. 脏页跟踪机制

**两种模式**：

| 模式 | 适用条件 | 工作原理 | 性能 |
|------|---------|---------|------|
| **硬件 A/D 位** | CPU 支持 EPT A/D | CPU 自动设置 Dirty 位 | ⭐⭐⭐⭐⭐ 极快 |
| **软件写保护** | CPU 不支持硬件 A/D | 移除写权限，异常时记录 | ⭐⭐ 较慢 |

**硬件 A/D 位流程**：
```
Guest 写入页面
    ↓
CPU 自动设置 SPTE 的 Dirty 位（bit 9）
    ↓
同时写入 PML buffer（如果启用）
    ↓
无 VM-Exit！
```

**软件写保护流程**：
```
开启脏页跟踪 → 移除所有 SPTE 写权限
    ↓
Guest 写入 → EPT Violation → VM-Exit
    ↓
恢复写权限 + mark_page_dirty_in_slot()
    ↓
VM-Resume
```

**关键函数**：
- `mark_page_dirty_in_slot()`：记录脏页到 dirty_bitmap
- `vmx_flush_pml_buffer()`：批量处理 PML buffer
- `spte_clear_dirty()`：清除 SPTE 的 Dirty 位
- `spte_write_protect()`：移除 SPTE 写权限

**为什么不用遍历 SPTE？**
- dirty_bitmap 已经记录了所有脏页
- O(1) 读取 bitmap vs O(N) 遍历 SPTE
- 性能差距巨大


### 5. EPT Violation 处理性能

| 场景 | 延迟 | 说明 |
|------|------|------|
| VM-Exit + 页表建立 | 2-5 μs | 首次访问 |
| 快速路径（无锁修复） | ~200 ns | 权限问题 |
| EPT 命中 | ~50 ns | 缓存命中 |
| RAM 访问（WB） | ~1 ns | 缓存命中 |
| MMIO 访问（UC） | ~100-500 ns | 无缓存 |

**性能优化策略**：
1. 使用大页（2M/1G）
2. 快速路径（无锁修复）
3. 预取（提前映射）
4. 内存类型优化（WB/UC）

---

## 实践练习

### 练习 1: 内存类型分析

```bash
cd /root/code/kvm-study/phase2-mem-virt/practice
./memtype_analysis
```

**输出**：
- RAM 使用 WB 内存类型
- GPU BAR 使用 UC 内存类型
- SPTE 构建过程

### 练习 2: EPT Violation 可视化

```bash
cd /root/code/kvm-study/phase2-mem-virt/practice
./ept_violation_demo
```

**输出**：
- 完整的 10 步处理流程
- RAM 和 MMIO 的差异
- EPT 命中优化

### 练习 3: ftrace 跟踪

```bash
# 挂载 debugfs
mount -t debugfs none /sys/kernel/debug

# 设置 ftrace
echo function > /sys/kernel/debug/tracing/current_tracer
echo kvm_page_fault > /sys/kernel/debug/tracing/set_ftrace_filter
echo kvm_tdp_mmu_map >> /sys/kernel/debug/tracing/set_ftrace_filter

# 开启跟踪
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 操作虚拟机
# ...

# 查看结果
cat /sys/kernel/debug/tracing/trace
```

### 练习 4: 查看系统信息

```bash
# 查看 e820 内存映射
cat /proc/iomem

# 查看 PCI 设备 BAR
lspci -vvv | grep -A 5 "Region"

# 查看 MTRR 设置
cat /proc/mtrr

# 查看 KVM 统计
cat /sys/kernel/debug/kvm/vcpu_stat
```

---

## 下一步学习

### 关卡 4: 脏页跟踪与热迁移 ✅ 已完成

**学习内容**：
- 脏页日志（Dirty Log）机制
- 写保护（Write Protection）
- 硬件 A/D 位与 PML（Page Modification Logging）
- 热迁移（Live Migration）流程
- 增量同步优化

**关键函数**：
- `kvm_mmu_slot_remove_write_access()`
- `kvm_vm_ioctl_get_dirty_log()`
- `mark_page_dirty_in_slot()`
- `vmx_flush_pml_buffer()`
- `spte_clear_dirty()`
- `spte_write_protect()`

**文档**：
- `ept-violation-handling.md` - 第 8 章：脏页跟踪与写保护机制

### 关卡 5: TDP MMU 并发机制 ✅ 已完成

**学习内容**：
- RCU 保护机制
- 原子操作（cmpxchg）
- 并发页表更新
- 快速页错误路径（Fast Page Fault）
- 性能优化策略

**关键函数**：
- `tdp_mmu_set_spte_atomic()`
- `try_cmpxchg64()` 原子操作
- `fast_page_fault()`
- `fast_pf_fix_direct_spte()`
- `rcu_read_lock()` / `rcu_read_unlock()`

**文档**：
- `tdp-mmu-concurrency.md` - TDP MMU 并发机制详解

---

## 源码阅读路线

### 推荐顺序

```
Step 1: 数据结构基础
├── arch/x86/kvm/mmu/spte.h        ← SPTE 位定义
├── arch/x86/kvm/mmu/mmu_internal.h ← 内部接口
└── arch/x86/kvm/mmu/tdp_iter.h    ← TDP 迭代器

Step 2: 缺页处理入口
├── arch/x86/kvm/mmu/mmu.c         ← kvm_handle_page_fault
│                                     kvm_tdp_page_fault
└── arch/x86/kvm/vmx/vmx.c         ← vmx_handle_exit

Step 3: TDP MMU 核心
├── arch/x86/kvm/mmu/tdp_mmu.c     ← kvm_tdp_mmu_map()
│                                     tdp_mmu_map_handle_target_level()
└── arch/x86/kvm/mmu/tdp_iter.c    ← 页表遍历

Step 4: 页面分配与回收
├── arch/x86/kvm/mmu/mmu.c         ← kvm_mmu_get_page()
└── arch/x86/kvm/mmu/tdp_mmu.c     ← root 管理
```

### 阅读技巧

1. **先读头文件**：`spte.h` 和 `mmu_internal.h` 定义了数据结构和常量
2. **跟踪调用链**：从 `kvm_handle_page_fault` 开始，跟踪每个函数调用
3. **关注原子操作**：TDP MMU 支持并发，很多 SPTE 更新使用 `cmpxchg`
4. **区分角色**：始终区分 "Guest 看到的页表" 和 "KVM 维护的 EPT"

---

## 关键文档索引

| 文档 | 路径 | 内容 |
|------|------|------|
| Phase 2 README | `phase2-mem-virt/README.md` | EPT 硬件原理、SPTE 格式、源码路线 |
| MMIO 识别机制 | `phase2-mem-virt/mmio-identification.md` | MMIO 识别、GPU BAR 处理 |
| EPT Violation 处理 | `phase2-mem-virt/ept-violation-handling.md` | 完整调用链、锁机制、性能优化 |
| 内存类型分析 | `phase2-mem-virt/practice/memtype_analysis.c` | 演示程序 |
| EPT Violation 可视化 | `phase2-mem-virt/practice/ept_violation_demo.c` | 演示程序 |

---

## 常见问题

### Q1: 为什么 SPTE 可以复用低位？

**A**: 因为 4KB 页对齐，bit 0-11 始终为 0，硬件不使用这些位，KVM 可以安全复用。

### Q2: GPU BAR 为什么被识别为 MMIO？

**A**: 
1. GPU BAR 在高地址区域（0xF0000000+）
2. 没有 struct page（pfn_valid = false）
3. e820 表标记为 reserved（不是 RAM）
4. 必须用 UC 防止缓存导致设备异常

### Q3: 如何减少 EPT Violation？

**A**:
1. 使用大页（2M/1G）减少页表层级
2. 快速路径（无锁修复权限问题）
3. 预取（提前建立映射）
4. 优化内存访问模式

### Q4: 为什么 MMIO 必须用 UC？

**A**:
1. 设备期望精确的访问时序
2. 缓存会导致设备状态不同步
3. 某些硬件会触发 Machine Check
4. 确保写操作立即反映到设备

---

## 学习成果检验

完成 Phase 2 后，你应该能够：

- [ ] 画出 GPA → HPA 的完整翻译路径
- [ ] 读懂 `spte.h` 中每个 SPTE 位定义的含义
- [ ] 用 ftrace 跟踪一次 EPT Violation 的处理过程
- [ ] 理解 TDP MMU 的根页面管理和并发机制
- [ ] 解释 MMIO 为什么必须用 UC 内存类型
- [ ] 描述 GPU BAR 如何被识别为 MMIO
- [ ] 分析 EPT Violation 的完整调用链（10 个步骤）
- [ ] 理解 mmu_lock 和 rcu_read_lock 的作用

---

## 时间规划

| 阶段 | 内容 | 预计时间 | 状态 |
|------|------|---------|------|
| 关卡 1 | EPT 硬件原理 | 1-2 天 | ✅ 已完成 |
| 关卡 2 | SPTE 格式与内存类型 | 2-3 天 | ✅ 已完成 |
| 关卡 3 | EPT Violation 处理 | 2-3 天 | ✅ 已完成 |
| 关卡 4 | 脏页跟踪与热迁移 | 2-3 天 | ✅ 已完成 |
| 关卡 5 | TDP MMU 并发机制 | 1-2 天 | ✅ 已完成 |
| **总计** | | **8-13 天** | **全部完成** |

---

## 参考资料

- Intel SDM Vol 3, Chapter 28.2: EPT Translation Mechanism
- Intel SDM Vol 3, Chapter 29: VMX Non-Root Operation
- Linux kernel source: `arch/x86/kvm/mmu/`
- Linux kernel source: `arch/x86/kvm/vmx/`
