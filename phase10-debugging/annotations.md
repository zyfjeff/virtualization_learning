# Phase 10：源码精读注释 - KVM 调试与测试

> 基于 Linux 6.12.93 源码 | 所有 trace events 均已验证存在

---

## 1. 完整 KVM trace events 目录

### 1.1 入口/出口事件

```
kvm:kvm_entry
  来源: arch/x86/kvm/trace.h:17
  参数: vcpu_id, rip, immediate_exit
  用途: 跟踪每次 VM-Entry
  命令: echo kvm:kvm_entry >> /sys/kernel/debug/tracing/set_event

kvm:kvm_exit
  来源: arch/x86/kvm/trace.h:336 (TRACE_EVENT_KVM_EXIT，宏体在 :297-331)
  参数: exit_reason (数字), guest_rip, isa, info1, info2, intr_info,
        error_code, vcpu_id —— 字段定义 :303-310
  ★ 没有 exit_reason_full 这个字段（全树 grep 不到）；trace **文本**里打的是
    符号名：TP_printk 是 `reason %s`（:325-330），字符串由 kvm_print_exit_reason()
    先 `exit_reason & 0xffff` 查 VMX_EXIT_REASONS、再把高位标志按
    VMX_EXIT_REASON_FLAGS 附上（:289-295；arch/x86/include/uapi/asm/vmx.h:96-158、
    :160-161）。所以想按原因聚合 trace 文本要抓符号名，想拿数字走 BPF 的
    args->exit_reason。
  用途: 跟踪每次 VM-Exit 及原因
  命令: echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event

kvm:kvm_userspace_exit
  来源: include/trace/events/kvm.h:22
  参数: vcpu_id, exit_reason, ret
  用途: 跟踪返回用户空间的事件
```

> ★ **上面以及本文所有 `set_event` 命令都用 `>>`**：以写方式打开 `set_event` 只要带
> `O_TRUNC`（`echo x > file` 与不带 `-a` 的 `tee` 都是），内核会先
> `ftrace_clear_events()` 把**全部**已启用事件清掉再处理本次写入
> （`kernel/trace/trace_events.c:2411` → `:2422-2423`，函数定义 `:883`）。
> 需要清场就显式写 `: > set_event`。规则的唯一来源：
> `../phase9-performance/measurement.md` §5 第 3 条。

### 1.2 中断相关事件

```
kvm:kvm_inj_virq
  来源: arch/x86/kvm/trace.h:341
  参数: vcpu_id, irq
  用途: 跟踪虚拟中断注入

kvm:kvm_inj_exception
  来源: arch/x86/kvm/trace.h:372
  参数: vcpu_id, exception, has_error_code, error_code, payload
  用途: 跟踪异常注入

kvm:kvm_ack_irq
  来源: include/trace/events/kvm.h
  参数: irq_source_id, gsi
  用途: 跟踪中断确认

kvm:kvm_set_irq
  来源: include/trace/events/kvm.h
  参数: irq_source_id, gsi, level
  用途: 跟踪中断设置

kvm:kvm_apic_accept_irq
  来源: arch/x86/kvm/trace.h:542
  参数: vcpu_id, vector, level, trig_mode
  用途: 跟踪 vLAPIC 接收中断

kvm:kvm_apicv_accept_irq
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, delivery_mode, trig_mode, vector
  用途: 跟踪 APICv 中断接受

kvm:kvm_pi_irte_update
  来源: arch/x86/kvm/trace.h
  参数: host_irq, vcpu_id, gsi, vector, pi_desc_addr, set
  用途: 跟踪 PI IRTE 更新

kvm:kvm_pv_eoi
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, pir
  用途: 跟踪 PV EOI

kvm:kvm_pic_set_irq
  来源: arch/x86/kvm/trace.h:484
  参数: chip, pin, level
  用途: 跟踪 PIC 中断设置

kvm:kvm_ioapic_set_irq
  来源: include/trace/events/kvm.h
  参数: pin, level, remote_irr
  用途: 跟踪 IOAPIC 中断设置
```

### 1.3 内存相关事件

```
kvm:kvm_page_fault
  来源: arch/x86/kvm/trace.h:402
  参数: vcpu_id, guest_rip, fault_address, error_code（字段定义 :406-411）
  用途: ★ 最重要的内存事件，跟踪每次 EPT Violation
  命令: echo kvm:kvm_page_fault >> /sys/kernel/debug/tracing/set_event

kvm:kvm_mmio
  来源: include/trace/events/kvm.h
  参数: vcpu_id, len, gpa, write, data
  用途: 跟踪 MMIO 操作

kvm:kvm_pio
  来源: arch/x86/kvm/trace.h:161
  参数: rw, port, size, count, rip
  用途: 跟踪 PIO 操作

kvm:kvm_unmap_hva_range
  来源: include/trace/events/kvm.h
  参数: mmu_notifier, start, end
  用途: 跟踪 mmu_notifier unmap

kvm:kvm_age_hva
  来源: include/trace/events/kvm.h
  参数: mmu_notifier, start, end
  用途: 跟踪 mmu_notifier aging
```

### 1.4 时钟相关事件

```
kvm:kvm_track_tsc
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, nr_vcpus_matched_tsc, online_vcpus, use_master_clock, host_clock
  用途: 跟踪 TSC 同步

kvm:kvm_write_tsc_offset
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, offset
  用途: 跟踪 TSC offset 写入

kvm:kvm_update_master_clock
  来源: arch/x86/kvm/trace.h
  参数: kvm, use_master_clock, host_clock
  用途: 跟踪主时钟更新

kvm:kvm_pvclock_update
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, system_time, tsc_timestamp, ...
  用途: 跟踪 pvclock 更新

kvm:kvm_hv_timer_state
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, hv_timer_in_use
  用途: 跟踪 hypervisor timer 状态

kvm:kvm_wait_lapic_expire
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, guest_tsc, tsc_deadline, dy_nsec
  用途: 跟踪 LAPIC 定时器到期等待
```

### 1.5 性能调优事件

```
kvm:kvm_halt_poll_ns
  来源: include/trace/events/kvm.h:347
  参数: grow (bool), vcpu_id, new (ns), old (ns)
  用途: ★ halt-polling 窗口自适应跟踪（增长/收缩的打印点在
        virt/kvm/kvm_main.c:3686 与 :3705）
  命令: echo kvm:kvm_halt_poll_ns >> /sys/kernel/debug/tracing/set_event

kvm:kvm_ple_window_update
  来源: arch/x86/kvm/trace.h:978
  参数: vcpu_id, new, old
  用途: PLE 窗口变化跟踪

kvm:kvm_pml_full
  来源: arch/x86/kvm/trace.h:963
  参数: vcpu_id —— **只有这一个**，没有 full_count（字段定义 :967-969，
        TP_printk 是 "vcpu %d: PML full"）
  用途: PML buffer 满事件

kvm:kvm_vcpu_wakeup
  来源: include/trace/events/kvm.h:43
  参数: ns, waited (bool), valid (bool) —— 没有 vcpu_id，也没有
        runnable/blocking 这两个字段（字段定义 :47-51）
  用途: vCPU 唤醒事件
```

### 1.6 嵌套虚拟化事件

```
kvm:kvm_nested_vmenter
  来源: arch/x86/kvm/trace.h
  参数: rip, vmcs, nested_vmcs, l2_rip, l2_rsp
  用途: L1→L2 VM-Entry

kvm:kvm_nested_vmexit
  来源: arch/x86/kvm/trace.h:679（`TRACE_EVENT_KVM_EXIT(kvm_nested_vmexit)`，
        宏体 :297-331）
  参数: 与 kvm_exit **完全相同**（exit_reason, guest_rip, isa, info1, info2,
        intr_info, error_code, vcpu_id）—— 没有 exit_reason_full，也没有 l1_rsp
  用途: L2→L1 VM-Exit

kvm:kvm_nested_vmenter_failed
  来源: arch/x86/kvm/trace.h
  参数: rip, error
  用途: L1→L2 VM-Entry 失败

kvm:kvm_nested_intercepts
  来源: arch/x86/kvm/trace.h
  参数: cr_read, cr_write, exceptions, intercept
  用途: 嵌套拦截位跟踪

kvm:kvm_nested_vmexit_inject
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, exit_reason
  用途: 嵌套 VM-Exit 注入到 L1

kvm:kvm_nested_intr_vmexit
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, vector
  用途: 嵌套中断引起的 VM-Exit
```

### 1.7 CPU 操作事件

```
kvm:kvm_cpuid
  来源: arch/x86/kvm/trace.h:214
  参数: vcpu_id, function, index, rax, rbx, rcx, rdx
  用途: CPUID 指令跟踪

kvm:kvm_msr
  来源: arch/x86/kvm/trace.h:428
  参数: vcpu_id, write (bool), ecx, data
  用途: MSR 读写跟踪
  实用片段 — 按 MSR 号统计退出频率:
    cat $TRACEFS/trace | grep kvm_msr | \
        grep -oP 'msr=0x[0-9a-f]+' | sort | uniq -c | sort -rn | head
    # 读/写方向: 分别统计 write=0 / write=1

kvm:kvm_cr
  来源: arch/x86/kvm/trace.h:460
  参数: vcpu_id, write (bool), cr, val
  用途: 控制寄存器读写跟踪

kvm:kvm_emulate_insn
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, failed, insn_bytes, rip
  用途: 指令模拟跟踪
```

---

## 2. perf kvm stat

### 2.1 基本用法

```bash
# 记录 10 秒的 KVM 统计
sudo perf kvm stat record -p $QEMU_PID -- sleep 10

# 查看 VM-Exit 原因分布
sudo perf kvm stat report

# 输出示例:
#  VM-Exit Reason        Count    %
#  ──────────────────    ─────    ───
#  EXTERNAL_INTERRUPT    15234    45.2%
#  EPT_VIOLATION         8456     25.1%
#  CPUID                 3211     9.5%
#  HLT                   1024     3.0%
#  IO_INSTRUCTION        512      1.5%
#  ...
```

### 2.2 高级用法

```bash
# 按 VM-Exit 延迟排序
sudo perf kvm stat record -p $QEMU_PID -- sleep 10
sudo perf kvm stat report --sort=reason

# 分析特定 vCPU
sudo perf kvm stat record -t $VCPU_TID -- sleep 10

# 与 CPU profile 结合
sudo perf kvm stat record -g -p $QEMU_PID -- sleep 10
sudo perf kvm stat report --stdio

# 生成报告
sudo perf kvm stat record -p $QEMU_PID -- sleep 60
sudo perf kvm stat report > /tmp/kvm-stats.txt
```

### 2.3 常用分析模式

```bash
# 对比不同工作负载的 VM-Exit 分布
for workload in idle network cpu mem; do
    echo "=== $workload ==="
    sudo perf kvm stat record -p $QEMU_PID -- sleep 10
    sudo perf kvm stat report --stdio | head -15
    echo ""
done

# 监控 VM-Exit 率
while true; do
    exits=$(sudo perf kvm stat record -p $QEMU_PID -- sleep 1 2>&1 | \
        grep -oP '\d+ exits' | head -1)
    echo "$(date +%H:%M:%S) $exits"
    sleep 1
done
```

---

## 3. KVM sysfs 与 debugfs 接口

### 3.1 sysfs 模块参数 (`/sys/module/kvm*/parameters/`)

★ **默认值不在这里**。逐参数的默认值、作用域与"能不能在一轮实验里连续扫"只有一份，
在 [`../phase9-performance/parameters.md`](../phase9-performance/parameters.md) ——
本节曾经抄了一份，抄出来的四个数**全是错的**（`halt_poll_ns` 写成 400000、
`nx_huge_pages` 标成 `[bool] 默认 1`、列了一个根本不存在的 `lapic_timer_advance_ns`、
`ple_window_shrink`/`ple_window_max` 两个默认值也不对），所以整段重做。
本节只回答调试者真正要问的两件事：**这个参数存不存在**、**运行时能不能改**。

```bash
# 存在性：直接列，别照文档抄
ls /sys/module/kvm/parameters/ /sys/module/kvm_intel/parameters/
# 可写性：%a 是八进制权限，444 就是只读，echo 会 EPERM
stat -c '%n  %a' /sys/module/kvm_intel/parameters/vpid
```

### kvm 模块（★ = `0444` 只读，运行时改不了）

```
halt_poll_ns                    0644  virt/kvm/kvm_main.c:79
halt_poll_ns_grow               0644  virt/kvm/kvm_main.c:84
halt_poll_ns_grow_start         0644  virt/kvm/kvm_main.c:89
halt_poll_ns_shrink             0644  virt/kvm/kvm_main.c:94   ← ★ 0 表示"一次失手就归零"
tdp_mmu                       ★ 0444  arch/x86/kvm/mmu/mmu.c:112
nx_huge_pages                   0644  arch/x86/kvm/mmu/mmu.c:87   ← ★ module_param_cb，
                                         只收 off/force/auto/never（解析在 :7259，
                                         分支 :7268-7284），**不是布尔**
nx_huge_pages_recovery_ratio    0644  arch/x86/kvm/mmu/mmu.c:89
nx_huge_pages_recovery_period_ms 0644 arch/x86/kvm/mmu/mmu.c:92
mmio_caching                  ★ 0444  arch/x86/kvm/mmu/spte.c:24
eager_page_split                0644  arch/x86/kvm/x86.c:194
min_timer_period_us             0644  arch/x86/kvm/x86.c:161
kvmclock_periodic_sync        ★ 0444  arch/x86/kvm/x86.c:164
pi_inject_timer                 0644  arch/x86/kvm/x86.c:186
tsc_tolerance_ppm               0644  arch/x86/kvm/x86.c:168
```

★ **`lapic_timer_advance_ns` 不是 kvm 模块参数**，`/sys/module/kvm/parameters/` 下没有它。
6.12.93 里模块参数只有 `lapic_timer_advance`（`bool`，`arch/x86/kvm/lapic.c:70-71`，0444）；
`lapic_timer_advance_ns` 是 **per-vCPU 的 debugfs 只读文件**
（`arch/x86/kvm/debugfs.c:67`），**手动设固定提前量做不到**。
★ 宿主内核可能与参考内核**恰好相反**（本机 6.8.0-51 有 `lapic_timer_advance_ns` 参数、
没有 `lapic_timer_advance` bool），所以**先 `ls` 再写文档**。

### kvm_intel 模块（这一组几乎全是 `0444`）

```
ept                           ★ 0444  arch/x86/kvm/vmx/vmx.c:99
eptad                         ★ 0444  arch/x86/kvm/vmx/vmx.c:106
vpid                          ★ 0444  arch/x86/kvm/vmx/vmx.c:90
enable_apicv                  ★ 0444  arch/x86/kvm/vmx/vmx.c:114
enable_ipiv                   ★ 0444  arch/x86/kvm/vmx/vmx.c:117
flexpriority                  ★ 0444  arch/x86/kvm/vmx/vmx.c:96
fasteoi                       ★ 0444  arch/x86/kvm/vmx/vmx.c:112
nested                        ★ 0444  arch/x86/kvm/vmx/vmx.c:125
pml                           ★ 0444  arch/x86/kvm/vmx/vmx.c:128
ple_gap                       ★ 0444  arch/x86/kvm/vmx/vmx.c:204
ple_window                    ★ 0444  arch/x86/kvm/vmx/vmx.c:207
ple_window_grow               ★ 0444  arch/x86/kvm/vmx/vmx.c:211
ple_window_shrink             ★ 0444  arch/x86/kvm/vmx/vmx.c:215
ple_window_max                ★ 0444  arch/x86/kvm/vmx/vmx.c:219
enlightened_vmcs              ★ 0444  arch/x86/kvm/vmx/vmx.c:536
emulate_invalid_guest_state   ★ 0444  arch/x86/kvm/vmx/vmx.c:109
dump_invalid_vmcs               0644  arch/x86/kvm/vmx/vmx.c:134
allow_smaller_maxphyaddr      ★ 只读  arch/x86/kvm/vmx/vmx.c:149  （S_IRUGO）
enable_shadow_vmcs            ★ 只读  arch/x86/kvm/vmx/nested.c:24（S_IRUGO）
```

**结论：想改 `kvm_intel` 那一组，只有 insmod 传参或内核启动参数一条路，改完要重启 VM。**
运行时真能写的只有 `halt_poll_ns` 一族、`eager_page_split`、`min_timer_period_us`、
`pi_inject_timer`、`tsc_tolerance_ppm`、`nx_huge_pages*`、`dump_invalid_vmcs`。

### 3.2 debugfs 接口 (`/sys/kernel/debug/kvm/`)

```
目录结构:
  /sys/kernel/debug/kvm/
  ├── <pid>-<vmid>/              ← 每个 VM 一个目录
  │   ├── pid                    ← vCPU 对应的 host PID
  │   ├── stats                  ← ★ VM 级统计数据 (二进制格式)
  │   ├── mmu_rmaps_stat         ← MMU rmap 统计 (x86 特定)
  │   ├── vcpu<n>/               ← 每个 vCPU 一个子目录
  │   │   ├── pid                ← vCPU 线程 PID
  │   │   ├── guest_mode         ← ★ 是否在 guest 模式 (0/1)
  │   │   ├── tsc-offset         ← ★ 当前 TSC 偏移
  │   │   ├── lapic_timer_advance_ns  ← LAPIC 定时器提前量
  │   │   ├── tsc-scaling-ratio       ← TSC 缩放比率
  │   │   └── tsc-scaling-ratio-frac-bits ← TSC 缩放分数位
  │   └── stats                ← vCPU 级统计
  │
  ├── vm_stat_*                ← 全局 VM 统计 (二进制)
  └── vcpu_stat_*              ← 全局 vCPU 统计 (二进制)

常用操作:
  # 查看某个 vCPU 是否在 guest 模式
  cat /sys/kernel/debug/kvm/<pid>-<vmid>/vcpu0/guest_mode

  # 查看 TSC 偏移
  cat /sys/kernel/debug/kvm/<pid>-<vmid>/vcpu0/tsc-offset

  # 查看 MMU rmap 统计
  cat /sys/kernel/debug/kvm/<pid>-<vmid>/mmu_rmaps_stat
```

### 3.3 KVM 统计数据 (stats) 接口

```
KVM 通过 debugfs 暴露二进制 stats 接口:
  - VM 级统计: /sys/kernel/debug/kvm/<pid>-<vmid>/stats
  - vCPU 级统计: /sys/kernel/debug/kvm/<pid>-<vmid>/vcpu<n>/stats

VM 级关键统计 (arch/x86/kvm/x86.c:233-246):
  mmu_shadow_zapped     ← MMU shadow 页被回收次数
  mmu_pte_write         ← PTE 写入次数
  mmu_pde_zapped        ← PDE 被回收次数
  mmu_flooded           ← MMU 被洪水攻击次数
  mmu_recycled          ← MMU 页回收次数
  mmu_cache_miss        ← MMU 缓存未命中
  mmu_unsync            ← 非同步 MMU 页数
  pages_4k              ← 4K 页数
  pages_2m              ← 2MB 大页数
  pages_1g              ← 1GB 大页数
  nx_lpage_splits       ← NX 大页拆分次数
  max_mmu_rmap_size     ← 最大 rmap 大小
  max_mmu_page_hash_collisions ← 最大 MMU 页哈希冲突

vCPU 级关键统计 (arch/x86/kvm/x86.c:259-271):
  pf_taken              ← 缺页发生次数
  pf_fixed              ← 缺页修复次数
  pf_emulate            ← 模拟的缺页
  pf_spurious           ← 虚假缺页
  pf_fast               ← 快速路径缺页
  pf_mmio_spte_created  ← MMIO SPTE 创建数
  pf_guest              ← Guest 侧缺页
  tlb_flush             ← TLB 刷新次数
  invlpg                ← INVLPG 指令次数
  exits                 ← ★ VM-Exit 总次数
  io_exits              ← IO VM-Exit 次数
  mmio_exits            ← MMIO VM-Exit 次数

也可以使用二进制接口 KVM_GET_STATS_FD (include/uapi/linux/kvm.h:1541)
  ioctl(vcpu_fd, KVM_GET_STATS_FD, 0)
  → 返回 stats fd, 可 mmap 读取二进制数据

工具:
  - virt-what: 通过 stats 识别虚拟化类型
  - 自定义工具: 解析二进制 stats 数据
```

### 3.4 实战: 使用 debugfs 监控 VM

```bash
#!/bin/bash
# 监控 KVM VM 状态

# 找到 KVM 目录
KVM_DIR=$(ls -d /sys/kernel/debug/kvm/*/ 2>/dev/null | head -1)
if [ -z "$KVM_DIR" ]; then
    echo "未找到运行中的 VM"
    exit 1
fi

echo "=== VM 信息 ==="
echo "目录: $KVM_DIR"
echo ""

echo "=== vCPU 状态 ==="
for vcpu_dir in "$KVM_DIR"/vcpu*/; do
    vcpu=$(basename "$vcpu_dir")
    pid=$(cat "$vcpu_dir/pid" 2>/dev/null)
    guest_mode=$(cat "$vcpu_dir/guest_mode" 2>/dev/null)
    tsc_offset=$(cat "$vcpu_dir/tsc-offset" 2>/dev/null)
    timer_adv=$(cat "$vcpu_dir/lapic_timer_advance_ns" 2>/dev/null)
    echo "  $vcpu: pid=$pid guest_mode=$guest_mode tsc_offset=$tsc_offset timer_adv=$timer_adv"
done
echo ""

echo "=== MMU rmap 统计 ==="
if [ -f "$KVM_DIR/mmu_rmaps_stat" ]; then
    cat "$KVM_DIR/mmu_rmaps_stat"
fi
echo ""

# 实时监控 vCPU guest_mode 变化
echo "=== 实时监控 (Ctrl+C 停止) ==="
while true; do
    for vcpu_dir in "$KVM_DIR"/vcpu*/; do
        vcpu=$(basename "$vcpu_dir")
        mode=$(cat "$vcpu_dir/guest_mode" 2>/dev/null)
        printf "%s: %s  " "$vcpu" "$( [ "$mode" = "1" ] && echo "IN_GUEST" || echo "OUT_GUEST" )"
    done
    printf "\r"
    sleep 0.1
done
```

---

## 4. ftrace 高级用法

### 4.1 Function tracer

```bash
TRACEFS=/sys/kernel/debug/tracing

# 跟踪所有 KVM 函数
echo function > $TRACEFS/current_tracer
echo kvm > $TRACEFS/set_ftrace_filter

# 只跟踪特定函数
echo kvm_arch_vcpu_ioctl_run > $TRACEFS/set_ftrace_filter

# 多函数过滤
echo kvm_arch_vcpu_ioctl_run > $TRACEFS/set_ftrace_filter
echo vcpu_run >> $TRACEFS/set_ftrace_filter
echo vcpu_enter_guest >> $TRACEFS/set_ftrace_filter

# 开始跟踪
echo 1 > $TRACEFS/tracing_on
sleep 5
echo 0 > $TRACEFS/tracing_on

# 查看结果
cat $TRACEFS/trace | head -50
```

### 4.2 Function graph tracer

```bash
TRACEFS=/sys/kernel/debug/tracing

# 使用 function_graph 显示调用关系
echo function_graph > $TRACEFS/current_tracer
echo kvm_arch_vcpu_ioctl_run > $TRACEFS/set_graph_function

echo 1 > $TRACEFS/tracing_on
sleep 2
echo 0 > $TRACEFS/tracing_on

# 输出示例 (调用树):
# kvm_arch_vcpu_ioctl_run() {
#   vcpu_load();
#   kvm_load_guest_fpu();
#   vcpu_run() {
#     vcpu_enter_guest() {
#       kvm_x86_call(vcpu_run)();
#       /* VM-Entry → Guest → VM-Exit */
#       kvm_x86_call(handle_exit)();
#     }
#   }
#   vcpu_put();
# }
```

### 4.3 事件 + 函数组合

```bash
TRACEFS=/sys/kernel/debug/tracing

# 同时跟踪 trace events 和函数
: > $TRACEFS/set_event                      # 显式清场（O_TRUNC 会清掉全部事件，§1.1 那条 ★）
echo kvm:kvm_entry >> $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_page_fault >> $TRACEFS/set_event

echo function > $TRACEFS/current_tracer
echo kvm_handle_page_fault > $TRACEFS/set_ftrace_filter

echo 1 > $TRACEFS/tracing_on
sleep 5
echo 0 > $TRACEFS/tracing_on

# 输出会同时包含 event 和 function trace
```

### 4.4 PID 过滤

```bash
TRACEFS=/sys/kernel/debug/tracing

# 只跟踪特定 QEMU 进程
echo $QEMU_PID > $TRACEFS/set_event_pid

# 或者跟踪特定 vCPU 线程
echo $VCPU1_TID > $TRACEFS/set_event_pid
echo $VCPU2_TID >> $TRACEFS/set_event_pid
```

### 4.5 实时流式输出

```bash
# 实时查看 trace 输出
cat /sys/kernel/debug/tracing/trace_pipe

# 带时间戳
echo options:irq-info > /sys/kernel/debug/tracing/trace_options

# 保存到文件
cat /sys/kernel/debug/tracing/trace_pipe > /tmp/trace.log &
# ... 运行测试 ...
kill %1
```

### 4.6 案例：VM 生命周期跟踪（创建 → 运行 → 销毁）

完整记录一个 VM 从创建到销毁的事件序列。以下事件/函数均经
6.12.93 核实：

```bash
TRACEFS=/sys/kernel/debug/tracing
echo > $TRACEFS/trace

# 生命周期 tracepoints
: > $TRACEFS/set_event                             # 显式清场（见 §1.1 那条 ★）
echo kvm:kvm_vcpu_wakeup >> $TRACEFS/set_event     # include/trace/events/kvm.h:43
echo kvm:kvm_entry >> $TRACEFS/set_event           # arch/x86/kvm/trace.h:17
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_userspace_exit >> $TRACEFS/set_event  # include/trace/events/kvm.h:22

# 函数级：覆盖创建/运行/销毁三个阶段的 ioctl 入口
echo function > $TRACEFS/current_tracer
{
  echo kvm_dev_ioctl                  # /dev/kvm 入口
  echo kvm_create_vm                  # virt/kvm/kvm_main.c:1146
  echo kvm_vm_ioctl
  echo kvm_vcpu_ioctl                 # virt/kvm/kvm_main.c:115
  echo kvm_arch_vcpu_ioctl_run        # arch/x86/kvm/x86.c:11579
  echo kvm_vm_release                 # virt/kvm/kvm_main.c:1408
} > $TRACEFS/set_ftrace_filter
```

预期序列（创建 → 运行 → 销毁）：

```
kvm_dev_ioctl / kvm_create_vm          ← KVM_CREATE_VM
kvm_vcpu_ioctl                         ← KVM_CREATE_VCPU / KVM_RUN
kvm_vcpu_wakeup / kvm_entry / kvm_exit ← 运行循环
kvm_userspace_exit                     ← 退到用户态 (如 KVM_EXIT_IO)
kvm_vm_release                         ← VM fd 关闭，销毁
```

分析片段 —— 计算相邻两次 `kvm_exit` 的时间间隔（观察退出节奏）：

```bash
# ftrace 文本格式: task-pid  [cpu]  flags  timestamp: event
cat $TRACEFS/trace | grep kvm_exit | awk '{
    split($4, t, ":");
    ts = t[1]*3600 + t[2]*60 + t[3];
    if (prev > 0) printf "%.3f us\n", (ts-prev)*1000000;
    prev = ts
}' | head -50
```

配合 `perf sched record -- sleep 10; perf sched latency` 可进一步看
vCPU 线程在宿主上的调度延迟。

---

## 5. KVM selftests 框架

### 5.1 目录结构

```
tools/testing/selftests/kvm/
├── Makefile
├── include/
│   └── kvm_util.h          # 测试工具库
├── lib/
│   ├── kvm_util.c          # KVM 操作封装
│   └── x86_64/             # x86 特定工具
│       └── processor.c     # 处理器操作
├── x86_64/                 # x86 特定测试
│   ├── vmx_*.c             # VMX 测试
│   ├── svm_*.c             # SVM 测试
│   ├── tsc_*.c             # TSC 测试
│   └── ...
├── dirty_log_test.c        # 脏页日志测试
├── dirty_log_perf_test.c   # ★ 脏页日志性能测试
├── demand_paging_test.c    # ★ 按需分页测试
├── access_tracking_perf_test.c # ★ 访问跟踪性能测试
├── guest_memfd_test.c      # ★ guest_memfd 测试 (6.12 新增)
├── memslot_perf_test.c     # ★ memslot 性能测试
├── kvm_page_table_test.c   # KVM 页表测试
├── max_guest_memory_test.c # 最大客户内存测试
├── kvm_create_max_vcpus.c  # 最大 vCPU 创建测试
└── ...
```

### 5.2 运行测试

```bash
# 编译
cd /root/code/linux-6.12.93/tools/testing/selftests/kvm/
make

# 运行单个测试
./dirty_log_test
./dirty_log_perf_test -s 1G -v 2 -n 5

# 运行所有测试
make run_tests

# 运行特定类别
./x86_64/vmx_apic_access_test
./x86_64/tsc_scaling_test
```

### 5.3 关键测试说明

```
dirty_log_test:
  验证脏页日志正确性
  测试 KVM_GET_DIRTY_LOG 接口

dirty_log_perf_test:
  ★ 测量脏页日志性能
  参数: -s (内存大小) -v (vCPU数) -n (迭代次数)
  输出: 每轮脏页数、收集时间

demand_paging_test:
  ★ 测量按需分页性能
  模拟热迁移初始阶段的内存行为
  参数: -s (内存大小) -v (vCPU数) -b (后台线程)

access_tracking_perf_test:
  ★ 测量访问跟踪性能
  验证 idle page tracking 的开销

guest_memfd_test:
  ★ 测试 guest_memfd (6.12 新增)
  验证私有内存区域创建和访问

memslot_perf_test:
  ★ 测量 memslot 操作性能
  测试添加/删除 memslot 的开销
```

### 5.4 编写自定义测试

```c
/* 最小测试模板 */
#include "test_util.h"
#include "kvm_util.h"
#include "vmx.h"

static void run_test(void)
{
    struct kvm_vcpu *vcpu;
    struct kvm_vm *vm;

    /* 创建 VM */
    vm = vm_create_with_one_vcpu(&vcpu, guest_code);

    /* 配置 VM */
    vm_init_descriptor_tables(vm);
    vcpu_init_descriptor_tables(vcpu);

    /* 运行 vCPU */
    vcpu_run(vcpu);

    /* 检查结果 */
    TEST_ASSERT(/* ... */);

    /* 清理 */
    kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
    run_test();
    return 0;
}
```

---

## 6. bpftrace 脚本集

### 6.1 VM-Exit 延迟分析

```bash
#!/usr/bin/bpftrace
# trace-vmexit-latency.bt

/* 测量 VM-Exit 处理延迟 */

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

/* 每 5 秒打印一次直方图 */
interval:s:5
{
    print(@latency_us);
    clear(@latency_us);
}
```

### 6.2 EPT 页错误热点

```bash
#!/usr/bin/bpftrace
# trace-ept-hotspot.bt

/* 统计 EPT 页错误的 GPA 热点 */

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

### 6.3 halt-polling 监控

```bash
#!/usr/bin/bpftrace
# trace-halt-poll.bt

/* 监控 halt-polling 窗口变化 */

tracepoint:kvm:kvm_halt_poll_ns
{
    if (args->grow)
        printf("vCPU %d: poll window GROW  %d → %d ns\n",
               args->vcpu_id, args->old, args->new);
    else
        printf("vCPU %d: poll window SHRINK %d → %d ns\n",
               args->vcpu_id, args->old, args->new);
}
```

### 6.4 vCPU 调度分析

```bash
#!/usr/bin/bpftrace
# trace-vcpu-schedule.bt

/* 分析 vCPU 线程在 Host 上的调度 */

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

### 6.5 运行 bpftrace 脚本

```bash
# 直接运行
sudo bpftrace trace-vmexit-latency.bt

# 或者一行命令
sudo bpftrace -e 'tracepoint:kvm:kvm_exit { @exits[args->exit_reason] = count(); }'

# 输出统计
# ★ 只能聚合**数字** args->exit_reason：内核那套符号名是 TP_printk 里
#   kvm_print_exit_reason() 用 __print_symbolic() 现译的（arch/x86/kvm/trace.h:289-295），
#   BPF 侧拿不到；要名字就照 arch/x86/include/uapi/asm/vmx.h:32-95 自己映射，
#   或者直接读 trace 文本里的 `reason <NAME>`。
sudo bpftrace -e '
tracepoint:kvm:kvm_exit { @reasons[args->exit_reason] = count(); }
interval:s:5 { print(@reasons, 10); clear(@reasons); }
'
```

### 6.6 MSR 热点统计

```bash
#!/usr/bin/bpftrace
# trace-msr-hotspot.bt

/* 按 MSR 号统计访问频率 */

tracepoint:kvm:kvm_msr
{
    @msr[args->ecx] = count();
}

interval:s:10
{
    printf("=== MSR 访问热点（top 20）===\n");
    print(@msr, 20);
    clear(@msr);
}
```

### 6.7 嵌套虚拟化分析

```bash
#!/usr/bin/bpftrace
# trace-nested-virt.bt

/* 跟踪 L1→L2 VM-Entry 和 L2→L1 VM-Exit */

tracepoint:kvm:kvm_nested_vmenter
{
    @l1_to_l2 = count();
}

tracepoint:kvm:kvm_nested_vmexit
{
    @l2_to_l1 = count();
    @exit_reasons[args->exit_reason] = count();
}

interval:s:10
{
    printf("=== 嵌套虚拟化统计 ===\n");
    printf("L1→L2 VM-Entry: %d\n", @l1_to_l2);
    printf("L2→L1 VM-Exit:  %d\n", @l2_to_l1);
    printf("L2 Exit 原因分布:\n");
    print(@exit_reasons, 10);
    clear(@l1_to_l2);
    clear(@l2_to_l1);
    clear(@exit_reasons);
}
```

### 6.8 MMU notifier 跟踪

```bash
#!/usr/bin/bpftrace
# trace-mmu-notifier.bt

/* 跟踪 mmu_notifier 事件（宿主内存管理通知 KVM 失效映射） */

tracepoint:kvm:kvm_unmap_hva_range
{
    @unmap_count = count();
    @unmap_ranges = hist(args->end - args->start);
}

tracepoint:kvm:kvm_age_hva
{
    @age_count = count();
}

interval:s:10
{
    printf("=== MMU notifier 统计 ===\n");
    printf("unmap 次数: %d\n", @unmap_count);
    printf("unmap 范围分布:\n");
    print(@unmap_ranges);
    printf("age 次数: %d\n", @age_count);
    clear(@unmap_count);
    clear(@unmap_ranges);
    clear(@age_count);
}
```

### 6.9 中断注入延迟

```bash
#!/usr/bin/bpftrace
# trace-irq-inject-latency.bt

/* 测量从中断接受到注入的延迟 */

tracepoint:kvm:kvm_apic_accept_irq
{
    @accept_time[args->vcpu_id, args->vector] = nsecs;
}

tracepoint:kvm:kvm_inj_virq /@accept_time[args->vcpu_id, args->irq]/
{
    $delta = nsecs - @accept_time[args->vcpu_id, args->irq];
    @inject_delay = hist($delta / 1000);  // μs
    delete(@accept_time[args->vcpu_id, args->irq]);
}

interval:s:10
{
    printf("=== 中断注入延迟（μs）===\n");
    print(@inject_delay);
    clear(@inject_delay);
}
```

### 6.10 vCPU 唤醒原因分析

```bash
#!/usr/bin/bpftrace
# trace-vcpu-wakeup.bt

/* 分析 vCPU 唤醒原因（halt-polling 超时 vs 中断唤醒） */

tracepoint:kvm:kvm_vcpu_wakeup
{
    if (args->waited)
        @blocked = count();
    else
        @polling = count();

    @wakeup_ns = hist(args->ns / 1000);  // μs
}

interval:s:10
{
    printf("=== vCPU 唤醒统计 ===\n");
    printf("阻塞等待: %d\n", @blocked);
    printf("轮询唤醒: %d\n", @polling);
    printf("唤醒延迟分布（μs）:\n");
    print(@wakeup_ns);
    clear(@blocked);
    clear(@polling);
    clear(@wakeup_ns);
}
```

### 6.11 内存带宽监控

```bash
#!/usr/bin/bpftrace
# trace-mem-bandwidth.bt

/* 监控 KVM 内存操作（缺页 + MMIO）的频率 */

tracepoint:kvm:kvm_page_fault
{
    @pf_count = count();
    @pf_by_vcpu[args->vcpu_id] = count();
}

tracepoint:kvm:kvm_mmio
{
    @mmio_count = count();
}

interval:s:5
{
    printf("=== 内存操作统计（5 秒）===\n");
    printf("EPT 缺页: %d\n", @pf_count);
    printf("MMIO 操作: %d\n", @mmio_count);
    printf("按 vCPU 分布:\n");
    print(@pf_by_vcpu, 10);
    clear(@pf_count);
    clear(@pf_by_vcpu);
    clear(@mmio_count);
}
```

### 6.12 中断风暴检测

```bash
#!/usr/bin/bpftrace
# trace-irq-storm.bt

/* 检测 EXTERNAL_INTERRUPT 风暴 */

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
            printf("⚠ 中断退出比例过高，考虑启用 APICv/PI\n");
        }
    }
    clear(@total);
    clear(@ext_int);
}
```
