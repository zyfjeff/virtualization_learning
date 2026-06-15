# 补充章节：CPU 虚拟化深度

> 基于 Linux 6.12.93 源码
>
> CPU 虚拟化是 KVM 的核心。VMX 提供了硬件辅助的 Guest 隔离，但 CPU 对外暴露的"接口"——
> CPUID、MSR、指令行为——都需要 KVM 软件来虚拟。本章节覆盖三大主题：
> **CPUID 虚拟化**、**MSR 虚拟化**、**特权指令虚拟化**。

---

## 📋 学习目标

完成本章后，你应该能：
1. 解释 CPUID 虚拟化的两种机制（静态过滤 vs 动态拦截）
2. 画出 MSR 访问的完整路径（MSR Bitmap → VM-Exit → KVM处理）
3. 列举至少 10 种触发 VM-Exit 的指令及其 KVM 处理方式
4. 解释 `kvm_x86_ops` 回调表如何连接 x86 通用层和 VMX 实现
5. 理解 `kvm_emulate_instruction()` 在指令模拟中的角色

---

## 🧩 CPU 虚拟化全景

### 为什么需要软件补充？

VMX 解决了最核心的问题：**Guest 代码在 Non-root 模式运行，特权操作触发 VM-Exit**。
但 CPU 对外暴露了三大"信息接口"，硬件不帮你虚拟化：

```
┌─────────────────────────────────────────────────────────────────────┐
│                  CPU 虚拟化的三大挑战                                │
│                                                                     │
│  ① CPUID — "我是什么CPU？"                                         │
│     Guest 执行 CPUID 指令 → 硬件不拦截！直接返回真实CPU信息        │
│     问题: Guest 看到了真实CPU型号/特性, 可能依赖不存在的特性        │
│     解决: CPU_BASED_USE_MSR_BITMAPS 不控制CPUID,                   │
│           KVM通过: CPUID VM-Exit (Secondary Exec Control)          │
│           或 用户空间预过滤 (QEMU KVM_SET_CPUID2)                  │
│                                                                     │
│  ② MSR — "模型特殊寄存器"                                         │
│     数百个MSR, 控制CPU各种特性 (TSC, EFER, APIC_BASE, PAT...)     │
│     问题: Guest 读写 MSR 可能影响Host或看到真实硬件状态            │
│     解决: MSR Bitmap (VMCS字段), 逐MSR控制是否拦截                │
│           拦截的 → VM-Exit → KVM软件模拟                          │
│           不拦截的 → 直接访问物理MSR (高性能)                      │
│                                                                     │
│  ③ 特权指令 — 敏感但不被VMX自动拦截的指令                         │
│     例如: CPUID, INVD, WBINVD, MONITOR, MWAIT, RDTSC, RDPMC       │
│           SGDT, SIDT, SLDT, STR (读取GDT/IDT/LDT/TR)              │
│           IN, OUT (IO端口访问)                                      │
│     解决: 通过 VM-Execution Controls 配置哪些指令触发 VM-Exit      │
│           Exit 后 KVM 模拟指令行为                                 │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 1. CPUID 虚拟化 (cpuid.c)

### 1.1 硬件机制

CPUID 指令比较特殊。VMX 提供了两种处理方式：

```
┌─ 方式一: CPUID VM-Exit (Secondary Exec Control) ─────────────────┐
│  VMCS.Secondary_VM_EXEC_CONTROL bit 12 = "CPUID exiting"         │
│  启用后: 每次 Guest 执行 CPUID → VM-Exit → KVM 处理              │
│  优点: 精确控制每次 CPUID 的返回值                                │
│  缺点: 每次 CPUID 都 VM-Exit, 性能差                              │
│  KVM 使用: 仅在嵌套虚拟化等特殊场景启用                           │
└──────────────────────────────────────────────────────────────────┘

┌─ 方式二: 静态过滤 (主流方式) ───────────────────────────────────┐
│  QEMU 在 vCPU 创建时调用 KVM_SET_CPUID2,                        │
│  将允许的 CPUID 条目列表传给 KVM                                 │
│  Guest 执行 CPUID → 硬件不拦截 → 返回真实值                     │
│  但是! KVM 在 VM-Entry 前检查 CPUID 依赖关系,                   │
│  禁用 Guest 不应看到的功能对应的 VMCS 控制位                     │
│                                                                  │
│  例: 如果 CPUID 告诉 Guest "没有 VMX",                          │
│      KVM 就不会在 VMCS 中暴露嵌套虚拟化能力                      │
│                                                                  │
│  优点: CPUID 不触发 VM-Exit, 性能最优                            │
│  缺点: Guest 看到真实CPUID值 (可能被用户空间预过滤)              │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 KVM CPUID 数据结构

```c
/* 来源: arch/x86/include/uapi/asm/kvm.h */

/*
 * 一个 CPUID 条目
 * QEMU 通过 KVM_SET_CPUID2 传入一组这样的条目
 * KVM 保存后, 在 VM-Entry 时用来过滤/修改 Guest 可见的 CPUID
 */
struct kvm_cpuid_entry2 {
    u32 function;        /* CPUID 叶号 (EAX 输入值) */
    u32 index;           /* 子叶号 (ECX 输入值, 用于有子叶的CPUID) */
    u32 flags;           /* KVM_CPUID_FLAG_* */
    u32 eax;             /* 返回值 EAX */
    u32 ebx;             /* 返回值 EBX */
    u32 ecx;             /* 返回值 ECX */
    u32 edx;             /* 返回值 EDX */
    u32 padding[3];      /* 保留 */
};

/*
 * KVM_SET_CPUID2 的 ioctl 参数
 */
struct kvm_cpuid2 {
    u32 nent;                       /* 条目数量 */
    u32 padding;
    struct kvm_cpuid_entry2 entries[];  /* 可变长度数组 */
};
```

### 1.3 CPUID 虚拟化流程

```
QEMU 配置 CPUID:

  ioctl(vcpu_fd, KVM_SET_CPUID2, &cpuid2)
    │
    ▼
  kvm_vcpu_ioctl_set_cpuid2()          [cpuid.c]
    │
    ├── kvm_cpuid_check_equal()        ← 检查是否与之前的相等
    │
    ├── vcpu->arch.cpuid_entries = 拷贝条目列表
    │   vcpu->arch.cpuid_nent = nent
    │
    └── kvm_update_cpuid_runtime()     ← 运行时更新
        │
        ├── 更新 APIC ID (根据 vcpu_id)
        ├── 更新 XSAVE 相关位
        ├── 更新 TSC 频率相关
        │
        └── kvm_vcpu_after_set_cpuid() ← ★ 关键! 根据CPUID调整VMCS
            │
            ├── 检查嵌套虚拟化是否允许
            ├── 更新 VMCS Secondary Exec Controls
            ├── 更新 VMCS PIN/CPU_BASED Controls
            ├── 更新 EPT/VPID/APICv 相关控制
            └── 更新 MSR Bitmap (x2APIC 相关)


Guest 执行 CPUID:

  ┌─ 如果没有 "CPUID exiting" 控制 ──────────────────────────────┐
  │  Guest 执行 CPUID → 直接返回真实 CPUID 值                    │
  │  (用户空间已通过 KVM_SET_CPUID2 告诉 KVM 允许哪些特性)       │
  │  KVM 在 VM-Entry 前已根据 CPUID 调整了 VMCS 控制位           │
  │  → Guest 即使看到真实 CPUID, 也看不到被 VMCS 禁用的功能      │
  └──────────────────────────────────────────────────────────────┘

  ┌─ 如果有 "CPUID exiting" 控制 (嵌套虚拟化) ──────────────────┐
  │  Guest 执行 CPUID → VM-Exit (EXIT_REASON_CPUID = 10)        │
  │  → handle_cpuid() [vmx.c]                                    │
  │    → kvm_emulate_cpuid()  [x86.c]                            │
  │      → 从 vcpu->arch.cpuid_entries 中查找匹配条目           │
  │      → 设置 EAX/EBX/ECX/EDX 为条目中的值                    │
  │      → 推进 Guest RIP (跳过 CPUID 指令, 2字节)              │
  │  → VM-Entry 继续 Guest                                       │
  └──────────────────────────────────────────────────────────────┘
```

### 1.4 KVM 修改的 CPUID 叶

```
KVM 特别处理的 CPUID 叶:

叶号              功能                        KVM 处理方式
───────────────  ──────────────────────────  ──────────────────────────
0x00000001       基础信息 + 特性位            修改:
                EAX: family/model/stepping    - APIC ID (EBX[31:24]) = vcpu_id
                EBX: brand, CLFLUSH, APIC ID  - 如果禁用了某些特性, 清除对应位
                ECX: SSE3, PCLMUL, ...        - 根据 vcpu->arch.cpuid_entries 返回
                EDX: FPU, SSE, ...

0x00000007       扩展特性 (sub-leaf 0)         修改:
                EBX: AVX2, BMI1, SMEP, ...    - 清除 Guest 不应看到的特性位
                ECX: AVX512, ...              - 例如: 如果不支持嵌套, 清除 VMX 位
                EDX: ...

0x0000000A       PMU (性能监控单元)            修改:
                EAX: PMU version, counters    - 根据 KVM 的 PMU 配置返回
                EDX: fixed counters           - 可能减少可见的计数器数量

0x40000000       Hypervisor 信息              ★ KVM 注入:
                EBX-EDX: "KVMKVMKVM\0\0\0"    - 告诉 Guest "你在虚拟化环境中"
                                               - Leaf 0x40000001+ 提供 KVM 特有信息

0x40000001       KVM 接口版本                 ★ KVM 注入:
                EAX: KVM CPUID 特性位          - KVM_FEATURE_CLOCKSOURCE
                (kvmclock, PV EOI, ..., 详见)  - KVM_FEATURE_PV_UNHALT
                                                - KVM_FEATURE_POLL_CONTROL 等

0x80000001       扩展特性                     修改:
                ECX: LAHF, SVM, ...           - 清除不支持的特性
                EDX: NX, 1GB页, RDTSCP, ...   - 根据 CPU 和 KVM 配置

0x80000002-4     品牌字符串                    直接透传或修改
                "Intel(R) Core(TM) ..."       - QEMU 可以自定义品牌字符串

0x80000008       地址宽度                     可能修改:
                EAX: 物理/虚拟地址位数         - maxphyaddr, maxvirtaddr
```

---

## 2. MSR 虚拟化 (vmx.c, x86.c)

### 2.1 MSR Bitmap 机制

MSR Bitmap 是 VMCS 中的一个 4KB 位图，控制 Guest 对 MSR 的读写是否触发 VM-Exit。

```
MSR Bitmap 布局 (4KB = 4096 bits):

偏移          大小       控制内容
───────────  ────────   ─────────────────────────────────────────
0x000        1024 bits  MSR 0x00000000 - 0x00001FFF 的读拦截
                        bit=0 → 不拦截 (直接读物理MSR)
                        bit=1 → 拦截 (VM-Exit → KVM处理)

0x080        1024 bits  MSR 0xC0000000 - 0xC0001FFF 的读拦截
                        (系统MSR: EFER, STAR, LSTAR, ...)

0x100        1024 bits  MSR 0x00000000 - 0x00001FFF 的写拦截

0x180        1024 bits  MSR 0xC0000000 - 0xC0001FFF 的写拦截

超出范围的 MSR (0x2000-0xBFFFFFFF) 读写总是触发 VM-Exit!
```

### 2.2 MSR 分类与处理策略

```
┌─ 直接透传 (不拦截, 高性能) ──────────────────────────────────────┐
│  MSR                     原因                                     │
│  ──────────────────────  ─────────────────────────────────────── │
│  IA32_SPEC_CTRL          安全相关 (Spectre缓解), 每个vCPU独立值   │
│  IA32_TSC                RDTSC 不经过MSR, 但写TSC需要            │
│  IA32_PRED_CMD           安全相关                                  │
│  x2APIC MSRs (0x800+)    APICv启用时部分透传                      │
│                                                                   │
│  实现: MSR Bitmap 对应位 = 0                                     │
│  性能: 无 VM-Exit, 接近原生速度                                  │
└──────────────────────────────────────────────────────────────────┘

┌─ 完全拦截 (每次读写都 VM-Exit) ─────────────────────────────────┐
│  MSR                     原因                                     │
│  ──────────────────────  ─────────────────────────────────────── │
│  IA32_EFER               控制长模式/NXE, Guest修改需要KVM处理   │
│  IA32_STAR/LSTAR/CSTAR   SYSCALL目标地址, Guest需要自己的值     │
│  IA32_KERNEL_GS_BASE     KERNEL GS base, Guest需要自己的值      │
│  IA32_PAT                Page Attribute Table                    │
│  IA32_TSC_AUX            TSC辅助, RDTSCP使用                     │
│  IA32_PERF_*             性能计数器                              │
│                                                                   │
│  实现: MSR Bitmap 对应位 = 1                                     │
│  处理: vmx_get_msr() / vmx_set_msr() [vmx.c]                   │
│        → kvm_get_msr_common() / kvm_set_msr_common() [x86.c]    │
│  性能: 每次 VM-Exit, 开销大                                      │
└──────────────────────────────────────────────────────────────────┘

┌─ 自动切换 (VM-Entry加载Guest值, VM-Exit恢复Host值) ────────────┐
│  MSR                     原因                                     │
│  ──────────────────────  ─────────────────────────────────────── │
│  IA32_SYSENTER_CS/ESP/EIP  SYSENTER/SYSEXIT 相关                │
│  IA32_STAR/LSTAR/CSTAR     如果用自动加载, 就不需要拦截         │
│  IA32_KERNEL_GS_BASE       同上                                  │
│                                                                   │
│  实现: VMCS Guest/Host MSR load/store lists                      │
│  优点: 无VM-Exit开销, 硬件自动切换                              │
│  限制: 列表最多8个MSR (MAX_NR_LOADSTORE_MSRS=8)                │
└──────────────────────────────────────────────────────────────────┘

┌─ 特殊: x2APIC MSR 透传 (APICv优化) ───────────────────────────┐
│  MSR 范围: 0x800 - 0x8FF (x2APIC 寄存器)                      │
│                                                                   │
│  APICv 启用时:                                                   │
│    部分x2APIC MSR (ICR除外) 透传给Guest                         │
│    通过 VIRTUAL_APIC_PAGE 硬件直接处理                          │
│    MSR Bitmap 对应位 = 0 (不拦截)                               │
│                                                                   │
│  ICR (中断命令寄存器) 例外:                                      │
│    写 ICR 必须拦截 → KVM 模拟 IPI 投递                          │
│    MSR Bitmap 对应位 = 1                                         │
│                                                                   │
│  实现: vmx_update_msr_bitmap_x2apic() [vmx.c]                  │
└──────────────────────────────────────────────────────────────────┘
```

### 2.3 MSR 访问代码路径

```c
/* 来源: arch/x86/kvm/vmx/vmx.c */

/*
 * Guest 读 MSR → 如果 MSR Bitmap 设置拦截 → VM-Exit
 * → handle_read_msr() [vmx.c]
 *   → vmx_get_msr() [vmx.c]
 *     → 特殊MSR直接处理 (TSC, EFER, APIC_BASE, ...)
 *     → 其他: kvm_get_msr_common() [x86.c]
 *       → 根据 MSR 号返回对应的值
 *     → 设置 ECX/RAX:RDX 为返回值
 *     → 推进 Guest RIP (跳过 RDMSR 指令, 2字节)
 *   → VM-Entry
 *
 * Guest 写 MSR → 如果 MSR Bitmap 设置拦截 → VM-Exit
 * → handle_write_msr() [vmx.c]
 *   → vmx_set_msr() [vmx.c]
 *     → 特殊MSR直接处理
 *       例如: IA32_EFER → 检查长模式切换, 更新VMCS
 *             TSC → vmx_write_tsc_offset()
 *             APIC_BASE → 更新 vLAPIC 配置
 *     → 其他: kvm_set_msr_common() [x86.c]
 *     → 推进 Guest RIP (跳过 WRMSR 指令, 2字节)
 *   → VM-Entry
 */
```

### 2.4 关键 MSR 虚拟化表

```
MSR                        地址          KVM 处理
────────────────────────  ────────────  ─────────────────────────────────
IA32_APIC_BASE            0x1B          返回 vLAPIC 基址 + BSP标志
IA32_FEATURE_CONTROL      0x3A          控制 VMX 启用 (Guest通常看到disabled)
IA32_TSC                  0x10          RDTSC直接读(硬件+offset), WRMSR调整offset
IA32_EFER                 0xC0000080    控制长模式(LME)、NXE、SCE
IA32_STAR                 0xC0000081    SYSCALL目标CS/SS
IA32_LSTAR                0xC0000082    64位SYSCALL目标RIP
IA32_CSTAR                0xC0000083    兼容模式SYSCALL目标RIP
IA32_KERNEL_GS_BASE       0xC0000102    SWAPGS使用的GS基址
IA32_TSC_AUX              0xC0000103    RDTSCP辅助值
IA32_PAT                  0x277         Page Attribute Table
IA32_SPEC_CTRL            0x48          Spectre缓解 (透传)
IA32_PRED_CMD             0x49          预测屏障 (透传)
IA32_MISC_ENABLE          0x1A0         杂项特性控制
IA32_PERF_GLOBAL_CTRL     0x38F         性能计数器全局控制
x2APIC (0x800-0x8FF)      various       APICv启用时部分透传
```

---

## 3. 指令虚拟化

### 3.1 VM-Exit 指令分类

```
┌─ VMX 自动拦截 (无需配置) ──────────────────────────────────────┐
│  指令                 Exit原因                  KVM处理         │
│  ──────────────────  ──────────────────────     ──────────────  │
│  VMCALL              EXIT_REASON_VMCALL (18)   hypercall处理   │
│  VMX指令族           EXIT_REASON_VMX*          嵌套VMX模拟     │
│    VMCLEAR, VMLAUNCH, VMPTRLD, VMPTRST,                        │
│    VMREAD, VMWRITE, VMXON, VMXOFF, VMRESUME                    │
│  XSETBV              EXIT_REASON_XSETBV (55)   XCR0更新        │
└────────────────────────────────────────────────────────────────┘

┌─ 通过 Primary Exec Control 配置 ───────────────────────────────┐
│  控制位               拦截的指令/操作           KVM处理         │
│  ──────────────────  ──────────────────────     ──────────────  │
│  IRQ_WINDOW_EXITING   (中断窗口)               enable_irq_window│
│  USE_TSC_OFFSETTING   RDTSC/RDTSCP (偏移)      硬件自动+offset  │
│  HLT_EXITING          HLT                      handle_hlt()     │
│  INVLPG_EXITING       INVLPG                   handle_invlpg()  │
│  MWAIT_EXITING        MWAIT                    handle_mwait()   │
│  RDPMC_EXITING        RDPMC                    handle_rdpmc()   │
│  RDTSC_EXITING        RDTSC/RDTSCP            handle_rdtsc()   │
│  CR3_LOAD_EXITING     MOV CR3 (写)            handle_cr()      │
│  CR3_STORE_EXITING    MOV CR3 (读)            handle_cr()      │
│  CR8_LOAD_EXITING     MOV CR8 (TPR)           handle_cr()      │
│  CR8_STORE_EXITING    MOV CR8 (读)            handle_cr()      │
│  USE_TPR_SHADOW       (TPR虚拟化)             硬件处理         │
│  NMI_EXITING          NMI                     handle_nmi()     │
│  MOV_DR_EXITING       MOV DR                  handle_dr()      │
│  UNCOND_IO_EXITING    IN/OUT (无条件)         handle_io()      │
│  USE_IO_BITMAP        IN/OUT (按bitmap)       handle_io()      │
│  MONITOR_EXITING      MONITOR                 handle_monitor() │
│  PAUSE_EXITING        PAUSE                   handle_pause()   │
└────────────────────────────────────────────────────────────────┘

┌─ 通过 Secondary Exec Control 配置 ─────────────────────────────┐
│  控制位               拦截的指令/操作           KVM处理         │
│  ──────────────────  ──────────────────────     ──────────────  │
│  WBinvd_EXITING       WBINVD                  handle_wbinvd()  │
│  DESCRIPTOR_EXITING   SGDT/SIDT/SLDT/STR     handle_desc()    │
│  RDTSCP              RDTSCP                  handle_rdtscp()  │
│  XSAVE/XRSTOR        XSAVE/XRSTORS           handle_xsaves()  │
│  EPT_VIOLATION       (EPT违规)               handle_ept_viol  │
│  INVPCID             INVPCID                 handle_invpcid() │
│  ENABLE_PML          (脏页日志)              handle_pml_full  │
│  BUS_LOCK_DETECTION  (总线锁)                handle_bus_lock  │
└────────────────────────────────────────────────────────────────┘

┌─ 通过 Exception Bitmap 配置 ───────────────────────────────────┐
│  异常号               触发条件                  KVM处理         │
│  ──────────────────  ──────────────────────     ──────────────  │
│  #DF (8)             Double Fault              handle_df()     │
│  #PF (14)            Page Fault                handle_pf()     │
│  #AC (17)            Alignment Check           handle_ac()     │
│  其他                 根据Guest配置             handle_exception│
└────────────────────────────────────────────────────────────────┘
```

### 3.2 指令模拟框架

```c
/* 来源: arch/x86/kvm/vmx/vmx.c, x86.c */

/*
 * VM-Exit 后的指令模拟有两种路径:
 *
 * 路径A: KVM 快速处理 (大部分指令)
 *   handle_io() / handle_cr() / handle_msr_read() / ...
 *   → 直接在 KVM 中模拟指令效果
 *   → 推进 Guest RIP
 *   → VM-Entry
 *   性能: 无额外开销, 纯内核态处理
 *
 * 路径B: 完整指令解码器 (复杂指令)
 *   kvm_emulate_instruction() [x86.c → emulate.c]
 *   → 解码 Guest 指令 (x86 指令集非常复杂!)
 *   → 模拟执行
 *   → 推进 Guest RIP
 *   → VM-Entry
 *   性能: 开销大 (指令解码器 ~30000 行代码)
 *   用途: MMIO访问, 嵌套VMX, 特殊场景
 *
 * 路径选择:
 *   VM-Exit 原因有专门 handler → 路径A (快速)
 *   无专门 handler 或需要模拟 → 路径B (完整解码)
 */

/*
 * vmx_handle_exit() → 分发到各 handler:
 *
 * static int (*const kvm_vmx_exit_handlers[])(...) = {
 *     [EXIT_REASON_EXCEPTION_NMI]     = handle_exception_nmi,
 *     [EXIT_REASON_EXTERNAL_INTERRUPT] = handle_external_interrupt,
 *     [EXIT_REASON_CPUID]             = handle_cpuid,
 *     [EXIT_REASON_HLT]               = handle_halt,
 *     [EXIT_REASON_IO_INSTRUCTION]    = handle_io,
 *     [EXIT_REASON_CR_ACCESS]         = handle_cr,
 *     [EXIT_REASON_DR_ACCESS]         = handle_dr,
 *     [EXIT_REASON_MSR_READ]          = handle_read_msr,
 *     [EXIT_REASON_MSR_WRITE]         = handle_write_msr,
 *     [EXIT_REASON_VMCALL]            = handle_vmcall,
 *     [EXIT_REASON_EPT_VIOLATION]     = handle_ept_violation,
 *     [EXIT_REASON_EPT_MISCONFIG]     = handle_ept_misconfig,
 *     [EXIT_REASON_WBINVD]            = handle_wbinvd,
 *     [EXIT_REASON_MWAIT_INSTRUCTION] = handle_mwait,
 *     [EXIT_REASON_PAUSE_INSTRUCTION] = handle_pause,
 *     ...
 * };
 */
```

### 3.3 IO 端口模拟

```c
/* 来源: arch/x86/kvm/vmx/vmx.c */

/*
 * Guest 执行 IN/OUT → VM-Exit (EXIT_REASON_IO_INSTRUCTION)
 * → handle_io()
 *   → 读取 EXIT_QUALIFICATION 获取:
 *     - 端口号
 *     - 访问大小 (1/2/4 字节)
 *     - 方向 (IN 还是 OUT)
 *     - 字符串操作 (INS/OUTS)
 *
 *   → 如果有 IO Bitmap 且不拦截:
 *     直接跳过指令 (硬件已执行)
 *
 *   → 如果拦截:
 *     string_io:  kvm_emulate_instruction() (完整解码)
 *     fast_io:    kvm_fast_pio() (快速路径)
 *     特殊端口:   路由到设备模拟
 *       0x20/0xA0: PIC (i8259.c)
 *       0x40-0x43: PIT (i8254.c)
 *       0xCF8/0xCFC: PCI配置空间
 */
```

---

## 4. kvm_x86_ops 回调表

`vt_x86_ops` 是连接 KVM x86 通用层与 VMX 实现的桥梁。

```
┌─ x86.c 通用层 ────────────────────────────────────────────────┐
│                                                                 │
│  kvm_vcpu_ioctl_run() → vcpu_run() → vcpu_enter_guest()      │
│    │                                                            │
│    ├── kvm_x86_call(vcpu_pre_run)(vcpu)                        │
│    │   → vmx_vcpu_pre_run()                                    │
│    │                                                            │
│    ├── kvm_x86_call(vcpu_run)(vcpu, flags)  ★ 核心            │
│    │   → vmx_vcpu_run()                                        │
│    │     → vmx_vcpu_enter_exit()  ← VM-Entry/Exit             │
│    │                                                            │
│    ├── kvm_x86_call(handle_exit)(vcpu, fastpath)              │
│    │   → vmx_handle_exit()                                     │
│    │     → 分发到各 handler                                    │
│    │                                                            │
│    ├── kvm_x86_call(inject_irq)(vcpu, reinjected)             │
│    │   → vmx_inject_irq()  ← 写VMCS Entry Intr Info          │
│    │                                                            │
│    ├── kvm_x86_call(get_msr)(vcpu, msr_info)                  │
│    │   → vmx_get_msr()                                         │
│    │                                                            │
│    ├── kvm_x86_call(set_msr)(vcpu, msr_info)                  │
│    │   → vmx_set_msr()                                         │
│    │                                                            │
│    └── kvm_x86_call(sync_pir_to_irr)(vcpu)                    │
│        → vmx_sync_pir_to_irr()  ← PI PIR→IRR同步             │
│                                                                 │
└────────────────────────────────────────────────────────────────┘

kvm_x86_call() 展开:
  kvm_x86_call(func)(args...)
  = static_call(kvm_x86_##func)(args...)
  = vt_x86_ops.func(args...)  (运行时通过 static_call 跳转)

static_call 是内核的间接调用优化:
  比函数指针快 (无间接跳转预测失败)
  比直接调用灵活 (运行时可选择VMX或SVM实现)
```

---

## 5. 实战: 观察 CPU 虚拟化

### 5.1 查看 Guest 可见的 CPUID

```bash
# 在 Guest 内:
cpuid -1 | head -50

# 关注:
#   leaf 0x40000000: Hypervisor (应看到 "KVMKVMKVM")
#   leaf 0x40000001: KVM 特性
#   leaf 0x00000001: 基础特性 (APIC ID 是否正确)

# 在 Host 上查看 QEMU 传给 KVM 的 CPUID:
# QEMU monitor:
#   info cpuid
# 或通过:
#   cat /proc/<qemu_pid>/cpuid  (如果有cpuid debugfs)
```

### 5.2 ftrace 追踪 CPUID/MSR

```bash
# 追踪 CPUID 拦截 (如果启用了CPUID exiting)
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_cpuid/enable

# 追踪 MSR 访问
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_msr/enable
# 注意: 只有被拦截的MSR才会触发这个tracepoint
# 透传的MSR不会产生trace

# 追踪指令模拟
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_emulate_insn/enable

cat /sys/kernel/debug/tracing/trace_pipe
```

### 5.3 对比 MSR Bitmap 效果

```bash
# 查看当前MSR Bitmap相关的VMCS控制
# (需要debugfs或vmcs dump工具)

# 方法: 通过 ftrace 统计 MSR VM-Exit
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_msr/enable
sleep 5
echo 0 > /sys/kernel/debug/tracing/events/kvm/kvm_msr/enable

# 统计各MSR的访问次数
cat /sys/kernel/debug/tracing/trace | \
    grep kvm_msr | awk '{print $NF}' | sort | uniq -c | sort -rn
```

---

## ✅ 验证清单

完成后确认能回答：
- [ ] CPUID 虚拟化的两种机制是什么？各有什么优缺点？
- [ ] MSR Bitmap 的 4KB 布局是什么？如何控制每个 MSR 的拦截？
- [ ] 列举 5 个直接透传的 MSR 和 5 个必须拦截的 MSR，解释原因
- [ ] VM-Exit 指令处理的两种路径（快速 vs 完整解码）分别在什么场景使用？
- [ ] kvm_x86_ops 中 vcpu_run 和 handle_exit 的调用时机是什么？
- [ ] kvmclock 用的 CPUID 叶号是什么？KVM 注入了哪些半虚拟化特性？
- [ ] x2APIC MSR 在 APICv 启用时如何处理？ICR 为什么例外？
