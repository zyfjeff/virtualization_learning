# 第二阶段：内存虚拟化（EPT）

> 基于 Linux 6.12.93 内核源码 | 预计学习时间：1-2 周

---

## 📋 学习目标

本阶段聚焦 KVM 内存虚拟化的核心机制——**扩展页表（Extended Page Table, EPT）**。
你将理解 Guest 物理地址（GPA）如何翻译为宿主机物理地址（HPA），以及 KVM 如何
通过软件维护的 SPTE（Shadow Page Table Entry）来管理 EPT 硬件。

完成本阶段后，你应该能够：
1. 画出 GPA → HPA 的完整翻译路径
2. 读懂 `spte.h` 中每个 SPTE 位定义的含义
3. 用 ftrace 跟踪一次 EPT Violation 的处理过程
4. 理解 TDP MMU 的根页面管理和并发机制

---

## 🏗️ EPT 硬件原理

### 1.1 两级地址翻译

EPT 实现了 **GPA → HPA** 的硬件翻译，与 Guest 自身的 **GVA → GPA** 页表协作，
形成完整的二级地址翻译链：

```
┌──────────────────────────────────────────────────────────────────┐
│                    CPU 地址翻译流程                                │
│                                                                  │
│  Guest Virtual Address (GVA)                                     │
│         │                                                        │
│         ▼                                                        │
│  ┌─────────────┐     Guest CR3                                   │
│  │ Guest Page  │ ──(walk)──▶ GPA (Guest Physical Address)        │
│  │  Table      │                                                  │
│  └─────────────┘         │                                       │
│                          ▼                                       │
│                  ┌─────────────┐     EPTP (VMCS)                 │
│                  │    EPT      │ ──(walk)──▶ HPA                 │
│                  │ (4-level)   │         (Host Physical Address)  │
│                  └─────────────┘                                  │
│                                                                  │
│  总计：4-level Guest PT × 4-level EPT = 最多 24 次内存访问       │
│  （不使用大页的情况下）                                            │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 EPT Violation 类型

当 EPT walk 失败时，硬件触发 **VM-Exit**，退出原因码为 `EPT Violation (48)`：

| 场景 | 触发条件 | KVM 处理方式 |
|------|----------|-------------|
| **缺页（Missing）** | EPT 叶条目 Present=0 | 分配物理页，建立 SPTE |
| **权限违规** | 访问类型与权限位不匹配 | 检查是否为脏页/访问位更新 |
| **Misconfiguration** | EPT 条目包含保留位组合 | KVM BUG，通常 panic |
| **MMIO 访问** | GPA 映射到设备 MMIO 区域 | 交由设备模拟处理 |

### 1.3 EPTP（EPT Pointer）

VMCS 中的 EPTP 寄存器指向 EPT 根页面：

```
  63    52  51  48 47   12 11  7 6   3 2 0
 ┌──────┬─────┬─────┬──────┬─────┬───┬───┐
 │ 预留 │ WBT │ 预留 │ GPA  │ 预留 │Ad │ 4 │
 │      │     │     │ 偏移 │     │/D │ 级 │
 └──────┴─────┴─────┴──────┴─────┴───┴───┘
                                        │
                                        └─ 0=4级, 1=5级(5-level paging)
                                   Ad/D ── 1=启用 Accessed/Dirty 位跟踪
```

---

## 📐 SPTE 格式详解

KVM 使用 **SPTE（Shadow Page Table Entry）** 来同时表示 EPT 条目和软件元信息。
每个 SPTE 是一个 64 位的值，既包含硬件 EPT 需要的信息，也包含 KVM 软件跟踪
所需的状态位。

### 2.1 SPTE 位布局图

以下是 KVM SPTE 的关键位定义（基于 `arch/x86/kvm/mmu/spte.h`）：

```
  63 62-12  11  10  9  8  7  6  5  4  3  2  1  0
 ┌───┬─────┬───┬───┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
 │HW │ PFN │ X │ W │ R│IG│MM│MM│MM│MM│PC│A │D │P │
 │大 │     │ │ │ │ │U │U │U │U │D │D │E │C │I │R │
 │页 │     │ │ │ │ │X │W │R │X │I │R ││C │C │T │E │
 │位 │     │ │ │ │ │ │ │ │ │R │T │Y ││S │S │Y │S │
 │   │     │ │ │ │ │ │ │ │ │T ││ │││S │S ││ │E │
 │   │     │ │ │ │ │ │ │ │ │Y ││ ││││ │││ │ │N │
 │   │     │ │ │ │ │ │ │ │ ││ ││ ││││ │││ │ │T │
 └───┴─────┴───┴───┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
  │    │    │   │  │  │  │  │  │  │  │  │  │  │  │
  │    │    │   │  │  │  │  │  │  │  │  │  │  │  └─ Present (有效位)
  │    │    │   │  │  │  │  │  │  │  │  │  │  └──── Writable (可写)
  │    │    │   │  │  │  │  │  │  │  │  │  └─────── Executable (用户态可执行)
  │    │    │   │  │  │  │  │  │  │  │  └────────── Dirty (脏页)
  │    │    │   │  │  │  │  │  │  │  └───────────── Accessed (已访问)
  │    │    │   │  │  │  │  │  │  └──────────────── PCE (PCID 相关)
  │    │    │   │  │  │  │  │  └─────────────────── MMU-present (软件存在位)
  │    │    │   │  │  │  │  └────────────────────── MMU-writable
  │    │    │   │  │  │  └───────────────────────── MMU-executable (用户)
  │    │    │   │  │  └──────────────────────────── MMU-executable (管理)
  │    │    │   │  └─────────────────────────────── MMU-RWX 权限掩码
  │    │    │   └────────────────────────────────── Writable (EPT 写权限)
  │    │    └────────────────────────────────────── Readable (EPT 读权限)
  │    └─────────────────────────────────────────── PFN (物理页帧号, bit12-51)
  └──────────────────────────────────────────────── 大页位
```

### 2.2 关键宏定义速查

以下是 `spte.h` 中的核心宏（实际值可能因内核配置略有不同）：

```c
/* 来源: arch/x86/kvm/mmu/spte.h */

/* 基础权限位 */
#define PT_PRESENT_MASK         (1ULL << 0)
#define PT_WRITABLE_MASK        (1ULL << 1)
#define PT_USER_MASK            (1ULL << 2)
#define PT_ACCESSED_MASK        (1ULL << 5)
#define PT_DIRTY_MASK           (1ULL << 6)
#define PT_PAGE_SIZE_MASK       (1ULL << 7)
#define PT64_NX_MASK            (1ULL << 63)

/* EPT 特有权限位 */
#define VMX_EPT_READABLE_MASK   (1ULL << 0)
#define VMX_EPT_WRITABLE_MASK   (1ULL << 1)
#define VMX_EPT_EXECUTABLE_MASK (1ULL << 2)

/* SPTE 软件位 - 用于 KVM 内部状态跟踪 */
#define SPTE_MMU_PRESENT_MASK   (1ULL << 62)
#define SPTE_MMU_WRITEABLE_MASK (1ULL << 1)   /* 与 EPT writable 共用 */

/* Accessed/Dirty 位（通过软件模拟）*/
#define SPTE_TDP_ACCESSED_MASK   /* 软件实现的访问位 */
#define SPTE_TDP_DIRTY_MASK      /* 软件实现的脏页位 */
```

### 2.3 权限分离：硬件位 vs 软件位

KVM 巧妙地在 SPTE 中混合了硬件和软件信息：

```
┌──────────────────────────────────────────────────────────┐
│              SPTE 权限分离模型                            │
│                                                          │
│  硬件看到的 SPTE（传给 EPT walker）:                      │
│  ┌────────────────────────────────────────────┐          │
│  │ Present | Read | Write | Execute | PFN ... │          │
│  └────────────────────────────────────────────┘          │
│                                                          │
│  软件看到的 SPTE（KVM 内部管理）:                          │
│  ┌────────────────────────────────────────────┐          │
│  │ MMU-present | MMU-writable | A/D bits      │          │
│  │ | shadow-present | locked | ...            │          │
│  └────────────────────────────────────────────┘          │
│                                                          │
│  关键点: 当 KVM 需要使页面不可写时:                        │
│    1. 清除硬件 Writable 位 → EPT 阻止写入                │
│    2. 写入触发 EPT Violation → KVM 处理脏页日志          │
│    3. 重新设置 Writable 位 → 恢复正常写入                │
└──────────────────────────────────────────────────────────┘
```

### 2.4 内存类型处理（Memory Type）

EPT SPTE 中需要显式设置内存类型，因为 **EPT 覆盖 host MTRR**。

#### 2.4.1 内存类型位布局

```
  63  62  61  ...  7  6  5  4  3  2  1  0
 ┌───┬───┬───┬───┬───┬──┬──┬──┬──┬──┬──┬──┐
 │...│...│...│...│...│IP│MT │MT │MT │W │R │
 │   │   │   │   │   │AT│ 2 │ 1 │ 0 │  │  │
 └───┴───┴───┴───┴───┴──┴──┴──┴──┴──┴──┴──┘
                        │  │  │
                        └──┴──┴── 内存类型 (bit 3-5)
                             │
                             └──── Ignore PAT (bit 6)
```

**内存类型编码**（bit 3-5，基于 MTRR）：
- `000` (0) = UC (Uncacheable)
- `001` (1) = WC (Write Combining)
- `010` (2) = 保留
- `011` (3) = 保留
- `100` (4) = WT (Write Through)
- `101` (5) = WP (Write Protect)
- `110` (6) = WB (Write Back)

#### 2.4.2 内存类型的动态决策

**关键代码**：`vmx_get_mt_mask()` in `arch/x86/kvm/vmx/vmx.c:7679`

```c
u8 vmx_get_mt_mask(struct kvm_vcpu *vcpu, gfn_t gfn, bool is_mmio)
{
    /*
     * 强制 UC 用于 host MMIO 区域
     * 如果允许 guest 用可缓存方式访问 MMIO，会导致 Machine Check
     */
    if (is_mmio)
        return MTRR_TYPE_UNCACHABLE << VMX_EPT_MT_EPTE_SHIFT;
        // 0 << 3 = 0x00 (UC)

    /* 如果忽略 guest PAT，强制 WB */
    if (vmx_ignore_guest_pat(vcpu->kvm))
        return (MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT) | VMX_EPT_IPAT_BIT;
        // 6 << 3 = 0x30 (WB) | IPAT

    return (MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT);
    // 6 << 3 = 0x30 (WB)
}
```

**决策逻辑**：
1. **MMIO 区域**（设备内存）→ **UC (Uncacheable)**，防止缓存导致设备行为异常
2. **普通 RAM** → **WB (Write-Back)**，最大化缓存性能
3. **IPAT 位**：控制是否忽略 guest PAT（通常不需要）

#### 2.4.3 MMIO 检测机制

**关键代码**：`kvm_is_mmio_pfn()` in `arch/x86/kvm/mmu/spte.c:108`

```c
static bool kvm_is_mmio_pfn(kvm_pfn_t pfn)
{
    if (pfn_valid(pfn))
        return !is_zero_pfn(pfn) && PageReserved(pfn_to_page(pfn)) &&
            /*
             * Some reserved pages, such as those from NVDIMM
             * DAX devices, are not for MMIO, and can be mapped
             * with cached memory type for better performance.
             */
            (!pat_enabled() || pat_pfn_immune_to_uc_mtrr(pfn));

    return !e820__mapped_raw_any(pfn_to_hpa(pfn),
                                 pfn_to_hpa(pfn + 1) - 1,
                                 E820_TYPE_RAM);
}
```

**检测逻辑**：
1. **pfn_valid(pfn) 为真**（有 `struct page`）：
   - 检查是否是 `PageReserved`（保留页）
   - 检查是否是零页
   - 检查 PAT 属性
2. **pfn_valid(pfn) 为假**（无 `struct page`）：
   - 查询 e820 内存映射表
   - 如果不是 `E820_TYPE_RAM`，则判定为 MMIO

#### 2.4.4 SPTE 构建时的内存类型设置

**关键代码**：`make_spte()` in `arch/x86/kvm/mmu/spte.c:211`

```c
u64 make_spte(struct kvm_vcpu *vcpu, struct kvm_mmu_page *sp,
              unsigned int pte_access, gfn_t gfn, kvm_pfn_t pfn,
              u64 lpage_mask, bool host_writable, bool speculative,
              bool can_unsync, bool reset_host_protection, u64 *sptep)
{
    // ... 其他位设置 ...

    // 动态获取内存类型
    if (shadow_memtype_mask)
        spte |= kvm_x86_call(get_mt_mask)(vcpu, gfn,
                                          kvm_is_mmio_pfn(pfn));

    // ... PFN 和其他位 ...
}
```

**关键点**：
- `shadow_memtype_mask` 在 EPT 模式下设置为 `VMX_EPT_MT_MASK | VMX_EPT_IPAT_BIT`
- 每个页的内存类型通过 `get_mt_mask()` 动态计算
- `kvm_is_mmio_pfn(pfn)` 判断是 RAM 还是 MMIO

#### 2.4.5 EPT 与 Shadow Paging 的内存类型处理对比

| 模式 | 内存类型设置 | 依赖 |
|------|------------|------|
| **EPT** | `shadow_memtype_mask = VMX_EPT_MT_MASK \| VMX_EPT_IPAT_BIT`<br>动态计算每个页的内存类型 | 必须在 SPTE 中显式设置，EPT 覆盖 host MTRR |
| **Shadow/NPT** | `shadow_memtype_mask = 0`<br>不在 SPTE 中设置内存类型 | 依赖 host MTRR 提供正确内存类型 |

**为什么 EPT 需要显式设置？**

EPT 有自己的内存类型控制，完全独立于 host MTRR。如果不在 EPT 页表中设置内存类型，硬件会使用默认的 UC，导致所有内存访问都不可缓存，性能极差。

#### 2.4.6 为什么 MMIO 必须用 UC？

```c
// vmx_get_mt_mask() 中的注释
/*
 * Force UC for host MMIO regions, as allowing the guest to access MMIO
 * with cacheable accesses will result in Machine Checks.
 */
```

**原因**：
1. **MMIO 是设备内存**（GPU、网卡寄存器等）
2. **设备期望精确的访问时序**
3. **如果 CPU 缓存了 MMIO 访问**：
   - 设备状态更新不会立即反映
   - 写操作可能被合并或重排序
   - 导致设备行为异常
   - 某些硬件会触发 Machine Check（致命错误）

#### 2.4.7 实际案例：GPU BAR 内存注册

**场景**：QEMU 虚拟机中注册 GPU 设备的 BAR（Base Address Register）区域

**流程**：

```
1. QEMU 通过 pci_device_register() 注册 GPU 设备
   ↓
2. GPU 驱动探测 BAR 区域（通常在 0xF0000000+）
   ↓
3. QEMU 调用 ioremap() 映射 GPU BAR 到宿主虚拟地址
   - HVA = 0x7f1234560000（示例）
   - HPA = 0xF0000000（GPU BAR 物理地址）
   ↓
4. QEMU 调用 KVM_SET_USER_MEMORY_REGION
   struct kvm_userspace_memory_region region = {
       .slot = 1,                              // GPU BAR slot
       .guest_phys_addr = 0xF0000000,          // GPA（Guest 看到的地址）
       .memory_size = 256 * 1024 * 1024,       // 256MB BAR 大小
       .userspace_addr = 0x7f1234560000,       // HVA
       .flags = KVM_MEM_READONLY,              // 如果只读
   };
   ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
   ↓
5. KVM 内部创建 kvm_memory_slot
   ↓
6. Guest 访问 GPU BAR 时触发 EPT Violation
   ↓
7. kvm_is_mmio_pfn(pfn) 检查
   - pfn = 0xF0000000 >> 12 = 0xF0000
   - pfn >= 0x3C000（0xF0000000 >> 12）→ 识别为 MMIO
   ↓
8. vmx_get_mt_mask() 返回 UC (0x00)
   ↓
9. EPT 页表项设置为 UC，防止缓存
   - SPTE bit 3-5 = 000 (UC)
   - SPTE bit 12-51 = PFN (物理页帧号)
   ↓
10. Guest 访问 GPU 寄存器时：
    - 不会被缓存
    - 直接访问设备寄存器
    - 设备状态立即反映
```

**SPTE 值对比**：

```c
// RAM 的 SPTE (GPA = 0x10000)
SPTE = 0x0000010000000037
       │              │
       │              └─ bit 0-2: R|W|U + WB(110)
       └─ bit 12-51: PFN

// GPU BAR 的 SPTE (GPA = 0xF0000000)
SPTE = 0x0000F00000000005
       │              │
       │              └─ bit 0-2: R|U + UC(000)
       └─ bit 12-51: PFN
```

---

## 📖 源码阅读路线

### 推荐阅读顺序

按以下顺序阅读源码，逐步深入：

```
┌─────────────────────────────────────────────────────────────┐
│                     源码阅读路线                              │
│                                                             │
│  Step 1: 数据结构基础                                        │
│  ├── arch/x86/kvm/mmu/spte.h        ← SPTE 位定义          │
│  ├── arch/x86/kvm/mmu/mmu_internal.h ← 内部接口             │
│  └── arch/x86/kvm/mmu/tdp_iter.h    ← TDP 迭代器           │
│                                                             │
│  Step 2: 缺页处理入口                                        │
│  ├── arch/x86/kvm/mmu/mmu.c         ← kvm_handle_page_fault │
│  │                                     kvm_tdp_page_fault   │
│  └── arch/x86/kvm/vmx/vmx.c         ← vmx_handle_exit      │
│                                                             │
│  Step 3: TDP MMU 核心                                        │
│  ├── arch/x86/kvm/mmu/tdp_mmu.c     ← kvm_tdp_mmu_map()    │
│  │                                     kvm_tdp_mmu_zap_spte │
│  └── arch/x86/kvm/mmu/tdp_iter.c    ← 页表遍历             │
│                                                             │
│  Step 4: 页面分配与回收                                      │
│  ├── arch/x86/kvm/mmu/mmu.c         ← kvm_mmu_get_page()   │
│  └── arch/x86/kvm/mmu/tdp_mmu.c     ← root 管理            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 关键函数索引

| 函数名 | 文件 | 作用 |
|--------|------|------|
| `kvm_handle_page_fault()` | `mmu/mmu.c` | 顶层缺页处理入口 |
| `kvm_tdp_page_fault()` | `mmu/mmu.c` | TDP 模式缺页处理 |
| `kvm_tdp_mmu_map()` | `mmu/tdp_mmu.c` | 建立 EPT 映射 |
| `kvm_tdp_mmu_get_root()` | `mmu/tdp_mmu.c` | 获取 TDP MMU 根 |
| `kvm_mmu_get_tdp_root()` | `mmu/tdp_mmu.c` | 获取/创建 TDP 根页面 |
| `tdp_mmu_set_spte_atomic()` | `mmu/tdp_mmu.c` | 原子设置 SPTE |
| `make_spte()` | `mmu/spte.c` | 构造 SPTE 值（包含内存类型设置） |
| `vmx_get_mt_mask()` | `vmx/vmx.c` | **动态计算内存类型**（UC/WB） |
| `kvm_is_mmio_pfn()` | `mmu/spte.c` | **判断 PFN 是否是 MMIO** |
| `kvm_mmu_set_ept_masks()` | `mmu/spte.c` | 设置 EPT 位掩码（不含内存类型） |
| `kvm_mmu_page_fault()` | `mmu/mmu.c` | vCPU 缺页入口 |

### 阅读技巧

1. **先读头文件**：`spte.h` 和 `mmu_internal.h` 定义了数据结构和常量，是理解后续代码的基础
2. **跟踪调用链**：从 `kvm_handle_page_fault` 开始，用 cscope/ctags 跟踪每个函数调用
3. **关注原子操作**：TDP MMU 支持并发，很多 SPTE 更新使用 `cmpxchg`
4. **区分角色**：始终区分 "Guest 看到的页表" 和 "KVM 维护的 EPT"

---

## 🔬 实践练习

### 练习 1：使用 ftrace 跟踪 EPT 缺页

```bash
# 1. 挂载 debugfs（如果未挂载）
mount -t debugfs none /sys/kernel/debug

# 2. 设置 ftrace 跟踪 KVM 缺页
echo function > /sys/kernel/debug/tracing/current_tracer
echo kvm_page_fault > /sys/kernel/debug/tracing/set_ftrace_filter
echo kvm_mmu_get_page >> /sys/kernel/debug/tracing/set_ftrace_filter
echo kvm_tdp_mmu_map >> /sys/kernel/debug/tracing/set_ftrace_filter

# 3. 开启跟踪
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 4. 启动/操作虚拟机
# ... 在虚拟机内执行内存密集操作 ...

# 5. 关闭跟踪并查看结果
echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace
```

### 练习 2：使用 tracepoint 精确跟踪

```bash
# 使用 KVM 提供的 tracepoint（比 function tracer 更精确）
echo kvm_entry > /sys/kernel/debug/tracing/set_event
echo kvm_exit >> /sys/kernel/debug/tracing/set_event
echo kvm_page_fault >> /sys/kernel/debug/tracing/set_event
echo kvm_mmu_paging_element >> /sys/kernel/debug/tracing/set_event

# 也可以使用 perf
perf record -e kvm:kvm_page_fault -a -g -- sleep 10
perf report
```

### 练习 3：分析 SPTE 值

```bash
# 通过 debugfs 查看 vCPU 的页表状态
# （需要 CONFIG_KVM_EXTERNAL_DEBUGGER 或自定义模块）

# 方法: 使用 gdb 附加到 QEMU 进程，查看 KVM 内部结构
gdb -p <qemu_pid>
(gdb) p *(struct kvm_vcpu_arch *)0x...
(gdb) p *(struct kvm_mmu *)0x...
```

### 练习 4：EPT 大页分析

```bash
# 检查虚拟机是否使用 2MB 大页
# 1. 在 Guest 中使用 aligned 分配
# 2. 在 Host 上使用 ftrace 观察是否出现 2MB 映射

echo kvm_mmu_set_spte >> /sys/kernel/debug/tracing/set_ftrace_filter

# 观察 SPTE 中的 PT_PAGE_SIZE_MASK (bit 7) 是否被设置
# 如果设置了，说明创建了 2MB 大页映射
```

### 练习 5：内存压力测试

```bash
# 目标：观察 EPT 缺页处理在不同内存压力下的性能

# Host 端准备
sysctl vm.min_free_kbytes=65536

# Guest 内运行内存压力工具
stress-ng --vm 4 --vm-bytes 2G --vm-method all

# Host 端监控
watch -n 1 'cat /proc/vmstat | grep -E "kvm|pgfault"'
```

---

## 📚 参考资料

- Intel SDM Vol 3, Chapter 29: VMX Non-Root Operation（EPT 硬件细节）
- Intel SDM Vol 3, Chapter 28.2: EPT Translation Mechanism
- Linux kernel source: `Documentation/virt/kvm/`
- 论文: *"A VMM-Based Performance Analysis Tool for Memory Virtualization"*

---

## ✅ 阶段检验清单

完成本阶段后，确认你能回答以下问题：

- [ ] EPT Violation 的三种主要类型是什么？KVM 如何分别处理？
- [ ] SPTE 中 Present=0 但 MMU-present=1 表示什么状态？
- [ ] `kvm_tdp_mmu_map()` 中，为什么 SPTE 写入要用 `cmpxchg` 而不是直接写？
- [ ] TDP MMU 的根页面如何管理？为什么需要引用计数？
- [ ] 当 Guest 执行 `mmap` 然后首次访问时，完整的 EPT 建立路径是什么？
- [ ] Accessed/Dirty 位是如何在软件层面模拟的？

---

## 🔍 VMM视角对比

### 用户态VMM vs KVM内核态内存管理

| 方面 | 用户态VMM (QEMU) | KVM内核态 |
|------|------------------|-----------|
| **内存分配** | malloc/mmap分配Host内存 | 通过memslot映射Guest内存 |
| **EPT管理** | 无法直接操作 | 直接管理EPT页表（4级结构） |
| **页错误处理** | 无法参与 | 内核态直接处理EPT Violation |
| **并发支持** | 无 | 多vCPU并发处理页错误（cmpxchg） |
| **大页支持** | 需要显式配置 | 自动检测并使用2MB/1GB大页 |
| **脏页跟踪** | 无法实现 | 通过SPTE软件位自动跟踪 |

### 关键差异：页错误处理

```
用户态VMM:
  Guest访问GPA → EPT缺失 → VM-Exit → 返回用户态 → 无法处理

KVM内核态:
  Guest访问GPA → EPT缺失 → VM-Exit → 内核态处理 → 建立EPT映射 → 重新进入Guest
  支持并发、大页、脏页跟踪
```

### 为什么KVM要直接管理EPT？

1. **性能**：避免返回用户态的开销（每次页错误节省~2μs）
2. **并发**：多个vCPU可以同时处理不同GPA的页错误
3. **高级特性**：支持大页映射、脏页跟踪、EPT A/D位

---

## ⚡ 性能优化技术

### 1. 大页映射 (Huge Pages)

**问题**：4KB页表导致EPT TLB miss率高

**解决**：使用2MB或1GB大页，减少EPT页表层级

```c
/* kvm_tdp_page_fault() 中检测大页对齐 */
if (fault->gfn % 512 == 0 && max_level >= PG_LEVEL_2M) {
    /* 使用2MB大页 */
    level = PG_LEVEL_2M;
} else if (fault->gfn % (512 * 512) == 0 && max_level >= PG_LEVEL_1G) {
    /* 使用1GB大页 */
    level = PG_LEVEL_1G;
}
```

**配置**：
```bash
# Host配置大页
echo 1024 > /proc/sys/vm/nr_hugepages  # 2MB大页
echo 4 > /proc/sys/vm/nr_1g_hugepages  # 1GB大页 (需要支持)

# QEMU使用大页
qemu-system-x86_64 -mem-path /dev/hugepages -mem-prealloc ...
```

**效果**：
- EPT TLB命中率提升
- 性能提升10-20%
- 内存带宽提升（减少页表遍历）

### 2. EPT A/D位 (Accessed/Dirty)

**问题**：软件模拟A/D位需要额外的VM-Exit

**解决**：使用硬件A/D位，自动跟踪访问和脏页

```c
/* vmx_hardware_setup() 中检测 */
if (!cpu_has_vmx_ept_ad_bits() || !enable_ept)
    enable_ept_ad_bits = 0;
```

**配置**：
```bash
# 查看是否启用
cat /sys/module/kvm_intel/parameters/eptad
# 输出: Y (启用) 或 N (禁用)
```

**效果**：
- 减少VM-Exit次数（脏页跟踪无需软件干预）
- 热迁移性能提升20-30%

### 3. MMIO缓存

**问题**：MMIO访问每次都触发VM-Exit

**解决**：缓存MMIO映射，减少VM-Exit

```c
/* kvm_mmu_set_mmio_spte_mask() 中启用 */
if (mmio_caching) {
    /* 缓存MMIO区域的SPTE */
    kvm_mmu_set_mmio_spte_mask(mask, value);
}
```

**配置**：
```bash
# 查看是否启用
cat /sys/module/kvm/parameters/mmio_caching
# 输出: Y (启用) 或 N (禁用)
```

**效果**：
- MMIO密集负载性能提升20%
- 减少VM-Exit次数

### 4. 脏页日志优化

**问题**：脏页日志导致所有写操作触发VM-Exit

**解决**：批量记录脏页，减少VM-Exit次数

```c
/* kvm_mmu_slot_remove_write_access() 中优化 */
/* 清除memslot的可写位，写操作触发EPT Violation */
for (gfn = slot->base_gfn; gfn < slot->base_gfn + slot->npages; gfn++) {
    /* 记录脏页 */
    mark_page_dirty(kvm, gfn);
}
```

**效果**：
- 热迁移首次迭代：性能下降50%（预期）
- 后续迭代：性能恢复正常

---

## ⚠️ 常见陷阱

### 陷阱1：memslot配置错误

**场景**：Guest访问GPA时崩溃或数据损坏

**症状**：EPT Violation频繁，或访问错误的HPA

**原因**：memslot的GPA/HVA/大小不匹配

**解决**：
```c
// 正确的memslot配置
struct kvm_userspace_memory_region region = {
    .slot = 0,
    .guest_phys_addr = 0x0,        // GPA起始
    .memory_size = 0x40000000,     // 1GB
    .userspace_addr = (uintptr_t)host_mem,  // HVA
    .flags = 0,                     // 0=可读写
};
ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
```

**检查**：
```bash
# 查看memslot信息
cat /sys/kernel/debug/kvm/<vm_id>/memslots
```

### 陷阱2：EPT页表并发冲突

**场景**：多个vCPU同时访问同一GPA，导致EPT页表损坏

**症状**：Guest访问错误的数据，或内核panic

**原因**：EPT页表更新未使用原子操作

**解决**：
```c
// kvm_tdp_mmu_map() 中使用cmpxchg
old_spte = *sptep;
new_spte = make_spte(...);
if (cmpxchg(sptep, old_spte, new_spte) == old_spte) {
    // 成功更新
} else {
    // 被其他vCPU抢先修改，重试
    return RET_PF_RETRY;
}
```

**关键**：必须使用`cmpxchg`保证原子性

### 陷阱3：大页对齐错误

**场景**：使用大页但GPA未对齐

**症状**：EPT Violation频繁，性能差

**原因**：GPA未对齐到2MB或1GB边界

**解决**：
```bash
# 确保GPA对齐
# 2MB大页: GPA % 0x200000 == 0
# 1GB大页: GPA % 0x40000000 == 0

# QEMU启动参数
qemu-system-x86_64 -mem-prealloc -mem-path /dev/hugepages ...
# QEMU会自动对齐
```

**检查**：
```bash
# 查看是否使用大页
grep -i huge /proc/meminfo
# 如果HugePages_Free减少，说明使用了大页
```

### 陷阱4：脏页日志未清理

**场景**：热迁移完成后，Guest性能下降

**症状**：每次写入都触发VM-Exit

**原因**：脏页日志未清理，所有页仍处于只读状态

**解决**：
```c
// 清理脏页日志
ioctl(vm_fd, KVM_CLEAR_DIRTY_LOG, &clear_log);
// 恢复页的可写位
kvm_mmu_slot_set_write_access(kvm, slot);
```

**检查**：
```bash
# 查看脏页数量
cat /sys/kernel/debug/kvm/<vm_id>/dirty_pages
```
