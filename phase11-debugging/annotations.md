# Phase 10：源码精读注释 - KVM 调试与追踪

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。
> 参数默认值与权限的唯一来源在 `../phase10-performance/parameters.md`，本章不重复。

---

## 1. 入口与退出事件

### Q: `kvm_entry` 携带哪些字段？

**文件**: `arch/x86/kvm/trace.h:17` — `TRACE_EVENT(kvm_entry, ...)`

```c
/* 来源: arch/x86/kvm/trace.h:17-36 */
TRACE_EVENT(kvm_entry,
    TP_PROTO(struct kvm_vcpu *vcpu, bool force_immediate_exit),
    TP_ARGS(vcpu, force_immediate_exit),

    TP_STRUCT__entry(
        __field( unsigned int,  vcpu_id         )
        __field( unsigned long, rip             )
        __field( bool,          immediate_exit  )
    ),

    TP_printk("vcpu %u, rip 0x%lx%s", __entry->vcpu_id, __entry->rip,
              __entry->immediate_exit ? "[immediate exit]" : "")
);
```

| 字段 | 类型 | 含义 |
|------|------|------|
| `vcpu_id` | `unsigned int` | vCPU 编号 |
| `rip` | `unsigned long` | Guest RIP（进入点）|
| `immediate_exit` | `bool` | 是否立即退出（QEMU 用于停止 vCPU）|

### Q: `kvm_exit` 携带哪些字段？有没有 `exit_reason_full`？

**文件**: `arch/x86/kvm/trace.h:297` — `TRACE_EVENT_KVM_EXIT` 宏

```c
/* 来源: arch/x86/kvm/trace.h:297-331 */
#define TRACE_EVENT_KVM_EXIT(name)
TRACE_EVENT(name,
    TP_PROTO(struct kvm_vcpu *vcpu, u32 isa),
    TP_ARGS(vcpu, isa),

    TP_STRUCT__entry(
        __field( unsigned int,  exit_reason )      /* ★ 数字，不是字符串 */
        __field( unsigned long, guest_rip   )
        __field( u32,           isa         )
        __field( u64,           info1       )
        __field( u64,           info2       )
        __field( u32,           intr_info   )
        __field( u32,           error_code  )
        __field( unsigned int,  vcpu_id     )
    ),
```

**★ 全树 grep `exit_reason_full` 零命中** —— 这个字段**不存在**。

### Q: trace 文本里的 `reason` 是数字还是符号名？

**文件**: `arch/x86/kvm/trace.h:289` — `kvm_print_exit_reason()`

```c
/* 来源: arch/x86/kvm/trace.h:289-296 */
#define kvm_print_exit_reason(exit_reason, isa)                    \
    (isa == KVM_ISA_VMX) ?                                        \
    __print_symbolic(exit_reason & 0xffff, VMX_EXIT_REASONS) :    \
    __print_symbolic(exit_reason, SVM_EXIT_REASONS),              \
    (isa == KVM_ISA_VMX && exit_reason & ~0xffff) ? " " : "",     \
    (isa == KVM_ISA_VMX) ?                                        \
    __print_flags(exit_reason & ~0xffff, " ", VMX_EXIT_REASON_FLAGS) : ""
```

**关键区分**：

| 视角 | `exit_reason` 形态 | 怎么取 |
|------|-------------------|--------|
| **trace 文本** | 符号名（如 `reason EPT_VIOLATION`）| `TP_printk` 调 `kvm_print_exit_reason()` 实时翻译 |
| **BPF / bpftrace** | 数字（`args->exit_reason`）| 没有符号翻译，需自行映射到 `vmx.h:32-95` |

所以想按原因聚合 trace 文本要抓**符号名**，想拿数字走 **BPF**。
按 `reason=[0-9]` 去 grep trace 文本永远抓不到东西。

### Q: `kvm_nested_vmexit` 的字段与 `kvm_exit` 一样吗？

**文件**: `arch/x86/kvm/trace.h:679`

```c
/* 来源: arch/x86/kvm/trace.h:679 */
TRACE_EVENT_KVM_EXIT(kvm_nested_vmexit);
```

**完全相同** —— 同一个宏展开，字段列表一字不差。没有额外的 `l1_rsp` 字段，
也没有 `exit_reason_full`。

### Q: 常用退出号（VMX）对照表？

**文件**: `arch/x86/include/uapi/asm/vmx.h:32-95`

| 退出号 | 名字 | 典型处理 |
|--------|------|---------|
| 1 | `EXTERNAL_INTERRUPT` | 内核处理宿主中断 |
| 3 | `TRIPLE_FAULT` | 返回用户空间 |
| 10 | `CPUID` | 内核/用户空间 |
| 18 | `VMCALL` | 超调用 |
| 28 | `MSR_WRITE` | 内核模拟 |
| 30 | `IO_INSTRUCTION` | 返回用户空间 |
| 40 | `PAUSE_INSTRUCTION` | PLE 路径 |
| 48 | `EPT_VIOLATION` | 内核建页表 |
| 49 | `EPT_MISCONFIG` | 内核处理 |
| 52 | `PREEMPTION_TIMER` | 内核更新定时器 |
| 62 | `PML_FULL` | 内核 flush PML buffer |

---

## 2. 中断相关事件

### Q: 中断路径上有哪些 tracepoint？

| 事件 | 文件:行 | 参数 | 用途 |
|------|---------|------|------|
| `kvm_inj_virq` | `trace.h:341` | `vcpu_id, irq` | 虚拟中断注入 |
| `kvm_inj_exception` | `trace.h:372` | `vcpu_id, exception, has_error_code, error_code, payload` | 异常注入（triple fault 诊断）|
| `kvm_apic_accept_irq` | `trace.h:542` | `vcpu_id, vector, level, trig_mode` | vLAPIC 接收中断 |
| `kvm_apicv_accept_irq` | `trace.h` | `vcpu_id, delivery_mode, trig_mode, vector` | APICv 中断接受 |
| `kvm_pi_irte_update` | `trace.h` | `host_irq, vcpu_id, gsi, vector, pi_desc_addr, set` | PI IRTE 更新 |
| `kvm_pv_eoi` | `trace.h` | `vcpu_id, pir` | PV EOI |
| `kvm_ack_irq` | `include/trace/events/kvm.h` | `irq_source_id, gsi` | 中断确认 |
| `kvm_set_irq` | `include/trace/events/kvm.h` | `irq_source_id, gsi, level` | 中断设置 |
| `kvm_pic_set_irq` | `trace.h:484` | `chip, pin, level` | PIC 中断设置 |
| `kvm_ioapic_set_irq` | `include/trace/events/kvm.h` | `pin, level, remote_irr` | IOAPIC 中断设置 |

---

## 3. 内存相关事件

### Q: `kvm_page_fault` 携带哪些字段？有没有 `level`？

**文件**: `arch/x86/kvm/trace.h:402` — `TRACE_EVENT(kvm_page_fault, ...)`

```c
/* 来源: arch/x86/kvm/trace.h:402-423 */
TRACE_EVENT(kvm_page_fault,
    TP_PROTO(struct kvm_vcpu *vcpu, u64 fault_address, u64 error_code),
    TP_ARGS(vcpu, fault_address, error_code),

    TP_STRUCT__entry(
        __field( unsigned int,  vcpu_id         )
        __field( unsigned long, guest_rip       )
        __field( u64,           fault_address   )
        __field( u64,           error_code      )
    ),

    TP_printk("vcpu %u rip 0x%lx address 0x%016llx error_code 0x%llx",
              __entry->vcpu_id, __entry->guest_rip,
              __entry->fault_address, __entry->error_code)
);
```

**★ 没有 `level` 字段** —— 不能从 trace 文本直接读映射级别。
确认实际映射级别要走 `/proc/<qemu-pid>/smaps` 的 `AnonHugePages`，
或看 `kvmmmu:kvm_mmu_set_spte`（注意不在 `kvm:` system 下，要写 `kvmmmu:`）。

| 事件 | 文件:行 | 参数 | 用途 |
|------|---------|------|------|
| `kvm_page_fault` | `trace.h:402` | `vcpu_id, guest_rip, fault_address, error_code` | ★ EPT Violation |
| `kvm_mmio` | `include/trace/events/kvm.h` | `vcpu_id, len, gpa, write, data` | MMIO 操作 |
| `kvm_pio` | `trace.h:161` | `rw, port, size, count, rip` | PIO 操作 |
| `kvm_unmap_hva_range` | `include/trace/events/kvm.h` | `mmu_notifier, start, end` | mmu_notifier unmap |
| `kvm_age_hva` | `include/trace/events/kvm.h` | `mmu_notifier, start, end` | mmu_notifier aging |

---

## 4. 时钟相关事件

### Q: `kvm_update_master_clock` 的字段含义是什么？

**文件**: `arch/x86/kvm/trace.h:906`

```c
/* 来源: arch/x86/kvm/trace.h:906-926 */
TRACE_EVENT(kvm_update_master_clock,
    TP_PROTO(bool use_master_clock, unsigned int host_clock, bool offset_matched),

    TP_printk("masterclock %d hostclock %s offsetmatched %u",
              __entry->use_master_clock,
              __print_symbolic(__entry->host_clock, host_clocks),
              __entry->offset_matched)
);
```

| 字段 | 类型 | 含义 |
|------|------|------|
| `use_master_clock` | `bool` | **本次重算的新决定**（开/关）|
| `host_clock` | `unsigned int` | 宿主 clock 模式（`none`/`tsc`）|
| `offset_matched` | `bool` | vCPU TSC 是否全部匹配 |

### Q: `kvm_track_tsc` 的 `nr_vcpus_matched_tsc` 含基准 vCPU 吗？

**文件**: `arch/x86/kvm/trace.h:928`

```c
/* 来源: arch/x86/kvm/trace.h:928-960 */
TRACE_EVENT(kvm_track_tsc,
    TP_PROTO(unsigned int vcpu_id, unsigned int nr_matched,
             unsigned int online_vcpus, bool use_master_clock,
             unsigned int host_clock),

    TP_STRUCT__entry(
        __field( unsigned int,  vcpu_id                 )
        __field( unsigned int,  nr_vcpus_matched_tsc    )   /* ★ 不含基准 vCPU */
        __field( unsigned int,  online_vcpus            )
        __field( bool,          use_master_clock        )   /* ★ 翻转前的旧值 */
        __field( unsigned int,  host_clock              )
    ),
```

**★ 与 `kvm_update_master_clock` 的语义差异**：

| 字段 | `kvm_update_master_clock` | `kvm_track_tsc` |
|------|--------------------------|-----------------|
| `use_master_clock` / `masterclock` | **本次重算的新决定** | **翻转前的旧值** |
| `offset_matched` / `nr_vcpus_matched_tsc` | 布尔 | **计数，不含基准 vCPU** |

正确读法：`nr_vcpus_matched_tsc + 1 == online_vcpus`（源码注释 `:2521-2528`）。

| 事件 | 文件:行 | 参数 | 用途 |
|------|---------|------|------|
| `kvm_track_tsc` | `trace.h:928` | `vcpu_id, nr_vcpus_matched_tsc, online_vcpus, use_master_clock, host_clock` | TSC 同步跟踪 |
| `kvm_write_tsc_offset` | `trace.h:879` | `vcpu_id, offset` | TSC offset 写入 |
| `kvm_update_master_clock` | `trace.h:906` | `use_master_clock, host_clock, offset_matched` | 主时钟更新 |
| `kvm_pvclock_update` | `trace.h:999` | `vcpu_id, system_time, tsc_timestamp, ...` | pvclock 更新 |
| `kvm_hv_timer_state` | `trace.h` | `vcpu_id, hv_timer_in_use` | hypervisor timer 状态 |
| `kvm_wait_lapic_expire` | `trace.h` | `vcpu_id, guest_tsc, tsc_deadline, dy_nsec` | LAPIC 定时器到期等待 |

---

## 5. 性能调优事件

### Q: `kvm_halt_poll_ns` 的参数含义？

**文件**: `include/trace/events/kvm.h:347`

```c
/* 来源: include/trace/events/kvm.h:347-377 */
TRACE_EVENT(kvm_halt_poll_ns,
    TP_PROTO(bool grow, unsigned int vcpu_id, unsigned int new,
             unsigned int old),

    TP_printk("vcpu %u: halt_poll_ns %u (%s %u)",
              __entry->vcpu_id,
              __entry->new,
              __entry->grow ? "grow" : "shrink",
              __entry->old)
);
```

| 字段 | 含义 |
|------|------|
| `grow` | `true`=增长，`false`=收缩 |
| `vcpu_id` | vCPU 编号 |
| `new` | 新窗口值（ns）|
| `old` | 旧窗口值（ns）|

### Q: `kvm_pml_full` 有几个参数？

**文件**: `arch/x86/kvm/trace.h:963`

```c
/* 来源: arch/x86/kvm/trace.h:963-977 */
TRACE_EVENT(kvm_pml_full,
    TP_PROTO(unsigned int vcpu_id),              /* ★ 只有这一个 */
    TP_ARGS(vcpu_id),

    TP_STRUCT__entry(
        __field( unsigned int,  vcpu_id   )
    ),

    TP_printk("vcpu %d: PML full", __entry->vcpu_id)
);
```

**★ 没有 `full_count`** —— 旧版写的"vcpu_id, full_count"不存在。

### Q: `kvm_vcpu_wakeup` 有没有 `vcpu_id`？

**文件**: `include/trace/events/kvm.h:43`

```c
/* 来源: include/trace/events/kvm.h:43-64 */
TRACE_EVENT(kvm_vcpu_wakeup,
    TP_PROTO(__u64 ns, bool waited, bool valid),     /* ★ 没有 vcpu_id */
    TP_ARGS(ns, waited, valid),

    TP_STRUCT__entry(
        __field( __u64,  ns      )
        __field( bool,   waited  )
        __field( bool,   valid   )
    ),

    TP_printk("%s time %lld ns, polling %s",
              __entry->waited ? "wait" : "poll",
              __entry->ns,
              __entry->valid ? "valid" : "invalid")
);
```

**★ 没有 `vcpu_id`，也没有 `runnable`/`blocking` 字段**。
区分睡没睡过的是 `waited` 字段：`true`=真正阻塞过，`false`=轮询唤醒。

| 事件 | 参数 | 说明 |
|------|------|------|
| `kvm_halt_poll_ns` | `grow, vcpu_id, new, old` | halt-polling 窗口自适应 |
| `kvm_ple_window_update` | `vcpu_id, new, old` | PLE 窗口变化 |
| `kvm_pml_full` | `vcpu_id`（只有这一个）| PML buffer 满 |
| `kvm_vcpu_wakeup` | `ns, waited, valid`（无 vcpu_id）| vCPU 唤醒 |

---

## 6. tracefs 全局状态：`>` 与 `>>` 不等价

### Q: 为什么 `echo evt > set_event` 会停掉别人挂的所有事件？

**文件**: `kernel/trace/trace_events.c:2411` — `ftrace_event_set_open()`

以写方式打开 `set_event` 时只要带 `O_TRUNC`（`echo x > file` 与不带 `-a` 的 `tee`
都是），内核会先 `ftrace_clear_events()`（`:883`）把**全部**已启用事件清掉，
然后才处理本次写入。

**三个文件都是全局状态**：

| 文件 | `>` 的效果 | `>>` 的效果 |
|------|-----------|------------|
| `set_event` | 清掉**全部**已启用事件，再装本次写入的 | 追加一个事件 |
| `set_ftrace_filter` | 把 filter **换成**只有本次写入的名字 | 追加一个函数 |
| `set_event_pid` | 清掉**全部**已有 PID | 追加一个 PID |

**规则**：
- 清场用显式 `: > set_event`（意图落在纸面上）
- 追加一律 `>>`
- `tracing_on=0` **不等于零开销**（只停 ring buffer 记录，static key 仍是开的）

---

## 7. CPU 操作事件

### Q: MSR / CPUID / CR 的 tracepoint 长什么样？

| 事件 | 文件:行 | 参数 |
|------|---------|------|
| `kvm_cpuid` | `trace.h:214` | `vcpu_id, function, index, rax, rbx, rcx, rdx` |
| `kvm_msr` | `trace.h:428` | `vcpu_id, write (bool), ecx, data` |
| `kvm_cr` | `trace.h:460` | `vcpu_id, write (bool), cr, val` |
| `kvm_emulate_insn` | `trace.h` | `vcpu_id, failed, insn_bytes, rip` |

**实用片段 — 按 MSR 号统计退出频率**：

```bash
cat $TRACEFS/trace | grep kvm_msr | \
    grep -oP 'msr=0x[0-9a-f]+' | sort | uniq -c | sort -rn | head
# 读/写方向: 分别统计 write=0 / write=1
```

---

## 8. perf kvm stat

### Q: `perf kvm stat` 必须 `-a` 吗？

**文件**: `tools/perf/builtin-kvm.c:1959-1960`

```c
if (target__none(&kvm->opts.target))
    … system_wide = true;
```

**只有不给 target 时才自动 system-wide**。用 `-p $PID` 只跟踪被包裹进程，
**vCPU 线程退出全丢**（vCPU 线程是 QEMU 主线程 fork 出来的独立线程，不在 `-p` 范围）。

正确用法：

```bash
# ★ 必须 -a system-wide
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report
```

### Q: 常用分析模式？

```bash
# 按 VM-Exit 延迟排序
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report --sort=reason

# 分析特定 vCPU（用 TID 而非 PID）
sudo perf kvm stat record -t $VCPU_TID -- sleep 10

# 与 CPU profile 结合
sudo perf kvm stat record -g -a -- sleep 10
sudo perf kvm stat report --stdio
```

---

## 9. debugfs 接口

### Q: KVM debugfs 目录结构？

```
/sys/kernel/debug/kvm/
├── <pid>-<vmid>/              ← 每个 VM 一个目录
│   ├── vcpu<n>/               ← 每个 vCPU 一个子目录
│   │   ├── pid                ← vCPU 线程 PID
│   │   ├── guest_mode         ← ★ 是否在 guest 模式 (0/1)
│   │   ├── tsc-offset         ← ★ 当前 TSC 偏移
│   │   └── lapic_timer_advance_ns  ← LAPIC 定时器提前量（6.12.93 只读）
│   │
│   ├── halt_attempted_poll    ← ★ 尝试 poll 次数（每项一个文件）
│   ├── halt_successful_poll   ← poll 内命中次数
│   ├── directed_yield_attempted  ← PV spinlock hypercall 路径（不是 PLE）
│   └── directed_yield_successful
│
└── （全局统计，每项一个文件）
```

**★ `directed_yield_*` 不属于 PLE 路径** —— 它们只由 guest hypercall
（`KVM_HC_KICK_CPU` / `KVM_HC_SCHED_YIELD`）递增，PLE 路径的
`kvm_vcpu_on_spin()` 一次都不加。没开 PV spinlock 的 guest 上这两个计数器恒为 0。

### Q: `stats` 是一个聚合文件还是每项一个文件？

**文件**: `virt/kvm/kvm_main.c:6352` — `kvm_stat_per_vm()`

```c
/* 来源: virt/kvm/kvm_main.c:6352,6363 */
debugfs_create_file(pdesc->name, …)    /* ★ 逐项注册 */
```

**每个统计项一个文本文件**，不存在叫 `stats` 的聚合文件。
`cat .../stats` 是无效命令。

### Q: `lapic_timer_advance_ns` 是模块参数吗？

**不是**。6.12.93 的真实形态：

| 项 | 形态 |
|----|------|
| 模块参数 | `lapic_timer_advance`（`bool`，`arch/x86/kvm/lapic.c:70-71`，0444）|
| 当前提前量 | per-vCPU **debugfs 只读文件** `lapic_timer_advance_ns`（`arch/x86/kvm/debugfs.c:67`）|
| 手动设固定值 | **做不到** |

> ★ 宿主若跑 6.8，形态正好相反（有 `lapic_timer_advance_ns` 参数、无 bool）。
> 先 `ls` 再写文档。

---

## 10. bpftrace 脚本集

### Q: 怎么测量 VM-Exit 处理延迟？

```bash
#!/usr/bin/bpftrace
# trace-vmexit-latency.bt

kprobe:vmx_handle_exit
{
    @start[tid] = nsecs;
}

kretprobe:vmx_handle_exit
{
    if (@start[tid]) {
        @latency_us = hist((nsecs - @start[tid]) / 1000);
        delete(@start[tid]);
    }
}

interval:s:5
{
    print(@latency_us);
    clear(@latency_us);
}
```

### Q: 怎么统计 EPT 缺页热点？

```bash
#!/usr/bin/bpftrace
# trace-ept-hotspot.bt

tracepoint:kvm:kvm_page_fault
{
    @gpa[args->fault_address >> 21] = count();  /* 按 2MB 对齐分组 */
}

interval:s:10
{
    printf("=== EPT 热点 (2MB 粒度) ===\n");
    print(@gpa, 20);
    clear(@gpa);
}
```

### Q: 怎么按退出原因统计？

```bash
# ★ 只能聚合**数字** args->exit_reason
# BPF 侧拿不到符号名，要名字照 vmx.h:32-95 自己映射
sudo bpftrace -e '
tracepoint:kvm:kvm_exit { @reasons[args->exit_reason] = count(); }
interval:s:5 { print(@reasons, 10); clear(@reasons); }
'
```

### Q: 怎么分析 vCPU 调度？

```bash
#!/usr/bin/bpftrace
# trace-vcpu-schedule.bt

tracepoint:kvm:kvm_entry
{
    @in_guest[tid] = nsecs;
}

tracepoint:kvm:kvm_exit
{
    if (@in_guest[tid]) {
        @guest_time_us = hist((nsecs - @in_guest[tid]) / 1000);
        delete(@in_guest[tid]);
    }
}

tracepoint:sched:sched_switch
{
    if (@in_guest[args->prev_pid]) {
        @preempted_us = hist((nsecs - @in_guest[args->prev_pid]) / 1000);
    }
}

interval:s:10
{
    printf("=== Guest 执行时间分布 ===\n");
    print(@guest_time_us);
    printf("=== 被抢占时间分布 ===\n");
    print(@preempted_us);
    clear(@guest_time_us);
    clear(@preempted_us);
}
```

### Q: 怎么检测中断风暴？

```bash
#!/usr/bin/bpftrace
# trace-irq-storm.bt

tracepoint:kvm:kvm_exit
{
    @total = count();
    if (args->exit_reason == 1) {  // 1 = EXTERNAL_INTERRUPT
        @ext_int = count();
    }
}

interval:s:5
{
    if (@total > 0) {
        $pct = 100.0 * @ext_int / @total;
        printf("EXTERNAL_INTERRUPT: %d (%.1f%%)\n", @ext_int, $pct);
        if ($pct > 40) {
            printf("WARNING: 中断退出比例过高，考虑启用 APICv/PI\n");
        }
    }
    clear(@total);
    clear(@ext_int);
}
```

---

## 11. 嵌套虚拟化事件

### Q: 嵌套相关的 tracepoint？

| 事件 | 位置 | 参数 |
|------|------|------|
| `kvm_nested_vmenter` | `trace.h` | `rip, vmcs, nested_vmcs, l2_rip, l2_rsp` |
| `kvm_nested_vmexit` | `trace.h:679` | 与 `kvm_exit` **完全相同** |
| `kvm_nested_vmenter_failed` | `trace.h` | `rip, error` |
| `kvm_nested_intercepts` | `trace.h` | `cr_read, cr_write, exceptions, intercept` |
| `kvm_nested_vmexit_inject` | `trace.h` | `vcpu_id, exit_reason` |
| `kvm_nested_intr_vmexit` | `trace.h` | `vcpu_id, vector` |

---

## 12. 完整调用链

```
tracefs 事件流:

  Guest 执行
    │
    ├→ VM-Entry
    │   └→ trace_kvm_entry(vcpu_id, rip, immediate_exit)
    │
    ├→ Guest 模式执行 → 某个时刻触发 VM-Exit
    │
    └→ VM-Exit 处理
        ├→ trace_kvm_exit(exit_reason, guest_rip, isa, info1, info2,
        │                 intr_info, error_code, vcpu_id)
        │   └→ TP_printk: kvm_print_exit_reason() 翻译成符号名
        │
        ├→ handle_exit() 分发
        │   ├→ EPT_VIOLATION → kvm_mmu_page_fault()
        │   │   └→ trace_kvm_page_fault(vcpu_id, guest_rip, fault_address, error_code)
        │   │
        │   ├→ PAUSE_INSTRUCTION → handle_pause()
        │   │   └→ kvm_vcpu_on_spin()
        │   │       └→ trace_kvm_ple_window_update(vcpu_id, new, old)
        │   │
        │   └→ EXTERNAL_INTERRUPT → handle_external_interrupt()
        │
        └→ 重新进入 Guest / 返回用户空间
            └→ trace_kvm_userspace_exit(vcpu_id, exit_reason, ret)


halt-polling 自适应:

  vCPU HLT → halt 处理
    │
    ├→ Phase 1: 忙等（halt_poll_ns 窗口内）
    │   ├→ 命中 → trace_kvm_halt_poll_ns(grow=false, vcpu_id, new, old)
    │   └→ 未命中 → 进入 Phase 2
    │
    ├→ Phase 2: 真正阻塞
    │   └→ kvm_vcpu_block()
    │       └→ 被中断唤醒
    │
    └→ Phase 3: 自适应调整
        ├→ 窗口内唤醒 → grow
        │   └→ trace_kvm_halt_poll_ns(grow=true, vcpu_id, new, old)
        └→ 远超窗口 → shrink
            └→ trace_kvm_halt_poll_ns(grow=false, vcpu_id, new, old)
```
