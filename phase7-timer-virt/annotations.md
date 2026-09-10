# Phase 7：源码精读注释 - 时钟虚拟化

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。

---

## 1. PIT 数据结构与 IO 端口模拟

### Q: PIT 的三个核心数据结构是什么？

**文件**: `arch/x86/kvm/i8254.h:9` — `struct kvm_kpit_channel_state`

```c
/* 来源: arch/x86/kvm/i8254.h:9 */
struct kvm_kpit_channel_state {
    u32 count;                /* 初始装载值（可以是 65536） */
    u16 latched_count;        /* 锁存计数值 */
    u8  count_latched;        /* 锁存状态 */
    u8  status_latched;
    u8  status;               /* ★ i8254.h:14（不是 status_count） */
    u8  read_state;           /* 读状态（LSB/MSB 交替） */
    u8  write_state;
    u8  write_latch;
    u8  rw_mode;
    u8  mode;                 /* 工作模式 0-5 */
    u8  bcd;                  /* not supported */
    u8  gate;                 /* timer start */
    ktime_t count_load_time;  /* ★ i8254.h:22（类型是 ktime_t，不是 s64） */
};
```

**文件**: `arch/x86/kvm/i8254.h:25` — `struct kvm_kpit_state`

```c
/* 来源: arch/x86/kvm/i8254.h:25 */
struct kvm_kpit_state {
    /* ★ "struct mutex lock" 之前的成员都受该 lock 保护 */
    struct kvm_kpit_channel_state channels[3];
    u32 flags;
    bool is_periodic;         /* ★ i8254.h:29（类型是 bool，不是 int） */
    s64 period;               /* 周期，单位 ns */
    struct hrtimer timer;     /* ★ i8254.h:31 — 字段名 timer，不是 pit_timer */

    struct mutex lock;
    atomic_t reinject;
    atomic_t pending;         /* 累积已触发未注入的定时器 */
    atomic_t irq_ack;
    struct kvm_irq_ack_notifier irq_ack_notifier;
};
```

**文件**: `arch/x86/kvm/i8254.h:40` — `struct kvm_pit`

```c
/* 来源: arch/x86/kvm/i8254.h:40 */
struct kvm_pit {
    struct kvm_io_device dev;
    struct kvm_io_device speaker_dev;
    struct kvm *kvm;
    struct kvm_kpit_state pit_state;
    int irq_source_id;
    struct kvm_irq_mask_notifier mask_notifier;
    struct kthread_worker *worker;   /* ★ 中断注入推迟到内核线程 */
    struct kthread_work expired;
};
```

### Q: PIT 的 IO 端口映射？

| IO 端口 | 功能 |
|---------|------|
| `0x40` | Channel 0 数据端口（系统定时器 → IRQ0） |
| `0x41` | Channel 1 数据端口（DRAM 刷新，已废弃） |
| `0x42` | Channel 2 数据端口（PC 扬声器） |
| `0x43` | 控制字端口 |

**控制字格式**（写 `0x43`）：

```
┌───┬───┬───┬───┬───┬───┬───┬───┐
│SC1│SC0│RW1│RW0│M2 │M1 │M0 │BCD│
└───┴───┴───┴───┴───┴───┴───┴───┘
SC: Select Channel (00=Ch0, 01=Ch1, 10=Ch2, 11=读回)
RW: Read/Write (00=锁存, 01=LSB only, 10=MSB only, 11=LSB then MSB)
M:  Mode (010=2 Rate Generator ★最常用, 011=3 Square Wave)
BCD: 0=二进制, 1=BCD
```

**关键常量**（`i8254.h:51-54`）：

| 宏 | 值 | 说明 |
|----|-----|------|
| `KVM_PIT_BASE_ADDRESS` | `0x40` | IO 端口基地址 |
| `KVM_SPEAKER_BASE_ADDRESS` | `0x61` | 扬声器端口 |
| `KVM_PIT_FREQ` | `1193181` | ★ 基频 Hz（不是 1193182） |

### Q: 写路径 — Guest 装载计数值时发生了什么？

**文件**: `arch/x86/kvm/i8254.c:365` — `pit_load_count()`

```c
/* 来源: arch/x86/kvm/i8254.c:365 */
static void pit_load_count(struct kvm_pit *pit, int channel, u32 val)
{
    struct kvm_kpit_state *ps = &pit->pit_state;

    if (val == 0)
        val = 0x10000;             /* ★ 0 表示最大值 65536 */

    ps->channels[channel].count = val;

    if (channel != 0) {
        ps->channels[channel].count_load_time = ktime_get();
        return;                    /* Channel 1/2 不创建定时器 */
    }

    /* ★ Channel 0: 根据模式创建或销毁定时器 */
    switch (ps->channels[0].mode) {
    case 0: case 1: case 4: case 5:
        create_pit_timer(pit, val, 0);   /* one-shot */
        break;
    case 2: case 3:
        create_pit_timer(pit, val, 1);   /* periodic */
        break;
    default:
        destroy_pit_timer(pit);
    }
}
```

### Q: `create_pit_timer()` 怎么防 Guest 打爆 host？

**文件**: `arch/x86/kvm/i8254.c:322` — `create_pit_timer()`

```c
/* 来源: arch/x86/kvm/i8254.c:322（节选） */
static void create_pit_timer(struct kvm_pit *pit, u32 val, int is_period)
{
    ...
    /* ★ 计数值 → 纳秒 */
    interval = mul_u64_u32_div(val, NSEC_PER_SEC, KVM_PIT_FREQ);  /* :332 */
    ...
    ps->period = interval;

    if (ps->is_periodic) {
        /* ★ 限流：防止 Guest 用极小周期打爆 host hrtimer */
        s64 min_period = min_timer_period_us * 1000LL;  /* 默认 200μs */
        if (ps->period < min_period) {                   /* x86.c:160 */
            ps->period = min_period;   /* 周期被抬高 */
        }
    }

    hrtimer_start(&ps->timer, ktime_add_ns(ktime_get(), interval),
                  HRTIMER_MODE_ABS);
}
```

**`min_timer_period_us`**（`x86.c:160`）默认 **200μs**，权限 `0644` 可运行时调。
这个限流只作用 periodic 模式；one-shot 不受限。

### Q: hrtimer 到期后为什么不直接注入中断？

**文件**: `arch/x86/kvm/i8254.c:268` — `pit_timer_fn()`

```c
/* 来源: arch/x86/kvm/i8254.c:268 */
static enum hrtimer_restart pit_timer_fn(struct hrtimer *data)
{
    struct kvm_kpit_state *ps = container_of(data, ...);
    struct kvm_pit *pt = pit_state_to_pit(ps);

    if (atomic_read(&ps->reinject))
        atomic_inc(&ps->pending);           /* 记账：累积未注入次数 */

    kthread_queue_work(pt->worker, &pt->expired);  /* ★ 推迟到内核线程 */

    if (ps->is_periodic) {
        hrtimer_add_expires_ns(&ps->timer, ps->period);
        return HRTIMER_RESTART;
    }
    return HRTIMER_NORESTART;
}
```

**为什么推迟？** hrtimer 回调在 softirq 上下文执行，持锁能力有限。`kvm_set_irq()`
需要获取 `irq_lock` 等 mutex，不能在 interrupt context 调用。所以推迟到 `kthread_worker`
内核线程中执行真正的注入：

```c
/* 来源: arch/x86/kvm/i8254.c:240 — pit_do_work() */
static void pit_do_work(struct kthread_work *work)
{
    ...
    kvm_set_irq(kvm, pit->irq_source_id, 0, 1, false);  /* :251 ★ 拉高 IRQ0 */
    kvm_set_irq(kvm, pit->irq_source_id, 0, 0, false);  /* :252 ★ 拉低（脉冲） */
}
```

### Q: 读路径 — Guest 读当前计数值怎么算？

**文件**: `arch/x86/kvm/i8254.c:115` — `pit_get_count()`

```c
/* 来源: arch/x86/kvm/i8254.c:115 */
static int pit_get_count(struct kvm_pit *pit, int channel)
{
    struct kvm_kpit_channel_state *c = &pit->pit_state.channels[channel];

    t = kpit_elapsed(pit, c, channel);        /* 距上次装载的时间 (ns) */
    d = mul_u64_u32_div(t, KVM_PIT_FREQ, NSEC_PER_SEC);  /* 流逝的计数 */

    switch (c->mode) {
    case 0: case 1: case 4: case 5:
        counter = (c->count - d) & 0xffff;    /* 线性递减 */
        break;
    case 3:
        /* ★ Mode 3 (Square Wave): 递减速度是 2 倍 */
        counter = c->count - (mod_64((2 * d), c->count));
        break;
    default:
        counter = c->count - mod_64(d, c->count);  /* Mode 2: 锯齿波 */
        break;
    }
    return counter;
}
```

**不推进定时器** — 纯按需计算。读操作不会触发 VM-Exit 以外的副作用。

---

## 2. APIC Timer — 三种模式对比

### Q: APIC Timer 的配置寄存器有哪些？

| 偏移 | 名称 | 功能 |
|------|------|------|
| `0x320` | `APIC_LVTT` | LVT Timer — 模式 + 掩码 + 向量 |
| `0x380` | `APIC_TMICT` | Initial Count — 写入后开始倒计时 |
| `0x390` | `APIC_TMCCT` | Current Count — 当前计数值（只读） |
| `0x3E0` | `APIC_TDCR` | Divide Configuration — 除数 |

**`APIC_LVTT` 格式**（`arch/x86/include/asm/apicdef.h:107-110`）：

```
[18:17] Timer Mode:  00=One-shot  01=Periodic  10=TSC-deadline
[16]    Masked:      1=屏蔽中断
[7:0]   Vector:      中断向量号
```

### Q: 三种模式的核心区别？

```
┌─ Periodic ──────────────────────────────────────────────────────┐
│  写: LVTT[18:17]=01, TMICT=N, TDCR=divide                      │
│  行为: count N→0 → 中断 → 自动重载 N → 重复                    │
│  VM-Exit: 写 TMICT/TDCR 时 Exit; 倒计时期间不 Exit             │
│  KVM: hrtimer 模拟, period = N × divide / bus_freq             │
└────────────────────────────────────────────────────────────────┘

┌─ One-shot ──────────────────────────────────────────────────────┐
│  写: LVTT[18:17]=00, TMICT=N                                   │
│  行为: count N→0 → 中断一次 → 停止                             │
│  KVM: hrtimer 模拟, 不重载                                      │
│  用途: tickless 内核 (NO_HZ), 只在需要时设下一个定时器         │
└────────────────────────────────────────────────────────────────┘

┌─ TSC-deadline (最高效!) ────────────────────────────────────────┐
│  写: LVTT[18:17]=10, IA32_TSC_DEADLINE MSR = deadline          │
│  行为: 硬件比较 TSC vs deadline                                │
│        TSC ≥ deadline → 触发中断                               │
│        到期后需重新写 deadline（one-shot 语义）                │
│                                                                 │
│  VM-Exit: ★ 只有写 MSR 时一次! 无周期 Exit                    │
│  KVM: 两步分工:                                                │
│    1. vmx_set_hv_timer() (vmx.c:8129)                          │
│       算出扣除 timer-advance、经 TSC scaling 换算的             │
│       host deadline → vmx->hv_deadline_tsc                     │
│    2. vmx_update_hv_timer() (vmx.c:7205)                       │
│       VM-Entry 前 vmcs_write32(PREEMPTION_TIMER_VALUE, delta)  │
│       (:7212/:7223/:7226) → 硬件触发 VM-Exit                   │
│    3. delta 装不进 32 位 → 回退软件路径                        │
│       start_sw_tscdeadline() (lapic.c:1953) 用 hrtimer         │
│  优势: 不需要 TMICT/TDCR → 减少 VM-Exit 次数                 │
└────────────────────────────────────────────────────────────────┘
```

### Q: KVM APIC Timer 的代码入口？

**文件**: `arch/x86/kvm/lapic.c`

```c
/* 来源: arch/x86/kvm/lapic.c:2269 — start_apic_timer() */
static void start_apic_timer(struct kvm_lapic *apic)
{
    __start_apic_timer(apic, APIC_TMICT);         /* :2258 */
}

/* 来源: arch/x86/kvm/lapic.c:2200 — restart_apic_timer() */
static void restart_apic_timer(struct kvm_lapic *apic)
{
    preempt_disable();

    if (!apic_lvtt_period(apic) && atomic_read(&apic->lapic_timer.pending))
        goto out;                      /* one-shot 已有 pending → 不重启 */

    /* ★ 优先尝试硬件定时器 (VMX preemption timer) */
    if (!start_hv_timer(apic))         /* :2141 — 返回 true 表示成功 */
        start_sw_timer(apic);          /* :2183 — 回退软件路径 */
out:
    preempt_enable();
}
```

**定时器路径优先级**：

```
1. VMX preemption timer（start_hv_timer, :2141）
   → 到期即硬件 VM-Exit
   → kvm_lapic_expired_hv_timer()（:2213）处理

2. hrtimer 软件路径
   → Periodic/One-shot: start_sw_period（:2105）
   → TSC-deadline: start_sw_tscdeadline（:1953）
```

### Q: hrtimer 到期回调做什么？

**文件**: `arch/x86/kvm/lapic.c:2883` — `apic_timer_fn()`

```c
/* 来源: arch/x86/kvm/lapic.c:2883 */
static enum hrtimer_restart apic_timer_fn(struct hrtimer *data)
{
    struct kvm_timer *ktimer = container_of(data, ...);
    struct kvm_lapic *apic = container_of(ktimer, ...);

    apic_timer_expired(apic, true);              /* :1915 — 记录到期 + 注入 */

    if (lapic_is_periodic(apic) && !WARN_ON_ONCE(!apic->lapic_timer.period)) {
        advance_periodic_target_expiration(apic);  /* 推进目标时间 */
        hrtimer_set_expires(&ktimer->timer, ktimer->target_expiration);
        return HRTIMER_RESTART;                   /* periodic → 继续 */
    }
    return HRTIMER_NORESTART;                     /* one-shot → 停止 */
}
```

**`apic_timer_expired()`**（`lapic.c:1915`）是到期的核心处理：标记 `pending=1`、
通过 `kvm_queue_pending_eoi` 或直接 `kvm_vcpu_kick()` 唤醒 vCPU。

---

## 3. TSC 虚拟化

### Q: Guest RDTSC 的值怎么算？

**文件**: `arch/x86/kvm/vmx/vmx.c:1951` — `vmx_write_tsc_offset()`
**文件**: `arch/x86/kvm/vmx/vmx.c:1956` — `vmx_write_tsc_multiplier()`

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:1951 */
void vmx_write_tsc_offset(struct kvm_vcpu *vcpu)
{
    vmcs_write64(TSC_OFFSET, vcpu->arch.tsc_offset);
}

/* 来源: arch/x86/kvm/vmx/vmx.c:1956 */
void vmx_write_tsc_multiplier(struct kvm_vcpu *vcpu)
{
    vmcs_write64(TSC_MULTIPLIER, vcpu->arch.tsc_scaling_ratio);
}
```

**公式**：

```
无 TSC scaling:   Guest TSC = Host TSC + TSC_OFFSET
有 TSC scaling:   Guest TSC = (Host TSC × TSC_MULTIPLIER) + TSC_OFFSET
```

`TSC_MULTIPLIER` 格式：48 位小数 + 整数位。`1.0 = 0x0000000100000000`。

| 用途 | 场景 |
|------|------|
| `TSC_OFFSET` | vCPU 创建、TSC unstable 重算、用户空间写 `MSR_IA32_TSC` |
| `TSC_MULTIPLIER` | 嵌套虚拟化（L2 需要 L1 频率）、跨 Host 迁移兼容 |

### Q: `kvm_synchronize_tsc()` 怎么保证多 vCPU TSC 一致？

**文件**: `arch/x86/kvm/x86.c:2717` — `kvm_synchronize_tsc()`

```c
/* 来源: arch/x86/kvm/x86.c:2717（签名 + 主干） */
static void kvm_synchronize_tsc(struct kvm_vcpu *vcpu, u64 *user_value)
{
    u64 data = user_value ? *user_value : 0;
    struct kvm *kvm = vcpu->kvm;
    u64 offset, ns, elapsed;

    raw_spin_lock_irqsave(&kvm->arch.tsc_write_lock, flags);
    offset = kvm_compute_l1_tsc_offset(vcpu, data);
    ns = get_kvmclock_base_ns();
    elapsed = ns - kvm->arch.last_tsc_nsec;
    ...
    /* data==0 → 强制同步（vCPU 创建）;
     * 否则若与上次写入的间隔在容差内 → 视为同一批，对齐;
     * 最终经 __kvm_synchronize_tsc()（x86.c:2670, 调用点 :2783）
     * 把 offset 写给本 vCPU 并按需广播给其余 vCPU */
}
```

**为什么需要容差？** 多 vCPU 创建时，QEMU 顺序写 `MSR_IA32_TSC`，每次写入的
host TSC 略有差异。如果差异在 `tsc_tolerance_ppm`（默认 250）范围内，KVM 认为它们
属于同一批次，使用相同的 offset，保证 Guest 看到所有 vCPU 的 TSC 一致。

### Q: vCPU 切换 pCPU 时 TSC 怎么处理？

**文件**: `arch/x86/kvm/x86.c:5008-5026`（`vcpu_load` 中的节选）

```c
/* 来源: arch/x86/kvm/x86.c:5008（节选） */

/* ★ 只在 TSC unstable 时才重算 offset，不是每次切换 pCPU */
if (unlikely(vcpu->cpu != cpu) || kvm_check_tsc_unstable()) {
    s64 tsc_delta = !vcpu->arch.last_host_tsc ? 0 :
                    rdtsc() - vcpu->arch.last_host_tsc;
    if (tsc_delta < 0)
        mark_tsc_unstable("KVM discovered backwards TSC");

    if (kvm_check_tsc_unstable()) {
        u64 offset = kvm_compute_l1_tsc_offset(vcpu,
                            vcpu->arch.last_guest_tsc);
        kvm_vcpu_write_tsc_offset(vcpu, offset);  /* ★ 不是直接 vmcs_write64 */
        vcpu->arch.tsc_catchup = 1;
    }
    ...
}
```

**TSC 稳定性判定**：

| 条件 | 来源 | 说明 |
|------|------|------|
| Invariant TSC | CPUID.80000007H:EDX[8] | 现代 CPU 几乎都支持 |
| constant TSC | `boot_cpu_has(X86_FEATURE_CONSTANT_TSC)` | 频率不随变频改变 |
| pCPU 间同步 | host 内核校准 | KVM 侧表现为 `kvm_check_tsc_unstable()` |

---

## 4. kvmclock — 零 VM-Exit 读时间

### Q: Guest 注册 pvclock 页面时 KVM 做了什么？

**文件**: `arch/x86/kvm/x86.c:2354` — `kvm_write_system_time()`

```c
/* 来源: arch/x86/kvm/x86.c:2354 */
static void kvm_write_system_time(struct kvm_vcpu *vcpu, gpa_t system_time,
                                  bool old_msr, bool host_initiated)
{
    struct kvm_arch *ka = &vcpu->kvm->arch;

    /* vCPU 0 的 kvmclock 版本切换 */
    if (vcpu->vcpu_id == 0 && !host_initiated) {
        if (ka->boot_vcpu_runs_old_kvmclock != old_msr)
            kvm_make_request(KVM_REQ_MASTERCLOCK_UPDATE, vcpu);
        ka->boot_vcpu_runs_old_kvmclock = old_msr;
    }

    vcpu->arch.time = system_time;                     /* ★ 只存 GPA */
    kvm_make_request(KVM_REQ_GLOBAL_CLOCK_UPDATE, vcpu);

    /* enable bit（最低位）决定是否激活 */
    if (system_time & 1)
        kvm_gpc_activate(&vcpu->arch.pv_time, system_time & ~1ULL,
                         sizeof(struct pvclock_vcpu_time_info));
    else
        kvm_gpc_deactivate(&vcpu->arch.pv_time);
}
```

**★ 本函数不写 pvclock 内容**：只保存 GPA、发请求、激活缓存槽位。真正的填充
发生在 `kvm_guest_time_update()`（`x86.c:3215`），在 vCPU 进入 Guest 前的请求处理
阶段执行。

### Q: `pvclock_vcpu_time_info` 结构长什么样？

**文件**: `arch/x86/include/asm/pvclock-abi.h:26`

```c
/* 来源: arch/x86/include/asm/pvclock-abi.h:26 */
struct pvclock_vcpu_time_info {
    u32   version;           /* seqlock 风格版本号（奇数=更新中） */
    u32   pad0;
    u64   tsc_timestamp;     /* 本次更新时的 TSC */
    u64   system_time;       /* 本次更新时的系统时间 (ns) */
    u32   tsc_to_system_mul; /* TSC → ns 缩放因子 */
    s8    tsc_shift;         /* TSC 移位调整 */
    u8    flags;             /* ★ u8 — PVCLOCK_TSC_STABLE_BIT 等 */
    u8    pad[2];
} __attribute__((__packed__)); /* ★ 总共 32 字节 */
```

### Q: Guest 怎么读当前时间？

```c
/* Guest 内核侧算法（零 VM-Exit）: */
do {
    version = pvti->version;              /* 1. 读版本号 */
} while (version & 1);                    /*    奇数=更新中，重试 */

current_tsc = RDTSC;                      /* 2. 读当前 TSC */
delta_tsc = current_tsc - pvti->tsc_timestamp;

delta_ns = (delta_tsc * pvti->tsc_to_system_mul) >> (22 + tsc_shift);
                                              /* 3. TSC → ns */
current_ns = pvti->system_time + delta_ns;  /* 4. 加上基准 */

if (version != pvti->version) goto retry;   /* 5. 版本号变了 → 重试 */
```

**整个过程**：0 次 VM-Exit。纯内存读 + 算术运算。seqlock 保证读到一致的数据。

### Q: wall clock 结构？

**文件**: `arch/x86/include/asm/pvclock-abi.h:37`

```c
/* 来源: arch/x86/include/asm/pvclock-abi.h:37 */
struct pvclock_wall_clock {
    u32   version;
    u32   sec;       /* 自 1970-01-01 的秒数 */
    u32   nsec;
};
```

Guest 通过 `MSR_KVM_WALL_CLOCK_NEW` 指定共享地址。提供 boot 时刻的墙上时间基准。

---

## 5. Timer Advance 优化 — TSC-deadline 专属

### Q: Timer Advance 解决什么问题？

**只服务 TSC-deadline 模式**。原理：让 host 定时器提前 expire，抵消 VM-Exit →
注入 → VM-Entry 的延迟，使中断到达 Guest 时尽量贴近 Guest 编程的 deadline。

```
Guest deadline ──────────────────────────────★（期望中断到达时刻）
                                           ↑
实际中断到达 ──────────────────────★        │
                 VM-Exit  注入  VM-Entry    │
                 ←────── 延迟 ─────────────→│
                                           
host timer expire ──────────★               │
                 ← advance →←── 延迟 ──────→│
```

**⚠️**：`lapic.c:62-68` 的注释开宗明义："tscdeadline mode only"。旧文档说"仅适用
Periodic/One-shot"正好相反。

### Q: 提前量怎么调？

**文件**: `arch/x86/kvm/lapic.c:70-78`

```c
/* 来源: arch/x86/kvm/lapic.c:70-71 */
static bool lapic_timer_advance __read_mostly = true;
module_param(lapic_timer_advance, bool, 0444);       /* ★ 唯一的模块参数 */

/* 来源: arch/x86/kvm/lapic.c:75-78 */
#define LAPIC_TIMER_ADVANCE_NS_INIT      1000   /* 初始提前量 1000 ns */
#define LAPIC_TIMER_ADVANCE_NS_MAX       5000   /* 上限 5000 ns */
#define LAPIC_TIMER_ADVANCE_ADJUST_STEP  8      /* 步进因子 1/8 */
```

**自适应算法**（`adjust_lapic_timer_advance()`，`lapic.c:1840`）：

```c
/* 来源: arch/x86/kvm/lapic.c:1840（简化） */
static void adjust_lapic_timer_advance(struct kvm_vcpu *vcpu, int advance_ns)
{
    ...
    /* ★ 步进式 1/8 增量调整（不是 EWMA） */
    if (timer_advance_ns > LAPIC_TIMER_ADVANCE_NS_MAX)
        timer_advance_ns = LAPIC_TIMER_ADVANCE_NS_INIT;  /* 超上限重置 */

    /* 太早到期（提前量过大）→ 减; 太晚 → 加 */
    timer_advance_ns -= ns / LAPIC_TIMER_ADVANCE_ADJUST_STEP;   /* :1856 */
    timer_advance_ns += ns / LAPIC_TIMER_ADVANCE_ADJUST_STEP;   /* :1861 */
}
```

| 常量 | 值 | 说明 |
|------|-----|------|
| `LAPIC_TIMER_ADVANCE_NS_INIT` | 1000 | 初始提前量 (ns) |
| `LAPIC_TIMER_ADVANCE_NS_MAX` | 5000 | 上限 (ns) |
| `LAPIC_TIMER_ADVANCE_ADJUST_STEP` | 8 | 每次调整误差的 1/8 |

**初始化**（`kvm_create_lapic()`，`lapic.c:2930-2931`）：

```c
if (lapic_timer_advance)
    apic->lapic_timer.timer_advance_ns = LAPIC_TIMER_ADVANCE_NS_INIT;
```

**观测方式**（只读，per-vCPU debugfs）：

```
/sys/kernel/debug/kvm/<pid>-<fd>/vcpu*/lapic_timer_advance_ns
(arch/x86/kvm/debugfs.c:67, 权限 0444)
```

**⚠️**：不存在 `lapic_timer_advance_ns` 模块参数，不能用 `/sys/module/kvm/parameters/`
调它。只有 `lapic_timer_advance` (bool) 是模块参数。

---

## 6. 各时钟源数据结构总览

```
时钟虚拟化数据结构:

┌─ struct kvm (VM 级别) ──────────────────────────────────────┐
│                                                              │
│  arch                                                       │
│  ├── pit ──────▶ struct kvm_pit                             │
│  │   └── pit_state.channels[3]                              │
│  │       └── count, mode, count_load_time ...               │
│  │                                                          │
│  ├── kvmclock                                             │
│  │   ├── pvclock_vcpu_time_info (每 vCPU 一份)             │
│  │   └── pvclock_wall_clock (全局 wall clock)              │
│  │                                                          │
│  └── tsc 相关                                              │
│      ├── last_tsc_nsec / last_tsc_write (同步基准)        │
│      ├── use_master_clock / master_kernel_ns              │
│      └── kvmclock_offset                                   │
│                                                              │
└──────────────────────────────────────────────────────────────┘

┌─ struct kvm_vcpu (vCPU 级别) ───────────────────────────────┐
│                                                              │
│  arch                                                       │
│  ├── apic ─────▶ struct kvm_lapic                           │
│  │   └── lapic_timer                                        │
│  │       ├── timer (host hrtimer)                           │
│  │       ├── period / target_expiration                     │
│  │       ├── timer_advance_ns (TSC-deadline 提前量)        │
│  │       ├── pending (是否有待注入中断)                     │
│  │       └── hv_timer_in_use (是否用 preemption timer)     │
│  │                                                          │
│  ├── tsc_offset ─── TSC 偏移量 (→ VMCS TSC_OFFSET)        │
│  └── tsc_scaling_ratio ── TSC 缩放比 (→ VMCS TSC_MULTIPLIER)│
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 7. 模块参数与观测口

### Q: 时钟相关可调参数有哪些？

**定义于**: `arch/x86/kvm/x86.c:160-168`, `arch/x86/kvm/lapic.c:70-71`

| 参数 | 类型 | 默认 | 权限 | 定义处 | 作用 |
|------|------|------|------|--------|------|
| `min_timer_period_us` | uint | 200 | 0644 | `x86.c:160` | PIT/APIC 最小定时器周期 (μs) |
| `kvmclock_periodic_sync` | bool | true | 0444 | `x86.c:163` | 是否周期性同步 kvmclock |
| `tsc_tolerance_ppm` | uint | 250 | 0644 | `x86.c:167` | TSC 同步容差 |
| `lapic_timer_advance` | bool | true | 0444 | `lapic.c:70` | TSC-deadline timer advance 开关 |

```bash
# 查看所有 KVM 参数
ls /sys/module/kvm/parameters/

# 运行时可调（0644）
echo 100 > /sys/module/kvm/parameters/min_timer_period_us
echo 500 > /sys/module/kvm/parameters/tsc_tolerance_ppm
```

### Q: 只读观测口？

```
每 vCPU 的 timer_advance_ns 当前值:
  /sys/kernel/debug/kvm/<pid>-<fd>/vcpu*/lapic_timer_advance_ns
  (debugfs.c:67, 0444)

Guest TSC offset:
  /sys/kernel/debug/kvm/<pid>-<fd>/vcpu*/tsc_offset
  (如果有对应的 debugfs 入口)
```

---

## 8. 完整调用链

```
═══════════════════ PIT 中断路径 ═══════════════════

  Guest 写 IO 端口 0x40/0x43
    │
    ▼
  pit_ioport_write()          (i8254.c:438)
    └→ pit_load_count()        (i8254.c:365)
        └→ create_pit_timer()  (i8254.c:322)
            ├→ interval = val × NSEC_PER_SEC / KVM_PIT_FREQ
            ├→ 限流: max(period, min_timer_period_us × 1000)
            └→ hrtimer_start()

  hrtimer 到期:
    pit_timer_fn()            (i8254.c:268)
      ├→ atomic_inc(&ps->pending)
      └→ kthread_queue_work()

  内核线程执行:
    pit_do_work()             (i8254.c:240)
      ├→ kvm_set_irq(..., 0, 1, ...)   ← 拉高 IRQ0
      └→ kvm_set_irq(..., 0, 0, ...)   ← 拉低（脉冲）

  Guest 读计数:
    pit_ioport_read() → pit_get_count()  (i8254.c:115)
      └→ 按需计算，不推进定时器


═══════════════════ APIC Timer 路径 ═══════════════════

  Guest 写 APIC_LVTT / APIC_TMICT
    │
    ▼
  start_apic_timer()          (lapic.c:2269)
    └→ __start_apic_timer()    (lapic.c:2258)
        └→ restart_apic_timer() (lapic.c:2200)
            ├→ start_hv_timer() (lapic.c:2141)
            │   └→ vmx_set_hv_timer() (vmx.c:8129)
            │       └→ vmx->hv_deadline_tsc = guest_deadline - advance
            │       └→ vmx_update_hv_timer() (vmx.c:7205)
            │           └→ vmcs_write32(PREEMPTION_TIMER_VALUE, delta)
            │
            └→ (失败回退) start_sw_timer() (lapic.c:2183)
                ├→ Periodic/One-shot: start_sw_period() (:2105)
                │   └→ hrtimer_start()
                └→ TSC-deadline: start_sw_tscdeadline() (:1953)
                    └→ hrtimer_start()

  到期:
    ├── HV timer: VM-Exit → kvm_lapic_expired_hv_timer() (:2213)
    │   └→ apic_timer_expired() (:1915)
    └── SW timer: apic_timer_fn() (:2883)
        └→ apic_timer_expired() (:1915)
            ├→ lapic_timer.pending = 1
            └→ kvm_vcpu_kick() / kvm_queue_pending_eoi()


═══════════════════ TSC 虚拟化 ═══════════════════

  Guest RDTSC:
    = (Host TSC × TSC_MULTIPLIER) + TSC_OFFSET
    （硬件自动，无 VM-Exit）

  TSC_OFFSET 写入:
    vmx_write_tsc_offset()     (vmx.c:1951)
      └→ vmcs_write64(TSC_OFFSET, vcpu->arch.tsc_offset)

  TSC 同步（多 vCPU）:
    kvm_synchronize_tsc()      (x86.c:2717)
      └→ __kvm_synchronize_tsc() (x86.c:2670)
          ├→ 计算 offset
          ├→ 本 vCPU: kvm_vcpu_write_tsc_offset()
          └→ 广播: 其余 vCPU 对齐

  vCPU load 自愈:
    if kvm_check_tsc_unstable():
      kvm_vcpu_write_tsc_offset()
      tsc_catchup = 1


═══════════════════ kvmclock 路径 ═══════════════════

  Guest 写 MSR_KVM_SYSTEM_TIME_NEW:
    kvm_write_system_time()    (x86.c:2354)
      ├→ vcpu->arch.time = GPA         ← 只存地址
      ├→ KVM_REQ_GLOBAL_CLOCK_UPDATE
      └→ kvm_gpc_activate(&vcpu->arch.pv_time, GPA, 32)

  vCPU 进入 Guest 前处理请求:
    kvm_guest_time_update()    (x86.c:3215)
      ├→ 读 host TSC + system_time
      ├→ 计算 tsc_to_system_mul + tsc_shift
      └→ 写 pvclock_vcpu_time_info 到 Guest 内存
          ★ 0 次 VM-Exit（Guest 纯内存读）

  Guest 读时间:
    do { version = pvti->version; } while (version & 1);
    delta_ns = (RDTSC - tsc_timestamp) × mul >> (22 + shift);
    current_ns = system_time + delta_ns;
    if (version != pvti->version) retry;
```
