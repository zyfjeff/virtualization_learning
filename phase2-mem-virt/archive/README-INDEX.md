# Phase 2: 内存虚拟化（EPT）- 学习导航

> 基于 Linux 6.12.93 内核源码 | 预计学习时间：1-2 周

---

## 📚 快速开始

### 学习路径

```
┌─────────────────────────────────────────────────────────────┐
│  关卡 1: EPT 硬件原理                                        │
│  ├── README.md - 第 1 章                                      │
│  └── 理解两级地址翻译：GVA → GPA → HPA                        │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│  关卡 2: SPTE 格式与内存类型                                  │
│  ├── README.md - 第 2 章                                      │
│  ├── mmio-identification.md                                   │
│  └── 理解 SPTE 位布局、内存类型、MMIO 识别                     │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│  关卡 3: EPT Violation 处理流程                               │
│  ├── ept-violation-handling.md                                │
│  └── 理解完整的 10 步处理流程                                  │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│  关卡 4: 脏页跟踪与热迁移（待完成）                           │
└─────────────────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────────────────┐
│  关卡 5: TDP MMU 并发机制（待完成）                           │
└─────────────────────────────────────────────────────────────┘
```

---

## 📖 文档索引

### 核心文档

| 文档 | 路径 | 内容 | 状态 |
|------|------|------|------|
| **Phase 2 README** | `README.md` | EPT 硬件原理、SPTE 格式、源码路线 | ✅ 完成 |
| **MMIO 识别机制** | `mmio-identification.md` | MMIO 识别、GPU BAR 处理、e820 表 | ✅ 完成 |
| **EPT Violation 处理** | `ept-violation-handling.md` | 完整调用链、锁机制、性能优化 | ✅ 完成 |
| **学习进度总结** | `progress-summary.md` | 知识点总结、下一步计划 | ✅ 完成 |
| **学习导航** | `README-INDEX.md` | 本文档 | ✅ 完成 |

### 实践程序

| 程序 | 路径 | 功能 | 运行方式 |
|------|------|------|---------|
| **内存类型分析** | `practice/memtype_analysis.c` | 演示 RAM/MMIO 内存类型差异 | `./memtype_analysis` |
| **EPT Violation 可视化** | `practice/ept_violation_demo.c` | 完整的 10 步处理流程演示 | `./ept_violation_demo` |

---

## 🎯 关卡详解

### 关卡 1: EPT 硬件原理

**学习目标**：
- 理解两级地址翻译（GVA → GPA → HPA）
- 掌握 EPT 页表结构（4 级）
- 了解 EPTP 和 EPT Violation 类型

**核心文档**：
- `README.md` - 第 1 章：EPT 硬件原理

**关键概念**：
```
Guest Virtual Address (GVA)
    ↓ (Guest Page Table)
Guest Physical Address (GPA)
    ↓ (EPT)
Host Physical Address (HPA)
```

**预计时间**：1-2 天

---

### 关卡 2: SPTE 格式与内存类型

**学习目标**：
- 掌握 SPTE 位布局（硬件位 + 软件位）
- 理解内存类型决策（WB vs UC）
- 了解 MMIO 识别机制（三层检查）
- 掌握 GPU BAR 的处理流程

**核心文档**：
- `README.md` - 第 2 章：SPTE 格式详解
- `mmio-identification.md` - MMIO 识别与处理机制

**关键概念**：
```c
vmx_get_mt_mask(vcpu, gfn, is_mmio)
    ├─ MMIO → UC (Uncacheable)
    └─ RAM  → WB (Write-Back)
```

**实践程序**：
```bash
cd practice
./memtype_analysis
```

**预计时间**：2-3 天

---

### 关卡 3: EPT Violation 处理流程

**学习目标**：
- 掌握完整的 10 步处理流程
- 理解锁机制（mmu_lock + rcu_read_lock）
- 了解性能优化（快速路径、大页、预取）
- 能够用 ftrace 跟踪页错误

**核心文档**：
- `ept-violation-handling.md` - EPT Violation 处理流程详解

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

**实践程序**：
```bash
cd practice
./ept_violation_demo
```

**预计时间**：2-3 天

---

## 🔍 源码阅读路线

### 推荐阅读顺序

```
Step 1: 数据结构基础
├── arch/x86/kvm/mmu/spte.h
├── arch/x86/kvm/mmu/mmu_internal.h
└── arch/x86/kvm/mmu/tdp_iter.h

Step 2: 缺页处理入口
├── arch/x86/kvm/mmu/mmu.c
│   ├── kvm_handle_page_fault()
│   └── kvm_tdp_page_fault()
└── arch/x86/kvm/vmx/vmx.c
    └── handle_ept_violation()

Step 3: TDP MMU 核心
├── arch/x86/kvm/mmu/tdp_mmu.c
│   ├── kvm_tdp_mmu_map()
│   └── tdp_mmu_map_handle_target_level()
└── arch/x86/kvm/mmu/tdp_iter.c

Step 4: 页面分配与回收
├── arch/x86/kvm/mmu/mmu.c
│   └── kvm_mmu_get_page()
└── arch/x86/kvm/mmu/tdp_mmu.c
    └── root 管理
```

---

## 🛠️ 实践练习

### 练习 1: 内存类型分析

```bash
cd phase2-mem-virt/practice
gcc -o memtype_analysis memtype_analysis.c
./memtype_analysis
```

**输出示例**：
```
场景 1: 普通 RAM (GPA = 0x10000)
  → 内存类型: WB (Write-Back)
  → 值: 0x30 (bit 3-5 = 110)

场景 2: GPU BAR (GPA = 0xF0000000)
  → 内存类型: UC (Uncacheable)
  → 值: 0x00 (bit 3-5 = 000)
```

### 练习 2: EPT Violation 可视化

```bash
cd phase2-mem-virt/practice
gcc -o ept_violation_demo ept_violation_demo.c
./ept_violation_demo
```

**输出示例**：
```
Step 1: 硬件触发 VM-Exit
  EXIT_REASON = 48 (EPT_VIOLATION)
  GUEST_PHYSICAL_ADDRESS = 0x10000

Step 2: handle_ept_violation()
  从 VMCS 读取 GPA: 0x10000

...

Step 9: tdp_mmu_map_handle_target_level()
  调用 make_spte() 构建 SPTE:
    SPTE = 0x10037
      - Present: 是
      - Writable: 是
      - Memory Type: WB (Write-Back)
```

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

## 📊 性能数据

### EPT Violation 处理延迟

| 场景 | 延迟 | 说明 |
|------|------|------|
| VM-Exit + 页表建立 | 2-5 μs | 首次访问 |
| 快速路径（无锁修复） | ~200 ns | 权限问题 |
| EPT 命中 | ~50 ns | 缓存命中 |

### 内存访问延迟

| 内存类型 | 延迟 | 适用场景 |
|---------|------|---------|
| WB (Write-Back) | ~1 ns（缓存命中） | 普通 RAM |
| UC (Uncacheable) | ~100-500 ns | MMIO 设备 |

**性能差距**：UC 比 WB 慢 100-500 倍！

---

## ✅ 学习成果检验

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

## 🎓 关键知识点速查

### SPTE 位布局

```
  63 62-12  11  10  9  8  7  6  5  4  3  2  1  0
 ┌───┬─────┬───┬───┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
 │HW │ PFN │ X │ W │ R│IG│MM│MM│MM│MM│PC│A │D │P │
 └───┴─────┴───┴───┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
```

### 内存类型编码

| 值 | 类型 | 说明 |
|---|------|------|
| 000 | UC | Uncacheable（MMIO） |
| 001 | WC | Write Combining |
| 100 | WT | Write Through |
| 101 | WP | Write Protect |
| 110 | WB | Write-Back（RAM） |

### MMIO 识别流程

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

---

## 🔗 下一步

完成 Phase 2 后，继续学习：

### 关卡 4: 脏页跟踪与热迁移

**待学习内容**：
- 脏页日志（Dirty Log）机制
- 写保护（Write Protection）
- 热迁移（Live Migration）流程
- 增量同步优化

### 关卡 5: TDP MMU 并发机制

**待学习内容**：
- RCU 保护机制
- 原子操作（cmpxchg）
- 并发页表更新
- 性能优化策略

---

## 📚 参考资料

- Intel SDM Vol 3, Chapter 28.2: EPT Translation Mechanism
- Intel SDM Vol 3, Chapter 29: VMX Non-Root Operation
- Linux kernel source: `arch/x86/kvm/mmu/`
- Linux kernel source: `arch/x86/kvm/vmx/`
- 论文: *"A VMM-Based Performance Analysis Tool for Memory Virtualization"*

---

## 💡 学习建议

1. **先读文档**：按顺序阅读核心文档，理解基本概念
2. **运行实践**：编译运行演示程序，直观理解流程
3. **阅读源码**：按照推荐的源码路线，深入理解实现
4. **跟踪调试**：使用 ftrace 跟踪实际运行，验证理解
5. **总结归纳**：每完成一个关卡，回顾知识点，确保掌握

---

## 📞 问题与讨论

如果遇到问题：

1. **查看文档**：先查阅相关文档和代码注释
2. **运行演示**：通过演示程序直观理解
3. **阅读源码**：深入源码查找答案
4. **实践验证**：通过 ftrace 和调试工具验证

---

**祝学习顺利！** 🚀
