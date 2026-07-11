# EPT Violation 处理流程详解

> 从硬件触发到页表建立的完整路径

---

## 1. 完整调用链概览

```
Guest 访问 GPA
    ↓
EPT 页表缺失或权限违规
    ↓
硬件触发 VM-Exit (EXIT_REASON_EPT_VIOLATION = 48)
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 1. vmx_handle_exit()                                        │
│    arch/x86/kvm/vmx/vmx.c:6131                              │
│    exit_handlers[EXIT_REASON_EPT_VIOLATION]                │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. handle_ept_violation()                                   │
│    arch/x86/kvm/vmx/vmx.c:5782                              │
│    - 读取 GUEST_PHYSICAL_ADDRESS                            │
│    - 读取 exit_qualification                                │
│    - 调用 __vmx_handle_ept_violation()                      │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. __vmx_handle_ept_violation()                             │
│    arch/x86/kvm/vmx/common.h:9                              │
│    - 解析 exit_qualification，构造 error_code               │
│    - 调用 kvm_mmu_page_fault()                              │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. kvm_mmu_page_fault()                                     │
│    arch/x86/kvm/mmu/mmu.c:6106                              │
│    - 检查是否是 MMIO (PFERR_RSVD_MASK)                      │
│    - 调用 kvm_mmu_do_page_fault()                           │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 5. kvm_mmu_do_page_fault()                                  │
│    arch/x86/kvm/mmu/mmu_internal.h:293                      │
│    - 初始化 fault 结构体                                    │
│    - 调用 kvm_tdp_page_fault() (TDP 模式)                   │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 6. kvm_tdp_page_fault()                                     │
│    arch/x86/kvm/mmu/mmu.c:4726                              │
│    - 调用 kvm_tdp_mmu_page_fault()                          │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 7. kvm_tdp_mmu_page_fault()                                 │
│    arch/x86/kvm/mmu/tdp_mmu.c                               │
│    - 调用 direct_page_fault()                               │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 8. direct_page_fault()                                      │
│    arch/x86/kvm/mmu/mmu.c:4575                              │
│    - 快速路径检查 (fast_page_fault)                         │
│    - 分配物理页 (kvm_faultin_pfn)                           │
│    - 获取 mmu_lock                                          │
│    - 调用 direct_map()                                      │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 9. direct_map()                                             │
│    arch/x86/kvm/mmu/mmu.c                                   │
│    - 调用 kvm_tdp_mmu_map()                                 │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 10. kvm_tdp_mmu_map()                                       │
│     arch/x86/kvm/mmu/tdp_mmu.c:1104                         │
│     - 遍历 EPT 页表 (tdp_mmu_for_each_pte)                 │
│     - 创建中间页表项                                         │
│     - 调用 tdp_mmu_map_handle_target_level()                │
└─────────────────────────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────────────────────────┐
│ 11. tdp_mmu_map_handle_target_level()                       │
│     arch/x86/kvm/mmu/tdp_mmu.c:1017                         │
│     - 调用 make_spte() 构建 SPTE 值                         │
│     - 原子写入 EPT 页表 (tdp_mmu_set_spte_atomic)           │
└─────────────────────────────────────────────────────────────┘
    ↓
VM-Resume，Guest 继续执行
```

---

## 2. 关键步骤详解

### 2.1 VM-Exit 触发（硬件）

当 Guest 访问 GPA 时，EPT walker 发现：
- EPT 叶条目 Present = 0（缺页）
- 或访问类型与权限位不匹配（权限违规）

硬件触发 VM-Exit，保存以下信息到 VMCS：
- `GUEST_PHYSICAL_ADDRESS`：触发异常的 GPA
- `EXIT_QUALIFICATION`：异常详情（读/写/执行、是否 present 等）

### 2.2 handle_ept_violation()

```c
// arch/x86/kvm/vmx/vmx.c:5782
static int handle_ept_violation(struct kvm_vcpu *vcpu)
{
    unsigned long exit_qualification = vmx_get_exit_qual(vcpu);
    gpa_t gpa;

    // ... NMI 相关处理 ...

    // 从 VMCS 读取触发异常的 GPA
    gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);
    trace_kvm_page_fault(vcpu, gpa, exit_qualification);

    // 检查 GPA 是否合法
    if (unlikely(allow_smaller_maxphyaddr && !kvm_vcpu_is_legal_gpa(vcpu, gpa)))
        return kvm_emulate_instruction(vcpu, 0);

    return __vmx_handle_ept_violation(vcpu, gpa, exit_qualification);
}
```

**关键操作**：
1. 从 VMCS 读取 `GUEST_PHYSICAL_ADDRESS`
2. 从 VMCS 读取 `EXIT_QUALIFICATION`
3. 传递给下一层处理

### 2.3 __vmx_handle_ept_violation()

```c
// arch/x86/kvm/vmx/common.h:9
static inline int __vmx_handle_ept_violation(struct kvm_vcpu *vcpu, gpa_t gpa,
                                             unsigned long exit_qualification)
{
    u64 error_code;

    /* Is it a read fault? */
    error_code = (exit_qualification & EPT_VIOLATION_ACC_READ)
                 ? PFERR_USER_MASK : 0;
    /* Is it a write fault? */
    error_code |= (exit_qualification & EPT_VIOLATION_ACC_WRITE)
                  ? PFERR_WRITE_MASK : 0;
    /* Is it a fetch fault? */
    error_code |= (exit_qualification & EPT_VIOLATION_ACC_INSTR)
                  ? PFERR_FETCH_MASK : 0;
    /* ept page table entry is present? */
    error_code |= (exit_qualification & EPT_VIOLATION_RWX_MASK)
                  ? PFERR_PRESENT_MASK : 0;

    if (exit_qualification & EPT_VIOLATION_GVA_IS_VALID)
        error_code |= (exit_qualification & EPT_VIOLATION_GVA_TRANSLATED) ?
                      PFERR_GUEST_FINAL_MASK : PFERR_GUEST_PAGE_MASK;

    return kvm_mmu_page_fault(vcpu, gpa, error_code, NULL, 0);
}
```

**关键操作**：
将 EPT 特有的 `exit_qualification` 转换为通用的 `error_code` 格式：

| exit_qualification 位 | 含义 | error_code 位 |
|----------------------|------|--------------|
| `EPT_VIOLATION_ACC_READ` | 读访问 | `PFERR_USER_MASK` |
| `EPT_VIOLATION_ACC_WRITE` | 写访问 | `PFERR_WRITE_MASK` |
| `EPT_VIOLATION_ACC_INSTR` | 执行访问 | `PFERR_FETCH_MASK` |
| `EPT_VIOLATION_RWX_MASK` | EPT 条目 present | `PFERR_PRESENT_MASK` |

### 2.4 kvm_mmu_page_fault()

```c
// arch/x86/kvm/mmu/mmu.c:6106
int noinline kvm_mmu_page_fault(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa, u64 error_code,
                                void *insn, int insn_len)
{
    int r, emulation_type = EMULTYPE_PF;
    bool direct = vcpu->arch.mmu->root_role.direct;

    // ... 软件保护 VM 相关处理 ...

    r = RET_PF_INVALID;
    
    // 检查是否是 MMIO 访问
    if (unlikely(error_code & PFERR_RSVD_MASK)) {
        if (WARN_ON_ONCE(error_code & PFERR_PRIVATE_ACCESS))
            return -EFAULT;

        r = handle_mmio_page_fault(vcpu, cr2_or_gpa, direct);
        if (r == RET_PF_EMULATE)
            goto emulate;
    }

    if (r == RET_PF_INVALID) {
        vcpu->stat.pf_taken++;

        // 调用实际的页错误处理
        r = kvm_mmu_do_page_fault(vcpu, cr2_or_gpa, error_code, false,
                                  &emulation_type, NULL);
        if (KVM_BUG_ON(r == RET_PF_INVALID, vcpu->kvm))
            return -EIO;
    }

    if (r < 0)
        return r;

    // ... 写保护和统计处理 ...

    if (r != RET_PF_EMULATE)
        return 1;

emulate:
    return x86_emulate_instruction(vcpu, cr2_or_gpa, emulation_type, insn,
                                   insn_len);
}
```

**关键操作**：
1. 检查是否是 MMIO（通过 `PFERR_RSVD_MASK`）
2. 调用 `kvm_mmu_do_page_fault()` 处理普通页错误
3. 如果需要模拟，调用 `x86_emulate_instruction()`

### 2.5 kvm_mmu_do_page_fault()

```c
// arch/x86/kvm/mmu/mmu_internal.h:293
static inline int kvm_mmu_do_page_fault(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa,
                                        u64 err, bool prefetch,
                                        int *emulation_type, u8 *level)
{
    struct kvm_page_fault fault = {
        .addr = cr2_or_gpa,
        .error_code = err,
        .access = err & PFERR_ACCESS_MASK,
        .max_level = KVM_MAX_HUGEPAGE_LEVEL,
        .req_level = PG_LEVEL_4K,
        .goal_level = PG_LEVEL_4K,
        .is_private = err & PFERR_PRIVATE_ACCESS,

        .pfn = KVM_PFN_ERR_FAULT,
        .hva = KVM_HVA_ERR_BAD,
    };
    int r;

    // 如果是直接映射模式，计算 GFN 和 slot
    if (vcpu->arch.mmu->root_role.direct) {
        fault.gfn = fault.addr >> PAGE_SHIFT;
        fault.slot = kvm_vcpu_gfn_to_memslot(vcpu, fault.gfn);
    }

    // 调用 TDP 或 shadow 页错误处理
    if (IS_ENABLED(CONFIG_MITIGATION_RETPOLINE) && fault.is_tdp)
        r = kvm_tdp_page_fault(vcpu, &fault);
    else
        r = vcpu->arch.mmu->page_fault(vcpu, &fault);

    // ... 错误处理和返回值处理 ...

    return r;
}
```

**关键操作**：
1. 初始化 `kvm_page_fault` 结构体
2. 计算 GFN 和对应的 memslot
3. 调用 `kvm_tdp_page_fault()`（TDP 模式）

### 2.6 kvm_tdp_page_fault()

```c
// arch/x86/kvm/mmu/mmu.c:4726
int kvm_tdp_page_fault(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
#ifdef CONFIG_X86_64
    if (tdp_mmu_enabled)
        return kvm_tdp_mmu_page_fault(vcpu, fault);
#endif

    return direct_page_fault(vcpu, fault);
}
```

**关键操作**：
- 如果启用 TDP MMU，调用 `kvm_tdp_mmu_page_fault()`
- 否则调用 `direct_page_fault()`

### 2.7 direct_page_fault()

```c
// arch/x86/kvm/mmu/mmu.c:4575
static int direct_page_fault(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    kvm_pfn_t orig_pfn;
    int r;

    // ... dummy root 检查 ...

    // 快速路径：检查是否是写保护导致的
    if (page_fault_handle_page_track(vcpu, fault))
        return RET_PF_WRITE_PROTECTED;

    // 快速路径：尝试不获取锁的快速修复
    r = fast_page_fault(vcpu, fault);
    if (r != RET_PF_INVALID)
        return r;

    // 分配内存缓存
    r = mmu_topup_memory_caches(vcpu, false);
    if (r)
        return r;

    // 分配物理页（Get User Pages）
    r = kvm_faultin_pfn(vcpu, fault, ACC_ALL);
    if (r != RET_PF_CONTINUE)
        return r;

    orig_pfn = fault->pfn;

    r = RET_PF_RETRY;
    write_lock(&vcpu->kvm->mmu_lock);  // 获取 MMU 锁

    if (is_page_fault_stale(vcpu, fault))
        goto out_unlock;

    // 释放一些旧页，为新映射腾出空间
    r = make_mmu_pages_available(vcpu);
    if (r)
        goto out_unlock;

    // 实际映射页面
    r = direct_map(vcpu, fault);

out_unlock:
    write_unlock(&vcpu->kvm->mmu_lock);  // 释放 MMU 锁
    kvm_release_pfn_clean(orig_pfn);
    return r;
}
```

**关键操作**：
1. **快速路径检查**：
   - `page_fault_handle_page_track()`：检查是否是脏页跟踪
   - `fast_page_fault()`：尝试无锁快速修复
2. **分配物理页**：`kvm_faultin_pfn()` 调用 GUP（Get User Pages）
3. **获取 mmu_lock**：`write_lock(&vcpu->kvm->mmu_lock)`
4. **实际映射**：`direct_map()` → `kvm_tdp_mmu_map()`

### 2.8 kvm_tdp_mmu_map()

```c
// arch/x86/kvm/mmu/tdp_mmu.c:1104
int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    struct kvm_mmu *mmu = vcpu->arch.mmu;
    struct kvm *kvm = vcpu->kvm;
    struct tdp_iter iter;
    struct kvm_mmu_page *sp;
    int ret = RET_PF_RETRY;

    // 调整大页级别（4K/2M/1G）
    kvm_mmu_hugepage_adjust(vcpu, fault);

    trace_kvm_mmu_spte_requested(fault);

    rcu_read_lock();

    // 遍历 EPT 页表，从根到叶
    tdp_mmu_for_each_pte(iter, mmu, fault->gfn, fault->gfn + 1) {
        int r;

        // ... nx huge page 处理 ...

        // 如果 SPTE 被冻结，重试
        if (is_frozen_spte(iter.old_spte))
            goto retry;

        // 如果到达目标级别，映射
        if (iter.level == fault->goal_level)
            goto map_target_level;

        // 如果当前级别有页表，继续向下遍历
        if (is_shadow_present_pte(iter.old_spte) &&
            !is_large_pte(iter.old_spte))
            continue;

        // 需要创建新的中间页表
        sp = tdp_mmu_alloc_sp(vcpu);
        tdp_mmu_init_child_sp(sp, &iter);

        sp->nx_huge_page_disallowed = fault->huge_page_disallowed;

        // 如果是大页，需要拆分
        if (is_shadow_present_pte(iter.old_spte))
            r = tdp_mmu_split_huge_page(kvm, &iter, sp, true);
        else
            r = tdp_mmu_link_sp(kvm, &iter, sp, true);

        if (r) {
            tdp_mmu_free_sp(sp);
            goto retry;
        }

        // ... nx huge page 跟踪 ...
    }

    // 遍历提前终止，重试
    WARN_ON_ONCE(iter.level == fault->goal_level);
    goto retry;

map_target_level:
    ret = tdp_mmu_map_handle_target_level(vcpu, fault, &iter);

retry:
    rcu_read_unlock();
    return ret;
}
```

**关键操作**：
1. **调整大页级别**：`kvm_mmu_hugepage_adjust()` 决定是否使用 2M/1G 大页
2. **遍历 EPT 页表**：`tdp_mmu_for_each_pte` 从根到叶遍历
3. **创建中间页表**：如果中间级别缺失，分配新页面
4. **拆分大页**：如果已有大页但需要更细粒度，拆分大页
5. **映射目标级别**：调用 `tdp_mmu_map_handle_target_level()`

### 2.9 tdp_mmu_map_handle_target_level()

```c
// arch/x86/kvm/mmu/tdp_mmu.c:1017
static int tdp_mmu_map_handle_target_level(struct kvm_vcpu *vcpu,
                                           struct kvm_page_fault *fault,
                                           struct tdp_iter *iter)
{
    struct kvm_mmu_page *sp = sptep_to_sp(rcu_dereference(iter->sptep));
    u64 new_spte;
    int ret = RET_PF_FIXED;
    bool writable = READ_ONCE(fault->slot->arch.lpage_info[...]);

    if (unlikely(!is_shadow_present_pte(iter->old_spte))) {
        // 新的叶节点，需要分配页面
        if (sp->role.level > PG_LEVEL_4K)
            track_possible_nx_huge_page(vcpu->kvm, sp);
    }

    // 构建新的 SPTE
    new_spte = make_spte(vcpu, sp, fault->slot, fault->gfn, fault->pfn,
                         fault->level, fault->spte_ptr,
                         fault->map_writable, true, true, true, NULL);

    if (fault->prefault)
        new_spte |= PT_ACCESSED_MASK;

    if (!fault->map_writable)
        new_spte &= ~PT_WRITABLE_MASK;

    // 原子写入 SPTE
    tdp_mmu_set_spte_atomic(vcpu, iter, new_spte);

    // ... 统计和跟踪 ...

    return ret;
}
```

**关键操作**：
1. **构建 SPTE**：调用 `make_spte()` 构建完整的 SPTE 值
   - 设置权限位（Present、Writable、User）
   - 设置内存类型（WB/UC）
   - 设置 Accessed/Dirty 位
   - 设置 PFN
2. **原子写入**：`tdp_mmu_set_spte_atomic()` 使用 `cmpxchg` 原子写入
3. **统计**：更新页错误统计

---

## 3. 关键数据结构

### 3.1 kvm_page_fault

```c
// arch/x86/kvm/mmu/mmu_internal.h:191
struct kvm_page_fault {
    /* 输入参数 */
    const gpa_t addr;           // 触发异常的 GPA
    const u32 error_code;       // 错误码
    const u8 access;            // 访问类型（读/写/执行）
    
    /* 计算得出 */
    const gfn_t gfn;            // Guest Frame Number
    struct kvm_memory_slot *slot; // 对应的 memslot
    
    /* 输出 */
    kvm_pfn_t pfn;              // 分配的物理页帧号
    hpa_t hva;                  // 宿主虚拟地址
    
    /* 级别调整 */
    int max_level;              // 最大支持的级别
    int req_level;              // 请求的级别
    int goal_level;             // 目标级别（4K/2M/1G）
    
    /* 大页相关 */
    bool huge_page_disallowed;  // 是否禁止大页
    bool prefault;              // 是否是预取
};
```

### 3.2 tdp_iter

```c
// arch/x86/kvm/mmu/tdp_iter.h
struct tdp_iter {
    /* 当前遍历位置 */
    tdp_ptep_t sptep;           // 当前 SPTE 指针
    u64 old_spte;               // 当前 SPTE 值
    int level;                  // 当前级别（1=4K, 2=2M, 3=1G, 4=512G）
    gfn_t gfn;                  // 当前 GFN
    
    /* 遍历范围 */
    gfn_t next;                 // 下一个要处理的 GFN
    int min_level;              // 最小级别
    int max_level;              // 最大级别
};
```

---

## 4. 锁机制

### 4.1 mmu_lock

```c
// arch/x86/kvm/mmu/mmu.c:4575
write_lock(&vcpu->kvm->mmu_lock);  // 获取写锁
// ... 修改 EPT 页表 ...
write_unlock(&vcpu->kvm->mmu_lock); // 释放写锁
```

**为什么需要 mmu_lock？**
- EPT 页表是共享资源，多个 vCPU 可能同时访问
- 修改页表必须原子化，防止并发冲突
- 使用 `write_lock` 而不是 `read_lock`，因为要修改页表

**为什么是 spinlock？**
- 持有时间短（只修改页表）
- 不允许睡眠（在 VM-Exit 处理中）
- 性能要求高

### 4.2 rcu_read_lock

```c
// arch/x86/kvm/mmu/tdp_mmu.c:1104
rcu_read_lock();
// ... 遍历和读取页表 ...
rcu_read_unlock();
```

**为什么需要 RCU？**
- 页表可能被其他线程释放
- RCU 保证遍历时页表不会被释放
- 允许并发读取，提高性能

---

## 5. 性能优化

### 5.1 快速路径

```c
// arch/x86/kvm/mmu/mmu.c:4575
r = fast_page_fault(vcpu, fault);
if (r != RET_PF_INVALID)
    return r;
```

**快速路径的条件**：
- SPTE 已存在，只是权限问题
- 可以无锁修复（只修改权限位）
- 不需要分配新页面

**快速路径的优势**：
- 不需要获取 `mmu_lock`
- 不需要分配物理页
- 性能提升 10-100 倍

### 5.2 大页支持

```c
// arch/x86/kvm/mmu/tdp_mmu.c:1104
kvm_mmu_hugepage_adjust(vcpu, fault);
```

**大页的优势**：
- 减少 EPT 页表层级（4 级 → 3 级或 2 级）
- 提高 TLB 命中率
- 性能提升 10-20%

**大页的条件**：
- GFN 对齐到 2M 或 1G 边界
- memslot 允许大页
- 没有冲突的权限需求

### 5.3 预取（Prefetch）

```c
// arch/x86/kvm/mmu/tdp_mmu.c:1017
if (fault->prefault)
    new_spte |= PT_ACCESSED_MASK;
```

**预取的优势**：
- 提前建立 EPT 映射
- 减少实际访问时的 VM-Exit
- 提高性能

---

## 6. 实践：跟踪 EPT Violation

### 6.1 使用 ftrace

```bash
# 挂载 debugfs
mount -t debugfs none /sys/kernel/debug

# 设置 ftrace 跟踪 KVM 页错误
echo function > /sys/kernel/debug/tracing/current_tracer
echo kvm_page_fault > /sys/kernel/debug/tracing/set_ftrace_filter
echo kvm_mmu_get_page >> /sys/kernel/debug/tracing/set_ftrace_filter
echo kvm_tdp_mmu_map >> /sys/kernel/debug/tracing/set_ftrace_filter

# 开启跟踪
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 启动/操作虚拟机
# ...

# 关闭跟踪并查看结果
echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace
```

### 6.2 使用 tracepoint

```bash
# 使用 KVM 提供的 tracepoint
echo kvm_entry > /sys/kernel/debug/tracing/set_event
echo kvm_exit >> /sys/kernel/debug/tracing/set_event
echo kvm_page_fault >> /sys/kernel/debug/tracing/set_event
echo kvm_mmu_paging_element >> /sys/kernel/debug/tracing/set_event

# 也可以使用 perf
perf record -e kvm:kvm_page_fault -a -g -- sleep 10
perf report
```

### 6.3 查看统计信息

```bash
# 查看 KVM 统计信息
cat /sys/kernel/debug/kvm/vcpu_stat

# 查看页错误统计
grep -E "pf_taken|pf_fixed|pf_emulate" /sys/kernel/debug/kvm/vcpu_stat
```

---

## 7. 常见问题

### 7.1 为什么有时需要模拟指令？

**场景**：
- MMIO 访问无法直接映射
- 需要 QEMU 模拟设备行为

**处理流程**：
```
MMIO 访问 → EPT Violation → KVM 检测到 MMIO
    ↓
返回 RET_PF_EMULATE → x86_emulate_instruction()
    ↓
转发给 QEMU（慢速路径）
    ↓
QEMU 模拟设备行为
```

### 7.2 为什么会发生 EPT Misconfiguration？

**原因**：
- EPT 页表包含保留位组合
- 通常是 KVM BUG

**处理**：
- 通常触发 kernel panic
- 检查 KVM 日志
- 可能是硬件或驱动问题

### 7.3 如何减少 EPT Violation？

**方法**：
1. **使用大页**：减少页表层级
2. **预取**：提前建立映射
3. **避免频繁换页**：减少内存压力
4. **优化内存访问模式**：减少随机访问

---

## 8. 脏页跟踪与写保护机制

脏页跟踪是虚拟机热迁移（Live Migration）的核心技术。KVM 需要精确记录哪些页面被修改过，以便只传输脏页到目标主机。

### 8.1 为什么需要脏页跟踪？

**热迁移场景**：

```
Host A (源)                          Host B (目标)
┌─────────────┐                      ┌─────────────┐
│  VM (运行中) │ ───── 迁移 ──────▶  │             │
│  内存: 4GB  │                      │  接收内存    │
└─────────────┘                      └─────────────┘
```

**问题**：VM 在迁移过程中还在运行，内存不断被修改

**解决方案**：脏页跟踪 + 增量同步

```
迭代 1: 复制所有 4GB 内存
迭代 2: 只复制第 1 轮期间修改的页面（假设 100MB）
迭代 3: 只复制第 2 轮期间修改的页面（假设 10MB）
...
迭代 N: 修改量很小，暂停 VM，完成迁移
```

### 8.2 两种脏页跟踪模式

KVM 根据 CPU 是否支持硬件 A/D 位，采用不同的跟踪策略：

| 模式 | 适用条件 | 工作原理 | 性能 |
|------|---------|---------|------|
| **硬件 A/D 位** | CPU 支持 EPT A/D（bit 21 of IA32_VMX_EPT_VPID_CAP） | CPU 自动设置 Dirty 位 | ⭐⭐⭐⭐⭐ 极快 |
| **软件写保护** | CPU 不支持硬件 A/D | 移除写权限，异常时记录 | ⭐⭐ 较慢 |

#### 检测硬件 A/D 支持

```c
// arch/x86/kvm/vmx/vmx.c:8447
if (!cpu_has_vmx_ept_ad_bits() || !enable_ept)
    enable_ept_ad_bits = 0;

// arch/x86/kvm/vmx/vmx.c:8509
if (enable_ept)
    kvm_mmu_set_ept_masks(enable_ept_ad_bits, ...);
```

#### 设置 SPTE 掩码

```c
// arch/x86/kvm/mmu/spte.c:431
void kvm_mmu_set_ept_masks(bool has_ad_bits, bool has_exec_only)
{
    shadow_user_mask        = VMX_EPT_READABLE_MASK;
    shadow_accessed_mask    = has_ad_bits ? VMX_EPT_ACCESS_BIT : 0ull;
    shadow_dirty_mask       = has_ad_bits ? VMX_EPT_DIRTY_BIT : 0ull;
    // ...
}
```

**关键点**：
- 如果 `has_ad_bits = true`：`shadow_dirty_mask = VMX_EPT_DIRTY_BIT`（bit 9）
- 如果 `has_ad_bits = false`：`shadow_dirty_mask = 0`（不使用硬件 Dirty 位）

### 8.3 硬件 A/D 位模式

#### 工作原理

```
Guest 写入页面
    ↓
CPU 自动设置 SPTE 的 Dirty 位（bit 9）
    ↓
同时，CPU 将 GPA 写入 PML buffer（如果启用 PML）
    ↓
无 VM-Exit！
```

#### SPTE 中的 A/D 位

```
SPTE bit 9-8（软件位，用于标记 A/D 模式）：
00 = SPTE_TDP_AD_ENABLED      → 使用硬件 A/D 位
01 = SPTE_TDP_AD_DISABLED     → 禁用 A/D 位
10 = SPTE_TDP_AD_WRPROT_ONLY  → 仅使用写保护

实际硬件 A/D 位（当 AD_ENABLED 时）：
bit 8: Accessed（访问位）
bit 9: Dirty（脏位）
```

#### 脏页记录

```c
// arch/x86/kvm/mmu/spte.c:272
if ((spte & PT_WRITABLE_MASK) && kvm_slot_dirty_track_enabled(slot)) {
    mark_page_dirty_in_slot(vcpu->kvm, slot, gfn);
}
```

**mark_page_dirty_in_slot() 实现**：

```c
// virt/kvm/kvm_main.c:3604
void mark_page_dirty_in_slot(struct kvm *kvm,
                             const struct kvm_memory_slot *memslot,
                             gfn_t gfn)
{
    if (memslot && kvm_slot_dirty_track_enabled(memslot)) {
        unsigned long rel_gfn = gfn - memslot->base_gfn;
        
        if (kvm->dirty_ring_size && vcpu)
            kvm_dirty_ring_push(vcpu, slot, rel_gfn);
        else if (memslot->dirty_bitmap)
            set_bit_le(rel_gfn, memslot->dirty_bitmap);  // 设置软件 bitmap
    }
}
```

### 8.4 PML（Page Modification Logging）

PML 是 Intel VT-x 提供的硬件特性，用于批量记录脏页。

#### PML 工作原理

```c
// arch/x86/kvm/vmx/vmx.c:6182
static void vmx_flush_pml_buffer(struct kvm_vcpu *vcpu)
{
    u64 *pml_buf;
    u16 pml_idx;

    pml_idx = vmcs_read16(GUEST_PML_INDEX);

    // PML buffer 为空，跳过
    if (pml_idx == (PML_ENTITY_NUM - 1))
        return;

    pml_buf = page_address(vmx->pml_pg);
    for (; pml_idx < PML_ENTITY_NUM; pml_idx++) {
        u64 gpa;

        gpa = pml_buf[pml_idx];  // 从硬件 buffer 读取 GPA
        kvm_vcpu_mark_page_dirty(vcpu, gpa >> PAGE_SHIFT);
    }

    // 重置 PML index
    vmcs_write16(GUEST_PML_INDEX, PML_ENTITY_NUM - 1);
}
```

**PML 机制**：
1. CPU 硬件维护一个 PML buffer（512 个条目）
2. 每次写入页面时，CPU 自动将 GPA 写入 buffer
3. Buffer 满时触发 VM-Exit（`EXIT_REASON_PML_FULL = 56`）
4. KVM 批量处理 buffer，调用 `mark_page_dirty_in_slot()`

**优势**：
- 批量处理，减少 VM-Exit 次数
- 与硬件 A/D 位结合使用，性能最佳

### 8.5 软件写保护模式

#### 开启脏页跟踪

```c
// arch/x86/kvm/mmu/mmu.c:6641
void kvm_mmu_slot_remove_write_access(struct kvm *kvm,
                                      const struct kvm_memory_slot *memslot,
                                      int start_level)
{
    if (tdp_mmu_enabled) {
        read_lock(&kvm->mmu_lock);
        kvm_tdp_mmu_wrprot_slot(kvm, memslot, start_level);
        read_unlock(&kvm->mmu_lock);
    }
}
```

**作用**：遍历 memslot 中的所有 SPTE，移除写权限

```c
// arch/x86/kvm/mmu/mmu.c:1205
static bool spte_write_protect(u64 *sptep, bool pt_protect)
{
    u64 spte = *sptep;

    if (!is_writable_pte(spte))
        return false;

    spte = spte & ~PT_WRITABLE_MASK;  // 移除写权限

    return mmu_spte_update(sptep, spte);
}
```

#### 写保护后的访问流程

```
Guest 写入被保护的页面
    ↓
EPT Violation（SPTE Writable=0）
    ↓
VM-Exit
    ↓
fast_page_fault()
    ↓
┌─────────────────────────────────────────────────┐
│ 1. 获取 SPTE（无锁）                            │
│                                                 │
│ 2. 恢复写权限                                    │
│    new_spte = spte | PT_WRITABLE_MASK           │
│                                                 │
│ 3. 原子更新（无锁）                              │
│    try_cmpxchg64(sptep, &spte, new_spte)        │
│                                                 │
│ 4. 记录脏页                                      │
│    mark_page_dirty_in_slot(kvm, slot, gfn)      │
└─────────────────────────────────────────────────┘
    ↓
VM-Resume
```

### 8.6 KVM_GET_DIRTY_LOG 流程

当 QEMU 调用 `ioctl(vm_fd, KVM_GET_DIRTY_LOG, &log)` 时：

```c
// virt/kvm/kvm_main.c:2204
static int kvm_get_dirty_log_protect(struct kvm *kvm, struct kvm_dirty_log *log)
{
    dirty_bitmap = memslot->dirty_bitmap;

    // Step 1: 同步硬件脏页日志到软件 bitmap
    kvm_arch_sync_dirty_log(kvm, memslot);
    // 触发所有 vCPU 的 VM-Exit
    // 在 VM-Exit 时调用 vmx_flush_pml_buffer()
    // 将 PML buffer 中的 GPA 写入 dirty_bitmap

    // Step 2: 复制 dirty_bitmap 给用户态
    KVM_MMU_LOCK(kvm);
    for (i = 0; i < n / sizeof(long); i++) {
        unsigned long mask;

        if (!dirty_bitmap[i])
            continue;

        mask = xchg(&dirty_bitmap[i], 0);  // 原子交换，清空 bitmap
        dirty_bitmap_buffer[i] = mask;

        offset = i * BITS_PER_LONG;
        kvm_arch_mmu_enable_log_dirty_pt_masked(kvm, memslot,
                                                offset, mask);
        // 清除 SPTE 的 Dirty 位或写权限
    }
    KVM_MMU_UNLOCK(kvm);

    // Step 3: 刷新 TLB
    if (flush)
        kvm_flush_remote_tlbs_memslot(kvm, memslot);

    // Step 4: 复制给用户态
    copy_to_user(log->dirty_bitmap, dirty_bitmap_buffer, n);
}
```

#### 清除 SPTE 的 Dirty 位

```c
// arch/x86/kvm/mmu/mmu.c:1233
static bool spte_clear_dirty(u64 *sptep)
{
    u64 spte = *sptep;

    KVM_MMU_WARN_ON(!spte_ad_enabled(spte));
    spte &= ~shadow_dirty_mask;  // 清除 Dirty 位
    return mmu_spte_update(sptep, spte);
}
```

### 8.7 硬件 A/D vs 软件写保护对比

| 操作 | 硬件 A/D 模式 | 软件写保护模式 |
|------|-------------|---------------|
| **开启脏页跟踪** | 清除 Dirty 位 | 移除写权限 |
| **Guest 读取** | EPT 命中（~1 ns） | EPT 命中（~1 ns） |
| **Guest 写入** | 硬件设置 Dirty 位（~1 ns） | EPT Violation + VM-Exit（~200 ns） |
| **记录脏页** | 检查 Dirty 位 | 在异常处理中记录 |
| **清除脏页** | 清除 Dirty 位 | 恢复写权限 |
| **性能** | ⭐⭐⭐⭐⭐ 极快 | ⭐⭐ 较慢 |

### 8.8 特殊情况：嵌套虚拟化

```c
// arch/x86/kvm/mmu/mmu_internal.h:148
static inline bool kvm_mmu_page_ad_need_write_protect(struct kvm_mmu_page *sp)
{
    /*
     * 当使用 EPT 页修改日志（PML）时，
     * CPU 脏日志中的 GPA 来自 L2 而不是 L1。
     * 因此，我们需要依赖写保护来记录脏页，
     * 这会绕过 PML，因为写入现在会导致 vmexit。
     */
    return kvm_x86_ops.cpu_dirty_log_size && sp->role.guest_mode;
}
```

**原因**：嵌套虚拟化（L1 运行 L2）时，PML 记录的 GPA 是 L2 的，而不是 L1 的，所以需要回退到写保护模式。

### 8.9 完整流程图

```
时间线：
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

T0: 开启脏页跟踪
    ioctl(KVM_SET_MEMORY_REGION, flags=KVM_MEM_LOG_DIRTY_PAGES)
        ↓
    kvm_mmu_slot_remove_write_access()
        ↓
    如果硬件 A/D 启用：
        清除所有 SPTE 的 Dirty 位
        SPTE = 0x...335 (Dirty=0, Writable=1)
    
    如果硬件 A/D 禁用：
        移除所有 SPTE 的写权限
        SPTE = 0x...335 (Writable=0)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

T1: Guest 写入页面
    Guest: *ptr = 0x12345678;
        ↓
    如果硬件 A/D 启用：
        CPU 自动设置 Dirty 位
        SPTE = 0x...337 (Dirty=1)
        同时，CPU 将 GPA 写入 PML buffer
        无 VM-Exit！
        
    如果硬件 A/D 禁用：
        EPT Violation (Writable=0)
            ↓
        VM-Exit
            ↓
        make_spte()
            ↓
        mark_page_dirty_in_slot()
            ↓
        set_bit_le(rel_gfn, dirty_bitmap)
            ↓
        恢复写权限
            ↓
        VM-Resume

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

T2: QEMU 获取脏页
    ioctl(KVM_GET_DIRTY_LOG, &log)
        ↓
    kvm_get_dirty_log_protect()
        ↓
    ┌─────────────────────────────────────────────────┐
    │ Step 1: kvm_arch_sync_dirty_log()               │
    │   触发所有 vCPU 的 VM-Exit                       │
    │   在 VM-Exit 时调用 vmx_flush_pml_buffer()      │
    │   将 PML buffer 中的 GPA 写入 dirty_bitmap      │
    └─────────────────────────────────────────────────┘
        ↓
    ┌─────────────────────────────────────────────────┐
    │ Step 2: 复制 dirty_bitmap                       │
    │   mask = xchg(&dirty_bitmap[i], 0)              │
    │   dirty_bitmap_buffer[i] = mask                 │
    │   （原子交换，清空 bitmap）                      │
    └─────────────────────────────────────────────────┘
        ↓
    ┌─────────────────────────────────────────────────┐
    │ Step 3: 清除 SPTE 的 Dirty 位                   │
    │   kvm_mmu_clear_dirty_pt_masked()               │
    │       ↓                                         │
    │   spte_clear_dirty()                            │
    │       spte &= ~shadow_dirty_mask                │
    │       （清除 Dirty 位）                          │
    └─────────────────────────────────────────────────┘
        ↓
    copy_to_user(log->dirty_bitmap, dirty_bitmap_buffer, n)
        ↓
    QEMU 收到脏页列表

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

### 8.10 关键要点

1. **脏页记录**：通过 `mark_page_dirty_in_slot()` 写入软件 `dirty_bitmap`
2. **硬件辅助**：PML buffer 批量记录，减少 VM-Exit
3. **收集脏页**：读取 `dirty_bitmap`，**不是**遍历 SPTE
4. **清除状态**：清除 SPTE 的 Dirty 位（或写权限）

**为什么不是遍历 SPTE？**
- 性能太差：需要遍历所有 SPTE（可能数百万个）
- `dirty_bitmap` 已经记录了所有脏页，直接读取即可
- O(1) 读取 bitmap vs O(N) 遍历 SPTE

**性能对比**：
- 硬件 A/D 位：写入延迟 ~1 ns（无 VM-Exit）
- 软件写保护：首次写入延迟 ~200 ns（VM-Exit）

---

## 9. Guest 内存属性控制与 IPAT

### 9.1 什么是 IPAT？

**IPAT (Ignore PAT)** 是 EPT 页表项中的第 6 位，用于控制是否忽略 Guest OS 的内存类型设置。

```
EPT PTE 格式：
[PFN:51-12] [MT:5-3] [IPAT:6] [其他标志]

IPAT = 0: 考虑 Guest 的 PAT/MTRR 设置
IPAT = 1: 忽略 Guest 的 PAT/MTRR 设置，使用 EPT 中设置的内存类型
```

### 9.2 KVM 如何决定是否设置 IPAT

通过查看 KVM 源码 `arch/x86/kvm/vmx/vmx.c:7676`：

```c
u8 vmx_get_mt_mask(struct kvm_vcpu *vcpu, gfn_t gfn, bool is_mmio)
{
    // 情况 1: MMIO 区域
    if (is_mmio)
        return MTRR_TYPE_UNCACHABLE << VMX_EPT_MT_EPTE_SHIFT;
        // 返回: UC (无 IPAT 位)
    
    // 情况 2: 无 VFIO 设备（默认情况）
    if (!kvm_arch_has_noncoherent_dma(vcpu->kvm))
        return (MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT) | VMX_EPT_IPAT_BIT;
        // 返回: WB + IPAT（IPAT = 1，忽略 Guest PAT）
    
    // 情况 3: 有 VFIO 非一致性 DMA 设备
    return (MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT);
    // 返回: WB（无 IPAT 位，IPAT = 0，考虑 Guest PAT）
}
```

### 9.3 三种场景总结

| 场景 | 条件 | 返回值 | IPAT 位 | Guest PAT |
|------|------|--------|---------|-----------|
| **MMIO 区域** | `is_mmio = true` | `UC` | N/A | 不适用（强制 UC） |
| **普通 VM** | `noncoherent_dma_count = 0` | `WB + IPAT` | **1** | **忽略** |
| **VFIO VM** | `noncoherent_dma_count > 0` | `WB` | **0** | **考虑** |

### 9.4 IPAT 默认行为

**✅ IPAT 默认开启**（对于大多数 VM）

#### 普通 VM（无 VFIO 设备）

```bash
# 检查是否有 VFIO 设备
cat /sys/kernel/debug/kvm/*/noncoherent_dma_count
# 输出: 0

# 结果
# IPAT = 1（开启）
# Guest 的 PAT/MTRR 设置被忽略
# 所有 RAM 区域使用 WB
```

**实际影响**：
- Guest OS 设置 `mtrr_add(..., "uncachable")` **不会生效**
- Guest OS 设置 `ioremap_wc(...)` **不会生效**
- 所有内存访问都使用 WB（可缓存）

#### VFIO VM（有 VFIO 非一致性 DMA 设备）

```bash
# 检查是否有 VFIO 设备
cat /sys/kernel/debug/kvm/*/noncoherent_dma_count
# 输出: 1（或更大）

# 结果
# IPAT = 0（关闭）
# Guest 的 PAT/MTRR 设置被考虑
# 可以设置 WC、UC 等内存类型
```

**实际影响**：
- GPU 直通时，`ioremap_wc(...)` **会生效**
- 可以正确设置 GPU 显存为 WC
- 内存类型由 Guest OS 控制

### 9.5 VFIO 设备如何触发 IPAT 关闭

当 VFIO 设备附加到 VM 时，KVM 会检查设备的一致性 DMA 能力：

```c
// virt/kvm/vfio.c:137
if (kv->noncoherent)
    kvm_arch_register_noncoherent_dma(dev->kvm);
```

**触发条件**：
- VFIO 设备不支持一致性 DMA（non-coherent）
- 例如：某些 GPU、网卡等设备

**结果**：
```c
// arch/x86/kvm/x86.c:13543
bool kvm_arch_has_noncoherent_dma(struct kvm *kvm)
{
    return atomic_read(&kvm->arch.noncoherent_dma_count);
}
```

当 `noncoherent_dma_count > 0` 时，KVM 关闭 IPAT，允许 Guest 控制内存类型。

### 9.6 为什么这样设计？

从代码注释中可以看到设计原则：

```c
/*
 * Force WB and ignore guest PAT if the VM does NOT have a non-coherent
 * device attached.  Letting the guest control memory types on Intel
 * CPUs may result in unexpected behavior, and so KVM's ABI is to trust
 * the guest to behave only as a last resort.
 */
```

**设计原则**：
1. **默认安全**：大多数情况下，不让 Guest 控制内存类型（避免错误）
2. **性能优先**：默认使用 WB，提供最佳缓存性能
3. **特殊情况**：只有在 VFIO 设备需要时，才信任 Guest

### 9.7 实际场景验证

#### 场景 1：普通 VM 尝试设置 UC

```bash
# Guest OS 尝试将 RAM 设置为 UC
mtrr_add(0x100000, 4096, "uncachable", 1);

# 实际效果：
# Guest PAT/MTRR: UC
# EPT: WB + IPAT（IPAT 位 = 忽略 Guest 设置）
# 最终: WB（Guest 设置被忽略）
```

**结果**：Guest 的设置**不起作用**。

#### 场景 2：GPU 直通设置 WC

```bash
# 附加 VFIO GPU 设备
virsh attach-device vm gpu-vfio.xml

# Guest 设置 GPU 显存为 WC
ioremap_wc(gpu_bar, size);

# 实际效果：
# Guest PAT: WC
# EPT: WB（无 IPAT 位）
# 最终: WC（考虑 Guest 设置）
```

**结果**：Guest 的设置**生效**。

#### 场景 3：MMIO 设备

```bash
# Guest 访问设备寄存器
void *mmio = ioremap(0xfexxxxxx, size);

# 实际效果：
# Guest PAT: UC（ioremap 默认）
# EPT: UC（强制）
# 最终: UC
```

**结果**：无论 Guest 怎么设置，MMIO 都是 UC。

### 9.8 如何验证当前 IPAT 状态

#### 在 Host 上检查

```bash
# 检查是否有 VFIO 设备
cat /sys/kernel/debug/kvm/*/noncoherent_dma_count

# 0 = 普通 VM（IPAT = 1）
# >0 = VFIO VM（IPAT = 0）

# 检查 VFIO 设备列表
ls /sys/bus/pci/drivers/vfio-pci/
```

#### 在 Guest 中测试

```c
// 测试代码
#include <stdio.h>
#include <sys/mman.h>

int main() {
    // 分配内存并设置为 WC
    void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    // 使用 perf 或其他工具检查实际内存类型
    // 如果 IPAT = 1，实际会使用 WB
    // 如果 IPAT = 0，会使用 WC
    
    return 0;
}
```

### 9.9 关键要点

1. **IPAT 默认开启**：大多数 VM 中，Guest 的内存类型设置被忽略
2. **VFIO 触发关闭**：只有附加 VFIO 非一致性 DMA 设备时，IPAT 才关闭
3. **MMIO 强制 UC**：无论 IPAT 状态，MMIO 区域始终使用 UC
4. **设计原则**：默认安全 + 性能优先，特殊情况下信任 Guest

**简单记忆**：
- **普通 VM** → IPAT = 1 → Guest 设置无效
- **VFIO VM** → IPAT = 0 → Guest 设置有效

---

## 10. 总结

EPT Violation 处理是 KVM 内存虚拟化的核心机制：

1. **硬件触发**：EPT 缺失或权限违规
2. **VM-Exit**：陷入 VMM（KVM）
3. **页表遍历**：从根到叶遍历 EPT 页表
4. **页面分配**：调用 GUP 分配物理页
5. **SPTE 构建**：设置权限、内存类型、PFN
6. **原子写入**：使用 cmpxchg 原子写入
7. **VM-Resume**：Guest 继续执行

**关键优化**：
- 快速路径（无锁修复）
- 大页支持（2M/1G）
- 预取（提前映射）
- 内存类型（WB/UC）

**性能影响**：
- 每次 EPT Violation 约 2-5 μs
- 使用大页可减少 50% Violation
- 快速路径可提升 10-100 倍性能
