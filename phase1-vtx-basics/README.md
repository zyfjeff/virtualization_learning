# 第一阶段：Intel VT-x 与 CPU 虚拟化

## 学习目标

1. 理解VMX Root/Non-root模式切换机制
2. 掌握VMCS结构和关键控制字段
3. 追踪从ioctl(KVM_RUN)到VMENTER的完整路径
4. 理解VM-Exit的分类和处理流程
5. 熟悉KVM核心数据结构和回调表
6. **掌握CPUID虚拟化机制（静态过滤 vs 动态拦截）** ← 新增
7. **理解MSR Bitmap和MSR虚拟化策略** ← 新增
8. **列举触发VM-Exit的指令及KVM处理方式** ← 新增

## 本章文件

| 文件 | 内容 |
|------|------|
| `README.md` | 本文件：VT-x硬件基础 + 学习指南 |
| `annotations.md` | 源码精读：vmx_x86_ops, vmx_hardware_setup, vmx_vcpu_run |
| `cpu-virtualization.md` | ★ CPU虚拟化：CPUID / MSR / 指令虚拟化 / kvm_x86_ops |
| `practice/` | ★ 实战练习：VMX验证 / CPUID Faulting / MSR测试 / VM-Exit开销测量 |

## 推荐阅读顺序

```
第1步: README.md (本文件)
  → 理解 VMX 硬件模式、VMCS 结构
  → 运行 vmx-info.ko 查看 CPU 虚拟化能力

第2步: annotations.md
  → vt_x86_ops 回调表: KVM 的完整接口
  → vmx_hardware_setup(): 硬件能力检测
  → vmx_vcpu_run(): VM-Entry/Exit 主循环
  → vmx_handle_exit(): Exit 处理分发

第3步: cpu-virtualization.md ← CPU 虚拟化深入
  → CPUID 虚拟化: 两种机制对比
  → MSR Bitmap: 4KB 位图控制逐 MSR 拦截
  → 指令虚拟化: 哪些指令触发 VM-Exit
  → kvm_x86_ops: 通用层 ↔ VMX 桥梁

第4步: practice/ ← 实战练习
  → ex1-vmx-verify: 验证 VMX 支持和能力
  → ex2-cpuid-fault: 测试 CPUID Faulting 机制
  → ex3-msr-test: 测量 MSR 访问时间
  → ex5-vmexit-overhead: 测量 VM-Exit 开销

第5步: 运行示例 + ftrace 追踪
  → kvm-demo: 完整 VM 生命周期
  → ftrace kvm_cpuid / kvm_msr: 观察拦截效果
```

## 硬件理论准备

### 必读文档
- Intel SDM Vol.3 Chapter 23-28: VMX基础
- Intel SDM Vol.3 Chapter 29: VMCS结构

### 关键硬件概念

```
┌──────────────────────────────────────────────────┐
│                  CPU执行模式                       │
│                                                    │
│  ┌──────────────────┐  ┌────────────────────────┐ │
│  │  VMX Root Mode    │  │  VMX Non-Root Mode     │ │
│  │  (VMM/Hypervisor) │  │  (Guest/虚拟机)         │ │
│  │                   │  │                         │ │
│  │  执行VMM代码       │  │  执行Guest代码           │ │
│  │  管理VMCS         │  │  受限于VMCS控制          │ │
│  │  处理VM-Exit      │  │  触发VM-Exit的条件:      │ │
│  │                   │  │   - 特定指令执行          │ │
│  │  VMENTER ──────────────► 进入Guest              │ │
│  │  VM-Exit ◄────────────── 特定事件触发           │ │
│  └──────────────────┘  └────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

### VMCS (Virtual Machine Control Structure)
```
VMCS 布局 (6.12.93 arch/x86/include/asm/vmx.h):

┌─ Guest State Area ──────────────────────────────────┐
│  CR0, CR3, CR4          ← Guest控制寄存器             │
│  DR7                    ← 调试寄存器                  │
│  RSP, RIP, RFLAGS       ← 通用寄存器                  │
│  CS/DS/ES/FS/GS/SS/TR   ← 段寄存器(选择子/基址/限制/AR)│
│  GDTR/IDTR              ← 描述符表寄存器               │
│  IA32_EFER              ← 扩展特性寄存器               │
└──────────────────────────────────────────────────────┘

┌─ Host State Area ───────────────────────────────────┐
│  CR0, CR3, CR4          ← Host控制寄存器              │
│  RSP, RIP               ← VM-Exit后恢复              │
│  CS/DS/ES/FS/GS/SS/TR   ← Host段寄存器               │
│  GDTR/IDTR              ← Host描述符表                │
└──────────────────────────────────────────────────────┘

┌─ VM-Execution Controls ─────────────────────────────┐
│  PIN_BASED_VM_EXEC_CONTROL   ← 引脚控制 (NMI/外部中断) │
│  CPU_BASED_VM_EXEC_CONTROL   ← CPU控制 (MSR/IO/CPUID) │
│  EXCEPTION_BITMAP            ← 哪些异常触发VM-Exit     │
│  VM_ENTRY_CONTROLS           ← VM-Entry控制           │
│  VM_EXIT_CONTROLS            ← VM-Exit控制            │
│  EPT_POINTER                 ← EPT页表基址 ★           │
│  VPID                        ← 虚拟处理器ID ★          │
└──────────────────────────────────────────────────────┘

┌─ VM-Exit Information Fields ────────────────────────┐
│  EXIT_REASON               ← 退出原因 (16位)         │
│  EXIT_QUALIFICATION         ← 退出限定信息            │
│  VM_EXIT_INTR_INFO          ← 中断信息                │
│  GUEST_PHYSICAL_ADDRESS     ← EPT Violation的GPA     │
│  GUEST_LINEAR_ADDRESS       ← 触发退出的线性地址       │
└──────────────────────────────────────────────────────┘
```

## 源码精读指南

### 文件阅读顺序

```
第1步: arch/x86/kvm/vmx/main.c (171行, 入口注册)
  └→ vt_x86_ops 回调表: VMX操作的完整接口
  └→ vt_init_ops 初始化回调表
  └→ vmx_init(): 模块入口

第2步: arch/x86/kvm/vmx/vmx.c (8500+行, VMX核心)
  └→ 模块参数: ept, vpid, apicv, nested...
  └→ vmx_hardware_setup(): 硬件能力检测和初始化
  └→ vmx_vcpu_create(): vCPU创建 (分配VMCS)
  └→ vmx_vcpu_run(): ★ 核心执行函数
  └→ vmx_handle_exit(): VM-Exit处理入口

第3步: arch/x86/kvm/vmx/vmx.h (结构定义)
  └→ struct vcpu_vmx: VMX vCPU扩展结构
  └→ union vmx_exit_reason: Exit原因位域

第4步: include/linux/kvm_host.h (核心数据结构)
  └→ struct kvm: VM实例
  └→ struct kvm_vcpu: 虚拟CPU

第5步: virt/kvm/kvm_main.c (KVM框架核心)
  └→ kvm_dev_ioctl(): ioctl入口
  └→ kvm_vcpu_ioctl(): vCPU ioctl
```

### 关键函数深度注释

详见 [annotations.md](./annotations.md)

## 实践练习

### 1. CPU虚拟化支持检查
```bash
# 查看CPU虚拟化特性
grep -E "vmx|ept|vpid" /proc/cpuinfo

# 关键特性解读:
# vmx          - Intel VT-x支持
# ept          - Extended Page Tables
# vpid         - Virtual Processor ID (避免TLB flush)
# ept_ad       - EPT Accessed/Dirty位
# flexpriority - TPR Shadow (灵活优先级)
# vnmi         - 虚拟NMI
```

### 2. KVM模块参数查看
```bash
# 查看kvm_intel参数
for f in /sys/module/kvm_intel/parameters/*; do
    echo "$(basename $f) = $(cat $f 2>/dev/null)"
done

# 查看kvm通用参数
for f in /sys/module/kvm/parameters/*; do
    echo "$(basename $f) = $(cat $f 2>/dev/null)"
done
```

### 3. ftrace追踪VM-Exit
```bash
# 启用kvm_exit tracepoint
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable

# 查看实时trace
cat /sys/kernel/debug/tracing/trace_pipe

# 清理
echo 0 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable
echo 0 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
```

### 4. 关键tracepoints列表
```bash
# 列出所有KVM tracepoints
ls /sys/kernel/debug/tracing/events/kvm/

# 核心tracepoints:
# kvm_entry          ← VM-Entry (进入Guest前)
# kvm_exit           ← VM-Exit (退出原因+qualification)
# kvm_inj_virq       ← 虚拟中断注入
# kvm_page_fault     ← 页错误 (GPA + error_code)
# kvm_msr            ← MSR读写
# kvm_cpuid          ← CPUID处理
# kvm_apic           ← APIC操作
# kvm_eoi            ← EOI处理
```

### 5. ftrace函数追踪
```bash
# 追踪vmx_vcpu_run函数调用链
echo function > /sys/kernel/debug/tracing/current_tracer
echo vmx_vcpu_run > /sys/kernel/debug/tracing/set_ftrace_filter
echo 1 > /sys/kernel/debug/tracing/tracing_on

# 运行一个VM然后查看结果
cat /sys/kernel/debug/tracing/trace

# 恢复
echo nop > /sys/kernel/debug/tracing/current_tracer
echo > /sys/kernel/debug/tracing/set_ftrace_filter
echo 0 > /sys/kernel/debug/tracing/tracing_on
```

## 可运行示例 (动手实践)

> 参考 Hypervisor 101 in Rust 模式：每学完一个概念，就运行对应示例验证

### 示例1: VMX能力检测 (安全入门)

```bash
cd /root/code/kvm-study/examples/minimal-vmx/vmx-info/
make
sudo insmod vmx-info.ko
dmesg | tail -30    # 查看CPU虚拟化能力
sudo rmmod vmx-info
```

这个模块 **不会执行VMXON**，只读取MSR报告CPU的虚拟化能力。
对应源码: `vmx_hardware_setup()` 中的特性检测逻辑。

### 示例2: KVM API完整演示

```bash
cd /root/code/kvm-study/examples/kvm-api-demo/
make
sudo ./kvm-demo     # 创建VM、设置vCPU、运行Guest代码
```

这个程序完整演示了 `ioctl(KVM_CREATE_VM)`, `ioctl(KVM_CREATE_VCPU)`, `ioctl(KVM_RUN)` 的用法。
对应源码: `kvm_dev_ioctl()` → `kvm_vcpu_ioctl()` → `vcpu_run()` 的完整路径。

### 示例3: 用ftrace观察示例运行

```bash
# 开启KVM tracepoints
echo 1 > /sys/kernel/debug/tracing/events/kvm/enable

# 运行kvm-demo，同时观察trace输出
cat /sys/kernel/debug/tracing/trace_pipe &
sudo ./kvm-demo
# 你应该能看到 kvm_entry, kvm_exit 等事件
```

## 验证清单

完成后确认能回答：
- [ ] `vt_x86_ops` 包含哪些关键回调？它们分别在什么时候被调用？
- [ ] `vmx_hardware_setup()` 检测了哪些CPU特性？如何禁用不支持的特性？
- [ ] `vmx_vcpu_run()` 的完整执行流程？VMENTER在哪里发生？
- [ ] VM-Exit后 `vmx_handle_exit()` 如何根据exit_reason分发处理？
- [ ] `struct vcpu_vmx` 中 `vmcs01` 和 `loaded_vmcs` 的关系？
- [ ] VMCS的Guest State和Host State分别保存什么？何时加载？
- [ ] EPT Pointer在VMCS中如何配置？与kvm->arch.eptp的关系？

---

## 🔍 VMM视角对比

### 用户态VMM vs KVM内核态实现

| 方面 | 用户态VMM (QEMU) | KVM内核态 |
|------|------------------|-----------|
| **VMCS管理** | ioctl(KVM_SET_SREGS)等间接设置 | 直接`vmcs_write()`，零系统调用开销 |
| **VM-Entry/Exit** | ioctl(KVM_RUN)返回到用户态 | `vmx_vcpu_run()`内核态循环，快速路径无切换 |
| **VM-Exit处理** | 每次VM-Exit都返回用户态 | 简单VM-Exit（HLT、外部中断）内核态直接处理 |
| **寄存器同步** | 通过ioctl读取/设置 | 直接使用vmcs_read/vmcs_write |

### 关键差异：VM-Exit处理路径

```
用户态VMM:
  Guest → VM-Exit → 内核态 → 用户态(QEMU) → 内核态 → Guest
  每次VM-Exit: 4次模式切换

KVM内核态:
  Guest → VM-Exit → 内核态处理 → Guest
  快速路径: 2次模式切换 (减少50%)
```

### 为什么KVM要在内核态处理部分VM-Exit？

1. **性能**：避免用户态/内核态切换开销（~1μs/次）
2. **延迟**：减少VM-Exit到VM-Entry的延迟
3. **并发**：多个vCPU可以并发处理VM-Exit

---

## ⚡ 性能优化技术

### 1. VPID (Virtual Processor ID)

**问题**：VM-Entry/Exit时需要刷新TLB，导致性能下降

**解决**：VPID为每个vCPU分配唯一ID，避免TLB刷新

```c
/* vmx_hardware_setup() 中检测VPID支持 */
if (!cpu_has_vmx_vpid() || !cpu_has_vmx_invvpid())
    enable_vpid = 0;
```

**配置**：
```bash
# 查看是否启用
cat /sys/module/kvm_intel/parameters/vpid
# 输出: Y (启用) 或 N (禁用)

# 手动启用 (需要重新加载模块)
modprobe -r kvm_intel
modprobe kvm_intel vpid=1
```

**效果**：减少VM-Entry开销10-15%

### 2. EPTP Switching

**问题**：多VM切换时需要切换EPT页表

**解决**：使用VMCS中的EPT_POINTER字段，快速切换

```c
/* vmx_vcpu_run() 中加载EPT */
if (kvm->arch.eptp != vmx->loaded_vmcs->eptp) {
    vmcs_write64(EPT_POINTER, kvm->arch.eptp);
    vmx->loaded_vmcs->eptp = kvm->arch.eptp;
    /* 可能需要INVEPT刷新TLB */
}
```

**优化**：使用INVEPT_SINGLE_CONTEXT代替INVEPT_ALL_CONTEXTS

### 3. VMCS Shadow

**问题**：嵌套虚拟化时，L1 VMM频繁读写VMCS触发VM-Exit

**解决**：使用VMCS Shadow，L1 VMM直接读写shadow，减少VM-Exit

```c
/* vmx_create_vcpu() 中分配VMCS shadow */
if (nested) {
    vmx->nested.vmcs02.vmcs_shadow = alloc_vmcs_shadow();
    vmcs_write64(VMCS_LINK_POINTER, __pa(vmcs_shadow));
}
```

**效果**：嵌套虚拟化性能提升20-30%

---

## ⚠️ 常见陷阱

### 陷阱1：VMCS字段未初始化

**场景**：创建vCPU后立即调用ioctl(KVM_RUN)

**症状**：VM-Entry失败，`exit_reason = EXIT_REASON_INVALID_STATE`

**原因**：Guest控制寄存器（CR0/CR4/EFER）未设置

**解决**：
```c
// 正确的初始化顺序
ioctl(vcpu_fd, KVM_SET_REGS, &regs);      // 设置通用寄存器
ioctl(vcpu_fd, KVM_SET_SREGS, &sregs);    // 设置控制寄存器 (CR0/CR3/CR4/EFER)
ioctl(vcpu_fd, KVM_SET_MSRS, &msrs);      // 设置MSR
ioctl(vcpu_fd, KVM_RUN, 0);               // 运行vCPU
```

**源码位置**：`vmx_vcpu_run()`检查`vmx->emulation_required`标志

### 陷阱2：MSR Bitmap配置不当

**场景**：Guest频繁读写MSR，导致大量VM-Exit

**症状**：`perf kvm stat report`显示大量`MSR_WRITE`/`MSR_READ`退出

**原因**：MSR Bitmap未配置，所有MSR都被拦截

**解决**：
```c
// vmx_setup_msr_bitmap() 中配置
// 只拦截需要虚拟化的MSR (如IA32_TSC, IA32_APIC_BASE)
// 其他MSR直接透传
vmx_disable_intercept_for_msr(vmx, MSR_IA32_TSC, MSR_TYPE_RW);
```

**调优**：
```bash
# 追踪MSR拦截频率
perf kvm stat record -- sleep 10
perf kvm stat report | grep MSR

# 如果MSR拦截过多，检查MSR Bitmap配置
```

### 陷阱3：VMCS加载/卸载错误

**场景**：vCPU在pCPU间迁移时崩溃

**症状**：`vmcs_read()`返回无效值，或内核panic

**原因**：VMCS必须在使用的pCPU上加载（VMXON + VMPTRLD）

**解决**：
```c
// vcpu_load() 中加载VMCS
vmx_vcpu_load(vcpu, cpu)
{
    if (vmx->loaded_vmcs->cpu != cpu) {
        /* 在目标pCPU上加载VMCS */
        vmcs_load(vmx->loaded_vmcs->vmcs);
        vmx->loaded_vmcs->cpu = cpu;
    }
}

// vcpu_put() 中卸载VMCS
vmx_vcpu_put(vcpu)
{
    vmcs_clear(vmx->loaded_vmcs->vmcs);
    vmx->loaded_vmcs->cpu = -1;
}
```

**关键**：VMCS与pCPU绑定，vCPU迁移时必须重新加载

### 陷阱4：VPID冲突

**场景**：多个vCPU使用相同的VPID

**症状**：TLB污染，Guest访问错误的内存页

**原因**：VPID分配算法有bug，导致重复分配

**解决**：
```c
// vmx_vcpu_setup() 中分配VPID
vmx->vpid = allocate_vpid();
// 确保VPID唯一 (0保留给root mode)
if (vmx->vpid == 0)
    vmx->vpid = atomic_inc_return(&next_vpid);
vmcs_write16(VIRTUAL_PROCESSOR_ID, vmx->vpid);
```

**检查**：
```bash
# 查看vCPU的VPID
cat /sys/kernel/debug/kvm/<vm_id>/vcpu/<vcpu_id>/vpid
```
