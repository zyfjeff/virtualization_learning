# 第二阶段源码注释：EPT 内存虚拟化

> 基于 Linux 6.12.93 源码 | 对应源码树 `arch/x86/kvm/mmu/`

---

## 1. SPTE 位定义（spte.h）

`arch/x86/kvm/mmu/spte.h` 是内存虚拟化的核心头文件，定义了 SPTE（Shadow Page Table Entry）
的位布局。KVM 使用 SPTE 来编程 EPT 硬件页表。

### 1.1 基础页表位定义

```c
/* 来源: arch/x86/kvm/mmu/spte.h */

/*
 * 基础 x86 页表位 —— 同时适用于 Guest PT 和 EPT
 * 这些位直接映射到 Intel SDM 中定义的页表条目位
 */
#define PT_PRESENT_MASK         (1ULL << 0)     /* 存在位 */
#define PT_WRITABLE_MASK        (1ULL << 1)     /* 可写位 */
#define PT_USER_MASK            (1ULL << 2)     /* 用户态位 */
#define PT_ACCESSED_MASK        (1ULL << 5)     /* 已访问位 */
#define PT_DIRTY_MASK           (1ULL << 6)     /* 脏页位 */
#define PT_PAGE_SIZE_MASK       (1ULL << 7)     /* 大页位 (2MB/1GB) */
#define PT_GLOBAL_MASK          (1ULL << 8)     /* 全局页位 */
#define PT64_NX_MASK            (1ULL << 63)    /* No-Execute 位 */

/* 权限掩码：R/W/X 组合 */
#define PT_RWX_MASK             (PT_WRITABLE_MASK | PT_USER_MASK | PT64_NX_MASK)
```

### 1.2 EPT 专用位定义

```c
/* 来源: arch/x86/kvm/mmu/spte.h */

/*
 * EPT 位定义 —— Intel VT-x 扩展页表专用
 * EPT 的权限位与标准页表不同：
 *   bit 0 = Read
 *   bit 1 = Write
 *   bit 2 = Execute (for supervisor-mode linear addresses)
 *   bit 10 = Execute (for user-mode linear addresses, 如果支持)
 */
#define VMX_EPT_READABLE_MASK           (1ULL << 0)
#define VMX_EPT_WRITABLE_MASK           (1ULL << 1)
#define VMX_EPT_EXECUTABLE_MASK         (1ULL << 2)
#define VMX_EPT_SUPPRESS_VE_BIT         (1ULL << 63)

/* EPT 内存类型编码 (Memory Type, MT) */
#define VMX_EPT_MT_UNCACHABLE           0       /* UC - 不可缓存 */
#define VMX_EPT_MT_WRITECOMBINING       1       /* WC - 写合并 */
#define VMX_EPT_MT_WRITETHROUGH         4       /* WT - 直写 */
#define VMX_EPT_MT_WRITEPROTECTED       5       /* WP - 写保护 */
#define VMX_EPT_MT_WRITEBACK            6       /* WB - 回写 (最常用) */
#define VMX_EPT_MT_MASK                 (7ULL << 3) /* MT 字段掩码 */
```

### 1.3 SPTE 完整位布局图

```
KVM SPTE 64-bit 位布局（EPT 模式）:

  63       52 51           12 11  10  9  8  7   6   5   4  3  2  1  0
 ┌──────────┬───────────────┬───┬───┬──┬──┬───┬───┬───┬──┬──┬──┬──┬──┐
 │ NX/特殊位 │    PFN        │SW1│SW2│IG│IG│PS │ IG│ A │ D │IG│ W│ R│ P│
 │(bit63)   │(物理页帧号)   │   │   │  │  │   │   │   │   │  │  │  │  │
 └──────────┴───────────────┴───┴───┴──┴──┴───┴───┴───┴──┴──┴──┴──┴──┘

字段说明:
  P     [bit 0]  Present: 页表条目有效
  R     [bit 1]  Readable / Writable (EPT 写权限)
  W     [bit 2]  Execute / User (EPT 用户态执行权限)
  D     [bit 5]  Accessed (通过 MMU-writable 机制模拟)
  A     [bit 6]  Dirty (通过软件位跟踪)
  PS    [bit 7]  Page Size: 1=大页(2MB/1GB), 0=4KB页
  PFN   [bit 12-51] 物理页帧号: 实际物理地址 = PFN << 12
  SW1   [bit 10] 软件位1: KVM 内部使用
  SW2   [bit 11] 软件位2: KVM 内部使用
  NX    [bit 63] No-Execute 或 Suppress #VE
```

### 1.4 软件状态位（MMU 元数据）

```c
/* 来源: arch/x86/kvm/mmu/spte.h */

/*
 * KVM 使用高位（bit 52-63）作为软件元数据位
 * 这些位对 EPT 硬件透明，但 KVM 用来跟踪页表状态
 *
 * 重要：这些位必须在写入 EPT 条目前被清除！
 */

/* MMU 权限镜像位 —— 记录 "原始" 权限，即使硬件位被临时修改 */
#define SPTE_PERM_MASK          /* 读/写/执行权限掩码 */
#define SPTE_MMU_WRITABLE_MASK  /* 软件可写位 - 表示 Guest 认为页面可写 */
#define SPTE_MMU_EXECUTABLE_MASK /* 软件可执行位 */

/*
 * 关键概念：Hardware Writable vs MMU Writable
 *
 * ┌──────────────────────────────────────────────────────┐
 * │  场景: 脏页日志（Dirty Logging）                      │
 * │                                                      │
 * │  初始状态: HW_W=1, MMU_W=1  → 正常读写               │
 * │                                                      │
 * │  开启脏页日志:                                        │
 * │    1. 清除 HW_Writable → 硬件阻止写入                 │
 * │    2. 保持 MMU_Writable → KVM 知道应该处理写请求      │
 * │    3. 下一次写入触发 EPT Violation                    │
 * │    4. KVM 记录脏页，重新设置 HW_Writable              │
 * │                                                      │
 * │  状态: HW_W=0, MMU_W=1  → 写入被拦截，等脏页记录     │
 * └──────────────────────────────────────────────────────┘
 */
```

---

## 2. 缺页处理入口：kvm_handle_page_fault()

`kvm_handle_page_fault()` 是 KVM 处理所有缺页的统一入口，由 VM-Exit
（退出原因 = EPT Violation 或 Page Fault）触发调用。

### 2.1 函数签名与参数

```c
/* 来源: arch/x86/kvm/mmu/mmu.c */

/*
 * kvm_handle_page_fault - 处理 VM-Exit 中的缺页异常
 *
 * @vcpu:        触发缺页的虚拟 CPU
 * @error_code:  硬件错误码（来自 VMCS EXIT_QUALIFICATION）
 * @fault_address: 触发缺页的线性/物理地址
 * @exec:        导致缺页的指令数据（用于 MMIO 模拟）
 * @no_reexecute: 是否禁止重新执行（内部重试标志）
 *
 * 错误码位定义（与 x86 PF error code 一致）:
 *   bit 0: P    - 0=页面不存在, 1=权限违规
 *   bit 1: W/R  - 0=读, 1=写
 *   bit 2: U/S  - 0=管理态, 1=用户态
 *   bit 3: RSVD - 保留位违规
 *   bit 4: I/D  - 0=数据访问, 1=指令取指
 *   bit 15: SGX - SGX 相关违规
 */
int kvm_handle_page_fault(struct kvm_vcpu *vcpu, u64 error_code,
                          gpa_t fault_address, char *insn, int *insn_len)
{
    int r;
    /* ... 见下方详细分析 ... */
}
```

### 2.2 处理流程详解

```c
/* 来源: arch/x86/kvm/mmu/mmu.c - kvm_handle_page_fault() 简化流程 */

int kvm_handle_page_fault(struct kvm_vcpu *vcpu, u64 error_code,
                          gpa_t fault_address, char *insn, int *insn_len)
{
    int r;

    /*
     * Step 1: 检查是否为嵌套虚拟化引起的缺页
     * 如果 L1 hypervisor 拦截了 EPT violation，需要注入给 L1
     */
    if (WARN_ON_ONCE(fault_address >> 32))  /* 地址有效性检查 */
        return -EFAULT;

    /*
     * Step 2: 处理需要用户态介入的情况
     * 某些缺页需要 QEMU 用户态处理（如 MMIO）
     */
    vcpu->arch.l1_tf_flush_l1d = true;

    /*
     * Step 3: 检查是否为 MMIO 访问
     * 如果 fault_address 映射到 MMIO 区域，需要设备模拟
     */
    if (error_code & PFERR_PRESENT_MASK) {
        /* 页面存在但权限违规 —— 不是缺页，是权限问题 */
        /* EPT misconfiguration 也走这里 */
    }

    /*
     * Step 4: 调用具体的缺页处理函数
     * 根据当前 MMU 模式选择处理路径：
     * - TDP 模式 (EPT/NPT): kvm_tdp_page_fault()
     * - 影子页表模式: kvm_shadow_page_fault()
     */

    /* 调用架构相关的 page fault 处理 */
    r = vcpu->arch.mmu->page_fault(vcpu, fault_address,
                                    error_code, false);
    /*
     * 这里 vcpu->arch.mmu->page_fault 指向:
     *   - kvm_tdp_page_fault() (TDP 模式)
     *   - kvm_shadow_page_fault() (影子页表模式)
     */

    return r;
}
```

### 2.3 调用流程图

```
VM-Exit (EPT Violation)
    │
    ▼
vmx_handle_exit()                    [vmx/vmx.c]
    │
    ▼
kvm_handle_page_fault()              [mmu/mmu.c]
    │
    ├── 检查 error_code
    │   ├── PFERR_PRESENT_MASK → 权限违规 / misconfiguration
    │   └── 无 PRESENT → 真正缺页（需要映射）
    │
    ├── 检查 MMIO 情况
    │   └── 如果是 MMIO → 返回给 QEMU 处理
    │
    └── 调用 mmu->page_fault()
        │
        ├── TDP 模式:
        │   └── kvm_tdp_page_fault()     [mmu/mmu.c]
        │       │
        │       └── kvm_tdp_mmu_map()    [mmu/tdp_mmu.c]
        │           │
        │           ├── 分配物理页 (kvm_mmu_alloc_sp())
        │           ├── 构造 SPTE (make_spte())
        │           └── 原子写入页表 (tdp_mmu_set_spte_atomic())
        │
        └── 影子页表模式:
            └── kvm_shadow_page_fault()  [mmu/mmu.c]
```

---

## 3. TDP 缺页处理：kvm_tdp_page_fault()

当 KVM 使用 EPT/NPT（两层地址翻译）时，缺页由 `kvm_tdp_page_fault()` 处理。

### 3.1 函数实现

```c
/* 来源: arch/x86/kvm/mmu/mmu.c */

/*
 * kvm_tdp_page_fault - 处理 TDP（Two-Dimensional Paging）模式缺页
 *
 * 这是 EPT/NPT 模式下的缺页处理入口
 * 核心工作: 建立 GPA → HPA 的 EPT 映射
 */
static int kvm_tdp_page_fault(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    /*
     * 检查是否可以使用直接映射（Direct Map）
     * 如果 GPA 对应的主机内存已经由 KVM memslot 管理，
     * 且不需要影子页表，可以使用直接 EPT 映射
     */

    /* 快速路径: 尝试直接建立映射 */
    return kvm_tdp_mmu_map(vcpu, fault);
}
```

### 3.2 缺页数据结构

```c
/* 来源: arch/x86/kvm/mmu/mmu_internal.h */

/*
 * struct kvm_page_fault - 缺页异常信息
 * 封装了一次缺页的所有上下文信息
 */
struct kvm_page_fault {
    /* 输入参数 */
    const gpa_t addr;           /* 触发缺页的 GPA 地址 */
    const u32 error_code;       /* 硬件错误码 */

    /* 解析后的信息 */
    const bool user_fault;      /* 是否用户态触发的缺页 */
    const bool write_fault;     /* 是否是写操作 */
    const bool exec_fault;      /* 是否是指取指操作 */
    const bool present;         /* EPT 条目是否已存在（权限违规）*/
    const bool rsvd;            /* 是否保留位违规 */
    const bool huge_page_disallowed;  /* 是否禁止大页映射 */

    /* 解析后的地址 */
    const gfn_t gfn;            /* Guest 页帧号 (GPA >> 12) */
    const hva_t hva;            /* 对应的宿主虚拟地址 */
    const hpa_t pfn;            /* 对应的宿主物理页帧号 */

    /* 内存槽信息 */
    struct kvm_memory_slot *slot; /* 对应的 memslot */

    /* 映射级别（4K/2M/1G） */
    int max_level;              /* 允许的最大映射级别 */
    int req_level;              /* 请求的映射级别 */
    bool goal_level;            /* 目标映射级别 */

    /* MMIO 相关 */
    bool is_tdp;                /* 是否为 TDP 缺页 */
};
```

---

## 4. TDP MMU 映射核心：kvm_tdp_mmu_map()

`kvm_tdp_mmu_map()` 是 EPT 映射的核心函数，负责为给定的 GPA 建立到 HPA 的映射。

### 4.1 函数实现

```c
/* 来源: arch/x86/kvm/mmu/tdp_mmu.c */

/*
 * kvm_tdp_mmu_map - 为 Guest GPA 建立 EPT 映射
 *
 * 核心流程:
 *   1. 遍历 EPT 页表，找到需要修改的叶条目
 *   2. 如果需要，分配中间层页表页
 *   3. 分配目标物理页
 *   4. 构造 SPTE 并原子写入
 *
 * 并发安全: 使用原子操作(cmpxchg)更新 SPTE，
 * 支持多个 vCPU 同时处理不同地址的缺页
 */
int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    struct kvm_mmu *mmu = vcpu->arch.mmu;
    struct tdp_iter iter;
    struct kvm_page_fault *f = fault;
    kvm_pte_t new_spte;
    int ret;

    /*
     * Step 1: 获取 TDP MMU 根页面
     *
     * TDP MMU 使用引用计数管理根页面:
     * - 每个 vCPU 在 page fault 期间持有根页面的引用
     * - 防止根页面在页表遍历过程中被释放
     * - 使用 RCU 保护根页面的生命周期
     */
    /* root = kvm_tdp_mmu_get_root(vcpu); */

    /*
     * Step 2: 遍历 EPT 页表
     *
     * tdp_root_for_each_leaf_pte() 从根开始遍历 EPT，
     * 找到 fault->gfn 对应的叶条目位置
     *
     * EPT 4级遍历:
     *   PML4 → PDPT → PD → PT → 叶条目
     *
     * 如果使用 2MB 大页:
     *   PML4 → PDPT → PD(2MB叶)
     */
    for_each_tdp_pte(mmu, iter, fault->gfn, fault->gfn + 1) {
        /*
         * Step 3: 检查当前叶条目状态
         *
         * 三种情况:
         * a) 条目已存在且映射正确 → 无需操作
         * b) 条目存在但映射到错误的页 → 需要替换
         * c) 条目不存在 → 需要创建新映射
         */

        /*
         * Step 4: 如果需要中间层页表但尚未分配，
         * 在这里分配（可能需要释放锁后再获取）
         */

        /*
         * Step 5: 分配目标物理页面
         * 从 KVM 的页面分配器获取一个 HPA
         */

        /*
         * Step 6: 构造新的 SPTE
         *
         * make_spte() 根据以下信息构造 SPTE:
         *   - 目标物理页帧号 (pfn)
         *   - 访问权限（读/写/执行）
         *   - 页面大小（4K/2M/1G）
         *   - 内存类型（WB/UC/...）
         *   - 脏页/访问位状态
         */
        /* new_spte = make_spte(vcpu, fault->slot, ACC_ALL,
         *                      iter.level, fault->gfn,
         *                      fault->pfn, ...); */

        /*
         * Step 7: 原子写入 SPTE
         *
         * 使用 cmpxchg (Compare-And-Swap) 原子更新:
         * - 如果当前 SPTE 未被其他 vCPU 修改 → 成功
         * - 如果已被修改 → 重试
         *
         * 这是 TDP MMU 并发安全的关键机制
         */
        /* ret = tdp_mmu_set_spte_atomic(vcpu->kvm, &iter, new_spte); */

        /* if (ret == 0) {
         *     // 成功: 刷新 TLB 如果需要
         *     return RET_PF_FIXED;
         * }
         * // 失败: cmpxchg 竞争，重新遍历
         */
    }

    return RET_PF_RETRY;  /* 需要重试 */
}
```

### 4.2 映射流程图

```
kvm_tdp_mmu_map() 内部流程:

    ┌─────────────────────┐
    │  获取 TDP MMU Root  │ ← 引用计数 +1
    └──────────┬──────────┘
               │
               ▼
    ┌─────────────────────┐
    │  遍历 EPT 页表      │
    │  (PML4→PDPT→PD→PT) │
    └──────────┬──────────┘
               │
        ┌──────┴──────┐
        │ 叶条目状态?  │
        └──┬───┬───┬──┘
           │   │   │
    ┌──────┘   │   └──────┐
    ▼          ▼          ▼
 不存在     已正确     需替换
    │       映射       │
    ▼          │        ▼
 ┌─────────┐  │   ┌─────────┐
 │分配物理页│  │   │替换映射 │
 │分配页表页│  │   │释放旧页 │
 └────┬────┘  │   └────┬────┘
      │       │        │
      └───────┼────────┘
              │
              ▼
    ┌─────────────────────┐
    │   make_spte()       │ ← 构造 SPTE 值
    │   组合: PFN|权限|MT │
    └──────────┬──────────┘
               │
               ▼
    ┌─────────────────────┐
    │ cmpxchg 原子写入    │
    │ (并发安全的关键!)   │
    └──────────┬──────────┘
               │
        ┌──────┴──────┐
        │ 成功?       │
        └──┬──────┬───┘
           │      │
          Yes     No
           │      │
           ▼      ▼
     返回成功  重新遍历
     RET_PF_FIXED  RET_PF_RETRY
```

---

## 5. SPTE 构造：make_spte()

`make_spte()` 函数（位于 `mmu/spte.c`）负责将各种参数组合成一个完整的 SPTE 值。

### 5.1 构造过程

```c
/* 来源: arch/x86/kvm/mmu/spte.c (概念性代码) */

/*
 * make_spte - 构造一个新的 SPTE
 *
 * 输入:
 *   pfn       - 目标物理页帧号
 *   level     - 页表层级 (PT_PG_LEVEL_4K / 2M / 1G)
 *   protection - 访问权限 (ACC_EXEC_MASK | ACC_WRITE_MASK | ...)
 *   dirty     - 是否初始标记为脏
 *   gfn       - Guest 页帧号
 *   speculative - 是否为推测性分配
 *
 * 输出:
 *   64位 SPTE 值
 */

/*
 * SPTE 构造过程:
 *
 * 1. 设置 Present 位
 *    spte = PT_PRESENT_MASK
 *
 * 2. 设置物理页帧号
 *    spte |= (pfn << PT64_LEVEL_BITS)   // PFN 放在高位
 *
 * 3. 设置权限位
 *    if (protection & ACC_WRITE_MASK)
 *        spte |= PT_WRITABLE_MASK
 *    if (!(protection & ACC_EXEC_MASK))
 *        spte |= PT64_NX_MASK
 *
 * 4. 设置大页位（如果需要）
 *    if (level > PG_LEVEL_4K)
 *        spte |= PT_PAGE_SIZE_MASK
 *
 * 5. 设置内存类型（EPT 模式）
 *    spte |= VMX_EPT_MT_WRITEBACK << 3   // 通常使用 WB
 *
 * 6. 设置 Accessed/Dirty 位
 *    if (dirty)
 *        spte |= SPTE_TDP_DIRTY_MASK
 *    spte |= SPTE_TDP_ACCESSED_MASK      // 新建映射标记已访问
 *
 * 返回: 完整的 SPTE 值
 */
```

---

## 6. TDP MMU 根页面管理

TDP MMU 使用多级根页面来支持并发访问和高效回收。

### 6.1 根页面结构

```c
/* 来源: arch/x86/kvm/mmu/tdp_mmu.h (概念性) */

/*
 * TDP MMU 根页面管理
 *
 * KVM 维护一个根页面列表，每个根页面关联一个 address space:
 *   - KVM_ADDRESS_SPACE_MEM: 常规内存
 *   - KVM_ADDRESS_SPACE_MEM_READONLY: 只读内存（用于 SMM 等）
 *
 * 根页面通过引用计数管理生命周期:
 *   - kvm_tdp_mmu_get_root(): 增加引用计数
 *   - kvm_tdp_mmu_put_root(): 减少引用计数，可能释放
 *
 * 并发保护:
 *   - 根页面列表使用 hlist + RCU 保护
 *   - 根页面本身使用 spinlock 保护
 *   - SPTE 更新使用原子操作
 */
```

### 6.2 根页面生命周期

```
根页面生命周期:

    ┌───────────────────┐
    │   创建根页面       │
    │   (首次 vCPU 需要) │
    └────────┬──────────┘
             │
             ▼
    ┌───────────────────┐
    │ 加入根页面列表     │
    │ hlist_add_head_rcu│
    │ refcount = 1      │
    └────────┬──────────┘
             │
    ┌────────┴────────┐
    │                 │
    ▼                 ▼
 ┌─────────┐   ┌─────────┐
 │ vCPU A  │   │ vCPU B  │     ← 每个 vCPU 在 page fault
 │ get_root│   │ get_root│       时获取引用
 │ ref++   │   │ ref++   │
 └────┬────┘   └────┬────┘
      │             │
      ▼             ▼
 ┌─────────┐   ┌─────────┐
 │ 处理    │   │ 处理    │     ← 并发处理不同 GPA 的缺页
 │ page    │   │ page    │       通过 cmpxchg 保证 SPTE 一致性
 │ fault   │   │ fault   │
 └────┬────┘   └────┬────┘
      │             │
      ▼             ▼
 ┌─────────┐   ┌─────────┐
 │ put_root│   │ put_root│     ← 完成后释放引用
 │ ref--   │   │ ref--   │
 └─────────┘   └─────────┘
                    │
                    ▼ (最后一个引用释放时)
            ┌───────────────┐
            │ 回收根页面     │
            │ 释放页表结构   │
            │ (可能延迟到   │
            │  RCU grace    │
            │  period 后)   │
            └───────────────┘
```

---

## 7. 原子 SPTE 更新

### 7.1 cmpxchg 机制

```c
/* 来源: arch/x86/kvm/mmu/tdp_mmu.c (概念性) */

/*
 * tdp_mmu_set_spte_atomic - 原子更新 SPTE
 *
 * 使用 cmpxchg 确保并发安全:
 *
 * expected = *sptep                    // 读取当前值
 * if (cmpxchg(sptep, expected, new)    // 原子比较并交换
 *     == expected)
 *     return 0;                        // 成功
 * else
 *     return -EBUSY;                   // 被其他 vCPU 抢先修改
 *
 * 为什么不用锁?
 * - EPT 缺页是热路径，锁会导致严重的并发瓶颈
 * - cmpxchg 是无锁操作，硬件保证原子性
 * - 失败时只需重新遍历，开销可控
 */
```

### 7.2 竞争场景

```
并发 SPTE 更新场景:

  vCPU A                          vCPU B
  (处理 GPA=0x1000)               (处理 GPA=0x2000)
  │                               │
  ├─ 遍历到 PD 层                  ├─ 遍历到同一 PD 层
  │                               │
  ├─ 需要分配 PT 页面              ├─ 也需要分配 PT 页面
  │                               │
  ├─ 分配 PT_A                     ├─ 分配 PT_B
  │                               │
  ├─ cmpxchg(PD_entry,            ├─ cmpxchg(PD_entry,
  │   0, PT_A) → 成功!             │   0, PT_B) → 失败!
  │                               │   (PD_entry 已被 vCPU A 设置)
  ├─ 继续写入 PT_A[...]            │
  │  设置 SPTE                     ├─ 发现 PD_entry 已有值
  │                               ├─ 释放 PT_B (回退)
  │                               ├─ 使用 PT_A 继续
  │                               ├─ 写入 PT_A[...]
  │                               │  设置 SPTE
  ▼                               ▼
```

---

## 8. 常见调试技巧

### 8.1 通过 ftrace 跟踪 SPTE 变化

```bash
# 跟踪 SPTE 的创建和修改
echo kvm_mmu_set_spte > /sys/kernel/debug/tracing/set_event
echo kvm_mmu_paging_element >> /sys/kernel/debug/tracing/set_event

# 观察大页 vs 4K 页的比例
# 在 trace 输出中查找 level 字段:
#   level=2 表示 2MB 大页
#   level=1 表示 4KB 页
```

### 8.2 通过 /proc 观察内存使用

```bash
# KVM 内存统计
cat /proc/meminfo | grep -i kvm

# 页表内存使用
grep -i "kvm\|mmu" /proc/slabinfo

# 查看每个 VM 的内存槽信息
cat /sys/kernel/debug/kvm/<vm_id>/memslot
```

---

## 9. 关键概念总结

| 概念 | 说明 | 源码位置 |
|------|------|----------|
| SPTE | KVM 的页表条目，混合硬件+软件位 | `spte.h` |
| PFN | 物理页帧号，SPTE 的核心载荷 | SPTE[12:51] |
| TDP | Two-Dimensional Paging (EPT/NPT) | `tdp_mmu.c` |
| cmpxchg | 原子比较交换，保证并发 SPTE 更新 | `tdp_mmu.c` |
| Root 页面 | EPT 的根页表，通过引用计数管理 | `tdp_mmu.c` |
| Memslot | KVM 内存槽，GPA 到 HVA 的映射 | `mmu.c` |
| Dirty Logging | 脏页跟踪，通过临时清除 W 位实现 | `mmu.c` |
