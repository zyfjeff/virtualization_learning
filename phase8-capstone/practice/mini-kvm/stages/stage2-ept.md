# Stage 2: EPT 内存虚拟化

> 对应课程 Phase 2: Memory Virtualization (EPT/TDP MMU)
>
> 关键源码: `arch/x86/kvm/mmu/tdp_mmu.c::kvm_tdp_mmu_map()`
>           `arch/x86/kvm/mmu/spte.c::make_spte()`

---

## 🎯 阶段目标

实现 EPT (Extended Page Tables)：
- 建立 4 级 EPT 页表结构
- 实现 GPA → HPA 地址映射
- 处理 EPT Violation (按需映射)

## 📖 核心概念

### EPT 页表结构

```
4KB 页映射 (4 级):
  PML4 [512] → PDPT [512] → PD [512] → PT [512] → 物理页

2MB 大页映射 (3 级):
  PML4 [512] → PDPT [512] → PD [512] → 2MB 物理页

1GB 大页映射 (2 级):
  PML4 [512] → PDPT [512] → 1GB 物理页
```

### EPT 权限位

```
EPT 条目位:
  [0]   Read          - 读权限
  [1]   Write         - 写权限
  [2]   Execute       - 执行权限 ( supervisor 模式)
  [5:3] Memory Type   - 内存类型 (6=Write-Back)
  [6]   Ignore PAT    - 忽略 PAT
  [7]   Large Page    - 大页标志
  [63]  Suppress VE   - 抑制 #VE
  [12:51] Address     - 物理地址 (4KB 对齐)
```

### EPT Violation

当 Guest 访问一个未映射的 GPA 时：
1. MMU 无法翻译 GPA → HPA
2. 触发 EPT Violation VM-Exit
3. VMM 在 VM-Exit 处理中建立 EPT 映射
4. 重新进入 Guest，重试原指令

## 🔧 mini-kvm.c 实现

### 1. EPT 初始化 (`mini_kvm_ept_init`)

```c
/* 分配 PML4 (根页表) */
pml4 = (struct mini_kvm_ept_page *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
kvm->ept.pml4 = pml4;

/* 构造 EPT Pointer (EPTP) */
kvm->ept.eptp = (__pa(pml4) & EPT_PTE_ADDR_MASK) |
                EPT_PTE_MEM_WB |        /* Write-Back */
                (EPT_LEVEL_PML4 - 1);   /* 4 级页表 */
```

**对应课程**:
- Phase 2, Section 1: SPTE 位定义
- 关键源码: `mmu/spte.h`

### 2. 映射页面 (`mini_kvm_ept_map_page`)

```c
/* 计算各级索引 */
pml4_idx = (gpa >> 39) & 0x1FF;
pdpt_idx = (gpa >> 30) & 0x1FF;
pd_idx   = (gpa >> 21) & 0x1FF;
pt_idx   = (gpa >> 12) & 0x1FF;

/* 按需分配中间页表 */
if (!(pml4->entries[pml4_idx] & EPT_PTE_READ)) {
    pdpt = alloc_page();
    pml4->entries[pml4_idx] = __pa(pdpt) | EPT_PTE_READ |
                              EPT_PTE_WRITE | EPT_PTE_EXEC | EPT_PTE_MEM_WB;
}

/* 写入叶条目 (GPA → HPA) */
pt->entries[pt_idx] = (hpa & EPT_PTE_ADDR_MASK) |
                      EPT_PTE_READ | EPT_PTE_WRITE |
                      EPT_PTE_EXEC | EPT_PTE_MEM_WB;
```

**对应课程**:
- Phase 2, Section 4: kvm_tdp_mmu_map()
- 关键源码: `tdp_mmu.c:1104`

### 3. EPT Violation 处理

```c
int mini_kvm_ept_handle_violation(struct mini_kvm_vcpu *vcpu)
{
    u64 gpa = vcpu->exit_qualification;  /* 触发违规的 GPA */
    u64 hpa;
    void *page;

    /* 分配物理页 */
    page = alloc_page(GFP_KERNEL);
    hpa = page_to_phys(page);

    /* 建立 EPT 映射 */
    mini_kvm_ept_map_page(vcpu->kvm, gpa & PAGE_MASK, hpa);

    /* 重新进入 Guest, 重试原指令 */
    return MINI_KVM_EXIT_RESUME_GUEST;
}
```

**对应课程**:
- Phase 2, Section 2: kvm_handle_page_fault()
- 关键源码: `mmu/mmu.c:4628`

## 🔑 关键差异: mini-kvm vs 真实 KVM

| 特性 | mini-kvm | 真实 KVM |
|------|----------|---------|
| 并发安全 | 无 (单 vCPU) | cmpxchg 原子操作 |
| 大页支持 | 仅 4KB | 2MB, 1GB |
| 按需映射 | 简单 alloc | 复杂页面分配器 |
| 脏页跟踪 | 无 | PML + 软件位 |
| MMIO 处理 | 无 | 特殊处理逻辑 |

## 🧪 实验验证

```bash
# 加载模块
sudo insmod mini-kvm.ko

# 查看 EPT 初始化日志
dmesg | grep -E "Stage 2|EPT"

# 预期输出:
# mini-kvm: === Stage 2: EPT 初始化 ===
# mini-kvm:   PML4 物理地址: 0x...
# mini-kvm:   EPTP: 0x...
# mini-kvm: === Stage 2 完成: EPT 根页表已建立 ===
```

## 📝 检查清单

- [ ] 解释 EPT 4 级页表结构
- [ ] 描述 EPT Violation 的完整处理流程
- [ ] 对比 EPT 与普通页表的区别
- [ ] 理解 EPTP 的格式和作用
- [ ] 解释为什么需要 A/D 位

## 🔗 下一步

Stage 3: 中断处理
