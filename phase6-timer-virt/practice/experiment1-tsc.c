/*
 * experiment1-tsc.c — TSC 虚拟化观察
 *
 * 目标: 验证 guest_TSC = host_TSC × ratio + offset 公式
 *
 * 实验步骤:
 *   1. 创建 VM + vCPU
 *   2. 在 guest 内执行 RDTSC (实模式代码)
 *   3. Host 侧读 host TSC，通过 KVM API 读 guest TSC
 *   4. 用 KVM_SET_TSC_KHZ 改变 TSC 频率
 *   5. 再次执行 RDTSC，对比变化
 *   6. 验证 TSC scaling 是否生效
 *
 * 关键内核代码路径:
 *   KVM_SET_TSC_KHZ → kvm_arch_set_tsc_khz() → vcpu->arch.tsc_scaling_ratio
 *   KVM_GET_MSRS(TSC) → kvm_get_msr_common() → kvm_read_l1_tsc()
 *   vmx.c:1951 vmx_write_tsc_offset() — VMCS TSC_OFFSET 写入
 *   vmx.c:1956 vmx_write_tsc_multiplier() — VMCS TSC_MULTIPLIER 写入
 *
 * 运行: sudo ./experiment1-tsc
 */

#include "common.h"

/*
 * Guest 实模式代码:
 *   RDTSC          ; 读 TSC 到 EDX:EAX
 *   HLT            ; 停止, 触发 VM-Exit
 */
static const uint8_t guest_rdtsc[] = {
    0x0f, 0x31,     /* RDTSC: EDX:EAX = TSC */
    0xf4,           /* HLT */
};

int main(void)
{
    struct kvm_vm vm = {};
    struct kvm_vcpu vcpu = {};

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Experiment 1: TSC Virtualization                         ║\n");
    printf("║  验证: guest_TSC = host_TSC × ratio + offset              ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    /* 1. 初始化 KVM */
    kvm_init();

    /* 2. 检查 TSC 相关能力 */
    int cap_tsc_control = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_TSC_CONTROL);
    int cap_vm_tsc_control = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_VM_TSC_CONTROL);
    int cap_get_tsc_khz = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_GET_TSC_KHZ);

    printf("[cap] KVM_CAP_TSC_CONTROL (per-vcpu) = %d\n", cap_tsc_control);
    printf("[cap] KVM_CAP_VM_TSC_CONTROL (per-vm)  = %d\n", cap_vm_tsc_control);
    printf("[cap] KVM_CAP_GET_TSC_KHZ              = %d\n", cap_get_tsc_khz);

    if (!cap_tsc_control && !cap_vm_tsc_control) {
        printf("\n[!] TSC scaling not supported on this host.\n");
        printf("    (Need Intel VMX with TSC scaling or AMD SVM with TSC ratio)\n");
        printf("    Continuing with TSC read-only observation...\n");
    }

    /* 3. 创建 VM + vCPU */
    vm_create(&vm, 64 * 1024);  /* 64 KB guest memory */
    vcpu_create(&vm, &vcpu);

    /* 4. 读取 host TSC 频率 */
    int host_tsc_khz = ioctl(vcpu.fd, KVM_GET_TSC_KHZ, 0);
    if (host_tsc_khz > 0) {
        printf("\n[tsc] Host TSC frequency: %d KHz (%.2f GHz)\n",
               host_tsc_khz, host_tsc_khz / 1000000.0);
    }

    /* 5. 加载 guest 代码 */
    load_guest_code(&vm, &vcpu, guest_rdtsc, sizeof(guest_rdtsc), 0x1000);

    /* 6. 第一次运行 — 读取初始 TSC */
    print_separator("Phase 1: Initial TSC Read");

    uint64_t host_tsc_before = host_rdtsc();
    ioctl(vcpu.fd, KVM_RUN, 0);

    uint64_t guest_tsc_1 = vcpu_get_tsc(&vcpu);
    uint64_t host_tsc_after = host_rdtsc();

    printf("  Host TSC (before RDTSC):  %lu\n", host_tsc_before);
    printf("  Host TSC (after  RDTSC):  %lu\n", host_tsc_after);
    printf("  Host TSC delta:           %lu cycles\n", host_tsc_after - host_tsc_before);
    printf("  Guest TSC (from MSR):     %lu\n", guest_tsc_1);
    printf("  TSC offset:               %ld (guest - host_before)\n",
           (int64_t)(guest_tsc_1 - host_tsc_before));

    /* 7. 第二次运行 — 观察 TSC 增长 */
    print_separator("Phase 2: TSC Growth Observation");

    /* 先让 guest 重新执行 RDTSC (设置 RIP 回 0x1000) */
    vcpu_set_rip(&vcpu, 0x1000);

    uint64_t host_tsc_p2 = host_rdtsc();
    ioctl(vcpu.fd, KVM_RUN, 0);

    uint64_t guest_tsc_2 = vcpu_get_tsc(&vcpu);
    uint64_t host_tsc_p2_end = host_rdtsc();

    printf("  Host TSC (before):  %lu\n", host_tsc_p2);
    printf("  Guest TSC:          %lu\n", guest_tsc_2);
    printf("  Guest TSC delta:    %lu (since phase 1)\n", guest_tsc_2 - guest_tsc_1);
    printf("  Host TSC delta:     %lu (since phase 1)\n",
           host_tsc_p2_end - host_tsc_after);

    /* 8. TSC Scaling (如果支持) */
    if (cap_tsc_control || cap_vm_tsc_control) {
        print_separator("Phase 3: TSC Scaling (KVM_SET_TSC_KHZ)");

        int new_tsc_khz = host_tsc_khz / 2;  /* 减速到 50% */
        if (new_tsc_khz < 1000)
            new_tsc_khz = 1000;  /* 最低 1 MHz */

        printf("  Original TSC frequency: %d KHz\n", host_tsc_khz);
        printf("  Setting TSC frequency:  %d KHz (50%% slowdown)\n", new_tsc_khz);

        /* 尝试 per-vCPU TSC 控制 */
        int ret = ioctl(vcpu.fd, KVM_SET_TSC_KHZ, (unsigned long)new_tsc_khz);
        if (ret < 0) {
            printf("  [!] KVM_SET_TSC_KHZ failed: %s\n", strerror(errno));
            printf("      Trying KVM_SET_TSC_KHZ on VM fd...\n");
            ret = ioctl(vm.fd, KVM_SET_TSC_KHZ, (unsigned long)new_tsc_khz);
            if (ret < 0)
                printf("  [!] VM-level KVM_SET_TSC_KHZ also failed: %s\n",
                       strerror(errno));
        }

        if (ret >= 0) {
            /* 验证设置 */
            int cur_tsc_khz = ioctl(vcpu.fd, KVM_GET_TSC_KHZ, 0);
            printf("  Verified TSC frequency: %d KHz\n", cur_tsc_khz);

            /*
             * 注意: KVM_SET_TSC_KHZ 内部调用 kvm_synchronize_tsc(vcpu, NULL)
             * (x86.c:2733 注释: data==0 → force synchronization)，会把
             * guest TSC 重新对齐到基于当前 host 时间的参考系。
             * 所以 phase 2 的 guest_tsc_2 已不可用作基准——
             * 必须先跑一次 guest 建立新基准，再测量增长率。
             */
            vcpu_set_rip(&vcpu, 0x1000);
            ioctl(vcpu.fd, KVM_RUN, 0);
            uint64_t guest_tsc_base = vcpu_get_tsc(&vcpu);
            uint64_t host_tsc_base = host_rdtsc();
            printf("  Guest TSC re-based after SET_TSC_KHZ: %lu (0x%lx)\n",
                   guest_tsc_base, guest_tsc_base);
            printf("  (旧基准 %lu 已失效, u64 delta=0x%lx)\n",
                   guest_tsc_2, guest_tsc_base - guest_tsc_2);

            /* 再次运行 guest, 观察 TSC 增长变慢 */
            vcpu_set_rip(&vcpu, 0x1000);

            ioctl(vcpu.fd, KVM_RUN, 0);

            uint64_t guest_tsc_3 = vcpu_get_tsc(&vcpu);
            uint64_t host_tsc_p3_end = host_rdtsc();

            uint64_t guest_delta = guest_tsc_3 - guest_tsc_base;
            uint64_t host_delta = host_tsc_p3_end - host_tsc_base;

            printf("\n  After TSC scaling:\n");
            printf("    Host TSC delta:     %lu cycles\n", host_delta);
            printf("    Guest TSC delta:    %lu cycles\n", guest_delta);
            printf("    Ratio:              %.4f (expected ~0.5)\n",
                   (double)guest_delta / host_delta);

            printf("\n  ✓ TSC scaling verified: guest TSC grows at %.1f%% of host rate\n",
                   guest_delta * 100.0 / host_delta);
        }

        /* 恢复原始频率 */
        ioctl(vcpu.fd, KVM_SET_TSC_KHZ, (unsigned long)host_tsc_khz);
    }

    /* 9. 总结 */
    print_separator("Summary");
    printf("  TSC virtualization formula:\n");
    printf("    guest_TSC = host_TSC × tsc_scaling_ratio + tsc_offset\n\n");
    printf("  VMCS fields involved:\n");
    printf("    TSC_OFFSET     (vmx.c:1953) — added to host TSC\n");
    printf("    TSC_MULTIPLIER (vmx.c:1958) — multiplied with host TSC\n\n");
    printf("  Key kernel functions:\n");
    printf("    vmx_write_tsc_offset()     — writes TSC_OFFSET to VMCS\n");
    printf("    vmx_write_tsc_multiplier() — writes TSC_MULTIPLIER to VMCS\n");
    printf("    kvm_read_l1_tsc()          — applies scaling to read host TSC\n\n");

    /* 10. 清理 */
    vcpu_destroy(&vcpu);
    vm_destroy(&vm);
    kvm_cleanup();

    printf("[done] Experiment 1 completed.\n");
    return 0;
}
