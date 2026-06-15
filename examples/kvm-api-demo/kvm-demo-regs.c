/*
 * ============================================================================
 * KVM 寄存器操作演示 (简化版)
 * ============================================================================
 *
 * 这个程序是 kvm-demo.c 的精简版，专注于展示:
 *   1. KVM_SET_REGS / KVM_GET_REGS — 通用寄存器的读写
 *   2. KVM_SET_SREGS / KVM_GET_SREGS — 特殊寄存器(段寄存器)的读写
 *   3. 观察 Guest 执行前后寄存器的变化
 *
 * 编译: gcc -Wall -o kvm-demo-regs kvm-demo-regs.c
 * 运行: sudo ./kvm-demo-regs
 *
 * 约 200 行代码，聚焦寄存器操作。
 *
 * 【内核中的寄存器存储】
 *
 *   KVM 中，vCPU 的寄存器存储在两个地方:
 *
 *   1) 内存中: vcpu->arch.regs[] 数组
 *      → 包含 16 个通用寄存器 (RAX, RBX, ..., RIP, RFLAGS)
 *      → KVM_SET_REGS 写入这里
 *      → VM-Exit 后从 VMCS 读回这里
 *      → 定义: arch/x86/include/asm/kvm_host.h
 *        enum kvm_reg { VCPU_REGS_RAX=0, ..., NR_VCPU_REGS }
 *
 *   2) VMCS 中: GUEST_RAX, GUEST_RBX, ..., GUEST_RIP
 *      → VM-Entry 前从 vcpu->arch.regs[] 写入 VMCS
 *      → VM-Exit 后从 VMCS 读回 vcpu->arch.regs[]
 *      → 由 vmx_vcpu_run() 中的 vmcs_writel/vmcs_readl 完成
 *
 *   特殊寄存器 (段寄存器, CR0, CR3 等):
 *     → KVM_SET_SREGS 直接写入 VMCS (通过 vmx_set_cr0 等函数)
 *     → 也有对应的缓存: vcpu->arch.cr0, vcpu->arch.cr2 等
 *     → 段寄存器: vmx->segment_cache
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define GUEST_MEM_SIZE  (2 * 1024 * 1024)
#define CODE_GPA        0x1000

/*
 * 打印通用寄存器的辅助函数
 *
 * 在 KVM_SET_REGS / KVM_GET_REGS 中使用的 kvm_regs 结构体
 * 定义在 include/uapi/linux/kvm.h:
 *
 *   struct kvm_regs {
 *       __u64 rax, rbx, rcx, rdx;
 *       __u64 rsi, rdi, rsp, rbp;
 *       __u64 r8,  r9,  r10, r11;
 *       __u64 r12, r13, r14, r15;
 *       __u64 rip, rflags;
 *   };
 *
 * 对应 x86 的 16 个 64 位通用寄存器 + RIP + RFLAGS
 */
static void print_regs(const char *label, struct kvm_regs *regs)
{
    printf("--- %s ---\n", label);
    printf("  RAX=0x%016llx  RBX=0x%016llx\n",
           (unsigned long long)regs->rax, (unsigned long long)regs->rbx);
    printf("  RCX=0x%016llx  RDX=0x%016llx\n",
           (unsigned long long)regs->rcx, (unsigned long long)regs->rdx);
    printf("  RSI=0x%016llx  RDI=0x%016llx\n",
           (unsigned long long)regs->rsi, (unsigned long long)regs->rdi);
    printf("  RSP=0x%016llx  RBP=0x%016llx\n",
           (unsigned long long)regs->rsp, (unsigned long long)regs->rbp);
    printf("  RIP=0x%016llx  RFLAGS=0x%016llx\n",
           (unsigned long long)regs->rip, (unsigned long long)regs->rflags);
    printf("  R8 =0x%016llx  R9 =0x%016llx\n",
           (unsigned long long)regs->r8,  (unsigned long long)regs->r9);
    printf("  R10=0x%016llx  R11=0x%016llx\n",
           (unsigned long long)regs->r10, (unsigned long long)regs->r11);
    printf("  R12=0x%016llx  R13=0x%016llx\n",
           (unsigned long long)regs->r12, (unsigned long long)regs->r13);
    printf("  R14=0x%016llx  R15=0x%016llx\n",
           (unsigned long long)regs->r14, (unsigned long long)regs->r15);
}

/*
 * 打印段寄存器的辅助函数
 *
 * kvm_segment 结构体对应 VMCS 中的一个段描述符:
 *
 *   struct kvm_segment {
 *       __u64 base;       // 段基址
 *       __u32 limit;      // 段限制
 *       __u16 selector;   // 段选择子
 *       __u8  type;       // 段类型 (4位)
 *       __u8  present;    // 存在位
 *       __u8  dpl;        // 描述符特权级
 *       __u8  db;         // 默认操作大小 (0=16位, 1=32位)
 *       __u8  s;          // 段类型 (0=系统, 1=代码/数据)
 *       __u8  l;          // 长模式标志
 *       __u8  g;          // 粒度 (0=字节, 1=4KB)
 *       __u8  unusable;   // 不可用标志
 *       __u8  padding;
 *   };
 *
 * 这些字段最终映射到 VMCS 的:
 *   GUEST_CS_BASE, GUEST_CS_LIMIT, GUEST_CS_AR_BYTES 等
 * AR = Access Rights，将 type/s/dpl/present/db/s/l/g 打包为一个 32 位值
 */
static void print_segment(const char *name, struct kvm_segment *seg)
{
    printf("  %s: sel=0x%04x base=0x%016llx limit=0x%08x "
           "type=%d s=%d dpl=%d present=%d db=%d l=%d g=%d\n",
           name, seg->selector,
           (unsigned long long)seg->base, seg->limit,
           seg->type, seg->s, seg->dpl, seg->present,
           seg->db, seg->l, seg->g);
}

static void print_sregs(const char *label, struct kvm_sregs *sregs)
{
    printf("--- %s ---\n", label);
    printf("  CR0=0x%llx  CR2=0x%llx  CR3=0x%llx  CR4=0x%llx\n",
           (unsigned long long)sregs->cr0, (unsigned long long)sregs->cr2,
           (unsigned long long)sregs->cr3, (unsigned long long)sregs->cr4);
    printf("  EFER=0x%llx\n", (unsigned long long)sregs->efer);
    print_segment("CS", &sregs->cs);
    print_segment("DS", &sregs->ds);
    print_segment("ES", &sregs->es);
    print_segment("FS", &sregs->fs);
    print_segment("GS", &sregs->gs);
    print_segment("SS", &sregs->ss);
}

int main(void)
{
    int kvm_fd, vm_fd, vcpu_fd;
    void *guest_mem;
    struct kvm_run *run;
    struct kvm_regs regs_before, regs_after;
    struct kvm_sregs sregs_before, sregs_after;
    int mmap_size, ret;

    printf("============================================================\n");
    printf("  KVM 寄存器操作演示\n");
    printf("  重点: KVM_SET_REGS / KVM_GET_REGS / KVM_SET_SREGS\n");
    printf("============================================================\n\n");

    /* ---- 1. 基础设置: 打开 KVM, 创建 VM, 分配内存 ---- */

    kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0) die("open /dev/kvm");

    ret = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (ret != KVM_API_VERSION) {
        fprintf(stderr, "API 版本不匹配: %d != %d\n", ret, KVM_API_VERSION);
        return 1;
    }
    printf("[步骤1] /dev/kvm 打开, API 版本 = %d\n", ret);

    vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0) die("KVM_CREATE_VM");
    printf("[步骤2] VM 创建成功, vm_fd = %d\n", vm_fd);

    /* 设置 x86 需要的身份映射和 TSS */
    ret = ioctl(vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000UL);
    if (ret < 0) die("KVM_SET_TSS_ADDR");

    unsigned long id_addr = 0xfffbe000UL;
    ret = ioctl(vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &id_addr);
    if (ret < 0) die("KVM_SET_IDENTITY_MAP_ADDR");

    /* 分配 Guest 内存 */
    guest_mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (guest_mem == MAP_FAILED) die("mmap guest mem");

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = GUEST_MEM_SIZE,
        .userspace_addr = (uint64_t)(unsigned long)guest_mem,
    };
    ret = ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
    if (ret < 0) die("KVM_SET_USER_MEMORY_REGION");
    printf("[步骤3] Guest 内存: GPA=0x0, 大小=%dKB\n", GUEST_MEM_SIZE / 1024);

    /*
     * 加载 Guest 代码
     *
     * 这次的代码比 kvm-demo.c 更丰富:
     *   mov $0x42, %al     ; AL = 0x42 (只设置低 8 位)
     *   mov $0x100, %bx    ; BX = 0x0100
     *   add %al, %bl       ; BL = BL + AL = 0x00 + 0x42 = 0x42
     *   hlt                ; 停机
     *
     * 机器码:
     *   b0 42       mov $0x42, %al
     *   66 bb 00 01 mov $0x0100, %bx   (66 是操作数大小前缀，表示 16 位操作数)
     *   00 c3       add %al, %bl
     *   f4          hlt
     */
    uint8_t *code = (uint8_t *)guest_mem + CODE_GPA;
    code[0] = 0xb0; code[1] = 0x42;              /* mov $0x42, %al */
    code[2] = 0x66; code[3] = 0xbb;              /* mov $0x0100, %bx */
    code[4] = 0x00; code[5] = 0x01;
    code[6] = 0x00; code[7] = 0xc3;              /* add %al, %bl */
    code[8] = 0xf4;                              /* hlt */
    printf("[步骤4] Guest 代码加载到 GPA=0x%x (8 字节)\n", CODE_GPA);

    /* ---- 2. 创建 vCPU 并映射 kvm_run ---- */

    vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0) die("KVM_CREATE_VCPU");
    printf("[步骤5] vCPU 创建成功, vcpu_fd = %d\n", vcpu_fd);

    mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) die("KVM_GET_VCPU_MMAP_SIZE");

    run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
               MAP_SHARED, vcpu_fd, 0);
    if (run == MAP_FAILED) die("mmap kvm_run");

    /* ---- 3. 设置寄存器 ---- */
    printf("\n[步骤6] 设置寄存器\n");

    /*
     * 先获取当前特殊寄存器的默认值
     *
     * KVM_GET_SREGS 内核路径:
     *   kvm_arch_vcpu_ioctl_get_sregs() [x86.c]
     *     → kvm_x86_get_sregs(vcpu, sregs)
     *       → vmx_get_segment() 对每个段寄存器:
     *         → vmread(GUEST_CS_SELECTOR)
     *         → vmread(GUEST_CS_BASE)
     *         → vmread(GUEST_CS_LIMIT)
     *         → vmread(GUEST_CS_AR_BYTES)
     *       → vmx_get_cr0() → vmcs_readl(CR0_READ_SHADOW)
     *       → vmx_get_cr4() → vmcs_readl(GUEST_CR4)
     */
    ret = ioctl(vcpu_fd, KVM_GET_SREGS, &sregs_before);
    if (ret < 0) die("KVM_GET_SREGS");
    print_sregs("KVM_GET_SREGS (默认值)", &sregs_before);

    /*
     * 修改段寄存器为实模式
     *
     * 然后写回:
     * KVM_SET_SREGS 内核路径:
     *   kvm_arch_vcpu_ioctl_set_sregs() [x86.c]
     *     → kvm_x86_set_sregs(vcpu, sregs)
     *       → vmx_set_cr0() → vmcs_writel(CR0_READ_SHADOW, cr0)
     *                         → vmx_set_cr4() → vmcs_writel(GUEST_CR4, cr4)
     *       → kvm_set_segment() 对每个段:
     *         → vmx_set_segment()
     *           → vmcs_writel(GUEST_CS_BASE, base)
     *           → vmcs_write32(GUEST_CS_LIMIT, limit)
     *           → vmcs_write16(GUEST_CS_SELECTOR, selector)
     *           → vmcs_write32(GUEST_CS_AR_BYTES, ar)
     */
    struct kvm_sregs sregs = sregs_before;
    memset(&sregs.cs, 0, sizeof(sregs.cs));
    sregs.cs.selector = 0;
    sregs.cs.base = 0;
    sregs.cs.limit = 0xFFFF;
    sregs.cs.type = 3;
    sregs.cs.s = 1;
    sregs.cs.dpl = 0;
    sregs.cs.present = 1;
    sregs.cs.db = 0;
    sregs.cs.g = 0;

    sregs.ds = sregs.cs;
    sregs.es = sregs.cs;
    sregs.fs = sregs.cs;
    sregs.gs = sregs.cs;
    sregs.ss = sregs.cs;
    sregs.ds.type = 3;  /* 数据段: 读写 */
    sregs.es.type = 3;
    sregs.fs.type = 3;
    sregs.gs.type = 3;
    sregs.ss.type = 3;

    sregs.cr0 = 0;
    sregs.efer = 0;

    ret = ioctl(vcpu_fd, KVM_SET_SREGS, &sregs);
    if (ret < 0) die("KVM_SET_SREGS");
    printf("\n  (已将 CS/DS/ES/FS/GS/SS 设为实模式, CR0=0)\n");

    /*
     * 设置通用寄存器
     *
     * 我们故意将 RAX 设为 0，让 Guest 代码来修改它
     *
     * KVM_SET_REGS 内核路径:
     *   kvm_arch_vcpu_ioctl_set_regs() [x86.c]
     *     → memcpy(vcpu->arch.regs, &regs->..., sizeof)
     *     → 注意: 这里只是写入内存缓存，不直接写 VMCS
     *     → VMCS 的 GUEST_RIP 等在 vmx_vcpu_run() 中写入
     */
    struct kvm_regs regs = {0};
    regs.rip = CODE_GPA;
    regs.rflags = 0x2;    /* 保留位 */
    regs.rax = 0;         /* 清零 RAX，让 Guest 代码修改 */

    ret = ioctl(vcpu_fd, KVM_SET_REGS, &regs);
    if (ret < 0) die("KVM_SET_REGS");

    /* 运行前读取寄存器状态 */
    ret = ioctl(vcpu_fd, KVM_GET_REGS, &regs_before);
    if (ret < 0) die("KVM_GET_REGS");
    print_regs("KVM_GET_REGS (运行前)", &regs_before);

    /* ---- 4. 运行 vCPU ---- */
    printf("\n[步骤7] 运行 vCPU (KVM_RUN)...\n");
    printf("  Guest 将执行:\n");
    printf("    mov $0x42, %%al     → AL 应变为 0x42\n");
    printf("    mov $0x100, %%bx    → BX 应变为 0x0100\n");
    printf("    add %%al, %%bl      → BL = BL + AL = 0x42\n");
    printf("    hlt                 → 触发 VM-Exit\n\n");

    ret = ioctl(vcpu_fd, KVM_RUN, 0);
    if (ret < 0 && errno != EAGAIN) die("KVM_RUN");

    /* ---- 5. 检查退出原因 ---- */
    printf("[步骤8] VM-Exit 处理\n");
    printf("  exit_reason = %u ", run->exit_reason);
    if (run->exit_reason == KVM_EXIT_HLT)
        printf("(KVM_EXIT_HLT - 预期退出)\n");
    else
        printf("(非预期退出!)\n");

    /* ---- 6. 运行后读取寄存器 ---- */

    /*
     * KVM_GET_REGS: 读取运行后的寄存器
     *
     * 内核路径:
     *   kvm_arch_vcpu_ioctl_get_regs() [x86.c]
     *     → memcpy(&regs->rax, vcpu->arch.regs[VCPU_REGS_RAX], ...)
     *     → ... 拷贝所有通用寄存器
     *
     * 注意: 这些值来自 vcpu->arch.regs[]，
     * 是在 VM-Exit 后由 vmx_vcpu_run() 从 VMCS 读回并缓存的:
     *   vcpu->arch.regs[VCPU_REGS_RAX] = vmcs_readl(GUEST_RAX)
     *   ... 其他寄存器同理
     *
     * 所以 KVM_GET_REGS 返回的是 Guest 执行完毕后的最终状态。
     */
    ret = ioctl(vcpu_fd, KVM_GET_REGS, &regs_after);
    if (ret < 0) die("KVM_GET_REGS");
    printf("\n");
    print_regs("KVM_GET_REGS (运行后)", &regs_after);

    /* 分析变化 */
    printf("\n[步骤9] 寄存器变化分析\n");
    printf("  RAX: 0x%016llx → 0x%016llx",
           (unsigned long long)regs_before.rax,
           (unsigned long long)regs_after.rax);
    if (regs_after.rax == 0x42)
        printf("  ← 'mov $0x42, %%al' 生效! AL=0x42");
    printf("\n");

    printf("  RBX: 0x%016llx → 0x%016llx",
           (unsigned long long)regs_before.rbx,
           (unsigned long long)regs_after.rbx);
    if ((regs_after.rbx & 0xFFFF) == 0x0142)
        printf("  ← BX=0x0100, add %%al,%%bl → BL=0x42, 合成 0x0142");
    printf("\n");

    printf("  RIP: 0x%016llx → 0x%016llx",
           (unsigned long long)regs_before.rip,
           (unsigned long long)regs_after.rip);
    printf("  ← 前进 %llu 字节 (9条指令字节)\n",
           (unsigned long long)(regs_after.rip - regs_before.rip));

    /* 再次获取段寄存器，确认没有变化 */
    ret = ioctl(vcpu_fd, KVM_GET_SREGS, &sregs_after);
    if (ret < 0) die("KVM_GET_SREGS");
    printf("\n");
    print_sregs("KVM_GET_SREGS (运行后)", &sregs_after);

    /* ---- 7. 清理 ---- */
    printf("\n[清理] 关闭文件描述符\n");
    munmap(run, mmap_size);
    munmap(guest_mem, GUEST_MEM_SIZE);
    close(vcpu_fd);
    close(vm_fd);
    close(kvm_fd);

    printf("\n============================================================\n");
    printf("  演示完成!\n");
    printf("\n");
    printf("  关键观察:\n");
    printf("  - KVM_SET_REGS 写入 vcpu->arch.regs[] (内存缓存)\n");
    printf("  - KVM_GET_REGS 从 vcpu->arch.regs[] 读取\n");
    printf("  - VM-Entry 前: regs[] → VMCS (vmcs_writel)\n");
    printf("  - VM-Exit 后:  VMCS → regs[] (vmcs_readl)\n");
    printf("  - KVM_SET_SREGS 直接写 VMCS (vmx_set_cr0 等)\n");
    printf("  - KVM_GET_SREGS 直接读 VMCS (vmx_get_segment 等)\n");
    printf("============================================================\n");

    return 0;
}
