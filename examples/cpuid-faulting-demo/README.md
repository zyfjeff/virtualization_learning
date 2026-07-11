# CPUID Faulting 示例项目

> 演示 Linux CPUID Faulting 特性的检测、启用和使用
>
> 基于 Linux 6.12.93 内核

---

## 🎯 项目目标

通过实际代码演示：

1. **检测 CPUID Faulting 支持** - 如何检查 CPU 是否支持该特性
2. **控制 CPUID Faulting** - 使用 arch_prctl 启用/禁用
3. **观察效果** - Ring 3 CPUID 触发 #GP 异常
4. **KVM 集成** - 在虚拟机中测试 CPUID Faulting

对应课程内容：Phase 1 - CPUID 虚拟化 - CPUID Faulting 机制

---

## 📚 背景知识

### CPUID Faulting 是什么？

CPUID Faulting 是 Intel CPU 的一个安全特性：

- **启用前**：Ring 3（用户态）可以执行 CPUID 指令，读取 CPU 信息
- **启用后**：Ring 3 执行 CPUID 会触发 #GP（General Protection Fault）异常
- **Ring 0**（内核态）不受影响，始终可以执行 CPUID

### 控制接口

```c
// 查询当前状态
long ret = syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0);
// 返回 1: CPUID 可用
// 返回 0: CPUID 被禁止

// 启用 CPUID Faulting（禁止用户态 CPUID）
syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);

// 禁用 CPUID Faulting（允许用户态 CPUID）
syscall(SYS_arch_prctl, ARCH_SET_CPUID, 1);
```

### 底层机制

```
用户态调用 arch_prctl(ARCH_SET_CPUID, 0)
  ↓
内核设置当前线程的 TIF_NOCPUID 标志
  ↓
写入 MSR_MISC_FEATURES_ENABLES (0x161) bit 0 = 1
  ↓
硬件启用 CPUID Faulting
  ↓
Ring 3 CPUID → #GP 异常
```

---

## 🚀 快速开始

### 1. 编译

```bash
make
```

### 2. 运行用户态测试

```bash
./test-cpuid-fault
```

**预期输出：**

```
=== CPUID Faulting 测试 ===

1. 检测 CPUID Faulting 支持
   ✓ CPU 支持 CPUID Faulting

2. 测试 CPUID（未启用 Faulting）
   ✓ CPUID 成功执行
   CPU 厂商: GenuineIntel
   CPU 型号: Intel(R) Core(TM) i7-...

3. 启用 CPUID Faulting
   调用 arch_prctl(ARCH_SET_CPUID, 0)
   ✓ 启用成功

4. 测试 CPUID（已启用 Faulting）
   ✗ CPUID 触发 SIGSEGV（预期行为）
   这说明 CPUID Faulting 生效了！

5. 禁用 CPUID Faulting
   调用 arch_prctl(ARCH_SET_CPUID, 1)
   ✓ 禁用成功

6. 再次测试 CPUID
   ✓ CPUID 恢复正常
   CPU 厂商: GenuineIntel

=== 测试完成 ===
```

### 3. 在 KVM 虚拟机中测试（可选）

```bash
# 启动一个测试虚拟机
qemu-system-x86_64 -enable-kvm -kernel /boot/vmlinuz-$(uname -r) \
    -append "root=/dev/sda1 console=ttyS0" -nographic

# 在虚拟机内编译并运行
scp test-cpuid-fault root@vm-ip:/root/
ssh root@vm-ip
./test-cpuid-fault
```

---

## 📖 文件说明

| 文件 | 说明 |
|------|------|
| `test-cpuid-fault.c` | 用户态测试程序，演示检测和启用 CPUID Faulting |
| `test-cpuid-fault-kvm.c` | KVM 相关测试，在虚拟机中验证 CPUID Faulting |
| `Makefile` | 编译脚本 |
| `README.md` | 本文档 |

---

## 🔍 代码解析

### 检测 CPUID Faulting 支持

```c
// 方法1: 通过 /proc/cpuinfo
if (system("grep -q cpuid_fault /proc/cpuinfo") == 0) {
    printf("✓ CPU 支持 CPUID Faulting\n");
}

// 方法2: 通过 CPUID leaf 7
unsigned int eax, ebx, ecx, edx;
__asm__ __volatile__(
    "cpuid"
    : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
    : "a"(7), "c"(0)
);
if (ebx & (1 << 31)) {
    printf("✓ CPU 支持 CPUID Faulting (CPUID leaf 7)\n");
}

// 方法3: 通过 arch_prctl
long ret = syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0);
if (ret >= 0) {
    printf("✓ CPU 支持 CPUID Faulting (arch_prctl 可用)\n");
}
```

### 测试 CPUID 执行

```c
// 设置信号处理器捕获 SIGSEGV
signal(SIGSEGV, sigsegv_handler);

// 尝试执行 CPUID
unsigned int eax, ebx, ecx, edx;
if (sigsetjmp(jmpbuf, 1) == 0) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0)
    );
    printf("✓ CPUID 成功执行\n");
} else {
    printf("✗ CPUID 触发 SIGSEGV\n");
}
```

### 启用/禁用 CPUID Faulting

```c
// 启用（禁止用户态 CPUID）
long ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);
if (ret == 0) {
    printf("✓ 启用 CPUID Faulting 成功\n");
}

// 禁用（允许用户态 CPUID）
ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 1);
if (ret == 0) {
    printf("✓ 禁用 CPUID Faulting 成功\n");
}
```

---

## 🎓 学习要点

### 1. CPUID Faulting 是 per-thread 的

```c
// 主线程启用 CPUID Faulting
syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);

// fork 子进程
if (fork() == 0) {
    // 子进程继承父进程的设置
    // CPUID 仍然被禁止
}

// 创建新线程
pthread_create(&thread, NULL, thread_func, NULL);
// 新线程也会继承设置
```

### 2. 与 KVM 虚拟化的关系

在 KVM 虚拟机中：

- **Host**：CPUID Faulting 由 Guest 内核控制
- **Guest**：可以独立启用 CPUID Faulting
- **CPUID 指令**：在 VMX 中总是触发 VM-Exit，KVM 模拟返回虚拟化后的值

```
Guest Ring 3 CPUID
  ↓
VM-Exit (EXIT_REASON_CPUID)
  ↓
KVM 检查: CPUID Faulting 启用?
  ↓
是 → 不处理，返回 1 → Guest 收到 #GP
否 → 模拟 CPUID，返回虚拟化值 → Guest 正常执行
```

### 3. 性能影响

- **未启用**：每次 Ring 3 CPUID → VM-Exit → KVM 处理 → VM-Entry
- **启用后**：Ring 3 CPUID → #GP（无 VM-Exit），性能提升 10-20 倍

---

## 🔧 故障排查

### 问题1: arch_prctl 返回 -ENODEV

```
错误: Function not implemented
原因: CPU 不支持 CPUID Faulting
解决: 检查 /proc/cpuinfo 是否有 cpuid_fault 标志
```

### 问题2: CPUID 没有触发 SIGSEGV

```
可能原因:
1. CPUID Faulting 未成功启用（检查返回值）
2. 信号处理器未正确设置
3. CPU 不支持该特性
```

### 问题3: 虚拟机中无法使用

```
可能原因:
1. QEMU 未暴露 CPUID Faulting 特性
2. Guest 内核版本太旧（需要 4.12+）
解决: 使用 -cpu host 或 -cpu qemu64,cpuid-fault=on
```

---

## 📚 相关文档

- [Phase 1: CPUID 虚拟化](../../phase1-vtx-basics/cpu-virtualization.md#111-cpuid-faulting-机制补充)
- [Intel SDM Vol.3: CPUID Faulting](https://software.intel.com/content/www/us/en/develop/download/intel-64-and-ia-32-architectures-sdm-volume-3.html)
- [Linux Kernel: arch_prctl](https://man7.org/linux/man-pages/man2/arch_prctl.2.html)

---

## 🎉 扩展练习

1. **多进程测试**：fork 后测试父子进程的 CPUID Faulting 是否独立
2. **多线程测试**：不同线程启用/禁用 CPUID Faulting，观察行为
3. **性能测试**：对比启用前后 CPUID 调用的性能差异
4. **KVM 测试**：在虚拟机中测试，验证 CPUID Faulting 的虚拟化

---

**作者**: KVM Study Project  
**许可**: GPL-2.0  
**日期**: 2026-06-29
