/*
 * 练习 5: VM-Exit 开销测量
 *
 * 目标: 测量 VM-Exit 的性能开销
 */

#include <stdio.h>
#include <time.h>

#define ITERATIONS 1000000

int main() {
    unsigned int eax, ebx, ecx, edx;
    struct timespec start, end;

    printf("========================================\n");
    printf("  练习 5: VM-Exit 开销测量\n");
    printf("========================================\n\n");

    printf("测试 %d 次操作\n\n", ITERATIONS);

    // 测试 1: CPUID (触发 VM-Exit)
    printf("1. CPUID 指令 (触发 VM-Exit)\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        asm volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double cpuid_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                        (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", cpuid_time / 1000000);
    printf("   平均: %.2f ns/次\n", cpuid_time / ITERATIONS);
    printf("   说明: 每次 CPUID 都触发 VM-Exit\n\n");

    // 测试 2: RDTSC (不触发 VM-Exit)
    printf("2. RDTSC 指令 (不触发 VM-Exit)\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < ITERATIONS; i++) {
        asm volatile("rdtsc" : "=a"(eax), "=d"(edx));
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double rdtsc_time = (end.tv_sec - start.tv_sec) * 1000000000.0 +
                        (end.tv_nsec - start.tv_nsec);
    printf("   总时间: %.2f ms\n", rdtsc_time / 1000000);
    printf("   平均: %.2f ns/次\n", rdtsc_time / ITERATIONS);
    printf("   说明: RDTSC 通常被透传，无 VM-Exit\n\n");

    // 测试 3: RDMSR (特权指令，用户态会 #GP，跳过)
    // 注意：RDMSR 是特权指令，不能在用户态直接执行
    // 要测量 MSR 的 VM-Exit 开销，请使用宿主侧 trace-msr-access.sh
    printf("3. RDMSR (跳过 - 特权指令，用户态无法执行)\n");
    printf("   说明: 使用宿主侧 trace-msr-access.sh 追踪 kvm:kvm_msr\n\n");

    printf("========================================\n");
    printf("  分析\n");
    printf("========================================\n");
    printf("指令                    时间(ns)    VM-Exit\n");
    printf("--------------------------------------------\n");
    printf("CPUID                   %6.2f      是\n", cpuid_time / ITERATIONS);
    printf("RDTSC                   %6.2f      否\n", rdtsc_time / ITERATIONS);
    printf("\n");

    printf("VM-Exit 开销估算:\n");
    printf("  CPUID vs RDTSC: %.2f ns (%.1fx)\n",
           (cpuid_time - rdtsc_time) / ITERATIONS,
           cpuid_time / rdtsc_time);
    printf("\n");

    printf("结论:\n");
    printf("  1. VM-Exit 开销约 %.0f ns\n", (cpuid_time - rdtsc_time) / ITERATIONS);
    printf("  2. 透传指令比拦截指令快 %.0f 倍\n", cpuid_time / rdtsc_time);
    printf("  3. MSR Bitmap 可以显著减少 VM-Exit\n");
    printf("  4. 要测量 MSR 开销，使用宿主侧 trace-msr-access.sh\n");
    printf("\n");

    printf("========================================\n");
    printf("  ✓ VM-Exit 开销测量完成\n");
    printf("========================================\n");

    return 0;
}
