# Mini-KVM：循序渐进实现一个简化版 KVM

> 伴随 Phase 0-10 课程，渐进式实现一个可运行的简化版 KVM 内核模块
>
> 基于 Linux 6.12.93 内核

---

## 🎯 项目目标

通过亲手实现一个简化版的 KVM，深入理解：

- **KVM 如何管理 VM 和 vCPU** (对应 Phase 0)
- **VMX 硬件如何执行 VM-Entry/Exit** (对应 Phase 1)
- **EPT 如何实现内存虚拟化** (对应 Phase 2)
- **中断如何注入到 Guest** (对应 Phase 3)
- **设备如何被模拟** (对应 Phase 4-5)
- **性能优化如何生效** (对应 Phase 8)

最终得到一个**可实际加载运行**的内核模块，能：
- 创建并运行一个极简的 Guest (执行 "Hello from Guest!" 串口输出)
- 处理 VM-Exit (HLT, CPUID, IO, EPT Violation)
- 通过 EPT 隔离 Guest 内存
- 模拟一个串口设备

---

## 📚 阶段与课程映射

```
Stage 1: VMX 基础 (Phase 1)
  ├── VMXON: 启用 VMX 操作
  ├── VMCLEAR/VMPTRLD: 管理 VMCS
  ├── VMCS 配置: 设置 Host/Guest 状态
  └── 首次 VM-Entry: 进入 Guest 模式

Stage 2: 内存虚拟化 (Phase 2)
  ├── EPT 页表: 4 级页表结构
  ├── GPA → HPA: Guest 物理地址映射
  ├── 内存分配: 为 Guest 分配物理页
  └── EPT Violation 处理: 按需映射

Stage 3: 中断处理 (Phase 3)
  ├── 虚拟 LAPIC: 简化版本地中断控制器
  ├── 中断注入: 通过 VMCS 注入外部中断
  └── NMI/异常: 处理非屏蔽中断

Stage 4: 设备模拟 (Phase 4-5)
  ├── PIO 处理: IO_INSTRUCTION VM-Exit
  ├── MMIO 处理: EPT Violation 触发
  └── 串口模拟: 0x3f8 端口输出 "Hello"

Stage 5: 运行循环 (Phase 0, 8)
  ├── vcpu_run: 主运行循环
  ├── Exit 分发: 根据 exit_reason 处理
  ├── halt-polling: 简化版 polling
  └── 返回用户空间: 无法处理的 exit
```

---

## 🏗️ 项目结构

```
examples/mini-kvm/
├── README.md              ← 本文件
├── Makefile               ← 构建脚本
├── mini-kvm.c             ← ★ 主模块 (所有 Stage 集中实现)
├── mini-kvm.h             ← 内部头文件
├── vmx.c                  ← Stage 1: VMX 操作
├── ept.c                  ← Stage 2: EPT 内存虚拟化
├── interrupt.c            ← Stage 3: 中断处理
├── device.c               ← Stage 4: 设备模拟
├── test-mini-kvm.c        ← 用户空间测试程序
└── stages/
    ├── stage1-vmx.md      ← Stage 1 详细说明
    ├── stage2-ept.md      ← Stage 2 详细说明
    ├── stage3-interrupt.md← Stage 3 详细说明
    ├── stage4-device.md   ← Stage 4 详细说明
    └── stage5-runloop.md  ← Stage 5 详细说明
```

---

## 🔨 构建与运行

### 前置条件

```bash
# 1. 确保内核源码在预期位置
ls /root/code/linux-6.12.93

# 2. 检查 CPU 支持 VMX
grep -E "vmx|ept|vpid" /proc/cpuinfo | head -1

# 3. 卸载现有 KVM 模块 (重要! 不能同时有两个 VMX 用户)
sudo rmmod kvm_intel kvm 2>/dev/null || true
```

### 构建

```bash
cd /root/code/kvm-study/examples/mini-kvm
make

# 构建用户空间测试程序
make user
```

### 运行

```bash
# 加载模块
sudo insmod mini-kvm.ko

# 查看日志
dmesg | tail -20

# 运行测试 (需要 root)
sudo ./test-mini-kvm

# 预期输出:
# Guest says: Hello from Mini-KVM Guest!
# VM exited normally.

# 卸载模块
sudo rmmod mini_kvm
```

### 重新加载 KVM

```bash
# 恢复标准 KVM
sudo modprobe kvm_intel
```

---

## 📖 学习路径

### 第一步：理解整体架构

```
用户空间                              内核空间 (mini-kvm.ko)
────────                              ─────────────────────
open("/dev/mini-kvm") ─────────────→  mini_kvm_open()
ioctl(fd, CREATE_VM)    ───────────→  mini_kvm_create_vm()
                                        └→ alloc VMCS
                                        └→ setup EPT
ioctl(vmfd, CREATE_VCPU) ──────────→  mini_kvm_create_vcpu()
                                        └→ alloc vCPU
ioctl(vcpufd, SET_MEMORY) ─────────→  mini_kvm_set_memory()
                                        └→ map GPA → HPA
ioctl(vcpufd, RUN) ────────────────→  mini_kvm_run()
                                        └→ vcpu_run() [Stage 5]
                                            └→ vmx_vcpu_run() [Stage 1]
                                                └→ VMRESUME
                                                └→ Guest 执行
                                                └→ VM-Exit
                                            └→ handle_exit()
                                                └→ HLT? → 停止
                                                └→ IO? → 模拟 [Stage 4]
                                                └→ EPT? → 映射 [Stage 2]
                                                └→ 其他 → 返回用户空间
```

### 第二步：逐个 Stage 深入

按顺序阅读 `stages/stage1-vmx.md` 到 `stages/stage5-runloop.md`，每个阶段：

1. **阅读说明**：理解本阶段的目标和原理
2. **查看代码**：在 `mini-kvm.c` 中找到对应的 Stage 标记
3. **对照课程**：参考对应的 phase 章节深入理解
4. **实验验证**：加载模块，运行测试，观察 dmesg 输出

### 第三步：扩展实验

完成基础后，尝试：

- 添加新的 VM-Exit 处理 (如 MSR 读写)
- 实现第二个串口设备
- 添加简单的中断控制器
- 实现 halt-polling 优化
- 支持多个 vCPU

---

## 🔍 关键代码路径

### VM-Entry 路径 (Stage 1)

```c
mini_kvm_run()
  └→ vmx_vcpu_run(vcpu)              [vmx.c]
      ├→ vmcs_write(GUEST_RIP, ...)   // 设置 Guest RIP
      ├→ vmcs_write(GUEST_RSP, ...)   // 设置 Guest RSP
      ├→ vmcs_write(HOST_RIP, ...)    // 设置 Host 返回地址
      ├→ vmcs_write(HOST_RSP, ...)    // 设置 Host 栈
      └→ asm volatile ("vmresume")    // ★ VM-Entry!
          └→ Guest 开始执行
          └→ VM-Exit 发生
          └→ 返回到 vmx_vcpu_run
```

### EPT Violation 处理 (Stage 2)

```c
handle_ept_violation(vcpu)
  └→ gpa = vmcs_read(EXIT_QUALIFICATION)
  └→ hpa = alloc_page()               // 分配物理页
  └→ ept_set_entry(gpa, hpa)          // 建立 EPT 映射
      └→ ept_pml4 → ept_pdpt → ept_pd → ept_pt → ept_entry
  └→ return RESUME_GUEST              // 重新进入 Guest
```

### IO 处理 (Stage 4)

```c
handle_io(vcpu)
  └→ port = vmcs_read(EXIT_QUALIFICATION) & 0xFFFF
  └→ if (port == 0x3f8) {             // 串口
      └→ if (write) {
          └→ ch = vcpu->arch.regs[RAX] & 0xFF
          └→ printk("Guest says: %c", ch)
      └→ }
  └→ }
  └→ advance_rip()                    // 跳过 IO 指令
  └→ return RESUME_GUEST
```

---

## ⚠️ 注意事项

1. **必须先卸载 KVM**：VMX 一次只能被一个模块使用
2. **不要在生产环境运行**：这是教学项目，未经安全审计
3. **需要 root 权限**：加载内核模块需要 root
4. **CPU 必须支持 VMX + EPT**：`grep vmx /proc/cpuinfo`
5. **崩溃恢复**：如果模块导致内核崩溃，硬重启后重新加载

---

## 📚 对应课程章节

| 项目 Stage | 对应 Phase | 主要章节 |
|-----------|-----------|---------|
| Stage 1: VMX 基础 | Phase 1 | vmx_vcpu_run, vmx_handle_exit |
| Stage 2: EPT 内存 | Phase 2 | kvm_tdp_mmu_map, make_spte |
| Stage 3: 中断处理 | Phase 3 | vmx_inject_irq, vmx_sync_pir_to_irr |
| Stage 4: 设备模拟 | Phase 4-5 | handle_io, handle_mmio |
| Stage 5: 运行循环 | Phase 0, 8 | vcpu_run, halt-polling |

---

## 🎓 预期学习成果

完成本项目后，你将能够：

- [ ] 从源码层面解释 VM-Entry/Exit 的完整过程
- [ ] 手动配置 VMCS 的各个字段
- [ ] 实现 EPT 页表并处理 EPT Violation
- [ ] 实现简单的设备模拟 (PIO/MMIO)
- [ ] 理解 KVM 的设计决策为什么是这样
- [ ] 为真实的 KVM 贡献代码
