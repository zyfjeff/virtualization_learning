# Phase 10：MicroVM 架构专项

> 基于 Linux 6.12.93 源码 | 面向 MicroVM 架构师和开发者

---

## 📚 阶段概述

本阶段从 KVM 内核态视角深入分析 MicroVM 的核心技术，包括：

1. **MicroVM vs 传统 VM 架构对比** — QEMU/Firecracker/Cloud Hypervisor/crosvm
2. **KVM VM 创建启动路径** — 从 KVM_CREATE_VM 到首次 VM-Entry
3. **最小设备模型** — MMIO 设备、virtio-MMIO、自定义设备
4. **安全性考虑** — jailer 沙箱、seccomp、攻击面最小化
5. **guest_memfd (6.12 新增)** — 私有内存区域、机密计算基础

---

## 🎯 学习目标

完成本阶段后，你应该能够：

- 从 KVM 源码层面理解 MicroVM 启动路径的每个步骤
- 量化不同 VMM (QEMU vs Firecracker) 的 KVM 使用差异
- 设计最小设备模型并理解其 KVM 交互
- 理解 guest_memfd 如何为机密计算奠基
- 针对 MicroVM 场景优化 KVM 参数

---

## 📖 章节导航

| 章节 | 内容 | 关键源码位置 |
|------|------|-------------|
| 1. VMM 对比 | QEMU vs Firecracker vs Cloud Hypervisor | `virt/kvm/kvm_main.c` |
| 2. 启动路径 | VM 创建到首次 VM-Entry 的完整路径 | `virt/kvm/kvm_main.c:5492` |
| 3. 设备模型 | MMIO 设备、virtio-MMIO 实现 | `virt/kvm/kvm_main.c` |
| 4. 安全模型 | jailer、ioctl 隔离 | `virt/kvm/kvm_main.c` |
| 5. guest_memfd | 私有内存、机密计算 | `virt/kvm/guest_memfd.c` |

---

## 🗺️ MicroVM 技术栈全景

```
┌─ 用户层 ─────────────────────────────────────────────────────────────┐
│                                                                       │
│  ┌─ Firecracker ─┐    ┌─ Cloud Hypervisor ─┐    ┌─ crosvm ─┐        │
│  │  Rust         │    │  Rust              │    │  Rust    │        │
│  │  最小设备模型  │    │  模块化 (PCI/MMIO)  │    │ ChromeOS │        │
│  │  ~125ms 启动  │    │  ~200ms 启动       │    │ 安全模型 │        │
│  └───────┬───────┘    └──────────┬─────────┘    └────┬─────┘        │
│          │                       │                    │              │
│          └───────────────────────┼────────────────────┘              │
│                                  │ KVM API (ioctl)                  │
└──────────────────────────────────┼──────────────────────────────────┘
                                   │
┌──────────────────────────────────┼──────────────────────────────────┐
│ 内核层 (KVM)                      │                                  │
│                                  │                                  │
│  ┌─────────────────────────────┐ │                                  │
│  │  virt/kvm/kvm_main.c        │ │  VM/vCPU 生命周期               │
│  │  arch/x86/kvm/x86.c         │ │  VM-Entry/VM-Exit 处理          │
│  │  arch/x86/kvm/vmx/vmx.c     │ │  VMX 实现                       │
│  │  arch/x86/kvm/mmu/tdp_mmu.c │ │  EPT 页表管理                   │
│  └─────────────────────────────┘ │                                  │
│                                  │                                  │
│  ┌─────────────────────────────┐ │                                  │
│  │  virt/kvm/guest_memfd.c     │ │  ★ 6.12 新增: 私有内存         │
│  └─────────────────────────────┘ │                                  │
│                                  │                                  │
└──────────────────────────────────┼──────────────────────────────────┘
                                   │
┌──────────────────────────────────┼──────────────────────────────────┐
│ 硬件层                           │                                  │
│  ┌─ Intel VT-x ─┐  ┌─ EPT ─┐   │  ┌─ VT-d ─┐  ┌─ TDX (未来) ─┐   │
│  │ VM-Entry     │  │ GPA→HPA│   │  │ DMA 隔离│  │ 硬件加密内存 │   │
│  │ VM-Exit      │  │ 大页支持│   │  │ 中断投递│  │ 远程证明    │   │
│  └──────────────┘  └────────┘   │  └─────────┘  └──────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 🔑 关键知识点速查

### MicroVM vs 传统 VM 的 KVM 使用差异

| 维度 | QEMU (传统 VM) | Firecracker (MicroVM) |
|------|---------------|----------------------|
| KVM ioctl 数量 | ~30+ | ~10 |
| 设备模型 | 完整 PCI + USB + 模拟设备 | 仅 virtio-MMIO |
| 中断控制器 | KVM in-kernel IRQCHIP | 用户空间实现 |
| 启动时间 | ~1-3s | ~125ms |
| 内存占用 | ~30-50MB | ~3-5MB |
| VM-Exit 频率 | ~100K/s | ~500K/s |

### 关键 KVM 参数调优 (MicroVM 场景)

```bash
# 启动优化
echo 0 > /sys/module/kvm/parameters/halt_poll_ns     # 禁用 polling, 减少启动延迟

# 运行时优化
echo 200000 > /sys/module/kvm/parameters/halt_poll_ns # 较小 polling 窗口
echo 1 > /sys/module/kvm_intel/parameters/ept         # 启用 EPT
echo 1 > /sys/module/kvm_intel/parameters/pml         # 启用 PML

# 安全加固
# 使用 jailer (seccomp + namespaces + cgroups)
# 最小化 KVM ioctl 使用
```

### guest_memfd 学习路径

```
传统内存模型:
  mmap → anonymous memory → KVM_SET_USER_MEMORY_REGION → EPT 映射
  Host 可读写 Guest 内存 ← 安全漏洞

guest_memfd 模型 (6.12+):
  KVM_CREATE_GUEST_MEMFD → 私有内存 → KVM_SET_USER_MEMORY_REGION2
  Host 无法访问 Guest 内存 ← 安全

未来: guest_memfd + Intel TDX / AMD SEV = 硬件加密内存 + 远程证明
```

---

## 🔗 与其他 Phase 的关联

```
本阶段建立在前面所有 phase 的基础上:

phase0 (框架层)        → 理解 KVM ioctl 入口
phase1 (VT-x)          → 理解 VM-Entry/Exit
phase2 (EPT)           → 理解内存虚拟化
phase3 (中断)          → 理解中断路径
phase4 (vhost)         → 理解 vhost 加速
phase5 (VFIO)          → 理解设备直通
phase6 (时钟)          → 理解 TSC/kvmclock
phase7 (实践)          → 综合运用
phase8 (性能优化)      → halt-polling/PLE 在 MicroVM 中的应用
phase9 (调试测试)      → bpftrace/perf 分析 MicroVM 性能
```

---

## 📚 扩展阅读

### MicroVM 项目源码

- [Firecracker](https://github.com/firecracker-microvm/firecracker) - AWS 开源的 MicroVM VMM (Rust)
- [Cloud Hypervisor](https://github.com/cloud-hypervisor/cloud-hypervisor) - Intel 开源的云计算 VMM (Rust)
- [crosvm](https://github.com/google/crosvm) - Google ChromeOS 的 VMM (Rust)
- [rust-vmm](https://github.com/rust-vmm) - VMM 组件共享库

### 关键文档

- [Firecracker Architecture](https://github.com/firecracker-microvm/firecracker/blob/main/docs/design.md)
- [Cloud Hypervisor Design](https://github.com/cloud-hypervisor/cloud-hypervisor/blob/main/docs/design.md)
- [KVM guest_memfd](https://docs.kernel.org/virt/kvm/api.html) - KVM API 文档
- [Intel TDX](https://www.intel.com/content/www/us/en/developer/tools/trust-domain-extensions/overview.html)

### 学术论文

- *"Firecracker: Lightweight Virtualization for Serverless Applications"* (2020)
- *"Cloud Hypervisor: A Lightweight VMM for Cloud Workloads"* (2022)
- *"KVM guest_memfd: Private Memory for Confidential Computing"* (2024)

---

## 🔧 实践建议

### 动手实验

1. **启动一个 Firecracker MicroVM**:
   ```bash
   curl -L https://github.com/firecracker-microvm/firecracker/releases/download/v1.6.0/firecracker-v1.6.0-x86_64.tgz | tar xz
   ./firecracker --config-file config.json
   ```

2. **观察 MicroVM 的 KVM 使用**:
   ```bash
   # 跟踪 Firecracker 的 KVM ioctl 调用
   sudo strace -e ioctl -p $(pgrep firecracker) 2>&1 | grep KVM
   
   # 对比 QEMU 的 KVM ioctl 调用
   sudo strace -e ioctl -p $(pgrep qemu) 2>&1 | grep KVM
   ```

3. **性能对比**:
   ```bash
   # 启动时间对比
   time ./firecracker --config-file config.json
   time qemu-system-x86_64 -enable-kvm -m 512M ...
   
   # VM-Exit 频率对比 (使用 phase8 的方法)
   sudo perf kvm stat record -p $(pgrep firecracker) -- sleep 10
   sudo perf kvm stat record -p $(pgrep qemu) -- sleep 10
   ```

---

## ✅ 验证清单

完成本阶段后，确认能回答：

- [ ] Firecracker 与 QEMU 使用哪些不同的 KVM ioctl？
- [ ] 为什么 MicroVM 的 VM-Exit 频率通常高于传统 VM？
- [ ] jailer 如何通过 seccomp 限制 syscall？
- [ ] guest_memfd 如何防止 Host 访问 Guest 内存？
- [ ] 如何为 MicroVM 场景调优 halt-polling？
- [ ] TDX/SEV 如何基于 guest_memfd 实现机密计算？
- [ ] 能否从零编写一个最小的 MicroVM (使用 KVM API)？
