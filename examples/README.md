# examples/ — 可运行示例代码

> 配合各 phase 的动手示例。每个子目录自带 README + Makefile，`make` 构建。
> 二进制产物已 gitignore（可由 Makefile 重建）。

## 示例索引

| 目录 | 内容 | 配合 phase | 构建/运行 |
|------|------|-----------|----------|
| [`kvm-api-demo/`](kvm-api-demo/README.md) | KVM API 用户态演示：完整 VM 生命周期（`kvm-demo.c`）+ 寄存器操作（`kvm-demo-regs.c`） | phase0 / phase9 基线 | `make && sudo ./kvm-demo` |
| [`cpuid-faulting-demo/`](cpuid-faulting-demo/README.md) | CPUID Faulting 特性检测/启用/使用 | phase1（CPUID 虚拟化） | `make && sudo ./test-cpuid-fault` |
| [`minimal-vmx/`](minimal-vmx/README.md) | 直接操作 VT-x 硬件的内核模块；`vmx-info/` 只读 MSR 报告能力（安全入门，不 VMXON） | phase1（VMX 特性检测） | `make && sudo insmod vmx-info/vmx-info.ko` |
| [`bpf-programs/`](bpf-programs/README.md) | BPF/bpftrace 追踪程序集（VM-Exit/EPT 缺页/中断延迟/VFIO DMA） | phase10 / phase11 | 见各 `.bpf` 头部说明 |

## 安全提示

- `minimal-vmx/vmx-demo.ko` 会执行 VMXON/VMCS 配置，**可能影响宿主稳定性**，谨慎加载；
  入门先用 `vmx-info/`（只读 MSR，不执行 VMXON）。
- 内核模块实验后 `rmmod` 卸载；涉及设备直通遵守 AGENTS.md 陷阱 #15。
- 确认走 KVM 而非 TCG（AGENTS.md #7）：`ls -l /proc/$(pgrep -f qemu-system-x86_64)/fd | grep -c kvm`。

## 与 practice/ 的区别

- `examples/`：独立可运行示例，演示单一机制（KVM API、CPUID Faulting、VMX、BPF）。
- 各 `phaseN/practice/`：围绕该章课程结论的**取证实验**（五段式，见 COURSE_FRAMEWORK §3.5）。
- `phase9-capstone/practice/mini-kvm/`：简化 KVM 内核模块（对照阅读，原 `examples/mini-kvm` 已迁此）。
