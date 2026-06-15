# 实践项目 1：VM 生命周期跟踪

> 目标：使用 ftrace/perf 完整跟踪虚拟机的生命周期

---

## 🎯 项目目标

通过跟踪工具完整记录一个虚拟机从创建到销毁的全过程，理解 KVM
内部的 vCPU 生命周期管理和 VM-Exit 处理机制。

---

## 📋 前置知识

- 第一阶段：KVM 基础架构（vCPU 创建、KVM_RUN ioctl）
- VM-Exit 类型和处理流程

---

## 🔧 实验环境

```bash
# 启动一个测试虚拟机
# 建议使用轻量级 VM 减少干扰
qemu-system-x86_64 \
    -enable-kvm \
    -m 1G \
    -smp 2 \
    -cpu host \
    -nographic \
    -kernel /boot/vmlinuz \
    -initrd /boot/initrd \
    -append "console=ttyS0 root=/dev/vda1" \
    -drive file=test.qcow2,format=qcow2 \
    -serial mon:stdio &

# 记录 QEMU PID
QEMU_PID=$!
```

---

## 📊 实验步骤

### 步骤 1：跟踪 VM 创建

```bash
# 设置跟踪
cat > /tmp/setup-trace.sh << 'EOF'
#!/bin/bash
TRACEFS=/sys/kernel/debug/tracing

# 清空之前的跟踪
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

# 设置要跟踪的事件
# ★ 注意: 以下 trace events 均已验证在 6.12.93 中存在
echo kvm:kvm_vcpu_wakeup > $TRACEFS/set_event
echo kvm:kvm_entry >> $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_userspace_exit >> $TRACEFS/set_event

# 也跟踪函数调用 (用于替代不存在的 kvm_vm_open/kvm_create_vcpu 等)
echo function > $TRACEFS/current_tracer
echo kvm_dev_ioctl > $TRACEFS/set_ftrace_filter
echo kvm_create_vm >> $TRACEFS/set_ftrace_filter
echo kvm_vm_ioctl >> $TRACEFS/set_ftrace_filter
echo kvm_vcpu_ioctl >> $TRACEFS/set_ftrace_filter
echo kvm_arch_vcpu_ioctl_run >> $TRACEFS/set_ftrace_filter
echo kvm_vm_release >> $TRACEFS/set_ftrace_filter

echo "跟踪已设置，请启动虚拟机"
EOF
chmod +x /tmp/setup-trace.sh
sudo /tmp/setup-trace.sh
```

### 步骤 2：记录 VM 启动过程

```bash
# 开始跟踪
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 启动虚拟机
qemu-system-x86_64 -enable-kvm -m 512M -smp 1 \
    -cpu host -nographic -serial mon:stdio &

# 等待几秒让 VM 完成启动
sleep 5

# 停止跟踪
echo 0 > /sys/kernel/debug/tracing/tracing_on

# 保存跟踪数据
cp /sys/kernel/debug/tracing/trace /tmp/vm-lifecycle-trace.txt
```

### 步骤 3：分析 VM-Exit 分布

```bash
# 统计 VM-Exit 原因分布
echo 1 > /sys/kernel/debug/tracing/tracing_on
sleep 10
echo 0 > /sys/kernel/debug/tracing/tracing_on

# 分析退出原因
cat /sys/kernel/debug/tracing/trace | grep "kvm_exit" | \
    awk '{print $NF}' | sort | uniq -c | sort -rn | head -20

# 使用 perf kvm stat
sudo perf kvm stat record -p $QEMU_PID -- sleep 10
sudo perf kvm stat report
```

### 步骤 4：分析 vCPU 调度

```bash
# 跟踪 vCPU 调度事件
echo sched:sched_switch > /sys/kernel/debug/tracing/set_event
echo kvm:kvm_entry >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event

echo 1 > /sys/kernel/debug/tracing/tracing_on
sleep 5
echo 0 > /sys/kernel/debug/tracing/tracing_on

# 分析 vCPU 在 host 上的调度行为
cat /sys/kernel/debug/tracing/trace | grep -E "kvm_entry|kvm_exit|sched_switch" | \
    head -50
```

### 步骤 5：跟踪 VM 销毁

```bash
# 重新开始跟踪
echo > /sys/kernel/debug/tracing/trace
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 在 QEMU monitor 中输入 quit 或 kill QEMU 进程
kill $QEMU_PID
sleep 2

echo 0 > /sys/kernel/debug/tracing/tracing_on

# 分析销毁过程
cat /sys/kernel/debug/tracing/trace | grep -E "kvm_vm|kvm_vcpu" | tail -30
```

---

## 📈 预期输出

### 典型的 VM 创建跟踪

```
# VM 创建 (通过 function trace 捕获)
kvm_dev_ioctl: command KVM_CREATE_VM (ioctl 0xae01)
kvm_create_vm: allocating new VM
kvm_vcpu_ioctl: command KVM_CREATE_VCPU (ioctl 0xae41)

# vCPU 唤醒 (trace event)
kvm_vcpu_wakeup: vcpu 0, runnable

# VM 运行循环 (trace events)
kvm_entry: vcpu 0, rip 0xfffffff0
kvm_exit: vcpu 0, reason EXTERNAL_INTERRUPT (1)
kvm_entry: vcpu 0, rip 0xfffffff0
kvm_exit: vcpu 0, reason CPUID (10)
kvm_userspace_exit: reason KVM_EXIT_IO
...

# VM 销毁 (通过 function trace 捕获)
kvm_vm_release: freeing VM
```

### VM-Exit 原因分布（典型值）

```
  15234 EXTERNAL_INTERRUPT    (外部中断)
   8456 PENDING_VIRT_INTR    (虚拟中断待处理)
   3211 EPT_VIOLATION         (EPT 缺页)
   1024 CPUID                (CPUID 指令)
    512 HLT                  (HLT 指令)
    256 IO_INSTRUCTION       (I/O 指令)
     32 MSR_WRITE           (MSR 写入)
```

---

## 🔍 深入分析

### 分析 VM-Exit 的时间分布

```bash
# 计算两次 VM-Exit 之间的间隔
cat /sys/kernel/debug/tracing/trace | grep "kvm_exit" | \
    awk '{
        split($3, t, ":");
        ts = t[1]*3600 + t[2]*60 + t[3];
        if (prev > 0) printf "%.3f us\n", (ts-prev)*1000000;
        prev = ts
    }' | head -50
```

### 分析 vCPU 的 host 时间片

```bash
# 使用 perf 分析 vCPU 线程的调度
perf sched record -- sleep 10
perf sched map
perf sched latency
```

---

## 📝 报告要求

1. 记录 VM 从创建到销毁的完整事件序列
2. 统计 10 秒内各种 VM-Exit 的数量和占比
3. 分析 VM-Exit 的时间间隔分布
4. 画出 vCPU 的 Host/Guest 切换时序图
5. 将关键事件映射到 KVM 源码中的函数

---

## 💡 扩展练习

- 对比不同 CPU 模型（host vs 固定模型）的 VM-Exit 分布
- 测试嵌套虚拟化下的 VM-Exit 差异
- 分析多 vCPU 场景下的 VM-Exit 分布
- 使用 KVM_STAT 接口（`/sys/kernel/debug/kvm/`）获取统计数据
