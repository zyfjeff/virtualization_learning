# Phase 3 文档事实核查报告

**核查日期**: 2026-07-08  
**参考文档**: intel-vtd.pdf (Intel® Virtualization Technology for Directed I/O Architecture Specification, Rev. 4.1)

---

## 📋 核查结果摘要

### ✅ 正确内容
- PI Descriptor 的 64 字节对齐要求 ✓
- PIR 字段 256 位（255:0）✓
- ON 位（Outstanding Notification）✓
- SN 位（Suppress Notification）✓
- NV 字段（Notification Vector）✓
- NDST 字段（Notification Destination）✓
- IRTE 的 PDA 字段指向 PI Descriptor ✓

### ❌ 需要修正的错误

#### 错误 1: IRTE 字段命名错误（严重）

**文档中的错误**：
```
IRTE (PI模式, DM=1):
┌─────────────────────── 低64位 ───────────────────────────┐
│ P │ FPD │ DM=1 │ RH │ TM │ DLM │ AVAIL │URG│ NV │Legacy│PDA[47:26]│
```

**VT-d 规范（Section 9.10, Figure 9-10）**：
- 位 15 的字段名是 **IM (IRTE Mode)**，不是 DM (Delivery Mode)
- IM=1 表示 Posted 模式
- IM=0 表示 Remapped 模式

**规范原文**：
> Bit 15 - IM: IRTE Mode  
> Value of 1 in this field indicate interrupt requests processed through this IRTE are posted.  
> Value of 0 in this field indicate interrupt requests processed through this IRTE are remapped.

**修正**：
```
IRTE (Posted模式, IM=1):
┌─────────────────────── 低64位 ───────────────────────────┐
│ P │ FPD │ Rsvd │ AVAIL │ Rsvd │ URG │ IM=1 │ VV │ Rsvd │ PDA[63:38] │
```

---

#### 错误 2: IRTE 中的向量字段命名错误（严重）

**文档中的错误**：
- 文档称位 23:16 为 "NV" (Notification Vector)

**VT-d 规范（Section 9.10, Table 9-10）**：
- 位 23:16 的字段名是 **VV (Virtual Vector)**，不是 NV
- VV 是 posting 到 PI Descriptor 的 vector

**规范原文**：
> Bits 23:16 - VV: Virtual Vector  
> This 8-bit field contains the vector associated with the interrupt requests posted through this IRTE.

**重要说明**：
- IRTE 中的 VV 字段是 **通知向量**（notification vector）
- 这个 VV 会被写入 PI Descriptor 的 NV 字段
- 两者虽然相关，但在不同结构中命名不同

**修正**：
- IRTE 中：VV (Virtual Vector) @ bits 23:16
- PI Descriptor 中：NV (Notification Vector) @ bits 279:272

---

#### 错误 3: PDA 字段位范围错误（中等）

**文档中的错误**：
```
PDA[47:26] (22位)
```

**VT-d 规范（Section 9.10）**：
- PDAL (Posted Descriptor Address Low): bits 63:38 (26位)
- PDAH (Posted Descriptor Address High): bits 127:96 (32位)
- 总共 58 位，支持 64 字节对齐的地址

**规范原文**：
> Bits 63:38 - PDAL: Posted Descriptor Address Low  
> This field specifies address bits 31:6 of the 64-byte aligned Posted Interrupt Descriptor.
> 
> Bits 127:96 - PDAH: Posted Descriptor Address High  
> This field specifies address bits 63:32 of the 64-byte aligned Posted Interrupt Descriptor.

**修正**：
- PDAL: bits 63:38 (26位, 对应地址 bits 31:6)
- PDAH: bits 127:96 (32位, 对应地址 bits 63:32)

---

#### 错误 4: Remappable 中断格式描述不完整（轻微）

**文档中的描述**：
```
Address [31:20] = 0xFEE00 (相同基址!)
[19:15] = IRTE 索引 bits 0-14 (15位)
[4]     = IRTE 索引 bit 15 (1位)
[3]     = subhandle valid flag
[2]     = dmar_format = 1
```

**VT-d 规范（Section 5.1.2.2, Figure 5-2）**：
- Address[31:20] = 0xFEE (Interrupt Identifier)
- Address[19:5] = Handle[14:0] (15位)
- Address[4] = Interrupt Format (必须为 1)
- Address[3] = SHV (SubHandle Valid)
- Address[2] = Handle[15] (1位)
- Address[1:0] = Don't care

**问题**：
- 文档将 "Handle" 称为 "IRTE 索引"，虽然概念上正确，但规范术语是 "Handle"
- Address[4] 是 "Interrupt Format" 位，不是 "dmar_format"
- 位范围描述不够精确

**修正**：
- Handle[15:0] = Address[19:5] + Address[2]
- Interrupt Format = Address[4] (必须为 1)
- SHV = Address[3]

---

## 🔧 需要更新的源码引用

### 内核源码中的字段命名

查看 `include/linux/dmar.h` 中的 `struct irte` 定义：

```c
/* Posted mode */
struct {
    __u64 p_present   : 1,   /*  0      */
          p_fpd       : 1,   /*  1      */
          p_res0      : 6,   /*  2 -  7 */
          p_avail     : 4,   /*  8 - 11 */
          p_res1      : 2,   /* 12 - 13 */
          p_urgent    : 1,   /* 14      */  // URG field
          p_pst       : 1,   /* 15      */  // ← 这个字段!
          p_vector    : 8,   /* 16 - 23 */  // VV field
          p_res2      : 14,  /* 24 - 37 */
          pda_l       : 26;  /* 38 - 63 */  // PDAL
};
```

**注意**：
- 内核代码中 `p_pst` 对应规范的 IM 字段
- 命名 `p_pst` 可能是 "posted" 的缩写，但规范术语是 IM (IRTE Mode)
- 内核代码中 `p_vector` 对应规范的 VV 字段

---

## 📝 修正建议

### 优先级 1: 立即修正（影响理解）

1. **将所有 "DM" 改为 "IM"**
   - 在 IRTE 格式描述中
   - 在相关代码注释中
   - DM (Delivery Mode) 和 IM (IRTE Mode) 是不同的概念

2. **区分 VV 和 NV**
   - IRTE 中：VV (Virtual Vector)
   - PI Descriptor 中：NV (Notification Vector)
   - 说明两者的关系：IRTE.VV → PI.NV

3. **修正 PDA 字段位范围**
   - PDAL: bits 63:38 (26位)
   - PDAH: bits 127:96 (32位)

### 优先级 2: 术语标准化

4. **使用规范术语**
   - Handle (而非 "IRTE 索引")
   - Interrupt Format (而非 "dmar_format")
   - Posted/Remapped (而非 PI/非PI)

5. **补充说明**
   - 添加规范章节引用
   - 说明内核代码与规范的对应关系

---

## 🎯 总结

文档的核心概念是正确的，但在**字段命名**和**位范围**上存在多处与 Intel VT-d 规范不一致的问题。主要问题：

1. **IM vs DM**: 混淆了 IRTE Mode 和 Delivery Mode
2. **VV vs NV**: 混淆了 Virtual Vector 和 Notification Vector
3. **PDA 位范围**: 描述不准确
4. **术语**: 未使用规范标准术语

建议立即修正这些错误，以确保文档的准确性和权威性。
