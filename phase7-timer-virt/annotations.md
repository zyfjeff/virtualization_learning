# 时钟虚拟化源码注释

> 基于 Linux 6.12.93 源码。所有 `path:line` 均已逐条核对；
> 修正历史见 [`corrections.md`](corrections.md)（勘误 2/3/5/10/11/12/13/14/17/18
> 直接对应本文档的旧版错误）。

---

## 1. PIT 数据结构与 IO 端口模拟

**文件**: `arch/x86/kvm/i8254.h`, `arch/x86/kvm/i8254.c`

```c
/* 来源: arch/x86/kvm/i8254.h:9-49（字段行号随文标注） */

/* PIT 单个通道的状态 */
struct kvm_kpit_channel_state {
	u32 count;              /* 初始装载值，可以是 65536 */
	u16 latched_count;      /* 锁存计数值 */
	u8 count_latched;       /* 锁存状态 */
	u8 status_latched;
	u8 status;              /* i8254.h:14（旧版文档误写 status_count） */
	u8 read_state;          /* 读状态（LSB/MSB 交替） */
	u8 write_state;
	u8 write_latch;
	u8 rw_mode;             /* 读/写模式 */
	u8 mode;                /* 工作模式 0-5 */
	u8 bcd;                 /* not supported */
	u8 gate;                /* timer start */
	ktime_t count_load_time; /* i8254.h:22（旧版误写 s64） */
};

/* PIT 整体状态 */
struct kvm_kpit_state {
	/* "struct mutex lock" 之前的成员都受该 lock 保护 */
	struct kvm_kpit_channel_state channels[3];
	u32 flags;
	bool is_periodic;       /* i8254.h:29（旧版误写 int） */
	s64 period;             /* 周期，单位 ns */
	struct hrtimer timer;   /* i8254.h:31 —— 字段名是 timer，不是 pit_timer */

	struct mutex lock;
	atomic_t reinject;      /* 旧版文档的 pit_state_pending 不存在 */
	atomic_t pending;       /* 累积已触发未注入的定时器 */
	atomic_t irq_ack;
	struct kvm_irq_ack_notifier irq_ack_notifier;
};

struct kvm_pit {
	struct kvm_io_device dev;
	struct kvm_io_device speaker_dev;
	struct kvm *kvm;
	struct kvm_kpit_state pit_state;
	int irq_source_id;
	struct kvm_irq_mask_notifier mask_notifier;
	struct kthread_worker *worker;   /* 中断注入推迟到内核线程 */
	struct kthread_work expired;
};

#define KVM_PIT_BASE_ADDRESS     0x40      /* i8254.h:51 */
#define KVM_SPEAKER_BASE_ADDRESS 0x61      /* i8254.h:52 */
#define KVM_PIT_FREQ             1193181   /* i8254.h:54 —— 不是 1193182 */
```

### PIT IO 端口映射

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

写路径：`pit_ioport_write()`（`i8254.c:438`）解析控制字与数据，数据写满后调
`pit_load_count()`：

```c
/* 来源: arch/x86/kvm/i8254.c:365 */
static void pit_load_count(struct kvm_pit *pit, int channel, u32 val)
{
	struct kvm_kpit_state *ps = &pit->pit_state;

	/* 0 表示最大值 0x10000 */
	if (val == 0)
		val = 0x10000;

	ps->channels[channel].count = val;

	if (channel != 0) {
		ps->channels[channel].count_load_time = ktime_get();
		return;
	}

	/* ★ Channel 0: 根据模式创建/销毁定时器 */
	switch (ps->channels[0].mode) {
	case 0:
	case 1:
	case 4:
	case 5:
		create_pit_timer(pit, val, 0);   /* one-shot */
		break;
	case 2:
	case 3:
		create_pit_timer(pit, val, 1);   /* periodic */
		break;
	default:
		destroy_pit_timer(pit);
	}
}
```

`create_pit_timer()` 把计数值换算成纳秒周期，并用 **min_period 限流**防止
guest 用极小周期打爆 host hrtimer：

```c
/* 来源: arch/x86/kvm/i8254.c:322（节选） */
static void create_pit_timer(struct kvm_pit *pit, u32 val, int is_period)
{
	...
	interval = mul_u64_u32_div(val, NSEC_PER_SEC, KVM_PIT_FREQ);  /* :332 */
	...
	ps->period = interval;
	ps->is_periodic = is_period;
	kvm_pit_reset_reinject(pit);

	if (ps->is_periodic) {
		s64 min_period = min_timer_period_us * 1000LL;  /* 默认 200μs, x86.c:160 */
		if (ps->period < min_period) {
			...
			ps->period = min_period;   /* 周期被抬高 */
		}
	}

	hrtimer_start(&ps->timer, ktime_add_ns(ktime_get(), interval),
		      HRTIMER_MODE_ABS);
}
```

hrtimer 到期回调只做记账和排队，**中断注入推迟到内核线程**：

```c
/* 来源: arch/x86/kvm/i8254.c:268 */
static enum hrtimer_restart pit_timer_fn(struct hrtimer *data)
{
	struct kvm_kpit_state *ps = container_of(data, struct kvm_kpit_state, timer);
	struct kvm_pit *pt = pit_state_to_pit(ps);

	if (atomic_read(&ps->reinject))
		atomic_inc(&ps->pending);

	kthread_queue_work(pt->worker, &pt->expired);

	if (ps->is_periodic) {
		hrtimer_add_expires_ns(&ps->timer, ps->period);
		return HRTIMER_RESTART;
	} else
		return HRTIMER_NORESTART;
}

/* 真正的注入在 pit_do_work()（i8254.c:240）: */
	kvm_set_irq(kvm, pit->irq_source_id, 0, 1, false);   /* :251 拉高 IRQ0 */
	kvm_set_irq(kvm, pit->irq_source_id, 0, 0, false);   /* :252 拉低（脉冲） */
```

读路径：`pit_ioport_read()`（`i8254.c:513`）按需算出当前计数，不推进定时器：

```c
/* 来源: arch/x86/kvm/i8254.c:115 */
static int pit_get_count(struct kvm_pit *pit, int channel)
{
	struct kvm_kpit_channel_state *c = &pit->pit_state.channels[channel];
	s64 d, t;
	int counter;

	t = kpit_elapsed(pit, c, channel);
	d = mul_u64_u32_div(t, KVM_PIT_FREQ, NSEC_PER_SEC);  /* 流逝的计数 */

	switch (c->mode) {
	case 0:
	case 1:
	case 4:
	case 5:
		counter = (c->count - d) & 0xffff;
		break;
	case 3:
		/* XXX: may be incorrect for odd counts */
		counter = c->count - (mod_64((2 * d), c->count));
		break;
	default:
		counter = c->count - mod_64(d, c->count);
		break;
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
                         [18:17] = Timer Mode:
                           00 = One-shot
                           01 = Periodic   (APIC_LVT_TIMER_PERIODIC, 1<<17)
                           10 = TSC-deadline (APIC_LVT_TIMER_TSCDEADLINE, 2<<17)
                         [16]    = Masked (APIC_LVT_MASKED, 1<<16)
                         [7:0]   = 中断向量
                         (来源: arch/x86/include/asm/apicdef.h:107-110)

0x380  APIC_TMICT         Initial Count Register (初始计数值)
                         写入后开始倒计时 (Periodic/One-shot)

0x390  APIC_TMCCT         Current Count Register (当前计数值, 只读)

0x3E0  APIC_TDCR          Timer Divide Configuration
                         [3:0] = 除数选择
```

### 三种模式详细对比

```
┌─ Periodic ──────────────────────────────────────────────────────┐
│  写: APIC_LVTT[18:17]=01, APIC_TMICT=N, APIC_TDCR=divide       │
│  行为: count从N递减到0 → 中断 → 自动重载N → 重复               │
│  VM-Exit: 写TMICT/TDCR时Exit, 倒计时期间不Exit                 │
│  KVM: hrtimer模拟, period = N × divide / bus_freq              │
└────────────────────────────────────────────────────────────────┘

┌─ One-shot ──────────────────────────────────────────────────────┐
│  写: APIC_LVTT[18:17]=00, APIC_TMICT=N                         │
│  行为: count从N递减到0 → 中断一次 → 停止                       │
│  KVM: hrtimer模拟, 不重载                                       │
│  用途: tickless内核 (NO_HZ), 只在需要时设下一个定时器           │
└────────────────────────────────────────────────────────────────┘

┌─ TSC-deadline (最高效!) ────────────────────────────────────────┐
│  写: APIC_LVTT[18:17]=10, IA32_TSC_DEADLINE MSR=deadline       │
│  行为: 硬件比较 TSC vs deadline                                 │
│        当 TSC >= deadline → 触发中断                            │
│        到期后需重新写 deadline (one-shot 语义)                  │
│                                                                 │
│  VM-Exit: 只有写MSR时一次! 无周期Exit                          │
│  KVM: ★ 不存在 TSC_DEADLINE 这个 VMCS 字段。两步分工:          │
│    1. vmx_set_hv_timer() (vmx.c:8129) 算出扣除 timer-advance、  │
│       经 TSC scaling 换算的 host deadline, 存                  │
│       vmx->hv_deadline_tsc                                     │
│    2. vmx_update_hv_timer() (vmx.c:7205) 在 VM-Entry 前        │
│       vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, delta_tsc)      │
│       (:7212/:7223/:7226), 用 VMX preemption timer 硬件触发     │
│    3. delta 装不进 32 位时回退软件路径                          │
│       start_sw_tscdeadline() (lapic.c:1953) 用 hrtimer          │
│  优势:                                                         │
│    - 不需要TMICT/TDCR (减少VM-Exit次数!)                        │
│    - 硬件比较, 精度最高                                         │
└────────────────────────────────────────────────────────────────┘
```

### KVM APIC Timer 实现

```c
/* 来源: arch/x86/kvm/lapic.c:2269 */
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
	if (!start_hv_timer(apic))        /* lapic.c:2141 */
		start_sw_timer(apic);     /* 回退到软件路径 */
out:
	preempt_enable();
}

/*
 * apic_timer_fn - hrtimer 到期回调
 * 来源: lapic.c:2883
 */
static enum hrtimer_restart apic_timer_fn(struct hrtimer *data)
{
	struct kvm_timer *ktimer = container_of(data, struct kvm_timer, timer);
	struct kvm_lapic *apic = container_of(ktimer, struct kvm_lapic, lapic_timer);

	apic_timer_expired(apic, true);

	if (lapic_is_periodic(apic) && !WARN_ON_ONCE(!apic->lapic_timer.period)) {
		advance_periodic_target_expiration(apic);
		hrtimer_set_expires(&ktimer->timer, ktimer->target_expiration);
		return HRTIMER_RESTART;
	} else
		return HRTIMER_NORESTART;
}

/*
 * 定时器路径优先级:
 *   1. VMX preemption timer (start_hv_timer, lapic.c:2141)
 *      → 到期即硬件 VM-Exit, kvm_lapic_expired_hv_timer() (lapic.c:2213) 处理
 *   2. hrtimer (start_sw_timer → start_sw_period lapic.c:2105 /
 *      start_sw_tscdeadline lapic.c:1953)
 */
```

---

## 3. TSC 虚拟化详细实现

**文件**: `arch/x86/kvm/vmx/vmx.c:1951` (vmx_write_tsc_offset),
`arch/x86/kvm/x86.c:2717` (kvm_synchronize_tsc)

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:1951-1959 */

/*
 * TSC_OFFSET 写入 VMCS
 * Guest RDTSC = Host TSC + TSC_OFFSET
 *
 * 何时调用:
 *   - vCPU创建时
 *   - TSC unstable 时每次 vcpu load 重算 (x86.c:5014-5024)
 *   - 用户空间写 MSR_IA32_TSC 时 (x86.c:3940 → kvm_synchronize_tsc)
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
```

`kvm_synchronize_tsc()` 的签名与主干（下面是节选 + 伪代码标注，不是逐行原文）：

```c
/* 来源: arch/x86/kvm/x86.c:2717（签名与开头为原文，循环部分为示意） */
static void kvm_synchronize_tsc(struct kvm_vcpu *vcpu, u64 *user_value)
{
	u64 data = user_value ? *user_value : 0;
	struct kvm *kvm = vcpu->kvm;
	u64 offset, ns, elapsed;
	unsigned long flags;
	bool matched = false;
	bool synchronizing = false;

	raw_spin_lock_irqsave(&kvm->arch.tsc_write_lock, flags);
	offset = kvm_compute_l1_tsc_offset(vcpu, data);
	ns = get_kvmclock_base_ns();
	elapsed = ns - kvm->arch.last_tsc_nsec;
	...
	/* data==0（vCPU 创建/用户显式写 0）→ 强制同步;
	 * 否则若与上次用户写入的虚拟周期间隔在容差内 → 视为同一批, 对齐;
	 * 最终经 __kvm_synchronize_tsc()（x86.c:2670, 调用点 :2783）
	 * 把 offset 写给本 vCPU 并按需广播给其余 vCPU */
}
```

### vcpu load 时的 TSC 自愈

```c
/* 来源: arch/x86/kvm/x86.c:5008-5026（vcpu_load 中的节选） */

	/* 挂起恢复等外部检测到的 TSC 调整 */
	if (unlikely(vcpu->arch.tsc_offset_adjustment)) {
		adjust_tsc_offset_host(vcpu, vcpu->arch.tsc_offset_adjustment);
		...
	}

	if (unlikely(vcpu->cpu != cpu) || kvm_check_tsc_unstable()) {
		s64 tsc_delta = !vcpu->arch.last_host_tsc ? 0 :
				rdtsc() - vcpu->arch.last_host_tsc;
		if (tsc_delta < 0)
			mark_tsc_unstable("KVM discovered backwards TSC");

		if (kvm_check_tsc_unstable()) {
			u64 offset = kvm_compute_l1_tsc_offset(vcpu,
						vcpu->arch.last_guest_tsc);
			kvm_vcpu_write_tsc_offset(vcpu, offset);
			vcpu->arch.tsc_catchup = 1;
		}
		...
```

注意条件是 `kvm_check_tsc_unstable()`，不是"换了 pCPU 就重算"；
写入走 `kvm_vcpu_write_tsc_offset()`，不是直接 `vmcs_write64`。

### TSC 稳定性检测

```
1. Invariant TSC: CPUID.80000007H:EDX[8]（现代CPU几乎都支持）
2. constant TSC: 频率不随变频改变, boot_cpu_has(X86_FEATURE_CONSTANT_TSC)
3. host 各 pCPU 之间: 由 host 内核的 TSC 校准/检查决定,
   KVM 侧表现为 kvm_check_tsc_unstable() 与 backwards TSC 检测
```

---

## 4. kvmclock 详细实现

**文件**: `arch/x86/kvm/x86.c:2354` (kvm_write_system_time),
`arch/x86/kvm/x86.c:3215` (kvm_guest_time_update)

```c
/* 来源: arch/x86/kvm/x86.c:2354-2377 */

/*
 * Guest 通过 MSR_KVM_SYSTEM_TIME_NEW 注册 pvclock 页的 GPA。
 * ★ 本函数并不写 pvclock 内容：只保存 GPA、发请求、激活缓存槽位。
 *   真正的填充发生在 kvm_guest_time_update()（x86.c:3215）。
 */
static void kvm_write_system_time(struct kvm_vcpu *vcpu, gpa_t system_time,
				  bool old_msr, bool host_initiated)
{
	struct kvm_arch *ka = &vcpu->kvm->arch;

	if (vcpu->vcpu_id == 0 && !host_initiated) {
		if (ka->boot_vcpu_runs_old_kvmclock != old_msr)
			kvm_make_request(KVM_REQ_MASTERCLOCK_UPDATE, vcpu);
		ka->boot_vcpu_runs_old_kvmclock = old_msr;
	}

	vcpu->arch.time = system_time;                 /* 只存 GPA */
	kvm_make_request(KVM_REQ_GLOBAL_CLOCK_UPDATE, vcpu);

	/* enable bit（最低位）决定是否激活 */
	if (system_time & 1)
		kvm_gpc_activate(&vcpu->arch.pv_time, system_time & ~1ULL,
				 sizeof(struct pvclock_vcpu_time_info));
	else
		kvm_gpc_deactivate(&vcpu->arch.pv_time);
}
```

```c
/*
 * pvclock_vcpu_time_info 结构（Guest 和 Host 共享）
 * 定义于: arch/x86/include/asm/pvclock-abi.h:26-35
 * （不在 kvm_para.h；注意 flags 是 u8、末尾是 pad[2]、总共 32 字节）
 */
struct pvclock_vcpu_time_info {
	u32   version;           /* seqlock 风格版本号（奇数=更新中） */
	u32   pad0;
	u64   tsc_timestamp;     /* TSC at this update */
	u64   system_time;       /* System time at this update (ns) */
	u32   tsc_to_system_mul; /* TSC → ns 缩放因子 */
	s8    tsc_shift;         /* TSC 移位调整 */
	u8    flags;             /* PVCLOCK_TSC_STABLE_BIT 等 */
	u8    pad[2];
} __attribute__((__packed__)); /* 32 bytes */

/*
 * Guest 读取当前时间的算法（Guest 内核侧）:
 *
 * 1. do { version = pvti->version; } while (version & 1);
 * 2. current_tsc = RDTSC;
 *    delta_tsc = current_tsc - pvti->tsc_timestamp;
 * 3. delta_ns = (delta_tsc * pvti->tsc_to_system_mul) >> (22 + tsc_shift);
 * 4. current_ns = pvti->system_time + delta_ns;
 * 5. if (version != pvti->version) retry;
 *
 * 整个过程: 0次VM-Exit! 纯内存读 + 算术运算
 */

/*
 * wall clock 结构（boot 时刻的墙上时间基准）
 * 定义于: arch/x86/include/asm/pvclock-abi.h:37-41
 * Guest 通过 MSR_KVM_WALL_CLOCK_NEW 指定共享地址
 */
struct pvclock_wall_clock {
	u32   version;
	u32   sec;       /* 自1970-01-01的秒数 */
	u32   nsec;
};
```

---

## 5. Timer Advance 优化（TSC-deadline 专属）

```c
/* 来源: arch/x86/kvm/lapic.c */

/*
 * lapic.c:62-68 的注释开宗明义:
 *   "Enable local APIC timer advancement (tscdeadline mode only)
 *    with adaptive tuning."
 * —— timer advance 只服务 TSC-deadline 模式（旧版文档说"仅适用
 *    Periodic/One-shot"正好相反）。原理: 让 host 定时器提前
 *    expire, 抵消 VM-Exit→注入→VM-Entry 的延迟, 使中断到达
 *    guest 时尽量贴近 guest 编程的 deadline。
 */

/* lapic.c:70-71 —— 唯一的模块参数是 bool 开关 */
static bool lapic_timer_advance __read_mostly = true;
module_param(lapic_timer_advance, bool, 0444);

/* lapic.c:75-78 —— 调整用的三个常量 */
#define LAPIC_TIMER_ADVANCE_NS_INIT      1000   /* 初始提前量 */
#define LAPIC_TIMER_ADVANCE_NS_MAX       5000
#define LAPIC_TIMER_ADVANCE_ADJUST_STEP  8      /* 每次调整 1/8 */

/*
 * 提前量保存在每个 vCPU 的 lapic_timer.timer_advance_ns，
 * 在 kvm_create_lapic() 里按开关初始化（lapic.c:2930-2931）:
 */
	if (lapic_timer_advance)
		apic->lapic_timer.timer_advance_ns = LAPIC_TIMER_ADVANCE_NS_INIT;

/*
 * 自适应算法: adjust_lapic_timer_advance()（lapic.c:1840-1866）
 * ★ 是步进式 1/8 增量调整，不是 EWMA:
 */
	if (unlikely(timer_advance_ns > LAPIC_TIMER_ADVANCE_NS_MAX))
		timer_advance_ns = LAPIC_TIMER_ADVANCE_NS_INIT;  /* 超上限重置 */
	...
	/* 太早到期(提前量过大) → 减; 太晚 → 加（各 ±ns/8） */
	timer_advance_ns -= ns/LAPIC_TIMER_ADVANCE_ADJUST_STEP;   /* :1856 */
	timer_advance_ns += ns/LAPIC_TIMER_ADVANCE_ADJUST_STEP;   /* :1861 */

/*
 * 观察方式: 每 vCPU 的 debugfs（只读）
 *   arch/x86/kvm/debugfs.c:67
 *   debugfs_create_file("lapic_timer_advance_ns", 0444, ...)
 * ★ 不存在 lapic_timer_advance_ns 模块参数, 不能用
 *   /sys/module/kvm/parameters/ 调它。
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
│      ├── last_tsc_nsec / last_tsc_write (同步基准)        │
│      ├── use_master_clock / master_kernel_ns              │
│      └── kvmclock_offset                                   │
│                                                              │
└──────────────────────────────────────────────────────────────┘

┌─ struct kvm_vcpu (vCPU级别) ───────────────────────────────┐
│                                                              │
│  arch                                                       │
│  ├── apic ─────────▶ struct kvm_lapic                     │
│  │   └── lapic_timer                                       │
│  │       ├── timer (Host hrtimer)                          │
│  │       ├── period / target_expiration                    │
│  │       ├── timer_advance_ns (TSC-deadline 提前量)        │
│  │       ├── pending (是否有待注入的中断)                  │
│  │       └── hv_timer_in_use (是否在用 preemption timer)   │
│  │                                                          │
│  ├── tsc_offset ──── TSC偏移量 (写入VMCS)                 │
│  └── tsc_scaling_ratio ── TSC缩放比 (写入VMCS)            │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 7. 关键模块参数与观测口

```
KVM 时钟相关模块参数（已逐个核对定义处）:

参数                     类型     默认     权限    定义处
───────────────────────  ──────  ──────  ─────  ──────────────
lapic_timer_advance      bool    true    0444   lapic.c:70-71
tsc_tolerance_ppm        uint    250     0644   x86.c:167-168
min_timer_period_us      uint    200     0644   x86.c:160-161
kvmclock_periodic_sync   bool    true    0444   x86.c:163-164

查看: /sys/module/kvm/parameters/

只读观测口（不是模块参数）:
  每 vCPU 的 timer_advance_ns 当前值:
    /sys/kernel/debug/kvm/<pid>-<fd>/vcpu*/lapic_timer_advance_ns
    (debugfs.c:67, 0444)
```
