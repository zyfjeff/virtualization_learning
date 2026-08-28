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
4. 理解 PI 的 ON/SN/NV/NDST 字段在调度中如何变化
5. 用 ftrace 观察中断注入的完整过程

---

## 📂 本章文件

| 文件 | 内容 |
|------|------|
| `README.md` | 本文件：技术全景 + 完整中断路径 + 跨层数据结构速览 |
| `annotations.md` | 源码精读：PI 描述符、IRTE、PIR→IRR 同步、桥梁函数、PIC/IOAPIC 模拟 |
| `posted-interrupts.md` | ★ Posted 模式系统深入：PI Descriptor / IRTE 格式 / 零 VM-Exit 机制 / KVM 实现 |
| `msi-affinity-migration.md` | ★ MSI 地址格式与亲和性迁移（Remapped vs Posted 的迁移路径差异） |
| `practice/` | ★ 6 个实验脚本：环境检查 / IRTE 观察 / PI 追踪 / 性能对比 / ON-SN / vCPU 迁移 |

> 数据结构与桥梁函数在本文件中只作速览，字段级细节以 `posted-interrupts.md` 为准。

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
   (Remapped 模式: IM=0, 不启用PI)
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

> 本节是跨层速览。逐字段深入见 [posted-interrupts.md](posted-interrupts.md) 第一、二部分；逐行源码注解见 [annotations.md](annotations.md) §1、§7。三者冲突时以 posted-interrupts.md 为准。

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
  偏移 0x20 ┌────┬────┬─────┬──────┬─────┬────────┐
            │ ON │ SN │ RSV │  NV  │ RSV │  NDST  │  KVM写: 调度时更新
            │ 1b │ 1b │ 14b │  8b  │ 8b  │  32b   │  IOMMU写: ON位
            └────┴────┴─────┴──────┴─────┴────────┘
             256  257 271:258 279:272 287:280 319:288
  偏移 0x28 ┌────────────────────────────────────────┐
            │        Reserved (24 bytes)              │  bits 511:320
  偏移 0x3F └────────────────────────────────────────┘

ON   = Outstanding Notification (bit 256, IOMMU设1 → KVM清0)
SN   = Suppress Notification (bit 257, KVM在vCPU调出时设1)
NV   = Notification Vector (bits 279:272, 通知中断的向量号)
NDST = Notification Destination (bits 319:288, pCPU的物理APIC ID)

对应 Linux 结构体字段 (arch/x86/include/asm/posted_intr.h):
  ON/SN + 14位保留 → notifications (u16)，NV → nv，NDST → ndst
```

### IRTE — 中断重映射表条目 (128位)

```
IRTE (Remapped 模式, IM=0):
┌─────────────────────── 低64位 ───────────────────────────┐
│ P │ FPD │ DM │ RH │ TM │ DLM │ AVAIL │ RSV │ IM=0 │ V │ RSV │ DST │
│ 1b│ 1b  │ 1b │ 1b │ 1b │  3b │  4b   │ 3b  │  1b  │8b │ 8b  │ 32b │
└──────────────────────────────────────────────────────────┘
  bit:  0    1     2    3    4   7:5   11:8  14:12   15  23:16 31:24 63:32
┌─────────────────────── 高64位 ───────────────────────────┐
│ SID (Source-ID) │ SQ │ SVT │ Reserved                   │
│     16b (BDF)   │ 2b │ 2b  │         44b                │
└──────────────────────────────────────────────────────────┘
  bit:    79:64    81:80 83:82        127:84

IRTE (Posted模式, IM=1):
┌─────────────────────── 低64位 ───────────────────────────┐
│ P │ FPD │ Rsvd │ AVAIL │ Rsvd │ URG │ IM=1 │ VV │ Rsvd │ PDAL │
│ 1b│ 1b  │  6b  │  4b   │  2b  │ 1b  │  1b  │ 8b │ 14b  │ 26b  │
└──────────────────────────────────────────────────────────┘
┌─────────────────────── 高64位 ───────────────────────────┐
│ SID │ SQ │ SVT │ Rsvd │ PDAH (PI描述符物理地址高位)      │
│ 16b │ 2b │ 2b  │ 12b  │            32b                   │
└──────────────────────────────────────────────────────────┘

字段说明 (参考 VT-d Spec Section 9.9 / Figure 9-9 = Remapped，Section 9.10 / Figure 9-10 = Posted):
  两种模式共有:
  P     = Present (存在位, bit 0)
  FPD   = Fault Processing Disable (bit 1)
  IM    = IRTE Mode (★ bit 15: 1=Posted模式, 0=Remapped模式)
  AVAIL = 软件可用 (bits 11:8, 硬件忽略)
  SID   = Source Identifier (设备BDF, bits 79:64)
  SQ    = Source-id Qualifier (bits 81:80)
  SVT   = Source Validation Type (bits 83:82)

  仅 Remapped 模式:
  DM    = Destination Mode (bit 2: 0=Physical, 1=Logical)
  RH    = Redirection Hint (bit 3)
  TM    = Trigger Mode (bit 4: 0=edge, 1=level)
  DLM   = Delivery Mode (bits 7:5: 000b=Fixed, 001b=Lowest Priority, 100b=NMI ...)
  V     = Vector (bits 23:16)
  DST   = Destination ID (bits 63:32; xAPIC 模式下仅 47:40 有效, x2APIC 模式下 63:32 全部有效)

  仅 Posted 模式:
  URG   = Urgent (紧急中断标志, bit 14)
  VV    = Virtual Vector (bits 23:16, 会被写入PI Descriptor的NV字段)
  PDAL  = Posted Descriptor Address Low (bits 63:38, 对应地址bits 31:6)
  PDAH  = Posted Descriptor Address High (bits 127:96, 对应地址bits 63:32)

关键: PDA字段指向vCPU的pi_desc物理地址(64字节对齐)!
这是IRTE和PI描述符之间的硬件连接。

**重要: VT-d规范术语说明** (参考 intel-vtd.pdf Section 9.9 / 9.10 / 9.11)

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

  易混淆点1: 模式选择位是 IM,不是 DM
    · Remapped 与 Posted 的区分只看 IM (bit 15)
    · Remapped 模式的 IM=0, Posted 模式的 IM=1

  易混淆点2: Remapped 模式里 DM 和 DLM 是两个不同字段
    · DM  (Destination Mode, bit 2)  : Destination ID 按 physical 还是 logical 解释
    · DLM (Delivery Mode, bits 7:5)  : 投递模式 (Fixed / Lowest Priority / NMI ...)
    · Posted 模式下 bits 7:2 整段为 Reserved,这两个字段都不存在
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

> 深入版本见 [posted-interrupts.md](posted-interrupts.md) §4.2 与 [annotations.md](annotations.md) §4。

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

> 源码级展开见 [posted-interrupts.md](posted-interrupts.md) §4.4 与 [annotations.md](annotations.md) §5。

```
时间线:
─────┬────────────────┬──────────────────┬──────────────────────┬───→
     │                │                  │                      │
 vCPU运行在pCPU-0   被调出            被调度到pCPU-1          vCPU运行
     │                │                  │                      │
     │ pi_desc:       │                  │ pi_desc:             │
     │  SN=0 (允许)   │                  │  SN=0 (允许)         │
     │  NDST=pCPU-0   │                  │  NDST=pCPU-1 ←更新!  │
     │                │                  │                      │
     │ IRTE:          │                  │ IRTE:                │
     │  IM=1 (Posted) │                  │  IM=1 (Posted)       │
     │  PDA=pi_desc   │                  │  PDA=pi_desc         │
     │  DestID=pCPU-0 │                  │  DestID=pCPU-1 ←更新!│
     │                │                  │                      │
     │ vmx_vcpu_     │ vmx_vcpu_put()  │ vmx_vcpu_load()      │
     │ (正常运行)     │ → pi_set_sn()   │ → pi_clear_sn()      │
     │                │   SN=1          │ → NDST=pCPU-1 APIC ID│
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

## 🎯 MSI 地址格式与亲和性迁移

这一主题（MSI Address/Data 的两种格式、Remapped 与 Posted 模式下亲和性迁移路径的差异）已拆分为独立文档：

- [msi-affinity-migration.md](msi-affinity-migration.md)

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

> **重要**：完整的实践练习和实验脚本已经整理到 `practice/` 目录中。

### 快速开始

```bash
# 进入实践目录
cd /root/code/kvm-study/phase3-interrupts/practice/

# 查看详细指南
cat README.md

# 环境检查（不需要 VM）
sudo bash ex1-pi-env-check.sh

# 启动带设备直通的 VM（实验 3-6 需要）
sudo bash /root/code/kvm-study/scripts/vm/setup-vfio-vm.sh start

# 运行实验
sudo bash ex3-pi-trace.sh        # PI 中断追踪
sudo bash ex5-on-sn-observe.sh   # ON/SN 行为观察
sudo bash ex6-vcpu-migration.sh  # vCPU 迁移观察

# 清理
sudo bash /root/code/kvm-study/scripts/vm/setup-vfio-vm.sh stop
```

### 实验列表

| 编号 | 实验名称 | 需要 VM | 难度 | 预计时间 | 核心知识点 |
|------|---------|---------|------|---------|-----------|
| 1 | PI 环境检查 | 否 | ★☆☆ | 10min | APICv/PI 支持检测 |
| 2 | IRTE 观察 | 否 | ★★☆ | 15min | IRTE 格式、Posted vs Remapped |
| 3 | PI 中断追踪 | **是** | ★★★ | 20min | 中断投递完整路径 |
| 4 | PI vs Remapped 性能对比 | **是** | ★★★ | 30min | 零 VM-Exit 性能优势 |
| 5 | ON/SN 行为观察 | **是** | ★★★ | 20min | 通知合并、抑制机制 |
| 6 | vCPU 迁移与 NDST 更新 | **是** | ★★★ | 20min | vCPU 调度、NDST 动态更新 |

### 统一测试环境

所有实验使用统一的测试环境构建和启动脚本：

- **构建脚本**: `/root/code/kvm-study/scripts/vm/setup-vfio-vm.sh`
- **功能**: 自动构建内核和 rootfs，启动带 VFIO 设备直通的 VM
- **详细说明**: 参见 `practice/README.md`

### 快速实验（不需要 VM）

如果只是想快速了解 PI 的基本概念，可以运行不需要 VM 的实验：

```bash
# 实验 1: 检查 PI 环境支持
sudo bash ex1-pi-env-check.sh

# 实验 2: 观察 IRTE 表项
sudo bash ex2-irte-observe.sh
```

### 完整实验（需要 VM）

要运行完整的 PI 实验，需要启动带 VFIO 设备直通的 VM：

```bash
# 1. 启动 VM（首次运行会自动构建环境）
sudo bash /root/code/kvm-study/scripts/vm/setup-vfio-vm.sh start

# 2. 在另一个终端运行实验
sudo bash ex3-pi-trace.sh        # 追踪 PI 中断投递
sudo bash ex4-pi-vs-remapped.sh  # 对比性能
sudo bash ex5-on-sn-observe.sh   # 观察 ON/SN 行为
sudo bash ex6-vcpu-migration.sh  # 观察 vCPU 迁移

# 3. 清理
sudo bash /root/code/kvm-study/scripts/vm/setup-vfio-vm.sh stop
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

**原因**：迁移时未更新IRTE的Dest_ID和PI描述符的NDST

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
