# KVM API 用户空间演示程序

通过用户空间 C 程序直接与 KVM ioctl 接口交互，演示虚拟机的完整生命周期。

## 文件列表

| 文件 | 行数 | 说明 |
|------|------|------|
| `kvm-demo.c` | ~600 | 完整生命周期演示，重度注释，覆盖所有关键 ioctl |
| `kvm-demo-regs.c` | ~250 | 精简版，聚焦寄存器操作 (SET/GET_REGS, SET/GET_SREGS) |
| `Makefile` | - | 构建脚本 |

## 编译和运行

```bash
# 编译
cd /root/code/kvm-study/examples/kvm-api-demo/
make

# 运行完整演示 (需要 root 或 kvm 组权限)
sudo ./kvm-demo

# 运行寄存器演示
sudo ./kvm-demo-regs
```

### 权限配置

如果不想用 root，可以添加 kvm 组权限:

```bash
# 确认 kvm 组存在
getent group kvm

# 将当前用户加入 kvm 组
sudo usermod -aG kvm $USER

# 设置设备权限
sudo chgrp kvm /dev/kvm
sudo chmod 660 /dev/kvm

# 之后可以不使用 sudo 运行
./kvm-demo
```

## 前置条件

```bash
# 1. CPU 支持虚拟化
grep -E "vmx|svm" /proc/cpuinfo

# 2. KVM 内核模块已加载
lsmod | grep kvm
# 应该看到 kvm_intel 或 kvm_amd

# 3. /dev/kvm 设备存在
ls -la /dev/kvm

# 如果模块未加载:
sudo modprobe kvm
sudo modprobe kvm_intel   # Intel CPU
# sudo modprobe kvm_amd   # AMD CPU
```

## 程序内容概述

### kvm-demo.c: 完整生命周期

程序按 10 个步骤执行，每步都有详细的中文注释:

```
步骤 1: open("/dev/kvm")              → 打开 KVM 设备
步骤 2: ioctl(KVM_CREATE_VM)          → 创建虚拟机实例
步骤 3: ioctl(KVM_SET_IDENTITY_MAP_ADDR) → x86 身份映射
        ioctl(KVM_SET_TSS_ADDR)       → TSS 设置
步骤 4: ioctl(KVM_SET_USER_MEMORY_REGION) → 分配 Guest 内存
步骤 5: 写入机器码到 Guest 内存       → 加载 Guest 代码
步骤 6: ioctl(KVM_CREATE_VCPU)        → 创建虚拟 CPU
步骤 7: mmap(vcpu_fd)                 → 映射 kvm_run 共享内存
步骤 8: ioctl(KVM_SET_REGS/SREGS)     → 配置寄存器
步骤 9: ioctl(KVM_RUN)                → ★ 运行 vCPU (VM-Entry)
步骤10: 读取 exit_reason              → 处理 VM-Exit
清理:   close(fd) + munmap            → 释放资源
```

Guest 代码:
```asm
mov $0x0042, %ax    ; 将 0x42 加载到 AX
hlt                 ; 停机，触发 VM-Exit
```

### kvm-demo-regs.c: 寄存器操作

精简版 (~250 行)，聚焦于:

1. **KVM_SET_REGS / KVM_GET_REGS** — 通用寄存器 (RAX-R15, RIP, RFLAGS)
2. **KVM_SET_SREGS / KVM_GET_SREGS** — 特殊寄存器 (CS/DS/ES/FS/GS/SS, CR0-CR4, EFER)
3. **执行前后对比** — 展示 Guest 代码对寄存器的修改

Guest 代码更丰富:
```asm
mov $0x42, %al     ; AL = 0x42
mov $0x100, %bx    ; BX = 0x0100
add %al, %bl       ; BL = BL + AL
hlt                ; 停机
```

## 内核源码映射

每个 ioctl 对应的内核代码路径（参考 `linux-6.12.93`）:

### ioctl 到内核函数的映射

| 用户空间 ioctl | 内核入口 | 关键函数 |
|---|---|---|
| `KVM_CREATE_VM` | `kvm_dev_ioctl()` | `kvm_create_vm()` → `kvm_arch_init_vm()` |
| `KVM_CREATE_VCPU` | `kvm_vm_ioctl()` | `kvm_vm_ioctl_create_vcpu()` → `vmx_create_vcpu()` |
| `KVM_SET_USER_MEMORY_REGION` | `kvm_vm_ioctl()` | `kvm_set_memory_region()` → `kvm_set_memslot()` |
| `KVM_SET_REGS` | `kvm_vcpu_ioctl()` | `kvm_arch_vcpu_ioctl_set_regs()` |
| `KVM_GET_REGS` | `kvm_vcpu_ioctl()` | `kvm_arch_vcpu_ioctl_get_regs()` |
| `KVM_SET_SREGS` | `kvm_vcpu_ioctl()` | `kvm_arch_vcpu_ioctl_set_sregs()` → `vmx_set_cr0()` 等 |
| `KVM_GET_SREGS` | `kvm_vcpu_ioctl()` | `kvm_arch_vcpu_ioctl_get_sregs()` → `vmx_get_segment()` |
| `KVM_RUN` | `kvm_vcpu_ioctl()` | `vcpu_run()` → `vmx_vcpu_run()` → `vmx_vmenter()` |
| `KVM_SET_TSS_ADDR` | `kvm_arch_vm_ioctl()` | `vmx_set_tss_addr()` |
| `KVM_SET_IDENTITY_MAP_ADDR` | `kvm_arch_vm_ioctl()` | 设置 `kvm->arch.ept_identity_map_addr` |

### 核心源文件参考

```
virt/kvm/kvm_main.c                    ← KVM 框架核心，ioctl 分发
  kvm_dev_ioctl()                      ← /dev/kvm ioctl 入口
  kvm_vm_ioctl()                       ← VM fd ioctl 入口
  kvm_vcpu_ioctl()                     ← vCPU fd ioctl 入口
  kvm_create_vm()                      ← 创建 struct kvm
  kvm_vcpu_ioctl_set_memory_region()   ← 内存区域设置

arch/x86/kvm/x86.c                     ← x86 架构通用代码
  kvm_arch_vcpu_ioctl_run()            ← KVM_RUN 架构入口
  vcpu_run()                           ← vCPU 执行主循环
  vcpu_enter_guest()                   ← 进入 Guest 前的准备

arch/x86/kvm/vmx/vmx.c                 ← ★ VMX 核心实现
  vmx_create_vcpu()                    ← 创建 vcpu_vmx，分配 VMCS
  vmx_vcpu_run()                       ← ★ VM-Entry/VM-Exit 处理
  vmx_handle_exit()                    ← ★ VM-Exit 分发
  vmx_set_segment()                    ← 写段寄存器到 VMCS
  vmx_get_segment()                    ← 从 VMCS 读段寄存器

arch/x86/kvm/vmx/main.c                ← VMX 回调注册
  vt_x86_ops                           ← kvm_x86_ops 的 VMX 实现

arch/x86/kvm/vmx/vmenter.S             ← ★ 汇编级 VM-Entry/VM-Exit
  __vmx_vcpu_run()                     ← 保存 Host 状态 → VMENTER
  vmx_vmenter()                        ← 执行 VMLAUNCH/VMRESUME

include/uapi/linux/kvm.h               ← 用户空间 API 定义
  struct kvm_run                       ← VM-Exit 信息共享结构
  struct kvm_regs                      ← 通用寄存器
  struct kvm_sregs                     ← 特殊寄存器 (段/控制)
  KVM_EXIT_*                           ← 退出原因枚举

include/linux/kvm_host.h               ← 内核内部数据结构
  struct kvm                           ← VM 实例
  struct kvm_vcpu                      ← 虚拟 CPU
```

### VM-Entry / VM-Exit 完整路径

```
用户空间                              内核空间
────────                              ────────
ioctl(KVM_RUN)
  │
  ▼
kvm_vcpu_ioctl()
  → kvm_arch_vcpu_ioctl_run()
    → vcpu_run()               ← 主循环 (while loop)
      → vcpu_enter_guest()
        → kvm_x86_run()
          → vmx_vcpu_run()     ← ★ VMX 核心
            │
            ├─ vmcs_load()     ← 加载 VMCS 到 CPU
            ├─ vmcs_writel(GUEST_RIP, ...)  ← 写寄存器到 VMCS
            │
            ├─ vmx_vcpu_enter_exit()
            │   → __vmx_vcpu_run()   ← 汇编代码 (vmenter.S)
            │     → 保存 Host 寄存器
            │     → vmx_vmenter()
            │       → VMLAUNCH/VMRESUME ← ★ 硬件指令!
            │       │
            │       ├── CPU 进入 Non-Root Mode
            │       ├── Guest 执行代码 (MOV + HLT)
            │       ├── HLT 触发 VM-Exit
            │       ├── CPU 回到 Root Mode
            │       │
            │     ← 恢复 Host 寄存器
            │
            ├─ vmcs_readl(GUEST_RIP)  ← 从 VMCS 读回寄存器
            │
            └─ vmx_handle_exit()     ← ★ Exit 处理
              → handle_halt()
                → exit_reason = KVM_EXIT_HLT
  │
  ▼ (ioctl 返回)
用户空间检查 run->exit_reason
```

## 使用 ftrace 观察内核行为

### 方法一：tracepoint 追踪

```bash
# 准备
echo > /sys/kernel/debug/tracing/trace

# 启用 KVM tracepoints
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_page_fault/enable

# 运行演示
sudo ./kvm-demo

# 查看结果
cat /sys/kernel/debug/tracing/trace

# 预期输出 (关键行):
#   kvm_entry: vcpu 0, rip 0x1000          ← VM-Entry，RIP=0x1000
#   kvm_exit:  reason HLT rip 0x1004 ...   ← HLT 触发 VM-Exit
#   kvm_page_fault: ...                     ← 首次访问 Guest 内存的 EPT Violation

# 清理
echo 0 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
echo 0 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable
echo 0 > /sys/kernel/debug/tracing/events/kvm/kvm_page_fault/enable
```

### 方法二：函数追踪

```bash
# 追踪关键函数调用
echo > /sys/kernel/debug/tracing/trace
echo function > /sys/kernel/debug/tracing/current_tracer
echo kvm_vcpu_ioctl >> /sys/kernel/debug/tracing/set_ftrace_filter
echo vmx_vcpu_run >> /sys/kernel/debug/tracing/set_ftrace_filter
echo handle_halt >> /sys/kernel/debug/tracing/set_ftrace_filter

# 运行
sudo ./kvm-demo

# 查看
cat /sys/kernel/debug/tracing/trace

# 恢复
echo nop > /sys/kernel/debug/tracing/current_tracer
echo > /sys/kernel/debug/tracing/set_ftrace_filter
```

### 方法三：使用项目中的 ftrace 脚本

```bash
# Phase 1 提供的 ftrace 脚本
/root/code/kvm-study/scripts/trace/trace-vmexit.sh
```

### 方法四：perf 事件分析

```bash
# 记录 KVM 事件
perf record -e kvm:kvm_exit,kvm:kvm_entry,kvm:kvm_page_fault -a ./kvm-demo

# 查看统计
perf report

# 单次统计
perf stat -e kvm:kvm_exit,kvm:kvm_entry ./kvm-demo
```

### 方法五：ftrace 脚本化演示

```bash
#!/bin/bash
# trace-kvm-demo.sh - 一键追踪本演示程序的内核路径

TRACEFS=/sys/kernel/debug/tracing

# 清理
echo > ${TRACEFS}/trace
echo nop > ${TRACEFS}/current_tracer
echo > ${TRACEFS}/set_ftrace_filter

# 启用 tracepoints
for tp in kvm_entry kvm_exit kvm_page_fault kvm_vcpu_wakeup; do
    echo 1 > ${TRACEFS}/events/kvm/${tp}/enable 2>/dev/null
done

# 运行
./kvm-demo

# 收集结果
echo ""
echo "===== Trace 结果 ====="
cat ${TRACEFS}/trace

# 清理
for tp in kvm_entry kvm_exit kvm_page_fault kvm_vcpu_wakeup; do
    echo 0 > ${TRACEFS}/events/kvm/${tp}/enable 2>/dev/null
done
```

## 与 Phase 1 学习材料的关联

本演示程序直接对应 Phase 1 (`phase1-vtx-basics/`) 的学习内容:

| Phase 1 主题 | 本演示对应 |
|---|---|
| VMX Root/Non-root 模式切换 | `KVM_RUN` ioctl → VM-Entry → VM-Exit |
| VMCS 结构和控制字段 | `KVM_SET_REGS/SREGS` → 写入 VMCS |
| VM-Entry 路径 | `vcpu_run()` → `vmx_vcpu_run()` → `vmx_vmenter()` |
| VM-Exit 分类和处理 | `handle_exit()` → 检查 `exit_reason` |
| `vt_x86_ops` 回调表 | 每个 ioctl 最终调用对应的回调函数 |
| `vmx_vcpu_run()` 流程 | KVM_RUN ioctl 的完整内核路径 |
| `vmx_handle_exit()` 分发 | 根据 HLT exit_reason 分发处理 |
| `struct vcpu_vmx` 结构 | `KVM_CREATE_VCPU` 创建的核心结构 |

### 建议的学习顺序

1. **先运行本演示** — 从用户空间视角建立直觉
2. **阅读注释中的内核路径** — 将 ioctl 映射到内核函数
3. **用 ftrace 观察** — 看到真实的内核执行
4. **对照 Phase 1 的源码精读** — 深入 `vmx.c`, `x86.c`, `kvm_main.c`
5. **修改 Guest 代码** — 尝试不同的指令，观察行为变化

### 实验建议

```bash
# 实验 1: 修改 Guest 代码
# 将 HLT 改为其他指令，观察不同的 VM-Exit 原因

# 实验 2: 添加 I/O 指令
# 在 Guest 代码中加入 'in %dx, %al' (0xEC)
# 观察 KVM_EXIT_IO 退出

# 实验 3: 观察 EPT Violation
# 用 ftrace 查看 kvm_page_fault 事件
# 对应 Guest 首次访问物理地址时的 EPT 映射

# 实验 4: 对比寄存器前后变化
# 使用 kvm-demo-regs 观察哪些寄存器被 Guest 修改

# 实验 5: 追踪函数调用链
# 用 ftrace function tracer 追踪 vmx_vcpu_run 的调用栈
```

## 关键数据结构图示

### kvm_run (用户空间-内核共享)

```
struct kvm_run {                    ← 通过 mmap(vcpu_fd) 映射
    /* 输入: 用户空间 → 内核 */
    __u8 request_interrupt_window;
    __u8 immediate_exit;
    __u8 padding1[6];

    /* 输出: 内核 → 用户空间 */
    __u32 exit_reason;              ← VM-Exit 原因
    __u8  ready_for_interrupt_injection;

    /* 退出信息 (union，根据 exit_reason 使用不同成员) */
    union {
        struct {                     ← KVM_EXIT_IO 时
            __u8 direction;         // 0=in, 1=out
            __u8 size;
            __u16 port;
            __u32 count;
            __u64 data_offset;
        } io;

        struct {                     ← KVM_EXIT_HLT 时
            (无额外字段)
        } hlt;

        struct {                     ← KVM_EXIT_MMIO 时
            __u64 phys_addr;
            __u8 data[8];
            __u32 len;
            __u8 is_write;
        } mmio;
    };
    ...
};
```

### 寄存器在内核中的存储位置

```
                   KVM_SET_REGS          VM-Entry 前
用户空间 regs ──────────────────→ vcpu->arch.regs[] ──────────→ VMCS
                   KVM_GET_REGS           (内存缓存)           (vmcs_writel)
                 ←────────────────── ←──────────────────────
                   (读取)              VM-Exit 后 (vmcs_readl)

                   KVM_SET_SREGS
用户空间 sregs ─────────────────→ VMCS (直接写入)
                   KVM_GET_SREGS         (vmx_set_cr0 等)
                 ←────────────────── ← VMCS (vmx_get_segment)
```

## 常见错误排查

| 错误 | 原因 | 解决 |
|------|------|------|
| `无法打开 /dev/kvm: Permission denied` | 权限不足 | `sudo` 或加入 kvm 组 |
| `无法打开 /dev/kvm: No such file` | 模块未加载 | `sudo modprobe kvm kvm_intel` |
| `KVM_SET_TSS_ADDR 失败` | 非 x86 架构 | 仅在 x86/x86_64 上需要 |
| `KVM_CREATE_VM 失败: Device busy` | /dev/kvm 被占用 | 关闭其他 VMM (VirtualBox 等) |
| `KVM_RUN 失败: Invalid argument` | 寄存器设置错误 | 检查 CR0/段寄存器配置 |

## 参考资料

- **内核源码**: `/root/code/linux-6.12.93/`
  - `include/uapi/linux/kvm.h` — 用户空间 API
  - `virt/kvm/kvm_main.c` — 框架核心
  - `arch/x86/kvm/vmx/vmx.c` — VMX 实现
- **本项目**:
  - `phase1-vtx-basics/README.md` — VT-x 基础学习指南
  - `notes/source-navigation.md` — 源码导航图
- **外部**:
  - [KVM API 文档](https://www.kernel.org/doc/html/latest/virt/kvm/)
  - Intel SDM Vol.3 Chapter 23-29 (VMX)
