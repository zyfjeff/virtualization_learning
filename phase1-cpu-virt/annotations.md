# Phase 1：源码精读注释 - CPU 虚拟化（VMX 层）

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。
>
> **体例：问题链**（COURSE_FRAMEWORK §3.2）。每节开头给问题链——
> 先带着问题读代码，再对答案；引申问题从上一个答案长出来，链尾指向下一节/实验。

---

## 1. VMX 架构分层

**问题链**（入口来自第一性原理："硬件辅助 trap-and-emulate 怎么落到代码？"）：

1. 入口：KVM 怎么把 x86 通用逻辑和 VMX 硬件 specifics 解耦？→ 下方回调表
2. 引申：为什么用 `kvm_x86_ops` 回调表而不是直接调 VMX 函数？
   （AMD SVM 要共用同一套 x86 通用层——抽象是为了多厂商）
3. 引申（反事实）：如果去掉这层抽象，KVM 支持 AMD 要改多少？→ 体会接口边界
4. 链尾：回调表里 `vcpu_run` 怎么真正进 Guest？→ §2（Entry/Exit 汇编）

### Q: KVM 如何把「x86 通用逻辑」和「Intel VMX  specifics」解耦？

KVM 在 `arch/x86/kvm/` 下做了一层抽象：

```
x86 通用层（x86.c）             VMX 实现（vmx/）
vcpu_enter_guest()              vmx_vcpu_run()
  └→ kvm_x86_call(vcpu_run)     └→ __vmx_vcpu_run()
      ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
      │   kvm_x86_ops 回调表（间接调用）   │
      └───────────────────────────────────┘
```

`kvm_x86_call(xxx)(...)` 是个宏，本质是 `static_call(kvm_x86_xxx)(...)`。`static_call`
比函数指针快 —— 它在代码里打了 patch point，运行时直接把调用点 patch 成目标函数地址，
省掉了一次间接跳转。

### Q: `vt_x86_ops` 这张表长什么样？

**文件**: `arch/x86/kvm/vmx/main.c:19-162`

```c
struct kvm_x86_ops vt_x86_ops __initdata = {
    .name = KBUILD_MODNAME,

    /* 硬件开关 */
    .enable_virtualization_cpu  = vmx_enable_virtualization_cpu,   /* VMXON */
    .disable_virtualization_cpu = vmx_disable_virtualization_cpu,  /* VMXOFF */

    /* VM / vCPU 生命周期 */
    .vm_size    = sizeof(struct kvm_vmx),       /* ★ 让通用层分配 private 区 */
    .vm_init    = vmx_vm_init,
    .vcpu_create = vmx_vcpu_create,             /* ★ 分配 VMCS 区域 */

    /* ★ 核心执行路径 */
    .vcpu_run           = vmx_vcpu_run,         /* VM-Entry → Guest → VM-Exit */
    .handle_exit        = vmx_handle_exit,      /* Exit 分发 */
    .handle_exit_irqoff = vmx_handle_exit_irqoff,

    /* 中断注入 */
    .inject_irq       = vmx_inject_irq,
    .inject_exception = vmx_inject_exception,
    .sync_pir_to_irr  = vmx_sync_pir_to_irr,    /* ★ Posted Interrupts 同步 */

    /* TLB / MMU */
    .flush_tlb_guest = vmx_flush_tlb_guest,     /* INVVPID */
    .load_mmu_pgd    = vmx_load_mmu_pgd,        /* 写 EPTP */

    /* ... 其他省略 */
};
```

### Q: 为什么表上标 `__initdata`，但回调还能在运行时用？

`__initdata` 让**结构体本身**在初始化后释放。释放能成立，是因为 `kvm_ops_update()`
一次性做了两件事——先把整张表拷进常驻的全局 `kvm_x86_ops`，再把可 static-call 的
回调逐个烤进调用点：

```c
/* 来源: arch/x86/kvm/x86.c:9698-9714 — kvm_ops_update() */
static inline void kvm_ops_update(struct kvm_x86_init_ops *ops)
{
	memcpy(&kvm_x86_ops, ops->runtime_ops, sizeof(kvm_x86_ops));

#define __KVM_X86_OP(func) \
	static_call_update(kvm_x86_##func, kvm_x86_ops.func);
#define KVM_X86_OP(func) \
	WARN_ON(!kvm_x86_ops.func); __KVM_X86_OP(func)
#define KVM_X86_OP_OPTIONAL __KVM_X86_OP
#define KVM_X86_OP_OPTIONAL_RET0(func) \
	static_call_update(kvm_x86_##func, (void *)kvm_x86_ops.func ? : \
					   (void *)__static_call_return0);
#include <asm/kvm-x86-ops.h>
#undef __KVM_X86_OP

	kvm_pmu_ops_update(ops->pmu_ops);
}
```

所以运行时**两个副本都在**：不能进 static_call 的字段（如 `.name`、`.nested_ops`、
`.cpu_dirty_log_size` 这类数据/子表）留在 `kvm_x86_ops` 全局变量里；能进的被 patch 成
直接调用。释放掉的只是厂商模块里那张 `__initdata` 源表。函数在 `.text` 段里，一直在。

**纠正旧表述（2026-11-25 核查，见本章 corrections.md 勘误 4）**：原文接着说"这样设计是为了
在嵌套虚拟化需要切换回调表……L1 KVM 做 nested VMX 时需要换实现"。这个理由**不成立**，
KVM 从不因嵌套而换 `kvm_x86_ops`：

- L1 跑 L2 时切换的是 **VMCS**（vmcs01 → vmcs02），执行路径仍是同一个 `vmx_vcpu_run()`；
- 嵌套相关回调挂在**子表**上，初始化时就一起拷进来了：
  `arch/x86/kvm/vmx/main.c:133` `.nested_ops = &vmx_nested_ops,`
  （子表本体 `arch/x86/kvm/vmx/nested.c:7310`），通用层用
  `kvm_x86_ops.nested_ops->get_state/enable_evmcs/...` 间接分发，例如
  `arch/x86/kvm/x86.c:4767`；
- static_call 这层抽象真正服务的对象是**厂商**（VMX vs SVM）与 retpoline/IBRS 下的
  间接调用开销，而不是嵌套。

---

## 2. VM-Entry / Exit 汇编路径

### Q: 从 C 到 VMENTER 之间到底发生了什么？

**文件**: `arch/x86/kvm/vmx/vmx.c:7344` — `vmx_vcpu_run()`
**文件**: `arch/x86/kvm/vmx/vmenter.S` — `__vmx_vcpu_run()`

```c
fastpath_t vmx_vcpu_run(struct kvm_vcpu *vcpu, u64 run_flags)
{
    /* ★ 1. 同步脏寄存器到 VMCS */
    if (kvm_register_is_dirty(vcpu, VCPU_REGS_RSP))
        vmcs_writel(GUEST_RSP, vcpu->arch.regs[VCPU_REGS_RSP]);

    /* ★ 2. 刷新 Host CR3/CR4（内核可能切了 PCID） */
    cr3 = __get_current_cr3_fast();
    if (unlikely(cr3 != vmx->loaded_vmcs->host_state.cr3)) {
        vmcs_writel(HOST_CR3, cr3);
        vmx->loaded_vmcs->host_state.cr3 = cr3;
    }

    /* 3. 等 LAPIC 定时器到期（避免无谓的 VM-Exit） */
    kvm_wait_lapic_expire(vcpu);

    /* ═══════════ 进入汇编 ═══════════ */
    vmx_vcpu_enter_exit(vcpu, __vmx_vcpu_run_flags(vmx));
    /* ═══════════ 回到 C ═══════════ */

    /* 4. VM-Exit 信息：exit_reason 在 enter_exit 里读，
     *    exit_qualification **不在这读** —— 惰性取 */
    ...
}
```

**`exit_reason` 与 `exit_qualification` 的读取位置/宽度都不一样**（2026-11-25 核查，
旧版把两者都写成在 `vmx_vcpu_run()` 里 `vmcs_read32`）：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:7326-7328 —— 在 vmx_vcpu_enter_exit() 内 */
	vmx->exit_reason.full = vmcs_read32(VM_EXIT_REASON);
	if (likely(!vmx->exit_reason.failed_vmentry))
		vmx->idt_vectoring_info = vmcs_read32(IDT_VECTORING_INFO_FIELD);
```

```c
/* 来源: arch/x86/kvm/vmx/vmx.h:708-716 */
static __always_inline unsigned long vmx_get_exit_qual(struct kvm_vcpu *vcpu)
{
	struct vcpu_vmx *vmx = to_vmx(vcpu);

	if (!kvm_register_test_and_mark_available(vcpu, VCPU_EXREG_EXIT_INFO_1))
		vmx->exit_qualification = vmcs_readl(EXIT_QUALIFICATION);

	return vmx->exit_qualification;
}
```

- `EXIT_QUALIFICATION` 是宽字段（64 位模式 64 bit），必须 `vmcs_readl`，旧版的
  `vmcs_read32(EXIT_QUALIFICATION)` 会截断高 32 位（EPT violation 的 GPA 相关位、
  APIC-access 的 offset 等虽然都在低 32 位，但按 32 位读本身就是错的类型）。
- `VCPU_EXREG_EXIT_INFO_1/2` 这套可用性位表示"只有第一个真正需要它的 exit handler
  才去读 VMCS"，多数 exit 根本不读 `exit_qualification`，省一次 VMCS 访问。
- `VM_EXIT_REASON` 本身是 32 位字段 ⇒ `vmcs_read32` 正确。

### Q: 汇编入口里到底做了什么？

**文件**: `arch/x86/kvm/vmx/vmenter.S:79` — `__vmx_vcpu_run()`

```asm
SYM_FUNC_START(__vmx_vcpu_run)
    /* 保存 Host 被调用者保存寄存器 */
    push %rbp
    push %r15 ... push %r12
    push %rbx

    /* 更新 VMCS 中的 HOST_RSP（指向当前栈顶） */
    call vmx_update_host_rsp    /* vmx.c:7231 */

    /* SPEC_CTRL MSR 切换（Spectre 缓解） */
    /* ... */

    /* ★ 关键：选择 vmlaunch 或 vmresume */
    bt   $VMX_RUN_VMRESUME_SHIFT, %ebx
    jnc  .Lvmlaunch

.Lvmresume:
    vmresume                    /* 或 vmlaunch（首次） */
    jmp  .Lvmfail

.Lvmlaunch:
    vmlaunch
    jmp  .Lvmfail

    /* --- CPU 进入 Non-Root 模式，执行 Guest --- */
    /* --- 某个时刻 VM-Exit 触发，CPU 回到 Root 模式 --- */
    /* --- 硬件自动：恢复 HOST_RSP/HOST_RIP，跳到 vmx_vmexit --- */

SYM_INNER_LABEL_ALIGN(vmx_vmexit, SYM_L_GLOBAL)   /* vmenter.S:193 */
    /* VM-Exit 后：保存 Guest 通用寄存器到 vcpu_vmx */
    /* 恢复 Host 寄存器 */
    pop %rbx
    pop %r12 ... pop %r15
    pop %rbp
    ret
SYM_FUNC_END(__vmx_vcpu_run)
```

**`HOST_RIP` 在哪设置？** `vmx.c:4361` — `vmcs_writel(HOST_RIP, (unsigned long)vmx_vmexit)`。
VM-Exit 后 CPU 自动跳到 `vmx_vmexit` 标签继续执行（不是回到 `.Lvmresume`）。

### Q: 为什么 Host 寄存器要「保存被调用者保存的」就够？

VMENTER 是**函数调用**。按照 System V ABI，callee 必须保存 `rbx/rbp/r12-r15`，caller
保存的寄存器（`rax/rcx/rdx/rsi/rdi/r8-r11`）由 `vmx_vcpu_run()` 自己负责。VM-Exit
后 CPU 处于 `__vmx_vcpu_run` 的上下文，**只需要恢复 callee-saved** —— 其它寄存器
VMX 硬件会在 VM-Exit 时从 Host Area 自动加载。

### Q: VM-Entry 可能失败，失败怎么处理？

VMENTER 指令如果 Guest 状态不合法，CF=1 表示失败、错误码写到 VM-instruction error
field。`__vmx_vcpu_run` 检测到后跳到错误处理路径，把 `vmx->fail = 1`、填
`exit_reason = INVALID_STATE`，回到 C 代码。C 代码再走 `handle_invalid_guest_state()`
模拟执行 —— 这条路径性能很差，说明 Guest 状态有问题（常见于实模式过渡）。

---

## 3. VM-Exit 分发

**问题链**（入口来自 VMM 对比："为什么多数 exit 不回用户态？"）：

1. 入口：VM-Exit 怎么找到对应处理函数？→ `vmx_handle_exit()`（vmx.c:6615）
2. 引申：`__vmx_handle_exit()` 怎么按 exit_reason 分发？快速路径 vs 慢速路径怎么分？
   （能内核态闭环的留内核，需要设备模拟的才回 QEMU）
3. 引申（反事实）：Bus Lock 检测为什么**必须**退出到用户态？
   （`bus_lock_detected` → KVM_EXIT_X86_BUS_LOCK，让 QEMU 处理——内核态闭环不了）
4. 链尾：exit 处理依赖哪些 vCPU 状态？→ §4（vcpu_vmx 结构）；
   真机看 exit 分布 → 实验 ex3（profile-vmexit.sh）

### Q: VM-Exit 是怎么找到对应处理函数的？

**文件**: `arch/x86/kvm/vmx/vmx.c:6615` — `vmx_handle_exit()`

```c
int vmx_handle_exit(struct kvm_vcpu *vcpu, fastpath_t exit_fastpath)
{
    int ret = __vmx_handle_exit(vcpu, exit_fastpath);

    /* Bus Lock 检测：退出到用户空间让 QEMU 处理 */
    if (to_vmx(vcpu)->exit_reason.bus_lock_detected) {
        if (ret > 0)
            vcpu->run->exit_reason = KVM_EXIT_X86_BUS_LOCK;
        return 0;
    }
    return ret;
}
```

### Q: `__vmx_handle_exit()` 内部分发逻辑？

**文件**: `arch/x86/kvm/vmx/vmx.c:6436` — `__vmx_handle_exit()`

```c
/* 简化版（省略 PML flush、nested 处理、vectoring info 检查等） */

/* emulation_required: Guest 状态无效，走模拟路径 */
if (vmx->emulation_required)
    return handle_invalid_guest_state(vcpu);

/* VM-Entry 失败：直接返回用户空间，不查处理函数表 */
if (exit_reason.failed_vmentry) {
    vcpu->run->exit_reason = KVM_EXIT_FAIL_ENTRY;
    vcpu->run->fail_entry.hardware_entry_failure_reason = exit_reason.full;
    return 0;
}

/* ★ 用 exit_reason.basic 索引处理函数表 —— O(1) 分发 */
exit_handler_index = array_index_nospec((u16)exit_reason.basic,
                                        kvm_vmx_max_exit_handlers);
if (!kvm_vmx_exit_handlers[exit_handler_index])
    goto unexpected_vmexit;      /* 未知 exit → KVM_EXIT_INTERNAL_ERROR */

return kvm_vmx_exit_handlers[exit_handler_index](vcpu);
```

**注意**：`array_index_nospec()` 是 Spectre v1 缓解 —— 防止 `exit_reason.basic` 被用户态
控制后做推测执行越界访问。

### Q: 哪些 Exit 走快速路径、哪些返回用户空间？

**文件**: `arch/x86/kvm/vmx/vmx.c:6095` — `kvm_vmx_exit_handlers[]`

| Exit reason | 处理函数 | 路径 | 说明 |
|-------------|---------|------|------|
| `EPT_VIOLATION` | `handle_ept_violation()` | 内核 | 建页表，重入 |
| `EXTERNAL_INTERRUPT` | `handle_external_interrupt()` | 内核 | 处理宿主中断 |
| `HLT` | `kvm_emulate_halt()` | 内核 | 设 `mp_state=HALTED` |
| `PREEMPTION_TIMER` | `handle_preemption_timer()` | 内核 | 更新定时器 |
| `APIC_WRITE` | `handle_apic_write()` | 内核 | 同步虚拟 LAPIC |
| `MSR_WRITE` | `kvm_emulate_wrmsr()` | 内核 | 模拟 MSR 写 |
| `MSR_READ` | `kvm_emulate_rdmsr()` | 内核 | 模拟 MSR 读 |
| `CPUID` | `kvm_emulate_cpuid()` | **内核，恒重入** | 见下方"两处纠正" |
| — | — | — | — |
| `IO_INSTRUCTION` | `handle_io()` | 用户空间* | `KVM_EXIT_IO`（无内核态设备时） |
| EPT violation 打到未映射 GPA | — | 用户空间 | `KVM_EXIT_MMIO` |
| `TRIPLE_FAULT` | `handle_triple_fault()` | 用户空间 | `KVM_EXIT_SHUTDOWN` |

*注：`handle_io()` 在内核态处理后可能返回 1（重入，命中内核态设备/PIO 直连）或 0（回用户空间，
填 `KVM_EXIT_IO`），取决于是否模拟完成。

**两处纠正（2026-11-25 核查，见本章 corrections.md 勘误 4）**：

1. 旧版这张表写的是 `CPUID | handle_cpuid() | 用户空间*`。**`handle_cpuid()` 在
   6.12.93 的 KVM 里不存在**（全树搜索只命中 `arch/x86/coco/tdx/tdx.c:425` 那个 TDX VE
   处理函数，与 KVM VMX 退出表无关）。真正的处理函数是 `kvm_emulate_cpuid()`。
2. **CPUID 不是慢速路径，也不会返回用户空间**：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:6103-6107 */
	[EXIT_REASON_CPUID]                   = kvm_emulate_cpuid,
	[EXIT_REASON_MSR_READ]                = kvm_emulate_rdmsr,
	[EXIT_REASON_MSR_WRITE]               = kvm_emulate_wrmsr,
	[EXIT_REASON_INTERRUPT_WINDOW]        = handle_interrupt_window,
	[EXIT_REASON_HLT]                     = kvm_emulate_halt,
```

```c
/* 来源: arch/x86/kvm/cpuid.c:1681-1697 */
int kvm_emulate_cpuid(struct kvm_vcpu *vcpu)
{
	u32 eax, ebx, ecx, edx;

	if (cpuid_fault_enabled(vcpu) && !kvm_require_cpl(vcpu, 0))
		return 1;

	eax = kvm_rax_read(vcpu);
	ecx = kvm_rcx_read(vcpu);
	kvm_cpuid(vcpu, &eax, &ebx, &ecx, &edx, false);
	kvm_rax_write(vcpu, eax);
	kvm_rbx_write(vcpu, ebx);
	kvm_rcx_write(vcpu, ecx);
	kvm_rdx_write(vcpu, edx);
	return kvm_skip_emulated_instruction(vcpu);
}
```

返回值恒为 `kvm_skip_emulated_instruction()` 的结果（1 = 继续内核态循环），所以
**Guest 的 CPUID 从不产生 `KVM_EXIT_*`**，QEMU 只在启动时用 `KVM_SET_CPUID2` 把表灌进
`vcpu->arch.cpuid_entries`（见 cpu-virtualization.md §1）。MSR 同理：只有 `kvm_get/set_msr_*`
未命中**且**开启了用户态 MSR 过滤（`KVM_CAP_X86_USER_SPACE_MSR`）时，才由
`kvm_msr_user_space()`（`arch/x86/kvm/x86.c:2032`，调用点 `:2069`/`:2091`）填
**`KVM_EXIT_X86_RDMSR` / `KVM_EXIT_X86_WRMSR`**；否则就地注入 #GP。
`KVM_EXIT_INTERNAL_ERROR` 与此无关，它只用于模拟失败一类
（`x86.c:8812` 在 `prepare_emulation_failure_exit()`（定义 `:8796`）里，对外入口是
`kvm_prepare_emulation_failure_exit()` `:8861`；`x86.c:11969` 在 `kvm_task_switch()`
里遇到 MMIO；`x86.c:13833` 在 `kvm_handle_memory_failure()`（定义 `:13817`）里）。

### Q: MSR 访问一定在内核处理吗？`user_space_msr` 机制是什么？

**文件**: `arch/x86/kvm/x86.c:2032` — `kvm_msr_user_space()`

**不一定**。KVM 提供了 `user_space_msr` 机制，允许将特定的 MSR 访问传递给用户空间 VMM（如 QEMU）处理。

**工作流程**：
```
Guest RDMSR/WRMSR
    ↓ VM-Exit (MSR_READ/WRITE)
kvm_emulate_rdmsr/wrmsr()
    ↓ KVM 无法处理
kvm_msr_user_space()
    ↓ 检查是否启用了 MSR 过滤
KVM_EXIT_X86_RDMSR/WRMSR
    ↓ 返回用户空间
QEMU: kvm_handle_rdmsr/wrmsr()
    ↓ 调用注册的 handler
返回模拟结果给 Guest
```

**QEMU 中的实现** (`target/i386/kvm/kvm.c`)：
```c
/* 1. 启用 MSR 过滤能力 */
kvm_vm_enable_cap(s, KVM_CAP_X86_USER_SPACE_MSR, 0,
                  KVM_MSR_EXIT_REASON_FILTER);

/* 2. 注册特定 MSR 的处理函数 */
kvm_filter_msr(s, MSR_CORE_THREAD_COUNT,   /* 0x35: CPU 拓扑 */
               kvm_rdmsr_core_thread_count, NULL);
kvm_filter_msr(s, MSR_PKG_ENERGY_STATUS,   /* 0x611: RAPL 能耗 */
               kvm_rdmsr_pkg_energy_status, NULL);
```

**哪些 MSR 需要在 VMM 层模拟**：
- `MSR_CORE_THREAD_COUNT` (0x35) — 返回虚拟 CPU 拓扑，依赖 `-smp` 配置
- RAPL 系列 (0x606/0x610/0x611/0x614) — 虚拟化能耗监控，多 VM 共享物理 CPU

**为什么需要这个机制**：某些 MSR 的值依赖 VMM 配置（如 CPU 拓扑）或需要跨 VM 聚合（如能耗数据），KVM 内核无法独立处理。

### Q: `exit_fastpath` 有几种？

**文件**: `arch/x86/include/asm/kvm_host.h:215`

| 值 | 含义 | 来源 |
|----|------|------|
| `EXIT_FASTPATH_NONE` | 走完整 `handle_exit` | 默认 |
| `EXIT_FASTPATH_REENTER_GUEST` | 直接重入，跳过分发 | Posted Interrupt 处理完 |
| `EXIT_FASTPATH_EXIT_HANDLED` | exit 已处理完 | 快速路径自己搞定 |
| `EXIT_FASTPATH_EXIT_USERSPACE` | 必须回用户空间 | MMIO/IO 需 QEMU |

---

## 4. `vcpu_vmx` 结构：VMX vCPU 状态

### Q: `vcpu_vmx` 的核心字段有哪些？

**文件**: `arch/x86/kvm/vmx/vmx.h:251+`

```c
struct vcpu_vmx {
    struct kvm_vcpu     vcpu;             /* ★ 必须在第一位（容器宏转换） */

    /* --- VMCS：VMX 控制块（每 vCPU 一份，vmcs 指向的内存物理页对齐） --- */
    struct loaded_vmcs  vmcs01;            /* L1 Guest 的 VMCS */
    struct loaded_vmcs *loaded_vmcs;       /* 当前活跃 VMCS
                                            * 非嵌套 = vmcs01
                                            * 嵌套 L2 = vmcs02 */

    /* --- VM-Exit 信息缓存（避免每次从 VMCS 读） --- */
    unsigned long       exit_qualification;
    u32                 exit_intr_info;
    union vmx_exit_reason exit_reason;

    /* --- Posted Interrupts --- */
    struct pi_desc      pi_desc;           /* ★ PI 描述符（硬件写这个） */
    struct list_head    pi_wakeup_list;    /* 等待 PI 唤醒时挂的链表 */

    /* --- 嵌套虚拟化 --- */
    struct nested_vmx   nested;            /* L1 的 VMX 状态（做嵌套时才用） */

    /* --- 其他 --- */
    int                 vpid;              /* VPID（避免 TLB flush） */
    bool                emulation_required;
    union vmx_exit_reason exit_reason;
    /* 注意: vcpu_vmx 里**没有** preemption_timer 字段，见下方纠正 */
};
```

**纠正旧表述（2026-11-25 核查）**：本节的字段表原来写
`u16 vpid;` + `struct hrtimer preemption_timer;`，两处都与 6.12.93 不符：

```c
/* 来源: arch/x86/kvm/vmx/vmx.h:317-329 */
	int vpid;
	bool emulation_required;

	union vmx_exit_reason exit_reason;

	/* Posted interrupt descriptor */
	struct pi_desc pi_desc;

	/* Used if this vCPU is waiting for PI notification wakeup. */
	struct list_head pi_wakeup_list;

	/* Support for a guest hypervisor (nested VMX) */
	struct nested_vmx nested;
```

- `vpid` 是 **`int`**（`vmx.h:317`），不是 `u16`；`u16` 的那两个在 `struct nested_vmx` 里
  （`vpid02` / `last_vpid`，`vmx.h:231-232`）。
- `struct hrtimer preemption_timer` **属于 `struct nested_vmx`，不属于 `vcpu_vmx`**
  （`arch/x86/kvm/vmx/vmx.h:210`，紧跟 `preemption_timer_deadline` /
  `has_preemption_timer_deadline` / `preemption_timer_expired`）。
  含义别搞反：L1 用的是**硬件** VMCS preemption timer（`handle_preemption_timer()`，
  见 §3 表）；那把 hrtimer 是 L0 替 **L2** 模拟 vmcs12 里的抢占定时器时才用的软件定时器。

### Q: `vmcs01` 和 `loaded_vmcs` 什么关系？

```
非嵌套场景:
  loaded_vmcs ──→ vmcs01（L1 Guest 用的 VMCS）

嵌套场景（Guest 里又跑了个 KVM）:
  loaded_vmcs ──→ vmcs01 （L1 在自己跑）
                  └─→ vmcs02 （L1 启动了 L2，切换到这里）
```

L1 VMM 调 `VMPTRLD` 切 VMCS 时，KVM 要拦截（nested VMX 实现）并切换到对应的 `vmcs02`。
`loaded_vmcs` 就是「当前物理 CPU 上真正 load 着的 VMCS」的指针。

### Q: `pi_desc` 在 Posted Interrupts 里起什么作用？

**文件**: `arch/x86/include/asm/posted_intr.h:12`

```c
struct pi_desc {
    union {
        u32 pir[8];      /* Posted Interrupt Request bitmap (256-bit) */
        u64 pir64[4];
    };
    union {
        struct {
            u16 notifications;  /* ★ bits: ON (outstanding) + SN (suppress) */
            u8  nv;             /* Notification Vector */
            u8  rsvd_2;
            u32 ndst;           /* Notification Destination (APIC ID) */
        };
        u64 control;
    };
    u32 rsvd[6];
} __aligned(64);              /* ★ 64 字节对齐（缓存行对齐） */
```

硬件投递外部中断时：
1. 检查 vCPU 是否在 Non-Root 模式（`vcpu->mode == IN_GUEST_MODE`）
2. 在 `pir[]` 对应位写 1（表示该向量有中断 pending）
3. 置 `notifications` 字段的 ON 位 = 1
4. 如果 vCPU 在 Guest 且「Process Posted Interrupts」exec control = 1 → **不产生 VM-Exit**，
   硬件在下次 VM-Entry 前自动把 PIR 合并到 VIRR

**注意**：`notifications` 是 u16，包含 ON 和 SN 两个位（不是独立的 bitfield）。`nv` 是 u8（不是 u32）。
结构体 `__aligned(64)` 是缓存行对齐，**不是物理页对齐**。这个结构是**硬件直接写的**，KVM 用原子操作更新。

---

## 5. VMX 特性检测

**问题链**（入口来自第一性原理："硬件能力怎么变成 KVM 的可用特性？"）：

1. 入口：`vmx_hardware_setup()` 怎么决定启用哪些特性？→ vmx.c:8404
2. 引申：为什么先读 capability MSR 再"按依赖裁剪"？
   （硬件告诉你支持什么，KVM 按依赖链决定启用什么）
3. 引申（反事实）：EPT 为什么要 ept/4levels/mt_wb/invept_global **四个条件全满足**？
   缺一个会怎样？（缺 invept_global 无法安全失效 → 宁可不用 EPT）
4. 链尾：这些特性开关怎么暴露成运行时参数？→ §6；
   真机解码 capability MSR → 实验 ex1（ex1-vmx-verify）

### Q: `vmx_hardware_setup()` 怎么决定启用哪些特性？

**文件**: `arch/x86/kvm/vmx/vmx.c:8404`

```c
__init int vmx_hardware_setup(void)
{
    /* ★ 1. 读 VMX capability MSRs（硬件告诉你支持什么） */
    if (setup_vmcs_config(&vmcs_config, &vmx_capability) < 0)
        return -EIO;

    /* ★ 2. 按依赖关系裁剪 */
    if (!cpu_has_vmx_ept() || !cpu_has_vmx_ept_4levels() ||
        !cpu_has_vmx_ept_mt_wb() || !cpu_has_vmx_invept_global())
        enable_ept = 0;

    /* EPT 是 unrestricted guest 的前提 */
    if (!cpu_has_vmx_unrestricted_guest() || !enable_ept)
        enable_unrestricted_guest = 0;

    /* APICv 是 PI 的前提 */
    if (!cpu_has_vmx_apicv())
        enable_apicv = 0;
    if (!enable_apicv)
        vt_x86_ops.sync_pir_to_irr = NULL;    /* ★ 禁用就清空回调 */

    if (!enable_apicv || !cpu_has_vmx_ipiv())
        enable_ipiv = false;

    return 0;
}
```

### Q: `setup_vmcs_config()` 具体读哪些 MSR？

**文件**: `arch/x86/kvm/vmx/vmx.c:2590`

| MSR | 内容 | `setup_vmcs_config()` 里的读取点 |
|-----|------|------|
| `MSR_IA32_VMX_BASIC` | VMCS revision、VMXON region 大小、true-CTLS 是否可用 | `vmx.c:2743` `rdmsrl(MSR_IA32_VMX_BASIC, basic_msr)` |
| `MSR_IA32_VMX_MISC` | 抢占定时器移位量、可加载 MSRs 数、`SMM/VMFUNC` 等杂项 | `vmx.c:2763` |
| `MSR_IA32_VMX_PINBASED_CTLS` | 引脚控制（外部中断退出/NMI 退出/**Process posted interrupts = bit 7**）| `vmx.c:2689` |
| `MSR_IA32_VMX_PROCBASED_CTLS` | 主 CPU 控制（IO/MSR/HLT/RDTSC/PLE 退出、CR3/CR8 退出等）| `vmx.c:2623` |
| `MSR_IA32_VMX_PROCBASED_CTLS2` | 二级控制（**Virtualize APIC accesses bit 0 / Enable EPT bit 1 / Enable RDTSCP bit 3 / virtualize x2APIC mode bit 4 / Enable VPID bit 5 / unrestricted guest bit 7 / APIC-register virtualization bit 8 / virtual-interrupt delivery bit 9 / PAUSE-loop exiting bit 10 / Enable INVPCID bit 12 / VMCS shadowing bit 14 / PML bit 17 / TSC scaling bit 25 / BUS_LOCK_DETECTION bit 30** 等，逐位见 `arch/x86/include/asm/vmxfeatures.h:62-89`）| `vmx.c:2626-2629` |
| `MSR_IA32_VMX_PROCBASED_CTLS3` | 三级控制（**IPI virtualization bit 4**，`VMX_FEATURE_IPI_VIRT = 3*32+4`，`vmxfeatures.h:92`）| `vmx.c:2676-2679` |
| `MSR_IA32_VMX_EXIT_CTLS` | VM-Exit 控制（Host 地址空间大小/ACK 中断/保存调试寄存器…）| `vmx.c:2683` |
| `MSR_IA32_VMX_ENTRY_CTLS` | VM-Entry 控制（加载 EFER/IA32_PAT/PERF_GLOBAL_CTRL…）| `vmx.c:2699-2701` |
| `MSR_IA32_VMX_EPT_VPID_CAP` | EPT/VPID 能力（4 级页表/WB/INVVPID 与 INVEPT 类型）| `vmx.c:2648` `rdmsr_safe(...)` |

**纠正旧表述（2026-11-25 核查）**：原表把 **Posted Interrupts 归到了 CTLS2**，并把
"虚拟 NMI 退出"归到 pin-based 之外的表述混在一起。按规范与源码，
**"Process posted interrupts" 是 Pin-Based 控制的 bit 7**：

- 规范：intel-vmx.pdf Vol.3C Table 25-5 "Definitions of Pin-Based VM-Execution
  Controls"，bit 7 = "Process posted interrupts"——"If this control is 1, the processor
  treats interrupts with the posted-interrupt notification vector … specially, updating
  the virtual-APIC page with posted-interrupt requests"。
- 源码：`cpu_has_vmx_posted_intr()` 读的就是 pin 表（`capabilities.h:92-95`
  `vmcs_config.pin_based_exec_ctrl & PIN_BASED_POSTED_INTR`），
  `PIN_BASED_POSTED_INTR = VMCS_CONTROL_BIT(POSTED_INTR)`，而
  `VMX_FEATURE_POSTED_INTR = (0*32+7)`（`vmxfeatures.h:21`，word 0 = pin-based ⇒ bit 7）。
- Secondary 控制表（Table 25-7）里没有 "posted" 这一位，别在 CTLS2 里找它。

每个 CTL MSR 的格式都一样：bits 31:0 是「必须为 1」的硬约束（allowed-0），
bits 63:32 是「可以为 1」（allowed-1）。6.12.93 里的实际算法是这段（不是旧版写的
`_vmx_report_error`，该符号在本树不存在）：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:2563-2580 */
static int adjust_vmx_controls(u32 ctl_min, u32 ctl_opt, u32 msr, u32 *result)
{
	u32 vmx_msr_low, vmx_msr_high;
	u32 ctl = ctl_min | ctl_opt;

	rdmsr(msr, vmx_msr_low, vmx_msr_high);

	ctl &= vmx_msr_high; /* bit == 0 in high word ==> must be zero */
	ctl |= vmx_msr_low;  /* bit == 1 in low word  ==> must be one  */

	/* Ensure minimum (required) set of control bits are supported. */
	if (ctl_min & ~ctl)
		return -EIO;

	*result = ctl;
	return 0;
}
```

即：**先砍掉硬件不允许为 1 的位，再强制置上硬件要求为 1 的位**；只要
`ctl_min`（KVM 必需集合，`KVM_REQUIRED_VMX_*`）里有任何一位没落到最终 `ctl` 上，
就返回 `-EIO`，`setup_vmcs_config()` 往上抛 → `vmx_hardware_setup()` 返回 -EIO →
`kvm_x86_vendor_init()` 失败，模块加载不了（不是"降级运行"）。64 位宽的那些
（tertiary controls）另有 `adjust_vmx_controls64()`（`vmx.c:2581-2588`），只做
`ctl_opt & allowed`，没有 must 集合。

> 补充：`error_on_inconsistent_vmcs_config`（`vmx.c:130-131`，默认 1，0444）也在
> `setup_vmcs_config()` 内部，管的是**能力自相矛盾**时怎么办：
> "EPT CAP 存在但 secondary 里没 Enable EPT"（`vmx.c:2651-2661`）、
> "VPID CAP 存在但没 Enable VPID"（`:2662-2671`）、
> "VM-Entry/VM-Exit 控制位成对约束不满足"（`:2705-2719`）。
> 为真 → 直接 `-EIO` 拒绝加载；为假 → 只 `pr_warn_once` 并把相关位清掉继续跑。
> 它跟上面"必需控制位缺失"是两条不同的判定路径。

### Q: 特性依赖链长什么样？

下面每条都按 6.12.93 的实际判据写（左边是开关，右边是 `file:line`）：

```
硬件 capability MSR / VMX-control 位
    │
    ├─ enable_ept       ← cpu_has_vmx_ept() && cpu_has_vmx_ept_4levels()
    │                     && cpu_has_vmx_ept_mt_wb() && cpu_has_vmx_invept_global()
    │                     (vmx.c:8434-8438)
    │   ├─ enable_ept_ad_bits        ← cpu_has_vmx_ept_ad_bits() && enable_ept
    │   │                             (vmx.c:8446-8447)
    │   └─ enable_unrestricted_guest ← cpu_has_vmx_unrestricted_guest() && enable_ept
    │                                  (vmx.c:8449-8450)
    │
    ├─ enable_vpid      ← cpu_has_vmx_vpid() && cpu_has_vmx_invvpid()
    │                     && (cpu_has_vmx_invvpid_single() || ..._global())
    │                     (vmx.c:8430-8432)
    │
    ├─ enable_apicv     ← cpu_has_vmx_apicv()
    │       │              == cpu_has_vmx_apic_register_virt()
    │       │               && cpu_has_vmx_virtual_intr_delivery()
    │       │               && cpu_has_vmx_posted_intr()      (capabilities.h:274-279)
    │       │              三个判据落在两张不同的控制表上（别都当成 Secondary）：
    │       │                · "APIC-register virtualization"  = Secondary bit 8
    │       │                  (capabilities.h:187-191, VMX_FEATURE_APIC_REGISTER_VIRT
    │       │                   = 2*32+8, vmxfeatures.h:70)
    │       │                · "virtual-interrupt delivery"    = Secondary bit 9
    │       │                  (capabilities.h:193-197, VMX_FEATURE_VIRT_INTR_DELIVERY
    │       │                   = 2*32+9, vmxfeatures.h:71)
    │       │                · "Process posted interrupts"     = **Pin-Based** bit 7
    │       │                  (capabilities.h:92-95 读 pin_based_exec_ctrl &
    │       │                   PIN_BASED_POSTED_INTR；
    │       │                   VMX_FEATURE_POSTED_INTR = 0*32+7, vmxfeatures.h:21)
    │       │
    │       ├─ enable_ipiv ← enable_apicv && cpu_has_vmx_ipiv()   (vmx.c:8495-8496)
    │       │              注意 ipiv 的硬件位在 **Tertiary** Exec controls:
    │       │              cpu_has_vmx_ipiv() = cpu_based_3rd_exec_ctrl &
    │       │              TERTIARY_EXEC_IPI_VIRT              (capabilities.h:281-284)
    │       │
    │       └─ Guest 侧 VT-d Posted Interrupt（IRTE 进 Posted 模式）
    │              ← enable_apicv && irq_remapping_cap(IRQ_POSTING_CAP)
    │              (arch/x86/kvm/x86.c:13662 → enable_posted_intr)
    │              —— 这是"IR 层的 PI"，与上面 CPU 侧的 "Process posted
    │                 interrupts" 是一对，缺任一侧都不成立
    │
    └─ flexpriority_enabled ← cpu_has_vmx_flexpriority()
                             == cpu_has_vmx_tpr_shadow()
                              && cpu_has_vmx_virtualize_apic_accesses()
                             (capabilities.h:286-290；vmx.c:8452-8453)
```

**纠正旧表述（2026-11-25 核查，见本章 corrections.md 勘误 4）**：原来这张图写的是
"APICv 要求：虚拟中断投递 + **EOI 虚拟化 + MSR 位图**"。按源码，
`cpu_has_vmx_apicv()`（`capabilities.h:274-279`）的三个合取项是
**APIC-register virtualization + virtual-interrupt delivery + Process posted interrupts**，
既**不含 EOI 虚拟化**，也**不含 MSR 位图**：

```c
/* 来源: arch/x86/kvm/vmx/capabilities.h:274-284 */
static inline bool cpu_has_vmx_apicv(void)
{
	return cpu_has_vmx_apic_register_virt() &&
		cpu_has_vmx_virtual_intr_delivery() &&
		cpu_has_vmx_posted_intr();
}

static inline bool cpu_has_vmx_ipiv(void)
{
	return vmcs_config.cpu_based_3rd_exec_ctrl & TERTIARY_EXEC_IPI_VIRT;
}
```

（补充两点容易混的：

- **EOI 虚拟化不是一道入门判据，也没有 `SECONDARY_EXEC_VIRTUALIZE_APIC_WRITES` 这个符号**
  （6.12.93 全树无此宏）。硬件侧的实现载体是 VMCS 的 `EOI_EXIT_BITMAP0..3`
  （字段号 `arch/x86/include/asm/vmx.h:235-242`；运行期由
  `vmx_load_eoi_exitmap()` `arch/x86/kvm/vmx/vmx.c:6958-6965` 按 LAPIC 的 ISRR 刷进去；
  vCPU 初始化时若 `enable_apicv && lapic_in_kernel()` 先把四个 bitmap 清零，
  `vmx.c:4755-4760`），只在 VID 生效时才有意义。`fasteoi` 参数
  （`vmx.c:111-112`，默认 1，0444）是另一码事：它只是 `handle_apic_access()`
  （`vmx.c:5676-5698`）里对"guest 用 MOV 写 xAPIC EOI 寄存器"这一种 access 走的捷径
  ——直接 `kvm_lapic_set_eoi()` + 跳指令，省掉一遍重量级指令模拟。
- **"FlexPriority 要求 TPR Shadow" 也不完整**：`cpu_has_vmx_flexpriority()`
  = `cpu_has_vmx_tpr_shadow() && cpu_has_vmx_virtualize_apic_accesses()`
  （`capabilities.h:286-290`），后者读的是 Secondary bit 0
  "Virtualize APIC accesses"（`capabilities.h:139-143`）。）

---

## 6. 模块参数：运行时可调的特性开关

### Q: 哪些 VMX 参数可以在运行时改？

**文件**: `arch/x86/kvm/vmx/vmx.c:89-125`

| 参数名（`/sys/module/kvm_intel/parameters/`） | 变量名 | 权限 | 默认 | 注册处 | 作用 |
|--------|--------|------|------|------|------|
| `ept` | `enable_ept` | 0444 | 1 | `vmx.c:98-99` `module_param_named` | Extended Page Tables |
| `eptad` | `enable_ept_ad_bits` | 0444 | 1 | `vmx.c:105-106` | EPT A/D 位 |
| `vpid` | `enable_vpid` | 0444 | 1 | `vmx.c:89-90` | Virtual Processor ID |
| **`enable_apicv`** | `enable_apicv` | 0444 | 1 | `vmx.c:114` `module_param` | APIC 虚拟化（含 PI）|
| **`enable_ipiv`** | `enable_ipiv` | 0444 | 1 | `vmx.c:117` `module_param` | 虚拟 IPI 投递 |
| `nested` | `nested` | 0444 | 1 | `vmx.c:124-125` | 嵌套虚拟化 |
| `unrestricted_guest` | `enable_unrestricted_guest` | 0444 | 1 | `vmx.c:101-103` | 无限制 Guest |
| `flexpriority` | `flexpriority_enabled` | 0444 | 1 | `vmx.c:95-96` | TPR Shadow |
| `emulate_invalid_guest_state` | 同名 | 0444 | 1 | `vmx.c:108-109` | 模拟无效 Guest 状态 |

**纠正旧表述（2026-11-25 核查，见本章 corrections.md 勘误 4）**：原表把这两行写成
"参数名 `apicv` / `ipiv`"。它们是 `module_param()`（不是 `module_param_named()`），
**参数名与变量同名**：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:113-117 */
module_param(enable_apicv, bool, 0444);

bool __read_mostly enable_ipiv = true;
module_param(enable_ipiv, bool, 0444);
```

宿主实测与之一致（不存在 `apicv`/`ipiv` 这两个文件）：

```
$ ls /sys/module/kvm_intel/parameters/ | grep -E 'apicv|ipiv'
enable_apicv          enable_ipiv
$ cat /sys/module/kvm_intel/parameters/enable_apicv   # → Y
$ cat /sys/module/kvm_intel/parameters/enable_ipiv    # → N
$ stat -c %a /sys/module/kvm_intel/parameters/enable_apicv   # → 444
```

还有一个容易踩的细节：`enable_apicv` 这个**变量并不在 vmx.c 里**，它定义在
`arch/x86/kvm/x86.c:229-230`（`bool __read_mostly enable_apicv = true;` +
`EXPORT_SYMBOL_GPL`，属于 kvm.ko），`vmx.c:114` 只是用 `module_param()` 把这个
**已存在变量的符号**注册成 kvm-intel.ko 的参数（全树检索 `module_param.*enable_apicv`
只命中这一处 —— SVM 侧没有重复注册，AMD 用 AVIC 那套自己的开关）。所以
`modprobe kvm_intel enable_apicv=0` 改的是 kvm.ko 的全局量，不是模块私有状态。

**参数名 vs 变量名**：`module_param_named(参数名, 变量名, ...)` 允许两者不同（如 `ept` 参数对应
`enable_ept` 变量），`module_param(变量名, ...)` 则同名 —— 上表最后一列就是区分这两者的依据。

**权限 `0444` 意味着什么？** 模块加载后**不能改**。要在加载时设置：

```bash
modprobe kvm_intel ept=1 nested=1
# 或写入 /etc/modprobe.d/kvm.conf
```

这些参数是**初始化时决策**用的 —— `vmx_hardware_setup()` 读完参数后决定是否启用特性。
运行中再改没意义，因为 VMCS 控制位已经定了。

---

## 7. 模块初始化调用链

### Q: `kvm_intel` 模块加载时都发生了什么？

**文件**: `arch/x86/kvm/vmx/main.c:164-171`

```c
struct kvm_x86_init_ops vt_init_ops __initdata = {
    .hardware_setup = vmx_hardware_setup,    /* ★ 入口 */
    .runtime_ops    = &vt_x86_ops,           /* 运行时回调表 */
    .pmu_ops        = &intel_pmu_ops,
};
```

```
module_init(vmx_init)                       ← vmx.c:8638 / 注册在 vmx.c:8699
  │
  ├→ kvm_is_vmx_supported() + hv_init_evmcs()          ← vmx.c:8642-8650
  │
  ├→ kvm_x86_vendor_init(&vt_init_ops)     ← 调用在 vmx.c:8651，
  │   │     定义在 x86.c:9741（不是 kvm_main.c！）
  │   │     └ 这里才是"把 vt_init_ops 交给通用层"的入口
  │   ├→ kvm_mmu_vendor_module_init()                            ← x86.c:9797
  │   ├→ ops->hardware_setup()                                  ← x86.c:9821
  │   │     └→ vmx_hardware_setup()                     ← vmx.c:8404
  │   │         ├→ setup_vmcs_config()       ← 读 VMX capability MSR（vmx.c:8415）
  │   │         ├→ 特性检测 + 按依赖裁剪（enable_ept/vpid/apicv/ipiv/...）
  │   │         └→ 就地改 vt_x86_ops 的回调（如 sync_pir_to_irr = NULL）
  │   ├→ kvm_ops_update(ops)                                     ← x86.c:9825
  │   │     ├→ memcpy(&kvm_x86_ops, ops->runtime_ops, ...)       ← x86.c:9700
  │   │     └→ static_call_update(kvm_x86_*, ...)                ← 烤进调用点
  │   └→ 逐 CPU 兼容性检查 kvm_x86_check_cpu_compat()         ← x86.c:9827-9828
  │
  ├→ vmx_setup_l1d_flush()                                      ← vmx.c:8662
  ├→ per-CPU INIT_LIST_HEAD + pi_init_cpu()                     ← vmx.c:8666-8670
  │
  └→ kvm_init(sizeof(struct vcpu_vmx), __alignof__(struct vcpu_vmx),
              THIS_MODULE)                                       ← vmx.c:8686
        └→ kvm_main.c:6476
            ├→ kvm_chardev_ops.owner = module                    ← kvm_main.c:6510
            └→ misc_register(&kvm_dev)  → /dev/kvm 出现          ← kvm_main.c:6534
```

**纠正旧表述（2026-11-25 核查，见本章 corrections.md 勘误 4）**：原来这段写的是
`module_init(kvm_intel) → kvm_init(&vt_init_ops) ← kvm_main.c → kvm_arch_hardware_setup()`。
三处与 6.12.93 不符：

1. **没有 `kvm_intel` 这个符号**，模块入口是 `vmx_init()`（`vmx.c:8638`，
   `module_init(vmx_init)` 在 `vmx.c:8699`）。
2. **`kvm_init()` 不接 `ops` 参数**，签名是
   `int kvm_init(unsigned vcpu_size, unsigned vcpu_align, struct module *module)`
   （`include/linux/kvm_host.h:1058`，定义 `virt/kvm/kvm_main.c:6476`）——
   它只负责通用字符设备/vCPU 对象大小，**不做 hardware_setup**。
   把 `vt_init_ops` 交出去的是 `kvm_x86_vendor_init()`（`x86.c:9741`）。
3. **6.12.93 里已经没有 `kvm_arch_hardware_setup()`**（全树无此符号）。
   通用层直接调 `ops->hardware_setup()`（`x86.c:9821`）。
   "开硬件"也挪走了：现在由 `enable_virt_at_load`（`kvm_main.c:5583-5584`，默认 1）
   或**第一个 VM 创建时**经 `kvm_enable_virtualization()`（`kvm_main.c:5693`，
   调用点 `kvm_create_vm()` `kvm_main.c:1228`）→
   `kvm_arch_enable_virtualization_cpu()`（`x86.c:12682`）→
   `kvm_x86_call(enable_virtualization_cpu)()`（`x86.c:12698`）→
   `vmx_enable_virtualization_cpu()`（`vmx.c:2853`，这才是 VMXON 的地方）。

顺序上有个细节值得记：**static_call 烤入发生在 `kvm_x86_vendor_init()` 里，
早于 `kvm_init()` 注册 `/dev/kvm`**——`vmx_init()` 的注释也明说
"Common KVM initialization _must_ come last, after this, /dev/kvm is exposed to
userspace!"（`vmx.c:8682-8685`）。

### Q: 为什么需要 `vt_init_ops` 和 `vt_x86_ops` 两张表？

| 表 | 生命周期 | 用途 |
|----|---------|------|
| `vt_init_ops` | `__initdata`，初始化后释放 | 只给 `kvm_x86_vendor_init()`（`x86.c:9741`）用一次，含 `hardware_setup` |
| `vt_x86_ops` | 回调指针拷进 `static_call`，结构体本身可释放 | 运行期所有 VMX 操作入口 |

分两张表的原因：`hardware_setup` 这类初始化函数只在加载时调用一次，没必要占着运行时
内存。而 `vcpu_run`/`handle_exit` 是每次 VM-Exit 都要跳的，必须常驻 —— 但不是通过
指针数组，而是通过 `static_call` 直接 patch 进调用点，**性能比函数指针高**。

---

## 8. 关键数据结构关系

```
kvm_intel.ko 加载
  │
  ├─ vt_init_ops (一次性)
  │   └→ vmx_hardware_setup()
  │       └→ 读 VMX capability MSRs
  │       └→ 决定 enable_ept/enable_vpid/...
  │
  └─ vt_x86_ops (运行时)
      └→ 拷进 static_call
          └→ kvm_x86_call(vcpu_run) = vmx_vcpu_run()


每 vCPU 状态:
  struct vcpu_vmx
    ├── struct kvm_vcpu vcpu       ← 通用层看到的
    ├── struct loaded_vmcs vmcs01  ← L1 的 VMCS 容器
    │   └── vmcs01.vmcs → 物理页对齐的 VMCS 内存
    ├── struct pi_desc pi_desc     ← PI 描述符 (硬件写, 64B 对齐)
    └── ...

VMCS 在物理内存里长这样 (简化):
  ┌─────────────────────────┐
  │ VMCS revision ID (32b)  │  ← 来自 MSR_IA32_VMX_BASIC
  ├─────────────────────────┤
  │ VMCS Data               │
  │  ├── Host Area          │  ← VM-Exit 后恢复
  │  ├── Guest Area         │  ← VM-Entry 时加载
  │  ├── Control Area       │  ← Pin/CPU/Exit/Entry 控制
  │  ├── Read-only Data     │  ← VM-Exit 信息
  │  └── Guest Non-reg      │
  └─────────────────────────┘
```
