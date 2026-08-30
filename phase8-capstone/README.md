# 第八阶段：毕业建造 — 最小 VMM

> 基于 Linux 6.12.93 内核源码 | 综合建造阶段

---

## 🎯 本阶段定位

前七个阶段逐层拆解了 KVM 的内核实现：框架、VT-x、EPT、IOMMU、中断、
virtio、VFIO、时钟。本阶段反过来**从零建造**：只用 `/dev/kvm` 暴露的
ioctl，写一个能启动 Linux 的用户态 VMM，然后逐级加上 virtio 设备、
VFIO 直通，最后与 QEMU/Firecracker 对标性能。

**与 `examples/mini-kvm` 的区分**（两者互补，不要混淆）：

| | `examples/mini-kvm` | 本阶段（phase8） |
|---|---|---|
| 造的是什么 | 一个"KVM"（**内核模块**，自己实现 VM-Entry/Exit） | 一个"VMM"（**用户态程序**，基于 `/dev/kvm`） |
| 视角 | KVM 内核态怎么实现 | VMM 怎么驱动 KVM |
| 前置 | phase0-2（VT-x/VMCS 机制） | phase0-7 全部 |

读源码时本阶段反复用到两条对照线：**QEMU 是怎么调这些 ioctl 的**
（`/root/code/qemu-10.1.0-rc2/`），以及 **KVM 内核侧收到后做了什么**
（`/root/code/linux-6.12.93/`）。

---

## 🪜 项目阶梯

| # | 项目 | 交付物 | 整合的 phase |
|---|------|--------|-------------|
| 1 | [可启动的最小 VMM](project1-minivmm-boot.md) | bzImage + initramfs 引导到 shell | phase0/1/2/4/7 |
| 2 | [自制 virtio-mmio 设备](project2-minivmm-virtio.md) | 在自己的 VMM 里实现 virtio-blk + console | phase5 |
| 3 | [VFIO 设备直通](project3-minivmm-vfio.md) | 把真实设备直通进自己的 VMM | phase3/4/6 |
| 4 | [性能对标](project4-minivmm-bench.md) | 与 QEMU/Firecracker 对比启动延迟与 VM-Exit 分布 | phase9/10/11 |

**建议顺序完成**：每个项目都建立在前一个的 VMM 代码之上。

---

## 🧱 基线代码

不要从零写第一个 ioctl 循环，从已验证的示例起步：

| 起点 | 内容 |
|------|------|
| `../examples/kvm-api-demo/kvm-demo.c` | 完整 KVM API 生命周期：open → CREATE_VM → TSS/identity map → memslot → CREATE_VCPU → mmap kvm_run → 运行循环（处理 `KVM_EXIT_HLT`/`KVM_EXIT_IO`），每步注释标注内核侧函数 |
| `../phase7-timer-virt/practice/common.h` | 可复用的最小 VMM 骨架：`vcpu_setup_cpuid()`、寄存器封装、端口 I/O 处理；三个已跑通的实验（TSC offset / kvmclock / TSC-deadline 中断）都在它之上 |
| `../examples/mini-kvm/` | 对照阅读：同一个概念（vCPU 循环、中断注入）在内核模块侧的实现 |

---

## 🔧 环境要求

| 项 | 要求 |
|----|------|
| CPU | Intel VT-x + EPT（`grep -E 'vmx|ept' /proc/cpuinfo`），TSC-deadline timer |
| 内核 | 6.12.93（本项目基线），加载 `kvm` + `kvm_intel` |
| 权限 | `/dev/kvm` 读写（实验均需 root） |
| 工件 | 自编内核（`scripts/vm/build-kernel.sh` 产物 bzImage）+ initramfs（`scripts/vm/build-rootfs-*.sh`） |
| 项目 3 额外 | 启用 VT-d（`intel_iommu=on`）、可直通的独立 IOMMU group 设备（详见 `../phase6-vfio/README.md` §1.4） |

确认运行真的走了 KVM（不是任何模拟回退）：程序应持有指向 `/dev/kvm`
的 fd，且宿主侧 `kvm:kvm_exit` tracepoint 有事件。

---

## ✅ 总体验收标准

完成全部 4 个项目后，你应该能够：

- [ ] 不借助任何现成 VMM，从 `/dev/kvm` 启动一个 Linux guest 到 shell
- [ ] 说出每个 KVM ioctl 在内核侧落到哪个函数（路径 + 行号）
- [ ] 手写 virtqueue 处理循环，解释 feature 协商与 `KVM_IRQFD`/ioeventfd 的作用
- [ ] 把真实 PCI 设备直通进自己的 VMM，解释 IOMMU group / ACS 的前置判定
- [ ] 用 `perf kvm stat` 与 ftrace 对比自己的 VMM 与 QEMU 的 VM-Exit 分布，
      解释差异来源

---

## 📝 说明

本阶段文档是**建造指南**：给出目标、里程碑、内核侧代码路径与验收标准，
VMM 代码由你自己实现。每个项目文档的"内核侧代码路径"表中所有引用均基于
6.12.93 源码逐一核实；发现与源码不符请按 `AGENTS.md` 规范提勘误。

参考资料：

- Linux KVM API 文档：`/root/code/linux-6.12.93/Documentation/virt/kvm/api.rst`
- x86 boot protocol：`/root/code/linux-6.12.93/Documentation/arch/x86/boot.rst`
- Virtio 规范：`../virtio-v1.3-csd01.pdf`
- Intel VMX 规范：`../intel-vmx.pdf`；VT-d 规范：`../intel-vtd.pdf`
