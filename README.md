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
- ★ KVM性能优化技术：PLE 与超卖自救 (phase9) / halt-polling (phase0) / VPID (phase1) / APICv (phase4)
- ★ 性能测量方法论：观测者扰动预算 + 跨 phase 结论索引 (phase9)
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
├── phase0-kvm-framework/        ← 第零阶段：KVM框架层深度解析
│   ├── README.md                ← 框架层核心概念 + 本章文件导航
│   ├── annotations.md           ← kvm_dev_ioctl→vcpu_enter_guest 全链路注释
│   ├── kvm-framework.md         ← VMM视角对比分析
│   └── practice/                ← 实战练习（手工步骤形式）
├── phase1-vtx-basics/           ← 第一阶段：VT-x + CPU虚拟化
│   ├── README.md                ← 学习指南 + VMM对比 + 性能优化 + 常见陷阱
│   ├── annotations.md           ← VT-x源码精读注释
│   ├── cpu-virtualization.md    ← ★ CPUID/MSR/指令虚拟化
│   └── practice/                ← 4 个 C 练习程序（make 构建）
├── phase2-mem-virt/             ← 第二阶段：内存虚拟化(EPT/TDP MMU)
│   ├── README.md                ← EPT原理 + SPTE格式 + 本章文件导航
│   ├── annotations.md           ← SPTE/缺页/TDP MMU 源码注释
│   ├── ept-violation-handling.md← ★ EPT Violation 完整处理流程
│   ├── tdp-mmu-concurrency.md   ← ★ TDP MMU 并发模型
│   ├── mmio-identification.md   ← ★ MMIO 识别与 IPAT（含核查报告）
│   ├── practice/                ← 2 个 C 练习程序（make 构建）
│   └── archive/                 ← 已归档的过程性文档
├── phase3-iommu/                ← 第三阶段：IOMMU 层（phase6 VFIO 的地基）
│   ├── README.md                ← 框架 + 概念 + 主流程 + 8 问清单
│   ├── translation/…/backends.md← 8 篇问题深入文档
│   ├── annotations.md           ← 源码注释
│   ├── corrections.md           ← 勘误
│   └── practice/                ← 4 个实验 + 引用核查脚本
├── phase4-interrupts/           ← 第四阶段：中断虚拟化 + VT-d中断重映射
│   ├── README.md                ← 技术全景 + 中断路径 + 数据结构速览
│   ├── annotations.md           ← pi_desc + IRTE + PIR→IRR + PI调度
│   ├── posted-interrupts.md     ← ★ Posted 模式系统深入
│   ├── msi-affinity-migration.md← ★ MSI 地址格式与亲和性迁移
│   └── practice/                ← 6 个实验脚本 + PI 演示内核模块
├── phase5-virtio/               ← 第五阶段：virtio / vhost / vhost-user
│   ├── README.md                ← 为什么需要vhost + 源码路线 + 本章文件导航
│   ├── annotations.md           ← vhost源码注释
│   ├── virtio-queue.md          ← ★ Virtqueue 深度解析
│   ├── vhost-architecture.md    ← ★ vhost 架构与数据结构
│   ├── vhost-net-datapath.md    ← ★ vhost-net 数据路径
│   ├── vhost-user-basics.md     ← ★ vhost-user 入门
│   ├── vhost-user-protocol-latest.md        ← 协议字段速查
│   ├── vhost-user-new-features-factcheck-v2.md ← 新特性核查(QEMU 11.1.0+DPDK)
│   ├── vhost-user-new-features-usecases.md  ← 新特性使用场景
│   ├── practice/                ← 全部练习与实测数据
│   └── archive/                 ← 已被取代的过程性文档
├── phase6-vfio/                 ← 第六阶段：VFIO设备直通
│   ├── README.md                ← VMM对比 + ACS/ATS + IOTLB/DMA批处理优化
│   ├── corrections.md           ← 勘误
│   └── practice/                ← VFIO 认领/DMA映射/MSI-X 实测练习
├── phase7-timer-virt/           ← 第七阶段：时钟虚拟化
│   ├── README.md                ← VMM对比 + TSC-deadline/kvmclock优化 + 概念区分
│   ├── annotations.md           ← 源码级注释
│   ├── corrections.md           ← 勘误
│   └── practice/                ← 3 个可运行实验（TSC/kvmclock/LAPIC Timer）
├── phase8-capstone/             ← ★ 第八阶段：毕业建造——最小 VMM
│   ├── README.md                ← 定位 + 项目阶梯 + 验收标准
│   ├── project1-minivmm-boot.md ← ★ 可启动最小 VMM（bzImage 引导）
│   ├── project2-minivmm-virtio.md← 自制 virtio-mmio 设备
│   ├── project3-minivmm-vfio.md ← VFIO 直通进自己的 VMM
│   └── project4-minivmm-bench.md← 与 QEMU/Firecracker 性能对标
├── phase9-performance/          ← 第九阶段：性能测量方法论 + 独占机制 + 结论索引
│   ├── README.md                ← 本章定位 + 文件清单 + 三条硬性规则
│   ├── measurement.md           ← 测量纪律：重复/噪声/分辨率/观测者扰动/开跑前自检
│   ├── parameters.md            ← ★ 参数默认值与权限的唯一来源（含"能不能运行时改"）
│   ├── annotations.md           ← 三块独占机制：PLE与定向让出 / EPT粒度与PML / 主时钟
│   ├── index.md                 ← 跨 phase 性能结论索引（A/B/C/D 可信度分级）
│   ├── corrections.md           ← 本章勘误
│   └── practice/                ← E1–E5 五个实验（md + 可直接跑的 bench-*.sh）
├── phase10-debugging/           ← 第十阶段：KVM调试与测试
│   ├── README.md                ← 调试场景速查 + 决策树
│   └── annotations.md           ← trace events目录 + selftests + bpftrace
├── phase11-microvm/             ← 第十一阶段：MicroVM架构专项
│   ├── README.md                ← MicroVM技术栈全景
│   └── annotations.md           ← 启动路径/设备模型/安全模型/guest_memfd
├── examples/                    ← 可运行示例代码 (★ 重点!)
│   ├── kvm-api-demo/            ← KVM API用户空间演示 (C语言)
│   │   ├── kvm-demo.c           ← ★ 完整VM生命周期 (make && ./kvm-demo)
│   │   ├── kvm-demo-regs.c      ← ★ 寄存器操作演示 (make && ./kvm-demo-regs)
│   │   ├── Makefile
│   │   └── README.md
│   ├── mini-kvm/                ← ★★ 实战项目: 简化版 KVM 内核模块实现
│   │   ├── mini-kvm.c           ← 内核模块主代码
│   │   ├── test-mini-kvm.c      ← 用户空间测试程序
│   │   ├── Makefile
│   │   └── stages/              ← 分阶段学习指南 (stage1-5)
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
└── scripts/                     ← 构建并启动实验 VM
    ├── README.md                ← ★ 实验环境唯一入口文档
    ├── vm/                      ← 构建与启动
    │   ├── build-kernel.sh          ← 内核编译
    │   ├── build-rootfs-ubuntu.sh   ← Ubuntu rootfs（推荐）
    │   ├── build-rootfs-allinone.sh ← busybox + 宿主工具
    │   ├── build-rootfs-minimal.sh  ← 最小 busybox initramfs
    │   ├── boot-vm.sh               ← VM 启动（默认启用 KVM）
    │   └── setup-vfio-vm.sh         ← VFIO 设备直通 VM
    ├── trace/                   ← 宿主侧观测 (trace-vmexit.sh, kvm-overview.sh 等)
    ├── images/                  ← 构建产物（已 gitignore）
    ├── shared/                  ← 9p 共享暂存区 → guest /mnt/shared
    └── archive/                 ← 已弃用脚本与历史文档
```

## 实验 VM 环境

### 快速开始

```bash
cd scripts/vm

# 1. 编译内核（如果还没有）
./build-kernel.sh

# 2. 构建 Ubuntu rootfs（推荐，包含所有测试工具）
sudo ./build-rootfs-ubuntu.sh

# 3. 启动 VM
./boot-vm.sh ubuntu --memory 4G --cpus 4 --queues 4
```

### 特性

- ✅ **默认启用 KVM**：`-enable-kvm -cpu host`，宿主侧 `kvm:*` tracepoint 才有事件、guest 内才能看到 VMX
- ✅ **统一构建**：所有实验使用同一个基础镜像
- ✅ **预装工具**：iperf3, ethtool, perf, bpftrace, stress-ng 等
- ✅ **灵活配置**：内存、CPU、队列数、网络类型均可调，`--tcg` 可回退纯软件模拟
- ✅ **9p 共享**：`scripts/shared/` 直通 guest `/mnt/shared`，改完立即生效

### 详细文档

- [实验 VM 环境指南](scripts/README.md)

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
| 3 | ★ IOMMU 层（phase6 的地基） | 2-3周 | iommu.c, intel/iommu.c, dma-iommu.c | ✓ | iommu-analysis.sh |
| 4 | 中断虚拟化 + VT-d IR | 2-3周 | lapic.c, posted_intr.c, irq_remapping.c | ✓ | trace-irq-latency.bpf |
| 5 | ★ vhost内核态加速 | 1周 | vhost.c, net.c | ✓ | iperf3+vhost |
| 6 | VFIO设备直通 | 2-3周 | vfio_main.c, vfio_pci_core.c | ✓ | trace-vfio-dma.bpf |
| 7 | 时钟虚拟化 | 1周 | i8254.c, lapic.c(timer), x86.c(kvmclock) | ✓ | 3 个 practice 实验 |
| 8 | ★ 毕业建造：最小 VMM | 持续 | 全部（KVM API 综合） | - | kvm-api-demo 起步 |
| 9 | ★ 性能测量方法论 + 独占机制 + 结论索引 | 1-2周 | vmx.c(PLE), mmu.c(大页/PML), x86.c(主时钟), kvm_main.c(directed yield) | ✓ | bench-*.sh（E1–E5） |
| 10 | ★ KVM调试与测试 | 1周 | trace.h, selftests/ | - | bpftrace 脚本集 |
| 11 | ★ MicroVM架构专项 | 1-2周 | guest_memfd.c, nested.c | ✓ | Firecracker/Cloud Hypervisor |

> 建议阅读顺序（子系统间依赖）：phase2 → **phase3(IOMMU)** → phase4 → phase6；
> phase3 同时是 phase6(VFIO) 的地基。phase8 毕业建造建议放在 0-7 之后。

**新增特性**：
- ✓ 每章包含VMM视角对比，帮助理解KVM设计决策
- ✓ 每章包含性能优化技术，指导实际调优；**实测结论统一收在 phase9/index.md**，
  按可信度分级，其他章节只留指针、不复制数字
- ✓ 每章包含常见陷阱，避免踩坑
- ✓ ★ IOMMU 专题 (phase3)：与 EPT 同一心智模型，8 问深入 + 三后端对照
- ✓ ★ 毕业建造 (phase8)：从裸 KVM API 写可启动最小 VMM，逐级加 virtio/VFIO/性能对标
- ✓ ★ 性能专题 (phase9): 测量纪律与观测者扰动预算 + 三块独占机制源码走读
  （PLE 与定向让出 / EPT 粒度与 PML / 主时钟与 TSC offset）+ E1–E5 可跑实验
  + 参数默认值唯一来源 + 跨章结论索引
- ✓ ★ 调试与测试专题 (phase10): 完整 trace events 目录 + selftests + bpftrace
- ✓ ★ MicroVM架构专项 (phase11): guest_memfd/jailer/启动路径优化
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
