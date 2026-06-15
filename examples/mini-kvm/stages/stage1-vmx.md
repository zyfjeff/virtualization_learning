# Stage 1: VMX 基础 - VMXON, VMCS, VM-Entry

> 对应课程 Phase 1: VT-x Basics
>
> 关键源码: `arch/x86/kvm/vmx/vmx.c::vmx_hardware_setup()`
>           `arch/x86/kvm/vmx/vmenter.S::__vmx_vcpu_run()`

---

## 🎯 阶段目标

实现 VMX 的基本操作：
- 检测 CPU 是否支持 VMX
- 执行 VMXON 启用 VMX 操作
- 配置 VMCS (Virtual Machine Control Structure)
- 执行首次 VM-Entry 进入 Guest 模式
- 处理第一次 VM-Exit 返回 Host

## 📖 核心概念

### VMX 操作模式

Intel VT-x 引入两种操作模式：
- **VMX Root Mode**: Host (VMM) 运行在 root 模式，拥有完全控制权
- **VMX Non-Root Mode**: Guest 运行在 non-root 模式，受限执行

模式切换通过 VM-Entry/VM-Exit 指令实现：
```
VMX Root Mode (Host)
    │
    ├── VMLAUNCH/VMRESUME ──→ VM-Entry
    │                              │
    │                              ▼
    │                    VMX Non-Root Mode (Guest)
    │                              │
    │                         Guest 执行...
    │                              │
    │                    VM-Exit (触发条件满足)
    │                              │
    └────── VM-Exit ───────────────┘
```

### VMCS (Virtual Machine Control Structure)

VMCS 是一个 4KB 的内存区域，保存了：
- **Host-State Area**: VM-Exit 后恢复的 Host 状态
- **Guest-State Area**: VM-Entry 加载的 Guest 状态
- **VM-Execution Control Fields**: 控制哪些事件触发 VM-Exit
- **VM-Exit Information Fields**: VM-Exit 的原因和上下文

VMCS 通过 VMWRITE/VMREAD 指令访问，不能直接内存访问。

## 🔧 mini-kvm.c 实现

### 1. VMX 初始化 (`mini_kvm_vmx_init`)

```c
/* 步骤 1: 检查 VMX 支持 */
if (!boot_cpu_has(X86_FEATURE_VMX)) {
    pr_err("CPU 不支持 VMX\n");
    return -ENODEV;
}

/* 步骤 2: 读取 VMX capability MSRs */
rdmsrl(MSR_IA32_VMX_BASIC, mini_kvm_global.vmx_basic);
mini_kvm_global.vmcs_revision_id = (u32)mini_kvm_global.vmx_basic;

/* 步骤 3: 配置 CR0/CR4 满足 VMX 要求 */
cr0 = read_cr0();
cr0 |= mini_kvm_global.vmx_cr0_fixed0;  /* 必须设置的位 */
cr0 &= mini_kvm_global.vmx_cr0_fixed1;  /* 必须清除的位 */
write_cr0(cr0);

cr4 = __read_cr4();
cr4 |= X86_CR4_VMXE;  /* ★ 关键: 启用 VMX */
__write_cr4(cr4);

/* 步骤 4: 分配并初始化 VMXON 区域 */
vmxon_region = (void *)__get_free_page(GFP_KERNEL);
*(u32 *)vmxon_region = mini_kvm_global.vmcs_revision_id;

/* 步骤 5: 执行 VMXON 指令 */
asm volatile ("vmxon %0" : : "m"(phys_addr) : "memory", "cc");
```

**对应课程**:
- Phase 1, Section 2: vmx_hardware_setup()
- 关键源码: `vmx.c:8404` (setup_vmcs_config)

### 2. VMCS 配置 (`mini_kvm_vcpu_vmx_setup`)

```c
/* VMCLEAR: 清除 VMCS 状态 */
asm volatile ("vmclear %0" : : "m"(phys_addr));

/* VMPTRLD: 加载当前 VMCS */
asm volatile ("vmptrld %0" : : "m"(phys_addr));

/* 配置 VM-Execution Controls */
vmcs_write(VMCS_CTRL_CPU_BASED,
    CPU_BASED_HLT_EXITING |      /* HLT 触发 VM-Exit */
    CPU_BASED_IO_EXITING  |      /* IO 触发 VM-Exit */
    CPU_BASED_ACTIVATE_SECONDARY);

vmcs_write(VMCS_CTRL_CPU_BASED2,
    CPU_BASED2_ENABLE_EPT);      /* 启用 EPT (Stage 2) */

/* 配置 Host State */
vmcs_write(VMCS_HOST_RIP, (u64) &&vmx_exit_handler);
vmcs_write(VMCS_HOST_CR0, read_cr0());
vmcs_write(VMCS_HOST_CS_SEL, __KERNEL_CS);

/* 配置 Guest State */
vmcs_write(VMCS_GUEST_RIP, initial_rip);
vmcs_write(VMCS_GUEST_RSP, initial_rsp);
vmcs_write(VMCS_GUEST_CR0, X86_CR0_PE | X86_CR0_PG);
vmcs_write(VMCS_GUEST_CS_AR, 0xa09b);  /* 64-bit code segment */
```

**对应课程**:
- Phase 1, Section 3: vmx_vcpu_run()
- 关键源码: `vmx.c:7344`

### 3. VM-Entry

```c
/* 通过汇编执行 VM-Entry */
asm volatile (
    "vmresume\n"     /* 或 vmlaunch (首次) */
    :
    :
    : "memory", "cc"
);
```

VM-Entry 成功后，CPU 进入 non-root 模式执行 Guest 指令。
当触发 VM-Exit 条件时，CPU 自动：
1. 保存 Guest 状态到 VMCS Guest-State Area
2. 加载 Host 状态从 VMCS Host-State Area
3. 跳转到 VMCS 中指定的 Host RIP

**对应课程**:
- Phase 1, Section 3: VMENTER 汇编入口
- 关键源码: `vmenter.S::__vmx_vcpu_run()`

### 4. VM-Exit 处理

```c
vmx_exit_handler:
    /* 读取 VM-Exit 信息 */
    vcpu->exit_reason = vmcs_read(VMCS_EXIT_REASON) & 0xFFFF;
    vcpu->exit_qualification = vmcs_read(VMCS_EXIT_QUALIFICATION);

    /* 分发到具体处理函数 */
    switch (vcpu->exit_reason) {
    case EXIT_REASON_HLT:
        /* HLT: 停止 vCPU */
        break;
    case EXIT_REASON_IO_INSTRUCTION:
        /* IO: 设备模拟 */
        break;
    case EXIT_REASON_EPT_VIOLATION:
        /* EPT Violation: 按需映射 */
        break;
    }
```

**对应课程**:
- Phase 1, Section 4: vmx_handle_exit()
- 关键源码: `vmx.c:6615`

## 🧪 实验验证

```bash
# 构建模块
make

# 加载模块 (先卸载 KVM)
sudo rmmod kvm_intel kvm
sudo insmod mini-kvm.ko

# 查看日志
dmesg | grep mini-kvm

# 预期输出:
# mini-kvm: === Stage 1: VMX 初始化 ===
# mini-kvm:   ✓ CPU 支持 VMX
# mini-kvm:   ✓ CPU 支持 EPT
# mini-kvm:   ✓ CR0 配置完成
# mini-kvm:   ✓ CR4 配置完成 (VMXE=1)
# mini-kvm:   ✓ VMXON 执行成功
# mini-kvm: === Stage 1 完成: VMX 已启用 ===
```

## 📝 检查清单

完成 Stage 1 后，确认能回答：
- [ ] VMX Root Mode 和 Non-Root Mode 的区别
- [ ] VMCS 的四大区域 (Host/Guest/Control/Exit Info)
- [ ] VMXON 区域的作用和格式
- [ ] VM-Entry 时 CPU 做了哪些事
- [ ] VM-Exit 时 CPU 做了哪些事
- [ ] 为什么需要 CR4.VMXE

## 🔗 下一步

Stage 2: 内存虚拟化 (EPT)
