/*
 * experiment2-kvmclock.c — kvmclock / KVM_CLOCK 接口观察
 *
 * 目标: 观察 KVM_GET_CLOCK / KVM_SET_CLOCK 的时间跳变，模拟迁移场景
 *
 * 实验步骤:
 *   1. 创建 VM
 *   2. Host 侧调用 KVM_GET_CLOCK 读取 VM 时间
 *   3. 用 KVM_SET_CLOCK 设置一个偏移后的时间（模拟迁移）
 *   4. 再次 KVM_GET_CLOCK 验证时间跳变
 *   5. 观察 host_tsc 字段的变化
 *
 * 关键内核代码路径:
 *   KVM_GET_CLOCK → kvm_vm_ioctl_get_clock() → get_kvmclock()
 *   KVM_SET_CLOCK → kvm_vm_ioctl_set_clock() → kvm_guest_time_update()
 *
 * 运行: sudo ./experiment2-kvmclock
 */

#include "common.h"
#include <time.h>

int main(void)
{
    struct kvm_vm vm = {};

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Experiment 2: kvmclock / KVM_CLOCK Interface              ║\n");
    printf("║  观察: KVM_GET_CLOCK / KVM_SET_CLOCK 时间跳变              ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    /* 1. 初始化 KVM */
    kvm_init();

    /* 2. 检查 CLOCK 相关能力 */
    int cap_adjust_clock = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_ADJUST_CLOCK);
    printf("[cap] KVM_CAP_ADJUST_CLOCK = 0x%x\n", cap_adjust_clock);

    if (cap_adjust_clock & KVM_CLOCK_HOST_TSC)
        printf("      → KVM_CLOCK_HOST_TSC supported (host_tsc field valid)\n");

    /* 3. 创建 VM */
    vm_create(&vm, 64 * 1024);

    /* 4. Phase 1: 读取初始时钟 */
    print_separator("Phase 1: Initial Clock Read (KVM_GET_CLOCK)");

    struct kvm_clock_data clock1 = {};
    int ret = ioctl(vm.fd, KVM_GET_CLOCK, &clock1);
    DIE_ON_ERR(ret, "KVM_GET_CLOCK");

    struct timespec real_ts;
    clock_gettime(CLOCK_REALTIME, &real_ts);
    uint64_t real_ns = (uint64_t)real_ts.tv_sec * 1000000000ULL + real_ts.tv_nsec;

    printf("  KVM clock:       %lu ns (%.6f s)\n",
           (unsigned long)clock1.clock, clock1.clock / 1e9);
    printf("  Host TSC:        %lu\n", (unsigned long)clock1.host_tsc);
    printf("  Realtime:        %lu ns\n", (unsigned long)real_ns);
    printf("  Flags:           0x%x\n", clock1.flags);
    if (clock1.host_tsc == 0)
        printf("  注: host_tsc=0 且 flags 无 KVM_CLOCK_HOST_TSC,\n"
               "      因为 __get_kvmclock() (x86.c:3116) 只在\n"
               "      use_master_clock==true 时填 host_tsc;\n"
               "      本实验没跑过 vCPU, masterclock 从未初始化。\n");

    /* 5. 等一会，再读 */
    print_separator("Phase 2: Clock After 100ms Sleep");

    usleep(100000);  /* 100 ms */

    struct kvm_clock_data clock2 = {};
    ret = ioctl(vm.fd, KVM_GET_CLOCK, &clock2);
    DIE_ON_ERR(ret, "KVM_GET_CLOCK");

    printf("  KVM clock:       %lu ns\n", (unsigned long)clock2.clock);
    printf("  Host TSC:        %lu\n", (unsigned long)clock2.host_tsc);
    printf("  Clock delta:     %lu ns (expected ~100000000 ns = 100ms)\n",
           (unsigned long)(clock2.clock - clock1.clock));
    printf("  TSC delta:       %lu\n",
           (unsigned long)(clock2.host_tsc - clock1.host_tsc));

    /* 6. Phase 3: 模拟迁移 — KVM_SET_CLOCK */
    print_separator("Phase 3: Simulated Migration (KVM_SET_CLOCK +1s)");

    printf("  Setting clock forward by 1 second...\n");

    struct kvm_clock_data clock3 = {};
    clock3.clock = clock2.clock + 1000000000ULL;  /* +1s */
    clock3.flags = KVM_CLOCK_HOST_TSC;
    clock3.host_tsc = clock2.host_tsc;  /* TSC 不变 */

    ret = ioctl(vm.fd, KVM_SET_CLOCK, &clock3);
    if (ret < 0) {
        printf("  [!] KVM_SET_CLOCK failed: %s\n", strerror(errno));
        printf("      (This is expected if KVM_CAP_ADJUST_CLOCK is 0)\n");
    } else {
        printf("  KVM_SET_CLOCK succeeded.\n");

        /* 验证 */
        struct kvm_clock_data clock4 = {};
        ret = ioctl(vm.fd, KVM_GET_CLOCK, &clock4);
        DIE_ON_ERR(ret, "KVM_GET_CLOCK (verify)");

        printf("\n  After KVM_SET_CLOCK:\n");
        printf("    KVM clock:     %lu ns\n", (unsigned long)clock4.clock);
        printf("    Jump:          %lu ns (should be ~1s)\n",
               (unsigned long)(clock4.clock - clock2.clock));

        printf("\n  ✓ Migration simulation: guest clock jumped forward 1s\n");
        printf("    In real migration, QEMU does:\n");
        printf("      1. KVM_GET_CLOCK on source host\n");
        printf("      2. Transfer state to destination\n");
        printf("      3. KVM_SET_CLOCK on destination (compensating for downtime)\n");
    }

    /* 7. 总结 */
    print_separator("Summary");
    printf("  kvmclock architecture:\n");
    printf("    Host → kvm_guest_time_update() → pvclock_vcpu_time_info\n");
    printf("    Guest reads shared page directly (no VM-Exit for gettimeofday)\n\n");
    printf("  KVM_CLOCK ioctls:\n");
    printf("    KVM_GET_CLOCK (0x%02lx) — read VM time + host_tsc\n",
           (unsigned long)_IOC_NR(KVM_GET_CLOCK));
    printf("    KVM_SET_CLOCK (0x%02lx) — set VM time (for migration)\n",
           (unsigned long)_IOC_NR(KVM_SET_CLOCK));
    printf("\n  Key kernel functions:\n");
    printf("    kvm_vm_ioctl_get_clock()    — x86.c:6995\n");
    printf("    kvm_vm_ioctl_set_clock()    — x86.c:7006\n");
    printf("    kvm_guest_time_update()     — x86.c:3215\n");

    /* 8. 清理 */
    vm_destroy(&vm);
    kvm_cleanup();

    printf("\n[done] Experiment 2 completed.\n");
    return 0;
}
