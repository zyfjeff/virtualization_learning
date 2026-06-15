# BPF 追踪程序 - KVM 深度学习项目

本目录包含用于追踪和分析 KVM 虚拟化性能的 BPF (Berkeley Packet Filter) 程序。
每个程序都配有中文注释, 详细解释 KVM 内核机制, 并提供 bpftrace 和 ftrace 等效命令。

## 目录结构

```
bpf-programs/
├── README.md                  # 本文档
├── trace-vmexit.c             # BCC BPF C 程序: VM-Exit 追踪
├── run-trace-vmexit.sh        # VM-Exit 追踪运行脚本 (BCC/bpftrace/ftrace 三合一)
├── trace-ept-faults.bpf       # bpftrace: EPT 缺页异常追踪
├── trace-irq-latency.bpf      # bpftrace: 中断注入延迟追踪
├── trace-vfio-dma.bpf         # bpftrace: VFIO DMA 映射追踪
└── kvm-overview.bpf           # bpftrace: KVM 全局性能仪表盘
```

## 快速开始

### 前置条件

```bash
# 安装 BPF 工具链
sudo apt install bpfcc-tools python3-bpfcc libbpfcc-dev   # BCC
sudo apt install bpftrace linux-tools-common linux-tools-$(uname -r)  # bpftrace + perf

# 确保内核支持 BPF
zcat /proc/config.gz | grep CONFIG_BPF
# 应输出: CONFIG_BPF=y
```

### 5 分钟上手

```bash
cd /root/code/kvm-study/examples/bpf-programs

# 方法1: KVM 全局概览 (推荐首先运行)
sudo bpftrace kvm-overview.bpf

# 方法2: VM-Exit 详细追踪 (三工具合一)
sudo bash run-trace-vmexit.sh              # BCC 模式
sudo bash run-trace-vmexit.sh --bpftrace   # bpftrace 模式
sudo bash run-trace-vmexit.sh --ftrace     # ftrace 模式

# 方法3: 专注某个子系统
sudo bpftrace trace-ept-faults.bpf      # 内存虚拟化
sudo bpftrace trace-irq-latency.bpf     # 中断虚拟化
sudo bpftrace trace-vfio-dma.bpf        # 设备直通
```

## 程序详解

### 1. trace-vmexit.c + run-trace-vmexit.sh

**功能**: 追踪 KVM VM-Exit 事件, 按退出原因分类统计

**内核映射**:
- 追踪点: `kvm:kvm_exit` (定义于 `arch/x86/kvm/trace.h`)
- 触发链: `vcpu_enter_guest()` → `vmx_handle_exit()` → `trace_kvm_exit()`
- exit_reason: 对应 Intel SDM Vol.3C Appendix C

**退出原因速查**:
| 原因码 | 名称 | 含义 | 频率 |
|--------|------|------|------|
| 0 | EXCEPTION_NMI | 异常/NMI | 低 |
| 1 | EXTERNAL_IRQ | 外部中断 | 高 |
| 7 | INT_WINDOW | 中断窗口等待 | 中 |
| 10 | CPUID | CPUID 指令 | 中 |
| 12 | HLT | HLT 指令 (空闲) | 中 |
| 18 | VMCALL | Hypercall | 低 |
| 24 | EPT_VIOLATION | EPT 缺页 | 最高 |
| 25 | EPT_MISCONFIG | EPT 错误 | 罕见 |
| 48 | MSR_WRITE | WRMSR | 低 |

**使用**:
```bash
# BCC (Python 用户态 + C 内核态)
sudo bash run-trace-vmexit.sh
sudo bash run-trace-vmexit.sh --pid 12345  # 追踪指定 VM

# bpftrace 一行命令
sudo bpftrace -e 'tracepoint:kvm:kvm_exit { @exits[args->exit_reason] = count(); }'

# ftrace
echo kvm:kvm_exit | sudo tee /sys/kernel/debug/tracing/set_event
sudo cat /sys/kernel/debug/tracing/trace_pipe
```

---

### 2. trace-ept-faults.bpf

**功能**: 追踪 EPT (Extended Page Table) 缺页异常

**KVM 内存虚拟化原理**:
```
GVA (Guest Virtual Address)
  ↓ Guest 页表 (CR3)
GPA (Guest Physical Address)
  ↓ EPT 页表 (EPTP)           ← 缺页发生在这里!
HPA (Host Physical Address)
```

**内核映射**:
- `handle_ept_violation()` → `kvm_mmu_page_fault()` → `kvm_tdp_page_fault()`
- 追踪点: `kvm:kvm_exit` (reason=24), `kvm:kvm_page_fault`
- 关键文件: `arch/x86/kvm/mmu/mmu.c`, `arch/x86/kvm/mmu/tdp_mmu.c`

**使用**:
```bash
sudo bpftrace trace-ept-faults.bpf
sudo bpftrace trace-ept-faults.bpf -p $(pidof qemu-system-x86)

# ftrace 等效
echo kvm:kvm_page_fault | sudo tee /sys/kernel/debug/tracing/set_event
sudo cat /sys/kernel/debug/tracing/trace_pipe
```

---

### 3. trace-irq-latency.bpf

**功能**: 测量中断从 host 接收到注入 guest 的全链路延迟

**中断注入流程**:
```
物理设备 → APIC → Host CPU (IRQ handler)
  → irqfd/eventfd → KVM
  → vIRR (虚拟中断请求寄存器)
  → 等待中断窗口 (RFLAGS.IF=1)
  → VM-Entry 时硬件递送
  → Guest IDT → Guest IRQ handler
```

**内核映射**:
- `irqfd_wakeup()` → `kvm_set_irq()` → `apic_set_irq()` → `inject_pending_event()`
- 追踪点: `irq:irq_handler_entry/exit`, `kvm:kvm_set_irq`, `kvm:kvm_entry`, `kvm:kvm_exit`
- 关键文件: `virt/kvm/eventfd.c`, `arch/x86/kvm/lapic.c`

**使用**:
```bash
sudo bpftrace trace-irq-latency.bpf

# ftrace 等效
trace-cmd record -e kvm:kvm_entry -e kvm:kvm_set_irq -e irq:irq_handler_entry sleep 10
trace-cmd report | grep -E 'kvm_entry|kvm_set_irq|irq_handler'
```

---

### 4. trace-vfio-dma.bpf

**功能**: 追踪 VFIO 设备直通的 DMA 映射/解映射操作

**VFIO DMA 映射流程**:
```
Guest Driver: DMA alloc (GPA)
  ↓ VFIO ioctl
QEMU: VFIO_IOMMU_MAP_DMA(iova, vaddr, size)
  ↓
VFIO: pin_user_pages_remote() → 锁定物理页
  ↓
IOMMU: iommu_map(iova → pfn) → 设置 IOMMU 页表
  ↓
物理设备: DMA read/write → IOMMU 翻译 → Host 内存
```

**内核映射**:
- `vfio_dma_do_map()` → `vfio_pin_pages_remote()` → `iommu_map()`
- kprobe: `vfio_dma_do_map`, `vfio_dma_do_unmap`, `iommu_map`, `pin_user_pages_remote`
- 关键文件: `drivers/vfio/vfio_iommu_type1.c`, `drivers/iommu/iommu.c`

**使用**:
```bash
sudo bpftrace trace-vfio-dma.bpf
sudo bpftrace trace-vfio-dma.bpf -p $(pidof qemu-system-x86)

# ftrace 等效 (kprobe_events)
echo 'p:vfio_map vfio_dma_do_map' | sudo tee -a /sys/kernel/debug/tracing/kprobe_events
echo 1 | sudo tee /sys/kernel/debug/tracing/events/kprobes/enable
sudo cat /sys/kernel/debug/tracing/trace_pipe
```

---

### 5. kvm-overview.bpf

**功能**: KVM 性能仪表盘 - 一站式追踪所有关键事件

**覆盖指标**:
- VM-Exit/Entry 频率和速率
- 退出原因分布 (Top 10)
- Guest 执行时间片分布
- EPT 缺页统计
- 中断注入统计
- 按 VM 进程分组

**使用**:
```bash
sudo bpftrace kvm-overview.bpf

# ftrace 等效 (全量追踪)
echo 'kvm:*' | sudo tee /sys/kernel/debug/tracing/set_event
sudo cat /sys/kernel/debug/tracing/trace_pipe

# 或使用 trace-cmd 记录后分析
sudo trace-cmd record -e kvm:* sleep 30
sudo trace-cmd report | grep -oP '\w+:\w+' | sort | uniq -c | sort -rn
```

## BPF 工具对比

| 特性 | BCC | bpftrace | ftrace |
|------|-----|----------|--------|
| 编程模型 | C + Python 用户态 | DTrace 风格脚本 | 内核内置 |
| 学习曲线 | 高 (需写 C) | 中 (一行命令) | 低 (echo/cat) |
| 聚合能力 | 强 (BPF map) | 强 (@map) | 弱 (需外部工具) |
| 输出格式 | 完全自定义 | 内置 hist/print | 原始事件流 |
| 性能开销 | 低 | 低 | 中 (序列化) |
| 适用场景 | 生产环境工具 | 快速调试/分析 | 简单追踪 |

## 性能调优速查

根据追踪结果, 常见问题和优化方向:

### EPT_VIOLATION 频率过高
```bash
# 症状: 退出原因中 EPT_VIOLATION 占比 > 70%
# 原因: 频繁缺页, 大页未生效, 或内存热迁移中
# 优化:
#   1. 使用大页内存: virsh edit <vm> -> <memoryBacking><hugepages/></memoryBacking>
#   2. 预分配内存: <memoryBacking><allocation mode="immediate"/></memoryBacking>
#   3. 检查 THP: cat /sys/kernel/mm/transparent_hugepage/enabled
```

### EXTERNAL_IRQ 频率过高
```bash
# 症状: 外部中断退出占比 > 30%
# 原因: 设备中断风暴, 或 irqbalance 未优化
# 优化:
#   1. 检查中断分布: cat /proc/interrupts
#   2. 设置 CPU 亲和性: virsh vcpupin <vm> <vcpu> <cpulist>
#   3. 使用中断节流 (coalescing)
```

### HLT 退出频繁但 guest 负载高
```bash
# 症状: HLT 退出多, 但 guest CPU 利用率不高
# 原因: halt-polling 未启用, 或 guest 频繁 idle
# 优化:
#   1. 启用 halt-polling: echo 500000 > /sys/module/kvm/parameters/halt_poll_ns
#   2. 检查 guest 电源管理: guest 内 cpupower frequency-set -g performance
```

### 中断注入延迟高
```bash
# 症状: trace-irq-latency 显示 P99 > 10μs
# 原因: guest 长时间关中断, 或 host 调度延迟
# 优化:
#   1. 使用虚拟中断递送 (Virtual Interrupt Delivery, VID)
#   2. 检查 host CPU 隔离: isolcpus + nohz_full
#   3. 减少 vCPU 数量 (减少调度竞争)
```

## 内核源码阅读指南

每个追踪脚本的注释都包含内核源码映射, 建议对照阅读:

```
# 推荐的源码阅读顺序
1. arch/x86/kvm/x86.c        - vcpu_run() 主循环 (核心)
2. arch/x86/kvm/vmx/vmx.c    - VMX 硬件接口 (Intel)
3. arch/x86/kvm/mmu/mmu.c    - 内存管理 (EPT)
4. arch/x86/kvm/irq_comm.c   - 中断路由
5. virt/kvm/kvm_main.c       - KVM 核心 (跨架构)
6. arch/x86/kvm/trace.h      - 所有追踪点定义
```

## 参考资源

- [Intel SDM Vol.3C](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) - VMX 硬件规范
- [KVM 内核文档](https://www.kernel.org/doc/html/latest/virt/kvm/) - KVM 内核子系统
- [BPF 工具文档](https://github.com/iovisor/bpftrace/blob/master/docs/tutorial.md) - bpftrace 教程
- [BCC 开发者指南](https://github.com/iovisor/bcc/blob/master/docs/tutorial_bcc.txt) - BCC 入门

## License

本目录下的 BPF 程序遵循 GPL-2.0 许可证 (与 Linux 内核 BPF 子系统一致)。
