# Phase 0 实践练习

> KVM 框架层实践练习

---

## 练习列表

| 编号 | 练习名称 | 需要 VM | 难度 | 预计时间 | 核心知识点 |
|------|---------|---------|------|---------|-----------|
| 1 | 跟踪 VM 生命周期 | 是 | ★☆☆ | 15min | KVM_RUN 调用链 |
| 2 | 分析 vCPU 调度 | 是 | ★★☆ | 20min | vCPU 线程调度 |
| 3 | 调试 memslot | 是 | ★★☆ | 20min | 内存 slot 管理 |
| 4 | 性能对比 | 是 | ★★★ | 30min | 用户态 vs 内核态 |

---

## 快速开始

```bash
# 所有练习都需要运行 VM
# 使用统一的 VM 启动脚本
sudo bash /root/code/kvm-study/scripts/setup-vm.sh start

# 在另一个终端运行练习
# 练习 1: 跟踪 VM 生命周期
sudo bash ex1-vm-lifecycle.sh

# 练习 2: 分析 vCPU 调度
sudo bash ex2-vcpu-sched.sh

# 练习 3: 调试 memslot
sudo bash ex3-memslot.sh

# 练习 4: 性能对比
sudo bash ex4-perf-compare.sh

# 清理
sudo bash /root/code/kvm-study/scripts/setup-vm.sh stop
```

---

## 练习详情

### 练习 1: 跟踪 VM 生命周期

**目标**: 理解从 ioctl(KVM_RUN) 到 VMENTER 的完整调用链

**方法**:
```bash
# 使用 ftrace 跟踪 KVM_RUN
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable

# 运行 VM 并观察 trace
cat /sys/kernel/debug/tracing/trace_pipe
```

**预期输出**:
```
kvm_entry: vcpu 0
kvm_exit: reason 0 vcpu 0
kvm_entry: vcpu 0
...
```

---

### 练习 2: 分析 vCPU 调度

**目标**: 理解 vCPU 线程的调度行为

**方法**:
```bash
# 使用 perf 分析 vCPU 线程
perf record -g -p $(pgrep qemu-system-x86) sleep 10
perf report
```

**关注点**:
- vCPU 线程的 CPU 占用率
- 上下文切换频率
- 内核态 vs 用户态时间比例

---

### 练习 3: 调试 memslot

**目标**: 理解内存 slot 的管理机制

**方法**:
```bash
# 查看 VM 的 memslot 信息
cat /sys/kernel/debug/kvm/<vm_id>/memslots

# 使用 gdb 附加到 QEMU 进程
gdb -p $(pgrep qemu-system-x86)
(gdb) p kvm->memslots
```

**关注点**:
- memslot 的数量和大小
- GPA 到 HVA 的映射关系
- 内存区域的类型（RAM、MMIO）

---

### 练习 4: 性能对比

**目标**: 对比用户态 VMM 和 KVM 内核态的性能差异

**方法**:
```bash
# 测试 KVM 的 VM-Exit 处理性能
perf stat -e kvm:kvm_exit -e kvm:kvm_entry sleep 10

# 对比不同场景的 VM-Exit 次数
# 场景 1: 空闲 VM
# 场景 2: CPU 密集型 VM
# 场景 3: IO 密集型 VM
```

**关注点**:
- VM-Exit 的频率
- 不同退出原因的比例
- 性能优化的效果

---

## 统一测试环境

所有练习使用统一的 VM 启动脚本：

- **启动脚本**: `/root/code/kvm-study/scripts/setup-vm.sh`
- **功能**: 自动构建内核和 rootfs，启动 VM
- **详细说明**: 参见 `/root/code/kvm-study/scripts/testing/README-UNIFIED.md`

---

## 故障排查

### 问题 1: 无法访问 debugfs

```bash
# 挂载 debugfs
sudo mount -t debugfs none /sys/kernel/debug
```

### 问题 2: perf 无法attach到进程

```bash
# 调整 perf_event_paranoid 设置
echo -1 > /proc/sys/kernel/perf_event_paranoid
```

### 问题 3: 无法找到 KVM 调试信息

```bash
# 确保 KVM 模块已加载
lsmod | grep kvm

# 确保 debugfs 已挂载
ls /sys/kernel/debug/kvm/
```

---

## 参考资料

- Phase 0 README: `/root/code/kvm-study/phase0-kvm-framework/README.md`
- KVM 源码: `/root/code/linux-6.12.93/arch/x86/kvm/`
- KVM 文档: `/root/code/linux-6.12.93/Documentation/virt/kvm/`
