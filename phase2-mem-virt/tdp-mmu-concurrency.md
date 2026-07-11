# TDP MMU 并发机制详解

> 多 vCPU 环境下的 EPT 页表并发管理

---

## 1. 为什么需要并发？

### 1.1 现代虚拟机的多 vCPU 场景

```
┌─────────────────────────────────────────────────────────┐
│                    虚拟机 (4 vCPU)                       │
│                                                          │
│  vCPU 0    vCPU 1    vCPU 2    vCPU 3                   │
│    │         │         │         │                       │
│    │         │         │         │                       │
│    ▼         ▼         ▼         ▼                       │
│  访问       访问       访问       访问                     │
│  0x10000   0x20000   0x30000   0x40000                  │
│    │         │         │         │                       │
│    └─────────┴─────────┴─────────┘                       │
│              │                                           │
│              ▼                                           │
│         EPT 页表（共享资源）                              │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

**问题**：多个 vCPU 同时触发 EPT Violation，需要并发更新 EPT 页表

**挑战**：
1. 多个 vCPU 可能同时修改同一个 EPT 页表项
2. 页表结构的修改必须原子化
3. 需要避免竞态条件和数据损坏
4. 性能要求高（不能频繁加锁）

---

## 2. TDP MMU 的并发设计

### 2.1 两种锁机制

TDP MMU 使用两种锁机制来平衡正确性和性能：

| 锁类型 | 用途 | 持有时间 | 性能影响 |
|--------|------|----------|----------|
| **mmu_lock** | 修改页表结构 | 短 | 中等 |
| **rcu_read_lock** | 读取页表 | 短 | 极低 |

### 2.2 mmu_lock（读写锁）

```c
// include/linux/kvm_host.h
struct kvm {
    // ...
    struct rw_semaphore mmu_lock;
    // ...
};
```

**用途**：
- 修改页表结构（创建/删除页表项）
- 修改 SPTE（Shadow Page Table Entry）
- 页表回收和清理

**特点**：
- 读写锁（read-write semaphore）
- 读模式：允许多个读者并发
- 写模式：独占访问

**使用场景**：
```c
// 写模式（独占）
write_lock(&kvm->mmu_lock);
// 修改页表结构
write_unlock(&kvm->mmu_lock);

// 读模式（共享）
read_lock(&kvm->mmu_lock);
// 读取页表
read_unlock(&kvm->mmu_lock);
```

### 2.3 rcu_read_lock（RCU 读锁）

```c
// 使用 RCU 保护页表读取
rcu_read_lock();
// 读取页表项
u64 spte = rcu_dereference(iter->sptep);
rcu_read_unlock();
```

**RCU（Read-Copy-Update）**：
- 无锁读取机制
- 允许多个读者并发读取
- 写者创建新副本，等待读者完成后释放旧副本
- 性能极高（读操作几乎无开销）

**关键 API**：
- `rcu_read_lock()`：进入 RCU 读临界区
- `rcu_read_unlock()`：退出 RCU 读临界区
- `rcu_dereference(p)`：安全地读取 RCU 保护的指针
- `call_rcu(head, func)`：延迟释放，等待所有读者完成

---

## 3. 原子操作：cmpxchg

### 3.1 为什么需要原子操作？

**场景**：两个 vCPU 同时修改同一个 SPTE

```
vCPU 0:                          vCPU 1:
────────                         ────────
读取 SPTE: 0x100                 读取 SPTE: 0x100
计算新值: 0x200                  计算新值: 0x300
写入 SPTE: 0x200                 写入 SPTE: 0x300
                                  ↓
                                 SPTE = 0x300（vCPU 0 的修改丢失！）
```

**问题**：vCPU 0 的修改被覆盖，导致数据丢失

**解决方案**：使用原子操作 `cmpxchg`

### 3.2 cmpxchg 原理

```c
// cmpxchg(ptr, old, new) 伪代码
u64 cmpxchg(u64 *ptr, u64 old, u64 new) {
    u64 current = *ptr;
    if (current == old) {
        *ptr = new;  // 修改成功
    }
    return current;  // 返回当前值
}
```

**特点**：
- 原子操作（硬件级别保证）
- 比较并交换（Compare-And-Swap）
- 如果当前值等于期望值，则更新
- 返回实际的值（无论是否更新）

### 3.3 try_cmpxchg64 实现

```c
// arch/x86/kvm/mmu/tdp_mmu.c:553
static inline int __must_check __tdp_mmu_set_spte_atomic(
    struct tdp_iter *iter,
    u64 new_spte)
{
    u64 *sptep = rcu_dereference(iter->sptep);

    // 原子操作：比较并交换
    if (!try_cmpxchg64(sptep, &iter->old_spte, new_spte))
        return -EBUSY;  // 失败，被其他 CPU 修改

    return 0;  // 成功
}
```

**工作流程**：
```
1. 读取当前 SPTE 值到 iter->old_spte
2. 计算新值 new_spte
3. 调用 try_cmpxchg64(sptep, &iter->old_spte, new_spte)
   - 如果 *sptep == iter->old_spte：
     - 更新 *sptep = new_spte
     - 返回 true（成功）
   - 如果 *sptep != iter->old_spte：
     - 更新 iter->old_spte = *sptep（获取最新值）
     - 返回 false（失败）
4. 如果失败，可以重试（使用新的 old_spte）
```

### 3.4 并发修改示例

```
场景：vCPU 0 和 vCPU 1 同时修改 SPTE

初始状态：SPTE = 0x100

vCPU 0:                          vCPU 1:
────────                         ────────
读取 SPTE: old = 0x100           读取 SPTE: old = 0x100
计算 new = 0x200                 计算 new = 0x300
                                  ↓
try_cmpxchg64(&SPTE, &old, 0x200)
  - *SPTE (0x100) == old (0x100) ✓
  - 更新 *SPTE = 0x200
  - 返回 true ✓
                                 try_cmpxchg64(&SPTE, &old, 0x300)
                                   - *SPTE (0x200) != old (0x100) ✗
                                   - 更新 old = 0x200
                                   - 返回 false ✗
                                  ↓
                                 重试：
                                 读取 SPTE: old = 0x200
                                 计算 new = 0x300
                                 try_cmpxchg64(&SPTE, &old, 0x300)
                                   - *SPTE (0x200) == old (0x200) ✓
                                   - 更新 *SPTE = 0x300
                                   - 返回 true ✓

最终状态：SPTE = 0x300（vCPU 1 的修改成功）
```

**关键**：没有数据丢失，两个 vCPU 的修改都正确应用

---

## 4. TDP MMU 并发流程

### 4.1 页表遍历（读取）

```c
// arch/x86/kvm/mmu/tdp_mmu.c:1104
int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    struct tdp_iter iter;
    
    // 使用 RCU 保护页表遍历
    rcu_read_lock();
    
    // 遍历页表
    tdp_mmu_for_each_pte(iter, mmu, fault->gfn, fault->gfn + 1) {
        // 安全地读取 SPTE
        u64 old_spte = rcu_dereference(iter.old_spte);
        
        // 处理页表项
        // ...
    }
    
    rcu_read_unlock();
}
```

**特点**：
- 使用 `rcu_read_lock()` 保护
- 多个 vCPU 可以并发遍历
- 无锁读取，性能极高

### 4.2 页表修改（写入）

```c
// arch/x86/kvm/mmu/tdp_mmu.c:576
static inline int tdp_mmu_set_spte_atomic(
    struct kvm *kvm,
    struct tdp_iter *iter,
    u64 new_spte)
{
    // 需要持有 mmu_lock（读模式）
    lockdep_assert_held_read(&kvm->mmu_lock);
    
    // 原子更新 SPTE
    int ret = __tdp_mmu_set_spte_atomic(iter, new_spte);
    if (ret)
        return ret;
    
    // 处理变更
    handle_changed_spte(kvm, iter->as_id, iter->gfn,
                       iter->old_spte, new_spte, iter->level, true);
    
    return 0;
}
```

**特点**：
- 需要持有 `mmu_lock`（读模式即可）
- 使用 `cmpxchg` 原子更新
- 多个 vCPU 可以并发修改不同的 SPTE

### 4.3 页表结构修改

```c
// 创建新的页表项
static int tdp_mmu_link_sp(
    struct kvm *kvm,
    struct tdp_iter *iter,
    struct kvm_mmu_page *sp,
    bool shared)
{
    u64 spte = make_nonleaf_spte(sp->spt, !kvm_ad_enabled());
    int ret = 0;
    
    if (shared) {
        // 原子更新（需要 mmu_lock 读锁）
        ret = tdp_mmu_set_spte_atomic(kvm, iter, spte);
        if (ret)
            return ret;
    } else {
        // 非原子更新（需要 mmu_lock 写锁）
        tdp_mmu_iter_set_spte(kvm, iter, spte);
    }
    
    return 0;
}
```

**特点**：
- 共享模式（shared=true）：使用原子操作，只需要读锁
- 独占模式（shared=false）：直接写入，需要写锁

---

## 5. 快速页错误路径（Fast Page Fault）

### 5.1 为什么需要快速路径？

**问题**：每次 EPT Violation 都需要获取 `mmu_lock`，性能开销大

**场景**：脏页跟踪后的首次写入
```
开启脏页跟踪 → 移除所有 SPTE 写权限
    ↓
Guest 写入 → EPT Violation
    ↓
只需要恢复写权限，不需要修改页表结构
    ↓
可以无锁修复！
```

### 5.2 快速路径实现

```c
// arch/x86/kvm/mmu/mmu.c:3466
static int fast_page_fault(struct kvm_vcpu *vcpu, 
                           struct kvm_page_fault *fault)
{
    struct kvm_mmu_page *sp;
    int ret = RET_PF_INVALID;
    u64 spte;
    u64 *sptep;
    
    // 检查是否可以走快速路径
    if (!page_fault_can_be_fast(vcpu->kvm, fault))
        return ret;
    
    // 进入无锁遍历模式
    walk_shadow_page_lockless_begin(vcpu);
    
    do {
        u64 new_spte;
        
        // 获取 SPTE 指针（无锁）
        sptep = kvm_tdp_mmu_fast_pf_get_last_sptep(vcpu, fault->gfn, &spte);
        
        // 检查 SPTE 是否存在
        if (!is_shadow_present_pte(spte))
            break;
        
        // 检查是否已经允许访问
        if (is_access_allowed(fault, spte)) {
            ret = RET_PF_SPURIOUS;  // 虚假异常
            break;
        }
        
        new_spte = spte;
        
        // 如果是写访问，恢复写权限
        if (fault->write && is_mmu_writable_spte(spte)) {
            new_spte |= PT_WRITABLE_MASK;
        }
        
        // 验证新 SPTE 是否允许访问
        if (new_spte == spte || !is_access_allowed(fault, new_spte))
            break;
        
        // 原子更新 SPTE（无锁）
        if (fast_pf_fix_direct_spte(vcpu, fault, sptep, spte, new_spte)) {
            ret = RET_PF_FIXED;
            break;
        }
        
    } while (++retry_count <= 4);
    
    walk_shadow_page_lockless_end(vcpu);
    
    return ret;
}
```

### 5.3 无锁修复实现

```c
// arch/x86/kvm/mmu/mmu.c:3403
static bool fast_pf_fix_direct_spte(
    struct kvm_vcpu *vcpu,
    struct kvm_page_fault *fault,
    u64 *sptep, u64 old_spte, u64 new_spte)
{
    // 原子更新 SPTE（无锁）
    if (!try_cmpxchg64(sptep, &old_spte, new_spte))
        return false;  // 失败，被其他 CPU 修改
    
    // 记录脏页
    if (is_writable_pte(new_spte) && !is_writable_pte(old_spte))
        mark_page_dirty_in_slot(vcpu->kvm, fault->slot, fault->gfn);
    
    return true;  // 成功
}
```

**特点**：
- 不需要 `mmu_lock`
- 使用 `cmpxchg` 原子更新
- 性能提升 10-100 倍

### 5.4 快速路径 vs 慢速路径对比

| 特性 | 快速路径 | 慢速路径 |
|------|---------|---------|
| **触发条件** | SPTE 存在，仅权限问题 | 需要分配页面或修改页表 |
| **锁机制** | 无锁（cmpxchg） | mmu_lock |
| **性能** | ~200 ns | 2-5 μs |
| **适用场景** | 脏页跟踪后的首次写入 | 首次访问、大页拆分等 |

---

## 6. 并发场景示例

### 6.1 场景 1：多个 vCPU 同时访问不同页面

```
vCPU 0: 访问 GPA 0x10000
vCPU 1: 访问 GPA 0x20000
vCPU 2: 访问 GPA 0x30000

时间线：
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

T0: vCPU 0, 1, 2 同时触发 EPT Violation

T1: 三个 vCPU 并发执行 kvm_tdp_mmu_map()
    ┌─────────────────────────────────────────┐
    │ rcu_read_lock()                         │
    │ 遍历页表（无锁）                         │
    │ rcu_read_unlock()                       │
    └─────────────────────────────────────────┘

T2: 三个 vCPU 并发修改不同的 SPTE
    ┌─────────────────────────────────────────┐
    │ vCPU 0: 修改 SPTE[0x10000]              │
    │   read_lock(&mmu_lock)                  │
    │   try_cmpxchg64(&SPTE[0x10000], ...)    │
    │   ✓ 成功                                │
    │   read_unlock(&mmu_lock)                │
    │                                         │
    │ vCPU 1: 修改 SPTE[0x20000]              │
    │   read_lock(&mmu_lock)                  │
    │   try_cmpxchg64(&SPTE[0x20000], ...)    │
    │   ✓ 成功                                │
    │   read_unlock(&mmu_lock)                │
    │                                         │
    │ vCPU 2: 修改 SPTE[0x30000]              │
    │   read_lock(&mmu_lock)                  │
    │   try_cmpxchg64(&SPTE[0x30000], ...)    │
    │   ✓ 成功                                │
    │   read_unlock(&mmu_lock)                │
    └─────────────────────────────────────────┘

T3: 三个 vCPU 并发完成，无冲突

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

**关键点**：
- 三个 vCPU 修改不同的 SPTE
- 使用 `read_lock`（共享锁）
- 无冲突，完全并发

### 6.2 场景 2：多个 vCPU 同时访问同一页面

```
vCPU 0: 访问 GPA 0x10000
vCPU 1: 访问 GPA 0x10000（同一页面）

时间线：
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

T0: vCPU 0 和 vCPU 1 同时触发 EPT Violation

T1: 两个 vCPU 并发执行 kvm_tdp_mmu_map()
    ┌─────────────────────────────────────────┐
    │ vCPU 0:                                 │
    │   读取 SPTE: old = 0x100                │
    │   计算 new = 0x200                      │
    │                                         │
    │ vCPU 1:                                 │
    │   读取 SPTE: old = 0x100                │
    │   计算 new = 0x300                      │
    └─────────────────────────────────────────┘

T2: 两个 vCPU 竞争修改同一个 SPTE
    ┌─────────────────────────────────────────┐
    │ vCPU 0:                                 │
    │   try_cmpxchg64(&SPTE, &old, 0x200)     │
    │   - *SPTE (0x100) == old (0x100) ✓      │
    │   - 更新 *SPTE = 0x200                  │
    │   - 返回 true ✓                         │
    │                                         │
    │ vCPU 1:                                 │
    │   try_cmpxchg64(&SPTE, &old, 0x300)     │
    │   - *SPTE (0x200) != old (0x100) ✗      │
    │   - 更新 old = 0x200                    │
    │   - 返回 false ✗                        │
    └─────────────────────────────────────────┘

T3: vCPU 1 重试
    ┌─────────────────────────────────────────┐
    │ vCPU 1:                                 │
    │   重新读取 SPTE: old = 0x200            │
    │   重新计算 new = 0x300                  │
    │   try_cmpxchg64(&SPTE, &old, 0x300)     │
    │   - *SPTE (0x200) == old (0x200) ✓      │
    │   - 更新 *SPTE = 0x300                  │
    │   - 返回 true ✓                         │
    └─────────────────────────────────────────┘

T4: 两个 vCPU 都完成，无数据丢失

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

**关键点**：
- 两个 vCPU 竞争同一个 SPTE
- `cmpxchg` 保证原子性
- 失败的 vCPU 自动重试
- 无数据丢失

### 6.3 场景 3：快速路径修复

```
背景：已开启脏页跟踪，所有 SPTE 被移除写权限

vCPU 0: 写入 GPA 0x10000
vCPU 1: 写入 GPA 0x20000

时间线：
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

T0: vCPU 0 和 vCPU 1 同时触发 EPT Violation
    SPTE[0x10000] = 0x335（Writable=0）
    SPTE[0x20000] = 0x335（Writable=0）

T1: 两个 vCPU 进入快速路径
    ┌─────────────────────────────────────────┐
    │ fast_page_fault()                       │
    │   - 不需要 mmu_lock                     │
    │   - 无锁遍历页表                        │
    └─────────────────────────────────────────┘

T2: 两个 vCPU 并发修复 SPTE
    ┌─────────────────────────────────────────┐
    │ vCPU 0:                                 │
    │   读取 SPTE: old = 0x335                │
    │   计算 new = 0x337（恢复写权限）         │
    │   try_cmpxchg64(&SPTE[0x10000], ...)    │
    │   ✓ 成功                                │
    │   mark_page_dirty()                     │
    │                                         │
    │ vCPU 1:                                 │
    │   读取 SPTE: old = 0x335                │
    │   计算 new = 0x337（恢复写权限）         │
    │   try_cmpxchg64(&SPTE[0x20000], ...)    │
    │   ✓ 成功                                │
    │   mark_page_dirty()                     │
    └─────────────────────────────────────────┘

T3: 两个 vCPU 都完成，耗时 ~200 ns

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

**关键点**：
- 快速路径不需要 `mmu_lock`
- 使用 `cmpxchg` 原子修复
- 性能提升 10-100 倍

---

## 7. RCU 在 TDP MMU 中的应用

### 7.1 RCU 的基本原理

```
RCU（Read-Copy-Update）机制：

读操作（无锁）：
  rcu_read_lock()
  读取数据
  rcu_read_unlock()

写操作（创建新副本）：
  分配新内存
  复制旧数据到新内存
  修改新内存
  原子更新指针指向新内存
  等待所有读者完成（grace period）
  释放旧内存
```

### 7.2 TDP MMU 中的 RCU 使用

```c
// 读取页表（无锁）
rcu_read_lock();
u64 *sptep = rcu_dereference(iter->sptep);
u64 spte = *sptep;
rcu_read_unlock();

// 修改页表（原子更新）
write_lock(&kvm->mmu_lock);
u64 new_spte = ...;
try_cmpxchg64(sptep, &old_spte, new_spte);
write_unlock(&kvm->mmu_lock);

// 释放页表（延迟释放）
call_rcu(&sp->rcu_head, tdp_mmu_free_sp_rcu_callback);
```

### 7.3 RCU 的优势

| 特性 | 传统锁 | RCU |
|------|--------|-----|
| **读操作** | 需要加锁 | 无锁 |
| **并发读** | 串行化 | 完全并发 |
| **性能** | 中等 | 极高 |
| **适用场景** | 读写频繁 | 读多写少 |

**TDP MMU 的特点**：
- 读操作远多于写操作
- 页表遍历频繁
- 适合使用 RCU

---

## 8. 性能优化策略

### 8.1 减少锁竞争

**策略 1：使用细粒度锁**
```c
// 不好：全局锁
write_lock(&kvm->mmu_lock);
修改 SPTE
write_unlock(&kvm->mmu_lock);

// 好：使用原子操作
try_cmpxchg64(sptep, &old, new);
```

**策略 2：使用读锁代替写锁**
```c
// 不好：写锁（独占）
write_lock(&kvm->mmu_lock);
修改 SPTE
write_unlock(&kvm->mmu_lock);

// 好：读锁（共享）
read_lock(&kvm->mmu_lock);
try_cmpxchg64(sptep, &old, new);
read_unlock(&kvm->mmu_lock);
```

### 8.2 快速路径优化

**适用场景**：
- SPTE 已存在
- 仅需修改权限位
- 不需要修改页表结构

**性能提升**：
- 无锁：~200 ns
- 有锁：2-5 μs
- **提升 10-25 倍**

### 8.3 批量操作

**场景**：清除脏页日志

```c
// 批量清除多个 SPTE 的 Dirty 位
for_each_set_bit(gfn, mask, BITS_PER_LONG) {
    tdp_mmu_clear_dirty_pt_masked(kvm, slot, gfn, mask, false);
}
```

**优势**：
- 减少锁获取次数
- 批量处理提高效率

---

## 9. 关键数据结构

### 9.1 kvm_mmu_page

```c
struct kvm_mmu_page {
    struct list_head link;           // 链表节点
    struct hlist_node hash_link;     // 哈希表节点
    
    bool tdp_mmu_page;               // 是否是 TDP MMU 页
    bool unsync;                     // 是否未同步
    
    union kvm_mmu_page_role role;    // 页角色
    gfn_t gfn;                       // Guest Frame Number
    
    u64 *spt;                        // Shadow Page Table
    
    union {
        int root_count;              // 根页引用计数
        refcount_t tdp_mmu_root_count;
    };
    
    struct rcu_head rcu_head;        // RCU 释放头
    // ...
};
```

### 9.2 tdp_iter

```c
struct tdp_iter {
    // 当前遍历位置
    tdp_ptep_t sptep;                // 当前 SPTE 指针
    u64 old_spte;                    // 当前 SPTE 值
    int level;                       // 当前级别（1=4K, 2=2M, 3=1G）
    gfn_t gfn;                       // 当前 GFN
    
    // 遍历范围
    gfn_t next;                      // 下一个要处理的 GFN
    int min_level;                   // 最小级别
    int max_level;                   // 最大级别
};
```

---

## 10. 总结

### 10.1 TDP MMU 并发机制要点

1. **两种锁机制**：
   - `mmu_lock`：修改页表结构
   - `rcu_read_lock`：读取页表

2. **原子操作**：
   - `cmpxchg`：比较并交换
   - 保证并发修改的正确性
   - 避免数据丢失

3. **快速路径**：
   - 无锁修复权限问题
   - 性能提升 10-100 倍

4. **RCU 机制**：
   - 无锁读取
   - 延迟释放
   - 适合读多写少场景

### 10.2 性能对比

| 场景 | 延迟 | 说明 |
|------|------|------|
| 快速路径（无锁） | ~200 ns | 权限修复 |
| 慢速路径（有锁） | 2-5 μs | 页表修改 |
| RCU 读取 | ~50 ns | 页表遍历 |

### 10.3 关键函数

| 函数 | 作用 |
|------|------|
| `tdp_mmu_set_spte_atomic()` | 原子更新 SPTE |
| `fast_page_fault()` | 快速页错误路径 |
| `fast_pf_fix_direct_spte()` | 无锁修复 SPTE |
| `try_cmpxchg64()` | 原子比较并交换 |
| `rcu_read_lock()` | RCU 读锁 |
| `call_rcu()` | 延迟释放 |

---

## 参考资料

- Linux Kernel Documentation: RCU
- Intel SDM Vol 3: Extended Page Tables
- Linux Kernel Source: `arch/x86/kvm/mmu/tdp_mmu.c`
- Linux Kernel Source: `arch/x86/kvm/mmu/mmu.c`
