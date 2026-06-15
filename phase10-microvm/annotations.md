# Phase 10：源码精读注释 - MicroVM 架构专项

> 基于 Linux 6.12.93 源码（实际代码行号已验证）

---

## 1. MicroVM vs 传统 VM 架构对比

### 1.1 VMM 架构对比表

```
┌────────────────┬──────────────────┬──────────────────┬──────────────────┐
│                │ QEMU             │ Firecracker      │ Cloud Hypervisor │
├────────────────┼──────────────────┼──────────────────┼──────────────────┤
│ 语言           │ C                │ Rust             │ Rust             │
│ 设备模型       │ 全功能 (PCI,USB) │ 最小 (MMIO only) │ 模块化 (PCI/MMIO)│
│ 目标工作负载   │ 通用             │ 微服务/容器      │ 云原生           │
│ 启动时间       │ ~1-3s            │ ~125ms           │ ~200ms           │
│ 内存占用       │ ~30-50MB         │ ~3-5MB           │ ~10-15MB         │
│ VM-Exit/s      │ ~100K (低)       │ ~500K (高)       │ ~200K            │
│ TCG 支持       │ ✓ (软件模拟)     │ ✗ (纯 KVM)       │ ✗ (纯 KVM)       │
│ 热迁移         │ ✓                │ 有限             │ ✓                │
│ 安全模型       │ 弱 (seccomp 可选)│ 强 (jailer)      │ 强 (jailer)      │
│ KVM API 使用   │ 完整 (所有ioctl) │ 最小 (仅必需)    │ 中等             │
└────────────────┴──────────────────┴──────────────────┴──────────────────┘
```

### 1.2 KVM ioctl 使用对比

```
QEMU 使用的 ioctl (完整):
  KVM_CREATE_VM
  KVM_CREATE_VCPU
  KVM_SET_USER_MEMORY_REGION
  KVM_CREATE_IRQCHIP          ← Firecracker 不用
  KVM_CREATE_PIT2             ← Firecracker 不用
  KVM_IRQ_LINE
  KVM_GET/SET_REGS/SREGS/MSRS
  KVM_RUN
  KVM_GET_VCPU_EVENTS         ← 调试用
  KVM_SET_GUEST_DEBUG         ← 调试用
  KVM_IOEVENTFD
  KVM_IRQFD
  ... (30+ ioctl)

Firecracker 使用的 ioctl (最小):
  KVM_CREATE_VM
  KVM_CREATE_VCPU
  KVM_SET_USER_MEMORY_REGION
  KVM_RUN
  KVM_GET/SET_REGS/SREGS/MSRS
  KVM_GET_SUPPORTED_CPUID
  KVM_SET_CPUID2
  ... (约 10 ioctl)

Firecracker 故意避免:
  - KVM_CREATE_IRQCHIP → 自己实现中断控制器
  - KVM_CREATE_PIT2 → 不模拟 PIT
  - KVM_GET_VCPU_EVENTS → 不暴露内部状态
  - 大部分调试相关的 ioctl
```

### 1.3 VM-Exit 特征差异

```
QEMU 典型 VM-Exit 分布 (通用 VM):
  EXTERNAL_INTERRUPT    45%
  EPT_VIOLATION         25%
  CPUID                 10%
  IO_INSTRUCTION        5%   (virtio PIO)
  HLT                   5%
  MSR_WRITE             3%
  其他                  7%
  总计                  ~100K/s

Firecracker VM-Exit 分布 (微服务):
  EXTERNAL_INTERRUPT    55%  (virtio-MMIO 中断)
  EPT_VIOLATION         20%
  CPUID                 8%
  IO_INSTRUCTION        8%   (串口、最小 PIO)
  HLT                   3%
  MSR_WRITE             2%
  其他                  4%
  总计                  ~500K/s (更频繁)

MicroVM VM-Exit 更高的原因:
  - 更小的 VM → 更高的中断密度
  - virtio-MMIO 比 virtio-PCI 更高效但更多 exit
  - 没有 in-kernel IRQ chip → 更多用户态处理
```

---

## 2. KVM VM 创建启动路径

### 2.1 完整启动流程

```
用户空间 (Firecracker/QEMU)          内核空间
───────────────────────────          ──────────

1. open("/dev/kvm")
   └→ 获得 KVM fd

2. ioctl(kvm_fd, KVM_CREATE_VM, 0) ─→ kvm_dev_ioctl() [kvm_main.c:5535]
                                      └→ kvm_dev_ioctl_create_vm() [kvm_main.c:5492]
                                         └→ kvm_create_vm() [kvm_main.c:1146]
                                            ├→ 分配 struct kvm
                                            ├→ 初始化锁、memslots、SRCU
                                            ├→ kvm_arch_init_vm()
                                            ├→ kvm_enable_virtualization()
                                            └→ 返回 VM fd ←──────────┐
                                                                      │
3. ioctl(vm_fd, KVM_CREATE_VCPU, 0) ─→ kvm_vm_ioctl() [kvm_main.c:5167]
                                      └→ kvm_vm_ioctl_create_vcpu() [kvm_main.c:4217]
                                         ├→ 分配 struct kvm_vcpu
                                         ├→ kvm_arch_vcpu_create()
                                         ├→ kvm_arch_vcpu_setup()
                                         └→ 返回 vCPU fd ←──────────┐
                                                                      │
4. ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region)
                                      └→ kvm_vm_ioctl_set_memory_region() [kvm_main.c:2124]
                                         ├→ 验证参数
                                         ├→ 更新 memslots
                                         └→ 建立 GPA→HVA 映射 ←─────┐
                                                                      │
5. ioctl(vcpu_fd, KVM_SET_REGS, &regs)  ← 设置初始寄存器
   ioctl(vcpu_fd, KVM_SET_SREGS, &sregs)
   ioctl(vcpu_fd, KVM_SET_CPUID2, &cpuid)
                                                                      │
6. mmap(vcpu_fd, 0, NULL)              ← 获取 kvm_run 共享页
                                                                      │
7. ioctl(vcpu_fd, KVM_RUN, 0) ────────→ kvm_vcpu_ioctl() [kvm_main.c:4445]
                                      └→ kvm_arch_vcpu_ioctl_run() [x86.c:11579]
                                         ├→ vcpu_load()
                                         ├→ kvm_load_guest_fpu()
                                         ├→ kvm_vcpu_srcu_read_lock()
                                         ├→ sync_regs()
                                         ├→ kvm_x86_call(vcpu_pre_run)()
                                         │
                                         └→ ★ vcpu_run() [x86.c:11343]
                                            └→ vcpu_enter_guest() [x86.c:10777]
                                               └→ kvm_x86_call(vcpu_run)()
                                                  └→ vmx_vcpu_run()
                                                     └→ VMRESUME → Guest!

★ 首次 VM-Entry 成功!
```

### 2.2 启动时间分析

```
典型启动时间分解 (Firecracker):
  KVM_CREATE_VM:                    ~0.5ms
  KVM_CREATE_VCPU:                  ~0.3ms
  KVM_SET_USER_MEMORY_REGION:       ~0.2ms (4GB 内存)
  KVM_SET_*REGS:                    ~0.1ms
  加载 kernel + initrd:             ~50ms (从文件读)
  首次 KVM_RUN:                     ~5ms (首次 EPT 建立)
  Guest 启动:                       ~50-100ms (Linux 内核启动)
  ────────────────────────────────
  总启动时间:                       ~100-150ms

优化方向 (MicroVM):
  - KVM_CREATE_VM: 已接近极限
  - 内存设置: 使用 hugepages 减少 EPT 建立时间
  - Guest 启动: 使用精简内核 + initramfs
```

### 2.3 MicroVM 启动优化

```bash
# 1. 使用大页内存 (减少 EPT 建立时间)
qemu -mem-prealloc -mem-path /dev/hugepages

# 或 Firecracker:
# 在配置中使用 huge_pages: true

# 2. 预分配内存 (避免启动时 page fault)
# QEMU: -mem-prealloc
# Firecracker: 默认预分配

# 3. 精简 Guest 内核
# 编译最小内核:
make tinyconfig
# 或:
make defconfig
# 然后禁用不需要的选项

# 4. 使用 initramfs 代替磁盘
# Firecracker: 默认使用 initramfs
# QEMU: -initrd /path/to/initramfs

# 5. 调整 halt-polling 减少首次运行延迟
echo 0 > /sys/module/kvm/parameters/halt_poll_ns
```

---

## 3. 最小设备模型

### 3.1 MMIO 设备 vs PIO 设备

```
PIO (Port I/O) 设备:
  - 独立地址空间 (x86 IO 端口)
  - 每次访问触发 IO_INSTRUCTION VM-Exit
  - KVM 处理: 返回用户空间 (KVM_EXIT_IO)
  - 延迟: ~1μs 每次访问

MMIO (Memory Mapped I/O) 设备:
  - 映射到内存地址空间
  - 每次访问触发 EPT_VIOLATION VM-Exit
  - KVM 处理: 返回用户空间 (KVM_EXIT_MMIO)
  - 延迟: ~1-2μs 每次访问

MicroVM 倾向 MMIO 的原因:
  - virtio-MMIO 比 virtio-PCI 更高效
  - 不需要模拟 PCI 总线
  - 设备描述更简单 (固定地址)
  - 更少的 KVM 交互
```

### 3.2 virtio-MMIO vs virtio-PCI

```
virtio-PCI (QEMU 默认):
  设备发现: PCI 总线扫描
  配置空间: PCI 配置寄存器
  中断: MSI/MSI-X (通过 KVM_IRQFD)
  性能: 高 (MSI 零 VM-Exit 投递)
  代码: ~30K 行 (QEMU 侧)

virtio-MMIO (Firecracker 默认):
  设备发现: 固定地址 (0xd0000000 开始)
  配置空间: MMIO 寄存器
  中断: 通过 eventfd (无 MSI)
  性能: 中 (中断需要用户态处理)
  代码: ~5K 行 (Firecracker 侧)

MicroVM 选择 virtio-MMIO 的权衡:
  优点:
    - 代码量少 (攻击面小)
    - 不需要 PCI 总线模拟
    - 设备描述简单
  缺点:
    - 中断路径更长 (eventfd → 用户态 → KVM)
    - 不支持 MSI (无 Posted Interrupts 优化)
```

### 3.3 实现最小 MMIO 设备

```c
/*
 * 最小 MMIO 设备实现示例
 *
 * 设备行为:
 *   - 读偏移 0x00: 返回 magic number
 *   - 写偏移 0x00: 触发 VM-Exit, 数据返回用户空间
 */

/* 用户空间设备模拟 */
struct my_mmio_device {
    int kvm_fd;
    int vcpu_fd;
    struct kvm_run *run;
    void *mmio_base;      /* GPA: 0x10000 */
    size_t mmio_size;     /* 4KB */
};

/* 处理 MMIO VM-Exit */
int handle_mmio_exit(struct my_mmio_device *dev)
{
    struct kvm_run *run = dev->run;

    if (run->exit_reason != KVM_EXIT_MMIO)
        return -1;

    if (run->mmio.phys_addr == 0x10000) {  /* 设备基地址 */
        if (run->mmio.is_write) {
            /* 写操作: 记录数据 */
            uint32_t val;
            memcpy(&val, run->mmio.data, run->mmio.len);
            printf("MMIO write: 0x%x\n", val);
        } else {
            /* 读操作: 返回 magic */
            uint32_t magic = 0xDEADBEEF;
            memcpy(run->mmio.data, &magic, sizeof(magic));
        }
    }

    return 0;
}

/* 主循环 */
void run_device(struct my_mmio_device *dev)
{
    while (1) {
        ioctl(dev->vcpu_fd, KVM_RUN, 0);

        switch (dev->run->exit_reason) {
        case KVM_EXIT_MMIO:
            handle_mmio_exit(dev);
            break;
        case KVM_EXIT_IO:
            /* 处理 PIO */
            break;
        case KVM_EXIT_HLT:
            return;  /* Guest halted */
        case KVM_EXIT_SHUTDOWN:
            return;  /* Triple fault */
        }
    }
}
```

---

## 4. 安全性考虑

### 4.1 KVM 安全模型

```
KVM 的安全边界:
  ┌────────────────────────────────────────────────┐
  │  用户空间 (QEMU/Firecracker)                   │
  │  ┌──────────────────────────────────────┐      │
  │  │  ioctl 接口                          │      │
  │  │  - KVM_RUN                           │      │
  │  │  - KVM_SET_USER_MEMORY_REGION        │      │
  │  │  - ...                               │      │
  │  └──────────────┬───────────────────────┘      │
  │                 │ ioctl                         │
  │  ┌──────────────▼───────────────────────┐      │
  │  │  KVM 内核模块                        │      │
  │  │  - 参数验证                          │      │
  │  │  - 状态机管理                        │      │
  │  │  - VM-Exit 处理                      │      │
  │  │  - 内存虚拟化                        │      │
  │  └──────────────────────────────────────┘      │
  │                                                │
  │  ┌──────────────────────────────────────┐      │
  │  │  Guest (不可信)                      │      │
  │  │  - VM-Entry/Exit                     │      │
  │  │  - EPT 隔离                          │      │
  │  └──────────────────────────────────────┘      │
  └────────────────────────────────────────────────┘

安全威胁:
  1. Guest → Host 逃逸
     防护: EPT 隔离、VMCS 状态验证、MSR 过滤
  2. 用户空间 VMM 被攻陷
     防护: jailer (seccomp + namespaces + cgroups)
  3. VMM 滥用 KVM ioctl
     防护: 参数验证、状态机约束
```

### 4.2 Firecracker jailer 架构

```
jailer 层次:
  ┌─ namespace 隔离 ─────────────────────────────┐
  │  - PID namespace (隔离进程树)                │
  │  - mount namespace (隔离文件系统)            │
  │  - net namespace (隔离网络栈)                │
  │  - user namespace (root 映射到非 root)       │
  └──────────────────────────────────────────────┘
  ┌─ seccomp 过滤 ──────────────────────────────┐
  │  允许的 syscall:                              │
  │  - read, write, ioctl (KVM)                  │
  │  - mmap, munmap (内存管理)                   │
  │  - epoll_wait (事件等待)                     │
  │  - futex (同步)                              │
  │  禁止的 syscall:                              │
  │  - execve (不允许执行新程序)                 │
  │  - fork/clone (不允许创建进程)               │
  │  - socket (网络被限制)                       │
  │  - open (文件访问被限制)                     │
  └──────────────────────────────────────────────┘
  ┌─ cgroup 限制 ────────────────────────────────┐
  │  - CPU 配额 (cpu.max)                        │
  │  - 内存限制 (memory.max)                     │
  │  - PID 限制 (pids.max)                       │
  └──────────────────────────────────────────────┘

攻击面减少:
  原始 QEMU: ~2000 个 syscall 可用
  Firecracker jailer: ~25 个 syscall 可用
  攻击面减少: ~99%
```

### 4.3 KVM ioctl 安全约束

```c
/* KVM 内部的安全检查 (kvm_main.c) */

/* 1. MM 一致性检查 */
if (vcpu->kvm->mm != current->mm || vcpu->kvm->vm_dead)
    return -EIO;

/* 2. 参数验证 */
/* KVM_SET_USER_MEMORY_REGION: */
if (mem->guest_phys_addr + mem->memory_size < mem->guest_phys_addr)
    return -EINVAL;  /* 溢出检查 */

/* 3. 状态机约束 */
/* KVM_RUN 前必须: */
/* - 至少一个 memslot */
/* - vCPU 初始化完成 */
/* - 寄存器已设置 */

/* 4. 互斥锁保护 */
/* kvm_vcpu_ioctl 持有 vcpu->mutex */
if (mutex_lock_killable(&vcpu->mutex))
    return -EINTR;
```

---

## 5. guest_memfd (6.12 新增)

### 5.1 概述

```
guest_memfd 是 KVM 在 6.12 中新增的特性:
  - 创建私有内存区域，Host 无法直接访问
  - 为机密计算 (TDX, SEV-SNP) 提供基础
  - 替代传统的 anonymous memory + KVM_SET_USER_MEMORY_REGION

传统内存模型:
  QEMU mmap() → 匿名内存 → KVM_SET_USER_MEMORY_REGION → EPT 映射
  Host 可以随意读写 Guest 内存 ← 安全漏洞!

guest_memfd 模型:
  KVM_CREATE_GUEST_MEMFD → 私有内存 → KVM_SET_USER_MEMORY_REGION2
  Host 无法直接访问 Guest 内存 ← 安全!

ioctl 接口:
  KVM_CREATE_GUEST_MEMFD (struct kvm_create_guest_memfd)
    - size: 内存大小
    - flags: KVM_GMEM_FLAG_*
  返回: 新的文件描述符

关联 ioctl:
  KVM_SET_USER_MEMORY_REGION2 (扩展版)
    - 支持 guest_memfd 文件
    - 设置 flags 表示使用私有内存
```

### 5.2 guest_memfd 实现

**文件**: `virt/kvm/guest_memfd.c:405-472`

```c
/* 来源: virt/kvm/guest_memfd.c:460-472 */

/*
 * kvm_gmem_create - 创建 guest_memfd
 *
 * 创建一个特殊的文件描述符，代表 Guest 的私有内存
 * Host 无法通过 mmap 访问该内存
 */
int kvm_gmem_create(struct kvm *kvm, struct kvm_create_guest_memfd *args)
{
    return __kvm_gmem_create(kvm, args->size, args->flags);
}

/* 底层实现 (simplified): */
static int __kvm_gmem_create(struct kvm *kvm, loff_t size, u64 flags)
{
    /* 创建 shmem 文件 (基于 tmpfs) */
    /* 设置 inode 操作禁止 Host 访问 */
    /* 关联到 KVM VM */
    /* 返回文件描述符 */
}
```

### 5.3 guest_memfd 使用流程

```c
/* 用户空间使用 guest_memfd */

/* 1. 创建 guest_memfd */
struct kvm_create_guest_memfd gmem_args = {
    .size = 0x10000000,  /* 256MB */
    .flags = 0,
};
int gmem_fd = ioctl(vm_fd, KVM_CREATE_GUEST_MEMFD, &gmem_args);

/* 2. 关联到 VM 内存区域 */
struct kvm_userspace_memory_region2 region = {
    .slot = 0,
    .guest_phys_addr = 0,
    .memory_size = 0x10000000,
    .userspace_addr = 0,  /* 不需要, guest_memfd 模式 */
    .flags = KVM_MEMORY_REGION_FLAG_GUEST_MEMFD,
    .guest_memfd_fd = gmem_fd,
    .guest_memfd_offset = 0,
};
ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION2, &region);

/* 3. Guest 访问该内存 */
/* 通过 EPT 映射，Guest 可以正常读写 */
/* Host 无法通过 mmap 直接访问 */
```

### 5.4 guest_memfd 与机密计算

```
guest_memfd 是机密计算的基础:

┌─ guest_memfd (6.12) ─────────────────────────────────────────┐
│  提供: 私有内存区域，Host 不可见                               │
│  作用: 内存隔离                                               │
│  安全级别: 基础 (Host 内核仍可窥探页表)                       │
└──────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─ Intel TDX (未来) ──────────────────────────────────────────┐
│  提供: 硬件加密的内存                                         │
│  基于: guest_memfd + TDX module                              │
│  作用: 内存加密 + 远程证明                                    │
│  安全级别: 强 (Host 内核无法读取加密内存)                     │
└──────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─ AMD SEV-SNP (未来) ────────────────────────────────────────┐
│  提供: 硬件加密内存 + 完整性保护                              │
│  基于: guest_memfd + PSP (Platform Security Processor)       │
│  作用: 内存加密 + 完整性 + 远程证明                           │
│  安全级别: 最强 (Host hypervisor 完全无法窥探)                │
└──────────────────────────────────────────────────────────────┘

MicroVM 意义:
  - guest_memfd 让 MicroVM 可以安全运行在共享基础设施上
  - 未来 TDX/SEV 集成将让 MicroVM 获得硬件级安全
  - 攻击面进一步减少 (Host 内核也无法读取 Guest 内存)
```

---

## 6. MicroVM KVM 调优清单

```
┌─ 启动优化 ─────────────────────────────────────────────────────┐
│  ✓ 使用 hugepages (-mem-prealloc -mem-path /dev/hugepages)     │
│  ✓ 预分配所有内存 (mem-prealloc=true)                          │
│  ✓ 使用精简 Guest 内核 (50MB 以内)                             │
│  ✓ 使用 initramfs 代替磁盘                                     │
│  ✓ 设置 halt_poll_ns=0 (减少启动延迟)                          │
└────────────────────────────────────────────────────────────────┘

┌─ 运行时优化 ───────────────────────────────────────────────────┐
│  ✓ 使用 virtio-MMIO (减少设备模拟开销)                         │
│  ✓ 启用 APICv + Posted Interrupts (减少中断路径开销)           │
│  ✓ 调整 halt_poll_ns (根据工作负载)                            │
│  ✓ 使用 vCPU pinning (taskset 或 cgroup cpuset)               │
│  ✓ 启用 TSC-deadline 模式 (减少定时器 VM-Exit)                 │
└────────────────────────────────────────────────────────────────┘

┌─ 安全加固 ─────────────────────────────────────────────────────┐
│  ✓ 使用 jailer (seccomp + namespaces + cgroups)                │
│  ✓ 最小化 KVM ioctl 使用 (只调用必需的)                        │
│  ✓ 禁用不需要的 KVM 特性 (嵌套虚拟化、调试)                    │
│  ✓ 使用只读 Guest 文件系统 (如适用)                            │
│  ✓ 启用 guest_memfd (6.12+, 机密计算基础)                      │
└────────────────────────────────────────────────────────────────┘

┌─ 监控与调试 ───────────────────────────────────────────────────┐
│  ✓ 跟踪 kvm:kvm_exit 统计 VM-Exit 分布                         │
│  ✓ 监控 kvm:kvm_halt_poll_ns 观察自适应行为                    │
│  ✓ 使用 perf kvm stat 分析性能瓶颈                             │
│  ✓ 检查 /sys/kernel/debug/kvm/ 获取运行时统计                  │
└────────────────────────────────────────────────────────────────┘
```
