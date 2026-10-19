# Complete EPT Establishment Path in KVM

This document traces the complete call chain when a Guest executes mmap and then first accesses memory, triggering an EPT violation that leads to EPT entry creation.

## Overview

When a Guest vCPU accesses a virtual address for the first time after mmap:
1. Guest page tables may or may not have a mapping
2. EPT (Extended Page Table) has no entry for the GPA
3. Hardware triggers an EPT violation VM-exit
4. KVM handles the fault, translates GPA → PFN
5. KVM creates the EPT entry mapping GPA → PFN
6. Guest resumes execution

## Complete Call Chain

### 1. Entry Point: EPT Violation Handler

**File:** `arch/x86/kvm/vmx/vmx.c:5782-5813`
```c
static int handle_ept_violation(struct kvm_vcpu *vcpu)
{
    unsigned long exit_qualification = vmx_get_exit_qual(vcpu);
    gpa_t gpa;
    
    // Read the guest physical address that caused the fault
    gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);
    trace_kvm_page_fault(vcpu, gpa, exit_qualification);
    
    // Check for illegal addresses
    if (unlikely(allow_smaller_maxphyaddr && !kvm_vcpu_is_legal_gpa(vcpu, gpa)))
        return kvm_emulate_instruction(vcpu, 0);
    
    return __vmx_handle_ept_violation(vcpu, gpa, exit_qualification);
}
```

**Exit reason dispatch:** `arch/x86/kvm/vmx/vmx.c:6131`
```c
[EXIT_REASON_EPT_VIOLATION] = handle_ept_violation,
```

### 2. EPT Violation to Page Fault Conversion

**File:** `arch/x86/kvm/vmx/common.h:9-32`
```c
static inline int __vmx_handle_ept_violation(struct kvm_vcpu *vcpu, gpa_t gpa,
                                             unsigned long exit_qualification)
{
    u64 error_code;
    
    // Convert EPT exit qualification to page fault error code
    error_code = (exit_qualification & EPT_VIOLATION_ACC_READ) ? PFERR_USER_MASK : 0;
    error_code |= (exit_qualification & EPT_VIOLATION_ACC_WRITE) ? PFERR_WRITE_MASK : 0;
    error_code |= (exit_qualification & EPT_VIOLATION_ACC_INSTR) ? PFERR_FETCH_MASK : 0;
    error_code |= (exit_qualification & EPT_VIOLATION_RWX_MASK) ? PFERR_PRESENT_MASK : 0;
    
    if (exit_qualification & EPT_VIOLATION_GVA_IS_VALID)
        error_code |= (exit_qualification & EPT_VIOLATION_GVA_TRANSLATED) ?
                      PFERR_GUEST_FINAL_MASK : PFERR_GUEST_PAGE_MASK;
    
    return kvm_mmu_page_fault(vcpu, gpa, error_code, NULL, 0);
}
```

### 3. Main Page Fault Handler

**File:** `arch/x86/kvm/mmu/mmu.c:6106-6168`
```c
int noinline kvm_mmu_page_fault(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa, u64 error_code,
                                void *insn, int insn_len)
{
    int r, emulation_type = EMULTYPE_PF;
    bool direct = vcpu->arch.mmu->root_role.direct;
    
    // Handle reserved bits faults (MMIO)
    r = RET_PF_INVALID;
    if (unlikely(error_code & PFERR_RSVD_MASK)) {
        r = handle_mmio_page_fault(vcpu, cr2_or_gpa, direct);
        if (r == RET_PF_EMULATE)
            goto emulate;
    }
    
    if (r == RET_PF_INVALID) {
        vcpu->stat.pf_taken++;
        
        // Main page fault handling path
        r = kvm_mmu_do_page_fault(vcpu, cr2_or_gpa, error_code, false,
                                  &emulation_type, NULL);
    }
    
    // Handle write-protected pages
    if (r == RET_PF_WRITE_PROTECTED)
        r = kvm_mmu_write_protect_fault(vcpu, cr2_or_gpa, error_code,
                                        &emulation_type);
    
    // Update statistics
    if (r == RET_PF_FIXED)
        vcpu->stat.pf_fixed++;
    else if (r == RET_PF_EMULATE)
        vcpu->stat.pf_emulate++;
    
    if (r != RET_PF_EMULATE)
        return 1;  // Return to guest
    
emulate:
    return x86_emulate_instruction(vcpu, cr2_or_gpa, emulation_type, insn, insn_len);
}
```

### 4. Page Fault Dispatch (Inline Function)

**File:** `arch/x86/kvm/mmu/mmu_internal.h:293-342`
```c
static inline int kvm_mmu_do_page_fault(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa,
                                        u64 err, bool prefetch,
                                        int *emulation_type, u8 *level)
{
    struct kvm_page_fault fault = {
        .addr = cr2_or_gpa,
        .error_code = err,
        .exec = err & PFERR_FETCH_MASK,
        .write = err & PFERR_WRITE_MASK,
        .present = err & PFERR_PRESENT_MASK,
        .rsvd = err & PFERR_RSVD_MASK,
        .user = err & PFERR_USER_MASK,
        .prefetch = prefetch,
        .is_tdp = likely(vcpu->arch.mmu->page_fault == kvm_tdp_page_fault),
        // ... other fields
        .pfn = KVM_PFN_ERR_FAULT,
        .hva = KVM_HVA_ERR_BAD,
    };
    int r;
    
    // For direct MMU (TDP), extract GFN from GPA
    if (vcpu->arch.mmu->root_role.direct) {
        fault.gfn = fault.addr >> PAGE_SHIFT;
        fault.slot = kvm_vcpu_gfn_to_memslot(vcpu, fault.gfn);
    }
    
    // Dispatch to TDP page fault handler
    if (IS_ENABLED(CONFIG_MITIGATION_RETPOLINE) && fault.is_tdp)
        r = kvm_tdp_page_fault(vcpu, &fault);
    else
        r = vcpu->arch.mmu->page_fault(vcpu, &fault);
    
    // ... error handling
    return r;
}
```

### 5. TDP Page Fault Handler

**File:** `arch/x86/kvm/mmu/mmu.c:4726-4734`
```c
int kvm_tdp_page_fault(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
#ifdef CONFIG_X86_64
    if (tdp_mmu_enabled)
        return kvm_tdp_mmu_page_fault(vcpu, fault);
#endif
    
    return direct_page_fault(vcpu, fault);
}
```

### 6. TDP MMU Page Fault Handler

**File:** `arch/x86/kvm/mmu/mmu.c:4673-4708`
```c
static int kvm_tdp_mmu_page_fault(struct kvm_vcpu *vcpu,
                                  struct kvm_page_fault *fault)
{
    kvm_pfn_t orig_pfn;
    int r;
    
    // Check for page tracking (write protection)
    if (page_fault_handle_page_track(vcpu, fault))
        return RET_PF_WRITE_PROTECTED;
    
    // Try fast page fault path (lockless)
    r = fast_page_fault(vcpu, fault);
    if (r != RET_PF_INVALID)
        return r;
    
    // Top up memory caches for page table allocations
    r = mmu_topup_memory_caches(vcpu, false);
    if (r)
        return r;
    
    // Translate GPA to PFN
    r = kvm_faultin_pfn(vcpu, fault, ACC_ALL);
    if (r != RET_PF_CONTINUE)
        return r;
    
    orig_pfn = fault->pfn;
    
    r = RET_PF_RETRY;
    read_lock(&vcpu->kvm->mmu_lock);
    
    if (is_page_fault_stale(vcpu, fault))
        goto out_unlock;
    
    // Create the EPT entry
    r = kvm_tdp_mmu_map(vcpu, fault);
    
out_unlock:
    read_unlock(&vcpu->kvm->mmu_lock);
    kvm_release_pfn_clean(orig_pfn);
    return r;
}
```

### 7. Fast Page Fault Path (Lockless Optimization)

**File:** `arch/x86/kvm/mmu/mmu.c:3466+`
```c
static int fast_page_fault(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    // Attempts to fix the fault without taking mmu_lock
    // Only works for simple permission fixups on existing mappings
    // For new mappings, returns RET_PF_INVALID and falls through to slow path
}
```

### 8. GPA to PFN Translation

#### 8.1 Fault in PFN

**File:** `arch/x86/kvm/mmu/mmu.c:4434-4541`
```c
static int kvm_faultin_pfn(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault,
                           unsigned int access)
{
    struct kvm_memory_slot *slot = fault->slot;
    int ret;
    
    fault->mmu_seq = vcpu->kvm->mmu_invalidate_seq;
    smp_rmb();
    
    // Check for private memory
    if (fault->is_private != kvm_mem_is_private(vcpu->kvm, fault->gfn)) {
        kvm_mmu_prepare_memory_fault_exit(vcpu, fault);
        return -EFAULT;
    }
    
    // Handle missing memory slot
    if (unlikely(!slot))
        return kvm_handle_noslot_fault(vcpu, fault, access);
    
    // Check for concurrent invalidation
    if (mmu_invalidate_retry_gfn_unsafe(vcpu->kvm, fault->mmu_seq, fault->gfn))
        return RET_PF_RETRY;
    
    // Translate GFN to PFN
    ret = __kvm_faultin_pfn(vcpu, fault);
    if (ret != RET_PF_CONTINUE)
        return ret;
    
    if (unlikely(is_error_pfn(fault->pfn)))
        return kvm_handle_error_pfn(vcpu, fault);
    
    // Check for invalidation again
    if (mmu_invalidate_retry_gfn_unsafe(vcpu->kvm, fault->mmu_seq, fault->gfn)) {
        kvm_release_pfn_clean(fault->pfn);
        return RET_PF_RETRY;
    }
    
    return RET_PF_CONTINUE;
}
```

#### 8.2 Internal PFN Resolution

**File:** `arch/x86/kvm/mmu/mmu.c:4398-4431`
```c
static int __kvm_faultin_pfn(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    bool async;
    
    if (fault->is_private)
        return kvm_faultin_pfn_private(vcpu, fault);
    
    async = false;
    // First attempt: translate GFN to PFN
    fault->pfn = __gfn_to_pfn_memslot(fault->slot, fault->gfn, false, false,
                                      &async, fault->write,
                                      &fault->map_writable, &fault->hva);
    if (!async)
        return RET_PF_CONTINUE;  // Success
    
    // If async is needed and allowed, set up async page fault
    if (!fault->prefetch && kvm_can_do_async_pf(vcpu)) {
        // ... async page fault setup
    }
    
    // Second attempt: allow sleeping
    fault->pfn = __gfn_to_pfn_memslot(fault->slot, fault->gfn, false, true,
                                      NULL, fault->write,
                                      &fault->map_writable, &fault->hva);
    
    return RET_PF_CONTINUE;
}
```

#### 8.3 GFN to PFN Translation

**File:** `virt/kvm/kvm_main.c:3005-3030`
```c
kvm_pfn_t __gfn_to_pfn_memslot(const struct kvm_memory_slot *slot, gfn_t gfn,
                               bool atomic, bool interruptible, bool *async,
                               bool write_fault, bool *writable, hva_t *hva)
{
    // Step 1: Convert GFN to HVA (Host Virtual Address)
    unsigned long addr = __gfn_to_hva_many(slot, gfn, NULL, write_fault);
    
    if (hva)
        *hva = addr;
    
    if (kvm_is_error_hva(addr)) {
        if (writable)
            *writable = false;
        return addr == KVM_HVA_ERR_RO_BAD ? KVM_PFN_ERR_RO_FAULT : KVM_PFN_NOSLOT;
    }
    
    // Do not map writable pfn in the readonly memslot
    if (writable && memslot_is_readonly(slot)) {
        *writable = false;
        writable = NULL;
    }
    
    // Step 2: Convert HVA to PFN using get_user_pages
    return hva_to_pfn(addr, atomic, interruptible, async, write_fault, writable);
}
```

#### 8.4 GFN to HVA Translation

**File:** `include/linux/kvm_host.h:1823-1834`
```c
static inline unsigned long
__gfn_to_hva_memslot(const struct kvm_memory_slot *slot, gfn_t gfn)
{
    // Simple offset calculation
    unsigned long offset = gfn - slot->base_gfn;
    offset = array_index_nospec(offset, slot->npages);
    return slot->userspace_addr + offset * PAGE_SIZE;
}
```

**File:** `virt/kvm/kvm_main.c:2693-2706`
```c
static unsigned long __gfn_to_hva_many(const struct kvm_memory_slot *slot, gfn_t gfn,
                                       gfn_t *nr_pages, bool write)
{
    if (!slot || slot->flags & KVM_MEMSLOT_INVALID)
        return KVM_HVA_ERR_BAD;
    
    if (memslot_is_readonly(slot) && write)
        return KVM_HVA_ERR_RO_BAD;
    
    if (nr_pages)
        *nr_pages = slot->npages - (gfn - slot->base_gfn);
    
    return __gfn_to_hva_memslot(slot, gfn);
}
```

#### 8.5 HVA to PFN Translation

**File:** `virt/kvm/kvm_main.c:2954-3003`
```c
kvm_pfn_t hva_to_pfn(unsigned long addr, bool atomic, bool interruptible,
                     bool *async, bool write_fault, bool *writable)
{
    struct vm_area_struct *vma;
    kvm_pfn_t pfn;
    int npages, r;
    
    // Try fast path using get_user_page_fast_only
    if (hva_to_pfn_fast(addr, write_fault, writable, &pfn))
        return pfn;
    
    if (atomic)
        return KVM_PFN_ERR_FAULT;
    
    // Slow path: use get_user_pages_unlocked
    npages = hva_to_pfn_slow(addr, async, write_fault, interruptible,
                             writable, &pfn);
    if (npages == 1)
        return pfn;
    
    // Handle special cases (VM_IO, VM_PFNMAP)
    mmap_read_lock(current->mm);
    // ... handle hardware poison and remapped pages
    
retry:
    vma = vma_lookup(current->mm, addr);
    
    if (vma == NULL)
        pfn = KVM_PFN_ERR_FAULT;
    else if (vma->vm_flags & (VM_IO | VM_PFNMAP)) {
        r = hva_to_pfn_remapped(vma, addr, write_fault, writable, &pfn);
        if (r == -EAGAIN)
            goto retry;
        if (r < 0)
            pfn = KVM_PFN_ERR_FAULT;
    } else {
        if (async && vma_is_valid(vma, write_fault))
            *async = true;
        pfn = KVM_PFN_ERR_FAULT;
    }
    
    mmap_read_unlock(current->mm);
    return pfn;
}
```

**File:** `virt/kvm/kvm_main.c:2807-2853`
```c
static int hva_to_pfn_slow(unsigned long addr, bool *async, bool write_fault,
                           bool interruptible, bool *writable, kvm_pfn_t *pfn)
{
    unsigned int flags = FOLL_HWPOISON | FOLL_HONOR_NUMA_FAULT;
    struct page *page;
    int npages;
    
    might_sleep();
    
    if (writable)
        *writable = write_fault;
    
    if (write_fault)
        flags |= FOLL_WRITE;
    if (async)
        flags |= FOLL_NOWAIT;
    if (interruptible)
        flags |= FOLL_INTERRUPTIBLE;
    
    // Use get_user_pages to fault in the page and get the PFN
    npages = get_user_pages_unlocked(addr, 1, &page, flags);
    if (npages != 1)
        return npages;
    
    // Map read fault as writable if possible
    if (unlikely(!write_fault) && writable) {
        struct page *wpage;
        if (get_user_page_fast_only(addr, FOLL_WRITE, &wpage)) {
            *writable = true;
            put_page(page);
            page = wpage;
        }
    }
    
    *pfn = page_to_pfn(page);
    return npages;
}
```

### 9. EPT Entry Creation (TDP MMU Map)

**File:** `arch/x86/kvm/mmu/tdp_mmu.c:1104-1183`
```c
int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    struct kvm_mmu *mmu = vcpu->arch.mmu;
    struct kvm *kvm = vcpu->kvm;
    struct tdp_iter iter;
    struct kvm_mmu_page *sp;
    int ret = RET_PF_RETRY;
    
    // Adjust for hugepage if possible
    kvm_mmu_hugepage_adjust(vcpu, fault);
    
    trace_kvm_mmu_spte_requested(fault);
    
    rcu_read_lock();
    
    // Walk the EPT page table hierarchy
    tdp_mmu_for_each_pte(iter, mmu, fault->gfn, fault->gfn + 1) {
        int r;
        
        if (fault->nx_huge_page_workaround_enabled)
            disallowed_hugepage_adjust(fault, iter.old_spte, iter.level);
        
        // Check for frozen SPTE
        if (is_frozen_spte(iter.old_spte))
            goto retry;
        
        // Reached target level
        if (iter.level == fault->goal_level)
            goto map_target_level;
        
        // Step down into lower level page table if it exists
        if (is_shadow_present_pte(iter.old_spte) && !is_large_pte(iter.old_spte))
            continue;
        
        // Allocate new page table
        sp = tdp_mmu_alloc_sp(vcpu);
        tdp_mmu_init_child_sp(sp, &iter);
        
        sp->nx_huge_page_disallowed = fault->huge_page_disallowed;
        
        // Either split huge page or link new page table
        if (is_shadow_present_pte(iter.old_spte))
            r = tdp_mmu_split_huge_page(kvm, &iter, sp, true);
        else
            r = tdp_mmu_link_sp(kvm, &iter, sp, true);
        
        if (r) {
            tdp_mmu_free_sp(sp);
            goto retry;
        }
        
        // Track NX huge pages if needed
        if (fault->huge_page_disallowed && fault->req_level >= iter.level) {
            spin_lock(&kvm->arch.tdp_mmu_pages_lock);
            if (sp->nx_huge_page_disallowed)
                track_possible_nx_huge_page(kvm, sp);
            spin_unlock(&kvm->arch.tdp_mmu_pages_lock);
        }
    }
    
    WARN_ON_ONCE(iter.level == fault->goal_level);
    goto retry;
    
map_target_level:
    // Install the leaf SPTE
    ret = tdp_mmu_map_handle_target_level(vcpu, fault, &iter);
    
retry:
    rcu_read_unlock();
    return ret;
}
```

### 10. Install Leaf EPT Entry

**File:** `arch/x86/kvm/mmu/tdp_mmu.c:1017-1064`
```c
static int tdp_mmu_map_handle_target_level(struct kvm_vcpu *vcpu,
                                           struct kvm_page_fault *fault,
                                           struct tdp_iter *iter)
{
    struct kvm_mmu_page *sp = sptep_to_sp(rcu_dereference(iter->sptep));
    u64 new_spte;
    int ret = RET_PF_FIXED;
    bool wrprot = false;
    
    if (WARN_ON_ONCE(sp->role.level != fault->goal_level))
        return RET_PF_RETRY;
    
    // Handle MMIO case
    if (unlikely(!fault->slot))
        new_spte = make_mmio_spte(vcpu, iter->gfn, ACC_ALL);
    else
        // Create the EPT entry
        wrprot = make_spte(vcpu, sp, fault->slot, ACC_ALL, iter->gfn,
                          fault->pfn, iter->old_spte, fault->prefetch, true,
                          fault->map_writable, &new_spte);
    
    if (new_spte == iter->old_spte)
        ret = RET_PF_SPURIOUS;
    else if (tdp_mmu_set_spte_atomic(vcpu->kvm, iter, new_spte))
        return RET_PF_RETRY;
    else if (is_shadow_present_pte(iter->old_spte) &&
             !is_last_spte(iter->old_spte, iter->level))
        kvm_flush_remote_tlbs_gfn(vcpu->kvm, iter->gfn, iter->level);
    
    // Handle write protection
    if (wrprot && fault->write)
        ret = RET_PF_WRITE_PROTECTED;
    
    // Handle MMIO emulation
    if (unlikely(is_mmio_spte(vcpu->kvm, new_spte))) {
        vcpu->stat.pf_mmio_spte_created++;
        trace_mark_mmio_spte(rcu_dereference(iter->sptep), iter->gfn, new_spte);
        ret = RET_PF_EMULATE;
    } else {
        trace_kvm_mmu_set_spte(iter->level, iter->gfn,
                              rcu_dereference(iter->sptep));
    }
    
    return ret;
}
```

### 11. Create SPTE Value

**File:** `arch/x86/kvm/mmu/spte.c:157-277`
```c
bool make_spte(struct kvm_vcpu *vcpu, struct kvm_mmu_page *sp,
               const struct kvm_memory_slot *slot,
               unsigned int pte_access, gfn_t gfn, kvm_pfn_t pfn,
               u64 old_spte, bool prefetch, bool can_unsync,
               bool host_writable, u64 *new_spte)
{
    int level = sp->role.level;
    u64 spte = SPTE_MMU_PRESENT_MASK;
    bool wrprot = false;
    
    // Set A/D bit mode
    if (sp->role.ad_disabled)
        spte |= SPTE_TDP_AD_DISABLED;
    else if (kvm_mmu_page_ad_need_write_protect(sp))
        spte |= SPTE_TDP_AD_WRPROT_ONLY;
    
    spte |= shadow_present_mask;
    if (!prefetch)
        spte |= spte_shadow_accessed_mask(spte);
    
    // NX huge page mitigation
    if (level > PG_LEVEL_4K && (pte_access & ACC_EXEC_MASK) &&
        is_nx_huge_page_enabled(vcpu->kvm)) {
        pte_access &= ~ACC_EXEC_MASK;
    }
    
    // Set execute permissions
    if (pte_access & ACC_EXEC_MASK)
        spte |= shadow_x_mask;
    else
        spte |= shadow_nx_mask;
    
    // Set user permissions
    if (pte_access & ACC_USER_MASK)
        spte |= shadow_user_mask;
    
    // Set page size for huge pages
    if (level > PG_LEVEL_4K)
        spte |= PT_PAGE_SIZE_MASK;
    
    // Set memory type (for EPT)
    if (shadow_memtype_mask)
        spte |= kvm_x86_call(get_mt_mask)(vcpu, gfn, kvm_is_mmio_pfn(pfn));
    
    if (host_writable)
        spte |= shadow_host_writable_mask;
    else
        pte_access &= ~ACC_WRITE_MASK;
    
    // Set memory encryption mask (for SME/SEV)
    if (shadow_me_value && !kvm_is_mmio_pfn(pfn))
        spte |= shadow_me_value;
    
    // Encode the PFN into the SPTE
    spte |= (u64)pfn << PAGE_SHIFT;
    
    // Set write permissions
    if (pte_access & ACC_WRITE_MASK) {
        spte |= PT_WRITABLE_MASK | shadow_mmu_writable_mask;
        
        // Check if we can unsync shadow pages
        if (is_last_spte(old_spte, level) && is_writable_pte(old_spte))
            goto out;
        
        if (mmu_try_to_unsync_pages(vcpu->kvm, slot, gfn, can_unsync, prefetch)) {
            wrprot = true;
            pte_access &= ~ACC_WRITE_MASK;
            spte &= ~(PT_WRITABLE_MASK | shadow_mmu_writable_mask);
        }
    }
    
    // Set dirty bit
    if (pte_access & ACC_WRITE_MASK)
        spte |= spte_shadow_dirty_mask(spte);
    
out:
    if (prefetch)
        spte = mark_spte_for_access_track(spte);
    
    // Mark page dirty in dirty bitmap
    if ((spte & PT_WRITABLE_MASK) && kvm_slot_dirty_track_enabled(slot)) {
        WARN_ON_ONCE(level > PG_LEVEL_4K);
        mark_page_dirty_in_slot(vcpu->kvm, slot, gfn);
    }
    
    *new_spte = spte;
    return wrprot;
}
```

### 12. Install SPTE Atomically

**File:** `arch/x86/kvm/mmu/tdp_mmu.c:576-592`
```c
static inline int __must_check tdp_mmu_set_spte_atomic(struct kvm *kvm,
                                                       struct tdp_iter *iter,
                                                       u64 new_spte)
{
    int ret;
    
    lockdep_assert_held_read(&kvm->mmu_lock);
    
    ret = __tdp_mmu_set_spte_atomic(iter, new_spte);
    if (ret)
        return ret;
    
    // Update bookkeeping
    handle_changed_spte(kvm, iter->as_id, iter->gfn, iter->old_spte,
                       new_spte, iter->level, true);
    
    return 0;
}
```

**File:** `arch/x86/kvm/mmu/tdp_mmu.c:533-557`
```c
static inline int __must_check __tdp_mmu_set_spte_atomic(struct tdp_iter *iter,
                                                         u64 new_spte)
{
    u64 *sptep = rcu_dereference(iter->sptep);
    
    WARN_ON_ONCE(iter->yielded || is_frozen_spte(iter->old_spte));
    
    // Atomically install the new SPTE using CMPXCHG
    if (!try_cmpxchg64(sptep, &iter->old_spte, new_spte))
        return -EBUSY;
    
    return 0;
}
```

### 13. Handle SPTE Change

**File:** `arch/x86/kvm/mmu/tdp_mmu.c:441-531`
```c
static void handle_changed_spte(struct kvm *kvm, int as_id, gfn_t gfn,
                                u64 old_spte, u64 new_spte, int level,
                                bool shared)
{
    bool was_present = is_shadow_present_pte(old_spte);
    bool is_present = is_shadow_present_pte(new_spte);
    bool was_leaf = was_present && is_last_spte(old_spte, level);
    bool is_leaf = is_present && is_last_spte(new_spte, level);
    bool pfn_changed = spte_to_pfn(old_spte) != spte_to_pfn(new_spte);
    
    // Validation checks
    WARN_ON_ONCE(level > PT64_ROOT_MAX_LEVEL);
    WARN_ON_ONCE(level < PG_LEVEL_4K);
    
    // Track dirty pages
    if (was_leaf && is_dirty_spte(old_spte) &&
        (!is_present || !is_dirty_spte(new_spte) || pfn_changed))
        kvm_set_pfn_dirty(spte_to_pfn(old_spte));
    
    // Handle removed page tables
    if (was_present && !was_leaf &&
        (is_leaf || !is_present || WARN_ON_ONCE(pfn_changed)))
        handle_removed_pt(kvm, spte_to_child_pt(old_spte, level), shared);
    
    // Track accessed pages
    if (was_leaf && is_accessed_spte(old_spte) &&
        (!is_present || !is_accessed_spte(new_spte) || pfn_changed))
        kvm_set_pfn_accessed(spte_to_pfn(old_spte));
}
```

## Summary: Complete Call Chain

```
Guest vCPU accesses virtual address
    ↓
Guest page table walk (hardware)
    ↓
EPT violation (no EPT entry for GPA)
    ↓
VM-exit: EXIT_REASON_EPT_VIOLATION
    ↓
handle_ept_violation() [vmx.c:5782]
    ↓
__vmx_handle_ept_violation() [common.h:9]
    ↓
kvm_mmu_page_fault() [mmu.c:6106]
    ↓
kvm_mmu_do_page_fault() [mmu_internal.h:293] (inline)
    ↓
kvm_tdp_page_fault() [mmu.c:4726]
    ↓
kvm_tdp_mmu_page_fault() [mmu.c:4673]
    ↓
    ├─→ fast_page_fault() [mmu.c:3466] (optimization, may fail)
    │
    └─→ kvm_faultin_pfn() [mmu.c:4434]
            ↓
        __kvm_faultin_pfn() [mmu.c:4398]
            ↓
        __gfn_to_pfn_memslot() [kvm_main.c:3005]
            ↓
            ├─→ __gfn_to_hva_many() [kvm_main.c:2693]
            │       ↓
            │   __gfn_to_hva_memslot() [kvm_host.h:1823]
            │       (GPA → HVA: slot->userspace_addr + offset * PAGE_SIZE)
            │
            └─→ hva_to_pfn() [kvm_main.c:2954]
                    ↓
                hva_to_pfn_slow() [kvm_main.c:2807]
                    ↓
                get_user_pages_unlocked() (Linux MM)
                    ↓
                page_to_pfn() → fault->pfn
    
    ↓
kvm_tdp_mmu_map() [tdp_mmu.c:1104]
    ↓
    Walk/allocate EPT page table hierarchy
    tdp_mmu_for_each_pte() iterates through levels
    ↓
tdp_mmu_map_handle_target_level() [tdp_mmu.c:1017]
    ↓
make_spte() [spte.c:157]
    (Create SPTE value: permissions, memory type, PFN encoding)
    ↓
tdp_mmu_set_spte_atomic() [tdp_mmu.c:576]
    ↓
__tdp_mmu_set_spte_atomic() [tdp_mmu.c:533]
    ↓
try_cmpxchg64() → Install EPT entry atomically
    ↓
handle_changed_spte() [tdp_mmu.c:441]
    (Update bookkeeping, dirty/accessed tracking)
    ↓
Return to guest with EPT entry installed
    ↓
Guest vCPU resumes execution successfully
```

## Key Data Structures

### kvm_page_fault
Contains all information about the fault:
- `addr`: GPA that caused the fault
- `gfn`: Guest Frame Number (addr >> PAGE_SHIFT)
- `pfn`: Page Frame Number (resolved from host)
- `slot`: Memory slot containing the GFN
- `write`, `exec`, `present`: Fault type flags
- `goal_level`: Target EPT level (4K, 2M, 1G)

### tdp_iter
Iterator for walking TDP (Two-Dimensional Paging) structures:
- `sptep`: Pointer to current SPTE
- `old_spte`: Current SPTE value
- `level`: Current page table level
- `gfn`: GFN at this point in the walk

### SPTE (Shadow Page Table Entry)
64-bit value encoding:
- Bits 0-11: Flags (present, writable, accessed, dirty, etc.)
- Bits 12-51: PFN (Page Frame Number)
- Bits 52-63: KVM-specific flags (memory type, A/D mode, etc.)

## Critical Paths

1. **GPA → HVA**: `__gfn_to_hva_memslot()` uses memslot mapping
2. **HVA → PFN**: `get_user_pages_unlocked()` faults in host page
3. **PFN → EPT**: `make_spte()` encodes PFN into EPT entry format
4. **Installation**: `try_cmpxchg64()` atomically installs EPT entry

## Locking

- `mmu_lock` (read mode): Held during EPT entry creation
- `rcu_read_lock()`: Protects page table traversal
- Atomic CMPXCHG: Ensures concurrent safety without full lock

## Performance Optimizations

1. **Fast page fault**: Lockless permission fixup
2. **Huge pages**: Map 2M or 1G pages when possible
3. **Async page fault**: Allow host to sleep on I/O
4. **Memory caches**: Pre-allocate page table pages
5. **A/D bits**: Hardware accessed/dirty tracking
