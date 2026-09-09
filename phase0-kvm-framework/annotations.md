# Phase 0：源码精读注释 - KVM 框架层

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。

---

## 1. KVM 设备 ioctl 入口

### Q: `/dev/kvm` 的 ioctl 支持哪些操作？其他操作去哪？

**文件**: `virt/kvm/kvm_main.c` — `kvm_dev_ioctl()`

```c
switch (ioctl) {
case KVM_GET_API_VERSION:
    r = KVM_API_VERSION;     /* 返回 12 */
    break;
case KVM_CREATE_VM:
    r = kvm_dev_ioctl_create_vm(arg);
    break;
case KVM_CHECK_EXTENSION:
    r = kvm_vm_ioctl_check_extension_generic(NULL, arg);
    break;
case KVM_GET_VCPU_MMAP_SIZE:
    r = PAGE_SIZE;            /* struct kvm_run */
#ifdef CONFIG_X86
    r += PAGE_SIZE;           /* pio data page */
#endif
#ifdef CONFIG_KVM_MMIO
    r += PAGE_SIZE;           /* coalesced mmio ring page */
#endif
    break;                    /* x86 上 = 3 * PAGE_SIZE */
default:
    return kvm_arch_dev_ioctl(filp, ioctl, arg);  /* 架构扩展 */
}
```

**`KVM_GET_VCPU_MMAP_SIZE` 返回的 3 页分别是什么？**

| 页 | 内容 | 用途 |
|----|------|------|
| 第 1 页 | `struct kvm_run` | 用户态/内核态共享的 vCPU 状态通信区 |
| 第 2 页 | PIO data buffer | x86 IO 端口访问的数据暂存 |
| 第 3 页 | coalesced MMIO ring | MMIO 合并的环形缓冲区 |

---

## 2. 创建 VM 实例

### Q: `kvm_create_vm()` 做了什么关键初始化？

**文件**: `virt/kvm/kvm_main.c` — `kvm_create_vm()`

```c
struct kvm *kvm = kvm_arch_alloc_vm();

kvm->mm = current->mm;                  /* ★ 继承调用者(QEMU)的内存空间 */

init_srcu_struct(&kvm->srcu);           /* SRCU: memslots 无锁读 */
init_srcu_struct(&kvm->irq_srcu);       /* SRCU: irq routing 无锁读 */

for (i = 0; i < kvm_arch_nr_memslot_as_ids(kvm); i++) {
    for (j = 0; j < 2; j++) {
        slots = &kvm->__memslots[i][j];  /* ★ 双缓冲 */
        slots->hva_tree = RB_ROOT_CACHED;
        slots->gfn_tree = RB_ROOT;
    }
    rcu_assign_pointer(kvm->memslots[i], &kvm->__memslots[i][0]);
}

r = kvm_enable_virtualization();        /* 启用 VMX/SVM */
r = kvm_init_mmu_notifier(kvm);        /* 注册 mmu_notifier */
```

### Q: 为什么 `kvm->mm = current->mm`？

KVM 内核模块需要访问 Guest 的物理内存。Guest 内存是 QEMU 用户态进程通过 mmap 分配的。
KVM 继承 QEMU 的 `mm_struct`，这样 KVM 在操作 EPT/嵌套页表时可以通过 `mm` 找到 Guest 内存
的宿主虚拟地址（HVA），再通过 `get_user_pages()` 拿到物理页。

### Q: memslots 为什么用双缓冲？

更新 memslots 时：
1. 拷贝当前 `__memslots[i][0]` → `__memslots[i][1]`
2. 在副本上做修改
3. `rcu_assign_pointer()` 原子切换指针
4. 等待所有 SRCU 读端退出后，旧副本变安全

这保证了 vCPU 运行期间可以无锁读 memslots（`srcu_dereference`），更新不需要加读端锁。

### Q: 创建 VM 的 fd 生命周期有什么陷阱？

**文件**: `virt/kvm/kvm_main.c` — `kvm_dev_ioctl_create_vm()`

```c
fd = get_unused_fd_flags(O_CLOEXEC);
kvm = kvm_create_vm(type, fdname);
file = anon_inode_getfile("kvm-vm", &kvm_vm_fops, kvm, O_RDWR);
fd_install(fd, file);
```

**一旦 `anon_inode_getfile()` 成功，就不能再 `kvm_put_kvm()` 了** — 因为
`file->f_op->release`（`kvm_vm_release()`）会在文件 fput 时负责清理。过早 put 会导致
double-free。

---

## 3. vCPU ioctl 入口

### Q: 为什么 vCPU ioctl 需要 mutex，但有例外？

**文件**: `virt/kvm/kvm_main.c` — `kvm_vcpu_ioctl()`

```c
/* 先尝试异步 ioctl（不需要 vcpu->mutex） */
r = kvm_arch_vcpu_async_ioctl(filp, ioctl, arg);
if (r != -ENOIOCTLCMD)
    return r;

/* ★ 获取 vCPU 互斥锁 */
if (mutex_lock_killable(&vcpu->mutex))
    return -EINTR;

switch (ioctl) {
case KVM_RUN: ...
case KVM_GET_REGS: ...
...
}
```

**异步 ioctl 是什么？** 不需要持有 vcpu->mutex 的操作，典型的是 `KVM_SET_SIGNAL_MASK`。
这些操作可以在 vCPU 运行时并发执行（比如 QEMU 的一个线程在 `KVM_RUN` 里跑着，另一个线程
改信号掩码）。

### Q: `KVM_RUN` 里为什么要检查 `vcpu->kvm->mm != current->mm`？

```c
if (vcpu->kvm->mm != current->mm || vcpu->kvm->vm_dead)
    return -EIO;
```

防止一个进程创建的 vCPU fd 被另一个进程拿来 `KVM_RUN`。因为 KVM 依赖 `current->mm` 来访问
Guest 内存，如果 mm 不一致，地址翻译会出错。

### Q: `vcpu->wants_to_run` 和 `immediate_exit` 什么关系？

```c
vcpu->wants_to_run = !READ_ONCE(vcpu->run->immediate_exit__unsafe);
r = kvm_arch_vcpu_ioctl_run(vcpu);
vcpu->wants_to_run = false;
```

用户态设 `kvm_run->immediate_exit__unsafe = 1` 后，`KVM_RUN` 在 setup 阶段（`vcpu_pre_run`
之前）检测到 `!wants_to_run` 就直接返回 `-EINTR`，不进入 `vcpu_run()` 循环。
QEMU 在信号处理或 VM 停止时用这个机制让 vCPU 线程尽快退出 `KVM_RUN`。

---

## 4. x86 vCPU 运行入口

### Q: `kvm_arch_vcpu_ioctl_run()` 在进 Guest 前做了哪些状态切换？

**文件**: `arch/x86/kvm/x86.c` — `kvm_arch_vcpu_ioctl_run()`

```c
vcpu_load(vcpu);                        /* 绑定 vCPU 到当前 pCPU */
kvm_load_guest_fpu(vcpu);               /* 用户态 FPU → Guest FPU */
kvm_vcpu_srcu_read_lock(vcpu);          /* SRCU 读锁保护 memslots */

/* ... 各种检查和同步 ... */

r = vcpu_run(vcpu);                     /* ★ 核心运行循环 */

/* 清理 */
kvm_put_guest_fpu(vcpu);                /* Guest FPU → 保存 */
store_regs(vcpu);                       /* vCPU 寄存器 → kvm_run */
kvm_vcpu_srcu_read_unlock(vcpu);
vcpu_put(vcpu);                         /* 解除 pCPU 绑定 */
```

### Q: 为什么 SRCU 读锁要保护整个运行期间？

vCPU 在 Guest 模式下执行时，通过 EPT 访问的 GPA→HVA 映射来自 memslots。如果运行期间
memslots 被更新（比如 QEMU 热插内存），没有 SRCU 保护的话，KVM 可能读到一半新一半旧的
memslots，导致地址翻译错误。

**阻塞时为什么要释放 SRCU 锁？** `kvm_vcpu_srcu_read_unlock()` 在 `vcpu_block()` 前调用。
因为 SRCU 读侧持有会阻止写端的 `synchronize_srcu()` 完成，导致 memslots 更新延迟。

---

## 5. vCPU 运行主循环

### Q: `vcpu_run()` 每次 VM-Exit 后做什么？

**文件**: `arch/x86/kvm/x86.c` — `vcpu_run()`

```c
for (;;) {
    if (kvm_vcpu_running(vcpu)) {
        r = vcpu_enter_guest(vcpu);     /* 进 Guest → VM-Exit → 处理 */
    } else {
        r = vcpu_block(vcpu);           /* halt/block */
    }
    if (r <= 0)
        break;

    /* ★ VM-Exit 后、重新进入前的固定清理 */
    kvm_clear_request(KVM_REQ_UNBLOCK, vcpu);
    if (kvm_cpu_has_pending_timer(vcpu))
        kvm_inject_pending_timer_irqs(vcpu);   /* 注入定时器中断 */
    if (dm_request_for_irq_injection(vcpu) &&
        kvm_vcpu_ready_for_interrupt_injection(vcpu)) {
        vcpu->run->exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
        break;                                  /* 返回用户空间 */
    }
}
```

### Q: `r` 的三种返回值含义？

| 返回值 | 含义 | 动作 |
|--------|------|------|
| `r > 0` | 继续循环 | 重新进入 Guest |
| `r == 0` | 返回用户空间 | `KVM_RUN` ioctl 返回，QEMU 处理 exit |
| `r < 0` | 错误 | `KVM_RUN` ioctl 返回错误码 |

### Q: 中断窗口退出（`KVM_EXIT_IRQ_WINDOW_OPEN`）是什么？

当 QEMU 使用用户态 LAPIC 模式（`!lapic_in_kernel(vcpu)`），需要注入外部中断但当前 Guest
状态不允许（比如中断被屏蔽），KVM 会打开中断窗口、请求一个"中断窗口打开"的 VM-Exit。
Guest 继续执行到中断可注入的那一刻，硬件触发 VM-Exit，KVM 返回用户空间让 QEMU 注入中断。

---

## 6. vCPU 可运行性判断

### Q: 什么条件让 vCPU 从 halt 状态唤醒？

**文件**: `arch/x86/kvm/x86.c` — `kvm_arch_vcpu_runnable()`

```c
int kvm_arch_vcpu_runnable(struct kvm_vcpu *vcpu)
{
    return kvm_vcpu_running(vcpu) || kvm_vcpu_has_events(vcpu);
}
```

`kvm_vcpu_has_events()` 按优先级检查：

```
async_pf.done 非空     → 异步缺页完成
INIT/SIPI 待处理       → 多核启动信号
pv_unhalted            → PV unhalt（Guest 主动请求唤醒）
待处理异常              → exception pending
NMI / SMI / PMI        → 不可屏蔽中断
外部中断可注入 + 有中断  → 普通硬件中断
Hyper-V 合成定时器      → 合成事件
嵌套虚拟化事件          → L1 VMM 的事件
Xen PV 事件            → Xen 合成事件
```

### Q: 为什么 posted interrupt 没出现在这个列表里？

PI 不通过 `kvm_vcpu_has_events()` 唤醒 vCPU。当 vCPU 在 Guest 模式时，PI 由硬件直接投递
到 IRR，不需要软件干预。当 vCPU 不在 Guest 模式时，PI notification 中断触发宿主的 IRQ 处理，
其中会 `kvm_vcpu_kick()` 唤醒 vCPU 线程 — 这走的是普通的中断路径，不需要在 `has_events`
里单独检查。

---

## 7. halt-polling

### Q: halt-polling 为什么要分三阶段？

**文件**: `virt/kvm/kvm_main.c` — `kvm_vcpu_halt()`

```c
/* Phase 1: 忙等（不释放 CPU） */
if (do_halt_poll) {
    do {
        if (kvm_vcpu_check_block(vcpu) < 0)
            goto out;           /* 有事件 → 立即恢复 */
        cpu_relax();
    } while (kvm_vcpu_can_poll(cur, stop));
}

/* Phase 2: 真正阻塞（释放 CPU） */
waited = kvm_vcpu_block(vcpu);

/* Phase 3: 自适应调整窗口 */
if (halt_ns <= vcpu->halt_poll_ns)
    ;                           /* 在窗口内唤醒 → poll 有效，保持 */
else if (halt_ns > max_halt_poll_ns)
    shrink_halt_poll_ns(vcpu);  /* 远超窗口 → poll 浪费，缩小 */
else
    grow_halt_poll_ns(vcpu);    /* 稍超窗口 → 窗口太小，增大 */
```

**为什么不只用 Phase 2？** halt-polling 的核心权衡：

- **忙等**：浪费 CPU 但中断延迟低（~μs 级）
- **阻塞**：节省 CPU 但中断延迟高（需调度器唤醒，~ms 级）

对于短时 halt（Guest 执行 HLT 后很快被中断唤醒），忙等比阻塞更快恢复。自适应算法让窗口
跟踪实际的唤醒间隔。

### Q: 进入 halt 前为什么要切换定时器？

**文件**: `arch/x86/kvm/x86.c` — `vcpu_block()`

```c
hv_timer = kvm_lapic_hv_timer_in_use(vcpu);
if (hv_timer)
    kvm_lapic_switch_to_sw_timer(vcpu);    /* hypervisor timer → software timer */

kvm_vcpu_srcu_read_unlock(vcpu);
kvm_vcpu_halt(vcpu);                        /* 可能阻塞 */
kvm_vcpu_srcu_read_lock(vcpu);

if (hv_timer)
    kvm_lapic_switch_to_hv_timer(vcpu);    /* 恢复 */
```

hypervisor timer（VMX preemption timer）只在 Guest 模式下递减。vCPU halt 后不在 Guest 模式，
hypervisor timer 停了，无法到期唤醒。必须切换到软件定时器（hrtimer），它在宿主内核中运行，
不受 Guest 模式影响。

---

## 8. vcpu_enter_guest(): 进入 Guest

### Q: 进入 Guest 前必须完成哪些状态切换？

**文件**: `arch/x86/kvm/x86.c` — `vcpu_enter_guest()`

```c
preempt_disable();
local_irq_disable();
smp_store_release(&vcpu->mode, IN_GUEST_MODE);   /* ★ 为什么用 release? */
kvm_vcpu_srcu_read_unlock(vcpu);
smp_mb__after_srcu_read_unlock();

if (kvm_lapic_enabled(vcpu))
    kvm_x86_call(sync_pir_to_irr)(vcpu);          /* posted interrupt 同步 */

/* ★ 核心 VMENTER */
exit_fastpath = kvm_x86_call(vcpu_run)(vcpu, run_flags);
```

### Q: 为什么 `mode` 用 `smp_store_release`？

与 posted interrupt 的 `pi_test_and_set_on()` 中的隐式屏障配对。执行顺序必须是：

```
vCPU 线程:                          中断来源线程:
smp_store_release(mode, IN_GUEST)    atomic_set(ON, 1) in PI desc
                                     if (mode == IN_GUEST) → 直接写 PIR
```

如果 `mode` 赋值不用 release 语义，CPU 可能把 `mode` 的写入重排到后面的操作之后。
中断来源线程看到 `mode == IN_GUEST_MODE` 后直接写 PIR 并认为 vCPU 会处理，但 vCPU 实际
还没完成进入准备 → 中断丢失。

### Q: `exit_fastpath` 有几种？

| 类型 | 含义 | 典型场景 |
|------|------|---------|
| `EXIT_FASTPATH_NONE` | 正常 VM-Exit，走完整 `handle_exit` | 大多数退出 |
| `EXIT_FASTPATH_REENTER_GUEST` | 直接重入，跳过 `handle_exit` | posted interrupt 处理完 |
| `EXIT_FASTPATH_EXIT_HANDLED` | exit 已处理，直接返回 | 快速路径自己处理完 |
| `EXIT_FASTPATH_EXIT_USERSPACE` | 需返回用户空间 | MMIO/IO 需 QEMU 处理 |

### Q: `cancel_injection` 是什么？

```c
cancel_injection:
    if (req_immediate_exit)
        kvm_make_request(KVM_REQ_EVENT, vcpu);
    kvm_x86_call(cancel_injection)(vcpu);
```

VMENTER 前发现需要立即退出（`req_immediate_exit` 被 `kvm_check_and_inject_events` 置位，
通常是因为注入过程中检测到 pending request）。此时中断已部分加载到注入状态机。
`cancel_injection` 让架构层清理注入状态，防止半注入的中断在下一次 VMENTER 时意外触发。
只有 `req_immediate_exit` 为真时才重新标记 `KVM_REQ_EVENT`，确保下次循环重新尝试注入。

---

## 9. 底层阻塞原语

### Q: `kvm_vcpu_block()` 为什么用 `rcuwait` 而不用普通 waitqueue？

**文件**: `virt/kvm/kvm_main.c` — `kvm_vcpu_block()`

```c
struct rcuwait *wait = kvm_arch_vcpu_get_wait(vcpu);

prepare_to_rcuwait(wait);
for (;;) {
    set_current_state(TASK_INTERRUPTIBLE);
    if (kvm_vcpu_check_block(vcpu) < 0)
        break;
    schedule();                       /* 让出 CPU */
}
finish_rcuwait(wait);
```

`rcuwait` 是专为 "一个 waiter + 一个 waker" 场景设计的轻量同步原语。相比普通 waitqueue：

- 无锁：不需要 spinlock 保护队列操作
- 单 waiter：vCPU 阻塞只有一个线程，不需要多 waiter 的复杂机制
- 唤醒简单：`rcuwait_wake_up()` 直接 `wake_up_process()`，不遍历链表

---

## 10. VM-Exit 处理分类

### Q: 哪些 VM-Exit 在内核态处理，哪些返回用户空间？

```
快速路径（内核处理，不返回用户空间）:
  EPT_VIOLATION        → kvm_mmu_page_fault()     → 建页表，重入
  EXTERNAL_INTERRUPT   → handle_external_interrupt → 处理宿主中断，重入
  HLT                  → handle_halt()             → mp_state=HALTED，循环
  PREEMPTION_TIMER     → handle_preemption_timer   → 更新定时器，重入
  APIC_WRITE           → handle_apic_write()       → 同步虚拟 LAPIC，重入

慢速路径（返回用户空间，QEMU 处理）:
  IO_INSTRUCTION       → KVM_EXIT_IO               → QEMU 模拟设备 IO
  MSR_READ/WRITE       → KVM_EXIT_INTERNAL_ERROR   → QEMU 处理未识别 MSR
  CPUID                → KVM_EXIT_INTERNAL_ERROR   → QEMU 处理 CPUID
  TRIPLE_FAULT         → KVM_EXIT_SHUTDOWN          → QEMU 决定重启/关机
  MMIO                 → KVM_EXIT_MMIO              → QEMU 模拟 MMIO 设备
```

### Q: `handle_exit_irqoff()` 和普通 `handle_exit()` 什么区别？

`handle_exit_irqoff()` 在 `local_irq_enable()` **之前**调用，运行在中断关闭状态。
用于处理对时序敏感的 exit（如外部中断需要尽快响应）。`handle_exit()` 在 `local_irq_enable()`
**之后**调用，可以安全地获取锁、分配内存。

---

## 11. 并发与锁

### Q: KVM 里有几把锁？各自保护什么？

| 锁 | 类型 | 保护对象 | 典型持有场景 |
|----|------|---------|-------------|
| `vcpu->mutex` | mutex | vCPU ioctl 串行化 | KVM_RUN、GET_REGS |
| `kvm->lock` | mutex | VM 级通用 | 设备分配、capability |
| `kvm->mmu_lock` | spin/rwlock | EPT 页表 | 页错误处理 |
| `kvm->slots_lock` | mutex | memslots 更新 | SET_USER_MEMORY_REGION |
| `kvm->irq_lock` | mutex | 中断路由表 | SET_GSI_ROUTING |
| `kvm->srcu` | SRCU | memslots 读侧 | vCPU 运行期间 |
| `vcpu->mode` | atomic | PI 投递判断 | VMENTER/VMEXIT |

### Q: 关键内存屏障有哪些？

**进入 Guest 前的 mode 切换**：
```c
smp_store_release(&vcpu->mode, IN_GUEST_MODE);
/* 与 PI 的 atomic ops 配对，确保 mode 在 PI 检查前可见 */
```

**退出 Guest 后的 mode 切换**：
```c
vcpu->mode = OUTSIDE_GUEST_MODE;
smp_wmb();
/* 确保 mode 更新在后续的 FPU/中断处理前可见 */
```

**SRCU unlock 后的屏障**：
```c
kvm_vcpu_srcu_read_unlock(vcpu);
smp_mb__after_srcu_read_unlock();
/* 确保 IN_GUEST_MODE 设置在请求检查之前可见 */
/* 防止 vCPU 已进入 Guest 但请求未被处理 */
```

---

## 12. 完整调用链

```
用户空间 (QEMU)
  │
  ├─ ioctl(/dev/kvm, KVM_CREATE_VM)
  │   └→ kvm_dev_ioctl() → kvm_dev_ioctl_create_vm()
  │      └→ kvm_create_vm()
  │          ├→ kvm->mm = current->mm        ← 继承 QEMU 内存空间
  │          ├→ init SRCU, memslots, buses
  │          ├→ kvm_enable_virtualization()   ← 启用 VMX/SVM
  │          └→ 返回 VM fd
  │
  ├─ ioctl(vcpu_fd, KVM_CREATE_VCPU)
  │   └→ kvm_vm_ioctl_create_vcpu()
  │      └→ 返回 vCPU fd (mmap kvm_run)
  │
  └─ ioctl(vcpu_fd, KVM_RUN)
      └→ kvm_vcpu_ioctl()                     ← vcpu->mutex 保护
         └→ kvm_arch_vcpu_ioctl_run()
             ├→ vcpu_load()                    ← 绑定 pCPU
             ├→ kvm_load_guest_fpu()           ← FPU 切换
             ├→ kvm_vcpu_srcu_read_lock()      ← SRCU 读锁
             ├→ sync_regs()                    ← kvm_run → vCPU 寄存器
             │
             ├→ vcpu_run()                     ← 主循环
             │   └→ for (;;) {
             │       ├→ kvm_vcpu_running()?
             │       │   ├→ 是: vcpu_enter_guest()
             │       │   │   ├→ 处理 KVM_REQ_*
             │       │   │   ├→ 事件注入
             │       │   │   ├→ mode = IN_GUEST_MODE
             │       │   │   ├→ vmx_vcpu_run()  ← VMRESUME
             │       │   │   │   └→ Guest 执行 → VM-Exit
             │       │   │   ├→ mode = OUTSIDE_GUEST_MODE
             │       │   │   └→ handle_exit()   ← 分发处理
             │       │   └→ 否: vcpu_block()
             │       │       └→ kvm_vcpu_halt()  ← halt-poll → block
             │       │
             │       ├→ 注入定时器中断
             │       ├→ 检查中断窗口
             │       └→ 继续循环
             │   }
             │
             └→ 清理: store_regs, FPU, SRCU unlock, vcpu_put
```
