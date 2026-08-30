# 时钟虚拟化

> 基于 Linux 6.12.93 内核源码
>
> 时钟虚拟化是虚拟化中最复杂也最容易被忽视的部分。CPU、内存、中断的虚拟化都有明确的硬件辅助，
> 但时钟虚拟化面临着**没有统一硬件标准**、**多CPU TSC不同步**、**迁移后时间跳变**等独特挑战。

---

## 📂 本章文件

| 文件 | 内容 |
|------|------|
| `README.md` | 本文件：概念 → 硬件时钟源全景 → KVM 各时钟虚拟化详解 → 实践练习 → 陷阱 |
| [`annotations.md`](annotations.md) | 源码精读：PIT / APIC Timer / TSC / kvmclock / Timer Advance 关键代码逐段注释（引用均逐条核对） |
| [`corrections.md`](corrections.md) | 勘误：对本章旧版（及写作过程中）发现的错误的逐条更正与源码依据 |
| [`practice/`](practice/README.md) | 动手实验：3 个基于 KVM API 的 C 程序（TSC scaling / kvmclock / LAPIC Timer） |

---

## 📋 学习目标

完成本章节后，你应该能够：
1. 区分 Linux 各类时钟（REALTIME / MONOTONIC / BOOTTIME / MONOTONIC_RAW）及其受时间调整的影响
2. 区分 **clocksource 与 clockevent** 两个子系统，解释为什么 TSC 是前者、
   TSC-deadline（LAPIC timer 的一种模式）是后者
3. 画出 x86 平台时钟设备的演进与归属关系
4. 理解 KVM 如何虚拟化每种时钟（PIT / APIC Timer / TSC / kvmclock）
5. 掌握 TSC Offset 和 TSC Scaling 在 VMCS 中的工作方式
6. 理解 TSC-deadline 为什么是最高效的定时器（从 clockevent 编程成本角度）
7. 对比 `tsc` 与 `kvm-clock` 两个 clocksource 的差异、优缺点与选型逻辑（rating 让位）
8. 解释 VM 启动时三个时间基准（kvmclock 纪元 / guest TSC / masterclock）的首次同步
9. 解释 PTP KVM 如何实现亚微秒级墙上时间同步
10. 解释 Guest 迁移后时间跳变的原因和解决方法

---

## 🕰️ Linux 时钟系统基本概念

> 理解时钟虚拟化之前，必须先理解 Linux 内核管理时间的数据结构。
> 所有 KVM 时钟机制（kvmclock、wall_clock、ptp_kvm）都是对这套体系在虚拟化场景下的映射。

### 内核核心数据结构：`struct timekeeper`

```c
/* kernel/time/timekeeping.c (简化) */
struct timekeeper {
    /* 共享同一个 clocksource (通常是 TSC) */
    struct tk_read_base tkr_mono;   /* 用于 REALTIME + MONOTONIC */
    struct tk_read_base tkr_raw;    /* 用于 MONOTONIC_RAW */

    u64         xtime_sec;          /* REALTIME 的秒部分 */
    struct timespec64 wall_to_monotonic; /* REALTIME 与 MONOTONIC 的偏移 */

    ktime_t     offs_real;          /* = -wall_to_monotonic */
    ktime_t     offs_mono;          /* = 0 (monotonic 从 boot 起算) */
    ktime_t     offs_boot;          /* 挂起时间累积 */
    ktime_t     offs_tai;           /* = offs_real + tai_offset */
};
```

### 基础概念：cycles、mult、shift、NTP slew

> 在理解各时钟的关系之前，需要先搞清楚几个基础概念。

#### cycles 是什么？

cycles 是 **时钟硬件计数器的原始读数**，是一个纯数字，没有单位。

```
┌─ TSC (Time Stamp Counter) ───────────────────────────────────┐
│                                                                │
│  TSC 是 CPU 内部的一个 64 位计数器:                           │
│    · 每个 CPU 周期 +1 (或每总线周期 +1，取决于 CPU 架构)     │
│    · RDTSC 指令读取 → 得到一个 64 位数字                     │
│    · 这个数字就是 "cycles"                                   │
│                                                                │
│  示例:                                                        │
│    CPU 频率 3 GHz → 每秒 30 亿个周期                         │
│    RDTSC 读到 3000000000 → 说明 CPU 启动后经过了 1 秒        │
│    RDTSC 读到 6000000000 → 说明经过了 2 秒                   │
│                                                                │
│  关键: cycles 本身不是时间，只是 "计数"                      │
│       需要知道频率才能换算成时间:                             │
│         时间 (秒) = cycles / 频率 (Hz)                       │
│         时间 (纳秒) = cycles × 10^9 / 频率                   │
└────────────────────────────────────────────────────────────────┘
```

#### cycles 与 CPU 频率的关系

```
┌─ 历史演进 ───────────────────────────────────────────────────┐
│                                                                │
│  早期 CPU:                                                    │
│    TSC 频率 = CPU 核心频率                                    │
│    CPU 变频 (Turbo Boost / P-states) → TSC 频率也变          │
│    → cycles 不能直接换算时间，因为频率在变！                 │
│                                                                │
│  现代 CPU (Invariant TSC):                                   │
│    TSC 频率固定，不受 CPU 变频影响                           │
│    通常 = CPU 基础频率 (非 Turbo)                            │
│    CPUID.80000007H:EDX[8] = 1 表示支持 Invariant TSC         │
│                                                                │
│  KVM 虚拟化场景:                                              │
│    Guest 的 TSC 由 Host TSC 派生:                             │
│      guest_cycles = host_cycles × multiplier + offset         │
│    multiplier 用于跨频率迁移 (见 §3 TSC 虚拟化)              │
└────────────────────────────────────────────────────────────────┘
```

#### mult 和 shift 是什么？

mult 和 shift 是内核用来**快速把 cycles 换算成纳秒**的整数参数，避免浮点运算。

```
┌─ 公式 ───────────────────────────────────────────────────────┐
│                                                                │
│  纳秒 = cycles × mult >> shift                               │
│                                                                │
│  其中:                                                        │
│    mult:  乘数 (32 位或 64 位整数)                           │
│    shift: 右移位数 (0-31)                                    │
│    >>:    右移运算 (等价于除以 2^shift)                      │
│                                                                │
│  本质: 用 "整数乘法 + 位移" 逼近 "乘以小数"                │
└────────────────────────────────────────────────────────────────┘

┌─ 举例 ───────────────────────────────────────────────────────┐
│                                                                │
│  假设: CPU 频率 = 2.667 GHz                                  │
│  目标: cycles → 纳秒                                         │
│                                                                │
│  精确公式:                                                    │
│    ns = cycles × (10^9 / 2.667×10^9)                         │
│       = cycles × 0.37509...                                  │
│                                                                │
│  用 mult/shift 逼近:                                          │
│    选择 shift = 10 (即除以 1024)                             │
│    mult = 0.37509... × 1024 ≈ 384                            │
│                                                                │
│    ns = cycles × 384 >> 10                                   │
│       = cycles × 384 / 1024                                  │
│       = cycles × 0.375                                       │
│                                                                │
│  误差: 0.375 vs 0.37509... ≈ 0.02%                          │
│  内核会选更精确的 mult/shift 组合，误差 < 1 ppm             │
└────────────────────────────────────────────────────────────────┘

┌─ 为什么不用浮点？───────────────────────────────────────────┐
│                                                                │
│  · 内核很多场景不能用浮点 (中断上下文、vDSO 用户态)        │
│  · 整数乘加 + 位移 = 几条指令，纳秒级                      │
│  · 浮点除法 = 几十条指令，慢 10-100 倍                       │
│                                                                │
│  vDSO 中读一次的代码 (简化):                                 │
│    cycles = rdtsc();                                          │
│    delta = cycles - last_cycles;                             │
│    ns = delta * mult >> shift;  // 一条 imul + 一条 shr      │
│    return last_ns + ns;                                       │
└────────────────────────────────────────────────────────────────┘
```

#### NTP slew 是什么？

NTP 有两种方式让系统时间与真实时间同步：**step**（跳变）和 **slew**（频率微调）。

```
┌─ NTP step (跳变) ───────────────────────────────────────────┐
│                                                                │
│  直接把时钟拨到正确时间:                                     │
│    系统时间 12:00:00                                         │
│    真实时间 12:00:05                                         │
│    → settimeofday() 直接把系统时间改成 12:00:05             │
│                                                                │
│  特点: 立即生效，但时间线有"断层"                           │
│  影响: 只影响 CLOCK_REALTIME                                │
│        CLOCK_MONOTONIC 永不跳变 (设计保证)                  │
└────────────────────────────────────────────────────────────────┘

┌─ NTP slew (频率微调) ──────────────────────────────────────┐
│                                                                │
│  稍微加快/减慢时钟频率，慢慢追到正确时间:                   │
│    系统时间 12:00:00                                         │
│    真实时间 12:00:05                                         │
│    偏差 5 秒                                                 │
│    → adjtimex() 把时钟频率加快 0.5%                         │
│    → 大约 1000 秒后，系统时间追上真实时间                  │
│                                                                │
│  特点: 时间线连续，但频率暂时不准                           │
│  影响: 影响所有用 tkr_mono 的时钟                           │
│        CLOCK_REALTIME / MONOTONIC / BOOTTIME 都受影响        │
│        (因为它们共享同一个 mult)                             │
│        CLOCK_MONOTONIC_RAW 不受影响 (用独立的 raw_mult)     │
└────────────────────────────────────────────────────────────────┘

┌─ 实现原理 ─────────────────────────────────────────────────┐
│                                                                │
│  NTP slew 的本质是修改 timekeeper 的 mult:                  │
│                                                                │
│    正常: mult = 384 (对应 2.667 GHz)                        │
│    slew: mult = 385 (加快 0.26%)                            │
│                                                                │
│    同样 cycles = 1000000:                                   │
│      正常: ns = 1000000 × 384 >> 10 = 375000 ns            │
│      slew: ns = 1000000 × 385 >> 10 = 375976 ns            │
│              ↑ 多了 976 ns，相当于时钟 "走得快了一点"       │
│                                                                │
│  这就是为什么 MONOTONIC 也受 NTP slew 影响:                │
│    · MONOTONIC 和 REALTIME 用同一个 tkr_mono.mult          │
│    · slew 改 mult → 两个时钟一起变快/慢                    │
│    · MONOTONIC 只是 "不回退"，但速率会变                   │
└────────────────────────────────────────────────────────────────┘
```

### 各时钟的关系

> 所有时钟都从同一个 clocksource（TSC）读取 cycles，区别只在"如何换算"和"从何时起算"。
> 理解各时钟的关键是搞清楚两个问题：
> 1. 从哪个时间点开始计数？
> 2. 是否包含"挂起时间"？—— 注意区分两种完全不同的"挂起"场景。

#### 三层结构

```
┌─────────────────────────────────────────────────────────────────┐
│  第1层: 硬件层 — clocksource                                    │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  TSC (Time Stamp Counter): 64位计数器, 只增不减         │   │
│  │  读取: RDTSC 指令 → 得到 cycles (纯数字, 无单位)        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                          ↓ cycles                              │
├─────────────────────────────────────────────────────────────────┤
│  第2层: 内核换算层 — timekeeper                                 │
│                                                                 │
│  ┌─ tkr_mono (用于 REALTIME / MONOTONIC / BOOTTIME) ──────┐  │
│  │  mult + shift: 将 cycles 换算成纳秒                     │  │
│  │  受 NTP slew 影响 (mult 会被微调)                       │  │
│  └─────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌─ tkr_raw (用于 MONOTONIC_RAW) ─────────────────────────┐  │
│  │  raw_mult: 将 cycles 换算成纳秒                         │  │
│  │  不受 NTP 影响 (纯硬件频率)                             │  │
│  └─────────────────────────────────────────────────────────┘  │
│                          ↓ 纳秒                                │
├─────────────────────────────────────────────────────────────────┤
│  第3层: 用户态接口 — clockid                                    │
│                                                                 │
│  各时钟 = 纳秒 + 偏移量 (区别只在"起点"和"挂起时间的定义")   │
└─────────────────────────────────────────────────────────────────┘
```

#### 各时钟的起点与偏移

```
时间轴示意:

1970 ─────────── boot ─────────── suspend ──────▶ now
  │                │                  │             │
  ▼                ▼                  ▼             ▼

REALTIME:  ───────┼──────────────────┼──────────────  (从 1970 起算, 可跳变)
                  │                  │
MONOTONIC: ───────┴──────────────────┴──────────────  (从 boot 起算, 不回退)

MONO_RAW:  ───────┴──────────────────┴──────────────  (同上, 但不受 NTP slew)

BOOTTIME:  ───────┴──────────────────┼──────────────  (从 boot 起算, + 挂起时间)
                                     ↑
                              只包含 "Guest 发起的挂起"
                              (如 suspend-to-RAM)
```

#### 计算公式

```c
// 统一形式: clock = cycles × mult >> shift + offset

CLOCK_REALTIME      = cycles × tkr_mono.mult >> shift + offs_real
CLOCK_MONOTONIC     = cycles × tkr_mono.mult >> shift
CLOCK_MONOTONIC_RAW = cycles × tkr_raw.mult >> shift    // 不受 NTP slew
CLOCK_BOOTTIME      = cycles × tkr_mono.mult >> shift + offs_boot
                    = CLOCK_MONOTONIC + 挂起时间
```

#### 关键区分：两种"挂起"场景

"挂起时间"这个词在不同场景下含义完全不同，这是最容易混淆的地方：

```
┌─ 场景 A: Guest 发起的挂起 (suspend-to-RAM / suspend-to-disk) ──────┐
│                                                                      │
│  发起者: Guest 内核 (echo mem > /sys/power/state)                   │
│  Guest 是否知情: ✓ 知道 (主动发起)                                  │
│  期间谁在流逝: Host 可能也挂起, 也可能运行其他 VM                   │
│                                                                      │
│  对 Guest 时钟的影响:                                               │
│    CLOCK_MONOTONIC  不推进 (设计如此, 挂起不算"运行时间")          │
│    CLOCK_BOOTTIME   推进   (包含挂起时长)                           │
│    CLOCK_REALTIME   不推进 (恢复后需要 NTP 校准)                    │
│                                                                      │
│  典型场景: 笔记本合盖, 服务器节能                                  │
└──────────────────────────────────────────────────────────────────────┘

┌─ 场景 B: VMM 发起的冻结 (快照 / 热迁移 / pause) ──────────────────┐
│                                                                      │
│  发起者: VMM (QEMU pause / snapshot / migrate)                      │
│  Guest 是否知情: ✗ 不知道 (从 Guest 视角时间"消失"了)              │
│  期间谁在流逝: Host 时间在流逝, 但 Guest vCPU 被暂停               │
│                                                                      │
│  对 Guest 时钟的影响:                                               │
│    CLOCK_MONOTONIC  不推进 (Guest 不知道这段时间存在)              │
│    CLOCK_BOOTTIME   不推进 (同上, 不算 "Guest 发起的挂起")         │
│    CLOCK_REALTIME   不推进 (需要 ptp_kvm + chrony 校准)            │
│                                                                      │
│  通知机制: PVCLOCK_GUEST_STOPPED 标志告知 Guest "你被暂停过"       │
│           → pvclock_touch_watchdogs() 重置 watchdog                │
│           → 但不会修改任何时钟的值                                 │
│                                                                      │
│  典型场景: VM 快照、热迁移、调试暂停                               │
└──────────────────────────────────────────────────────────────────────┘

对比总结:
┌────────────────────┬─────────────────────┬─────────────────────┐
│                    │ 场景 A: Guest 挂起   │ 场景 B: VMM 冻结    │
├────────────────────┼─────────────────────┼─────────────────────┤
│ 发起者             │ Guest 内核          │ VMM (QEMU)          │
│ Guest 是否知情     │ ✓ 知道              │ ✗ 不知道            │
│ CLOCK_MONOTONIC    │ 不推进              │ 不推进              │
│ CLOCK_BOOTTIME     │ ✓ 包含挂起时长      │ ✗ 不包含            │
│ CLOCK_REALTIME     │ 不推进 (需 NTP)     │ 不推进 (需 ptp_kvm) │
│ 通知机制           │ ACPI 唤醒流程       │ PVCLOCK_GUEST_STOPPED│
│ 典型用途           │ 笔记本合盖          │ VM 快照/迁移        │
└────────────────────┴─────────────────────┴─────────────────────┘
```

### 各时钟对比

| 时钟 | ID | 起点 | 受 NTP step 影响 | 受 NTP slew 影响 | 典型用途 |
|------|----|------|-----------------|-----------------|---------|
| **CLOCK_REALTIME** | 0 | Unix epoch (1970) | ✅ 可以跳变/回退 | ✅ 变慢/快 | `date`, 文件时间戳, 网络协议 |
| **CLOCK_MONOTONIC** | 1 | 系统启动 | ❌ 永不回退 | ✅ 变慢/快 | 测量间隔, 超时, 性能统计 |
| **CLOCK_MONOTONIC_RAW** | 4 | 系统启动 | ❌ | ❌ | 精确间隔测量 (无调整) |
| **CLOCK_BOOTTIME** | 7 | 系统启动 | ❌ 永不回退 | ✅ 变慢/快 | 包含挂起时间的单调时钟 |
| **CLOCK_TAI** | 11 | TAI epoch | ✅ (含闰秒) | ✅ | 科学计算 (不含闰秒偏移) |

> **关键区分**：MONOTONIC 不受 NTP **step**（跳变）影响，但受 NTP **slew**（频率微调）影响 ——
> 因为 REALTIME 和 MONOTONIC 共享同一个 `tkr_mono.mult`，NTP slew 改变 `mult` 时两者同步变化。

### 时间调整对各时钟的影响汇总

```
调整类型              REALTIME  MONOTONIC  MONO_RAW  BOOTTIME
─────────────────────────────────────────────────────────────
NTP step              ✅ 跳变    ❌ 不变    ❌ 不变    ❌ 不变
(settimeofday)                      (不会回退)

NTP frequency slew   ✅ 变慢/快  ✅ 变慢/快  ❌ 不变    ✅ 变慢/快
(adjtimex)                        (共享 mult)            (共享 mult)

闰秒插入              ✅ +1s     ❌ 不变    ❌ 不变    ❌ 不变

Guest 内 suspend      ❌ 不变    ❌ 不变    ❌ 不变    ✅ +挂起时间
(场景 A)                          (MONOTONIC             (只包含
                                  永不回退)              Guest 发起的挂起)

VMM 快照/迁移         ❌ 不变    ❌ 不变    ❌ 不变    ❌ 不变
(场景 B)              (需 ptp_kvm              (PVCLOCK_GUEST_STOPPED
                      校准墙钟)                只通知, 不改时间)
```

---

## 🧭 关键概念区分：clocksource vs clockevent

> Linux 把硬件时钟分成**两个完全不同的子系统**。本章最容易混淆的说法
> （"TSC-deadline 是一种时钟源"、"Constant TSC 演进出了 TSC-deadline"）
> 都源于没分清这两者。先建立这个区分，再往下读。

### 两个问题，两个子系统

| | **clocksource（时间源）** | **clockevent（事件源）** |
|---|---|---|
| 回答的问题 | **"现在几点了？"** | **"在未来某时刻叫醒我"** |
| 角色 | 只读计数器，被 timekeeping 反复读 | 可编程定时器，编程后到点发中断 |
| 核心结构 | `struct clocksource`<br/>`read()` + `mult/shift` + `rating`<br/>（include/linux/clocksource.h:101） | `struct clock_event_device`<br/>`set_next_event()` + `next_event` + `features`<br/>（include/linux/clockchips.h:100） |
| 消费者 | timekeeping → CLOCK_REALTIME / MONOTONIC / ... | tick、hrtimer、调度、NO_HZ 空闲 |
| 选择机制 | rating 高者胜，`/sys/.../current_clocksource` 可运行时切换 | rating + features，`/sys/.../current_device` |

### x86 设备的归属表

| 设备 | clocksource | clockevent | 说明 |
|---|---|---|---|
| **TSC** | ✅ `tsc`（rating 300，tsc.c:1187） | ❌ | 纯计数器，只能读，产生不了中断 |
| **kvm-clock** | ✅ `kvm-clock`（rating 400，kvmclock.c:157） | ❌ | 半虚拟化时钟源 |
| **LAPIC Timer**（one-shot/periodic） | ❌ | ✅ `lapic`（rating 100，apic.c:494） | |
| **LAPIC Timer**（TSC-deadline 模式） | ❌ | ✅ `lapic-deadline`（apic.c:585） | **TSC 只是它的时间基准** |
| HPET | ✅ `hpet` | ✅ 比较器 | 双重角色 |
| PIT | ✅ `pit`（低 rating，校准/看门狗用） | ✅ channel 0 → IRQ0 | 双重角色 |
| ACPI PM Timer | ✅ `acpi_pm` | ❌ | |

### TSC-deadline 是 clockevent，不是时钟源

这是本章最关键的澄清。TSC-deadline 是 **LAPIC timer 这个 clockevent 设备的一种工作模式**
（特性位 `X86_FEATURE_TSC_DEADLINE_TIMER` = CPUID.01H:ECX[24]，cpufeatures.h:137；
名字里有 "TSC" 只是因为它的**时间基准**是 TSC）：

```c
/* 来源: arch/x86/kernel/apic/apic.c:494 — LAPIC timer clockevent 的基本形态 */
static struct clock_event_device lapic_clockevent = {
    .name           = "lapic",
    .features       = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT |
                      CLOCK_EVT_FEAT_C3STOP | CLOCK_EVT_FEAT_DUMMY,
    .set_next_event = lapic_next_event,      /* one-shot: 写 APIC_TMICT */
    ...
};

/* 来源: arch/x86/kernel/apic/apic.c:584 — setup_APIC_timer(): 若 CPU 支持
 * TSC-deadline, 把同一个 clockevent 设备改造成 deadline 变体 */
if (this_cpu_has(X86_FEATURE_TSC_DEADLINE_TIMER)) {
    levt->name = "lapic-deadline";            /* 设备名都变了 */
    levt->features &= ~(CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_DUMMY);
    levt->set_next_event = lapic_next_deadline;   /* 换编程函数 */
    clockevents_config_and_register(levt,
        tsc_khz * (1000 / TSC_DIVISOR),       /* 频率 = TSC 频率/8 (:590) */
        0xF, ~0UL);
}

/* 来源: arch/x86/kernel/apic/apic.c:419 — 编程方式: 读当前 TSC, 写 deadline MSR */
static int lapic_next_deadline(unsigned long delta, struct clock_event_device *evt)
{
    tsc = rdtsc();
    wrmsrl(MSR_IA32_TSC_DEADLINE, tsc + (((u64) delta) * TSC_DIVISOR));
    return 0;
}
```

所以 TSC 与 TSC-deadline 的关系是 **"时间基准" 与 "设备"**，不是并列的两个时钟源：

```
                       ┌── 读 ──▶ clocksource (tsc / kvm-clock)
                       │            → timekeeping → CLOCK_REALTIME / MONOTONIC
    TSC (64位计数器) ───┤            (提供 "现在的时间")
                       │
                       └── 与 deadline 比较 ──▶ clockevent (lapic-deadline)
                                     → 本地定时器中断 → tick / hrtimer
                                     (提供 "未来的事件")
```

对应到 KVM 语境：

- **TSC 虚拟化**（TSC_OFFSET / TSC_MULTIPLIER）同时服务两条路径：guest 的
  RDTSC（clocksource 读路径）与 deadline 比较（clockevent 编程路径）用的都是
  同一个 "guest TSC = host TSC × multiplier + offset"。
- **kvmclock** 是纯 clocksource（把 guest TSC 换算成纳秒，见 §4）。
- **LAPIC timer 三种模式**（one-shot / periodic / tsc-deadline）全部是
  clockevent，KVM 侧由 `lapic.c` 模拟，见 §2。
- 因此 "TSC-deadline 为什么高效" 的答案要从 **clockevent 编程成本** 角度找
  （一次 MSR 写 vs 写 TMICT + 除数），而不是从 "时钟源精度" 角度找。

---

## 🕐 硬件时钟源全景

### 为什么 x86 有这么多时钟？

x86 的历史包袱导致了多种时钟共存，每种都有自己的设计目的：

```
┌─────────────────────────────────────────────────────────────────────┐
│              x86 时钟设备演进 (时间源 + 事件源)                       │
│   注: 下面按年代混排, 每项属于 clocksource 还是 clockevent          │
│       见上节归属表                                                   │
│                                                                     │
│  ① PIT (8254, 1976)                                                │
│     用途: PC兼容性的基础 (BIOS、DOS时代的定时器)                     │
│     频率: 1.193181 MHz (固定, KVM_PIT_FREQ=1193181)                │
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
│     演进: Constant TSC → Invariant TSC (nonstop_tsc /              │
│           tsc_known_freq / tsc_adjust 等特性)                     │
│     注意: TSC-deadline **不是 TSC 的演进**, 而是 LAPIC Timer 的    │
│           一种模式 (只是拿 TSC 做时间基准), 见上节概念区分         │
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

### 各时钟设备对比（按子系统分类）

> 先分清类别（见上节）：下表拆成 **clocksource（读时间）** 与
> **clockevent（设定时器）** 两张，避免再拿两者直接比"速度"。

**clocksource — 回答"现在几点"**

| 设备 | 读取方式 | VM-Exit? | 迁移友好? | 适用场景 |
|--------|---------|----------|----------|---------|
| TSC | RDTSC 指令 (~ns 级) | ✗ 无 | ✗ 需 TSC 同步/缩放 | 高精度时间戳，TSC 可靠时的默认时钟源 |
| kvmclock | 共享页内存读 + 乘加 | ✗ 无 | ✓✓ 经 KVM_SET_CLOCK | TSC 不可靠时的首选，见 §4 对比 |
| HPET | MMIO | ★ 每次读 Exit | ✓ | 替代 PIT（慢） |
| acpi_pm | IO 端口 | ★ 每次读 Exit | ✓ | 兜底 |
| PIT | IO 端口 | ★ 每次读 Exit | ✓ | 仅校准/看门狗 |

**clockevent — 回答"到点叫我"**

| 设备/模式 | 编程方式 | VM-Exit? | 说明 |
|--------|---------|----------|---------|
| LAPIC timer (one-shot) | 写 TMICT (MMIO) | ★ 每次编程 Exit | NO_HZ tickless 默认 |
| LAPIC timer (periodic) | 写 TMICT 一次，自动重装 | ★ 配置时 Exit | 传统周期 tick |
| LAPIC timer (tsc-deadline) | 写 MSR_IA32_TSC_DEADLINE | ★ 每次编程 Exit，可用 preemption timer 硬件加速 | 最高效定时器（clockevent，不是时钟源！） |
| HPET 比较器 | MMIO | ★ 每次编程 Exit | |
| PIT channel 0 | IO 端口 | ★ 每次编程 Exit | 兼容启动 |

> 注意比较口径：clocksource 比的是**读一次时间**的开销；
> clockevent 比的是**编程一次事件 + 事件到期**的开销。
> "TSC 比 TSC-deadline 快/慢" 这类说法没有意义——它们不在同一个子系统里。

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
│  Guest写 → pit_ioport_write()  (i8254.c:438)                     ││
│    → pit_load_count(): 设置计数值                                ││
│    → 启动 hrtimer 模拟硬件计数                                   ││
│                                                                   ││
│  硬件计数到期 → KVM hrtimer 回调                                 ││
│    → pit_timer_fn()  (i8254.c:268)                               ││
│    → kvm_set_irq(0) → 投递 IRQ0 到 PIC/IOAPIC                   ││
│    → 注入到 vCPU                                                ││
│                                                                   ││
│  Guest读 → pit_ioport_read()  (i8254.c:513)                      ││
│    → pit_get_count(): 根据hrtimer计算当前计数值                  ││
│    → 返回模拟的计数值                                            ││
└──────────────────────────────────────────────────────────────────┘│
```

**关键数据结构**:

```c
/* 来源: arch/x86/kvm/i8254.h:9-49（行号随文标注） */

/* PIT 单个通道的状态 */
struct kvm_kpit_channel_state {
    u32 count;                  /* 初始装载值，可以是 65536 */
    u16 latched_count;          /* 锁存的计数值 */
    u8 count_latched;           /* 锁存状态 */
    u8 status_latched;          /* 状态锁存 */
    u8 status;                  /* 状态 (i8254.h:14) */
    u8 read_state;              /* 读状态 */
    u8 write_state;             /* 写状态 */
    u8 write_latch;             /* 写锁存 */
    u8 rw_mode;                 /* 读/写模式 (LSB/MSB) */
    u8 mode;                    /* 工作模式 (0-5) */
    u8 bcd;                     /* BCD模式 (not supported) */
    u8 gate;                    /* 门控输入 (timer start) */
    ktime_t count_load_time;    /* 计数装载时刻 (i8254.h:22, host时间) */
};

/* PIT 整体状态（hrtimer 在这里，不在 struct kvm_pit） */
struct kvm_kpit_state {
    struct kvm_kpit_channel_state channels[3];
    u32 flags;
    bool is_periodic;           /* i8254.h:29 */
    s64 period;                 /* 周期, 单位 ns */
    struct hrtimer timer;       /* i8254.h:31 Host高精度定时器 */
    struct mutex lock;
    atomic_t reinject;
    atomic_t pending;           /* 累积已触发未注入的定时器 */
    /* ... */
};

struct kvm_pit {
    struct kvm_io_device dev;
    struct kvm *kvm;
    struct kvm_kpit_state pit_state;    /* PIT 状态 */
    int irq_source_id;
    struct kthread_worker *worker;      /* 中断注入推迟到内核线程 */
    struct kthread_work expired;
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
period = count / 1193181 (秒)  (KVM_PIT_FREQ = 1193181)
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
  → 更新 apic->lapic_timer 寄存器
  → restart_apic_timer()
    │
    ├── Periodic/One-shot:
    │     计算超时 = now + count × divide_count × apic_bus_cycle_ns
    │     hrtimer_start(&apic->lapic_timer.timer, ...)
    │     → Host内核 hrtimer 到期时:
    │       apic_timer_fn() → apic_timer_expired()
    │       → kvm_apic_local_deliver() → 注入中断到vCPU
    │
    └── TSC-deadline:
          Guest 写 MSR_IA32_TSC_DEADLINE (MSR 0x6E0)
          → KVM 存入 apic->lapic_timer.tscdeadline
          → HW加速: vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, delta)
          硬件自动比较 TSC vs deadline
          到期时自动触发VM-Exit → KVM注入中断
          ★ 几乎零软件开销!

Guest读 APIC_TMCCT (当前计数值)
  → apic_mmio_read() → VM-Exit
  → 对于TSC-deadline: 从TSC差值计算剩余计数
```

**TSC-deadline 的 VMCS 加速**:

```c
/* arch/x86/kvm/vmx/vmx.c + lapic.c */

/*
 * TSC-deadline 模式的硬件加速:
 *
 * Guest 写 MSR_IA32_TSC_DEADLINE (MSR 0x6E0) 触发 VM-Exit
 * WRMSR exit → kvm_set_msr_common() (x86.c:3890) 处理:
 *   → kvm_set_lapic_tscdeadline_msr() (lapic.c:2585):
 *   1. hrtimer_cancel() 后存入 apic->lapic_timer.tscdeadline
 *   2. start_apic_timer() → restart_apic_timer() → start_hv_timer()
 *      → vmx_set_hv_timer()
 *   3. 计算 delta = deadline - current_TSC (并扣除 timer_advance)
 *   4. vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, delta_cycles)
 *      (使用 VMX preemption timer，不是虚构的 TSC_DEADLINE VMCS 字段)
 *
 *   如果 "use TSC scaling" secondary exec control 启用:
 *     硬件自动将Guest的deadline转换为Host的TSC值
 *     硬件直接比较Host TSC vs 转换后的deadline
 *     到期时自动VM-Exit (preemption timer fired)
 *   否则:
 *     KVM软件回退: hrtimer (start_sw_tscdeadline)
 *
 * 这就是TSC-deadline高效的原因:
 *   写MSR → 一次VM-Exit → KVM设置preemption timer → 硬件自动处理
 *   到期VM-Exit后注入中断，不需要KVM计算hrtimer
 */
```

### 3. TSC 虚拟化 (x86.c, vmx.c)

TSC 是最复杂的部分。Guest 通过 RDTSC 指令读取时间戳，这条指令不需要 VM-Exit。但问题是多 CPU 的 TSC 可能不同步。

#### 核心概念：Offset 和 Scaling

Guest TSC 和 Host TSC 的关系可以用一个**线性变换**描述：

```
Guest TSC = Host TSC × scaling + offset
```

这两个参数解决两个不同的问题：

```
┌─ TSC Offset (加常数) ──────────────────────────────────────────┐
│                                                                  │
│  解决的问题: Guest 和 Host 的 TSC "起点"不同                  │
│                                                                  │
│  比喻: 两支温度计                                                │
│    Host 温度计:   0°C ──────▶ 100°C                            │
│    Guest 温度计:  10°C ─────▶ 110°C                            │
│                  ↑                                              │
│              offset = +10                                       │
│    两支温度计刻度相同 (频率相同), 只是起点差 10                │
│                                                                  │
│  典型场景:                                                      │
│    · VM 刚创建时: Guest TSC 要从 0 开始, 但 Host TSC 已经跑了  │
│      很久 → offset = -host_tsc_at_vm_create                   │
│    · VM 迁移后: Host-A 的 TSC 是 500万, Host-B 是 300万       │
│      → 调整 offset 让 Guest 时间连续                           │
│                                                                  │
│  只改 offset: Guest TSC 整条时间线平移, 斜率 (频率) 不变      │
└──────────────────────────────────────────────────────────────────┘

┌─ TSC Scaling (乘系数) ─────────────────────────────────────────┐
│                                                                  │
│  解决的问题: Guest 和 Host 的 TSC "频率"不同                  │
│                                                                  │
│  比喻: 两个钟表                                                  │
│    Host 钟表:     1 秒走 3 GHz ticks                            │
│    Guest 钟表:    1 秒走 2 GHz ticks                            │
│                   ↑                                             │
│              scaling = 2/3                                      │
│    两个钟表起点可以相同, 但走速不同                             │
│                                                                  │
│  典型场景:                                                      │
│    · 跨频率迁移: Host-A 是 3 GHz, Host-B 是 2 GHz             │
│      → scaling = 2/3, 让 Guest 仍看到 2 GHz 的 TSC            │
│    · 嵌套虚拟化: L0 Host 是 3 GHz, L1 想给 L2 暴露 2 GHz      │
│      → scaling = 2/3                                            │
│                                                                  │
│  只改 scaling: Guest TSC 的斜率 (频率) 改变                   │
│    → 时间流逝的速度变了                                         │
└──────────────────────────────────────────────────────────────────┘

┌─ 两者结合的图解 ──────────────────────────────────────────────┐
│                                                                  │
│  Guest TSC                                                      │
│       │                        scaling = 0.8                   │
│       │                       ╱                                │
│       │                     ╱   offset = +100                  │
│       │                   ╱     (Guest 起点比 Host 晚)        │
│  100 ─┤─ ─ ─ ─ ─ ─ ─ ─●─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─              │
│       │               ╱ |                                      │
│       │             ╱   |                                      │
│       │           ╱     |                                      │
│       │         ╱       |                                      │
│       │       ╱         |                                      │
│       │     ╱           |                                      │
│       │   ╱             |                                      │
│       │ ╱               |                                      │
│     0 ●─────────────────┼───────────────────── Host TSC        │
│       0                125                                     │
│                                                                  │
│  公式: Guest = Host × 0.8 + 100                               │
│    Host = 0   → Guest = 0 × 0.8 + 100 = 100                  │
│    Host = 125 → Guest = 125 × 0.8 + 100 = 200                │
│                                                                  │
│  物理意义:                                                    │
│    · Host 过了 125 ticks → Guest 过了 100 ticks (频率变慢)   │
│    · Guest 的起点是 100, 不是 0 (offset 补偿)                │
└──────────────────────────────────────────────────────────────────┘
```

**硬件实现**：CPU 在 Guest 执行 RDTSC 时，**自动**完成这个计算，无需 VM-Exit：

```
Guest 执行 RDTSC:
  1. CPU 读取 Host TSC (物理计数器)
  2. 硬件计算: Guest TSC = Host TSC × TSC_MULTIPLIER + TSC_OFFSET
  3. 返回 Guest TSC 给 Guest
  
  整个过程: 无 VM-Exit, 几十纳秒完成
```

**VMCS 中的 TSC 控制字段**:

> 命名说明：功能叫 "TSC scaling"，但 Intel VMCS 字段叫 `TSC_MULTIPLIER`，AMD 对应 MSR 叫 `TSC_RATIO`。三者是一回事。

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

┌─ TSC scaling ──────────────────────────────────────────────────┐
│  (VMCS 字段名: TSC_MULTIPLIER; AMD MSR: TSC_RATIO)            │
│                                                                   │
│  Guest RDTSC = (Host TSC × TSC_MULTIPLIER) + TSC_OFFSET        │
│                                                                   │
│  用途: 让Guest看到不同频率的TSC                                  │
│    - 嵌套虚拟化: L2 Guest需要看到L1的TSC频率                     │
│    - 迁移兼容: 不同Host CPU频率不同, 通过缩放统一               │
│                                                                   │
│  格式: 64位定点数, 低48位为小数 (VMX, vmx.c:8502)             │
│    1.0 = 0x0001000000000000 (不缩放, 即 2^48)                 │
│    2.0 = 0x0002000000000000 (2倍速)                            │
│    0.5 = 0x0000800000000000 (半速)                             │
│    注: AMD SVM 用 32 位小数 (svm.c:5478),                     │
│        1.0 = 0x0000000100000000                                │
│                                                                   │
│  启用条件:                                                       │
│    VMX: IA32_VMX_PROCBASED_CTLS2 bit 25                         │
│         ("enable TSC scaling" secondary exec control)            │
│         KVM: SECONDARY_EXEC_TSC_SCALING (vmx.h:79)              │
│    前提: CPU 支持 Invariant TSC (CPUID.80000007H:EDX[8])       │
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
    kvm_synchronize_tsc() 检查各pCPU的TSC偏移
    如果不同步: kvm_check_tsc_unstable() 返回 true
    → 标记为 "TSC unstable", 不使用TSC做时钟源
    → kvmclock回退到system_time计算，不使用纯TSC


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
│    struct pvclock_vcpu_time_info {  /* asm/pvclock-abi.h:26 */  │
│        u32 version;         ← 版本号 (奇数=更新中)             │
│        u32 pad0;            ← 对齐填充                        │
│        u64 tsc_timestamp;   ← 写结构时的Host TSC              │
│        u64 system_time;     ← 写结构时的Host monotonic时间     │
│        u32 tsc_to_system_mul; ← TSC→时间的缩放因子            │
│        s8  tsc_shift;         ← TSC移位调整                    │
│        u8  flags;             ← PVCLOCK_TSC_STABLE等 (u8!)    │
│        u8  pad[2];          ← 填充 (总共32字节)               │
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
│  → 但 "Guest 默认用哪个" 由 rating 决定, 见下节对比:             │
│    TSC 可靠时反而是 tsc 胜出, kvmclock 主动让位                  │
└──────────────────────────────────────────────────────────────────┘
```

### TSC vs kvm-clock：两个 clocksource 的选型与取舍

两者**同源**——最终都派生自 host 物理 TSC，差别在"读法"和"保证"：

- `tsc` clocksource：guest 直接 RDTSC，拿到的是
  `host TSC × multiplier + offset` 的原始 cycles（§3）。
- `kvm-clock`：host 在 pvclock 页里给出锚点 `(tsc_timestamp, system_time)`，
  guest 读页 + 一次乘加就得到纳秒。锚点本身也是 guest TSC
  （`tsc_timestamp = kvm_read_l1_tsc()`, x86.c:3265）+ host 内核时间。

| 维度 | `tsc` clocksource | `kvm-clock` |
|------|-------------------|-------------|
| 读取路径 | 单条 RDTSC（tsc.c `read_tsc`） | per-CPU 共享页读 + seqcount 重试（kvmclock.c:74/:84） |
| 返回值 | 原始 cycles（timekeeping 自己乘加） | 已经是纳秒（换算在 pvclock 公式里） |
| VM-Exit | 无 | 无 |
| vDSO | `VDSO_CLOCKMODE_TSC`（tsc.c:1197） | `VDSO_CLOCKMODE_PVCLOCK`（kvmclock.c:256），前提 `PVCLOCK_TSC_STABLE_BIT`（kvmclock.c:253） |
| 频率获取 | CPUID 0x15/0x16 或校准 | **从 pvclock 页直接拿**：`kvm_get_tsc_khz()` 置 `TSC_KNOWN_FREQ`（kvmclock.c:117-121），guest 跳过 PIT/HPET 校准 |
| 迁移 | 靠 TSC_OFFSET/TSC_MULTIPLIER 补偿；跨频率迁移无 scaling 硬件则不可行 | VMM 用 `KVM_SET_CLOCK` 恢复 `kvmclock_offset`（x86.c:7047），语义更直接 |
| 附带职责 | 无 | 即使 clocksource 选了 tsc，**sched_clock 仍是 kvmclock**（kvmclock.c:321）；还带 wallclock（kvmclock.c:61）与 `GUEST_STOPPED`（kvmclock.c:135） |
| 失效模式 | TSC 被标不稳定 → 整个时钟源被注销（tsc.c:1414-1417） | masterclock 失效 → 掉到 per-vCPU 独立基准慢路径 |

**选择逻辑（决定 guest 默认时钟源的关键代码）**：

```c
/* 来源: arch/x86/kernel/kvmclock.c:334-347 (kvmclock_init 尾部) */
/*
 * X86_FEATURE_NONSTOP_TSC is TSC runs at constant rate
 * with P/T states and does not stop in deep C-states.
 *
 * Invariant TSC exposed by host means kvmclock is not necessary:
 * can use TSC as clocksource.
 */
if (boot_cpu_has(X86_FEATURE_CONSTANT_TSC) &&
    boot_cpu_has(X86_FEATURE_NONSTOP_TSC) &&
    !check_tsc_unstable())
        kvm_clock.rating = 299;          /* ★ 主动从 400 降到 299 */

clocksource_register_hz(&kvm_clock, NSEC_PER_SEC);
```

```
rating:  tsc = 300 (tsc.c:1189)
         kvm-clock = 400 默认 (kvmclock.c:160)
                   → guest TSC 是 constant + nonstop + 未标不稳定时降到 299

结果:
  · host TSC 可靠 (现代服务器 + -cpu host + vCPU TSC 对齐)
      → kvm-clock 让位, guest 默认 clocksource = tsc
  · TSC 不可靠 (老硬件 / 跨插槽漂移 / 无 scaling 的跨频迁移)
      → init_tsc_clocksource() 拒绝注册 tsc (tsc.c:1414-1417)
      → guest 默认 = kvm-clock (400)
```

**使用场景建议**：

| 场景 | 推荐 | 原因 |
|------|------|------|
| 现代 host + 不迁移 | 顺其自然（默认即 tsc） | 单指令最快；kvmclock 仍兜底 sched_clock |
| 需要热迁移 / 快照恢复 | 保留 kvmclock 可用 | `KVM_SET_CLOCK` 恢复时间基准；tsc 依赖 scaling 硬件 |
| host TSC 不稳定 | 强制 `clocksource=kvm-clock` | tsc 会被内核标不稳定并注销 |
| 对 `clock_gettime` 极端敏感 | 两者都有 vDSO 快速路径 | kvm-clock 的 vDSO 需要 `PVCLOCK_TSC_STABLE_BIT`（masterclock 开启） |

> ⚠️ 勘误：本节旧版说"现代 Linux Guest 默认使用 kvmclock 作为 clocksource"，
> 这只在 TSC 不可靠时成立。现代环境下内核**故意让位**（kvmclock.c:345
> 注释 "Invariant TSC exposed by host means kvmclock is not necessary"）。
> 可在 Guest 里实测：`cat /sys/devices/system/clocksource/clocksource0/current_clocksource`。

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
│    · VMM 恢复 VM 后调用 KVM_KVMCLOCK_CTRL (x86.c:6195)        │
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

### 4.5 kvmclock 的局限与 PTP KVM

kvmclock 解决了**单调时钟**的问题（高精度、无 VM-Exit），但**墙上时间**（CLOCK_REALTIME）的同步仍然存在缺口：

| 机制 | 精度 | 持续同步 | 用途 |
|------|------|---------|------|
| `wall_clock` (pvclock_wall_clock) | ~毫秒 | ❌ 启动时读一次 | Guest 初始化系统时间 |
| `kvmclock` (pvclock_vcpu_time_info) | 纳秒级 | ✅ TSC 计算 | sched_clock / 单调时钟 |
| **`ptp_kvm`** (KVM_HC_CLOCK_PAIRING) | **~亚微秒** | **✅ hypercall 持续同步** | **墙上时间精密同步** |

#### PTP KVM 原理

PTP KVM 通过 hypercall 让 Guest 每次都能获取 Host 的**精确墙上时间 + 对应 TSC**，
由 PTP 子系统交给 chrony 做精密同步：

```
Guest (ptp_kvm driver)                    Host (KVM)
─────────────────                        ────────────

kvm_arch_ptp_get_crosststamp()
    │
    ├── 读 pvclock 页 (seqcount 版本)
    │
    ├── KVM_HC_CLOCK_PAIRING ──hypercall──▶ kvm_pv_clock_pairing()
    │   (传 clock_pair GPA)                  │
    │                                        ├── kvm_get_walltime_and_clockread()
    │                                        │   原子读取:
    │                                        │     ts    = CLOCK_REALTIME (墙上时间)
    │                                        │     cycle = host TSC
    │                                        │   ↑ 同一次调用, 原子性保证精度
    │                                        │
    │                                        ├── clock_pair.sec  = ts.tv_sec
    │                                        ├── clock_pair.nsec = ts.tv_nsec
    │                                        ├── clock_pair.tsc  = guest TSC
    │                                        │
    │                                        └── kvm_write_guest(gpa)
    │
    ◀── 读取 clock_pair ──────────────────┘
    │
    └── 精确的 (墙上时间, TSC) 对
        → PTP 子系统 → chrony PHC refclock
```

**为什么 PTP KVM 精度高？**

1. **原子性**：`kvm_get_walltime_and_clockread()` 同时读 `CLOCK_REALTIME` 和 TSC，
   避免两次读取之间的时间差
2. **无网络延迟**：hypercall 是同步的，延迟只有 ~1μs
3. **交叉时间戳**：返回的 `(墙上时间, TSC)` 对精确对应，PTP 可计算精确时钟偏移

#### 使用方式

```bash
# Guest 内加载 ptp_kvm 模块
modprobe ptp_kvm
ls /dev/ptp*    # → /dev/ptp0

# chrony 配置
# /etc/chrony/chrony.conf:
refclock PHC /dev/ptp0 poll 3 dpoll -2 offset 0

# 验证精度
chronyc sources   # 偏移 < 1μs
```

#### 关键内核代码

```c
/* Host 侧: arch/x86/kvm/x86.c:9928 */
static int kvm_pv_clock_pairing(struct kvm_vcpu *vcpu, gpa_t paddr,
                                unsigned long clock_type)
{
    /* 原子读取墙上时间 + TSC */
    kvm_get_walltime_and_clockread(&ts, &cycle);

    clock_pairing.sec  = ts.tv_sec;
    clock_pairing.nsec = ts.tv_nsec;
    clock_pairing.tsc  = kvm_read_l1_tsc(vcpu, cycle);

    kvm_write_guest(vcpu->kvm, paddr, &clock_pairing, sizeof(...));
}

/* Guest 侧: drivers/ptp/ptp_kvm_x86.c:95 */
int kvm_arch_ptp_get_crosststamp(u64 *cycle, struct timespec64 *tspec, ...)
{
    /* hypercall 获取 host 的 (墙上时间, TSC) 对 */
    kvm_hypercall2(KVM_HC_CLOCK_PAIRING, clock_pair_gpa,
                   KVM_CLOCK_PAIRING_WALLCLOCK);

    tspec->tv_sec  = clock_pair->sec;
    tspec->tv_nsec = clock_pair->nsec;
    *cycle = __pvclock_read_cycles(src, clock_pair->tsc);
}
```

#### Guest 内的完整时间同步层次

```
Guest 内时间
    │
    ├── sched_clock() → kvmclock (TSC-based, 单调)
    │                    └─ 不需要持续同步
    │
    ├── CLOCK_MONOTONIC → kvmclock
    │                    └─ 纯 TSC 计算, 不受 NTP slew 影响 (见上节)
    │
    ├── CLOCK_REALTIME → ptp_kvm (hypercall, 亚微秒精度)
    │                    └─ chrony 用 PTP 设备持续同步
    │
    └── date / gettimeofday
         └─ 底层 = CLOCK_REALTIME
         └─ 通过 ptp_kvm + chrony 保持与 host 墙上时间一致
```

### 冷启动：时间如何进入 Guest (事实核查补充)

```
VM 冷启动时, Guest 的墙钟 (CLOCK_REALTIME) 从何而来?

┌─ 步骤 1: Guest 内核早期启动 ─────────────────────────────────┐
│  检测 CPUID 0x40000001 (KVM 签名)                            │
│  kvmclock_init():                                             │
│    ├─ 写 MSR_KVM_SYSTEM_TIME_NEW 注册 pvclock 页             │
│    ├─ 注册 kvm-clock clocksource                             │
│    ├─ pv_ops.time.sched_clock = kvm_sched_clock_read         │
│    ├─ x86_platform.get_wallclock = kvm_get_wallclock         │
│    └─ x86_platform.set_wallclock = kvm_set_wallclock         │
│                                                                │
│  注意: set_wallclock 返回 -ENODEV!                            │
│        → Guest 里 hwclock -w (写回 RTC) 会失败               │
│        → 这是 pvclock 故意不给写, 防止 guest 修改 host 时间   │
└────────────────────────────────────────────────────────────────┘

┌─ 步骤 2: timekeeping_init() ────────────────────────────────┐
│  timekeeping_init()                                          │
│    → read_persistent_clock64()                               │
│    → kvm_get_wallclock():                                    │
│                                                                │
│      写 MSR_KVM_WALL_CLOCK_NEW, host 侧填页:                │
│        wall_nsec = ktime_get_real_ns() - get_kvmclock_ns(kvm);│
│                                                                │
│      即 "host 真实墙钟 − 当前 kvmclock" = guest 墙钟原点    │
└────────────────────────────────────────────────────────────────┘

┌─ 步骤 3: 建立时间基准 ──────────────────────────────────────┐
│  CLOCK_REALTIME = wall_clock + kvmclock                      │
│                                                                │
│  · wall_clock: boot 时刻的 host 墙钟                         │
│  · kvmclock: 从 boot 开始的增量 (基于 pvclock)               │
│  · 精度: 纳秒级, 一次性完成                                  │
│                                                                │
│  之后再无 "同步":                                            │
│    guest realtime = 开机原点 + clocksource 增量               │
│    而 clocksource (tsc 或 kvm-clock) 最终都由 host 物理 TSC 派生│
│    → guest 和 host 流逝速率天然同源                          │
│    → 不存在晶振 drift                                        │
│    → 这就是冷启动不需要 NTP/PTP 的原因                       │
└────────────────────────────────────────────────────────────────┘

┌─ 没有 kvmclock 时的回退 ────────────────────────────────────┐
│  如果 kvmclock 不可用 (no-kvmclock 或 CLOCKSOURCE2 关闭):   │
│    → 退回 mach_get_cmos_time() 读 CMOS                      │
│    → 秒级精度, 且每次读都 VM-Exit                            │
│    → 只在 boot 时读一次, 之后用 TSC 或其他 clocksource       │
└────────────────────────────────────────────────────────────────┘

┌─ 启动后各时钟在 Guest 中的对应关系 ─────────────────────────┐
│                                                               │
│  对照上节 "Linux 时钟系统基本概念":                          │
│                                                               │
│  Host CLOCK_REALTIME  ──→  wall_clock (启动快照)              │
│                            + ptp_kvm (持续同步, 见 §4.5)      │
│                                                               │
│  Host CLOCK_MONOTONIC ──→  kvmclock (TSC-based)              │
│                            = 纯 TSC 计算, 单调递增            │
│                                                               │
│  Host CLOCK_MONOTONIC_RAW →  (Guest 无直接等价物)            │
│                            sched_clock() 最接近               │
│                                                               │
│  Host CLOCK_BOOTTIME  ──→  kvmclock + 挂起补偿               │
│                            PVCLOCK_GUEST_STOPPED 通知 guest   │
│                                                               │
│  重要: kvmclock 不受 host NTP slew 影响 (纯 TSC)             │
│       但 Guest 的 CLOCK_REALTIME 通过 ptp_kvm + chrony       │
│       持续与 host 墙上时间同步                               │
└───────────────────────────────────────────────────────────────┘
```

### VM 启动：三个时间基准的首次同步 (事实核查补充)

上一节讲的是 **guest 侧**怎么把时间读进来。这一节补齐 **host 侧**：
VM 从无到有时，guest 的 TSC、kvmclock、masterclock 这三个基准分别
在哪里被"第一次强制对齐"。

```
时间线:  KVM_CREATE_VM → KVM_CREATE_VCPU(×N) → 首次 KVM_RUN → guest boot
             │                │                     │
             ①                ②                     ③
```

**① kvmclock 纪元归零 — VM 创建时 (x86.c:12841)**

```c
/* 来源: arch/x86/kvm/x86.c:12841 (kvm_arch_init_vm) */
kvm->arch.kvmclock_offset = -get_kvmclock_base_ns();
```

`get_kvmclock_base_ns()`（x86.c:2300-2310）是 host 的单调时间
（TSC 时钟下 = `ktime_get_raw() + offs_boot`，否则 `ktime_get_boottime_ns()`）。
offset 取它的负值 → **新 VM 的 kvmclock 从 0 开始**。之后
`get_kvmclock_ns(kvm) = base + kvmclock_offset`（x86.c:3136）就是 guest 时间。

QEMU 的暂停/恢复会搬运这个纪元：`hw/i386/kvm/clock.c:163`
`kvmclock_vm_state_change()` 在 VM 暂停时 `KVM_GET_CLOCK`（:105）、
恢复时 `KVM_SET_CLOCK`（:189），保证暂停期间的时间差由 VMM 决定怎么补。

**② guest TSC 首次强制同步 — 每个 vCPU 创建时 (x86.c:12463-12470)**

```c
/* 来源: arch/x86/kvm/x86.c:12463 (kvm_arch_vcpu_postcreate) */
void kvm_arch_vcpu_postcreate(struct kvm_vcpu *vcpu)
{
    ...
    vcpu_load(vcpu);
    kvm_synchronize_tsc(vcpu, NULL);   /* ★ user_value = NULL → data = 0 */
    vcpu_put(vcpu);
    ...
}
```

`kvm_synchronize_tsc()`（x86.c:2717）里 `data == 0` 是**强制同步**信号
（:2732-2737 注释 "Force synchronization when creating a vCPU, or when
userspace explicitly writes a zero value"），然后分两条路：

```
host TSC 稳定 (常见):
  offset = kvm->arch.cur_tsc_offset        (x86.c:2774)
    · 第一个 vCPU: cur_tsc_offset = 0
        → guest TSC = host TSC × multiplier + 0   (起步即与 host 对齐)
    · 后续 vCPU: 继承同一 offset
        → 所有 vCPU 的 guest TSC 严格一致 (迁移到其他 pCPU 也不跳)

host TSC 不稳定:
  data += nsec_to_cycles(elapsed)          (x86.c:2777-2779)
    · 用 "距上次同步流逝的墙钟时间" 推进基准再重算 offset
    → guest TSC 对齐到 host 单调时间轴, 而不是裸的 host TSC
```

offset 最终经 `__kvm_synchronize_tsc()` → `kvm_vcpu_write_tsc_offset()`
（x86.c:2613）写进 VMCS 的 `TSC_OFFSET`。同一条路径也被两个"手动"场景复用：
迁移恢复写 `MSR_IA32_TSC`（host_initiated，x86.c:3939-3940）和
`KVM_SET_TSC_OFFSET` ioctl（x86.c:5770-5785）。

**③ masterclock 建立 — 首次 KVM_RUN 时 (x86.c:10809 / :3082 / :3015)**

②的每次同步都会调 `kvm_track_tsc_matching()`（x86.c:2515-2544）：
当**所有在线 vCPU 的 TSC 都匹配**（`nr_vcpus_matched_tsc + 1 == online_vcpus`）
且 **host 自身用 TSC 做 clocksource** 时（:2526-2528），置
`KVM_REQ_MASTERCLOCK_UPDATE` 请求（:2538）。该请求在 vCPU **下一次进入
guest 前**处理（`vcpu_enter_guest()` x86.c:10809-10810）：

```
kvm_update_masterclock()                    x86.c:3082
 └─ pvclock_update_vm_gtod_copy()           x86.c:3015
      ├─ 快照 master_kernel_ns / master_cycle_now   (:3030-3032)
      └─ use_master_clock = host_tsc_clocksource && vcpus_matched
             && !backwards_tsc_observed && !boot_vcpu_runs_old_kvmclock
                                            (:3034-3036)

之后每次刷新 pvclock 页 (kvm_guest_time_update, x86.c:3215):
  system_time = kernel_ns + kvmclock_offset          (:3302)
  if (use_master_clock)
      flags |= PVCLOCK_TSC_STABLE_BIT                (:3304-3310)
      → guest 端据此决定: sched_clock 稳定 (kvmclock.c:321)
                        + vDSO pvclock 快速路径 (kvmclock.c:253)
```

**推论（也是 practice/实验2 观察到的现象）**：在首次 `KVM_RUN` 之前，
`use_master_clock` 还是初始值（false），所以此刻 `KVM_GET_CLOCK` 拿不到
`host_tsc` / `KVM_CLOCK_HOST_TSC` 标志——`__get_kvmclock()` 只在
`ka->use_master_clock` 为真时才填这些字段（x86.c:3116）。

**三句话总结启动同步**：
1. VM 创建 → kvmclock 纪元归零（`kvmclock_offset = -base`）。
2. vCPU 创建 → guest TSC 强制对齐（稳定则共享 offset，不稳定则按墙钟推进）。
3. 首次 KVM_RUN → masterclock 快照 + `PVCLOCK_TSC_STABLE_BIT` 下发给 guest。

### Snapshot / 热迁移 / 热升级的时间处理 (事实核查补充)

```
VM 快照/迁移时, 必须一起处理的四件套:

┌──────────────────────────────────────────────────────────────────┐
│  #   对象            保存              恢复                      │
├──────────────────────────────────────────────────────────────────┤
│  1   TSC 频率        KVM_GET_TSC_KHZ   KVM_SET_TSC_KHZ          │
│  2   TSC 值          MSR_IA32_TSC      写回 → KVM 重算 OFFSET   │
│  3   kvmclock offset KVM_GET_CLOCK     KVM_SET_CLOCK             │
│  4   pvclock 页注册  两个 KVM MSR      写回 MSR, 页内容随内存快照│
│  +   watchdog 抑制   —                 KVM_KVMCLOCK_CTRL        │
└──────────────────────────────────────────────────────────────────┘

顺序有硬约束 (原因见下文):
  SET_TSC_KHZ  必须早于  SET_MSRS       // TSC deadline timer 状态
  SET_SREGS    必须早于  SET_LAPIC      // apic base msr
  SET_LAPIC    必须早于  SET_MSRS       // TSC deadline MSR 需要 LAPIC 就绪
  MSR_IA32_TSC 必须早于  MSR_IA32_TSC_DEADLINE
```

#### 恢复顺序的硬约束：为什么必须这个顺序？

这些约束源于硬件寄存器之间的依赖关系，违反会导致状态不一致或恢复失败。

```
┌─ 约束 1: SET_TSC_KHZ 必须早于 SET_MSRS ──────────────────────┐
│                                                                  │
│  原因: MSR_IA32_TSC_DEADLINE 的值需要结合 TSC 频率解释        │
│                                                                  │
│  TSC deadline timer 的到期时间 = IA32_TSC_DEADLINE 的值        │
│  但 "这个值对应什么时刻" 取决于 TSC 频率:                       │
│    · TSC = 3 GHz 时, deadline = 30亿 → 1 秒后到期              │
│    · TSC = 2 GHz 时, deadline = 30亿 → 1.5 秒后到期            │
│                                                                  │
│  KVM 内部处理:                                                  │
│    KVM_SET_MSRS(MSR_IA32_TSC_DEADLINE, value) 时:              │
│      → 需要把 deadline 转换为 host TSC 值                      │
│      → 转换公式依赖 tsc_scaling_ratio (由 KVM_SET_TSC_KHZ 设置)│
│      → 如果 TSC 频率还没设好, 转换结果错误                    │
│                                                                  │
│  错误顺序的后果:                                                │
│    Guest 的定时器到期时间不正确                                 │
│    可能导致定时器提前/延迟触发, 甚至永久不触发                  │
└──────────────────────────────────────────────────────────────────┘

┌─ 约束 2: SET_SREGS 必须早于 SET_LAPIC ───────────────────────┐
│                                                                  │
│  原因: LAPIC 的基地址在特殊寄存器中                           │
│                                                                  │
│  LAPIC 寄存器通过 MMIO 访问, 基地址由 MSR_IA32_APICBASE 决定: │
│    · 默认基地址: 0xFEE00000                                    │
│    · 可以重映射到其他地址 (x2APIC 模式等)                      │
│                                                                  │
│  KVM 内部处理:                                                  │
│    KVM_SET_SREGS 包含设置 APICBASE MSR                         │
│    KVM_SET_LAPIC 需要知道 LAPIC 基地址才能正确解析状态         │
│                                                                  │
│  错误顺序的后果:                                                │
│    LAPIC 状态可能写入错误的地址空间                            │
│    或者 KVM 无法正确识别 LAPIC 的工作模式 (xAPIC vs x2APIC)   │
└──────────────────────────────────────────────────────────────────┘

┌─ 约束 3: SET_LAPIC 必须早于 SET_MSRS ────────────────────────┐
│                                                                  │
│  原因: TSC deadline timer 是 LAPIC 的一部分, 需要 LAPIC 先就绪│
│                                                                  │
│  IA32_TSC_DEADLINE MSR 的行为依赖 LAPIC 状态:                 │
│    · LAPIC 必须已启用 (Global Enable 位)                      │
│    · LAPIC 必须配置为 TSC deadline 模式 (LVT Timer Register)  │
│    · 这些配置在 LAPIC 状态中, 不在 MSR 中                     │
│                                                                  │
│  KVM 内部处理:                                                  │
│    KVM_SET_LAPIC: 恢复 LAPIC 基状态 (LVT, TMICT, 模式等)      │
│    KVM_SET_MSRS(MSR_IA32_TSC_DEADLINE): 设置 deadline 值      │
│      → 需要检查 LAPIC 是否已配置为 deadline 模式              │
│      → 如果 LAPIC 还没恢复, 无法正确设置 deadline             │
│                                                                  │
│  错误顺序的后果:                                                │
│    TSC deadline timer 状态不一致                               │
│    deadline 可能被忽略或触发异常                                │
└──────────────────────────────────────────────────────────────────┘

┌─ 约束 4: MSR_IA32_TSC 必须早于 MSR_IA32_TSC_DEADLINE ────────┐
│                                                                  │
│  原因: deadline 是 "绝对 TSC 值", 依赖当前 TSC 值解释        │
│                                                                  │
│  Guest 设置 deadline 的典型逻辑:                               │
│    current_tsc = RDTSC()                                       │
│    deadline = current_tsc + delta                              │
│    WRMSR(IA32_TSC_DEADLINE, deadline)                          │
│                                                                  │
│  恢复时:                                                        │
│    如果先设置 deadline = 1000000                               │
│    然后设置 TSC = 5000000 (比 deadline 还大!)                 │
│    → deadline 已经 "过期", 立即触发中断                       │
│                                                                  │
│  正确顺序:                                                     │
│    先设置 TSC = 5000000                                        │
│    再设置 deadline = 6000000 (在 TSC 之后)                    │
│    → 定时器在正确的未来时刻触发                               │
│                                                                  │
│  错误顺序的后果:                                                │
│    定时器立即触发, 或永久不触发 (取决于实现)                   │
└──────────────────────────────────────────────────────────────────┘

正确的恢复顺序总结:

  ① SET_TSC_KHZ        // 先设 TSC 频率, 后续 deadline 计算依赖它
  ② SET_SREGS          // 设置 APIC base 等特殊寄存器
  ③ SET_LAPIC          // 恢复 LAPIC 状态 (包括 timer 模式)
  ④ SET_MSRS           // 最后恢复 MSR, 包括:
       a. MSR_IA32_TSC          // 先设 TSC 值
       b. MSR_IA32_TSC_DEADLINE // 再设 deadline (必须在 TSC 之后)
```

#### TSC Scaling 在快照/迁移中如何处理？

> **关键点：TSC_MULTIPLIER 本身不直接保存，而是通过 KVM_SET_TSC_KHZ 间接设置。**

```
┌─ 保存流程 (源 Host) ──────────────────────────────────────────┐
│                                                                  │
│  QEMU 调用 KVM_GET_TSC_KHZ → 得到 guest_tsc_khz             │
│    · 这是 Guest 看到的 TSC 频率 (已经经过 scaling)            │
│    · 例如: Host TSC = 3 GHz, 但 Guest 看到的是 2.5 GHz        │
│    · KVM_GET_TSC_KHZ 返回 2500000                             │
│                                                                  │
│  QEMU 把 guest_tsc_khz 存入快照文件                          │
│                                                                  │
│  注意: TSC_MULTIPLIER 是 KVM 内部根据频率计算出来的,         │
│        不直接暴露给用户态                                     │
└──────────────────────────────────────────────────────────────────┘

┌─ 恢复流程 (目标 Host) ────────────────────────────────────────┐
│                                                                  │
│  ★ VMM 只需做一件事:                                           │
│    把源端 KVM_GET_TSC_KHZ 获取的值，原样传给目标端             │
│    KVM_SET_TSC_KHZ。KVM 会自动根据目标 Host 的 TSC 频率       │
│    计算 TSC_MULTIPLIER，保证 Guest 看到的频率不变。            │
│                                                                  │
│  情况 1: 同 Host 恢复 (或目标 Host TSC 频率相同)              │
│  ┌────────────────────────────────────────────────────┐       │
│  │  源 Host TSC = 3 GHz, Guest 看到 3 GHz            │       │
│  │  目标 Host TSC = 3 GHz                            │       │
│  │                                                    │       │
│  │  KVM_SET_TSC_KHZ(3000000)                         │       │
│  │    → KVM 自动计算 multiplier = 3/3 = 1.0          │       │
│  │    → Guest 仍然看到 3 GHz (不做缩放)              │       │
│  └────────────────────────────────────────────────────┘       │
│                                                                  │
│  情况 2: 跨 Host 迁移 (目标 Host TSC 频率不同)               │
│  ┌────────────────────────────────────────────────────┐       │
│  │  源 Host: TSC = 3 GHz, Guest 看到 2.5 GHz         │       │
│  │  目标 Host: TSC = 2 GHz, 支持 TSC scaling        │       │
│  │                                                    │       │
│  │  VMM 调用: KVM_SET_TSC_KHZ(2500000)  ← 源端的值 │       │
│  │    → KVM 自动计算: multiplier = 2.5/2 = 1.25      │       │
│  │    → 写入 VMCS: TSC_MULTIPLIER = 1.25             │       │
│  │    → Guest 仍然看到 2.5 GHz 的 TSC (频率不变!)   │       │
│  │                                                    │       │
│  │  ★ 这就是 TSC scaling 的核心作用:                  │       │
│  │    VMM 不需要关心目标 Host 频率是多少,            │       │
│  │    只需要传入 Guest 应该看到的频率,               │       │
│  │    KVM 自动计算 scaling 值                        │       │
│  └────────────────────────────────────────────────────┘       │
│                                                                  │
│  情况 3: 目标 Host 不支持 TSC scaling                         │
│  ┌────────────────────────────────────────────────────┐       │
│  │  如果目标 Host CPU 不支持 TSC scaling 硬件:       │       │
│  │    → 请求频率 > 硬件频率: 成功, 走软件 catchup   │       │
│  │      (kvm_guest_time_update 按墙上时间计算      │       │
│  │       应有 TSC 并调 offset 追平,               │       │
│  │       x86.c:3277-3283)                          │       │
│  │    → 请求频率 ≤ 硬件频率: 无法降速模拟,        │       │
│  │      ioctl 返回 -EINVAL (x86.c:6173-6190)       │       │
│  │    → Guest 感知到频率变化 (应用可能受影响)        │       │
│  │                                                    │       │
│  │  这是为什么迁移前要检查 CPU 兼容性                │       │
│  └────────────────────────────────────────────────────┘       │
└──────────────────────────────────────────────────────────────────┘

┌─ KVM 内部计算 ────────────────────────────────────────────────┐
│                                                                  │
│  KVM_SET_TSC_KHZ(khz) 触发:                                   │
│    1. 计算 tsc_scaling_ratio = khz × 2^48 / host_tsc_khz     │
│       (48位定点格式, x86.c:2452; AMD SVM 为 2^32)            │
│    2. vmcs_write64(TSC_MULTIPLIER, tsc_scaling_ratio)         │
│                                                                  │
│  KVM_SET_MSRS(MSR_IA32_TSC, value) 触发:                     │
│    1. 计算 tsc_offset = value - host_tsc                      │
│       (考虑 scaling: value = host_tsc × multiplier + offset)  │
│    2. vmcs_write64(TSC_OFFSET, tsc_offset)                    │
│                                                                  │
│  所以: TSC_MULTIPLIER 和 TSC_OFFSET 都是 KVM 根据           │
│        guest_tsc_khz 和 guest_tsc_value 计算出来的,          │
│        不直接保存在快照中                                     │
└──────────────────────────────────────────────────────────────────┘
```

跳变治理的三条原则:

┌─ 原则一: MONOTONIC 绝不推进 ─────────────────────────────────┐
│  KVM_SET_CLOCK 时 flags 必须为 0                              │
│                                                                │
│  这就是 Firecracker PR #5809 的全部内容:                      │
│    · 恢复时 clock.flags = 0, 不信任快照中的 flags             │
│    · 防止 KVM_CLOCK_REALTIME 导致单调钟跳变                  │
│                                                                │
│  为什么后果特别严重:                                          │
│    kvmclock 污染的是地基                                      │
│    它不只是 clocksource                                       │
│    kvmclock_init() 还把它注册成了 sched_clock                 │
│    所以即便 guest 把 clocksource 选成了 tsc                   │
│    kvmclock 跳变仍会影响:                                     │
│      · printk 时间戳                                          │
│      · 调度统计                                               │
│      · softlockup 判定                                        │
│    牵连面比 "只影响 clocksource" 大得多                       │
└────────────────────────────────────────────────────────────────┘

┌─ 原则二: 告知暂停, 而非伪造时间 ────────────────────────────┐
│  KVM_KVMCLOCK_CTRL → PVCLOCK_GUEST_STOPPED                  │
│    → guest 主动宽恕这段空白                                 │
│                                                                │
│  不是伪造时间让 guest 以为自己一直在跑                        │
│  而是告知事实并让它宽恕这段空白                              │
└────────────────────────────────────────────────────────────────┘

┌─ 原则三: REALTIME 必须在 guest 侧尽早校准 ──────────────────┐
│  不校的后果是硬伤:                                          │
│    · TLS 证书校验失败                                       │
│    · token/Kerberos 票据过期                                │
│    · 和外部系统时间戳对不上                                 │
│    · 日志乱序                                               │
│                                                                │
│  方案对比:                                                  │
│    · ptp_kvm + chrony: 亚微秒, 推荐默认                     │
│    · hwclock --hctosys: 1 秒, 精简 rootfs 兜底              │
│    · 网络 NTP: 毫秒级, 长期兜底                             │
│                                                                │
│  执行时机: resume → 校时 → 再放业务流量                     │
│            不要等业务跑起来了才跳墙钟                        │
└────────────────────────────────────────────────────────────────┘

┌─ 快照恢复后的立即墙上时间同步 (三种方案) ─────────────────┐
│                                                                │
│  方案 1: QEMU Guest Agent (QGA) — 立即同步                   │
│  ┌────────────────────────────────────────────────────┐       │
│  │  Host 侧:                                         │       │
│  │    快照恢复后, QEMU 通过 QGA socket 调用:          │       │
│  │    { "execute": "guest-set-time",                  │       │
│  │      "arguments": { "time": <nanoseconds> } }      │       │
│  │                                                    │       │
│  │  Guest 内:                                        │       │
│  │    qemu-ga 收到 → clock_settime(CLOCK_REALTIME)   │       │
│  │    → 墙上时间立即校正                              │       │
│  │                                                    │       │
│  │  优点: 立即生效, 不依赖 NTP 周期                  │       │
│  │  缺点: 需要 qemu-ga; 直接 step 跳变               │       │
│  └────────────────────────────────────────────────────┘       │
│                                                                │
│  方案 2: KVM_KVMCLOCK_CTRL + PVCLOCK_GUEST_STOPPED            │
│  ┌────────────────────────────────────────────────────┐       │
│  │  Host 侧 (QEMU resume 流程):                      │       │
│  │    1. KVM_SET_CLOCK       ← 恢复 kvmclock_offset  │       │
│  │    2. KVM_KVMCLOCK_CTRL   ← 设置 GUEST_STOPPED    │       │
│  │         │                                          │       │
│  │         └─ kvm_set_guest_paused():                 │       │
│  │              pvclock_set_guest_stopped_request=true │       │
│  │                                                    │       │
│  │  Guest 内核 (kvmclock.c:143):                     │       │
│  │    读 pvclock 页时检测到 PVCLOCK_GUEST_STOPPED:   │       │
│  │    → pvclock_touch_watchdogs()                    │       │
│  │      (重置 softlockup / RCU stall / hung task)    │       │
│  │                                                    │       │
│  │  作用: 告知内核 "你被暂停过", 重置超时检测器      │       │
│  │  局限: 不直接校正墙上时间, 需要用户态配合         │       │
│  └────────────────────────────────────────────────────┘       │
│                                                                │
│  方案 3: chrony + ptp_kvm — 最高精度                          │
│  ┌────────────────────────────────────────────────────┐       │
│  │  Guest 内 chrony 配置:                            │       │
│  │    refclock PHC /dev/ptp0 poll 0 dpoll -2         │       │
│  │    makestep 0.1 3    ← 前 3 次允许 step           │       │
│  │                                                    │       │
│  │  快照恢复后:                                      │       │
│  │    chrony 通过 ptp_kvm 检测偏差                   │       │
│  │    → 偏差 > threshold → makestep 立即校正         │       │
│  │    → 否则 slew 慢慢对齐                           │       │
│  │                                                    │       │
│  │  手动触发 (可选):                                 │       │
│  │    chronyc makestep   ← 强制立即 step             │       │
│  │                                                    │       │
│  │  优点: 亚微秒精度, 可选择 step/slew               │       │
│  │  缺点: 需要 ptp_kvm + chrony; poll 有延迟        │       │
│  └────────────────────────────────────────────────────┘       │
│                                                                │
│  推荐组合方案:                                                │
│  ┌────────────────────────────────────────────────────┐       │
│  │  Host: QEMU resume → KVM_SET_CLOCK               │       │
│  │                           → KVM_KVMCLOCK_CTRL     │       │
│  │        (可选) QGA guest-set-time ← 立即粗校正     │       │
│  │                                                    │       │
│  │  Guest: 内核检测 GUEST_STOPPED → 重置 watchdog   │       │
│  │         chrony + ptp_kvm → makestep 精确校正      │       │
│  │                                                    │       │
│  │  结果: 毫秒级立即校正 + 亚微秒后续精校            │       │
│  └────────────────────────────────────────────────────┘       │
│                                                                │
│  Guest 内推荐配置:                                            │
│  ┌────────────────────────────────────────────────────┐       │
│  │  # 加载 ptp_kvm                                   │       │
│  │  modprobe ptp_kvm                                 │       │
│  │                                                    │       │
│  │  # /etc/chrony/chrony.conf                        │       │
│  │  refclock PHC /dev/ptp0 poll 0 dpoll -2 offset 0  │       │
│  │  makestep 0.1 3                                   │       │
│  │                                                    │       │
│  │  # 快照恢复脚本 (可选):                           │       │
│  │  # /usr/lib/systemd/system-sleep/kvm-clock-sync   │       │
│  │  #!/bin/sh                                        │       │
│  │  [ "$1" = "post" ] && chronyc makestep 2>/dev/null│       │
│  └────────────────────────────────────────────────────┘       │
└────────────────────────────────────────────────────────────────┘

┌─ 迁移后 NTP/PTP 同步行为 (对照 §0 时钟系统基本概念) ──────┐
│                                                                │
│  迁移后 Guest 时间与真实时间存在偏差:                         │
│    偏差 ≈ 停机时间 (通常毫秒到秒级)                          │
│                                                                │
│  NTP 根据偏差大小选择不同的校正策略:                          │
│                                                                │
│  偏差 < 128ms (step threshold):                               │
│    ┌────────────────────────────────────────────────────┐     │
│    │  REALTIME   → NTP slew: 微调频率, 慢慢对齐          │     │
│    │  MONOTONIC  → 也跟着 slew (共享 tkr_mono.mult)      │     │
│    │             永不跳变, 只可能频率微调                 │     │
│    └────────────────────────────────────────────────────┘     │
│                                                                │
│  偏差 ≥ 128ms:                                                │
│    ┌────────────────────────────────────────────────────┐     │
│    │  REALTIME   → NTP step: 直接跳到正确时间             │     │
│    │  MONOTONIC  → 不受影响 (永不跳变)                   │     │
│    │             只可能后续 slew 微调频率                 │     │
│    └────────────────────────────────────────────────────┘     │
│                                                                │
│  用 PTP KVM + chrony 时:                                      │
│    ┌────────────────────────────────────────────────────┐     │
│    │  亚微秒精度 → 偏差极小 → 几乎总是 slew, 不跳       │     │
│    │  chrony step threshold 可设为 0 → 永不 step         │     │
│    └────────────────────────────────────────────────────┘     │
│                                                                │
│  所以 "MONOTONIC 慢慢对齐, REALTIME 会跳变" 不完全准确:      │
│    · 偏差小时 REALTIME 也不跳 (slew)                          │
│    · 偏差大时 REALTIME 才跳 (step)                            │
│    · MONOTONIC 只可能 slew, 永不 step                         │
└────────────────────────────────────────────────────────────────┘

跨主机迁移的额外问题:

  · TSC 频率不同 → 必须有 TSC scaling 硬件支持
  · host 之间 realtime 有偏差 → 残差需要 guest 侧校
  · backwards_tsc_observed → 永久关掉 masterclock, guest 掉进慢路径
  · flags 兼容性 → 5.16+ 主机打的快照带 flags=0xC
                   灌到 5.10 KVM 上会直接 -EINVAL
```

### 5. Guest 视角的时钟层次

```
Guest Linux 内核的时钟层次:

┌─ clocksource (时间源, 读取当前时间) ──────────────────────────┐
│  按 rating 选优 (不是固定顺序):                                │
│    · tsc        rating 300 (tsc.c:1189)                        │
│    · kvm-clock  rating 400 (kvmclock.c:160); 但 guest TSC 可靠 │
│                 (constant + nonstop + 未标不稳定) 时主动降到   │
│                 299 (kvmclock.c:342-345), 让位给 tsc          │
│    · hpet / acpi_pm / pit  低 rating 兜底                      │
│                                                                │
│  结果: 现代 host 上 guest 默认通常是 tsc;                      │
│        TSC 不可靠时才是 kvm-clock (详见 §4 对比小节)           │
│                                                                │
│  查看: cat /sys/devices/system/clocksource/clocksource0/       │
│         current_clocksource                                    │
└────────────────────────────────────────────────────────────────┘

┌─ clockevent (事件源, 设置定时器) ─────────────────────────────┐
│  优先级从高到低:                                               │
│    1. lapic-deadline ← APIC Timer TSC-deadline 模式 (apic.c:585)│
│    2. lapic          ← APIC Timer one-shot/periodic (apic.c:494)│
│    3. hpet / pit                                              │
│                                                                │
│  查看: cat /sys/devices/system/clockevents/clockevent0/        │
│         current_device                                         │
└────────────────────────────────────────────────────────────────┘

KVM 典型组合:
  clocksource = tsc 或 kvm-clock (rating 决定, 见 §4 对比)
  clockevent  = lapic-deadline (TSC-deadline, preemption timer 加速)
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
    kvm_write_system_time()     ← 注册pvclock页GPA (不写内容!)
    kvm_write_wall_clock()      ← 注册wall clock GPA
    kvm_guest_time_update()     ← 真正填充时间信息到pvclock页

第5步: 理解Timer advance (高级优化)
  arch/x86/kvm/lapic.c
    lapic_timer_advance         ← 模块参数 (bool, 开关)
    timer_advance_ns            ← 内部自动调整值 (非模块参数)
    adjust_lapic_timer_advance() ← 动态调整提前量 (步进式, 非EWMA)
```

---

## 🔬 实践练习

### 练习1: 查看Guest使用的时钟源

```bash
# 在Guest内执行:
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
# 预期: tsc 或 kvm-clock
# (guest TSC 可靠 → tsc; kvm-clock rating 让位机制见 §4 对比小节)

cat /sys/devices/system/clocksource/clocksource0/available_clocksource
# 预期: 含 tsc 和 kvm-clock (其余视虚拟硬件而定)

cat /sys/devices/system/clockevents/clockevent0/current_device
# 预期: lapic-deadline (支持 TSC-deadline 时) 或 lapic

# 查看TSC特征 (/proc/cpuinfo 里的名字, 见 cpufeatures.h):
grep -oE "constant_tsc|nonstop_tsc|tsc_deadline_timer|tsc_known_freq|tsc_adjust" /proc/cpuinfo | sort -u
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
- [ ] 区分 clocksource 与 clockevent，说出 TSC / kvm-clock / LAPIC timer /
      PIT / HPET 各属于哪一类（含双重角色者）
- [ ] 解释为什么 TSC-deadline 是 clockevent（`lapic-deadline`）而不是时钟源
- [ ] 画出 clocksource 演进（PIT → HPET → TSC → kvm-clock）与
      clockevent 演进（PIT → LAPIC timer one-shot/periodic → tsc-deadline）两条线
- [ ] 解释 TSC_OFFSET 和 TSC_MULTIPLIER 在 VMCS 中的作用
- [ ] 说明 TSC-deadline 为什么比 Periodic/One-shot 模式高效
- [ ] 对比 `tsc` 与 `kvm-clock` 两个 clocksource 的读取路径、迁移语义与失效模式，
      说出 rating 400→299 让位逻辑（kvmclock.c:342-345）
- [ ] 说出 VM 启动三个时间基准的首次同步点：`kvm_arch_init_vm` 归零
      `kvmclock_offset`、`kvm_arch_vcpu_postcreate` 强制同步 TSC、
      首次 KVM_RUN 建立 masterclock
- [ ] 解释 vm 迁移时 Guest 时间连续性的保证机制
- [ ] 说明 kvmclock 的 pvclock 协议如何工作
- [ ] 解释 vDSO 在时钟读取中的性能优势
- [ ] 说明 masterclock 优化的原理和触发条件
- [ ] 解释 ptp_kvm 的实现原理 (hypercall 三元组)
- [ ] 说明 PVCLOCK_GUEST_STOPPED 机制的作用
- [ ] 解释 Firecracker PR #5809 的问题和修复 (KVM_CLOCK_REALTIME)
- [ ] 解释为什么现代 Linux Guest 默认 clocksource 往往是 tsc 而非 kvm-clock
- [ ] 列出 Guest 中查看当前时钟源/时钟事件设备的命令

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
/* vmx.c + lapic.c 中配置 */
/* Guest写IA32_TSC_DEADLINE MSR → VM-Exit → KVM处理 */
if (mode == TSC_DEADLINE) {
    /* 1. 存入 apic->lapic_timer.tscdeadline */
    /* 2. vmx_set_hv_timer() → vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, delta) */
    /* 硬件通过 preemption timer 自动比较，到期时 VM-Exit */
}
```

**效果**：
- 减少定时器相关VM-Exit 90%
- 中断延迟降低到~20ns

### 2. Timer Advance（仅 TSC-deadline）

**问题**：host 定时器到期到中断真正注入 guest 之间，存在
hrtimer/preemption-timer 触发 → KVM 处理 → VM-Entry 注入的延迟（约数百 ns～μs）

**解决**：把到期时刻**提前** `timer_advance_ns`，抵消这段延迟。
只服务 TSC-deadline 模式（`lapic.c:62-68` 注释 "tscdeadline mode only"），
两条路径各自实现提前：

```c
/* HW 路径: vmx_set_hv_timer() — vmx.c:8129（节选） */
delta_tsc = max(guest_deadline_tsc, guest_tscl) - guest_tscl;
lapic_timer_advance_cycles = nsec_to_cycles(vcpu, ktimer->timer_advance_ns);
if (delta_tsc > lapic_timer_advance_cycles)
    delta_tsc -= lapic_timer_advance_cycles;   /* ★ deadline 提前 */
else
    delta_tsc = 0;

/* SW 回退路径: start_sw_tscdeadline() — lapic.c:1953（节选） */
expire = ktime_add_ns(now, ns);
expire = ktime_sub_ns(expire, ktimer->timer_advance_ns);  /* ★ 同上 */
```

**配置与观察**：
```bash
# 唯一的模块参数是 bool 开关（权限 0444，运行时只读，
# 只能在模块加载时改: modprobe kvm lapic_timer_advance=0）
cat /sys/module/kvm/parameters/lapic_timer_advance   # 默认 Y

# 提前量本身是每 vCPU 的内部自适应值，不是模块参数，只能只读观察:
cat /sys/kernel/debug/kvm/<pid>-<fd>/vcpu0/lapic_timer_advance_ns
# 初始 1000 (LAPIC_TIMER_ADVANCE_NS_INIT, lapic.c:75)，自适应调整
```

**效果**：
- 抵消定时器注入延迟，让中断尽量贴近 guest 编程的 deadline
- 详见下文"陷阱4"对常见调优误区的澄清

### 3. TSC同步

**问题**：多pCPU的TSC不同步

**解决**：启动时检查TSC同步，标记为unstable

```c
/* x86.c 中实现 */

/* kvm_synchronize_tsc() — x86.c:2717
 * 检查各pCPU的TSC偏移，如果差异过大则标记为不稳定
 */
kvm_synchronize_tsc(vcpu, user_value)
{
    /* ... 检查 TSC 偏移 ... */
    if (kvm_check_tsc_unstable()) {
        /* 不使用纯TSC作为时钟源 */
        kvm->arch.use_master_clock = false;
        /* kvmclock 回退到 system_time 计算 */
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

**原因**：迁移时 KVM_SET_CLOCK 未正确补偿停机时间

**实际代码** (x86.c:7006 `kvm_vm_ioctl_set_clock()`):
```c
// kvm_vm_ioctl_set_clock() 中
// QEMU 在迁移后调用 KVM_SET_CLOCK 补偿停机时间
// 如果补偿不准确，guest 时间会跳变
//
// kvmclock 使用 PVCLOCK_GUEST_STOPPED 标志通知 guest
// pvclock_touch_watchdogs() 重置 watchdog 定时器
```

**解决**：确保 QEMU 正确调用 KVM_SET_CLOCK 补偿迁移停机时间

### 陷阱4：Timer Advance 理解错误

**场景**：定时器延迟不稳定

**症状**：`cyclictest`显示延迟抖动

**常见误解**：试图通过 `echo N > /sys/module/kvm/parameters/lapic_timer_advance_ns` 调优

**实际机制** (lapic.c:1840 `adjust_lapic_timer_advance()`):
- 模块参数：`lapic_timer_advance` (bool, 默认 true) — 开关
- `timer_advance_ns` 是**内部自动调整值**，不是用户可调参数
- 算法：步进式增量调整（非 EWMA），每次调整 1/8
  - 提前到期 → 减小 advance
  - 延迟到期 → 增大 advance
  - 超过 MAX(5000) → 重置为 INIT(1000)
- **仅适用于 TSC-deadline 模式**（Periodic/One-shot 不使用 timer advance）

**正确做法**：
```bash
# 开关 timer advance（bool 模块参数，权限 0444 → 运行时只读，
# 只能在模块加载时指定）
modprobe -r kvm_intel kvm && modprobe kvm lapic_timer_advance=0
# 或内核命令行: kvm.lapic_timer_advance=0

# 观察当前自动调整值（每 vCPU 的 debugfs，只读; debugfs.c:67）
cat /sys/kernel/debug/kvm/<pid>-<fd>/vcpu0/lapic_timer_advance_ns
```
