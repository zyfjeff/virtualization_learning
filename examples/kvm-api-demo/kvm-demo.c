/*
 * ============================================================================
 * KVM API 完整生命周期演示程序
 * ============================================================================
 *
 * 本程序是一个教育性质的示例，展示如何通过用户空间 ioctl 接口与 KVM 交互，
 * 创建一个最小化的虚拟机并运行一段 Guest 代码。
 *
 * 编译: gcc -Wall -o kvm-demo kvm-demo.c
 * 运行: sudo ./kvm-demo   (需要 /dev/kvm 读写权限)
 *
 * 作者: KVM 深度学习项目
 * 参考: linux-6.12.93 内核源码
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

/* ============================================================================
 * 宏定义和辅助函数
 * ============================================================================ */

/* 错误处理宏：如果条件为真，打印错误信息并退出 */
#define KVM_CHECK(ret, msg)                                     \
    do {                                                        \
        if ((ret) < 0) {                                        \
            fprintf(stderr, "[错误] %s: %s (errno=%d)\n",      \
                    msg, strerror(errno), errno);               \
            exit(EXIT_FAILURE);                                 \
        }                                                       \
    } while (0)

/* 打印信息宏 */
#define KVM_INFO(fmt, ...)                                      \
    fprintf(stdout, "[信息] " fmt "\n", ##__VA_ARGS__)

/* Guest 内存布局常量 */
#define GUEST_MEM_SIZE    (2 * 1024 * 1024)   /* 2 MB Guest 内存 */
#define GUEST_CODE_GPA    0x1000              /* 代码加载的物理地址 (GPA) */

/* ============================================================================
 * 第一步：打开 /dev/kvm 并检查 API 版本
 * ============================================================================
 *
 * 【内核路径】
 *   用户空间: open("/dev/kvm", O_RDWR)
 *     → 字符设备打开 → kvm_dev_ioctl()
 *
 *   对应的内核代码:
 *     virt/kvm/kvm_main.c::kvm_dev_ioctl()
 *       └→ KVM_GET_API_VERSION: 返回 KVM_API_VERSION (目前为 12)
 *
 *   KVM_API_VERSION = 12 是一个长期稳定的版本号，从 KVM 早期到现在
 *   一直保持兼容。如果版本不匹配，用户空间程序应该拒绝继续。
 * ============================================================================ */
static int open_kvm(void)
{
    int kvm_fd;
    int api_ver;

    KVM_INFO("===== 第一步：打开 /dev/kvm =====");

    /*
     * 打开 KVM 字符设备。
     * /dev/kvm 是 KVM 模块注册的全局字符设备 (misc device)。
     *
     * 在内核中：
     *   virt/kvm/kvm_main.c 中通过 misc_register() 注册了 /dev/kvm
     *   其 ioctl 处理函数是 kvm_dev_ioctl()
     */
    kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    KVM_CHECK(kvm_fd, "无法打开 /dev/kvm，请确保 kvm_intel 或 kvm_amd 模块已加载");

    /*
     * KVM_GET_API_VERSION ioctl
     *
     * 内核处理路径:
     *   kvm_dev_ioctl() [kvm_main.c]
     *     → case KVM_GET_API_VERSION: return KVM_API_VERSION;
     *
     * 这是最简单的 ioctl，直接返回一个整数常量。
     * 它的作用类似于版本握手——确保用户空间和内核空间的 KVM 接口兼容。
     */
    api_ver = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    KVM_CHECK(api_ver, "KVM_GET_API_VERSION 失败");

    KVM_INFO("KVM API 版本: %d (期望值: %d)", api_ver, KVM_API_VERSION);

    if (api_ver != KVM_API_VERSION) {
        fprintf(stderr, "[错误] API 版本不匹配! 内核=%d, 期望=%d\n",
                api_ver, KVM_API_VERSION);
        close(kvm_fd);
        exit(EXIT_FAILURE);
    }

    /*
     * 可选：检查 KVM 扩展能力
     *
     * KVM_CHECK_EXTENSION ioctl 用于查询 KVM 支持的各种特性。
     *
     * 内核路径:
     *   kvm_dev_ioctl() [kvm_main.c]
     *     → kvm_dev_ioctl_check_extension()
     *       → kvm_vm_ioctl_check_extension()
     *         → 根据 ext 编号返回对应的能力值
     *
     * 常见的 extension:
     *   KVM_CAP_IRQCHIP      - 是否支持中断控制器模拟
     *   KVM_CAP_USER_MEMORY  - 是否支持 KVM_SET_USER_MEMORY_REGION
     *   KVM_CAP_IOEVENTFD    - 是否支持 ioeventfd
     *   KVM_CAP_IMMEDIATE_EXIT - 是否支持 immediate_exit 标志
     */
    int has_user_memory = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
    KVM_INFO("KVM_CAP_USER_MEMORY: %s", has_user_memory ? "支持" : "不支持");

    int has_immediate_exit = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT);
    KVM_INFO("KVM_CAP_IMMEDIATE_EXIT: %s", has_immediate_exit ? "支持" : "不支持");

    KVM_INFO("✓ /dev/kvm 打开成功，API 版本确认\n");
    return kvm_fd;
}

/* ============================================================================
 * 第二步：创建虚拟机 (VM)
 * ============================================================================
 *
 * 【内核路径】
 *   用户空间: ioctl(kvm_fd, KVM_CREATE_VM, 0)
 *     → kvm_dev_ioctl() [kvm_main.c]
 *       → kvm_dev_ioctl_create_vm()
 *         → kvm_create_vm()
 *           → kvm_arch_init_vm()    [x86 架构初始化]
 *           → kvm_init_mmu()        [MMU 初始化]
 *           → kvm_alloc_vp_memory() [虚拟处理器内存]
 *
 *   返回值是一个新的文件描述符 (VM fd)，后续所有 VM 级别的操作
 *   都通过这个 fd 进行。
 *
 *   每个 VM 在内核中对应一个 struct kvm 实例，它包含：
 *     - 内存区域列表 (memory slots)
 *     - vCPU 列表
 *     - MMU 相关结构
 *     - 中断路由表
 *     - 架构特定数据 (kvm->arch)
 * ============================================================================ */
static int create_vm(int kvm_fd)
{
    int vm_fd;

    KVM_INFO("===== 第二步：创建虚拟机 =====");

    /*
     * KVM_CREATE_VM ioctl
     *
     * 参数是一个 flags 值 (通常为 0)。在较老的内核中，这个参数
     * 曾被用作 VM 类型，但在新版本中已废弃，始终传 0。
     *
     * 内核中 kvm_create_vm() 的关键操作:
     *
     *   1) 分配 struct kvm 结构体
     *      → kzalloc(sizeof(struct kvm), GFP_KERNEL_ACCOUNT)
     *
     *   2) 初始化 VM 的各种锁和链表
     *      → spin_lock_init(&kvm->mmu_lock)
     *      → INIT_LIST_HEAD(&kvm->vm_list)  // vCPU 链表
     *
     *   3) 架构初始化
     *      → kvm_arch_init_vm(kvm, type)
     *        → 在 x86 上：初始化 EPT、中断控制器等
     *
     *   4) 创建匿名 inode 文件
     *      → anon_inode_getfile("kvm-vm", &kvm_vm_fops, kvm, O_RDWR)
     *      → 这就是返回给用户空间的文件描述符
     *
     *   5) 安装到进程的文件描述符表
     *      → fd_install(fd, file)
     *
     * 注意：kvm_create_vm() 还调用了 kvm_arch_post_init_vm()
     *   → vmx_vm_init() → 初始化 VMX 相关的 VM 级别结构
     */
    vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    KVM_CHECK(vm_fd, "KVM_CREATE_VM 失败");

    KVM_INFO("VM 创建成功，vm_fd = %d", vm_fd);
    KVM_INFO("(内核中对应 struct kvm *kvm 实例)\n");

    return vm_fd;
}

/* ============================================================================
 * 第三步：设置 x86 身份映射地址和 TSS
 * ============================================================================
 *
 * 这两个 ioctl 是 x86 特有的，用于设置实模式下的身份映射地址
 * 和任务状态段 (TSS)。
 *
 * 在实模式下，CPU 需要一段"身份映射"的内存区域（虚拟地址 = 物理地址）
 * 来处理某些内部操作（如实模式下的中断向量表访问）。
 *
 * TSS (Task State Segment) 在 VMX non-root 模式下用于硬件任务切换。
 * 即使我们不使用硬件任务切换，VMX 仍然要求一个有效的 TSS 地址。
 *
 * 【内核路径】
 *   KVM_SET_IDENTITY_MAP_ADDR:
 *     → kvm_arch_vm_ioctl() [x86.c]
 *       → case KVM_SET_IDENTITY_MAP_ADDR
 *         → kvm_x86_set_identity_map_addr()
 *
 *   KVM_SET_TSS_ADDR:
 *     → kvm_arch_vm_ioctl() [x86.c]
 *       → case KVM_SET_TSS_ADDR
 *         → static_call(kvm_x86_set_tss_addr)(kvm, addr)
 *           → vmx_set_tss_addr() [vmx/vmx.c]
 *             → 在内部创建 TSS 的身份映射
 * ============================================================================ */
static void setup_identity_map(int vm_fd)
{
    KVM_INFO("===== 第三步：设置身份映射地址和 TSS =====");

    /*
     * 身份映射地址：通常设为 0xfffbe000 (一个不会被 Guest 正常使用的地址)
     *
     * 这个地址用于实模式下 CPU 内部操作的身份映射。
     * 在 VMX 模式下，某些实模式操作需要 CPU 访问特定的内存地址，
     * KVM 需要为这些地址建立身份映射 (GPA = HVA)。
     *
     * 内核中，这会触发:
     *   kvm->arch.ept_identity_map_addr = addr;
     *   后续在 VM-Entry 时，如果 EPT 未映射此地址，
     *   会触发 EPT violation，KVM 会自动建立映射。
     */
    unsigned long identity_addr = 0xfffbe000UL;
    int ret = ioctl(vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &identity_addr);
    KVM_CHECK(ret, "KVM_SET_IDENTITY_MAP_ADDR 失败");
    KVM_INFO("身份映射地址设为: 0x%lx", identity_addr);

    /*
     * TSS 地址：设为 0xfffbd000
     *
     * TSS 在实模式下仍然需要存在，因为 VMX 硬件在 VM-Entry/VM-Exit
     * 过程中可能需要访问 TSS。
     *
     * 内核中 vmx_set_tss_addr() 会：
     *   1) 记录 TSS 的 GPA
     *   2) 在后续第一次 VM-Entry 前，为 TSS 建立身份映射
     *   3) 在 VMCS 中设置相关字段
     *
     * 注意：这两个地址必须在同一个 4KB 页面边界上，
     * 并且不能与 Guest 的其他内存映射冲突。
     */
    unsigned long tss_addr = 0xfffbd000UL;
    ret = ioctl(vm_fd, KVM_SET_TSS_ADDR, tss_addr);
    KVM_CHECK(ret, "KVM_SET_TSS_ADDR 失败");
    KVM_INFO("TSS 地址设为: 0x%lx", tss_addr);

    KVM_INFO("✓ x86 身份映射和 TSS 设置完成\n");
}

/* ============================================================================
 * 第四步：为 Guest 分配内存
 * ============================================================================
 *
 * 【内核路径】
 *   ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region)
 *     → kvm_vm_ioctl() [kvm_main.c]
 *       → kvm_vm_ioctl_set_memory_region()
 *         → kvm_set_memory_region()
 *           → kvm_set_memslot()
 *             → kvm_arch_commit_memory_region()
 *               → kvm_mmu_init_vm() [如果是第一次设置]
 *
 *   这个 ioctl 告诉 KVM：用户空间的某段内存对应 Guest 的某段物理地址。
 *   之后 Guest 访问该物理地址时，KVM 会将访问重定向到用户空间映射的内存。
 *
 *   数据结构 kvm_userspace_memory_region:
 *     slot          - 内存槽编号 (0-31)，每个 VM 最多 32 个 slot
 *     flags         - 标志位 (KVM_MEM_LOG_DIRTY_PAGES 等)
 *     guest_phys_addr - Guest 物理地址 (GPA)，Guest "看到"的地址
 *     memory_size   - 内存大小
 *     userspace_addr - 用户空间虚拟地址 (HVA)，Host 上的实际地址
 *
 *   内存映射关系:
 *     Guest GPA ──→ Host HVA ──→ Host 物理页 (通过 Host 页表)
 *
 *   当 Guest 访问 GPA 时:
 *     1) 如果 EPT 已映射 → 直接访问 (快速路径)
 *     2) 如果 EPT 未映射 → EPT Violation → KVM 处理
 *        → 查找 memslot → 找到对应的 HVA
 *        → 分配/映射物理页 → 更新 EPT 页表
 *        → 重新执行指令
 * ============================================================================ */
static void *setup_guest_memory(int vm_fd)
{
    void *mem;
    struct kvm_userspace_memory_region region;
    int ret;

    KVM_INFO("===== 第四步：为 Guest 分配内存 =====");

    /*
     * 使用 mmap 分配一块匿名内存。
     * MAP_SHARED | MAP_ANONYMOUS 确保：
     *   - 内存被物理分页（不交换出去之前不会分配实际物理页）
     *   - 可以被 KVM 内核代码通过用户空间地址访问
     *
     * 注意：这里使用 MAP_SHARED 而不是 MAP_PRIVATE，
     * 因为 KVM 需要通过 get_user_pages() 获取这些页面的物理地址，
     * MAP_PRIVATE 的写时复制 (CoW) 语义会导致问题。
     */
    mem = mmap(NULL, GUEST_MEM_SIZE,
               PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS,
               -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap 失败");
        exit(EXIT_FAILURE);
    }

    KVM_INFO("用户空间内存映射: HVA=%p, 大小=%d KB",
             mem, GUEST_MEM_SIZE / 1024);

    /*
     * 设置 KVM 内存区域
     *
     * 这个 ioctl 建立了 GPA → HVA 的映射关系:
     *   Guest 物理地址 0 ──→ 用户空间地址 mem
     *   大小: 2MB
     *
     * 内核中 kvm_set_memory_region() 的详细流程:
     *
     *   1) 参数验证
     *      → 检查 slot 编号、地址对齐、不重叠等
     *
     *   2) 创建新的 memslot
     *      → kvm_prepare_memory_region()
     *        → 填充 struct kvm_memory_slot 结构
     *        → 记录 base_gfn, npages, userspace_addr 等
     *
     *   3) 提交变更
     *      → kvm_commit_memory_region()
     *        → kvm_arch_commit_memory_region()
     *          → 在 x86 上可能触发 EPT 更新
     *
     * 重要：此时只是"注册"了内存区域，实际的 EPT 页表条目
     * 是在 Guest 第一次访问时通过 EPT violation 按需建立的。
     * 这就是所谓的 "demand paging"（按需分页）。
     */
    memset(&region, 0, sizeof(region));
    region.slot            = 0;                        /* 使用 slot 0 */
    region.flags           = 0;                        /* 无特殊标志 */
    region.guest_phys_addr = 0;                        /* Guest GPA 从 0 开始 */
    region.memory_size     = GUEST_MEM_SIZE;           /* 2 MB */
    region.userspace_addr  = (uint64_t)(unsigned long)mem; /* 用户空间地址 */

    ret = ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
    KVM_CHECK(ret, "KVM_SET_USER_MEMORY_REGION 失败");

    KVM_INFO("KVM 内存区域: GPA=0x0 → HVA=%p, 大小=%d KB",
             mem, GUEST_MEM_SIZE / 1024);
    KVM_INFO("(此时 EPT 尚未建立映射，Guest 首次访问时触发 EPT Violation)\n");

    return mem;
}

/* ============================================================================
 * 第五步：加载 Guest 代码
 * ============================================================================
 *
 * 我们直接在 Guest 的物理内存中写入 x86 机器码。
 * Guest 将在实模式 (Real Mode) 下运行。
 *
 * 实模式下的地址计算:
 *   物理地址 = 段基址 × 16 + 偏移量
 *
 * 在实模式下，CS 段基址由 CS 寄存器左移 4 位得到。
 * 如果我们设置 CS = 0, IP = 0x1000，那么:
 *   物理地址 = 0 × 16 + 0x1000 = 0x1000
 *
 * Guest 代码（x86 实模式）:
 *   mov $0x42, %al    ; 将 0x42 加载到 AL 寄存器
 *   hlt               ; 停机，触发 VM-Exit (KVM_EXIT_HLT)
 * ============================================================================ */
static void load_guest_code(void *guest_mem)
{
    uint8_t *code = (uint8_t *)guest_mem + GUEST_CODE_GPA;

    KVM_INFO("===== 第五步：加载 Guest 代码 =====");
    KVM_INFO("Guest 代码将放置在 GPA=0x%x", GUEST_CODE_GPA);

    /*
     * 编写 x86 机器码
     *
     * 指令序列:
     *   b8 42 00    mov $0x0042, %ax   ; AX = 0x42 (操作数: 立即数 → AX)
     *   f4          hlt                ; 停机指令
     *
     * 机器码详解:
     *   0xb8 = MOV r16, imm16 (操作码)
     *   0x42, 0x00 = 立即数 0x0042 (小端序)
     *   0xf4 = HLT 指令
     *
     * 当 Guest 执行 HLT 时:
     *   CPU 进入停机状态 → 触发 VM-Exit
     *   VM-Exit 原因: "HLT instruction" (exit_reason = 12)
     *   KVM 将退出原因映射为 KVM_EXIT_HLT 返回给用户空间
     *
     * 在内核中，HLT 的 VM-Exit 处理路径:
     *   vmx_handle_exit() [vmx/vmx.c]
     *     → handle_halt() [vmx/vmx.c]
     *       → kvm_emulate_halt() [x86.c]
     *         → vcpu->run->exit_reason = KVM_EXIT_HLT
     *         → return 0  // 退出 vcpu_run() 循环
     */
    code[0] = 0xb8;   /* MOV AX, imm16 */
    code[1] = 0x42;   /* 低字节 */
    code[2] = 0x00;   /* 高字节 */
    code[3] = 0xf4;   /* HLT */

    KVM_INFO("Guest 代码 (hex):");
    KVM_INFO("  %02x %02x %02x  → mov $0x0042, %%ax",
             code[0], code[1], code[2]);
    KVM_INFO("  %02x          → hlt", code[3]);
    KVM_INFO("✓ Guest 代码加载完成\n");
}

/* ============================================================================
 * 第六步：创建 vCPU
 * ============================================================================
 *
 * 【内核路径】
 *   ioctl(vm_fd, KVM_CREATE_VCPU, 0)
 *     → kvm_vm_ioctl() [kvm_main.c]
 *       → kvm_vm_ioctl_create_vcpu()
 *         → kvm_arch_vcpu_create()
 *           → kvm_x86_vcpu_create()
 *             → vmx_create_vcpu() [vmx/vmx.c]
 *               → 分配 struct vcpu_vmx
 *               → vmx_vcpu_setup() → 初始化 VMCS
 *               → 分配 VMXON region
 *               → 分配 VMCS
 *
 *   返回的 vCPU fd 是 VM fd 的"子文件"，用于：
 *     - KVM_RUN: 运行 vCPU
 *     - KVM_GET_REGS/KVM_SET_REGS: 读写通用寄存器
 *     - KVM_GET_SREGS/KVM_SET_SREGS: 读写段寄存器
 *     - KVM_GET_VCPU_EVENTS: 获取中断/异常状态
 *
 *   每个 vCPU 在内核中对应一个:
 *     struct kvm_vcpu  ← 通用 vCPU 结构 (include/linux/kvm_host.h)
 *       + struct vcpu_vmx  ← VMX 扩展 (arch/x86/kvm/vmx/vmx.h)
 *
 *   vCPU 也绑定了一个线程——KVM_RUN ioctl 是阻塞的，
 *   在 VM-Exit 需要用户空间处理时才返回。
 * ============================================================================ */
static int create_vcpu(int vm_fd)
{
    int vcpu_fd;

    KVM_INFO("===== 第六步：创建 vCPU =====");

    /*
     * KVM_CREATE_VCPU ioctl
     * 参数: vCPU 的 ID (从 0 开始)
     *
     * 内核中 vmx_create_vcpu() 的关键操作:
     *
     *   1) 分配 vcpu_vmx 结构体
     *      → vcpu_vmx = kzalloc(sizeof(struct vcpu_vmx), GFP_KERNEL_ACCOUNT)
     *      → 这个结构体包含了通用 kvm_vcpu + VMX 特有字段
     *
     *   2) 分配 VMXON 区域
     *      → vmx->vmx_region = alloc_page()
     *      → VMXON 区域是 CPU 进入 VMX 模式时需要的内存区域
     *
     *   3) 分配 VMCS (Virtual Machine Control Structure)
     *      → vmx->vmcs01.vmcs = alloc_page()
     *      → vmcs01 是 L1 (当前) VMCS，嵌套虚拟化还有 vmcs02
     *
     *   4) 初始化 VMCS 字段
     *      → vmx_vcpu_setup(vmx)
     *        → vmwrite(GUEST_CR0, ...)
     *        → vmwrite(GUEST_CS_SELECTOR, ...)
     *        → vmwrite(HOST_CR0, ...)
     *        → ... 设置所有 Guest/Host 状态和 VM-Execution 控制字段
     *
     *   5) 将 vCPU 添加到 VM 的 vCPU 链表
     *      → list_add(&vcpu->list, &kvm->vcpus)
     *
     *   6) 创建 vCPU 文件描述符
     *      → anon_inode_getfile("kvm-vcpu", &kvm_vcpu_fops, vcpu, O_RDWR)
     *
     * struct vcpu_vmx 的关键字段 (vmx.h):
     *   vcpu          - 基类 kvm_vcpu
     *   vmcs01        - L1 VMCS (当前 VMCS)
     *   loaded_vmcs   - 当前加载到 CPU 上的 VMCS
     *   vpid          - Virtual Processor ID (避免 TLB flush)
     *   pi_desc       - Posted Interrupt 描述符
     *   msr_autoload  - 自动加载的 MSR 列表
     * ============================================================================ */
    vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    KVM_CHECK(vcpu_fd, "KVM_CREATE_VCPU 失败");

    KVM_INFO("vCPU 创建成功，vcpu_fd = %d", vcpu_fd);
    KVM_INFO("(内核中对应 struct kvm_vcpu + struct vcpu_vmx)");
    KVM_INFO("  包含: VMCS, VMXON区域, vCPU状态等\n");

    return vcpu_fd;
}

/* ============================================================================
 * 第七步：映射 kvm_run 结构体
 * ============================================================================
 *
 * 【内核路径】
 *   ioctl(vcpu_fd, KVM_GET_VCPU_MMAP_SIZE, 0)
 *     → kvm_vcpu_ioctl() [kvm_main.c]
 *       → return sizeof(struct kvm_run) (对齐到页面大小)
 *
 *   mmap(NULL, mmap_size, PROT_READ|PROT_WRITE, MAP_SHARED, vcpu_fd, 0)
 *     → kvm_vcpu_mmap() [kvm_main.c]
 *       → 返回 vcpu->run 对应的页面
 *
 *   kvm_run 结构体是用户空间和内核空间之间通信的"共享内存"。
 *   当 VM-Exit 发生时，内核将退出信息写入这个结构体，
 *   用户空间从这里读取退出原因和相关数据。
 *
 *   这种设计避免了每次 VM-Exit 都需要 copy_to_user 的开销。
 * ============================================================================ */
static struct kvm_run *map_vcpu_run(int kvm_fd, int vcpu_fd)
{
    int mmap_size;
    struct kvm_run *run;

    KVM_INFO("===== 第七步：映射 kvm_run 结构体 =====");

    /*
     * 首先查询 kvm_run 结构体需要多大的 mmap 区域。
     * 这个大小通常是一个页面 (4KB) 或更大，
     * 因为 kvm_run 后面可能跟着一些可变长度的数据区域。
     *
     * 注意：KVM_GET_VCPU_MMAP_SIZE 必须在 kvm_fd (/dev/kvm) 上调用，
     * 而非 vcpu_fd。它由 kvm_dev_ioctl() 处理，返回 sizeof(struct kvm_run)。
     */
    mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    KVM_CHECK(mmap_size, "KVM_GET_VCPU_MMAP_SIZE 失败");
    KVM_INFO("kvm_run mmap 大小: %d 字节", mmap_size);

    /*
     * 将 vCPU fd mmap 到用户空间。
     *
     * 内核中 kvm_vcpu_mmap():
     *   → 返回 vcpu->run 所在的页面
     *   → vcpu->run 在 vcpu_create() 时分配:
     *     vcpu->run = vzalloc(sizeof(struct kvm_run))
     *
     * MAP_SHARED 确保用户空间和内核对同一块内存的修改互相可见。
     * 这比 ioctl 传递数据高效得多——零拷贝通信。
     */
    run = mmap(NULL, mmap_size,
               PROT_READ | PROT_WRITE,
               MAP_SHARED,
               vcpu_fd, 0);
    if (run == MAP_FAILED) {
        perror("mmap kvm_run 失败");
        exit(EXIT_FAILURE);
    }

    KVM_INFO("kvm_run 映射到用户空间: %p", run);
    KVM_INFO("(用户空间和内核空间通过这块共享内存通信，零拷贝)\n");

    return run;
}

/* ============================================================================
 * 第八步：设置 vCPU 寄存器状态
 * ============================================================================
 *
 * 在实模式下运行 Guest，我们需要设置:
 *   - RIP = 代码起始地址 (0x1000)
 *   - CS = 0 (段基址 = 0)
 *   - RFLAGS = 0x2 (bit 1 始终为 1，这是 x86 硬件要求)
 *
 * 【内核路径】
 *   KVM_SET_REGS:
 *     → kvm_vcpu_ioctl() [kvm_main.c]
 *       → kvm_arch_vcpu_ioctl_set_regs()
 *         → vcpu->arch.regs[VCPU_REGS_RIP] = regs.rip
 *         → ... 设置其他通用寄存器
 *
 *   KVM_SET_SREGS:
 *     → kvm_vcpu_ioctl() [kvm_main.c]
 *       → kvm_arch_vcpu_ioctl_set_sregs()
 *         → kvm_x86_set_sregs(vcpu, sregs)
 *           → vmx_set_cr0()   → 写入 VMCS 的 GUEST_CR0
 *           → vmx_set_cr4()   → 写入 VMCS 的 GUEST_CR4
 *           → kvm_set_segment() → 写入 VMCS 的 GUEST_CS/DS/ES...
 *           → vmx_decache_cr0_guest_bits()
 *           → vmx_refresh_apicv_exec_ctrls()
 *
 *   这些寄存器值最终被写入 VMCS 的 Guest State Area。
 *   当执行 VM-Entry 时，CPU 从 VMCS 的 Guest State 恢复这些值。
 * ============================================================================ */
static void setup_vcpu_registers(int vcpu_fd)
{
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    int ret;

    KVM_INFO("===== 第八步：设置 vCPU 寄存器 =====");

    /*
     * 获取当前特殊寄存器 (段寄存器等) 的默认值
     * 然后修改为我们需要的值。
     *
     * KVM_GET_SREGS 内核路径:
     *   → kvm_arch_vcpu_ioctl_get_sregs()
     *     → kvm_x86_get_sregs(vcpu, sregs)
     *       → vmx_get_segment() → vmread(GUEST_CS_SELECTOR) 等
     */
    ret = ioctl(vcpu_fd, KVM_GET_SREGS, &sregs);
    KVM_CHECK(ret, "KVM_GET_SREGS 失败");

    /*
     * 实模式下的段寄存器设置
     *
     * 实模式的特征:
     *   - CR0.PE = 0 (Protection Enable = 0)
     *   - 段基址 = selector << 4
     *   - 没有分页 (CR0.PG = 0)
     *
     * CS 段:
     *   selector = 0 → base = 0
     *   所以 CS:IP = 0:0x1000 → 物理地址 0x1000
     */
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    sregs.cs.limit = 0xFFFF;      /* 实模式段限制 = 64KB */
    sregs.cs.type = 3;             /* 可读可执行 (Real Mode) */
    sregs.cs.s = 1;                /* 代码/数据段 (非系统段) */
    sregs.cs.dpl = 0;              /* 特权级 0 */
    sregs.cs.present = 1;          /* 段存在 */
    sregs.cs.db = 0;               /* 16位段 */
    sregs.cs.g = 0;                /* 粒度 = 字节 */

    /* 数据段: DS = ES = FS = GS = SS */
    sregs.ds.base = 0;
    sregs.ds.selector = 0;
    sregs.ds.limit = 0xFFFF;
    sregs.ds.type = 3;             /* 可读可写 */
    sregs.ds.s = 1;
    sregs.ds.dpl = 0;
    sregs.ds.present = 1;

    sregs.es = sregs.ds;
    sregs.fs = sregs.ds;
    sregs.gs = sregs.ds;
    sregs.ss = sregs.ds;

    /* 实模式: CR0.PE = 0, CR0.PG = 0 */
    sregs.cr0 = 0;

    /*
     * EFER (Extended Feature Enable Register)
     * 实模式下不需要启用长模式或兼容模式，所以设为 0。
     * 如果要运行 64 位 Guest，需要设置 EFER.LME = 1 和 EFER.LMA = 1。
     */
    sregs.efer = 0;

    ret = ioctl(vcpu_fd, KVM_SET_SREGS, &sregs);
    KVM_CHECK(ret, "KVM_SET_SREGS 失败");

    KVM_INFO("特殊寄存器已设置 (实模式: CR0=0, CS.base=0, CS.sel=0)");

    /*
     * 设置通用寄存器
     *
     * RIP = 0x1000 → 代码起始地址
     * RFLAGS = 0x2 → bit 1 是 reserved bit，硬件要求始终为 1
     */
    memset(&regs, 0, sizeof(regs));
    regs.rip = GUEST_CODE_GPA;         /* 代码入口点 */
    regs.rflags = 0x2;                  /* 保留位必须为 1 */

    /*
     * KVM_SET_REGS
     *
     * 内核路径:
     *   kvm_arch_vcpu_ioctl_set_regs()
     *     → vcpu->arch.regs[VCPU_REGS_RIP] = regs.rip
     *     → vcpu->arch.regs[VCPU_REGS_RSP] = regs.rsp
     *     → ... 复制所有 16 个通用寄存器
     *
     * 注意：这些值被存储在 vcpu->arch.regs[] 数组中，
     * 不是直接写入 VMCS。在 VM-Entry 之前，
     * vmx_vcpu_run() 会将它们写入 VMCS 的 GUEST_RIP 等字段。
     *
     * 或者在 vmx_load_mmu_context() / prepare_vmcs02() 时
     * 通过 vmcs_writel(GUEST_RIP, regs[VCPU_REGS_RIP]) 写入。
     */
    ret = ioctl(vcpu_fd, KVM_SET_REGS, &regs);
    KVM_CHECK(ret, "KVM_SET_REGS 失败");

    KVM_INFO("通用寄存器已设置: RIP=0x%x, RFLAGS=0x%llx",
             GUEST_CODE_GPA, (unsigned long long)regs.rflags);
    KVM_INFO("✓ vCPU 寄存器配置完成\n");
}

/* ============================================================================
 * 第九步：运行 vCPU
 * ============================================================================
 *
 * 【内核路径】 - 这是最核心的调用！
 *
 *   ioctl(vcpu_fd, KVM_RUN, 0)
 *     → kvm_vcpu_ioctl() [kvm_main.c]
 *       → kvm_arch_vcpu_ioctl_run() [x86.c]
 *         → vcpu_run(vcpu)  ← ★ 主循环
 *           → while (1):
 *             → kvm_check_request()  // 检查待处理的请求
 *             → vcpu_enter_guest(vcpu)  ← ★ 进入 Guest
 *               → xfer_to_guest_mode()  // 信号处理等
 *               → kvm_x86_prepare_switch_mmu()
 *               → static_call(kvm_x86_run)(vcpu)
 *                 → vmx_vcpu_run(vcpu) [vmx/vmx.c]  ← ★★★
 *
 *                   vmx_vcpu_run() 的详细流程:
 *
 *                   1) 加载 VMCS 到 CPU
 *                      → vmcs_load(vmx->loaded_vmcs->vmcs)
 *                        → __vmcs_load() → VMCLEAR + VMPTRLD
 *
 *                   2) 将 vCPU 寄存器写入 VMCS Guest State
 *                      → vmcs_writel(GUEST_RIP, vcpu->arch.regs[RIP])
 *                      → vmcs_writel(GUEST_RSP, vcpu->arch.regs[RSP])
 *                      → ...
 *
 *                   3) 检查是否需要处理中断注入
 *                      → vmx_inject_irq()
 *
 *                   4) 执行 VM-Entry!
 *                      → vmx_vcpu_enter_exit(vcpu, vmx, true)
 *                        → __vmx_vcpu_run() [vmenter.S]  ← 汇编!
 *                          → 保存 Host 寄存器
 *                          → vmcs_writel(HOST_RIP, return地址)
 *                          → vmcs_writel(HOST_RSP, Host栈顶)
 *                          → vmx_vmenter()
 *                            → VMRESUME 或 VMLAUNCH 指令!!!
 *
 *                      === CPU 进入 VMX Non-Root Mode ===
 *                      === Guest 代码开始执行 ===
 *
 *                   5) VM-Exit 发生!
 *                      === CPU 回到 VMX Root Mode ===
 *                      → __vmx_vcpu_run() 返回
 *
 *                   6) 从 VMCS 读取 Guest 寄存器
 *                      → vcpu->arch.regs[RIP] = vmcs_readl(GUEST_RIP)
 *                      → ...
 *
 *                   7) 处理 VM-Exit
 *                      → vmx_handle_exit(vcpu, exit_reason)
 *                        → 根据 exit_reason 分发到对应的处理函数
 *
 *             → 检查是否需要退出到用户空间
 *               → 如果 exit_reason 需要用户空间处理 → break
 *               → 否则继续 while 循环 (内部 VM-Exit 处理完毕)
 *
 *   对于我们的程序:
 *     Guest 执行 MOV + HLT
 *     HLT 触发 VM-Exit (exit_reason = 12)
 *     vmx_handle_exit() → handle_halt()
 *       → 设置 vcpu->run->exit_reason = KVM_EXIT_HLT
 *       → 返回 0 → 退出 vcpu_run() 循环
 *     KVM_RUN ioctl 返回到用户空间
 * ============================================================================ */
static void run_vcpu(int vcpu_fd, struct kvm_run *run)
{
    int ret;

    KVM_INFO("===== 第九步：运行 vCPU =====");
    KVM_INFO("执行 KVM_RUN ioctl...");
    KVM_INFO("(CPU 即将执行 VM-Entry，进入 VMX Non-Root Mode)");
    KVM_INFO("(Guest 将执行: MOV AX, 0x42; HLT)");
    KVM_INFO("");

    /*
     * KVM_RUN ioctl - 让 vCPU 开始执行 Guest 代码
     *
     * 这是一个阻塞调用：
     *   - 进入后，CPU 执行 VM-Entry，开始运行 Guest
     *   - Guest 代码执行直到发生 VM-Exit
     *   - 如果 VM-Exit 需要用户空间处理，ioctl 返回
     *   - 如果 VM-Exit 可以由内核处理，继续执行 Guest
     *
     * 参数通常为 0。如果设置了 immediate_exit 标志
     * (run->immediate_exit = 1)，则不执行 Guest 直接返回，
     * 用于信号处理等场景。
     */
    ret = ioctl(vcpu_fd, KVM_RUN, 0);
    if (ret < 0 && errno != EAGAIN) {
        perror("KVM_RUN 失败");
        exit(EXIT_FAILURE);
    }

    /*
     * KVM_RUN 返回后，检查退出原因
     */
    KVM_INFO("KVM_RUN 返回！VM-Exit 发生。");
    KVM_INFO("");
}

/* ============================================================================
 * 第十步：处理 VM-Exit
 * ============================================================================
 *
 * kvm_run 结构体的 exit_reason 字段告诉我们为什么从 Guest 退出。
 *
 * 常见的退出原因:
 *   KVM_EXIT_UNKNOWN       (0)  - 未知原因
 *   KVM_EXIT_EXCEPTION     (1)  - 异常 (需要用户空间处理)
 *   KVM_EXIT_IO            (2)  - I/O 端口访问 (in/out 指令)
 *   KVM_EXIT_HLT           (5)  - HLT 指令
 *   KVM_EXIT_MMIO          (6)  - MMIO 访问
 *   KVM_EXIT_SHUTDOWN      (8)  - 三重错误，VM 需要重启
 *   KVM_EXIT_INTERNAL_ERROR(17) - KVM 内部错误
 *   KVM_EXIT_SYSTEM_EVENT  (24) - 系统事件 (关机/重启)
 *
 * 在内核中，exit_reason 从 VMCS 的 EXIT_REASON 字段读取，
 * 经过 KVM 的 exit handler 处理后，转换为用户空间的枚举值。
 *
 * 对应关系 (部分):
 *   VMX EXIT_REASON_HLT (12) → KVM_EXIT_HLT (5)
 *   VMX EXIT_REASON_IO_INSTRUCTION (30) → KVM_EXIT_IO (2)
 *   VMX EXIT_REASON_EPT_VIOLATION (48) → 内核处理，不返回用户空间
 * ============================================================================ */
static void handle_exit(struct kvm_run *run)
{
    KVM_INFO("===== 第十步：处理 VM-Exit =====");
    KVM_INFO("");

    KVM_INFO("退出原因 (exit_reason) = %u", run->exit_reason);

    switch (run->exit_reason) {
    case KVM_EXIT_HLT:
        /*
         * KVM_EXIT_HLT: Guest 执行了 HLT 指令
         *
         * 这是我们的预期退出原因。
         * 内核处理路径:
         *   vmx_handle_exit()
         *     → kvm_emulate_halt()
         *       → kvm_vcpu_halt() 或 kvm_vcpu_block()
         *         → 设置 exit_reason = KVM_EXIT_HLT
         *
         * 注意：如果启用了 halt-polling (halt_poll_ns > 0)，
         * KVM 会先自旋等待一段时间，看是否有中断到来。
         * 如果在轮询期间中断到来，就不需要真正地 halt。
         * 这减少了频繁 halt/wakeup 的开销。
         */
        KVM_INFO("→ KVM_EXIT_HLT: Guest 执行了 HLT 指令!");
        KVM_INFO("  这说明 Guest 代码已成功执行到 HLT 指令。");
        KVM_INFO("  虚拟机正常退出。");
        break;

    case KVM_EXIT_IO:
        /*
         * KVM_EXIT_IO: Guest 尝试访问 I/O 端口
         *
         * 如果 Guest 使用了 in/out 指令访问未映射的端口，
         * 会触发这个退出。用户空间需要模拟 I/O 操作。
         *
         * run->io 结构体包含:
         *   direction - 读 (KVM_EXIT_IO_IN) 或写 (KVM_EXIT_IO_OUT)
         *   port      - I/O 端口号
         *   size      - 数据大小 (1/2/4 字节)
         *   count     - 指令重复次数
         *   data_offset - 数据在 kvm_run 后的偏移
         */
        KVM_INFO("→ KVM_EXIT_IO: Guest 尝试 I/O 端口访问");
        KVM_INFO("  方向: %s, 端口: 0x%x, 大小: %d",
                 run->io.direction ? "OUT" : "IN",
                 run->io.port, run->io.size);
        break;

    case KVM_EXIT_INTERNAL_ERROR:
        KVM_INFO("→ KVM_EXIT_INTERNAL_ERROR: KVM 内部错误!");
        KVM_INFO("  suberror: %u", run->internal.suberror);
        break;

    case KVM_EXIT_SHUTDOWN:
        KVM_INFO("→ KVM_EXIT_SHUTDOWN: 三重错误，VM 需要重启");
        break;

    case KVM_EXIT_UNKNOWN:
        KVM_INFO("→ KVM_EXIT_UNKNOWN: 未知退出原因");
        KVM_INFO("  hardware_exit_reason: 0x%llx",
                 (unsigned long long)run->hw.hardware_exit_reason);
        break;

    default:
        KVM_INFO("→ 其他退出原因: %u", run->exit_reason);
        break;
    }

    KVM_INFO("");
    KVM_INFO("✓ VM-Exit 处理完成\n");
}

/* ============================================================================
 * 清理函数
 * ============================================================================
 *
 * 关闭所有文件描述符。
 *
 * 在 Linux 中，KVM 资源的生命周期绑定在文件描述符上:
 *   - 关闭 vCPU fd → 内核释放 vCPU 资源 (kvm_vcpu_destroy)
 *   - 关闭 VM fd → 内核释放 VM 资源 (kvm_destroy_vm)
 *     → kvm_destroy_vm()
 *       → kvm_arch_destroy_vm()
 *       → kvm_free_irq_routing()
 *       → kvm_destroy_dirty_ring()
 *       → ... 释放所有关联资源
 *   - 关闭 /dev/kvm fd → 只是关闭对设备文件的引用
 *
 * mmap 的 kvm_run 区域在 vCPU fd 关闭后自动失效。
 * mmap 的 Guest 内存在 VM fd 关闭后自动失效。
 * ============================================================================ */
static void cleanup(int kvm_fd, int vm_fd, int vcpu_fd,
                    void *guest_mem, struct kvm_run *run, int mmap_size)
{
    KVM_INFO("===== 清理资源 =====");

    if (run && run != MAP_FAILED) {
        munmap(run, mmap_size);
        KVM_INFO("已释放 kvm_run mmap 区域");
    }

    if (guest_mem && guest_mem != MAP_FAILED) {
        munmap(guest_mem, GUEST_MEM_SIZE);
        KVM_INFO("已释放 Guest 内存映射");
    }

    if (vcpu_fd >= 0) {
        close(vcpu_fd);
        KVM_INFO("已关闭 vCPU fd");
    }

    if (vm_fd >= 0) {
        close(vm_fd);
        KVM_INFO("已关闭 VM fd");
    }

    if (kvm_fd >= 0) {
        close(kvm_fd);
        KVM_INFO("已关闭 /dev/kvm fd");
    }

    KVM_INFO("✓ 清理完成\n");
}

/* ============================================================================
 * 主函数
 * ============================================================================ */
int main(int argc, char *argv[])
{
    int kvm_fd = -1;
    int vm_fd = -1;
    int vcpu_fd = -1;
    int mmap_size;
    void *guest_mem = MAP_FAILED;
    struct kvm_run *run = MAP_FAILED;

    printf("============================================================\n");
    printf("  KVM API 完整生命周期演示\n");
    printf("  参考内核: linux-6.12.93\n");
    printf("  核心源码: virt/kvm/kvm_main.c, arch/x86/kvm/vmx/vmx.c\n");
    printf("============================================================\n\n");

    /*
     * 完整的 KVM 虚拟机创建和运行流程:
     *
     * 用户空间操作                    内核对应
     * ─────────────────────────────────────────────────────────
     * 1. open("/dev/kvm")          → kvm_dev_ioctl (全局fd)
     * 2. ioctl(fd, KVM_CREATE_VM)  → kvm_create_vm (VM fd)
     * 3. ioctl(vm, KVM_SET_IDENTITY_MAP_ADDR) → x86 身份映射
     * 4. ioctl(vm, KVM_SET_TSS_ADDR) → TSS 设置
     * 5. ioctl(vm, KVM_SET_USER_MEMORY_REGION) → 内存映射
     * 6. 加载 Guest 代码到内存      → (纯用户空间操作)
     * 7. ioctl(vm, KVM_CREATE_VCPU) → vmx_create_vcpu (vCPU fd)
     * 8. mmap(vcpu_fd) → kvm_run  → 共享内存映射
     * 9. ioctl(vcpu, KVM_SET_SREGS) → 段寄存器 → VMCS
     * 10. ioctl(vcpu, KVM_SET_REGS) → 通用寄存器 → vcpu->arch.regs
     * 11. ioctl(vcpu, KVM_RUN)     → ★ VM-Entry → Guest 执行 → VM-Exit
     * 12. 读取 run->exit_reason     → 处理退出
     * 13. close(所有fd)             → 资源释放
     */

    /* 第 1 步：打开 KVM */
    kvm_fd = open_kvm();

    /* 第 2 步：创建 VM */
    vm_fd = create_vm(kvm_fd);

    /* 第 3 步：设置 x86 身份映射和 TSS */
    setup_identity_map(vm_fd);

    /* 第 4 步：为 Guest 分配内存 */
    guest_mem = setup_guest_memory(vm_fd);

    /* 第 5 步：加载 Guest 代码到 Guest 内存 */
    load_guest_code(guest_mem);

    /* 第 6 步：创建 vCPU */
    vcpu_fd = create_vcpu(vm_fd);

    /* 第 7 步：映射 kvm_run 结构体 */
    run = map_vcpu_run(kvm_fd, vcpu_fd);
    mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);

    /* 第 8 步：设置 vCPU 寄存器 */
    setup_vcpu_registers(vcpu_fd);

    /* 第 9 步：运行 vCPU */
    run_vcpu(vcpu_fd, run);

    /* 第 10 步：处理退出 */
    handle_exit(run);

    /* 清理 */
    cleanup(kvm_fd, vm_fd, vcpu_fd, guest_mem, run, mmap_size);

    printf("============================================================\n");
    printf("  演示完成！\n");
    printf("\n");
    printf("  回顾 - 内核中发生的事情:\n");
    printf("  1. KVM_CREATE_VM  → 创建 struct kvm, 初始化 EPT/MMU\n");
    printf("  2. KVM_CREATE_VCPU → 创建 vcpu_vmx, 分配 VMCS\n");
    printf("  3. KVM_SET_REGS   → 写入 vcpu->arch.regs[]\n");
    printf("  4. KVM_RUN        → VM-Entry (VMLAUNCH/VMRESUME)\n");
    printf("                    → Guest 执行 MOV + HLT\n");
    printf("                    → VM-Exit (HLT → exit_reason=12)\n");
    printf("                    → KVM 处理 → 返回 KVM_EXIT_HLT\n");
    printf("  5. close(fd)      → kvm_destroy_vm → 释放所有资源\n");
    printf("============================================================\n");

    return 0;
}

/* ============================================================================
 * 附录：使用 ftrace 追踪本程序的执行
 * ============================================================================
 *
 * 可以用 ftrace 观察本程序触发的内核路径。在运行程序前执行:
 *
 *   # 1. 清空 trace buffer
 *   echo > /sys/kernel/debug/tracing/trace
 *
 *   # 2. 启用 KVM 相关 tracepoints
 *   echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
 *   echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable
 *   echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_vcpu_wakeup/enable
 *
 *   # 3. (可选) 追踪关键函数调用
 *   echo kvm_vcpu_ioctl >> /sys/kernel/debug/tracing/set_ftrace_filter
 *   echo vmx_vcpu_run >> /sys/kernel/debug/tracing/set_ftrace_filter
 *   echo kvm_arch_vcpu_ioctl_run >> /sys/kernel/debug/tracing/set_ftrace_filter
 *
 *   # 4. 运行本程序
 *   ./kvm-demo
 *
 *   # 5. 查看 trace 结果
 *   cat /sys/kernel/debug/tracing/trace
 *
 *   预期会看到类似:
 *   kvm_entry: vcpu 0, rip 0x1000     ← VM-Entry, RIP=0x1000
 *   kvm_exit:  reason HLT rip 0x1004  ← VM-Exit, HLT, RIP 前进到 0x1004
 *
 *   这说明:
 *     - Guest 从 RIP=0x1000 (MOV 指令) 开始执行
 *     - Guest 执行了 3 字节 MOV + 1 字节 HLT = RIP 到达 0x1004
 *     - HLT 触发了 VM-Exit
 *
 *   # 6. 清理
 *   echo 0 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
 *   echo 0 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable
 *   echo > /sys/kernel/debug/tracing/set_ftrace_filter
 *
 * ============================================================================
 * 附录：使用 perf 查看性能相关事件
 * ============================================================================
 *
 *   perf record -e kvm:kvm_exit,kvm:kvm_entry,kvm:kvm_page_fault ./kvm-demo
 *   perf report
 *
 * 预期结果:
 *   - kvm_entry: 1 次 (RIP=0x1000)
 *   - kvm_exit: 1 次 (HLT, RIP=0x1004)
 *   - kvm_page_fault: 若干次 (EPT Violation，因为首次访问 Guest 内存)
 *
 * ============================================================================
 */
