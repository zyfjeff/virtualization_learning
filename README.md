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
- ★ KVM性能优化技术 (halt-polling, VPID, APICv, PLE)
- ★ KVM调试与测试 (ftrace, perf kvm stat, selftests, bpftrace)
- ★ MicroVM架构专项 (启动路径, 最小设备模型, guest_memfd, 安全模型)

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
├── phase8-performance/          ← ★ 第八阶段：KVM性能优化深入 (新增!)
│   ├── README.md                ← 性能优化概览 + 调优参数
│   └── annotations.md           ← halt-polling/VPID/APICv/PLE源码注释
├── phase9-debugging/            ← ★ 第九阶段：KVM调试与测试 (新增!)
│   ├── README.md                ← 调试场景速查 + 决策树
│   └── annotations.md           ← trace events目录 + selftests + bpftrace
├── phase10-microvm/             ← ★ 第十阶段：MicroVM架构专项 (新增!)
│   ├── README.md                ← MicroVM技术栈全景
│   └── annotations.md           ← 启动路径/设备模型/安全模型/guest_memfd
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
├── mini-kvm/                  ← ★★ 实战项目: 简化版 KVM 实现 (新增!)
│   ├── README.md                ← 项目说明 + 学习路径
│   ├── mini-kvm.c               ← 内核模块主代码 (725 行)
│   ├── mini-kvm.h               ← 内部头文件 (数据结构 + VMCS 编码)
│   ├── test-mini-kvm.c          ← 用户空间测试程序
│   ├── Makefile                 ← 构建脚本
│   └── stages/                  ← 分阶段学习指南
│       ├── stage1-vmx.md        ← Stage 1: VMX 基础 (对应 Phase 1)
│       ├── stage2-ept.md        ← Stage 2: EPT 内存虚拟化 (对应 Phase 2)
│       ├── stage3-interrupt.md  ← Stage 3: 中断注入 (对应 Phase 3)
│       ├── stage4-device.md     ← Stage 4: 设备模拟 (对应 Phase 4-5)
│       └── stage5-runloop.md    ← Stage 5: 运行循环 (对应 Phase 0, 8)
└── scripts/                     ← 实践脚本
    ├── testing/                 ← ★ 统一测试环境 (新增!)
    │   ├── build-rootfs-ubuntu.sh    ← Ubuntu rootfs 构建（推荐）
    │   ├── build-rootfs-allinone.sh  ← All-in-One rootfs 构建
    │   ├── boot-vm-unified.sh        ← 统一 VM 启动脚本
    │   ├── README-UNIFIED.md         ← 统一测试环境使用指南
    │   └── MIGRATION-GUIDE.md        ← 迁移指南
    ├── ftrace/                  ← ftrace 脚本集 (trace-vmexit.sh等)
    └── perf/                    ← perf 脚本集 (kvm-overview.sh等)
```

## 统一测试环境

> ★ 推荐使用统一的测试环境构建和启动脚本

### 快速开始

```bash
cd scripts/testing

# 1. 编译内核（如果还没有）
./build-kernel.sh

# 2. 构建 Ubuntu rootfs（推荐，包含所有测试工具）
sudo ./build-rootfs-ubuntu.sh

# 3. 启动 VM（统一启动脚本）
./boot-vm-unified.sh ubuntu --memory 4G --cpus 4 --queues 4
```

### 特性

- ✅ **统一构建**：所有实验使用同一个基础镜像
- ✅ **预装工具**：iperf3, ethtool, perf, bpftrace, stress-ng 等
- ✅ **灵活配置**：支持内存、CPU、队列数、网络类型等配置
- ✅ **快捷命令**：run-network-test, run-stress-test, tune-virtio

### 详细文档

- [统一测试环境使用指南](scripts/testing/README-UNIFIED.md)
- [迁移指南](scripts/testing/MIGRATION-GUIDE.md)

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
| 8 | ★ KVM性能优化深入 | 1-2周 | kvm_main.c(halt_poll), vmx.c(PLE) | ✓ | perf kvm stat |
| 9 | ★ KVM调试与测试 | 1周 | trace.h, selftests/ | - | bpftrace 脚本集 |
| 10 | ★ MicroVM架构专项 | 1-2周 | guest_memfd.c, nested.c | ✓ | Firecracker/Cloud Hypervisor |

**新增特性**：
- ✓ 每章包含VMM视角对比，帮助理解KVM设计决策
- ✓ 每章包含性能优化技术，指导实际调优
- ✓ 每章包含常见陷阱，避免踩坑
- ✓ ★ 性能优化专题 (phase8): halt-polling/PLE/VPID 源码级分析
- ✓ ★ 调试与测试专题 (phase9): 完整 trace events 目录 + selftests + bpftrace
- ✓ ★ MicroVM架构专项 (phase10): guest_memfd/jailer/启动路径优化
- ✓ 配套KVM调试实战指南 (`notes/debugging-guide.md`)
- ✓ 所有 trace events 和函数行号均基于 6.12.93 实际源码验证

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
