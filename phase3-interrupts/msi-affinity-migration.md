# MSI 地址格式与亲和性迁移

> Phase 3 深度主题 | 从 README.md 拆出，内容未作改动
>
> 前置阅读：[README.md](README.md) 的「技术全景」与「完整中断路径」

---

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
│ 模式 2: IR 模式（无 PI，IM=0）                                  │
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
│   ② 更新pi_desc.NDST = CPU-1                                    │
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
    - 只改Dest_ID和NDST
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
