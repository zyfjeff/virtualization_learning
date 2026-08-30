/*
 * common.h — Phase 6 时钟虚拟化实验的共享辅助函数
 *
 * 提供 KVM VM/vCPU 创建、guest 代码执行、TSC 读取等基础功能。
 *
 * 参考:
 *   - phase5-vfio/practice/vfio-claim-trace.c (VFIO 实验的类似模式)
 *   - Linux kernel Documentation/virt/kvm/api.rst
 *
 * 注意: x86 上 rip 在 struct kvm_regs (KVM_GET/SET_REGS) 中，
 *       不在 struct kvm_sregs (段寄存器) 中。
 */

#ifndef __KVM_TIMER_COMMON_H
#define __KVM_TIMER_COMMON_H

#include <asm/kvm.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

/* MSR 定义 — 系统头文件可能不全 */
#ifndef MSR_IA32_TSC
#define MSR_IA32_TSC            0x00000010
#endif
#ifndef MSR_IA32_TSC_DEADLINE
#define MSR_IA32_TSC_DEADLINE   0x000006e0
#endif

/* ========== 错误处理 ========== */

#define DIE(fmt, ...) do { \
    fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__); \
    exit(1); \
} while (0)

#define DIE_ON_ERR(ret, msg) do { \
    if (ret < 0) \
        DIE("%s: %s (ret=%d)", msg, strerror(errno), ret); \
} while (0)

/* ========== KVM FD 管理 ========== */

static int kvm_fd = -1;

static inline void kvm_init(void)
{
    kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0)
        DIE("open /dev/kvm: %s", strerror(errno));

    int api_ver = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (api_ver != 12)
        DIE("KVM API version = %d, expected 12", api_ver);

    printf("[kvm] /dev/kvm opened, API version = %d\n", api_ver);
}

static inline void kvm_cleanup(void)
{
    if (kvm_fd >= 0)
        close(kvm_fd);
}

/* ========== VM 创建 ========== */

struct kvm_vm {
    int fd;
    void *mem;          /* guest 物理内存 (mmap) */
    size_t mem_size;
};

static inline void vm_create(struct kvm_vm *vm, size_t mem_size)
{
    vm->fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    DIE_ON_ERR(vm->fd, "KVM_CREATE_VM");

    vm->mem_size = mem_size;
    vm->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm->mem == MAP_FAILED)
        DIE("mmap guest memory: %s", strerror(errno));

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = mem_size,
        .userspace_addr = (uintptr_t)vm->mem,
    };
    int ret = ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, &region);
    DIE_ON_ERR(ret, "KVM_SET_USER_MEMORY_REGION");

    printf("[vm] created, mem_size = %zu KB\n", mem_size / 1024);
}

/*
 * 创建 in-kernel IRQCHIP (LAPIC + IOAPIC)。
 *
 * 必须在 vcpu_create() 之前调用: 否则 kvm_create_lapic() 因
 * !irqchip_in_kernel() 直接返回而不创建 APIC (lapic.c:2904),
 * 导致 KVM_GET_LAPIC / KVM_SET_LAPIC 返回 EINVAL。
 */
static inline void vm_create_irqchip(struct kvm_vm *vm)
{
    int ret = ioctl(vm->fd, KVM_CREATE_IRQCHIP, 0);
    DIE_ON_ERR(ret, "KVM_CREATE_IRQCHIP");
    printf("[vm] in-kernel irqchip created (LAPIC + IOAPIC)\n");
}

static inline void vm_destroy(struct kvm_vm *vm)
{
    if (vm->mem && vm->mem != MAP_FAILED)
        munmap(vm->mem, vm->mem_size);
    if (vm->fd >= 0)
        close(vm->fd);
}

/* ========== vCPU 创建 ========== */

struct kvm_vcpu {
    int fd;
    struct kvm_run *run;
    size_t run_size;
};

static inline void vcpu_create(struct kvm_vm *vm, struct kvm_vcpu *vcpu)
{
    vcpu->fd = ioctl(vm->fd, KVM_CREATE_VCPU, 0);
    DIE_ON_ERR(vcpu->fd, "KVM_CREATE_VCPU");

    int mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    DIE_ON_ERR(mmap_size, "KVM_GET_VCPU_MMAP_SIZE");
    vcpu->run_size = (size_t)mmap_size;

    void *run_mmap = mmap(NULL, vcpu->run_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, vcpu->fd, 0);
    if (run_mmap == MAP_FAILED)
        DIE("mmap kvm_run: %s", strerror(errno));
    vcpu->run = (struct kvm_run *)run_mmap;

    printf("[vcpu] created, run_size = %zu\n", vcpu->run_size);
}

static inline void vcpu_destroy(struct kvm_vcpu *vcpu)
{
    if (vcpu->run && vcpu->run != MAP_FAILED)
        munmap(vcpu->run, vcpu->run_size);
    if (vcpu->fd >= 0)
        close(vcpu->fd);
}

/* ========== Guest 代码加载 ========== */

/*
 * 将机器码加载到 guest 物理地址
 * 并设置初始寄存器状态 (实模式, CS:IP = 0x0000:guest_addr)
 *
 * 注意: rip 通过 KVM_SET_REGS 设置 (struct kvm_regs)
 *       段寄存器通过 KVM_SET_SREGS 设置 (struct kvm_sregs)
 */
static inline void load_guest_code(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
                            const uint8_t *code, size_t code_len,
                            uint64_t guest_addr)
{
    if (guest_addr + code_len > vm->mem_size)
        DIE("code too large for guest memory");

    memcpy((char *)vm->mem + guest_addr, code, code_len);

    /* 设置段寄存器: 实模式 */
    struct kvm_sregs sregs = {};
    int ret = ioctl(vcpu->fd, KVM_GET_SREGS, &sregs);
    DIE_ON_ERR(ret, "KVM_GET_SREGS");

    sregs.cs.base = 0;
    sregs.cs.selector = 0;

    ret = ioctl(vcpu->fd, KVM_SET_SREGS, &sregs);
    DIE_ON_ERR(ret, "KVM_SET_SREGS");

    /* 设置通用寄存器: rip 指向代码入口 */
    struct kvm_regs regs = {};
    ret = ioctl(vcpu->fd, KVM_GET_REGS, &regs);
    DIE_ON_ERR(ret, "KVM_GET_REGS");

    regs.rip = guest_addr;
    regs.rflags = 0x2;  /* 保留位，必须为 1 */

    ret = ioctl(vcpu->fd, KVM_SET_REGS, &regs);
    DIE_ON_ERR(ret, "KVM_SET_REGS");

    printf("[load] %zu bytes loaded at guest PA 0x%lx\n",
           code_len, (unsigned long)guest_addr);
}

/*
 * 设置 vCPU 的 RIP (用于重新执行 guest 代码)
 */
static inline void vcpu_set_rip(struct kvm_vcpu *vcpu, uint64_t rip)
{
    struct kvm_regs regs = {};
    int ret = ioctl(vcpu->fd, KVM_GET_REGS, &regs);
    DIE_ON_ERR(ret, "KVM_GET_REGS");

    regs.rip = rip;

    ret = ioctl(vcpu->fd, KVM_SET_REGS, &regs);
    DIE_ON_ERR(ret, "KVM_SET_REGS");
}

/* ========== MSR 操作 ========== */

static inline uint64_t vcpu_get_msr(struct kvm_vcpu *vcpu, uint32_t msr_index)
{
    struct {
        struct kvm_msrs info;
        struct kvm_msr_entry entries[1];
    } msr_data = {};

    msr_data.info.nmsrs = 1;
    msr_data.info.entries[0].index = msr_index;

    int ret = ioctl(vcpu->fd, KVM_GET_MSRS, &msr_data);
    if (ret < 0)
        DIE("KVM_GET_MSRS(0x%x): %s", msr_index, strerror(errno));
    if (ret != 1)
        DIE("KVM_GET_MSRS(0x%x): returned %d entries", msr_index, ret);

    return msr_data.info.entries[0].data;
}

static inline void vcpu_set_msr(struct kvm_vcpu *vcpu, uint32_t msr_index, uint64_t data)
{
    struct {
        struct kvm_msrs info;
        struct kvm_msr_entry entries[1];
    } msr_data = {};

    msr_data.info.nmsrs = 1;
    msr_data.info.entries[0].index = msr_index;
    msr_data.info.entries[0].data = data;

    int ret = ioctl(vcpu->fd, KVM_SET_MSRS, &msr_data);
    if (ret < 0)
        DIE("KVM_SET_MSRS(0x%x): %s", msr_index, strerror(errno));
    if (ret != 1)
        DIE("KVM_SET_MSRS(0x%x): wrote %d entries", msr_index, ret);
}

/* ========== KVM_RUN 辅助 ========== */

/*
 * 执行 KVM_RUN 直到 exit_reason 为 HLT 或超过 max_exits 次退出
 * 返回实际的 exit 次数
 */
static inline int run_until_hlt(struct kvm_vcpu *vcpu, int max_exits)
{
    int exits = 0;

    while (exits < max_exits) {
        int ret = ioctl(vcpu->fd, KVM_RUN, 0);
        if (ret < 0 && errno != EINTR)
            DIE("KVM_RUN: %s", strerror(errno));

        exits++;

        switch (vcpu->run->exit_reason) {
        case KVM_EXIT_HLT:
            printf("[run] HLT after %d exits\n", exits);
            return exits;

        case KVM_EXIT_IO:
            /* 可选: 处理 PIO (此处忽略) */
            break;

        case KVM_EXIT_INTERNAL_ERROR:
            DIE("KVM internal error: suberror=%d",
                vcpu->run->internal.suberror);

        case KVM_EXIT_SHUTDOWN:
            DIE("Triple fault (Shutdown)");

        default:
            DIE("Unexpected exit_reason = %d", vcpu->run->exit_reason);
        }
    }

    printf("[run] max_exits (%d) reached\n", max_exits);
    return exits;
}

/* ========== TSC 辅助 ========== */

/* 读取 host TSC (inline assembly) */
static inline uint64_t host_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 通过 KVM API 获取 vCPU 可见的 TSC */
static inline uint64_t vcpu_get_tsc(struct kvm_vcpu *vcpu)
{
    return vcpu_get_msr(vcpu, MSR_IA32_TSC);
}

/* ========== 时钟辅助 ========== */

static inline uint64_t vm_get_clock(int vm_fd)
{
    struct kvm_clock_data data = {};
    int ret = ioctl(vm_fd, KVM_GET_CLOCK, &data);
    if (ret < 0)
        DIE("KVM_GET_CLOCK: %s", strerror(errno));
    return data.clock;
}

static inline void vm_set_clock(int vm_fd, uint64_t clock)
{
    struct kvm_clock_data data = {};
    data.clock = clock;
    int ret = ioctl(vm_fd, KVM_SET_CLOCK, &data);
    if (ret < 0)
        DIE("KVM_SET_CLOCK: %s", strerror(errno));
}

/* ========== 打印辅助 ========== */

static inline void print_separator(const char *title)
{
    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("  %s\n", title);
    printf("═══════════════════════════════════════════════════════════════\n\n");
}

#endif /* __KVM_TIMER_COMMON_H */
