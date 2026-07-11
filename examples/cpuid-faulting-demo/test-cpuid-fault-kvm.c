// SPDX-License-Identifier: GPL-2.0
/*
 * CPUID Faulting KVM 测试程序
 *
 * 在 KVM 虚拟机中测试 CPUID Faulting 特性
 *
 * 功能:
 *   1. 检测虚拟机是否支持 CPUID Faulting
 *   2. 测试 CPUID 虚拟化效果
 *   3. 启用 CPUID Faulting
 *   4. 验证在 VMX 中的行为
 *
 * 编译:
 *   gcc -o test-cpuid-fault-kvm test-cpuid-fault-kvm.c
 *
 * 运行 (在 KVM 虚拟机内):
 *   ./test-cpuid-fault-kvm
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
#include <cpuid.h>

#ifndef ARCH_GET_CPUID
#define ARCH_GET_CPUID  0x1011
#endif
#ifndef ARCH_SET_CPUID
#define ARCH_SET_CPUID  0x1012
#endif

static sigjmp_buf jmpbuf;
static volatile sig_atomic_t got_signal = 0;

static void signal_handler(int sig)
{
    got_signal = sig;
    siglongjmp(jmpbuf, 1);
}

/* 检测是否在虚拟机中运行 */
static int detect_hypervisor(void)
{
    unsigned int eax, ebx, ecx, edx;

    /* CPUID leaf 1 ECX bit 31 = hypervisor present */
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (ecx & (1u << 31)) {
            return 1;  /* 在虚拟机中 */
        }
    }

    return 0;
}

/* 获取 Hypervisor 签名 */
static void get_hypervisor_signature(char *sig)
{
    unsigned int eax, ebx, ecx, edx;

    /* CPUID leaf 0x40000000 */
    if (__get_cpuid(0x40000000, &eax, &ebx, &ecx, &edx)) {
        memcpy(sig, &ebx, 4);
        memcpy(sig + 4, &ecx, 4);
        memcpy(sig + 8, &edx, 4);
        sig[12] = '\0';
    } else {
        strcpy(sig, "Unknown");
    }
}

/* 测试 CPUID 虚拟化 */
static void test_cpuid_virtualization(void)
{
    unsigned int eax, ebx, ecx, edx;

    printf("\n=== 测试 CPUID 虚拟化 ===\n\n");

    /* 获取 CPU 厂商 */
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
        char vendor[13];
        memcpy(vendor, &ebx, 4);
        memcpy(vendor + 4, &edx, 4);
        memcpy(vendor + 8, &ecx, 4);
        vendor[12] = '\0';

        printf("1. CPU 厂商: %s\n", vendor);

        /* 在虚拟机中，QEMU 可以自定义厂商字符串 */
        if (strcmp(vendor, "GenuineIntel") != 0 &&
            strcmp(vendor, "AuthenticAMD") != 0) {
            printf("   注意: 厂商字符串可能被虚拟化\n");
        }
    }

    /* 获取 Hypervisor 签名 */
    char hyp_sig[13];
    get_hypervisor_signature(hyp_sig);
    printf("\n2. Hypervisor 签名: %s\n", hyp_sig);

    if (strcmp(hyp_sig, "KVMKVMKVM") == 0) {
        printf("   ✓ 运行在 KVM 虚拟机中\n");
    } else if (strcmp(hyp_sig, "QEMUQEMU") == 0) {
        printf("   ✓ 运行在 QEMU 虚拟机中\n");
    } else {
        printf("   运行在其他虚拟化环境中\n");
    }

    /* 获取 CPU 型号 */
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        unsigned int family = (eax >> 8) & 0xf;
        unsigned int model = (eax >> 4) & 0xf;
        unsigned int stepping = eax & 0xf;

        if (family == 0xf) {
            family += (eax >> 20) & 0xff;
        }
        if (family == 0x6 || family == 0xf) {
            model += ((eax >> 16) & 0xf) << 4;
        }

        printf("\n3. CPU 型号信息:\n");
        printf("   Family: 0x%x\n", family);
        printf("   Model:  0x%x\n", model);
        printf("   Stepping: 0x%x\n", stepping);

        /* 在虚拟机中，这些信息可能被虚拟化 */
        printf("   注意: 这些值可能被 QEMU 虚拟化\n");
    }
}

/* 测试 CPUID Faulting */
static void test_cpuid_faulting_in_vm(void)
{
    printf("\n=== 在虚拟机中测试 CPUID Faulting ===\n\n");

    /* 检测支持 */
    printf("1. 检测 CPUID Faulting 支持\n");

    /* 通过 arch_prctl 检测 */
    long ret = syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0);
    if (ret < 0) {
        printf("   ✗ arch_prctl 不可用，可能不支持 CPUID Faulting\n");
        printf("   原因: KVM 未暴露该特性给 Guest\n");
        printf("   解决: 使用 -cpu host 或 -cpu qemu64,cpuid-fault=on\n");
        return;
    }

    printf("   ✓ Guest 支持 CPUID Faulting\n\n");

    /* 测试 CPUID（未启用） */
    printf("2. 测试 CPUID（未启用 Faulting）\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    got_signal = 0;
    unsigned int eax, ebx, ecx, edx;

    if (sigsetjmp(jmpbuf, 1) == 0) {
        __asm__ __volatile__(
            "cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(0)
        );
        printf("   ✓ CPUID 成功执行\n");
        printf("   注意: 在 VMX 中，这会触发 VM-Exit，KVM 模拟返回\n");
    } else {
        printf("   ✗ CPUID 触发信号 %d\n", got_signal);
    }

    printf("\n");

    /* 启用 CPUID Faulting */
    printf("3. 启用 CPUID Faulting\n");
    ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);
    if (ret < 0) {
        perror("   启用失败");
        return;
    }
    printf("   ✓ 启用成功\n\n");

    /* 测试 CPUID（已启用） */
    printf("4. 测试 CPUID（已启用 Faulting）\n");

    got_signal = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        __asm__ __volatile__(
            "cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(0)
        );
        printf("   ✗ 异常：CPUID 应该被禁止\n");
    } else {
        printf("   ✓ CPUID 触发信号 %d\n", got_signal);
        printf("   在 VMX 中的行为:\n");
        printf("     Ring 3 CPUID → VM-Exit → KVM 检查\n");
        printf("     → CPUID Faulting 启用 → 不处理 → #GP\n");
        printf("     性能提升: 无 VM-Exit 处理开销\n");
    }

    printf("\n");

    /* 禁用 CPUID Faulting */
    printf("5. 禁用 CPUID Faulting\n");
    ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 1);
    if (ret < 0) {
        perror("   禁用失败");
        return;
    }
    printf("   ✓ 禁用成功\n");
}

/* 对比 Host 和 Guest 的 CPUID */
static void compare_host_guest_cpuid(void)
{
    printf("\n=== Host vs Guest CPUID 对比 ===\n\n");

    printf("在虚拟机中，CPUID 返回值可能被虚拟化:\n\n");

    unsigned int eax, ebx, ecx, edx;

    /* Leaf 0: 厂商 */
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
        char vendor[13];
        memcpy(vendor, &ebx, 4);
        memcpy(vendor + 4, &edx, 4);
        memcpy(vendor + 8, &ecx, 4);
        vendor[12] = '\0';

        printf("Leaf 0x00 (厂商):\n");
        printf("  Guest: %s\n", vendor);
        printf("  可能: QEMU 可以自定义 (如 \"QEMUVirtualCPU\")\n\n");
    }

    /* Leaf 1: 特性 */
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        printf("Leaf 0x01 (特性):\n");
        printf("  EAX: 0x%08x (CPU 签名)\n", eax);
        printf("  ECX: 0x%08x (特性标志)\n", ecx);
        printf("  EDX: 0x%08x (特性标志)\n", edx);
        printf("  可能: QEMU 可以启用/禁用某些特性位\n\n");
    }

    /* Leaf 7: 扩展特性 */
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        printf("Leaf 0x07 (扩展特性):\n");
        printf("  EBX: 0x%08x\n", ebx);
        printf("  其中 bit 31 (CPUID Faulting): %s\n",
               (ebx & (1u << 31)) ? "启用" : "禁用");
        printf("  可能: QEMU 可以控制暴露哪些扩展特性\n\n");
    }

    printf("关键区别:\n");
    printf("  Host: CPUID 直接返回硬件值\n");
    printf("  Guest: CPUID → VM-Exit → KVM 返回虚拟化值\n");
    printf("  性能: Guest CPUID 有 VM-Exit 开销\n");
}

int main(void)
{
    printf("=== CPUID Faulting KVM 测试 ===\n\n");

    /* 检测是否在虚拟机中 */
    printf("0. 检测虚拟化环境\n");
    if (!detect_hypervisor()) {
        printf("   ✗ 不在虚拟机中运行\n");
        printf("   请在 KVM 虚拟机中运行此程序\n");
        printf("   启动 VM: qemu-system-x86_64 -enable-kvm ...\n");
        return 1;
    }
    printf("   ✓ 在虚拟机中运行\n");

    /* 测试 CPUID 虚拟化 */
    test_cpuid_virtualization();

    /* 测试 CPUID Faulting */
    test_cpuid_faulting_in_vm();

    /* 对比 Host 和 Guest */
    compare_host_guest_cpuid();

    printf("\n=== 测试完成 ===\n");
    return 0;
}
