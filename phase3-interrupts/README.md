# 第三阶段：中断虚拟化与 VT-d 中断重映射

> 基于 Linux 6.12.93 内核源码 | 预计学习时间：2-3 周
>
> **重要**：中断虚拟化和 VT-d 中断重映射是同一条数据路径的两半，必须结合学习。

---

## 📋 学习目标

本阶段覆盖从**物理设备中断**到 **Guest vCPU** 的完整路径：

```
设备MSI → IOMMU(IRTE) → PI描述符(PIR) → vCPU(IR→ISR) → Guest中断处理
         ↑ VT-d层         ↑ Posted Interrupts机制        ↑ KVM层
```

完成本阶段后，你应该能够：
1. 画出设备中断到Guest的完整路径（传统模式和PI模式）
2. 理解 IRTE 格式及其与 PI 描述符的关联
3. 跟踪 `vmx_pi_update_irte()` 这个KVM-VT-d桥梁函数
4. 理解 PI 的 ON/SN/NDV/NDA 字段在调度中如何变化
5. 用 ftrace 观察中断注入的完整过程

---

## 🏗️ 技术全景

### 1.1 为什么需要这两层？

```
┌──────────────────────────────────────────────────────────────────────┐
│              完整中断虚拟化 = KVM中断层 + VT-d中断重映射              │
│                                                                      │
│  传统模式 (无APICv/PI):                                              │
│    设备MSI → IOMMU重映射 → 物理APIC → Host内核 → KVM → vLAPIC      │
│    ✗ 每次中断至少1次VM-Exit (Host处理→注入到Guest)                   │
│                                                                      │
│  APICv模式 (Virtual Interrupt Delivery):                             │
│    设备MSI → IOMMU重映射 → 物理APIC → Host内核 → KVM → vLAPIC     │
│    → VM-Entry时硬件自动注入 (减少VM-Exit, 但注入仍需软件触发)       │
│                                                                      │
│  APICv + Posted Interrupts 模式:                                    │
│    设备MSI → IOMMU → 直接写PI描述符 → vCPU的PIR                    │
│    → VM-Entry时硬件自动PIR→IRR → Guest直接处理                      │
│    ✓ 零VM-Exit！中断从设备直达Guest                                  │
│                                                                      │
│  关键桥梁函数: vmx_pi_update_irte()                                 │
│    KVM侧 (pi_desc) ←→ VT-d侧 (IRTE)                               │
│    将vCPU的PI描述符物理地址写入IRTE的PDA字段                        │
└──────────────────────────────────────────────────────────────────────┘
```

### 1.2 中断硬件演进：PIC → APIC → APICv + PI

理解中断虚拟化之前，先理解物理硬件的演进。每一代硬件解决了什么问题，KVM 就对应有什么样的虚拟化方案。

```
┌─────────────────────────────────────────────────────────────────────┐
│                   中断硬件演进                                       │
│                                                                     │
│  ① PIC (8259A, 单CPU时代)                                          │
│     设备 → IR引脚 → PIC芯片(INR/IMR/ISR) → INTR引脚 → CPU         │
│     ✗ 只有8个引脚, 只能级连, 只能支持单CPU                          │
│     KVM: kvm_pic 模拟 master/slave 级连                             │
│                                                                     │
│  ② APIC (多CPU时代)                                                │
│     设备 → IOAPIC(24个RTE) → 总线消息 → LAPIC → CPU               │
│     ✓ 支持多CPU, 24个引脚, 可通过MSI直接投递                        │
│     KVM: kvm_ioapic + kvm_lapic 模拟                                │
│                                                                     │
│  ③ MSI/MSI-X (绕过IOAPIC)                                         │
│     设备 → 直接产生MSI消息(Address+Data) → 总线 → LAPIC → CPU     │
│     ✓ 无引脚数量限制, 延迟更低                                      │
│     KVM: 通过irqfd机制, MSI消息直接路由到vLAPIC                    │
│                                                                     │
│  ④ APICv (硬件辅助中断投递)                                        │
│     vAPIC Page (Host+Guest共享): Guest读写LAPIC寄存器不触发VM-Exit │
│     Virtual Interrupt Delivery: 硬件自动评估IRR并注入               │
│     ✓ 大幅减少中断相关VM-Exit                                       │
│     KVM: VMCS.VIRTUAL_APIC_PAGE_ADDR + VMCS.GUEST_INTR_STATUS     │
│                                                                     │
│  ⑤ Posted Interrupts (零VM-Exit中断投递)                          │
│     设备MSI → IOMMU(IR) → 直接写PI描述符(PIR) → pCPU → vCPU     │
│     ✓ 外部中断不触发VM-Exit, 硬件自动PIR→IRR                      │
│     KVM: vmx_sync_pir_to_irr + vmx_pi_update_irte                 │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.3 PIC 虚拟化 (kvm_pic)

**硬件原理**: PIC (8259A) 通过 IR0~IR7 引脚接收中断，内部有 IRR(请求)/IMR(屏蔽)/ISR(服务中) 三个寄存器。CPU 每执行完一条指令检查 INTR 引脚电平。

**KVM 模拟** (源码: `arch/x86/kvm/i8259.c`):

```
虚拟PIC中断处理流程:

① 中断投递 (等价于硬件拉高INTR引脚)
   irqfd注入 → kvm_set_irq → kvm_pic_set_irq → pic_set_irq1
   → 设置 IRR[irq] = 1 (区分边沿/电平触发)
   → pic_update_irq() 评估优先级

② 中断评估 (等价于CPU检查INTR引脚)
   pic_get_irq(): 结合IMR(屏蔽)、IRR(请求)、ISR(服务中)
   选择最高优先级的中断
   slave PIC 通过 IR2 级连到 master PIC

③ 通知vCPU (等价于INTR引脚拉高)
   pic_irq_request(): 设置 s->output = level
   pic_unlock(): 遍历vCPU, kvm_vcpu_kick() 唤醒

④ Guest收到中断 (VM-Entry时注入)
   kvm_check_and_inject_events() → kvm_cpu_get_interrupt()
   → kvm_cpu_get_extint() → kvm_pic_read_irq() → pic_intack()
   → kvm_x86_call(inject_irq)() → vmcs_write32(VM_ENTRY_INTR_INFO_FIELD)

⑤ INTA确认 (等价于CPU发INTA)
   pic_intack(): 设置 ISR[irq]=1, 清除 IRR[irq]=1 (边沿触发)
   如果 auto_eoi: 直接清除 ISR

⑥ EOI (Guest写PIC端口)
   Guest写IO端口0x20/0xA0 → pic_ioport_write()
   → pic_clear_isr(): 清除 ISR[irq]=0
   → pic_update_irq(): 检查是否还有pending中断
```

**关键: 虚拟化中的"INTR引脚"**

物理硬件中，CPU每条指令后检查 INTR 引脚电平。虚拟化中没有物理引脚，KVM 通过两种方式模拟：
- **VM-Entry 时检查**: `kvm_check_and_inject_events()` 在每次 VM-Entry 前检查是否有待注入中断
- **Interrupt Window Exiting**: 如果 Guest 关了中断 (IF=0)，KVM 开启 `CPU_BASED_INTR_WINDOW_EXITING`，硬件在 Guest 重新开中断 (STI) 后立即触发 VM-Exit，让 KVM 注入中断

### 1.4 IOAPIC 虚拟化 (kvm_ioapic)

**硬件原理**: IOAPIC 有 24 个引脚，每个对应一个 RTE (Redirection Table Entry, 64位)。引脚收到信号后，将 RTE 格式化为中断消息发给 LAPIC。

**KVM 模拟** (源码: `arch/x86/kvm/ioapic.c`):

```
虚拟IOAPIC中断处理流程:

① Guest写RTE (MMIO)
   Guest写IOAPIC寄存器 → ioapic_mmio_write → ioapic_write_indirect
   → 更新 redirtbl[index] (64位RTE分两次32位写)

② 中断触发
   irqfd → kvm_set_irq → kvm_ioapic_set_irq → ioapic_set_irq
   → 设置 ioapic->irr |= (1 << irq)
   → ioapic_service()

③ RTE格式化为LAPIC消息
   ioapic_service():
   → 检查 RTE.mask 和 RTE.remote_irr (level触发的EOI阻塞)
   → 将RTE字段填入 kvm_lapic_irq:
     {vector, dest_id, dest_mode, trig_mode, delivery_mode}
   → kvm_irq_delivery_to_apic() 投递

④ 找到目标LAPIC
   kvm_irq_delivery_to_apic():
   → 快路径: kvm_irq_delivery_to_apic_fast (kvm_apic_map缓存)
   → 慢路径: 遍历所有vCPU匹配dest_id
   → kvm_apic_set_irq → __apic_accept_irq

⑤ 投递到vCPU
   __apic_accept_irq():
   → 设置 vLAPIC TMR (触发模式)
   → kvm_x86_call(deliver_interrupt)() → vmx_deliver_interrupt()
     ├── 尝试PI投递: vmx_deliver_posted_interrupt()
     │   → PI.PIR[vector]=1, PI.ON=1, 发通知IPI
     │   ★ 如果成功: 零VM-Exit! Guest直接处理
     └── PI失败回退: kvm_lapic_set_irr()
         → 写vLAPIC IRR → kick vCPU
```

**Remote IRR 的妙用 (Level触发)**:

```
Level触发中断共享场景:
  IOAPIC引脚 ←── 设备A ──┐
              ←── 设备B ──┤ 共享同一引脚
              
  设备A触发中断:
    ① IOAPIC: RTE.remote_irr = 1, 发level-assert给LAPIC
    ② LAPIC: IRR[vec]=1, 投递给CPU
    ③ CPU: 处理设备A中断
    ④ CPU写EOI → LAPIC → IOAPIC: remote_irr = 0
    ⑤ 此时设备B仍 holding 引脚为高:
       remote_irr(0) XOR 引脚(1) = 1 → 再发level-assert
    ⑥ IOAPIC: remote_irr = 1, 再投递
    ⑦ CPU: 处理设备B中断
    ⑧ 设备B释放引脚 → 不再触发
    
  通过 remote_irr XOR 引脚电平 的机制，保证共享引脚上的所有中断都被服务。
```

### 1.5 vAPIC Page 与 APICv

**问题**: KVM 模拟的 LAPIC，Guest 每次读写 LAPIC 寄存器都会触发 VM-Exit（因为是 MMIO）。中断相关的读写非常频繁，性能很差。

**解决**: APICv 通过 VMCS 中的 `VIRTUAL_APIC_PAGE_ADDR` 字段，让 Host 和 Guest **共享**一块 4KB 的 APIC Page：

```c
/* arch/x86/kvm/vmx/vmx.c */
/* 把vLAPIC的regs物理地址注册到VMCS */
vmcs_write64(VIRTUAL_APIC_PAGE_ADDR, __pa(vmx->vcpu.arch.apic->regs));
```

```
vAPIC Page 机制:

┌─ Host (KVM) ──────────────────────── Guest ──────────────────┐
│                                                                │
│  kvm_lapic.regs (4KB)  ◄──── 共享物理页 ────►  Guest看到的    │
│  ├── IRR[0..7] (0x200)     KVM直接写IRR     LAPIC MMIO区域   │
│  ├── ISR[0..7] (0x100)     KVM直接写ISR                       │
│  ├── TMR[0..7] (0x300)                                          │
│  ├── VTPR (0x080)        Guest直接读/写TPR  (不VM-Exit!)     │
│  └── ...                                                         │
│                                                                │
│  Guest写TPR/EOI: 直接修改共享页, 不VM-Exit                    │
│  Guest写ICR (发IPI): 仍需VM-Exit (KVM需要路由IPI)            │
│  Guest读IRR/ISR: 直接读共享页, 不VM-Exit                      │
│                                                                │
│  KVM注入中断: 直接写 IRR[vec] → 硬件生成LAPIC事件给CPU      │
└────────────────────────────────────────────────────────────────┘

Virtual Interrupt Delivery (VID):
  VMCS.GUEST_INTR_STATUS.RVI = 最高优先级待处理中断
  VM-Entry时硬件自动比较 RVI vs PPR:
    如果 RVI > PPR → 自动注入, Guest直接处理 (零额外VM-Exit)
```

### 1.6 MSI/MSI-X

**为什么需要 MSI**: IOAPIC 有 24 个引脚限制。MSI 让设备直接产生中断消息写入总线，绕过 IOAPIC，无引脚限制。

```
MSI 中断投递:

设备内部 MSI Table:
┌─────────────────────────────────────────────────────┐
│ Entry 0: Address=0xFEE00000  Data=0x0042            │
│ Entry 1: Address=0xFEE00000  Data=0x0053            │
│ ...                                                   │
└─────────────────────────────────────────────────────┘
  Address 低20位 = 0xFEE00 = APIC基址 (MSI专用)
  Address[19:12] = dest_id (目标APIC ID)
  Address[3:2]   = dest_mode (0=物理, 1=逻辑)
  Data[7:0]      = vector (中断向量)
  Data[17:15]    = delivery_mode

设备触发中断 → 写(Address, Data)到PCI总线 → 路由到LAPIC

无IR时:  Address直接指向目标LAPIC
有IR时:  Address指向IOMMU的IR表, Data编码IRTE索引
         IOMMU通过IRTE重映射后投递
```

---



```### 1.7 源码阅读顺序（按数据流）

```
第1步: 理解PI描述符 (两个层的交汇点)
  arch/x86/include/asm/posted_intr.h     ← pi_desc 硬件结构

第2步: 理解IRTE (VT-d层)
  drivers/iommu/intel/irq_remapping.c    ← irq_2_iommu, intel_ir_data, IRTE

第3步: 理解KVM中断注入
  arch/x86/kvm/lapic.c                   ← vLAPIC, 中断注入到IRR
  arch/x86/kvm/irq.c                     ← kvm_set_irq, IRQ路由

第4步: 理解PI调度操作 (关键!)
  arch/x86/kvm/vmx/posted_intr.c         ← vmx_vcpu_pi_load/put
  arch/x86/kvm/vmx/vmx.c                ← vmx_sync_pir_to_irr

第5步: 理解KVM-VT-d桥梁
  arch/x86/kvm/vmx/vmx.c                ← vmx_pi_update_irte()
```

---

## 🔗 完整中断路径：传统 vs PI

### 传统模式 (每次中断 → VM-Exit)

```
① 设备触发 MSI
   写入 MSI Address + MSI Data
        │
② IOMMU 接收 MSI
   查找 IRTE[MSI Data中的索引]
   验证 Source-ID (设备BDF)
        │
③ IOMMU 重映射
   修改向量号 + 目标APIC ID
   (传统模式: DM=0, 不启用PI)
        │
④ 投递到物理 LAPIC
   触发 pCPU 外部中断
        │
⑤ ★ Host内核中断处理 ★
   Host IRQ handler 被调用
        │
⑥ KVM 处理
   kvm_set_irq() → kvm_irq_delivery_to_apic()
   → vLAPIC IRR 设置对应位
        │
⑦ ★ VM-Entry 时注入 ★
   检查IRR最高优先级
   移入ISR，清除IRR
   通过VMCS VM-Entry Interruption-Information 注入
        │
⑧ Guest 执行中断处理函数

每步开销: ⑤ ⑦ 各一次 VM-Exit (或需要KVM介入)
```

### PI 模式 (零VM-Exit!)

```
① 设备触发 MSI
   MSI Address = IOMMU IR 基址
   MSI Data    = IRTE 索引
        │
② IOMMU 接收并查找 IRTE
   看到 IM=1 → Posted模式!
   读取 IRTE 中的:
   - PDA (PI Descriptor Address) → 指向vCPU的pi_desc
   - Vector → 通知向量
   - Dest ID → pCPU的物理APIC ID
        │
③ ★ IOMMU 直接写 PI 描述符 ★
   PIR[vector] = 1          (硬件原子操作)
   ON = 1                   (设置通知位)
        │
④ IOMMU 发送通知中断
   目标 = Dest ID (pCPU APIC ID)
   向量 = Notification Vector (如 0x00)
        │
⑤ pCPU 收到通知中断
        │
   ┌────┴──── 两种情况 ────┐
   │                        │
   vCPU正在运行             vCPU未运行 (halted)
   │                        │
   硬件在VM-Exit时          pi_wakeup_handler()
   自动 PIR → IRR           ├── 清除ON
   (零额外开销!)            ├── kvm_vcpu_kick()
   │                        └── 唤醒vCPU
   ▼                        │
⑥ VM-Entry                 VM-Entry
   硬件自动 PIR → IRR       (PIR → IRR 自动同步)
   Guest直接处理中断        │
   ★ 零 VM-Exit! ★         Guest直接处理中断
```

---

## 🔧 核心数据结构（跨层）

### pi_desc — PI描述符 (KVM + VT-d 共享)

```
                    ← KVM 写入 →    ← IOMMU 写入 →

PI Descriptor (64 bytes, 64-byte aligned):

  偏移 0x00 ┌────────────────────────────────────────┐
            │          PIR[63:0]                       │  IOMMU写: 中断请求
            │   每个bit = 一个向量的中断请求位          │  KVM读: 同步到IRR
  偏移 0x08 ├────────────────────────────────────────┤
            │          PIR[127:64]                     │
  偏移 0x10 ├────────────────────────────────────────┤
            │          PIR[191:128]                    │
  偏移 0x18 ├────────────────────────────────────────┤
            │          PIR[255:192]                    │
            └────────────────────────────────────────┘
  偏移 0x20 ┌────┬────┬────┬────┬──────────┬────────┐
            │ ON │ SN │RSV │ SV │   NDV    │  NDA   │  KVM写: 调度时更新
            │ 1b │ 1b │ 1b │ 1b │   8b     │  32b   │  IOMMU写: ON位
            └────┴────┴────┴────┴──────────┴────────┘

ON  = Outstanding Notification (IOMMU设1 → KVM清0)
SN  = Suppress Notification (KVM在vCPU调出时设1)
NDV = Notification Vector (通知中断的向量号)
NDA = Notification Destination (pCPU的物理APIC ID)
```

### IRTE — 中断重映射表条目 (128位)

```
IRTE (传统模式, DM=0):
┌─────────────────────── 低64位 ───────────────────────────┐
│ P │ FPD │ DM │ RH │ TM │ DLM │ AVAIL │ RSV │ Vector │ RSV │ Dest ID │
│ 1b│ 1b  │ 1b │ 1b │ 1b │  3b │  4b   │ 5b  │  8b    │ 8b  │  32b    │
└──────────────────────────────────────────────────────────┘
┌─────────────────────── 高64位 ───────────────────────────┐
│ SID (Source-ID) │ SQ │ SVT │ Reserved                   │
│     16b (BDF)   │ 2b │ 2b  │         44b                │
└──────────────────────────────────────────────────────────┘

IRTE (Posted模式, IM=1):
┌─────────────────────── 低64位 ───────────────────────────┐
│ P │ FPD │ Rsvd │ AVAIL │ Rsvd │ URG │ IM=1 │ VV │ Rsvd │ PDAL │
│ 1b│ 1b  │  6b  │  4b   │  2b  │ 1b  │  1b  │ 8b │ 14b  │ 26b  │
└──────────────────────────────────────────────────────────┘
┌─────────────────────── 高64位 ───────────────────────────┐
│ SID │ SQ │ SVT │ Rsvd │ PDAH (PI描述符物理地址高位)      │
│ 16b │ 2b │ 2b  │ 12b  │            32b                   │
└──────────────────────────────────────────────────────────┘

字段说明 (参考 VT-d Spec Section 9.10):
  P     = Present (存在位)
  FPD   = Fault Processing Disable
  URG   = Urgent (紧急中断标志)
  IM    = IRTE Mode (★ 1=Posted模式, 0=Remapped模式)
  VV    = Virtual Vector (通知向量,会被写入PI Descriptor的NV字段)
  PDAL  = Posted Descriptor Address Low (bits 63:38, 对应地址bits 31:6)
  PDAH  = Posted Descriptor Address High (bits 127:96, 对应地址bits 63:32)
  SID   = Source Identifier (设备BDF)
  SQ    = Source-id Qualifier
  SVT   = Source Validation Type

关键: PDA字段指向vCPU的pi_desc物理地址(64字节对齐)!
这是IRTE和PI描述符之间的硬件连接。

**重要: VT-d规范术语说明** (参考 intel-vtd.pdf Section 9.10)

  在VT-d规范中,Posted模式IRTE的字段命名:
    · IM (IRTE Mode, bit 15): 1=Posted模式, 0=Remapped模式
    · VV (Virtual Vector, bits 23:16): 通知向量
    · PDAL (bits 63:38): PI描述符地址低位
    · PDAH (bits 127:96): PI描述符地址高位
    
  在PI Descriptor中 (Section 9.11):
    · NV (Notification Vector, bits 279:272): 通知向量
    
  两者关系:
    · IRTE.VV 会被IOMMU写入PI Descriptor的NV字段
    · 虽然值相同,但在不同结构中命名不同
    · IRTE中叫VV (Virtual Vector)
    · PI Descriptor中叫NV (Notification Vector)
    
  注意: 不要与Remapped模式的DM (Delivery Mode)字段混淆!
    · Remapped模式: DM字段 (bits 5-7) 指定投递模式
    · Posted模式: IM字段 (bit 15) 指定IRTE模式
```

### irq_2_iommu — IRQ到IOMMU的映射

```c
/* drivers/iommu/intel/irq_remapping.c */
struct irq_2_iommu {
    struct intel_iommu *iommu;    /* 关联的IOMMU实例 */
    u16 irte_index;               /* 在IR表中的索引 */
    u16 sub_handle;               /* 子句柄(MSI-X多向量) */
    u8  irte_mask;                /* IRTE掩码 */
    bool posted_msi;              /* PI模式是否启用 */
    bool posted_vcpu;             /* PI投递到vCPU */
};
```

---

## 🌉 桥梁函数: vmx_pi_update_irte()

这是连接 KVM 层和 VT-d 层的关键函数。当设备直通给VM时调用。

```
vmx_pi_update_irte() 调用链:

QEMU: ioctl(KVM_IRQFD) ← 配置中断投递
  → kvm_set_irq_routing()
    → kvm_vfio_setup_pi_irte()     ← 如果有VFIO设备
      → kvm_x86_ops.pi_update_irte()
        → vmx_pi_update_irte()      ← ★ KVM→VT-d的桥梁

vmx_pi_update_irte(vcpu, gvec, girq, set):
  │
  ├── 1. 通过girq找到对应的Linux IRQ
  │      kvm_find_irq_routing(girq) → irq_desc
  │
  ├── 2. 获取IRTE信息
  │      irq_2_iommu = irq_desc→chip_data
  │      ir_data = intel_ir_data
  │
  ├── 3. 构造Posted模式IRTE (参考VT-d Spec Section 9.10)
  │      irte.IM = 1          ← Posted模式 (IRTE Mode field, bit 15)
  │      irte.PDA = __pa(&vmx→pi_desc)  ← vCPU的PI描述符物理地址
  │      irte.VV = POSTED_INTR_VECTOR   ← Virtual Vector (通知向量, bits 23:16)
  │      irte.Dest_ID = pCPU APIC ID    ← 当前pCPU
  │      
  │      注: IRTE中的VV字段会被写入PI Descriptor的NV字段
  │          IRTE.VV → PI.NV (两者值相同,但命名不同)
  │
  └── 4. 写入IOMMU
         modify_irte(irq_2_iommu, &irte)
         → 写入IR表
         → qi_flush_iec()  ← 刷新IOMMU的IRTE缓存
```

---

## 🔄 vCPU调度与PI/IRTE状态变化

```
时间线:
─────┬────────────────┬──────────────────┬──────────────────────┬───→
     │                │                  │                      │
 vCPU运行在pCPU-0   被调出            被调度到pCPU-1          vCPU运行
     │                │                  │                      │
     │ pi_desc:       │                  │ pi_desc:             │
     │  SN=0 (允许)   │                  │  SN=0 (允许)         │
     │  NDA=pCPU-0    │                  │  NDA=pCPU-1  ←更新!  │
     │                │                  │                      │
     │ IRTE:          │                  │ IRTE:                │
     │  IM=1 (Posted)     │                  │  IM=1 (Posted)       │
     │  PDA=pi_desc   │                  │  PDA=pi_desc         │
     │  DestID=pCPU-0 │                  │  DestID=pCPU-1 ←更新!│
     │                │                  │                      │
     │ vmx_vcpu_     │ vmx_vcpu_put()  │ vmx_vcpu_load()      │
     │ (正常运行)     │ → pi_set_sn()   │ → pi_clear_sn()      │
     │                │   SN=1          │ → NDA=pCPU-1 APIC ID │
     │                │                 │ → 更新IRTE DestID    │
     │                │                 │                      │
     │                │  如果此时设备    │                      │
     │                │  发MSI:          │                      │
     │                │  PIR[vec]=1     │                      │
     │                │  ON=1           │                      │
     │                │  但SN=1 →       │                      │
     │                │  不发通知中断    │                      │
     │                │  等vCPU重新调度  │                      │
     │                │  时检查PIR       │                      │
```

---

## 🎯 MSI地址格式与亲和性迁移

### MSI Address的两种格式

MSI地址根据是否启用IR（中断重映射）有不同的编码方式：

```c
/* arch/x86/include/asm/msi.h */

typedef struct x86_msi_addr_lo {
    union {
        // ═══ 传统模式 (无 IR) ═══
        struct {
            u32  reserved_0       :  2,
                 dest_mode_logical:  1,   // 目标模式
                 redirect_hint    :  1,
                 reserved_1       :  1,
                 virt_destid_8_14 :  7,
                 destid_0_7       :  8,   // ★ CPU ID (8位)
                 base_address     : 12;   // 0xFEE00
        };
        
        // ═══ IR 模式 (有 IOMMU) ═══
        struct {
            u32  dmar_reserved_0  :  2,
                 dmar_index_15    :  1,   // ★ IRTE 索引 bit 15
                 dmar_subhandle_valid: 1,
                 dmar_format      :  1,   // ★ =1 表示 IR 模式
                 dmar_index_0_14  : 15,   // ★ IRTE 索引 bits 0-14
                 dmar_base_address: 12;   // 0xFEE00 (相同基址!)
        };
    };
} arch_msi_msg_addr_lo_t;
```

**关键区别**：
- **传统模式**：Address包含目标CPU ID，Data包含实际vector（每个CPU ~230个，N个CPU共N×230个）
- **IR模式**：Address包含Handle（16位=65536个），Data包含Subhandle
- **IR的优势**：Source-ID验证（安全隔离）、解耦设备和目标CPU（灵活迁移）、支持Posted模式

**VT-d规范术语说明** (参考 intel-vtd.pdf Section 5.1.2.2)

  在VT-d规范中,Remappable中断格式的字段命名:
    · Address[31:20] = 0xFEE (Interrupt Identifier)
    · Address[19:5] = Handle[14:0] (15位)
    · Address[4] = Interrupt Format (必须为1,表示Remappable格式)
    · Address[3] = SHV (SubHandle Valid)
    · Address[2] = Handle[15] (1位)
    · Address[1:0] = Don't care
    · Data[15:0] = Subhandle
    
  Handle的计算 (Section 5.1.3):
    if (SHV == 0)
        interrupt_index = Handle;
    else
        interrupt_index = Handle + Subhandle;
        
  注意: 内核代码中称为"IRTE索引",规范中称为"Handle"
  两者是同一概念,只是命名不同。

### 三种模式的亲和性迁移对比

当需要将设备中断从pCPU-0迁移到pCPU-1时，三种模式的处理方式完全不同：

```
┌─────────────────────────────────────────────────────────────────┐
│ 模式 1: 非 IR 模式（无中断重映射）                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│ 设备MSI表直接包含目标:                                          │
│   Entry 0: Address = 0xFEE00000 | (CPU-0 << 12)               │
│            Data = vector 32                                      │
│                                                                  │
│ 迁移到CPU-1:                                                    │
│   ① 锁住设备 (防止发中断)                                       │
│   ② 重新编程设备的MSI表:                                        │
│      Address = 0xFEE00500 (CPU-1)                               │
│      Data = ??? (可能要找新vector)                              │
│   ③ 解锁设备                                                    │
│                                                                  │
│ 问题:                                                           │
│   ✗ 必须动设备的MSI表 (硬件层面)                                │
│   ✗ 设备可能不支持热更新                                        │
│   ✗ 操作期间可能丢中断                                          │
│   ✗ 需要设备驱动配合                                            │
│   ✗ 复杂度最高                                                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ 模式 2: IR 模式（无 PI，DM=0）                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│ 设备MSI表包含IRTE索引:                                          │
│   Entry 0: Address = IR 基址 + IRTE 索引 10                     │
│            Data = subhandle                                       │
│                                                                  │
│ IRTE[10] 包含:                                                  │
│   Vector = 32 (系统vector)                                      │
│   Dest_ID = CPU-0                                               │
│                                                                  │
│ 迁移到CPU-1:                                                    │
│   ① 更新IRTE[10] (在IOMMU中):                                   │
│      Dest_ID = CPU-1                                            │
│      Vector = ??? (可能要找新vector)                            │
│   ② 刷新IOMMU缓存                                               │
│                                                                  │
│ 特点:                                                           │
│   ✓ 不需要动设备的MSI表 (比非IR好)                              │
│   ✓ 只需更新IOMMU的IRTE                                         │
│   ⚠️ 但Vector字段可能需要改 (如果新CPU上vector 32已占用)        │
│   ⚠️ 仍需管理vector分配                                         │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ 模式 3: IR + PI 模式（IM=1）★                                   │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│ 设备MSI表包含IRTE索引:                                          │
│   Entry 0: Address = IR 基址 + IRTE 索引 10                     │
│            Data = subhandle                                       │
│                                                                  │
│ IRTE[10] 包含:                                                  │
│   NV = 0xf7 (notification vector, 预留的)                       │
│   Dest_ID = CPU-0                                               │
│   PDA = pi_desc 物理地址                                        │
│                                                                  │
│ 迁移到CPU-1:                                                    │
│   ① 更新IRTE[10]:                                               │
│      Dest_ID = CPU-1                                            │
│      NV = 0xf7 (不用改!)                                        │
│   ② 更新pi_desc.NDA = CPU-1                                     │
│   ③ 刷新IOMMU缓存                                               │
│                                                                  │
│ 特点:                                                           │
│   ✓ 不需要动设备的MSI表                                         │
│   ✓ 只需更新IOMMU的IRTE                                         │
│   ✓ NV是预留的, 不会冲突, 不用改                                │
│   ✓ 最简单, 最快速                                              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Notification Vector的巧妙设计

PI模式之所以在亲和性迁移时更简单，关键在于Notification Vector的设计：

```
传统模式 vs PI模式的向量管理:

┌─ 传统模式 (每个设备一个vector) ─────────────────────────────┐
│  设备 A (网卡):  vector = 32 (在CPU-0)                       │
│  设备 B (NVMe):  vector = 33 (在CPU-0)                       │
│  设备 C (GPU):   vector = 34 (在CPU-0)                       │
│                                                              │
│  每个设备需要独立的vector                                     │
│  → 需要管理, 可能冲突                                        │
│  → 迁移时需要找可用的vector                                  │
└──────────────────────────────────────────────────────────────┘

┌─ PI模式 (所有设备共享notification vector) ──────────────────┐
│  设备 A (网卡):                                              │
│    → IOMMU写PI descriptor A的PIR[guest_vector_A]             │
│    → 发通知中断vector = 0xf7                                 │
│                                                              │
│  设备 B (NVMe):                                              │
│    → IOMMU写PI descriptor B的PIR[guest_vector_B]             │
│    → 发通知中断vector = 0xf7 (同一个!)                       │
│                                                              │
│  设备 C (GPU):                                               │
│    → IOMMU写PI descriptor C的PIR[guest_vector_C]             │
│    → 发通知中断vector = 0xf7 (还是同一个!)                   │
│                                                              │
│  关键:                                                       │
│    · 所有PI通知都用vector 0xf7                               │
│    · 不同设备通过PI descriptor区分 (每个vCPU一个)            │
│    · 不同guest vector通过PIR位图区分 (256个bit)              │
│    · 不会冲突!                                               │
└──────────────────────────────────────────────────────────────┘

源码定义:
  /* arch/x86/include/asm/irq_vectors.h */
  #define POSTED_INTR_VECTOR            0xf7
  #define POSTED_INTR_WAKEUP_VECTOR     0xf6
  
  这些向量在系统启动时预留:
    · 不分配给普通设备
    · 不参与普通向量分配
    · 专门用于PI机制
```

### PI的分层区分机制

PI模式通过三层机制区分不同的中断：

```
第1层: 哪个vCPU?
  · 每个vCPU有独立的PI Descriptor
  · IRTE通过PDA字段区分 (指向不同vCPU的pi_desc)
  
第2层: 哪个Guest Vector?
  · PI Descriptor内有256位的PIR位图
  · 每个bit对应一个guest vector
  · IOMMU设置PIR[guest_vector] = 1
  
第3层: 通知中断
  · 所有PI通知都用vector 0xf7
  · 通过PI descriptor地址区分不同vCPU
  · 通过PIR位图区分不同guest vector
```

### 为什么PI模式不会vector冲突？

```
vCPU迁移时的vector问题:

非PI模式的担忧:
  · 迁移到新CPU时, 新CPU的vector可能已被占用
  · 需要找空闲vector, 可能失败
  · 需要修改IRTE.Vector字段

PI模式的答案:
  · Notification vector (0xf7) 是系统预留的
    - 不占用普通vector池 (32-255)
    - 每个CPU都有, 不会冲突
  · 所有vCPU共享0xf7
    - 不同设备通过PI descriptor区分
    - 不同guest vector通过PIR位图区分
  · vCPU迁移时:
    - 只改Dest_ID和NDA
    - NV不用改 (永远是0xf7)
    - 不会有vector冲突问题

这是PI模式的巧妙设计:
  用"共享通知向量 + 分层区分"
  避免了vector管理复杂性
  简化了迁移逻辑
```

### 三种模式总结对比

```
┌─────────────────────────────────────────────────────────────────┐
│                     非IR模式        IR模式(无PI)    IR+PI模式   │
├─────────────────────────────────────────────────────────────────┤
│ 改哪里?              设备MSI表       IRTE            IRTE+PI desc│
│                      (硬件层面)      (IOMMU软件)     (IOMMU软件) │
│                                                                 │
│ 需要改vector?        是(Data字段)    是(可能)        否(NV预留)  │
│                      可能冲突        可能冲突         不会冲突    │
│                                                                 │
│ 设备感知?            是              否               否         │
│                      要重编程设备    只改IOMMU       只改IOMMU   │
│                                                                 │
│ 锁设备?              是              否               否         │
│ 可能丢中断?          是              否               否         │
│                                                                 │
│ 复杂度:              最高            中等             最低       │
│ 性能影响:            大              中               小         │
│ VM-Exit:             -               需要             零次       │
└─────────────────────────────────────────────────────────────────┘

关键洞察:
  非IR → 改设备 (硬件层面, 复杂)
  IR   → 改IOMMU (软件层面, 简单)
  PI   → 改IOMMU + vector不用管 (最简单)
  
PI模式的核心价值:
  不仅是"零VM-Exit"
  还简化了亲和性迁移
  消除了vector管理复杂性
```

---

## 📊 关键源码函数速查

| 函数 | 文件 | 作用 |
|------|------|------|
| `vmx_pi_update_irte()` | vmx/vmx.c | ★ KVM→VT-d桥梁，设置PI模式IRTE |
| `vmx_sync_pir_to_irr()` | vmx/vmx.c | PIR→IRR同步 |
| `vmx_vcpu_pi_load()` | vmx/posted_intr.c | vCPU加载时更新PI描述符 |
| `vmx_vcpu_pi_put()` | vmx/posted_intr.c | vCPU卸载时抑制通知 |
| `vmx_set_rvi()` | vmx/vmx.c | 更新RVI触发VID |
| `kvm_apic_update_irr()` | lapic.c | PIR位图拷贝到IRR |
| `modify_irte()` | intel/irq_remapping.c | 更新IRTE并刷新缓存 |
| `intel_ir_compose_msi_msg()` | intel/irq_remapping.c | 构造MSI消息(含IRTE索引) |
| `pi_wakeup_handler()` | vmx/posted_intr.c | halted vCPU的PI唤醒 |

---

## 🔬 实践练习

### 练习1: 查看中断重映射状态
```bash
# 检查VT-d中断重映射是否启用
dmesg | grep -E "DMAR.*IR|remapping"

# 查看IOMMU信息
dmesg | grep -E "iommu|IRTE"

# 检查PI是否启用
dmesg | grep -i "posted"
```

### 练习2: ftrace 追踪中断路径
```bash
# 追踪中断注入
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_inj_virq/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_apic_accept_irq/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable

# 运行VM，观察trace
cat /sys/kernel/debug/tracing/trace_pipe
```

### 练习3: bpftrace 追踪 PI 操作
```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_pi_irte_update {
    printf("PI IRTE update: vcpu=%d gvec=%d girq=%d\n",
           args->vcpu_id, args->gvec, args->girq);
}
'
```

### 练习4: 对比传统 vs PI 模式
```bash
# 1. 禁用APICv, 使用传统模式
echo 0 > /sys/module/kvm_intel/parameters/enable_apicv
# 重启VM, 观察 VM-Exit 中的 EXTERNAL_INTERRUPT 次数

# 2. 启用APICv+PI
echo 1 > /sys/module/kvm_intel/parameters/enable_apicv
# 重启VM, 观察 VM-Exit 是否显著减少
```

---

## ✅ 验证清单

完成后确认能回答：
- [ ] 画出一个直通设备中断到达Guest vCPU的完整路径
- [ ] IRTE的PDA字段指向什么？什么时候设置？
- [ ] `vmx_pi_update_irte()` 做了什么？谁调用它？
- [ ] vCPU从pCPU-0迁移到pCPU-1时，PI描述符和IRTE分别要更新哪些字段？
- [ ] `vmx_sync_pir_to_irr()` 在什么时候被调用？为什么需要原子操作？
- [ ] 如果SN=1时有PI到达，会发生什么？vCPU如何得知？
- [ ] PI模式下，Guest处理中断需要几次VM-Exit？

---

## 🔍 VMM视角对比

### 用户态VMM vs KVM内核态中断处理

| 方面 | 用户态VMM (QEMU) | KVM内核态 |
|------|------------------|-----------|
| **中断注入** | ioctl(KVM_INTERRUPT) | 直接写VMCS或Posted Interrupts |
| **中断延迟** | ~10μs (系统调用+信号处理) | <1μs (Posted Interrupts) |
| **中断合并** | 软件实现 | 硬件支持 (APICv) |
| **vLAPIC管理** | 用户态模拟 | 内核态vLAPIC + APICv |
| **中断路由** | 用户态配置 | 内核态路由表 |

### 关键差异：中断注入路径

```
用户态VMM:
  外部中断 → Host内核 → QEMU → ioctl(KVM_INTERRUPT) → KVM → VM-Entry注入
  延迟: ~10μs

KVM内核态 (Posted Interrupts):
  外部中断 → IOMMU → 直接写PI描述符 → 硬件自动PIR→IRR → VM-Entry注入
  延迟: <1μs (零VM-Exit!)
```

### 为什么Posted Interrupts性能更好？

1. **零VM-Exit**：中断从设备直达Guest，无需Host介入
2. **硬件加速**：PIR→IRR同步由硬件完成
3. **低延迟**：中断延迟从~10μs降低到<1μs

---

## ⚡ 性能优化技术

### 1. APICv (Virtual Interrupt Delivery)

**问题**：每次中断注入都需要VM-Exit

**解决**：使用APICv，硬件自动评估IRR并注入

```c
/* vmx_hardware_setup() 中检测 */
if (!cpu_has_vmx_apicv())
    enable_apicv = 0;
```

**配置**：
```bash
# 查看是否启用
cat /sys/module/kvm_intel/parameters/enable_apicv
# 输出: Y (启用) 或 N (禁用)

# 手动启用
modprobe -r kvm_intel
modprobe kvm_intel enable_apicv=1
```

**效果**：
- 减少中断相关VM-Exit 80%
- 中断延迟降低50%

### 2. Posted Interrupts

**问题**：APICv仍需VM-Exit来处理中断

**解决**：Posted Interrupts实现零VM-Exit中断投递

```c
/* vmx_pi_update_irte() 中配置 */
/* 设置PI模式IRTE */
irte.IM = 1;                    /* Posted模式 */
irte.PDA = __pa(&vmx->pi_desc); /* PI描述符地址 */
irte.NV = POSTED_INTR_VECTOR;   /* 通知向量 */
```

**配置**：
```bash
# 需要启用APICv + IOMMU IR
modprobe kvm_intel enable_apicv=1
# IOMMU需要启用中断重映射
echo "intel_iommu=on" >> /etc/default/grub
update-grub && reboot
```

**效果**：
- 零VM-Exit中断投递
- 中断延迟<1μs
- 高吞吐场景性能提升3-5倍

### 3. 中断合并 (Interrupt Coalescing)

**问题**：高频中断（如网卡）导致大量VM-Exit

**解决**：合并多个中断为一次注入

```c
/* kvm_apic_set_irq() 中实现 */
/* 如果短时间内有多个相同向量的中断，合并为一次 */
if (time_before(now, last_inject_time + coalesce_ns)) {
    /* 合并中断 */
    set_bit(vector, vcpu->arch.irq_pending);
} else {
    /* 立即注入 */
    kvm_x86_call(inject_irq)(vcpu, vector);
}
```

**效果**：
- 减少VM-Exit次数50%
- 吞吐提升20-30%

### 4. IPI虚拟化

**问题**：vCPU间发送IPI需要VM-Exit

**解决**：使用IPIv，硬件自动投递IPI

```c
/* vmx_hardware_setup() 中检测 */
if (!enable_apicv || !cpu_has_vmx_ipiv())
    enable_ipiv = false;
```

**效果**：
- 减少IPI相关VM-Exit
- 多核扩展性能提升

---

## ⚠️ 常见陷阱

### 陷阱1：APICv未启用

**场景**：中断延迟高，VM-Exit频繁

**症状**：`perf kvm stat report`显示大量`EXTERNAL_INTERRUPT`退出

**原因**：APICv未启用或CPU不支持

**解决**：
```bash
# 检查CPU是否支持APICv
grep -E "apicv|posted_interrupt" /proc/cpuinfo

# 启用APICv
modprobe -r kvm_intel
modprobe kvm_intel enable_apicv=1

# 验证
cat /sys/module/kvm_intel/parameters/enable_apicv
```

### 陷阱2：Posted Interrupts配置错误

**场景**：直通设备中断无法到达Guest

**症状**：Guest设备无响应，dmesg显示中断超时

**原因**：IRTE未正确配置PI模式

**解决**：
```c
// vmx_pi_update_irte() 中配置
irte.IM = 1;                    /* Posted模式 */
irte.PDA = __pa(&vmx->pi_desc); /* PI描述符物理地址 */
irte.NV = POSTED_INTR_VECTOR;   /* 通知向量 */
irte.Dest_ID = cpu;             /* 目标pCPU */
```

**检查**：
```bash
# 查看IRTE状态
cat /sys/kernel/debug/iommu/irq_remap
# 检查DM位是否为1
```

### 陷阱3：vCPU迁移时PI状态未更新

**场景**：vCPU迁移后中断丢失

**症状**：vCPU在某些pCPU上收不到中断

**原因**：迁移时未更新IRTE的Dest_ID和PI描述符的NDA

**解决**：
```c
// vmx_vcpu_load() 中更新
if (vcpu->cpu != old_cpu) {
    /* 更新PI描述符 */
    vmx->pi_desc.nda = new_cpu;
    
    /* 更新IRTE */
    modify_irte(irte_index, &new_irte);
}
```

### 陷阱4：中断向量冲突

**场景**：Guest中断处理混乱

**症状**：Guest调用错误的中断处理函数

**原因**：多个设备使用相同的中断向量

**解决**：
```c
// 分配唯一的中断向量
vector = allocate_vector();
// 确保向量在合法范围内 (32-255)
if (vector < 32 || vector > 255)
    return -EINVAL;
```

**检查**：
```bash
# 查看中断向量分配
cat /sys/kernel/debug/kvm/<vm_id>/irq_routing
```
