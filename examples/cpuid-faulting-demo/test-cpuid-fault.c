// SPDX-License-Identifier: GPL-2.0
/*
 * CPUID Faulting 测试程序
 *
 * 演示 Linux CPUID Faulting 特性的检测、启用和使用
 *
 * 功能:
 *   1. 检测 CPU 是否支持 CPUID Faulting
 *   2. 测试 CPUID 指令执行
 *   3. 启用 CPUID Faulting
 *   4. 验证 Ring 3 CPUID 触发 #GP
 *   5. 禁用 CPUID Faulting
 *   6. 验证 CPUID 恢复正常
 *
 * 编译:
 *   gcc -o test-cpuid-fault test-cpuid-fault.c
 *
 * 运行:
 *   ./test-cpuid-fault
 *
 * 对应课程: Phase 1 - CPUID 虚拟化 - CPUID Faulting 机制
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <cpuid.h>

/* arch_prctl 命令 */
#ifndef ARCH_GET_CPUID
#define ARCH_GET_CPUID  0x1011
#endif
#ifndef ARCH_SET_CPUID
#define ARCH_SET_CPUID  0x1012
#endif

/* 全局跳转缓冲区，用于捕获 SIGSEGV */
static sigjmp_buf jmpbuf;
static volatile sig_atomic_t got_signal = 0;

/* 信号处理器 */
static void signal_handler(int sig)
{
    got_signal = sig;
    siglongjmp(jmpbuf, 1);
}

/* 检测 CPUID Faulting 支持 */
static int detect_cpuid_fault_support(void)
{
    /* 方法1: 通过 /proc/cpuinfo */
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "cpuid_fault")) {
                fclose(fp);
                return 1;  /* 支持 */
            }
        }
        fclose(fp);
    }

    /* 方法2: 通过 CPUID leaf 7 */
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if (ebx & (1u << 31)) {
            return 1;  /* CPUID leaf 7 EBX[31] = 1 表示支持 */
        }
    }

    /* 方法3: 通过 arch_prctl */
    long ret = syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0);
    if (ret >= 0) {
        return 1;  /* arch_prctl 可用表示支持 */
    }

    return 0;  /* 不支持 */
}

/* 测试 CPUID 执行 */
static int test_cpuid_execution(void)
{
    unsigned int eax, ebx, ecx, edx;
    char vendor[13];

    /* 设置信号处理器 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    got_signal = 0;

    /* 尝试执行 CPUID */
    if (sigsetjmp(jmpbuf, 1) == 0) {
        /* 执行 CPUID leaf 0（获取厂商信息） */
        __asm__ __volatile__(
            "cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(0)
        );

        /* 成功执行，提取厂商字符串 */
        memcpy(vendor, &ebx, 4);
        memcpy(vendor + 4, &edx, 4);
        memcpy(vendor + 8, &ecx, 4);
        vendor[12] = '\0';

        printf("   ✓ CPUID 成功执行\n");
        printf("   CPU 厂商: %s\n", vendor);
        return 1;  /* 成功 */
    } else {
        /* 捕获到信号 */
        printf("   ✗ CPUID 触发信号 %d (%s)\n",
               got_signal,
               got_signal == SIGSEGV ? "SIGSEGV" : "SIGILL");
        return 0;  /* 失败 */
    }
}

/* 启用 CPUID Faulting */
static int enable_cpuid_faulting(void)
{
    long ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);
    if (ret == 0) {
        return 1;  /* 成功 */
    } else {
        perror("   arch_prctl(ARCH_SET_CPUID, 0) 失败");
        return 0;  /* 失败 */
    }
}

/* 禁用 CPUID Faulting */
static int disable_cpuid_faulting(void)
{
    long ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 1);
    if (ret == 0) {
        return 1;  /* 成功 */
    } else {
        perror("   arch_prctl(ARCH_SET_CPUID, 1) 失败");
        return 0;  /* 失败 */
    }
}

/* 查询当前状态 */
static int query_cpuid_status(void)
{
    long ret = syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0);
    if (ret == 1) {
        return 1;  /* CPUID 可用 */
    } else if (ret == 0) {
        return 0;  /* CPUID 被禁止 */
    } else {
        return -1;  /* 错误 */
    }
}

/* 测试 fork 后的继承行为 */
static void test_fork_inheritance(void)
{
    printf("\n=== 测试 fork 继承行为 ===\n\n");

    /* 父进程启用 CPUID Faulting */
    printf("1. 父进程启用 CPUID Faulting\n");
    if (!enable_cpuid_faulting()) {
        printf("   启用失败，跳过测试\n");
        return;
    }

    /* fork 子进程 */
    pid_t pid = fork();
    if (pid < 0) {
        perror("   fork 失败");
        disable_cpuid_faulting();
        return;
    }

    if (pid == 0) {
        /* 子进程 */
        printf("\n2. 子进程测试 CPUID\n");
        int result = test_cpuid_execution();

        if (result == 0) {
            printf("   子进程继承了父进程的设置 (CPUID 被禁止)\n");
        } else {
            printf("   子进程没有继承 (CPUID 可用)\n");
        }

        /* 子进程退出 */
        exit(0);
    } else {
        /* 父进程 */
        int status;
        waitpid(pid, &status, 0);

        printf("\n3. 父进程禁用 CPUID Faulting\n");
        disable_cpuid_faulting();
    }
}

int main(void)
{
    printf("=== CPUID Faulting 测试 ===\n\n");

    /* 步骤1: 检测支持 */
    printf("1. 检测 CPUID Faulting 支持\n");
    if (!detect_cpuid_fault_support()) {
        printf("   ✗ CPU 不支持 CPUID Faulting\n");
        printf("   测试终止\n");
        return 1;
    }
    printf("   ✓ CPU 支持 CPUID Faulting\n\n");

    /* 步骤2: 测试 CPUID（未启用 Faulting） */
    printf("2. 测试 CPUID（未启用 Faulting）\n");
    if (!test_cpuid_execution()) {
        printf("   异常：CPUID 应该可以执行\n");
        return 1;
    }
    printf("\n");

    /* 步骤3: 查询当前状态 */
    printf("3. 查询当前状态\n");
    int status = query_cpuid_status();
    if (status == 1) {
        printf("   当前状态: CPUID 可用\n");
    } else if (status == 0) {
        printf("   当前状态: CPUID 被禁止\n");
    } else {
        printf("   查询失败\n");
    }
    printf("\n");

    /* 步骤4: 启用 CPUID Faulting */
    printf("4. 启用 CPUID Faulting\n");
    printf("   调用 arch_prctl(ARCH_SET_CPUID, 0)\n");
    if (!enable_cpuid_faulting()) {
        printf("   启用失败\n");
        return 1;
    }
    printf("   ✓ 启用成功\n\n");

    /* 步骤5: 测试 CPUID（已启用 Faulting） */
    printf("5. 测试 CPUID（已启用 Faulting）\n");
    if (test_cpuid_execution()) {
        printf("   异常：CPUID 应该被禁止\n");
        disable_cpuid_faulting();
        return 1;
    }
    printf("   ✓ 这说明 CPUID Faulting 生效了！\n\n");

    /* 步骤6: 禁用 CPUID Faulting */
    printf("6. 禁用 CPUID Faulting\n");
    printf("   调用 arch_prctl(ARCH_SET_CPUID, 1)\n");
    if (!disable_cpuid_faulting()) {
        printf("   禁用失败\n");
        return 1;
    }
    printf("   ✓ 禁用成功\n\n");

    /* 步骤7: 再次测试 CPUID */
    printf("7. 再次测试 CPUID\n");
    if (!test_cpuid_execution()) {
        printf("   异常：CPUID 应该恢复正常\n");
        return 1;
    }
    printf("\n");

    /* 步骤8: 测试 fork 继承（可选） */
    test_fork_inheritance();

    printf("\n=== 测试完成 ===\n");
    return 0;
}
