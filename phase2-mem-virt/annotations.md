# Phase 2：源码精读注释 - EPT 内存虚拟化

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。

---

## 1. SPTE 位布局

### Q: SPTE 的 64 位里，每一位都是硬件定义的吗？

不是。KVM 的 SPTE 是**硬件位与软件位的混合体**。低 12 位和高 12 位都混入了 KVM 自定义
的软件位，硬件 EPT 看到这些位会忽略（或当 reserved 位）。写入 EPT 硬件前需要清除。

### Q: 基础页表位定义在哪？

**背景：x86 页表项的结构**

x86 页表（PTE/PDE/PDPT）是一个 64 位的值，包含：
- **控制位**（低位）：权限、状态等
- **物理地址**（中间位）：指向下一级页表或物理页
- **扩展位**（高位）：NX、扩展属性等

**定义位置**：`arch/x86/kvm/mmu.h:15-29`

```c
// 基础控制位（所有页表通用）
#define PT_PRESENT_MASK         (1ULL << 0)     // 存在位：0=无效，1=有效
#define PT_WRITABLE_MASK        (1ULL << 1)     // 可写位：0=只读，1=可读写
#define PT_USER_MASK            (1ULL << 2)     // 用户态位：0=仅内核，1=用户+内核
#define PT_ACCESSED_MASK        (1ULL << 5)     // 已访问位：硬件自动设置
#define PT_DIRTY_MASK           (1ULL << 6)     // 脏页位：写入时硬件自动设置
#define PT_PAGE_SIZE_MASK       (1ULL << 7)     // 大页位：0=4KB，1=2MB/1GB
#define PT64_NX_MASK            (1ULL << 63)    // No-Execute：1=不可执行
```

**为什么这些位重要？**

```
Guest 访问内存时的权限检查：

CPU 读取页表项
  ↓
检查 Present 位
  ├─ 0 → 触发 Page Fault（缺页）
  └─ 1 → 继续检查
      ↓
      检查权限位（根据访问类型）
      ├─ 读访问 → 检查 Present
      ├─ 写访问 → 检查 Present + Writable
      └─ 执行 → 检查 Present + NX（NX=1 则拒绝）
          ↓
          检查用户/内核权限
          ├─ Ring 3（用户态）→ 检查 User 位
          └─ Ring 0（内核态）→ 跳过 User 检查
              ↓
              更新 A/D 位（硬件自动）
              ├─ 访问 → 设置 Accessed 位
              └─ 写入 → 设置 Dirty 位
```

**Guest 页表 vs EPT 的位定义**

虽然位位置相同，但语义不同：

| 位 | Guest 页表含义 | EPT 含义 | 关键区别 |
|----|---------------|---------|---------|
| bit 0 | **Present**（存在） | **Read**（可读） | Guest：0=完全不可访问；EPT：0=不可读但可写/执行 |
| bit 1 | Writable（可写） | Write（可写） | 相同 |
| bit 2 | User（用户态权限） | Execute（可执行） | **完全不同**！ |

**为什么位定义相似但语义不同？**

历史原因 + 设计简化：
- x86 页表最初设计：Present/Writable/User
- EPT 设计时沿用了 bit 位置，但改为 Read/Write/Execute
- KVM 需要同时处理两种语义

**实际影响**：

```c
// 设置 SPTE 时需要根据页表类型使用不同的语义
if (is_ept) {
    // EPT 语义：R/W/X
    spte = VMX_EPT_READABLE_MASK;    // bit 0 = Read
    if (writable)
        spte |= VMX_EPT_WRITABLE_MASK; // bit 1 = Write
    if (executable)
        spte |= VMX_EPT_EXECUTABLE_MASK; // bit 2 = Execute
} else {
    // Guest 页表语义：Present/Writable/User
    spte = PT_PRESENT_MASK;          // bit 0 = Present
    if (writable)
        spte |= PT_WRITABLE_MASK;    // bit 1 = Writable
    if (user)
        spte |= PT_USER_MASK;        // bit 2 = User
}
```

**实际影响**：

```c
// KVM 设置 SPTE 时
spte = PT_PRESENT_MASK;      // bit 0 = 1（存在）
if (writable)
    spte |= PT_WRITABLE_MASK; // bit 1 = 1（可写）
if (user)
    spte |= PT_USER_MASK;     // bit 2 = 1（用户态）
if (executable)
    spte |= PT64_NX_MASK;     // bit 63 = 0（可执行，NX=0）
```

### Q: 物理地址放在哪几位？

**物理地址的位置**：**默认 bits 51:12**，但可能动态调整

```c
// arch/x86/kvm/mmu/spte.h:39-43
#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
#define SPTE_BASE_ADDR_MASK (physical_mask & ~(u64)(PAGE_SIZE-1))
#else
#define SPTE_BASE_ADDR_MASK (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE-1))
#endif
```

**两种模式**：

| 模式 | 条件 | 物理地址范围 | 使用场景 |
|------|------|-------------|---------|
| **固定模式** | 未启用 CONFIG_DYNAMIC_PHYSICAL_MASK | bits 51:12（52 位） | 普通 VM |
| **动态模式** | 启用 CONFIG_DYNAMIC_PHYSICAL_MASK | 由 `physical_mask` 决定 | TDX/Hyper-V IVM/SME |

**拆解固定模式的宏**：

```c
(1ULL << 52) - 1              // 0x000FFFFFFFFFFFFF（低 52 位全 1）
~(u64)(PAGE_SIZE-1)           // 0xFFFFFFFFFFFFF000（清除低 12 位）
// 结果：0x000FFFFFFFFFFFFF & 0xFFFFFFFFFFFFF000
//     = 0x000FFFFFFFFFFFFF000
//     = bits 51:12
```

**动态模式下 physical_mask 的初始化**：

```c
// 默认初始化（arch/x86/mm/pgtable.c:11）
phys_addr_t physical_mask = (1ULL << 52) - 1;  // 默认 52 位

// 特殊场景下会被缩小：

// 1. TDX 安全虚拟机（arch/x86/coco/tdx/tdx.c:1040）
physical_mask &= cc_mask - 1;  // 缩小到安全内存范围

// 2. Hyper-V IVM 隔离虚拟机（arch/x86/hyperv/ivm.c:675）
physical_mask &= ms_hyperv.shared_gpa_boundary - 1;

// 3. SME 内存加密（arch/x86/mm/mem_encrypt_identity.c:565）
physical_mask &= ~me_mask;  // 排除加密位
```

**为什么是 bits 51:12？**

```
64 位 SPTE 的布局：

63    52 51        12 11     0
┌──────┬────────────┬────────┐
│扩展位│ 物理地址    │控制位   │
│(12位)│ (40位)     │(12位)  │
└──────┴────────────┴────────┘

物理地址 = 40 位 = 支持最大 1PB 物理内存
低 12 位 = 页内偏移（4KB 页），不需要存储
高 12 位 = 控制位和扩展属性
```

**关键理解：SPTE 存储的是 Host HPA，不是 Guest GPA**

```
Guest GPA → EPT 转换 → Host HPA
                        ↑
                   SPTE 存储这个地址
```

- SPTE 中的物理地址字段存储的是**宿主物理地址（HPA）**
- 使用**宿主的物理地址宽度**，不是 Guest 的物理地址宽度
- Guest GPA 的物理地址位数**不直接影响** SPTE 布局
- 影响 SPTE 的是**宿主的物理地址宽度**，在特殊场景下会被缩小

**实际使用**：

```c
// 从 SPTE 提取物理地址
u64 paddr = spte & SPTE_BASE_ADDR_MASK;

// 从物理页帧号（PFN）构造 SPTE 的地址部分
u64 spte_addr = (u64)pfn << PAGE_SHIFT;  // PFN << 12
spte |= spte_addr;
```

### Q: Guest GPA 位数会影响 EPT 页表吗？

**答案**：**会影响 EPT 页表级数**

**EPT 页表级数与 GPA 覆盖范围**：

| 页表级数 | 覆盖 GPA 位数 | 覆盖范围 | EPTP 配置 |
|---------|--------------|---------|-----------|
| 4 级 | 48 位 | 256 TB | `VMX_EPTP_PWL_4` |
| 5 级 | 57 位 | 128 PB | `VMX_EPTP_PWL_5` |

**KVM 如何选择 EPT 页表级数？**

```c
// arch/x86/kvm/mmu/mmu.c:5465-5476
static inline int kvm_mmu_get_tdp_level(struct kvm_vcpu *vcpu)
{
    /* tdp_root_level is architecture forced level, use it if nonzero */
    if (tdp_root_level)
        return tdp_root_level;

    /* Use 5-level TDP if and only if it's useful/necessary. */
    if (max_tdp_level == 5 && cpuid_maxphyaddr(vcpu) <= 48)
        return 4;  // Guest GPA <= 48 位，4 级页表足够

    return max_tdp_level;  // 否则使用最大支持的级数
}
```

**决策逻辑**：

```
Guest MAXPHYADDR（CPUID 配置）
  ↓
├─ <= 48 位 → 使用 4 级 EPT 页表
│   原因：4 级页表可覆盖 256TB，足够
│   优势：减少一级页表遍历，性能更好
│
└─ > 48 位 → 使用 5 级 EPT 页表（如果宿主支持）
    原因：需要覆盖更大的 GPA 空间
    限制：需要宿主支持 5 级页表（la57）
```

**EPTP 中的 Page Walk Length 配置**：

```c
// arch/x86/kvm/vmx/vmx.c:3411-3423
u64 construct_eptp(struct kvm_vcpu *vcpu, hpa_t root_hpa, int root_level)
{
    u64 eptp = VMX_EPTP_MT_WB;

    eptp |= (root_level == 5) ? VMX_EPTP_PWL_5 : VMX_EPTP_PWL_4;

    if (enable_ept_ad_bits &&
        (!is_guest_mode(vcpu) || nested_ept_ad_enabled(vcpu)))
        eptp |= VMX_EPTP_AD_ENABLE_BIT;
    eptp |= root_hpa;

    return eptp;
}
```

**EPTP 格式**（bits 7:3 = Page Walk Length）：

```
63    52 51    12 11 8 7   5 4 3 2 0
┌──────┬────────┬─────┬───┬─┴─┬─┴─┐
│ Host │ Root   │ 保留 │PWL│ AD │MT │
│ GPA  │ HPAddr │     │   │   │   │
└──────┴────────┴─────┴───┴───┴───┘

PWL (Page Walk Length):
  0x18 (3) = 4 级页表
  0x20 (4) = 5 级页表
```

**实际影响**：

| 场景 | Guest MAXPHYADDR | EPT 级数 | 性能影响 |
|------|-----------------|---------|---------|
| 普通 VM（48 位 GPA） | 48 位 | 4 级 | 最优 |
| 大内存 VM（57 位 GPA） | 57 位 | 5 级 | 多一级遍历 |
| 嵌套虚拟化 | 取决于 L1 | 可能降级 | L0 需要匹配 L1 配置 |

**关键理解**：

1. **Guest GPA 位数影响 EPT 页表级数**，但不影响 SPTE 布局
2. **4 级 EPT 页表**覆盖 48 位 GPA（256TB），足够大多数场景
3. **5 级 EPT 页表**覆盖 57 位 GPA（128PB），用于超大内存 VM
4. KVM 会**自动选择**最优的页表级数，无需手动配置

### Q: 软件位（KVM 元数据）分布在哪？

**文件**: `arch/x86/kvm/mmu/spte.h:18-91`

| 位 | 掩码 | 含义 |
|----|------|------|
| 9 | `DEFAULT_SPTE_HOST_WRITABLE` = `BIT_ULL(9)` | 宿主认为可写（非 EPT） |
| 10 | `DEFAULT_SPTE_MMU_WRITABLE` = `BIT_ULL(10)` | KVM MMU 认为可写（非 EPT） |
| 11 | `SPTE_MMU_PRESENT_MASK` = `BIT_ULL(11)` | KVM 认为"存在"（区分 MMIO SPTE） |
| 52:53 | `SPTE_TDP_AD_MASK` = `(3ULL << 52)` | EPT A/D 位跟踪类型 |
| 54:55 | `SHADOW_ACC_TRACK_SAVED_MASK` | 访问跟踪时保存的原始 R/X 位 |
| 57 | `EPT_SPTE_HOST_WRITABLE` = `BIT_ULL(57)` | 宿主认为可写（EPT 模式） |
| 58 | `EPT_SPTE_MMU_WRITABLE` = `BIT_ULL(58)` | KVM MMU 认为可写（EPT 模式） |

**为什么 EPT 模式的软件位在高比特（57/58）而非低比特（9/10）？** EPT 低位的可用
ignore 位太少 —— bit 0/1/2 是 R/W/X，bit 3:5 是 Memory Type，bit 6 是 IPAT，bit 7
是 Ignore PAT，bit 8/9 是 A/D。EPT 的叶条目几乎**没有**空闲低位可以借用。

### Q: `SPTE_TDP_AD_MASK` 的三种取值是什么？

**一句话回答**：控制 SPTE 中 Accessed/Dirty（A/D）位的行为模式。

**背景：什么是 A/D 位？**

x86 页表项（PTE）有两个硬件自动维护的位：
- **Accessed（A）位**：CPU 访问这个页时，硬件自动设为 1
- **Dirty（D）位**：CPU 写这个页时，硬件自动设为 1

这些位用于：
- 内存管理（页面置换算法需要知道哪些页被访问过）
- 写时复制（COW，需要知道哪些页被修改过）
- 实时迁移（需要知道哪些页是脏的，需要传输）

**问题：EPT 的 A/D 位**

EPT 也支持 A/D 位（如果硬件支持），但 KVM 有时需要控制是否启用：
- 启用 A/D：硬件自动维护，性能好
- 禁用 A/D：KVM 手动跟踪，用于特殊场景
- 仅写保护：只跟踪脏页，不跟踪访问

**三种模式**：

```c
// arch/x86/kvm/mmu/spte.h:33-36
#define SPTE_TDP_AD_ENABLED       (0ULL << 52)   // 默认：A/D 启用
#define SPTE_TDP_AD_DISABLED      (1ULL << 52)   // A/D 禁用
#define SPTE_TDP_AD_WRPROT_ONLY   (2ULL << 52)   // 仅写保护
```

| 模式 | 值 | 含义 | 使用场景 |
|------|----|------|---------|
| **AD_ENABLED** | 0 | 硬件自动维护 A/D 位 | 默认模式，性能最好 |
| **AD_DISABLED** | 1 | 硬件不维护 A/D 位 | 老硬件不支持，或嵌套虚拟化 L2 使用 PML |
| **AD_WRPROT_ONLY** | 2 | 只跟踪脏页（D 位） | 写保护模式，用于内存快照、迁移优化 |

**为什么 AD_ENABLED = 0？**

性能优化！默认路径不需要设置额外位：

```c
// 创建 SPTE 时
if (ad_mode == SPTE_TDP_AD_ENABLED) {
    // 不需要设置 bit 52-53，默认就是 0
    // 硬件自动维护 A/D 位
    spte |= PT_ACCESSED_MASK;  // 只设置 A 位
} else {
    // 需要设置 bit 52-53 来标记模式
    spte |= ad_mode;
}
```

**实际使用场景**：

1. **AD_ENABLED（默认）**：
   - 普通 VM 运行
   - 硬件自动维护 A/D 位
   - 性能最好

2. **AD_DISABLED**：
   - 老 CPU 不支持 EPT A/D 位（2010 年前的 CPU）
   - 嵌套虚拟化：L1 KVM 的 L2 Guest 使用 PML（Page Modification Logging）
   - PML 需要手动跟踪脏页，不能依赖硬件 A/D 位

3. **AD_WRPROT_ONLY**：
   - 内存快照：需要知道哪些页被修改过
   - 写时复制（COW）优化：只跟踪写操作
   - 实时迁移优化：只传输脏页

**源码位置**：

```c
// arch/x86/kvm/mmu/spte.c:175-178
if (sp->role.ad_disabled)
    spte |= SPTE_TDP_AD_DISABLED;
else if (kvm_mmu_page_ad_need_write_protect(sp))
    spte |= SPTE_TDP_AD_WRPROT_ONLY;
// 否则默认是 AD_ENABLED（值为 0，不需要设置）
```

### Q: `shadow_present_mask` 是什么？

**一句话回答**：SPTE 中用来表示"这个页表项有效"的位掩码。

**为什么需要它？**

硬件页表（无论是 Guest 页表还是 EPT）都需要一种方式判断"这个 entry 是否有效"。不同硬件的判断方式不同：

```
传统页表（x86 4-level）:
  ┌─────────────────────────────────────┐
  │ Entry 有效 = Present 位 (bit 0) = 1 │
  │ 如果 bit 0 = 0 → 页不存在           │
  └─────────────────────────────────────┘

EPT（支持 exec-only）:
  ┌─────────────────────────────────────┐
  │ Entry 有效 = 整个 entry 非零         │
  │ 不需要特定位，只要不是全 0 就有效     │
  │ 这样可以实现"只可执行"（无读无写）   │
  └─────────────────────────────────────┘
```

**KVM 的抽象**：

KVM 用 `shadow_present_mask` 统一这两种行为：

```c
// 检查 SPTE 是否有效
bool is_present = (spte & shadow_present_mask) != 0;
```

| 模式 | shadow_present_mask | 含义 |
|------|---------------------|------|
| **EPT（exec-only）** | `0` | 只要 SPTE 非零就有效（检查 `spte != 0`） |
| **EPT（无 exec-only）** | `0x1`（bit 0） | 必须 bit 0 = 1 才有效 |
| **Shadow/NPT** | `0x1`（bit 0） | 必须 bit 0 = 1 才有效 |

**为什么 EPT 支持 exec-only 时 mask = 0？**

因为 EPT 硬件只需要 entry 非零就认为有效。如果设置 mask = 0：
- `spte & 0` 永远 = 0
- `0 != 0` = false
- 所以 KVM 改用 `spte != 0` 来判断

这样就能实现"只可执行"的页：设置 bit 1（Write）和 bit 2（Execute）为 1，bit 0（Read）为 0。传统页表做不到（bit 0 必须是 1），但 EPT 可以。

**初始化位置**：

```c
// EPT 模式（vmx.c）
if (enable_ept) {
    if (cpu_has_vmx_ept_execute_only())
        shadow_present_mask = 0;       // exec-only：非零即有效
    else
        shadow_present_mask = 0x1;     // 无 exec-only：必须 bit 0
}

// Shadow/NPT 模式
else {
    shadow_present_mask = PT_PRESENT_MASK;  // 传统：必须 bit 0
}
```

### Q: EPT 位定义在哪？

**文件**: `arch/x86/include/asm/vmx.h:534-544`（不是 `spte.h`！）

```c
#define VMX_EPT_READABLE_MASK           0x1ull          /* bit 0: R */
#define VMX_EPT_WRITABLE_MASK           0x2ull          /* bit 1: W */
#define VMX_EPT_EXECUTABLE_MASK         0x4ull          /* bit 2: X */
#define VMX_EPT_IPAT_BIT                (1ull << 6)    /* bit 6: IPAT */
#define VMX_EPT_ACCESS_BIT              (1ull << 8)    /* bit 8: A */
#define VMX_EPT_DIRTY_BIT               (1ull << 9)    /* bit 9: D */
#define VMX_EPT_SUPPRESS_VE_BIT         (1ull << 63)   /* bit 63: Suppress #VE */
```

### Q: SPTE 位布局全景图

```
  63       58 57 55  53 52 51          12 11 10  9  8  7  6  5  4  3 2 1 0
 ┌──────────┬──┬──┬───┬───┬──────────────┬───┬──┬──┬──┬──┬──┬──┬──┬─┴─┴─┐
 │NX/Suppress│EPT│Saved│AD │  PFN         │MMU│HW│HW│A │PS│D │A │  MT  │X W R│
 │  VE (63) │MW │Bits │Typ│ (51:12)      │Pre│MW│Wr│  │  │  │  │  │(5:3)│(2)(1)│
 │          │(58)│(54) │(52)│             │(11)│(10)│(9)│  │(7)│  │  │   │     │
 └──────────┴──┴──┴───┴───┴──────────────┴───┴──┴──┴──┴──┴──┴──┴──┴─────┴────┘

非 EPT 模式:
  - bit 9 = DEFAULT_SPTE_HOST_WRITABLE
  - bit 10 = DEFAULT_SPTE_MMU_WRITABLE
  - bit 11 = SPTE_MMU_PRESENT_MASK

EPT 模式:
  - bit 57 = EPT_SPTE_HOST_WRITABLE
  - bit 58 = EPT_SPTE_MMU_WRITABLE
  - bit 11 = SPTE_MMU_PRESENT_MASK（共用）
  - bits 52:53 = SPTE_TDP_AD_MASK（A/D 跟踪类型）
  - bits 54:55 = 访问跟踪时保存的原始 R/X 位
```

---

## 2. 缺页处理入口

### Q: EPT Violation 触发后，代码从 VM-Exit 走到哪里？

**文件**: `arch/x86/kvm/mmu/mmu.c:4628` — `kvm_handle_page_fault()`

```c
int kvm_handle_page_fault(struct kvm_vcpu *vcpu, u64 error_code,
                            u64 fault_address, char *insn, int insn_len)
{
    ...
    if (!flags) {
        /* ★ 正常路径：无异步缺页标志 */
        r = kvm_mmu_page_fault(vcpu, fault_address, error_code,
                               insn, insn_len);
    } else if (flags & KVM_PV_REASON_PAGE_NOT_PRESENT) {
        /* ★ 异步缺页：Guest 页不在宿主内存 */
        kvm_async_pf_task_wait_schedule(fault_address);
    }
    ...
}
```

这个函数是 VM-Exit 处理函数 `handle_ept_violation()` 的最终调用目标。

### Q: `kvm_mmu_page_fault()` 内部分发逻辑？

**文件**: `arch/x86/kvm/mmu/mmu.c:6106`

```c
int noinline kvm_mmu_page_fault(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa,
                                u64 error_code, void *insn, int insn_len)
{
    /* ★ 保留位违规 → MMIO 模拟 */
    if (unlikely(error_code & PFERR_RSVD_MASK)) {
        r = handle_mmio_page_fault(vcpu, cr2_or_gpa, direct);
        if (r == RET_PF_EMULATE)
            goto emulate;
    }

    /* ★ 核心路径：分发到 MMU page fault handler */
    r = kvm_mmu_do_page_fault(vcpu, cr2_or_gpa, error_code, ...);

    /* 写保护违规 → 可能需要模拟 */
    if (r == RET_PF_WRITE_PROTECTED)
        r = kvm_mmu_write_protect_fault(vcpu, ...);

    /* ★ 需要指令模拟 */
    if (r == RET_PF_EMULATE)
        return x86_emulate_instruction(vcpu, cr2_or_gpa, ...);
}
```

### Q: `kvm_mmu_do_page_fault()` 怎么找到正确的处理函数？

**文件**: `arch/x86/kvm/mmu/mmu_internal.h:293`

```c
static inline int kvm_mmu_do_page_fault(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa,
                                        u64 err, bool prefetch, ...)
{
    struct kvm_page_fault fault = {
        .addr = cr2_or_gpa,
        .error_code = err,
        .prefetch = prefetch,
        ...
        .exec   = err & PFERR_FETCH_MASK,     /* ★ 从 error_code 解析 */
        .write  = err & PFERR_WRITE_MASK,
        .present = err & PFERR_PRESENT_MASK,
        .is_tdp = vcpu->arch.mmu->root_role.direct,
    };
    ...
    /* retpoline 缓解：直接调用而非间接 */
    return kvm_tdp_page_fault(vcpu, &fault);
}
```

`kvm_page_fault` 结构体在栈上构造，把 error_code 的各位**预解析**成 bool 字段，
避免下游代码重复解析。

---

## 3. `struct kvm_page_fault`：缺页上下文

### Q: 这个结构体封装了哪些信息？

**文件**: `arch/x86/kvm/mmu/mmu_internal.h:190`

```c
struct kvm_page_fault {
    /* ★ 输入参数 */
    const gpa_t addr;           /* 触发缺页的 GPA */
    const u64 error_code;       /* 硬件错误码 */
    const bool prefetch;        /* 是否为预取 */

    /* ★ 从 error_code 解析 */
    const bool exec;            /* 取指 */
    const bool write;           /* 写操作 */
    const bool present;         /* 权限违规（entry 已存在） */
    const bool rsvd;            /* 保留位违规 */
    const bool user;            /* 用户态触发 */

    /* ★ 派生状态 */
    const bool is_tdp;          /* TDP（EPT）模式 */
    bool huge_page_disallowed;  /* NX 大页缓解禁止大页 */

    /* ★ 映射级别决策 */
    u8 max_level;               /* 允许的最大映射级别 */
    u8 req_level;               /* 请求的映射级别 */
    u8 goal_level;              /* ★ 最终目标级别（4K/2M/1G） */

    /* ★ 解析后的地址 */
    gfn_t gfn;                  /* Guest 页帧号 */
    struct kvm_memory_slot *slot; /* 对应 memslot（可能 NULL） */
    kvm_pfn_t pfn;              /* 宿主物理页帧号 */
    hva_t hva;                  /* 宿主虚拟地址 */
    bool map_writable;          /* 是否可写映射 */
};
```

### Q: `max_level` / `req_level` / `goal_level` 三级的关系？

```
max_level ← 硬件与 memslot 约束
  │         （hugepage 是否可用、memslot 边界对齐）
  │
  ▼
req_level ← 宿主页表约束
  │         （宿主是否用了大页，PFN 对齐情况）
  │
  ▼
goal_level ← 最终决策
            （min(req_level, max_level)，
              再受 nx_huge_page 缓解影响）
```

`goal_level` 决定创建 4K / 2M / 1G 映射。`kvm_mmu_hugepage_adjust()` 负责计算。

---

## 4. TDP 缺页路径

### Q: `kvm_tdp_page_fault()` 怎么选择 TDP MMU 还是旧路径？

**文件**: `arch/x86/kvm/mmu/mmu.c:4726`

```c
int kvm_tdp_page_fault(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
#ifdef CONFIG_X86_64
    if (tdp_mmu_enabled)
        return kvm_tdp_mmu_page_fault(vcpu, fault);  /* ★ 新路径 */
#endif
    return direct_page_fault(vcpu, fault);            /* 旧路径 */
}
```

`tdp_mmu_enabled` 是编译期+运行期双重控制。6.12 里 TDP MMU（并发安全、无 mmu_lock
写锁）是默认路径。

### Q: `kvm_tdp_mmu_page_fault()` 的核心流程？

**文件**: `arch/x86/kvm/mmu/mmu.c:4673`

```c
static int kvm_tdp_mmu_page_fault(struct kvm_vcpu *vcpu,
                                  struct kvm_page_fault *fault)
{
    /* 1. 写跟踪检查（dirty logging） */
    if (page_fault_handle_page_track(vcpu, fault))
        return RET_PF_WRITE_PROTECTED;

    /* 2. 快速路径：直接 SPTE 修复 */
    r = fast_page_fault(vcpu, fault);
    if (r != RET_PF_INVALID)
        return r;

    /* 3. 补充内存缓存（页表页、SPTE 缓存） */
    r = mmu_topup_memory_caches(vcpu, false);

    /* 4. 解析 PFN：GPA → HVA → PFN */
    r = kvm_faultin_pfn(vcpu, fault, ACC_ALL);

    /* ★ 5. 读锁！不是写锁 —— TDP MMU 的并发关键 */
    read_lock(&vcpu->kvm->mmu_lock);

    if (is_page_fault_stale(vcpu, fault))
        goto out_unlock;

    /* ★ 6. 核心映射 */
    r = kvm_tdp_mmu_map(vcpu, fault);

out_unlock:
    read_unlock(&vcpu->kvm->mmu_lock);
    ...
}
```

**为什么用读锁？** TDP MMU 的 SPTE 更新用 `cmpxchg` 原子操作，多个 vCPU 可以
并发处理不同地址的缺页。读锁只防止页表结构在遍历中被释放（RCU 保护），不阻止
并发写入。

对比 `direct_page_fault()`（旧路径，`mmu.c:4576`）用的是 `write_lock()`。

---

## 5. TDP MMU 映射核心：`kvm_tdp_mmu_map()`

### Q: 函数做了什么？

**文件**: `arch/x86/kvm/mmu/tdp_mmu.c:1104`

```c
int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    struct kvm_mmu *mmu = vcpu->arch.mmu;
    struct tdp_iter iter;
    struct kvm_mmu_page *sp;

    kvm_mmu_hugepage_adjust(vcpu, fault);    /* 确定 goal_level */

    rcu_read_lock();                          /* ★ RCU 保护页表遍历 */

    tdp_mmu_for_each_pte(iter, mmu, fault->gfn, fault->gfn + 1) {
        /* 被冻结的 SPTE → 放弃重试 */
        if (is_frozen_spte(iter.old_spte))
            goto retry;

        /* ★ 到达目标级别 → 安装叶 SPTE */
        if (iter.level == fault->goal_level)
            goto map_target_level;

        /* 已有下级页表 → 继续下降 */
        if (is_shadow_present_pte(iter.old_spte) &&
            !is_large_pte(iter.old_spte))
            continue;

        /* ★ 需要分配中间层页表 */
        sp = tdp_mmu_alloc_sp(vcpu);
        tdp_mmu_init_child_sp(sp, &iter);

        if (is_shadow_present_pte(iter.old_spte))
            r = tdp_mmu_split_huge_page(kvm, &iter, sp, true);  /* 拆大页 */
        else
            r = tdp_mmu_link_sp(kvm, &iter, sp, true);           /* 链接新页表 */

        if (r) {
            tdp_mmu_free_sp(sp);
            goto retry;     /* cmpxchg 失败 → 重试 */
        }
    }

map_target_level:
    ret = tdp_mmu_map_handle_target_level(vcpu, fault, &iter);

retry:
    rcu_read_unlock();
    return ret;
}
```

### Q: 叶 SPTE 怎么安装？

**文件**: `arch/x86/kvm/mmu/tdp_mmu.c:1017` — `tdp_mmu_map_handle_target_level()`

```c
static int tdp_mmu_map_handle_target_level(struct kvm_vcpu *vcpu,
                                           struct kvm_page_fault *fault,
                                           struct tdp_iter *iter)
{
    u64 new_spte;
    bool wrprot;

    /* ★ 构造新 SPTE */
    wrprot = make_spte(vcpu, sp, fault->slot, ACC_ALL, iter->gfn,
                       fault->pfn, iter->old_spte, fault->prefetch, true,
                       fault->map_writable, &new_spte);

    if (new_spte == iter->old_spte)
        ret = RET_PF_SPURIOUS;                /* 无变化 → 伪缺页 */
    else if (tdp_mmu_set_spte_atomic(vcpu->kvm, iter, new_spte))
        return RET_PF_RETRY;                  /* ★ cmpxchg 失败 → 重试 */

    /* 写保护但 fault 是写 → 需要模拟 */
    if (wrprot && fault->write)
        ret = RET_PF_WRITE_PROTECTED;

    return ret;
}
```

---

## 6. `make_spte()`：构造 SPTE 值

### Q: 怎么把 PFN、权限、内存类型拼成 64 位？

**文件**: `arch/x86/kvm/mmu/spte.c:157`

```c
bool make_spte(struct kvm_vcpu *vcpu, struct kvm_mmu_page *sp,
               const struct kvm_memory_slot *slot,
               unsigned int pte_access, gfn_t gfn, kvm_pfn_t pfn,
               u64 old_spte, bool prefetch, bool can_unsync,
               bool host_writable, u64 *new_spte)
{
    int level = sp->role.level;
    u64 spte = SPTE_MMU_PRESENT_MASK;          /* ★ bit 11：KVM 认为存在 */

    /* 1. A/D 跟踪类型 */
    if (sp->role.ad_disabled)
        spte |= SPTE_TDP_AD_DISABLED;          /* bits 52:53 = 01 */
    else if (kvm_mmu_page_ad_need_write_protect(sp))
        spte |= SPTE_TDP_AD_WRPROT_ONLY;       /* bits 52:53 = 10 */

    /* 2. 硬件 present 位 */
    spte |= shadow_present_mask;                /* EPT: R 或 0 */

    /* 3. 访问位（非预取时设置） */
    if (!prefetch)
        spte |= spte_shadow_accessed_mask(spte);

    /* 4. NX 大页缓解：大页 + 可执行 → 强制去执行 */
    if (level > PG_LEVEL_4K && (pte_access & ACC_EXEC_MASK) &&
        is_nx_huge_page_enabled(vcpu->kvm)) {
        pte_access &= ~ACC_EXEC_MASK;
    }

    /* 5. 执行权限 */
    if (pte_access & ACC_EXEC_MASK)
        spte |= shadow_x_mask;
    else
        spte |= shadow_nx_mask;

    /* 6. 用户权限 */
    if (pte_access & ACC_USER_MASK)
        spte |= shadow_user_mask;

    /* 7. 大页标记 */
    if (level > PG_LEVEL_4K)
        spte |= PT_PAGE_SIZE_MASK;              /* bit 7 */

    /* 8. 内存类型（EPT 专属） */
    if (shadow_memtype_mask)
        spte |= kvm_x86_call(get_mt_mask)(vcpu, gfn, kvm_is_mmio_pfn(pfn));

    /* 9. 宿主可写标记 */
    if (host_writable)
        spte |= shadow_host_writable_mask;
    else
        pte_access &= ~ACC_WRITE_MASK;          /* 宿主不可写 → 去写权限 */

    /* 10. PFN 放入 bits 51:12 */
    spte |= (u64)pfn << PAGE_SHIFT;

    /* 11. 写权限 + MMU 可写标记 */
    if (pte_access & ACC_WRITE_MASK) {
        spte |= PT_WRITABLE_MASK | shadow_mmu_writable_mask;

        /* 旧 SPTE 已可写 → 跳过 unsync（优化） */
        if (is_last_spte(old_spte, level) && is_writable_pte(old_spte))
            goto out;

        /* 尝试 unsync 影子页；失败则写保护 */
        if (mmu_try_to_unsync_pages(...)) {
            pte_access &= ~ACC_WRITE_MASK;
            spte &= ~(PT_WRITABLE_MASK | shadow_mmu_writable_mask);
        }
    }

    /* 12. 脏页标记 */
    if (pte_access & ACC_WRITE_MASK)
        spte |= spte_shadow_dirty_mask(spte);

out:
    *new_spte = spte;
    return wrprot;     /* true = 需要写保护，调用者需模拟写 */
}
```

### Q: 为什么 `make_spte()` 返回 `bool`？

返回 `wrprot`：true 表示"虽然 Guest 想要写权限，但 SPTE 被降级为只读了"。
原因可能是影子页无法 unsync。调用方拿到 `wrprot=true` + `fault->write=true`
后走模拟路径（`RET_PF_WRITE_PROTECTED`），不直接重入 Guest。

---

## 7. 原子 SPTE 更新

### Q: TDP MMU 不用 mmu_lock 写锁，怎么保证并发安全？

**文件**: `arch/x86/kvm/mmu/tdp_mmu.c:533` — `__tdp_mmu_set_spte_atomic()`

```c
static inline int __must_check __tdp_mmu_set_spte_atomic(struct tdp_iter *iter,
                                                         u64 new_spte)
{
    u64 *sptep = rcu_dereference(iter->sptep);

    /* ★ 原子比较并交换：只有一个 vCPU 能成功 */
    if (!try_cmpxchg64(sptep, &iter->old_spte, new_spte))
        return -EBUSY;               /* 被别的 vCPU 抢先了 */

    return 0;
}
```

**`iter->old_spte` 是双向的**：
- 作为 cmpxchg 的 expected 值（输入）
- 失败时被硬件更新为当前值（输出）→ 调用者可以用新值重试

### Q: 竞争场景长什么样？

```
vCPU A (GPA=0x1000)              vCPU B (GPA=0x2000)
│                                │
├─ 遍历到 PD 层                   ├─ 遍历到同一 PD 层
│                                │
├─ 需要分配 PT 页面               ├─ 也需要分配 PT 页面
├─ 分配 PT_A                      │
│                                ├─ 分配 PT_B
├─ cmpxchg(PD[idx],               │
│    old=0, new=PT_A) → 成功!     │
│                                ├─ cmpxchg(PD[idx],
│                                │   old=0, new=PT_B) → 失败!
│                                │  (iter->old_spte 已被更新为 PT_A)
│                                │
├─ 继续向下遍历                    ├─ 发现 PD[idx] 已有值
│  写入 PT_A[...]                  ├─ 释放 PT_B
│                                ├─ 使用 PT_A 继续遍历
│                                ├─ 写入 PT_A[...]
```

**为什么失败方要释放自己分配的页表？** 胜利方已经把自己的页表链接进了 EPT，
失败方的页表没人引用了，必须回收。否则泄漏。

### Q: 外层包装 `tdp_mmu_set_spte_atomic()` 做什么额外的事？

**文件**: `arch/x86/kvm/mmu/tdp_mmu.c:576`

```c
static inline int tdp_mmu_set_spte_atomic(struct kvm *kvm,
                                          struct tdp_iter *iter,
                                          u64 new_spte)
{
    lockdep_assert_held_read(&kvm->mmu_lock);    /* ★ 读锁即可 */

    ret = __tdp_mmu_set_spte_atomic(iter, new_spte);
    if (ret)
        return ret;

    /* ★ 更新记账：处理 A/D 位变化、TLB 失效等 */
    handle_changed_spte(kvm, iter->as_id, iter->gfn,
                        iter->old_spte, new_spte, iter->level, true);
    return 0;
}
```

---

## 8. TDP MMU 根页面管理

### Q: 根页面的生命周期怎么管理？

**获取引用**: `arch/x86/kvm/mmu/tdp_mmu.h:15`

```c
static inline bool kvm_tdp_mmu_get_root(struct kvm_mmu_page *root)
{
    return refcount_inc_not_zero(&root->tdp_mmu_root_count);
}
```

**释放引用**: `arch/x86/kvm/mmu/tdp_mmu.c:76`

```c
void kvm_tdp_mmu_put_root(struct kvm *kvm, struct kvm_mmu_page *root)
{
    if (!refcount_dec_and_test(&root->tdp_mmu_root_count))
        return;                              /* 还有其他引用 → 不释放 */

    list_del_rcu(&root->link);               /* 从根列表移除 */
    call_rcu(&root->rcu_head, tdp_mmu_free_sp_rcu_callback);
    /* ★ RCU grace period 后才真正释放内存 */
}
```

**为什么需要引用计数？** 多个 vCPU 并发处理缺页时，每个都持有根的引用。
一个 vCPU 想切换根（比如 memslot 变化要 zap 全部），必须等所有引用释放后才能
安全释放旧根。`call_rcu` 确保正在遍历的 vCPU 不会访问已释放的内存。

---

## 9. 完整调用链

```
VM-Exit (EPT Violation)
  │
  ▼
vmx_handle_exit() → handle_ept_violation()
  │
  ▼
kvm_handle_page_fault()             [mmu.c:4628]
  │
  ├── async PF? → kvm_async_pf_task_wait_schedule()
  │
  └── ★ 正常路径:
      ▼
kvm_mmu_page_fault()                [mmu.c:6106]
  │
  ├── PFERR_RSVD → handle_mmio_page_fault() → MMIO 模拟
  │
  └── ★ 核心分发:
      ▼
kvm_mmu_do_page_fault()             [mmu_internal.h:293]
  │ 构造 struct kvm_page_fault
  │
  ▼
kvm_tdp_page_fault()                [mmu.c:4726]
  │
  ├── tdp_mmu_enabled?
  │   │
  │   ▼ ★ TDP MMU 路径（默认）
  │ kvm_tdp_mmu_page_fault()        [mmu.c:4673]
  │   │
  │   ├── page_fault_handle_page_track() → 脏页跟踪
  │   ├── fast_page_fault()          → 快速路径
  │   ├── kvm_faultin_pfn()          → GPA → PFN
  │   │
  │   ▼ read_lock(&mmu_lock)        ← ★ 读锁！
  │ kvm_tdp_mmu_map()               [tdp_mmu.c:1104]
  │   │
  │   ├── kvm_mmu_hugepage_adjust()  → 确定 goal_level
  │   ├── tdp_mmu_for_each_pte()    → 遍历 EPT
  │   │   ├── 中间层缺页 → 分配 + tdp_mmu_link_sp()
  │   │   └── 大页需拆 → tdp_mmu_split_huge_page()
  │   │
  │   ▼
  │ tdp_mmu_map_handle_target_level() [tdp_mmu.c:1017]
  │   │
  │   ├── make_spte()               [spte.c:157]
  │   │   └→ 组合: present | PFN | 权限 | MT | A/D
  │   │
  │   └── tdp_mmu_set_spte_atomic() [tdp_mmu.c:576]
  │       └→ try_cmpxchg64()        ← ★ 并发安全核心
  │
  └── !tdp_mmu_enabled?
      │
      ▼ ★ 旧路径
  direct_page_fault()                [mmu.c:4576]
    │
    ▼ write_lock(&mmu_lock)          ← 写锁（并发差）
  direct_map() → FNAME(fetch)()
```

---

## 10. 关键数据结构关系

```
每 vCPU:
  struct kvm_vcpu
    └── arch.mmu → struct kvm_mmu
          ├── root_role.direct = true    ← TDP 模式标识
          ├── root.hpa                   ← EPT 根页物理地址
          └── page_fault → kvm_tdp_page_fault  ← 函数指针

每个 EPT 根:
  struct kvm_mmu_page (root)
    ├── tdp_mmu_root_count (refcount)   ← 引用计数
    ├── link (list_head)                ← 挂在全局根列表
    ├── role.level / role.ad_disabled   ← 角色信息
    └── spt → 页表页（512 个 SPTE）

TDP MMU 全局:
  struct kvm
    └── arch.tdp_mmu_roots             ← 根列表
          └── hlist_head → kvm_mmu_page (多个根)
                                │
                                └── spt[i] → 下级页表或叶 SPTE
```

---

## 11. 模块参数

### Q: 哪些 MMU 参数可以在运行时改？

**文件**: `arch/x86/kvm/mmu/mmu.c:67-80`（典型参数）

| 参数名 | 变量名 | 权限 | 默认 | 作用 |
|--------|--------|------|------|------|
| `nx_huge_pages` | `nx_huge_pages` | 0444 | auto | NX 大页缓解 |
| `nx_huge_pages_recovery_ratio` | 同名 | 0644 | 60 | 回收比率 |
| `tdp_mmu` | `tdp_mmu_enabled` | 0444 | y | TDP MMU 并发路径 |
| `hugepages` | `allow_hugetlbep` | 0444 | y | 允许大页映射 |
| `dirty_log_time_acc` | — | — | — | 脏页日志时间片 |

**注意**: `nx_huge_pages` 的缓解逻辑会修改 `make_spte()` 的行为：大页 + 可执行
→ 强制去执行位，降为只读数据页。这是针对 iTLB multihit 的缓解。
