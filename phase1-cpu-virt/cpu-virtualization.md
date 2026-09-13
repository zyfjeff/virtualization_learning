# 补充章节：CPU 虚拟化深度

> 基于 Linux 6.12.93 源码
>
> CPU 虚拟化是 KVM 的核心。VMX 提供了硬件辅助的 Guest 隔离，但 CPU 对外暴露的"接口"——
> CPUID、MSR、指令行为——都需要 KVM 软件来虚拟。本章节覆盖三大主题：
> **CPUID 虚拟化**、**MSR 虚拟化**、**特权指令虚拟化**。

---

## 📋 学习目标

完成本章后，你应该能：
1. 解释 CPUID 虚拟化的机制（CPUID 总是触发 VM-Exit，KVM 返回虚拟化值）
2. 解释 CPUID Faulting 的作用和工作原理
3. 画出 MSR 访问的完整路径（MSR Bitmap → VM-Exit → KVM处理）
4. 列举至少 10 种触发 VM-Exit 的指令及其 KVM 处理方式
5. 解释 `kvm_x86_ops` 回调表如何连接 x86 通用层和 VMX 实现
6. 理解 `kvm_emulate_instruction()` 在指令模拟中的角色

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
│     Guest 执行 CPUID 指令 → ★ 总是触发 VM-Exit ★                  │
│     (Intel SDM: CPUID 在 VMX non-root 模式下无条件导致 VM-Exit)   │
│     解决: KVM 通过 kvm_emulate_cpuid() 返回虚拟化值               │
│           虚拟化值由 QEMU 通过 KVM_SET_CPUID2 配置                 │
│           Guest 看到的是 QEMU 配置的"虚拟 CPU"信息                │
│                                                                     │
│  ② MSR — "模型特殊寄存器"                                         │
│     数百个MSR, 控制CPU各种特性 (TSC, EFER, APIC_BASE, PAT...)     │
│     问题: Guest 读写 MSR 可能影响Host或看到真实硬件状态            │
│     解决: MSR Bitmap (VMCS字段), 逐MSR控制是否拦截                │
│           拦截的 → VM-Exit → KVM软件模拟                          │
│           不拦截的 → 直接访问物理MSR (高性能)                      │
│                                                                     │
│  ③ 特权指令 — 敏感但不被VMX自动拦截的指令                         │
│     例如: INVD, WBINVD, MONITOR, MWAIT, RDTSC, RDPMC              │
│           SGDT, SIDT, SLDT, STR (读取GDT/IDT/LDT/TR)              │
│           IN, OUT (IO端口访问)                                      │
│     解决: 通过 VM-Execution Controls 配置哪些指令触发 VM-Exit      │
│           Exit 后 KVM 模拟指令行为                                 │
│     注意: CPUID 不属于此类, CPUID 总是触发 VM-Exit                │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 1. CPUID 虚拟化 (cpuid.c)

### 1.1 硬件机制

CPUID 指令在 VMX 中的行为非常明确：

```
┌─ 核心事实: CPUID 总是触发 VM-Exit ─────────────────────────────────┐
│                                                                      │
│  Intel SDM Vol.3, Section 25.1.2 明确规定:                          │
│    "CPUID instruction (always; see Section 25.2)"                   │
│                                                                      │
│  在 VMX non-root 模式下:                                            │
│    Guest 执行 CPUID → 无条件触发 VM-Exit                            │
│    Exit Reason = 10 (EXIT_REASON_CPUID)                             │
│    没有控制位可以关闭此行为（不存在 "CPUID exiting" 控制位）        │
│                                                                      │
│  常见误解:                                                           │
│    ✗ "CPUID 可以不拦截, 直接返回真实值" ← 错误!                    │
│    ✗ "Secondary Exec Control bit 12 是 CPUID exiting" ← 错误!      │
│      (bit 12 实际是 INVPCID)                                        │
│    ✗ "存在两种方式: 拦截和不拦截" ← 错误!                          │
│                                                                      │
│  实际行为:                                                           │
│    ✓ CPUID 总是导致 VM-Exit                                         │
│    ✓ KVM 通过 kvm_emulate_cpuid() 处理                              │
│    ✓ 返回 QEMU 通过 KVM_SET_CPUID2 配置的值                        │
│    ✓ Guest 看到的是完全虚拟化的 CPUID 值                            │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### 1.1.1 CPUID Faulting 机制（补充）

CPUID Faulting 是一个独立于 VMX 的 CPU 特性，用于控制 CPUID 指令在不同特权级的行为。

#### 1.1.1.1 原理

```
┌─ CPUID Faulting 控制 MSR ──────────────────────────────────────────┐
│                                                                      │
│  第一步: 确认 CPU 支持                                              │
│    读 MSR_PLATFORM_INFO (0xCE) bit 31                              │
│    或 CPUID leaf 7, sub-leaf 0, EBX bit 31                        │
│    = 1 表示支持                                                     │
│                                                                      │
│  第二步: 启用                                                       │
│    写 MSR_MISC_FEATURES_ENABLES (0x161) bit 0 = 1                 │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘

┌─ 启用后的行为 ─────────────────────────────────────────────────────┐
│                                                                      │
│  Ring 3 (CPL > 0, 用户态) 执行 CPUID:                              │
│    → ★ 触发 #GP (General Protection Fault) ★                      │
│    → 不是 VM-Exit! 是 #GP 异常                                    │
│    → 用户态程序收到 SIGSEGV, 无法获取 CPUID 信息                  │
│                                                                      │
│  Ring 0 (CPL = 0, 内核态) 执行 CPUID:                              │
│    → 正常执行, 返回 CPUID 值                                       │
│    → 如果同时有 VMX: 会触发 VM-Exit (因为 CPUID 总是 VM-Exit)    │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

#### 1.1.1.2 事件优先级: CPUID Faulting vs VM-Exit

这是理解 CPUID Faulting 在虚拟化中作用的关键：

```
┌─ Intel SDM 事件优先级 ─────────────────────────────────────────────┐
│                                                                      │
│  在 VMX non-root 模式下, 当 Ring 3 执行 CPUID 时:                  │
│                                                                      │
│  1. CPU 执行 CPUID 指令                                            │
│  2. CPUID Faulting 检查 (如果启用 + Ring 3)                       │
│     → 触发 #GP 异常                                                │
│     → ★ #GP 先于 VM-Exit ★                                       │
│     → 指令没有完成, 不会触发 CPUID 的 VM-Exit                     │
│                                                                      │
│  3. Guest 的 #GP 异常处理程序接管                                  │
│     → 通常转换为 SIGSEGV 信号发给用户态程序                       │
│                                                                      │
│  结论: CPUID Faulting #GP 优先于 CPUID VM-Exit                    │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘

┌─ 对比: 启用 vs 未启用 ─────────────────────────────────────────────┐
│                                                                      │
│  未启用 CPUID Faulting:                                            │
│    Ring 3 CPUID → VM-Exit → KVM 返回虚拟化值 → VM-Entry          │
│    每次开销: ~1-2 μs (VM-Exit/Entry 开销)                         │
│                                                                      │
│  启用 CPUID Faulting:                                              │
│    Ring 3 CPUID → #GP (无 VM-Exit!)                               │
│    Guest #GP 处理程序捕获异常                                      │
│    每次开销: ~0.1 μs (#GP 异常处理)                               │
│                                                                      │
│  ★ 性能提升: 10-20 倍 ★                                         │
│                                                                      │
│  注意: Ring 0 的 CPUID 不受影响, 总是 VM-Exit                     │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

#### 1.1.1.3 KVM 中的实现：为什么要暴露 CPUID Faulting？

**核心问题**：CPUID Faulting 是物理 CPU 的特性，KVM 为什么要告诉 Guest "我支持"？

**场景**：考虑**嵌套虚拟化** — Guest 里运行另一个 VMM（如 L1 KVM），这个 L1 VMM 也想给它的 Guest（L2）做 CPUID 虚拟化。

```
┌─────────────────────────────────────────────────────────────┐
│  L2 Guest (Guest 的 Guest)                                  │
│    Ring 3 执行 CPUID                                        │
└──────────────────┬──────────────────────────────────────────┘
                   │ #GP (CPUID Faulting)
                   ▼
┌─────────────────────────────────────────────────────────────┐
│  L1 VMM (Guest 里的 KVM)                                    │
│    拦截 #GP，模拟 CPUID 结果                                │
│    → 可以返回虚拟化的 CPU 信息                              │
└──────────────────┬──────────────────────────────────────────┘
                   │ VM-Exit
                   ▼
┌─────────────────────────────────────────────────────────────┐
│  L0 VMM (宿主 KVM)                                          │
│    处理 VM-Exit，返回给 L1                                  │
└─────────────────────────────────────────────────────────────┘
```

**关键洞察**：

1. **没有 CPUID Faulting**：
   - Ring 3 的 CPUID 直接执行，返回真实硬件信息
   - L1 VMM **无法拦截**，无法做 CPUID 虚拟化
   - L2 Guest 看到真实的 CPU 拓扑/特性，破坏虚拟化抽象

2. **有 CPUID Faulting**：
   - L1 VMM 启用 CPUID Faulting（写 `MSR_MISC_FEATURES_ENABLES`）
   - Ring 3 的 CPUID 触发 #GP
   - L1 VMM 拦截 #GP，**完全控制** CPUID 返回值
   - L2 Guest 看到 L1 VMM 虚拟化的 CPU 信息

**KVM 的设计**：

```c
// 1. 默认启用：告诉 Guest "我支持 CPUID Faulting"
if (kvm_check_has_quirk(vcpu->kvm, KVM_X86_QUIRK_STUFF_FEATURE_MSRS)) {
    vcpu->arch.msr_platform_info = MSR_PLATFORM_INFO_CPUID_FAULT;  // bit 31 = 1
}

// 2. Guest 可以启用（写 MSR_MISC_FEATURES_ENABLES bit 0）
case MSR_MISC_FEATURES_ENABLES:
    if (data & ~MSR_MISC_FEATURES_ENABLES_CPUID_FAULT || ...)
        return 1;  // 拒绝非法值
    vcpu->arch.msr_misc_features_enables = data;
    break;

// 3. CPUID 处理时的检查（防御性）
if (cpuid_fault_enabled(vcpu) && !kvm_require_cpl(vcpu, 0))
    return 1;  // Ring 3 + 已启用 → 不处理（硬件会触发 #GP）
```

**为什么 KVM 默认启用？**

- **向后兼容**：L1 VMM（如旧版 KVM）可能依赖这个特性
- **灵活性**：Guest 可以选择不启用（不写 `MSR_MISC_FEATURES_ENABLES`）
- **嵌套虚拟化支持**：让 Guest 有能力做自己的 CPUID 虚拟化

**实际上**：

- CPUID Faulting #GP 在**硬件层面**发生，优先于 VM-Exit
- Ring 3 CPUID 根本不会到达 `kvm_emulate_cpuid()`
- 代码中的检查是**防御性**的，防止边缘情况（如某些 CPU 不支持）

**总结**：CPUID Faulting 在 KVM 中的实现，本质上是为了**支持嵌套虚拟化的 CPUID 虚拟化**。KVM 默认暴露这个能力，让 L1 VMM 可以拦截 Ring 3 的 CPUID，实现完全的 CPU 信息虚拟化。

#### 1.1.1.4 对虚拟化的影响

```
┌─ 性能影响 ──────────────────────────────────────────────────────────┐
│                                                                      │
│  Guest Ring 3 频繁调用 CPUID 的场景:                               │
│                                                                      │
│  未启用 CPUID Faulting:                                            │
│    每次 CPUID → VM-Exit → KVM 处理 → VM-Entry                    │
│    每秒 100 万次 → ~1-2 秒 CPU 时间用于 VM-Exit                  │
│                                                                      │
│  启用 CPUID Faulting:                                              │
│    每次 CPUID → #GP (无 VM-Exit)                                  │
│    每秒 100 万次 → ~0.1 秒 CPU 时间                              │
│    ★ 性能提升: 10-20 倍 ★                                       │
│                                                                      │
│  Guest Ring 0 调用 CPUID:                                          │
│    启用前后没有区别, 总是 VM-Exit                                  │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘

┌─ 功能影响 ──────────────────────────────────────────────────────────┐
│                                                                      │
│  Guest Ring 0 (内核):                                              │
│    - CPUID 正常工作, 返回虚拟化值                                 │
│    - 没有影响                                                      │
│                                                                      │
│  Guest Ring 3 (用户态):                                            │
│    - CPUID 触发 #GP                                                │
│    - 用户态程序无法直接读取 CPUID                                  │
│    - 必须通过系统调用 (如 getauxval, vDSO) 获取 CPU 信息         │
│    - 某些旧程序可能崩溃 (如果不处理 #GP)                          │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘

┌─ 使用建议 ──────────────────────────────────────────────────────────┐
│                                                                      │
│  建议启用:                                                          │
│    - 安全敏感场景 (防止用户态探测 CPU 信息)                       │
│    - 用户态频繁调用 CPUID 的性能敏感场景                           │
│    - 现代 Guest OS (Linux 4.x+, Windows 10+)                      │
│                                                                      │
│  不建议启用:                                                        │
│    - 运行旧版软件 (依赖 Ring 3 CPUID)                              │
│    - 调试/分析工具需要用户态 CPUID 访问                            │
│    - Guest 内核没有提供替代接口                                     │
│                                                                      │
│  配置方法 (QEMU):                                                   │
│    qemu-system-x86_64 -cpu host,+cpuid_fault ...                  │
│    或                                                               │
│    qemu-system-x86_64 -cpu qemu64,cpuid-fault=on ...              │
│                                                                      │
│  Guest 内验证:                                                      │
│    cat /proc/cpuinfo | grep cpuid_fault                            │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### 1.2 KVM CPUID 数据结构

```c
/* 来源: arch/x86/include/uapi/asm/kvm.h */

/*
 * 一个 CPUID 条目
 * QEMU 通过 KVM_SET_CPUID2 传入一组这样的条目
 * KVM 保存后, 在 Guest 执行 CPUID 触发 VM-Exit 时返回这些值
 * 注意: CPUID 在 VMX non-root 下总是触发 VM-Exit, 所以这些值
 *       总是会返回给 Guest (不存在"不拦截"的情况)
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
            │     (例: 如果 Guest 没有 INVPCID, 禁用 ENABLE_INVPCID)
            ├── 更新 VMCS PIN/CPU_BASED Controls
            ├── 更新 MSR Bitmap (x2APIC 相关)
            ├── 更新 CR4 保留位 (根据 Guest 支持的特性)
            └── 更新 Exception Bitmap (根据 MAXPHYADDR 等)


Guest 执行 CPUID (完整流程):

  ┌─ 总是触发 VM-Exit (无条件) ─────────────────────────────────────┐
  │                                                                  │
  │  Guest 执行 CPUID 指令                                         │
  │    → ★ 无条件触发 VM-Exit (EXIT_REASON_CPUID = 10) ★          │
  │    → 这是 Intel 硬件的固定行为, 没有控制位可以关闭             │
  │                                                                  │
  │  VM-Exit 后 KVM 处理:                                          │
  │    vmx_handle_exit()                                            │
  │      → kvm_vmx_exit_handlers[EXIT_REASON_CPUID]                │
  │      → kvm_emulate_cpuid()  [cpuid.c]                          │
  │        │                                                         │
  │        │  1. 读取 Guest 请求的 leaf (EAX) 和 sub-leaf (ECX)    │
  │        │  2. kvm_cpuid() 查找 vcpu->arch.cpuid_entries[]       │
  │        │  3. 返回 QEMU 配置的值                                │
  │        │  4. 写入 Guest 寄存器 (EAX/EBX/ECX/EDX)               │
  │        │  5. 推进 Guest RIP (跳过 CPUID 指令, 2字节)           │
  │        │                                                         │
  │    → VM-Entry 继续 Guest                                        │
  │                                                                  │
  │  Guest 看到的是: QEMU 配置的虚拟 CPUID 值                      │
  │  不是物理机的真实 CPUID 值                                      │
  │                                                                  │
  └──────────────────────────────────────────────────────────────────┘


拓扑模拟的具体例子 (-smp 8,sockets=2,cores=2,threads=2):

  QEMU 为每个 vCPU 配置 CPUID leaf 0x0B (Extended Topology):

  Level 0 (SMT/线程级):
    EAX = 1        ← 1 bit 用于区分同一 core 内的 thread
    EBX = 2        ← 每个 core 有 2 个 thread
    ECX = 1        ← level 类型 = SMT
    EDX = vcpu的 x2APIC ID

  Level 1 (Core/核心级):
    EAX = 3        ← 2 bits 用于区分同一 package 内的 core
    EBX = 4        ← 每个 package 有 4 个 logical processor (2 cores × 2 threads)
    ECX = 2        ← level 类型 = Core
    EDX = vcpu的 x2APIC ID

  Level 2:
    ECX = 0        ← 没有更多 level

  Guest Linux 解析后得到: 2 sockets × 2 cores × 2 threads = 8 CPUs
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

0x0000000B       ★ 扩展拓扑 (Extended Topology) ★  KVM/QEMU 虚拟化:
                Level 0 (SMT):                - EAX: 线程ID位宽
                  EAX: 位宽                     - EBX: 每core的逻辑处理器数
                  EBX: 逻辑处理器数             - ECX: level类型=1(SMT)
                  ECX: level类型=1              - EDX: x2APIC ID
                  EDX: x2APIC ID              Level 1 (Core):
                Level 1 (Core):                 - EAX: 核心ID位宽
                  EAX: 位宽                     - EBX: 每package的逻辑处理器数
                  EBX: 逻辑处理器数             - ECX: level类型=2(Core)
                  ECX: level类型=2              - EDX: x2APIC ID
                  EDX: x2APIC ID              QEMU 根据 -smp 参数计算这些值
                Guest 由此得知: socket/core/thread 拓扑

0x0000001F       V2扩展拓扑 (更细粒度)         类似 0x0B, 增加 Module/Die 级别
                                                Level 类型: 3=Module, 4=Die

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

### 2.2.1 讨论：哪些 MSR 真的需要拦截？

**问题**：CPUID 已经提供了完整的 CPU 拓扑信息，QEMU 为什么还要拦截 `MSR_CORE_THREAD_COUNT` (0x35)？

**分析**：

```
┌─ CPUID vs MSR_CORE_THREAD_COUNT ──────────────────────────────────┐
│                                                                      │
│  CPUID 提供的拓扑信息:                                             │
│    - Leaf 0x0B: Extended Topology Enumeration                      │
│    - Leaf 0x1F: V2 Extended Topology (multi-die 支持)             │
│    - Leaf 0x04: Deterministic Cache Parameters                     │
│    → 可以报告: 核心数、线程数、APIC ID 拓扑                       │
│                                                                      │
│  MSR_CORE_THREAD_COUNT (0x35):                                     │
│    - bits 15..0:  每个 package 的线程数                            │
│    - bits 31..16: 每个 package 的核心数                            │
│    → 信息与 CPUID 重叠                                              │
│                                                                      │
│  Linux 内核的选择:                                                  │
│    - 不使用 MSR_CORE_THREAD_COUNT                                  │
│    - 通过 CPUID leaf 0x0B/0x1F 获取拓扑                           │
│    - grep 结果: arch/x86/ 中没有 rdmsr(0x35)                      │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘

┌─ QEMU 为什么还要拦截? ──────────────────────────────────────────────┐
│                                                                      │
│  可能的原因:                                                        │
│    1. 兼容性考虑                                                    │
│       - 某些旧的 Guest OS (Windows 早期版本) 可能使用这个 MSR     │
│       - 某些应用程序 (性能分析工具) 可能直接读取                  │
│                                                                      │
│    2. 防御性编程                                                    │
│       - 避免 Guest 读到物理 CPU 的真实值                          │
│       - 即使 Guest 不使用，拦截也比不拦截安全                     │
│       - 防止信息泄露                                                │
│                                                                      │
│    3. 历史遗留                                                      │
│       - 早期 QEMU 实现的遗留                                      │
│       - 当时 CPUID 拓扑模拟不够完善                               │
│                                                                      │
│  结论:                                                              │
│    - 对于现代 Guest OS: 不必要 (它们使用 CPUID)                   │
│    - 对于旧版 Guest OS: 可能有必要                                 │
│    - 对于安全隔离: 有防御价值                                      │
│                                                                      │
│  源码: target/i386/kvm/kvm.c:3182                                  │
│    ret = kvm_filter_msr(s, MSR_CORE_THREAD_COUNT,                  │
│                         kvm_rdmsr_core_thread_count, NULL);        │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

**实际拦截的 MSR 列表**（QEMU 10.1.0-rc2）：

| MSR | 地址 | 用途 | 必要性 |
|-----|------|------|--------|
| `MSR_CORE_THREAD_COUNT` | 0x35 | CPU 拓扑 | ⚠️ 可能不必要（CPUID 已覆盖） |
| `MSR_RAPL_POWER_UNIT` | 0x606 | RAPL 功率单位 | ✅ 必要（避免泄露物理功耗） |
| `MSR_PKG_POWER_LIMIT` | 0x610 | 封装功率限制 | ✅ 必要（虚拟化功率管理） |
| `MSR_PKG_ENERGY_STATUS` | 0x611 | 封装能量状态 | ✅ 必要（避免泄露物理功耗） |
| `MSR_PKG_POWER_INFO` | 0x614 | 封装功率信息 | ✅ 必要（虚拟化功率管理） |

**源码引用**：`target/i386/kvm/kvm.c:3182-3227`

### 2.2.2 QEMU 如何处理 MSR：VMM 视角的完整流程

作为 VMM 实现者，如何决定每个 MSR 的处理方式？让我们从 KVM 的 API 和 QEMU 的实现来理解完整的决策流程。

#### KVM 的两类 MSR

`KVM_GET_MSR_INDEX_LIST` 返回 KVM 能处理的 MSR，分为两类：

```c
// arch/x86/kvm/x86.c:310-315
/*
 * msrs_to_save: MSRs that require host support,
 *               i.e. should be probed via RDMSR.
 *
 * emulated_msrs: MSRs that KVM emulates without
 *                strictly requiring host support.
 */
```

| 类别 | 含义 | 示例 |
|------|------|------|
| `msrs_to_save` | 需要宿主物理 CPU 支持 | `IA32_TSC`, `MSR_STAR`, `MSR_LSTAR`, 性能监控 MSR |
| `emulated_msrs` | KVM 完全模拟，不需要宿主支持 | `MSR_KVM_*`, Hyper-V MSR, VMX MSR, `MSR_PLATFORM_INFO` |

#### QEMU 的处理流程

```
┌─────────────────────────────────────────────────────────────┐
│  1. 启动时获取 KVM 支持的 MSR 列表                          │
│     ioctl(kvm_fd, KVM_GET_MSR_INDEX_LIST, &msr_list)      │
│     → 返回 msrs_to_save[] + emulated_msrs[]               │
└──────────────────┬──────────────────────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────────────────────┐
│  2. 遍历列表，设置 has_msr_* 标志                          │
│     for each msr in msr_list:                              │
│         switch (msr):                                      │
│         case MSR_STAR:       has_msr_star = true; break;   │
│         case MSR_TSC_AUX:    has_msr_tsc_aux = true; break;│
│         case MSR_IA32_XSS:   has_msr_xss = true; break;    │
│         ...                                                │
└──────────────────┬──────────────────────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────────────────────┐
│  3. 查询每个 MSR 宿主支持的值                               │
│     kvm_arch_get_supported_msr_feature(MSR_X):            │
│         → ioctl(KVM_GET_MSRS, { MSR_X })                  │
│         → msrs_to_save: 返回物理 MSR 的值                 │
│         → emulated_msrs: 返回 KVM 模拟的值                │
└──────────────────┬──────────────────────────────────────────┘
                   │
                   ▼
┌─────────────────────────────────────────────────────────────┐
│  4. vCPU 初始化时设置 MSR 给 Guest                          │
│     kvm_init_msrs() / kvm_put_msrs():                      │
│         if (has_msr_star):                                 │
│             add_msr(MSR_STAR, env->star)                  │
│         if (has_msr_tsc_aux):                              │
│             add_msr(MSR_TSC_AUX, env->tsc_aux)            │
│         ...                                                │
│         ioctl(KVM_SET_MSRS, msr_buf)                      │
└─────────────────────────────────────────────────────────────┘
```

#### 源码示例

**步骤 2：设置标志**（`target/i386/kvm/kvm.c:2538-2574`）

```c
for (i = 0; i < kvm_msr_list->nmsrs; i++) {
    switch (kvm_msr_list->indices[i]) {
    case MSR_STAR:
        has_msr_star = true;  // 宿主支持 MSR_STAR
        break;
    case MSR_TSC_AUX:
        has_msr_tsc_aux = true;
        break;
    case MSR_IA32_BNDCFGS:
        has_msr_bndcfgs = true;
        break;
    // ... 其他 MSR
    }
}
```

**步骤 3：查询值**（`target/i386/kvm/kvm.c:589-615`）

```c
uint64_t kvm_arch_get_supported_msr_feature(KVMState *s, uint32_t index)
{
    // 对 msrs_to_save：KVM 读物理 MSR 返回
    // 对 emulated_msrs：KVM 返回模拟值
    
    msr_data.entries[0].index = index;
    ret = kvm_ioctl(s, KVM_GET_MSRS, &msr_data);
    
    return msr_data.entries[0].data;
}
```

**步骤 4：设置给 Guest**（`target/i386/kvm/kvm.c:3922-3955`）

```c
static int kvm_put_msrs(X86CPU *cpu, int level)
{
    kvm_msr_buf_reset(cpu);
    
    // 无条件设置（总是需要）
    kvm_msr_entry_add(cpu, MSR_IA32_SYSENTER_CS, env->sysenter_cs);
    kvm_msr_entry_add(cpu, MSR_PAT, env->pat);
    
    // 条件设置（检查宿主支持）
    if (has_msr_star) {
        kvm_msr_entry_add(cpu, MSR_STAR, env->star);
    }
    if (has_msr_tsc_aux) {
        kvm_msr_entry_add(cpu, MSR_TSC_AUX, env->tsc_aux);
    }
    
    // 提交给 KVM
    return kvm_buf_set_msrs(cpu);  // ioctl(KVM_SET_MSRS)
}
```

#### VMM 设计者的决策原则

作为 VMM 实现者，面对一个 MSR 时的决策流程：

```
Guest 想访问 MSR X
    │
    ▼
1. 这个 MSR 是什么？
    │
    ├─ 已知 MSR，查表决定处理方式
    ├─ 未知 MSR → 默认注入 #GP（安全优先）
    │
    ▼
2. 需要虚拟化吗？
    │
    ├─ 不需要 → 透传（MSR Bitmap 不拦截）
    ├─ 需要 → 继续判断
    │
    ▼
3. 需要 VMM 配置信息吗？
    │
    ├─ 不需要 → KVM 内核处理
    ├─ 需要 → user_space_msr 机制
    │
    ▼
4. 应该暴露给 Guest 吗？
    │
    ├─ 应该 → 上述三种之一
    ├─ 不应该 → 注入 #GP
```

| 类别 | 判断标准 | VMM 工作量 |
|------|---------|-----------|
| 透传 | 只读/VMCS 自动处理 | 低（确认即可） |
| KVM 处理 | 需要虚拟化但不需 VMM 配置 | 无（KVM 默认处理） |
| VMM 处理 | 需要 VMM 配置或跨 VM 信息 | 高（需写 handler） |
| 不暴露 | 安全敏感/硬件特定 | 无（默认 #GP） |

**关键洞察**：大部分 MSR 已经被 KVM 合理分类，VMM 只需要关注：
1. 需要透传的性能关键 MSR
2. 需要自定义模拟逻辑的特殊 MSR（通常很少）

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
│  CPUID               EXIT_REASON_CPUID (10)    kvm_emulate_cpuid│
│                        ★ 无条件拦截, 无控制位 ★  返回虚拟化值   │
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
 *     [EXIT_REASON_CPUID]             = kvm_emulate_cpuid,
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
# 追踪 CPUID 拦截 (CPUID 总是触发 VM-Exit)
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_cpuid/enable
# 注意: 每次 Guest CPUID 都会产生 trace, 数量可能很大!

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

## 6. vCPU 唤醒机制与竞态陷阱

### 6.1 问题背景

VMM 需要在以下场景打断正在 guest 里运行的 vCPU：
- **Pause/Resume**：热迁移、快照时需要冻结 VM
- **设备模拟**：virtio 队列有数据需要处理
- **CPU hotplug**：动态添加/移除 vCPU
- **调试**：gdb 断点、NMI 注入

关键问题：**如何让正在 guest 里执行的 vCPU 立即返回用户态？**

### 6.2 两种机制：immediate_exit vs 信号

#### 6.2.1 immediate_exit 机制

`kvm_run` 结构中的 `immediate_exit` 字段：

```c
// include/uapi/linux/kvm.h:217
struct kvm_run {
    __u8 request_interrupt_window;
    __u8 immediate_exit;        // ← 这个字段
    __u8 padding1[6];
    // ...
};
```

**用法**：VMM 设置 `immediate_exit = 1`，KVM 在 KVM_RUN 入口检查，如果为 1 则立即返回 `-EINTR`。

#### 6.2.2 信号机制

发送实时信号（如 `SIGRTMIN`）给 vCPU 线程：

```rust
// Firecracker 的实现
self.vcpu_thread.kill(sigrtmin())?;
```

信号触发后：
1. 内核设置 `_TIF_SIGPENDING` 标志
2. 如果 vCPU 在 guest 里，硬件中断导致 VM-Exit
3. KVM 检查 `_TIF_SIGPENDING`，调用信号处理器
4. KVM_RUN 返回 `-EINTR`

### 6.3 ⚠️ 关键陷阱：immediate_exit 只在 KVM_RUN 入口检查

**这是最重要的知识点！**

```c
// virt/kvm/kvm_main.c:4491
vcpu->wants_to_run = !READ_ONCE(vcpu->run->immediate_exit__unsafe);
// ↑ 只在 KVM_RUN 入口设置一次

r = kvm_arch_vcpu_ioctl_run(vcpu);

// arch/x86/kvm/x86.c:11676
if (!vcpu->wants_to_run) {  // 检查 wants_to_run
    r = -EINTR;
    goto out;
}

r = vcpu_run(vcpu);  // 进入 vCPU 主循环
```

**`vcpu_run` 的主循环里不检查 `wants_to_run`！**

```c
// arch/x86/kvm/x86.c:11343
static int vcpu_run(struct kvm_vcpu *vcpu)
{
    for (;;) {
        if (kvm_vcpu_running(vcpu)) {      // 只检查 mp_state，不检查 wants_to_run！
            r = vcpu_enter_guest(vcpu);
        } else {
            r = vcpu_block(vcpu);
        }
        
        // ... 处理 VM-Exit ...
        
        if (__xfer_to_guest_mode_work_pending()) {  // 检查信号！
            r = xfer_to_guest_mode_handle_work(vcpu);
            if (r) return r;
        }
    }
}
```

**结论**：
- `immediate_exit` 只能阻止 vCPU **第一次进入** KVM_RUN
- 一旦 vCPU 进入 `vcpu_run` 主循环，`immediate_exit` 就**不再被检查**
- **只有信号能中断正在 guest 里运行的 vCPU**

### 6.4 竞态场景分析

#### 场景 1：vCPU 还没进 KVM_RUN（✅ 安全）

```
vCPU 线程                      用户态线程
────────                      ──────────
                              set_kvm_immediate_exit(1)
                              kill(sigrtmin())
                              
信号被投递并处理
_TIF_SIGPENDING 清除

进入 KVM_RUN
读取 immediate_exit → 1       ← 这里会检查！
设置 wants_to_run = FALSE
检查 wants_to_run → FALSE
返回 -EINTR                   ← 立即退出 ✅
```

即使信号丢失，`immediate_exit` 在 KVM_RUN 入口会被检查，vCPU 立即返回。

#### 场景 2：vCPU 已经在 guest 里（⚠️ 有风险）

```
vCPU 线程（在 guest 里）       用户态线程
────────────────────           ──────────
                               set_kvm_immediate_exit(1)
                               kill(sigrtmin())
                               
VM-Exit（自然发生）
KVM 处理 VM-Exit
                               信号被投递
                               信号处理器执行
                               _TIF_SIGPENDING 清除
                               
检查 _TIF_SIGPENDING → FALSE   ← 信号已处理！
检查 kvm_vcpu_running() → TRUE  ← 不检查 wants_to_run！
VMENTER → 回到 guest            ← 又进入 guest 了！⚠️
```

**问题**：
1. `_TIF_SIGPENDING` 已经被清除（信号在 VM-Exit 处理期间被消费）
2. `kvm_vcpu_running()` 只检查 `mp_state`，不检查 `wants_to_run`
3. **`immediate_exit` 不会在 VM-Exit 后重新检查！**
4. vCPU 重新进入 guest

**信号丢失后的后果**：vCPU 会在 guest 里持续运行，直到下一次自然 VM-Exit（I/O、定时器等）。

### 6.5 三种 VMM 的设计对比

| VMM | 唤醒机制 | 信号丢失风险 | 保底机制 | 超时 |
|-----|---------|-------------|---------|------|
| **QEMU** | 仅 immediate_exit | 无（不用信号） | 不需要 | 无 |
| **Firecracker** | immediate_exit + 信号 | 极低（微秒级窗口） | 自然 VM-Exit | 30 秒 |
| **cloud-hypervisor** | 仅信号 | 高（无 immediate_exit 保底） | 重试 | 1 秒 |

#### 6.5.1 QEMU：纯 immediate_exit

```rust
// QEMU 不使用信号唤醒 vCPU
fn signal_thread(&self) {
    // 不存在这个函数
}
```

**设计哲学**：QEMU 的 vCPU 线程持续运行，不需要真正的 pause/resume。如果需要打断（如设备模拟），通过共享内存标志 + 自然 VM-Exit 检查。

**优点**：无竞态、无超时、最简单  
**缺点**：不支持完整的 pause/resume 语义

#### 6.5.2 Firecracker：双保险

```rust
// src/vmm/src/vstate/vcpu.rs:622-636
pub fn send_event(&mut self, event: VcpuEvent) -> Result<(), VcpuSendEventError> {
    self.event_sender.send(event)?;
    
    self.vcpu_fd.set_kvm_immediate_exit(1);  // 保底
    fence(Ordering::Release);
    
    self.vcpu_thread.kill(sigrtmin())?;       // 立即中断
    Ok(())
}
```

**信号处理器**：只做内存屏障，确保 immediate_exit 可见

```rust
extern "C" fn handle_signal(_: c_int, _: *mut siginfo_t, _: *mut c_void) {
    fence(Ordering::Acquire);  // 确保 immediate_exit 的写可见
}
```

**确认机制**：30 秒超时，无重试

```rust
// src/vmm/src/vstate/vm.rs:297
.recv_timeout(crate::RECV_TIMEOUT_SEC)  // 30 秒！
```

**设计哲学**：
- 信号是"快路径"：立即中断正在 guest 里运行的 vCPU
- immediate_exit 是"保底"：确保即使信号丢失，vCPU 下次退出时也会看到

**优点**：简单可靠  
**缺点**：极端情况下等待 30 秒

#### 6.5.3 cloud-hypervisor：信号 + 重试

```rust
// cloud-hypervisor 的 signal_thread
fn signal_thread(&self) {
    unsafe {
        libc::pthread_kill(handle.as_pthread_t() as _, SIGRTMIN());
    }
    // 没有 set_kvm_immediate_exit！
}

// 重试逻辑
fn wait_until_signal_acknowledged(&self) -> Result<()> {
    let mut count = 0;
    loop {
        if self.vcpu_run_interrupted.load(Ordering::SeqCst) {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(1));
        count += 1;
        if count >= 1000 {
            return Err(Error::SignalAcknowledgeTimeout);  // 1 秒超时
        } else if count % 10 == 0 {
            self.signal_thread();  // 每 10ms 重试
        }
    }
}
```

**设计哲学**：信号是边沿触发（丢失就丢失了），必须重试。

**历史问题**：[Issue #7427](https://github.com/cloud-hypervisor/cloud-hypervisor/issues/7427) - 信号竞态导致死锁

**优点**：完整的 pause/resume 语义  
**缺点**：复杂、有竞态、需要重试

### 6.6 核心知识点总结

```
┌─────────────────────────────────────────────────────────────┐
│              vCPU 唤醒机制的核心陷阱                          │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  1. immediate_exit 只在 KVM_RUN 入口检查一次                 │
│     ↓                                                        │
│     进入 vcpu_run 主循环后不再检查                           │
│     ↓                                                        │
│     无法中断正在 guest 里运行的 vCPU                         │
│                                                              │
│  2. 信号是唯一能中断 guest 执行的机制                        │
│     ↓                                                        │
│     设置 _TIF_SIGPENDING → 触发 VM-Exit → 返回用户态        │
│     ↓                                                        │
│     但信号有竞态窗口（微秒级）                               │
│                                                              │
│  3. 不同 VMM 的设计权衡                                      │
│     ↓                                                        │
│     QEMU：简单（不用信号）                                   │
│     Firecracker：双保险（信号 + immediate_exit）             │
│     cloud-hypervisor：信号 + 重试                            │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### 6.7 源码对照

```bash
# wants_to_run 只在 KVM_RUN 入口检查
grep -n "wants_to_run" /root/code/linux-6.12.93/arch/x86/kvm/x86.c
# 11597:  if (!vcpu->wants_to_run) {  # KVM_RUN 入口
# 11676:  if (!vcpu->wants_to_run) {  # KVM_RUN 主入口

# vcpu_run 主循环不检查 wants_to_run
sed -n '11343,11391p' /root/code/linux-6.12.93/arch/x86/kvm/x86.c | grep wants_to_run
# （无输出）

# XFER_TO_GUEST_MODE_WORK 包含 _TIF_SIGPENDING
grep -A 3 "define XFER_TO_GUEST_MODE_WORK" \
    /root/code/linux-6.12.93/include/linux/entry-kvm.h
# _TIF_NEED_RESCHED | _TIF_SIGPENDING | _TIF_NOTIFY_SIGNAL | ...
```

---

## ✅ 验证清单

完成后确认能回答：
- [ ] CPUID 在 VMX non-root 模式下的行为是什么？为什么没有 "CPUID exiting" 控制位？
- [ ] CPUID Faulting 是什么？启用后 Ring 0 和 Ring 3 的 CPUID 行为分别是什么？
- [ ] Guest 的 CPU 拓扑信息（socket/core/thread）是如何通过 CPUID leaf 0x0B 模拟的？
- [ ] MSR Bitmap 的 4KB 布局是什么？如何控制每个 MSR 的拦截？
- [ ] 列举 5 个直接透传的 MSR 和 5 个必须拦截的 MSR，解释原因
- [ ] VM-Exit 指令处理的两种路径（快速 vs 完整解码）分别在什么场景使用？
- [ ] kvm_x86_ops 中 vcpu_run 和 handle_exit 的调用时机是什么？
- [ ] kvmclock 用的 CPUID 叶号是什么？KVM 注入了哪些半虚拟化特性？
- [ ] x2APIC MSR 在 APICv 启用时如何处理？ICR 为什么例外？
- [ ] immediate_exit 在什么时候被检查？为什么不能中断正在 guest 里运行的 vCPU？
- [ ] 信号如何中断 vCPU？_TIF_SIGPENDING 在哪个检查点被处理？
- [ ] 三种 VMM（QEMU/Firecracker/cloud-hypervisor）的 vCPU 唤醒策略有什么区别？各自的优缺点是什么？
