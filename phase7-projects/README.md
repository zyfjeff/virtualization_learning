# 第六阶段：实践项目总览

> 基于 Linux 6.12.93 内核源码 | 综合实践阶段

---

## 📋 阶段概述

前六个阶段覆盖了 KVM 的核心子系统：CPU 虚拟化、内存虚拟化、中断虚拟化、
Virtio 设备虚拟化、VFIO 设备直通和时钟虚拟化。本阶段通过 **7 个实践项目** 将这些知识融会贯通，
每个项目都要求你动手操作、观察现象、分析数据。

---

## 🗂️ 项目列表

### 项目 1：VM 生命周期跟踪

📄 [vm-lifecycle-trace.md](vm-lifecycle-trace.md)

**目标**：使用 ftrace 和 perf 完整跟踪一个虚拟机的生命周期，从创建到销毁。

**核心技能**：
- ftrace function tracer 和 tracepoint 的使用
- 识别 KVM vCPU 创建、调度、退出、销毁的关键函数
- 分析 VM-Exit 原因分布
- 理解 KVM 内部的事件模型

**预计时间**：2-3 天

---

### 项目 2：EPT 性能分析

📄 [ept-performance.md](ept-performance.md)

**目标**：分析 EPT（扩展页表）在真实工作负载下的性能特征。

**核心技能**：
- EPT 缺页处理延迟测量
- 大页（2MB/1GB）vs 4K 页的性能对比
- 脏页日志（Dirty Logging）的性能影响
- EPT 指针缓存（EPTP Switching）的效果

**预计时间**：3-4 天

---

### 项目 3：Virtio 性能与行为分析

📄 [virtio-analysis.md](virtio-analysis.md)

**目标**：深入分析 virtio 设备的 I/O 路径，对比 QEMU 与 vhost 的性能。

**核心技能**：
- Virtio 设备 feature 协商验证
- vring 数据流追踪（desc/avail/used）
- QEMU 用户空间后端 vs vhost 内核后端性能对比
- virtio-net/virtio-blk 的 VM-Exit 分布分析
- TSO/多队列/event_idx 等功能对性能的影响

**预计时间**：2-3 天

---

### 项目 4：VFIO 延迟分析

📄 [vfio-latency.md](vfio-latency.md)

**目标**：分析 VFIO 设备直通的延迟特征和瓶颈。

**核心技能**：
- DMA 映射/解映射延迟测量
- IOMMU 翻译开销分析
- MMIO 访问延迟测量
- 中断投递延迟（传统模式 vs PI 模式）

**预计时间**：3-4 天

---

### 项目 5：中断路径跟踪

📄 [irq-path-trace.md](irq-path-trace.md)

**目标**：完整跟踪一个外部设备中断从硬件到 Guest 的路径。

**核心技能**：
- 中断注入延迟测量
- Posted Interrupt 效果验证
- APICv 功能分析
- 中断路由表的构建和理解

**预计时间**：2-3 天

---

### 项目 6：时钟虚拟化性能与精度分析

📄 [timer-performance.md](timer-performance.md)

**目标**：对比不同时钟源的性能、精度和稳定性，深入理解 KVM 时钟虚拟化。

**核心技能**：
- 时钟源切换与读取延迟测量（kvm-clock / TSC / HPET / PIT）
- cyclictest 定时器精度测试
- TSC 多CPU同步稳定性检测
- Timer Advance 优化效果验证
- 时钟源选择决策分析

**预计时间**：2-3 天

---

### 项目 7：CPU 虚拟化深度分析

📄 [cpu-virtualization.md](cpu-virtualization.md)

**目标**：通过实验深入理解 CPUID、MSR、指令虚拟化的实际行为。

**核心技能**：
- CPUID 对比分析（Host vs Guest，半虚拟化叶）
- MSR 拦截追踪与频率统计
- 指令 VM-Exit 分布分析
- 半虚拟化特性（kvmclock / PV EOI / PV TLB flush）验证
- MSR Bitmap 优化分析

**预计时间**：2-3 天

---

## 🔧 环境准备

### 硬件要求

| 项目 | 最低要求 | 推荐配置 |
|------|----------|----------|
| CPU | Intel 6代+ (支持 VMX/EPT) | Intel 10代+ (支持 APICv/PI) |
| 内存 | 16 GB | 32 GB+ |
| 磁盘 | 50 GB 可用空间 | 100 GB+ SSD |
| IOMMU | 支持 VT-d/AMD-Vi | VT-d + ACS |
| 网卡 | 用于 VFIO 测试 | Intel X520/X710 |

### 软件要求

```bash
# 1. 内核要求
uname -r
# 推荐: 6.x 或更新版本（本项目基于 6.12.93）

# 2. 安装必要工具
apt install linux-tools-common linux-tools-generic
apt install trace-cmd kernelshark
apt install qemu-system-x86
apt install stress-ng
apt install iperf3
apt install hwloc numactl
apt install pciutils
apt install gdb

# 3. 加载 KVM 模块
modprobe kvm
modprobe kvm_intel nested=1  # 如果测试嵌套虚拟化
modprobe vfio
modprobe vfio-pci
modprobe vfio_iommu_type1

# 4. 确保 IOMMU 启用
cat /proc/cmdline | grep -E "intel_iommu|amd_iommu"
# 如果没有: 在 GRUB 中添加 intel_iommu=on iommu=pt
```

### 调试环境配置

```bash
# 1. 挂载 debugfs 和 tracefs
mount -t debugfs none /sys/kernel/debug
mount -t tracefs none /sys/kernel/tracing

# 2. 检查 KVM tracepoint
ls /sys/kernel/debug/tracing/events/kvm/

# 3. 设置 ftrace 权限
echo 1 > /proc/sys/kernel/perf_event_paranoid  # 如果 > 1

# 4. 启用 KVM 调试接口（如果需要）
# CONFIG_KVM_EXTERNAL_DEBUGGER=y (内核配置)
```

---

## 📊 通用分析方法

### ftrace 基础

```bash
# 设置 function tracer
echo function > /sys/kernel/debug/tracing/current_tracer
echo kvm_vcpu_run > /sys/kernel/debug/tracing/set_ftrace_filter
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 设置 tracepoint
echo kvm:kvm_entry > /sys/kernel/debug/tracing/set_event
echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 收集数据
trace-cmd record -e kvm:kvm_entry -e kvm:kvm_exit -e kvm:kvm_page_fault
trace-cmd report
```

### perf 基础

```bash
# KVM 统计
perf kvm stat live
perf kvm stat record -- sleep 30
perf kvm stat report

# 热点分析
perf record -g -a -- sleep 30
perf report

# 特定事件
perf record -e kvm:kvm_page_fault -a -- sleep 10
```

### 数据可视化

```bash
# 使用 kernelshark 可视化
trace-cmd record -e all
kernelshark trace.dat

# 使用火焰图
perf record -g -a -- sleep 10
perf script | stackcollapse-perf.pl | flamegraph.pl > kvm.svg
```

---

## 📝 项目报告模板

每个项目完成后，建议编写简要报告，包含：

1. **实验环境**：硬件型号、内核版本、VM 配置
2. **测试方法**：使用的工具、参数、工作负载
3. **关键发现**：性能数据、延迟分布、瓶颈分析
4. **源码对应**：将观察到的行为映射到源码中的具体函数
5. **改进思考**：有哪些可以优化的地方

---

## 🎯 完成标准

完成所有 4 个项目后，你应该能够：
- [ ] 独立完成 KVM 性能分析的全套工作流
- [ ] 将观察到的性能数据与源码中的函数对应
- [ ] 识别 KVM 子系统的性能瓶颈并提出优化方向
- [ ] 使用 ftrace/perf 诊断虚拟机相关问题
- [ ] 撰写清晰的技术分析报告
