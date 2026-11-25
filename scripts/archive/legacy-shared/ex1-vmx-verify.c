/*
 * 练习 1: 验证 VMX 支持
 *
 * 目标: 读取 CPU 虚拟化能力 MSR
 * 注意: 使用 /dev/cpu/0/msr 读取 MSR
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

// 读取 MSR
int read_msr(int fd, unsigned int msr, unsigned long long *value) {
    off_t offset = msr;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        return -1;
    }
    if (read(fd, value, 8) != 8) {
        return -1;
    }
    return 0;
}

int main() {
    unsigned int eax, ebx, ecx, edx;

    printf("========================================\n");
    printf("  练习 1: VMX 支持验证\n");
    printf("========================================\n\n");

    // CPUID leaf 1: 检查 VMX 支持
    printf("1. 检查 CPUID leaf 1 (特性标志)\n");
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));

    if (ecx & (1 << 5)) {
        printf("   ✓ CPU 支持 VMX (VT-x)\n");
    } else {
        printf("   ✗ CPU 不支持 VMX\n");
        printf("   无法继续练习\n");
        return 1;
    }

    // 检查 EPT 支持
    if (ecx & (1 << 7)) {
        printf("   ✓ 支持 Flexpriority (TPR Shadow)\n");
    }

    printf("\n");

    // 打开 MSR 设备
    int msr_fd = open("/dev/cpu/0/msr", O_RDONLY);
    if (msr_fd < 0) {
        printf("2. 读取 IA32_VMX_BASIC MSR (0x480)\n");
        printf("   ✗ 无法打开 /dev/cpu/0/msr\n");
        printf("   提示: 需要加载 msr 模块: modprobe msr\n");
        printf("\n");
        printf("========================================\n");
        printf("  ✓ VMX 验证完成 (部分)\n");
        printf("========================================\n");
        return 0;
    }

    // 读取 IA32_VMX_BASIC MSR (0x480)
    printf("2. 读取 IA32_VMX_BASIC MSR (0x480)\n");
    unsigned long long vmx_basic;
    if (read_msr(msr_fd, 0x480, &vmx_basic) == 0) {
        unsigned int revision = vmx_basic & 0x7FFFFFFF;
        unsigned int size = (vmx_basic >> 32) & 0x1FFF;
        unsigned int true_controls = (vmx_basic >> 55) & 1;

        printf("   VMCS 修订版: %u\n", revision);
        printf("   VMCS 大小: %u 字节\n", size);
        printf("   True Controls: %s\n", true_controls ? "支持" : "不支持");
    } else {
        printf("   ✗ 读取失败\n");
    }
    printf("\n");

    // 读取 IA32_VMX_EPT_VPID_CAP MSR (0x48C)
    printf("3. 读取 IA32_VMX_EPT_VPID_CAP MSR (0x48C)\n");
    unsigned long long ept_vpid;
    if (read_msr(msr_fd, 0x48C, &ept_vpid) == 0) {
        printf("   EPT 支持: %s\n", (ept_vpid & 1) ? "是" : "否");
        printf("   EPT 仅 4 级页表: %s\n", ((ept_vpid >> 6) & 1) ? "是" : "否");
        printf("   EPT 2MB 大页: %s\n", ((ept_vpid >> 16) & 1) ? "是" : "否");
        printf("   EPT 1GB 大页: %s\n", ((ept_vpid >> 17) & 1) ? "是" : "否");
        printf("   INVEPT 支持: %s\n", ((ept_vpid >> 20) & 1) ? "是" : "否");
        printf("   VPID 支持: %s\n", ((ept_vpid >> 26) & 1) ? "是" : "否");
        printf("   INVVPID 支持: %s\n", ((ept_vpid >> 32) & 1) ? "是" : "否");
    } else {
        printf("   ✗ 读取失败\n");
    }
    printf("\n");

    // 读取 IA32_VMX_PROCBASED_CTLS MSR (0x482)
    printf("4. 读取 IA32_VMX_PROCBASED_CTLS MSR (0x482)\n");
    unsigned long long proc_ctls;
    if (read_msr(msr_fd, 0x482, &proc_ctls) == 0) {
        unsigned int allowed0 = proc_ctls & 0xFFFFFFFF;
        unsigned int allowed1 = (proc_ctls >> 32) & 0xFFFFFFFF;

        printf("   必须启用的控制 (allowed-0): 0x%08x\n", allowed0);
        printf("   可以启用的控制 (allowed-1): 0x%08x\n", allowed1);
        printf("   HLT exiting: %s\n", (allowed1 & (1 << 7)) ? "支持" : "不支持");
        printf("   MSR bitmap: %s\n", (allowed1 & (1 << 28)) ? "支持" : "不支持");
    } else {
        printf("   ✗ 读取失败\n");
    }
    printf("\n");

    close(msr_fd);

    printf("========================================\n");
    printf("  ✓ VMX 验证完成\n");
    printf("========================================\n");

    return 0;
}
