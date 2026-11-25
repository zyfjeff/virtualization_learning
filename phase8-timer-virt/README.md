# Phase 8｜时钟虚拟化

> guest 怎么知道时间？宿主 TSC 可能变频/迁移不同步，kvmclock 怎么给 guest 稳定时钟？
> 前置：phase1（TSC MSR）、phase0（KVM 框架）、phase5（APIC Timer 中断）
> 版本基线：Linux 6.12.93 | Intel SDM Vol.3 Ch 17（TSC）/ Ch 10（APIC Timer）
>
> 章节骨架遵循 [`../COURSE_FRAMEWORK.md`](../COURSE_FRAMEWORK.md)。

---

## 📋 学习目标

**核心问题**：guest 要读时间（clocksource）和定时（clockevent）。裸机用 TSC/HPET/APIC Timer，
但虚拟化下 TSC 可能变频、迁移后不同步。guest 怎么拿到一个单调、稳定、迁移连续的时间？

1. 理解 Linux clocksource/clockevent 框架与硬件时钟源全景
2. 掌握 kvmclock/pvclock 共享页机制与 TSC 偏移/缩放
3. 理解 TSC 可靠性判定与 kvm-clock rating 让位
4. 理解 lapic timer advance 自适应（参数权限 → phase10）
5. 了解 PTP/chrony 精度、VMClock/VMGenID 互补

---

## 📚 前置与衔接

- **前序 phase**：phase1 的 TSC MSR（IA32_TSC）、phase5 的 APIC Timer（clockevent 中断）。
- **VMM 经验衔接**：你在 QEMU 用 `-cpu host` 透传 TSC、见过 guest `dmesg` 的
  "tsc: Refined TSC clocksource calibration"。本章讲内核怎么维护 guest 时钟。

---

## 📂 本章文件

问题接力（§3.2 规则 6）：

```
README 第一性原理（核心问题：guest 怎么拿到稳定时间？）
  │ 引出：Linux 时钟框架和硬件时钟源是什么？
  ▼
clock-basics.md（clocksource/clockevent + TSC/HPET/PIT/APIC 全景）
  │ 引出：KVM 怎么虚拟化这些时钟？
  ▼
kvm-clock-virtualization.md（kvmclock/pvclock/TSC 偏移/PTP）
  │ 引出：实时性怎么保证？迁移时钟怎么连续？
  ▼
kvm-clock-realtime.md + vmclock.md + vmclock-analysis-stage1.md ── 取证 → practice
```

| 文件 | 回答什么 | 引出什么 → 去哪 |
|------|---------|----------------|
| `README.md` | 核心问题 + 第一性原理 + 全景 | 时钟框架 → [clock-basics.md](clock-basics.md) |
| [clock-basics.md](clock-basics.md) | ★ Linux clocksource/clockevent + 硬件时钟源 | KVM 虚拟化 → [kvm-clock-virtualization.md](kvm-clock-virtualization.md) |
| [kvm-clock-virtualization.md](kvm-clock-virtualization.md) | ★ kvmclock/pvclock/TSC 偏移/PTP/chrony | 实时性 → [kvm-clock-realtime.md](kvm-clock-realtime.md) |
| [kvm-clock-realtime.md](kvm-clock-realtime.md) | 实时场景时钟精度 | VMClock → [vmclock.md](vmclock.md) |
| [vmclock.md](vmclock.md) | VMClock 设备 | 分析 → [vmclock-analysis-stage1.md](vmclock-analysis-stage1.md) |
| [annotations.md](annotations.md) | 问题链精读：kvmclock/TSC/lapic | 取证 → [practice/](practice/README.md) |
| [corrections.md](corrections.md) | 勘误 | 蒸馏 → AGENTS.md |

---

## 📖 学习路线

```
第1步: 核心问题 + 第一性原理（本文件）
第2步: clock-basics.md — clocksource/clockevent + 硬件时钟源
第3步: VMM 对比（本文件）— 时钟为什么内核维护
第4步: kvm-clock-virtualization.md — kvmclock/pvclock/TSC 偏移
第5步: 问题链精读 annotations；kvm-clock-realtime/vmclock 进阶
第6步: practice ex1-3 取证；参数权限 → phase10
第7步: 陷阱勘误；深度自测 → phase9（毕业建造）
```

---

## 🏗️ 第一性原理与技术全景

### 根本问题

guest 要读时间（clocksource）和定时（clockevent）。裸机用 TSC/HPET/APIC Timer。
虚拟化下物理 TSC 可能变频、vCPU 迁移到不同 pCPU 后 TSC 不同步——guest 不能直接信物理 TSC。

### 不可变约束

| 约束 | 内容 |
|------|------|
| 向后兼容 | guest OS 时钟驱动不改 |
| 单调稳定 | 时钟不能回退/跳变 |
| 迁移连续 | vCPU 迁移后 guest 时间连续 |
| 精度 | 实时场景要亚微秒 |

### 设计空间与取舍

- **TSC 透传**：guest 直接读 TSC（快，vDSO 无 VM-Exit），但变频/迁移要 offset/scaling 补偿。
- **kvmclock（半虚拟化）**：宿主维护 pvclock 共享页，guest 读页算时间，免 VM-Exit、
  迁移连续（宿主更新偏移）。代价：需 guest 支持 kvm-clock 驱动。
- **模拟 HPET/PIT**：全模拟，慢（每次读 VM-Exit），兜底。
- **取舍**：kvmclock 用共享内存换"稳定 + 免 exit"；TSC 透传配偏移处理迁移。
  guest TSC 可靠时 tsc 优先（rating 高），kvm-clock 让位。

### 机制全景

```
clocksource（读时间）：tsc / kvm-clock / hpet / acpi_pm
clockevent（定时中断）：lapic-deadline / lapic / hpet
kvmclock：宿主写 pvclock 共享页（tsc_timestamp/system_time/offset）→ guest vDSO 读算
TSC 虚拟化：VMCS TSC_OFFSET + TSC_SCALING → 迁移/变频补偿
```

详见 [clock-basics.md](clock-basics.md) 与 [kvm-clock-virtualization.md](kvm-clock-virtualization.md)。

---

## 🔍 VMM视角对比

| 方面 | 用户态 VMM (QEMU) | KVM 内核态 |
|------|------------------|-----------|
| 时钟维护 | 设 TSC 频率/迁移通知 | 维护 pvclock 页、TSC offset/scaling |
| 读时间 | — | guest vDSO 读共享页（无 VM-Exit） |
| 定时 | — | lapic timer 注入（advance 自适应） |
| 迁移 | KVM_GET/SET_CLOCK | 内核更新偏移保证连续 |

**为什么时钟内核维护**：TSC 偏移/pvclock 要随 vCPU 调度/迁移实时更新，用户态来不及。
参数默认值与权限见 [phase10 parameters.md](../phase10-performance/parameters.md)。

---

## 📖 问题链精读入口

| 材料 | 入口问题 | 关键引申 | 源码入口 |
|------|---------|---------|---------|
| clock-basics | clocksource 和 clockevent 为什么分开？ | rating 怎么决定谁优先？ | `kernel/time/` |
| kvm-clock-virtualization | kvmclock 怎么免 VM-Exit？ | pvclock 页谁写谁读？ | `arch/x86/kernel/kvmclock.c` |
| annotations（TSC） | 迁移后 TSC 怎么连续？ | offset/scaling 何时更新？ | `vmx.c` TSC 处理 |
| annotations（lapic） | timer advance 为什么自适应？ | 为什么是 per-vCPU 不是模块参数？ | `lapic.c:75` |

---

## 🕰️ Linux 时钟系统基础

> 本节详情已拆至 **[clock-basics.md](clock-basics.md)**（内容未改）：
> clocksource/clockevent 框架、TSC/HPET/PIT/APIC Timer 硬件全景。

## 🔧 KVM 时钟虚拟化详解

> 本节详情已拆至 **[kvm-clock-virtualization.md](kvm-clock-virtualization.md)**（内容未改）：
> kvmclock/pvclock、TSC 偏移/缩放、PTP/chrony 精度、lapic timer advance。

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
# ⚠️ KVM **没有**导出 pvclock 页 GPA 的 debugfs 入口（arch/x86/kvm/debugfs.c:61-75
# 只创建 guest_mode、tsc-offset、lapic_timer_advance_ns、tsc-scaling-ratio、
# tsc-scaling-ratio-frac-bits；实测宿主 vcpu0/ 亦如此）。想验证 guest 在用
# 半虚拟化时钟，可看每 vCPU 的 TSC 锚点和 KVM_GET_CLOCK：
cat /sys/kernel/debug/kvm/<pid>-<vmid>/vcpu0/tsc-offset
# 配合练习5 的 KVM_GET_CLOCK 程序观察 clock/flags/host_tsc

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

# 预期: stratum 1, 精度亚微秒级（量级预期，本仓待实测；方法见 phase10/measurement.md）
```

### 练习5: 观察 kvmclock_offset 变化

```bash
# 在 Host 上观察 kvmclock_offset
# 需要 debugfs 和 root 权限

# 方法 1: 通过 KVM_GET_CLOCK ioctl
# ⚠️ KVM_GET_CLOCK / KVM_SET_CLOCK 都是 **VM ioctl**（api.rst §4.29/§4.30
# "Type: vm ioctl"；内核分发在 kvm_vm_ioctl，x86.c:7307-7311）。
# 实测（2026-11-25，宿主 6.8.0-51）：在 /dev/kvm fd 或 vCPU fd 上调用返回
# EINVAL，必须用 KVM_CREATE_VM 创建的 VM fd。
cat > /tmp/get_kvmclock.c << 'EOF'
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

int main(void) {
    int kvm_fd = open("/dev/kvm", O_RDWR);
    int vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0UL);   /* ★ 必须 VM fd */
    struct kvm_clock_data data;

    if (ioctl(vm_fd, KVM_GET_CLOCK, &data)) { perror("KVM_GET_CLOCK"); return 1; }
    printf("clock: %llu\n", data.clock);
    printf("flags: 0x%x\n", data.flags);
    printf("realtime: %llu\n", data.realtime);
    printf("host_tsc: %llu\n", data.host_tsc);   /* KVM_CLOCK_HOST_TSC 置位时有效 */

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
- [ ] 说出 VM 启动三个时间基准的首次同步点：`kvm_arch_init_vm` 置
      `kvmclock_offset = -get_kvmclock_base_ns()`（纪元从 0 起算，offset 本身
      非 0，x86.c:12841）、`kvm_arch_vcpu_postcreate` 强制同步 TSC、
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

深度自测（答不出 = 没学会）：

- [ ] **反事实**：如果 TSC 永远可靠且迁移同步，kvmclock 还有存在必要吗？它解决的核心问题是什么？
- [ ] **不可简化**：Timer Advance 为什么是 per-vCPU 自适应值而不是一个全局模块参数？
- [ ] **推导**：从"vCPU 迁移到不同 pCPU、TSC 可能不同步"推出"需要 offset 补偿或共享页时钟"。

---

## 🚀 下一步

- **Phase 9（毕业建造）**：用前面所有机制造最小 VMM
- **本章进阶**：KVM_CLOCK_REALTIME（Firecracker PR #5809）、VMClock/VMGenID

---

## ⚡ 性能与测量

- 实测结论与分级：[`../phase10-performance/index.md`](../phase10-performance/index.md)
- 参数默认值与权限：[`../phase10-performance/parameters.md`](../phase10-performance/parameters.md)
- 本章现状：旧讲义"TSC-deadline 减少 VM-Exit 90%""中断延迟 ~20ns""读 PIT ~1000/300ns、
  kvmclock ~30ns"**无本仓实测出处**，登记 corrections 待审计。
  机制（TSC-deadline 硬件比较/Timer Advance 自适应/TSC 同步）保留如下，数字标待实测。

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

**效果**（机制，量化待实测）：
- 硬件自动比较 TSC 与 deadline，省去周期性写 TMICT/除数的 VM-Exit
- 中断贴近 guest 编程的 deadline（配合 Timer Advance）

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

**配置与观察**（以下按 **6.12.93 基线**）：
```bash
# 唯一的模块参数是 bool 开关（权限 0444，运行时只读，
# 只能在模块加载时改: modprobe kvm lapic_timer_advance=0）
cat /sys/module/kvm/parameters/lapic_timer_advance   # 默认 Y

# 提前量本身是每 vCPU 的内部自适应值，不是模块参数，只能只读观察:
cat /sys/kernel/debug/kvm/<pid>-<fd>/vcpu0/lapic_timer_advance_ns
# 初始 1000 (LAPIC_TIMER_ADVANCE_NS_INIT, lapic.c:75)，自适应调整
```

> ⚠️ **宿主版本差异（2026-11-25 实测）**：当前实验宿主是 Ubuntu
> **6.8.0-51-generic**，其 kvm 模块的参数接口与 6.12.93 基线**不同**：
> 没有 bool `lapic_timer_advance` 参数（上面第一条 `cat` 在本机会报
> No such file），取而代之是一个**运行时可写**的 int 参数
> `lapic_timer_advance_ns`（`/sys/module/kvm/parameters/lapic_timer_advance_ns`，
> 权限 0644，实测当前值 -1）。per-vCPU debugfs 文件在本机存在（实测值 1000）。
> 用宿主观测口做实验前先 `ls /sys/module/kvm/parameters/ | grep timer`
> 确认接口形态；基线机制描述仍以 6.12.93 源码为准。

**效果**：
- 抵消定时器注入延迟，让中断尽量贴近 guest 编程的 deadline
- 详见下文"陷阱4"对常见调优误区的澄清

### 3. TSC同步

**问题**：多pCPU的TSC不同步

**解决**：host 内核的 TSC watchdog/校准发现不同步后 `mark_tsc_unstable()`
（`arch/x86/kernel/tsc.c`，KVM 自己的 `vcpu_load` 检测到倒退也会标
`mark_tsc_unstable("KVM discovered backwards TSC")`，`x86.c:5018`）。
`kvm_synchronize_tsc()`（`x86.c:2717`）**不检查各 pCPU 的偏移、也不标记
unstable**，它只**消费** host 的稳定状态来决定 offset：TSC 稳定 → 所有 vCPU
共享 `cur_tsc_offset`；不稳定 → 按流逝墙钟推进 guest TSC 值后重算 offset。
`use_master_clock` 的取舍在别处：`pvclock_update_vm_gtod_copy()`
（`x86.c:3034-3037`），masterclock 关掉后 kvmclock 退化为 per-vCPU 现读现算的
慢路径（`x86.c:3260-3263`）。

```c
/* 来源: arch/x86/kvm/x86.c:2771-2780 kvm_synchronize_tsc()（节选） */
	if (synchronizing &&
	    vcpu->arch.virtual_tsc_khz == kvm->arch.last_tsc_khz) {
		if (!kvm_check_tsc_unstable()) {
			offset = kvm->arch.cur_tsc_offset;
		} else {
			u64 delta = nsec_to_cycles(vcpu, elapsed);
			data += delta;
			offset = kvm_compute_l1_tsc_offset(vcpu, data);
		}
		matched = true;
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

### 陷阱2：guest 掉到低价时钟源（PIT/HPET/acpi_pm）

**场景**：Guest 使用 PIT 或 HPET 做 clocksource，性能差

**症状**：`cyclictest` 显示高延迟；`current_clocksource` 显示 `hpet`/`acpi_pm` 等

**⚠️ 注意（与旧版本文档相反）**：现代 host（invariant TSC + vCPU 对齐）上
guest 默认 clocksource **应该是 `tsc`**，不是 `kvm-clock`——kvmclock 在 TSC 可靠时
主动把 rating 从 400 降到 299 让位（`arch/x86/kernel/kvmclock.c:342-345`，
见 §4 对比小节与勘误 22）。所以"不是 kvm-clock 就切到 kvm-clock"是错的，
先判断 tsc 为什么没被选中。

**排查/解决**：
```bash
# Guest中检查
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
cat /sys/devices/system/clocksource/clocksource0/available_clocksource
dmesg | grep -i "tsc\|clocksource"   # tsc 是否被标 unstable、kvm-clock 是否注册

# 仅当 tsc 被标 unstable、且需要固定 kvm-clock 时：
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

**正确做法**（按 6.12.93 基线；**当前宿主 6.8.0-51-generic 接口不同**——
它没有 `lapic_timer_advance` bool 参数，只有运行时可写的 int
`lapic_timer_advance_ns` 模块参数，见上文"宿主版本差异"框）：
```bash
# 开关 timer advance（bool 模块参数，权限 0444 → 运行时只读，
# 只能在模块加载时指定）
modprobe -r kvm_intel kvm && modprobe kvm lapic_timer_advance=0
# 或内核命令行: kvm.lapic_timer_advance=0

# 观察当前自动调整值（每 vCPU 的 debugfs，只读; debugfs.c:67）
cat /sys/kernel/debug/kvm/<pid>-<fd>/vcpu0/lapic_timer_advance_ns
```

---

> 阶段检验见上方「✅ 验证清单」（含操作性条目与深度自测）。
