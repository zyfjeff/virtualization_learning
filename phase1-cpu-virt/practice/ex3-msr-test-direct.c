/*
 * 练习 3: MSR 访问测试（修正版）
 *
 * 目标: 测量透传 vs 拦截 MSR 的真实开销差异
 * 方法: 直接在用户态使用 RDMSR 指令，避免系统调用开销
 */

#include <stdio.h>
#include <time.h>
#include <stdint.h>

#define ITERATIONS 1000000

// 直接执行 RDMSR 指令
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// 测量时间（ns）
static inline uint64_t rdtsc_ns(void)
{
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    uint64_t tsc = ((uint64_t)high << 32) | low;
    // 假设 TSC 频率为 2.5 GHz（需要根据实际情况调整）
    return tsc * 10 / 25;  // 粗略转换为 ns
}

int main()
{
    uint64_t value;
    struct timespec start, end;

    printf("========================================\n");
    printf("  练习 3: MSR 访问测试（用户态 RDMSR）\n");
    printf("========================================\n\n");

    printf("测试 %d 次 RDMSR 指令\n\n", ITERATIONS);

    // 先执行一次，确保 MSR 设备可用（虽然我们不使用它）
    // 这里只是检查是否有权限执行 RDMSR
    // 注意：用户态直接执行 RDMSR 可能需要特殊权限或配置

    // 测试 1: IA32_TSC (0x10) - 透传
    printf("1. IA32_TSC (MSR 0x10) - 透传\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        value = rdmsr(0x10);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double tsc_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                      (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", tsc_time / 1000000);
    printf("   平均: %.2f ns/次\n", tsc_time / ITERATIONS);
    printf("   说明: TSC 被透传，无 VM-Exit\n\n");

    // 测试 2: IA32_EFER (0xC0000080) - 拦截
    printf("2. IA32_EFER (MSR 0xC0000080) - 拦截\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        value = rdmsr(0xC0000080);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double efer_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                       (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", efer_time / 1000000);
    printf("   平均: %.2f ns/次\n", efer_time / ITERATIONS);
    printf("   说明: EFER 被拦截，触发 VM-Exit\n\n");

    // 测试 3: IA32_APIC_BASE (0x1B) - 拦截
    printf("3. IA32_APIC_BASE (MSR 0x1B) - 拦截\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        value = rdmsr(0x1B);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double apic_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                       (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", apic_time / 1000000);
    printf("   平均: %.2f ns/次\n", apic_time / ITERATIONS);
    printf("   说明: APIC_BASE 被拦截\n\n");

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

    printf("预期结果:\n");
    printf("  - 透传 MSR: ~100-300 ns (RDMSR 指令开销)\n");
    printf("  - 拦截 MSR: ~1500-3000 ns (VM-Exit + KVM 处理 + VM-Entry)\n");
    printf("  - 差异: 5x-20x\n");
    printf("\n");

    printf("========================================\n");
    printf("  ✓ MSR 测试完成\n");
    printf("========================================\n");

    return 0;
}
