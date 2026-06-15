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
