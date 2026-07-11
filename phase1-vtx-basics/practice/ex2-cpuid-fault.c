/*
 * 练习 2: CPUID Faulting 测试
 *
 * 目标: 理解 CPUID Faulting 如何阻止用户态 CPUID
 */

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
    printf("   捕获到信号 %d (CPUID 被阻止)\n", sig);
    siglongjmp(jmpbuf, 1);
}

int main() {
    unsigned int eax, ebx, ecx, edx;
    long ret;

    printf("========================================\n");
    printf("  练习 2: CPUID Faulting 测试\n");
    printf("========================================\n\n");

    // 测试 1: 检查 CPUID Faulting 支持
    printf("1. 检查 CPUID Faulting 支持\n");
    ret = syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0);
    if (ret >= 0) {
        printf("   ✓ 支持 CPUID Faulting (返回值: %ld)\n", ret);
    } else {
        printf("   ✗ 不支持 CPUID Faulting\n");
        return 1;
    }
    printf("\n");

    // 测试 2: 正常执行 CPUID
    printf("2. 正常执行 CPUID (未启用 Faulting)\n");
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    printf("   ✓ CPUID 执行成功\n");
    printf("   EAX=0x%x (最大 CPUID leaf)\n", eax);
    printf("\n");

    // 测试 3: 启用 CPUID Faulting
    printf("3. 启用 CPUID Faulting\n");
    ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);
    if (ret == 0) {
        printf("   ✓ CPUID Faulting 已启用\n");
    } else {
        printf("   ✗ 启用失败 (返回值: %ld)\n", ret);
        return 1;
    }
    printf("\n");

    // 测试 4: 尝试执行 CPUID (应该触发 #GP)
    printf("4. 尝试执行 CPUID (应该失败)\n");
    signal(SIGSEGV, handler);

    if (sigsetjmp(jmpbuf, 1) == 0) {
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
        printf("   ✗ CPUID 执行成功 (不应该发生)\n");
    } else {
        printf("   ✓ CPUID 被阻止 (符合预期)\n");
    }
    printf("\n");

    // 测试 5: 禁用 CPUID Faulting
    printf("5. 禁用 CPUID Faulting\n");
    ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 1);
    if (ret == 0) {
        printf("   ✓ CPUID Faulting 已禁用\n");
    } else {
        printf("   ✗ 禁用失败 (返回值: %ld)\n", ret);
        return 1;
    }
    printf("\n");

    // 测试 6: 再次执行 CPUID (应该成功)
    printf("6. 再次执行 CPUID (应该成功)\n");
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    printf("   ✓ CPUID 执行成功\n");
    printf("   EAX=0x%x\n", eax);
    printf("\n");

    printf("========================================\n");
    printf("  ✓ CPUID Faulting 测试完成\n");
    printf("========================================\n");

    return 0;
}
