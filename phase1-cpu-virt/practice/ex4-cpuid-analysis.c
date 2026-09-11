/*
 * Exercise 4: CPUID 虚拟化双机制对比
 *
 * 目标: 遍历 CPUID leaf 0-1F，记录每次执行，分析 KVM 如何过滤
 * 连接: ../annotations.md §5, §2
 *
 * KVM 对 CPUID 的虚拟化有两种机制:
 *   1. KVM CPUID 拦截 — Guest 任何 ring 执行 CPUID 都触发 VM-Exit,
 *      KVM 从 vcpu->arch.cpuid_entries 返回过滤后的值
 *      (kvm_emulate_cpuid(), cpuid.c:1681)
 *   2. CPUID Faulting — 用户态 (Ring 3) 执行 CPUID 触发 #GP
 *      (cpuid.c:1685: cpuid_fault_enabled 检查)
 *
 * 两者可以同时启用:
 *   - KVM 拦截: 过滤返回值（Guest 内核态）
 *   - Faulting: 阻止用户态探测（Guest 用户态）
 *
 * 源码引用:
 *   arch/x86/kvm/cpuid.c:1629 — kvm_cpuid()
 *   arch/x86/kvm/cpuid.c:1681 — kvm_emulate_cpuid()
 *   arch/x86/kvm/cpuid.h:168  — supports_cpuid_fault()
 *   arch/x86/kvm/vmx/vmx.c:6103 — exit handler table
 *
 * 规范引用:
 *   Intel SDM Vol 2, CPUID instruction
 *   Intel SDM Vol 3, Section 25.1.2 (VM-Exits caused by instructions)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
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

/* ================================================================
 *  CPUID 执行 + 记录 (保留供未来扩展)
 * ================================================================ */
static void do_cpuid(uint32_t leaf, uint32_t subleaf,
                     uint32_t *eax, uint32_t *ebx,
                     uint32_t *ecx, uint32_t *edx)
{
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}

/* ================================================================
 *  已知的关键 CPUID leaf 说明
 * ================================================================ */
static const char *leaf_description(uint32_t leaf)
{
    switch (leaf) {
    case 0x00: return "Vendor ID + max basic leaf";
    case 0x01: return "Processor Info + Feature Bits";
    case 0x02: return "Cache/TLB Descriptors";
    case 0x03: return "Processor Serial Number";
    case 0x04: return "Deterministic Cache Parameters";
    case 0x05: return "MONITOR/MWAIT";
    case 0x06: return "Thermal and Power Management";
    case 0x07: return "Extended Features (subleaf 0)";
    case 0x08: return "Direct Cache Access Info";
    case 0x09: return "Direct Cache Access Info";
    case 0x0A: return "Architectural Performance Monitoring";
    case 0x0B: return "Extended Topology (x2APIC ID)";
    case 0x0C: return "Reserved";
    case 0x0D: return "Processor Extended State (XSAVE)";
    case 0x0E: return "Reserved";
    case 0x0F: return "Intel RDT Monitoring";
    case 0x10: return "Intel RDT Allocation";
    case 0x11: return "Reserved";
    case 0x12: return "Intel SGX";
    case 0x13: return "Reserved";
    case 0x14: return "Intel Processor Trace";
    case 0x15: return "Time Stamp Counter and Nominal Core Crystal Clock";
    case 0x16: return "Processor Frequency Information";
    case 0x17: return "SoC Vendor Attributes";
    case 0x18: return "Deterministic Address Translation Parameters";
    case 0x19: return "Key Locker";
    case 0x1A: return "Hybrid Information";
    case 0x1B: return "PCONFIG Information";
    case 0x1C: return "Last Branch Record";
    case 0x1D: return "Reserved";
    case 0x1E: return "TMUL Information";
    case 0x1F: return "V2 Extended Topology";
    default:   return "Unknown";
    }
}

/* ================================================================
 *  分析关键位的过滤情况
 * ================================================================ */
static void analyze_leaf01(uint32_t ecx, uint32_t edx)
{
    printf("  === Leaf 1 关键位分析 ===\n");

    /* ECX 位 */
    struct { int bit; const char *name; const char *note; } ecx_bits[] = {
        {  0, "SSE3",          "" },
        {  1, "PCLMULQDQ",    "" },
        {  3, "MONITOR",       "" },
        {  5, "VMX",           "KVM 透传（嵌套虚拟化）或屏蔽" },
        {  6, "SMX",           "通常被 KVM 屏蔽" },
        {  7, "EIST",          "Enhanced Intel SpeedStep" },
        {  8, "TM2",           "" },
        {  9, "SSSE3",         "" },
        { 12, "FMA",           "" },
        { 13, "CX16",          "" },
        { 14, "xTPR Update",   "" },
        { 15, "PDCM",          "Perf/Debug Capability MSR" },
        { 21, "XSAVE",         "" },
        { 22, "OSXSAVE",       "需要 CR4.OSXSAVE=1" },
        { 28, "AVX",           "" },
        { 31, "RDRAND",        "" },
    };

    printf("\n  ECX = 0x%08x\n", ecx);
    for (size_t i = 0; i < sizeof(ecx_bits)/sizeof(ecx_bits[0]); i++) {
        int v = (ecx >> ecx_bits[i].bit) & 1;
        printf("  bit %2d: %d  %-15s %s\n",
               ecx_bits[i].bit, v, ecx_bits[i].name, ecx_bits[i].note);
    }

    /* EDX 位 */
    struct { int bit; const char *name; } edx_bits[] = {
        {  0, "FPU"   }, {  4, "TSC"   }, {  5, "MSR"   },
        {  6, "PAE"   }, {  8, "CX8"   }, {  9, "APIC"  },
        { 11, "SEP"   }, { 15, "CMOV"  }, { 16, "CLFSH" },
        { 19, "CLFLUSH"}, { 23, "MMX"   }, { 24, "FXSR"  },
        { 25, "SSE"   }, { 26, "SSE2"  }, { 28, "HTT"   },
    };

    printf("\n  EDX = 0x%08x\n", edx);
    for (size_t i = 0; i < sizeof(edx_bits)/sizeof(edx_bits[0]); i++) {
        int v = (edx >> edx_bits[i].bit) & 1;
        printf("  bit %2d: %d  %s\n", edx_bits[i].bit, v, edx_bits[i].name);
    }

    printf("\n  分析:\n");
    printf("  - VMX (ECX bit 5): 如果 =1, 说明嵌套虚拟化已启用\n");
    printf("    (KVM 通过 KVM_SET_CPUID2 注入这个位)\n");
    printf("  - APIC (EDX bit 9): KVM 可能修改 APIC ID\n");
    printf("  - HTT (EDX bit 28): 反映物理拓扑\n");
}

static void analyze_leaf07(uint32_t ebx, uint32_t ecx)
{
    printf("  === Leaf 7 关键位分析 ===\n");

    struct { int bit; const char *name; int reg; const char *note; } bits[] = {
        /* EBX */
        {  0, "FSGSBASE",       0, "" },
        {  1, "TSC_ADJUST",     0, "" },
        {  3, "BMI1",           0, "" },
        {  5, "AVX2",           0, "" },
        {  7, "SMEP",           0, "" },
        {  8, "BMI2",           0, "" },
        {  9, "ENH_REP_MOVSB",  0, "Enh REP MOVSB" },
        { 10, "INVPCID",        0, "KVM 可能透传" },
        { 11, "RTX",            0, "" },
        { 12, "RDT_M",          0, "" },
        { 14, "MPX",            0, "通常被屏蔽" },
        { 15, "RDT_A",          0, "" },
        { 16, "AVX512F",        0, "" },
        { 18, "RDSEED",         0, "" },
        { 19, "ADX",            0, "" },
        { 20, "SMAP",           0, "" },
        { 23, "CLFLUSHOPT",     0, "" },
        { 24, "CLWB",           0, "" },
        { 25, "INTEL_PT",       0, "" },
        { 26, "AVX512PF",       0, "" },
        { 29, "SHA_NI",         0, "" },
        /* ECX */
        {  0, "PREFETCHWT1",    1, "" },
        {  2, "UMIP",           1, "" },
        {  3, "PKU",            1, "" },
        {  4, "OSPKE",          1, "" },
        { 11, "SGX_LC",         1, "" },
        { 22, "RDPID",          1, "KVM 可能透传" },
        { 25, "CLDEMOTE",       1, "" },
        { 27, "MOVDIRI",        1, "" },
        { 28, "MOVDIR64B",      1, "" },
    };

    printf("\n  EBX = 0x%08x\n", ebx);
    printf("  ECX = 0x%08x\n", ecx);
    printf("\n");
    for (size_t i = 0; i < sizeof(bits)/sizeof(bits[0]); i++) {
        uint32_t reg_val = bits[i].reg == 0 ? ebx : ecx;
        int v = (reg_val >> bits[i].bit) & 1;
        printf("  %s bit %2d: %d  %-18s %s\n",
               bits[i].reg == 0 ? "EBX" : "ECX",
               bits[i].bit, v, bits[i].name, bits[i].note);
    }

    printf("\n  分析:\n");
    printf("  - INVPCID (EBX bit 10): KVM 透传需要二级控制 ENABLE_INVPCID\n");
    printf("  - RDPID (ECX bit 22): KVM 透传需要二级控制 ENABLE_RDTSCP\n");
    printf("  - TSX (EBX bit 11,12): 如果 MSR_IA32_TSX_CTRL 设置了\n");
    printf("    TSX_CTRL_CPUID_CLEAR, KVM 会清掉这些位 (cpuid.c:1649)\n");
}

/* ================================================================
 *  CPUID Faulting 测试
 * ================================================================ */
static sigjmp_buf jmpbuf;
static int got_signal = 0;

void fault_handler(int sig)
{
    (void)sig;
    got_signal = 1;
    siglongjmp(jmpbuf, 1);
}

static void test_cpuid_faulting(void)
{
    uint32_t eax, ebx, ecx, edx;
    long ret;

    printf("\n=== CPUID Faulting 测试 ===\n\n");

    /* 检查支持 */
    ret = syscall(SYS_arch_prctl, ARCH_GET_CPUID, 0);
    if (ret < 0) {
        printf("  ✗ CPUID Faulting 不支持\n");
        return;
    }
    printf("  ✓ CPUID Faulting 支持\n");

    /* 未启用时执行 CPUID */
    printf("\n  [1] 未启用 Faulting, Ring 3 CPUID: ");
    got_signal = 0;
    signal(SIGSEGV, fault_handler);
    if (sigsetjmp(jmpbuf, 1) == 0) {
        do_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
        printf("成功 (EAX=0x%x)\n", eax);
    } else {
        printf("失败 (SIGSEGV)\n");
    }

    /* 启用 CPUID Faulting */
    ret = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);
    if (ret != 0) {
        printf("  ✗ 启用 CPUID Faulting 失败\n");
        return;
    }
    printf("  ✓ CPUID Faulting 已启用\n");

    /* 启用后执行 CPUID */
    printf("\n  [2] 已启用 Faulting, Ring 3 CPUID: ");
    got_signal = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        do_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
        printf("成功 (不应该发生)\n");
    } else {
        printf("失败 — SIGSEGV (符合预期)\n");
        printf("  说明: Ring 3 CPUID 被 fault, 但 Ring 0 不受影响\n");
    }

    /* 禁用 */
    syscall(SYS_arch_prctl, ARCH_SET_CPUID, 1);
    printf("\n  ✓ CPUID Faulting 已禁用\n");
}

/* ================================================================
 *  主函数
 * ================================================================ */
int main(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_leaf;

    printf("================================================================\n");
    printf("  Exercise 4: CPUID 虚拟化双机制对比\n");
    printf("  连接: ../annotations.md §5, §2\n");
    printf("  源码: cpuid.c:1629 — kvm_cpuid()\n");
    printf("================================================================\n\n");

    /* Step 1: 获取最大 leaf */
    printf("[1] CPUID Leaf 0 — 获取最大基本 leaf\n");
    do_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    max_leaf = eax;
    printf("  Max basic leaf: 0x%x\n", max_leaf);
    printf("  Vendor: ");
    char vendor[13];
    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';
    printf("%s\n\n", vendor);

    /* Step 2: 遍历 CPUID leaf 0-1F */
    printf("[2] 遍历 CPUID Leaf 0-1F\n");
    printf("  每次 CPUID 在 VMX non-root 模式下都触发 VM-Exit\n");
    printf("  KVM 从 vcpu->arch.cpuid_entries 返回过滤后的值\n\n");

    printf("  %-6s %-4s %-10s %-10s %-10s %-10s  %s\n",
           "Leaf", "Sub", "EAX", "EBX", "ECX", "EDX", "Description");
    printf("  ──── ──── ────────── ────────── ────────── ──────────  ───────────────────\n");

    for (uint32_t leaf = 0; leaf <= 0x1F && leaf <= max_leaf; leaf++) {
        /* Leaf 4, 7, B, D 有 subleaves */
        if (leaf == 4 || leaf == 7 || leaf == 0xB || leaf == 0xD) {
            /* 尝试 subleaf 0-3 */
            for (uint32_t sub = 0; sub < 4; sub++) {
                do_cpuid(leaf, sub, &eax, &ebx, &ecx, &edx);

                /* Leaf 4: EAX[4:0]=0 表示 no more caches */
                if (leaf == 4 && (eax & 0x1F) == 0 && sub > 0)
                    break;
                /* Leaf B: ECX[15:8]=0 表示 no more levels */
                if (leaf == 0xB && ((ecx >> 8) & 0xFF) == 0 && sub > 0)
                    break;

                printf("  0x%02x   %u   0x%08x 0x%08x 0x%08x 0x%08x  %s\n",
                       leaf, sub, eax, ebx, ecx, edx,
                       sub == 0 ? leaf_description(leaf) : "");
            }
        } else {
            do_cpuid(leaf, 0, &eax, &ebx, &ecx, &edx);
            printf("  0x%02x   0   0x%08x 0x%08x 0x%08x 0x%08x  %s\n",
                   leaf, eax, ebx, ecx, edx, leaf_description(leaf));
        }
    }

    /* Step 3: 分析关键 leaf */
    printf("\n[3] 关键 leaf 分析\n\n");

    printf("--- Leaf 1 (Processor Info) ---\n");
    do_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    analyze_leaf01(ecx, edx);

    printf("\n--- Leaf 7 (Extended Features) ---\n");
    do_cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    analyze_leaf07(ebx, ecx);

    /* Step 4: CPUID Faulting 对比 */
    test_cpuid_faulting();

    /* Step 5: 总结 */
    printf("\n================================================================\n");
    printf("  双机制对比\n");
    printf("================================================================\n\n");

    printf("  ┌─────────────────────┬────────────────────┬────────────────────┐\n");
    printf("  │                     │ KVM CPUID 拦截      │ CPUID Faulting     │\n");
    printf("  ├─────────────────────┼────────────────────┼────────────────────┤\n");
    printf("  │ 触发条件             │ Guest 任何 ring     │ Guest Ring 3 only  │\n");
    printf("  │ 触发方式             │ VM-Exit (硬件)      │ #GP (软件异常)     │\n");
    printf("  │ 处理方式             │ kvm_emulate_cpuid()│ 注入 #GP 给 Guest  │\n");
    printf("  │ 返回值               │ 过滤后的 CPUID 值   │ 不返回 (异常)       │\n");
    printf("  │ 源码位置             │ cpuid.c:1681       │ cpuid.c:1685       │\n");
    printf("  │ 用途                 │ 虚拟化特性          │ 安全 (防探测)       │\n");
    printf("  │ Ring 0 影响          │ 是 (VM-Exit)        │ 否 (只影响 Ring 3) │\n");
    printf("  └─────────────────────┴────────────────────┴────────────────────┘\n\n");

    printf("  源码引用:\n");
    printf("    kvm_emulate_cpuid(): arch/x86/kvm/cpuid.c:1681\n");
    printf("    kvm_cpuid():         arch/x86/kvm/cpuid.c:1629\n");
    printf("    cpuid_fault check:   arch/x86/kvm/cpuid.c:1685\n");
    printf("    exit handler table:  arch/x86/kvm/vmx/vmx.c:6103\n");
    printf("\n");

    printf("================================================================\n");
    printf("  ✓ CPUID 虚拟化分析完成\n");
    printf("================================================================\n");

    return 0;
}
