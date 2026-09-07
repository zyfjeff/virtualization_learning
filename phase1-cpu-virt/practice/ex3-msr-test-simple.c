/*
 * 练习 3: MSR 访问测试 (简化版)
 *
 * 目标: 使用 rdmsr 指令直接测量 MSR 访问开销
 * 注意: rdmsr 会触发 VM-Exit（如果被拦截）
 */

#include <stdio.h>
#include <time.h>

#define ITERATIONS 1000000

// 读取 MSR (使用 rdmsr 指令)
static inline unsigned long long read_msr_direct(unsigned int msr) {
    unsigned int low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((unsigned long long)high << 32) | low;
}

int main() {
    struct timespec start, end;
    unsigned long long value;

    printf("========================================\n");
    printf("  练习 3: MSR 访问测试 (简化版)\n");
    printf("========================================\n\n");

    printf("测试 %d 次 MSR 读取\n\n", ITERATIONS);

    // 测试 1: IA32_TSC (0x10) - 通常透传
    printf("1. IA32_TSC (MSR 0x10) - 通常透传\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        value = read_msr_direct(0x10);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double tsc_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                      (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", tsc_time / 1000000);
    printf("   平均: %.2f ns/次\n", tsc_time / ITERATIONS);
    printf("   最后一次读取值: 0x%llx\n", value);
    printf("   说明: TSC 通常被透传，无 VM-Exit\n\n");

    // 测试 2: IA32_EFER (0xC0000080) - 通常拦截
    printf("2. IA32_EFER (MSR 0xC0000080) - 通常拦截\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        value = read_msr_direct(0xC0000080);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double efer_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                       (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", efer_time / 1000000);
    printf("   平均: %.2f ns/次\n", efer_time / ITERATIONS);
    printf("   最后一次读取值: 0x%llx\n", value);
    printf("   说明: EFER 通常被拦截，触发 VM-Exit\n\n");

    // 测试 3: IA32_APIC_BASE (0x1B) - 通常拦截
    printf("3. IA32_APIC_BASE (MSR 0x1B) - 通常拦截\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        value = read_msr_direct(0x1B);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double apic_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                       (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", apic_time / 1000000);
    printf("   平均: %.2f ns/次\n", apic_time / ITERATIONS);
    printf("   最后一次读取值: 0x%llx\n", value);
    printf("   说明: APIC_BASE 通常被拦截\n\n");

    printf("========================================\n");
    printf("  分析\n");
    printf("========================================\n");
    printf("IA32_TSC (透传):  %.2f ns\n", tsc_time / ITERATIONS);
    printf("IA32_EFER (拦截): %.2f ns\n", efer_time / ITERATIONS);
    printf("IA32_APIC (拦截): %.2f ns\n", apic_time / ITERATIONS);
    printf("\n");
    printf("开销差异:\n");
    if (tsc_time > 0) {
        printf("  EFER vs TSC: %.2fx\n", efer_time / tsc_time);
        printf("  APIC vs TSC: %.2fx\n", apic_time / tsc_time);
    }
    printf("\n");

    printf("========================================\n");
    printf("  ✓ MSR 测试完成\n");
    printf("========================================\n");

    return 0;
}
