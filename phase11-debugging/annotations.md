# Phase 11：源码精读注释 - KVM 调试与追踪

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
| 2 | `TRIPLE_FAULT` | 返回用户空间 |
| 10 | `CPUID` | 内核/用户空间 |
| 18 | `VMCALL` | 超调用 |
| 28 | `CR_ACCESS` | 内核模拟（`vmx.h:57`）|
| 30 | `IO_INSTRUCTION` | 返回用户空间 |
| 32 | `MSR_WRITE` | 内核模拟 |
| 40 | `PAUSE_INSTRUCTION` | PLE 路径 |
| 48 | `EPT_VIOLATION` | 内核建页表 |
| 49 | `EPT_MISCONFIG` | 内核处理 |
| 52 | `PREEMPTION_TIMER` | 内核更新定时器 |
| 62 | `PML_FULL` | 内核 flush PML buffer |

---

## 2. 中断相关事件

### Q: 中断路径上有哪些 tracepoint？

> ★ 下表字段名同时对照 `arch/x86/kvm/trace.h` / `include/trace/events/kvm.h` 的
> `TP_STRUCT__entry` 与宿主 `/sys/kernel/debug/tracing/events/kvm/<evt>/format` 核实。
> **这一组中断事件里只有 `kvm_pi_irte_update` 带 `vcpu_id`**（`kvm_set_irq` 是
> `gsi/level/irq_source_id`，同样没有 vCPU 字段），
> 其余事件没有 `vcpu_id` 字段（APIC 类事件用 `apicid`，注入类事件干脆不带 vCPU）。
> 要按 vCPU 归因，只能借 tracepoint 公共字段 `common_pid`（= 触发事件的线程 TID）。

| 事件 | 文件:行 | 字段（`args->…` 可用名） | 用途 |
|------|---------|--------------------------|------|
| `kvm_inj_virq` | `trace.h:341` | `vector, soft, reinjected` | 虚拟中断注入 |
| `kvm_inj_exception` | `trace.h:372` | `exception, has_error, error_code, reinjected` | 异常注入（triple fault 诊断）|
| `kvm_apic_accept_irq` | `trace.h:542` | `apicid, dm, tm, vec` | vLAPIC 接收中断 |
| `kvm_apicv_accept_irq` | `trace.h:1405` | `apicid, dm, tm, vec` | APICv 中断接受 |
| `kvm_pi_irte_update` | `trace.h:1080` | `host_irq, vcpu_id, gsi, gvec, pi_desc_addr, set` | PI IRTE 更新 |
| `kvm_pv_eoi` | `trace.h:583` | `apicid, vector` | PV EOI |
| `kvm_ack_irq` | `include/trace/events/kvm.h:183` | `irqchip, pin` | 中断确认 |
| `kvm_set_irq` | `include/trace/events/kvm.h:66` | `gsi, level, irq_source_id` | 中断设置 |
| `kvm_pic_set_irq` | `trace.h:484` | `chip, pin, elcr, imr, coalesced` | PIC 中断设置 |
| `kvm_ioapic_set_irq` | `include/trace/events/kvm.h:98` | `e, pin, coalesced` | IOAPIC 中断设置 |

字段名容易踩的三个坑（源码为准）：

- **`level` / `trig_mode` 不是字段**。`kvm_apic_accept_irq` 的 `TP_printk` 把
  `tm` 翻译成 `(level|edge)` 文本、把 `dm` 查 `kvm_deliver_mode` 表
  （`trace.h:560-563`）；`kvm_pic_set_irq` 的 `level/edge` 文本由
  `elcr & (1<<pin)` 现场算出（`trace.h` 同段），都不是可读字段。
- **`kvm_pi_irte_update` 的向量字段叫 `gvec`，不叫 `vector`**（`trace.h:1080`），
  写 `args->vector` 的 bpftrace 脚本会直接报错。
- **`kvm_ioapic_set_irq` 把 RTE 原值打包进 `e`**（`kvm.h:98`），
  `remote_irr`/触发模式要从 `e` 的位域自己解，没有独立字段。

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

| 事件 | 文件:行 | 字段 | 用途 |
|------|---------|------|------|
| `kvm_page_fault` | `trace.h:402` | `vcpu_id, guest_rip, fault_address, error_code` | ★ EPT Violation |
| `kvm_mmio` | `include/trace/events/kvm.h:213` | `type, len, gpa, val` | MMIO 操作 |
| `kvm_pio` | `trace.h:161` | `rw, port, size, count, val` | PIO 操作 |
| `kvm_unmap_hva_range` | `include/trace/events/kvm.h:441` | `start, end` | mmu_notifier unmap |
| `kvm_age_hva` | `include/trace/events/kvm.h:459` | `start, end` | mmu_notifier aging |

三个必须按源码写的细节：

- **`kvm_mmio` 没有 `write`/`data` 字段**：方向编码在 `type` 里
  （`KVM_TRACE_MMIO_READ_UNSATISFIED=0 / READ=1 / WRITE=2`，`kvm.h:204-206`），
  数据在 `val`（`__u64`），不是 `data`；也**没有 `vcpu_id`**。
- **`kvm_pio` 最后一个字段是 `val`，不是 `rip`**（`trace.h:166-172`）。
  `TP_printk` 是 `pio_%s at 0x%x size %d count %d val 0x%x %s`，整行不含 RIP。
- **`mmu_notifier` 不是字段**，只是 `TP_printk` 文本前缀
  （`mmu notifier unmap range: %#016lx -- %#016lx`）；字段只有 `start, end`。

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

| 事件 | 文件:行 | 字段 | 用途 |
|------|---------|------|------|
| `kvm_track_tsc` | `trace.h:928` | `vcpu_id, nr_vcpus_matched_tsc, online_vcpus, use_master_clock, host_clock` | TSC 同步跟踪 |
| `kvm_write_tsc_offset` | `trace.h:879` | `vcpu_id, previous_tsc_offset, next_tsc_offset` | TSC offset 写入 |
| `kvm_update_master_clock` | `trace.h:906` | `use_master_clock, host_clock, offset_matched` | 主时钟更新 |
| `kvm_pvclock_update` | `trace.h:999` | `vcpu_id, version, tsc_timestamp, system_time, tsc_to_system_mul, tsc_shift, flags` | pvclock 更新 |
| `kvm_hv_timer_state` | `trace.h:1542` | `vcpu_id, hv_timer_in_use` | hypervisor timer 状态 |
| `kvm_wait_lapic_expire` | `trace.h:1035` | `vcpu_id, delta` | LAPIC 定时器到期等待 |

★ 两处旧文档写错的字段名（源码 + 宿主 format 双重核对）：

- **`kvm_write_tsc_offset` 没有 `offset` 字段**，是
  `previous_tsc_offset` / `next_tsc_offset` 一对（`trace.h:884-888`），
  `TP_printk` 为 `vcpu=%u prev=%llu next=%llu`。要算变化量得自己相减。
- **`kvm_wait_lapic_expire` 只有 `vcpu_id` 和 `delta`**（`trace.h:1035-1042`），
  没有 `guest_tsc` / `tsc_deadline` / `dy_nsec`。`delta` 是带符号的
  "提前/滞后"纳秒数，`TP_printk` 用 `delta < 0 ? "early" : "late"` 标注。

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

| 事件 | 文件:行 | 字段 |
|------|---------|------|
| `kvm_cpuid` | `trace.h:214` | `function, index, rax, rbx, rcx, rdx, found, used_max_basic` |
| `kvm_msr` | `trace.h:428` | `write (unsigned), ecx, data, exception` |
| `kvm_cr` | `trace.h:460` | `rw (unsigned), cr, val` |
| `kvm_emulate_insn` | `trace.h:817` | `rip, csbase, len, insn[15], flags, failed` |

★ 这一组事件**全都没有 `vcpu_id` 字段**（`trace.h:220-229`、`:432-437`、
`:464-468`、`:817` 的 `TP_STRUCT__entry`，宿主 `format` 同样确认）。
`kvm_cr` 的方向字段叫 **`rw`** 不叫 `write`；`kvm_msr` 的 `write` 类型是
`unsigned` 不是 `bool`，且多一个 `exception`（`#GP` 标志）。

**实用片段 — 按 MSR 号统计频率**（照 `TP_printk` 的实际文本，不是 `msr=0x…`）：

```bash
# trace 文本形如: kvm_msr: msr_write 48 = 0x63   /   msr_read c0000100 = 0x…
cat $TRACEFS/trace | grep 'kvm_msr:' | \
    grep -oP 'msr_(read|write) \K[0-9a-f]+' | sort | uniq -c | sort -rn | head

# 读/写方向: 文本里没有 write=0/1，只有 msr_read / msr_write 两个词
cat $TRACEFS/trace | grep -o 'kvm_msr: msr_\(read\|write\)' | sort | uniq -c
```

> 按 `msr=0x` 或 `write=[01]` 去 grep 会永远抓不到东西，和 `reason=[0-9]` 是同一类坑。

---

## 8. perf kvm stat

### Q: `perf kvm stat` 必须 `-a` 吗？

**文件**: `tools/perf/builtin-kvm.c:1959-1960`

```c
if (target__none(&kvm->opts.target))
    … system_wide = true;
```

**只有不给 target 时才自动 system-wide**。`perf kvm stat` 的默认视图是全局汇总，
不加 `-a` 时容易把"没采到"当成"没有退出"，所以本章一律写 `-a`。

> ★ 注意成因（旧文档这里写反了）：vCPU 线程**是 QEMU 进程内的线程**
> （`qemu_thread_create()`，同一 TGID），不是 fork 出的独立进程；
> 而 `perf -p PID` 恰恰会枚举 `/proc/<pid>/task` 把整个线程组都放进 thread map
> （`tools/perf/util/thread_map.c:34-58`，`thread_map__new_by_pid()`）。
> 所以"`-p` 会把 vCPU 线程全丢"这个说法站不住，`-a` 的理由是
> **避免 target/汇总口径问题**，不是"线程不在范围内"。

正确用法：

```bash
# ★ 推荐 -a system-wide
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report
```

### Q: 常用分析模式？

```bash
# 按 VM-Exit 平均延迟排序（选项名是 --key，取值 sample|time；没有 --sort）
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report --key=time --stdio

# 只看某个 vCPU（report 端用 --vcpu，record 端用 -t TID）
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report --vcpu 0 --stdio

# 与 CPU profile 结合：perf kvm stat record 显式禁用了 -g/--call-graph
# （tools/perf/builtin-kvm.c:1766-1767，PARSE_OPT_DISABLED → "is not usable"），
# 要调用栈就用普通 perf record：
sudo perf record -g -a -e kvm:kvm_exit -- sleep 30
sudo perf report --stdio
```

`perf kvm stat report` 的全部选项（`tools/perf/builtin-kvm.c:1786-1800`）：
`--event`、`--vcpu`、`-k/--key {sample|time}`、`-p/--pid`、`-f/--force`、`--stdio`。
**没有 `--sort`**，写成 `--sort=reason` 会被 parse-options 拒掉。

---

## 9. debugfs 接口

### Q: KVM debugfs 目录结构？

```
/sys/kernel/debug/kvm/
├── <pid>-<vm fd>/             ← 每个 VM 一个目录（实测 771290-8）
│   │                             snprintf("%d-%s", task_pid_nr(current), fdname)
│   │                             virt/kvm/kvm_main.c:1060；fdname 是 VM 的 fd 号
│   │                             （kvm_main.c:5499 取 fd、:5501 写进 fdname）
│   ├── vcpu<n>/               ← 目录名 "vcpu%d"（kvm_main.c:4203）
│   │   ├── pid                ← vCPU 线程 PID，0444（kvm_main.c:4207）
│   │   ├── guest_mode         ← ★ 是否在 guest 模式 (0/1)，0444（debugfs.c:61）
│   │   ├── tsc-offset         ← ★ 当前 TSC 偏移，0444（debugfs.c:63）
│   │   └── lapic_timer_advance_ns  ← LAPIC 定时器提前量（6.12.93 只读，debugfs.c:67）
│   │
│   ├── halt_attempted_poll    ← ★ 尝试 poll 次数（每项一个文件）
│   ├── halt_successful_poll   ← poll 内命中次数
│   ├── directed_yield_attempted  ← PV spinlock hypercall 路径（不是 PLE）
│   └── directed_yield_successful
│
└── （全局统计，同样每项一个文件）
```

> 目录名的第二段**不是 vmid**，是 `KVM_CREATE_VM` 返回的那个 fd 序号
> （`kvm_main.c:5499-5501`）；宿主实测目录 `771290-8` 即 "QEMU PID - VM fd 8"。

**★ `directed_yield_*` 不属于 PLE 路径** —— 它们只由 guest hypercall
（`KVM_HC_KICK_CPU` / `KVM_HC_SCHED_YIELD`）递增，PLE 路径的
`kvm_vcpu_on_spin()` 一次都不加。没开 PV spinlock 的 guest 上这两个计数器恒为 0。

### Q: `stats` 是一个聚合文件还是每项一个文件？

**逐项注册的两处**（6.12.93）：

```c
/* 来源: virt/kvm/kvm_main.c:1092,1107 — kvm_create_vm_debugfs()（per-VM 目录）*/
debugfs_create_file(pdesc->name, kvm_stats_debugfs_mode(pdesc),
                    kvm->debugfs_dentry, stat_data, &stat_fops_per_vm);

/* 来源: virt/kvm/kvm_main.c:6352,6363 — kvm_init_debug()（/sys/kernel/debug/kvm 全局）*/
debugfs_create_file(pdesc->name, …)
```

**每个统计项一个文本文件**，6.12.93 的 debugfs 里**不存在名为 `stats` 的文件**
（`grep '"stats"' virt/kvm/kvm_main.c` 零命中；宿主
`/sys/kernel/debug/kvm/<pid>-<fd>/` 实测也只有 `exits`、`halt_successful_poll`
等逐项文件）。`cat .../stats` 是无效命令。

> 二进制 blob 走的是另一条路：`KVM_GET_STATS_FD` ioctl
> （`include/uapi/linux/kvm.h:1541`）返回一个名为 `kvm-vm-stats` 的 anon inode fd
> （`kvm_main.c:5121-5139`），那是**匿名 fd，不是 debugfs 路径**。
> 旧文档/索引里"`/sys/kernel/debug/kvm/.../stats` 是二进制文件"的说法按源码应删掉。
> ★ 注意：全树没有 `kvm_stat_per_vm()` 这个函数，旧引用里的函数名是错的。

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

| 事件 | 位置 | 字段（trace.h + 宿主 format 双核） |
|------|------|------------------------------------|
| `kvm_nested_vmenter` | `trace.h:603` | `rip, vmcb, nested_rip, int_ctl, event_inj, tdp_enabled, guest_pgd, isa` |
| `kvm_nested_vmexit` | `trace.h:679` | 与 `kvm_exit` **完全相同**（同宏 `TRACE_EVENT_KVM_EXIT`）|
| `kvm_nested_vmenter_failed` | `trace.h:1677` | `msg`（`__string`）`, err` |
| `kvm_nested_intercepts` | `trace.h:647` | `cr_read, cr_write, exceptions, intercept1, intercept2, intercept3` |
| `kvm_nested_vmexit_inject` | `trace.h:684` | `exit_code, exit_info1, exit_info2, exit_int_info, exit_int_info_err, isa` |
| `kvm_nested_intr_vmexit` | `trace.h:719` | `rip`（**只有这一个字段**）|

★ 旧表里这几行字段名是错的，必须按源码：

- `kvm_nested_vmenter` **没有 `vmcs` / `nested_vmcs` / `l2_rip` / `l2_rsp`**。
  字段名就叫 `vmcb`（VMX/SVM 共用，`TP_printk` 按 `isa` 印成
  "vmcs:" 或 "vmcb:"，`trace.h:632-644`），L2 入口是 `nested_rip`，
  **不存在任何 rsp 字段**（与 C11 对 `kvm_nested_vmexit` 的结论一致）。
- `kvm_nested_vmexit_inject` 没有 `vcpu_id`，退出原因字段叫 **`exit_code`** 不是 `exit_reason`。
- `kvm_nested_intr_vmexit` 只有一个 `rip`，没有 `vcpu_id`、没有 `vector`。
- `kvm_nested_vmenter_failed` 是 `msg` + `err`（`err` 查
  `VMX_VMENTER_INSTRUCTION_ERRORS` 表），没有 `rip`。

---

## 12. 完整调用链

```
tracefs 事件流:

  Guest 执行
    │
    ├→ VM-Entry
    │   └→ trace_kvm_entry(vcpu, force_immediate_exit)      ← vmx.c:7372
    │       （字段 vcpu_id / rip / immediate_exit，trace.h:21-25）
    │
    ├→ Guest 模式执行 → 某个时刻触发 VM-Exit
    │
    └→ VM-Exit 处理
        ├→ trace_kvm_exit(vcpu, KVM_ISA_VMX)                ← vmx.c:7489
        │   （字段 exit_reason / guest_rip / isa / info1 / info2 /
        │     intr_info / error_code / vcpu_id，trace.h:302-311）
        │   └→ TP_printk: kvm_print_exit_reason() 翻译成符号名
        │
        ├→ handle_exit() 分发
        │   ├→ EPT_VIOLATION → kvm_mmu_page_fault()
        │   │   └→ trace_kvm_page_fault(vcpu, fault_address, error_code)
        │   │       （vmx.c:5799、mmu.c:4655）
        │   │
        │   ├→ PAUSE_INSTRUCTION → handle_pause()           ← vmx.c:5911
        │   │   ├→ grow_ple_window()                        ← vmx.c:5914
        │   │   │   └→ trace_kvm_ple_window_update(vcpu_id, new, old)
        │   │   │       ★ 事件在 grow/shrink_ple_window() 里发（vmx.c:1428/1444），
        │   │   │         不在 kvm_vcpu_on_spin() 里——后者一个统计/trace 都不加
        │   │   └→ kvm_vcpu_on_spin()                       ← vmx.c:5922
        │   │
        │   └→ EXTERNAL_INTERRUPT → handle_external_interrupt()
        │
        └→ 重新进入 Guest / 返回用户空间
            └→ trace_kvm_userspace_exit(vcpu->run->exit_reason, r)
                ★ 只有两个实参：字段是 `reason` + `errno`，**没有 vcpu_id**
                （include/trace/events/kvm.h:22-41；调用点 virt/kvm/kvm_main.c:4495）


halt-polling 自适应:   （对照 virt/kvm/kvm_main.c:3811-3882 kvm_vcpu_halt()）

  vCPU HLT → kvm_vcpu_halt()
    │
    ├→ Phase 1: 忙等（vcpu->halt_poll_ns 窗口内轮询）
    │   ├→ 命中 → goto out
    │   │   ★ **不发任何 kvm_halt_poll_ns 事件**：调整分支里
    │   │     `if (halt_ns <= vcpu->halt_poll_ns) ;` 是空操作
    │   │     （kvm_main.c:3865-3866）；只有 halt_attempted_poll /
    │   │     halt_successful_poll 统计 +1（kvm_main.c:3771-3774）
    │   └→ 未命中 → 进入 Phase 2
    │
    ├→ Phase 2: 真正阻塞
    │   └→ kvm_vcpu_block()（kvm_main.c:3733）→ 被唤醒
    │
    └→ Phase 3: 自适应调整（trace_kvm_halt_poll_ns 只在这里发，
        调用点 kvm_main.c:3686 grow / :3705 shrink）
        ├→ 窗口外但仍 < max_halt_poll_ns → grow_halt_poll_ns()
        │   └→ trace_kvm_halt_poll_ns(grow=true, vcpu_id, new, old)
        ├→ halt_ns > max_halt_poll_ns（睡太久）→ shrink_halt_poll_ns()
        │   └→ trace_kvm_halt_poll_ns(grow=false, vcpu_id, new, old)
        └→ 非法唤醒（!vcpu_valid_wakeup）→ 也走 shrink
            └→ trace_kvm_halt_poll_ns(grow=false, vcpu_id, new, old)

    出口处无条件发一次 trace_kvm_vcpu_wakeup(halt_ns, waited, valid)
        （kvm_main.c:3880；字段 ns / waited / valid，**无 vcpu_id**，见 C9）
```

> ★ 旧图把 grow 写成"窗口内唤醒"、把 shrink 写成"远超窗口"且给 Phase 1 也画了一条
> `grow=false` 事件，都不对：**窗口内命中时这个 tracepoint 根本不触发**，
> grow 的语义是"这次睡得不长、下次可以多 poll 一点"（`halt_ns < max_halt_poll_ns`）。
