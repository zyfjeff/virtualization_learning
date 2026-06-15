# Phase 0：源码精读注释 - KVM框架层

> 基于 Linux 6.12.93 源码

---

## 1. kvm_dev_ioctl() - KVM设备ioctl入口

**文件**: `virt/kvm/kvm_main.c:4666-4752`

这是KVM设备文件（`/dev/kvm`）的ioctl处理入口，负责处理系统级的KVM操作。

```c
/* 来源: virt/kvm/kvm_main.c */

static long kvm_dev_ioctl(struct file *filp,
                          unsigned int ioctl, unsigned long arg)
{
    long r = -EINVAL;
    void __user *argp = (void __user *)arg;

    switch (ioctl) {
    
    /* ============================================
     * KVM_GET_API_VERSION - 获取KVM API版本
     * 返回: KVM_API_VERSION (通常是12)
     * ============================================ */
    case KVM_GET_API_VERSION:
        r = KVM_API_VERSION;
        break;

    /* ============================================
     * KVM_CHECK_EXTENSION - 检查KVM扩展支持
     * 参数: int extension_id
     * 返回: 扩展支持程度 (0=不支持, >0=支持)
     * ============================================ */
    case KVM_CHECK_EXTENSION:
        r = kvm_dev_ioctl_check_extension_generic(arg);
        break;

    /* ============================================
     * KVM_GET_VCPU_MMAP_SIZE - 获取vCPU mmap区域大小
     * 返回: sizeof(struct kvm_run) + 额外空间
     * 用途: QEMU需要知道mmap多少内存给kvm_run
     * ============================================ */
    case KVM_GET_VCPU_MMAP_SIZE:
        r = PAGE_SIZE;  /* 通常是4KB */
        break;

    /* ============================================
     * ★ KVM_CREATE_VM - 创建虚拟机 (最重要!)
     * 参数: unsigned long type (通常为0)
     * 返回: VM文件描述符
     * ============================================ */
    case KVM_CREATE_VM:
        /* 检查是否允许创建更多VM */
        r = -EINVAL;
        if (kvm_usage_count == 0)
            break;
        
        /* 创建VM */
        r = kvm_dev_ioctl_create_vm(arg);
        break;

    /* ============================================
     * KVM_GET_NR_TSC_PAGE - 获取TSC页数量
     * 用于kvmclock多vCPU同步
     * ============================================ */
    case KVM_GET_NR_TSC_PAGE:
        r = KVM_NR_TSC_PAGE;
        break;

    /* ============================================
     * KVM_GET_TSC_KHZ - 获取TSC频率
     * 返回: TSC频率 (kHz)
     * ============================================ */
    case KVM_GET_TSC_KHZ:
        r = kvm_arch_get_tsc_khz();
        break;

    default:
        r = -EINVAL;
        break;
    }

    return r;
}
```

**学习要点**：
- 这是用户空间与KVM的第一次交互入口
- `KVM_CREATE_VM`是最关键的ioctl，创建VM实例
- 返回值通常是文件描述符或状态码

---

## 2. kvm_dev_ioctl_create_vm() - 创建VM实例

**文件**: `virt/kvm/kvm_main.c:4584-4649`

```c
/* 来源: virt/kvm/kvm_main.c */

static int kvm_dev_ioctl_create_vm(unsigned long type)
{
    int r;
    struct kvm *kvm;
    struct file *file;
    int fd;

    /* ============================================
     * Step 1: 分配VM结构
     * ============================================ */
    kvm = kvm_create_vm(type);
    if (IS_ERR(kvm))
        return PTR_ERR(kvm);

    /* ============================================
     * Step 2: 创建VM文件描述符
     * ============================================ */
    fd = get_unused_fd_flags(O_CLOEXEC);
    if (fd < 0) {
        r = fd;
        goto err_put_kvm;
    }

    file = anon_inode_getfile("kvm-vm", &kvm_vm_fops, kvm, O_RDWR);
    if (IS_ERR(file)) {
        r = PTR_ERR(file);
        goto err_put_fd;
    }

    /* ============================================
     * Step 3: 安装文件描述符
     * ============================================ */
    fd_install(fd, file);

    return fd;

err_put_fd:
    put_unused_fd(fd);
err_put_kvm:
    kvm_put_kvm(kvm);
    return r;
}
```

**调用链**：
```
kvm_dev_ioctl_create_vm()
  └→ kvm_create_vm(type)
       ├→ kzalloc(sizeof(struct kvm))           ← 分配VM结构
       ├→ kvm_arch_init_vm(kvm, type)            ← 架构初始化
       │    └→ kvm_vm_init_vmcs(kvm)             ← 分配VMCS shadow
       ├→ kvm_init_mmu(kvm)                      ← 初始化MMU
       ├→ kvm_create_vm_debugfs(kvm)             ← 创建debugfs
       └→ return kvm
```

**关键数据结构初始化**：
```
kvm_create_vm() 内部:
  │
  ├── kvm->mm = current->mm                      ← 继承当前进程内存空间
  ├── spin_lock_init(&kvm->mmu_lock)             ← MMU锁
  ├── mutex_init(&kvm->slots_lock)               ← memslot锁
  ├── INIT_LIST_HEAD(&kvm->vm_list)              ← VM链表
  ├── atomic_set(&kvm->online_vcpus, 0)          ← vCPU计数
  ├── kvm->memslots = kvm_alloc_memslots()       ← 分配memslots
  │    └→ 分配 struct kvm_memslots
  │         └→ slots[] 数组 (KVM_MEM_SLOTS_NUM个)
  └→ kvm->arch = ...                             ← 架构相关初始化
```

---

## 3. kvm_vcpu_ioctl() - vCPU ioctl入口

**文件**: `virt/kvm/kvm_main.c:4352-4577`

```c
/* 来源: virt/kvm/kvm_main.c */

static long kvm_vcpu_ioctl(struct file *filp,
                           unsigned int ioctl, unsigned long arg)
{
    struct kvm_vcpu *vcpu = filp->private_data;
    void __user *argp = (void __user *)arg;
    int r;

    switch (ioctl) {
    
    /* ============================================
     * ★ KVM_RUN - 运行vCPU (最核心!)
     * 参数: 无
     * 返回: 0 (成功) 或 负数 (错误)
     * ============================================ */
    case KVM_RUN:
        r = -EINVAL;
        if (arg)
            break;
        
        /* 绑定vCPU到当前pCPU */
        vcpu_load(vcpu);
        
        /* ★ 运行vCPU主循环 */
        r = kvm_arch_vcpu_ioctl_run(vcpu);
        
        /* 解绑vCPU */
        vcpu_put(vcpu);
        break;

    /* ============================================
     * KVM_GET_REGS - 获取通用寄存器
     * 参数: struct kvm_regs *
     * 返回: 0
     * ============================================ */
    case KVM_GET_REGS:
        r = -EINVAL;
        if (!argp)
            break;
        
        r = kvm_arch_vcpu_ioctl_get_regs(vcpu, argp);
        break;

    /* ============================================
     * KVM_SET_REGS - 设置通用寄存器
     * 参数: struct kvm_regs *
     * 返回: 0
     * ============================================ */
    case KVM_SET_REGS:
        r = -EINVAL;
        if (!argp)
            break;
        
        r = kvm_arch_vcpu_ioctl_set_regs(vcpu, argp);
        break;

    /* ============================================
     * KVM_GET_SREGS - 获取特殊寄存器
     * 包括: CR0/CR3/CR4, EFER, 段寄存器等
     * ============================================ */
    case KVM_GET_SREGS:
        r = kvm_arch_vcpu_ioctl_get_sregs(vcpu, argp);
        break;

    /* ============================================
     * KVM_SET_SREGS - 设置特殊寄存器
     * 必须在KVM_RUN之前调用!
     * ============================================ */
    case KVM_SET_SREGS:
        r = kvm_arch_vcpu_ioctl_set_sregs(vcpu, argp);
        break;

    /* ============================================
     * KVM_GET_MSRS - 获取MSR寄存器
     * ============================================ */
    case KVM_GET_MSRS:
        r = kvm_arch_vcpu_ioctl_get_msrs(vcpu, argp);
        break;

    /* ============================================
     * KVM_SET_MSRS - 设置MSR寄存器
     * ============================================ */
    case KVM_SET_MSRS:
        r = kvm_arch_vcpu_ioctl_set_msrs(vcpu, argp);
        break;

    /* ============================================
     * KVM_INTERRUPT - 注入中断
     * 参数: struct kvm_interrupt *
     * ============================================ */
    case KVM_INTERRUPT:
        r = kvm_vm_ioctl_interrupt(vcpu, argp);
        break;

    /* ============================================
     * KVM_GET_DIRTY_LOG - 获取脏页日志
     * 用于热迁移、内存快照
     * ============================================ */
    case KVM_GET_DIRTY_LOG:
        r = kvm_vm_ioctl_get_dirty_log(vcpu->kvm, argp);
        break;

    default:
        r = -EINVAL;
        break;
    }

    return r;
}
```

**学习要点**：
- `KVM_RUN`是核心，调用`kvm_arch_vcpu_ioctl_run()`进入vCPU主循环
- 寄存器操作（GET/SET_REGS/SREGS/MSRS）用于在VM-Entry前设置Guest状态
- `KVM_INTERRUPT`用于向Guest注入中断

---

## 4. kvm_arch_vcpu_ioctl_run() - x86 vCPU运行入口

**文件**: `arch/x86/kvm/x86.c:4375-4450`

```c
/* 来源: arch/x86/kvm/x86.c */

int kvm_arch_vcpu_ioctl_run(struct kvm_vcpu *vcpu)
{
    int r;

    /* ============================================
     * Step 1: 检查Guest状态有效性
     * ============================================ */
    if (unlikely(!kvm_arch_vcpu_ioctl_get_mpstate(vcpu, &vcpu->arch.mp_state)))
        return -EINVAL;

    /* ============================================
     * Step 2: 检查寄存器是否已设置
     * ============================================ */
    if (unlikely(!vcpu->arch.regs_avail)) {
        /* 寄存器未初始化，需要先调用KVM_SET_REGS */
        return -EINVAL;
    }

    /* ============================================
     * Step 3: 绑定vCPU到当前pCPU
     * ============================================ */
    vcpu_load(vcpu);

    /* ============================================
     * Step 4: ★ 进入vCPU运行主循环
     * ============================================ */
    r = vcpu_run(vcpu);

    /* ============================================
     * Step 5: 解绑vCPU
     * ============================================ */
    vcpu_put(vcpu);

    return r;
}
```

**关键检查**：
```
进入vcpu_run()前的必要条件:
  ├── mp_state != KVM_MP_STATE_UNINITIALIZED
  ├── regs_avail != 0 (寄存器已初始化)
  ├── sregs已设置 (CR0/CR3/CR4/EFER)
  └── 至少有一个memslot (Guest内存已配置)
```

---

## 5. vcpu_run() - vCPU运行主循环

**文件**: `arch/x86/kvm/x86.c:3417-3520`

这是KVM中最关键的函数之一，实现了vCPU的执行循环。

```c
/* 来源: arch/x86/kvm/x86.c */

static int vcpu_run(struct kvm_vcpu *vcpu)
{
    int r;

    /* ============================================
     * Step 1: 设置vCPU状态为运行
     * ============================================ */
    vcpu->arch.l1tf_flush_l1d = true;

    /* ============================================
     * Step 2: ★ 主循环 - 反复进入Guest
     * ============================================ */
    while (1) {
        
        /* ============================================
         * 检查1: vCPU是否halted?
         * ============================================ */
        if (vcpu->arch.mp_state == KVM_MP_STATE_HALTED) {
            /* halt-polling + 阻塞 */
            r = kvm_vcpu_halt(vcpu);
            
            if (r < 0)
                break;  /* 出错，退出循环 */
            
            if (r == 0)
                continue;  /* 被唤醒，继续循环 */
        }

        /* ============================================
         * 检查2: 是否有待处理请求?
         * ============================================ */
        if (kvm_check_request(KVM_REQ_VM_DEAD, vcpu)) {
            r = -EIO;
            break;
        }

        /* ============================================
         * Step 3: ★ 进入Guest
         * ============================================ */
        r = vcpu_enter_guest(vcpu);

        /* ============================================
         * Step 4: 处理返回值
         * ============================================ */
        if (r <= 0) {
            /* r < 0: 出错 */
            /* r == 0: 需要返回用户空间 (MMIO, HLT等) */
            break;
        }

        /* ============================================
         * 检查3: 是否有信号需要处理?
         * ============================================ */
        if (signal_pending(current)) {
            r = -EINTR;
            break;
        }

        /* ============================================
         * 继续循环，重新进入Guest
         * ============================================ */
        kvm_check_and_inject_events(vcpu);
    }

    /* ============================================
     * Step 5: 清理并返回
     * ============================================ */
    return r;
}
```

**主循环流程图**：
```
vcpu_run()
  │
  └──→ while (1) {
         │
         ├── if (mp_state == HALTED)
         │       └── kvm_vcpu_halt()
         │           ├── halt-polling (轮询)
         │           └── kvm_vcpu_block() (阻塞)
         │
         ├── vcpu_enter_guest()
         │   └── vmx_vcpu_run()
         │       └── VMENTER → Guest → VM-Exit
         │
         ├── if (r <= 0) break
         │   └── r < 0: 出错
         │   └── r == 0: 返回用户空间
         │
         ├── if (signal_pending) break
         │
         └── 继续循环
       }
```

**关键洞察**：
- 这是一个**无限循环**，只有在特定条件下才会退出
- 快速路径（如外部中断、HLT）在内核态直接处理，不返回用户空间
- 慢速路径（如MMIO、CPUID）返回用户空间，由QEMU处理

---

## 6. vcpu_enter_guest() - 进入Guest前的准备

**文件**: `arch/x86/kvm/x86.c:3319-3415`

```c
/* 来源: arch/x86/kvm/x86.c */

static int vcpu_enter_guest(struct kvm_vcpu *vcpu)
{
    int r;

    /* ============================================
     * Step 1: 设置vCPU状态为运行
     * ============================================ */
    vcpu->arch.mp_state = KVM_MP_STATE_RUNNABLE;

    /* ============================================
     * Step 2: 处理待处理请求
     * ============================================ */
    if (kvm_check_request(KVM_REQ_GET_VMCS12_PAGES, vcpu)) {
        /* 嵌套虚拟化相关 */
    }

    if (kvm_check_request(KVM_REQ_LOAD_MMU, vcpu)) {
        /* 加载MMU上下文 */
        kvm_mmu_load(vcpu);
    }

    if (kvm_check_request(KVM_REQ_MMU_SYNC, vcpu)) {
        /* 同步MMU */
        kvm_mmu_sync_roots(vcpu);
    }

    /* ============================================
     * Step 3: 检查并注入事件
     * ============================================ */
    r = kvm_check_and_inject_events(vcpu);
    if (r)
        return r;

    /* ============================================
     * Step 4: 架构相关预处理
     * ============================================ */
    r = kvm_x86_call(vcpu_pre_run)(vcpu);
    if (r)
        return r;

    /* ============================================
     * Step 5: ★ 进入Guest
     * ============================================ */
    r = kvm_x86_call(vcpu_run)(vcpu);

    /* ============================================
     * Step 6: 处理VM-Exit
     * ============================================ */
    if (r > 0) {
        r = vcpu_handle_exit(vcpu, r);
    }

    return r;
}
```

**关键准备工作**：
```
vcpu_enter_guest() 内部:
  │
  ├── 处理待处理请求 (KVM_REQ_*)
  │   ├── KVM_REQ_GET_VMCS12_PAGES (嵌套)
  │   ├── KVM_REQ_LOAD_MMU (加载MMU)
  │   └── KVM_REQ_MMU_SYNC (同步MMU)
  │
  ├── kvm_check_and_inject_events()
  │   ├── 检查待处理中断
  │   ├── 检查待处理NMI
  │   └── 检查待处理异常
  │
  ├── kvm_x86_call(vcpu_pre_run)()
  │   └── vmx_vcpu_pre_run()
  │       ├── 检查Guest状态有效性
  │       └── 设置emulation_required标志
  │
  └── kvm_x86_call(vcpu_run)()
      └── vmx_vcpu_run()
          └── VMENTER指令
```

---

## 7. kvm_vcpu_halt() - halt-polling实现

**文件**: `virt/kvm/kvm_main.c:3428-3478`

```c
/* 来源: virt/kvm/kvm_main.c */

static int kvm_vcpu_halt(struct kvm_vcpu *vcpu)
{
    bool starts_poll = false;
    ktime_t start;

    /* ============================================
     * Step 1: 决定是否开始halt-polling
     * ============================================ */
    if (vcpu->arch.halt_poll_allowed &&
        halt_poll_ns > 0) {
        starts_poll = true;
    }

    /* ============================================
     * Step 2: halt-polling循环
     * ============================================ */
    if (starts_poll) {
        start = ktime_get();

        while (1) {
            ktime_t now = ktime_get();
            s64 elapsed = ktime_to_ns(ktime_sub(now, start));

            /* 超过halt_poll_ns，停止轮询 */
            if (elapsed >= halt_poll_ns)
                break;

            /* 检查是否有事件到达 */
            if (kvm_arch_vcpu_runnable(vcpu))
                return 0;  /* 立即唤醒 */

            /* 短暂等待，让出CPU */
            cpu_relax();
        }
    }

    /* ============================================
     * Step 3: 真正阻塞
     * ============================================ */
    return kvm_vcpu_block(vcpu);
}
```

**halt-polling流程图**：
```
Guest执行HLT
  │
  └──→ vmx_handle_exit() → handle_halt()
         └── mp_state = KVM_MP_STATE_HALTED
         └── return 1 (继续vcpu_run循环)
  │
  └──→ vcpu_run()循环
         │
         └── if (mp_state == HALTED)
               └── kvm_vcpu_halt()
                   │
                   ├── halt-polling (轮询halt_poll_ns)
                   │   ├── 检查kvm_arch_vcpu_runnable()
                   │   │   └── 有中断? → return 0 (立即唤醒)
                   │   └── cpu_relax()
                   │
                   └── kvm_vcpu_block() (阻塞)
                       └── wait_event_interruptible()
                           └── 阻塞直到中断到达
```

**调优参数**：
- `halt_poll_ns`（默认400000ns = 400μs）
- 增大：降低中断延迟，但增加CPU占用
- 减小：降低CPU占用，但增加中断延迟

---

## 8. kvm_arch_vcpu_runnable() - 检查vCPU是否可运行

**文件**: `arch/x86/kvm/x86.c:3281-3317`

```c
/* 来源: arch/x86/kvm/x86.c */

int kvm_arch_vcpu_runnable(struct kvm_vcpu *vcpu)
{
    /* ============================================
     * 检查1: 是否有待处理中断?
     * ============================================ */
    if (kvm_cpu_has_interrupt(vcpu))
        return 1;

    /* ============================================
     * 检查2: 是否有待处理NMI?
     * ============================================ */
    if (kvm_cpu_has_nmi(vcpu))
        return 1;

    /* ============================================
     * 检查3: 是否有待处理异常?
     * ============================================ */
    if (kvm_cpu_has_exception(vcpu))
        return 1;

    /* ============================================
     * 检查4: 是否有待处理SMI?
     * ============================================ */
    if (kvm_cpu_has_smi(vcpu))
        return 1;

    /* ============================================
     * 检查5: 是否有shutdown请求?
     * ============================================ */
    if (kvm_check_request(KVM_REQ_VM_DEAD, vcpu))
        return 1;

    /* 没有事件，继续阻塞 */
    return 0;
}
```

**检查顺序**：
```
kvm_arch_vcpu_runnable()
  │
  ├── kvm_cpu_has_interrupt()
  │   └── 检查vcpu->arch.irq_pending
  │   └── 检查vcpu->arch.apic->irr (中断请求寄存器)
  │
  ├── kvm_cpu_has_nmi()
  │   └── 检查vcpu->arch.nmi_pending
  │
  ├── kvm_cpu_has_exception()
  │   └── 检查vcpu->arch.exception.pending
  │
  └── kvm_cpu_has_smi()
      └── 检查vcpu->arch.smi_pending
```

---

## 9. vcpu_handle_exit() - VM-Exit处理分发

**文件**: `arch/x86/kvm/x86.c:3522-3615`

```c
/* 来源: arch/x86/kvm/x86.c */

static int vcpu_handle_exit(struct kvm_vcpu *vcpu, fastpath_t exit_fastpath)
{
    int r;

    /* ============================================
     * 检查1: 快速路径处理
     * ============================================ */
    if (exit_fastpath == EXIT_FASTPATH_REENTER_GUEST) {
        /* 可以直接重新进入Guest */
        return 1;
    }

    if (exit_fastpath == EXIT_FASTPATH_RETREAT) {
        /* 需要回退RIP */
        kvm_vcpu_reset_rip(vcpu);
        return 1;
    }

    /* ============================================
     * Step 2: 调用架构相关的exit处理
     * ============================================ */
    r = kvm_x86_call(handle_exit)(vcpu, exit_fastpath);

    /* ============================================
     * Step 3: 处理返回值
     * ============================================ */
    if (r <= 0) {
        /* r < 0: 出错 */
        /* r == 0: 需要返回用户空间 */
        return r;
    }

    /* ============================================
     * Step 4: 检查是否需要返回用户空间
     * ============================================ */
    if (vcpu->run->exit_reason != KVM_EXIT_UNKNOWN) {
        /* 需要返回用户空间 (MMIO, CPUID等) */
        return 0;
    }

    /* 继续循环 */
    return 1;
}
```

**exit_fastpath类型**：
```
EXIT_FASTPATH_NONE:
  └── 完整的exit处理，可能返回用户空间

EXIT_FASTPATH_REENTER_GUEST:
  └── 可以直接重新进入Guest (如外部中断已处理)

EXIT_FASTPATH_RETREAT:
  └── 需要回退RIP后重新进入 (如IO指令)
```

**常见exit_reason及处理**：
```
exit_reason = EXTERNAL_INTERRUPT:
  └── 快速路径: 注入中断到Guest，重新进入

exit_reason = EPT_VIOLATION:
  └── 慢速路径: 处理页错误，重新进入

exit_reason = IO_INSTRUCTION:
  └── 慢速路径: 返回用户空间，QEMU模拟

exit_reason = CPUID:
  └── 慢速路径: 返回用户空间，QEMU处理

exit_reason = HLT:
  └── 快速路径: 设置mp_state=HALTED，继续循环
```

---

## 10. 关键数据结构关系图

```
┌─────────────────────────────────────────────────────────────────────┐
│  KVM 核心数据结构关系                                                │
│                                                                     │
│  ┌─────────────┐                                                    │
│  │ struct kvm  │←──── VM实例                                        │
│  └──────┬──────┘                                                    │
│         │                                                           │
│         ├──→ memslots (RCU保护)                                     │
│         │    └──→ struct kvm_memslots                               │
│         │         └──→ slots[KVM_MEM_SLOTS_NUM]                     │
│         │              └──→ struct kvm_memory_slot                  │
│         │                   ├── id (slot编号)                       │
│         │                   ├── base_gfn (GPA起始)                  │
│         │                   ├── npages (页数)                       │
│         │                   ├── userspace_addr (HVA)                │
│         │                   └── arch (架构相关)                     │
│         │                                                           │
│         ├──→ vcpus[KVM_MAX_VCPUS]                                   │
│         │    └──→ struct kvm_vcpu                                   │
│         │         ├── kvm (所属VM)                                  │
│         │         ├── vcpu_id (vCPU编号)                            │
│         │         ├── cpu (当前pCPU)                                │
│         │         ├── run (kvm_run共享内存)                         │
│         │         ├── arch (x86扩展)                                │
│         │         │    ├── cr0, cr3, cr4, efer                      │
│         │         │    ├── apic (虚拟LAPIC)                         │
│         │         │    ├── mmu (MMU上下文)                          │
│         │         │    └── mp_state (运行状态)                      │
│         │         └── wq (等待队列)                                 │
│         │                                                           │
│         ├──→ irq_routing (中断路由表)                               │
│         │    └──→ struct kvm_irq_routing_table                      │
│         │         └──→ entries[]                                    │
│         │              └──→ struct kvm_irq_routing_entry            │
│         │                   ├── gsi (全局中断号)                    │
│         │                   ├── type (IRQCHIP/MSI)                  │
│         │                   └── u (目标信息)                        │
│         │                                                           │
│         └──→ arch (x86架构扩展)                                     │
│              └──→ struct kvm_arch                                   │
│                   ├── ept_pointers[] (EPT页表基址)                  │
│                   ├── tsc_offset (TSC偏移)                          │
│                   └── ... (其他架构相关字段)                        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 11. VM-Exit处理分类

```
VM-Exit发生后的处理路径:

┌─ 快速路径 (内核态直接处理) ─────────────────────────────────┐
│                                                              │
│  EXIT_REASON_EXTERNAL_INTERRUPT:                            │
│    └── 注入中断到Guest，重新进入                             │
│                                                              │
│  EXIT_REASON_HLT:                                           │
│    └── 设置mp_state=HALTED，halt-polling                     │
│                                                              │
│  EXIT_REASON_EPT_VIOLATION:                                 │
│    └── 处理页错误，建立EPT映射，重新进入                     │
│                                                              │
│  EXIT_REASON_PREEMPTION_TIMER:                              │
│    └── 更新抢占定时器，重新进入                              │
│                                                              │
└──────────────────────────────────────────────────────────────┘

┌─ 慢速路径 (返回用户空间) ───────────────────────────────────┐
│                                                              │
│  EXIT_REASON_IO_INSTRUCTION:                                │
│    └── vcpu->run->exit_reason = KVM_EXIT_IO                 │
│    └── return 0 (返回用户空间，QEMU模拟)                     │
│                                                              │
│  EXIT_REASON_CPUID:                                         │
│    └── vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR     │
│    └── return 0 (返回用户空间，QEMU处理)                     │
│                                                              │
│  EXIT_REASON_MSR_READ/WRITE:                                │
│    └── vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR     │
│    └── return 0 (返回用户空间，QEMU处理)                     │
│                                                              │
│  EXIT_REASON_MMIO:                                          │
│    └── vcpu->run->exit_reason = KVM_EXIT_MMIO               │
│    └── return 0 (返回用户空间，QEMU模拟)                     │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

---

## 12. 并发模型

```
KVM 并发模型:

┌─ vCPU线程 ─────────────────────────────────────────────────┐
│                                                             │
│  每个vCPU运行在独立的线程中                                  │
│  QEMU为每个vCPU创建一个线程                                  │
│  线程调用ioctl(KVM_RUN)进入vcpu_run()循环                   │
│                                                             │
│  并发点:                                                     │
│  ├── 多个vCPU同时处理EPT页错误                              │
│  ├── 多个vCPU同时注入中断                                   │
│  └── vCPU线程与QEMU线程并发访问kvm_run                      │
│                                                             │
└─────────────────────────────────────────────────────────────┘

┌─ 锁机制 ───────────────────────────────────────────────────┐
│                                                             │
│  kvm->mmu_lock (spinlock):                                  │
│    └── 保护EPT页表操作                                      │
│    └── 在EPT页错误处理时持有                                │
│                                                             │
│  kvm->slots_lock (mutex):                                   │
│    └── 保护memslots操作                                     │
│    └── 在KVM_SET_USER_MEMORY_REGION时持有                   │
│                                                             │
│  kvm->irq_lock (spinlock):                                  │
│    └── 保护中断路由表                                       │
│    └── 在KVM_SET_GSI_ROUTING时持有                          │
│                                                             │
│  RCU保护:                                                    │
│    └── memslots通过RCU保护，读端无锁                         │
│    └── 使用srcu_read_lock/unlock访问                        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 13. 内存屏障和原子操作

```
关键内存屏障:

1. memslot访问:
   srcu_read_lock(&kvm->srcu)
   memslot = srcu_dereference(kvm->memslots)
   ...
   srcu_read_unlock(&kvm->srcu)

2. EPT页表更新:
   spin_lock(&kvm->mmu_lock)
   ... 更新EPT页表 ...
   spin_unlock(&kvm->mmu_lock)

3. 中断注入:
   atomic_set(&vcpu->arch.irq_pending, 1)
   kvm_vcpu_kick(vcpu)

4. vCPU状态:
   vcpu->arch.mp_state = KVM_MP_STATE_HALTED
   smp_wmb()  ← 写屏障
   wake_up_interruptible(&vcpu->wq)
```
