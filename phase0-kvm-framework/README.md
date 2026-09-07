# Phase 0：KVM框架层深度解析

> 基于 Linux 6.12.93 内核源码 | 预计学习时间：1-2 周
>
> **重要性**：这是理解KVM内核态实现的基础。所有后续阶段（CPU/内存/中断虚拟化）都建立在这个框架之上。

---

## 📋 学习目标

本阶段深入分析KVM的核心框架代码，理解从用户空间ioctl到硬件执行的完整路径。

完成本阶段后，你应该能够：
1. 画出 `ioctl(KVM_RUN)` 到 `VMENTER` 指令的完整调用链
2. 解释 `struct kvm` 和 `struct kvm_vcpu` 的关键字段及其作用
3. 理解KVM的并发模型（vCPU线程、MMU锁、irq_lock）
4. 掌握halt-polling机制的工作原理和调优方法
5. 分析memslot管理和GPA→HVA→HPA的转换流程
6. 对比用户态VMM与KVM内核态实现的差异

---

## 📂 本章文件

| 文件 | 内容 |
|------|------|
| `README.md` | 本文件：KVM 框架层学习主线 + 整体架构 |
| `annotations.md` | 源码精读：`kvm_dev_ioctl()` → `vcpu_enter_guest()` 全链路逐函数注解，含 halt-polling |
| `kvm-framework.md` | ★ VMM 视角深度对比：设计差异 / 数据流 / 并发模型 / 内存管理 |
| `practice/` | ★ 实战练习：VM 生命周期 / vCPU 调度 / memslot / 性能对比（手工步骤形式） |

---

## 🏗️ KVM整体架构

### 1.1 从VMM专家视角看KVM

作为用户态VMM开发者，你已经熟悉以下模式：

```
┌─ 用户态VMM (QEMU/crosvm) ──────────────────────────────────┐
│                                                              │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────────┐     │
│  │ 设备模拟     │  │ 内存管理      │  │ vCPU线程        │     │
│  │ (网卡/磁盘)  │  │ (mmap+ioctl) │  │ (ioctl KVM_RUN) │     │
│  └──────┬──────┘  └──────┬───────┘  └────────┬───────┘     │
│         │                │                    │              │
│         └────────────────┼────────────────────┘              │
│                          │                                   │
│                    ioctl(KVM_*)                                │
│                          │                                   │
└──────────────────────────┼──────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────┐
│                    KVM 内核模块                                │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              KVM 框架层 (kvm_main.c)                    │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────────────┐     │ │
│  │  │ ioctl处理 │  │ VM/vCPU  │  │ 内存slot管理      │     │ │
│  │  │ 分发      │  │ 生命周期 │  │ (memslot, rmap)  │     │ │
│  │  └────┬─────┘  └────┬─────┘  └────────┬─────────┘     │ │
│  │       │              │                  │               │ │
│  │       └──────────────┼──────────────────┘               │ │
│  └──────────────────────┼──────────────────────────────────┘ │
│                         │                                    │
│  ┌──────────────────────▼──────────────────────────────────┐ │
│  │              架构相关层 (x86.c)                           │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────────────┐     │ │
│  │  │ vCPU运行 │  │ 中断注入 │  │ MSR/IO拦截处理    │     │ │
│  │  │ 主循环   │  │ 框架     │  │                   │     │ │
│  │  └────┬─────┘  └────┬─────┘  └────────┬─────────┘     │ │
│  └───────┼──────────────┼──────────────────┼──────────────┘ │
│          │              │                  │                │
│  ┌───────▼──────────────▼──────────────────▼──────────────┐ │
│  │              VMX/SVM 实现层 (vmx.c)                     │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────────────┐     │ │
│  │  │ VMCS管理  │  │ VM-Entry │  │ VM-Exit处理       │     │ │
│  │  │           │  │ /Exit    │  │                   │     │ │
│  │  └──────────┘  └──────────┘  └──────────────────┘     │ │
│  └────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
                           │
                           ▼
                    ┌─────────────┐
                    │   硬件       │
                    │ VT-x/AMD-V  │
                    └─────────────┘
```

### 1.2 关键差异：用户态VMM vs KVM内核态

| 方面 | 用户态VMM (QEMU) | KVM内核态 |
|------|------------------|-----------|
| **VMCS管理** | 通过ioctl(KVM_SET_SREGS)等间接设置 | 直接`vmcs_write()`，零开销 |
| **VM-Exit处理** | ioctl返回到用户态，需要上下文切换 | 内核态直接处理，快速路径无切换 |
| **内存映射** | mmap共享内存 + ioctl设置memslot | EPT直接映射，支持并发页错误 |
| **中断注入** | ioctl(KVM_INTERRUPT)返回到用户态 | 直接写VMCS VM_ENTRY_INTR_INFO |
| **性能** | 每次VM-Exit都需要用户态/内核态切换 | 快速路径（中断注入、IO）无切换 |

**关键洞察**：KVM的设计目标是**最小化VM-Exit的处理开销**。对于简单的VM-Exit（如外部中断、HLT），KVM在内核态直接处理并重新进入Guest，避免返回用户态。只有复杂的VM-Exit（如MMIO、CPUID）才返回用户态由QEMU处理。

---

## 📖 源码阅读路线

### 推荐阅读顺序

```
Step 1: 核心数据结构（理解KVM的"对象模型"）
├── include/linux/kvm_host.h          ← struct kvm, struct kvm_vcpu
├── arch/x86/include/asm/kvm_host.h   ← struct kvm_vcpu_arch, struct kvm_arch
└── arch/x86/kvm/vmx/vmx.h            ← struct vcpu_vmx (VMX扩展)

Step 2: 模块初始化和ioctl入口
├── virt/kvm/kvm_main.c               ← kvm_dev_ioctl(), kvm_vcpu_ioctl()
├── arch/x86/kvm/x86.c                ← kvm_arch_vcpu_ioctl_run()
└── arch/x86/kvm/vmx/main.c           ← vt_x86_ops回调表

Step 3: vCPU运行主循环（最核心！）
├── arch/x86/kvm/x86.c                ← vcpu_run(), vcpu_enter_guest()
├── arch/x86/kvm/vmx/vmx.c            ← vmx_vcpu_run()
└── arch/x86/kvm/vmx/vmenter.S        ← __vmx_vcpu_run() 汇编入口

Step 4: 内存管理和中断框架
├── virt/kvm/kvm_main.c               ← memslot管理, kvm_set_memory_region()
├── arch/x86/kvm/x86.c                ← kvm_arch_vcpu_ioctl_get/set_sregs()
└── arch/x86/kvm/irq_comm.c           ← 中断路由和投递
```

### 关键函数索引

| 函数名 | 文件 | 作用 |
|--------|------|------|
| `kvm_dev_ioctl()` | kvm_main.c:4666 | KVM设备ioctl入口 |
| `kvm_vcpu_ioctl()` | kvm_main.c:4352 | vCPU ioctl入口 |
| `kvm_arch_vcpu_ioctl_run()` | x86.c:4375 | x86 vCPU运行入口 |
| `vcpu_run()` | x86.c:3417 | ★ vCPU运行主循环 |
| `vcpu_enter_guest()` | x86.c:3319 | 进入Guest前的准备工作 |
| `kvm_arch_vcpu_runnable()` | x86.c:3281 | 检查vCPU是否可运行 |
| `kvm_vcpu_block()` | kvm_main.c:3480 | vCPU阻塞（halt） |
| `kvm_vcpu_halt()` | kvm_main.c:3428 | halt-polling + 阻塞 |

---

## 🔬 核心数据结构

### 1. struct kvm - VM实例

```c
/* 来源: include/linux/kvm_host.h */

struct kvm {
    /* === 框架层字段 === */
    struct mm_struct *mm;               /* 用户空间内存映射 */
    spinlock_t mmu_lock;                /* MMU操作锁 */
    struct mutex slots_lock;            /* memslot操作锁 */
    struct kmem_cache *private_mem_cache; /* 私有内存缓存 */
    
    /* === VM状态 === */
    atomic_t online_vcpus;              /* 在线vCPU数量 */
    struct list_head vm_list;           /* 全局VM链表 */
    struct pid *pid;                    /* 创建VM的进程ID */
    
    /* === 内存管理 === */
    struct kvm_memslots __rcu *memslots; /* 内存slot表 (RCU保护) */
    struct srcu_struct srcu;            /* memslot访问的SRCU锁 */
    
    /* === vCPU管理 === */
    struct list_head vcpus;             /* vCPU链表 */
    struct kvm_vcpu *vcpus[KVM_MAX_VCPUS]; /* vCPU数组 */
    
    /* === 中断管理 === */
    struct kvm_irq_routing_table __rcu *irq_routing; /* 中断路由表 */
    struct kvm_io_bus *buses[KVM_NR_BUSES]; /* IO总线（PIO/MMIO） */
    struct kvm_pic *vpic;               /* 虚拟PIC */
    struct kvm_ioapic *vioapic;         /* 虚拟IOAPIC */
    
    /* === 架构相关 === */
    struct kvm_arch arch;               /* x86架构扩展 */
    
    /* === 统计信息 === */
    struct kvm_stat_data stat;          /* VM统计信息 */
    
    /* ... 省略其他字段 ... */
};
```

**关键字段解析**：

```
┌──────────────────────────────────────────────────────────────┐
│  struct kvm 核心字段关系                                      │
│                                                              │
│  ┌─────────────┐                                             │
│  │ struct kvm  │                                             │
│  └──────┬──────┘                                             │
│         │                                                     │
│         ├──→ memslots (RCU保护)                               │
│         │    └──→ struct kvm_memslots                         │
│         │         └──→ slots[] (内存slot数组)                 │
│         │              └──→ struct kvm_memory_slot            │
│         │                   ├── id (slot编号)                 │
│         │                   ├── gpa (GPA起始)                 │
│         │                   ├── npages (页数)                 │
│         │                   ├── userspace_addr (HVA)          │
│         │                   └── arch (架构相关字段)           │
│         │                                                     │
│         ├──→ vcpus[]                                          │
│         │    └──→ struct kvm_vcpu *                           │
│         │         ├── vcpu_id                                 │
│         │         ├── cpu (当前pCPU)                          │
│         │         ├── arch (x86扩展)                          │
│         │         └── run (kvm_run共享内存)                   │
│         │                                                     │
│         └──→ irq_routing (中断路由表)                         │
│              └──→ struct kvm_irq_routing_table                │
│                   └──→ entries[] (路由条目)                   │
│                        └──→ struct kvm_irq_routing_entry      │
│                             ├── gsi (全局中断号)              │
│                             ├── type (IRQCHIP/MSI)            │
│                             └── u (目标信息)                  │
└──────────────────────────────────────────────────────────────┘
```

### 2. struct kvm_vcpu - 虚拟CPU

```c
/* 来源: include/linux/kvm_host.h */

struct kvm_vcpu {
    /* === 框架层字段 === */
    struct kvm *kvm;                    /* 所属VM */
    int vcpu_id;                        /* vCPU编号 */
    int cpu;                            /* 当前运行的pCPU (-1=未运行) */
    struct kvm_run *run;                /* 与用户空间共享的kvm_run结构 */
    
    /* === 寄存器状态 === */
    unsigned long regs[VCPU_REGS_NR];   /* 通用寄存器缓存 */
    unsigned long regs_avail;           /* 可用寄存器位图 */
    unsigned long regs_dirty;           /* 脏寄存器位图 */
    
    /* === 请求和标志 === */
    unsigned long requests;             /* 待处理请求位图 */
    bool guest_mode;                    /* 是否在Guest模式 */
    bool preempted;                     /* 是否被抢占 */
    
    /* === 中断相关 === */
    struct kvm_vcpu_arch arch;          /* x86架构扩展 */
    
    /* === 调度相关 === */
    wait_queue_head_t wq;               /* 等待队列（halt时阻塞） */
    bool ready;                         /* 是否可运行 */
    
    /* === 统计信息 === */
    u64 stat.exits;                     /* VM-Exit次数 */
    
    /* ... 省略其他字段 ... */
};
```

### 3. struct kvm_vcpu_arch - x86架构扩展

```c
/* 来源: arch/x86/include/asm/kvm_host.h */

struct kvm_vcpu_arch {
    /* === 寄存器缓存 === */
    u64 cr0;                            /* CR0控制寄存器 */
    u64 cr2;                            /* CR2（页错误地址） */
    u64 cr3;                            /* CR3（页表基址） */
    u64 cr4;                            /* CR4控制寄存器 */
    u64 efer;                           /* 扩展特性寄存器 */
    
    /* === 段寄存器 === */
    struct kvm_segment sregs[8];        /* CS/DS/ES/FS/GS/SS/TR/LDTR */
    struct kvm_dtable idt;              /* IDT寄存器 */
    struct kvm_dtable gdt;              /* GDT寄存器 */
    
    /* === 中断相关 === */
    struct kvm_lapic *apic;             /* 虚拟LAPIC */
    u32 apic_base;                      /* LAPIC基址MSR */
    unsigned long irq_pending;          /* 待处理中断位图 */
    
    /* === MMU === */
    struct kvm_mmu *mmu;                /* 当前MMU上下文 */
    struct kvm_mmu root_mmu;            /* 根MMU（Guest分页） */
    struct kvm_mmu *walk_mmu;           /* 页表遍历MMU */
    
    /* === 时钟 === */
    u64 tsc_offset;                     /* TSC偏移 */
    u64 tsc_scaling_ratio;              /* TSC缩放比例 */
    u64 last_host_tsc;                  /* 上次Host TSC */
    
    /* === 调度相关 === */
    u64 last_blocking_time;             /* 上次阻塞时间 */
    bool halt_poll_allowed;             /* 是否允许halt-polling */
    
    /* ... 省略其他字段 ... */ */
};
```

---

## 🚀 ioctl处理流程

### 1. KVM_CREATE_VM - 创建虚拟机

```
QEMU: ioctl(kvm_fd, KVM_CREATE_VM, 0)
    │
    ▼
kvm_dev_ioctl() [kvm_main.c:5535]
    │
    ├── case KVM_CREATE_VM:            [kvm_main.c:5546]
    │       └── kvm_create_vm()
    │           │
    │           ├── kvm = kzalloc(sizeof(struct kvm))
    │           │   ← 分配VM结构
    │           │
    │           ├── kvm_arch_init_vm(kvm, flags)
    │           │   └── kvm_vm_init_vmcs(kvm) [vmx/vmx.c]
    │           │       └── 分配VMCS shadow区域
    │           │
    │           ├── kvm_init_mmu(kvm)
    │           │   └── 初始化MMU上下文
    │           │
    │           ├── kvm_create_vm_debugfs(kvm)
    │           │   └── 创建debugfs目录
    │           │
    │           ├── anon_inode_getfile("kvm-vm", &kvm_vm_fops)
    │           │   └── 创建VM文件描述符
    │           │
    │           └── fd_install(fd, file)
    │               └── 返回fd给QEMU
    │
    └── return fd (VM文件描述符)
```

**VMM视角**：用户态VMM通过`open("/dev/kvm")`获取KVM fd，然后调用`ioctl(kvm_fd, KVM_CREATE_VM)`创建VM。KVM在内核态分配`struct kvm`，并返回一个文件描述符供后续ioctl使用。

### 2. KVM_CREATE_VCPU - 创建虚拟CPU

```
QEMU: ioctl(vm_fd, KVM_CREATE_VCPU, vcpu_id)
    │
    ▼
kvm_vm_ioctl() [kvm_main.c:5160]      ← ★ 建 vCPU 走的是 **VM fd**，不是 vcpu fd
    │
    ├── case KVM_CREATE_VCPU:          [kvm_main.c:5170]
    │       └── kvm_vm_ioctl_create_vcpu(kvm, vcpu_id)   [kvm_main.c:4217]
    │           │
    │           ├── vcpu = kvm_arch_vcpu_create(kvm, vcpu_id)
    │           │   └── kvm_vcpu_init(vcpu, kvm, vcpu_id)
    │           │       ├── vcpu->kvm = kvm
    │           │       ├── vcpu->vcpu_id = vcpu_id
    │           │       ├── vcpu->cpu = -1 (未绑定pCPU)
    │           │       └── init_waitqueue_head(&vcpu->wq)
    │           │
    │           ├── kvm_arch_vcpu_create(vcpu)
    │           │   └── vmx_create_vcpu(vcpu) [vmx/vmx.c]
    │           │       ├── vmx = kzalloc(sizeof(struct vcpu_vmx))
    │           │       ├── alloc_vmcs(vmx, false)
    │           │       │   └── 分配VMCS区域（4KB对齐）
    │           │       ├── vmx_vcpu_setup(vmx)
    │           │       │   └── 初始化VMCS字段
    │           │       └── vcpu->arch.apic = kvm_create_lapic(vcpu)
    │           │           └── 创建虚拟LAPIC
    │           │
    │           ├── kvm_create_vcpu_debugfs(vcpu)
    │           │
    │           ├── kvm->vcpus[vcpu_id] = vcpu
    │           │   └── 添加到vCPU数组
    │           │
    │           ├── anon_inode_getfile("kvm-vcpu", &kvm_vcpu_fops)
    │           │   └── 创建vCPU文件描述符
    │           │
    │           └── fd_install(fd, file)
    │               └── 返回fd给QEMU
    │
    └── return fd (vCPU文件描述符)
```

### 3. KVM_RUN - 运行虚拟CPU（最核心！）

```
QEMU: ioctl(vcpu_fd, KVM_RUN, 0)
    │
    ▼
kvm_vcpu_ioctl() [kvm_main.c:4445]
    │
    ├── case KVM_RUN:                  [kvm_main.c:4471]
    │       └── kvm_arch_vcpu_ioctl_run(vcpu)   ← 6.12 起**只有一个参数**，
    │               [x86.c:11579]                  `struct kvm_run *` 已从签名里去掉
    │           │
    │           ├── vcpu_load(vcpu)
    │           │   └── 绑定vCPU到当前pCPU
    │           │
    │           ├── r = vcpu_run(vcpu)
    │           │   │
    │           │   ├── while (1) {
    │           │   │       │
    │           │   │       ├── vcpu->arch.mp_state == KVM_MP_STATE_HALTED?
    │           │   │       │   └── kvm_vcpu_halt(vcpu)
    │           │   │       │       ├── halt-polling（轮询一段时间）
    │           │   │       │       └── 如果仍无事件，阻塞等待
    │           │   │       │
    │           │   │       ├── vcpu_enter_guest(vcpu)
    │           │   │       │   │
    │           │   │       │   ├── vcpu->arch.mp_state = KVM_MP_STATE_RUNNABLE
    │           │   │       │   │
    │           │   │       │   ├── kvm_x86_call(vcpu_pre_run)(vcpu)
    │           │   │       │   │   └── vmx_vcpu_pre_run()
    │           │   │       │   │       └── 检查Guest状态有效性
    │           │   │       │   │
    │           │   │       │   ├── kvm_x86_call(vcpu_run)(vcpu)
    │           │   │       │   │   └── vmx_vcpu_run(vcpu) [vmx/vmx.c]
    │           │   │       │   │       │
    │           │   │       │   │       ├── 同步脏寄存器到VMCS
    │           │   │       │   │       │   └── vmcs_writel(GUEST_RSP, vcpu->arch.regs[RSP])
    │           │   │       │   │       │   └── vmcs_writel(GUEST_RIP, vcpu->arch.regs[RIP])
    │           │   │       │   │       │
    │           │   │       │   │       ├── vmx_vcpu_enter_exit(vcpu)
    │           │   │       │   │       │   └── __vmx_vcpu_run() [vmenter.S]
    │           │   │       │   │       │       │
    │           │   │       │   │       │       ├── 保存Host寄存器
    │           │   │       │   │       │       ├── vmcs_writel(HOST_RSP, ...)
    │           │   │       │   │       │       ├── vmcs_writel(HOST_RIP, ...)
    │           │   │       │   │       │       │
    │           │   │       │   │       │       ├── ★ VMENTER 指令
    │           │   │       │   │       │       │   └── CPU进入Guest模式
    │           │   │       │   │       │       │
    │           │   │       │   │       │       ├── Guest执行...
    │           │   │       │   │       │       │
    │           │   │       │   │       │       ├── ★ VM-Exit 触发
    │           │   │       │   │       │       │   └── CPU回到Host模式
    │           │   │       │   │       │       │
    │           │   │       │   │       │       └── 读取Exit信息到vmx结构
    │           │   │       │   │       │
    │           │   │       │   │       └── return exit_fastpath
    │           │   │       │   │
    │           │   │       │   ├── r = vcpu_handle_exit(vcpu, exit_fastpath)
    │           │   │       │   │   └── kvm_x86_call(handle_exit)(vcpu, exit_fastpath)
    │           │   │       │   │       └── vmx_handle_exit()
    │           │   │       │   │           └── 根据exit_reason分发处理
    │           │   │       │   │
    │           │   │       │   └── return r
    │           │   │       │
    │           │   │       ├── if (r <= 0) break
    │           │   │       │   └── 需要返回用户空间（MMIO等，HLT默认不返回）
    │           │   │       │
    │           │   │       ├── signal_pending(current)?
    │           │   │       │   └── break (有信号需要处理)
    │           │   │       │
    │           │   │       └── 继续循环，重新进入Guest
    │           │   │   }
    │           │   │
    │           │   └── return r
    │           │
    │           ├── vcpu_put(vcpu)
    │           │   └── 解绑vCPU从pCPU
    │           │
    │           └── return r
    │
    └── return r (返回到QEMU)
```

**关键洞察**：
- `vcpu_run()`是一个无限循环，只有在特定条件下才会退出（返回用户空间）
- 快速路径（如外部中断、HLT）在内核态直接处理，不返回用户空间
- 慢速路径（如MMIO、CPUID）返回用户空间，由QEMU处理

---

## 🔍 VM-Exit 原因详解

VM-Exit 是 KVM 虚拟化的核心机制。当 Guest 执行某些敏感操作或触发特定事件时，CPU 硬件会从 Guest 模式切换回 Host 模式，并将控制权交给 KVM。理解常见的 VM-Exit 原因对性能调优和问题诊断至关重要。

### 1. Intel VM-Exit 硬件原因（Exit Reason）

Intel VT-x 定义了硬件级别的 VM-Exit 原因，存储在 VMCS 的 `VM_EXIT_REASON` 字段中。以下是 KVM 实际处理的最常见原因：

| Exit Reason | 数值 | 描述 | KVM 处理方式 | 是否返回用户态 |
|------------|------|------|-------------|---------------|
| `EXIT_REASON_EXCEPTION_NMI` | 0 | 异常或 NMI | `handle_exception_nmi()` | 视异常类型而定 |
| `EXIT_REASON_EXTERNAL_INTERRUPT` | 1 | **外部中断** | `handle_external_interrupt()` | ❌ 内核态处理 |
| `EXIT_REASON_TRIPLE_FAULT` | 2 | 三重错误（崩溃） | `handle_triple_fault()` | ❌ 注入关机事件 |
| `EXIT_REASON_INTERRUPT_WINDOW` | 7 | 中断窗口打开 | `handle_interrupt_window()` | ❌ 内核态处理 |
| `EXIT_REASON_CPUID` | 10 | CPUID 指令 | `kvm_emulate_cpuid()` | ❌ 内核态处理 |
| `EXIT_REASON_HLT` | 12 | HLT 指令 | `kvm_emulate_halt()` | ❌ 内核态处理 |
| `EXIT_REASON_CR_ACCESS` | 28 | 控制寄存器访问 | `handle_cr()` | ❌ 内核态处理 |
| `EXIT_REASON_IO_INSTRUCTION` | 30 | I/O 指令（IN/OUT） | `handle_io()` | 视端口而定 |
| `EXIT_REASON_MSR_READ` | 31 | RDMSR 指令 | `kvm_emulate_rdmsr()` | 部分返回用户态 |
| `EXIT_REASON_MSR_WRITE` | 32 | WRMSR 指令 | `kvm_emulate_wrmsr()` | 部分返回用户态 |
| `EXIT_REASON_EPT_VIOLATION` | 48 | **EPT 页错误** | `handle_ept_violation()` | ❌ 内核态处理 |
| `EXIT_REASON_EPT_MISCONFIG` | 49 | **MMIO 访问** | `handle_ept_misconfig()` | 快速路径内核态，慢速路径返回用户态 |

**源码引用**: `arch/x86/include/uapi/asm/vmx.h:32-92`（完整定义），`arch/x86/kvm/vmx/vmx.c:6095-6147`（处理函数表）

### 2. 常见 VM-Exit 场景分析

#### 快速路径（内核态处理，不返回用户态）

```
① 外部中断（EXIT_REASON_EXTERNAL_INTERRUPT）
   Guest 执行中 ← 硬件中断到达（网卡/定时器/键盘）
       │
       ▼
   vmx_handle_exit() → handle_external_interrupt()
       │
       ├── 更新虚拟 LAPIC 状态
       ├── 不修改 kvm_run 结构
       └── return 1 → 继续 vcpu_run() 循环，重新进入 Guest
   
   耗时：~1-2 μs（无上下文切换）

② CPUID 指令（EXIT_REASON_CPUID）
   Guest 执行 CPUID ← 查询 CPU 特性
       │
       ▼
   vmx_handle_exit() → kvm_emulate_cpuid()
       │
       ├── 调用 kvm_cpuid() 从 vCPU 的 CPUID 表查询结果
       │   └── 表在初始化时由 QEMU 通过 KVM_SET_CPUID2 设置
       ├── 直接写入 vCPU 寄存器（eax/ebx/ecx/edx）
       ├── 调用 kvm_skip_emulated_instruction() 跳过 CPUID 指令
       └── return 1 → 继续 vcpu_run() 循环，重新进入 Guest
   
   耗时：~1-3 μs（无用户态/内核态切换）

③ HLT 指令（EXIT_REASON_HLT）
   Guest 执行 HLT ← 空闲等待中断
       │
       ▼
   vmx_handle_exit() → kvm_emulate_halt()
       │
       ├── 设置 mp_state = KVM_MP_STATE_HALTED
       └── return 1 → vcpu_run() 进入 halt-polling
   
   耗时：~0.5 μs（加上 halt-polling 延迟）

④ EPT Violation（EXIT_REASON_EPT_VIOLATION）
   Guest 访问 GPA ← EPT 页表缺失或权限不足
       │
       ▼
   vmx_handle_exit() → handle_ept_violation()
       │
       ├── kvm_mmu_page_fault()
       │   ├── 查找 memslot → gfn_to_hva()
       │   ├── 获取物理页 → hva_to_pfn()
       │   └── 更新 EPT 页表 → kvm_tdp_mmu_map()
       │
       └── return 1 → 重新进入 Guest，EPT 命中
   
   耗时：~5-20 μs（取决于是否分配新页）

⑤ EPT Misconfig（EXIT_REASON_EPT_MISCONFIG）— MMIO 访问
   Guest 访问 MMIO 区域的 GPA ← EPT 页表项是特殊的 MMIO SPTE
       │
       ▼
   vmx_handle_exit() → handle_ept_misconfig()
       │
       ├── 检查是否是 MMIO SPTE（权限位 W+X = 110b）
       ├── kvm_mmu_page_fault() → handle_mmio_page_fault()
       │   ├── 识别为 MMIO 访问
       │   ├── 尝试快速 MMIO 路径（kvm_io_bus_write）
       │   │   └── 成功 → return，继续 Guest
       │   └── 慢速路径 → x86_emulate_instruction()
       │       ├── 解码 MMIO 指令，提取地址和数据
       │       ├── 设置 kvm_run->exit_reason = KVM_EXIT_MMIO
       │       ├── 填充 kvm_run->mmio 结构（phys_addr, data, len, is_write）
       │       ├── 设置 complete_userspace_io = complete_emulated_mmio
       │       └── return 0 → ioctl(KVM_RUN) 返回到 VMM
       │
       └── VMM 处理：
           - 根据 kvm_run->mmio.phys_addr 查找注册的 MMIO 处理函数
           - 支持 data match 规则（如 KVM_IOEVENTFD 的 datamatch 参数）
           - 模拟设备行为，填写返回值到 kvm_run->mmio.data
           - 再次 ioctl(KVM_RUN) 进入 Guest
       
       KVM 完成仿真：
           - 调用 complete_emulated_mmio() 完成剩余的 MMIO 片段
           - 继续 Guest 执行
   
   耗时：~10-50 μs（包含用户态/内核态切换）

**EPT Misconfig 的巧妙设计**：
- KVM 为 MMIO 区域的 GPA 在 EPT 中设置特殊的页表项（SPTE）
- 这个 SPTE 的权限位故意设置为 `W+X`（bits 2:0 = 110b），这是 Intel SDM 规定的非法组合
- Guest 访问时，硬件检测到无效 EPT 配置，自动触发 `EXIT_REASON_EPT_MISCONFIG`
- 这是一种利用硬件验证机制来拦截 MMIO 访问的优化，避免额外的页表遍历

**MMIO 的两条路径**：
1. **快速路径**：`kvm_io_bus_write()` 直接完成（已注册的 ioeventfd 等）→ 继续 Guest
2. **慢速路径**：`x86_emulate_instruction()` 解码指令 → 返回用户态 → VMM 分发处理

**VMM 的 MMIO 分发**：
- VMM 通过 `KVM_SET_USER_MEMORY_REGION` 注册 MMIO 区域（无 memslot 的 GPA）
- VMM 可通过 `KVM_IOEVENTFD` 注册带 data match 的规则（如 virtio 的 notify 端口）
- KVM 解析出 MMIO 的地址和数据后，VMM 根据这些规则分发到对应的处理函数
```

#### 慢速路径（返回用户态，由 VMM 处理）

```
① 被 MSR 过滤器拦截的 MSR（EXIT_REASON_MSR_READ/WRITE）
   Guest 执行 RDMSR/WRMSR ← 访问被 VMM 过滤器拦截的 MSR
       │
       ▼
   vmx_handle_exit() → kvm_emulate_rdmsr/wrmsr()
       │
       ├── 第一层：检查 VMM 设置的 MSR 过滤器（KVM_X86_SET_MSR_FILTER）
       │   ├── MSR 在过滤器中且需要拦截 → 进入第三层
       │   └── MSR 不在过滤器中 → 进入第二层
       │
       ├── 第二层：检查 KVM 内置白名单
       │   ├── MSR 在白名单中（如 TSC、APIC_BASE）→ 内核态处理，return 1
       │   └── MSR 不在白名单中 → 进入第三层
       │
       └── 第三层：调用 kvm_msr_user_space()
           ├── 设置 kvm_run->exit_reason = KVM_EXIT_X86_RDMSR/WRMSR
           ├── 填充 kvm_run->msr 结构（index, data, reason）
           └── return 0 → ioctl(KVM_RUN) 返回到 VMM
   
   VMM 处理（以 QEMU 为例）：
   - 遍历 msr_handlers 数组，查找匹配的 MSR
   - 调用对应的 handler（如 kvm_rdmsr_pkg_power_limit）
   - 填写返回值到 kvm_run->msr.data
   - 设置 kvm_run->msr.error（0=成功，1=失败）
   - 再次 ioctl(KVM_RUN) 进入 Guest
   
   耗时：~20-100 μs（包含用户态/内核态切换）

**QEMU 实际拦截的 MSR**（电源管理相关）：
- `MSR_CORE_THREAD_COUNT` — CPU 核心/线程计数
- `MSR_RAPL_POWER_UNIT` — RAPL 功率单位
- `MSR_PKG_POWER_LIMIT` — 封装功率限制
- `MSR_PKG_POWER_INFO` — 封装功率信息
- `MSR_PKG_ENERGY_STATUS` — 封装能量状态

源码引用：`target/i386/kvm/kvm.c:3182-3227`（过滤器注册），`:5930-5967`（处理逻辑）

**"部分返回"的准确含义**：
- **大部分 MSR**（TSC、APIC_BASE、MTRR 等）→ KVM 内置白名单，内核态处理
- **特定 MSR**（电源管理等）→ VMM 通过过滤器拦截，返回用户态
- **未知 MSR** → 注入 #GP 异常（或根据配置返回用户态）

**关键**：VMM 必须通过 `KVM_X86_SET_MSR_FILTER` 明确告诉 KVM 哪些 MSR 需要拦截。
如果没有设置过滤器，或 MSR 不在过滤器中，KVM 会尝试在内核态处理或注入 #GP。

② 特定 I/O 端口（EXIT_REASON_IO_INSTRUCTION）
   Guest 执行 IN/OUT ← 访问未映射的 I/O 端口
       │
       ▼
   vmx_handle_exit() → handle_io()
       │
       ├── kvm_fast_pio_out/in() → 快速路径（已知端口）
       │   └── 内核态处理，return 1
       │
       └── 慢速路径 → 设置 kvm_run->exit_reason = KVM_EXIT_IO
           └── return 0 → ioctl(KVM_RUN) 返回到 VMM
       
   VMM 处理：
   - 查找 I/O 端口处理函数
   - 模拟设备行为（如 PIT、RTC）
   - 再次 ioctl(KVM_RUN) 进入 Guest
   
   耗时：~15-80 μs（取决于设备复杂度）

**注意**：MMIO 访问（如 PCI 配置空间）走的是 `EXIT_REASON_EPT_MISCONFIG`，
通过 EPT 页表项的特殊标记（W+X = 110b）触发 VM-Exit，然后在内核态仿真或返回用户态。
详见"快速路径"部分的第⑤项。
```

### 3. KVM 返回给用户态的 Exit Reason

KVM 在内核态处理完 VM-Exit 后，可能需要返回用户态由 QEMU 处理。此时 `kvm_run->exit_reason` 设置为以下值之一：

| Exit Reason | 数值 | 触发场景 | QEMU 处理方式 |
|------------|------|---------|--------------|
| `KVM_EXIT_IO` | 2 | I/O 端口访问 | 调用 `kvm_handle_io()` 模拟 |
| `KVM_EXIT_HLT` | 5 | HLT 指令（特殊配置） | 通常内核态处理，仅 `lapic_in_kernel=false` 时返回 |
| `KVM_EXIT_MMIO` | 6 | 未映射的 MMIO 访问 | 查找 MMIO 处理函数，模拟设备 |
| `KVM_EXIT_SHUTDOWN` | 8 | 三重错误（Guest 崩溃） | 重置或终止 VM |
| `KVM_EXIT_INTR` | 10 | Host 信号中断 | 处理信号或终止 vCPU |
| `KVM_EXIT_X86_RDMSR` | 29 | 未处理 MSR 读取 | 返回值或注入 #GP |
| `KVM_EXIT_X86_WRMSR` | 30 | 未处理 MSR 写入 | 接受或注入 #GP |
| `KVM_EXIT_MEMORY_FAULT` | 39 | 内存访问错误 | 检查并修复映射 |

**源码引用**: `include/uapi/linux/kvm.h:146-185`（完整定义）

### 4. 使用 ftrace 观察 VM-Exit

```bash
# 启用 KVM tracepoints
echo 1 > /sys/kernel/debug/tracing/events/kvm/enable

# 启动 VM
cd /root/code/kvm-study/scripts/vm
./boot-vm.sh ubuntu --memory 4G --cpus 4

# 实时观察 VM-Exit（新终端）
cat /sys/kernel/debug/tracing/trace_pipe | grep kvm_exit

# 典型输出：
# qemu-system-x86-12345 [001] .... 12345.678901: kvm_exit: reason EXTERNAL_INTERRUPT rip 0xffffffff810000a0 info 0 0
# qemu-system-x86-12345 [001] .... 12345.678902: kvm_entry: vcpu 0 rip 0xffffffff810000a0
# qemu-system-x86-12345 [001] .... 12345.678903: kvm_exit: reason EPT_VIOLATION rip 0xffffffff810000a5 info 0x1234 0x0
# qemu-system-x86-12345 [001] .... 12345.678904: kvm_page_fault: address 0x1234 error_code 0x0
# qemu-system-x86-12345 [001] .... 12345.678905: kvm_entry: vcpu 0 rip 0xffffffff810000a5

# 统计 VM-Exit 频率
perf record -e kvm:kvm_exit -a -g -- sleep 10
perf report --stdio
```

### 5. VM-Exit 性能影响

| VM-Exit 类型 | 典型频率 | 单次开销 | 优化建议 |
|-------------|---------|---------|---------|
| 外部中断 | 高（~10k/s） | ~1-2 μs | 使用 Posted Interrupts 减少 VM-Exit |
| EPT Violation | 中（首次访问时） | ~5-20 μs | 使用大页（2MB/1GB）减少页错误 |
| CPUID | 低（~1k/s） | ~10-50 μs | 缓存结果，减少调用次数 |
| MSR 访问 | 中（~5k/s） | ~20-100 μs | 启用 MSR 位图，减少未处理 MSR |
| I/O 端口 | 低（~500/s） | ~15-80 μs | 使用 MMIO 替代 PIO，virtio 设备 |

**关键洞察**：
- **高频 VM-Exit**（外部中断、EPT Violation）是性能瓶颈的主要来源
- **快速路径**（内核态处理）比**慢速路径**（返回用户态）快 5-10 倍
- **Posted Interrupts**（phase4 详解）可以将外部中断的 VM-Exit 次数降为 0
- **EPT 大页**（phase2 详解）可以将页错误次数降低 512 倍（2MB 页 vs 4KB 页）

---

## 🧵 vCPU调度模型

### 1. halt-polling机制

当Guest执行HLT指令时，vCPU需要阻塞等待中断。但立即阻塞会导致中断延迟增加。KVM采用**halt-polling**策略：先轮询一段时间，如果期间有中断到达则立即唤醒；否则才真正阻塞。

```
Guest执行HLT指令
    │
    ▼
vmx_handle_exit() → handle_halt()
    │
    ├── vcpu->arch.mp_state = KVM_MP_STATE_HALTED
    │
    └── return 1 (继续vcpu_run循环)
    │
    ▼
vcpu_run()循环
    │
    ├── if (vcpu->arch.mp_state == KVM_MP_STATE_HALTED)
    │       └── kvm_vcpu_halt(vcpu)
    │           │
    │           ├── start_time = ktime_get()
    │           │
    │           ├── while (ktime - start_time < halt_poll_ns) {
    │           │       │
    │           │       ├── kvm_arch_vcpu_runnable(vcpu)?
    │           │       │   └── 检查是否有待处理中断/事件
    │           │       │
    │           │       ├── if (runnable) return (立即唤醒)
    │           │       │
    │           │       └── cpu_relax() (短暂等待)
    │           │   }
    │           │
    │           └── kvm_vcpu_block(vcpu)
    │               └── wait_event_interruptible(vcpu->wq, runnable)
    │                   └── 阻塞直到有中断到达
    │
    └── 继续循环
```

**调优参数**：
- `halt_poll_ns`（默认 200000 ns = 200 μs，`KVM_HALT_POLL_NS_DEFAULT`，
  `arch/x86/include/asm/kvm_host.h:71`）：轮询窗口上限
- 增大：降低中断延迟，但增加CPU占用
- 减小：降低CPU占用，但增加中断延迟

★ 这对权衡在本机上**没有传说的那么灵**，实测结论（含"什么时候完全零收益"）见
[`../phase9-performance/index.md`](../phase9-performance/index.md) §1.2。

**VMM视角对比**：
- 用户态VMM（特殊配置，如 `-kernel-irqchip off`）：ioctl返回KVM_EXIT_HLT，QEMU处理唤醒
- KVM内核态（默认，`lapic_in_kernel()` 返回 true）：halt-polling + 内核态阻塞，完全在内核态处理，无需返回用户空间

### 2. vCPU阻塞和唤醒

```
vCPU阻塞：
  kvm_vcpu_block()
    └── wait_event_interruptible(vcpu->wq, kvm_arch_vcpu_runnable(vcpu))
        └── 阻塞在等待队列

vCPU唤醒：
  中断到达 → kvm_set_irq() → kvm_vcpu_kick(vcpu)
    └── wake_up_interruptible(&vcpu->wq)
        └── vCPU从阻塞中唤醒
```

---

## 🧠 内存管理框架

### 1. memslot管理

KVM使用**memslot**来管理Guest物理内存。每个memslot描述一段连续的GPA区域，映射到Host的用户空间内存（HVA）。

```
QEMU: ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region)
    │
    ▼
kvm_vm_ioctl_set_memory_region() [kvm_main.c]
    │
    ├── 验证参数（GPA、HVA、大小）
    │
    ├── kvm_set_memory_region(kvm, &region)
    │   │
    │   ├── kvm_alloc_memslots(kvm)
    │   │   └── 分配memslots结构（如果首次）
    │   │
    │   ├── kvm_prepare_memory_region(kvm, old, new, change)
    │   │   └── 准备新的memslot
    │   │
    │   ├── kvm_commit_memory_region(kvm, old, new, change)
    │   │   ├── 更新EPT页表（如果必要）
    │   │   └── 刷新TLB（如果必要）
    │   │
    │   └── kvm_arch_commit_memory_region(kvm, old, new, change)
    │       └── 架构相关操作
    │
    └── return 0
```

### 2. GPA → HVA → HPA 转换

```
Guest访问物理地址 GPA
    │
    ▼
EPT页表查找（硬件）
    │
    ├── EPT命中
    │   └── 直接访问HPA，无VM-Exit
    │
    └── EPT缺失（EPT Violation）
        │
        ▼
    VM-Exit → vmx_handle_exit() → handle_ept_violation()
        │
        ├── kvm_mmu_page_fault(vcpu, gpa, error_code)
        │   │
        │   ├── gfn_to_hva(kvm, gfn)
        │   │   └── 查找memslot，返回HVA
        │   │
        │   ├── hva_to_pfn(hva)
        │   │   └── 查询Host页表，返回HPA (PFN)
        │   │
        │   └── kvm_tdp_mmu_map(vcpu, gpa, pfn)
        │       └── 更新EPT页表，建立GPA→HPA映射
        │
        └── 重新进入Guest，EPT命中
```

---

## 🔌 中断注入框架

### 1. 中断路由表

KVM维护一个中断路由表，将GSI（Global System Interrupt）映射到具体的中断目标（PIC、IOAPIC、MSI）。

```
QEMU: ioctl(vm_fd, KVM_SET_GSI_ROUTING, &routing)
    │
    ▼
kvm_vm_ioctl_set_gsi_routing() [kvm_main.c]
    │
    ├── 验证路由表
    │
    ├── kvm_set_irq_routing(kvm, entries, nr)
    │   ├── 分配新的路由表
    │   ├── 填充路由条目
    │   └── rcu_assign_pointer(kvm->irq_routing, new_table)
    │
    └── return 0
```

### 2. 中断投递流程

```
外部中断到达（如网卡MSI）
    │
    ▼
Host内核中断处理 → kvm_set_irq(irq)
    │
    ├── kvm_irq_delivery_to_apic(kvm, irq_source_id, vector, dest_id)
    │   │
    │   ├── 查找目标vLAPIC
    │   │
    │   ├── kvm_apic_set_irq(apic, vector, dest_id, ...)
    │   │   └── 设置vLAPIC IRR（中断请求寄存器）
    │   │
    │   └── kvm_vcpu_kick(vcpu)
    │       └── 唤醒vCPU
    │
    └── VM-Entry时注入到Guest
        └── vmcs_write32(VM_ENTRY_INTR_INFO, vector)
```

---

## 🔍 VMM视角对比

### 用户态VMM vs KVM内核态实现

| 方面 | 用户态VMM (QEMU) | KVM内核态 |
|------|------------------|-----------|
| **VM-Entry/Exit** | ioctl(KVM_RUN)返回，需要上下文切换 | 直接vmx_vcpu_run()，快速路径无切换 |
| **寄存器管理** | 通过ioctl(KVM_SET_REGS)等间接设置 | 直接vmcs_write()，零开销 |
| **内存映射** | mmap共享内存 + ioctl设置memslot | EPT直接映射，支持并发页错误 |
| **中断注入** | ioctl(KVM_INTERRUPT)返回到用户态 | 直接写VMCS，或Posted Interrupts零VM-Exit |
| **halt处理** | 特殊配置：ioctl返回KVM_EXIT_HLT | halt-polling + 内核态阻塞，默认不返回用户态 |
| **MMIO处理** | ioctl返回KVM_EXIT_MMIO，QEMU模拟 | 快速路径内核态处理，复杂MMIO返回用户态 |

### 为什么KVM要这样设计？

1. **性能**：减少用户态/内核态切换开销
2. **灵活性**：用户态可以模拟复杂设备（VGA、USB）
3. **安全性**：内核态可以直接访问硬件，用户态隔离
4. **可维护性**：框架层与架构层分离，易于扩展

### 实战建议

- **快速路径**（中断注入、HLT、简单IO）→ KVM内核态处理
- **慢速路径**（复杂MMIO、CPUID）→ 返回用户态由QEMU处理
- **混合模式**（virtio）→ 数据面内核态（vhost），控制面用户态

---

## ⚠️ 常见陷阱

### 陷阱1（debug）：静默回退到软件模拟

**场景**：VMM 调用 `open("/dev/kvm")` 成功，但未正确走 `ioctl(KVM_RUN)` 路径

**症状**：
- 所有 `kvm_exit` tracepoint 为零事件
- Guest 运行极慢（比硬件虚拟化慢 10-100 倍）
- `/proc/<qemu-pid>/fd` 中没有指向 `/dev/kvm` 的文件描述符

**原因**：某些 VMM（如 QEMU）在未显式启用 KVM 时，会**静默回退**到纯软件模拟（TCG），不报错但性能极差

**诊断**：
```bash
# 检查 QEMU 进程是否真的在使用 KVM
ls -l /proc/$(pgrep -f '^qemu-system-x86_64')/fd | grep -c kvm
# 返回 >0 表示走 KVM，=0 表示走 TCG
```

**解决**：启动 VM 时必须显式传 `-enable-kvm`（QEMU）或调用 `ioctl(KVM_CREATE_VM)` + `ioctl(KVM_CREATE_VCPU)` + `ioctl(KVM_RUN)`（通用 VMM）

**验证**：`scripts/vm/boot-vm.sh` 默认带上 `-enable-kvm -cpu host` 并在启动前自检

### 陷阱2（debug）：tracefs 的 `echo >` 会清掉所有已有配置

**场景**：调试 VM 时，先配置好 `kvm_exit` 的 filter，再用 `echo` 添加 `kvm_entry`

**症状**：
- `kvm_exit` 事件突然消失
- 只看到 `kvm_entry` 事件

**原因**：`set_event`、`set_ftrace_filter`、`set_event_pid` 三个文件上**带 `O_TRUNC` 的写**（`echo x > file`、不带 `-a` 的 `tee`）会**先清掉全部已有配置**，再写入本次内容

**示例**：
```bash
# 错误做法：第二次 echo 会清掉 kvm_exit
echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_entry > /sys/kernel/debug/tracing/set_event  # ← kvm_exit 被清掉！

# 正确做法：用 >> 追加，或一次性写入多个事件
echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event
echo kvm:kvm_entry >> /sys/kernel/debug/tracing/set_event

# 或者一次性写入
echo 'kvm:kvm_exit kvm:kvm_entry' > /sys/kernel/debug/tracing/set_event
```

**解决**：
- 添加事件时用 `>>`（追加）而非 `>`（覆盖）
- 清场时显式写 `: > set_event` 并注明
- `set_ftrace_filter` 和 `set_event_pid` 同理

**源码位置**：`kernel/trace/trace_events.c:2411-2423`（`set_event` 清空逻辑）

### 陷阱3（性能调优）：halt-polling 调大不是万能的

**场景**：为了降低中断延迟，将 `halt_poll_ns` 从默认 200μs 调到 1ms 甚至 10ms

**症状**：
- CPU 占用率显著上升（idle 时也高达 30-50%）
- 中断延迟并未明显降低
- 整体性能反而下降

**原因**：halt-polling 的收益**只在"唤醒源随机且大概率落在 polling 窗口内"时成立**。如果唤醒事件早于窗口起点（如定时器到期时间确定），polling 无法让它更早，反而白白消耗 CPU

**实测结论**：
- 空闲场景：零收益，纯浪费 CPU
- flood 场景：买不到延迟，反而多付 CPU
- 收益曲线在"窗口刚够盖住典型 halt"处就饱和

**正确做法**：
- 默认 200μs 通常是合理起点
- 根据工作负载实测：只有"唤醒间隔 < polling 窗口"时才有收益
- 详细实测数据见 [`../phase9-performance/index.md`](../phase9-performance/index.md) §1.2

**源码位置**：`arch/x86/kvm/x86.c` → `kvm_vcpu_halt()` → halt-polling 循环

### 陷阱4（VMM）：`KVM_EXIT_SHUTDOWN` 与 `KVM_EXIT_SYSTEM_EVENT` 的区别

**场景**：VMM 收到 `kvm_run->exit_reason`，未区分 `KVM_EXIT_SHUTDOWN` 和 `KVM_EXIT_SYSTEM_EVENT`

**症状**：
- Guest 三重错误（Triple Fault）后，VMM 不知道是崩溃还是正常关机
- Guest 主动调用 `poweroff` 时，VMM 误以为是崩溃而重启

**原因**：KVM 返回两种不同的关机/重启通知：

| Exit Reason | 触发场景 | 含义 | VMM 应如何处理 |
|------------|---------|------|---------------|
| `KVM_EXIT_SHUTDOWN` (8) | 三重错误（Guest 代码崩溃） | Guest 异常终止 | 重置或终止 VM |
| `KVM_EXIT_SYSTEM_EVENT` (24) | Guest 主动关机/重启/崩溃 | 根据 `system_event.type` 区分 | 按 type 字段处理 |

`KVM_EXIT_SYSTEM_EVENT` 的 `type` 字段：
- `KVM_SYSTEM_EVENT_SHUTDOWN` (1) — Guest 正常关机（如 `poweroff`）
- `KVM_SYSTEM_EVENT_RESET` (2) — Guest 请求重启（如 `reboot`）
- `KVM_SYSTEM_EVENT_CRASH` (3) — Guest 内核崩溃（如 kernel panic）

**QEMU 参考实现**：`accel/kvm/kvm-all.c:3265-3319`

**解决**：VMM 必须分别处理这两种 exit reason，并根据 `system_event.type` 执行对应动作（关机、重启、记录崩溃日志）

**源码位置**：`include/uapi/linux/kvm.h:155,174`（定义），`arch/x86/kvm/x86.c:10855-10907`（触发逻辑）

---

## ⚡ 性能优化技术

### 1. halt-polling调优

**原理**：在vCPU halt后先轮询一段时间，而不是立即阻塞

**参数**：
```bash
# 查看当前值
cat /sys/module/kvm/parameters/halt_poll_ns
# 默认: 200000 (200μs) —— KVM_HALT_POLL_NS_DEFAULT，arch/x86/include/asm/kvm_host.h:71

# 调整（6.12.93 四个 halt-polling 参数都是 0644，可直接写）
echo 400000 > /sys/module/kvm/parameters/halt_poll_ns
```

**调优建议**：★ 这件事本仓已经实测过，结论与"按负载类型调窗口"的流传说法方向相反：
**空闲场景零收益、flood 场景买不到延迟反而多付 CPU，且收益曲线在"窗口刚够盖住典型
halt"处就饱和** —— 具体数值、样本量与实验条件只有一份，见
[`../phase9-performance/index.md`](../phase9-performance/index.md) §1.2（本仓规则：
别处只写指针，不复制数字）。

机制侧的判据：只有"唤醒源随机且大概率落在 polling 窗口内"才有收益；唤醒事件早于
窗口起点时 polling 无法让它更早。四个参数各自的作用域、默认值与自适应算法见
[`../phase9-performance/parameters.md`](../phase9-performance/parameters.md) §1。

### 2. vCPU亲和性

**原理**：将vCPU线程绑定到特定pCPU，减少迁移开销

**方法**：
```bash
# 获取QEMU进程PID
QEMU_PID=$(pidof qemu-system-x86)

# 绑定vCPU 0到pCPU 0
taskset -p 0x1 $QEMU_PID

# 绑定vCPU 1到pCPU 1
taskset -p 0x2 $QEMU_PID
```

**效果**：减少TLB刷新、L3缓存污染

### 3. 大页内存

**原理**：使用2MB大页减少TLB miss

**方法**：
```bash
# Host配置大页
echo 1024 > /proc/sys/vm/nr_hugepages

# QEMU使用大页
qemu-system-x86_64 -mem-path /dev/hugepages -mem-prealloc ...
```

**效果**：EPT TLB命中率提升，性能提升10-20%

---

## 📊 实践练习

> **重要**：完整的实践练习和实验脚本已经整理到 `practice/` 目录中。

### 快速开始

```bash
# 进入实践目录
cd practice/

# 查看详细指南
cat README.md

# 启动 VM（所有练习都需要，前台运行，建议单独开一个终端）
cd ../../scripts/vm && ./boot-vm.sh ubuntu --memory 4G --cpus 4

# Phase 0 的练习为手工步骤形式（无封装脚本），按 practice/README.md 的
# 「练习详情」依次执行 ftrace / perf / QEMU trace 命令

# 清理：在 Guest 内执行 poweroff（或在 QEMU monitor 中 quit）
```

### 练习列表

详细步骤见 [practice/README.md](practice/README.md)。

| 编号 | 练习名称 | 主要工具 | 难度 | 预计时间 | 核心知识点 |
|------|---------|---------|------|---------|-----------|
| 1 | [跟踪 VM 生命周期](practice/README.md#练习-1-跟踪-vm-生命周期) | ftrace | ★☆☆ | 15min | KVM_RUN 调用链 |
| 2 | [分析 vCPU 调度](practice/README.md#练习-2-分析-vcpu-调度) | perf record/report | ★★☆ | 20min | vCPU 线程调度 |
| 3 | [调试 memslot](practice/README.md#练习-3-调试-memslot) | QEMU monitor / --trace / strace | ★★☆ | 20min | 内存 slot 管理 |
| 4 | [性能对比](practice/README.md#练习-4-性能对比) | perf stat | ★★★ | 30min | 用户态 vs 内核态 |

### 统一测试环境

所有练习使用统一的 VM 启动脚本：

- **构建脚本**: `scripts/vm/build-kernel.sh` + `scripts/vm/build-rootfs-ubuntu.sh`
- **启动脚本**: `scripts/vm/boot-vm.sh`（前台运行 QEMU）
- **详细说明**: 参见 `scripts/README.md` 与 `practice/README.md`

### 快速练习（不需要脚本）

如果只是想快速了解 KVM 框架，可以直接使用 ftrace：

```bash
# 启用 KVM tracepoints
echo 1 > /sys/kernel/debug/tracing/events/kvm/enable

# 启动 VM
qemu-system-x86_64 -m 1G ...

# 查看 trace
cat /sys/kernel/debug/tracing/trace_pipe | grep kvm

# 观察到的事件：
# kvm_entry: vcpu 0, rip 0xffffffff810000a0
# kvm_exit: reason EXTERNAL_INTERRUPT, rip 0xffffffff810000a0
# kvm_page_fault: address 0x7fff12340000, error_code 0x2
```

# 查看memslot的GPA、HVA、大小
```

### 练习4：性能对比

```bash
# 先读回原值存档（6.12.93 的默认是 200000 = 200μs，不是 400000）
ORIG=$(cat /sys/module/kvm/parameters/halt_poll_ns)

# 四档对比：禁用 / 原值 / 拉大一倍 / 再拉大
for v in 0 "$ORIG" 400000 1000000; do
    echo "$v" > /sys/module/kvm/parameters/halt_poll_ns
    # 运行同一个工作负载，测量延迟；每档重复若干次取中位数
done

# 收尾恢复原值 —— 模块参数是全局的，不恢复会污染下一轮
echo "$ORIG" > /sys/module/kvm/parameters/halt_poll_ns
```

★ 这个 A/B 本仓已经做过，结论比"调大就更快"复杂，见
[`../phase9-performance/index.md`](../phase9-performance/index.md) §1.2。
自己做时要满足 [`../phase9-performance/measurement.md`](../phase9-performance/measurement.md)
的三条纪律：**有对照组、每档重复取中位数、两组同一观测档位**（一边开 trace 一边不开
直接比耗时，测到的是 tracer 自己）。

---

## ✅ 验证清单

完成本阶段后，确认你能回答：

- [ ] 画出`ioctl(KVM_RUN)`到`VMENTER`指令的完整调用链
- [ ] 解释`struct kvm`中`memslots`、`vcpus`、`irq_routing`的作用
- [ ] 说明halt-polling机制的工作原理和调优方法
- [ ] 分析GPA→HVA→HPA的转换流程
- [ ] 对比用户态VMM和KVM内核态的实现差异
- [ ] 解释为什么KVM要在内核态处理部分VM-Exit
- [ ] 列举至少5种常见的VM-Exit原因，并说明哪些走快速路径、哪些走慢速路径
- [ ] 使用ftrace观察VM-Exit，能识别EXTERNAL_INTERRUPT、EPT_VIOLATION、CPUID等常见类型
- [ ] 说明如何判断一次 VM 运行是否真的走了 KVM（而非静默回退到软件模拟）
- [ ] 区分 `KVM_EXIT_SHUTDOWN` 与 `KVM_EXIT_SYSTEM_EVENT` 的触发场景和处理方式
- [ ] 列举至少3个调试 VM 时常见的陷阱（tracefs 配置、静默回退、halt-polling 误区）

---

## 📚 参考资料

- Linux kernel source: `virt/kvm/kvm_main.c`
- Linux kernel source: `arch/x86/kvm/x86.c`
- Linux kernel source: `include/linux/kvm_host.h`
- KVM API documentation: `Documentation/virt/kvm/api.rst`
- KVM design paper: *"KVM: An Infrastructure for Virtualizing x86 Systems"*
