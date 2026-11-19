# AMD AVIC (Advanced Virtual Interrupt Controller) 详解

> 基于 AMD IOMMU 规范 (amd-virtualization.pdf) | Linux 6.12.93 内核源码
> 
> **对比参考**：Intel VT-d Posted Interrupts 见 [posted-interrupts.md](posted-interrupts.md)

---

## 📚 前置知识

本文件假设你已经理解：
- ✅ 中断虚拟化的基本概念（Phase 5 README §1.1-1.2）
- ✅ IOMMU 中断重映射基础（Phase 3 `interrupts.md`）
- ✅ Intel VT-d IRTE 与 Posted Interrupts（posted-interrupts.md）

---

## 📋 学习目标

完成本文件后，你应该能够：
1. 理解 AMD IOMMU 的 IRTE 格式（Basic / GuestMode=0 / GuestMode=1）
2. 掌握 AMD Guest Virtual APIC Table 的工作原理
3. 对比 AMD AVIC 与 Intel VT-d PI 的异同
4. 理解 GA Log (Guest vAPIC Log) 的作用
5. 跟踪 Linux 内核中 AMD-Vi 的中断虚拟化实现

---

## 🏗️ 技术全景

### 1. AMD 与 Intel 中断虚拟化的架构对比

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    AMD vs Intel 中断虚拟化架构                           │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Intel VT-d (Posted Interrupts):                                         │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │ 设备 MSI → IOMMU → IRTE (IM=1) → PI Descriptor → vCPU PIR     │    │
│  │              ↑                ↑                     ↑            │    │
│  │         IRTE.PDA 指向 PI Descriptor (64B)                      │    │
│  │         IRTE.VV (Virtual Vector) → PI.NV (Notification Vector)│    │
│  │         IRTE.DestID → PI.NDST (目标 pCPU APIC ID)             │    │
│  │                                                                  │    │
│  │  硬件行为:                                                       │    │
│  │    · IOMMU 原子设置 PIR[vector]=1, ON=1                          │    │
│  │    · 发送通知中断 (Notification Vector) 给 pCPU                  │    │
│  │    · VM-Entry 时硬件自动 PIR → IRR                               │    │
│  │    · 零 VM-Exit!                                                 │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  AMD IOMMU (Guest Virtual APIC):                                         │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │ 设备 MSI → IOMMU → IRTE (GuestMode=1) → Guest Virtual APIC    │    │
│  │              ↑                  ↑                    Table       │    │
│  │         IRTE.GVAPTR 指向 Guest vAPIC Backing Page              │    │
│  │         IRTE.Vector → 计算 vIRR 位偏移                          │    │
│  │         IRTE.Destination → doorbell 目标 pCPU                   │    │
│  │                                                                  │    │
│  │  硬件行为:                                                       │    │
│  │    · IOMMU 原子设置 vIRR[vector]=1 (在 Guest vAPIC Page)        │    │
│  │    · 如果 IsRun=1: 发送 doorbell 中断给 pCPU                     │    │
│  │    · 如果 IsRun=0: 写入 GA Log, 等待软件调度                     │    │
│  │    · Guest 直接处理 (零 VM-Exit, 但有 doorbell)                  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2. 核心概念对比

| 概念 | Intel VT-d | AMD IOMMU |
|------|------------|-----------|
| **中断重映射表** | IRTE (128-bit) | IRTE (32-bit 或 128-bit) |
| **模式选择位** | `IM` (bit 15): 0=Remapped, 1=Posted | `GuestMode` (bit 7): 0=Remapped, 1=Virtualized |
| **Posted/虚拟化目标** | PI Descriptor (64B, 物理地址) | Guest Virtual APIC Table (4KB, 物理地址) |
| **向量字段** | `VV` (Virtual Vector, bits 23:16) | `Vector` (bits 71:64, 8-bit) |
| **目标地址字段** | `PDAL/PDAH` (PI Descriptor 地址) | `GuestVirtualAPICTableRootPointer` (vAPIC 地址) |
| **目标 CPU** | `Dest_ID` (bits 319:288) | `Destination` (bits 15:8) |
| **运行状态提示** | 无 (通过 SN 位抑制通知) | `IsRun` (bit 6): 0=Guest 未运行, 1=运行中 |
| **日志机制** | 无 | `GALogIntr` (bit 2): 记录未投递的中断 |
| **零 VM-Exit 机制** | PI Descriptor + 通知中断 | vAPIC Backing Page + doorbell 中断 |

---

## 🔧 AMD IRTE 格式详解

### 3.1 基础 IRTE 格式 (32-bit, Guest Virtual APIC 未启用)

**规范引用**: amd-virtualization.pdf, Section 2.2.5.1, Figure 15, Table 20

```
127                                                              0
┌──────────────────────────────────────────────────────────────────────┐
│ Reserved          │ Vector │  Destination │DM│RqEoi│IntType│SupIOPF│RemapEn│
│    96:72          │ 71:64  │    15:8      │ 7 │  6  │  4:2  │   1   │   0   │
│                   │  8-bit │    8-bit     │1b │ 1b  │  3b   │  1b   │  1b   │
└──────────────────────────────────────────────────────────────────────┘

字段说明 (amd-virtualization.pdf, Table 20):
  Vector (71:64)    : 中断向量号 (8-bit)
  Destination (15:8): 目标 APIC 物理/逻辑地址
  DM (7)            : Destination Mode, 1=逻辑模式, 0=物理模式
  RqEoi (6)         : Request EOI, 1=需要 EOI 周期
  IntType (4:2)     : 中断类型, 000b=Fixed, 001b=Arbitrated
  SupIOPF (1)       : 抑制 IO_PAGE_FAULT 事件
  RemapEn (0)       : 重映射使能, 1=有效, 0=中断被目标终止
```

### 3.2 Guest Virtual APIC 启用时的 IRTE 格式

**规范引用**: amd-virtualization.pdf, Section 2.2.5.2, Table 21

当启用 Guest Virtual APIC 时，IRTE 格式根据 `GuestMode` 位选择：

| MMIO 0030h | MMIO 0018h | IRTE 大小 | 用途 |
|------------|------------|-----------|------|
| GASup=0    | 任意       | 32-bit    | 仅中断重映射 (Table 20/Figure 15) |
| GASup=1    | GAEn=0     | 32-bit    | 仅中断重映射 |
| GASup=1    | GAEn=1, GAMEn=0 | 128-bit | GuestMode=0: 重映射 (Table 22/Figure 17) |
| GASup=1    | GAEn=1, GAMEn=1 | 128-bit | GuestMode=1: 虚拟化 (Table 23/Figure 18) |

### 3.3 IRTE GuestMode=0 (中断重映射, 非虚拟化)

**规范引用**: amd-virtualization.pdf, Section 2.2.5.2, Figure 17, Table 22

```
127                                                              0
┌──────────────────────────────────────────────────────────────────────┐
│                    Reserved (127:72)                                  │
│                         56-bit                                        │
├──────────────────────────────────────────────────────────────────────┤
│ Vector │                   Reserved (63:16)                          │
│ 71:64  │                       48-bit                                │
│ 8-bit  │                                                             │
├────────┴─────────────────────────────────────────────────────────────┤
│ Reserved │GuestMode│ Destination │DM│RqEoi│IntType│SupIOPF│RemapEn│
│  15:9    │    7    │    15:8     │ 7 │  6  │  4:2  │   1   │   0   │
│  7-bit   │   1b    │    8-bit    │1b │ 1b  │  3b   │  1b   │  1b   │
│          │  =0     │             │   │     │       │       │       │
└──────────────────────────────────────────────────────────────────────┘

与基础格式的区别:
  · 使用 128-bit IRTE (而非 32-bit)
  · GuestMode = 0 (bit 7)
  · Vector 和 Destination 含义与基础格式相同
  · 用于 Guest 未启用 vAPIC 时的中断重映射
```

### 3.4 IRTE GuestMode=1 (Guest Virtual APIC 虚拟化) ★

**规范引用**: amd-virtualization.pdf, Section 2.2.5.2, Figure 18, Table 23

```
127                                                              0
┌──────────────────────────────────────────────────────────────────────┐
│                    Reserved (127:116)                                 │
│                         12-bit                                        │
├──────────────────────────────────────────────────────────────────────┤
│         Guest Virtual APIC Table Root Pointer [51:32]                 │
│                        20-bit                                         │
├──────────────────────────────────────────────────────────────────────┤
│ Reserved │  Guest Virtual APIC Table Root Pointer [31:12]            │
│  75:72   │                       20-bit                               │
│  4-bit   │                                                            │
├──────────┴───────────────────────────────────────────────────────────┤
│ Vector │                          GATag[31:0]                         │
│ 71:64  │                           32-bit                             │
│ 8-bit  │                                                              │
├────────┴─────────────────────────────────────────────────────────────┤
│ Reserved │ Destination │GuestMode│IsRun│ Reserved │GALogIntr│SupIOPF│RemapEn│
│  31:16   │    15:8     │    7    │  6  │   5:3    │    2    │   1   │   0   │
│  16-bit  │    8-bit    │   1b    │ 1b  │  3-bit   │   1b    │  1b   │  1b   │
│          │             │  =1     │     │          │         │       │       │
└──────────────────────────────────────────────────────────────────────┘

★ 关键字段说明 (Table 23):

  GuestVirtualAPICTableRootPointer (115:76, 40-bit):
    · 指向 Guest vAPIC Backing Page 的系统物理地址
    · 4KB 对齐 (bits 11:0 隐含为 0)
    · 这是 IRTE 与 Guest vAPIC 之间的硬件连接！

  Vector (71:64, 8-bit):
    · 用于计算 Guest vAPIC backing page 中 vIRR 的地址
    · 位索引 = Vector mod 32
    · 字节偏移 = (Vector / 32) << 4
    · 目标字节 = GVAPTR + 0x200 + 字节偏移

  GATag (63:32, 32-bit):
    · 用于 GA Log 记录，标识目标 vAPIC
    · 软件写入，IOMMU 用于日志记录

  Destination (15:8, 8-bit):
    · doorbell 消息的目标物理 CPU
    · 仅当 IsRun=1 时使用
    · 注：x2APIC 模式下扩展为 32-bit (Figure 20)

  Destination (15:8, 8-bit):
    · doorbell 消息的目标物理 CPU
    · 仅当 IsRun=1 时使用
    · 注：x2APIC 模式下扩展为 32-bit (Figure 20)

  GuestMode (7) = 1:
    · 表示此 IRTE 使用 Guest Virtual APIC 格式

  IsRun (6): 运行状态提示
    · 0 = Guest 未运行，中断信息记录到 vAPIC 内存页和 GA Log
    · 1 = Guest 正在运行，IOMMU 发送 doorbell 信号

  GALogIntr (2): GA Log 记录使能
    · 1 = 当 IsRun=0 时，使用 GATag 写入 GA Log
    · 0 = IsRun=0 时不写入 GA Log

  RemapEn (0): 使能位
    · 1 = GVAPTR, Vector, GATag, Destination, GuestMode, IsRun 有效
    · 0 = IRTE 内容被忽略 (除 SupIOPF)，访问产生 IO_PAGE_FAULT
```

---

## 🎯 Guest Virtual APIC Table 工作原理

### 4.1 vAPIC Backing Page 结构

**规范引用**: amd-virtualization.pdf, Section 2.2.8, Figure 16

Guest Virtual APIC Table 是一个 4KB 的物理页面，包含 Guest vLAPIC 的状态：

```
Guest Virtual APIC Backing Page (4KB):

偏移 0x000 ┌────────────────────────────────────────┐
           │         Reserved (0x000-0x1FF)           │
           │              512 bytes                    │
偏移 0x200 ├────────────────────────────────────────┤
           │         Virtual IRR (vIRR)               │
           │         256 bits = 32 bytes              │
           │         每个 bit 对应一个中断向量          │
           │                                          │
           │    bit 0  = vector 0                     │
           │    bit 1  = vector 1                     │
           │    ...                                   │
           │    bit 255 = vector 255                  │
           │                                          │
           │    布局: 32 个 8-bit 字节                 │
           │    Byte 0: bits 7-0   (vectors 0-7)      │
           │    Byte 1: bits 15-8  (vectors 8-15)     │
           │    ...                                   │
           │    Byte 31: bits 255-248 (vectors 248-255)│
偏移 0x220 ├────────────────────────────────────────┤
           │         Reserved (0x220-0xFFF)           │
           │              3552 bytes                  │
偏移 0xFFF └────────────────────────────────────────┘

IOMMU 写入 vIRR 的计算 (amd-virtualization.pdf, Section 2.2.5.2):

  给定 IRTE.Vector = V:
    位索引 = V mod 32
    字节偏移 = (V / 32) << 4
    目标地址 = GVAPTR + 0x200 + 字节偏移
    
  示例: Vector = 0x42 (66)
    位索引 = 66 mod 32 = 2
    字节偏移 = (66 / 32) << 4 = 2 << 4 = 32 = 0x20
    目标地址 = GVAPTR + 0x200 + 0x20 = GVAPTR + 0x220
    操作: 原子设置 [GVAPTR + 0x220] 的 bit 2

硬件行为:
  · IOMMU 原子设置 vIRR 中对应位
  · 不修改其他位 (atomic OR)
  · Guest 直接读取 vIRR (通过共享页面)
```

### 4.2 中断投递流程

**规范引用**: amd-virtualization.pdf, Section 2.2.5.2 (步骤 9)

```
设备 MSI 中断到达 IOMMU:

① IOMMU 接收中断
   使用 DeviceID 查找 DTE (Device Table Entry)
   
② 读取 IRTE
   使用 DTE 中的 Interrupt Table Root Pointer 和 Vector
   选择对应的 IRTE
   
③ 检查 IRTE[RemapEn]
   如果 = 0: 产生 IO_PAGE_FAULT 事件
   如果 = 1: 继续处理
   
④ 检查 IRTE[GuestMode]
   如果 = 0: 使用 Figure 17/Table 22 格式，普通重映射
   如果 = 1: 使用 Figure 18/Table 23 格式，Guest 虚拟化
   
⑤ Guest Virtual APIC 处理 (GuestMode=1):
   
   a) 计算 vIRR 位置:
      位索引 = IRTE[Vector] mod 32
      字节偏移 = (IRTE[Vector] / 32) << 4
      目标地址 = IRTE[GVAPTR] + 0x200 + 字节偏移
      
   b) 原子设置 vIRR 位:
      原子设置目标字节中的对应位
      (vIRR[vector] = 1)
      
   c) 读取 IRTE 并检查 IsRun:
   
      ┌──── 两种情况 ────┐
      │                  │
   IsRun=1              IsRun=0
      │                  │
   Guest 正在运行       Guest 未运行
      │                  │
   发送 doorbell        检查 GALogIntr
   中断给 pCPU          │
      │                  ├─ GALogIntr=1: 写入 GA Log
   pCPU 收到            │                 使用 GATag
   doorbell             │                 ├─ 调度 Guest
      │                  │                 │  (设置 vCPU 为运行状态)
   VM-Entry             │                 └─ 下次调度时处理
   硬件自动评估         │
   vIRR vs vPPR         ├─ GALogIntr=0: 不记录
      │                  │                 中断留在 vIRR
   vIRR > vPPR?         │                 等待下次调度
      │                  │
   是: 自动注入          │
   Guest 直接处理        │
   ★ 零 VM-Exit! ★      │
      │                  │
   否: 不注入            │
   等待更高优先级        │
   中断或 vPPR 降低      │
```

---

## 🔗 GA Log (Guest vAPIC Log) 机制

### 5.1 GA Log 的作用

**规范引用**: amd-virtualization.pdf, Section 2.7

GA Log 用于记录**无法立即投递**的中断请求：

```
GA Log 使用场景:

  当 IRTE[IsRun]=0 (Guest 未运行) 且 IRTE[GALogIntr]=1 时:
  
  ① 设备发送中断
  ② IOMMU 设置 vIRR[vector]=1 (在 vAPIC backing page)
  ③ 检查 IsRun=0 → Guest 未运行
  ④ 不发送 doorbell (避免无谓的中断)
  ⑤ 写入 GA Log entry:
     ┌─────────────────────────────────┐
     │  GATag (32-bit)  │  DeviceID   │
     │  标识目标 vAPIC   │  源设备 ID  │
     └─────────────────────────────────┘
  ⑥ 软件读取 GA Log, 调度目标 vCPU
  ⑦ vCPU 开始运行后, 直接处理 vIRR 中的中断
     (无需再次 VM-Exit, 因为 vIRR 已经设置)

GA Log 的好处:
  · 避免不必要的 doorbell 中断
  · 帮助 VMM 做调度决策 (知道哪个 vCPU 有 pending 中断)
  · 减少系统开销
```

### 5.2 GA Log 结构

**规范引用**: amd-virtualization.pdf, Section 2.7.1, Figure 72

```
GA Log 环形缓冲区 (系统物理内存):

┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  IOMMU 在 tail 写入新 entry                                │
│  软件在 head 读取并消费 entry                               │
│                                                             │
│  ┌────┬────┬────┬────┬────┬────┬────┬────┬────┐           │
│  │    │    │    │ E3 │ E2 │ E1 │    │    │    │           │
│  └────┴────┴────┴────┴────┴────┴────┴────┴────┘           │
│                 ↑              ↑                            │
│               head           tail                          │
│                                                             │
│  当 head == tail: 缓冲区为空                                │
│  当 tail + 1 == head (模 size): 缓冲区满                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘

GA Log Entry 格式 (64-bit, amd-virtualization.pdf, Table 75):

┌─────────────────────────────────────────────────────────────┐
│  Reserved (63:48)  │  DeviceID (47:32)  │  GATag (31:0)    │
│      16-bit        │       16-bit       │      32-bit      │
└─────────────────────────────────────────────────────────────┘

  DeviceID: 发起中断请求的设备 BDF
  GATag: 来自 IRTE, 标识目标 vAPIC
```

---

## 🌉 AMD vs Intel: 关键差异深入对比

### 6.1 数据结构对比

| 特性 | Intel VT-d PI | AMD Guest Virtual APIC |
|------|---------------|------------------------|
| **Posted/虚拟化目标** | PI Descriptor (64 bytes) | vAPIC Backing Page (4KB) |
| **对齐要求** | 64-byte aligned | 4KB aligned |
| **中断请求位** | PIR[255:0] (256 bits, 独立区域) | vIRR[255:0] (在 vAPIC page 偏移 0x200) |
| **通知位** | ON (Outstanding Notification) | 无 (使用 IsRun + GA Log) |
| **抑制位** | SN (Suppress Notification) | IsRun (0=Guest 未运行) |
| **通知向量** | NV (Notification Vector, 8-bit) | 无 (doorbell 使用固定机制) |
| **目标 CPU** | NDST (32-bit APIC ID) | Destination (8-bit) |
| **向量字段** | VV (Virtual Vector, 8-bit) | Vector (8-bit) |
| **地址字段** | PDAL/PDAH (58-bit, 64B 对齐) | GVAPTR (40-bit, 4KB 对齐) |

### 6.2 中断投递机制对比

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    Intel PI vs AMD AVIC 中断投递                         │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Intel VT-d Posted Interrupts:                                           │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │ 1. IOMMU 查找 IRTE (IM=1)                                       │    │
│  │ 2. 读取 PDA → PI Descriptor 物理地址                            │    │
│  │ 3. 原子设置: PIR[VV]=1, ON=1                                    │    │
│  │ 4. 发送通知中断 (NV) 给 NDST 指定的 pCPU                        │    │
│  │ 5. pCPU 收到通知中断:                                            │    │
│  │    · 如果 vCPU 正在运行: 硬件在 VM-Exit 时自动 PIR→IRR           │    │
│  │    · 如果 vCPU 未运行: pi_wakeup_handler() 唤醒                  │    │
│  │ 6. VM-Entry 时硬件自动 PIR→IRR, Guest 处理                      │    │
│  │                                                                   │    │
│  │  关键点:                                                          │    │
│  │    · 总是发送通知中断 (即使 vCPU 未运行)                          │    │
│  │    · 使用 SN 位抑制通知 (vCPU 调出时设置)                         │    │
│  │    · PIR 是独立区域, 与 Guest vLAPIC 分离                         │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  AMD Guest Virtual APIC:                                                 │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │ 1. IOMMU 查找 IRTE (GuestMode=1)                                │    │
│  │ 2. 读取 GVAPTR → vAPIC Backing Page 物理地址                    │    │
│  │ 3. 计算 vIRR 位置, 原子设置 vIRR[Vector]=1                      │    │
│  │ 4. 检查 IsRun:                                                   │    │
│  │    · 如果 IsRun=1: 发送 doorbell 给 Destination pCPU             │    │
│  │    · 如果 IsRun=0: 检查 GALogIntr, 可能写入 GA Log               │    │
│  │ 5. pCPU 收到 doorbell (仅当 IsRun=1):                            │    │
│  │    · VM-Entry 时硬件评估 vIRR vs vPPR                            │    │
│  │    · 如果 vIRR > vPPR: 自动注入, Guest 处理                      │    │
│  │ 6. 如果 IsRun=0:                                                 │    │
│  │    · 中断留在 vIRR, 等待 vCPU 调度                               │    │
│  │    · GA Log 记录事件, 帮助 VMM 调度                              │    │
│  │                                                                   │    │
│  │  关键点:                                                          │    │
│  │    · 仅当 IsRun=1 时发送 doorbell                                 │    │
│  │    · vIRR 直接在 vAPIC page 中, 与 Guest vLAPIC 共享              │    │
│  │    · 使用 GA Log 记录未投递的中断                                 │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### 6.3 运行状态管理对比

| 场景 | Intel VT-d | AMD IOMMU |
|------|------------|-----------|
| **vCPU 正在运行** | PIR[vec]=1, ON=1, 发送通知中断 | vIRR[vec]=1, IsRun=1, 发送 doorbell |
| **vCPU 未运行** | PIR[vec]=1, ON=1, SN=1 → 不发送通知 | vIRR[vec]=1, IsRun=0, 写入 GA Log |
| **vCPU 调出** | 设置 SN=1 抑制通知 | 设置 IsRun=0 |
| **vCPU 载入** | 清除 SN=0, 更新 NDST | 设置 IsRun=1, 更新 Destination |
| **vCPU 迁移** | 更新 PI.NDST + IRTE.DestID | 更新 IRTE.Destination |

### 6.4 零 VM-Exit 实现对比

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    零 VM-Exit 机制对比                                   │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Intel VT-d:                                                             │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │ 硬件自动完成:                                                    │    │
│  │   1. 清 ON (Outstanding Notification)                           │    │
│  │   2. PIR → VIRR (原子转移)                                      │    │
│  │   3. 更新 RVI (Requested Virtual Interrupt)                     │    │
│  │   4. 评估中断 (RVI vs PPR)                                      │    │
│  │   5. 如果 RVI > PPR: 注入中断                                   │    │
│  │                                                                   │    │
│  │ 触发时机: VM-Exit 时 (外部中断向量 = 通知向量)                   │    │
│  │ 规范引用: Intel SDM Section 30.6                                  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  AMD IOMMU:                                                              │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │ 硬件自动完成:                                                    │    │
│  │   1. vIRR 已设置 (IOMMU 原子操作)                               │    │
│  │   2. VM-Entry 时评估 vIRR vs vPPR                               │    │
│  │   3. 如果 vIRR > vPPR: 注入中断                                 │    │
│  │                                                                   │    │
│  │ 触发时机: VM-Entry 时                                             │    │
│  │ 规范引用: AMD APM Volume 2, Chapter 15 (AVIC)                    │    │
│  └─────────────────────────────────────────────────────────────────┘    │
│                                                                          │
│  关键差异:                                                                │
│    · Intel 需要通知中断触发 VM-Exit, 然后在 VM-Exit 时自动处理          │
│    · AMD 的 vIRR 直接在 vAPIC page, VM-Entry 时自动评估                 │
│    · 两者最终都实现零 VM-Exit 的中断注入                                 │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 🔧 Linux 内核实现

### 7.1 AMD-Vi IRTE 结构

**源码引用**: `drivers/iommu/amd/amd_iommu_types.h`

```c
/* 来源: drivers/iommu/amd/amd_iommu_types.h */

/* IRTE 基础格式 (32-bit 简化版) */
struct irte_basic {
    union {
        struct {
            __u64 valid     : 1,    /* RemapEn */
                  supp      : 1,    /* SupIOPF */
                  int_type  : 3,    /* IntType */
                  rq_eoi    : 1,    /* RqEoi */
                  dm        : 1,    /* Destination Mode */
                  destination: 8,   /* Destination */
                  vector    : 8,    /* Vector */
                  reserved  : 41;
        };
    };
};

/* IRTE Guest 虚拟化格式 (128-bit) */
union irte_ga {
    struct {
        __u64 val_lo;               /* 低 64 位 */
        __u64 val_hi;               /* 高 64 位 */
    };
    struct {
        /* 低 64 位 */
        __u64 valid     : 1,        /* RemapEn */
              supp      : 1,        /* SupIOPF */
              ga_log_intr: 1,       /* GALogIntr */
              reserved1 : 1,
              guest_mode: 1,        /* GuestMode */
              destination: 9,       /* Destination[8:0] */
              reserved2 : 7,
              ga_tag    : 32,       /* GATag */
              vector    : 9,        /* Vector[8:0] */
              reserved3 : 23;
              
        /* 高 64 位 */
        __u64 gvptr_lo  : 20,       /* GVAPTR[31:12] */
              reserved4 : 4,
              gvptr_hi  : 20,       /* GVAPTR[51:32] */
              reserved5 : 20;
    };
};
```

### 7.2 AMD-Vi 中断虚拟化初始化

**源码引用**: `drivers/iommu/amd/iommu.c`

```c
/* 来源: drivers/iommu/amd/iommu.c (简化) */

/* 启用 Guest Virtual APIC */
void amd_iommu_enable_guest_vapic(void)
{
    /* 设置 MMIO Offset 0018h[GAEn] = 1 */
    /* 设置 MMIO Offset 0018h[GAMEn] = 1 (如果支持) */
    
    /* 初始化 GA Log */
    /* 分配 GA Log 缓冲区 */
    /* 设置 MMIO Offset 00E0h (GALogBaseAddr) */
    /* 设置 MMIO Offset 2020h[GALogRun] = 1 */
}

/* 设置 IRTE 用于 Guest Virtual APIC */
int amd_iommu_setup_guest_irte(struct irq_remap_ops *ops,
                                struct irq_data *irq_data,
                                struct vcpu_data *vcpu_data)
{
    struct irte_ga *irte;
    
    /* 分配 IRTE */
    irte = get_irte(irq_data);
    
    /* 设置 GuestMode=1 */
    irte->guest_mode = 1;
    
    /* 设置 GVAPTR (Guest vAPIC Backing Page 物理地址) */
    irte->gvptr_lo = (vcpu_data->vapic_addr >> 12) & 0xFFFFF;
    irte->gvptr_hi = (vcpu_data->vapic_addr >> 32) & 0xFFFFF;
    
    /* 设置 Vector */
    irte->vector = vcpu_data->vector;
    
    /* 设置 Destination (目标 pCPU) */
    irte->destination = cpu_to_apicid(vcpu_data->cpu);
    
    /* 设置 IsRun (运行状态) */
    irte->is_run = vcpu_data->is_running ? 1 : 0;
    
    /* 设置 GALogIntr */
    irte->ga_log_intr = 1;  /* 启用 GA Log 记录 */
    
    /* 设置 RemapEn */
    irte->valid = 1;
    
    /* 刷新 IRTE 缓存 */
    amd_iommu_flush_interrupt_table();
    
    return 0;
}
```

### 7.3 KVM AMD-Vi 集成

**源码引用**: `arch/x86/kvm/svm/svm.c` (AMD SVM)

```c
/* 来源: arch/x86/kvm/svm/svm.c (简化) */

/* AMD SVM 的 AVIC 支持 */
struct kvm_x86_ops svm_x86_ops = {
    .name = "kvm_amd",
    
    /* AVIC (Advanced Virtual Interrupt Controller) */
    .avic_init = svm_avic_init,
    .avic_vcpu_load = svm_avic_vcpu_load,
    .avic_vcpu_put = svm_avic_vcpu_put,
    
    /* 中断投递 */
    .deliver_interrupt = svm_deliver_interrupt,
    
    /* ... */
};

/* AVIC 初始化 */
int svm_avic_init(struct kvm *kvm)
{
    /* 分配 AVIC backing page */
    /* 设置物理 APIC backing page */
    /* 配置 AVIC 控制寄存器 */
    
    return 0;
}

/* vCPU 载入时更新 AVIC 状态 */
void svm_avic_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
    struct vcpu_svm *svm = to_svm(vcpu);
    
    /* 更新 IOMMU IRTE 的 Destination */
    /* 设置 IsRun = 1 */
    
    /* 更新 AVIC physical APIC ID table */
}

/* vCPU 卸载时更新 AVIC 状态 */
void svm_avic_vcpu_put(struct kvm_vcpu *vcpu)
{
    struct vcpu_svm *svm = to_svm(vcpu);
    
    /* 设置 IsRun = 0 */
    /* 更新 IOMMU IRTE */
}
```

---

## 📊 性能对比

### 8.1 理论性能对比

| 指标 | Intel VT-d PI | AMD Guest Virtual APIC |
|------|---------------|------------------------|
| **中断延迟** | < 1μs (零 VM-Exit) | < 1μs (零 VM-Exit) |
| **IOMMU 处理** | 设置 PIR + 发送通知中断 | 设置 vIRR + 可能发送 doorbell |
| **Guest 未运行时** | 仍发送通知中断 (由 KVM 处理) | 不发送 doorbell, 写入 GA Log |
| **内存开销** | 64B per vCPU (PI Descriptor) | 4KB per vCPU (vAPIC Backing Page) |
| **缓存友好性** | 较好 (64B 小结构) | 一般 (4KB 大页面) |

### 8.2 实际性能考虑

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    性能优化技术对比                                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Intel VT-d:                                                             │
│    · 通知向量合并: 多个中断可以合并为一次通知                             │
│    · SN 位抑制: vCPU 调出时避免无效通知                                   │
│    · PI Descriptor 小, 缓存友好                                          │
│                                                                          │
│  AMD IOMMU:                                                              │
│    · IsRun 检查: 避免向未运行的 Guest 发送 doorbell                       │
│    · GA Log: 记录未投递的中断, 帮助调度决策                               │
│    · vAPIC Backing Page 与 vLAPIC 共享, 减少拷贝                         │
│                                                                          │
│  共同点:                                                                  │
│    · 都实现零 VM-Exit 的中断注入                                          │
│    · 都需要 IOMMU 和 CPU 的协同支持                                       │
│    · 性能差异主要在于实现细节, 而非架构本质                               │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 🔬 实践建议

### 9.1 检查 AMD AVIC 支持

```bash
# 检查 CPU 是否支持 AVIC
grep -E "avic|avic_" /proc/cpuinfo

# 检查 IOMMU 是否启用
dmesg | grep -i "AMD-Vi\|IOMMU"

# 检查 Guest Virtual APIC 支持
cat /sys/module/amd_iommu/parameters/guest_vapic
# 输出: Y (启用) 或 N (禁用)
```

### 9.2 启用 AMD Guest Virtual APIC

```bash
# 内核参数 (如果需要)
echo "amd_iommu=on" >> /etc/default/grub
echo "iommu=pt" >> /etc/default/grub
update-grub && reboot

# 加载模块时启用
modprobe amd_iommu guest_vapic=1
```

### 9.3 调试 GA Log

```bash
# 查看 GA Log 事件
dmesg | grep -i "GA Log\|GALog"

# 查看 IOMMU 事件日志
cat /sys/kernel/debug/iommu/amd/events
```

---

## ✅ 验证清单

完成后确认能回答：
- [ ] AMD IRTE 的 `GuestMode` 位作用是什么？
- [ ] Guest Virtual APIC Table Root Pointer 指向什么？
- [ ] `IsRun` 位如何影响中断投递？
- [ ] GA Log 的作用是什么？何时使用？
- [ ] AMD AVIC 与 Intel VT-d PI 的主要区别是什么？
- [ ] 两者如何实现零 VM-Exit 中断注入？

---

## 📚 参考资料

**规范引用**:
- amd-virtualization.pdf, Section 2.2.5 (Interrupt Remapping Tables)
- amd-virtualization.pdf, Section 2.2.8 (Guest Virtual APIC Table)
- amd-virtualization.pdf, Section 2.7 (Guest Virtual APIC Logging)
- AMD64 APM Volume 2, Chapter 15 (Advanced Virtual Interrupt Controller)

**源码引用**:
- `drivers/iommu/amd/amd_iommu_types.h` — IRTE 结构定义
- `drivers/iommu/amd/iommu.c` — AMD-Vi 初始化与 IRTE 管理
- `arch/x86/kvm/svm/svm.c` — KVM AMD SVM 与 AVIC 集成

**相关文档**:
- [posted-interrupts.md](posted-interrupts.md) — Intel VT-d Posted Interrupts
- [msi-affinity-migration.md](msi-affinity-migration.md) — MSI 亲和性迁移
