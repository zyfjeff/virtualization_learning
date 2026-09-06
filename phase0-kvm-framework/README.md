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

### 陷阱1：vCPU未绑定到pCPU

**场景**：调用`ioctl(KVM_RUN)`前忘记调用`vcpu_load()`

**症状**：vmx_vcpu_run()中访问`vmx->loaded_vmcs`时崩溃

**原因**：vCPU必须绑定到pCPU才能访问VMCS

**解决**：确保在`kvm_arch_vcpu_ioctl_run()`中调用`vcpu_load(vcpu)`

**源码位置**：`kvm_arch_vcpu_ioctl_run()` → `vcpu_load(vcpu)`

### 陷阱2：memslot更新未刷新EPT

**场景**：修改memslot后立即运行Guest，访问旧的GPA

**症状**：EPT Violation，但映射到错误的HPA

**原因**：memslot更新后需要刷新EPT页表和TLB

**解决**：在`kvm_commit_memory_region()`中调用`kvm_arch_flush_shadow_memslot()`

**源码位置**：`kvm_commit_memory_region()` → `kvm_arch_commit_memory_region()`

### 陷阱3：中断路由表RCU保护

**场景**：直接访问`kvm->irq_routing`而未使用RCU

**症状**：并发更新路由表时崩溃

**原因**：`irq_routing`使用RCU保护，需要使用`rcu_dereference()`访问

**解决**：使用`srcu_read_lock()` + `rcu_dereference()`访问路由表

**源码位置**：`kvm_set_irq()` → `rcu_dereference(kvm->irq_routing)`

### 陷阱4：halt-polling参数设置不当

**场景**：halt_poll_ns设置过大（如10ms）

**症状**：CPU占用率高，但性能无明显提升

**原因**：轮询时间过长，浪费CPU周期

**解决**：根据工作负载调优，通常200μs-500μs较合适

**源码位置**：`kvm_vcpu_halt()` → halt-polling循环

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
- [ ] 列出至少3个常见的KVM开发陷阱

---

## 📚 参考资料

- Linux kernel source: `virt/kvm/kvm_main.c`
- Linux kernel source: `arch/x86/kvm/x86.c`
- Linux kernel source: `include/linux/kvm_host.h`
- KVM API documentation: `Documentation/virt/kvm/api.rst`
- KVM design paper: *"KVM: An Infrastructure for Virtualizing x86 Systems"*
