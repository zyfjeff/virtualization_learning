# Phase 1：源码精读注释 - CPU 虚拟化（VMX 层）

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。

---

## 1. VMX 架构分层

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

`__initdata` 让**结构体本身**在初始化后释放。但初始化结束时，KVM 把每个回调指针
拷进了 `static_call_key`：

```c
/* arch/x86/kvm/x86.c:9698 — kvm_ops_update() */
#define __KVM_X86_OP(func) \
    static_call_update(kvm_x86_##func, kvm_x86_ops.func);
/* ... 展开为每个回调做 static_call_update */
```

释放的是**表本身**，不是函数。函数在 `.text` 段里，一直在。这样设计是为了在嵌套虚拟化
需要切换回调表（VMX → SVM 不可能，但 L1 KVM 做 nested VMX 时需要换实现）时保留灵活性。

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

    /* 4. 读 VM-Exit 信息 */
    vmx->exit_qualification = vmcs_read32(EXIT_QUALIFICATION);
    vmx->exit_reason.full   = vmcs_read32(VM_EXIT_REASON);
    ...
}
```

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

SYM_INNER_LABEL(vmx_vmexit, SYM_L_GLOBAL)
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
| — | — | — | — |
| `IO_INSTRUCTION` | `handle_io()` | 用户空间* | `KVM_EXIT_IO` |
| `MMIO` | — | 用户空间 | `KVM_EXIT_MMIO` |
| `CPUID` | `handle_cpuid()` | 用户空间* | 处理后重入 |
| `TRIPLE_FAULT` | `handle_triple_fault()` | 用户空间 | `KVM_EXIT_SHUTDOWN` |

*注：`handle_io()` 和 `handle_cpuid()` 在内核态处理后可能返回 1（重入）或 0（回用户空间），取决于是否模拟完成。

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
    u16                 vpid;              /* VPID（避免 TLB flush） */
    struct hrtimer      preemption_timer;  /* 抢占定时器 */
};
```

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

| MSR | 内容 |
|-----|------|
| `MSR_IA32_VMX_BASIC` | VMCS revision、VMXON region 大小、true-CTLS 是否可用 |
| `MSR_IA32_VMX_PINBASED_CTLS` | 引脚控制（外部中断/NMI/虚拟 NMI 等）|
| `MSR_IA32_VMX_PROCBASED_CTLS` | 主 CPU 控制（IO 退出/MSR 退出/HLT 退出等）|
| `MSR_IA32_VMX_PROCBASED_CTLS2` | 二级控制（EPT/VPID/RDTSCP/ Posted Interrupts 等）|
| `MSR_IA32_VMX_EXIT_CTLS` | VM-Exit 控制（Host 地址空间大小/ACK 中断等）|
| `MSR_IA32_VMX_ENTRY_CTLS` | VM-Entry 控制（加载 EFER/IA32_PAT 等）|
| `MSR_IA32_VMX_EPT_VPID_CAP` | EPT/VPID 能力（4 级页表/WB/INVVPID 类型等）|

每个 CTL MSR 的格式都一样：bits 31:0 是「必须为 1」的硬约束，bits 63:32 是「可以为 1」
的可选能力。KVM 取交集合：`_vmx_report_error` 检查硬件是否满足最低要求。

### Q: 特性依赖链长什么样？

```
硬件 capability MSR
    │
    ├─ EPT 要求：4 级页表 + WB 内存类型 + INVEPT 全局
    │   └─ EPT_AD 要求：EPT + A/D 位支持
    │       └─ unrestricted_guest 要求：EPT
    │
    ├─ VPID 要求：INVVPID（single 或 global）
    │
    ├─ APICv 要求：虚拟中断投递 + EOI 虚拟化 + MSR 位图
    │   └─ IPIv 要求：APICv + 虚拟 IPI 投递
    │       └─ Posted Interrupts 要求：APICv
    │
    └─ FlexPriority 要求：TPR Shadow
```

---

## 6. 模块参数：运行时可调的特性开关

### Q: 哪些 VMX 参数可以在运行时改？

**文件**: `arch/x86/kvm/vmx/vmx.c:89-125`

| 参数名 | 变量名 | 权限 | 默认 | 作用 |
|--------|--------|------|------|------|
| `ept` | `enable_ept` | 0444 | 1 | Extended Page Tables |
| `eptad` | `enable_ept_ad_bits` | 0444 | 1 | EPT A/D 位 |
| `vpid` | `enable_vpid` | 0444 | 1 | Virtual Processor ID |
| `apicv` | `enable_apicv` | 0444 | 1 | APIC 虚拟化（含 PI）|
| `ipiv` | `enable_ipiv` | 0444 | 1 | 虚拟 IPI 投递 |
| `nested` | `nested` | 0444 | 1 | 嵌套虚拟化 |
| `unrestricted_guest` | `enable_unrestricted_guest` | 0444 | 1 | 无限制 Guest |
| `flexpriority` | `flexpriority_enabled` | 0444 | 1 | TPR Shadow |
| `emulate_invalid_guest_state` | 同名 | 0444 | 1 | 模拟无效 Guest 状态 |

**参数名 vs 变量名**：`module_param_named(参数名, 变量名, ...)` 允许两者不同（如 `ept` 参数对应
`enable_ept` 变量），`module_param(变量名, ...)` 则同名。

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
module_init(kvm_intel)
  │
  ├→ kvm_init(&vt_init_ops)              ← kvm_main.c
  │   │
  │   ├→ kvm_arch_hardware_setup()       ← x86.c
  │   │   └→ vt_init_ops.hardware_setup()
  │   │       └→ vmx_hardware_setup()
  │   │           ├→ setup_vmcs_config()       ← 读 VMX MSR
  │   │           ├→ 特性检测 + 裁剪
  │   │           └→ 配置回调表（禁用不用的）
  │   │
  │   ├→ static_call_update(kvm_x86_*, ...) ← 把回调指针烤进代码
  │   │
  │   └→ kvm_chardev_ops 注册（/dev/kvm）
  │
  └→ 完成，可以接受 ioctl 了
```

### Q: 为什么需要 `vt_init_ops` 和 `vt_x86_ops` 两张表？

| 表 | 生命周期 | 用途 |
|----|---------|------|
| `vt_init_ops` | `__initdata`，初始化后释放 | 只给 `kvm_init()` 用一次，含 `hardware_setup` |
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
