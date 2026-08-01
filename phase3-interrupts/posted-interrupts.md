# Posted Interrupts (PI) 深度学习

> 基于 Linux 6.12.93 + Intel VT-d Spec + QEMU 10.1.0-rc2

---

## 🎯 学习目标

掌握 Posted Interrupts 的核心机制：
1. PI Descriptor 的硬件结构和字段含义
2. PI 模式的完整工作流程
3. KVM 中 PI 相关的关键代码
4. PI 与 Remapped 模式的差异
5. PI 在 VFIO 设备直通中的应用

---

## 📚 学习路线

```
1. PI Descriptor 硬件结构
   · 64 字节对齐的物理结构
   · PIR (Posted Interrupt Request) 位图
   · ON/SN/NV/NDST 控制字段

2. PI 模式 IRTE 格式
   · IM=1 标识 PI 模式
   · VV (Virtual Vector) 字段
   · PDA (Posted Descriptor Address) 字段

3. PI 工作流程
   · 设备发送 MSI
   · IOMMU 拦截并写入 PI Descriptor
   · 发送通知中断到 pCPU
   · 硬件自动处理，零 VM-Exit

4. KVM 代码实现
   · pi_desc 结构定义
   · vmx_pi_update_irte() 函数
   · vmx_sync_pir_to_irr() 函数
   · PI 调度相关代码

5. 实践环节
   · 查看系统 PI Descriptor
   · 追踪 PI 相关 trace 事件
   · 对比 PI vs Remapped 模式
```

---

## 🔍 第一部分：PI Descriptor 硬件结构

### 1.1 PI Descriptor 概述

**来源**: Intel VT-d Spec Section 9.11

```
Posted Interrupt Descriptor (PID)
  · 64 字节大小
  · 64 字节对齐
  · 每个 vCPU 一个
  · 物理地址存储在 IRTE 的 PDA 字段

作用:
  · 记录 pending 的中断 (PIR 位图)
  · 控制通知行为 (ON/SN 字段)
  · 指定通知目标 (NV/NDST 字段)
```

### 1.2 PI Descriptor 内存布局

```
Posted Interrupt Descriptor (64 bytes):

  Byte 0x00 ┌────────────────────────────────────────┐
            │                                         │
            │        PIR[255:0] (32 bytes)            │
            │    每个 bit 对应一个中断向量            │
            │    bit 0  → vector 0                    │
            │    bit 32 → vector 32                   │
            │    bit 255 → vector 255                 │
            │                                         │
  Byte 0x1F └────────────────────────────────────────┘
  
  Byte 0x20 ┌────┬────┬────────────┬─────────────────┐
            │ ON │ SN │ Reserved   │      NV         │
            │1bit│1bit│  6 bits    │    8 bits       │
            └────┴────┴────────────┴─────────────────┘
            
  Byte 0x24 ┌─────────────────────────────────────────┐
            │              NDST (32 bits)             │
            │         目标 pCPU 的 APIC ID            │
            └─────────────────────────────────────────┘
            
  Byte 0x28 ┌─────────────────────────────────────────┐
            │                                         │
            │         Reserved (36 bytes)             │
            │                                         │
  Byte 0x3F └─────────────────────────────────────────┘
```

### 1.3 关键字段详解

#### ON (Outstanding Notification) - 防止重复通知

**核心作用**: 避免为同一个 vCPU 的多个 pending 中断发送多个通知中断

**工作机制**:
```
① 第一个中断到达:
   · IOMMU 设置 PIR[vec1]=1
   · 检查 ON=0 → 设置 ON=1
   · 发送通知中断到 NDST

② 第二个中断到达（ON 仍为 1）:
   · IOMMU 设置 PIR[vec2]=1
   · 检查 ON=1 → 不发送通知
   · 多个中断"搭便车"在同一个通知上

③ CPU 处理通知中断:
   · 硬件自动清除 ON=0
   · 硬件将 PIR 同步到 VIRR
   · 评估虚拟中断

④ 第三个中断到达（ON 已为 0）:
   · IOMMU 设置 PIR[vec3]=1
   · 检查 ON=0 → 设置 ON=1
   · 发送新的通知中断
```

**性能优化效果**（10G 网卡，100 个包/μs）:
```
没有 ON 字段：
  · 100 次通知中断 → 性能差

有 ON 字段：
  · 1 次通知中断 → 性能提升 100 倍
```

#### SN (Suppress Notification) - 抑制非紧急中断

**核心作用**: 允许 VMM 暂时屏蔽非紧急中断的通知，用于 vCPU 调度、迁移等场景

**与 URG (Urgent) 字段配合**:
```
URG=0: 非紧急中断（普通设备中断）
URG=1: 紧急中断（错误、超时等关键中断）

SN=0: 正常模式，所有中断都发送通知
SN=1: 抑制模式
  · 非紧急中断 (URG=0): 不发送通知（只写 PIR）
  · 紧急中断 (URG=1): 仍然发送通知（确保关键中断不被延迟）
```

**典型使用场景**:
```
场景 1: vCPU 被抢占
  · VMM 调度 vCPU 出去时设置 SN=1
  · 避免被非紧急中断打断
  · 但仍接收紧急中断（如设备错误）
  · vCPU 重新调度时清除 SN=0

场景 2: vCPU 迁移
  · 迁移过程中设置 SN=1
  · 避免在迁移过程中收到通知
  · 迁移完成后清除 SN=0

场景 3: 关键操作
  · VMM 执行关键操作（如 EPT 更新）时临时设置 SN=1
  · 避免被中断打断
  · 操作完成后清除 SN=0
```

#### NV (Notification Vector) - 通知向量

**作用**: 指定通知中断的向量号，通常为 POSTED_INTR_VECTOR (0xf7)

#### NDST (Notification Destination) - vCPU 到 pCPU 的桥梁

**核心作用**: 告诉 IOMMU 通知中断应该发送到哪个物理 CPU

**本质**:
```
NDST = 物理 CPU 的 APIC ID（不是 vCPU ID！）
表示：vCPU 当前在哪个物理 CPU 上运行
类型：u32（32 位字段）
```

**代码验证**:
```c
/* arch/x86/kvm/vmx/posted_intr.c:97,109 */
dest = cpu_physical_id(cpu);  // 获取物理 CPU 的 APIC ID
new.ndst = dest;              // 设置 NDST

/* arch/x86/include/asm/smp.h:129 */
#define cpu_physical_id(cpu)  per_cpu(x86_cpu_to_apicid, cpu)
```

**更新时机**:
```
vCPU 调度到 pCPU:
  vmx_vcpu_load(vcpu, cpu)
    → vmx_vcpu_pi_load(vcpu, cpu)
      → dest = cpu_physical_id(cpu)
      → pi_desc->ndst = dest
      → pi_desc->nv = POSTED_INTR_VECTOR
      → pi_desc->sn = 0

vCPU 从 pCPU 卸载:
  vmx_vcpu_put(vcpu)
    → vmx_vcpu_pi_put(vcpu)
      → 如果 vCPU 被抢占，设置 SN=1

vCPU 迁移:
  · vCPU 从 pCPU-0 迁移到 pCPU-3
  · KVM 更新 pi_desc->ndst = pCPU-3 的 APIC ID
  · 所有设备中断自动路由到 pCPU-3
```

**设计优势**:
```
集中管理：每个 vCPU 一个 NDST
更新高效：vCPU 迁移只更新一个字段
自动路由：所有设备中断自动路由到新 pCPU
无需更新 IRTE：IRTE 的 PDA 不变，指向同一个 PI Descriptor
```

### 1.4 IOMMU 的原子操作

**来源**: Intel VT-d Spec Section 5.2.3

当设备发送 MSI，IOMMU 写入 PI Descriptor 时：

```
① 原子地读取 PI Descriptor 的当前值

② 设置 PIR[vector] = 1

③ 计算是否发送通知:
   X = ((ON == 0) & (URG | (SN == 0)))
   
   解释:
   · ON == 0: 当前没有 pending 通知
   · URG: 中断被标记为紧急（IRTE.URG=1）
   · SN == 0: 没有抑制通知
   · 如果 X=1，表示需要发送通知

④ 如果 X=1:
   · 设置 ON = 1
   · 发送通知中断（向量=NV，目标=NDST）

⑤ 如果 X=0:
   · 不设置 ON（已经是 1）
   · 不发送通知（防止重复通知）

⑥ 原子地写回 PI Descriptor
```

---

## 🔍 第二部分：PI 模式 IRTE 格式

### 2.1 PI 模式 vs Remapped 模式

```
Remapped 模式 (IM=0):
  · IOMMU 重映射中断向量和目标 CPU
  · 生成新的 MSI 消息
  · 投递到物理 LAPIC
  · 需要 Host 内核处理

Posted 模式 (IM=1):
  · IOMMU 直接写入 PI Descriptor
  · 不生成新的 MSI 消息
  · 发送通知中断到 pCPU
  · 硬件自动处理，零 VM-Exit
```

### 2.2 PI 模式 IRTE 字段

**来源**: Intel VT-d Spec Section 9.10

```
Posted 模式 IRTE (128 bits):

  Low 64 bits:
  ┌────┬─────┬──────┬──────┬─────┬─────┬─────┬──────┬──────┬─────────┐
  │ P  │ FPD │Rsvd  │ AVAIL│Rsvd │ URG │ IM  │  VV  │Rsvd  │  PDAL   │
  │1bit│1bit │6bits │4bits │2bits│1bit │1bit │8bits │14bits│ 26bits  │
  └────┴─────┴──────┴──────┴─────┴─────┴─────┴──────┴──────┴─────────┘
  
  High 64 bits:
  ┌──────────┬──────┬──────┬──────┬───────────────────────────────┐
  │   SID    │  SQ  │  SVT │ Rsvd │            PDAH               │
  │  16bits  │2bits │2bits │12bits│           32bits              │
  └──────────┴──────┴──────┴──────┴───────────────────────────────┘

关键字段:
  P    = Present (有效位)
  FPD  = Fault Processing Disable
  URG  = Urgent (紧急中断标志)
  IM   = IRTE Mode (1=Posted, 0=Remapped)
  VV   = Virtual Vector (通知向量)
  PDAL = Posted Descriptor Address Low (bits 31:6 of address)
  PDAH = Posted Descriptor Address High (bits 63:32 of address)
  SID  = Source ID (设备 BDF)
  SQ   = Source ID Qualifier
  SVT  = Source Validation Type
```

### 2.3 PDA 字段详解

```
PDA (Posted Descriptor Address) = PI Descriptor 的物理地址

PDA 组成:
  PDAH (bits 127:96) = address[63:32]
  PDAL (bits 63:38)  = address[31:6]
  
注意:
  · PI Descriptor 必须 64 字节对齐
  · address[5:0] 总是 0，所以不需要存储
  · PDA 总共 58 bits，支持完整的物理地址空间

示例:
  PI Descriptor 物理地址 = 0x123456000
  
  PDAH = 0x00000001 (address[63:32])
  PDAL = 0x12345600 (address[31:6], 右移 6 位)
```

### 2.4 PI 模式 IRTE 初始化

```c
/* 内核代码: drivers/iommu/intel/irq_remapping.c */

static void prepare_irte_posted(struct irte *irte)
{
    memset(irte, 0, sizeof(*irte));
    
    irte->present = 1;    /* P=1, 有效 */
    irte->p_pst = 1;      /* IM=1, Posted 模式 */
    
    /* 其他字段后续设置:
     *   p_vector = 通知向量 (VV)
     *   pda_l = PDA 低位
     *   pda_h = PDA 高位
     *   sid = 设备 BDF
     */
}
```

---

## 🔍 第三部分：PI 工作流程

### 3.1 设备初始化阶段 (Host 侧)

```
QEMU 启用 VFIO 设备:
  ioctl(VFIO_DEVICE_SET_IRQS)
    ↓
VFIO 内核驱动:
  pci_alloc_irq_vectors()
    ↓
IOMMU 分配 IRTE:
  intel_irq_remapping_prepare_irte()
  prepare_irte_posted()  ← 设置 IM=1
    ↓
设置 IRTE 字段:
  irte.p_vector = POSTED_INTR_VECTOR
  irte.pda_l = pi_desc_addr[31:6]
  irte.pda_h = pi_desc_addr[63:32]
  irte.sid = device BDF
    ↓
写入设备 MSI-X 表:
  __pci_write_msi_msg()
  Address = IR 基址 + Handle
  Data = Subhandle
```

### 3.2 中断发送阶段

```
设备收到数据包:
  查找 MSI-X 表
  发送 MSI (Address, Data)
    ↓
IOMMU 拦截:
  提取 Handle = Address[19:5] + Address[2]
  查 IRTE[Handle]
  检查 IM=1 → Posted 模式
    ↓
IOMMU 写入 PI Descriptor:
  原子操作:
    PIR[VV] = 1  ← 设置 pending 中断
    if (ON == 0 && (URG || SN == 0)):
      ON = 1  ← 设置通知标志
      发送通知中断到 NDST
    ↓
pCPU 收到通知中断:
  向量 = NV (通常为 POSTED_INTR_VECTOR)
    ↓
硬件特殊处理（Intel SDM Section 30.6）:
  ① 检查物理向量 == posted-interrupt notification vector
  ② 不触发 VM-Exit！硬件自动处理
  ③ 原子地清除 ON = 0
  ④ 发送 EOI 给本地 APIC
  ⑤ 将 PIR 逻辑或到 VIRR，清除 PIR
  ⑥ 更新 RVI = max(旧 RVI, PIR 中最高位的索引)
  ⑦ 评估待处理的虚拟中断
  ⑧ 如果识别到虚拟中断，立即投递给 Guest
    ↓
Guest 处理中断:
  零 VM-Exit！
```

### 3.3 零 VM-Exit 的关键机制

**Intel SDM Section 30.6 的说明**:

当 "process posted interrupts" VM-execution control 为 1 时：

```
外部中断到达时的处理：
  ① 本地 APIC 确认中断，获取物理向量
  
  ② 关键判断:
     if (物理向量 == posted-interrupt notification vector) {
         // 不触发 VM-Exit！
         // 硬件特殊处理
         继续执行步骤 3-7
     } else {
         // 正常 VM-Exit
         触发 VM-Exit，保存向量到 VM-exit 信息字段
     }
  
  ③ 原子地清除 PI Descriptor 的 ON 位
  
  ④ 向本地 APIC 的 EOI 寄存器写 0
  
  ⑤ 将 PIR 逻辑或到 VIRR，并清除 PIR
     （原子操作，不可被中断）
  
  ⑥ 更新 RVI
  
  ⑦ 评估待处理的虚拟中断
  
  整个过程以不可中断的方式执行。
  如果步骤 7 识别到虚拟中断，处理器可以立即投递该中断。
```

**VT-d Spec Section 5.2.5 的说明**:

```
"This allows all interrupts for this virtual processor that 
 are received while it is active (running) are processed by 
 the processor hardware without transferring control to the 
 VMM software."

"The processor hardware processes these notification events 
 by transferring any posted interrupts in the Posted Interrupt 
 Descriptor to the Virtual-APIC page of the virtual processor 
 and directly delivering it (without VMM software intervention) 
 to the virtual processor."

关键词：
  · "without transferring control to the VMM" = 不触发 VM-Exit
  · "directly delivering it" = 直接投递给 Guest
  · "without VMM software intervention" = 不需要 VMM 干预
```

### 3.4 与 Remapped 模式的对比

```
Remapped 模式:
  设备 MSI → IOMMU → 重映射 → 物理 LAPIC
  → Host 内核 IRQ handler → KVM → vLAPIC
  → VM-Entry 注入
  
  需要：
  · 完整的 host 内核中断处理路径
  · 多次软件层处理
  · 较长的延迟 (~10μs)

PI 模式:
  设备 MSI → IOMMU → PI Descriptor → 通知中断
  → 硬件特殊处理 → PIR → VIRR → Guest
  
  优势：
  · IOMMU 硬件直接处理（非常快）
  · 硬件自动处理通知中断（零 VM-Exit）
  · 不需要 host 内核介入
  · 不需要 KVM 干预
  · 整体路径极短，延迟极低 (<1μs)
  · 性能提升约 10 倍以上
```

---

## 🔍 第四部分：KVM 代码实现

### 4.1 PI Descriptor 结构定义

**来源**: `arch/x86/include/asm/posted_intr.h`

```c
struct pi_desc {
    /* Posted Interrupt Request bitmap (256 bits) */
    union {
        struct {
            DECLARE_BITMAP(pir, 256);  /* PIR 位图 */
        };
        u32 pir_32[8];
        u64 pir_64[4];
    };
    
    /* Control fields */
    union {
        struct {
            u8  on      : 1,  /* Outstanding Notification */
                sn      : 1,  /* Suppress Notification */
                rsvd_1  : 6;  /* Reserved */
            u8  nv;           /* Notification Vector */
            u32 ndst;         /* Notification Destination (pCPU APIC ID) */
        };
        u64 control;          /* 用于原子操作 */
    };
    
    u8  rsvd_2[28];          /* Reserved to 64 bytes */
} __aligned(64);
```

### 4.2 关键函数：vmx_pi_update_irte()

**来源**: `arch/x86/kvm/vmx/vmx.c`

```c
void vmx_pi_update_irte(struct kvm_vcpu *vcpu,
                        struct kvm_kernel_irq_routing_entry *e,
                        int dest_id)
{
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    struct pi_desc *pi_desc = &vmx->pi_desc;
    struct irte irte;
    
    /* 构造 Posted 模式 IRTE */
    irte.p_present = 1;
    irte.p_pst = 1;                    /* IM=1, Posted 模式 */
    irte.p_vector = POSTED_INTR_VECTOR; /* 通知向量 */
    irte.p_urgent = 0;
    
    /* 设置 PDA (PI Descriptor 物理地址) */
    irte.pda_l = virt_to_phys(pi_desc) >> 6;
    irte.pda_h = (u64)virt_to_phys(pi_desc) >> 32;
    
    /* 设置 Source ID (设备 BDF) */
    irte.sid = kvm_assigned_dev_interrupt_msix_get_sid(e);
    
    /* 设置目标 pCPU */
    irte.dest_id = dest_id;
    
    /* 写入 IOMMU */
    modify_irte(&irte);
}
```

### 4.3 关键函数：vmx_sync_pir_to_irr()

**来源**: `arch/x86/kvm/vmx/vmx.c:6912`

```c
int vmx_sync_pir_to_irr(struct kvm_vcpu *vcpu)
{
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    int max_irr;
    bool got_posted_interrupt;
    
    if (KVM_BUG_ON(!enable_apicv, vcpu->kvm))
        return -EIO;
    
    if (pi_test_on(&vmx->pi_desc)) {
        /* ON=1 说明 IOMMU 投递了中断 */
        pi_clear_on(&vmx->pi_desc);
        /* 内存屏障，确保在读取 PIR 之前清除 ON */
        smp_mb__after_atomic();
        
        /* 将 PIR 拷贝到 vLAPIC 的 IRR */
        got_posted_interrupt =
            kvm_apic_update_irr(vcpu, vmx->pi_desc.pir, &max_irr);
    } else {
        max_irr = kvm_lapic_find_highest_irr(vcpu);
        got_posted_interrupt = false;
    }
    
    /* 更新 RVI (Requested Virtual Interrupt) */
    if (!is_guest_mode(vcpu) && kvm_vcpu_apicv_active(vcpu))
        vmx_set_rvi(max_irr);
    else if (got_posted_interrupt)
        kvm_make_request(KVM_REQ_EVENT, vcpu);
    
    return max_irr;
}
```

**调用时机**:

```c
/* arch/x86/kvm/x86.c:11010-11017 */

/* vcpu_enter_guest() 中，进入 Guest 前 */

/*
 * Process pending posted interrupts to handle the case where the
 * notification IRQ arrived in the host, or was never sent (because the
 * target vCPU wasn't running).
 */
if (kvm_lapic_enabled(vcpu))
    kvm_x86_call(sync_pir_to_irr)(vcpu);
```

**注意**: `vmx_sync_pir_to_irr()` 主要用于处理通知中断在 Host 模式到达的情况，或者 vCPU 不在运行时的情况。当 vCPU 在 Guest 模式运行时，硬件会自动处理 PIR→VIRR 同步。

### 4.4 PI 调度相关代码

**来源**: `arch/x86/kvm/vmx/posted_intr.c`

```c
/* vCPU 加载到 pCPU 时 */
void vmx_vcpu_pi_load(struct kvm_vcpu *vcpu, int cpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    struct pi_desc old, new;
    unsigned int dest;
    
    if (!enable_apicv || !lapic_in_kernel(vcpu))
        return;
    
    /* 如果 vCPU 没有被迁移且不在 wakeup 列表上，只需清除 SN */
    if (pi_desc->nv != POSTED_INTR_WAKEUP_VECTOR && vcpu->cpu == cpu) {
        if (pi_test_and_clear_sn(pi_desc))
            goto after_clear_sn;
    }
    
    /* 原子更新整个 control 字段 */
    do {
        old.control = READ_ONCE(pi_desc->control);
        new = old;
        
        new.sn = 0;    /* 清除 SN，允许通知 */
        
        /* 设置 NDST = 当前 pCPU 的物理 APIC ID */
        dest = per_cpu(x86_cpu_to_physical_apicid, cpu);
        new.ndst = dest;
        
        /* 设置通知向量 */
        new.nv = POSTED_INTR_VECTOR;
        
    } while (cmpxchg64(&pi_desc->control,
                       old.control, new.control) != old.control);

after_clear_sn:
    /* 如果 PIR 非空，触发 KVM_REQ_EVENT */
    if (!pi_is_pir_empty(pi_desc))
        kvm_make_request(KVM_REQ_EVENT, vcpu);
}

/* vCPU 从 pCPU 卸载时 */
void vmx_vcpu_pi_put(struct kvm_vcpu *vcpu)
{
    struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);
    
    if (!vmx_needs_pi_wakeup(vcpu))
        return;
    
    /* 如果 vCPU 将被阻塞且中断未被屏蔽，启用 wakeup handler */
    if (kvm_vcpu_is_blocking(vcpu) && !vmx_interrupt_blocked(vcpu))
        pi_enable_wakeup_handler(vcpu);
    
    /* 如果 vCPU 被抢占，设置 SN 位抑制通知 */
    if (vcpu->preempted)
        pi_set_sn(pi_desc);
}

/* 通知中断处理（vCPU 被阻塞时） */
void pi_wakeup_handler(void)
{
    int cpu = smp_processor_id();
    struct list_head *wakeup_list = &per_cpu(wakeup_vcpus_on_cpu, cpu);
    raw_spinlock_t *spinlock = &per_cpu(wakeup_vcpus_on_cpu_lock, cpu);
    struct vcpu_vmx *vmx;
    
    raw_spin_lock(spinlock);
    list_for_each_entry(vmx, wakeup_list, pi_wakeup_list) {
        /* 如果 ON=1，唤醒 vCPU */
        if (pi_test_on(&vmx->pi_desc))
            kvm_vcpu_wake_up(&vmx->vcpu);
    }
    raw_spin_unlock(spinlock);
}
```

---

## 🔍 第五部分：实践环节

### 5.1 查看系统 PI Descriptor

```bash
# 查看 IOMMU 的 IRTE (包括 Posted 模式)
sudo cat /sys/kernel/debug/iommu/intel/ir_translation_struct

# 输出示例:
Posted Interrupt supported on IOMMU: dmar0
 IR table address:0x123456000
 Entry SrcID   PDA_high PDA_low  Vct IRTE_high		IRTE_low
 0     03:00.0 00000001 23456040 f7  0000000123456789	00000000abcdef01
 1     03:00.1 00000001 23456080 f7  000000012345678a	00000000abcdef01

解读:
  Entry 0:
    · SrcID = 03:00.0 (设备 BDF)
    · PDA = 0x123456040 (PI Descriptor 物理地址)
    · Vct = 0xf7 (通知向量)
```

### 5.2 追踪 PI 相关 trace 事件

```bash
# 启用 KVM PI 相关 trace 事件
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_pi_irte_update/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_pi_desc_init/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_pi_set_pir/enable

# 查看 trace 输出
cat /sys/kernel/debug/tracing/trace_pipe

# 示例输出:
kvm_pi_irte_update: vcpu=0 vector=32 irte_index=10
kvm_pi_set_pir: vcpu=0 vector=32 pir=0x1
kvm_pi_desc_init: vcpu=0 pi_desc_addr=0x123456000
```

### 5.3 对比 PI vs Remapped 模式

```bash
# 方法 1: 查看 IRTE 模式
sudo cat /sys/kernel/debug/iommu/intel/ir_translation_struct | grep "Posted"

# 方法 2: 使用 perf 统计 VM-Exit
sudo perf kvm stat record -a
sudo perf kvm stat report

# 查看 EXTERNAL_INTERRUPT 退出次数
# PI 模式下应该显著少于 Remapped 模式

# 方法 3: 禁用 APICv 对比
# 禁用 APICv (强制使用 Remapped 模式)
echo 0 > /sys/module/kvm_intel/parameters/enable_apicv

# 运行测试
# ...

# 启用 APICv (允许 PI 模式)
echo 1 > /sys/module/kvm_intel/parameters/enable_apicv

# 再次运行测试
# ...
```

---

## 📊 总结

### PI 核心要点

```
1. PI Descriptor 是硬件结构
   · 64 字节对齐，每个 vCPU 一个
   · 包含 PIR、ON、SN、NV、NDST 字段
   · IOMMU 和 CPU 硬件共享访问

2. PI 模式通过 IM=1 标识
   · IRTE 包含 VV (通知向量) 和 PDA (PI Descriptor 地址)
   · IOMMU 直接写入 PI Descriptor，不生成新的 MSI

3. PI 工作流程
   · 设备发 MSI → IOMMU 写 PI Descriptor
   · 通知中断到达 → 硬件特殊处理（不触发 VM-Exit）
   · PIR → VIRR → Guest 直接处理
   · 整个过程：0 次 VM-Exit

4. PI 的真正优势
   · IOMMU 硬件直接处理
   · 硬件自动处理通知中断
   · 不需要 host 内核介入
   · 不需要 KVM 干预
   · 整体路径极短，延迟极低 (<1μs)
   · 性能提升约 10 倍以上

5. ON 字段的作用
   · 防止重复发送通知中断
   · 多个中断"搭便车"在同一个通知上
   · 性能提升 10-100 倍

6. SN 字段的作用
   · 抑制非紧急中断的通知
   · 用于 vCPU 调度、迁移等场景
   · 紧急中断不受影响

7. NDST 字段的作用
   · 告诉 IOMMU 通知中断发送到哪个物理 CPU
   · vCPU 迁移时只需更新 NDST
   · 所有设备中断自动路由到新 pCPU

8. 适用场景
   · VFIO 设备直通
   · 高性能网卡 (10G/100G)
   · GPU 直通
   · 需要极低延迟的设备
```

### 常见误解澄清

```
误解 1: "PI 模式在某些情况下会有 VM-Exit"
  ✗ 错误！
  ✓ 正确：PI 模式在所有情况下都可以实现零 VM-Exit
    - vCPU 在 Guest 模式 → 硬件特殊处理，0 次 VM-Exit
    - vCPU 在 Host 模式 → 直接处理，0 次 VM-Exit
    - vCPU 被阻塞 → 唤醒处理，0 次 VM-Exit

误解 2: "通知中断会触发 VM-Exit"
  ✗ 错误！
  ✓ 正确：当向量 = posted-interrupt notification vector 时
    · 硬件特殊处理
    · 不触发 VM-Exit
    · 自动完成 PIR→VIRR 同步

误解 3: "PI 模式需要 KVM 干预"
  ✗ 错误！
  ✓ 正确：
    · 硬件自动处理通知中断
    · 不需要 KVM 干预
    · sync_pir_to_irr 只在特殊情况下使用

误解 4: "PI 模式延迟 ~1-2μs"
  ✗ 错误！
  ✓ 正确：
    · 真正的零 VM-Exit
    · 延迟 <1μs（接近硬件极限）
    · 性能提升 10 倍以上
```

---

## 🎯 下一步

完成 PI 学习后，可以继续：
1. 实践：在真实环境中测试 PI
2. 深入：研究 PI 与 APICv 的协同
3. 扩展：了解 Posted Interrupts 在嵌套虚拟化中的应用
4. 优化：探索 PI 的性能调优技巧

---

**参考源码**:
- 内核: `arch/x86/include/asm/posted_intr.h`, `arch/x86/kvm/vmx/vmx.c`, `arch/x86/kvm/vmx/posted_intr.c`
- IOMMU: `drivers/iommu/intel/irq_remapping.c`
- QEMU: `hw/vfio/pci.c`, `accel/kvm/kvm-all.c`
- 规范: `intel-vtd.pdf` Section 9.10, 9.11; `intel-vmx.pdf` Section 30.6
