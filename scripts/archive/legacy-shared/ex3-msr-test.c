/*
 * 练习 3: MSR 访问测试
 *
 * 目标: 观察 MSR Bitmap 的作用，测量不同 MSR 的访问时间
 * 注意: 使用 /dev/cpu/0/msr 读取 MSR
 */

#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#define ITERATIONS 100000

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
    unsigned long long value;
    struct timespec start, end;

    printf("========================================\n");
    printf("  练习 3: MSR 访问测试\n");
    printf("========================================\n\n");

    // 打开 MSR 设备
    int msr_fd = open("/dev/cpu/0/msr", O_RDONLY);
    if (msr_fd < 0) {
        printf("✗ 无法打开 /dev/cpu/0/msr\n");
        printf("  提示: 需要加载 msr 模块: modprobe msr\n");
        return 1;
    }

    printf("测试 %d 次 MSR 读取\n\n", ITERATIONS);

    // 测试 1: IA32_TSC (0x10) - 通常透传
    printf("1. IA32_TSC (MSR 0x10) - 通常透传\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        read_msr(msr_fd, 0x10, &value);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double tsc_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                      (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", tsc_time / 1000000);
    printf("   平均: %.2f ns/次\n", tsc_time / ITERATIONS);
    printf("   说明: TSC 通常被透传，无 VM-Exit\n\n");

    // 测试 2: IA32_EFER (0xC0000080) - 通常拦截
    printf("2. IA32_EFER (MSR 0xC0000080) - 通常拦截\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        read_msr(msr_fd, 0xC0000080, &value);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double efer_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                       (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", efer_time / 1000000);
    printf("   平均: %.2f ns/次\n", efer_time / ITERATIONS);
    printf("   说明: EFER 通常被拦截，触发 VM-Exit\n\n");

    // 测试 3: IA32_APIC_BASE (0x1B) - 通常拦截
    printf("3. IA32_APIC_BASE (MSR 0x1B) - 通常拦截\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        read_msr(msr_fd, 0x1B, &value);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double apic_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                       (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", apic_time / 1000000);
    printf("   平均: %.2f ns/次\n", apic_time / ITERATIONS);
    printf("   说明: APIC_BASE 通常被拦截\n\n");

    close(msr_fd);

    printf("========================================\n");
    printf("  分析\n");
    printf("========================================\n");
    printf("IA32_TSC (透传):  %.2f ns\n", tsc_time / ITERATIONS);
    printf("IA32_EFER (拦截): %.2f ns\n", efer_time / ITERATIONS);
    printf("IA32_APIC (拦截): %.2f ns\n", apic_time / ITERATIONS);
    printf("\n");
    printf("开销差异:\n");
    printf("  EFER vs TSC: %.2fx\n", efer_time / tsc_time);
    printf("  APIC vs TSC: %.2fx\n", apic_time / tsc_time);
    printf("\n");

    printf("========================================\n");
    printf("  ✓ MSR 测试完成\n");
    printf("========================================\n");

    return 0;
}
