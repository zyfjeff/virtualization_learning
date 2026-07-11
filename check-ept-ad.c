/*
 * 检查 CPU 是否支持 EPT A/D 位 (Intel)
 *
 * 通过 CPUID 和 MSR 查询 Intel VT-x 扩展特性
 */

#include <stdio.h>
#include <cpuid.h>

int main() {
    unsigned int eax, ebx, ecx, edx;

    printf("=== CPU EPT A/D 位支持检查 (Intel) ===\n\n");

    // 1. 检查是否支持 VMX (VT-x)
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (ecx & (1 << 5)) {
            printf("✓ CPU 支持 VMX (VT-x)\n");
        } else {
            printf("✗ CPU 不支持 VMX (VT-x)\n");
            return 1;
        }

        // 检查 CPU 特性标志中的 EPT 支持
        // 注意: Linux 内核会在 /proc/cpuinfo 中添加 ept 和 ept_ad 标志
        // 但这些不是直接的 CPUID 位，而是内核解析后的结果
    }

    printf("\n");

    // 2. 读取 IA32_VMX_EPT_VPID_CAP MSR (0x48C) 检查 EPT 能力
    printf("=== 读取 IA32_VMX_EPT_VPID_CAP MSR (0x48C) ===\n");
    unsigned long long ept_vpid_cap;

    // rdmsr 需要 ring 0 权限，用户态程序无法直接读取
    // 但我们可以通过 /dev/cpu/0/msr 读取
    printf("注意: rdmsr 指令需要 ring 0 权限\n");
    printf("用户态程序需要通过 /dev/cpu/0/msr 读取\n\n");

    // 3. 检查 /proc/cpuinfo 中的特性标志
    printf("=== 检查 /proc/cpuinfo 特性标志 ===\n");
    printf("在终端运行: grep -o 'ept_ad' /proc/cpuinfo | head -1\n");
    printf("如果输出 'ept_ad'，说明 CPU 支持 EPT A/D 位\n\n");

    // 4. 检查 KVM 模块参数
    printf("=== 检查 KVM 模块参数 ===\n");
    printf("在终端运行: cat /sys/module/kvm_intel/parameters/eptad\n");
    printf("如果输出 'Y'，说明 KVM 已启用 EPT A/D 位\n\n");

    printf("=== 总结 ===\n");
    printf("检查 EPT A/D 位支持的最佳方法:\n");
    printf("  1. grep -o 'ept_ad' /proc/cpuinfo | head -1\n");
    printf("     → 如果输出 'ept_ad'，CPU 支持\n");
    printf("  2. cat /sys/module/kvm_intel/parameters/eptad\n");
    printf("     → 如果输出 'Y'，KVM 已启用\n");

    return 0;
}
