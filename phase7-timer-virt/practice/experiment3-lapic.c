/*
 * experiment3-lapic.c — LAPIC Timer 行为观察（TSC-deadline 中断投递）
 *
 * 目标: 验证 LAPIC Timer TSC-deadline 模式如何通过 VMX preemption timer
 *       在 deadline 到期时向 guest 投递中断。
 *
 * 为什么需要一个"真"guest:
 *   TSC-deadline 到期后，KVM 走 vmx_set_hv_timer() 把 delta 写进
 *   VMX_PREEMPTION_TIMER_VALUE (vmx.c:8129/7205)。preemption timer 到期
 *   触发 VM-Exit，KVM 再把这个定时器中断注入 guest。**中断要能被注入，
 *   guest 必须满足: (1) IF=1 (开中断), (2) 有可用的中断向量表入口,
 *   (3) handler 发 EOI** (否则 ISR 位不释放, 同向量的下一个中断被
 *   PPR 挡住)。一个只会 `HLT` 的实模式 guest（IF=0、无 IDT）永远收
 *   不到中断，vCPU 一 halt 就死锁在 kvm_vcpu_block()。所以本实验
 *   构造了一个最小实模式 guest：开中断 + 自旋 + 中断处理程序用
 *   `OUT` 端口退出，让用户态能"看见"每一次定时器中断。
 *
 * 实验步骤:
 *   Phase 0: KVM_CREATE_IRQCHIP + KVM_SET_CPUID2 (leaf 1 ECX[24],
 *            否则 timer_mode_mask=0, deadline MSR 写入被静默拒绝;
 *            ECX[21] X2APIC + 写 APICBASE 切 x2APIC, 给 handler 的
 *            EOI 用)
 *   Phase 1: KVM_GET_LAPIC 读取默认寄存器 (需先建 in-kernel irqchip)
 *   Phase 2: KVM_SET_LAPIC 打开 APIC (SPIV) 并把 LVTT 设为 TSC-deadline
 *   Phase 3: 写 MSR_IA32_TSC_DEADLINE, 运行 guest, 统计中断到达次数与延迟
 *
 * 关键内核代码路径:
 *   kvm_set_lapic_tscdeadline_msr() — lapic.c:2585 — 存 deadline 并启动
 *   restart_apic_timer()            — lapic.c:2200 — HW/SW 双路径选择
 *   start_hv_timer()                — lapic.c:2141 — HW preemption timer
 *   vmx_set_hv_timer()              — vmx.c:8129   — 计算 host deadline
 *   vmx_update_hv_timer()           — vmx.c:7205   — 写 VMX_PREEMPTION_TIMER_VALUE
 *
 * LAPIC Timer 寄存器 (MMIO, 通过 KVM_GET/SET_LAPIC 访问):
 *   0xF0  APIC_SPIV  — bit8 = APIC 软件使能
 *   0x320 APIC_LVTT  — bits18:17 模式 (00=one-shot,01=periodic,10=tsc-deadline)
 *                      bit16 = masked, bits7:0 = vector
 *   0x380 APIC_TMICT — 初始计数值 (periodic/one-shot 用, deadline 模式不用)
 *   0x390 APIC_TMCCT — 当前计数值 (只读)
 *   0x3E0 APIC_TDCR  — 除数配置
 *
 * 运行: sudo ./experiment3-lapic
 */

#include "common.h"
#include <time.h>

/* LAPIC 寄存器偏移与位 (来源: arch/x86/include/asm/apicdef.h) */
#define APIC_SPIV                  0xF0
#define   APIC_SPIV_APIC_ENABLED   (1 << 8)
#define APIC_LVTT                  0x320
#define   APIC_LVT_TIMER_ONESHOT    (0 << 17)
#define   APIC_LVT_TIMER_PERIODIC   (1 << 17)
#define   APIC_LVT_TIMER_TSCDEADLINE (2 << 17)
#define   APIC_LVT_MASKED           (1 << 16)
#define APIC_TMICT                 0x380
#define APIC_TMCCT                 0x390
#define APIC_TDCR                  0x3E0

#define TIMER_VECTOR   0x20       /* 定时器中断向量 */
/*
 * handler 用 OUT 端口让用户态"看见"每次中断。
 * 千万别用 0x20! in-kernel PIC (i8259) 把 0x20-0x21 / 0xa0-0xa1 / 0x4d0-4d1
 * 注册在 KVM_PIO_BUS 上 (i8259.c:612/617/621), guest 对这些端口的
 * IN/OUT 在内核里被 picdev_*_{read,write}() 就地消费、**不退出到用户态**,
 * guest 会在 KVM_RUN 里永远自旋。0xe9 (debug port) 没有内核模拟设备,
 * OUT 必然触发 KVM_EXIT_IO。
 */
#define TIMER_PORT     0xe9
#define PERIOD_MS      2          /* 每个 deadline 的目标间隔 */

/* ---- 在 kvm_lapic_state.regs[] 上按偏移读写 32 位寄存器 ---- */
static uint32_t lapic_reg_get(const struct kvm_lapic_state *s, uint32_t off)
{
    uint32_t v;
    memcpy(&v, &((const uint8_t *)s->regs)[off], sizeof(v));
    return v;
}
static void lapic_reg_set(struct kvm_lapic_state *s, uint32_t off, uint32_t v)
{
    memcpy(&((uint8_t *)s->regs)[off], &v, sizeof(v));
}
static const char *lvtt_mode_name(uint32_t lvtt)
{
    switch (lvtt & (APIC_LVT_TIMER_PERIODIC | APIC_LVT_TIMER_TSCDEADLINE)) {
    case APIC_LVT_TIMER_ONESHOT:     return "One-shot";
    case APIC_LVT_TIMER_PERIODIC:    return "Periodic";
    case APIC_LVT_TIMER_TSCDEADLINE: return "TSC-deadline";
    }
    return "Unknown";
}

/*
 * 喂一个最小 CPUID: leaf 1 带 ECX[24] (TSC_DEADLINE_TIMER) 与
 * ECX[21] (X2APIC)。
 *
 * 不设会踩一个隐蔽的坑: 本实验若不给 leaf 1 建 CPUID 条目,
 * kvm_update_cpuid() 里 best==NULL、整个 if 块不执行, timer_mode_mask
 * 保持 kzalloc 初值 0 (有 leaf1 但缺该位时则是 1<<17, cpuid.c:399-402);
 * mask=0 时 apic_update_lvtt() (lapic.c:1781) 把 LVTT 的模式位全掩掉 →
 * timer_mode 永远是 0 (one-shot), TSC_DEADLINE MSR 写入被
 * kvm_set_lapic_tscdeadline_msr() 的门挡下 (host 发起的 KVM_SET_MSRS
 * 照常返回 1, 但定时器不会臂展)。QEMU 总会设置 CPUID 所以
 * 平时看不到; 直接用 KVM API 时它是硬前提。
 *
 * X2APIC 位是给 handler 发 EOI 用的: 中断注入后 ISR[0x20] 置位, 不发
 * EOI 则同向量被 PPR 挡住 (kvm_apic_has_interrupt():
 * highest_irr <= apic_get_ppr() → 不投递), 第二个中断永远不来。
 * 实模式够不着 xAPIC MMIO (0xFEE00000 > 1MB), 只能走 x2APIC MSR
 * (WRMSR 实模式可用); 而 kvm_set_apic_base() (x86.c:675-676) 规定
 * guest CPUID 没有 X2APIC 时 APICBASE 的 X2APIC_ENABLE 位算保留位,
 * 所以这里必须一并声明。
 */
static void vcpu_setup_cpuid(struct kvm_vcpu *vcpu)
{
    struct {
        struct kvm_cpuid2 cpuid;
        struct kvm_cpuid_entry2 entries[1];
    } data = {};

    data.cpuid.nent = 1;
    data.entries[0].function = 1;
    data.entries[0].eax = 0x000606e3;
    data.entries[0].ecx = (1u << 24) |  /* X86_FEATURE_TSC_DEADLINE_TIMER */
                          (1u << 21);   /* X86_FEATURE_X2APIC */
    int ret = ioctl(vcpu->fd, KVM_SET_CPUID2, &data);
    DIE_ON_ERR(ret, "KVM_SET_CPUID2");
    printf("[cpuid] leaf 1 set, ECX[24]=TSC_DEADLINE_TIMER ECX[21]=X2APIC\n");
}

int main(void)
{
    struct kvm_vm vm = {};
    struct kvm_vcpu vcpu = {};
    int ret;

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Experiment 3: LAPIC Timer (TSC-deadline interrupt)         ║\n");
    printf("║  验证: preemption timer 到期 → KVM 注入定时器中断           ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    kvm_init();

    int cap_deadline = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_TSC_DEADLINE_TIMER);
    printf("[cap] KVM_CAP_TSC_DEADLINE_TIMER = %d\n", cap_deadline);

    /*
     * 必须先建 in-kernel IRQCHIP 再建 vCPU:
     * kvm_create_lapic() 在 !irqchip_in_kernel() 时不创建 APIC
     * (lapic.c:2904), 导致 KVM_GET/SET_LAPIC 返回 EINVAL。
     */
    vm_create(&vm, 64 * 1024);
    vm_create_irqchip(&vm);
    vcpu_create(&vm, &vcpu);
    vcpu_setup_cpuid(&vcpu);

    /*
     * 把 APIC 切到 x2APIC 模式: 默认 0xFEE00900 (BASE | BSP bit8 |
     * ENABLE bit11) 加 X2APIC_ENABLE bit10 → 0xFEE00D00。
     * 目的: 让实模式 handler 能用 WRMSR(0x80b) 发 EOI (见上文注释)。
     * 必须排在 vcpu_setup_cpuid() 之后: 没有 guest X2APIC CPUID 时,
     * kvm_set_apic_base() 把 X2APIC_ENABLE 当保留位拒掉 (x86.c:675-676)。
     * 位定义: BSP=(1<<8), X2APIC_ENABLE=(1<<10), ENABLE=(1<<11)
     * (msr-index.h:900-901, apicdef.h:153)。
     */
    vcpu_set_msr(&vcpu, 0x1b, 0xFEE00D00ULL);

    int tsc_khz = ioctl(vcpu.fd, KVM_GET_TSC_KHZ, 0);
    if (tsc_khz <= 0)
        tsc_khz = 2500000;   /* fallback: 2.5 GHz */
    printf("[tsc] virtual TSC = %d KHz\n", tsc_khz);

    /* ============ Phase 1: 读 LAPIC 默认状态 ============ */
    print_separator("Phase 1: KVM_GET_LAPIC defaults");

    struct kvm_lapic_state lapic = {};
    ret = ioctl(vcpu.fd, KVM_GET_LAPIC, &lapic);
    DIE_ON_ERR(ret, "KVM_GET_LAPIC");

    uint32_t spiv0 = lapic_reg_get(&lapic, APIC_SPIV);
    uint32_t lvtt0 = lapic_reg_get(&lapic, APIC_LVTT);
    uint32_t tmict0 = lapic_reg_get(&lapic, APIC_TMICT);
    uint32_t tdcr0 = lapic_reg_get(&lapic, APIC_TDCR);
    printf("  SPIV  = 0x%08x  (APIC enabled = %s)\n", spiv0,
           (spiv0 & APIC_SPIV_APIC_ENABLED) ? "yes" : "no");
    printf("  LVTT  = 0x%08x  mode=%s masked=%d vector=0x%02x\n", lvtt0,
           lvtt_mode_name(lvtt0), !!(lvtt0 & APIC_LVT_MASKED), lvtt0 & 0xff);
    printf("  TMICT = 0x%08x\n", tmict0);
    printf("  TDCR  = 0x%08x\n", tdcr0);

    /* ============ Phase 2: 配置为 TSC-deadline ============ */
    print_separator("Phase 2: KVM_SET_LAPIC → enable APIC + TSC-deadline");

    /* 打开 APIC (SPIV bit8)，否则 kvm_apic_present() 判为 sw-disabled */
    lapic_reg_set(&lapic, APIC_SPIV, spiv0 | APIC_SPIV_APIC_ENABLED);
    /* LVTT: TSC-deadline 模式, 不屏蔽, 向量 = TIMER_VECTOR */
    lapic_reg_set(&lapic, APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | TIMER_VECTOR);
    lapic_reg_set(&lapic, APIC_TMICT, 0);   /* deadline 模式不用 initial count */

    ret = ioctl(vcpu.fd, KVM_SET_LAPIC, &lapic);
    DIE_ON_ERR(ret, "KVM_SET_LAPIC");

    /* 读回确认 */
    struct kvm_lapic_state lapic_rb = {};
    ret = ioctl(vcpu.fd, KVM_GET_LAPIC, &lapic_rb);
    DIE_ON_ERR(ret, "KVM_GET_LAPIC (readback)");
    uint32_t lvtt_rb = lapic_reg_get(&lapic_rb, APIC_LVTT);
    uint32_t spiv_rb = lapic_reg_get(&lapic_rb, APIC_SPIV);
    printf("  readback SPIV = 0x%08x (enabled=%s)  LVTT = 0x%08x  mode=%s vector=0x%02x\n",
           spiv_rb, (spiv_rb & APIC_SPIV_APIC_ENABLED) ? "yes" : "no", lvtt_rb,
           lvtt_mode_name(lvtt_rb), lvtt_rb & 0xff);
    printf("  ✓ LAPIC configured: SPIV enabled, LVTT=TSC-deadline, vector=0x%x\n",
           TIMER_VECTOR);

    /* ============ Phase 3: 构造 guest 并观测中断 ============ */
    print_separator("Phase 3: TSC-deadline interrupt delivery");

    /*
     * Guest 内存布局 (实模式, CS.base=0, 线性=物理):
     *
     *   0x0080  IVT[0x20] = 远指针 → 0000:0x2000   (中断向量表)
     *   0x1000  主程序:  sti ; jmp $   (开中断后自旋等待)
     *   0x2000  handler: out 0xe9,al ; wrmsr(0x80b) 发 EOI ; iret
     *
     * deadline 到期 → preemption timer VM-Exit → KVM 注入向量 0x20
     * → guest 跳到 0x2000 → OUT 触发 KVM_EXIT_IO → 用户态计数。
     *
     * EOI 不能省: 中断注入置位 ISR[0x20], 不发 EOI 则同向量被
     * PPR 挡住 (kvm_apic_has_interrupt() lapic.c), 第 2 个中断永远
     * 不来。实模式够不着 xAPIC MMIO (0xFEE00000 > 1MB), 所以借
     * x2APIC MSR: WRMSR(0x80b=APIC_EOI), 内核侧走
     * kvm_x2apic_msr_write() → __kvm_apic_update_eoi() (lapic.c:3308)。
     */
    uint8_t ivt_entry[4] = { 0x00, 0x20, 0x00, 0x00 };  /* off=0x2000, seg=0 */
    memcpy((char *)vm.mem + 0x80, ivt_entry, 4);

    uint8_t handler[] = { 0xe6, TIMER_PORT,             /* out 0xe9, al       */
                          0xb9, 0x0b, 0x08, 0x00, 0x00, /* mov ecx, 0x80b     */
                          0x31, 0xc0,                   /* xor eax, eax       */
                          0x31, 0xd2,                   /* xor edx, edx       */
                          0x0f, 0x30,                   /* wrmsr  → EOI       */
                          0xcf };                       /* iret               */
    memcpy((char *)vm.mem + 0x2000, handler, sizeof(handler));

    uint8_t main_code[] = { 0xfb,             /* sti  */
                            0xeb, 0xfe };     /* jmp $ (自旋) */
    load_guest_code(&vm, &vcpu, main_code, sizeof(main_code), 0x1000);

    uint64_t delta_cycles = (uint64_t)tsc_khz * PERIOD_MS;  /* khz * ms = cycles */
    printf("  deadline delta = %lu cycles (~%d ms)\n\n",
           (unsigned long)delta_cycles, PERIOD_MS);

    int rounds = 5;
    int total_irqs = 0;
    for (int r = 0; r < rounds; r++) {
        uint64_t tsc_now = vcpu_get_tsc(&vcpu);
        uint64_t deadline = tsc_now + delta_cycles;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        vcpu_set_msr(&vcpu, MSR_IA32_TSC_DEADLINE, deadline);

        /* 诊断: 读回 deadline — 内核 kvm_get_lapic_tscdeadline_msr() 与写入
         * 共用同一道门 (!kvm_apic_present || !apic_lvtt_tscdeadline → 返回 0)。
         * 顺带读 APICBASE (0x1b) 确认 HW-enable 位。 */
        uint64_t dl_rb = vcpu_get_msr(&vcpu, MSR_IA32_TSC_DEADLINE);
        uint64_t apic_base = vcpu_get_msr(&vcpu, 0x1b);
        if (r == 0)
            printf("  [diag] deadline readback=0x%lx apic_base=0x%lx\n",
                   (unsigned long)dl_rb, (unsigned long)apic_base);

        /* 运行直到 handler 的 OUT 退出 (= 一次中断被投递) */
        int got = 0;
        for (int it = 0; it < 1000 && !got; it++) {
            ret = ioctl(vcpu.fd, KVM_RUN, 0);
            if (ret < 0 && errno != EINTR)
                DIE("KVM_RUN: %s", strerror(errno));
            if (vcpu.run->exit_reason == KVM_EXIT_IO &&
                vcpu.run->io.direction == KVM_EXIT_IO_OUT &&
                vcpu.run->io.port == TIMER_PORT) {
                got = 1;
                total_irqs++;
            } else if (vcpu.run->exit_reason == KVM_EXIT_INTERNAL_ERROR) {
                DIE("KVM internal error sub=%d", vcpu.run->internal.suberror);
            } else if (vcpu.run->exit_reason == KVM_EXIT_SHUTDOWN) {
                DIE("guest triple-fault");
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

        printf("  round %d: deadline=%lu  interrupt %s  latency=%.3f ms\n",
               r + 1, (unsigned long)deadline, got ? "delivered" : "TIMEOUT",
               ms);
        if (!got)
            break;
    }

    printf("\n  ✓ %d/%d timer interrupts delivered (~%d ms apart)\n",
           total_irqs, rounds, PERIOD_MS);
    printf("    deadline 到期 → preemption timer VM-Exit → KVM 注入中断。\n");
    printf("    每轮只需 1 次 deadline 编程 (本实验经用户态 KVM_SET_MSRS 写入,\n");
    printf("    vCPU 未在运行, 不产生 VM-Exit); one-shot 模式则每个周期都要\n");
    printf("    由 guest 重写 TMICT (一次 MMIO 写 = 一次 VM-Exit)。\n");

    /* 总结 */
    print_separator("Summary");
    printf("  LAPIC Timer 模式 (LVTT bits 18:17):\n");
    printf("    One-shot=00  Periodic=01  TSC-deadline=10 (via MSR 0x6E0)\n\n");
    printf("  TSC-deadline 硬件加速链:\n");
    printf("    wrmsr(TSC_DEADLINE) → kvm_set_lapic_tscdeadline_msr() lapic.c:2585\n");
    printf("    → start_hv_timer() lapic.c:2141 → vmx_set_hv_timer() vmx.c:8129\n");
    printf("    → vmx_update_hv_timer() vmx.c:7205 写 VMX_PREEMPTION_TIMER_VALUE\n");
    printf("    → preemption timer 到期 → KVM 注入向量 0x%02x\n\n", TIMER_VECTOR);
    printf("  关键前提: guest 必须 IF=1、有 IDT 入口, 且 handler 发 EOI,\n");
    printf("  否则中断投递不了 (halt 的 vCPU 死锁在 kvm_vcpu_block(),\n");
    printf("  或不发 EOI 时同向量被 PPR 挡住)。另外必须先 KVM_SET_CPUID2\n");
    printf("  声明 TSC_DEADLINE_TIMER, 否则(不建 leaf1 条目时)timer_mode_mask=0,\n");
    printf("  deadline MSR 写入被拒 (cpuid.c:399-402)。\n");

    vcpu_destroy(&vcpu);
    vm_destroy(&vm);
    kvm_cleanup();

    printf("\n[done] Experiment 3 completed.\n");
    return 0;
}
