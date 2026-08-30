# Phase 9：源码精读注释 - KVM 性能优化深入

> 基于 Linux 6.12.93 源码（实际代码行号已验证）

---

## 1. halt-polling 自适应算法

**文件**: `virt/kvm/kvm_main.c:3670-3706` (grow/shrink), `virt/kvm/kvm_main.c:3811-3882` (kvm_vcpu_halt)

### 1.1 核心参数

```c
/* 来源: virt/kvm/kvm_main.c:78-97 */

/* 全局上限: 每个vCPU的halt_poll_ns不会超过此值 */
unsigned int halt_poll_ns = KVM_HALT_POLL_NS_DEFAULT;  /* 默认400000ns = 400μs */
module_param(halt_poll_ns, uint, 0644);

/* 增长因子: 每次增长乘以该值 (默认加倍) */
unsigned int halt_poll_ns_grow = 2;
module_param(halt_poll_ns_grow, uint, 0644);

/* 增长起始值: 从0开始增长时，首先跳到该值 */
unsigned int halt_poll_ns_grow_start = 10000;  /* 10μs */
module_param(halt_poll_ns_grow_start, uint, 0644);

/* 缩小因子: 每次缩小除以该值 (默认减半) */
unsigned int halt_poll_ns_shrink = 2;
module_param(halt_poll_ns_shrink, uint, 0644);
```

### 1.2 grow_halt_poll_ns()

```c
/* 来源: virt/kvm/kvm_main.c:3670-3687 */

static void grow_halt_poll_ns(struct kvm_vcpu *vcpu)
{
	unsigned int old, val, grow, grow_start;

	old = val = vcpu->halt_poll_ns;
	grow_start = READ_ONCE(halt_poll_ns_grow_start);
	grow = READ_ONCE(halt_poll_ns_grow);
	if (!grow)
		goto out;

	val *= grow;                   /* ★ 翻倍 (默认) */
	if (val < grow_start)
		val = grow_start;          /* ★ 至少达到grow_start */

	vcpu->halt_poll_ns = val;
out:
	trace_kvm_halt_poll_ns_grow(vcpu->vcpu_id, val, old);
}
```

### 1.3 shrink_halt_poll_ns()

```c
/* 来源: virt/kvm/kvm_main.c:3689-3706 */

static void shrink_halt_poll_ns(struct kvm_vcpu *vcpu)
{
	unsigned int old, val, shrink, grow_start;

	old = val = vcpu->halt_poll_ns;
	shrink = READ_ONCE(halt_poll_ns_shrink);
	grow_start = READ_ONCE(halt_poll_ns_grow_start);
	if (shrink == 0)
		val = 0;                   /* shrink=0 → 直接归零 */
	else
		val /= shrink;             /* ★ 减半 (默认) */

	if (val < grow_start)
		val = 0;                   /* ★ 低于起始值 → 直接归零 */

	vcpu->halt_poll_ns = val;
	trace_kvm_halt_poll_ns_shrink(vcpu->vcpu_id, val, old);
}
```

### 1.4 自适应流程

```
kvm_vcpu_halt() 中的自适应逻辑:

  ┌─ Phase 1: halt-polling (忙等 vcpu->halt_poll_ns) ─────────┐
  │  唤醒? → goto out                                         │
  │  超时? → 进入 Phase 2                                      │
  └────────────────────────────────────────────────────────────┘
                         │
                         ▼
  ┌─ Phase 2: kvm_vcpu_block() (真正阻塞, schedule()) ────────┐
  │  唤醒? → 返回                                              │
  └────────────────────────────────────────────────────────────┘
                         │
                         ▼
  ┌─ Phase 3: 自适应调整 ────────────────────────────────────┐
  │                                                           │
  │  if (!vcpu_valid_wakeup(vcpu)):                           │
  │    → shrink (无效唤醒, polling 浪费时间)                   │
  │                                                           │
  │  else if (halt_ns <= vcpu->halt_poll_ns):                 │
  │    → 不变 (在poll窗口内唤醒, 正好)                        │
  │                                                           │
  │  else if (halt_ns > halt_poll_ns):                        │
  │    → shrink (阻塞时间太长, polling 没覆盖到)              │
  │                                                           │
  │  else (halt_ns < halt_poll_ns 且 > 当前值):               │
  │    → grow (阻塞时间短, polling 应该能覆盖)                │
  │                                                           │
  └───────────────────────────────────────────────────────────┘

典型收敛过程 (中断间隔 50μs):
  halt_poll_ns: 10μs → 20μs → 40μs → 80μs → 40μs → 80μs ...
  (先快速增长，然后在目标值附近震荡)
```

### 1.5 真实 trace event

```
kvm:kvm_halt_poll_ns (include/trace/events/kvm.h)
  参数:
    grow: bool (true=grow, false=shrink)
    vcpu_id: unsigned int
    new: unsigned int (新的 halt_poll_ns)
    old: unsigned int (旧的 halt_poll_ns)

跟踪命令:
  echo kvm:kvm_halt_poll_ns > /sys/kernel/debug/tracing/set_event
```

### 1.6 MicroVM 调优建议

```
场景 1: 网络密集 (低延迟需求)
  halt_poll_ns = 800000     # 增大上限到 800μs
  halt_poll_ns_grow = 2     # 快速翻倍增长
  效果: 中断延迟降低 ~50%，CPU 占用增加 ~10-20%

场景 2: 高密度部署 (CPU 利用率优先)
  halt_poll_ns = 0          # 完全禁用 halt-polling
  效果: CPU 利用率提升 ~5-10%，中断延迟增加 ~200μs

场景 3: 平衡 (推荐默认)
  halt_poll_ns = 400000     # 默认值
  适合大多数混合工作负载

场景 4: 实时负载
  halt_poll_ns = 200000     # 较小上限，减少 jitter
  halt_poll_ns_grow_start = 5000
```

---

## 2. VM-Exit 减少技术汇总

### 2.1 VPID (Virtual Processor ID)

**目的**: 避免 VM-Entry 时刷新整个 TLB

```
无 VPID:
  每次 VM-Entry 执行 INVVPID(all) 刷新 TLB
  开销: ~数百 ns 每次 VM-Entry

有 VPID:
  VMCS 中为每个 vCPU 分配唯一 VPID (1-65535)
  Guest TLB 条目标记 VPID
  VM-Entry 时只刷新 VPID=0 的条目
  不同 vCPU 的 TLB 条目共存，无需刷新

实现:
  vmx_vcpu_run() 中:
    vmcs_write16(VIRTUAL_PROCESSOR_ID, vmx->vpid02)

模块参数:
  vpid=1 (默认启用)
```

### 2.2 APICv (Virtual APIC)

**目的**: Guest 访问 LAPIC 寄存器不触发 VM-Exit

```
无 APICv:
  Guest 写 TPR → VM-Exit → KVM 处理 → VM-Entry
  开销: ~500ns 每次 LAPIC 访问

有 APICv:
  VMCS 配置 Virtual APIC Page (物理页给硬件使用)
  Guest 写 TPR/EOI → 硬件直接更新 Virtual APIC Page
  Virtual Interrupt Delivery: 硬件自动评估 IRR 并注入中断

VMCS 控制位:
  - VIRTUALIZE_APIC_ACCESSES (Secondary Exec Control)
  - VIRTUALIZE_INTR_DELIVERY (Secondary Exec Control)
  - VIRTUALIZE_EOI_EXIT (Secondary Exec Control)
```

### 2.3 Posted Interrupts (零 VM-Exit 中断投递)

```
传统中断路径:
  设备 MSI → Host 内核 → KVM kvm_set_irq() → vLAPIC IRR → VM-Exit → 注入
  至少 1 次 VM-Exit

Posted Interrupts 路径:
  设备 MSI → IOMMU (IRTE IM=1) → PI 描述符 PIR[vec]=1, ON=1
  → 通知中断到 pCPU
  → KVM sync_pir_to_irr() → 写入 RVI
  → 硬件 VID 自动注入 (如果 Guest 在运行)
  零 VM-Exit!

关键代码:
  vmx_sync_pir_to_irr() (vmx.c:6912)
  vmx_deliver_posted_interrupt() (vmx.c:4269)
```

### 2.4 PLE (Pause Loop Exiting) — 自旋锁优化

**文件**: `arch/x86/kvm/vmx/vmx.c:5911-5924`

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:5911-5924 */

/*
 * handle_pause - 处理 PAUSE 指令引起的 VM-Exit
 *
 * PLE 机制:
 *   当 Guest 在短时间内执行多次 PAUSE (自旋锁忙等)，
 *   硬件触发 PAUSE-loop exit。KVM 检测到这个模式，
 *   认为 vCPU 在等待锁，将其让出给持锁的 vCPU。
 */
static int handle_pause(struct kvm_vcpu *vcpu)
{
	if (!kvm_pause_in_guest(vcpu->kvm))
		grow_ple_window(vcpu);    /* 扩大PLE窗口，减少后续PAUSE exit */

	/* ★ 关键: 让出CPU给其他可能持锁的vCPU */
	kvm_vcpu_on_spin(vcpu, true);
	return kvm_skip_emulated_instruction(vcpu);
}
```

**PLE 参数**:
```
ple_gap    [uint]  两次 PAUSE 之间的时间上限 (默认 128)
ple_window [uint]  PAUSE 循环时间窗口 (默认 4096)

当 PAUSE 间隔 < ple_gap 且持续时间 > ple_window 时:
  → 触发 PAUSE-loop exit
  → grow_ple_window() 增大窗口 (减少后续触发)
  → kvm_vcpu_on_spin() 让出 CPU

当 PAUSE 间隔 > ple_gap 时 (非自旋):
  → shrink_ple_window() 缩小窗口
```

**真实 trace event**:
```
kvm:kvm_ple_window_update (trace.h)
  参数: vcpu_id, new_window, old_window

跟踪命令:
  echo kvm:kvm_ple_window_update > /sys/kernel/debug/tracing/set_event
```

### 2.5 VM-Exit 减少效果对比

```
工作负载              无优化 VM-Exit/s    全优化 VM-Exit/s    减少比例
──────────────────   ─────────────────   ─────────────────   ────────
网络密集 (10Gbps)    ~500K               ~50K               ~90%
CPU 密集 (计算)      ~100K               ~20K               ~80%
内存密集             ~1M (EPT)           ~100K              ~90%
锁竞争密集           ~2M (PAUSE)         ~50K               ~97%
混合 (典型)          ~500K               ~100K              ~80%
```

---

## 3. EPT 性能优化

### 3.1 大页映射 (2MB / 1GB)

```
4K 页:  4 级 EPT (PML4 → PDPT → PD → PT) → 每次缺页创建 1 个叶条目
2MB 页: 3 级 EPT (PML4 → PDPT → PD[2MB叶]) → 1 个条目覆盖 512 个 4K 页
1GB 页: 2 级 EPT (PML4 → PDPT[1GB叶]) → 1 个条目覆盖 262144 个 4K 页

性能影响:
  - EPT 遍历: 4 级 → 2-3 级 (TLB miss 开销减少)
  - 缺页次数: N → N/512 (2MB) 或 N/262144 (1GB)
  - TLB 容量: 有效 TLB 条目数量增加

启用条件:
  - Host 内存: THP (transparent hugepage) 或 hugetlbfs
  - Guest 物理地址对齐 (2MB/1GB 边界)
  - EPT A/D 位支持
```

### 3.2 Access/Dirty 位 (EPT A/D)

```
无 A/D 位:
  KVM 使用软件位模拟 Accessed/Dirty
  写保护: 每次写入都触发 VM-Exit，KVM 更新软件位
  开销: ~500ns 每次写操作

有 A/D 位 (enable_ept_ad=1):
  硬件自动设置 EPT 条目的 A/D 位
  无需写保护，写入不触发 VM-Exit
  仅在 TLB flush 时批量同步
  开销: ~0ns (硬件自动)

启用条件:
  cpu_has_vmx_ept_ad_bits() == true

模块参数:
  eptad=1 (默认启用)
```

### 3.3 PML (Page Modification Logging)

```
目的: 高效脏页跟踪 (用于热迁移)

无 PML:
  每次写入触发 EPT Violation → KVM 记录脏页 → 重新映射
  开销: ~500ns 每次写入 (脏页日志开启后)

有 PML:
  硬件记录修改过的 GPA 到 PML buffer (512 个条目)
  PML buffer 满时触发 VM-Exit → KVM 批量处理
  正常写入不触发 VM-Exit
  开销: ~0ns 每次写入 (直到 PML buffer 满)

PML 处理:
  handle_pml_full() → KVM 消费 PML buffer → 清空 → 继续

模块参数:
  pml=1 (默认启用)

真实 trace event:
  kvm:kvm_pml_full (trace.h)
  参数: vcpu_id, full_count
```

---

## 4. vCPU 调度与迁移

### 4.1 抢占通知 (Preemption Notifiers)

```c
/* 来源: virt/kvm/kvm_main.c (preempt_notifier 注册) */

/*
 * KVM 在 vCPU 加载到 pCPU 时注册抢占通知:
 *
 * preempt_notifier_register(&vcpu->preempt_notifier)
 *
 * 当 vCPU 线程被抢占时:
 *   sched_out_fn() → vmx_vcpu_pi_put() → 设置 PI SN 位
 *
 * 当 vCPU 线程被调度回来时:
 *   sched_in_fn() → vmx_vcpu_pi_load() → 清除 PI SN 位 + 更新 NDST
 */
```

### 4.2 kvm_vcpu_on_spin() — 自旋锁检测

**文件**: `virt/kvm/kvm_main.c:4037-4099`

```c
/* 来源: virt/kvm/kvm_main.c:4037-4099 (简化) */

/*
 * kvm_vcpu_on_spin - 处理 vCPU 的 PAUSE/自旋
 *
 * 当 vCPU 执行 PAUSE 指令触发 PLE exit 时调用
 * 目的: 找到持锁的 vCPU，让出 CPU 给它
 *
 * 算法:
 *   1. 在当前 VM 的其他 vCPU 中寻找
 *   2. 优先选择:
 *      - 在 kernel 模式运行的 vCPU (更可能持锁)
 *      - 在等待队列上的 vCPU
 *   3. 找到后调用 yield_to() 让出 CPU
 */
void kvm_vcpu_on_spin(struct kvm_vcpu *me, bool yield_to_kernel_mode)
{
	/* 遍历当前 VM 的所有 vCPU */
	/* 寻找合适的 yield 目标 */
	/* 调用 yield_to(target_vcpu_task) */
}
```

### 4.3 vCPU 调度迁移

```
vCPU 从 pCPU-A 迁移到 pCPU-B 时的关键操作:

  1. vcpu_put() on pCPU-A:
     ├── vmx_vcpu_pi_put()  → PI SN=1, 注册 wakeup handler
     ├── vmx_load_vmcs()    → 清空 (无 active VMCS)
     └── 解除 preempt notifier

  2. vcpu_load() on pCPU-B:
     ├── vmx_vcpu_pi_load() → 更新 PI NDST=B, 清除 SN
     ├── vmx_load_vmcs()    → 加载 VMCS 到 pCPU-B
     ├── 更新 HOST_CR3/CR4
     ├── 注册 preempt notifier
     └── 如果 TSC 频率不同 → 更新 TSC_OFFSET

关键 trace events:
  kvm:kvm_wait_lapic_expire — LAPIC 定时器等待
  kvm:kvm_track_tsc — TSC 同步跟踪
```

---

## 5. TSC 同步优化

### 5.1 主时钟 (Master Clock)

```
目的: 为所有 vCPU 提供一致的时间视图

机制:
  - KVM 维护一个 "master clock" (基于 Host TSC)
  - 所有 vCPU 的 kvmclock 使用同一个 master clock 作为基准
  - 通过 pvclock_vcpu_time_info 共享给 Guest
  - Guest 读时间 = RDTSC + offset (零 VM-Exit!)

启用条件:
  - 所有 vCPU 的 TSC 频率相同
  - Host TSC 是 invariant (CPUID.80000007:EDX[8])
  - kvm->arch.use_master_clock = true

真实 trace event:
  kvm:kvm_update_master_clock
  kvm:kvm_track_tsc
  kvm:kvm_pvclock_update (x86 特定)
```

### 5.2 TSC Offset 同步

```
每个 vCPU 在 VMCS 中都有独立的 TSC_OFFSET:
  Guest RDTSC = Host TSC + TSC_OFFSET

当 vCPU 迁移到不同 pCPU 时:
  if (新 pCPU 的 TSC 频率不同):
    重新计算 TSC_OFFSET
    vmcs_write64(TSC_OFFSET, new_offset)

如果支持 TSC scaling:
  Guest RDTSC = (Host TSC × TSC_MULTIPLIER) + TSC_OFFSET
  可以补偿不同频率的 pCPU

真实 trace event:
  kvm:kvm_write_tsc_offset
```

### 5.3 kvmclock vs TSC-deadline

```
┌─ kvmclock (kvm_pv_clock) ───────────────────────────────────┐
│  共享内存: pvclock_vcpu_time_info                            │
│  Guest 读时间: 纯内存读 + 算术 (零 VM-Exit)                  │
│  精度: 取决于 TSC 稳定性                                     │
│  同步: 需要主时钟同步                                        │
│  适用: 通用时间获取                                          │
└──────────────────────────────────────────────────────────────┘

┌─ TSC-deadline (APIC Timer 模式) ───────────────────────────┐
│  机制: 硬件比较 RDTSC vs IA32_TSC_DEADLINE MSR              │
│  Guest 设置: 写 MSR (一次 VM-Exit)                          │
│  到期: 硬件自动触发中断 (零 VM-Exit)                        │
│  精度: 最高 (硬件比较)                                      │
│  适用: tickless 内核 (NO_HZ), 高精度定时器                  │
└──────────────────────────────────────────────────────────────┘

MicroVM 推荐:
  使用 TSC-deadline 模式 + kvmclock
  最小化定时器相关的 VM-Exit
```

---

## 6. 性能调试命令集

```bash
# === halt-polling 监控 ===
echo kvm:kvm_halt_poll_ns > /sys/kernel/debug/tracing/set_event
cat /sys/kernel/debug/tracing/trace | grep halt_poll | tail -20

# === PLE 窗口变化 ===
echo kvm:kvm_ple_window_update > /sys/kernel/debug/tracing/set_event

# === PML buffer 满事件 ===
echo kvm:kvm_pml_full > /sys/kernel/debug/tracing/set_event

# === TSC 同步跟踪 ===
echo kvm:kvm_track_tsc > /sys/kernel/debug/tracing/set_event
echo kvm:kvm_write_tsc_offset >> /sys/kernel/debug/tracing/set_event

# === VM-Exit 统计 ===
sudo perf kvm stat record -p $QEMU_PID -- sleep 10
sudo perf kvm stat report

# === 模块参数查看 ===
cat /sys/module/kvm_intel/parameters/halt_poll_ns
cat /sys/module/kvm_intel/parameters/ple_window
cat /sys/module/kvm_intel/parameters/ept

# === KVM 统计接口 ===
cat /sys/kernel/debug/kvm/*
```
