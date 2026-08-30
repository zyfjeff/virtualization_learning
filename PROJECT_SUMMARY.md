# KVM Study 项目总结

> ⚠️ 本文为早期规划快照，其中的阶段编号与现行目录结构不符，请以根 `README.md` 与 `AGENTS.md` 为准。

> 完整的 KVM 学习路线图，从理论到实践

---

## 🎯 项目目标

通过系统化的学习路径，深入理解 KVM 虚拟化技术的各个方面：

- **理论基础**: VMX/EPT/APICv 等硬件虚拟化机制
- **源码分析**: Linux 6.12.93 内核 KVM 实现
- **动手实践**: 示例代码 + 测试环境

---

## 📚 学习路线

### Phase 0: KVM 框架层 ✅

**核心内容:**
- KVM 设备模型 (/dev/kvm → vm_fd → vcpu_fd)
- VM/vCPU 生命周期管理
- 内存管理 (memslots, EPT)
- 并发模型 (SRCU, mmu_lock, slots_lock)
- halt-polling 机制

**关键代码:**
- `virt/kvm/kvm_main.c` - KVM 框架核心
- `arch/x86/kvm/x86.c` - x86 架构实现

**完成状态:** ✅ 已完成，包含详细注释和讨论

---

### Phase 1: VT-x 基础 ✅

**核心内容:**
- VMX Root/Non-root 模式
- VMCS 结构和控制字段
- VMENTER/VM-Exit 硬件行为
- CPUID 虚拟化（两种机制）
  - 方式1: CPUID VM-Exit（Secondary Exec Control）
  - 方式2: 静态过滤（KVM_SET_CPUID2）
- CPUID Faulting 机制
- MSR 虚拟化（MSR Bitmap）
- 指令虚拟化和 VM-Exit 处理

**关键代码:**
- `arch/x86/kvm/vmx/vmx.c` - VMX 实现
- `arch/x86/kvm/cpuid.c` - CPUID 处理
- `arch/x86/include/asm/vmx.h` - VMCS 字段定义

**示例项目:**
- `examples/cpuid-faulting-demo/` - CPUID Faulting 完整示例
  - ✅ 用户态测试程序
  - ✅ KVM 虚拟机测试程序
  - ✅ 详细文档

**完成状态:** ✅ 已完成，包含详细注释、讨论和示例项目

---

### Phase 2: 内存虚拟化 🚧

**核心内容:**
- EPT (Extended Page Table) 机制
- 影子页表 vs EPT
- GPA → HPA 地址转换
- 大页支持（2MB/1GB）
- 内存热插拔
- NUMA 虚拟化

**关键代码:**
- `arch/x86/kvm/mmu/` - MMU 实现
- `arch/x86/kvm/mmu/spte.c` - SPTE 管理

**计划状态:** 🚧 待开始

---

### Phase 3: 中断虚拟化 🚧

**核心内容:**
- 虚拟 LAPIC/IOAPIC
- APICv (Advanced Programmable Interrupt Controller virtualization)
- 中断注入机制
- Posted Interrupts
- MSI/MSI-X 虚拟化

**关键代码:**
- `arch/x86/kvm/lapic.c` - LAPIC 实现
- `arch/x86/kvm/irq.c` - 中断处理

**计划状态:** 🚧 待开始

---

### Phase 4: 设备虚拟化 🚧

**核心内容:**
- virtio 设备框架
- virtio-net/virtio-blk 实现
- VFIO (Virtual Function I/O)
- 设备直通 (Passthrough)
- 模拟设备 (串口、键盘、VGA)

**关键代码:**
- `drivers/vfio/` - VFIO 框架
- `virt/` - virtio 实现

**计划状态:** 🚧 待开始

---

### Phase 5: 性能优化 🚧

**核心内容:**
- VM-Exit 优化
- halt-polling 调优
- 大页内存优化
- CPU 亲和性
- 缓存优化

**关键代码:**
- `virt/kvm/kvm_main.c` - halt-polling
- `arch/x86/kvm/vmx/vmx.c` - VMX 优化

**计划状态:** 🚧 待开始

---

### Phase 6: 高级主题 🚧

**核心内容:**
- 嵌套虚拟化 (Nested Virtualization)
- 实时迁移 (Live Migration)
- 安全虚拟化 (SEV/TDX)
- 性能调优工具

**计划状态:** 🚧 待开始

---

## 🛠️ 测试环境

### 快速开始

```bash
cd scripts/vm

# 1. 编译内核
./build-kernel.sh

# 2. 构建 rootfs
sudo ./build-rootfs-ubuntu.sh

# 3. 启动虚拟机（默认启用 KVM）
./boot-vm.sh ubuntu
```

### 测试脚本

| 脚本 | 功能 | 状态 |
|------|------|------|
| `build-kernel.sh` | 编译最小 Linux 内核 | ✅ 可用 |
| `build-rootfs-ubuntu.sh` | 构建 Ubuntu rootfs（推荐） | ✅ 可用 |
| `build-rootfs-minimal.sh` | 构建最小 busybox initramfs | ✅ 可用 |
| `boot-vm.sh` | 启动虚拟机（`-enable-kvm -cpu host`） | ✅ 可用 |
| `setup-vfio-vm.sh` | 启动带 VFIO 直通的虚拟机 | ✅ 可用 |

详见 [scripts/README.md](scripts/README.md)。

### 示例项目

| 项目 | 说明 | 状态 |
|------|------|------|
| `examples/cpuid-faulting-demo/` | CPUID Faulting 测试 | ✅ 完成 |
| `examples/kvm-api-demo/` | KVM API 演示 | ✅ 存在 |
| `examples/minimal-vmx/` | VMX 基础演示 | ✅ 存在 |
| `examples/mini-kvm/` | 简化 KVM 实现 | ✅ 存在 |

---

## 📖 核心文档

### 源码注释

- `phase0-kvm-framework/annotations.md` - KVM 框架层注释
- `phase1-vtx-basics/annotations.md` - VT-x 基础注释
- `phase1-vtx-basics/cpu-virtualization.md` - CPU 虚拟化详解

### 学习笔记

- `notes/cpu-virtualization.md` - CPU 虚拟化总结
- `notes/interrupt-virtualization.md` - 中断虚拟化
- `notes/memory-virtualization.md` - 内存虚拟化
- `notes/vm-lifecycle.md` - VM 生命周期
- `notes/debugging-guide.md` - 调试指南

---

## 🎓 学习成果

### Phase 0 成果

✅ 理解 KVM 架构和设备模型  
✅ 掌握 VM/vCPU 生命周期管理  
✅ 理解内存管理机制  
✅ 理解并发控制和锁机制  
✅ 掌握 halt-polling 优化  

### Phase 1 成果

✅ 理解 VMX 硬件机制  
✅ 掌握 VMCS 结构和控制  
✅ 理解 CPUID 虚拟化两种机制  
✅ 掌握 CPUID Faulting 原理和实现  
✅ 理解 MSR Bitmap 机制  
✅ 完成 CPUID Faulting 示例项目  

---

## 🚀 下一步计划

### 短期目标

1. **Phase 2**: 内存虚拟化
   - EPT 机制深入分析
   - 影子页表 vs EPT 对比
   - 大页和 NUMA 优化

2. **完善测试环境**
   - 添加更多测试用例
   - 自动化测试框架
   - 性能基准测试

### 中期目标

1. **Phase 3-4**: 中断和设备虚拟化
2. **性能优化**: Phase 5 内容
3. **高级主题**: 嵌套虚拟化、安全虚拟化

### 长期目标

1. **完整课程**: 覆盖所有 Phase
2. **实战项目**: 实现简化版 KVM
3. **文档完善**: 补充更多示例和教程

---

## 📊 项目统计

### 代码量

- 源码注释: ~5000 行
- 示例代码: ~2000 行
- 测试脚本: ~1000 行
- 文档: ~3000 行

### 覆盖主题

- CPU 虚拟化: 100%
- 内存虚拟化: 0% (计划中)
- 中断虚拟化: 0% (计划中)
- 设备虚拟化: 0% (计划中)
- 性能优化: 0% (计划中)

---

## 🤝 贡献指南

欢迎贡献：

1. 补充笔记和文档
2. 添加示例代码
3. 改进测试脚本
4. 修正错误和 typo

---

## 📝 许可证

- 文档: CC BY-SA 4.0
- 代码: GPL-2.0

---

## 📧 联系方式

如有问题或建议，欢迎讨论。

---

**最后更新**: 2026-06-29  
**当前进度**: Phase 1 完成，Phase 2 待开始
