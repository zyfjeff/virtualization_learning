# MMIO 识别与处理机制

> 深入解析 KVM 如何区分 RAM 和设备内存（如 GPU BAR）

---

## 1. MMIO 识别机制概述

### 1.1 第一层：pfn_valid() 检查

```c
// arch/x86/kvm/mmu/spte.c:108
static bool kvm_is_mmio_pfn(kvm_pfn_t pfn)
{
    if (pfn_valid(pfn))  // 是否有对应的 struct page
        // ...
}
```

**`pfn_valid(pfn)` 的含义**：
- 检查内核是否为这个物理页帧号维护了 `struct page`
- 如果有 → 说明这个物理地址被内核管理（可能是 RAM 或保留页）
- 如果没有 → 说明这个物理地址不在内核管理范围内（通常是 MMIO）

**示例**：
```
物理地址          pfn_valid()    说明
0x00000000-       true           常规 RAM（被内核管理）
0x00100000
0xF0000000-       false          MMIO 区域（设备内存，不在内核管理范围）
0xFFFFFFFF
```

### 1.2 第二层：PageReserved() 检查

如果 `pfn_valid(pfn)` 为真，还需要进一步检查：

```c
if (pfn_valid(pfn))
    return !is_zero_pfn(pfn) && 
           PageReserved(pfn_to_page(pfn)) &&
           (!pat_enabled() || pat_pfn_immune_to_uc_mtrr(pfn));
```

**`PageReserved()` 的含义**：
- 检查 `struct page` 的 PG_reserved 标志
- 如果被设置 → 说明这个页被保留，不是普通 RAM
- 常见场景：
  - BIOS/UEFI 保留区域
  - 设备映射到 RAM 地址空间的区域
  - NVDIMM DAX 设备

**为什么需要检查？**

某些保留页虽然不是普通 RAM，但也不是 MMIO（例如 NVDIMM DAX 设备），它们可以被缓存以提升性能。

### 1.3 第三层：e820 内存映射表检查

如果 `pfn_valid(pfn)` 为假，使用 e820 表判断：

```c
return !e820__mapped_raw_any(pfn_to_hpa(pfn),
                             pfn_to_hpa(pfn + 1) - 1,
                             E820_TYPE_RAM);
```

**e820 内存映射表**：

BIOS/UEFI 在启动时告诉操作系统，哪些物理地址区域是什么类型：

| 类型 | 值 | 说明 | 是否 MMIO |
|------|---|------|----------|
| `E820_TYPE_RAM` | 1 | 可用 RAM | ❌ 否 |
| `E820_TYPE_RESERVED` | 2 | 保留区域 | ✅ 可能是 |
| `E820_TYPE_ACPI` | 3 | ACPI 表 | ✅ 是 |
| `E820_TYPE_NVS` | 4 | ACPI NVS | ✅ 是 |
| `E820_TYPE_UNUSABLE` | 5 | 不可用 | ✅ 是 |
| `E820_TYPE_PMEM` | 7 | 持久内存 | ❌ 否（特殊处理） |

**查看 e820 表**：
```bash
dmesg | grep -i e820
# 或
cat /proc/iomem
```

**示例输出**：
```
00000000-0009ffff : System RAM
000a0000-000fffff : reserved          ← VGA 显存（MMIO）
00100000-bfffffff : System RAM
c0000000-cfffffff : reserved          ← 设备 MMIO
f0000000-feffffff : reserved          ← GPU BAR 等（MMIO）
ff000000-ffffffff : reserved          ← BIOS
```

---

## 2. GPU BAR 的完整识别流程

### 2.1 GPU BAR 的物理地址特征

GPU BAR（Base Address Register）通常位于高地址区域：

```
地址范围              用途
0x00000000-0x000FFFFF 传统设备区域（VGA、BIOS）
0xF0000000-0xFBFFFFFF PCI 设备 MMIO（包括 GPU BAR）
0xFC000000-0xFFFFFFFF APIC、ACPI 等
```

**典型 GPU BAR 布局**：
```
BAR0: 0xF0000000-0xF0FFFFFF  (256MB) - 显存映射
BAR1: 0xF1000000-0xF1FFFFFF  (16MB)  - MMIO 寄存器
BAR2: 0xF2000000-0xF23FFFFF  (4MB)   - 控制寄存器
```

### 2.2 识别流程

```
Guest 访问 GPA = 0xF0000000（GPU BAR）
    ↓
EPT Violation → KVM 处理
    ↓
kvm_is_mmio_pfn(pfn) 检查
    ↓
┌─────────────────────────────────────────┐
│ Step 1: pfn_valid(0xF0000)            │
│   → false（没有 struct page）          │
│   → GPU BAR 不在内核管理范围          │
└─────────────────────────────────────────┘
    ↓
┌─────────────────────────────────────────┐
│ Step 2: e820__mapped_raw_any()        │
│   检查 0xF0000000 是否 E820_TYPE_RAM  │
│   → false（e820 表中标记为 reserved）  │
│   → 返回 true（是 MMIO）              │
└─────────────────────────────────────────┘
    ↓
vmx_get_mt_mask(vcpu, gfn, is_mmio=true)
    ↓
return MTRR_TYPE_UNCACHABLE << 3
    ↓
SPTE bit 3-5 = 000 (UC)
```

### 2.3 为什么 GPU BAR 没有 struct page？

**内核只为主存（RAM）分配 struct page**：

```c
// 内核启动时
for each E820_TYPE_RAM region:
    allocate struct page for each 4KB page
    add to buddy allocator
```

**GPU BAR 不在 RAM 区域**：
- e820 表标记为 `E820_TYPE_RESERVED`
- 内核不会为其分配 `struct page`
- 只能通过 `ioremap()` 映射到内核虚拟地址空间
- `pfn_valid()` 返回 false

---

## 3. MMIO 处理的关键细节

### 3.1 为什么 MMIO 必须用 UC？

**场景**：GPU 渲染完成标志

```
GPU 硬件行为：
1. GPU 完成渲染
2. 写入状态寄存器：*status_reg = 0x01（完成标志）
3. 触发中断通知 CPU

CPU 行为（如果使用 WB 缓存）：
1. CPU 读取 *status_reg
2. CPU 返回缓存中的旧值：0x00（未完成）
3. CPU 永远看不到完成标志！
4. 驱动死锁

CPU 行为（使用 UC）：
1. CPU 读取 *status_reg
2. 直接访问 GPU 硬件
3. 返回最新值：0x01（完成）
4. 驱动继续执行
```

### 3.2 MMIO 访问的性能影响

| 内存类型 | 读延迟 | 写延迟 | 适用场景 |
|---------|--------|--------|---------|
| WB (Write-Back) | ~1 ns（缓存命中） | ~1 ns（缓存命中） | 普通 RAM |
| WT (Write-Through) | ~100 ns | ~100 ns | 特殊设备 |
| UC (Uncacheable) | ~100-500 ns | ~100-500 ns | MMIO 设备 |

**性能差距**：UC 比 WB 慢 100-500 倍！

**这就是为什么 GPU 驱动会批量提交命令**：
- 减少 MMIO 访问次数
- 使用 ring buffer 批量写入
- 一次性提交大量渲染命令

### 3.3 MMIO 的特殊处理

**1. 只读映射**

某些 MMIO 区域只能读不能写：

```c
struct kvm_userspace_memory_region region = {
    .slot = 1,
    .guest_phys_addr = 0xF0000000,
    .memory_size = 256 * 1024 * 1024,
    .userspace_addr = hva,
    .flags = KVM_MEM_READONLY,  // 只读
};
```

**2. MMIO 缓存**

对于频繁的 MMIO 访问，KVM 会缓存映射：

```c
// arch/x86/kvm/mmu/spte.c
void kvm_mmu_set_mmio_spte_mask(u64 mmio_value, u64 mmio_mask, u64 access_mask)
{
    shadow_mmio_value = mmio_value;
    shadow_mmio_mask = mmio_mask;
    shadow_mmio_access_mask = access_mask;
}
```

**3. MMIO 拦截**

某些 MMIO 访问会触发 VM-Exit，由 QEMU 模拟：

```
Guest 写入 GPU 寄存器
    ↓
EPT Violation (MMIO)
    ↓
KVM 检查：是设备 MMIO
    ↓
转发给 QEMU（慢速路径）
    ↓
QEMU 模拟设备行为
    ↓
返回结果给 Guest
```

---

## 4. 实践：查看系统的 MMIO 区域

### 4.1 查看 e820 内存映射

```bash
# 方法 1: dmesg
dmesg | grep -i "BIOS-provided e820"

# 方法 2: /proc/iomem
cat /proc/iomem

# 示例输出：
# 00000000-0009ffff : System RAM
# 000a0000-000fffff : reserved          ← MMIO
# 00100000-bfffffff : System RAM
# c0000000-cfffffff : reserved          ← MMIO
# f0000000-feffffff : reserved          ← GPU BAR (MMIO)
```

### 4.2 查看 PCI 设备 BAR

```bash
# 列出所有 PCI 设备及其 BAR
lspci -vvv | grep -A 5 "Region"

# 示例输出：
# 01:00.0 VGA compatible controller: NVIDIA Corporation ...
#     Region 0: Memory at f0000000 (32-bit, non-prefetchable) [size=16M]
#     Region 1: Memory at e0000000 (64-bit, prefetchable) [size=256M]
#     Region 3: Memory at f1000000 (32-bit, non-prefetchable) [size=32M]
```

### 4.3 查看 MTRR 设置

```bash
# 查看 MTRR（Memory Type Range Registers）
cat /proc/mtrr

# 示例输出：
# reg00: base=0x000000000 (    0MB), size=  128MB, count=1: write-back
# reg01: base=0x0f0000000 ( 3840MB), size=  256MB, count=1: uncachable  ← GPU BAR
# reg02: base=0x100000000 ( 4096MB), size= 2048MB, count=1: write-back
```

---

## 5. IPAT 位与 Guest 内存类型控制

### 5.1 什么是 IPAT？

**IPAT (Ignore PAT)** 是 EPT 页表项中的第 6 位，用于控制是否忽略 Guest OS 的内存类型设置。

```
EPT PTE 格式：
[PFN:51-12] [MT:5-3] [IPAT:6] [其他标志]

IPAT = 0: 考虑 Guest 的 PAT/MTRR 设置
IPAT = 1: 忽略 Guest 的 PAT/MTRR 设置，使用 EPT 中设置的内存类型
```

**定义位置**（`arch/x86/include/asm/vmx.h:537`）：
```c
#define VMX_EPT_IPAT_BIT    (1ull << 6)
```

### 5.2 IPAT 默认行为

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

### 5.3 VFIO 设备如何触发 IPAT 关闭

当 VFIO 设备附加到 VM 时，KVM 会检查设备的一致性 DMA 能力：

**代码位置**（`virt/kvm/vfio.c:137`）：
```c
static void kvm_vfio_update_coherency(struct kvm_device *dev)
{
    struct kvm_vfio *kv = dev->private;
    bool noncoherent = false;
    struct kvm_vfio_file *kvf;

    list_for_each_entry(kvf, &kv->file_list, node) {
        if (!kvm_vfio_file_enforced_coherent(kvf->file)) {
            noncoherent = true;
            break;
        }
    }

    if (noncoherent != kv->noncoherent) {
        kv->noncoherent = noncoherent;

        if (kv->noncoherent)
            kvm_arch_register_noncoherent_dma(dev->kvm);
        else
            kvm_arch_unregister_noncoherent_dma(dev->kvm);
    }
}
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

### 5.4 为什么这样设计？

**代码注释**（`arch/x86/kvm/vmx/vmx.c:7685`）：
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

### 5.5 实际场景验证

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

### 5.6 如何验证当前 IPAT 状态

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

### 5.7 关键要点

1. **IPAT 默认开启**：大多数 VM 中，Guest 的内存类型设置被忽略
2. **VFIO 触发关闭**：只有附加 VFIO 非一致性 DMA 设备时，IPAT 才关闭
3. **MMIO 强制 UC**：无论 IPAT 状态，MMIO 区域始终使用 UC
4. **设计原则**：默认安全 + 性能优先，特殊情况下信任 Guest

**简单记忆**：
- **普通 VM** → IPAT = 1 → Guest 设置无效
- **VFIO VM** → IPAT = 0 → Guest 设置有效

---

## 6. MMIO 识别的完整逻辑（修正版）

### 6.1 实际检查逻辑

**代码位置**（`arch/x86/kvm/mmu/spte.c:110`）：

```c
static bool kvm_is_mmio_pfn(kvm_pfn_t pfn)
{
    if (pfn_valid(pfn))
        return !is_zero_pfn(pfn) && PageReserved(pfn_to_page(pfn)) &&
            /*
             * Some reserved pages, such as those from NVDIMM
             * DAX devices, are not for MMIO, and can be mapped
             * with cached memory type for better performance.
             * However, the above check misconceives those pages
             * as MMIO, and results in KVM mapping them with UC
             * memory type, which would hurt the performance.
             * Therefore, we check the host memory type in addition
             * and only treat UC/UC-/WC pages as MMIO.
             */
            (!pat_enabled() || pat_pfn_immune_to_uc_mtrr(pfn));

    return !e820__mapped_raw_any(pfn_to_hpa(pfn),
                                 pfn_to_hpa(pfn + 1) - 1,
                                 E820_TYPE_RAM);
}
```

### 6.2 完整流程图

```
kvm_is_mmio_pfn(pfn)
    ↓
┌─ pfn_valid(pfn) = true（有 struct page）
│   ↓
│   检查三个条件（AND 关系）：
│   1. !is_zero_pfn(pfn)        → 不是零页
│   2. PageReserved(pfn_to_page(pfn))  → 是保留页
│   3. (!pat_enabled() || pat_pfn_immune_to_uc_mtrr(pfn))
│      → PAT 未启用，或者页面内存类型为 UC/UC-/WC
│   ↓
│   三个条件都满足 → 返回 true（是 MMIO）
│   任一条件不满足 → 返回 false（不是 MMIO）
│
└─ pfn_valid(pfn) = false（无 struct page）
    ↓
    检查 e820 表：
    !e820__mapped_raw_any(..., E820_TYPE_RAM)
    ↓
    ├─ 是 E820_TYPE_RAM → 返回 false（不是 MMIO）
    └─ 其他类型 → 返回 true（是 MMIO）
```

### 6.3 PAT 检查的重要性

**为什么需要 PAT 检查？**

某些保留页面虽然不是普通 RAM，但也不是 MMIO：

| 页面类型 | PageReserved | 内存类型 | 是否 MMIO | 说明 |
|---------|--------------|---------|----------|------|
| 普通 RAM | false | WB | ❌ | 可缓存 |
| GPU BAR | true/false | UC | ✅ | 不可缓存 |
| NVDIMM DAX | true | WB | ❌ | 可缓存（特殊优化） |
| ACPI NVS | true | UC | ✅ | 不可缓存 |

**PAT 检查的作用**：
- 区分 NVDIMM DAX（保留但可缓存）和普通 MMIO（保留且不可缓存）
- 避免将 NVDIMM DAX 误认为 MMIO 而使用 UC，影响性能

**代码注释**：
```c
/*
 * Some reserved pages, such as those from NVDIMM
 * DAX devices, are not for MMIO, and can be mapped
 * with cached memory type for better performance.
 * However, the above check misconceives those pages
 * as MMIO, and results in KVM mapping them with UC
 * memory type, which would hurt the performance.
 * Therefore, we check the host memory type in addition
 * and only treat UC/UC-/WC pages as MMIO.
 */
```

---

## 7. 总结

### 7.1 MMIO 识别的核心逻辑

```
kvm_is_mmio_pfn(pfn)
    ↓
┌─ pfn_valid(pfn) = true
│   ├─ is_zero_pfn(pfn)? → 不是 MMIO
│   ├─ PageReserved(pfn)? → 可能是 MMIO
│   └─ pat_pfn_immune_to_uc_mtrr(pfn)? → 检查 PAT 属性
│
└─ pfn_valid(pfn) = false
    └─ e820__mapped_raw_any(pfn)? → 检查 e820 表
        ├─ 是 E820_TYPE_RAM → 不是 MMIO
        └─ 其他类型 → 是 MMIO
```

### 7.2 内存类型决策（完整逻辑）

**实际代码**（`arch/x86/kvm/vmx/vmx.c:7676`）：

```c
u8 vmx_get_mt_mask(struct kvm_vcpu *vcpu, gfn_t gfn, bool is_mmio)
{
    // MMIO 区域：强制 UC
    if (is_mmio)
        return MTRR_TYPE_UNCACHABLE << VMX_EPT_MT_EPTE_SHIFT;
    
    // 非 MMIO 区域
    if (!kvm_arch_has_noncoherent_dma(vcpu->kvm))
        return (MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT) | VMX_EPT_IPAT_BIT;
    
    return MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT;
}
```

**完整决策逻辑**：

```
vmx_get_mt_mask(vcpu, gfn, is_mmio)
    ↓
┌─ is_mmio = true
│   └─ 返回 UC（强制，无 IPAT 位）
│      防止缓存导致 Machine Check
│
└─ is_mmio = false
    ↓
    检查：kvm_arch_has_noncoherent_dma(vcpu->kvm)
    ↓
    ├─ false（无 VFIO 非一致性 DMA 设备）
    │   └─ 返回 WB + IPAT
    │      忽略 Guest PAT 设置
    │
    └─ true（有 VFIO 非一致性 DMA 设备）
        └─ 返回 WB（无 IPAT 位）
           允许 Guest 控制内存类型
```

**三种场景总结**：

| 场景 | 条件 | 返回值 | IPAT 位 | Guest PAT |
|------|------|--------|---------|-----------|
| **MMIO 区域** | `is_mmio = true` | `UC` | N/A | 不适用（强制 UC） |
| **普通 VM** | `noncoherent_dma_count = 0` | `WB + IPAT` | **1** | **忽略** |
| **VFIO VM** | `noncoherent_dma_count > 0` | `WB` | **0** | **考虑** |

### 7.3 GPU BAR 的典型处理流程

```
1. QEMU 注册 GPU 设备
   ↓
2. GPU BAR 映射到高地址（0xF0000000+）
   ↓
3. QEMU 调用 KVM_SET_USER_MEMORY_REGION
   ↓
4. KVM 创建 kvm_memory_slot
   ↓
5. Guest 访问 GPU BAR
   ↓
6. EPT Violation → KVM 处理
   ↓
7. kvm_is_mmio_pfn() 识别为 MMIO
   ↓
8. vmx_get_mt_mask() 返回 UC
   ↓
9. EPT SPTE 设置为 UC (bit 3-5 = 000)
   ↓
10. Guest 直接访问 GPU 硬件（不缓存）
```

### 7.4 关键要点

1. **MMIO 识别是两层条件判断**：
   - 有 struct page：检查是否保留页 + PAT 检查
   - 无 struct page：检查 e820 表

2. **PAT 检查区分 NVDIMM DAX 和 MMIO**：
   - NVDIMM DAX：保留页但可缓存（WB）
   - MMIO：保留页且不可缓存（UC/UC-/WC）

3. **GPU BAR 没有 struct page**：不在内核管理范围

4. **MMIO 必须用 UC**：防止缓存导致设备状态不同步

5. **IPAT 默认开启**：大多数 VM 中，Guest 的内存类型设置被忽略

6. **VFIO 触发 IPAT 关闭**：只有附加 VFIO 非一致性 DMA 设备时，IPAT 才关闭

7. **GPU BAR 始终 UC**：无论 IPAT 状态，MMIO 区域始终使用 UC

8. **性能影响巨大**：UC 比 WB 慢 100-500 倍

9. **GPU 驱动优化**：批量提交命令，减少 MMIO 访问

---

## 参考资料

- Intel SDM Vol 3, Chapter 11: Memory Cache Control
- Linux Kernel Source: `arch/x86/kvm/mmu/spte.c`
- Linux Kernel Source: `arch/x86/kvm/vmx/vmx.c`
- PCI Local Bus Specification: BAR (Base Address Register)
