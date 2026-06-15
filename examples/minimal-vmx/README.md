# Minimal VMX 演示 - 直接操作VT-x硬件

> 参考 [Hypervisor 101 in Rust](https://tandasat.github.io/Hypervisor-101-in-Rust/) 的动手练习模式

## 文件说明

| 文件 | 风险 | 说明 |
|------|------|------|
| `vmx-info.c` | ★ 安全 | 只读取MSR，报告CPU虚拟化能力 |
| `vmx-demo.c` | ★★★ 有风险 | 执行VMXON/VMCS/VMLAUNCH |

## 快速开始

### 步骤1: VMX能力检测 (安全入门)

```bash
cd /root/code/kvm-study/examples/minimal-vmx/vmx-info/

# 编译和加载
make
sudo insmod vmx-info.ko

# 查看输出
sudo dmesg | tail -40

# 卸载
sudo rmmod vmx-info
```

**预期输出** (示例):
```
vmx-info: ============================================
vmx-info: VMX Capability MSR Reader
vmx-info: ============================================
vmx-info: ✓ CPU支持VMX (X86_FEATURE_VMX)
vmx-info: --- VMX Capability MSRs ---
vmx-info:   IA32_VMX_BASIC              : 0x...
vmx-info:   [解码 IA32_VMX_BASIC]
vmx-info:     VMCS Revision ID    : 1
vmx-info:     VMXON Region Size   : 4096 bytes
vmx-info:     True Controls       : YES
vmx-info:   [解码 IA32_VMX_EPT_VPID_CAP]
vmx-info:     [ 0] EPT supported                        : YES  ← enable_ept=1的前提
vmx-info:     [20] EPT A/D bits                          : YES  ← enable_ept_ad_bits=1的前提
vmx-info:     [25] PML                                   : YES  ← enable_pml=1
vmx-info:     [26] VPID supported                        : YES  ← enable_vpid=1的前提
```

**学习重点**: 对比这些MSR输出和 `vmx_hardware_setup()` 中的检测逻辑:
```c
// arch/x86/kvm/vmx/vmx.c:8430
if (!cpu_has_vmx_vpid() || !cpu_has_vmx_invvpid() || ...)
    enable_vpid = 0;

if (!cpu_has_vmx_ept() || !cpu_has_vmx_ept_4levels() || ...)
    enable_ept = 0;
```

### 步骤2: 完整VMX操作演示 (需要卸载KVM)

```bash
# ★ 先卸载KVM (否则VMXON会失败)
sudo rmmod kvm_intel 2>/dev/null
sudo rmmod kvm 2>/dev/null

# 编译 (在主目录)
cd /root/code/kvm-study/examples/minimal-vmx/
make

# 加载
sudo insmod vmx-demo.ko
sudo dmesg | tail -50

# 卸载
sudo rmmod vmx_demo

# ★ 恢复KVM
sudo modprobe kvm_intel
```

**预期输出** (示例):
```
vmx-demo: ============================================
vmx-demo: Minimal VMX Demo
vmx-demo: ============================================
vmx-demo: ✓ CPU支持VMX
vmx-demo:   VMCS Revision ID = 1
vmx-demo: ✓ 启用CR4.VMXE
vmx-demo: 分配 VMXON: virt=... phys=...
vmx-demo: 分配 VMCS: virt=... phys=...
vmx-demo: --- 执行 VMXON ---
vmx-demo: ✓ VMXON 成功 - 现在在VMX Root模式
vmx-demo: --- VMLAUNCH ---
vmx-demo: ✗ VMLAUNCH 失败! (预期 - 没有配置有效Guest代码)
vmx-demo: --- VMXOFF ---
vmx-demo: ✓ VMXOFF 完成
```

## 与KVM源码的对应关系

| 本演示操作 | KVM源码位置 | 说明 |
|-----------|------------|------|
| VMXON | `vmx_enable_virtualization_cpu()` | 每CPU启用VMX |
| VMCLEAR + VMPTRLD | `vmx_vcpu_create()` | 为每个vCPU创建VMCS |
| 设置Guest State | `vmx_vcpu_reset()` | 初始化Guest寄存器 |
| 设置Host State | `vmx_vcpu_run()` 中 | VM-Exit后恢复Host |
| 设置执行控制 | `setup_vmcs_config()` | 配置VM-Exit条件 |
| VMLAUNCH | `__vmx_vcpu_run()` [vmenter.S] | 汇编实现的VM-Entry |
| VM-Exit处理 | `vmx_handle_exit()` | Exit原因分发 |

## 安全提示

- `vmx-info.c`: **完全安全**，只读MSR
- `vmx-demo.c`: 有风险，但最坏情况是内核oops (重启恢复)
- 运行 `vmx-demo` 前必须卸载KVM (否则VMXON冲突)
- 建议在虚拟机或测试机上运行

## 深入学习

完成后尝试:
1. 对比 `vmx-demo.c` 和 `vmx_vcpu_run()` 的配置差异
2. 给 `vmx-demo.c` 添加EPT配置 (设置EPT_POINTER字段)
3. 实现一个真正的Guest代码页 (分配物理页，设置VMCS中的GUEST_RIP指向它)
4. 处理第一个成功的VM-Exit (比如CPUID指令触发Exit)
