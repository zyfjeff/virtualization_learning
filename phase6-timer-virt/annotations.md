# 时钟虚拟化源码注释

> 基于 Linux 6.12.93 源码

---

## 1. PIT 数据结构与IO端口模拟

**文件**: `arch/x86/kvm/i8254.h`, `arch/x86/kvm/i8254.c`

```c
/* 来源: arch/x86/kvm/i8254.h */

/*
 * PIT 单个通道的状态
 * 模拟8254硬件的内部寄存器
 */
struct kvm_kpit_channel_state {
    u32 count;              /* 初始装载值 (决定周期) */
    u16 latched_count;      /* 锁存计数值 (读回用) */
    u8 count_latched;       /* 锁存状态 */
    u8 status_latched;      /* 状态锁存 */
    u8 status_count;        /* 状态/计数 */
    u8 read_state;          /* 读状态 (LSB/MSB交替) */
    u8 write_state;         /* 写状态 */
    u8 write_latch;         /* 写锁存 */
    u8 rw_mode;             /* 读/写模式: 0=LSB, 1=MSB, 3=LSB→MSB */
    u8 mode;                /* 工作模式 0-5 */
    u8 bcd;                 /* BCD模式 (通常为0) */
    u8 gate;                /* 门控输入 (Channel 0通常=1) */
    s64 count_load_time;    /* ★ 装载时的Host时间 (ns) */
};

/*
 * PIT 整体状态
 */
struct kvm_kpit_state {
    struct kvm_kpit_channel_state channels[3];  /* 3个通道 */
    atomic_t pit_state_pending;      /* 异步状态更新 */
    int is_periodic;                 /* 是否周期性模式 */
    u32    flags;                    /* 标记 */
    struct hrtimer pit_timer;        /* ★ Host高精度定时器 */
    struct kvm *kvm;
    /* ... */
};

struct kvm_pit {
    struct kvm *kvm;
    struct kvm_kpit_state pit_state;
    spinlock_t lock;
    /* ... */
};
```

### PIT IO端口映射

```
IO端口   功能
──────   ──────────────────────────────────
0x40     Channel 0 数据端口 (系统定时器 → IRQ0)
0x41     Channel 1 数据端口 (DRAM刷新, 已废弃)
0x42     Channel 2 数据端口 (PC扬声器)
0x43     控制字端口 (设置channel/mode/rw)

控制字格式 (写0x43):
┌───┬───┬───┬───┬───┬───┬───┬───┐
│SC1│SC0│RW1│RW0│M2 │M1 │M0 │BCD│
└───┴───┴───┴───┴───┴───┴───┴───┘
SC: Select Channel (00=Ch0, 01=Ch1, 10=Ch2, 11=读回)
RW: Read/Write (00=锁存, 01=LSB, 10=MSB, 11=LSB then MSB)
M:  Mode (000=0, ..., 110=6, 111=7)
    Mode 2 = Rate Generator (最常用)
    Mode 3 = Square Wave Generator

BCD: 0=二进制, 1=BCD
```

### PIT 模拟关键路径

```c
/* 来源: arch/x86/kvm/i8254.c:365 */

/*
 * Guest 写 PIT 计数值
 * 路径: IO端口写 → kvm_pit_ioport_write → pit_load_count
 */
static void pit_load_count(struct kvm_pit *pit, int channel, u32 val)
{
	struct kvm_kpit_state *ps = &pit->pit_state;

	/* 0 表示最大值 0x10000 (2^16) */
	if (val == 0)
		val = 0x10000;

	ps->channels[channel].count = val;

	if (channel != 0) {
		ps->channels[channel].count_load_time = ktime_get();
		return;
	}

	/* ★ Channel 0: 根据模式创建定时器 */
	switch (ps->channels[0].mode) {
	case 0:
	case 1:
	case 4:
		create_pit_timer(pit, val, 0);  /* one-shot 模式 */
		break;
	case 2:
	case 3:
		create_pit_timer(pit, val, 1);  /* periodic 模式 */
		break;
	default:
		destroy_pit_timer(pit);         /* 其他模式: 停止定时器 */
	}
}

/*
 * create_pit_timer - 创建PIT定时器
 *
 * 实际实现中KVM使用 hrtimer 模拟，但通过 create_pit_timer 封装:
 *   - 计算超时时间: timeout_ns = val × 10^9 / PIT_FREQ (1193182 Hz)
 *   - 启动 hrtimer
 *   - periodic模式下 hrtimer 回调中自动重启
 */
```

/*
 * Host hrtimer 到期回调
 * 模拟硬件计数到0后触发IRQ
 */
static enum hrtimer_restart kvm_pit_timer_expired(struct hrtimer *timer)
{
    struct kvm_pit *pit = container_of(timer, ...);
    
    /* 注入 IRQ0 到 PIC/IOAPIC */
    kvm_set_irq(pit->kvm, KVM_PIT_CHANNEL_MASK, 0, 1, 1);
    
    /* 如果是periodic模式: 重新启动hrtimer */
    if (pit->pit_state.is_periodic) {
        hrtimer_add_expires_ns(timer, period_ns);
        return HRTIMER_RESTART;
    }
    return HRTIMER_NORESTART;
}

/*
 * Guest 读 PIT 计数值
 * 不启动新的定时器, 而是根据时间差计算当前计数
 */
static int pit_get_count(struct kvm_pit *pit, int channel)
{
    struct kvm_kpit_channel_state *c = &pit->pit_state.channels[channel];
    u64 elapsed_ns = ktime_get_ns() - c->count_load_time;
    
    /* 计算经过了几个计数周期 */
    u64 elapsed_counts = div_u64(elapsed_ns * 1193182, 1000000000ULL);
    
    /* 当前计数 = 初始值 - 经过的计数 */
    u32 counter;
    if (c->mode == 2 || c->mode == 3) {  /* 周期性 */
        counter = c->count - (elapsed_counts % c->count);
    } else {
        counter = c->count - elapsed_counts;
        if (counter > c->count) counter = 0;  /* 已减到0 */
    }
    return counter;
}
```

---

## 2. APIC Timer 详细实现

**文件**: `arch/x86/kvm/lapic.c`

### Timer 配置寄存器

```
APIC Timer 相关寄存器:

偏移   名称               功能
────   ────               ────
0x320  APIC_LVTT          LVT Timer Register
                         [17:16] = Timer Mode:
                           00 = 固定 (Fixed)
                           01 = SMI
                           10 = NMI
                           11 = INIT
                         [18]    = TSC-deadline模式位
                         [7:0]   = 中断向量

0x380  APIC_TMICT         Initial Count Register (初始计数值)
                         写入后开始倒计时

0x390  APIC_TMCCT         Current Count Register (当前计数值, 只读)
                         Guest读这个获取剩余计数

0x3E0  APIC_TDCR          Timer Divide Configuration
                         [3:0] = 除数选择:
                           0000 = /2, 0001 = /4, ..., 1011 = /128
```

### 三种模式详细对比

```
┌─ Periodic ──────────────────────────────────────────────────────┐
│  写: APIC_LVTT[17:16]=00, APIC_TMICT=N, APIC_TDCR=divide      │
│  行为: count从N递减到0 → 中断 → 自动重载N → 重复             │
│  频率: freq = bus_freq / (divide × N)                          │
│  VM-Exit: 写TMICT/TDCR时Exit, 倒计时期间不Exit                │
│  KVM: hrtimer模拟, period = N × divide / bus_freq             │
└────────────────────────────────────────────────────────────────┘

┌─ One-shot ──────────────────────────────────────────────────────┐
│  写: APIC_LVTT[17:16]=00, APIC_TMICT=N                        │
│  行为: count从N递减到0 → 中断一次 → 停止 (count=0后不变)      │
│  KVM: hrtimer模拟, 不重载                                      │
│  用途: tickless内核 (NO_HZ), 只在需要时设下一个定时器          │
└────────────────────────────────────────────────────────────────┘

┌─ TSC-deadline (最高效!) ────────────────────────────────────────┐
│  写: APIC_LVTT[18]=1, IA32_TSC_DEADLINE_MSR=deadline          │
│  行为: 硬件比较 RDTSC vs deadline                              │
│        当 RDTSC >= deadline → 触发中断                          │
│        deadline自动清零 (one-shot语义, 需重新写deadline)       │
│                                                                 │
│  VM-Exit: 只写MSR时一次Exit! 无周期Exit                       │
│  KVM: 直接写VMCS TSC_DEADLINE字段, 硬件自动处理             │
│       vmcs_write64(TSC_DEADLINE, guest_deadline_to_host)      │
│  优势:                                                         │
│    - 不需要TMICT/TDCR (减少VM-Exit次数!)                       │
│    - 硬件比较, 精度最高                                         │
│    - 可以用VMCS的预取定时器加速                                 │
└────────────────────────────────────────────────────────────────┘
```

### KVM APIC Timer 实现

```c
/* 来源: arch/x86/kvm/lapic.c:2269 */

/*
 * start_apic_timer - 启动APIC Timer
 *
 * 当Guest写APIC_TMICT (或TSC_DEADLINE MSR) 时调用
 * 调用链: start_apic_timer → __start_apic_timer → restart_apic_timer
 *         → start_hv_timer() (优先硬件定时器) 或 start_sw_timer() (回退hrtimer)
 */
static void start_apic_timer(struct kvm_lapic *apic)
{
	__start_apic_timer(apic, APIC_TMICT);
}

/* lapic.c:2258 */
static void __start_apic_timer(struct kvm_lapic *apic, u32 count_reg)
{
	atomic_set(&apic->lapic_timer.pending, 0);

	/* Periodic/One-shot: 设置目标过期时间 */
	if ((apic_lvtt_period(apic) || apic_lvtt_oneshot(apic))
	    && !set_target_expiration(apic, count_reg))
		return;

	/* TSC-deadline 或重新计时 */
	restart_apic_timer(apic);
}

/* lapic.c:2200 */
static void restart_apic_timer(struct kvm_lapic *apic)
{
	preempt_disable();

	if (!apic_lvtt_period(apic) && atomic_read(&apic->lapic_timer.pending))
		goto out;

	/* ★ 优先尝试硬件定时器 (VMX preemption timer) */
	if (!start_hv_timer(apic))
		start_sw_timer(apic);     /* 回退到软件 hrtimer */
out:
	preempt_enable();
}

/*
 * apic_timer_fn - hrtimer 到期回调
 *
 * 来源: lapic.c:2883
 *
 * 通过 apic_timer_expired() 注入中断到vCPU
 * (不是直接调用 kvm_apic_local_deliver)
 */
static enum hrtimer_restart apic_timer_fn(struct hrtimer *data)
{
	struct kvm_timer *ktimer = container_of(data, struct kvm_timer, timer);
	struct kvm_lapic *apic = container_of(ktimer, struct kvm_lapic, lapic_timer);

	/* ★ 注入定时器中断到vCPU */
	apic_timer_expired(apic, true);

	if (lapic_is_periodic(apic) && !WARN_ON_ONCE(!apic->lapic_timer.period)) {
		advance_periodic_target_expiration(apic);
		hrtimer_set_expires(&ktimer->timer, ktimer->target_expiration);
		return HRTIMER_RESTART;
	} else
		return HRTIMER_NORESTART;
}

/*
 * 定时器优先级:
 *   1. VMX preemption timer (start_hv_timer) → 最快, 硬件直接触发VM-Exit
 *   2. hrtimer (start_sw_timer → start_sw_period/start_sw_tscdeadline)
 *   3. 如果两种都失败, 使用 pending 标志延迟注入
 */
```

---

## 3. TSC 虚拟化详细实现

**文件**: `arch/x86/kvm/vmx/vmx.c:1951` (vmx_write_tsc_offset), `arch/x86/kvm/x86.c:2717` (kvm_synchronize_tsc)

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:1951-1959 */

/*
 * TSC_OFFSET 写入 VMCS
 * Guest RDTSC = Host TSC + TSC_OFFSET
 *
 * 何时调用:
 *   - vCPU创建时
 *   - vCPU迁移到不同pCPU时 (如果TSC频率不同)
 *   - 用户空间通过 KVM_SET_TSC_OFFSET 设置时
 */
void vmx_write_tsc_offset(struct kvm_vcpu *vcpu)
{
    vmcs_write64(TSC_OFFSET, vcpu->arch.tsc_offset);
}

/*
 * TSC_MULTIPLIER 写入 VMCS
 * Guest RDTSC = (Host TSC × TSC_MULTIPLIER) + TSC_OFFSET
 *
 * 用途:
 *   - 嵌套虚拟化: L2 Guest需要看到L1的TSC频率
 *   - 迁移兼容: 不同Host CPU频率不同, 缩放统一
 *
 * 格式: 48位小数 + 整数位
 *   1.0 = 0x0000000100000000
 */
void vmx_write_tsc_multiplier(struct kvm_vcpu *vcpu)
{
    vmcs_write64(TSC_MULTIPLIER, vcpu->arch.tsc_scaling_ratio);
}

/* 来源: arch/x86/kvm/x86.c */

/*
 * 同步所有vCPU的TSC (概念性描述)
 *
 * 来源: arch/x86/kvm/x86.c:2717
 * 签名: static void kvm_synchronize_tsc(struct kvm_vcpu *vcpu, u64 *user_value)
 *
 * 调用时机:
 *   - vCPU创建时 (user_value=NULL, data=0, 强制同步)
 *   - 用户空间通过 KVM_SET_TSC_OFFSET 设置时
 *   - vCPU从不同pCPU迁移回来时
 *
 * 策略: 以"最早"的TSC为基准, 其他vCPU通过offset对齐
 * 内部调用 __kvm_synchronize_tsc() → kvm_compute_l1_tsc_offset()
 */
void kvm_synchronize_tsc(struct kvm *kvm, u64 data)
{
    /* 遍历所有vCPU */
    for each vcpu:
        /* 计算每个vCPU需要的offset */
        offset = kvm_compute_current_tsc_offset(vcpu, master_tsc);
        vcpu->arch.tsc_offset = offset;
        
        /* 写入VMCS */
        kvm_x86_call(write_tsc_offset)(vcpu);
}

/*
 * Guest TSC 读取
 * 在VMCS中: guest_tsc = host_tsc + TSC_OFFSET
 * 硬件自动处理, KVM不需要拦截RDTSC
 * 
 * 但如果Guest使用 TSC scaling:
 *   guest_tsc = (host_tsc × multiplier) + offset
 *   乘法由硬件完成 (如果CPU支持TSC scaling)
 */
```

### TSC 稳定性检测

```c
/* 来源: arch/x86/kvm/x86.c */

/*
 * KVM 在初始化时检测TSC稳定性
 * 如果不稳定, 标记 kvm_clock_is_unstable
 * 影响: Guest可能不使用TSC作为clocksource
 */

/* 检测条件: */
/* 1. Invariant TSC: CPUID.80000007H:EDX[8] */
/*    现代CPU几乎都支持 */

/* 2. TSC constant: CPU频率不随变频改变 */
/*    boot_cpu_has(X86_FEATURE_CONSTANT_TSC) */

/* 3. 非AMD的TSC缩放问题 */
/*    AMD CPU可能有TSC缩放不一致 */
```

---

## 4. kvmclock 详细实现

**文件**: `arch/x86/kvm/x86.c:2354` (kvm_write_system_time), `arch/x86/kvm/x86.c:3215` (kvm_guest_time_update)

```c
/* 来源: arch/x86/kvm/x86.c:2354 */

/*
 * 更新 pvclock_vcpu_time_info (kvmclock 的核心结构)
 * 
 * Guest通过 MSR 0x4B564D01 (MSR_KVM_SYSTEM_TIME) 指定
 * 共享内存的物理地址。KVM将时间信息写入这个地址。
 * Guest直接读内存获取时间, 无需VM-Exit。
 */
static void kvm_write_system_time(struct kvm_vcpu *vcpu, gpa_t system_time)
{
    /* 映射Guest物理地址到Host虚拟地址 */
    struct pvclock_vcpu_time_info *pvti;
    pvti = kvm_vcpu_gfn_to_hva_cache(vcpu, gpa_to_gfn(system_time), ...);
    
    /* 写入时间信息 */
    struct pvclock_vcpu_time_info hv_clock;
    hv_clock.tsc_timestamp = kvm_read_l1_tsc(vcpu, host_tsc);
    hv_clock.system_time = now_ns;  /* Host monotonic时间 */
    hv_clock.tsc_to_system_mul = clock_tsc_multiplier;
    hv_clock.tsc_shift = clock_tsc_shift;
    hv_clock.flags = KVM_CLOCK_TSC_STABLE;
    
    /* 版本控制: 奇数=更新中, 偶数=更新完成 */
    hv_clock.version = pvti->version + 1;
    smp_wmb();  /* 确保数据先写 */
    
    /* 拷贝到Guest内存 */
    kvm_write_guest(vcpu->kvm, system_time, &hv_clock, sizeof(hv_clock));
    
    hv_clock.version++;  /* 变成偶数 */
    smp_wmb();
    kvm_write_guest(vcpu->kvm, system_time, &hv_clock, sizeof(u32));
}

/*
 * pvclock_vcpu_time_info 结构 (Guest和Host共享)
 * 定义于: arch/x86/include/uapi/asm/kvm_para.h
 */
struct pvclock_vcpu_time_info {
    u32   version;           /* 版本号 (seqlock风格) */
    u32   pad0;
    u64   tsc_timestamp;     /* TSC at this update */
    u64   system_time;       /* System time at this update (ns) */
    u32   tsc_to_system_mul; /* TSC → ns 缩放因子 */
    s8    tsc_shift;         /* TSC 移位调整 */
    u16   flags;             /* KVM_CLOCK_TSC_STABLE 等 */
    u8    pad[3];
} __attribute__((__packed__));

/*
 * Guest 读取当前时间的算法 (在Guest内核中):
 *
 * 1. do { version = pvti->version; } while (version & 1);
 *    // 等待更新完成 (version为偶数)
 *
 * 2. current_tsc = RDTSC;
 *    delta_tsc = current_tsc - pvti->tsc_timestamp;
 *
 * 3. delta_ns = (delta_tsc * pvti->tsc_to_system_mul) >> (22 + tsc_shift);
 *    // 将TSC差值转换为纳秒
 *
 * 4. current_ns = pvti->system_time + delta_ns;
 *    // 加上基准时间
 *
 * 5. } while (version != pvti->version);
 *    // 如果更新中途读取了, 重试
 *
 * 整个过程: 0次VM-Exit! 纯内存读 + 算术运算
 */

/*
 * wall clock 结构 (年月日时分秒)
 * Guest通过 MSR_KVM_WALL_CLOCK 指定共享地址
 */
struct pvclock_wall_clock {
    u32   version;
    u32   sec;       /* 自1970-01-01的秒数 */
    u32   nsec;      /* 纳秒部分 */
};
```

---

## 5. Timer Advance 优化 (高级)

```c
/* 来源: arch/x86/kvm/lapic.c */

/*
 * Timer Advance: 提前触发hrtimer补偿延迟
 *
 * 问题:
 *   Guest设置deadline = T1
 *   KVM收到deadline → hrtimer_start(T1)
 *   Host hrtimer到期 = T1 + delta1  (hrtimer有误差)
 *   KVM处理 = T1 + delta1 + delta2  (VM-Exit处理延迟)
 *   Guest收到中断 = T1 + delta1 + delta2 + delta3  (VM-Entry延迟)
 *   总延迟 = delta1 + delta2 + delta3 ≈ 几百ns到几μs
 *
 * 对于对时间敏感的Guest (如实时应用), 这个延迟不可接受
 *
 * 解决: 提前一点触发hrtimer
 *   hrtimer_start(T1 - advance_ns)
 *   这样Guest收到中断 ≈ T1
 *
 * 参数: lapic_timer_advance_ns
 *   默认: true (自动调整advance_ns)
 *   自动调整: 根据历史延迟动态更新 advance_ns
 *
 * 注意: 仅适用于 Periodic/One-shot 模式
 *       TSC-deadline 模式由硬件处理, 不需要advance
 */
static bool lapic_timer_advance __read_mostly = true;
module_param(lapic_timer_advance, bool, 0444);

static u32 lapic_timer_advance_ns __read_mostly = 0;
/* 0 = 自动调整 */

/*
 * 动态调整算法 (adjust_timer_advance_ns):
 *
 * 记录每次:
 *   期望时间: deadline
 *   实际收到时间: actual_time
 *   延迟: actual_time - deadline
 *
 * 用指数加权移动平均 (EWMA) 更新 advance_ns:
 *   advance_ns = advance_ns * 0.9 + delay * 0.1
 */
```

---

## 6. 各时钟源 KVM 数据结构总览

```
时钟虚拟化数据结构:

┌─ struct kvm (VM级别) ──────────────────────────────────────┐
│                                                              │
│  arch                                                       │
│  ├── pit ─────────▶ struct kvm_pit  (PIT模拟)              │
│  │   └── pit_state.channels[3] (3个channel状态)           │
│  │                                                          │
│  ├── kvmclock (全局时间信息)                               │
│  │   ├── pvclock_vcpu_time_info (每个vCPU一份)             │
│  │   └── pvclock_wall_clock (全局wall clock)              │
│  │                                                          │
│  └── tsc相关                                               │
│      ├── tsc_default (默认TSC值, 用于同步)                │
│      └── kvmclock_update (定期更新时间信息)               │
│                                                              │
└──────────────────────────────────────────────────────────────┘

┌─ struct kvm_vcpu (vCPU级别) ───────────────────────────────┐
│                                                              │
│  arch                                                       │
│  ├── apic ─────────▶ struct kvm_lapic                     │
│  │   └── lapic_timer                                       │
│  │       ├── timer (Host hrtimer, Periodic/One-shot)      │
│  │       ├── period (周期时间)                              │
│  │       ├── timer_mode (Periodic/One-shot/TSC-deadline)  │
│  │       ├── pending (是否有待注入的中断)                  │
│  │       └── hv_timer_in_use (是否用VMCS预取定时器)       │
│  │                                                          │
│  ├── tsc_offset ──── TSC偏移量 (写入VMCS)                │
│  └── tsc_scaling_ratio ── TSC缩放比 (写入VMCS)          │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 7. 关键模块参数

```
KVM 时钟相关模块参数 (kvm/kvm_intel):

lapic_timer_advance     [bool]    启用timer advance (默认1)
lapic_timer_advance_ns  [uint]    advance量 (0=自动, 默认0)
tsc_tolerance_ppm       [uint]    TSC容差 (ppm, 默认0)
kvmclock_periodic_sync  [bool]    定期同步kvmclock (默认1)

查看: /sys/module/kvm/parameters/
      /sys/module/kvm_intel/parameters/
```
