# Phase 7 勘误

> 基线：Linux 6.12.93 源码。以下所有错误均已对照 `linux-6.12.93/` 实际代码验证。

---

## 勘误 1：PIT 频率 off-by-one

**原文** (README.md §1)：PIT 频率 1.193182 MHz

**实际**：`KVM_PIT_FREQ = 1193181` Hz（`arch/x86/kvm/i8254.h:54`），不是 1193182。

**影响**：微小，但所有基于 1193182 计算的 period 公式都有微小偏差。

---

## 勘误 2：PIT 函数名错误

**原文** (README.md §1)：`kvm_pit_ioport_write()` / `kvm_pit_ioport_read()` / `kvm_pit_timer_expired()`

**实际**（`arch/x86/kvm/i8254.c`）：

| 原文 | 实际函数名 | 行号 |
|------|-----------|------|
| `kvm_pit_ioport_write()` | `pit_ioport_write()` | `i8254.c:438` | <!-- check-refs:ignore -->
| `kvm_pit_ioport_read()` | `pit_ioport_read()` | `i8254.c:513` | <!-- check-refs:ignore -->
| `kvm_pit_timer_expired()` | `pit_timer_fn()` | `i8254.c:268` | <!-- check-refs:ignore -->

---

## 勘误 3：PIT 数据结构字段错误

**原文** (annotations.md §1)：`status_count` 字段、`count_load_time` 类型为 `s64`、`pit_timer` 字段

**实际**（`arch/x86/kvm/i8254.h`）：

| 原文 | 实际 |
|------|------|
| `u8 status_count` | `u8 status`（`i8254.h:14`） |
| `s64 count_load_time` | `ktime_t count_load_time`（`i8254.h:22`） |
| `struct hrtimer pit_timer` | `struct hrtimer timer`（在 `kvm_kpit_state` 中，`i8254.h:31`） |
| `atomic_t pit_state_pending` | **不存在**。实际有 `atomic_t reinject`、`atomic_t pending`、`atomic_t irq_ack` |
| `int is_periodic` | `bool is_periodic`（`i8254.h:29`） |

---

## 勘误 4：`start_hv_timer()` 文件路径错误

**原文** (README.md §2)：`start_hv_timer()` 在 `vmx.c:2141` <!-- check-refs:ignore -->

**实际**：`start_hv_timer()` 在 **`lapic.c:2141`**，不在 `vmx.c`。它调用 `kvm_x86_call(set_hv_timer)` 分发到 VMX 的 `vmx_set_hv_timer()`。

---

## 勘误 5：TSC-deadline 使用 `vmcs_write64(TSC_DEADLINE, ...)`

**原文** (README.md §2)：TSC-deadline 模式使用 `vmcs_write64(TSC_DEADLINE, deadline)`

**实际**：**不存在 `TSC_DEADLINE` VMCS 字段**。正确流程：

1. Guest 写 `MSR_IA32_TSC_DEADLINE` → KVM 存入 `apic->lapic_timer.tscdeadline`
2. HW 加速使用 `VMX_PREEMPTION_TIMER_VALUE`（32 位 VMCS 字段）通过 `vmcs_write32()` 设置
3. 两步分工：`vmx_set_hv_timer()`（`vmx.c:8129`）算出含 timer-advance 与
   TSC scaling 换算的 deadline 存入 `vmx->hv_deadline_tsc`；真正写
   `vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, delta_tsc)` 的是
   `vmx_update_hv_timer()`（`vmx.c:7205`，写入在 `:7212/:7223/:7226`）

**影响**：这是架构级错误，读者会误解 TSC-deadline 的硬件实现。
（注：本条最初写"`vmx_set_hv_timer()`（`vmx.c:7223` 附近）"也不准——
`:7223` 在 `vmx_update_hv_timer()` 里，`vmx_set_hv_timer()` 定义在 `:8129`。）

---

## 勘误 6：`CPUID.06H:EAX[2]` 检测 TSC scaling

**原文** (README.md §3)：`CPUID.06H:EAX[2]` 表示 TSC scaling 支持

**实际**：CPUID leaf 0x06 是 "Thermal and Power Management"。EAX[2] = ARAT (Always Running APIC Timer)。

VMX TSC scaling 的检测通过 **VMX secondary execution controls MSR**（`SECONDARY_EXEC_TSC_SCALING`），不是 CPUID 06H。

SVM TSC scaling 通过 `CPUID.8000000AH:EDX` 和 `MSR_AMD64_TSC_RATIO` 检测。

---

## 勘误 7：虚构函数 `kvm_arch_check_tsc_migration()` 和字段 `kvm->arch.no_tsc_offset`

**原文** (README.md §3)：存在 `kvm_arch_check_tsc_migration()` 函数和 `kvm->arch.no_tsc_offset` 字段

**实际**：**两者都不存在**于内核源码中。这是虚构的代码。

---

## 勘误 8：`pvclock_vcpu_time_info` 定义位置错误

**原文** (README.md §4, annotations.md §4)：定义于 `arch/x86/include/uapi/asm/kvm_para.h`

**实际**：定义于 **`arch/x86/include/asm/pvclock-abi.h:26`**。`kvm_para.h` 不包含此结构体。

---

## 勘误 9：`pvclock_vcpu_time_info` 字段类型和顺序错误

**原文** (README.md §4)：`version(u32), flags(u32), tsc_timestamp(u64), system_time(u64), tsc_to_system_mul(u32), tsc_shift(s8)`

**实际**（`pvclock-abi.h:26-35`）：

```c
struct pvclock_vcpu_time_info {
    u32   version;           /* 正确 */
    u32   pad0;              /* ← 原文遗漏 */
    u64   tsc_timestamp;     /* 正确 */
    u64   system_time;       /* 正确 */
    u32   tsc_to_system_mul; /* 正确 */
    s8    tsc_shift;         /* 正确 */
    u8    flags;             /* ← 原文写成 u32，实际是 u8 */
    u8    pad[2];            /* ← 原文遗漏 */
};
```

**错误汇总**：
- `flags` 类型：原文 `u32` → 实际 `u8`
- 遗漏字段：`pad0`、`pad[2]`
- 字段顺序：`flags` 在末尾，不在 `version` 之后

---

## 勘误 10：`adjust_timer_advance_ns()` 函数名错误

**原文** (README.md §2, annotations.md §5)：`adjust_timer_advance_ns()`

**实际**：函数名是 **`adjust_lapic_timer_advance()`**（`lapic.c:1840`）。

---

## 勘误 11：Timer Advance 算法描述错误

**原文** (annotations.md §5)：EWMA 算法，公式 `advance_ns = advance_ns * 0.9 + delay * 0.1`

**实际**（`lapic.c:1840-1866`）：**步进式增量调整**，不是 EWMA：

```c
if (advance_expire_delta < 0) {        /* 太早 */
    timer_advance_ns -= ns / LAPIC_TIMER_ADVANCE_ADJUST_STEP;  /* STEP=8，减 1/8 */
} else {                                /* 太晚 */
    timer_advance_ns += ns / LAPIC_TIMER_ADVANCE_ADJUST_STEP;   /* 加 1/8 */
}
if (timer_advance_ns > LAPIC_TIMER_ADVANCE_NS_MAX)  /* MAX=5000 */
    timer_advance_ns = LAPIC_TIMER_ADVANCE_NS_INIT;  /* 重置为 1000 */
```

---

## 勘误 12：Timer Advance 适用模式描述错误

**原文** (annotations.md §5)：Timer Advance "仅适用于 Periodic/One-shot 模式"，TSC-deadline 不需要

**实际**：**正好相反**。代码注释（`lapic.c:62-68`）明确说 "Enable local APIC timer advancement (**tscdeadline mode only**)"。自适应调整由 `expired_tscdeadline` 驱动，是 TSC-deadline 专属的。`start_sw_period()`（`lapic.c:2105`）不使用 `timer_advance_ns`。

---

## 勘误 13：`lapic_timer_advance_ns` 模块参数不存在

**原文** (annotations.md §5, §7)：存在 `lapic_timer_advance_ns` 模块参数（u32，默认 0）

**实际**：
- **模块参数**：只有 `lapic_timer_advance`（boolean，默认 `true`），在 `lapic.c:70-71`
- `timer_advance_ns` 是内部自动调整值，初始化自 `LAPIC_TIMER_ADVANCE_NS_INIT = 1000`（`lapic.c:2932`）
- 存在 debugfs 文件 `lapic_timer_advance_ns` 用于观察（`debugfs.c:67`），但不是 sysfs 可调参数

**陷阱4 中的 `echo 5000 > /sys/module/kvm/parameters/lapic_timer_advance_ns` 不可执行**。

---

## 勘误 14：`tsc_tolerance_ppm` 默认值错误

**原文** (annotations.md §7)：`tsc_tolerance_ppm` 默认值 0

**实际**：默认值是 **250**（`x86.c:167`）。

---

## 勘误 15：陷阱3 代码片段虚构

**原文** (README.md 陷阱3)：

```c
if (vcpu->cpu != old_cpu) {
    new_offset = guest_tsc - host_tsc;
    vmcs_write64(TSC_OFFSET, new_offset);
}
```

**实际**：**这段代码不存在**。实际代码（`x86.c:5014-5023`）：

1. 条件检查是 `kvm_check_tsc_unstable()`，不是 `vcpu->cpu != old_cpu`
2. 使用 `kvm_compute_l1_tsc_offset()`（不是简单减法）
3. 通过 `kvm_vcpu_write_tsc_offset()` 写入（不是直接 `vmcs_write64`）

---

## 勘误 16：`KVM_KVMCLOCK_CTRL` 行号错误

**原文** (README.md §4)：`KVM_KVMCLOCK_CTRL` 在 `x86.c:5100`

**实际**：在 **`x86.c:6195`**。

---

## 勘误 17：`kvm_synchronize_tsc()` 签名在代码块中错误

**原文** (annotations.md §3) 代码块：

```c
void kvm_synchronize_tsc(struct kvm *kvm, u64 data)
```

**实际**（`x86.c:2717`）：

```c
void kvm_synchronize_tsc(struct kvm_vcpu *vcpu, u64 *user_value)
```

- 第一个参数：`struct kvm_vcpu *vcpu`（不是 `struct kvm *kvm`）
- 第二个参数：`u64 *user_value`（指针，不是值）

---

## 勘误 18：`kvm_write_system_time()` 描述误导

**原文** (annotations.md §4)：此函数直接映射 guest 内存并写 `pvclock_vcpu_time_info`

**实际**（`x86.c:2354-2377`）：函数只存储 GPA 并触发 `KVM_REQ_GLOBAL_CLOCK_UPDATE`。实际的 pvclock 写入发生在 `kvm_guest_time_update()`（`x86.c:3215`）。

---

## 勘误 19：TSC scaling 检测写成 `MSR_IA32_VMX_MISC (bit 25)`

**原文** (README.md §3 TSC_MULTIPLIER 框)：
"硬件支持: … VMX: MSR_IA32_VMX_MISC (bit 25 = TSC scaling)"

**实际**：`IA32_VMX_MISC` 与 TSC scaling 无关。VMX TSC scaling 是
**secondary processor-based VM-execution control**（MSR
`IA32_VMX_PROCBASED_CTLS2`）的 bit 25：

- `SECONDARY_EXEC_TSC_SCALING`（`arch/x86/include/asm/vmx.h:79`）
- 位定义 `VMX_FEATURE_TSC_SCALING ( 2*32+ 25)`
  （`arch/x86/include/asm/vmxfeatures.h:85`）
- KVM 检测：`cpu_has_vmx_tsc_scaling()` 查
  `vmcs_config.cpu_based_2nd_exec_ctrl`（`arch/x86/kvm/vmx/capabilities.h:262-265`）

"bit 25"这个数字本身是对的（在 secondary controls 里），错的是所属的 MSR。

---

## 勘误 20：practice/README.md 源码对照表三处行号漂移

**原文** (practice/README.md §源码对照)：

| 原文 | 实际（已核对 6.12.93） |
|------|----------------------|
| `kvm_vm_ioctl_get_clock()` `x86.c:3107` | `x86.c:6995` |
| `kvm_vm_ioctl_set_clock()` `x86.c:7019` | `x86.c:7006`（`:7019` 在其函数体内，"附近"可用，但定义在 `:7006`） |
| `vmx_set_hv_timer()` `vmx/vmx.c:8133` | `vmx/vmx.c:8129` |

其余条目（`kvm_read_l1_tsc` `x86.c:2580`、`vmx_write_tsc_offset` `:1951`、
`vmx_write_tsc_multiplier` `:1956`、`kvm_guest_time_update` `:3215`、
`restart_apic_timer` `lapic.c:2200`、`start_hv_timer` `lapic.c:2141`）均已核对无误。

---

## 勘误 21：TSC-deadline 被当成"时钟源"/"TSC 的演进"

**原文**（README.md 多处）：

- 硬件时钟源全景 ③ TSC："演进: Constant TSC → Invariant TSC → TSC-deadline"
- "各时钟源对比"单表把 TSC、TSC-deadline、APIC Timer、kvmclock 并列成"时钟源"
- 验证清单："画出 PIT → APIC Timer → TSC-deadline → kvmclock 的演进关系"

**实际**（已核对 6.12.93）：clocksource 与 clockevent 是两个独立子系统
（`include/linux/clocksource.h:101` vs `include/linux/clockchips.h:100`）。
**TSC-deadline 是 LAPIC timer 这个 clockevent 设备的一种模式**，不是时钟源，
更不是 TSC 演进出来的东西——它只是拿 TSC 做时间基准：

- `lapic_clockevent` 基本形态 features = PERIODIC|ONESHOT|C3STOP|DUMMY
  （`arch/x86/kernel/apic/apic.c:494-508`），没有任何 TSC 特性位
- 支持 deadline 时 `setup_APIC_timer()` 把设备改名 `lapic-deadline`、
  清掉 PERIODIC、换 `set_next_event = lapic_next_deadline`、
  按 `tsc_khz * (1000 / TSC_DIVISOR)` 注册（`apic.c:584-593`，
  `TSC_DIVISOR` = 8，`apic.c:259`）
- 编程方式 = `rdtsc()` + `wrmsrl(MSR_IA32_TSC_DEADLINE, ...)`
  （`apic.c:419-429`）
- 特性位 `X86_FEATURE_TSC_DEADLINE_TIMER` = CPUID.01H:ECX[24]
  （`arch/x86/include/asm/cpufeatures.h:137`）

**修正**：README 新增 "🧭 关键概念区分：clocksource vs clockevent" 章节
（含设备归属表、lapic-deadline 源码证据、TSC 双路径图）；全景图的"演进"
行去掉 TSC-deadline；对比表按子系统拆成两张；验证清单改为两条独立演进线。

---

## 勘误 22："现代 Linux Guest 默认使用 kvmclock 作为 clocksource"

**原文**（README.md §4 优势框、§5 时钟层次、练习1 预期）：
"→ 现代Linux Guest默认使用kvmclock作为clocksource"、
"1. kvm-clock ← 半虚拟化, 最快, Guest默认选择"。

**实际**（已核对 6.12.93）：默认时钟源由 **rating** 决定，而且内核在
TSC 可靠时**故意让位**：

```c
/* 来源: arch/x86/kernel/kvmclock.c:334-345 */
/* Invariant TSC exposed by host means kvmclock is not necessary:
 * can use TSC as clocksource. */
if (boot_cpu_has(X86_FEATURE_CONSTANT_TSC) &&
    boot_cpu_has(X86_FEATURE_NONSTOP_TSC) &&
    !check_tsc_unstable())
        kvm_clock.rating = 299;      /* 从 400 (kvmclock.c:160) 降下来 */
```

- `tsc` clocksource rating = 300（`arch/x86/kernel/tsc.c:1187-1189`）
- TSC 不稳定时 `init_tsc_clocksource()` 直接不注册 `tsc`
  （`tsc.c:1414-1417`），kvm-clock（400）才成为默认

即：现代 host（invariant TSC + vCPU TSC 对齐）上，guest 默认
clocksource 往往是 **tsc**；kvm-clock 是 TSC 不可靠场景的赢家，
且无论选谁，sched_clock 始终是 kvmclock（`kvmclock.c:321`）。

**修正**：§4 新增 "TSC vs kvm-clock：两个 clocksource 的选型与取舍"
（对比表 + rating 让位代码 + 场景建议）；§5、练习1 预期、验证清单同步改正。

---

## 勘误 23：缺失"VM 启动首次时间同步"内容（补充，非纠错）

**原文**：README 只讲了"冷启动：时间如何进入 Guest"（guest 侧读取），
没有讲 host 侧三个时间基准何时被第一次强制对齐。

**已补充**（均已核对 6.12.93）：

1. **kvmclock 纪元归零**：`kvm_arch_init_vm` 里
   `kvm->arch.kvmclock_offset = -get_kvmclock_base_ns()`（`x86.c:12841`），
   新 VM 的 kvmclock 从 0 开始；`get_kvmclock_base_ns()` 定义
   `x86.c:2300-2310`。QEMU 暂停/恢复经 `hw/i386/kvm/clock.c:163`
   `kvmclock_vm_state_change()` 搬运（GET :105 / SET :189）。
2. **guest TSC 首次强制同步**：每个 `KVM_CREATE_VCPU` 后
   `kvm_arch_vcpu_postcreate()` 调 `kvm_synchronize_tsc(vcpu, NULL)`
   （`x86.c:12463-12470`）；`data == 0` 即强制同步
   （`:2732-2737` 注释原文 "Force synchronization when creating a vCPU"）。
   TSC 稳定 → 复用 `cur_tsc_offset`（`:2774`，首个 vCPU 为 0）；
   不稳定 → `data += nsec_to_cycles(elapsed)`（`:2777-2779`）按墙钟推进。
3. **masterclock 建立**：`kvm_track_tsc_matching()`（`x86.c:2515-2544`）
   在全部 vCPU TSC 匹配且 host 用 TSC 时钟时置
   `KVM_REQ_MASTERCLOCK_UPDATE`（`:2538`），于首次 `KVM_RUN` 的
   `vcpu_enter_guest()` 处理（`:10809-10810`）→ `kvm_update_masterclock()`
   （`:3082`）→ `pvclock_update_vm_gtod_copy()`（`:3015-3045`）；
   `kvm_guest_time_update()` 填 `system_time = kernel_ns + kvmclock_offset`
   （`:3302`）并按 `use_master_clock` 下发 `PVCLOCK_TSC_STABLE_BIT`
   （`:3304-3310`）。这也解释了 practice/实验2 在首次 KVM_RUN 前
   `KVM_GET_CLOCK` 拿不到 `host_tsc`（`__get_kvmclock()` `:3107/:3116`）。

---

## 勘误 24：练习1 的 `/proc/cpuinfo` grep 关键字错误

**原文**：`grep -E "constant_tsc|tsc_deadline|tsc_reliable" /proc/cpuinfo`

**实际**：`/proc/cpuinfo` 里没有 `tsc_deadline`，flag 名是
**`tsc_deadline_timer`**（`arch/x86/include/asm/cpufeatures.h:137`
定义 `X86_FEATURE_TSC_DEADLINE_TIMER (4*32+24)`，注释即
`"tsc_deadline_timer" TSC deadline timer`）。`tsc_reliable`
（`X86_FEATURE_TSC_RELIABLE`，cpufeatures.h:103）一般由裸机平台代码设置，
KVM 不默认暴露给 guest，在 guest 里基本看不到。

**修正**：练习1 改为
`grep -oE "constant_tsc|nonstop_tsc|tsc_deadline_timer|tsc_known_freq|tsc_adjust" /proc/cpuinfo | sort -u`。

---

## 勘误 25：practice/README.md 源码对照表 `kvm_apic_put()` 不存在

**原文** (practice/README.md §源码对照)：

> | 3 | `KVM_SET_LAPIC` | `kvm_apic_put()` | `lapic.c` |

**实际**：`grep -rn "kvm_apic_put" arch/x86/kvm/` 在 6.12.93 里**零命中**，
该函数不存在。`KVM_SET_LAPIC` 的真实链路：

```
case KVM_SET_LAPIC              — x86.c:5917 (kvm_arch_vcpu_ioctl)
→ kvm_vcpu_ioctl_set_lapic()    — x86.c:5122
→ kvm_apic_set_state()          — lapic.c:3103
```

`kvm_apic_set_state()` 内部关键步骤（lapic.c:3103-3140）：
单独 `apic_set_spiv()` 处理 SPIV（保持 SW-disabled 计数正确）→
`memcpy` 整块寄存器 → `kvm_recalculate_apic_map()` → `apic_update_lvtt()` →
`__start_apic_timer(apic, APIC_TMCCT)` → `KVM_REQ_EVENT`。

同表 `KVM_SET_MSRS(TSC_DEADLINE) → vmx_set_msr()` 一行也偏粗：TSC_DEADLINE
属 common MSR，走 `kvm_set_msr_common()`（case @ x86.c:3890）→
`kvm_set_lapic_tscdeadline_msr()`（lapic.c:2585），不经过 `vmx_set_msr()`。 <!-- check-refs:ignore -->

**修正**：源码对照表已按上述链路改写，并补 `KVM_CREATE_IRQCHIP`
（case @ x86.c:7090）与 `vmx_update_hv_timer()`（vmx.c:7205）两行。

---

## 勘误 26：直接用 KVM API 写 TSC_DEADLINE 被静默拒绝（缺 `KVM_SET_CPUID2`）

**背景**：practice/experiment3 最初跑起来后 `KVM_RUN` 永不返回（没有
preemption timer VM-Exit）。用 ftrace `kvm_exit` + kprobe + 最小二分程序
（SET_LAPIC → 写 deadline → 读回）定位，读回为 0，说明写入被拒。

**根因**：`kvm_vcpu_after_set_cpuid()`（`cpuid.c:371` 定义，由 `KVM_SET_CPUID2` →
`kvm_vcpu_ioctl_set_cpuid2()` → `kvm_set_cpuid()`（`cpuid.c:457`）在 `cpuid.c:504` 调进来）只在 guest CPUID leaf 1 带
`X86_FEATURE_TSC_DEADLINE_TIMER` 时才设置
`apic->lapic_timer.timer_mode_mask = 3<<17`（`cpuid.c:398-403`，
`best = kvm_find_cpuid_entry(vcpu, 1); if (best && apic) {...}`）。
没有 `KVM_SET_CPUID2` 时 mask 保持 kzalloc 初值 0，`apic_update_lvtt()`
按 `timer_mode = LVTT & timer_mode_mask` 算出 `timer_mode` 恒为 0
（one-shot）（`lapic.c:1779-1782`）。于是
`kvm_set_lapic_tscdeadline_msr()`（lapic.c:2585）的门
`!kvm_apic_present(vcpu) || !apic_lvtt_tscdeadline(apic)` 命中 → 直接
return，deadline 不生效；而 `KVM_SET_MSRS` **照常返回 1**，没有任何报错。
读回路径 `kvm_get_lapic_tscdeadline_msr()` 共用同一道门，被拒时读回 0，
正好当诊断信号。

另注意：6.12 的 `apic_lvtt_tscdeadline()` 查的是缓存的
`apic->lapic_timer.timer_mode`，**不是** LVTT 寄存器原值
（`lapic.c:554-567`），所以只看 SET_LAPIC 后 LVTT 读回正确没用。

**修正**：experiment3 在建好 vCPU 后先 `KVM_SET_CPUID2`（leaf 1
ECX[24]）；practice/README.md Experiment 3 补"前置条件 ②"一节。
QEMU 总会设 CPUID，所以这个坑只在裸用 KVM API 时暴露。

**验证**：加 CPUID 后最小程序读回 `0x123456789`；完整实验 5/5 中断到达。

---

## 勘误 27：handler 不发 EOI，第二个中断永远不来（实模式用 x2APIC MSR 发 EOI）

**背景**：勘误 26 修复后，experiment3 第 1 轮中断正常到达（延迟 2.010 ms），
第 2 轮 `KVM_RUN` 挂死。

**根因**：中断注入置位 `ISR[0x20]`；最初版 handler 只有 `out 0xe9; iret`，
不发 EOI，ISR 位不释放 → `kvm_apic_has_interrupt()` 里
`highest_irr <= apic_get_ppr()` 成立，同向量的下一个中断被挡。这不是
deadline 臂展问题（deadline 读回非零、preemption timer 正常到期），
纯粹是投递侧被优先级挡住。

**修正与技巧**：实模式线性地址最多 1MB，够不着 xAPIC MMIO（0xFEE00000），
所以用 **x2APIC MSR 发 EOI**（`WRMSR 0x80b`，实模式可执行）。这条路径
有两道门，都要过：
1. `kvm_x2apic_msr_write()` 要求 `apic_x2apic_mode()`，否则静默返回
   （`lapic.c:3313-3314`）→ 需写 `APICBASE(0x1b) = 0xFEE00D00`
   置 `X2APIC_ENABLE`；
2. `kvm_set_apic_base()` 规定 guest CPUID 没有 X2APIC 时，`X2APIC_ENABLE`
   算保留位、写入被拒（`x86.c:675-676`）→ 需先在 `KVM_SET_CPUID2` leaf 1
   里声明 ECX[21]。

APICBASE 位定义：`BSP=(1<<8)`、`X2APIC_ENABLE=(1<<10)`（apicdef.h:153）、
`ENABLE=(1<<11)`（msr-index.h:900-901）；默认值 `0xFEE00900`
（BASE|BSP|ENABLE），加 `0x400` 得 `0xFEE00D00`。

**验证**：修复后 `apic_base` 读回 `0xfee00d00`，5/5 中断全部到达，
间隔稳定在 ~2.003 ms。practice/README.md Experiment 3 补"前置条件 ③"
与更新后的 guest 内存布局（handler 多 `wrmsr(0x80b)`）。

---

## 勘误 28：勘误 26/27 两处表述过度概括（phase8 复核时澄清）

**触发**：`../phase9-capstone` 毕业项目实测时复核了本文件与
`practice/README.md` 引用的两条"前置条件"，发现两处措辞会把读者带偏。
勘误 26/27 记录的**实验本身没错**（experiment3 当时确实完全没调
`KVM_SET_CPUID2`），但由此归纳出的一般性说法不精确。

**澄清 1（对应勘误 26 / README 前置条件 ②）**：
"`kvm_vcpu_after_set_cpuid()` 只在带 `TSC_DEADLINE_TIMER` 时才设 `3<<17`，不设则
mask 保持 0" —— 这是 if/else，不是"设或清零"（`cpuid.c:399-402`）：

```c
if (cpuid_entry_has(best, X86_FEATURE_TSC_DEADLINE_TIMER))
    apic->lapic_timer.timer_mode_mask = 3 << 17;
else
    apic->lapic_timer.timer_mode_mask = 1 << 17;
```

- guest CPUID **有 leaf1 条目**但缺该位 → mask = `1<<17`，写 LVT Timer 的
  deadline 位(bit18)被 `lapic.c:2391` 掩掉（`2<<17`→0=one-shot，
  `3<<17`→`1<<17`=periodic，`apicdef.h:107-109`），deadline 模式设不上。
- 只有**从未给 leaf1 建条目**（`cpuid.c:398` `kvm_find_cpuid_entry` 返回
  NULL，整个 if 块不执行）mask 才保持 kzalloc 的 0 —— experiment3 属此类。

引用行号 `cpuid.c:398-403` 一律应为 `cpuid.c:399-402`。

**澄清 2（对应勘误 27 / README 前置条件 ③）**：
"`kvm_x2apic_msr_write()` 否则静默返回（lapic.c:3313-3314）" —— 该函数是
`return 1`（`lapic.c:3312-3313`），不是"静默"。这个 1 的去向看发起方：
guest 发起的 WRMSR 经 `kvm_emulate_wrmsr()`（`x86.c:2079`）→
`complete_emulated_insn_gp()` → `kvm_inject_gp()` **注入 #GP**；宿主发起的
`KVM_SET_MSRS`（experiment3 用的这条）由 `kvm_do_msr_access()`（`x86.c:500`）
把 1 返回用户态、写入被拒。所以实验观察到"没生效"，机制是"拒绝/#GP"而非
"静默丢弃"。同节 `kvm_set_apic_base()` 的保留位计算在 `x86.c:675-676`、
拒绝判断在 `:678-679`，引用宜写 `x86.c:675-679`。

**同步修正**：`practice/README.md` 前置条件 ②/③ 与文末对照表已按本条改写。

---

## 勘误 29：四个"6.12.93 里根本不存在"的函数名（phase8 check-refs.py 机械扫出）

**触发**：`../phase9-capstone/practice/mini-kvm/check-refs.py` 新增第三条核对
——把文档里每个 `name()` 与它紧贴的 `file:line` 交叉验证（名字不在被引文件的定义
区间内就报），扫出四个**在 6.12.93 全树 grep 不到**的名字（phase8 那侧记为 J14/J15
两轮）。它们都不是"行号漂移"，而是照抄了旧内核或别的命名，静态读代码时看不出问题。

| 旧写法 | 6.12.93 实际 | 出现位置 |
|--------|-------------|----------|
| `kvm_update_cpuid()` | `kvm_vcpu_after_set_cpuid()`（`cpuid.c:371`） | 5 处，见下 | <!-- check-refs:ignore -->
| `kvm_arch_set_tsc_khz()` | `kvm_set_tsc_khz()`（`x86.c:2465`）→ `set_tsc_khz()`（`x86.c:2429`） | `practice/README.md` 第 50 行（Experiment 1 代码路径）、第 280 行（对照表） | <!-- check-refs:ignore -->
| `apic_get_ppr()` | `apic_has_interrupt_for_ppr()` 里的 `(highest_irr & 0xF0) <= ppr`（`lapic.c:963`） | `practice/README.md` 第 189 行（前置条件 ③）、`practice/experiment3-lapic.c` 第 107-109 行 | <!-- check-refs:ignore -->
| `__kvm_apic_update_eoi()` | `apic_set_eoi()`（`lapic.c:1489`） | `practice/README.md` 第 229-231 行（第 3 节链路）、第 292 行（对照表 EOI 行）、`practice/experiment3-lapic.c` 第 233-235 行 | <!-- check-refs:ignore -->

判据先说清：这四个名字在本树**全树 0 命中**——用
`grep -rn '\<kvm_update_cpuid\>' --include='*.c' --include='*.h'` 这类命令数的
（`/root/code/linux-6.12.93/` 不是 git 仓库，`git grep` 在这里只会报
`not a git repository`，拿它的空输出当"查无此名"是个假绿灯，本轮踩过一次）。
四条链路逐级读源码确认：

- **CPUID**：`case KVM_SET_CPUID2`（`x86.c:5957`）→ `kvm_vcpu_ioctl_set_cpuid2()`
  （调用 `x86.c:5964`，定义 `cpuid.c:555`）→ `kvm_set_cpuid()`（定义
  `cpuid.c:457`，被调 `cpuid.c:571`）→ `kvm_vcpu_after_set_cpuid()`（定义
  `cpuid.c:371`，被调 `cpuid.c:504`），`timer_mode_mask` 的 if/else 在它内部
  （`cpuid.c:399-402`，见勘误 28）。别和 `kvm_update_cpuid_runtime()`
  （`cpuid.c:340`）混，那是运行时能力刷新。
- **TSC**：`case KVM_SET_TSC_KHZ`（`x86.c:6173`）→ `kvm_set_tsc_khz()`
  （`x86.c:2465`）→ `set_tsc_khz()`（`x86.c:2429`）→
  `kvm_vcpu_write_tsc_multiplier()`（`x86.c:2636`）写
  `vcpu->arch.tsc_scaling_ratio`（`x86.c:2646`）。
- **PPR**：`kvm_apic_has_interrupt()`（`lapic.c:2965`）先 `__apic_update_ppr()`
  （`lapic.c:968`）再进 `apic_has_interrupt_for_ppr()`（`lapic.c:956`），判据
  `highest_irr == -1 || (highest_irr & 0xF0) <= ppr` 在 `lapic.c:963`。
  本树根本没有 PPR 的读取函数：PPR 由 `__apic_update_ppr()` 就地算出后写回
  `APIC_PROCPRI` 寄存器（`lapic.c:985`），其余地方取局部变量。
- **EOI**：`kvm_x2apic_msr_write()`（`lapic.c:3308`）→ `kvm_lapic_reg_write()`
  （`lapic.c:2297`）的 `case APIC_EOI:`（`lapic.c:2317`）→ `apic_set_eoi()`
  （`lapic.c:1489`，static）→ `apic_clear_isr()`（`lapic.c:798`）清 ISR
  + `apic_update_ppr()`（`lapic.c:990`）。去掉前导下划线的
  `kvm_apic_update_eoi` 在本树同样 0 命中，对外的两个入口是
  `kvm_lapic_set_eoi()`（`lapic.c:2481`）与 `kvm_apic_set_eoi_accelerated()`
  （`lapic.c:1517`），都不在这条 MSR 路径上。

**同步修正**：上表位置已全部改写（下列行号一律按**改后**的文件计）。除本条之外，
本文件另有 5 处 `check-refs:` 豁免标记（勘误 2 的三行原文表、勘误 4 的原文行、
第 402 行"不经过 `vmx_set_msr()`"那句），都是**故意保留的错误原文**，不是漏网。
`kvm_update_cpuid` 的 5 处改点：`practice/README.md` 第 174 行（前置条件 ②）、
第 288 行（对照表 `KVM_SET_CPUID2` 行）、`practice/experiment3-lapic.c` 第 98 行、
本文件第 415-416 行（勘误 26 根因）、第 480 行（勘误 28 澄清 1 的引用块）。 <!-- check-refs:ignore -->

---

## 已验证正确的内容

以下声明经核查**确认正确**：

- ✅ APIC Timer 三种模式（Periodic/One-shot/TSC-deadline）
- ✅ `restart_apic_timer()` 在 `lapic.c:2200`
- ✅ `__start_apic_timer()` 在 `lapic.c:2258`
- ✅ `vmx_write_tsc_offset()` 在 `vmx.c:1951`
- ✅ `vmx_write_tsc_multiplier()` 在 `vmx.c:1956`
- ✅ `kvm_guest_time_update()` 在 `x86.c:3215`
- ✅ SVM 使用 `MSR_AMD64_TSC_RATIO`
- ✅ `CPUID.80000007H:EDX[8]` = TSC invariant
- ✅ `pvclock_wall_clock` 字段（version, sec, nsec）
- ✅ `kvmclock_periodic_sync` 模块参数
- ✅ IO ports 0x40-0x43, 0x61
- ✅ APIC 寄存器偏移 0x320, 0x380, 0x390, 0x3E0
- ✅ Channel 0 → IRQ0 → PIC

---

## 参考

- 核查基线：`/root/code/linux-6.12.93/`
- 相关文件：
  - `arch/x86/kvm/i8254.c` / `i8254.h` — PIT
  - `arch/x86/kvm/lapic.c` / `lapic.h` — APIC Timer
  - `arch/x86/kvm/vmx/vmx.c` — VMX TSC
  - `arch/x86/kvm/x86.c` — TSC, kvmclock
  - `arch/x86/include/asm/pvclock-abi.h` — pvclock 结构体

---

## 勘误（2026-11-25 框架改造）：性能数字无出处 + 结构变更

**性能数字待审计**：旧 README「⚡ 性能优化技术」与 VMM 对比含
"TSC-deadline 减少 VM-Exit 90%""中断延迟 ~20ns""读 PIT ~1000/300ns、kvmclock ~30ns"
无本仓实测出处。已改「⚡ 性能与测量」纯指针（→ phase10），机制（TSC-deadline 硬件比较/
Timer Advance 自适应/TSC 同步）保留，数字标待审计。Timer Advance 参数权限（0444 只读、
per-vCPU 自适应）已正确指向 phase10 parameters.md，保留。

**practice README 标题旧编号**：`practice/README.md:1` "Phase 7" → "Phase 8"，已修。

**结构变更（非错误）**：README >1000 行强制拆分——
Linux 时钟基础(508行) → [clock-basics.md](clock-basics.md)，
KVM 时钟虚拟化详解(1941行) → [kvm-clock-virtualization.md](kvm-clock-virtualization.md)，内容未改。
`vmclock-phase1-analysis.md` → [vmclock-analysis-stage1.md](vmclock-analysis-stage1.md)
（避免与 phase1-cpu-virt 混淆，COURSE_FRAMEWORK §5 命名规范）。

---

## 勘误 30（2026-11-25 二轮事实核查，已同步修正正文）

> 核查材料：`/root/code/linux-6.12.93/`（基线）、`/root/code/linux`（git，v5.10/
> v5.16/v6.13/v7.3 tags）、QEMU 10.1.0-rc2/11.1.0、Firecracker 本地 git、
> util-linux v2.39.3（`/tmp/util-linux-src`）、宿主实测（**Ubuntu 6.8.0-51-generic**）。
> 以下所有"实际"均经源码 grep 或本机运行核实。

### 30.1 pvclock 换算公式 ">> (22 + tsc_shift)" 常数与移位方向均错

**原文**：annotations.md §4/§8、kvm-clock-virtualization.md §4 框：
`delta_ns = (delta_tsc × mul) >> (22 + tsc_shift)`

**实际**：guest 侧唯一实现 `pvclock_scale_delta()`
（`arch/x86/include/asm/pvclock.h:44-77`）：shift≥0 → `delta <<= shift`
（shift<0 → `delta >>= -shift`），随后 `(delta × mul) >> 32`（x86_64 为
`mulq + shrd $32`）。合并闭式 = **`(delta × mul) >> (32 − tsc_shift)`**。
常数 22 在任何定义中都不存在（VMX/SVM/pvclock 的定点小数都是 32 位）。

### 30.2 README §性能-3 给 `kvm_synchronize_tsc()` 配了不存在的代码体

**原文**：`if (kvm_check_tsc_unstable()) { kvm->arch.use_master_clock = false; }`，
并称该函数"检查各 pCPU 的 TSC 偏移，差异过大则标记为不稳定"。

**实际**：`x86.c:2717-2786` 全函数无 `use_master_clock` 赋值、也不标记 unstable——
它**消费** host 已有的 TSC 状态：稳定→共享 `cur_tsc_offset`（:2774），
不稳定→`data += nsec_to_cycles(elapsed)` 重算 offset（:2776-2779）。
`use_master_clock` 的决定在 `pvclock_update_vm_gtod_copy()`（`x86.c:3034-3037`）；
marking 来自 host tsc watchdog 与 KVM `vcpu_load` 的
`mark_tsc_unstable("KVM discovered backwards TSC")`（`x86.c:5018`）。
正文已替换为真实节选（x86.c:2771-2780）。

### 30.3 KVM_GET_CLOCK/KVM_SET_CLOCK 被当成 /dev/kvm 或 vCPU ioctl

**原文**：README 练习5（`open("/dev/kvm")` 后直接 `ioctl(fd, KVM_GET_CLOCK,...)`）、
练习2（grep debugfs 里的 pvclock GPA）、kvm-clock-realtime.md 全篇 24 处
`ioctl(vcpu_fd, KVM_GET/SET_CLOCK,...)`。

**实际**：两者均为 **VM ioctl**（api.rst §4.29/§4.30 "Type: vm ioctl"，
`Documentation/virt/kvm/api.rst:1021-1027/:1071`；分发在 `kvm_vm_ioctl`，
`x86.c:7307-7311`）。本机实测（自写程序，宿主 6.8.0-51）：
`/dev/kvm` fd → EINVAL，vCPU fd → EINVAL，VM fd → 成功。
另：KVM 没有导出 pvclock 页 GPA 的 debugfs 入口——per-vCPU 只有
`guest_mode`/`tsc-offset`/`lapic_timer_advance_ns`/`tsc-scaling-ratio*`
（`arch/x86/kvm/debugfs.c:61-75`，本机实测一致）。练习5 示例已改为
KVM_CREATE_VM 后用 VM fd，realtime 篇 `vcpu_fd` 已全部改 `vm_fd`。

### 30.4 pvclock 页锚点字段语义写错（kvm-clock-virtualization.md §4 框）

**原文**："tsc_timestamp ← 写结构时的 Host TSC"、"system_time ← 写结构时的
Host monotonic 时间"。

**实际**：`tsc_timestamp = kvm_read_l1_tsc(v, host_tsc)` 是 **guest(L1) TSC**
（`x86.c:3265`）；`system_time = kernel_ns + kvmclock_offset` 是 **guest kvmclock**
（`x86.c:3302`）。若真是裸 host TSC，任何 TSC_OFFSET≠0 的 VM 该协议都不自洽。

### 30.5 lapic timer advance 参数接口：宿主与 6.12.93 基线不一致（已加显著标注）

**基线 6.12.93**（文档描述正确）：只有 bool `lapic_timer_advance`（`lapic.c:70-71`，
0444）；提前量 per-vCPU 自适应、经 debugfs 只读观察（`debugfs.c:67`）。
**宿主 Ubuntu 6.8.0-51-generic 实测**：**没有** `lapic_timer_advance` 参数；
存在 int 参数 `lapic_timer_advance_ns`（`/sys/module/kvm/parameters/`，
**-rw-r--r--（0644，运行时可写）**，当前值 **-1**）。README/annotations 相关命令
处已加"宿主版本差异"框；用宿主观测口前先
`ls /sys/module/kvm/parameters/ | grep timer`。

### 30.6 `kvm_queue_pending_eoi()` 不存在

**原文**：annotations.md §2/§8 称 `apic_timer_expired()` 通过
`kvm_queue_pending_eoi` 或 `kvm_vcpu_kick()` 唤醒 vCPU。

**实际**：全树 grep 零命中。真实路径（`lapic.c:1915-1950`）：
`atomic_inc(&lapic_timer.pending)` + `kvm_make_request(KVM_REQ_UNBLOCK)`，
仅 `from_timer_fn` 时 `kvm_vcpu_kick()`；APICv/posted-timer 路径走
`kvm_apic_inject_pending_timer_irqs()`（:1925-1943）。

### 30.7 VMX `TSC_MULTIPLIER` 的 1.0 写成了 SVM 的值

**原文**：annotations.md §3 "1.0 = 0x0000000100000000"。

**实际**：VMX 是 **16.48** 定点（SDM Vol.3 "TSC Scaling"；
`kvm_caps.tsc_scaling_ratio_frac_bits = 48`，`vmx.c:8502`），
1.0 = 2^48 = `0x0001000000000000`；`0x100000000`(2^32) 对应 **SVM**
（`svm.c:5478`，frac_bits=32）。kvm-clock-virtualization.md 原本就写对了，
两处已统一。

### 30.8 kvm-clock-realtime.md 三组行号是旧内核残留

**原文 → 实际（6.12.93）**：
- `__get_kvmclock` "x86.c:1661-1694" → **x86.c:3107-3139**
- `kvm_vm_ioctl_set_clock` "x86.c:4321-4358" → **x86.c:7006-7048**
- "`include/uapi/linux/kvm.h:1085-1099`（含 KVM_CLOCK_VALID_FLAGS）" →
  flags/结构在 **kvm.h:1025-1038**；`KVM_CLOCK_VALID_FLAGS` **不在 uapi**，
  在 `arch/x86/include/asm/kvm_host.h:2377-2378`。

### 30.9 GET_CLOCK 快照 flags："0xC / 5.16" 数字错

**原文**（kvm-clock-virtualization.md 迁移注意事项）：
"5.16+ 主机打的快照带 flags=0xC，灌到 5.10 KVM 上会直接 -EINVAL"。

**实际**：masterclock 生效时 `__get_kvmclock` 置
TSC_STABLE|REALTIME|HOST_TSC = 2|4|8 = **0xE**（`x86.c:3123/:3128`；
git 核实 v5.16 的 `x86.c:2856/:2861` 与 uapi 值完全相同，即 5.16 起就是这套编号）。
**-EINVAL 半句成立**：git `v5.10` `arch/x86/kvm/x86.c:5619-5621` 对一切非零
flags 直接 `-EINVAL`。

### 30.10 `KVM_SET_TSC_OFFSET` ioctl 不存在

**原文**（kvm-clock-virtualization.md 两处）：把用户态设置 TSC 偏移写成
`KVM_SET_TSC_OFFSET` ioctl。

**实际**：uapi 只有 `KVM_SET_TSC_KHZ/KVM_GET_TSC_KHZ`
（`include/uapi/linux/kvm.h:1223-1224`）。真实接口：
① `KVM_SET_MSRS` 写 `MSR_IA32_TSC`（host_initiated →
`kvm_synchronize_tsc`，`x86.c:3939-3940`）；
② **`KVM_VCPU_TSC_OFFSET` vCPU device attribute**
（`arch/x86/include/uapi/asm/kvm.h:920`；经 `KVM_SET_DEVICE_ATTR` 落在
`kvm_arch_tsc_set_attr`，`x86.c:5757`，case @ :5765）。

### 30.11 practice 实验2 预期 `KVM_CAP_ADJUST_CLOCK = 0x9` 错

**实际**：`case KVM_CAP_ADJUST_CLOCK: r = KVM_CLOCK_VALID_FLAGS`（`x86.c:4714`）
= 2|4|8 = **0xE**；宿主 6.8.0-51 实测 `0xe`。预期输出已改。

### 30.12 README 陷阱2 与勘误 22 矛盾（残留旧结论）

**原文**："current_clocksource 应该是 kvm-clock；不是就切到 kvm-clock"。

**实际**：现代 host（invariant TSC + vCPU 对齐）guest 默认**应是 tsc**
——kvmclock 主动 400→299 让位（`kvmclock.c:342-345`）。陷阱2 已改写为
"tsc 没被选中先查原因，仅 TSC unstable 时固定 kvm-clock"。

### 30.13 kvm-clock-realtime.md §1.1 核心公式失真

**原文**："guest_time = host_tsc × tsc_to_system_mul + system_time"，
"system_time = 宿主机 CLOCK_MONOTONIC"。

**实际**：`__pvclock_read_cycles`（`arch/x86/include/asm/pvclock.h:84-89`）：
`ns = pvclock_scale_delta(guest_tsc − tsc_timestamp) + system_time`；
锚点与读取同为 guest TSC；system_time 含 kvmclock_offset（`x86.c:3302`）。
§1.1 已重写；连带 30.1（"22+shift"）同源。

### 30.14 vmclock.md：「KVM/QEMU Host 侧未实现」对 QEMU 而言已过时

**实际**：QEMU 自 2025-01-07 commit **`3634039b93`**（"hw/acpi: Add vmclock
device"，git 核实）即有 host 侧设备，本仓基线 `qemu-10.1.0-rc2/hw/acpi/vmclock.c`
就在：分配 `VMCLOCK_ADDR 0xfeffb000` 4KiB 页（`include/hw/acpi/vmclock.h:11`）、
生成 SSDT、realize 写常量、post_load 递增 `disruption_marker`（:64-97）。
**仅填充时间参数的 KVM 内核侧确实未实现**（7.4.0-rc3 树 `VMCLOCK_MAGIC` 只有
`drivers/ptp/ptp_vmclock.c` 命中）。§8.1 表与 §8.4 已拆分改写；QEMU 也不填
时间字段（counter_id=INVALID）。

### 30.15 vmclock.md §7.1 ACPI 示例的 `_HID "VCLK0001"`/`_CID "VMCLK0001"` 无人匹配

**实际**：内核驱动只匹配 **AMZNC10C / VMCLOCK**（`drivers/ptp/ptp_vmclock.c:771-774`），
QEMU/Firecracker 亦同（`hw/acpi/vmclock.c:40-41`、`devices/acpi/vmclock.rs:200-201`）。
示例已按 QEMU 实际 AML 改写（device VCLK、_STA 0xf、QWORD cacheable/read-only
0xfeffb000）；"通知码 0x80"本机无规范原文可证，标**待核实**（内核 handler 不检查
event 值，`ptp_vmclock.c:522-530`）。

### 30.16 vmclock.md §7.2 DT `compatible = "vmclock"` 不会绑定

**实际**：binding 为 const **`amazon,vmclock`**
（`Documentation/devicetree/bindings/ptp/amazon,vmclock.yaml:20`；驱动 of 表
`ptp_vmclock.c:778-780`；Firecracker fdt 同值）。

### 30.17 vmclock 驱动合入版本："Linux 7.0+" / "7.0 (2026)" 错

**实际**：commit `205032724226`（author 2024-10-06）首个包含它的 tag 是
**v6.13**（`git tag --contains` 于 /root/code/linux 核实）。vmclock.md §8.1/§8.5
与 kvm-clock-realtime.md §6.1 对比表均已改。

### 30.18 "clock_status=UNKNOWN → 驱动返回 -EINVAL" 臆造

**实际**：驱动唯一的状态检查是 **UNRELIABLE**（`ptp_vmclock.c:124-125`）。
Phase 1 无法同步的真实机制：`counter_id=INVALID` 使 `vmclock_ptp_register()`
直接 `return NULL`（`ptp_vmclock.c:332/:338-345`，probe 调用点 :749），PTP 设备
根本不存在，谈不上返回错误；可用面只剩 mmap 通知字段。

### 30.19 "Firecracker Phase 1 提供迁移通知" 名不副实

**实际**：FC 无热迁移——`disruption_marker` 唯一递增路径是快照恢复
`do_post_restore()`（`devices/acpi/vmclock.rs:122-153`；函数注释即
"after snapshot restore"；引入 commit `ef215b91b` 原话 "we don't support live
migration, the device won't do anything at the moment"）。FC 文档中
"Live migration through the disruption_marker" 是设计意图。迁移递增目前只有
**QEMU** 实现（`hw/acpi/vmclock.c:78-80`）。vmclock.md §8.3 与
vmclock-analysis-stage1.md §功能分解/价值2 已改写。

### 30.20 §4.2 定点数示例中间十进制值错

**原文**：`0x44B82FA0A` = "18014398506.97"；`0x89705F4136B4A597` =
"9765625000734658967"；`2^93` = "5.76×10²⁷"。

**实际**（Python 精确计算）：`0x44B82FA0A` = **18446744074**；
`0x89705F4136B4A597` = **9903520314283042199**（恰为 10⁻⁹×2⁹³）；
2⁹³ ≈ **9.9035×10²⁷**。原数值组合的除法结果 ≈1.69×10⁻⁹，与其自称的 1×10⁻⁹
都不自洽。已修正。

### 30.21 行号/措辞类小项（已随手修正）

| 原文 | 实际 |
|------|------|
| debugfs 文件名 `tsc_offset` | `tsc-offset`（`arch/x86/kvm/debugfs.c:63`，连字符） |
| "kvm_arch_init_vm **归零** kvmclock_offset" | `= -get_kvmclock_base_ns()`（`x86.c:12841`）：归零的是 clock 纪元，offset 非 0 |
| get_kvmclock_base_ns "TSC 时钟下/否则" 两分支 | 按**编译期** `#ifdef CONFIG_X86_64`（`x86.c:2247/#else :2305`）选择，非运行时 |
| "检测 CPUID 0x40000001 (KVM 签名)" | 签名 leaf = **0x40000000**（`KVM_CPUID_SIGNATURE`，`kvm_para.h:10`）；0x40000001 是 feature 页 |
| wall epoch `wall_nsec = ktime_get_real_ns() − get_kvmclock_ns()` | 这是**回退路径**（`x86.c:3400`）；主路径为 `kvm_get_wall_clock_epoch()` 原子配对（`x86.c:3346-3397`） |
| `struct kvm_clock_pairing { u64 sec; u64 nsec; u64 tsc; u32 flags; }` | sec/nsec 为 **`__s64`**，且有 `__u32 pad[9]`（`arch/x86/include/uapi/asm/kvm_para.h:75-81`） |
| FC `vmclock.rs` 289 行 / `tests/host_tools/vmclock.c` 188 行 | `wc -l` 实测 **288 / 187** |
| FC Phase 1 "首次合入 2025-10-21" | author 2025-10-21，committer（进 main）**2025-11-20**（`git show ef215b91b`） |
| "kvmclock 支持迁移（TSC不行）" | tsc 同样可迁移（VMM 恢复 TSC 值 + offset/scaling 补偿；无 scaling 硬件时跨频迁移不可行），已限定措辞 |

### 30.22 未标注数字的处置

kvm-clock-virtualization.md "时钟读取性能对比"（PIT ~1000ns 等）、vDSO 框
（~100-500ns / ~20-30ns / 10-100 倍）、ptp_kvm（hypercall ~μs、期望偏移 <1μs）
与 README 练习4（"预期亚微秒级"）——README 早已声明这些**无本仓实测出处**，
但拆出文档未带标记。本次按 phase10 规则统一标注"量级估计，本仓未实测，
待实测（方法见 phase10/measurement.md）"，未改机制叙述。

### 30.23 待核实（本机无资料，不臆断）

- VMClock 规范版本自相矛盾：文头 v1.0（"2025-12-15 发布"）vs 正文/引用 v1.1；
  本机无 UAPI.13 规范 PDF，两处的版本与日期归属均**待核实**。
- ACPI 通知码 0x80：内核 handler 不检查 event 值（`ptp_vmclock.c:522-530`），
  规范文本本机不可得，**待核实**。
- §4.2 "运行 1 天误差 ≈ 几微秒/1 月 ≈ 几十微秒"：按 1ns tick 的 2⁻⁶⁴ s 量化
  误差量级推算与结论一致，但非本仓实测，保留为量级说明。
- Firecracker PR 编号 #5809：机制叙述与 6.12.93 代码、Firecracker 现行代码
  （`vm.rs restore_state(..., clock_realtime)`，默认 `flags=0`）吻合；PR 编号
  本身本机不可核，**待核实**。
