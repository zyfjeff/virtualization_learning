# 时钟虚拟化

> 基于 Linux 6.12.93 内核源码
>
> 时钟虚拟化是虚拟化中最复杂也最容易被忽视的部分。CPU、内存、中断的虚拟化都有明确的硬件辅助，
> 但时钟虚拟化面临着**没有统一硬件标准**、**多CPU TSC不同步**、**迁移后时间跳变**等独特挑战。

---

## 📋 学习目标

完成本章节后，你应该能够：
1. 画出 x86 平台所有时钟源的演进关系图
2. 理解 KVM 如何虚拟化每种时钟（PIT / APIC Timer / TSC / kvmclock）
3. 掌握 TSC Offset 和 TSC Scaling 在 VMCS 中的工作方式
4. 理解 TSC-deadline 模式为什么是最高效的定时器
5. 解释 Guest 迁移后时间跳变的原因和解决方法

---

## 🕐 硬件时钟源全景

### 为什么 x86 有这么多时钟？

x86 的历史包袱导致了多种时钟共存，每种都有自己的设计目的：

```
┌─────────────────────────────────────────────────────────────────────┐
│                    x86 时钟源演进                                    │
│                                                                     │
│  ① PIT (8254, 1976)                                                │
│     用途: PC兼容性的基础 (BIOS、DOS时代的定时器)                     │
│     频率: 1.193182 MHz (固定)                                       │
│     连接: Channel 0 → IRQ0 → PIC                                    │
│     问题: 只有3个channel, 精度低, 只能产生周期性中断                │
│     状态: 现代系统仍必须模拟 (Guest OS启动依赖)                     │
│                                                                     │
│  ② APIC Timer (Local APIC内部, 多核时代)                           │
│     用途: 每个CPU独立的定时器, 调度/tick/profiling                   │
│     频率: 基于总线频率或TSC (可配置除数)                            │
│     模式: Periodic / One-shot / TSC-deadline                        │
│     优点: 每个CPU独立, 不依赖外部芯片                               │
│                                                                     │
│  ③ TSC (Time Stamp Counter, CPU内部)                                │
│     用途: 高精度时间戳, RDTSC指令读取                                │
│     频率: CPU主频 (每CPU独立, 可能不同步!)                          │
│     优点: 最快 (单条指令, 无VM-Exit)                                │
│     问题: 多CPU不同步, 频率可能变化 (变频CPU)                       │
│     演进: Constant TSC → Invariant TSC → TSC-deadline              │
│                                                                     │
│  ④ HPET (High Precision Event Timer, 2004)                         │
│     用途: 替代PIT+RTC, 提供更高精度                                 │
│     频率: 通常 10-100 MHz                                           │
│     问题: MMIO访问较慢, 需要VM-Exit                                 │
│                                                                     │
│  ⑤ kvmclock (KVM半虚拟化时钟, 2008)                                │
│     用途: 专门为虚拟化设计的高效时钟                                 │
│     原理: Host和Guest共享pvclock结构, Guest直接读内存获取时间       │
│     优点: 无VM-Exit, 精度最高, 支持迁移                            │
│     限制: 仅KVM Guest (需要Guest内核支持)                           │
│                                                                     │
│  ⑥ Hyper-V TSC Page (微软半虚拟化)                                  │
│     用途: Hyper-V虚拟化时钟                                          │
│     原理: 类似kvmclock, 共享时间结构体                              │
└─────────────────────────────────────────────────────────────────────┘
```

### 各时钟源对比

| 时钟源 | 精度 | 速度(读) | VM-Exit? | 迁移友好? | 适用场景 |
|--------|------|---------|----------|----------|---------|
| PIT | ~1μs | IO端口 | ★ 每次读写都Exit | ✓ | 兼容性(启动) |
| APIC Timer | ~μs | MMIO | ★ 配置时Exit | ✓ | 调度tick |
| TSC | ~ns | RDTSC指令 | ✗ 无Exit | ✗ 需同步 | 高精度时间戳 |
| TSC-deadline | ~ns | MSR写 | ✗ 仅触发时 | ✗ | 最高效定时器 |
| HPET | ~ns | MMIO | ★ 每次读写Exit | ✓ | 替代PIT |
| kvmclock | ~ns | 内存读 | ✗ 无Exit | ✓✓ | ★★ 虚拟化首选 |

---

## 🔧 KVM 时钟虚拟化详解

### 1. PIT 虚拟化 (i8254.c)

**硬件原理**: PIT (8254) 有 3 个 Channel：
- Channel 0: 系统定时器 → IRQ0
- Channel 1: DRAM 刷新 (已废弃)
- Channel 2: PC 扬声器

**KVM 模拟** (源码: `arch/x86/kvm/i8254.c`):

```
PIT 虚拟化:

┌─ Guest ──────────────────┐    ┌─ Host (KVM) ──────────────────┐
│                           │    │                                │
│  Guest OS 初始化 PIT:     │    │                                │
│  写IO端口 0x40-0x43       │    │                                │
│  (设置channel/freq/mode)  │    │                                │
│         │                  │    │                                │
│         ▼                  │    │                                │
│  读IO端口 (获取计数值)    │    │                                │
│         │                  │    │                                │
└─────────┼──────────────────┘    │                                │
          │                       │                                │
          │ IO端口读写            │                                │
          │ (每次都是VM-Exit!)    │                                │
          ▼                       │                                │
┌─────────────────────────────────────────────────────────────────┐│
│  KVM PIT 模拟:                                                   ││
│                                                                   ││
│  Guest写 → kvm_pit_ioport_write()                                ││
│    → pit_load_count(): 设置计数值                                ││
│    → 启动 hrtimer 模拟硬件计数                                   ││
│                                                                   ││
│  硬件计数到期 → KVM hrtimer 回调                                 ││
│    → kvm_pit_timer_expired()                                     ││
│    → kvm_set_irq(0) → 投递 IRQ0 到 PIC/IOAPIC                   ││
│    → 注入到 vCPU                                                ││
│                                                                   ││
│  Guest读 → kvm_pit_ioport_read()                                 ││
│    → pit_get_count(): 根据hrtimer计算当前计数值                  ││
│    → 返回模拟的计数值                                            ││
└──────────────────────────────────────────────────────────────────┘│
```

**关键数据结构**:

```c
/* arch/x86/kvm/i8254.h */

/* PIT 单个通道的状态 */
struct kvm_kpit_channel_state {
    u32 count;                  /* 计数值 (初始装载值) */
    u16 latched_count;          /* 锁存的计数值 */
    u8 count_latched;           /* 锁存状态 */
    u8 status_latched;          /* 状态锁存 */
    u8 status_count;            /* 状态/计数 */
    u8 read_state;              /* 读状态 */
    u8 write_state;             /* 写状态 */
    u8 write_latch;             /* 写锁存 */
    u8 rw_mode;                 /* 读/写模式 (LSB/MSB) */
    u8 mode;                    /* 工作模式 (0-5) */
    u8 bcd;                     /* BCD模式 */
    u8 gate;                    /* 门控输入 */
    s64 count_load_time;        /* 计数装载时间 (ns, host时间) */
};

/* PIT 整体 */
struct kvm_pit {
    struct kvm *kvm;
    struct kvm_kpit_state pit_state;    /* PIT 状态 */
    struct hrtimer pit_timer;           /* Host高精度定时器 */
    /* ... */
};
```

**PIT模式 (Mode 2 = Rate Generator)**:
```
    ┌───┐       ┌───┐       ┌───┐
    │   │       │   │       │   │
────┘   └───────┘   └───────┘   └── OUT (IRQ0)
    │←─count─→│←─count─→│
    │←────── period ────→│

count = 初始装载值
period = count / 1193182 (秒)
每次计数到0: OUT产生脉冲 → 触发IRQ0
自动重新装载count, 周期性触发
```

**性能问题**: PIT 每次 IO 端口读写都触发 VM-Exit。一个 Linux Guest 每秒可能有数千次 PIT 访问（调度、jiffies更新），性能开销巨大。

### 2. APIC Timer 虚拟化 (lapic.c)

**硬件原理**: 每个 CPU 的 Local APIC 内部有一个定时器，通过 APIC_LVTT (LVT Timer Register) 配置。

**三种工作模式**:

```
┌─ Periodic 模式 ─────────────────────────────────────────────────┐
│  写 APIC_LVTT: vector=32, mode=periodic                        │
│  写 APIC_TMICT: initial_count = N                              │
│  写 APIC_TDCR: divide_config                                   │
│                                                                 │
│  计数器从N递减到0 → 触发中断 → 自动重新装载N                   │
│  周期 = initial_count × divide_value / bus_frequency            │
│                                                                 │
│  对应Guest: 传统Linux内核的tick (CONFIG_HZ=1000 → 1ms/tick)    │
└─────────────────────────────────────────────────────────────────┘

┌─ One-shot 模式 ─────────────────────────────────────────────────┐
│  写 APIC_LVTT: vector=32, mode=oneshot                         │
│  写 APIC_TMICT: initial_count = N                              │
│                                                                 │
│  计数器从N递减到0 → 触发一次中断 → 停止                        │
│  需要Guest重新设置initial_count才能再次触发                     │
│                                                                 │
│  对应Guest: NO_HZ内核 (tickless), 只在需要时设定时器           │
└─────────────────────────────────────────────────────────────────┘

┌─ TSC-deadline 模式 (最高效!) ───────────────────────────────────┐
│  写 APIC_LVTT: vector=32, mode=tscdeadline                     │
│  写 IA32_TSC_DEADLINE MSR: deadline = 目标TSC值               │
│                                                                 │
│  硬件持续比较: RDTSC vs IA32_TSC_DEADLINE                     │
│  当 RDTSC >= deadline → 触发中断                              │
│                                                                 │
│  ★ 优点:                                                       │
│    - 不需要写TMICT/除数配置 (减少VM-Exit!)                     │
│    - 基于TSC, 精度最高                                          │
│    - 硬件直接比较, 无hrtimer开销                                │
│    - 可以用VMCS的preemption timer硬件加速                       │
│                                                                 │
│  对应Guest: 现代Linux内核默认使用此模式 (如果CPU支持)          │
└─────────────────────────────────────────────────────────────────┘
```

**KVM APIC Timer 实现**:

```
KVM 模拟 APIC Timer:

Guest写 APIC_TMICT (初始计数值)
  → apic_mmio_write() → VM-Exit
  → kvm_apic_set_reg()
  → start_apic_timer()
    │
    ├── Periodic/One-shot:
    │     计算超时 = now + count × divide / freq
    │     hrtimer_start(&apic->lapic_timer.timer, ...)
    │     → Host内核 hrtimer 到期时:
    │       apic_timer_expired()
    │       → kvm_apic_set_irq() → 注入中断到vCPU
    │
    └── TSC-deadline:
          vmcs_write64(TSC_DEADLINE, deadline)  ← 交给硬件!
          硬件自动比较 TSC vs deadline
          到期时自动触发VM-Exit → KVM注入中断
          ★ 几乎零软件开销!

Guest读 APIC_TMCCT (当前计数值)
  → apic_mmio_read() → VM-Exit
  → 对于TSC-deadline: 从TSC差值计算剩余计数
```

**TSC-deadline 的 VMCS 加速**:

```c
/* arch/x86/kvm/vmx/vmx.c */

/*
 * TSC-deadline 模式的硬件加速:
 *
 * VMCS 有一个 TSC_DEADLINE 字段 (实际上是通过MSR 0x6E0访问)
 * 当Guest写 IA32_TSC_DEADLINE MSR时:
 *   如果 VMCS 中 "use TSC scaling" 启用:
 *     硬件自动将Guest的deadline转换为Host的TSC值
 *     硬件直接比较Host TSC vs 转换后的deadline
 *     到期时自动VM-Exit
 *   否则:
 *     KVM软件模拟: hrtimer
 *
 * 这就是TSC-deadline高效的原因:
 *   写deadline → 直接写VMCS字段 → 硬件自动处理 → 到期VM-Exit
 *   整个过程只有"写MSR"触发一次VM-Exit, 不需要KVM计算定时器
 */
```

### 3. TSC 虚拟化 (x86.c, vmx.c)

TSC 是最复杂的部分。Guest 通过 RDTSC 指令读取时间戳，这条指令不需要 VM-Exit。但问题是多 CPU 的 TSC 可能不同步。

**VMCS 中的 TSC 控制**:

```
VMCS TSC 相关字段:

┌─ TSC_OFFSET ─────────────────────────────────────────────────────┐
│  Guest RDTSC = Host TSC + TSC_OFFSET                            │
│                                                                   │
│  用途: 让Guest看到的时间与Host不同                                │
│    - VM迁移后: 调整offset使Guest时间连续                         │
│    - 多VM之间: 每个VM有不同的offset                               │
│                                                                   │
│  写 VMCS: vmcs_write64(TSC_OFFSET, offset)                      │
│  读 Guest TSC: RDTSC → 硬件自动加上 offset → 返回给Guest       │
└──────────────────────────────────────────────────────────────────┘

┌─ TSC_MULTIPLIER (TSC Scaling) ───────────────────────────────────┐
│  Guest RDTSC = (Host TSC × TSC_MULTIPLIER) + TSC_OFFSET        │
│                                                                   │
│  用途: 让Guest看到不同频率的TSC                                  │
│    - 嵌套虚拟化: L2 Guest需要看到L1的TSC频率                     │
│    - 迁移兼容: 不同Host CPU频率不同, 通过缩放统一               │
│                                                                   │
│  格式: 48位小数 + 整数部分                                       │
│    1.0 = 0x0000000100000000 (不缩放)                            │
│    2.0 = 0x0000000200000000 (2倍速)                              │
│    0.5 = 0x0000000080000000 (半速)                               │
│                                                                   │
│  硬件支持: CPUID.80000007H:EDX[8] = TSC invariant               │
│           CPUID.06H:EAX[2] = TSC scaling support                 │
└──────────────────────────────────────────────────────────────────┘
```

**TSC 同步问题**:

```
问题1: 多CPU TSC 不同步

  CPU-0: RDTSC = 1000000
  CPU-1: RDTSC = 1000020   ← 比CPU-0快了20个tick!
  CPU-2: RDTSC = 999980    ← 比CPU-0慢了20个tick!

  Guest vCPU 在CPU-0和CPU-1之间迁移:
    在CPU-0上: RDTSC = 1000000
    迁移到CPU-1: RDTSC = 1000020  ← 时间突然前进了20!
    如果Guest用TSC做时间差计算, 结果就会出错

  KVM解决: 启动时检查TSC同步
    kvm_arch_check_tsc_migration()
    如果不同步: 标记为 "TSC unstable", 不使用TSC做时钟源


问题2: 变频CPU (Turbo Boost, P-states)

  CPU降频时: TSC频率可能跟着变 (非Invariant TSC)
  CPU-0 全速: TSC = 3 GHz
  CPU-0 降频: TSC = 1.5 GHz  ← 频率变了!

  Guest用TSC计算时间差:
    delta = (TSC2 - TSC1) / 3GHz  ← 假设频率不变
    实际经过时间可能完全不同!

  KVM解决: Invariant TSC (CPUID.80000007H:EDX[8])
    现代CPU的TSC是恒定频率, 不受变频影响
    但KVM仍需检查: boot_cpu_has(X86_FEATURE_CONSTANT_TSC)


问题3: VM 迁移 (Live Migration)

  迁移前 Host-A: Host TSC = 5000000, Guest sees = 5000000 + offset
  迁移后 Host-B: Host TSC = 3000000, Guest sees = 3000000 + ?

  如果offset不变: Guest sees = 3000000 + 0 = 3000000
  ← Guest时间从5000000突然跳回3000000! 退回了200万tick!

  KVM解决: 迁移后重新计算offset
    new_offset = old_guest_tsc - new_host_tsc
    = 5000000 - 3000000 = 2000000
    vmcs_write64(TSC_OFFSET, new_offset)
    → Guest sees = 3000000 + 2000000 = 5000000  ✓ 连续!
```

**KVM TSC 虚拟化关键函数**:

```c
/* arch/x86/kvm/vmx/vmx.c */

/* Guest RDTSC = (Host TSC × multiplier) + offset
 *
 * 写入VMCS:
 */
void vmx_write_tsc_offset(struct kvm_vcpu *vcpu)
{
    vmcs_write64(TSC_OFFSET, vcpu->arch.tsc_offset);
}

void vmx_write_tsc_multiplier(struct kvm_vcpu *vcpu)
{
    vmcs_write64(TSC_MULTIPLIER, vcpu->arch.tsc_scaling_ratio);
}

/* arch/x86/kvm/x86.c */

/*
 * KVM_SET_TSC_OFFSET: 用户空间设置TSC偏移
 * kvm_synchronize_tsc(): 同步所有vCPU的TSC
 *   → 确保同一VM的所有vCPU看到一致的TSC
 *   → 在vCPU创建和迁移时调用
 */
```

### 4. kvmclock — 半虚拟化时钟 (x86.c)

kvmclock 是 KVM 为虚拟化专门设计的高效时钟。它使用 **pvclock** 协议，Guest 和 Host 共享一块内存区域，Guest 直接读内存获取时间，无需任何 VM-Exit。

```
kvmclock 原理:

┌─ Host (KVM) ──────────────────────────────────────────────────┐
│                                                                 │
│  kvm_write_system_time(vcpu, system_time_msr)                  │
│    → 在Guest物理地址 system_time_msr 处写入:                   │
│                                                                 │
│    struct pvclock_vcpu_time_info {                              │
│        u32 version;         ← 版本号 (奇数=更新中)             │
│        u32 flags;           ← KVM_CLOCK_TSC_STABLE等           │
│        u64 tsc_timestamp;   ← 写结构时的Host TSC              │
│        u64 system_time;     ← 写结构时的Host monotonic时间     │
│        u32 tsc_to_system_mul; ← TSC→时间的缩放因子            │
│        s8  tsc_shift;         ← TSC移位调整                    │
│    };                                                           │
│                                                                 │
│  Host定期更新 (每秒或vCPU调度时):                               │
│    version++ (变奇数)                                           │
│    tsc_timestamp = 当前TSC                                      │
│    system_time = 当前monotonic时间                              │
│    计算新的 mul/shift                                            │
│    version++ (变偶数, 表示更新完成)                             │
└─────────────────────────────────────────────────────────────────┘

┌─ Guest (Linux) ───────────────────────────────────────────────┐
│                                                                 │
│  Guest 需要当前时间:                                           │
│    1. 直接读共享内存中的 pvclock_vcpu_time_info                │
│    2. current_tsc = RDTSC                                      │
│    3. delta_tsc = current_tsc - pvti.tsc_timestamp             │
│    4. delta_ns = delta_tsc × mul >> (22 + shift)              │
│    5. current_ns = pvti.system_time + delta_ns                │
│                                                                 │
│    ★ 整个过程: 0次 VM-Exit! 纯内存读 + 算术运算              │
│    ★ 精度: 纳秒级                                               │
│    ★ 支持迁移: Host更新pvti即可, Guest无需感知                 │
│                                                                 │
│  Guest 需要wall clock (年月日时分秒):                          │
│    读另一个共享结构: struct pvclock_wall_clock                  │
│    { version, sec, nsec }                                      │
└─────────────────────────────────────────────────────────────────┘
```

**kvmclock_offset 的核心作用** (事实核查补充):

```c
/* arch/x86/kvm/x86.c:7047 */

/*
 * kvmclock_offset 是 guest kvmclock 相对 host 的那个 offset
 * KVM_SET_CLOCK 干的唯一事情就是设它
 *
 * 关键: KVM_SET_CLOCK 的完整语义 (Linux 5.16+)
 */
case KVM_SET_CLOCK:
    if (data.flags & ~KVM_CLOCK_VALID_FLAGS)
        return -EINVAL;

    /* KVM_CLOCK_REALTIME 标志处理 (Firecracker PR #5809 修复的问题) */
    if (data.flags & KVM_CLOCK_REALTIME) {
        u64 now_real_ns = ktime_get_real_ns();
        
        /* 避免 kvmclock 回退 */
        if (now_real_ns > data.realtime)
            data.clock += now_real_ns - data.realtime;
    }

    if (ka->use_master_clock)
        now_raw_ns = ka->master_kernel_ns;
    else
        now_raw_ns = get_kvmclock_base_ns();
    
    /* 核心: 设置 kvmclock_offset */
    ka->kvmclock_offset = data.clock - now_raw_ns;

/*
 * ⚠️ 陷阱: Linux 5.16+ 引入 KVM_CLOCK_REALTIME
 * 
 * Firecracker PR #5809 的问题:
 *   - GET_CLOCK 结果包含 KVM_CLOCK_REALTIME 标志
 *   - 恢复快照时原样 SET_CLOCK 回去
 *   - KVM 会将"快照到恢复之间流逝的墙钟"加到 kvmclock
 *   - Guest 的 CLOCK_MONOTONIC 凭空向前跳!
 *
 * 修复: 恢复时 clock.flags = 0, 不信任快照中的 flags
 */
```

**vDSO 加速机制** (事实核查补充):

```
vDSO (Virtual Dynamic Shared Object) 是性能关键!

┌─ 无 vDSO (纯 syscall) ─────────────────────────────────────┐
│  clock_gettime()                                            │
│    → syscall clock_gettime                                   │
│    → 陷入内核                                                │
│    → 读取 clocksource (可能触发 VM-Exit!)                   │
│    → 返回用户态                                              │
│  开销: ~100-500 ns                                          │
└──────────────────────────────────────────────────────────────┘

┌─ 有 vDSO ───────────────────────────────────────────────────┐
│  clock_gettime()                                            │
│    → vDSO 函数 (无 syscall!)                                 │
│    → 读取 pvclock 共享页 (内存读)                           │
│    → 计算: ns = system_time + scale(rdtsc() - timestamp)    │
│    → 返回时间                                                │
│  开销: ~20-30 ns (纯用户态!)                                │
└──────────────────────────────────────────────────────────────┘

vDSO 的前提:
  · clocksource 必须支持 vDSO (kvm-clock, tsc 都支持)
  · pvclock 页必须有 PVCLOCK_TSC_STABLE_BIT 标志
  · 如果 TSC 不稳定, vDSO 会退化为 syscall

性能差异: 10-100 倍!
```

**kvmclock 的优势**:

```
┌──────────────────────────────────────────────────────────────────┐
│              时钟读取性能对比 (越低越好)                          │
│                                                                   │
│  PIT:       ~1000 ns (IO端口 + VM-Exit × 2)                     │
│  HPET:      ~500 ns  (MMIO + VM-Exit × 2)                       │
│  APIC:      ~300 ns  (MSR/内存 + VM-Exit)                       │
│  TSC:       ~20 ns   (RDTSC指令, 无VM-Exit)                     │
│  kvmclock:  ~30 ns   (内存读 + 计算, 无VM-Exit)                 │
│                                                                   │
│  kvmclock 几乎和 TSC 一样快, 但:                                 │
│    - 支持迁移 (TSC不行)                                          │
│    - 返回绝对时间 (TSC只是tick数)                                │
│    - 多CPU一致 (TSC可能不同步)                                   │
│                                                                   │
│  → 现代Linux Guest默认使用kvmclock作为clocksource               │
└──────────────────────────────────────────────────────────────────┘
```

**masterclock 优化** (事实核查补充):

```c
/* arch/x86/kvm/x86.c:3034-3042 */

/*
 * masterclock 是 kvmclock 的重要优化
 * 
 * 问题: 多 vCPU 时, 每个 vCPU 的 pvclock 页独立更新
 *       不同 vCPU 读到的时间可能有微小差异 (纳秒级)
 *       对于需要严格时间一致性的场景 (如数据库), 这是问题
 *
 * 解决: 当条件满足时, 使用全局统一的基准时间
 */

/* 触发条件 */
ka->use_master_clock = host_tsc_clocksource &&    /* host 用 TSC 做 clocksource */
                       vcpus_matched &&            /* 所有 vCPU 的 TSC 已对齐 */
                       !ka->backwards_tsc_observed && /* 未观察到 TSC 倒退 */
                       ...;

if (ka->use_master_clock) {
    /* 存下全局快照 */
    kvm_get_time_and_clockread(&ka->master_kernel_ns, &ka->master_cycle_now);
}

/* 使用 masterclock 时 */
if (ka->use_master_clock)
    now_raw_ns = ka->master_kernel_ns;  /* 全局基准 */
else
    now_raw_ns = get_kvmclock_base_ns(); /* 每个 vCPU 独立计算 */

/*
 * masterclock 的好处:
 *   · 所有 vCPU 看到完全一致的时间
 *   · KVM_GET_CLOCK 可以置 KVM_CLOCK_TSC_STABLE 标志
 *   · pvclock 页有 PVCLOCK_TSC_STABLE_BIT → guest 走 vDSO 无锁快速路径
 *
 * masterclock 失效的情况:
 *   · host TSC 不稳定 (非 invariant TSC)
 *   · vCPU 的 TSC 未对齐
 *   · 观察到 TSC 倒退
 *   · 失效后 guest 每次读都要做跨 CPU 一致性保护, 慢得多
 */
```

**ptp_kvm 深度解析** (事实核查补充):

```
ptp_kvm 名字有误导性 —— 它不跑任何 IEEE 1588 报文!

┌─ 传统 PTP (Precision Time Protocol) ──────────────────────────┐
│  网络 PTP:                                                     │
│    · ptp4l 运行 IEEE 1588 协议                                 │
│    · 时间戳在网卡 PHY 层打 (硬件时间戳)                       │
│    · 精度: 亚微秒级                                            │
│    · 需要网络设备支持                                          │
│    · 需要网络可达                                              │
└──────────────────────────────────────────────────────────────────┘

┌─ ptp_kvm 的实现 ──────────────────────────────────────────────┐
│  ptp_kvm 不跑 PTP 协议, 而是:                                  │
│    · 借用 PHC (PTP Hardware Clock) 内核抽象                    │
│    · 把 KVM 的 host 时间包装成 /dev/ptp0 设备                  │
│    · 让 chrony 等工具能直接用                                  │
│                                                                  │
│  底层是一次 hypercall:                                         │
│    KVM_HC_CLOCK_PAIRING (arch/x86/kvm/x86.c:9928)             │
│                                                                  │
│    struct kvm_clock_pairing {                                   │
│        u64 sec;      /* host realtime 秒 */                    │
│        u64 nsec;     /* host realtime 纳秒 */                  │
│        u64 tsc;      /* 对应的 guest TSC */                    │
│        u32 flags;                                              │
│    };                                                          │
│                                                                  │
│  Host 侧实现 (x86.c:9946-9951):                                │
│    if (!kvm_get_walltime_and_clockread(&ts, &cycle))           │
│        return -KVM_EOPNOTSUPP;                                 │
│                                                                  │
│    clock_pairing.sec = ts.tv_sec;                              │
│    clock_pairing.nsec = ts.tv_nsec;                            │
│    clock_pairing.tsc = kvm_read_l1_tsc(vcpu, cycle);          │
│                                                                  │
│  精髓:                                                         │
│    · 三元组: (host realtime sec, nsec, 对应的 guest TSC)       │
│    · host 在同一瞬间原子地读出墙钟和计数器                     │
│    · guest 拿到后可以精确配对 "host 墙钟" 和 "自己的 TSC"     │
│    · 没有网络往返、没有抖动、没有不确定的延迟                  │
│    · 这是网络 PTP 都做不到的精度!                              │
└──────────────────────────────────────────────────────────────────┘

┌─ 部署配置 ────────────────────────────────────────────────────┐
│  Guest 侧:                                                     │
│    modprobe ptp_kvm            # 出现 /dev/ptp0                │
│                                                                  │
│    # /etc/chrony.conf                                            │
│    refclock PHC /dev/ptp0 poll 0 dpoll -2 stratum 1            │
│    makestep 1.0 -1             # 偏差 >1s 时无条件 step        │
│                                                                  │
│  为什么精度高于网络 PTP:                                        │
│    · 网络 PTP: 需要估计网络延迟, 有抖动                        │
│    · ptp_kvm: hypercall 是确定性的, 无抖动                      │
│    · 精度: 亚微秒级 vs 毫秒级                                   │
└──────────────────────────────────────────────────────────────────┘

┌─ PVCLOCK_GUEST_STOPPED 机制 ──────────────────────────────────┐
│  专为 VM 暂停设计的机制:                                       │
│                                                                  │
│  问题: VM 被 VMM 暂停时, guest 内核不知情                     │
│        MONOTONIC 和 BOOTTIME 都不前进                          │
│        从 guest 视角就是 "CPU 被抢了很久"                      │
│                                                                  │
│  解决:                                                         │
│    · VMM 恢复 VM 后调用 KVM_KVMCLOCK_CTRL (x86.c:5100)        │
│    · KVM 置一个 request                                        │
│    · 下次刷 pvclock 页时打上 PVCLOCK_GUEST_STOPPED 标志       │
│    · guest 读到这个 flag 会执行 pvclock_touch_watchdogs():     │
│        - touch softlockup watchdog                            │
│        - reset RCU stall detector                             │
│        - reset hung task detector                              │
│    · 然后清 flag                                                │
│                                                                  │
│  这才是 "告诉 guest 你被暂停过" 的正确方式:                    │
│    · 不是伪造时间                                               │
│    · 而是告知事实并让它宽恕这段空白                            │
└──────────────────────────────────────────────────────────────────┘
```

### 5. Guest 视角的时钟层次

```
Guest Linux 内核的时钟层次:

┌─ clocksource (时间源, 读取当前时间) ──────────────────────────┐
│  优先级从高到低:                                               │
│    1. kvm-clock        ← 半虚拟化, 最快, Guest默认选择        │
│    2. tsc              ← 如果CPU有Invariant TSC               │
│    3. hpet             ← HPET                                 │
│    4. acpi_pm          ← ACPI PM timer                        │
│    5. pit              ← PIT (最后选择, 最慢)                 │
│                                                                │
│  查看: cat /sys/devices/system/clocksource/clocksource0/       │
│         current_clocksource                                    │
└────────────────────────────────────────────────────────────────┘

┌─ clockevent (事件源, 设置定时器) ─────────────────────────────┐
│  优先级从高到低:                                               │
│    1. lapic            ← APIC Timer (TSC-deadline模式最优)    │
│    2. hpet             ← HPET                                 │
│    3. pit              ← PIT                                  │
│                                                                │
│  查看: cat /sys/devices/system/clockevents/clockevent0/        │
│         current_device                                         │
└────────────────────────────────────────────────────────────────┘

KVM 建议:
  clocksource = kvm-clock   (共享内存, 无VM-Exit)
  clockevent  = lapic       (TSC-deadline, 硬件加速)
```

---

## 🔬 源码阅读顺序

```
第1步: 理解PIT模拟
  arch/x86/kvm/i8254.h         ← 数据结构 (kvm_pit, kvm_kpit_channel_state)
  arch/x86/kvm/i8254.c         ← PIT完整模拟

第2步: 理解APIC Timer
  arch/x86/kvm/lapic.c         ← 搜索 "timer" 相关函数
    start_apic_timer()          ← 定时器启动
    apic_timer_expired()        ← 定时器到期
    apic_mmio_read/write        ← Guest读写触发

第3步: 理解TSC虚拟化
  arch/x86/kvm/vmx/vmx.c
    vmx_write_tsc_offset()      ← TSC偏移写入VMCS
    vmx_write_tsc_multiplier()  ← TSC缩放写入VMCS
  arch/x86/kvm/x86.c
    kvm_synchronize_tsc()       ← 多vCPU TSC同步

第4步: 理解kvmclock
  arch/x86/kvm/x86.c
    kvm_write_system_time()     ← 写pvclock_vcpu_time_info
    kvm_write_wall_clock()      ← 写pvclock_wall_clock
    kvm_guest_time_update()     ← 更新时间信息

第5步: 理解Timer advance (高级优化)
  arch/x86/kvm/lapic.c
    lapic_timer_advance_ns      ← 模块参数
    adjust_timer_advance_ns()   ← 动态调整提前量
```

---

## 🔬 实践练习

### 练习1: 查看Guest使用的时钟源

```bash
# 在Guest内执行:
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
# 预期: kvm-clock

cat /sys/devices/system/clocksource/clocksource0/available_clocksource
# 预期: kvm-clock tsc hpet acpi_pm

cat /sys/devices/system/clockevents/clockevent0/current_device
# 预期: lapic

# 查看TSC特征:
grep -E "constant_tsc|tsc_deadline|tsc_reliable" /proc/cpuinfo
```

### 练习2: 观察 pvclock 共享页

```bash
# 在 Host 上查看 pvclock 页的 GPA
grep -E "pvclock|system_time" /sys/kernel/debug/kvm/*/vcpu* 2>/dev/null | head -10

# 在 Guest 内查看 pvclock 页内容
# 需要 root 权限和 debugfs
cat /sys/kernel/debug/pvclock 2>/dev/null || echo "pvclock debugfs not available"

# 使用 perf 观察 vDSO 调用
perf record -g clock_gettime sleep 1
perf report | grep -A 5 "clock_gettime"
```

### 练习3: 对比不同时钟源的性能

```bash
# 在Guest内安装 latencytest 或 cyclictest
# 使用默认kvm-clock:
cyclictest -t1 -p 80 -n -i 1000 -l 10000 -q

# 切换到TSC:
echo tsc > /sys/devices/system/clocksource/clocksource0/current_clocksource
cyclictest -t1 -p 80 -n -i 1000 -l 10000 -q

# 切换到PIT (最差):
echo pit > /sys/devices/system/clocksource/clocksource0/current_clocksource
cyclictest -t1 -p 80 -n -i 1000 -l 10000 -q
```

### 练习4: 配置 ptp_kvm + chrony

```bash
# Guest 侧配置:
modprobe ptp_kvm
ls -l /dev/ptp*  # 应该看到 /dev/ptp0

# 安装 chrony
apt-get install chrony  # Debian/Ubuntu
yum install chrony      # RHEL/CentOS

# 配置 /etc/chrony.conf
cat > /etc/chrony.conf << EOF
refclock PHC /dev/ptp0 poll 0 dpoll -2 stratum 1
makestep 1.0 -1
EOF

# 启动 chrony
systemctl restart chrony

# 查看同步状态
chronyc sources
chronyc tracking

# 预期: stratum 1, 精度亚微秒级
```

### 练习5: 观察 kvmclock_offset 变化

```bash
# 在 Host 上观察 kvmclock_offset
# 需要 debugfs 和 root 权限

# 方法 1: 通过 KVM_GET_CLOCK ioctl
cat > /tmp/get_kvmclock.c << 'EOF'
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

int main(int argc, char *argv[]) {
    int fd = open("/dev/kvm", O_RDWR);
    struct kvm_clock_data data;
    
    ioctl(fd, KVM_GET_CLOCK, &data);
    printf("clock: %llu\n", data.clock);
    printf("flags: 0x%x\n", data.flags);
    printf("realtime: %llu\n", data.realtime);
    
    return 0;
}
EOF

gcc -o /tmp/get_kvmclock /tmp/get_kvmclock.c
/tmp/get_kvmclock
```

### 练习6: ftrace 追踪定时器事件

```bash
# 追踪APIC Timer
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_apic/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable

# 观察定时器相关的VM-Exit
cat /sys/kernel/debug/tracing/trace_pipe | grep -E "exit|timer"
```

---

## ✅ 验证清单

完成后确认能回答：
- [ ] 画出 PIT → APIC Timer → TSC-deadline → kvmclock 的演进关系
- [ ] 解释 TSC_OFFSET 和 TSC_MULTIPLIER 在 VMCS 中的作用
- [ ] 说明 TSC-deadline 为什么比 Periodic/One-shot 模式高效
- [ ] 解释 vm 迁移时 Guest 时间连续性的保证机制
- [ ] 说明 kvmclock 的 pvclock 协议如何工作
- [ ] 解释 vDSO 在时钟读取中的性能优势
- [ ] 说明 masterclock 优化的原理和触发条件
- [ ] 解释 ptp_kvm 的实现原理 (hypercall 三元组)
- [ ] 说明 PVCLOCK_GUEST_STOPPED 机制的作用
- [ ] 解释 Firecracker PR #5809 的问题和修复 (KVM_CLOCK_REALTIME)
- [ ] 解释为什么现代 Linux Guest 默认使用 kvm-clock + lapic
- [ ] 列出 Guest 中查看当前时钟源的命令

---

## 🔍 VMM视角对比

### 用户态VMM vs KVM内核态时钟管理

| 方面 | 用户态VMM (QEMU) | KVM内核态 |
|------|------------------|-----------|
| **PIT模拟** | 用户态模拟 | 内核态模拟（i8254.c） |
| **APIC Timer** | 无法直接管理 | 内核态管理 + TSC-deadline硬件加速 |
| **TSC管理** | 通过ioctl设置offset | 直接vmcs_write(TSC_OFFSET) |
| **kvmclock** | 无法实现 | 内核态维护pvclock结构 |

### 关键差异：时钟虚拟化效率

```
用户态VMM:
  Guest读PIT → VM-Exit → 返回用户态 → 计算计数值 → 返回Guest
  每次访问: ~1000ns

KVM内核态:
  Guest读PIT → VM-Exit → 内核态计算 → 返回Guest
  每次访问: ~300ns
  
  Guest读kvmclock → 直接读共享内存 → 计算时间
  每次访问: ~30ns (无VM-Exit!)
```

---

## ⚡ 性能优化技术

### 1. TSC-deadline模式

**问题**：Periodic/One-shot模式需要写TMICT和除数，触发VM-Exit

**解决**：使用TSC-deadline，硬件自动比较TSC和deadline

```c
/* vmx.c 中配置 */
/* Guest写IA32_TSC_DEADLINE MSR */
if (mode == TSC_DEADLINE) {
    vmcs_write64(TSC_DEADLINE, deadline);
    /* 硬件自动比较，到期时VM-Exit */
}
```

**效果**：
- 减少定时器相关VM-Exit 90%
- 中断延迟降低到~20ns

### 2. Timer Advance

**问题**：定时器到期时vCPU可能不在运行状态

**解决**：提前通知vCPU，减少延迟

```c
/* lapic.c 中实现 */
/* 提前 lapic_timer_advance_ns 通知vCPU */
if (timer_advance_ns > 0) {
    advance_deadline = deadline - timer_advance_ns;
    kvm_vcpu_kick(vcpu);
}
```

**配置**：
```bash
# 查看当前值
cat /sys/module/kvm/parameters/lapic_timer_advance_ns
# 默认: 1000 (1μs)

# 调优
echo 2000 > /sys/module/kvm/parameters/lapic_timer_advance_ns
```

**效果**：
- 减少定时器延迟
- 实时应用性能提升

### 3. TSC同步

**问题**：多pCPU的TSC不同步

**解决**：启动时检查TSC同步，标记为unstable

```c
/* x86.c 中实现 */
kvm_arch_check_tsc_migration()
{
    if (tsc_unstable) {
        /* 不使用TSC作为时钟源 */
        kvm->arch.no_tsc_offset = true;
    }
}
```

**检查**：
```bash
# Guest中查看
dmesg | grep -i tsc
# 如果看到"TSC unstable"，说明TSC不同步
```

---

## ⚠️ 常见陷阱

### 陷阱1：TSC不同步

**场景**：Guest在不同pCPU间迁移时时间跳变

**症状**：Guest内核日志显示"time jump"

**原因**：多pCPU的TSC不同步

**解决**：
```bash
# Host中检查TSC同步
dmesg | grep -i "tsc.*unstable"

# 如果TSC不稳定，使用kvmclock
# Guest内核参数
echo "clocksource=kvm-clock" >> /etc/default/grub
update-grub && reboot
```

### 陷阱2：kvmclock未启用

**场景**：Guest使用PIT或HPET，性能差

**症状**：`cyclictest`显示高延迟

**原因**：kvmclock未启用

**解决**：
```bash
# Guest中检查
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
# 应该是: kvm-clock

# 如果不是，手动切换
echo kvm-clock > /sys/devices/system/clocksource/clocksource0/current_clocksource

# 永久生效
echo "clocksource=kvm-clock" >> /etc/default/grub
update-grub && reboot
```

### 陷阱3：VM迁移后时间跳变

**场景**：VM热迁移后Guest时间回退

**症状**：Guest日志显示"time went backwards"

**原因**：TSC offset未正确更新

**解决**：
```c
// kvm_arch_vcpu_load() 中更新
if (vcpu->cpu != old_cpu) {
    /* 重新计算TSC offset */
    new_offset = guest_tsc - host_tsc;
    vmcs_write64(TSC_OFFSET, new_offset);
}
```

### 陷阱4：Timer Advance设置不当

**场景**：定时器延迟不稳定

**症状**：`cyclictest`显示延迟抖动

**原因**：`lapic_timer_advance_ns`设置不当

**解决**：
```bash
# 调优
# 如果延迟高，增大advance
echo 5000 > /sys/module/kvm/parameters/lapic_timer_advance_ns

# 如果CPU占用高，减小advance
echo 500 > /sys/module/kvm/parameters/lapic_timer_advance_ns
```
