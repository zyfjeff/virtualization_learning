# KVM 深度学习项目 - VMM专家版

> **面向用户态VMM专家**，深入理解KVM内核态实现
>
> 基于 Linux 6.12.93 内核源码的 KVM 虚拟化技术深度学习项目

## 🎯 课程定位

本课程专为**用户态VMM专家**设计，帮助你深入理解KVM内核态实现。

**你已熟悉的**：
- KVM API (ioctl接口)
- Virtio架构和vring协议
- 基本的虚拟化概念 (VM-Exit, VMCS)
- 用户态VMM实现 (QEMU/crosvm)

**本课程聚焦**：
- KVM框架层核心代码 (`kvm_main.c`, `x86.c`)
- 内核态VM-Exit处理 (快速路径 vs 慢速路径)
- EPT页表管理 (并发、大页、脏页跟踪)
- 中断虚拟化 (Posted Interrupts零VM-Exit)
- vhost内核态加速 (数据面卸载)
- 性能优化技术 (halt-polling, VPID, APICv)

## 环境信息

- **源码目录**: `/root/code/linux-6.12.93`
- **CPU特性**: VMX, EPT, VPID, EPT_AD, Posted Interrupts, APICv, FlexPriority
- **工具链**: ftrace, BPF/bpftrace, perf

## 项目结构

```
kvm-study/
├── README.md                    ← 本文件
├── notes/                       ← 学习笔记汇总
│   ├── source-navigation.md     ← 源码导航图
│   └── debugging-guide.md       ← ★ KVM调试实战指南 (新增!)
├── phase0-kvm-framework/        ← ★ 第零阶段：KVM框架层深度解析 (新增!)
│   ├── README.md                ← 框架层核心概念
│   ├── annotations.md           ← kvm_main.c/x86.c源码注释
│   └── kvm-framework.md         ← VMM视角对比分析
├── phase1-vtx-basics/           ← 第一阶段：VT-x + CPU虚拟化
│   ├── README.md                ← 学习指南 + VMM对比 + 性能优化 + 常见陷阱
│   ├── annotations.md           ← VT-x源码精读注释
│   └── cpu-virtualization.md    ← ★ CPUID/MSR/指令虚拟化
├── phase2-mem-virt/             ← 第二阶段：内存虚拟化(EPT/TDP MMU)
│   ├── README.md                ← VMM对比 + 大页/脏页/MMIO缓存优化
│   └── annotations.md
├── phase3-interrupts/           ← 第三阶段：中断虚拟化 + VT-d中断重映射
│   ├── README.md                ← VMM对比 + APICv/PI/中断合并优化
│   └── annotations.md           ← pi_desc + IRTE + PIR→IRR + PI调度
├── phase4-virtio/               ← ★ 第四阶段：vhost内核态加速 (重构!)
│   ├── README.md                ← 聚焦vhost，删除用户态virtio细节
│   └── annotations.md           ← vhost源码注释
├── phase5-vfio/                 ← 第五阶段：VFIO设备直通
│   ├── README.md                ← VMM对比 + IOTLB/DMA批处理优化
│   └── annotations.md
├── phase6-timer-virt/           ← 第六阶段：时钟虚拟化
│   ├── README.md                ← VMM对比 + TSC-deadline/kvmclock优化
│   └── annotations.md           ← 源码级注释
├── phase7-projects/             ← 第七阶段：综合实践项目
│   ├── vm-lifecycle-trace.md    ← 项目1: VM生命周期
│   ├── ept-performance.md       ← 项目2: EPT性能
│   ├── virtio-analysis.md       ← ★ 项目3: vhost性能分析
│   ├── vfio-latency.md          ← 项目4: VFIO延迟
│   ├── irq-path-trace.md        ← 项目5: 中断路径
│   ├── timer-performance.md     ← 项目6: 时钟性能
│   └── cpu-virtualization.md    ← 项目7: CPU虚拟化
├── examples/                    ← 可运行示例代码 (★ 重点!)
│   ├── kvm-api-demo/            ← KVM API用户空间演示 (C语言)
│   │   ├── kvm-demo.c           ← ★ 完整VM生命周期 (make && ./kvm-demo)
│   │   ├── kvm-demo-regs.c      ← ★ 寄存器操作演示 (make && ./kvm-demo-regs)
│   │   ├── Makefile
│   │   └── README.md
│   ├── minimal-vmx/             ← 最小VMX内核模块
│   │   ├── vmx-info/            ← ★ VMX能力检测 (安全,不会崩溃)
│   │   │   └── vmx-info.c
│   │   ├── vmx-demo.c           ← VMXON/VMCS配置演示
│   │   ├── Makefile
│   │   └── README.md
│   └── bpf-programs/            ← BPF/bpftrace追踪程序集
│       ├── trace-vmexit.c       ← VM-Exit追踪 (BCC)
│       ├── trace-ept-faults.bpf ← EPT页错误追踪
│       ├── trace-irq-latency.bpf← 中断延迟测量
│       ├── trace-vfio-dma.bpf   ← VFIO DMA追踪
│       ├── kvm-overview.bpf     ← KVM综合概览
│       └── README.md
└── scripts/                     ← 实践脚本
    ├── ftrace/                  ← ftrace 脚本集 (trace-vmexit.sh等)
    └── perf/                    ← perf 脚本集 (kvm-overview.sh等)
```

## 学习方法论

> 面向VMM专家的优化学习路径：
> **VMM对比 → 内核态深入 → 性能优化 → 实战调试**

每个阶段的学习流程：
1. **VMM对比** (30分钟): 理解用户态VMM vs KVM内核态的差异
2. **源码** (1-2小时): 按 `annotations.md` 注释精读关键函数
3. **性能优化** (30分钟): 学习KVM特有的性能优化技术
4. **追踪** (30分钟): 使用 ftrace/BPF 观察实际运行中的代码路径
5. **实践** (1小时): 运行 `examples/` 中的示例代码验证理解
6. **调试** (30分钟): 参考 `notes/debugging-guide.md` 排查问题

### 可运行示例快速入门

```bash
# ★ 示例1: KVM API演示 - 从用户空间创建和运行虚拟机
cd examples/kvm-api-demo/
make && ./kvm-demo

# ★ 示例2: VMX能力检测 - 安全查看CPU虚拟化特性
cd examples/minimal-vmx/vmx-info/
make && sudo insmod vmx-info.ko
dmesg | tail -20

# ★ 示例3: BPF追踪 - 实时观察VM-Exit
sudo bpftrace examples/bpf-programs/trace-vmexit.bpf
```

## 学习路线图

| 阶段 | 主题 | 预计时间 | 核心源码 | VMM对比 | 可运行示例 |
|------|------|---------|---------|---------|-----------|
| 0 | ★ KVM框架层 | 1-2周 | kvm_main.c, x86.c | ✓ | kvm-demo |
| 1 | VT-x + CPU虚拟化 | 1-2周 | vmx.c, cpuid.c, x86.c | ✓ | vmx-info.ko |
| 2 | 内存虚拟化(EPT) | 2-3周 | mmu.c, tdp_mmu.c, spte.c | ✓ | trace-ept-faults.bpf |
| 3 | 中断虚拟化 + VT-d IR | 2-3周 | lapic.c, posted_intr.c, irq_remapping.c | ✓ | trace-irq-latency.bpf |
| 4 | ★ vhost内核态加速 | 1周 | vhost.c, net.c | ✓ | iperf3+vhost |
| 5 | VFIO设备直通 | 2-3周 | vfio_main.c, vfio_pci_core.c | ✓ | trace-vfio-dma.bpf |
| 6 | 时钟虚拟化 | 1周 | i8254.c, lapic.c(timer), x86.c(kvmclock) | ✓ | cyclictest对比 |
| 7 | 综合实践 | 持续 | 全部 | - | 7个综合项目 |

**新增特性**：
- ✓ 每章包含VMM视角对比，帮助理解KVM设计决策
- ✓ 每章包含性能优化技术，指导实际调优
- ✓ 每章包含常见陷阱，避免踩坑
- ✓ 配套KVM调试实战指南 (`notes/debugging-guide.md`)

## 快速开始

```bash
# 1. 检查CPU虚拟化支持
cat /proc/cpuinfo | grep -E "vmx|ept|vpid"

# 2. 查看KVM模块参数
for f in /sys/module/kvm_intel/parameters/*; do
    echo "$(basename $f) = $(cat $f 2>/dev/null)"
done

# 3. 开始第一阶段学习
cd phase1-vtx-basics/
cat README.md

# 4. 运行第一个示例
cd ../examples/kvm-api-demo/
make && sudo ./kvm-demo
```

## 参考资料

- [Hypervisor 101 in Rust](https://tandasat.github.io/Hypervisor-101-in-Rust/) - 优秀的Hypervisor入门教程
- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) - Intel处理器手册
- [KVM Forum](https://kvmforum.de/) - KVM开发者会议
- Linux 6.12.93 内核源码 (`/root/code/linux-6.12.93`)

## 验证清单

每阶段完成后确认能回答：
- [ ] 数据从用户空间ioctl到硬件执行的完整路径
- [ ] 关键数据结构的内存布局和作用
- [ ] 硬件特性在VMCS中的配置方式
- [ ] 错误/异常的处理流程
- [ ] 性能关键路径和优化点
- [ ] 能否编写/修改示例代码来验证自己的理解
