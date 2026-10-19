# VMClock：虚拟机高效时间同步

> 基于 VMClock 规范 v1.0（UAPI.13），David Woodhouse (Amazon) 2025-12-15 发布
>
> VMClock 是一项新的半虚拟化时钟同步技术，旨在解决现有机制（kvmclock、ptp_kvm）在
> 大规模云环境下遇到的 **CPU 超分**、**重复校准**和**迁移时间跳变**问题。

---

## 📋 前置知识

阅读本文前，建议先掌握 Phase 8 §4（kvmclock 与 pvclock 协议）的内容，特别是：
- ✅ pvclock_vcpu_time_info 共享页结构
- ✅ kvmclock 的计算公式（`system_time + scale(tsc - timestamp)`）
- ✅ PVCLOCK_TSC_STABLE_BIT 与 vDSO 快速路径
- ✅ ptp_kvm 的 `KVM_HC_CLOCK_PAIRING` hypercall

---

## 📂 本章文件

| 文件 | 内容 |
|------|------|
| `README.md` | 时钟虚拟化完整教程（PIT / APIC Timer / TSC / kvmclock / PTP KVM） |
| `annotations.md` | 源码精读 |
| **`vmclock.md`** | **本文：VMClock 技术深度解析** |
| `practice/` | 动手实验 |

---

## 🎯 学习目标

完成本文后，你应该能够：
1. 列出传统 Guest 自主校时方案面临的三个核心问题
2. 描述 VMClock 的核心思想——**Hypervisor 校时，Guest 直读**
3. 解释 `vmclock_abi` 结构中关键字段的含义
4. 推导 VMClock 的时间计算公式
5. 对比 VMClock 与 kvmclock 的设计差异与适用场景
6. 说明 VMClock 如何优雅处理热迁移与快照恢复

---

## §1 问题背景：Guest 自主校时的三大困境

### 传统校时方案的工作原理

现代系统的时间通常源自 CPU 内部计数器（x86 TSC、ARM Arch Counter），以标称恒定频率
（1–4 GHz）运行。但硬件晶振的实际频率会随温度、电压等环境因素漂移，容差约 ±50 PPM。
这个漂移必须通过与外部精确时钟同步来不断修正。

Guest 自主校时的典型流程（如 ptp_kvm 或网络 NTP）：

```
┌───────────────────────────────────────────────────────────────────┐
│  Guest 校时一次需要做的事情                                         │
│                                                                    │
│   ① 读 CPU 计数器 (TSC_1)                                         │
│   ② 读外部时钟 (REF)  ←── hypercall / 网络往返                    │
│   ③ 再读 CPU 计数器 (TSC_2)                                       │
│   ④ 假设 REF 与 (TSC_1 + TSC_2) / 2 对应                         │
│   ⑤ 得到一对 { CPU counter, real time }                           │
│   ⑥ 重复多次，取 (TSC_2 - TSC_1) 最小的一次（减少误差）           │
│   ⑦ 用多组这样的数据，拟合出 CPU counter 的精确频率               │
└───────────────────────────────────────────────────────────────────┘
```

这个方案在裸金属上运行良好，但在**大规模虚拟化**环境下暴露了三个严重问题。

### 困境一：CPU 超分导致校准精度崩塌

```
┌─ 问题：Steal Time 污染校准采样 ─────────────────────────────────┐
│                                                                   │
│  物理 CPU 时间线:                                                │
│  ├──── VM-A vCPU0 ────┤├── VM-B vCPU0 ──┤├── VM-A vCPU0 ────┤ │
│                        ↑                    ↑                     │
│                   调度出去              调度回来                  │
│                                                                   │
│  Guest 校时流程:                                                 │
│    ① 读 TSC_1 = 1000000        ← 正常执行                      │
│        ↓                                                          │
│    ┄┄┄ vCPU 被调度出去 ┄┄┄  ← steal time，Guest 不执行          │
│        ↓                                                          │
│    ② 读外部时钟 REF           ← 这期间墙钟在走                  │
│        ↓                                                          │
│    ┄┄┄ vCPU 被调度回来 ┄┄┄                                       │
│        ↓                                                          │
│    ③ 读 TSC_2 = 1000050        ← 正常执行                      │
│                                                                   │
│  Guest 看到: TSC_2 - TSC_1 = 50                                  │
│  但实际: REF 对应的时间远超 50 ticks 的墙钟间隔                  │
│  → 校时误差被 steal time 放大！                                  │
│                                                                   │
│  补救措施: 重复多次，取 latency 最小的一组                        │
│  → 但每个 Guest 都在做同样的事…                                 │
└───────────────────────────────────────────────────────────────────┘
```

### 困境二：重复校准浪费 CPU

```
┌─ 问题：N 个 Guest 校准同一个晶振 ──────────────────────────────┐
│                                                                   │
│  物理 Host (4 pCPU):                                             │
│  ┌──────────────────────────────────────┐                       │
│  │  物理 TSC 晶振 (唯一!)               │                       │
│  └──────────┬───────────────────────────┘                       │
│             │                                                    │
│  ┌──────────┼────────────────────────────────────────────┐      │
│  │  VM-1  → 自己校时 (NTP/ptp_kvm)  → 消耗 CPU         │      │
│  │  VM-2  → 自己校时 (NTP/ptp_kvm)  → 消耗 CPU         │      │
│  │  VM-3  → 自己校时 (NTP/ptp_kvm)  → 消耗 CPU         │      │
│  │  …                                                    │      │
│  │  VM-100 → 自己校时 (NTP/ptp_kvm)  → 消耗 CPU        │      │
│  └───────────────────────────────────────────────────────┘      │
│                                                                   │
│  所有 Guest 校准的是**同一个物理晶振**！                         │
│  100 个 Guest = 100 倍重复劳动                                   │
│  而且每个 Guest 的校准结果都因 steal time 有差异                 │
└───────────────────────────────────────────────────────────────────┘
```

### 困境三：热迁移导致时间断崖

```
┌─ 问题：迁移后频率和值都可能跳变 ──────────────────────────────┐
│                                                                   │
│  迁移前 Host-A:                                                  │
│    物理 TSC 频率 = 3.0 GHz                                      │
│    Guest 校准结果: 频率 ≈ 3.0 GHz (±50 PPM)                    │
│    Guest 时间 = TSC × (1/3.0GHz) + offset_A                     │
│                                                                   │
│             ↓─── 热迁移 ───↓                                     │
│                                                                   │
│  迁移后 Host-B:                                                  │
│    物理 TSC 频率 = 2.5 GHz  ← 频率变了!                         │
│    Guest 校准结果: 还是 Host-A 的 3.0 GHz → 错!                │
│    如果重算: offset 可能突变 → 时间跳变!                        │
│                                                                   │
│  后果:                                                           │
│    · Guest 用旧频率计算时间 → 速率偏差 ~20%                     │
│    · 直到重新完成校时（可能需要几十秒到几分钟）                  │
│    · 这段时间内: 数据库事务时间戳错误 → 数据损坏风险            │
│    · TLS 证书校验、Kerberos 票据 → 认证失败                     │
│                                                                   │
│  现有 kvmclock 缓解方案 (KVM_SET_CLOCK + PVCLOCK_GUEST_STOPPED):│
│    · 补偿偏移，但**不解决频率变化**                              │
│    · 跨频率迁移需要 TSC scaling 硬件支持                        │
│    · 精度依赖 VMM 对停机时间的估计准确性                        │
└───────────────────────────────────────────────────────────────────┘
```

### 三大困境总结

| 困境 | 根因 | 现有方案的局限 |
|------|------|---------------|
| **Steal time 污染校准** | vCPU 超分时 Guest 执行不连续 | 重复采样可降低概率，但无法消除 |
| **N 个 Guest 重复校准** | 每个 Guest 独立做同样的工作 | 浪费 CPU；各 Guest 结果不一致 |
| **热迁移时间断崖** | 迁移后物理晶振变了，Guest 不知道 | kvmclock 补偿偏移但不保证频率；重校时需收敛时间 |

---

## §2 VMClock 的核心思想

### 一句话总结

> **让 Hypervisor 代替 Guest 做校时，把校时结果（公式 + 参数）直接写进共享内存，
> Guest 只需读内存 + 算术运算即可获得精确时间。**

### 设计哲学对比

```
┌─ 传统方案 (Guest 自主校时) ────────────────────────────────────┐
│                                                                   │
│  Guest                              Host / 外部时钟              │
│  ┌─────────────────┐               ┌──────────────────┐        │
│  │ 读 TSC          │──hypercall──▶│ 读外部时钟       │        │
│  │ 读外部时钟      │◀──返回──────│                  │        │
│  │ 读 TSC          │               │                  │        │
│  │ 计算频率/偏移   │               │                  │        │
│  │ (消耗 CPU!)     │               │                  │        │
│  │ 重复 N 次…     │               │                  │        │
│  └─────────────────┘               └──────────────────┘        │
│                                                                   │
│  问题: Guest 做了所有校时工作，但用的是 Host 的晶振             │
└───────────────────────────────────────────────────────────────────┘

┌─ VMClock 方案 (Hypervisor 校时，Guest 直读) ───────────────────┐
│                                                                   │
│  Host/Hypervisor                    Guest                        │
│  ┌─────────────────┐               ┌──────────────────┐        │
│  │ 用 NTP/PTP 校时 │               │                  │        │
│  │ 精确测量 TSC 频率│               │                  │        │
│  │ 写入共享内存:   │──共享内存──▶│ 读共享内存       │        │
│  │   T_1 (参考时间) │  (零开销!)   │ 读 TSC           │        │
│  │   C_1 (TSC值)   │               │ 计算:            │        │
│  │   P (tick周期)  │               │  T = T_1 + P×    │        │
│  │   (含误差界)    │               │    (TSC_now - C_1)│        │
│  └─────────────────┘               │ (几次乘加，纳秒级)│        │
│                                     └──────────────────┘        │
│                                                                   │
│  优势:                                                           │
│    · Guest 零校时开销                                            │
│    · 所有 Guest 共享同一份精确参数 → 一致性                      │
│    · 迁移后 Hypervisor 立即更新参数 → Guest 无感知              │
│    · 提供误差界 → Guest 知道时间精度                             │
└───────────────────────────────────────────────────────────────────┘
```

---

## §3 vmclock_abi 结构详解

VMClock 通过 Hypervisor 放置在共享内存中的一个 C 结构体工作。Guest 通过
ACPI 或 Device Tree 发现该设备及其共享页的物理地址。

> **规范引用**: UAPI.13 VMClock (uapi-group.org/specifications/specs/vmclock/)

### 3.1 完整结构布局

```c
/* 来源: UAPI.13 VMClock 规范 v1.1 */

struct vmclock_abi {
    /* ─── 常量字段（设备生命周期内不变）────────── */
    uint32_t magic;              /* 0x00: 魔数 0x4b4c4356 ("VCLK") */
    uint32_t size;               /* 0x04: 共享区域大小（通常为 4KiB 页） */
    uint16_t version;            /* 0x08: 规范版本（当前 = 1） */
    uint8_t  counter_id;         /* 0x0a: 硬件计数器类型 */
    uint8_t  time_type;          /* 0x0b: 时间类型（UTC/TAI/Monotonic） */

    /* ─── 动态字段（用 seq_count 同步更新）────── */
    uint32_t seq_count;          /* 0x0c: 序列锁（奇数=更新中，偶数=就绪） */
    uint64_t disruption_marker;  /* 0x10: 迁移/中断计数器 */
    uint64_t flags;              /* 0x18: 特性标志位 */
    uint16_t pad;                /* 0x20: 填充 */
    uint8_t  clock_status;       /* 0x22: 同步状态 */
    uint8_t  leap_second_smearing_hint; /* 0x23: 闰秒平滑提示 */
    int16_t  tai_offset_sec;     /* 0x24: TAI→UTC 偏移（秒） */
    uint8_t  leap_indicator;     /* 0x26: 闰秒指示 */
    uint8_t  counter_period_shift; /* 0x27: 周期位移量 */

    /* ─── 核心时间参数 ─────────────────────────── */
    uint64_t counter_value;      /* 0x28: 参考时刻的计数器值 C₁ */
    uint64_t counter_period_frac_sec;  /* 0x30: 单 tick 周期（定点数） */
    uint64_t counter_period_esterror_rate_frac_sec;  /* 0x38: 周期估计误差 */
    uint64_t counter_period_maxerror_rate_frac_sec;  /* 0x40: 周期最大误差 */
    uint64_t time_sec;           /* 0x48: 参考时间（秒） */
    uint64_t time_frac_sec;      /* 0x50: 参考时间（小数部分） */
    uint64_t time_esterror_nanosec;  /* 0x58: 时间估计误差（纳秒） */
    uint64_t time_maxerror_nanosec;  /* 0x60: 时间最大误差（纳秒） */
    uint64_t vm_generation_counter;  /* 0x68: VM 代数计数器 */
};
```

### 3.2 关键设计决策

#### 为什么用 TAI 而不是 UTC？

规范**强烈推荐**使用 TAI（International Atomic Time）作为时间标准：

```
UTC 的问题：
  · UTC 有闰秒 → 同一个秒数可能出现两次
  · Guest 计算时间时需要处理闰秒歧义
  · 需要 leap_indicator 字段辅助判断

TAI 的优势：
  · 无闰秒 → 每秒严格等长，计算简单
  · 通过 tai_offset_sec 给出 TAI→UTC 的偏移
  · 当前值 = -37（即 UTC = TAI - 37 秒）
  · Guest 如需 UTC 可自行转换，无需处理闰秒边界
```

#### counter_id：支持多种硬件计数器

| 值 | 含义 | 说明 |
|---|---|---|
| `0x00` | `VMCLOCK_COUNTER_ARM_VCNT` | ARM 虚拟计数器 |
| `0x01` | `VMCLOCK_COUNTER_X86_TSC` | x86 TSC |
| `0xFF` | `VMCLOCK_COUNTER_INVALID` | 无有效计数器 |

> **与 kvmclock 的关键差异**：kvmclock 硬编码使用 x86 TSC；VMClock 是跨架构的。

#### clock_status：同步状态机

```
                    ┌─────────────┐
         创建 ───▶ │ INITIALIZING│
                    │   (0x01)    │
                    └──────┬──────┘
                           │ NTP/PTP 同步完成
                           ▼
          ┌────────────────────────────────┐
          │         SYNCHRONIZED          │◀──────────────┐
          │            (0x02)             │                │
          └───┬──────────────────────┬───┘                │
              │                      │                    │
     外部时钟丢失              外部时钟恢复              │
              │                      │                    │
              ▼                      └────────────────────┘
    ┌─────────────────┐
    │   FREERUNNING   │
    │     (0x03)      │
    │ (参数仍可用，    │
    │  但不保证持续    │
    │  校准)          │
    └────────┬────────┘
             │ 检测到硬件故障
             ▼
    ┌─────────────────┐
    │   UNRELIABLE    │
    │     (0x04)      │
    │ (参数不可信!)   │
    └─────────────────┘
```

---

## §4 时间计算公式

### 4.1 基本公式

VMClock 提供的核心参数构成一个简单的线性变换：

```
┌──────────────────────────────────────────────────────────────────┐
│  已知量（从 vmclock_abi 读取）:                                  │
│    T₁ = time_sec + time_frac_sec     (参考时间)                 │
│    C₁ = counter_value                 (参考时刻的计数器值)       │
│    P  = counter_period_frac_sec × 2^-(64 + counter_period_shift)│
│         (单个 tick 的周期，单位秒)                               │
│                                                                   │
│  Guest 运行时测量:                                               │
│    C_now = 当前读到的硬件计数器值（如 RDTSC）                    │
│                                                                   │
│  计算公式:                                                       │
│    T_now = T₁ + P × (C_now - C₁)                               │
│                                                                   │
│  物理意义:                                                       │
│    "参考时间" + "经过的 tick 数" × "每个 tick 的秒数"           │
│         = "当前时间"                                             │
└──────────────────────────────────────────────────────────────────┘
```

### 4.2 定点数精度的意义

单 tick 周期的计算使用了高精度定点数表示：

```
┌─ 示例: 1 GHz 计数器 ───────────────────────────────────────────┐
│                                                                   │
│  精确周期 = 1 ns = 10⁻⁹ 秒                                     │
│                                                                   │
│  朴素表示（无 shift）:                                           │
│    counter_period_frac_sec = 0x44B82FA0A                        │
│    P = 0x44B82FA0A / 2⁶⁴                                        │
│    = 18014398506.97 / 1.8×10¹⁹                                 │
│    ≈ 1.000000000 × 10⁻⁹ 秒                                     │
│                                                                   │
│  问题: 64 位定点数的精度有限，长时间内误差会累积               │
│    运行 1 天后: 误差 ≈ 几微秒                                  │
│    运行 1 月后: 误差 ≈ 几十微秒                                │
│                                                                   │
│  精确表示（使用 shift = 29）:                                    │
│    counter_period_frac_sec = 0x89705F4136B4A597                │
│    counter_period_shift = 29                                     │
│    P = 0x89705F4136B4A597 / 2^(64+29)                           │
│    = 9765625000734658967 / 5.76×10²⁷                           │
│    ≈ 1.000000000000000 × 10⁻⁹ 秒（更高精度!）                 │
│                                                                   │
│  shift 的作用: 用更多的有效位表示周期，减少长期累积误差          │
└──────────────────────────────────────────────────────────────────┘
```

### 4.3 误差界计算

VMClock 的独特特性之一是提供**误差界**，让 Guest 知道时间的可信程度：

```
┌─ 误差界公式 ───────────────────────────────────────────────────┐
│                                                                   │
│  真实时间 ∈ [T_calc - E_total, T_calc + E_total]               │
│                                                                   │
│  其中:                                                           │
│    T_calc = T₁ + P × (C_now - C₁)          (计算出的时间)      │
│                                                                   │
│    E_total = E_time + E_period × (C_now - C₁)                  │
│                                                                   │
│    E_time = time_esterror_nanosec 或 time_maxerror_nanosec      │
│             (参考时间 T₁ 的误差)                                │
│                                                                   │
│    E_period = counter_period_esterror_rate_frac_sec             │
│               或 counter_period_maxerror_rate_frac_sec           │
│             (单 tick 周期的误差率)                               │
│                                                                   │
│  两类误差:                                                       │
│    · 估计误差 (esterror): 典型情况下的误差                      │
│    · 最大误差 (maxerror): 最坏情况下的误差上界                  │
│                                                                   │
│  用途:                                                           │
│    · 数据库事务排序: 两个事件的误差界不重叠 → 可确定先后顺序  │
│    · 安全关键系统: 知道时间精度是否满足要求                      │
│    · 迁移后评估: maxerror 突然增大 → 需要重新校时              │
└──────────────────────────────────────────────────────────────────┘
```

### 4.4 seq_count 序列锁的读法

VMClock 使用经典的 seqcount 机制保证 Guest 读到一致的参数集：

```c
/* Guest 读取时间的伪代码 */

static uint64_t vmclock_read_time(struct vmclock_abi *vmc)
{
    uint32_t seq;
    uint64_t t_sec, t_frac, c_val, period;
    uint8_t  period_shift;

    do {
        /* ① 读 seq_count，必须是偶数（表示无更新在进行） */
        seq = vmc->seq_count;
        if (seq & 1) {
            /* 奇数 = Hypervisor 正在更新，自旋等待 */
            continue;
        }

        /* ② 内存屏障：确保后续读取在 seq 之后 */
        smp_rmb();

        /* ③ 读取所有时间参数 */
        t_sec        = vmc->time_sec;
        t_frac       = vmc->time_frac_sec;
        c_val        = vmc->counter_value;
        period       = vmc->counter_period_frac_sec;
        period_shift = vmc->counter_period_shift;

        /* ④ 读当前硬件计数器 */
        uint64_t c_now = read_counter();  /* RDTSC 或 ARM arch counter */

        /* ⑤ 再次读 seq_count，检查是否中途被改过 */
        smp_rmb();
    } while (vmc->seq_count != seq);
    /* 如果 seq 变了 → 参数可能不一致 → 重头再来 */

    /* ⑥ 计算时间: T = T₁ + P × (C_now - C₁) */
    uint64_t delta_ticks = c_now - c_val;
    /* T = t_sec + t_frac/2^64 + period * delta_ticks / 2^(64+shift) */
    return compute_time(t_sec, t_frac, period, period_shift, delta_ticks);
}
```

---

## §5 VMClock 如何优雅处理热迁移

### 5.1 disruption_marker：迁移信号

```
┌─ 迁移前后的 VMClock 状态变化 ─────────────────────────────────┐
│                                                                   │
│  迁移前 (Host-A):                                              │
│    vmc->disruption_marker = 0                                   │
│    vmc->counter_value = 5000000000   (Host-A 的 TSC)           │
│    vmc->counter_period = 1ns / 2^29  (Host-A 的 3 GHz)         │
│    vmc->time_sec = 1700000000        (校准后的墙上时间)         │
│                                                                   │
│             ↓─── 热迁移发生 ───↓                                 │
│                                                                   │
│  迁移后 (Host-B):                                              │
│    vmc->disruption_marker = 1  ← ★ 加 1，通知 Guest!          │
│    vmc->counter_value = 3000000000   (Host-B 的 TSC，值不同!)  │
│    vmc->counter_period = 1ns / 2^29  (Host-B 可能频率不同)     │
│    vmc->time_sec = 1700000005        (迁移花了 5 秒)            │
│                                                                   │
│  Guest 的行为:                                                   │
│    ① 读到 disruption_marker 从 0 变成 1                        │
│    ② 丢弃之前所有 NTP 校准状态                                 │
│    ③ 直接用新的 (T₁, C₁, P) 计算时间 ← 立即精确!             │
│    ④ 可选: 重新开始 NTP 精化（但不急，当前参数已可用）        │
│                                                                   │
│  对比 kvmclock:                                                  │
│    · kvmclock 需要 QEMU 调 KVM_SET_CLOCK 补偿偏移              │
│    · 频率变化需要 TSC scaling 硬件                              │
│    · PVCLOCK_GUEST_STOPPED 只通知，不直接修正                  │
│    · VMClock 直接给新参数 → Guest 零等待                       │
└───────────────────────────────────────────────────────────────────┘
```

### 5.2 迁移预通知（DISRUPTION_SOON / DISRUPTION_IMMINENT）

VMClock 的 flags 提供了**迁移预警**能力：

| 标志位 | 含义 | Guest 行为 |
|--------|------|-----------|
| `VMCLOCK_FLAG_DISRUPTION_SOON` (bit 1) | ~1 天内可能有迁移 | 降低校准精度期望，准备切换 |
| `VMCLOCK_FLAG_DISRUPTION_IMMINENT` (bit 2) | ~1 小时内可能迁移 | 进一步收紧误差界，可能需要通知应用层 |

这是 kvmclock 完全没有的能力。

### 5.3 vm_generation_counter：快照/克隆检测

```
┌─ vm_generation_counter 的变化时机 ────────────────────────────┐
│                                                                   │
│  会增加的场景（VM 被恢复到更早/非唯一状态）:                   │
│    ✓ 从快照恢复                                                │
│    ✓ 从备份恢复                                                │
│    ✓ VM 克隆/复制/导入                                         │
│    ✓ 灾难恢复故障切换                                          │
│                                                                   │
│  不变的场景（正常运维操作）:                                    │
│    ✗ 暂停/恢复 (pause/resume)                                  │
│    ✗ 关机/重启 (shutdown/reboot)                                │
│    ✗ 宿主机重启/升级                                            │
│    ✗ 热迁移 (live migration) ← 注意：这不改变 generation!      │
│                                                                   │
│  Guest 行为:                                                     │
│    · 检测到 vm_generation_counter 变化                          │
│    · 重新生成 UUID（避免与其他克隆冲突）                       │
│    · 重置网络连接（TCP 状态可能不一致）                        │
│    · 重新播种随机数生成器（熵池可能重复）                      │
│                                                                   │
│  与 disruption_marker 的关系:                                    │
│    · 两者正交但有时相关                                         │
│    · 热迁移: disruption_marker 变，vm_generation_counter 不变  │
│    · 快照恢复: 两者都可能变                                     │
└───────────────────────────────────────────────────────────────────┘
```

---

## §6 VMClock 与 kvmclock 的对比

### 6.1 设计哲学对比

| 维度 | kvmclock (pvclock) | VMClock |
|------|-------------------|---------|
| **谁做校时** | Guest 自己（通过 ptp_kvm / NTP） | Hypervisor 代为完成 |
| **参数来源** | Guest TSC + host 单调时钟锚点 | Hypervisor 外部校时后写入 |
| **频率表示** | 隐含使用 guest TSC（假设频率已知） | 显式 `counter_period_frac_sec`（含误差界） |
| **误差信息** | 无 | 提供 `esterror` 和 `maxerror` |
| **迁移处理** | `KVM_SET_CLOCK` + `PVCLOCK_GUEST_STOPPED` | `disruption_marker` + 参数直接替换 |
| **快照检测** | 无（依赖外部 VMGenID） | 内建 `vm_generation_counter` |
| **迁移预警** | 无 | `DISRUPTION_SOON` / `DISRUPTION_IMMINENT` |
| **架构支持** | x86 only | x86 (TSC) + ARM (Arch Timer) |
| **发现机制** | KVM CPUID + MSR 注册 | ACPI / Device Tree（标准设备模型） |
| **KVM 耦合度** | 强耦合（KVM-specific hypercall） | 解耦（纯共享内存，无需特定 hypervisor） |

### 6.2 时间计算公式对比

```
┌─ kvmclock (pvclock) 公式 ──────────────────────────────────────┐
│                                                                   │
│  time = system_time + ((tsc - tsc_timestamp) × mul) >> shift    │
│                                                                   │
│  参数:                                                           │
│    system_time:    host 单调时钟在锚点时刻的值（纳秒）          │
│    tsc_timestamp:  锚点时刻的 guest TSC 值                       │
│    mul, shift:     TSC→纳秒 的换算参数（由 KVM 计算）          │
│                                                                   │
│  隐含假设: TSC 频率恒定且已知                                    │
│  误差源: tsc_timestamp 写入时刻的抖动                           │
│  迁移时: 需重新计算 system_time 和 tsc_timestamp                │
└───────────────────────────────────────────────────────────────────┘

┌─ VMClock 公式 ──────────────────────────────────────────────────┐
│                                                                   │
│  time = T₁ + P × (C_now - C₁)                                  │
│                                                                   │
│  参数:                                                           │
│    T₁ = time_sec + time_frac_sec     (参考时间，高精度)         │
│    C₁ = counter_value                (参考时刻的计数器值)       │
│    P  = counter_period_frac_sec × 2^-(64+shift) (tick 周期)    │
│                                                                   │
│  显式表示: 频率（通过周期 P）和误差都直接给出                   │
│  误差界: time_esterror/maxerror + period_esterror/maxerror      │
│  迁移时: 直接替换 (T₁, C₁, P) 三元组                          │
└───────────────────────────────────────────────────────────────────┘
```

### 6.3 迁移场景下的行为对比

```
┌─ 场景: 跨频率迁移 (Host-A 3GHz → Host-B 2.5GHz) ─────────────┐
│                                                                   │
│  kvmclock:                                                      │
│    1. QEMU 保存 kvmclock_offset = guest_time - host_monotonic   │
│    2. 迁移到 Host-B                                             │
│    3. QEMU 调 KVM_SET_CLOCK 恢复 kvmclock_offset                │
│    4. 如果 Host-B 不支持 TSC scaling:                           │
│       → Guest TSC 频率变成 2.5 GHz                              │
│       → 但 mul/shift 还是按 3 GHz 算的                         │
│       → 时间计算错误! ❌                                        │
│    5. 如果支持 TSC scaling:                                     │
│       → KVM 重算 TSC_MULTIPLIER = 2.5/3.0                      │
│       → Guest TSC 仍"看到" 3 GHz                               │
│       → mul/shift 仍有效 ✅                                    │
│    6. PVCLOCK_GUEST_STOPPED 通知 Guest 被暂停过                │
│    7. Guest 需要重新 NTP 同步墙钟                              │
│                                                                   │
│  VMClock:                                                       │
│    1. Hypervisor 在 Host-B 上用 NTP/PTP 精确校时               │
│    2. 写入新的 vmclock_abi:                                     │
│       · counter_value = 当前 Host-B TSC                         │
│       · counter_period = 1/2.5GHz（精确值，含误差界）          │
│       · time_sec = 当前精确墙钟                                │
│       · disruption_marker++                                     │
│    3. Guest 读到 disruption_marker 变化                         │
│    4. 立即用新参数计算时间 ← 精确! ✅                          │
│    5. 可选: 重新开始 NTP 精化（但不急）                        │
│                                                                   │
│  结论: VMClock 不依赖 TSC scaling 硬件                         │
│        Hypervisor 直接告诉 Guest 新频率是多少                    │
└───────────────────────────────────────────────────────────────────┘
```

### 6.4 功能覆盖对比

| 功能 | kvmclock | VMClock | 说明 |
|------|----------|---------|------|
| 高精度单调时钟 | ✅ | ✅ | 两者都提供纳秒级精度 |
| 墙上时间 | 需 ptp_kvm 辅助 | ✅ 直接提供 | VMClock 直接给出 TAI/UTC |
| 误差界 | ❌ | ✅ | VMClock 独有 |
| 迁移无缝切换 | ⚠️ 需 TSC scaling | ✅ 参数直接替换 | VMClock 不依赖特定硬件 |
| 迁移预警 | ❌ | ✅ | DISRUPTION_SOON/IMMINENT |
| 快照/克隆检测 | ❌ 需 VMGenID | ✅ 内建 | vm_generation_counter |
| vDSO 快速路径 | ✅ | ✅（设计目标） | 两者都支持用户态直读 |
| ARM 支持 | ❌ | ✅ | VMClock 跨架构 |
| 闰秒处理 | 不涉及 | ✅ leap_indicator | VMClock 显式处理 |
| CPU 超分健壮性 | ⚠️ 受 steal time 影响 | ✅ Hypervisor 代校 | VMClock 核心优势 |

---

## §7 VMClock 的设备发现

### 7.1 ACPI 发现方式

Hypervisor 需要：

1. **放置共享页**：在 RAM/ROM/设备内存中放置 `vmclock_abi` 结构，不得位于
   `Convertible` 或 `NVDIMM` 区域，不得与缓存禁用映射共享同一页。

2. **创建 ACPI 设备**：

```asl
Device (VMCK)
{
    Name (_HID, "VCLK0001")    // 硬件 ID
    Name (_DDN, "VMClock")     // DOS 设备名
    Name (_CID, "VMCLK0001")   // 兼容 ID

    Method (_CRS, 0, Serialized)
    {
        // 资源描述符：共享内存区域
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0xFE800000, 0x1000)  // 4KiB 页
        })
        Return (RBUF)
    }

    // 可选: 支持通知
    Method (_STA) { Return (0x0F) }  // 设备存在且启用
}
```

3. **通知机制**（可选）：当 `seq_count` 更新为新的偶数值时，触发 ACPI Notify
   （通知码 0x80）。需要设置 `VMCLOCK_FLAG_NOTIFICATION_PRESENT` 标志。

### 7.2 Device Tree 发现方式

```dts
vmclock@fe800000 {
    compatible = "vmclock";
    reg = <0xfe800000 0x1000>;  // 基地址 + 大小
};
```

### 7.3 与 kvmclock 发现方式的对比

```
┌─ kvmclock 发现 ────────────────────────────────────────────────┐
│  1. Guest 检测 KVM CPUID 签名 (CPUID 0x40000000)               │
│  2. 检查 KVM_FEATURE_CLOCKSOURCE feature bit                   │
│  3. Guest 写 MSR_KVM_SYSTEM_TIME 注册 pvclock 页 GPA          │
│  4. KVM 填充 pvclock_vcpu_time_info 到该页                    │
│                                                                   │
│  特点: KVM 专属，依赖 hypercall 和 MSR                        │
└───────────────────────────────────────────────────────────────────┘

┌─ VMClock 发现 ─────────────────────────────────────────────────┐
│  1. Guest 枚举 ACPI / Device Tree 发现 VMClock 设备            │
│  2. 映射共享内存页                                              │
│  3. 检查 magic == 0x4b4c4356 ("VCLK")                        │
│  4. 直接读取参数，开始使用                                     │
│                                                                   │
│  特点: 标准设备模型，与 Hypervisor 类型无关                   │
│        不需要 hypercall，不需要 MSR                            │
│        理论上任何 VMM（QEMU、crosvm、cloud-hypervisor）都能实现│
└───────────────────────────────────────────────────────────────────┘
```

---

## §8 实现状态（截至 2026-10）

### 8.1 总览

截至 2026 年 10 月，VMClock 的实现情况如下：

| 组件 | 状态 | 版本/位置 |
|------|------|----------|
| **规范** | ✅ 已发布 | UAPI.13 v1.1 (uapi-group.org) |
| **Linux Guest 驱动** | ✅ 已合入主线 | Linux 7.0+ (2024-10 首次合入，7.3.0 持续改进) |
| **Firecracker Host 侧 (Phase 1)** | ✅ 已实现 | 仅通知机制，不含时间同步 |
| **KVM/QEMU Host 侧** | ❌ 未实现 | 主线内核无相关代码 |

### 8.2 Linux Guest 驱动（已合入主线）

**源码引用**: `drivers/ptp/ptp_vmclock.c` (Linux 7.3.0, 797 行)
**UAPI 头文件**: `include/uapi/linux/vmclock-abi.h` (202 行)
**Kconfig**: `CONFIG_PTP_1588_CLOCK_VMCLOCK`

Guest 侧驱动功能完整：
- ACPI 设备发现 (`AMZNC10C` / `VMCLOCK` HID)
- Device Tree 设备发现 (`amazon,vmclock` compatible)
- seqcount 同步读取
- PTP 时钟注册 (`/dev/ptpN`)
- 用户态映射 (`/dev/vmclockN`，只读 mmap)
- poll/epoll 通知支持
- `vm_generation_counter` 快照检测
- TAI/UTC 转换
- kvmclock 兼容模式（`CSID_X86_KVM_CLK`）

**关键代码** (Guest 读取时间)：

```c
/* 来源: drivers/ptp/ptp_vmclock.c:155-162 (Linux 7.3.0) */
delta = cycle - le64_to_cpu(st->clk->counter_value);

frac_sec = mul_u64_u64_shr_add_u64(&tspec->tv_sec, delta,
                                   le64_to_cpu(st->clk->counter_period_frac_sec),
                                   st->clk->counter_period_shift,
                                   le64_to_cpu(st->clk->time_frac_sec));
tspec->tv_nsec = mul_u64_u64_shr(frac_sec, NSEC_PER_SEC, 64);
tspec->tv_sec += le64_to_cpu(st->clk->time_sec);
```

### 8.3 Firecracker Host 侧实现（Phase 1：仅通知机制）

> **源码引用**: `src/vmm/src/devices/acpi/vmclock.rs` (Firecracker, 289 行)
> 首次合入: 2025-10-21 by Babis Chalios (Amazon)

Firecracker 的 VMClock 实现是**分阶段的**。当前 Phase 1 只实现了**通知机制**，
**没有**填充时间参数字段。

#### Phase 1 已实现的功能

```
┌─ Firecracker VMClock Phase 1 实现 ─────────────────────────────┐
│                                                                   │
│  ✅ 共享内存分配:                                                │
│     · 在 guest 物理内存末尾分配 4KiB 页                         │
│     · 写入 vmclock_abi 头部 (magic, size, version)              │
│                                                                   │
│  ✅ ACPI 设备暴露:                                               │
│     · HID = "AMZNC10C" (Amazon 专有 ID)                         │
│     · CID = "VMCLOCK"                                           │
│     · _CRS 指向共享页物理地址                                   │
│     · _STA = 0x0F (设备存在且启用)                              │
│                                                                   │
│  ✅ 中断通知:                                                    │
│     · 分配 GSI，注册 eventfd                                     │
│     · 更新时触发中断，guest 可 poll() 等待                      │
│                                                                   │
│  ✅ 快照恢复通知:                                                │
│     · do_post_restore() 中:                                     │
│       ① seq_count → 奇数 (开始更新)                             │
│       ② disruption_marker++                                      │
│       ③ vm_generation_counter++                                  │
│       ④ seq_count → 偶数 (更新完成)                             │
│       ⑤ 触发中断通知 guest                                      │
│                                                                   │
│  ✅ 状态保存/恢复:                                               │
│     · Serialize/Deserialize vmclock_abi 到快照文件              │
│     · 恢复时重建设备并通知 guest                                 │
│                                                                   │
│  ✅ ARM64 支持:                                                  │
│     · Device Tree 节点 (compatible = "amazon,vmclock")          │
│     · 中断通过 GIC SPI 传递                                     │
└───────────────────────────────────────────────────────────────────┘
```

#### Phase 1 未实现的功能（关键！）

```
┌─ 未填充的字段 ─────────────────────────────────────────────────┐
│                                                                   │
│  ❌ counter_id = VMCLOCK_COUNTER_INVALID (0xFF)                 │
│     → Guest 驱动看到 INVALID 不会注册 PTP 设备                 │
│                                                                   │
│  ❌ clock_status = VMCLOCK_STATUS_UNKNOWN                       │
│     → Guest 驱动会返回 -EINVAL                                  │
│                                                                   │
│  ❌ counter_value = 0         (参考时刻的 TSC 值)              │
│  ❌ counter_period_frac_sec = 0 (TSC tick 周期)                │
│  ❌ time_sec = 0              (参考时间，秒)                    │
│  ❌ time_frac_sec = 0         (参考时间，小数)                  │
│  ❌ time_type = 0 (UTC)       但未设 TAI_OFFSET_VALID          │
│  ❌ tai_offset_sec = 0                                          │
│  ❌ 所有 error bounds = 0                                       │
│                                                                   │
│  结果: Guest 的 ptp_vmclock 驱动能发现设备，但无法使用         │
│        时间同步功能。只能使用通知机制。                         │
└───────────────────────────────────────────────────────────────────┘
```

#### Firecracker Phase 1 的实际用途

根据 Firecracker 文档 (`docs/snapshotting/snapshot-support.md`)：

> VMClock ... Currently, it handles two cases:
> 1. Live migration through the `disruption_marker` field.
> 2. Restore from snapshots through the `vm_generation_counter`.
>
> User space libraries, e.g. userspace PRNGs can mmap() `vmclock_abi`
> and monitor changes in `vm_generation_counter` to observe when they
> need to adapt and/or recreate state.

**Phase 1 的目标用户不是时间同步，而是**：
1. **随机数生成器重播种**：检测到 `vm_generation_counter` 变化 → 重新播种 PRNG
2. **UUID 重新生成**：避免克隆 VM 的 UUID 冲突
3. **网络连接重置**：检测到快照恢复后断开旧连接
4. **应用层迁移感知**：通过 `disruption_marker` 知道发生了迁移

#### Firecracker VMClock 测试工具

**源码引用**: `tests/host_tools/vmclock.c` (Firecracker, 188 行)

这个 C 工具验证了 Phase 1 的功能边界——只读取通知相关字段：

```c
/* 来源: tests/host_tools/vmclock.c:86-101 */
void print_vmclock(struct vmclock_abi *vmclock)
{
  printf("VMCLOCK_FLAG_VM_GEN_COUNTER_PRESENT: %s\n", ...);
  printf("VMCLOCK_FLAG_NOTIFICATION_PRESENT: %s\n", ...);
  printf("VMCLOCK_MAGIC: 0x%x\n", vmclock->magic);
  printf("VMCLOCK_SIZE: 0x%x\n", vmclock->size);
  printf("VMCLOCK_VERSION: %u\n", vmclock->version);
  printf("VMCLOCK_CLOCK_STATUS: %u\n", vmclock->clock_status);
  printf("VMCLOCK_COUNTER_ID: %u\n", vmclock->counter_id);
  printf("VMCLOCK_DISRUPTION_MARKER: %lu\n", read_disruption_marker(vmclock));
  printf("VMCLOCK_VM_GENERATION_COUNTER: %lu\n", read_vm_generation_counter(vmclock));
  /* 注意：不打印 counter_value / time_sec / counter_period 等时间字段 */
}
```

### 8.4 KVM/QEMU Host 侧（未实现）

搜索 Linux 7.3.0 内核源码树：

```bash
# KVM 中没有 vmclock_abi 填充代码
$ grep -rn "vmclock_abi" arch/x86/kvm/
# 无结果

# 整个内核树中无 hypervisor 侧填充代码
$ grep -rn "VMCLOCK_MAGIC" --include="*.c" | grep -v ptp_vmclock
# 无结果
```

KVM/QEMU 的 VMClock 实现需要：
1. **QEMU (用户态)**：分配共享页、生成 ACPI 表、迁移协调
2. **KVM (内核态)**：精确 cross-timestamp、周期性更新参数

### 8.5 实现时间线

```
2024-06     David Woodhouse 提交 RFC PATCH v2 (ptp_vmclock guest 驱动)
2024-07     PATCH v3
2024-10-06  ★ Guest 驱动首次合入 Linux 主线 (commit 205032724226)
            最初仅支持 ACPI，设备 ID "AMZNC10C"
2025-02     修复 driver data 设置时序 (进入 net-next)
2025-10-21  ★ Firecracker Phase 1 合入 (x86_64)
            commit ef215b91b by Babis Chalios
2025-12-05  Firecracker 添加 ARM64 支持
2026-01-30  Guest 驱动添加 vm_generation_counter + ACPI 通知
            (合入 Linux 7.0)
2026-05     Guest 驱动切换至 ktime_get_snapshot_id()
2026-06     Guest 驱动使用 hw_cycles 精确配对 TSC
2026-08-13  Guest 驱动最新改进（防止 mmap 变可写）
2026-10     当前状态：Guest 驱动完整，Firecracker Phase 1，KVM 未实现
```

### 8.6 预测：后续阶段

基于 Firecracker Phase 1 的设计，预计后续阶段会：

```
Phase 2 (预期): 基础时间同步
  · 填充 counter_id = VMCLOCK_COUNTER_X86_TSC / ARM_VCNT
  · 填充 clock_status = VMCLOCK_STATUS_SYNCHRONIZED
  · 周期性更新 counter_value / time_sec / counter_period_frac_sec
  · 填充 tai_offset_sec = -37
  · Guest 可以注册 PTP 设备并使用 chrony 同步

Phase 3 (预期): 误差界 + 迁移无缝
  · 填充 esterror / maxerror 字段
  · 迁移时更新 disruption_marker + 新参数
  · DISRUPTION_SOON / DISRUPTION_IMMINENT 预警
  · Guest 迁移后零等待获得精确时间
```

---

## §9 适用场景与部署建议

### 9.1 推荐使用 VMClock 的场景

| 场景 | 原因 |
|------|------|
| **大规模云环境（数千 VM）** | Hypervisor 统一校时，避免 N 个 Guest 重复校准 |
| **vCPU 严重超分** | Guest 不做校时采样，不受 steal time 影响 |
| **频繁热迁移** | `disruption_marker` 机制无缝切换，零收敛等待 |
| **跨架构虚拟化** | 同时支持 x86 TSC 和 ARM Arch Timer |
| **需要误差界的应用** | 金融交易、数据库事务排序等需要知道时间精度 |
| **快照密集型工作负载** | `vm_generation_counter` 内建快照检测 |
| **非 KVM Hypervisor** | 标准 ACPI/DT 设备模型，不依赖特定 hypervisor |

### 9.2 仍应使用 kvmclock 的场景

| 场景 | 原因 |
|------|------|
| **KVM-only 环境，不迁移** | kvmclock 已足够，rating 400 |
| **需要最低延迟的时间读取** | kvmclock vDSO 已高度优化（~20-30 ns） |
| **Guest 内核版本较老** | VMClock 驱动尚未合入主线（截至 6.12） |
| **已有成熟的 ptp_kvm + chrony 方案** | 迁移成本 vs 收益评估 |

### 9.3 共存方案

VMClock 和 kvmclock 可以共存，通过 clocksource rating 选择：

```
Guest 启动
  │
  ├── 检测到 kvmclock (rating 400/299)
  │   └── KVM 半虚拟化，传统方案
  │
  ├── 检测到 VMClock (假设 rating 450?)
  │   └── 更精确，有误差界，迁移友好
  │
  └── clocksource 子系统选 rating 高者
```

> **注意**：VMClock 的 rating 值取决于未来内核补丁的具体设定。由于 VMClock 提供了
> 更多功能（误差界、迁移无缝），预期 rating 会高于 kvmclock。

---

## §10 技术总结

### VMClock 解决的三个核心问题

```
┌──────────────────────────────────────────────────────────────────┐
│  问题 1: Steal Time 污染校准                                     │
│  ─────────────────────────────                                   │
│  解法: Hypervisor 代为校时，Guest 不做采样                      │
│  效果: 消除 steal time 对时间精度的影响                         │
│                                                                   │
│  问题 2: N 个 Guest 重复校准同一晶振                            │
│  ────────────────────────────────────                            │
│  解法: Hypervisor 校时一次，所有 Guest 共享结果                 │
│  效果: CPU 开销从 O(N) 降到 O(1)，所有 Guest 时间一致          │
│                                                                   │
│  问题 3: 热迁移后时间断崖                                       │
│  ─────────────────────────                                       │
│  解法: disruption_marker + 参数直接替换                         │
│  效果: 迁移后 Guest 立即获得精确时间，零收敛等待                │
└──────────────────────────────────────────────────────────────────┘
```

### VMClock 的设计精髓

1. **关注点分离**：校时是 Hypervisor 的责任（它有外部时钟源），Guest 只需消费结果
2. **显式误差界**：不假装时间是精确的，而是告诉 Guest 误差有多大
3. **标准化发现**：ACPI/DT 设备模型，不与特定 hypervisor 绑定
4. **跨架构**：counter_id 支持 x86 TSC、ARM Arch Timer 等
5. **迁移友好**：disruption_marker 是核心创新，让 Guest 知道何时该信任参数

### 与现有技术的关系

```
                ┌─────────────────────────────────────┐
                │        Guest 时间需求                 │
                └──────────────┬──────────────────────┘
                               │
         ┌─────────────────────┼─────────────────────┐
         │                     │                      │
         ▼                     ▼                      ▼
  ┌─────────────┐     ┌──────────────┐      ┌──────────────┐
  │ kvmclock    │     │ VMClock      │      │ 网络 NTP     │
  │ (KVM 专属)  │     │ (新兴标准)   │      │ (通用兜底)   │
  │             │     │              │      │              │
  │ 高精度      │     │ 高精度       │      │ 毫秒级       │
  │ 无迁移保证  │     │ 迁移友好     │      │ 独立于 VMM   │
  │ 无误差界    │     │ 有误差界     │      │ 受网络影响   │
  └──────┬──────┘     └──────┬───────┘      └──────┬───────┘
         │                    │                      │
         └────────────────────┼──────────────────────┘
                              │
                    ┌─────────▼─────────┐
                    │  chrony / NTPd    │
                    │  (用户态同步)     │
                    └───────────────────┘
```

---

## ✅ 验证清单

完成本文后，确认能回答：
- [ ] 列出 Guest 自主校时方案的三个核心问题
- [ ] 解释 VMClock 的核心设计思想
- [ ] 描述 `vmclock_abi` 的 seqcount 同步机制
- [ ] 推导 VMClock 的时间计算公式 `T = T₁ + P × (C_now - C₁)`
- [ ] 说明 `counter_period_shift` 的作用（提高定点数精度）
- [ ] 对比 VMClock 与 kvmclock 在迁移场景下的行为差异
- [ ] 解释 `disruption_marker` 如何让 Guest 无缝处理迁移
- [ ] 说明 `vm_generation_counter` 与 `disruption_marker` 的区别
- [ ] 列出 VMClock 的 ACPI 发现方式
- [ ] 分析 VMClock 的适用场景与局限

---

## 📚 参考资料

| 资料 | 来源 |
|------|------|
| VMClock 规范 v0.95 | https://david.woodhou.se/VMClock.pdf |
| UAPI.13 VMClock 规范 v1.1 | https://uapi-group.org/specifications/specs/vmclock/ |
| Linux Guest 驱动 | `drivers/ptp/ptp_vmclock.c` (Linux 7.3.0) |
| Firecracker Host 实现 (Phase 1) | `src/vmm/src/devices/acpi/vmclock.rs` |
| Firecracker 测试工具 | `tests/host_tools/vmclock.c` |
| Firecracker 文档 | `docs/snapshotting/snapshot-support.md` |
| Linux PTP vmclock 首次合入 | LKML commit 205032724226, 2024-10-06 |

---

> **文档状态**：本文基于 VMClock 规范 v1.1、Linux 7.3.0 guest 驱动和 Firecracker
> Phase 1 host 实现撰写。VMClock 的完整时间同步功能需要 host 侧 Phase 2/3 实现，
> 以及 KVM/QEMU 的对应支持。建议关注 David Woodhouse 和 Babis Chalios 的后续补丁。
