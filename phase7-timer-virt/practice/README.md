# Phase 7 时钟虚拟化 — 动手实验

> 基于 KVM API 的 C 程序实验，验证时钟虚拟化的核心机制。

---

## 📋 实验列表

| 实验 | 目标 | 对应章节 |
|------|------|---------|
| `experiment1-tsc` | 验证 `guest_TSC = host_TSC × ratio + offset` 公式 | README §3 TSC 虚拟化 |
| `experiment2-kvmclock` | 观察 KVM_GET_CLOCK / KVM_SET_CLOCK 时间跳变 | README §4 kvmclock |
| `experiment3-lapic` | 验证 TSC-deadline 到期 → preemption timer VM-Exit → 中断注入 guest | README §2 APIC Timer |

---

## 🔧 编译和运行

```bash
# 编译所有实验
make

# 运行（需要 root 权限访问 /dev/kvm）
sudo ./experiment1-tsc
sudo ./experiment2-kvmclock
sudo ./experiment3-lapic

# 清理
make clean
```

---

## 📖 实验详解

### Experiment 1: TSC 虚拟化 (`experiment1-tsc.c`)

**目标**：验证 TSC scaling 公式 `guest_TSC = host_TSC × ratio + offset`

**实验步骤**：
1. 创建 VM + vCPU
2. 在 guest 内执行 `RDTSC` 指令（实模式代码）
3. Host 侧用 `host_rdtsc()` 读 host TSC
4. 通过 `KVM_GET_MSRS(MSR_IA32_TSC)` 读 guest TSC
5. 用 `KVM_SET_TSC_KHZ` 改变 TSC 频率
6. 再次执行 `RDTSC`，验证 TSC 增长速率变化

**关键内核代码路径**：
```
KVM_SET_TSC_KHZ → kvm_arch_set_tsc_khz() → vcpu->arch.tsc_scaling_ratio
KVM_GET_MSRS(TSC) → kvm_get_msr_common() → kvm_read_l1_tsc()
vmx_write_tsc_offset()     — vmx.c:1951 — VMCS TSC_OFFSET 写入
vmx_write_tsc_multiplier() — vmx.c:1956 — VMCS TSC_MULTIPLIER 写入
```

**预期输出**：
```
[cap] KVM_CAP_TSC_CONTROL (per-vcpu) = 1
[tsc] Host TSC frequency: 3000000 KHz (3.00 GHz)
[load] 4 bytes loaded at guest PA 0x1000

═══════════════════════════════════════════════════════════════
  Phase 1: Initial TSC Read
═══════════════════════════════════════════════════════════════

  Host TSC (before RDTSC):  1234567890123
  Host TSC (after  RDTSC):  1234567890456
  Host TSC delta:           333 cycles
  Guest TSC (from MSR):     1234567890100
  TSC offset:               -23 (guest - host_before)

═══════════════════════════════════════════════════════════════
  Phase 3: TSC Scaling (KVM_SET_TSC_KHZ)
═══════════════════════════════════════════════════════════════

  Original TSC frequency: 3000000 KHz
  Setting TSC frequency:  1500000 KHz (50% slowdown)
  Verified TSC frequency: 1500000 KHz

  After TSC scaling:
    Host TSC delta:     1000000 cycles
    Guest TSC delta:    500000 cycles
    Ratio:              0.5000 (expected ~0.5)

  ✓ TSC scaling verified: guest TSC grows at 50.0% of host rate
```

---

### Experiment 2: kvmclock 接口 (`experiment2-kvmclock.c`)

**目标**：观察 `KVM_GET_CLOCK` / `KVM_SET_CLOCK` 的时间跳变，模拟迁移场景

**实验步骤**：
1. 创建 VM
2. 调用 `KVM_GET_CLOCK` 读取 VM 时间
3. 等 100ms，再次读取，观察时间增长
4. 调用 `KVM_SET_CLOCK` 设置偏移后的时间（模拟迁移）
5. 再次 `KVM_GET_CLOCK` 验证时间跳变

**关键内核代码路径**：
```
KVM_GET_CLOCK → kvm_vm_ioctl_get_clock() → get_kvmclock()
KVM_SET_CLOCK → kvm_vm_ioctl_set_clock() → kvm_guest_time_update()
```

**预期输出**：
```
[cap] KVM_CAP_ADJUST_CLOCK = 0x9
      → KVM_CLOCK_HOST_TSC supported (host_tsc field valid)

═══════════════════════════════════════════════════════════════
  Phase 1: Initial Clock Read (KVM_GET_CLOCK)
═══════════════════════════════════════════════════════════════

  KVM clock:       1234567890 ns (1.234568 s)
  Host TSC:        3703703670
  Realtime:        1725000000000 ns
  Flags:           0x0

═══════════════════════════════════════════════════════════════
  Phase 3: Simulated Migration (KVM_SET_CLOCK +1s)
═══════════════════════════════════════════════════════════════

  Setting clock forward by 1 second...
  KVM_SET_CLOCK succeeded.

  After KVM_SET_CLOCK:
    KVM clock:     2234567890 ns
    Jump:          1000000000 ns (should be ~1s)

  ✓ Migration simulation: guest clock jumped forward 1s
    In real migration, QEMU does:
      1. KVM_GET_CLOCK on source host
      2. Transfer state to destination
      3. KVM_SET_CLOCK on destination (compensating for downtime)
```

---

### Experiment 3: LAPIC Timer TSC-deadline 中断投递 (`experiment3-lapic.c`)

**目标**：验证 TSC-deadline 到期 → VMX preemption timer VM-Exit → KVM 向
guest 注入定时器中断的完整链路。

**为什么需要一个"真"guest**：deadline 到期后 KVM 要把中断**注入**回 guest，
前提有三条：(1) guest `IF=1`，(2) 有可用的 IDT 入口，(3) **handler 必须发
EOI**（否则 ISR 位不释放，同向量的下一个中断被 PPR 挡住）。只会 `HLT` 的
guest（IF=0、无 IDT）收不到中断，vCPU 一 halt 就死锁在 `kvm_vcpu_block()`。
所以本实验构造了最小实模式 guest：开中断 + 自旋 + 中断处理程序用 `OUT` 端口
退出（再发 EOI），让用户态能"看见"每一次定时器中断。

**实验步骤**：
1. 创建 VM → `KVM_CREATE_IRQCHIP` → 创建 vCPU（顺序不能乱，见下）
2. **`KVM_SET_CPUID2`**：leaf 1 ECX[24] TSC_DEADLINE_TIMER + ECX[21] X2APIC
   （两个都是硬前提，见下面两个坑）
3. **`KVM_SET_MSRS(MSR_IA32_APICBASE) = 0xFEE00D00`** 切 x2APIC 模式
4. `KVM_GET_LAPIC` 读取默认寄存器状态（SPIV / LVTT / TMICT / TDCR）
5. `KVM_SET_LAPIC`：打开 APIC（SPIV bit8），LVTT 设为 TSC-deadline 模式 + 向量
6. 构造实模式 guest：`IVT[0x20] → handler`，主程序 `sti; jmp $`
7. 每轮：读 guest TSC → `KVM_SET_MSRS(MSR_IA32_TSC_DEADLINE) = TSC + 2ms`
   → `KVM_RUN`，统计 `KVM_EXIT_IO`（handler 的 OUT）到达次数与延迟，共 5 轮

**前置条件 ①：必须先建 in-kernel irqchip**：
```
KVM_CREATE_VCPU → kvm_arch_vcpu_postcreate() → kvm_create_lapic()
                  lapic.c:2898 — 函数开头:
                  if (!irqchip_in_kernel(vcpu->kvm)) return 0;   /* lapic.c:2904 */
```
没有 irqchip 就没有 LAPIC，`KVM_GET/SET_LAPIC` 返回 EINVAL。

**前置条件 ②（隐蔽坑）：必须 `KVM_SET_CPUID2` 声明 TSC_DEADLINE_TIMER**：
`kvm_update_cpuid()` 按 guest CPUID leaf 1 是否带
`X86_FEATURE_TSC_DEADLINE_TIMER` 把 `apic->lapic_timer.timer_mode_mask` 设成
`3<<17` 或 `1<<17`（if/else，`cpuid.c:399-402`）。本实验当时**完全没调
`KVM_SET_CPUID2`**，`kvm_find_cpuid_entry(vcpu,1)` 返回 NULL、整个 if 块不执行，
mask 保持 kzalloc 初值 0，`apic_update_lvtt()` 把模式位全掩掉
（`timer_mode = LVTT & timer_mode_mask`，`lapic.c:1781`），`timer_mode` 永远是
0（one-shot）；此时写 `MSR_IA32_TSC_DEADLINE` 被
`kvm_set_lapic_tscdeadline_msr()` 的 `!apic_lvtt_tscdeadline()` 门挡下
（`lapic.c:2585`）—— `KVM_SET_MSRS` 照常返回 1，但定时器不会臂展，`KVM_RUN`
永不返回。QEMU 总会设置 CPUID 所以平时看不到；直接用 KVM API 时它是硬前提。
（6.12 的 `apic_lvtt_tscdeadline()` 查的是缓存的 `timer_mode`，不是 LVTT
寄存器，`lapic.c:554-567`。若 leaf1 存在但缺该位，mask 是 `1<<17` 而非 0，
deadline 位被 `lapic.c:2391` 掩掉，详见 `../corrections.md` 勘误 28。）

**前置条件 ③（隐蔽坑）：handler 不发 EOI，第二个中断永远不来**：
中断注入置位 `ISR[vector]`；不发 EOI 则 `kvm_apic_has_interrupt()` 里
`highest_irr <= apic_get_ppr()` 成立，同向量被挡。实模式够不着 xAPIC MMIO
（`0xFEE00000 > 1MB`），本实验改用 **x2APIC MSR 发 EOI**（`WRMSR(0x80b)`，
实模式可用）。这条路径有两道门：
- `kvm_x2apic_msr_write()` 要求 `apic_x2apic_mode()`，否则 `return 1`
  （`lapic.c:3312-3313`）：本实验走宿主 `KVM_SET_MSRS`，该 1 被
  `kvm_do_msr_access()`（`x86.c:500`）返回用户态、写入被拒（若是 guest
  自己 WRMSR 则会注入 #GP）→ 必须先写 `APICBASE(0x1b)` 置
  `X2APIC_ENABLE`(bit10)；
- `kvm_set_apic_base()` 规定：guest CPUID 没有 X2APIC 时，`APICBASE` 的
  `X2APIC_ENABLE` 算保留位、写入被拒（`x86.c:675-679`）→ 步骤 2 的
  ECX[21] 由此而来。（详见 `../corrections.md` 勘误 28。）

**Guest 内存布局**（实模式，CS.base=0）：
```
0x0080  IVT[0x20] = 0000:0x2000     中断向量表入口 (向量 0x20)
0x1000  主程序:  sti ; jmp $         开中断后自旋等待
0x2000  handler: out 0xe9, al ;      OUT 触发 KVM_EXIT_IO 让用户态计数
                 wrmsr(0x80b) ;      x2APIC EOI — 释放 ISR[0x20]
                 iret
```

**⚠️ handler 的 OUT 端口为什么是 0xe9 而不是 0x20**：
in-kernel PIC (i8259) 把 `0x20-0x21 / 0xa0-0xa1 / 0x4d0-4d1` 注册在
KVM_PIO_BUS 上（`i8259.c:612/617/621`），guest 对这些端口的 IN/OUT 在**内核里
就地消费、不退出到用户态**。handler 若 OUT 0x20，KVM_RUN 永远不会返回，
guest 在里面无限自旋。0xe9 (debug port) 没有内核模拟设备，OUT 必然产生
`KVM_EXIT_IO`。

**关键内核代码路径**：
```
KVM_SET_MSRS(TSC_DEADLINE) → kvm_set_msr_common() x86.c:3766
                             (case MSR_IA32_TSC_DEADLINE @ x86.c:3890)
  → kvm_set_lapic_tscdeadline_msr() lapic.c:2585 — 存 deadline
  → restart_apic_timer() lapic.c:2200            — HW/SW 双路径选择
  → start_hv_timer() lapic.c:2141                — HW preemption timer
  → vmx_set_hv_timer() vmx.c:8129                — 计算 host deadline
  → vmx_update_hv_timer() vmx.c:7205             — 写 VMX_PREEMPTION_TIMER_VALUE
preemption timer 到期 → VM-Exit → KVM 注入向量 0x20 → guest handler OUT 0xe9
handler WRMSR(0x80b) → case APIC_BASE_MSR... x86.c:3888
  → kvm_x2apic_msr_write() lapic.c:3308 → __kvm_apic_update_eoi() 清 ISR
```

**LAPIC Timer 寄存器**（经 `KVM_GET/SET_LAPIC` 访问 `kvm_lapic_state.regs[]`）：
```
0x0F0 APIC_SPIV   — bit8 = APIC 软件使能 (不开则 kvm_apic_present() 判 disabled)
0x320 APIC_LVTT   — bits18:17 模式, bit16 = masked, bits7:0 = vector
  00 = One-shot, 01 = Periodic, 10 = TSC-deadline
0x380 APIC_TMICT  — 初始计数值 (periodic/one-shot 用; deadline 模式不用)
0x390 APIC_TMCCT  — 当前计数值 (只读)
0x3E0 APIC_TDCR   — 除数配置
```

**预期输出**（实测，2.5 GHz host）：
```
[cpuid] leaf 1 set, ECX[24]=TSC_DEADLINE_TIMER ECX[21]=X2APIC
[tsc] virtual TSC = 2499999 KHz

  SPIV  = 0x000000ff  (APIC enabled = no)
  LVTT  = 0x00010000  mode=One-shot masked=1 vector=0x00
  ...
  readback SPIV = 0x000001ff (enabled=yes)  LVTT = 0x00040020  mode=TSC-deadline vector=0x20
  ✓ LAPIC configured: SPIV enabled, LVTT=TSC-deadline, vector=0x20

  deadline delta = 4999998 cycles (~2 ms)

  [diag] deadline readback=0x518672 apic_base=0xfee00d00
  round 1: deadline=5342834   interrupt delivered  latency=2.010 ms
  round 2: deadline=10442336  interrupt delivered  latency=2.003 ms
  round 3: deadline=15464326  interrupt delivered  latency=2.003 ms
  round 4: deadline=20484056  interrupt delivered  latency=2.003 ms
  round 5: deadline=25503606  interrupt delivered  latency=2.003 ms

  ✓ 5/5 timer interrupts delivered (~2 ms apart)
    deadline 到期 → preemption timer VM-Exit → KVM 注入中断。
    每轮只需 1 次 deadline 编程 (本实验经用户态 KVM_SET_MSRS 写入,
    vCPU 未在运行, 不产生 VM-Exit); one-shot 模式则每个周期都要
    由 guest 重写 TMICT (一次 MMIO 写 = 一次 VM-Exit)。
```
`[diag]` 行的 `deadline readback` 非零是 deadline 被接受的最直接证据
（内核读回路径 `kvm_get_lapic_tscdeadline_msr()` 与写入共用同一道门，
被拒时读回 0）；`apic_base=0xfee00d00` 说明 x2APIC 模式已生效。

---

## 🔬 源码对照

| 实验 | KVM API | 内核函数 | 源码位置 |
|------|---------|---------|---------|
| 1 | `KVM_SET_TSC_KHZ` | `kvm_arch_set_tsc_khz()` | `x86.c` |
| 1 | `KVM_GET_MSRS(TSC)` | `kvm_read_l1_tsc()` | `x86.c:2580` |
| 1 | `vmx_write_tsc_offset()` | — | `vmx/vmx.c:1951` |
| 1 | `vmx_write_tsc_multiplier()` | — | `vmx/vmx.c:1956` |
| 2 | `KVM_GET_CLOCK` | `kvm_vm_ioctl_get_clock()` | `x86.c:6995` |
| 2 | `KVM_SET_CLOCK` | `kvm_vm_ioctl_set_clock()` | `x86.c:7006` |
| 2 | `kvm_guest_time_update()` | — | `x86.c:3215` |
| 3 | `KVM_CREATE_IRQCHIP` | `kvm_arch_vm_ioctl()` case @ `x86.c:7090` | `x86.c` |
| 3 | `KVM_SET_CPUID2` | `kvm_update_cpuid()` — 决定 `timer_mode_mask` | `cpuid.c:399-402` |
| 3 | `KVM_SET_MSRS(APICBASE)` | `kvm_set_apic_base()` — 无 X2APIC CPUID 时拒 `X2APIC_ENABLE` | `x86.c:671` / `:675-679` |
| 3 | `KVM_SET_LAPIC` | `kvm_vcpu_ioctl_set_lapic()` → `kvm_apic_set_state()` | `x86.c:5122` / `lapic.c:3103` |
| 3 | `KVM_SET_MSRS(TSC_DEADLINE)` | `kvm_set_msr_common()` → `kvm_set_lapic_tscdeadline_msr()` | `x86.c:3890` / `lapic.c:2585` |
| 3 | handler `WRMSR(0x80b)` EOI | `kvm_x2apic_msr_write()` → `__kvm_apic_update_eoi()` | `x86.c:3888` / `lapic.c:3308` |
| 3 | `restart_apic_timer()` | — | `lapic.c:2200` |
| 3 | `start_hv_timer()` | — | `lapic.c:2141` |
| 3 | `vmx_set_hv_timer()` | — | `vmx/vmx.c:8129` |
| 3 | `vmx_update_hv_timer()` | — | `vmx/vmx.c:7205` |

---

## ⚠️ 注意事项

1. **需要 root 权限**：所有实验需要 `sudo` 运行以访问 `/dev/kvm`
2. **需要 Intel VT-x**：TSC scaling 需要 CPU 支持 `TSC_CONTROL` 能力
3. **KVM 模块加载**：确保 `kvm` 和 `kvm_intel` 模块已加载
4. **实验环境**：建议在物理机上运行，嵌套虚拟化可能影响 TSC 行为

```bash
# 检查 KVM 模块
lsmod | grep kvm

# 检查 TSC 相关特性标志
grep -oE "constant_tsc|nonstop_tsc|tsc_deadline_timer|tsc_known_freq|tsc_adjust" \
    /proc/cpuinfo | sort -u
# 注意: flag 名是 tsc_deadline_timer (cpufeatures.h:137)，没有 tsc_deadline
```

---

## 📚 参考资料

- Phase 7 主文档: `../README.md`
- 源码注释: `../annotations.md`
- 勘误: `../corrections.md`
- Linux KVM API: `/root/code/linux-6.12.93/Documentation/virt/kvm/api.rst`
- Intel VMX 规范: `../../intel-vmx.pdf`
