# Phase 1 实战练习 - VT-x 基础

> 通过 QEMU 虚拟机动手实践 VT-x 核心概念

---

## 🎯 练习目标

完成以下练习后，你应该能够：
1. ✅ 验证 VMX 硬件支持
2. ✅ 观察 CPUID Faulting 机制
3. ✅ 理解 MSR Bitmap 工作原理
4. ✅ 测试嵌套虚拟化
5. ✅ 测量 VM-Exit 开销

---

## 📋 环境准备

### 启动测试 VM

```bash
# 宿主机上执行
cd /root/code/kvm-study/scripts/vm
./boot-vm.sh minimal
```

> `boot-vm.sh` 默认传 `-enable-kvm -cpu host`。本 phase 的实验要在 guest 内看到 VMX，
> 还需要宿主开启嵌套虚拟化（`cat /sys/module/kvm_intel/parameters/nested` 应为 `Y`），
> 脚本启动前会自检并在未开启时告警。

### 验证环境

```bash
# VM 内执行
uname -r                    # 应该显示 6.12.93-kvm-study
cat /proc/cpuinfo | grep vmx # 应该看到 vmx 标志
ls /mnt/shared/              # 应该看到测试程序
```

---

## 🔬 练习 1: 验证 VMX 支持

### 目标
理解 CPU 如何报告虚拟化支持

### 步骤

#### 1.1 检查 CPUID 虚拟化标志

```bash
# 查看 CPU 特性
cat /proc/cpuinfo | grep -E "vmx|svm|ept|vpid"
```

**预期输出**:
```
flags: ... vmx ept vpid ...
```

#### 1.2 使用 cpuid 指令详细查看

```bash
# 安装 cpuid 工具（如果可用）
# 或者使用我们的测试程序

# 查看 CPUID leaf 1 (特性标志)
/mnt/shared/test-cpuid-fault
```

#### 1.3 读取 VMX 能力 MSR

```bash
# 创建测试程序
cat > /tmp/vmx-capabilities.c << 'EOF'
#include <stdio.h>

int main() {
    unsigned int eax, ebx, ecx, edx;
    
    // CPUID leaf 1: 检查 VMX 支持
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    
    if (ecx & (1 << 5)) {
        printf("✓ CPU 支持 VMX (VT-x)\n");
    } else {
        printf("✗ CPU 不支持 VMX\n");
        return 1;
    }
    
    // 读取 IA32_VMX_BASIC MSR (0x480)
    unsigned long long vmx_basic;
    asm volatile("rdmsr" : "=A"(vmx_basic) : "c"(0x480));
    
    printf("\nIA32_VMX_BASIC MSR (0x480):\n");
    printf("  VMCS 修订版: %llu\n", vmx_basic & 0x7FFFFFFF);
    printf("  VMCS 大小: %llu 字节\n", (vmx_basic >> 32) & 0x1FFF);
    
    // 读取 IA32_VMX_EPT_VPID_CAP MSR (0x48C)
    unsigned long long ept_vpid;
    asm volatile("rdmsr" : "=A"(ept_vpid) : "c"(0x48C));
    
    printf("\nIA32_VMX_EPT_VPID_CAP MSR (0x48C):\n");
    printf("  EPT 支持: %s\n", (ept_vpid & 1) ? "是" : "否");
    printf("  VPID 支持: %s\n", ((ept_vpid >> 26) & 1) ? "是" : "否");
    printf("  EPT 大页: %s\n", ((ept_vpid >> 6) & 1) ? "是" : "否");
    
    return 0;
}
EOF

gcc -o /tmp/vmx-capabilities /tmp/vmx-capabilities.c
/tmp/vmx-capabilities
```

**思考题**:
- VMCS 的作用是什么？
- 为什么需要 EPT 和 VPID？

---

## 🔬 练习 2: CPUID Faulting 机制

### 目标
理解 CPUID Faulting 如何阻止用户态程序探测 CPU

### 步骤

#### 2.1 测试 CPUID Faulting 基本行为

```bash
# 运行完整的 CPUID Faulting 测试
/mnt/shared/test-cpuid-fault
```

**预期输出**:
```
=== CPUID Faulting 测试 ===

1. 检测 CPUID Faulting 支持
   ✓ CPU 支持 CPUID Faulting

2. 测试 CPUID（未启用 Faulting）
   ✓ CPUID 成功执行
   CPU 厂商: Intel(R) ...

3. 启用 CPUID Faulting
   调用 arch_prctl(ARCH_SET_CPUID, 0)
   ✓ 启用成功

4. 测试 CPUID（已启用 Faulting）
   ✗ CPUID 触发信号 11 (SIGSEGV)
   ✓ 这说明 CPUID Faulting 生效了！

5. 禁用 CPUID Faulting
   调用 arch_prctl(ARCH_SET_CPUID, 1)
   ✓ 禁用成功

6. 再次测试 CPUID
   ✓ CPUID 恢复正常
```

#### 2.2 手动验证 CPUID Faulting

```bash
# 创建简单的测试程序
cat > /tmp/test-cpuid.c << 'EOF'
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef ARCH_GET_CPUID
#define ARCH_GET_CPUID  0x1011
#endif
#ifndef ARCH_SET_CPUID
#define ARCH_SET_CPUID  0x1012
#endif

static sigjmp_buf jmpbuf;

void handler(int sig) {
    printf("捕获到信号 %d (CPUID 被阻止)\n", sig);
    siglongjmp(jmpbuf, 1);
}

int main() {
    unsigned int eax, ebx, ecx, edx;
    
    // 测试 1: 正常执行 CPUID
    printf("测试 1: 正常执行 CPUID\n");
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    printf("  EAX=0x%x (最大 CPUID leaf)\n", eax);
    
    // 测试 2: 启用 CPUID Faulting
    printf("\n测试 2: 启用 CPUID Faulting\n");
    long ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);
    if (ret == 0) {
        printf("  ✓ CPUID Faulting 已启用\n");
    }
    
    // 测试 3: 尝试执行 CPUID（应该触发 #GP）
    printf("\n测试 3: 尝试执行 CPUID（应该失败）\n");
    signal(SIGSEGV, handler);
    
    if (sigsetjmp(jmpbuf, 1) == 0) {
        asm volatile("cpuid" : "=a"(eax) : "a"(0));
        printf("  ✗ CPUID 执行成功（不应该发生）\n");
    } else {
        printf("  ✓ CPUID 被阻止（符合预期）\n");
    }
    
    // 测试 4: 禁用 CPUID Faulting
    printf("\n测试 4: 禁用 CPUID Faulting\n");
    syscall(SYS_arch_prctl, ARCH_SET_CPUID, 1);
    printf("  ✓ CPUID Faulting 已禁用\n");
    
    // 测试 5: 再次执行 CPUID（应该成功）
    printf("\n测试 5: 再次执行 CPUID\n");
    asm volatile("cpuid" : "=a"(eax) : "a"(0));
    printf("  ✓ CPUID 执行成功，EAX=0x%x\n", eax);
    
    return 0;
}
EOF

gcc -o /tmp/test-cpuid /tmp/test-cpuid.c
/tmp/test-cpuid
```

**思考题**:
- CPUID Faulting 在什么场景下有用？
- 为什么 Ring 0 不受 CPUID Faulting 影响？

---

## 🔬 练习 3: MSR Bitmap 和 VM-Exit

### 目标
理解 MSR 访问如何触发 VM-Exit

### 步骤

#### 3.1 观察 MSR 访问

```bash
# 创建 MSR 测试程序
cat > /tmp/test-msr.c << 'EOF'
#include <stdio.h>

int main() {
    unsigned long long value;
    
    printf("=== MSR 访问测试 ===\n\n");
    
    // 读取 IA32_TSC (0x10) - 通常透传
    printf("1. 读取 IA32_TSC (0x10)\n");
    asm volatile("rdmsr" : "=A"(value) : "c"(0x10));
    printf("   TSC = %llu\n", value);
    printf("   说明: 这个 MSR 通常被透传，无 VM-Exit\n\n");
    
    // 读取 IA32_EFER (0xC0000080) - 通常拦截
    printf("2. 读取 IA32_EFER (0xC0000080)\n");
    asm volatile("rdmsr" : "=A"(value) : "c"(0xC0000080));
    printf("   EFER = 0x%llx\n", value);
    printf("   说明: 这个 MSR 通常被拦截，触发 VM-Exit\n\n");
    
    // 读取 IA32_APIC_BASE (0x1B) - 通常拦截
    printf("3. 读取 IA32_APIC_BASE (0x1B)\n");
    asm volatile("rdmsr" : "=A"(value) : "c"(0x1B));
    printf("   APIC_BASE = 0x%llx\n", value);
    printf("   说明: 这个 MSR 通常被拦截\n\n");
    
    return 0;
}
EOF

gcc -o /tmp/test-msr /tmp/test-msr.c
/tmp/test-msr
```

#### 3.2 测量 MSR 访问时间

```bash
# 创建性能测试程序
cat > /tmp/msr-perf.c << 'EOF'
#include <stdio.h>
#include <time.h>

#define ITERATIONS 1000000

int main() {
    unsigned long long value;
    struct timespec start, end;
    
    printf("=== MSR 访问性能测试 ===\n\n");
    printf("测试 %d 次 MSR 读取\n\n", ITERATIONS);
    
    // 测试 IA32_TSC（透传）
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        asm volatile("rdmsr" : "=A"(value) : "c"(0x10));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double tsc_time = (end.tv_sec - start.tv_sec) * 1000000000.0 + 
                      (end.tv_nsec - start.tv_nsec);
    printf("IA32_TSC (透传): %.2f ns/次\n", tsc_time / ITERATIONS);
    
    // 测试 IA32_EFER（拦截）
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        asm volatile("rdmsr" : "=A"(value) : "c"(0xC0000080));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double efer_time = (end.tv_sec - start.tv_sec) * 1000000000.0 + 
                       (end.tv_nsec - start.tv_nsec);
    printf("IA32_EFER (拦截): %.2f ns/次\n", efer_time / ITERATIONS);
    
    printf("\n开销差异: %.2fx\n", efer_time / tsc_time);
    
    return 0;
}
EOF

gcc -O2 -o /tmp/msr-perf /tmp/msr-perf.c
/tmp/msr-perf
```

**思考题**:
- 为什么透传的 MSR 比拦截的 MSR 快？
- VM-Exit 的开销主要来自哪里？

---

## 🔬 练习 4: 嵌套虚拟化

### 目标
在 VM 内运行 KVM（嵌套虚拟化）

### 步骤

#### 4.1 检查嵌套虚拟化支持

```bash
# 检查 VM 是否支持嵌套虚拟化
cat /sys/module/kvm_intel/parameters/nested
# 应该输出 Y 或 1
```

#### 4.2 加载 KVM 模块

```bash
# 加载 KVM 模块
modprobe kvm
modprobe kvm_intel

# 检查设备
ls -l /dev/kvm
```

#### 4.3 运行嵌套 VM（可选）

```bash
# 如果有第二个内核镜像和 initramfs
# 可以在 VM 内启动另一个 VM

# 这需要：
# 1. 在 VM 内安装 QEMU
# 2. 复制内核和 initramfs 到 VM
# 3. 启动嵌套 VM

# 简化版本：只验证 KVM 模块可以加载
dmesg | grep kvm
```

**思考题**:
- 嵌套虚拟化的性能开销主要来自哪里？
- 什么场景需要嵌套虚拟化？

---

## 🔬 练习 5: VM-Exit 开销测量

### 目标
量化 VM-Exit 的性能开销

### 步骤

#### 5.1 使用 perf 工具

```bash
# 安装 perf（如果可用）
# apt install linux-tools-generic

# 如果 perf 不可用，使用手动计时
cat > /tmp/vmexit-overhead.c << 'EOF'
#include <stdio.h>
#include <time.h>

#define ITERATIONS 100000

int main() {
    struct timespec start, end;
    unsigned long long dummy;
    
    printf("=== VM-Exit 开销测量 ===\n\n");
    printf("测试 %d 次操作\n\n", ITERATIONS);
    
    // 测试 1: CPUID（触发 VM-Exit）
    printf("1. CPUID 指令（触发 VM-Exit）\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        asm volatile("cpuid" : "=a"(dummy) : "a"(0) : "ebx", "ecx", "edx");
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double cpuid_time = (end.tv_sec - start.tv_sec) * 1000000000.0 + 
                        (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", cpuid_time / 1000000);
    printf("   平均: %.2f ns/次\n\n", cpuid_time / ITERATIONS);
    
    // 测试 2: RDTSC（不触发 VM-Exit）
    printf("2. RDTSC 指令（不触发 VM-Exit）\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        asm volatile("rdtsc" : "=A"(dummy));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double rdtsc_time = (end.tv_sec - start.tv_sec) * 1000000000.0 + 
                        (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", rdtsc_time / 1000000);
    printf("   平均: %.2f ns/次\n\n", rdtsc_time / ITERATIONS);
    
    // 测试 3: MOV 指令（基线）
    printf("3. MOV 指令（基线）\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        asm volatile("mov %0, %%rax" : : "i"(0x12345678) : "rax");
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double mov_time = (end.tv_sec - start.tv_sec) * 1000000000.0 + 
                      (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", mov_time / 1000000);
    printf("   平均: %.2f ns/次\n\n", mov_time / ITERATIONS);
    
    printf("=== 分析 ===\n");
    printf("VM-Exit 开销: %.2f ns\n", (cpuid_time - rdtsc_time) / ITERATIONS);
    printf("CPUID vs MOV: %.2fx\n", cpuid_time / mov_time);
    
    return 0;
}
EOF

gcc -O2 -o /tmp/vmexit-overhead /tmp/vmexit-overhead.c
/tmp/vmexit-overhead
```

**思考题**:
- CPUID 的开销主要来自哪里？
- 如何减少 VM-Exit 开销？

---

## 📊 实验报告模板

完成练习后，填写以下报告：

```markdown
# Phase 1 实验报告

## 练习 1: VMX 支持验证
- CPU 型号: _______________
- VMX 支持: ✓/✗
- EPT 支持: ✓/✗
- VPID 支持: ✓/✗

## 练习 2: CPUID Faulting
- CPUID Faulting 支持: ✓/✗
- 启用前 CPUID: 成功/失败
- 启用后 CPUID: 成功/失败
- 禁用后 CPUID: 成功/失败

## 练习 3: MSR Bitmap
- IA32_TSC 访问时间: _____ ns
- IA32_EFER 访问时间: _____ ns
- 开销差异: _____ x

## 练习 4: 嵌套虚拟化
- KVM 模块加载: 成功/失败
- /dev/kvm 设备: 存在/不存在

## 练习 5: VM-Exit 开销
- CPUID 开销: _____ ns
- RDTSC 开销: _____ ns
- MOV 基线: _____ ns

## 总结
- 学到的关键概念: _______________
- 遇到的问题: _______________
- 解决方案: _______________
```

---

## 🎓 参考答案

### 练习 1 思考题

**Q: VMCS 的作用是什么？**  
A: VMCS (Virtual Machine Control Structure) 是 VMX 的核心数据结构，存储：
- Guest 状态（寄存器、段选择子等）
- Host 状态（VM-Exit 后恢复）
- VM-Execution 控制（哪些事件触发 VM-Exit）
- VM-Exit 信息（退出原因、质量等）

**Q: 为什么需要 EPT 和 VPID？**  
A: 
- EPT (Extended Page Table): 实现 Guest 物理地址到 Host 物理地址的转换，避免软件影子页表的开销
- VPID (Virtual Processor ID): 标记 TLB 条目属于哪个 vCPU，避免 VM-Entry/Exit 时刷新 TLB

### 练习 2 思考题

**Q: CPUID Faulting 在什么场景下有用？**  
A: 
- 安全场景：防止用户态程序探测 CPU 特性（侧信道攻击）
- 虚拟化场景：在嵌套虚拟化中控制 Guest 看到的 CPU 特性
- 兼容性场景：模拟不同 CPU 型号

**Q: 为什么 Ring 0 不受 CPUID Faulting 影响？**  
A: CPUID Faulting 只影响 Ring 3 (CPL=3) 的 CPUID 执行。Ring 0 (CPL=0) 的 CPUID 仍然会触发 VM-Exit（在 VMX 中），但不会触发 #GP。

### 练习 3 思考题

**Q: 为什么透传的 MSR 比拦截的 MSR 快？**  
A: 
- 透传：直接读取物理 MSR，无 VM-Exit，~50-100 ns
- 拦截：触发 VM-Exit → KVM 处理 → VM-Entry，~1000-2000 ns
- 开销差异主要来自 VM-Exit/Entry 的上下文切换

**Q: VM-Exit 的开销主要来自哪里？**  
A: 
- 保存/恢复 CPU 状态（寄存器、段选择子等）
- 切换 VMCS（Guest/Host 状态）
- KVM 处理逻辑
- VM-Entry 重新加载状态

### 练习 5 思考题

**Q: CPUID 的开销主要来自哪里？**  
A: 
- VM-Exit 触发和处理
- KVM 模拟 CPUID 返回值
- VM-Entry 恢复执行

**Q: 如何减少 VM-Exit 开销？**  
A: 
- 使用 MSR Bitmap 透传不敏感的 MSR
- 使用 CPUID Faulting 减少 CPUID VM-Exit
- 使用 VPID 避免 TLB 刷新
- 使用 Posted Interrupts 减少中断 VM-Exit
- 使用 APICv 虚拟化中断控制器

---

## 🚀 下一步

完成这些练习后，你可以：
1. 进入 Phase 2: 内存虚拟化（EPT）
2. 进入 Phase 4: 中断虚拟化（APICv）
3. 深入源码阅读 KVM 实现

---

**提示**: 如果在练习中遇到问题，可以：
- 查看 VM 的 dmesg 日志
- 使用 strace 跟踪系统调用
- 阅读 KVM 源码（arch/x86/kvm/）
- 参考 Intel SDM Volume 3 (VMX)
