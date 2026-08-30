# KVM调试实战指南

> 面向VMM专家的KVM内核态调试技巧
>
> **本文是快速参考卡片**，完整的调试参考手册见 [`phase10-debugging/annotations.md`](../phase10-debugging/annotations.md)
> （包含完整的 trace events 目录、selftests 框架、bpftrace 脚本集等）

---

## 📖 本指南 vs phase10-debugging

| 本指南 (debugging-guide.md) | phase10-debugging |
|----------------------------|------------------|
| 快速参考卡片 | 完整参考手册 |
| 常用命令速查 | trace events 完整目录 |
| 典型问题处理 | 调试场景决策树 |
| 现场排查技巧 | selftests + bpftrace |

**建议**: 先看本指南快速上手，遇到问题再查阅 phase10-debugging 获取详细参考

---

## 🎯 调试工具概览

| 工具 | 用途 | 适用场景 |
|------|------|----------|
| **ftrace** | 函数调用跟踪 | 追踪KVM函数、VM-Exit原因 |
| **perf kvm** | 性能分析 | VM-Exit分布、热点函数 |
| **GDB** | 内核调试 | 调试KVM数据结构 |
| **dmesg** | 日志查看 | 错误信息、初始化日志 |
| **debugfs** | 运行时信息 | KVM内部状态查看 |

---

## 🔍 ftrace 高效用法

### 1. 基础配置

```bash
# 挂载debugfs (如果未自动挂载)
mount -t debugfs none /sys/kernel/debug

# 查看可用的KVM tracepoints
ls /sys/kernel/debug/tracing/events/kvm/

# 核心tracepoints:
# kvm_entry      ← VM-Entry (进入Guest前)
# kvm_exit       ← VM-Exit (退出原因)
# kvm_page_fault ← EPT页错误
# kvm_inj_virq   ← 虚拟中断注入
# kvm_msr        ← MSR读写
# kvm_cpuid      ← CPUID处理
```

### 2. 追踪特定VM

```bash
# 获取QEMU进程PID
QEMU_PID=$(pidof qemu-system-x86)

# 只追踪该进程的KVM事件
echo "pid == $QEMU_PID" > /sys/kernel/debug/tracing/events/kvm/filter

# 启用KVM事件
echo 1 > /sys/kernel/debug/tracing/events/kvm/enable

# 启动追踪
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 运行VM...

# 查看结果
cat /sys/kernel/debug/tracing/trace

# 清理
echo 0 > /sys/kernel/debug/tracing/tracing_on
echo 0 > /sys/kernel/debug/tracing/events/kvm/enable
```

### 3. 追踪特定函数调用链

```bash
# 追踪vmx_vcpu_run的完整调用链
echo vmx_vcpu_run > /sys/kernel/debug/tracing/set_ftrace_filter
echo function_graph > /sys/kernel/debug/tracing/current_tracer
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 运行VM...

# 查看调用图
cat /sys/kernel/debug/tracing/trace

# 清理
echo nop > /sys/kernel/debug/tracing/current_tracer
echo > /sys/kernel/debug/tracing/set_ftrace_filter
```

### 4. 追踪特定VM-Exit原因

```bash
# 只追踪EPT_VIOLATION
echo 'exit_reason == 48' > /sys/kernel/debug/tracing/events/kvm/kvm_exit/filter

# 或追踪多种原因
echo 'exit_reason == 48 || exit_reason == 1' > \
  /sys/kernel/debug/tracing/events/kvm/kvm_exit/filter

# exit_reason 代码:
# 1  = EXTERNAL_INTERRUPT
# 10 = CPUID
# 15 = INVD
# 28 = IO_INSTRUCTION
# 48 = EPT_VIOLATION
```

### 5. 使用trace-cmd

```bash
# 录制
trace-cmd record -e kvm:kvm_exit -e kvm:kvm_entry -e kvm:kvm_page_fault \
  -p function_graph -g vmx_vcpu_run -- sleep 10

# 报告
trace-cmd report

# 使用kernelshark可视化
kernelshark trace.dat
```

---

## 📊 perf kvm 分析

### 1. VM-Exit分布统计

```bash
# 录制
perf kvm stat record -- sleep 30

# 报告
perf kvm stat report

# 输出示例:
#  Event            Samples    Samples%  Time%    Min Time    Max Time    Avg time
#  EXTERNAL_INTERRUPT  12345   45.67%    12.34%   1.23us      5.67us      2.34us
#  EPT_VIOLATION       8901    32.89%    45.67%   2.34us      10.56us     4.56us
#  IO_INSTRUCTION      5678    20.89%    35.67%   3.45us      15.67us     6.78us
#  ...
```

### 2. 热点函数分析

```bash
# 录制所有事件
perf record -g -a -- sleep 30

# 查看KVM相关热点
perf report --sort=dso,comm,symbol | grep kvm

# 或查看火焰图
perf script | stackcollapse-perf.pl | flamegraph.pl > kvm.svg
```

### 3. 特定KVM事件分析

```bash
# 分析EPT页错误
perf record -e kvm:kvm_page_fault -a -- sleep 10
perf report

# 分析中断注入
perf record -e kvm:kvm_inj_virq -a -- sleep 10
perf report
```

### 4. 实时分析

```bash
# 实时查看VM-Exit统计
perf kvm stat live

# 或每5秒更新
watch -n 5 'perf kvm stat record -- sleep 5 && perf kvm stat report'
```

---

## 🐛 GDB 调试 KVM

### 1. 附加到QEMU进程

```bash
# 获取QEMU PID
QEMU_PID=$(pidof qemu-system-x86)

# 附加GDB
gdb -p $QEMU_PID

# 查看KVM数据结构
(gdb) p *(struct kvm *)0x...
(gdb) p *(struct kvm_vcpu *)0x...
(gdb) p *(struct vcpu_vmx *)0x...
```

### 2. 查找KVM数据结构地址

```bash
# 在QEMU中查找kvm结构
(gdb) info variables kvm

# 或查找vcpu
(gdb) info variables vcpu

# 查看内存布局
(gdb) x/100x 0x<address>
```

### 3. 调试KVM内核模块

```bash
# 加载KVM符号
gdb vmlinux
(gdb) add-symbol-file /path/to/kvm.ko 0x<load_address>

# 设置断点
(gdb) b vmx_vcpu_run
(gdb) b kvm_handle_page_fault
(gdb) b vmx_pi_update_irte

# 附加到QEMU
(gdb) attach $QEMU_PID

# 继续执行
(gdb) c
```

### 4. 常用调试命令

```bash
# 查看vCPU状态
(gdb) p vcpu->arch.mp_state
(gdb) p vcpu->arch.cr0
(gdb) p vcpu->arch.cr3
(gdb) p vcpu->arch.regs[VCPU_REGS_RSP]

# 查看VMCS字段
(gdb) p vmx->vmcs01.vmcs
(gdb) p vmx->exit_reason.full

# 查看EPT页表
(gdb) p kvm->arch.eptp
(gdb) p *(struct kvm_mmu_page *)0x...
```

---

## 🔧 常见问题排查

### 问题1：VM-Entry失败

**症状**：`dmesg`显示"kvm: vm entry failed"

**排查步骤**：
```bash
# 1. 查看详细错误
dmesg | grep -i "vm entry failed"

# 2. 检查Guest状态
# 通常是CR0/CR4/EFER未正确设置

# 3. 检查VMCS字段
# 使用GDB查看vmcs配置

# 4. 检查CPU特性
grep -E "vmx|ept|vpid" /proc/cpuinfo
```

**常见原因**：
- Guest控制寄存器（CR0/CR4/EFER）未设置
- 段寄存器配置错误
- VMCS字段未初始化

### 问题2：性能异常差

**症状**：VM-Exit次数异常多

**排查步骤**：
```bash
# 1. 分析VM-Exit分布
perf kvm stat record -- sleep 30
perf kvm stat report

# 2. 查看哪种VM-Exit最多
# 如果是MSR：检查MSR Bitmap配置
# 如果是IO：检查IO Bitmap配置
# 如果是EPT_VIOLATION：检查内存映射

# 3. 追踪具体函数
echo kvm_vcpu_run > /sys/kernel/debug/tracing/set_ftrace_filter
echo function > /sys/kernel/debug/tracing/current_tracer
```

**常见原因**：
- MSR Bitmap未配置，所有MSR都被拦截
- IO Bitmap未配置，所有IO都被拦截
- EPT未启用，使用影子页表

### 问题3：内存映射失败

**症状**：Guest访问内存时崩溃

**排查步骤**：
```bash
# 1. 检查memslot配置
cat /sys/kernel/debug/kvm/<vm_id>/memslots

# 2. 检查EPT页错误
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_page_fault/enable

# 3. 查看EPT错误原因
cat /sys/kernel/debug/tracing/trace | grep page_fault
# 关注 error_code:
# bit 0: P (0=缺失, 1=权限)
# bit 1: W/R (0=读, 1=写)
# bit 2: U/S (0=管理态, 1=用户态)
```

**常见原因**：
- memslot GPA/HVA/大小不匹配
- EPT页表损坏
- 大页对齐错误

### 问题4：中断丢失

**症状**：Guest设备无响应

**排查步骤**：
```bash
# 1. 检查中断注入
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_inj_virq/enable

# 2. 查看中断路由
cat /sys/kernel/debug/kvm/<vm_id>/irq_routing

# 3. 检查Posted Interrupts
cat /sys/module/kvm_intel/parameters/enable_apicv

# 4. 检查IRTE配置
cat /sys/kernel/debug/iommu/irq_remap
```

**常见原因**：
- 中断路由表未配置
- APICv未启用
- Posted Interrupts配置错误
- vCPU迁移时PI状态未更新

---

## 📈 性能调优实战

### 1. 减少VM-Exit次数

```bash
# 1. 分析VM-Exit分布
perf kvm stat report

# 2. 针对高频VM-Exit优化
# MSR拦截过多: 调整MSR Bitmap
# IO拦截过多: 调整IO Bitmap
# EPT_VIOLATION过多: 使用大页内存

# 3. 启用优化特性
echo 1 > /sys/module/kvm_intel/parameters/ept
echo 1 > /sys/module/kvm_intel/parameters/vpid
echo 1 > /sys/module/kvm_intel/parameters/enable_apicv
```

### 2. 优化内存性能

```bash
# 1. 使用大页内存
echo 1024 > /proc/sys/vm/nr_hugepages
qemu-system-x86_64 -mem-path /dev/hugepages -mem-prealloc ...

# 2. 启用EPT A/D位
echo 1 > /sys/module/kvm_intel/parameters/eptad

# 3. 启用MMIO缓存
echo 1 > /sys/module/kvm/parameters/mmio_caching
```

### 3. 优化中断性能

```bash
# 1. 启用APICv + Posted Interrupts
modprobe -r kvm_intel
modprobe kvm_intel enable_apicv=1

# 2. 启用IOMMU中断重映射
echo "intel_iommu=on" >> /etc/default/grub
update-grub && reboot

# 3. 绑定中断亲和性
echo 2 > /proc/irq/<irq_num>/smp_affinity
```

### 4. 优化CPU性能

```bash
# 1. 绑定vCPU亲和性
taskset -p 0x1 $QEMU_PID  # vCPU 0 → pCPU 0
taskset -p 0x2 $QEMU_PID  # vCPU 1 → pCPU 1

# 2. 调整halt-polling
echo 400000 > /sys/module/kvm/parameters/halt_poll_ns

# 3. 启用VPID
echo 1 > /sys/module/kvm_intel/parameters/vpid
```

---

## 🔥 高级技巧

### 1. 动态修改KVM参数

```bash
# 某些参数可以运行时修改
echo 1000000 > /sys/module/kvm/parameters/halt_poll_ns

# 某些参数需要重新加载模块
modprobe -r kvm_intel
modprobe kvm_intel enable_apicv=1 vpid=1 ept=1
```

### 2. 使用bpftrace

```bash
# 追踪VM-Exit延迟
bpftrace -e '
tracepoint:kvm:kvm_exit {
    @start[tid] = nsecs;
}

tracepoint:kvm:kvm_entry {
    if (@start[tid]) {
        @usecs = hist((nsecs - @start[tid]) / 1000);
        delete(@start[tid]);
    }
}
'

# 追踪EPT页错误
bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @faults[args->gfn] = count();
}

END {
    print(@faults);
}
'
```

### 3. 自定义ftrace脚本

```bash
#!/bin/bash
# trace-vmexit.sh

TRACE_DIR=/sys/kernel/debug/tracing

# 清理
echo 0 > $TRACE_DIR/tracing_on
echo > $TRACE_DIR/trace

# 配置
echo function > $TRACE_DIR/current_tracer
echo vmx_vcpu_run > $TRACE_DIR/set_ftrace_filter
echo vmx_handle_exit >> $TRACE_DIR/set_ftrace_filter
echo kvm_handle_page_fault >> $TRACE_DIR/set_ftrace_filter

# 启用
echo 1 > $TRACE_DIR/tracing_on

echo "Tracing... Press Ctrl+C to stop"
cat $TRACE_DIR/trace_pipe
```

---

## 📚 参考资源

- Linux kernel documentation: `Documentation/trace/ftrace.rst`
- perf examples: `perf list`, `perf record --help`
- KVM tracepoints: `ls /sys/kernel/debug/tracing/events/kvm/`
- GDB kernel debugging: `Documentation/dev-tools/gdb-kernel-debugging.rst`
