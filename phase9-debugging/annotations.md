# Phase 9：源码精读注释 - KVM 调试与测试

> 基于 Linux 6.12.93 源码 | 所有 trace events 均已验证存在

---

## 1. 完整 KVM trace events 目录

### 1.1 入口/出口事件

```
kvm:kvm_entry
  来源: arch/x86/kvm/trace.h:17
  参数: vcpu_id, rip
  用途: 跟踪每次 VM-Entry
  命令: echo kvm:kvm_entry > /sys/kernel/debug/tracing/set_event

kvm:kvm_exit
  来源: arch/x86/kvm/trace.h:336 (TRACE_EVENT_KVM_EXIT)
  参数: vcpu_id, rip, exit_reason (数字), exit_reason_full (字符串)
  用途: 跟踪每次 VM-Exit 及原因
  命令: echo kvm:kvm_exit > /sys/kernel/debug/tracing/set_event

kvm:kvm_userspace_exit
  来源: include/trace/events/kvm.h
  参数: vcpu_id, exit_reason, ret
  用途: 跟踪返回用户空间的事件
```

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
  参数: vcpu_id, fault_address, error_code
  用途: ★ 最重要的内存事件，跟踪每次 EPT Violation
  命令: echo kvm:kvm_page_fault > /sys/kernel/debug/tracing/set_event

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
  来源: include/trace/events/kvm.h
  参数: grow (bool), vcpu_id, new (ns), old (ns)
  用途: ★ halt-polling 窗口自适应跟踪
  命令: echo kvm:kvm_halt_poll_ns > /sys/kernel/debug/tracing/set_event

kvm:kvm_ple_window_update
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, new, old
  用途: PLE 窗口变化跟踪

kvm:kvm_pml_full
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, full_count
  用途: PML buffer 满事件

kvm:kvm_vcpu_wakeup
  来源: include/trace/events/kvm.h
  参数: vcpu_id, runnable, blocking
  用途: vCPU 唤醒事件
```

### 1.6 嵌套虚拟化事件

```
kvm:kvm_nested_vmenter
  来源: arch/x86/kvm/trace.h
  参数: rip, vmcs, nested_vmcs, l2_rip, l2_rsp
  用途: L1→L2 VM-Entry

kvm:kvm_nested_vmexit
  来源: arch/x86/kvm/trace.h
  参数: vcpu_id, rip, exit_reason, exit_reason_full, l1_rsp
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

## 3. ftrace 高级用法

### 3.1 Function tracer

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

### 3.2 Function graph tracer

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

### 3.3 事件 + 函数组合

```bash
TRACEFS=/sys/kernel/debug/tracing

# 同时跟踪 trace events 和函数
echo kvm:kvm_entry > $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_page_fault >> $TRACEFS/set_event

echo function > $TRACEFS/current_tracer
echo kvm_handle_page_fault > $TRACEFS/set_ftrace_filter

echo 1 > $TRACEFS/tracing_on
sleep 5
echo 0 > $TRACEFS/tracing_on

# 输出会同时包含 event 和 function trace
```

### 3.4 PID 过滤

```bash
TRACEFS=/sys/kernel/debug/tracing

# 只跟踪特定 QEMU 进程
echo $QEMU_PID > $TRACEFS/set_event_pid

# 或者跟踪特定 vCPU 线程
echo $VCPU1_TID > $TRACEFS/set_event_pid
echo $VCPU2_TID >> $TRACEFS/set_event_pid
```

### 3.5 实时流式输出

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

---

## 4. debugfs KVM 接口

### 4.1 VM 级统计

```bash
# 列出所有 KVM VM 目录
ls /sys/kernel/debug/kvm/

# 每个 VM 的统计数据
cat /sys/kernel/debug/kvm/<vm-id>/stats

# 常见统计字段:
# - mmu_shadow_zapped
# - mmu_unsync
# - mmu_recycled
# - remote_tlb_flush
# - request_irq_exits
# - signal_exits
# - ...
```

### 4.2 vCPU 级 debugfs

```bash
# 在 arch/x86/kvm/debugfs.c 中定义
# 路径: /sys/kernel/debug/kvm/<vm-id>/vcpu/<vcpu-id>/

# 可用文件:
# - guest_mode (当前是否在 guest 模式)
# - tsc-offset (当前 TSC 偏移)
# - lapic_timer_advance_ns (LAPIC 定时器提前量)
# - tsc-scaling-ratio (TSC 缩放比率)
# - tsc-scaling-ratio-frac-bits (TSC 缩放的分数位数)
# - mmu_rmaps_stat (MMU rmap 统计)
```

### 4.3 全局 KVM 统计

```bash
# 系统级 KVM 统计
cat /sys/kernel/debug/kvm/*/stats

# 或使用 kvm_stat 工具
sudo kvm_stat

# 或使用 virt-what
cat /sys/kernel/debug/kvm/0/stats
```

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
sudo bpftrace -e '
tracepoint:kvm:kvm_exit { @reasons[args->exit_reason_full] = count(); }
interval:s:5 { print(@reasons, 10); clear(@reasons); }
'
```
