# Phase 0：源码精读注释 - KVM框架层

> 基于 Linux 6.12.93 源码（实际代码行号已验证）

---

## 1. kvm_dev_ioctl() - KVM设备ioctl入口

**文件**: `virt/kvm/kvm_main.c:5535-5568`

这是KVM设备文件（`/dev/kvm`）的ioctl处理入口，负责处理系统级的KVM操作。

```c
/* 来源: virt/kvm/kvm_main.c:5535-5568 */

static long kvm_dev_ioctl(struct file *filp,
			  unsigned int ioctl, unsigned long arg)
{
	int r = -EINVAL;

	switch (ioctl) {
	case KVM_GET_API_VERSION:
		if (arg)
			goto out;
		r = KVM_API_VERSION;     /* 返回12 */
		break;
	case KVM_CREATE_VM:
		r = kvm_dev_ioctl_create_vm(arg);
		break;
	case KVM_CHECK_EXTENSION:
		r = kvm_vm_ioctl_check_extension_generic(NULL, arg);
		break;
	case KVM_GET_VCPU_MMAP_SIZE:
		if (arg)
			goto out;
		r = PAGE_SIZE;     /* struct kvm_run */
#ifdef CONFIG_X86
		r += PAGE_SIZE;    /* pio data page */
#endif
#ifdef CONFIG_KVM_MMIO
		r += PAGE_SIZE;    /* coalesced mmio ring page */
#endif
		break;
	default:
		return kvm_arch_dev_ioctl(filp, ioctl, arg);
	}
out:
	return r;
}
```

**学习要点**：
- 这是用户空间与KVM的第一次交互入口（打开 `/dev/kvm` 后调用 ioctl）
- `KVM_CREATE_VM` 是最关键的 ioctl，创建VM实例
- `KVM_GET_VCPU_MMAP_SIZE` 返回用户态需要 mmap 的大小：在 x86 上通常是 `3 * PAGE_SIZE`
  - 第1页：`struct kvm_run`（用户态和内核态共享的vCPU状态）
  - 第2页：PIO 数据缓冲区
  - 第3页：coalesced MMIO 环形缓冲区
- `default` 分支会走到 `kvm_arch_dev_ioctl()`，处理架构特定的 ioctl

---

## 2. kvm_dev_ioctl_create_vm() - 创建VM实例

**文件**: `virt/kvm/kvm_main.c:5492-5533`

```c
/* 来源: virt/kvm/kvm_main.c:5492-5533 */

static int kvm_dev_ioctl_create_vm(unsigned long type)
{
	char fdname[ITOA_MAX_LEN + 1];
	int r, fd;
	struct kvm *kvm;
	struct file *file;

	/* ============================================
	 * Step 1: 分配文件描述符
	 * ============================================ */
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	snprintf(fdname, sizeof(fdname), "%d", fd);

	/* ============================================
	 * Step 2: 创建VM (核心分配逻辑)
	 * ============================================ */
	kvm = kvm_create_vm(type, fdname);
	if (IS_ERR(kvm)) {
		r = PTR_ERR(kvm);
		goto put_fd;
	}

	/* ============================================
	 * Step 3: 创建匿名inode文件
	 * ============================================ */
	file = anon_inode_getfile("kvm-vm", &kvm_vm_fops, kvm, O_RDWR);
	if (IS_ERR(file)) {
		r = PTR_ERR(file);
		goto put_kvm;
	}

	/* 发送uevent通知 (用于cgroup等) */
	kvm_uevent_notify_change(KVM_EVENT_CREATE_VM, kvm);

	/* ============================================
	 * Step 4: 安装文件描述符到当前进程
	 * ============================================ */
	fd_install(fd, file);
	return fd;

put_kvm:
	kvm_put_kvm(kvm);
put_fd:
	put_unused_fd(fd);
	return r;
}
```

**注意**：一旦 `anon_inode_getfile()` 成功，就不能再调用 `kvm_put_kvm()` 了——因为 `file->f_op->release`（即 `kvm_vm_release()`）会在文件最终 fput 时负责清理。

---

## 3. kvm_create_vm() - VM分配与初始化

**文件**: `virt/kvm/kvm_main.c:1146-1265`

这是VM创建的核心函数，初始化大量数据结构。

```c
/* 来源: virt/kvm/kvm_main.c:1146-1265 (简化) */

static struct kvm *kvm_create_vm(unsigned long type, const char *fdname)
{
	struct kvm *kvm = kvm_arch_alloc_vm();   /* 架构相关分配 */
	struct kvm_memslots *slots;
	int r, i, j;

	if (!kvm)
		return ERR_PTR(-ENOMEM);

	/* ============================================
	 * 基础锁和链表初始化
	 * ============================================ */
	KVM_MMU_LOCK_INIT(kvm);
	mmgrab(current->mm);
	kvm->mm = current->mm;                /* ★ 继承调用者的内存空间 */
	kvm_eventfd_init(kvm);
	mutex_init(&kvm->lock);               /* VM级通用锁 */
	mutex_init(&kvm->irq_lock);           /* 中断路由锁 */
	mutex_init(&kvm->slots_lock);         /* memslot操作锁 */
	mutex_init(&kvm->slots_arch_lock);
	xa_init(&kvm->vcpu_array);            /* vCPU xarray */

	/* ============================================
	 * SRCU结构初始化 (用于无锁读侧访问memslots)
	 * ============================================ */
	init_srcu_struct(&kvm->srcu);
	init_srcu_struct(&kvm->irq_srcu);

	r = kvm_init_irq_routing(kvm);        /* 初始化中断路由表 */
	refcount_set(&kvm->users_count, 1);   /* 引用计数 */

	/* ============================================
	 * memslots初始化 (每个address space有2个slot数组用于双缓冲)
	 * ============================================ */
	for (i = 0; i < kvm_arch_nr_memslot_as_ids(kvm); i++) {
		for (j = 0; j < 2; j++) {
			slots = &kvm->__memslots[i][j];
			slots->hva_tree = RB_ROOT_CACHED;  /* HVA红黑树 */
			slots->gfn_tree = RB_ROOT;         /* GFN红黑树 */
			hash_init(slots->id_hash);         /* id哈希表 */
			slots->generation = i;
		}
		rcu_assign_pointer(kvm->memslots[i], &kvm->__memslots[i][0]);
	}

	/* ============================================
	 * IO总线初始化 (PIO/MMIO)
	 * ============================================ */
	for (i = 0; i < KVM_NR_BUSES; i++) {
		rcu_assign_pointer(kvm->buses[i],
			kzalloc(sizeof(struct kvm_io_bus), GFP_KERNEL_ACCOUNT));
	}

	/* ============================================
	 * 架构相关初始化
	 * ============================================ */
	r = kvm_arch_init_vm(kvm, type);
	r = kvm_enable_virtualization();      /* 启用硬件虚拟化 (VMX/SVM) */
	r = kvm_init_mmu_notifier(kvm);       /* 注册MMU notifier */
	r = kvm_coalesced_mmio_init(kvm);     /* coalesced MMIO */
	r = kvm_create_vm_debugfs(kvm, fdname);
	r = kvm_arch_post_init_vm(kvm);       /* 架构后初始化 */

	/* 注册到全局VM链表 */
	mutex_lock(&kvm_lock);
	list_add(&kvm->vm_list, &vm_list);
	mutex_unlock(&kvm_lock);

	preempt_notifier_inc();
	kvm_init_pm_notifier(kvm);

	return kvm;

	/* 错误清理路径省略... */
}
```

**调用链**：
```
kvm_dev_ioctl_create_vm(type)
  └→ kvm_create_vm(type, fdname)
       ├→ kvm_arch_alloc_vm()              ← x86: kzalloc(sizeof(struct kvm_vcpu) + ...)
       ├→ kvm->mm = current->mm            ← 继承QEMU进程的内存空间
       ├→ init_srcu_struct(&kvm->srcu)     ← SRCU用于无锁读memslots
       ├→ kvm_init_irq_routing()           ← 初始化中断路由表
       ├→ 初始化 memslots[i][0/1]          ← 双缓冲memslot数组
       ├→ kvm_arch_init_vm()               ← x86: 初始化VMCS配置等
       ├→ kvm_enable_virtualization()      ← 启用VMX/SVM
       ├→ kvm_init_mmu_notifier()          ← 注册mmu_notifier
       ├→ kvm_coalesced_mmio_init()        ← MMIO合并
       ├→ kvm_arch_post_init_vm()          ← x86: tdp_mmu_init等
       └→ 加入全局vm_list
```

---

## 4. kvm_vcpu_ioctl() - vCPU ioctl入口

**文件**: `virt/kvm/kvm_main.c:4445-4670`

```c
/* 来源: virt/kvm/kvm_main.c:4445-4670 (简化) */

static long kvm_vcpu_ioctl(struct file *filp,
			   unsigned int ioctl, unsigned long arg)
{
	struct kvm_vcpu *vcpu = filp->private_data;
	void __user *argp = (void __user *)arg;
	int r;

	/* ============================================
	 * 前置检查: 确保调用者的mm与VM的mm一致
	 * ============================================ */
	if (vcpu->kvm->mm != current->mm || vcpu->kvm->vm_dead)
		return -EIO;

	/* 先尝试异步ioctl (不需要vcpu->mutex的) */
	r = kvm_arch_vcpu_async_ioctl(filp, ioctl, arg);
	if (r != -ENOIOCTLCMD)
		return r;

	/* ★ 获取vCPU互斥锁 (串行化对同一vCPU的操作) */
	if (mutex_lock_killable(&vcpu->mutex))
		return -EINTR;

	switch (ioctl) {

	/* ============================================
	 * ★ KVM_RUN - 运行vCPU (最核心!)
	 * ============================================ */
	case KVM_RUN: {
		struct pid *oldpid;
		r = -EINVAL;
		if (arg)
			goto out;

		/* 检查vCPU线程是否发生变化 */
		oldpid = rcu_access_pointer(vcpu->pid);
		if (unlikely(oldpid != task_pid(current))) {
			/* 线程改变了，需要更新pid和通知架构层 */
			r = kvm_arch_vcpu_run_pid_change(vcpu);
			if (r)
				break;
			newpid = get_task_pid(current, PIDTYPE_PID);
			rcu_assign_pointer(vcpu->pid, newpid);
			if (oldpid)
				synchronize_rcu();
			put_pid(oldpid);
		}

		/* ★ 设置wants_to_run标志 (受immediate_exit控制) */
		vcpu->wants_to_run = !READ_ONCE(vcpu->run->immediate_exit__unsafe);

		/* ★★ 进入架构相关的vCPU运行函数 */
		r = kvm_arch_vcpu_ioctl_run(vcpu);

		vcpu->wants_to_run = false;
		trace_kvm_userspace_exit(vcpu->run->exit_reason, r);
		break;
	}

	case KVM_GET_REGS: { /* 获取通用寄存器 */
		struct kvm_regs *kvm_regs = kzalloc(sizeof(*kvm_regs), GFP_KERNEL);
		r = kvm_arch_vcpu_ioctl_get_regs(vcpu, kvm_regs);
		if (!r && copy_to_user(argp, kvm_regs, sizeof(*kvm_regs)))
			r = -EFAULT;
		kfree(kvm_regs);
		break;
	}

	case KVM_SET_REGS: { /* 设置通用寄存器 */
		struct kvm_regs *kvm_regs = memdup_user(argp, sizeof(*kvm_regs));
		r = kvm_arch_vcpu_ioctl_set_regs(vcpu, kvm_regs);
		kfree(kvm_regs);
		break;
	}

	case KVM_GET_SREGS: { /* 获取特殊寄存器 (CR0/CR3/CR4/EFER/段寄存器等) */
		/* ... kzalloc + kvm_arch_vcpu_ioctl_get_sregs + copy_to_user ... */
	}

	case KVM_SET_SREGS: { /* 设置特殊寄存器 (必须在KVM_RUN前调用) */
		/* ... memdup_user + kvm_arch_vcpu_ioctl_set_sregs ... */
	}

	case KVM_GET_MP_STATE: { /* 获取多处理器状态 */ }
	case KVM_SET_MP_STATE: { /* 设置多处理器状态 */ }
	case KVM_TRANSLATE:    { /* 地址翻译 (GVA→GPA) */ }
	case KVM_SET_GUEST_DEBUG: { /* 设置Guest调试 */ }
	case KVM_SET_SIGNAL_MASK:   { /* 设置vCPU信号掩码 */ }
	case KVM_GET_FPU:  { /* 获取FPU状态 */ }
	case KVM_SET_FPU:  { /* 设置FPU状态 */ }
	case KVM_GET_STATS_FD: { /* 获取统计文件描述符 */ }
#ifdef CONFIG_KVM_GENERIC_PRE_FAULT_MEMORY
	case KVM_PRE_FAULT_MEMORY: { /* 预缺页处理 */ }
#endif
	default:
		r = kvm_arch_vcpu_ioctl(filp, ioctl, arg);
	}
out:
	mutex_unlock(&vcpu->mutex);
	return r;
}
```

**学习要点**：
- `KVM_RUN` 是核心，进入 `kvm_arch_vcpu_ioctl_run()`
- 所有 vCPU ioctl 都受 `vcpu->mutex` 保护（异步ioctl除外）
- 寄存器操作采用 `kzalloc/memdup_user` + 架构回调模式：通用层处理用户空间拷贝，架构层处理实际寄存器
- `vcpu->wants_to_run` 控制是否真正进入vCPU运行循环，受 `kvm_run->immediate_exit__unsafe` 控制

---

## 5. kvm_arch_vcpu_ioctl_run() - x86 vCPU运行入口

**文件**: `arch/x86/kvm/x86.c:11579-11697`

这是KVM中最关键的函数之一，负责从用户态进入Guest执行。

```c
/* 来源: arch/x86/kvm/x86.c:11579-11697 */

int kvm_arch_vcpu_ioctl_run(struct kvm_vcpu *vcpu)
{
	struct kvm_queued_exception *ex = &vcpu->arch.exception;
	struct kvm_run *kvm_run = vcpu->run;
	u32 sync_valid_fields;
	int r;

	/* ============================================
	 * Step 1: MMU后初始化 (每轮运行前)
	 * ============================================ */
	r = kvm_mmu_post_init_vm(vcpu->kvm);
	if (r)
		return r;

	/* ============================================
	 * Step 2: 加载vCPU到当前pCPU
	 * ============================================ */
	vcpu_load(vcpu);

	/* ============================================
	 * Step 3: 激活信号集 (如果用户设置了自定义信号掩码)
	 * ============================================ */
	kvm_sigset_activate(vcpu);

	kvm_run->flags = 0;

	/* ============================================
	 * Step 4: 加载Guest FPU状态
	 * ============================================ */
	kvm_load_guest_fpu(vcpu);

	/* 获取SRCU读侧锁 (用于安全访问memslots) */
	kvm_vcpu_srcu_read_lock(vcpu);

	/* ============================================
	 * Step 5: 处理未初始化状态
	 * ============================================ */
	if (unlikely(vcpu->arch.mp_state == KVM_MP_STATE_UNINITIALIZED)) {
		if (!vcpu->wants_to_run) {
			r = -EINTR;
			goto out;
		}
		/* 阻塞等待INIT信号 */
		kvm_vcpu_srcu_read_unlock(vcpu);
		kvm_vcpu_block(vcpu);
		kvm_vcpu_srcu_read_lock(vcpu);

		if (kvm_apic_accept_events(vcpu) < 0) {
			r = 0;
			goto out;
		}
		r = -EAGAIN;
		if (signal_pending(current)) {
			r = -EINTR;
			kvm_run->exit_reason = KVM_EXIT_INTR;
			++vcpu->stat.signal_exits;
		}
		goto out;
	}

	/* ============================================
	 * Step 6: 验证并同步寄存器
	 * ============================================ */
	sync_valid_fields = kvm_sync_valid_fields(vcpu->kvm);
	if ((kvm_run->kvm_valid_regs & ~sync_valid_fields) ||
	    (kvm_run->kvm_dirty_regs & ~sync_valid_fields)) {
		r = -EINVAL;
		goto out;
	}

	if (kvm_run->kvm_dirty_regs) {
		r = sync_regs(vcpu);       /* 从kvm_run同步到vCPU寄存器 */
		if (r != 0)
			goto out;
	}

	/* ============================================
	 * Step 7: 重新同步TPR (用户态LAPIC模式)
	 * ============================================ */
	if (!lapic_in_kernel(vcpu)) {
		if (kvm_set_cr8(vcpu, kvm_run->cr8) != 0) {
			r = -EINVAL;
			goto out;
		}
	}

	/* ============================================
	 * Step 8: 处理来自用户空间的异常 (嵌套虚拟化)
	 * ============================================ */
	if (vcpu->arch.exception_from_userspace && is_guest_mode(vcpu) &&
	    kvm_x86_ops.nested_ops->is_exception_vmexit(vcpu, ex->vector,
							 ex->error_code)) {
		kvm_queue_exception_vmexit(vcpu, ex->vector,
					   ex->has_error_code, ex->error_code,
					   ex->has_payload, ex->payload);
		ex->injected = false;
		ex->pending = false;
	}
	vcpu->arch.exception_from_userspace = false;

	/* ============================================
	 * Step 9: 处理MMIO/PIO完成回调
	 * ============================================ */
	if (unlikely(vcpu->arch.complete_userspace_io)) {
		int (*cui)(struct kvm_vcpu *) = vcpu->arch.complete_userspace_io;
		vcpu->arch.complete_userspace_io = NULL;
		r = cui(vcpu);
		if (r <= 0)
			goto out;
	} else {
		WARN_ON_ONCE(vcpu->arch.pio.count);
		WARN_ON_ONCE(vcpu->mmio_needed);
	}

	if (!vcpu->wants_to_run) {
		r = -EINTR;
		goto out;
	}

	/* ============================================
	 * Step 10: 架构预处理
	 * ============================================ */
	r = kvm_x86_call(vcpu_pre_run)(vcpu);
	if (r <= 0)
		goto out;

	/* ============================================
	 * ★ Step 11: 进入vCPU运行主循环
	 * ============================================ */
	r = vcpu_run(vcpu);

out:
	/* ============================================
	 * 清理工作
	 * ============================================ */
	kvm_put_guest_fpu(vcpu);
	if (kvm_run->kvm_valid_regs && likely(!vcpu->arch.guest_state_protected))
		store_regs(vcpu);             /* 保存vCPU寄存器到kvm_run */
	post_kvm_run_save(vcpu);           /* 保存其他状态 */
	kvm_vcpu_srcu_read_unlock(vcpu);

	kvm_sigset_deactivate(vcpu);
	vcpu_put(vcpu);
	return r;
}
```

**完整执行流程**：
```
kvm_arch_vcpu_ioctl_run(vcpu)
  │
  ├── kvm_mmu_post_init_vm()               ← MMU后初始化
  ├── vcpu_load(vcpu)                      ← 绑定vCPU到当前pCPU
  ├── kvm_sigset_activate(vcpu)            ← 激活信号掩码
  ├── kvm_load_guest_fpu(vcpu)             ← 加载Guest FPU
  ├── kvm_vcpu_srcu_read_lock(vcpu)        ← 获取SRCU读锁
  │
  ├── [mp_state == UNINITIALIZED]
  │   └── kvm_vcpu_block() + kvm_apic_accept_events()
  │
  ├── sync_regs(vcpu)                      ← 从kvm_run同步dirty寄存器
  ├── [嵌套] 处理 exception_from_userspace
  ├── [!] complete_userspace_io 回调
  │
  ├── kvm_x86_call(vcpu_pre_run)(vcpu)     ← vmx_vcpu_pre_run()
  │   └── 检查Guest状态有效性
  │   └── 设置 emulation_required 标志
  │
  ├── ★ vcpu_run(vcpu)                     ← 核心运行循环
  │
  └── 清理:
      ├── kvm_put_guest_fpu(vcpu)
      ├── store_regs(vcpu)                 ← 寄存器 → kvm_run
      ├── post_kvm_run_save(vcpu)
      ├── kvm_vcpu_srcu_read_unlock(vcpu)
      ├── kvm_sigset_deactivate(vcpu)
      └── vcpu_put(vcpu)
```

**关键洞察**：
- 这个函数管理着从**用户态到Guest**的完整状态转换
- FPU 状态在用户态和Guest之间切换（`kvm_load_guest_fpu` / `kvm_put_guest_fpu`）
- SRCU 读锁保护整个运行期间对 memslots 的安全访问
- `kvm_run` 是用户态和内核态之间的共享通信结构
- `vcpu->wants_to_run` 由 `kvm_run->immediate_exit__unsafe` 控制

---

## 6. vcpu_run() - vCPU运行主循环

**文件**: `arch/x86/kvm/x86.c:11343-11391`

这是KVM中最核心的执行循环，反复将vCPU送入Guest执行。

```c
/* 来源: arch/x86/kvm/x86.c:11343-11391 */

static int vcpu_run(struct kvm_vcpu *vcpu)
{
	int r;

	vcpu->run->exit_reason = KVM_EXIT_UNKNOWN;

	for (;;) {
		vcpu->arch.at_instruction_boundary = false;

		/* ============================================
		 * ★ 判断: vCPU是否可以运行?
		 * 如果可运行 → 进入Guest
		 * 如果不可运行(如halted) → 阻塞等待
		 * ============================================ */
		if (kvm_vcpu_running(vcpu)) {
			r = vcpu_enter_guest(vcpu);
		} else {
			r = vcpu_block(vcpu);
		}

		if (r <= 0)
			break;

		/* ============================================
		 * 以下在每次VM-Exit后、重新进入Guest前执行
		 * ============================================ */

		/* 清除UNBLOCK请求 */
		kvm_clear_request(KVM_REQ_UNBLOCK, vcpu);

		/* Xen PV事件注入 */
		if (kvm_xen_has_pending_events(vcpu))
			kvm_xen_inject_pending_events(vcpu);

		/* 注入待处理的定时器中断 */
		if (kvm_cpu_has_pending_timer(vcpu))
			kvm_inject_pending_timer_irqs(vcpu);

		/* 检查是否需要中断窗口退出 (用户态需要注入中断) */
		if (dm_request_for_irq_injection(vcpu) &&
			kvm_vcpu_ready_for_interrupt_injection(vcpu)) {
			r = 0;
			vcpu->run->exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
			++vcpu->stat.request_irq_exits;
			break;
		}

		/* 处理通用guest-mode工作 (如调度、信号等) */
		if (__xfer_to_guest_mode_work_pending()) {
			kvm_vcpu_srcu_read_unlock(vcpu);
			r = xfer_to_guest_mode_handle_work(vcpu);
			kvm_vcpu_srcu_read_lock(vcpu);
			if (r)
				return r;
		}
	}

	return r;
}
```

**主循环流程图**：
```
vcpu_run()
  │
  └──→ for (;;) {
         │
         ├── kvm_vcpu_running(vcpu)?
         │   ├── 是 → vcpu_enter_guest(vcpu)
         │   │        └→ 进入Guest → VM-Exit → 处理exit
         │   │
         │   └── 否 → vcpu_block(vcpu)
         │            └→ halt-polling 或 真正阻塞
         │
         ├── if (r <= 0) break
         │   └── r < 0: 出错
         │   └── r == 0: 需要返回用户空间
         │
         ├── 清除 KVM_REQ_UNBLOCK
         ├── 注入 Xen PV 事件
         ├── 注入待处理定时器中断
         ├── 检查中断窗口请求 (→ KVM_EXIT_IRQ_WINDOW_OPEN)
         ├── 处理 xfer_to_guest_mode_work
         │
         └── 继续循环，重新判断
       }
```

**关键洞察**：
- `kvm_vcpu_running()` 只检查 mp_state == RUNNABLE 且未 apf.halted
- halt/block 逻辑被提取到 `vcpu_block()` 中，不在循环体内
- `vcpu_enter_guest()` 返回 `r > 0` 表示继续循环，`r == 0` 表示返回用户空间，`r < 0` 表示错误
- 定时器中断在每次循环迭代时检查并注入
- 中断窗口退出用于用户态中断注入场景（如 QEMU 需要模拟 PIC 但被锁住了）

---

## 7. vcpu_block() - vCPU阻塞/halt处理

**文件**: `arch/x86/kvm/x86.c:11273-11341`

当vCPU不可运行时（如执行了HLT指令），进入这个函数。

```c
/* 来源: arch/x86/kvm/x86.c:11272-11341 */

static inline int vcpu_block(struct kvm_vcpu *vcpu)
{
	bool hv_timer;

	if (!kvm_arch_vcpu_runnable(vcpu)) {
		/*
		 * ★ 在halt-polling/block之前切换到软件定时器
		 * 因为Guest的定时器可能使用hypervisor timer，
		 * 而hypervisor timer只在guest模式下运行
		 */
		hv_timer = kvm_lapic_hv_timer_in_use(vcpu);
		if (hv_timer)
			kvm_lapic_switch_to_sw_timer(vcpu);

		/* 释放SRCU锁后阻塞 */
		kvm_vcpu_srcu_read_unlock(vcpu);

		/* ★ 根据状态选择halt或block */
		if (vcpu->arch.mp_state == KVM_MP_STATE_HALTED)
			kvm_vcpu_halt(vcpu);        /* HALT: 带halt-polling */
		else
			kvm_vcpu_block(vcpu);       /* 其他: 直接阻塞 */

		kvm_vcpu_srcu_read_lock(vcpu);

		if (hv_timer)
			kvm_lapic_switch_to_hv_timer(vcpu);

		/*
		 * 如果仍然不可运行(如收到信号)，
		 * 返回1让vcpu_run循环处理 (返回用户空间)
		 */
		if (!kvm_arch_vcpu_runnable(vcpu))
			return 1;
	}

	/* 嵌套模式: 检查嵌套事件 */
	if (is_guest_mode(vcpu)) {
		int r = kvm_check_nested_events(vcpu);
		if (r < 0 && r != -EBUSY)
			return 0;
	}

	if (kvm_apic_accept_events(vcpu) < 0)
		return 0;

	/* 处理从HALTED/AP_RESET_HOLD状态的恢复 */
	switch (vcpu->arch.mp_state) {
	case KVM_MP_STATE_HALTED:
	case KVM_MP_STATE_AP_RESET_HOLD:
		vcpu->arch.pv.pv_unhalted = false;
		vcpu->arch.mp_state = KVM_MP_STATE_RUNNABLE;
		break;
	}

	return 1;   /* 继续vcpu_run循环 */
}
```

---

## 8. kvm_arch_vcpu_runnable() - 检查vCPU是否可运行

**文件**: `arch/x86/kvm/x86.c:11267-11269`

```c
/* 来源: arch/x86/kvm/x86.c:11267-11269 */

int kvm_arch_vcpu_runnable(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_running(vcpu) || kvm_vcpu_has_events(vcpu);
}
```

这个函数由两个子检查组成：

**kvm_vcpu_running()** (x86.c:11211-11215) — 检查vCPU是否处于可运行状态：
```c
static bool kvm_vcpu_running(struct kvm_vcpu *vcpu)
{
	return (vcpu->arch.mp_state == KVM_MP_STATE_RUNNABLE &&
		!vcpu->arch.apf.halted);
}
```

**kvm_vcpu_has_events()** (x86.c:11217-11265) — 检查是否有待处理事件：
```c
static bool kvm_vcpu_has_events(struct kvm_vcpu *vcpu)
{
	/* 异步缺页完成 */
	if (!list_empty_careful(&vcpu->async_pf.done))
		return true;

	/* INIT/SIPI 信号 */
	if (kvm_apic_has_pending_init_or_sipi(vcpu) &&
	    kvm_apic_init_sipi_allowed(vcpu))
		return true;

	/* PV unhalt */
	if (vcpu->arch.pv.pv_unhalted)
		return true;

	/* 待处理异常 */
	if (kvm_is_exception_pending(vcpu))
		return true;

	/* NMI */
	if (kvm_test_request(KVM_REQ_NMI, vcpu) ||
	    (vcpu->arch.nmi_pending && kvm_x86_call(nmi_allowed)(vcpu, false)))
		return true;

	/* SMI (系统管理模式) */
	if (kvm_test_request(KVM_REQ_SMI, vcpu) ||
	    (vcpu->arch.smi_pending && kvm_x86_call(smi_allowed)(vcpu, false)))
		return true;

	/* PMI (性能监控中断) */
	if (kvm_test_request(KVM_REQ_PMI, vcpu))
		return true;

	/* 外部中断 */
	if (kvm_arch_interrupt_allowed(vcpu) && kvm_cpu_has_interrupt(vcpu))
		return true;

	/* Hyper-V 合成定时器 */
	if (kvm_hv_has_stimer_pending(vcpu))
		return true;

	/* 嵌套虚拟化事件 */
	if (is_guest_mode(vcpu) && kvm_x86_ops.nested_ops->has_events &&
	    kvm_x86_ops.nested_ops->has_events(vcpu, false))
		return true;

	/* Xen PV 事件 */
	if (kvm_xen_has_pending_events(vcpu))
		return true;

	return false;
}
```

**检查层次**：
```
kvm_arch_vcpu_runnable(vcpu)
  │
  ├── kvm_vcpu_running(vcpu)
  │   ├── mp_state == KVM_MP_STATE_RUNNABLE
  │   └── !apf.halted (异步缺页未halt)
  │
  └── kvm_vcpu_has_events(vcpu)
      ├── async_pf.done 非空 (异步缺页完成)
      ├── INIT/SIPI 待处理
      ├── pv_unhalted (PV unhalt)
      ├── 待处理异常 (exception pending)
      ├── NMI 待处理
      ├── SMI 待处理
      ├── PMI 待处理
      ├── 外部中断可注入 + 有中断
      ├── Hyper-V 合成定时器
      ├── 嵌套虚拟化事件
      └── Xen PV 事件
```

---

## 9. kvm_vcpu_halt() - halt-polling实现

**文件**: `virt/kvm/kvm_main.c:3811-3882`

当Guest执行HLT时，vCPU进入halt状态。为了降低中断延迟，KVM使用halt-polling策略。

```c
/* 来源: virt/kvm/kvm_main.c:3811-3882 (简化) */

void kvm_vcpu_halt(struct kvm_vcpu *vcpu)
{
	unsigned int max_halt_poll_ns = kvm_vcpu_max_halt_poll_ns(vcpu);
	bool halt_poll_allowed = !kvm_arch_no_poll(vcpu);
	ktime_t start, cur, poll_end;
	bool waited = false;
	bool do_halt_poll;
	u64 halt_ns;

	if (vcpu->halt_poll_ns > max_halt_poll_ns)
		vcpu->halt_poll_ns = max_halt_poll_ns;

	do_halt_poll = halt_poll_allowed && vcpu->halt_poll_ns;

	start = cur = poll_end = ktime_get();

	/* ============================================
	 * ★ Phase 1: halt-polling 循环 (忙等待)
	 * 在vcpu->halt_poll_ns时间内忙等
	 * ============================================ */
	if (do_halt_poll) {
		ktime_t stop = ktime_add_ns(start, vcpu->halt_poll_ns);

		do {
			if (kvm_vcpu_check_block(vcpu) < 0)
				goto out;       /* 有事件到达，立即退出 */
			cpu_relax();
			poll_end = cur = ktime_get();
		} while (kvm_vcpu_can_poll(cur, stop));
	}

	/* ============================================
	 * ★ Phase 2: 真正阻塞
	 * 如果polling没有等到事件，则调度出去
	 * ============================================ */
	waited = kvm_vcpu_block(vcpu);

	cur = ktime_get();

out:
	halt_ns = ktime_to_ns(cur) - ktime_to_ns(start);

	/* 更新halt-polling统计 */
	if (do_halt_poll)
		update_halt_poll_stats(vcpu, start, poll_end, !waited);

	/* ============================================
	 * ★ Phase 3: 动态调整halt_poll_ns
	 * 自适应调整polling窗口大小
	 * ============================================ */
	if (halt_poll_allowed) {
		max_halt_poll_ns = kvm_vcpu_max_halt_poll_ns(vcpu);

		if (!vcpu_valid_wakeup(vcpu)) {
			shrink_halt_poll_ns(vcpu);   /* 无效唤醒 → 缩小 */
		} else if (max_halt_poll_ns) {
			if (halt_ns <= vcpu->halt_poll_ns)
				;   /* 在poll窗口内唤醒 → 保持不变 */
			else if (halt_ns > max_halt_poll_ns)
				shrink_halt_poll_ns(vcpu);  /* 长时间阻塞 → 缩小 */
			else if (vcpu->halt_poll_ns < max_halt_poll_ns)
				grow_halt_poll_ns(vcpu);    /* 短时间阻塞 → 增大 */
		} else {
			vcpu->halt_poll_ns = 0;
		}
	}
}
```

**halt-polling流程图**：
```
Guest执行HLT
  │
  └→ vmx_handle_exit() → handle_halt()
     └→ mp_state = KVM_MP_STATE_HALTED
     └→ return 1 (继续vcpu_run循环)
  │
  └→ vcpu_run() 循环
     │
     └→ kvm_vcpu_running() == false
        └→ vcpu_block()
           │
           ├── kvm_arch_vcpu_runnable()?
           │   ├── 是 → return 1 (有事件，不需要阻塞)
           │   └── 否 → 进入阻塞路径
           │
           ├── 切换到软件定时器
           │
           ├── mp_state == HALTED?
           │   ├── 是 → kvm_vcpu_halt()
           │   │        │
           │   │        ├── Phase 1: halt-polling (忙等)
           │   │        │   ├── kvm_vcpu_check_block()
           │   │        │   │   └→ kvm_arch_vcpu_runnable()?
           │   │        │   │       └→ 有事件 → return -1 (退出)
           │   │        │   ├── cpu_relax()
           │   │        │   └→ 超时 → Phase 2
           │   │        │
           │   │        ├── Phase 2: kvm_vcpu_block() (真正阻塞)
           │   │        │   └→ schedule() 让出CPU
           │   │        │   └→ 被唤醒 → 返回
           │   │        │
           │   │        └── Phase 3: 动态调整halt_poll_ns
           │   │            ├── 短唤醒 → grow (增大)
           │   │            └── 长阻塞 → shrink (缩小)
           │   │
           │   └── 否 → kvm_vcpu_block() (直接阻塞)
           │
           └── 恢复hypervisor定时器
```

**自适应halt-polling参数**：
- `vcpu->halt_poll_ns`：每个 vCPU 独立维护，动态调整（下面两条改的就是它）
- 上限：模块参数 `halt_poll_ns`，6.12.93 默认 **200000 ns = 200 μs**
  （`KVM_HALT_POLL_NS_DEFAULT`，`arch/x86/include/asm/kvm_host.h:71`）。
  ★ 截断点在 `kvm_vcpu_halt()`（`virt/kvm/kvm_main.c:3820-3821`），而真正用的上限由
  `kvm_vcpu_max_halt_poll_ns()`（`:3787-3802`）给出：**per-VM 的
  `kvm->max_halt_poll_ns` 优先于模块参数**（VMM 用 `KVM_CAP_HALT_POLL` 设）
- 增大：`grow_halt_poll_ns()`（`:3670-3687`）`val *= halt_poll_ns_grow`（默认 2），
  再用 `halt_poll_ns_grow_start`（默认 10 μs）**兜住下限**；`grow == 0` 直接不增长
- 缩小：`shrink_halt_poll_ns()`（`:3689-3706`）`val /= halt_poll_ns_shrink`（默认 2），
  但 **`shrink == 0` 表示立刻归零**；结果低于 `grow_start` 同样归零
- 增大条件：唤醒发生在窗口内（poll 有效但窗口太小）
- 缩小条件：窗口跑完仍未唤醒（poll 白烧 CPU）

参数默认值/权限的单一来源见 [`../phase9-performance/parameters.md`](../phase9-performance/parameters.md) §1；
"这样调到底值多少钱"的实测见
[`../phase9-performance/index.md`](../phase9-performance/index.md) §1.2。

---

## 10. vcpu_enter_guest() - 进入Guest的完整流程

**文件**: `arch/x86/kvm/x86.c:10777-11209`

这是KVM中最复杂的函数之一，处理进入Guest前的所有准备、执行VMENTER、以及处理VM-Exit。

```c
/* 来源: arch/x86/kvm/x86.c:10777-11209 (简化) */

static int vcpu_enter_guest(struct kvm_vcpu *vcpu)
{
	int r;
	bool req_int_win = dm_request_for_irq_injection(vcpu) &&
			   kvm_cpu_accept_dm_intr(vcpu);
	fastpath_t exit_fastpath;
	u64 run_flags, debug_ctl;
	bool req_immediate_exit = false;

	/* ════════════════════════════════════════════
	 * Part A: 处理待处理请求 (KVM_REQ_*)
	 * ════════════════════════════════════════════ */
	if (kvm_request_pending(vcpu)) {
		/* 致命错误 */
		if (kvm_check_request(KVM_REQ_VM_DEAD, vcpu)) {
			r = -EIO; goto out;
		}

		/* dirty ring同步 */
		if (kvm_dirty_ring_check_request(vcpu)) { r = 0; goto out; }

		/* 嵌套状态页加载 */
		if (kvm_check_request(KVM_REQ_GET_NESTED_STATE_PAGES, vcpu)) { ... }

		/* MMU相关 */
		if (kvm_check_request(KVM_REQ_MMU_FREE_OBSOLETE_ROOTS, vcpu))
			kvm_mmu_free_obsolete_roots(vcpu);
		if (kvm_check_request(KVM_REQ_MMU_SYNC, vcpu))
			kvm_mmu_sync_roots(vcpu);
		if (kvm_check_request(KVM_REQ_LOAD_MMU_PGD, vcpu))
			kvm_mmu_load_pgd(vcpu);

		/* TLB刷新 */
		if (kvm_check_request(KVM_REQ_TLB_FLUSH, vcpu))
			kvm_vcpu_flush_tlb_all(vcpu);
		kvm_service_local_tlb_flush_requests(vcpu);

		/* 时钟更新 */
		if (kvm_check_request(KVM_REQ_MASTERCLOCK_UPDATE, vcpu))
			kvm_update_masterclock(vcpu->kvm);
		if (kvm_check_request(KVM_REQ_GLOBAL_CLOCK_UPDATE, vcpu))
			kvm_gen_kvmclock_update(vcpu);
		if (kvm_check_request(KVM_REQ_CLOCK_UPDATE, vcpu))
			kvm_guest_time_update(vcpu);

		/* Triple Fault → 退出到用户空间 */
		if (kvm_test_request(KVM_REQ_TRIPLE_FAULT, vcpu)) {
			vcpu->run->exit_reason = KVM_EXIT_SHUTDOWN;
			r = 0; goto out;
		}

		/* 异步缺页halt */
		if (kvm_check_request(KVM_REQ_APF_HALT, vcpu)) {
			vcpu->arch.apf.halted = true; r = 1; goto out;
		}

		/* SMI/NMI/PMI 处理 */
		if (kvm_check_request(KVM_REQ_NMI, vcpu)) process_nmi(vcpu);
		if (kvm_check_request(KVM_REQ_PMI, vcpu)) kvm_pmu_deliver_pmi(vcpu);

		/* APICv更新 */
		if (kvm_check_request(KVM_REQ_APICV_UPDATE, vcpu))
			kvm_vcpu_update_apicv(vcpu);

		/* 更多请求处理... */
	}

	/* ════════════════════════════════════════════
	 * Part B: 事件注入
	 * ════════════════════════════════════════════ */
	if (kvm_check_request(KVM_REQ_EVENT, vcpu) || req_int_win ||
	    kvm_xen_has_interrupt(vcpu)) {
		++vcpu->stat.req_event;

		r = kvm_apic_accept_events(vcpu);
		if (r < 0) { r = 0; goto out; }

		if (vcpu->arch.mp_state == KVM_MP_STATE_INIT_RECEIVED) {
			r = 1; goto out;
		}

		/* ★ 检查并注入中断/异常/NMI */
		r = kvm_check_and_inject_events(vcpu, &req_immediate_exit);
		if (r < 0) { r = 0; goto out; }

		if (req_int_win)
			kvm_x86_call(enable_irq_window)(vcpu);

		if (kvm_lapic_enabled(vcpu)) {
			update_cr8_intercept(vcpu);
			kvm_lapic_sync_to_vapic(vcpu);
		}
	}

	/* ============================================
	 * Part C: 重新加载MMU (如果需要)
	 * ============================================ */
	r = kvm_mmu_reload(vcpu);
	if (unlikely(r))
		goto cancel_injection;

	/* ════════════════════════════════════════════
	 * Part D: 进入Guest (VMENTER)
	 * ════════════════════════════════════════════ */

	preempt_disable();
	kvm_x86_call(prepare_switch_to_guest)(vcpu);

	/* 关闭中断，防止posted interrupt干扰 */
	local_irq_disable();

	/* ★ 设置vCPU模式为 IN_GUEST_MODE */
	smp_store_release(&vcpu->mode, IN_GUEST_MODE);

	kvm_vcpu_srcu_read_unlock(vcpu);
	smp_mb__after_srcu_read_unlock();

	/* 同步posted interrupt (PIR → IRR) */
	if (kvm_lapic_enabled(vcpu))
		kvm_x86_call(sync_pir_to_irr)(vcpu);

	/* 检查是否需要立即退出 */
	if (kvm_vcpu_exit_request(vcpu)) {
		vcpu->mode = OUTSIDE_GUEST_MODE;
		local_irq_enable();
		preempt_enable();
		kvm_vcpu_srcu_read_lock(vcpu);
		r = 1; goto cancel_injection;
	}

	/* 加载调试寄存器 */
	if (vcpu->arch.switch_db_regs) {
		set_debugreg(DR7_FIXED_1, 7);
		/* ... 加载DR0-DR3 ... */
	}

	/* 更新guest timing */
	guest_timing_enter_irqoff();

	/* ★★★★★ 核心VMENTER循环 ★★★★★ */
	for (;;) {
		exit_fastpath = kvm_x86_call(vcpu_run)(vcpu, run_flags);
		if (likely(exit_fastpath != EXIT_FASTPATH_REENTER_GUEST))
			break;
		/* REENTER: 直接重新进入，跳过完整exit处理 */
		if (kvm_lapic_enabled(vcpu))
			kvm_x86_call(sync_pir_to_irr)(vcpu);
		if (unlikely(kvm_vcpu_exit_request(vcpu))) {
			exit_fastpath = EXIT_FASTPATH_EXIT_HANDLED;
			break;
		}
		run_flags = 0;
		++vcpu->stat.exits;
	}

	/* 恢复调试寄存器 */
	if (unlikely(vcpu->arch.switch_db_regs & KVM_DEBUGREG_WONT_EXIT)) {
		kvm_x86_call(sync_dirty_debug_regs)(vcpu);
		/* ... */
	}

	/* 记录进入的CPU和TSC */
	vcpu->arch.last_vmentry_cpu = vcpu->cpu;
	vcpu->arch.last_guest_tsc = kvm_read_l1_tsc(vcpu, rdtsc());

	/* ★ 设置模式为 OUTSIDE_GUEST_MODE */
	vcpu->mode = OUTSIDE_GUEST_MODE;
	smp_wmb();

	/* 同步FPU xfd状态 */
	if (vcpu->arch.xfd_no_write_intercept)
		fpu_sync_guest_vmexit_xfd_state();

	/* ★ IRQoff阶段的exit处理 */
	kvm_x86_call(handle_exit_irqoff)(vcpu);

	/* 分支预测刷新 (安全性) */
	if (cpu_feature_enabled(X86_FEATURE_IBPB_EXIT_TO_USER))
		this_cpu_write(x86_ibpb_exit_to_user, true);

	/* ★ 处理pending中断 (VM-Exit后) */
	kvm_before_interrupt(vcpu, KVM_HANDLING_IRQ);
	local_irq_enable();
	++vcpu->stat.exits;        /* 统计VM-Exit次数 */
	local_irq_disable();
	kvm_after_interrupt(vcpu);

	/* 计算guest时间 */
	guest_timing_exit_irqoff();

	local_irq_enable();
	preempt_enable();

	kvm_vcpu_srcu_read_lock(vcpu);
	smp_mb__after_srcu_read_lock();

	/* ★ EXIT_FASTPATH_EXIT_USERSPACE: 需要返回用户空间 */
	if (unlikely(exit_fastpath == EXIT_FASTPATH_EXIT_USERSPACE))
		return 0;

	/* ★★ 调用架构相关的exit处理 */
	r = kvm_x86_call(handle_exit)(vcpu, exit_fastpath);
	return r;

cancel_injection:
	if (req_immediate_exit)
		kvm_make_request(KVM_REQ_EVENT, vcpu);
	kvm_x86_call(cancel_injection)(vcpu);
out:
	return r;
}
```

**完整流程图**：
```
vcpu_enter_guest(vcpu)
  │
  ├── Part A: 处理 KVM_REQ_* 请求
  │   ├── KVM_REQ_VM_DEAD → -EIO
  │   ├── KVM_REQ_TLB_FLUSH → TLB刷新
  │   ├── KVM_REQ_MMU_SYNC → MMU同步
  │   ├── KVM_REQ_LOAD_MMU_PGD → 加载MMU PGD
  │   ├── KVM_REQ_CLOCK_UPDATE → 时钟更新
  │   ├── KVM_REQ_TRIPLE_FAULT → KVM_EXIT_SHUTDOWN
  │   ├── KVM_REQ_NMI → 处理NMI
  │   ├── KVM_REQ_APF_HALT → 异步缺页halt
  │   ├── KVM_REQ_APICV_UPDATE → APICv更新
  │   └── ... 更多请求
  │
  ├── Part B: 事件注入
  │   ├── kvm_apic_accept_events() → 接受APIC事件
  │   ├── kvm_check_and_inject_events() → 注入中断/异常/NMI
  │   ├── enable_irq_window() → 打开中断窗口
  │   └── kvm_lapic_sync_to_vapic() → 同步LAPIC
  │
  ├── Part C: MMU重新加载
  │   └── kvm_mmu_reload(vcpu)
  │
  ├── Part D: VMENTER
  │   ├── preempt_disable()
  │   ├── local_irq_disable()
  │   ├── vcpu->mode = IN_GUEST_MODE
  │   ├── kvm_vcpu_srcu_read_unlock()
  │   ├── smp_mb__after_srcu_read_unlock()
  │   ├── sync_pir_to_irr() → posted interrupt同步
  │   │
  │   ├── ★ for (;;) {
  │   │     exit_fastpath = kvm_x86_call(vcpu_run)(vcpu, run_flags)
  │   │     │   └→ vmx_vcpu_run() → VMRESUME/VMRUN 指令
  │   │     │   └→ Guest执行 → VM-Exit发生
  │   │     if (exit_fastpath != REENTER_GUEST) break;
  │   │     ... // REENTER快速路径
  │   │   }
  │   │
  │   ├── vcpu->mode = OUTSIDE_GUEST_MODE
  │   ├── handle_exit_irqoff() → IRQoff阶段exit处理
  │   ├── local_irq_enable() → 处理pending IRQ
  │   ├── ++vcpu->stat.exits
  │   ├── preempt_enable()
  │   │
  │   └── kvm_x86_call(handle_exit)(vcpu, exit_fastpath)
  │       └→ vmx_handle_exit() → 分发到具体exit reason处理函数
  │
  └── return r
```

**exit_fastpath 类型**：
```
EXIT_FASTPATH_NONE:
  └── 正常VM-Exit，走完整的handle_exit处理路径

EXIT_FASTPATH_REENTER_GUEST:
  └── 可以直接重新进入Guest，不经过handle_exit
  └── 典型场景: posted interrupt处理完成，Guest可以继续

EXIT_FASTPATH_EXIT_HANDLED:
  └── exit已经处理完毕，直接返回

EXIT_FASTPATH_EXIT_USERSPACE:
  └── 需要返回用户空间处理 (MMIO, IO等)
  └── vcpu_enter_guest直接返回0
```

---

## 11. kvm_vcpu_block() - 底层阻塞原语

**文件**: `virt/kvm/kvm_main.c:3733-3763`

```c
/* 来源: virt/kvm/kvm_main.c:3733-3763 */

bool kvm_vcpu_block(struct kvm_vcpu *vcpu)
{
	struct rcuwait *wait = kvm_arch_vcpu_get_wait(vcpu);
	bool waited = false;

	vcpu->stat.generic.blocking = 1;

	preempt_disable();
	kvm_arch_vcpu_blocking(vcpu);     /* 架构通知: 即将阻塞 */
	prepare_to_rcuwait(wait);          /* 注册到rcuwait */
	preempt_enable();

	/* ============================================
	 * 循环阻塞: 每次唤醒都检查是否可以继续
	 * ============================================ */
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);

		/* ★ 检查是否应该唤醒 */
		if (kvm_vcpu_check_block(vcpu) < 0)
			break;

		waited = true;
		schedule();                    /* ★ 让出CPU */
	}

	preempt_disable();
	finish_rcuwait(wait);
	kvm_arch_vcpu_unblocking(vcpu);    /* 架构通知: 已唤醒 */
	preempt_enable();

	vcpu->stat.generic.blocking = 0;

	return waited;
}
```

`kvm_vcpu_check_block()` 内部就是调用 `kvm_arch_vcpu_runnable()` —— 如果vCPU有事件或可运行，则返回负值（需要唤醒）。

---

## 12. 关键数据结构关系图

```
┌─────────────────────────────────────────────────────────────────────┐
│  KVM 核心数据结构关系                                                │
│                                                                     │
│  ┌─────────────┐                                                    │
│  │ struct kvm  │←──── VM实例                                        │
│  └──────┬──────┘                                                    │
│         │                                                           │
│         ├──→ memslots[i] (RCU保护, 通过srcu_dereference访问)         │
│         │    └──→ struct kvm_memslots                               │
│         │         ├── hva_tree (RB_ROOT_CACHED) ← HVA排序           │
│         │         ├── gfn_tree (RB_ROOT) ← GFN排序                  │
│         │         ├── id_hash ← slot ID哈希                         │
│         │         └── generation (用于检测memslot变更)               │
│         │                                                           │
│         ├──→ vcpu_array (xarray)                                    │
│         │    └──→ struct kvm_vcpu                                   │
│         │         ├── kvm (所属VM)                                  │
│         │         ├── vcpu_id (vCPU编号)                            │
│         │         ├── cpu (当前pCPU)                                │
│         │         ├── run (kvm_run共享内存, 用户态可访问)            │
│         │         ├── pid (运行vCPU的线程pid)                       │
│         │         ├── wants_to_run (是否真正运行)                   │
│         │         ├── mode (IN_GUEST_MODE / OUTSIDE_GUEST_MODE)     │
│         │         ├── mutex (vCPU级互斥锁)                         │
│         │         ├── halt_poll_ns (自适应halt-polling窗口)         │
│         │         ├── arch (x86扩展):                               │
│         │         │    ├── cr0, cr3, cr4, efer                      │
│         │         │    ├── apic (虚拟LAPIC)                         │
│         │         │    ├── mmu (MMU上下文)                          │
│         │         │    ├── mp_state (运行状态)                      │
│         │         │    ├── exception (异常队列)                     │
│         │         │    ├── nmi_pending                              │
│         │         │    └── apf (异步缺页)                           │
│         │         └── wq / rcuwait (等待队列)                      │
│         │                                                           │
│         ├──→ buses[KVM_NR_BUSES] (PIO/MMIO总线, RCU保护)            │
│         │                                                           │
│         ├──→ irq_routing (中断路由表, irq_lock保护)                 │
│         │    └──→ struct kvm_irq_routing_table                      │
│         │                                                           │
│         ├──→ srcu / irq_srcu (SRCU结构)                             │
│         ├──→ lock (VM级通用mutex)                                   │
│         ├──→ slots_lock (memslot操作mutex)                          │
│         ├──→ mm (所属进程的内存空间)                                 │
│         ├──→ users_count (引用计数)                                 │
│         └──→ arch (x86架构扩展: struct kvm_arch)                    │
│              ├── tdp_mmu_enabled (EPT是否启用)                      │
│              ├── kvmclock相关字段                                   │
│              └── ...                                                │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ struct kvm_run (用户态和内核态共享, 通过mmap访问)             │   │
│  │                                                              │   │
│  │  ├── exit_reason (VM-Exit原因, 告诉用户态如何处理)           │   │
│  │  ├── flags                                                  │   │
│  │  ├── kvm_valid_regs (内核支持的寄存器集合)                   │   │
│  │  ├── kvm_dirty_regs (用户态修改了哪些寄存器)                 │   │
│  │  ├── immediate_exit__unsafe (设置后立即退出)                 │   │
│  │  ├── cr8 (TPR, 用户态LAPIC模式)                              │   │
│  │  ├── 联合体:                                                 │   │
│  │  │   ├── io (KVM_EXIT_IO)                                   │   │
│  │  │   ├── mmio (KVM_EXIT_MMIO)                               │   │
│  │  │   ├── shutdown (KVM_EXIT_SHUTDOWN)                       │   │
│  │  │   └── ...                                                │   │
│  │  └── sync_regs区域 (通用寄存器等)                            │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 13. VM-Exit处理分类

```
VM-Exit发生后的处理路径:

┌─ 快速路径 (内核态直接处理, 不返回用户空间) ──────────────────────┐
│                                                                   │
│  EXIT_REASON_EXTERNAL_INTERRUPT:                                  │
│    └→ handle_external_interrupt()                                 │
│    └→ 注入中断到Guest，重新进入                                   │
│                                                                   │
│  EXIT_REASON_HLT:                                                 │
│    └→ handle_halt()                                               │
│    └→ mp_state = KVM_MP_STATE_HALTED, 继续vcpu_run循环           │
│    └→ 下次循环 kvm_vcpu_running() 返回false → vcpu_block()        │
│                                                                   │
│  EXIT_REASON_EPT_VIOLATION:                                       │
│    └→ handle_ept_violation() → kvm_mmu_page_fault()              │
│    └→ 处理页错误，建立EPT映射，重新进入                           │
│                                                                   │
│  EXIT_REASON_PREEMPTION_TIMER:                                    │
│    └→ handle_preemption_timer()                                   │
│    └→ 更新抢占定时器，重新进入                                    │
│                                                                   │
│  EXIT_FASTPATH_REENTER_GUEST:                                     │
│    └→ posted interrupt处理完成，直接REENTER                       │
│    └→ 不经过handle_exit，在vcpu_enter_guest内循环中直接重入       │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘

┌─ 慢速路径 (返回用户空间, QEMU处理) ─────────────────────────────┐
│                                                                   │
│  EXIT_REASON_IO_INSTRUCTION:                                      │
│    └→ handle_io()                                                 │
│    └→ vcpu->run->exit_reason = KVM_EXIT_IO                        │
│    └→ return 0 (vcpu_enter_guest返回 → vcpu_run返回 →            │
│         kvm_arch_vcpu_ioctl_run返回 → 用户空间)                   │
│                                                                   │
│  EXIT_REASON_CPUID:                                               │
│    └→ handle_cpuid()                                              │
│    └→ vcpu->run->exit_reason = KVM_EXIT_INTERNAL_ERROR            │
│    └→ return 0 (返回用户空间)                                     │
│                                                                   │
│  EXIT_REASON_MSR_READ/WRITE:                                      │
│    └→ handle_rdmsr() / handle_wrmsr()                             │
│    └→ 如果未处理 → KVM_EXIT_INTERNAL_ERROR → return 0             │
│                                                                   │
│  EXIT_REASON_TRIPLE_FAULT:                                        │
│    └→ 在vcpu_enter_guest的request处理中 → KVM_EXIT_SHUTDOWN       │
│    └→ return 0 (返回用户空间)                                     │
│                                                                   │
│  EXIT_REASON_APIC_ACCESS:                                         │
│    └→ 某些情况需要返回用户空间处理                                │
│                                                                   │
│  EXIT_REASON_IOAPIC_EOI:                                          │
│    └→ vcpu->run->exit_reason = KVM_EXIT_IOAPIC_EOI               │
│    └→ return 0                                                    │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

---

## 14. 并发模型

```
KVM 并发模型:

┌─ vCPU线程 ─────────────────────────────────────────────────────┐
│                                                                 │
│  每个vCPU运行在独立的线程中                                      │
│  QEMU为每个vCPU创建一个线程                                      │
│  线程调用ioctl(KVM_RUN) → kvm_vcpu_ioctl() →                   │
│    kvm_arch_vcpu_ioctl_run() → vcpu_run() 循环                 │
│                                                                 │
│  并发点:                                                         │
│  ├── 多个vCPU同时处理EPT页错误                                  │
│  ├── 多个vCPU同时注入中断                                       │
│  ├── vCPU线程与QEMU线程并发访问kvm_run                          │
│  └── 其他线程通过vcpu ioctl操作vCPU状态                         │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘

┌─ 锁机制 ───────────────────────────────────────────────────────┐
│                                                                 │
│  vcpu->mutex (mutex):                                           │
│    └── 保护vCPU ioctl操作 (KVM_RUN, GET_REGS等)                │
│    └── 确保同一vCPU的操作串行化                                  │
│                                                                 │
│  kvm->lock (mutex):                                             │
│    └── VM级通用锁                                                │
│                                                                 │
│  kvm->mmu_lock (spinlock/rwlock):                               │
│    └── 保护EPT页表操作                                          │
│    └── 在EPT页错误处理时持有                                    │
│                                                                 │
│  kvm->slots_lock (mutex):                                       │
│    └── 保护memslots操作                                         │
│    └── 在KVM_SET_USER_MEMORY_REGION时持有                       │
│                                                                 │
│  kvm->irq_lock (mutex):                                         │
│    └── 保护中断路由表                                           │
│    └── 在KVM_SET_GSI_ROUTING时持有                              │
│                                                                 │
│  SRCU保护 (kvm->srcu):                                          │
│    └── memslots通过SRCU保护，读端无锁                           │
│    └── vcpu运行期间持有srcu读锁                                  │
│    └── 阻塞时释放srcu读锁 (kvm_vcpu_srcu_read_unlock)           │
│    └── 更新memslots时通过synchronize_srcu等待读端退出           │
│                                                                 │
│  vcpu->mode (atomic):                                           │
│    └── IN_GUEST_MODE / OUTSIDE_GUEST_MODE                      │
│    └── 用于posted interrupt投递判断                              │
│    └── 写时用smp_store_release，读时用smp_load_acquire          │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 15. 内存屏障和原子操作

```
关键内存屏障:

1. memslot访问:
   kvm_vcpu_srcu_read_lock(&vcpu)
   slots = srcu_dereference(kvm->memslots[i], &kvm->srcu)
   ...
   kvm_vcpu_srcu_read_unlock(&vcpu)

2. EPT页表更新:
   spin_lock(&kvm->mmu_lock)
   ... 更新EPT页表 ...
   spin_unlock(&kvm->mmu_lock)

3. 中断注入:
   atomic_set(&vcpu->arch.irq_pending, 1)
   kvm_vcpu_kick(vcpu)  ← 发送IPI唤醒目标vCPU

4. vCPU进入Guest前的模式切换:
   smp_store_release(&vcpu->mode, IN_GUEST_MODE)
   └── 确保mode设置在posted interrupt检查之前
   └── 与pi_test_and_set_on中的隐式屏障配对

5. vCPU退出Guest后的模式切换:
   vcpu->mode = OUTSIDE_GUEST_MODE
   smp_wmb()
   └── 确保mode更新在后续操作之前可见

6. SRCU unlock后的屏障:
   kvm_vcpu_srcu_read_unlock(vcpu)
   smp_mb__after_srcu_read_unlock()
   └── 确保IN_GUEST_MODE设置在请求检查之前可见
   └── 防止vCPU已进入Guest但请求未被处理

7. posted interrupt:
   pi_test_and_set_on(pi_desc)
   └── 隐式屏障，与smp_store_release(&vcpu->mode)配对
   └── 确保如果vCPU在guest模式，PIR中的中断会被投递
```

---

## 16. 完整调用链总结

```
用户空间 (QEMU)
  │
  ├── ioctl(/dev/kvm, KVM_CREATE_VM, 0)
  │   └→ kvm_dev_ioctl()                     [kvm_main.c:5535]
  │      └→ kvm_dev_ioctl_create_vm()         [kvm_main.c:5492]
  │         └→ kvm_create_vm()                [kvm_main.c:1146]
  │            └→ 返回VM文件描述符
  │
  ├── ioctl(vcpu_fd, KVM_RUN, 0)
  │   └→ kvm_vcpu_ioctl()                     [kvm_main.c:4445]
  │      └→ kvm_arch_vcpu_ioctl_run()         [x86.c:11579]
  │         ├── vcpu_load()
  │         ├── kvm_sigset_activate()
  │         ├── kvm_load_guest_fpu()
  │         ├── kvm_vcpu_srcu_read_lock()
  │         ├── sync_regs()
  │         ├── kvm_x86_call(vcpu_pre_run)()  → vmx_vcpu_pre_run()
  │         │
  │         ├── ★ vcpu_run()                   [x86.c:11343]
  │         │   └→ for (;;) {
  │         │      │
  │         │      ├── kvm_vcpu_running()?
  │         │      │   ├── 是 → vcpu_enter_guest()  [x86.c:10777]
  │         │      │   │   │
  │         │      │   │   ├── Part A: 处理KVM_REQ_*请求
  │         │      │   │   ├── Part B: 事件注入
  │         │      │   │   ├── Part C: MMU重载
  │         │      │   │   │
  │         │      │   │   ├── Part D: VMENTER
  │         │      │   │   │   ├── vcpu->mode = IN_GUEST_MODE
  │         │      │   │   │   ├── local_irq_disable()
  │         │      │   │   │   ├── kvm_x86_call(vcpu_run)()
  │         │      │   │   │   │   └→ vmx_vcpu_run()
  │         │      │   │   │   │       └→ VMRESUME → Guest → VM-Exit
  │         │      │   │   │   │
  │         │      │   │   │   ├── vcpu->mode = OUTSIDE_GUEST_MODE
  │         │      │   │   │   ├── handle_exit_irqoff()
  │         │      │   │   │   ├── local_irq_enable()
  │         │      │   │   │   └── kvm_x86_call(handle_exit)()
  │         │      │   │   │       └→ vmx_handle_exit()
  │         │      │   │   │           └→ 根据exit_reason分发处理
  │         │      │   │   │
  │         │      │   │   └→ return r
  │         │      │   │
  │         │      │   └── 否 → vcpu_block()      [x86.c:11272]
  │         │      │       └→ kvm_arch_vcpu_runnable()?
  │         │      │           ├── 是 → return 1
  │         │      │           └── 否 → kvm_vcpu_halt()  [kvm_main.c:3811]
  │         │      │               ├── halt-polling (忙等)
  │         │      │               └── kvm_vcpu_block() (阻塞)
  │         │      │
  │         │      ├── 注入定时器中断
  │         │      ├── 检查中断窗口
  │         │      └── 继续循环
  │         │    }
  │         │
  │         ├── store_regs()
  │         ├── post_kvm_run_save()
  │         ├── kvm_vcpu_srcu_read_unlock()
  │         ├── kvm_sigset_deactivate()
  │         └── vcpu_put()
  │
  └── ioctl(vcpu_fd, KVM_GET_REGS, &regs)
      └→ kvm_vcpu_ioctl()
         └→ kvm_arch_vcpu_ioctl_get_regs()
```
