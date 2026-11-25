# Linux 时钟系统基础与硬件时钟源

> 承接：[README](README.md) 第一性原理（guest 怎么知道时间？）。
> 本文回答：Linux clocksource/clockevent 框架、硬件时钟源（TSC/HPET/PIT/APIC Timer）全景。
> 引出：KVM 怎么虚拟化这些时钟 → [kvm-clock-virtualization.md](kvm-clock-virtualization.md)。
>
> 本文自 README 拆出（2026-11-25 框架拆分），内容未改。

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

