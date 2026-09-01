/*
 * minivmm.c — phase8 毕业建造 Project 1: 可启动的最小 VMM
 *
 * 不依赖 QEMU 等现成 VMM, 只用 /dev/kvm ioctl 把 Linux bzImage +
 * initramfs 引导到 shell。走 32-bit boot protocol (不做实模式)。
 *
 * 设计文档: ../project1-minivmm-boot.md
 *
 * 引导协议:
 *   /root/code/linux-6.12.93/Documentation/arch/x86/boot.rst
 *   ("32-bit Boot Protocol" 一节)
 * 字段偏移:
 *   /root/code/linux-6.12.93/arch/x86/include/uapi/asm/bootparam.h
 *
 * 用法:
 *   ./minivmm -k <bzImage> -i <initramfs> [-m MB] [-c cmdline]
 */

#define _GNU_SOURCE
#include <asm/bootparam.h>
#include <asm/e820.h>
#include <asm/kvm.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/kvm.h>
#include <linux/vfio.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef E820_RAM
#define E820_RAM 1
#endif

/* ---------- 常量 (来源见注释) ---------- */

/* VMX 需要宿主保留页, 不能落在 guest RAM 里 (kvmtool 同款取值) */
#define TSS_ADDR            0xfffbd000UL
#define IDENTITY_MAP_ADDR   0xfffbc000UL

/* zero page / cmdline 放低内存, 与 QEMU x86_load_linux() 惯例一致
 * (qemu-10.1.0-rc2/hw/i386/x86-common.c:633) */
#define BOOT_PARAMS_GPA     0x7000UL
#define CMDLINE_GPA         0x9000UL
#define CMDLINE_MAX         4096

/* 32-bit 协议要求 GDT 已加载 (__BOOT_CS=0x10, __BOOT_DS=0x18)。
 * VMX vmentry 还额外要求: TR 非空且指向合法 32 位 TSS 描述符
 * (SDM Vol3 27.3.2), LDT 要么合法要么标记不可用。 */
#define GDT_GPA             0x8000UL
#define TSS_GPA             0x8100UL
#define TSS_SEL             0x20

/* COM1 = 0x3f8, ISA IRQ4 → 默认 irqchip 路由下即 gsi 4 */
#define COM1_BASE           0x3f8
#define COM1_NR_REGS        8
#define SERIAL_IRQ_GSI      4

/* 寄存器索引与位定义一律对齐
 * /root/code/linux-6.12.93/include/uapi/linux/serial_reg.h */
#define UART_RX     0   /* in: 接收缓冲 (RBR); out: 发送保持 (THR) */
#define UART_DLL    0   /* DLAB=1: 分频器低字节 */
#define UART_IER    1   /* in/out: 中断使能 */
#define UART_DLM    1   /* DLAB=1: 分频器高字节 */
#define UART_IIR    2   /* in: 中断标识 */
#define UART_FCR    2   /* out: FIFO 控制 */
#define UART_LCR    3   /* 线路控制 */
#define UART_MCR    4   /* Modem 控制 */
#define UART_LSR    5   /* 线路状态 */
#define UART_MSR    6   /* Modem 状态 */
#define UART_SCR    7   /* 暂存器 */
#define UART_EFR    7   /* LCR=0xBF 时的扩展功能寄存器 */

/* IER (serial_reg.h:25-28) */
#define UART_IER_RDI    0x01  /* 接收数据可用 */
#define UART_IER_THRI   0x02  /* 发送保持寄存器空 */
#define UART_IER_RLSI   0x04  /* 接收线路状态 */
#define UART_IER_MSI    0x08  /* Modem 状态 */
#define UART_IER_ALL_INTR   0x0f  /* 可实现的中断使能位 */

/* IIR (serial_reg.h:35-48)。bits 7:6 是 guest 判定端口类型的唯一依据:
 * 0b00=8250, 0b10=16550(坏), 0b11=16550A —— 见 8250_port.c:1241
 * autoconfig() 的 switch (serial_in(IIR) & UART_IIR_FIFO_ENABLED) */
#define UART_IIR_MSI        0x00
#define UART_IIR_THRI       0x02
#define UART_IIR_RDI        0x04
#define UART_IIR_RLSI       0x06
#define UART_IIR_NO_INT     0x01
#define UART_IIR_ID         0x0e
#define UART_IIR_FIFO_ENABLED   0xc0

/* FCR (serial_reg.h:55; 触发位 83) */
#define UART_FCR_ENABLE_FIFO    0x01
#define UART_FCR_CLEAR_RCVR     0x02
#define UART_FCR_CLEAR_XMIT     0x04
#define UART_FCR_DMA_SELECT     0x08
#define UART_FCR_TRIGGER_MASK   0xc0

/* LCR (serial_reg.h:110) */
#define UART_LCR_DLAB       0x80  /* 1 = 偏移 0/1 变成 DLL/DLM */

/* MCR/MSR (serial_reg.h:133-137, 151-159。回环模式下引脚映射:
 * DTR→DSR, RTS→CTS, OUT1→RI, OUT2→DCD */
#define UART_MCR_DTR        0x01
#define UART_MCR_RTS        0x02
#define UART_MCR_OUT1       0x04
#define UART_MCR_OUT2       0x08
#define UART_MCR_LOOP       0x10
#define UART_MSR_DCD        0x80
#define UART_MSR_RI         0x40
#define UART_MSR_DSR        0x20
#define UART_MSR_CTS        0x10
#define UART_MSR_ANY_DELTA  0x0f

/* LSR (serial_reg.h:141-147) */
#define UART_LSR_DR     0x01
#define UART_LSR_OE     0x02
#define UART_LSR_PE     0x04
#define UART_LSR_FE     0x08
#define UART_LSR_BI     0x10
#define UART_LSR_THRE   0x20
#define UART_LSR_TEMT   0x40

#define DIE(fmt, ...) do { \
    fprintf(stderr, "minivmm: " fmt "\n", ##__VA_ARGS__); \
    exit(1); \
} while (0)

#define DIE_ERRNO(msg) DIE("%s: %s", msg, strerror(errno))

/* 用户态头文件不提供内核的 X86_CR0_* 宏 */
#define CR0_PE  (1UL << 0)
#define CR0_ET  (1UL << 4)
#define CR0_NE  (1UL << 5)
#define CR0_NW  (1UL << 29)
#define CR0_CD  (1UL << 30)
#define CR0_PG  (1UL << 31)

/* KVM_MAX_CPUID_ENTRIES 是内核内部宏 (kvm_host.h), uapi 不导出 */
#define MAX_CPUID_ENTRIES 256

/* ---------- 全局状态 ---------- */

static int kvm_fd = -1, vm_fd = -1, vcpu_fd = -1;
static struct kvm_run *run;
static void *guest_mem;
static uint64_t mem_size;

static volatile sig_atomic_t running = 1;
static int blob_mode;

/* ---------- 启动计时（项目 4 的判据） ----------
 * t_start = VMM 进程进入 main 的时刻，两个里程碑都相对它：
 *   1. 首个发往宿主的串口字节 —— guest 控制台第一次写 THR
 *   2. MINIVMM_READY —— guest /init 在 cmdline 带 autotest 时打印的
 *      就绪标记（见 scripts/vm/build-rootfs-minimal.sh 的 create_init），
 *      打完标记 guest 自己 reboot，靠下面的复位端口模拟收尾
 * 同一套标记 QEMU 那边也能用（脚本抓串口），两边判据才可比。 */
static double t_start, t_first_byte;

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void console_output(uint8_t ch)
{
    static const char marker[] = "MINIVMM_READY";
    static uint8_t hist[sizeof(marker) - 1];   /* 最近 N 个输出字节 */
    const size_t n = sizeof(marker) - 1;

    if (!t_first_byte) {
        t_first_byte = now_sec();
        fprintf(stderr, "\n[boot] 首个串口字节 +%.3f ms\n",
                (t_first_byte - t_start) * 1e3);
    }

    ssize_t w = write(STDOUT_FILENO, &ch, 1);
    (void)w;

    memmove(hist, hist + 1, n - 1);
    hist[n - 1] = ch;
    if (memcmp(hist, marker, n) == 0) {
        fprintf(stderr, "\n[boot] guest 就绪 (MINIVMM_READY) +%.3f ms\n",
                (now_sec() - t_start) * 1e3);
        running = 0;
    }
}

/* ---------- 16550A 串口状态 ----------
 * thr_ipending 是"THRI 已拉出但尚未被读 IIR 确认"的闩锁：真实芯片上
 * THRE 会一直保持 1，若不闩锁则 IRQ 引脚长高、guest 中断风暴。
 * 语义对照 qemu-10.1.0-rc2/hw/char/serial.c:232 (serial_xmit) 与 :508-512
 * (case 2 读 IIR 时清 thr_ipending)。
 * irq_level 记录当前 IRQ4 线的电平，只在变化时下发 KVM_IRQ_LINE。 */
#define SER_RX_SZ 1024    /* 必须是 2 的幂 */

struct serial_state {
    uint8_t ier, fcr, lcr, mcr, scr;
    uint8_t lsr, msr, iir;
    uint8_t thr_ipending;
    uint16_t divider;             /* DLAB=1 时偏移 0/1 的波特率分频器 */
    uint8_t rx_ring[SER_RX_SZ];
    unsigned rx_head, rx_tail;    /* 自由递增计数，不做回绕取模 */
    int irq_level;
    pthread_mutex_t lock;
};
static struct serial_state serial = {
    .lsr  = UART_LSR_THRE | UART_LSR_TEMT,
    .msr  = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS,
    .iir  = UART_IIR_NO_INT,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static struct termios saved_termios;
static int termios_saved;

/* ---------- 16550A 串口模拟 ----------
 * 行为模型对照 qemu-10.1.0-rc2/hw/char/serial.c；寄存器语义对照
 * /root/code/linux-6.12.93/include/uapi/linux/serial_reg.h。
 * 为什么必须做完整：guest 的 autoconfig() 用 IER 读写测试 + IIR 的
 * FIFO_ENABLED 位判定端口类型，autoconfig_irq() 靠"写 THR 后是否收到
 * THRI 中断"反推 IRQ 号。只做 TX 打字的极简模拟会得到
 * "ttyS0 ... (irq = 0) is a 16450" —— irq=0 时 tty 写路径不发字符，
 * 用户态输出全部丢失（实测踩过）。 */

static void kvm_irq_line(unsigned gsi, int level)
{
    struct kvm_irq_level irq = { .irq = gsi, .level = level };

    if (ioctl(vm_fd, KVM_IRQ_LINE, &irq) < 0 && errno != EINTR) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "minivmm: KVM_IRQ_LINE 失败: %s\n",
                    strerror(errno));
    }
}

static unsigned rx_count_locked(void)
{
    return serial.rx_head - serial.rx_tail;
}

/* 计算 IIR 并驱动 IRQ 线。优先级照 datasheet：RLSI > RDI > THRI > MSI
 * (qemu hw/char/serial.c:118 serial_update_irq 同序) */
static void serial_update_irq_locked(void)
{
    uint8_t err = serial.lsr & (UART_LSR_OE | UART_LSR_PE |
                                UART_LSR_FE | UART_LSR_BI);
    uint8_t tmp = UART_IIR_NO_INT;
    int level;

    if ((serial.ier & UART_IER_RLSI) && err)
        tmp = UART_IIR_RLSI;
    else if ((serial.ier & UART_IER_RDI) && (serial.lsr & UART_LSR_DR))
        tmp = UART_IIR_RDI;
    else if ((serial.ier & UART_IER_THRI) && serial.thr_ipending)
        tmp = UART_IIR_THRI;
    else if ((serial.ier & UART_IER_MSI) && (serial.msr & UART_MSR_ANY_DELTA))
        tmp = UART_IIR_MSI;

    serial.iir = tmp | (serial.iir & UART_IIR_FIFO_ENABLED);

    level = (tmp != UART_IIR_NO_INT);
    if (level == serial.irq_level)
        return;
    serial.irq_level = level;
    kvm_irq_line(SERIAL_IRQ_GSI, level);
}

/* 收一个字符（宿主 → guest）。调用者持锁 */
static void serial_recv_locked(uint8_t c)
{
    if (rx_count_locked() >= SER_RX_SZ - 1) {
        serial.lsr |= UART_LSR_OE;   /* 溢出丢弃新字符，与硬件一致 */
    } else {
        serial.rx_ring[serial.rx_head % SER_RX_SZ] = c;
        serial.rx_head++;
        serial.lsr |= UART_LSR_DR;
    }
    serial_update_irq_locked();
}

static void serial_write_reg(uint8_t reg, uint8_t val)
{
    pthread_mutex_lock(&serial.lock);
    switch (reg) {
    case UART_RX:   /* THR */
        if (serial.lcr & UART_LCR_DLAB) {
            serial.divider = (serial.divider & 0xff00) | val;
            break;
        }
        serial.thr_ipending = 0;
        serial.lsr &= ~(UART_LSR_THRE | UART_LSR_TEMT);
        if (serial.mcr & UART_MCR_LOOP)
            serial_recv_locked(val);    /* 回环：TX 直接进 RX */
        else
            console_output(val);
        /* 本项目按"立即发送完成"建模：无波特率时序，THRE/TEMT 当场置回，
         * 并拉一次 THRI 闩锁 —— 驱动读 IIR 后自然落下去 */
        serial.lsr |= UART_LSR_THRE | UART_LSR_TEMT;
        serial.thr_ipending = 1;
        serial_update_irq_locked();
        break;
    case UART_IER:
        if (serial.lcr & UART_LCR_DLAB) {
            serial.divider = (serial.divider & 0x00ff) |
                             ((uint16_t)val << 8);
            break;
        }
        /* 只实现低 4 位：真实 16550A 没有 Xscale 的 UUE(bit6)，
         * autoconfig_16550a() 正是用"能否写回 bit6"区分 Xscale */
        serial.ier = val & UART_IER_ALL_INTR;
        if ((serial.ier & UART_IER_THRI) && (serial.lsr & UART_LSR_THRE))
            serial.thr_ipending = 1;    /* 开 THRI 时 THRE 已空 → 立刻上报 */
        serial_update_irq_locked();
        break;
    case UART_FCR:
        if ((val ^ serial.fcr) & UART_FCR_ENABLE_FIFO)
            val |= UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT;
        if (val & UART_FCR_CLEAR_RCVR) {
            serial.rx_head = serial.rx_tail = 0;
            serial.lsr &= ~(UART_LSR_DR | UART_LSR_BI);
        }
        if (val & UART_FCR_CLEAR_XMIT) {
            serial.lsr |= UART_LSR_THRE;
            serial.thr_ipending = 1;
        }
        serial.fcr = val & (UART_FCR_ENABLE_FIFO | UART_FCR_DMA_SELECT |
                            UART_FCR_TRIGGER_MASK);
        if (serial.fcr & UART_FCR_ENABLE_FIFO)
            serial.iir |= UART_IIR_FIFO_ENABLED;
        else
            serial.iir &= ~UART_IIR_FIFO_ENABLED;
        serial_update_irq_locked();
        break;
    case UART_LCR:
        serial.lcr = val;
        break;
    case UART_MCR:
        serial.mcr = val;
        break;
    case UART_SCR:  /* 偏移 7；EFR 在 16550A 上不存在，读回 0 即可 */
        serial.scr = val;
        break;
    default:        /* LSR / MSR 只读，写忽略 */
        break;
    }
    pthread_mutex_unlock(&serial.lock);
}

static uint8_t serial_read_reg(uint8_t reg)
{
    uint8_t val;

    pthread_mutex_lock(&serial.lock);
    switch (reg) {
    case UART_RX:
        if (serial.lcr & UART_LCR_DLAB) {
            val = serial.divider & 0xff;
            break;
        }
        if (rx_count_locked()) {
            val = serial.rx_ring[serial.rx_tail % SER_RX_SZ];
            serial.rx_tail++;
        } else {
            val = 0;
        }
        if (!rx_count_locked())
            serial.lsr &= ~(UART_LSR_DR | UART_LSR_BI);
        serial_update_irq_locked();
        break;
    case UART_IER:
        val = (serial.lcr & UART_LCR_DLAB) ? (uint8_t)(serial.divider >> 8)
                                          : serial.ier;
        break;
    case UART_IIR:
        val = serial.iir;
        if ((val & UART_IIR_ID) == UART_IIR_THRI) {
            serial.thr_ipending = 0;    /* 读 IIR 即确认 THRI */
            serial_update_irq_locked();
        }
        break;
    case UART_LCR:
        val = serial.lcr;
        break;
    case UART_MCR:
        val = serial.mcr;
        break;
    case UART_LSR:
        val = serial.lsr;
        /* 读 LSR 即确认 break / overrun，对应 RLSI 撤销 */
        if (val & (UART_LSR_BI | UART_LSR_OE)) {
            serial.lsr &= ~(UART_LSR_BI | UART_LSR_OE);
            serial_update_irq_locked();
        }
        break;
    case UART_MSR:
        if (serial.mcr & UART_MCR_LOOP) {
            /* 回环引脚映射：OUT1→RI, OUT2→DCD, RTS→CTS, DTR→DSR
             * (8250_port.c:1215-1218 的 LOOP 测试按 MCR=OUT2|RTS 期望
             *  MSR==DCD|CTS) */
            val = ((serial.mcr & (UART_MCR_OUT1 | UART_MCR_OUT2)) << 4) |
                  ((serial.mcr & UART_MCR_RTS) << 3) |
                  ((serial.mcr & UART_MCR_DTR) << 5);
        } else {
            val = serial.msr;
            if (val & UART_MSR_ANY_DELTA) {
                serial.msr &= 0xf0;
                serial_update_irq_locked();
            }
        }
        break;
    default:                            /* 偏移 7 = SCR */
        val = serial.scr;
        break;
    }
    pthread_mutex_unlock(&serial.lock);
    return val;
}

/* vCPU 线程: 处理一次串口 IO。返回 0 表示已处理 */
static int serial_io(uint16_t port, int is_write, uint8_t *val)
{
    if (port >= COM1_BASE && port < COM1_BASE + COM1_NR_REGS) {
        uint8_t reg = port - COM1_BASE;

        if (is_write)
            serial_write_reg(reg, *val);
        else
            *val = serial_read_reg(reg);
        return 0;
    }

    /* COM2/3/4: 读回 0xff, 8250 探测判定为不存在 */
    if ((port >= 0x2e8 && port < 0x2f0) ||
        (port >= 0x2f8 && port < 0x300) ||
        (port >= 0x3e8 && port < 0x3f0)) {
        if (!is_write)
            *val = 0xff;
        return 0;
    }

    return -1;  /* 交给通用 IO 处理 */
}

/* ---------- 复位端口 ----------
 * guest 里 reboot -f 最终走 native_machine_emergency_restart()：
 * BOOT_KBD 分支 outb(0xfe, 0x64) 拉复位脉冲
 * (arch/x86/kernel/reboot.c:667)，BOOT_CF9_* 分支写 0xcf9 的 bit1
 * 请求硬复位 (reboot.c:698)。两处都翻译成"VMM 退出"，autotest 模式下
 * guest 才能自己把宿主收掉，否则只能靠 timeout 兜。
 * 读 0x64 返回 0：状态位 bit1 是"输入缓冲满"，恒 0 表示随时可写，
 * 让 kb_wait() (reboot.c:520-530) 第一圈就 break。 */
static int power_io(uint16_t port, int is_write, uint8_t *val)
{
    if (port != 0x64 && port != 0xcf9)
        return -1;

    if (!is_write) {
        *val = 0x00;
        return 0;
    }
    if ((port == 0x64 && *val == 0xfe) || (port == 0xcf9 && (*val & 0x02))) {
        fprintf(stderr, "\n[vmm] guest 复位请求 (port 0x%x val 0x%02x)\n",
                port, *val);
        running = 0;
    }
    return 0;
}

/* 通用 IO 端口: 只保证内核探测不卡死。
 * 宽读必须全 1 (如 PCI 32 位读需 0xffffffff 判定无设备) */
static uint32_t generic_io_in(uint16_t port, int size)
{
    (void)port;
    return size == 4 ? 0xffffffffU : size == 2 ? 0xffffU : 0xffU;
}

/* ---------- 输入线程: stdin → RX ring → RDA 中断 ---------- */

static void *input_thread(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    ssize_t n;

    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        pthread_mutex_lock(&serial.lock);
        for (ssize_t i = 0; i < n; i++)
            serial_recv_locked(buf[i]);
        pthread_mutex_unlock(&serial.lock);
    }
    return NULL;
}

/* ---------- bzImage 加载 (32-bit boot protocol) ---------- */

static void *read_file(const char *path, size_t *size_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        DIE_ERRNO(path);
    struct stat st;
    if (fstat(fd, &st) < 0)
        DIE_ERRNO(path);
    void *buf = malloc(st.st_size);
    if (!buf)
        DIE("malloc %zd 失败", (size_t)st.st_size);
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + off, st.st_size - off);
        if (n < 0)
            DIE_ERRNO(path);
        if (n == 0)
            DIE("%s: 短读", path);
        off += n;
    }
    close(fd);
    *size_out = st.st_size;
    return buf;
}

/* 在 guest RAM 里建平坦 GDT + TSS 描述符。
 * 描述符编码见 SDM Vol3 3.4.5 (段描述符):
 *   [15:0]=limit0-15, [39:16]=base0-23, [47:40]=access,
 *   [51:48]=limit16-19, [55:52]=flags, [63:56]=base24-31 */
static void build_gdt(void)
{
    uint64_t *gdt = (uint64_t *)((uint8_t *)guest_mem + GDT_GPA);
    memset(gdt, 0, 5 * 8);
    /* 0x10: 平坦 32 位代码段, base 0, limit 4G */
    gdt[2] = 0x00cf9b000000ffffULL;
    /* 0x18: 平坦 32 位数据段 */
    gdt[3] = 0x00cf93000000ffffULL;
    /* 0x20: 32 位忙 TSS, base TSS_GPA, limit 103 */
    gdt[4] = 0x00008b0000000067ULL | ((uint64_t)TSS_GPA << 16);
    memset((uint8_t *)guest_mem + TSS_GPA, 0, 104);
}

/* 自检 blob: 反复经 COM1 打印 "A\n"。用于隔离 "vmentry 被拒"
 * 与 "内核镜像/加载" 两类问题 —— blob 能跑则 sregs/VMCS 状态合法。
 *
 * 编码注意: guest 处于 32 位保护模式 (CS.D=1), B8+rd / BA 类指令的
 * 立即数是 32 位 —— 裸写 `ba f8 03` (mov dx,0x3f8) 会被解码成
 * `mov edx, imm32` 并把后面两条指令吞成立即数高位, 曾导致调试时
 * 误判为 "out 不触发 VM-Exit / 跳转失效"。16 位目的寄存器必须加
 * 0x66 操作数前缀。 */
static uint64_t load_selftest_blob(void)
{
    static const uint8_t blob[] = {
        0xb0, 'A',                  /* 0x0: mov al,'A'      */
        0x66, 0xba, 0xf8, 0x03,     /* 0x2: mov dx,0x3f8    */
        0xee,                       /* 0x6: out dx,al       */
        0xb0, '\n',                 /* 0x7: mov al,'\n'     */
        0xee,                       /* 0x9: out dx,al       */
        0xb9, 0x00, 0x00, 0x10, 0x00, /* 0xa: mov ecx,1M   */
        0x90,                       /* 0xf: nop (delay)     */
        0x49,                       /* 0x10: dec ecx        */
        0x75, 0xfc,                 /* 0x11: jnz 0xf        */
        0xeb, 0xeb,                 /* 0x13: jmp 0x0        */
    };
    uint64_t rip = 0x100000;
    memcpy((uint8_t *)guest_mem + rip, blob, sizeof(blob));
    printf("[blob] 自检代码 %zu 字节 @ GPA %llx (不加载内核)\n",
           sizeof(blob), (unsigned long long)rip);
    return rip;
}

struct boot_image {
    uint64_t kernel_gpa;      /* 内核载入 GPA (= 入口 RIP) */
    uint64_t initrd_gpa;
    uint64_t initrd_size;
};

/* boot.rst: setup 头从镜像偏移 0x01f1 开始,
 * 末尾 = 0x0202 + 偏移 0x0201 处的字节 */
static struct boot_image load_bzimage(const char *kernel_path,
                                      const char *initrd_path,
                                      const char *cmdline)
{
    struct boot_image img = { 0 };

    size_t ksize;
    uint8_t *kbuf = read_file(kernel_path, &ksize);
    if (ksize < 0x202 + 2)
        DIE("%s: 太小, 不是有效 bzImage", kernel_path);

    uint8_t setup_sects = kbuf[0x1f1];
    if (setup_sects == 0)
        setup_sects = 4;
    size_t setup_len = (size_t)(setup_sects + 1) * 512;
    if (ksize <= setup_len)
        DIE("%s: setup 段越界", kernel_path);

    uint8_t *hdr_src = kbuf + 0x1f1;
    size_t hdr_len = 0x0202 + (size_t)kbuf[0x0201] - 0x1f1;

    struct boot_params *bp =
        (struct boot_params *)((uint8_t *)guest_mem + BOOT_PARAMS_GPA);
    memset(bp, 0, sizeof(*bp));

    /* 校验 "HdrS" 魔数 (偏移 0x202) 与协议版本 (0x206) */
    uint32_t magic;
    memcpy(&magic, kbuf + 0x202, 4);
    if (magic != 0x53726448)
        DIE("setup header 魔数错误: %08x", magic);
    uint16_t version;
    memcpy(&version, kbuf + 0x206, 2);
    if (version < 0x0200)
        DIE("boot protocol 版本 %x 过旧", version);

    memcpy(&bp->hdr, hdr_src, hdr_len);
    bp->hdr.type_of_loader = 0xff;
    bp->hdr.loadflags |= LOADED_HIGH | CAN_USE_HEAP;
    bp->hdr.heap_end_ptr = 0xffff;

    /* 载入 protected-mode 内核 */
    uint64_t load_addr = bp->hdr.pref_address;   /* 协议 2.10+ */
    uint32_t init_size = bp->hdr.init_size;
    if (version < 0x020a || load_addr == 0 ||
        load_addr + init_size > mem_size) {
        load_addr = 0x100000;   /* 退回 1MB */
        if (load_addr + (ksize - setup_len) > mem_size)
            DIE("guest 内存 %lluMB 装不下内核",
                (unsigned long long)(mem_size >> 20));
    }
    memcpy((uint8_t *)guest_mem + load_addr, kbuf + setup_len,
           ksize - setup_len);
    bp->hdr.code32_start = (uint32_t)load_addr;
    img.kernel_gpa = load_addr;
    printf("[load] 内核 %zu 字节 → GPA %llx (pref_address)\n",
           ksize - setup_len, (unsigned long long)load_addr);

    /* cmdline */
    size_t cl_len = strlen(cmdline) + 1;
    if (cl_len > CMDLINE_MAX)
        DIE("cmdline 过长");
    memcpy((uint8_t *)guest_mem + CMDLINE_GPA, cmdline, cl_len);
    bp->hdr.cmd_line_ptr = CMDLINE_GPA;
    if (version >= 0x0206)
        bp->hdr.cmdline_size = (uint32_t)(cl_len - 1);

    /* initramfs: 放内存顶端 */
    size_t isize;
    void *ibuf = read_file(initrd_path, &isize);
    uint64_t initrd_gpa = (mem_size - isize) & ~0xfffUL;
    if (initrd_gpa <= load_addr + init_size)
        DIE("内存太小, initrd 与内核重叠");
    memcpy((uint8_t *)guest_mem + initrd_gpa, ibuf, isize);
    bp->hdr.ramdisk_image = (uint32_t)initrd_gpa;
    bp->hdr.ramdisk_size = (uint32_t)isize;
    img.initrd_gpa = initrd_gpa;
    img.initrd_size = isize;
    printf("[load] initrd %zu 字节 → GPA %llx; cmdline = \"%s\"\n",
           isize, (unsigned long long)initrd_gpa, cmdline);
    free(ibuf);

    /* e820: 至少 2 段 —— e820.c:456 append_e820_table 把
     * "nr_entries < 2" 视为坏 BIOS 直接忽略全表 (退回
     * BIOS-e801/88 伪映射, 内存接近 0 → panic)。
     * 按 1MB 分界拆两段 (QEMU fw_cfg 同款做法):
     *   [0, 0x9fc00) RAM (EBDA 之上不声明, 留白即可)
     *   [1MB, mem_size) RAM —— 内核 pref_address=16MB 落在这里
     * boot_params (0x7000) 在 setup 早期即被 copy_bootdata 拷走,
     * 标记为 RAM 无碍; initrd 范围由 setup_arch 读到
     * ramdisk_image/size 后自行保留。 */
    bp->e820_table[0].addr = 0;
    bp->e820_table[0].size = 0x9fc00;
    bp->e820_table[0].type = E820_RAM;
    bp->e820_table[1].addr = 0x100000;
    bp->e820_table[1].size = mem_size - 0x100000;
    bp->e820_table[1].type = E820_RAM;
    bp->e820_entries = 2;

    build_gdt();

    free(kbuf);
    return img;
}

/* ---------- VM / vCPU 搭建 ---------- */

static void vm_setup(void)
{
    kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0)
        DIE_ERRNO("open /dev/kvm");
    if (ioctl(kvm_fd, KVM_GET_API_VERSION, 0) != 12)
        DIE("KVM API 版本不是 12");
    if (ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY) <= 0)
        DIE("KVM_CAP_USER_MEMORY 不可用");

    vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0)
        DIE_ERRNO("KVM_CREATE_VM");

    /* VMX 保留页, 必须在 guest RAM 之外 */
    if (ioctl(vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &(uint64_t){IDENTITY_MAP_ADDR}) < 0)
        DIE_ERRNO("KVM_SET_IDENTITY_MAP_ADDR");
    if (ioctl(vm_fd, KVM_SET_TSS_ADDR, (unsigned long)TSS_ADDR) < 0)
        DIE_ERRNO("KVM_SET_TSS_ADDR");

    /* 硬约束: irqchip 必须先于 vCPU 创建
     * (arch/x86/kvm/x86.c:7098-7099, created_vcpus 非零则 -EINVAL) */
    if (ioctl(vm_fd, KVM_CREATE_IRQCHIP, 0) < 0)
        DIE_ERRNO("KVM_CREATE_IRQCHIP");

    struct kvm_pit_config pit_cfg = { 0 };
    if (ioctl(vm_fd, KVM_CREATE_PIT, &pit_cfg) < 0)
        DIE_ERRNO("KVM_CREATE_PIT");

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = mem_size,
        .userspace_addr = (uint64_t)(uintptr_t)guest_mem,
    };
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0)
        DIE_ERRNO("KVM_SET_USER_MEMORY_REGION");
    printf("[vm] guest RAM %lluMB @ HVA %p\n",
           (unsigned long long)(mem_size >> 20), guest_mem);
}

static void setup_cpuid(void)
{
    size_t sz = sizeof(struct kvm_cpuid2) +
                MAX_CPUID_ENTRIES * sizeof(struct kvm_cpuid_entry2);
    struct kvm_cpuid2 *cpuid = calloc(1, sz);
    if (!cpuid)
        DIE("calloc cpuid");
    cpuid->nent = MAX_CPUID_ENTRIES;

    if (ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0)
        DIE_ERRNO("KVM_GET_SUPPORTED_CPUID");

    bool has_sig = false, has_feat = false;

    for (uint32_t i = 0; i < cpuid->nent; i++) {
        struct kvm_cpuid_entry2 *e = &cpuid->entries[i];

        if (e->function == 1 && e->index == 0) {
            /* 第一版不暴露 x2APIC：in-kernel LAPIC 保持 xAPIC 模式，寄存器走
             * MMIO (0xfee00000)。若暴露该位而 guest 真切到 x2APIC，写 APIC
             * MSR 会在 lapic.c:3312 的 !apic_x2apic_mode() 检查处 return 1，
             * 经 x86.c:3889 回到 kvm_set_msr_common —— 后果是给 guest 注入
             * #GP，不是静默丢弃。
             * TSC_DEADLINE_TIMER (leaf1 ECX bit24, cpufeatures.h:137) 必须
             * 保留：缺它 cpuid.c:399-402 会把 timer_mode_mask 从 3<<17 降成
             * 1<<17，guest 写 LVT Timer 时 bit18 被 lapic.c:2391 掩掉，
             * TSC Deadline 模式根本设不上。 */
            e->ecx &= ~(1U << 21);
            /* X86_FEATURE_HYPERVISOR (cpufeatures.h:144, leaf1 ECX bit31)
             * 必须自己补上：KVM_GET_SUPPORTED_CPUID 返回的 ECX 里没有它
             * (本机实测 0x76fab223)，而 guest 的 __kvm_cpuid_base()
             * (arch/x86/kernel/kvm.c:877) 拿这一位当准入门槛。缺了就没有
             * "Hypervisor detected: KVM"，整套 PV (kvmclock / PV EOI /
             * ASYNC_PF) 全部失效。
             * 注意即使补上，clocksource 最终仍可能是 tsc：宿主暴露
             * CONSTANT_TSC + NONSTOP_TSC 时 kvmclock 主动把自己的 rating
             * 降到 299 (kvmclock.c:342-345)，低于 tsc 的 300
             * (tsc.c:1189)，于是 dmesg 里先 "Switched to clocksource
             * kvm-clock" 再 "Switched to clocksource tsc" —— 这是设计如
             * 此，不是 VMM 的缺陷；kvm-clock 仍在提供 sched_clock 和
             * MSR_KVM_SYSTEM_TIME/WALL_CLOCK 的 pvclock 更新。 */
            e->ecx |= (1U << 31);
            /* EBX[31:24] = 初始 APIC ID，必须与 in-kernel LAPIC 的 ID 0
             * 一致，否则 guest 打 "[Firmware Bug]: APIC ID mismatch"；
             * EBX[23:16] = 每包逻辑处理器数，宿主拓扑(96 线程)对单 vCPU
             * guest 无意义，改成 1。EBX[15:8] CLFLUSH 保留宿主值 */
            e->ebx = (e->ebx & 0x0000ffff) | (1U << 16);
        }
        if (e->function == 0x40000000)
            has_sig = true;
        if (e->function == 0x40000001)
            has_feat = true;
    }

    /* 宿主 KVM 已经把 0x40000000/0x40000001 填好（实测 eax=0x01007efb，
     * 含 CLOCKSOURCE2/PV_EOI/PV_UNHALT/ASYNC_PF 等），照抄即可；
     * 只有缺叶的老内核才需要在这里补最小集合 */
    if (!has_sig || !has_feat) {
        if (!has_sig) {
            struct kvm_cpuid_entry2 *e = &cpuid->entries[cpuid->nent++];
            e->function = 0x40000000;
            e->eax = 0x40000001;
            e->ebx = 0x4b4d564b;  /* "KVMK" */
            e->ecx = 0x564b4d56;  /* "VKMV" */
            e->edx = 0x4d;        /* "M\0\0\0" */
        }
        if (!has_feat) {
            struct kvm_cpuid_entry2 *e = &cpuid->entries[cpuid->nent++];
            e->function = 0x40000001;
            /* kvm_para.h: CLOCKSOURCE=0, CLOCKSOURCE2=3,
             * PV_EOI=6, PV_TLB_FLUSH=9 */
            e->eax = (1U << 0) | (1U << 3) | (1U << 6) | (1U << 9);
        }
    }

    if (ioctl(vcpu_fd, KVM_SET_CPUID2, cpuid) < 0)
        DIE_ERRNO("KVM_SET_CPUID2");
    printf("[vcpu] CPUID %u 个叶已设置 (x2APIC 已屏蔽)\n", cpuid->nent);
    free(cpuid);
}

/* boot.rst 32-bit 协议入口约定: 保护模式、分页关、平坦段
 * __BOOT_CS=0x10 / __BOOT_DS=0x18、中断关 */
static void setup_sregs(void)
{
    struct kvm_sregs sregs;
    if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0)
        DIE_ERRNO("KVM_GET_SREGS");

    sregs.cr0 |= CR0_PE | CR0_ET | CR0_NE;
    /* 复位态 CR0 带 CD|NW (x86.c:12618-12626), 直接引导内核前必须清掉 */
    sregs.cr0 &= ~(CR0_PG | CR0_CD | CR0_NW);
    sregs.cr3 = 0;
    sregs.efer = 0;

    /* S=1 (代码/数据段) 是 VMX entry 硬性检查 (§27.3.1.2),
     * KVM 原样写入 VMCS (vmx_segment_access_rights @ vmx.c:3593) */
    struct kvm_segment code = {
        .base = 0, .limit = 0xffffffff, .selector = 0x10,
        .type = 0xb,      /* exec/read, accessed */
        .s = 1, .present = 1, .dpl = 0, .db = 1, .l = 0, .g = 1,
    };
    struct kvm_segment data = {
        .base = 0, .limit = 0xffffffff, .selector = 0x18,
        .type = 0x3,      /* read/write, accessed */
        .s = 1, .present = 1, .dpl = 0, .db = 1, .l = 0, .g = 1,
    };
    sregs.cs = code;
    sregs.ds = sregs.es = sregs.ss = sregs.fs = sregs.gs = data;

    /* TR: VMX 要求非空且指向合法 32 位 TSS 描述符 */
    sregs.tr = (struct kvm_segment){
        .base = TSS_GPA, .limit = 103, .selector = TSS_SEL,
        .type = 0xb,      /* busy 32-bit TSS */
        .present = 1, .dpl = 0, .db = 0, .l = 0, .g = 0, .s = 0,
    };
    /* LDT: 标记不可用, 跳过 VMX 描述符检查 */
    sregs.ldt = (struct kvm_segment){ .unusable = 1 };

    sregs.gdt.base = GDT_GPA;
    sregs.gdt.limit = 5 * 8 - 1;
    sregs.idt.base = 0;
    sregs.idt.limit = 0xffff;

    if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0)
        DIE_ERRNO("KVM_SET_SREGS");
}

static void vcpu_setup(struct boot_image *img)
{
    vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0)
        DIE_ERRNO("KVM_CREATE_VCPU");

    int mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0)
        DIE_ERRNO("KVM_GET_VCPU_MMAP_SIZE");
    run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
               vcpu_fd, 0);
    if (run == MAP_FAILED)
        DIE_ERRNO("mmap kvm_run");

    setup_cpuid();
    setup_sregs();

    struct kvm_regs regs = { 0 };
    regs.rip = img->kernel_gpa;
    regs.rsi = BOOT_PARAMS_GPA;
    regs.rflags = 0x2;    /* 中断关 (IF=0) */
    if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0)
        DIE_ERRNO("KVM_SET_REGS");
    printf("[vcpu] rip=%llx rsi=%llx\n",
           (unsigned long long)regs.rip, (unsigned long long)regs.rsi);

    /* in-kernel irqchip 场景: vCPU 创建时处于 INIT/SIPI-wait 状态
     * (VMCS guest activity state = 1), 该状态下 vmentry 检查会拒绝
     * 任意 RIP (INVALID_STATE)。直接寄存器启动必须像
     * QEMU/kvmtool 一样把它标记为 RUNNABLE。 */
    struct kvm_mp_state mp_state = { .mp_state = KVM_MP_STATE_RUNNABLE };
    if (ioctl(vcpu_fd, KVM_SET_MP_STATE, &mp_state) < 0)
        DIE_ERRNO("KVM_SET_MP_STATE");
}

/* ---------- 项目 2: virtio-mmio (modern, VERSION=2) ----------
 * 寄存器偏移对齐 include/uapi/linux/virtio_mmio.h；split virtqueue 布局
 * 对齐 include/uapi/linux/virtio_ring.h 与 virtio 规范 §2.7。guest 侧驱动
 * drivers/virtio/virtio_mmio.c 用 cmdline `virtio_mmio.device=<size>@<base>:<irq>`
 * 注册 platform device（vm_cmdline_set() @ virtio_mmio.c:718）。
 *
 * 数据面：guest 写 QUEUE_NOTIFY → KVM_EXIT_MMIO → service_vq() 走
 * desc→avail→后端→used，然后置 INTERRUPT_STATUS 并拉 GSI；guest 读
 * INTERRUPT_STATUS、写 INTERRUPT_ACK 后我们清位、落 GSI（电平模型，
 * 对照 project2-minivmm-virtio.md M4）。M3 再把 notify/irq 换成
 * ioeventfd/irqfd。 */

/* MMIO 寄存器偏移 (virtio_mmio.h) */
#define VMMIO_MAGIC             0x000
#define VMMIO_VERSION           0x004
#define VMMIO_DEVICE_ID         0x008
#define VMMIO_VENDOR_ID         0x00c
#define VMMIO_DEV_FEATURES      0x010
#define VMMIO_DEV_FEATURES_SEL  0x014
#define VMMIO_DRV_FEATURES      0x020
#define VMMIO_DRV_FEATURES_SEL  0x024
#define VMMIO_QUEUE_SEL         0x030
#define VMMIO_QUEUE_NUM_MAX     0x034
#define VMMIO_QUEUE_NUM         0x038
#define VMMIO_QUEUE_READY       0x044
#define VMMIO_QUEUE_NOTIFY      0x050
#define VMMIO_INT_STATUS        0x060
#define VMMIO_INT_ACK           0x064
#define VMMIO_STATUS            0x070
#define VMMIO_QUEUE_DESC_LOW    0x080
#define VMMIO_QUEUE_DESC_HIGH   0x084
#define VMMIO_QUEUE_AVAIL_LOW   0x090
#define VMMIO_QUEUE_AVAIL_HIGH  0x094
#define VMMIO_QUEUE_USED_LOW    0x0a0
#define VMMIO_QUEUE_USED_HIGH   0x0a4
#define VMMIO_CONFIG_GEN        0x0fc
#define VMMIO_CONFIG            0x100

#define VMMIO_MAGIC_VALUE       0x74726976u     /* "virt" */
#define VMMIO_INT_VRING         (1u << 0)       /* virtio_mmio.h:149 */

/* 设备 ID (virtio_ids.h:33-34) */
#define VIRTIO_ID_BLK           2
#define VIRTIO_ID_CONSOLE       3

/* feature / status / 请求类型 */
#define VIRTIO_F_VERSION_1_BIT  32              /* virtio_config.h:67 */
#define VIRTIO_S_FEATURES_OK    8               /* virtio_config.h:42 */
#define VIRTIO_S_DRIVER_OK      4               /* virtio_config.h:40 */
#define VIRTIO_BLK_T_IN         0               /* virtio_blk.h:165 */
#define VIRTIO_BLK_T_OUT        1               /* :166 */
#define VIRTIO_BLK_T_FLUSH      4               /* :174 */
#define VIRTIO_BLK_S_OK         0               /* :317 */
#define VIRTIO_BLK_S_IOERR      1               /* :318 */
#define VIRTIO_BLK_S_UNSUPP     2               /* :319 */

/* GPA 布局：必须落在 guest RAM 之外、e820 不声明的空洞，才会触发
 * EPT violation → KVM_EXIT_MMIO（project2 已知陷阱 1）。RAM 默认 512MB
 * = 0x20000000，取 0xd0000000 起的两个 0x200 窗口。 */
#define VIRTIO_CONSOLE_BASE     0xd0000000ULL
#define VIRTIO_BLK_BASE         0xd0000200ULL
#define VIRTIO_REGION_SIZE      0x200ULL
#define VIRTIO_CONSOLE_GSI      5
#define VIRTIO_BLK_GSI          6

/* split virtqueue 结构 (virtio_ring.h)。x86 小端，直接按内存布局访问。 */
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define VRING_DESC_F_NEXT       1               /* virtio_ring.h:41 */
#define VRING_DESC_F_WRITE      2               /* :43 */

#define VRING_NUM_MAX           256
#define VQ_MAX                  4
#define VQ_MAX_SEGS             64

struct vq {
    uint32_t num;
    uint64_t desc, avail, used;   /* GPA */
    int ready;
    uint16_t last_avail_idx;      /* 设备侧消费游标 */
};

struct seg {
    void *hva;
    uint32_t len;
    int writable;
};

struct virtio_dev;
struct virtio_dev {
    uint64_t base;
    unsigned gsi;
    uint32_t device_id;
    const char *name;
    uint32_t status;
    uint32_t int_status;
    uint64_t dev_features, drv_features;
    uint32_t dev_feat_sel, drv_feat_sel;
    uint32_t queue_sel;
    int nqueues;
    struct vq vq[VQ_MAX];
    uint8_t config[64];
    size_t config_len;
    void *backend;
    int irq_fd;                     /* M3: KVM_IRQFD 绑的中断 eventfd */
    int notify_fd[VQ_MAX];          /* M3: 各队列 KVM_IOEVENTFD */
};

/* GPA→HVA，带边界检查；guest 内存是单一 region，GPA 0 起 (见 vm_setup) */
static void *gpa2hva(uint64_t gpa, uint64_t len)
{
    if (gpa + len < gpa || gpa + len > mem_size)
        return NULL;
    return (uint8_t *)guest_mem + gpa;
}

static int use_eventfd;             /* -e: M3 ioeventfd + irqfd 数据面下沉 */
static int passthrough_mode;        /* -p: 项目3 VFIO 直通（先建 MP 表） */
/* MINIVMM_MSIX_SLOW=1：跳过 KVM_IRQFD（PI 链路不会建立），设备每个 MSI-X
 * 必走 vfio_msihandler→eventfd，由用户态线程计数并经 KVM_IRQ_LINE 注入。
 * eventfd 计数即"设备是否发中断"的可靠判据（见 corrections.md E 节）。 */
static int msix_slowpath;

static void virtio_raise_irq(struct virtio_dev *dev)
{
    dev->int_status |= VMMIO_INT_VRING;
    if (use_eventfd) {
        /* irqfd 无 resample 时内核自动做一次 assert→deassert 脉冲
         * (virt/kvm/eventfd.c:48-52)，对 PIC 边沿中断正好是一次触发 */
        uint64_t one = 1;
        ssize_t w = write(dev->irq_fd, &one, sizeof(one));
        (void)w;
    } else {
        kvm_irq_line(dev->gsi, 1);
    }
}

static void virtio_lower_irq(struct virtio_dev *dev)
{
    if (use_eventfd)
        return;                     /* irqfd 已自动 deassert */
    if (!dev->int_status)
        kvm_irq_line(dev->gsi, 0);
}

/* 把一条描述符链收进 segs（最多 VQ_MAX_SEGS 段）。返回 0 成功。 */
static int gather_chain(struct vq *vq, uint16_t head,
                        struct seg *segs, int *nsegs)
{
    struct vring_desc *dt = gpa2hva(vq->desc,
                                    (uint64_t)sizeof(*dt) * vq->num);
    int n = 0;
    uint32_t guard = 0;
    uint16_t idx = head;

    if (!dt)
        return -1;
    for (;;) {
        if (idx >= vq->num || guard++ > vq->num)
            return -1;              /* 越界或成环 */
        struct vring_desc d = dt[idx];
        void *hva = gpa2hva(d.addr, d.len);
        if (!hva)
            return -1;
        if (n < VQ_MAX_SEGS) {
            segs[n].hva = hva;
            segs[n].len = d.len;
            segs[n].writable = !!(d.flags & VRING_DESC_F_WRITE);
            n++;
        }
        if (!(d.flags & VRING_DESC_F_NEXT))
            break;
        idx = d.next;
    }
    *nsegs = n;
    return 0;
}

/* 从描述符链偏移 off 处拷 len 字节出来（outhdr 可能跨段） */
static void chain_read(struct seg *segs, int nsegs, size_t off,
                       void *dst, size_t len)
{
    uint8_t *d = dst;
    size_t done = 0;

    for (int i = 0; i < nsegs && done < len; i++) {
        if (off >= segs[i].len) {
            off -= segs[i].len;
            continue;
        }
        size_t take = segs[i].len - off;
        if (take > len - done)
            take = len - done;
        memcpy(d + done, (uint8_t *)segs[i].hva + off, take);
        done += take;
        off = 0;
    }
}

/* 写一个 used 元素并推进 used->idx */
static void push_used(struct vq *vq, uint16_t head, uint32_t len)
{
    uint16_t *used_idx = gpa2hva(vq->used + 2, 2);
    struct {
        uint32_t id, len;
    } *uring = gpa2hva(vq->used + 4, (uint64_t)8 * vq->num);

    if (!used_idx || !uring)
        return;
    uint16_t u = *used_idx;
    uring[u % vq->num].id = head;
    uring[u % vq->num].len = len;
    __sync_synchronize();           /* 先填 ring，再让 idx 可见 (§2.7 内存序) */
    *used_idx = u + 1;
}

/* ---- 后端 ---- */

/* console 的待回灌输入 FIFO（echo：发出去的字节再作为输入送回 guest） */
#define CON_IN_SZ 8192
static struct {
    uint8_t buf[CON_IN_SZ];
    unsigned head, tail;            /* 自由递增计数 */
} con_in;

static void con_in_push(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        con_in.buf[con_in.head++ % CON_IN_SZ] = b[i];
}
static int con_in_pop(uint8_t *b)
{
    if (con_in.tail == con_in.head)
        return 0;
    *b = con_in.buf[con_in.tail++ % CON_IN_SZ];
    return 1;
}
static size_t con_in_count(void)
{
    return con_in.head - con_in.tail;
}

struct blk_backend {
    int fd;
    uint64_t capacity;              /* 512 字节扇区数 */
};

static uint32_t console_handle(struct virtio_dev *dev, int qi,
                               struct seg *segs, int n);
static uint32_t blk_handle(struct virtio_dev *dev, int qi,
                           struct seg *segs, int n);
static void service_vq(struct virtio_dev *dev, int qi);

static uint32_t console_handle(struct virtio_dev *dev, int qi,
                               struct seg *segs, int n)
{
    if (qi == 1) {                  /* transmit: guest→host，段只读 */
        for (int i = 0; i < n; i++) {
            fwrite(segs[i].hva, 1, segs[i].len, stdout);
            con_in_push(segs[i].hva, segs[i].len);   /* echo 回 receiveq */
        }
        fflush(stdout);
        service_vq(dev, 0);         /* 有已挂的 receive 缓冲就立即投递 */
        return 0;
    }
    /* qi==0 receive: 把待回灌输入写进设备可写段 */
    uint32_t written = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *p = segs[i].hva;
        for (uint32_t j = 0; j < segs[i].len; j++) {
            uint8_t b;
            if (!con_in_pop(&b))
                return written;
            p[j] = b;
            written++;
        }
    }
    return written;
}

static uint32_t blk_handle(struct virtio_dev *dev, int qi,
                           struct seg *segs, int n)
{
    (void)qi;
    struct blk_backend *bk = dev->backend;

    if (n < 2)                      /* 至少 outhdr + status */
        return 0;

    struct {
        uint32_t type, ioprio;
        uint64_t sector;
    } hdr;
    chain_read(segs, n, 0, &hdr, sizeof(hdr));

    uint32_t type = le32toh(hdr.type);
    uint64_t off = le64toh(hdr.sector) * 512;
    int last = n - 1;
    uint8_t *status = segs[last].hva;   /* 末段 1 字节状态，设备可写 */

    *status = VIRTIO_BLK_S_OK;
    if (type == VIRTIO_BLK_T_IN) {
        uint32_t total = 0;
        for (int i = 1; i < last; i++) {
            ssize_t r = pread(bk->fd, segs[i].hva, segs[i].len, off);
            if (r < 0) {
                *status = VIRTIO_BLK_S_IOERR;
                break;
            }
            off += r;
            total += r;
            if ((size_t)r < segs[i].len)
                break;
        }
        return total + 1;           /* 读到的数据 + 状态字节 */
    }
    if (type == VIRTIO_BLK_T_OUT) {
        for (int i = 1; i < last; i++) {
            ssize_t w = pwrite(bk->fd, segs[i].hva, segs[i].len, off);
            if (w < 0) {
                *status = VIRTIO_BLK_S_IOERR;
                break;
            }
            off += w;
            if ((size_t)w < segs[i].len)
                break;
        }
        return 1;                   /* 只写了状态字节 */
    }
    if (type == VIRTIO_BLK_T_FLUSH) {
        fdatasync(bk->fd);
        return 1;
    }
    *status = VIRTIO_BLK_S_UNSUPP;
    return 1;
}

/* 该队列此刻是否该消费：console receive 只在有待回灌输入时才动 guest 缓冲，
 * 否则会白白吞掉驱动挂上的空 buffer */
static int dev_can_consume(struct virtio_dev *dev, int qi)
{
    if (dev->device_id == VIRTIO_ID_CONSOLE && qi == 0)
        return con_in_count() > 0;
    return 1;
}

static uint32_t dev_handle(struct virtio_dev *dev, int qi,
                           struct seg *segs, int n)
{
    if (dev->device_id == VIRTIO_ID_CONSOLE)
        return console_handle(dev, qi, segs, n);
    return blk_handle(dev, qi, segs, n);
}

/* 消费 avail ring 里所有就绪的描述符链 */
static void service_vq(struct virtio_dev *dev, int qi)
{
    if (qi < 0 || qi >= dev->nqueues)
        return;
    struct vq *vq = &dev->vq[qi];

    if (!vq->ready || !vq->num)
        return;
    uint16_t *avail_idx = gpa2hva(vq->avail + 2, 2);
    uint16_t *avail_ring = gpa2hva(vq->avail + 4, (uint64_t)2 * vq->num);
    if (!avail_idx || !avail_ring)
        return;

    __sync_synchronize();           /* guest 先写 desc/avail 再 notify */
    uint16_t idx = *avail_idx;
    int did = 0;

    while ((uint16_t)vq->last_avail_idx != idx) {
        if (!dev_can_consume(dev, qi))
            break;
        uint16_t head = avail_ring[vq->last_avail_idx % vq->num];
        struct seg segs[VQ_MAX_SEGS];
        int nsegs;

        if (gather_chain(vq, head, segs, &nsegs) == 0) {
            uint32_t used_len = dev_handle(dev, qi, segs, nsegs);
            push_used(vq, head, used_len);
            did = 1;
        }
        vq->last_avail_idx++;
    }
    if (did)
        virtio_raise_irq(dev);
}

/* ---- MMIO 寄存器读写 ---- */

static uint64_t virtio_mmio_read(struct virtio_dev *dev, uint64_t off, int len)
{
    struct vq *vq = &dev->vq[dev->queue_sel < VQ_MAX ? dev->queue_sel : 0];

    switch (off) {
    case VMMIO_MAGIC:        return VMMIO_MAGIC_VALUE;
    case VMMIO_VERSION:      return 2;                 /* modern */
    case VMMIO_DEVICE_ID:    return dev->device_id;
    case VMMIO_VENDOR_ID:    return 0xffff;
    case VMMIO_DEV_FEATURES:
        return (dev->dev_features >> (32 * (dev->dev_feat_sel & 1))) &
               0xffffffffu;
    case VMMIO_QUEUE_NUM_MAX:
        return (dev->queue_sel < (uint32_t)dev->nqueues) ? VRING_NUM_MAX : 0;
    case VMMIO_QUEUE_READY:  return vq->ready;
    case VMMIO_INT_STATUS:   return dev->int_status;
    case VMMIO_STATUS:       return dev->status;
    case VMMIO_CONFIG_GEN:   return 0;
    default:
        if (off >= VMMIO_CONFIG &&
            off + (uint64_t)len <= VMMIO_CONFIG + dev->config_len) {
            uint64_t v = 0;
            memcpy(&v, dev->config + (off - VMMIO_CONFIG), len);
            return v;
        }
        return 0;
    }
}

static void virtio_reset(struct virtio_dev *dev)
{
    dev->status = 0;
    dev->int_status = 0;
    kvm_irq_line(dev->gsi, 0);
    for (int i = 0; i < dev->nqueues; i++) {
        dev->vq[i].ready = 0;
        dev->vq[i].last_avail_idx = 0;
    }
}

static void virtio_mmio_write(struct virtio_dev *dev, uint64_t off, int len,
                              uint64_t val)
{
    struct vq *vq = &dev->vq[dev->queue_sel < VQ_MAX ? dev->queue_sel : 0];

    switch (off) {
    case VMMIO_DEV_FEATURES_SEL: dev->dev_feat_sel = val; break;
    case VMMIO_DRV_FEATURES_SEL: dev->drv_feat_sel = val; break;
    case VMMIO_DRV_FEATURES:
        if (dev->drv_feat_sel & 1)
            dev->drv_features |= (val & 0xffffffffu) << 32;
        else
            dev->drv_features |= val & 0xffffffffu;
        break;
    case VMMIO_QUEUE_SEL:      dev->queue_sel = val; break;
    case VMMIO_QUEUE_NUM:      vq->num = val; break;
    case VMMIO_QUEUE_READY:    vq->ready = val; break;
    case VMMIO_QUEUE_DESC_LOW:
        vq->desc = (vq->desc & 0xffffffff00000000ULL) | (uint32_t)val; break;
    case VMMIO_QUEUE_DESC_HIGH:
        vq->desc = (vq->desc & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32); break;
    case VMMIO_QUEUE_AVAIL_LOW:
        vq->avail = (vq->avail & 0xffffffff00000000ULL) | (uint32_t)val; break;
    case VMMIO_QUEUE_AVAIL_HIGH:
        vq->avail = (vq->avail & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32); break;
    case VMMIO_QUEUE_USED_LOW:
        vq->used = (vq->used & 0xffffffff00000000ULL) | (uint32_t)val; break;
    case VMMIO_QUEUE_USED_HIGH:
        vq->used = (vq->used & 0xffffffffULL) | ((uint64_t)(uint32_t)val << 32); break;
    case VMMIO_QUEUE_NOTIFY:   service_vq(dev, (int)(val & 0xffff)); break;
    case VMMIO_INT_ACK:
        dev->int_status &= ~val;
        virtio_lower_irq(dev);
        break;
    case VMMIO_STATUS:
        if (val == 0) {
            virtio_reset(dev);
        } else {
            dev->status = val;      /* FEATURES_OK 等原样保留，核心会回读确认 */
        }
        break;
    default:
        if (off >= VMMIO_CONFIG &&
            off + (uint64_t)len <= VMMIO_CONFIG + dev->config_len)
            memcpy(dev->config + (off - VMMIO_CONFIG), &val, len);
        break;
    }
}

/* ---- 设备实例 ---- */

static struct blk_backend blk_be = { .fd = -1 };

static struct virtio_dev vdev_console = {
    .base = VIRTIO_CONSOLE_BASE,
    .gsi = VIRTIO_CONSOLE_GSI,
    .device_id = VIRTIO_ID_CONSOLE,
    .name = "virtio-console",
    .dev_features = (1ULL << VIRTIO_F_VERSION_1_BIT),
    .nqueues = 2,                   /* 0=receive, 1=transmit */
    /* virtio_console_config: cols/rows/max_nr_ports/emerg_wr (LE) */
    .config = { 80, 0, 24, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
    .config_len = 12,
};

static struct virtio_dev vdev_blk = {
    .base = VIRTIO_BLK_BASE,
    .gsi = VIRTIO_BLK_GSI,
    .device_id = VIRTIO_ID_BLK,
    .name = "virtio-blk",
    .dev_features = (1ULL << VIRTIO_F_VERSION_1_BIT),
    .nqueues = 1,
    .backend = &blk_be,
    /* config.capacity 在 blk_open() 里按文件大小填 (LE64) */
    .config_len = 8,
};

/* 打开/创建 blk 后端文件；capacity 写进 config 空间前 8 字节 */
static void blk_open(const char *path)
{
    blk_be.fd = open(path, O_RDWR);
    if (blk_be.fd < 0) {
        blk_be.fd = open(path, O_RDWR | O_CREAT, 0644);
        if (blk_be.fd < 0)
            DIE("open blk 后端 %s: %s", path, strerror(errno));
        if (ftruncate(blk_be.fd, 64 << 20) < 0)
            DIE_ERRNO("ftruncate blk 后端");
        printf("[blk] 新建后端文件 %s (64MB)\n", path);
    }
    off_t sz = lseek(blk_be.fd, 0, SEEK_END);
    if (sz < 0)
        DIE_ERRNO("lseek blk 后端");
    blk_be.capacity = (uint64_t)sz / 512;
    uint64_t cap_le = htole64(blk_be.capacity);
    memcpy(vdev_blk.config, &cap_le, 8);
    printf("[blk] 后端 %s, capacity=%llu 扇区\n", path,
           (unsigned long long)blk_be.capacity);
}

/* KVM_EXIT_MMIO 入口：命中两个 virtio 窗口则分发，否则维持原"未模拟"行为 */
static int handle_virtio_mmio(void)
{
    uint64_t pa = run->mmio.phys_addr;
    int len = run->mmio.len;
    struct virtio_dev *dev = NULL;

    if (pa >= VIRTIO_CONSOLE_BASE &&
        pa < VIRTIO_CONSOLE_BASE + VIRTIO_REGION_SIZE)
        dev = &vdev_console;
    else if (pa >= VIRTIO_BLK_BASE &&
             pa < VIRTIO_BLK_BASE + VIRTIO_REGION_SIZE)
        dev = &vdev_blk;
    if (!dev)
        return -1;

    uint64_t off = pa - dev->base;
    if (run->mmio.is_write) {
        uint64_t val = 0;
        memcpy(&val, run->mmio.data, len);
        virtio_mmio_write(dev, off, len, val);
    } else {
        uint64_t val = virtio_mmio_read(dev, off, len);
        memset(run->mmio.data, 0, len);
        memcpy(run->mmio.data, &val, len);
    }
    return 0;
}

/* ---------- 项目 2 M3: ioeventfd + irqfd 数据面下沉 ----------
 * KVM_IOEVENTFD 把 guest 对 QUEUE_NOTIFY 的写绑到 eventfd，命中后**不再**
 * 退出到 VMM（case @ kvm_main.c:5266 → kvm_ioeventfd() @ eventfd.c:999）；
 * KVM_IRQFD 把中断绑到 eventfd，VMM 写 eventfd 即注入（case @
 * kvm_main.c:5257 → kvm_irqfd() @ eventfd.c:579）。对照
 * ../project2-minivmm-virtio.md M3 与 phase5 的"数据面下沉"。
 *
 * 线程模型：notify 由独立 worker 线程经 epoll 收，service_vq() 改在 worker
 * 里跑；vCPU 线程只处理其余 MMIO（寄存器配置）。单 vCPU 教学场景可接受。 */

struct notify_reg {
    struct virtio_dev *dev;
    int qi;
    int fd;
};
#define MAX_NOTIFY_REGS 8
static struct notify_reg notify_regs[MAX_NOTIFY_REGS];
static int n_notify_regs;
static int epfd = -1;

static void add_ioeventfd(struct virtio_dev *dev, int qi)
{
    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0)
        DIE_ERRNO("eventfd notify");

    struct kvm_ioeventfd iofd = {
        .datamatch = (uint64_t)qi,              /* 只匹配本队列号 */
        .addr = dev->base + VMMIO_QUEUE_NOTIFY,
        .len = 4,                                /* guest 用 writel */
        .fd = fd,
        .flags = KVM_IOEVENTFD_FLAG_DATAMATCH,
    };
    if (ioctl(vm_fd, KVM_IOEVENTFD, &iofd) < 0)
        DIE_ERRNO("KVM_IOEVENTFD");

    dev->notify_fd[qi] = fd;
    if (n_notify_regs >= MAX_NOTIFY_REGS)
        DIE("notify_reg 溢出");
    notify_regs[n_notify_regs] = (struct notify_reg){ dev, qi, fd };

    struct epoll_event ev = { .events = EPOLLIN,
                              .data.u32 = (uint32_t)n_notify_regs };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        DIE_ERRNO("epoll_ctl add notify");
    n_notify_regs++;
}

static void virtio_eventfds_setup(void)
{
    epfd = epoll_create1(0);
    if (epfd < 0)
        DIE_ERRNO("epoll_create1");

    struct virtio_dev *devs[] = { &vdev_console, &vdev_blk };
    for (size_t d = 0; d < sizeof(devs) / sizeof(devs[0]); d++) {
        struct virtio_dev *dev = devs[d];

        dev->irq_fd = eventfd(0, EFD_NONBLOCK);
        if (dev->irq_fd < 0)
            DIE_ERRNO("eventfd irq");
        struct kvm_irqfd kifd = { .fd = dev->irq_fd, .gsi = dev->gsi };
        if (ioctl(vm_fd, KVM_IRQFD, &kifd) < 0)
            DIE_ERRNO("KVM_IRQFD");

        for (int qi = 0; qi < dev->nqueues; qi++)
            add_ioeventfd(dev, qi);
    }
    printf("[vmm] M3: ioeventfd(%d) + irqfd(2) 已注册\n", n_notify_regs);
}

static void *virtio_worker(void *arg)
{
    (void)arg;
    struct epoll_event evs[MAX_NOTIFY_REGS];

    while (running) {
        int n = epoll_wait(epfd, evs, MAX_NOTIFY_REGS, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            struct notify_reg *r = &notify_regs[evs[i].data.u32];
            uint64_t cnt;
            ssize_t rd = read(r->fd, &cnt, sizeof(cnt));
            (void)rd;
            service_vq(r->dev, r->qi);
        }
    }
    return NULL;
}

/* ---------- 主循环 ---------- */

/* 前向声明：PCI 配置/BAR 转发定义在项目 3 一节（handle_io 之后） */
#define PCI_CFG_ADDR_PORT   0xcf8
#define PCI_CFG_DATA_PORT   0xcfc
static int pci_config_io(uint16_t port, int is_write, uint32_t *val, int size);
static int handle_pt_mmio(void);

static void handle_io(void)
{
    uint8_t *base = (uint8_t *)run + run->io.data_offset;
    const int is_write = (run->io.direction == KVM_EXIT_IO_OUT);

    for (uint16_t i = 0; i < run->io.count; i++) {
        uint8_t *p = base + i * run->io.size;
        uint8_t val = is_write ? *p : 0;
        int handled;

        /* PCI 配置机制 1：需要按宽度取完整值 */
        if (run->io.port == PCI_CFG_ADDR_PORT ||
            (run->io.port >= PCI_CFG_DATA_PORT &&
             run->io.port < PCI_CFG_DATA_PORT + 4)) {
            uint32_t wv = 0;
            if (is_write) {
                switch (run->io.size) {
                case 1: wv = *(uint8_t *)p; break;
                case 2: wv = *(uint16_t *)p; break;
                default: wv = *(uint32_t *)p; break;
                }
            }
            if (pci_config_io(run->io.port, is_write, &wv, run->io.size) == 0) {
                if (!is_write) {
                    switch (run->io.size) {
                    case 1: *(uint8_t *)p = wv; break;
                    case 2: *(uint16_t *)p = wv; break;
                    default: *(uint32_t *)p = wv; break;
                    }
                }
                continue;
            }
        }

        handled = (serial_io(run->io.port, is_write, &val) == 0 ||
                   power_io(run->io.port, is_write, &val) == 0);
        if (is_write)
            continue;

        /* 未建模端口读回全 1（宽读要 0xffffffff 才让 PCI 探测判定"无设备"） */
        uint32_t out = handled ? val : generic_io_in(run->io.port,
                                                    run->io.size);
        switch (run->io.size) {
        case 1: *(uint8_t *)p = out; break;
        case 2: *(uint16_t *)p = out; break;
        default: *(uint32_t *)p = out; break;
        }
    }
}

static unsigned long exit_counts[64];
static volatile sig_atomic_t stats_requested;
static void pt_stats_dump(void);

static void dump_exit_stats(void)
{
    static const char *reason_names[] = {
        [KVM_EXIT_IO] = "IO", [KVM_EXIT_HLT] = "HLT",
        [KVM_EXIT_MMIO] = "MMIO", [KVM_EXIT_SHUTDOWN] = "SHUTDOWN",
        [KVM_EXIT_IRQ_WINDOW_OPEN] = "IRQWIN",
        [KVM_EXIT_FAIL_ENTRY] = "FAIL_ENTRY",
        [KVM_EXIT_INTERNAL_ERROR] = "INTERNAL",
        [KVM_EXIT_DEBUG] = "DEBUG", [KVM_EXIT_INTR] = "INTR",
    };
    fprintf(stderr, "[stats] 用户态 exit 计数:");
    int any = 0;
    for (unsigned i = 0; i < 64; i++) {
        if (!exit_counts[i])
            continue;
        any = 1;
        fprintf(stderr, " %s=%lu",
                (i < sizeof(reason_names)/sizeof(reason_names[0]) &&
                 reason_names[i]) ? reason_names[i] : "OTHER",
                exit_counts[i]);
    }
    if (!any)
        fprintf(stderr, " (零 — KVM_RUN 从未返回用户态)");
    fprintf(stderr, "\n");
    if (passthrough_mode)
        pt_stats_dump();
}

static void run_loop(void)
{
    while (running) {
        if (ioctl(vcpu_fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR) {
                if (stats_requested) {
                    stats_requested = 0;
                    dump_exit_stats();
                }
                continue;
            }
            DIE_ERRNO("KVM_RUN");
        }

        if (run->exit_reason < 64)
            exit_counts[run->exit_reason]++;
        if (stats_requested) {
            stats_requested = 0;
            dump_exit_stats();
        }

        switch (run->exit_reason) {
        case KVM_EXIT_IO:
            handle_io();
            break;
        case KVM_EXIT_HLT:
            /* in-kernel irqchip 场景下等中断, 直接继续 */
            break;
        case KVM_EXIT_MMIO:
            if (handle_virtio_mmio() == 0)
                break;
            if (handle_pt_mmio() == 0)
                break;
            fprintf(stderr, "[mmio] 未模拟: %s @ %llx len %u\n",
                    run->mmio.is_write ? "write" : "read",
                    (unsigned long long)run->mmio.phys_addr,
                    run->mmio.len);
            if (!run->mmio.is_write)
                memset(run->mmio.data, 0, run->mmio.len);
            break;
        case KVM_EXIT_SHUTDOWN:
            fprintf(stderr, "[vmm] guest triple-fault / shutdown\n");
            return;
        case KVM_EXIT_FAIL_ENTRY:
            DIE("KVM_EXIT_FAIL_ENTRY: %llx",
                (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
        case KVM_EXIT_INTERNAL_ERROR:
            fprintf(stderr, "KVM_EXIT_INTERNAL_ERROR: suberror %u, raw:\n",
                    run->internal.suberror);
            {
                uint8_t *p = (uint8_t *)&run->internal;
                for (int i = 0; i < 48; i += 8) {
                    fprintf(stderr, "  +%02x:", i);
                    for (int j = 0; j < 8; j++)
                        fprintf(stderr, " %02x", p[i + j]);
                    fprintf(stderr, "\n");
                }
                struct kvm_regs r;
                if (ioctl(vcpu_fd, KVM_GET_REGS, &r) == 0)
                    fprintf(stderr, "  rip=%llx rflags=%llx\n",
                            (unsigned long long)r.rip,
                            (unsigned long long)r.rflags);
                fprintf(stderr, "  mem[0x100000]:");
                for (int i = 0; i < 16; i++)
                    fprintf(stderr, " %02x",
                            ((uint8_t *)guest_mem)[0x100000 + i]);
                fprintf(stderr, "\n");
            }
            DIE("KVM_EXIT_INTERNAL_ERROR: suberror %u",
                run->internal.suberror);
        default:
            fprintf(stderr, "[vmm] 未处理 exit_reason %u\n",
                    run->exit_reason);
            break;
        }
    }
}

/* ---------- 信号与终端 ---------- */

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static void on_stats(int sig)
{
    (void)sig;
    stats_requested = 1;
}

static void termios_restore(void)
{
    if (termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static void termios_raw(void)
{
    if (!isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0)
        return;
    termios_saved = 1;
    atexit(termios_restore);
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* ---------- 项目 3: MP 表（让 guest 进 APIC/IOAPIC 模式） ----------
 * 没有 MP 表/ACPI MADT 时 guest 走 virtual-wire + 8259（项目 1/2 即如此），
 * LAPIC/IOAPIC 不启用，PCI MSI 无从谈起。给 guest 一张 Intel MP 规范 1.4
 * 的表：内核 find_mp_table() 会扫 0x0-0x400 / 639KB / 0xF0000 三段找
 * MP 浮动指针（mpparse.c:612-614），我们把浮动指针放 639KB(0x9fc00) 处。
 * 结构定义对齐 arch/x86/include/asm/mpspec_def.h。
 * IOAPIC 地址/ID 与 KVM in-kernel ioapic 对齐：base 0xfec00000
 * (ioapic.h:18 IOAPIC_DEFAULT_BASE_ADDRESS)、id=0 (ioapic.c:704)。 */

#define MP_FLOAT_GPA    0x9fc00UL
#define MP_TABLE_GPA    0x9fd00UL
#define MP_LAPIC_ADDR   0xfee00000UL
#define MP_IOAPIC_ADDR  0xfec00000UL

struct mpf_intel {
    char signature[4];      /* "_MP_" */
    uint32_t physptr;
    uint8_t length;         /* 单位: 16 字节 */
    uint8_t specification;
    uint8_t checksum;
    uint8_t feature1, feature2, feature3, feature4, feature5;
};                          /* 16 字节 */

struct mpc_table {
    char signature[4];      /* "PCMP" */
    uint16_t length;
    char spec;
    char checksum;
    char oem[8];
    char productid[12];
    uint32_t oemptr;
    uint16_t oemsize;
    uint16_t oemcount;
    uint32_t lapic;
    uint32_t reserved;
};                          /* 44 字节 */

struct mpc_cpu {
    uint8_t type;           /* 0 = MP_PROCESSOR */
    uint8_t apicid;
    uint8_t apicver;
    uint8_t cpuflag;
    uint32_t cpufeature;
    uint32_t featureflag;
    uint32_t reserved[2];
};                          /* 20 字节 */

struct mpc_bus {
    uint8_t type;           /* 1 = MP_BUS */
    uint8_t busid;
    char bustype[6];
};                          /* 8 字节 */

struct mpc_ioapic {
    uint8_t type;           /* 2 = MP_IOAPIC */
    uint8_t apicid;
    uint8_t apicver;
    uint8_t flags;          /* MPC_APIC_USABLE = 1 */
    uint32_t apicaddr;
};                          /* 8 字节 */

struct mpc_intsrc {
    uint8_t type;           /* 3 = MP_INTSRC */
    uint8_t irqtype;        /* 0 = mp_INT */
    uint16_t irqflag;
    uint8_t srcbus;
    uint8_t srcbusirq;
    uint8_t dstapic;
    uint8_t dstirq;
};                          /* 8 字节 */

struct mpc_lintsrc {
    uint8_t type;           /* 4 = MP_LINTSRC */
    uint8_t irqtype;        /* 3=ExtINT, 1=NMI */
    uint16_t irqflag;
    uint8_t srcbusid;
    uint8_t srcbusirq;
    uint8_t destapic;
    uint8_t destapiclint;
};                          /* 8 字节 */

static uint8_t mp_checksum(void *p, size_t len)
{
    uint8_t sum = 0;
    uint8_t *b = p;

    for (size_t i = 0; i < len; i++)
        sum += b[i];
    return (uint8_t)(0x100 - sum);
}

static void build_mptable(void)
{
    uint8_t *base = (uint8_t *)guest_mem + MP_TABLE_GPA;
    uint8_t *p = base;

    struct mpc_table *mt = (struct mpc_table *)p;
    memset(mt, 0, sizeof(*mt));
    memcpy(mt->signature, "PCMP", 4);
    mt->spec = 4;                                   /* MP spec 1.4 */
    memcpy(mt->oem, "MINIVMM ", 8);
    memcpy(mt->productid, "KVM-STUDY   ", 12);
    mt->lapic = MP_LAPIC_ADDR;
    p += sizeof(*mt);

    struct mpc_cpu *cpu = (struct mpc_cpu *)p;
    memset(cpu, 0, sizeof(*cpu));
    cpu->type = 0;                                  /* MP_PROCESSOR */
    cpu->apicid = 0;
    cpu->apicver = 0x14;
    cpu->cpuflag = 0x3;                             /* ENABLED|BOOTPROCESSOR */
    p += sizeof(*cpu);

    struct mpc_bus *bus = (struct mpc_bus *)p;
    bus->type = 1; bus->busid = 0;
    memcpy(bus->bustype, "ISA   ", 6);
    p += sizeof(*bus);
    bus = (struct mpc_bus *)p;
    bus->type = 1; bus->busid = 1;
    memcpy(bus->bustype, "PCI   ", 6);
    p += sizeof(*bus);

    struct mpc_ioapic *ioapic = (struct mpc_ioapic *)p;
    ioapic->type = 2;
    ioapic->apicid = 0;                             /* 与 KVM ioapic->id 一致 */
    ioapic->apicver = 0x11;
    ioapic->flags = 1;                              /* MPC_APIC_USABLE */
    ioapic->apicaddr = MP_IOAPIC_ADDR;
    p += sizeof(*ioapic);

    /* ISA IRQ 恒等映射到 IOAPIC pin（跳过级联的 IRQ2） */
    for (int irq = 0; irq < 16; irq++) {
        if (irq == 2)
            continue;
        struct mpc_intsrc *is = (struct mpc_intsrc *)p;
        memset(is, 0, sizeof(*is));
        is->type = 3;                               /* MP_INTSRC, mp_INT */
        is->srcbus = 0;                             /* ISA bus */
        is->srcbusirq = irq;
        is->dstapic = 0;                            /* IOAPIC id */
        is->dstirq = irq;
        p += sizeof(*is);
    }

    struct mpc_lintsrc *ls = (struct mpc_lintsrc *)p;
    memset(ls, 0, sizeof(*ls));
    ls->type = 4; ls->irqtype = 3;                  /* LINT0 = ExtINT */
    ls->destapic = 0; ls->destapiclint = 0;
    p += sizeof(*ls);
    ls = (struct mpc_lintsrc *)p;
    memset(ls, 0, sizeof(*ls));
    ls->type = 4; ls->irqtype = 1;                  /* LINT1 = NMI */
    ls->destapic = 0; ls->destapiclint = 1;
    p += sizeof(*ls);

    mt->length = (uint16_t)(p - base);
    mt->checksum = 0;
    mt->checksum = mp_checksum(base, mt->length);

    struct mpf_intel *mpf =
        (struct mpf_intel *)((uint8_t *)guest_mem + MP_FLOAT_GPA);
    memset(mpf, 0, sizeof(*mpf));
    memcpy(mpf->signature, "_MP_", 4);
    mpf->physptr = MP_TABLE_GPA;
    mpf->length = 1;                                /* 16 字节 */
    mpf->specification = 4;
    mpf->checksum = 0;
    mpf->checksum = mp_checksum(mpf, sizeof(*mpf));

    printf("[mp] MP 表: float@%lx config@%lx len=%u\n",
           (unsigned long)MP_FLOAT_GPA, (unsigned long)MP_TABLE_GPA,
           mt->length);
}

/* ---------- 项目 3: VFIO 直通 + PCI 配置空间 ----------
 * fd 层级 /dev/vfio/vfio → group → device（include/uapi/linux/vfio.h）；
 * DMA 用 VFIO_IOMMU_MAP_DMA 把 guest RAM 以 IOVA=GPA 1:1 映射
 * （../project3-minivmm-vfio.md M2、../phase6-vfio/README.md）。
 * guest 无 ACPI，PCI 枚举走配置机制 1（PIO 0xcf8/0xcfc）：host bridge 放
 * 00:00.0，直通设备放 00:01.0，config 读写转发 VFIO_PCI_CONFIG_REGION。
 * BAR 用影子寄存器做 sizing（写全 1 读回 size mask），64-bit BAR 占两个
 * dword。数据面 BAR 访问经 KVM_EXIT_MMIO 转发 VFIO region。 */

#define PT_BUS              0
#define PT_DEVFN            0x08        /* 00:01.0 */
#define VFIO_BDF            "0000:4b:00.0"
#define VFIO_GROUP_ID       35
#define PCI_NUM_BARS        6

static struct {
    int container, group, device;
    struct vfio_region_info *region[VFIO_PCI_NUM_REGIONS];
    uint64_t bar_size[PCI_NUM_BARS];
    uint32_t bar_type[PCI_NUM_BARS];    /* BAR 低 4 位（类型/预取） */
    int bar_is64[PCI_NUM_BARS];
    uint64_t bar_val[PCI_NUM_BARS];     /* guest 写入的影子值 */
    uint8_t hostbridge_cfg[256];
    uint16_t msix_cfg_vec, msix_q_vec;  /* 影子：legacy 向量寄存器，仅统计 */
    /* ---- MSI-X 路由（项目3 M4） ---- */
    int msix_cap;                   /* MSI-X cap 在 config 的偏移 */
    int msix_nr;                    /* 向量数 */
    uint32_t msix_table_off;        /* MSI-X 表在 BAR 内偏移 */
    int msix_table_bar;             /* MSI-X 表所在 BAR */
    uint8_t msix_shadow[32 * 16];   /* 影子 MSI-X 表(每向量16B) */
    uint16_t msix_msgctl;           /* 影子 Message Control(Enable/FuncMask 可写) */
    int msix_efd[32];
    int msix_routing_done;
    uint64_t pt_notifies;           /* BAR0+0x10 (QUEUE_NOTIFY) 写计数 */
    uint16_t pt_notify_last;        /* 最近一次 notify 的队列号 */
    uint64_t pt_ctrl_fwd;           /* 转发到物理表的 Vector Control 写次数 */
    int ready;
} vf;

static uint32_t pci_cfg_addr;           /* 0xcf8 最后写入值 */

static void vfio_setup(void)
{
    vf.container = open("/dev/vfio/vfio", O_RDWR);
    if (vf.container < 0)
        DIE_ERRNO("open /dev/vfio/vfio");
    if (ioctl(vf.container, VFIO_GET_API_VERSION) != VFIO_API_VERSION)
        DIE("VFIO_API_VERSION 不匹配");
    if (!ioctl(vf.container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU))
        DIE("不支持 VFIO_TYPE1_IOMMU");

    char gpath[64];
    snprintf(gpath, sizeof(gpath), "/dev/vfio/%d", VFIO_GROUP_ID);
    vf.group = open(gpath, O_RDWR);
    if (vf.group < 0)
        DIE_ERRNO("open vfio group");
    if (ioctl(vf.group, VFIO_GROUP_SET_CONTAINER, &vf.container) < 0)
        DIE_ERRNO("VFIO_GROUP_SET_CONTAINER");
    if (ioctl(vf.container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0)
        DIE_ERRNO("VFIO_SET_IOMMU");

    vf.device = ioctl(vf.group, VFIO_GROUP_GET_DEVICE_FD, VFIO_BDF);
    if (vf.device < 0)
        DIE_ERRNO("VFIO_GROUP_GET_DEVICE_FD");

    struct vfio_device_info dinfo = { .argsz = sizeof(dinfo) };
    if (ioctl(vf.device, VFIO_DEVICE_GET_INFO, &dinfo) < 0)
        DIE_ERRNO("VFIO_DEVICE_GET_INFO");
    /* 清掉设备被宿主前一个驱动用过的残留状态 */
    if (dinfo.flags & VFIO_DEVICE_FLAGS_RESET)
        fprintf(stderr, "[vfio] VFIO_DEVICE_RESET rc=%d\n",
                ioctl(vf.device, VFIO_DEVICE_RESET));

    for (uint32_t i = 0; i < dinfo.num_regions && i < VFIO_PCI_NUM_REGIONS; i++) {
        struct vfio_region_info *ri = calloc(1, sizeof(*ri));
        ri->argsz = sizeof(*ri);
        ri->index = i;
        if (ioctl(vf.device, VFIO_DEVICE_GET_REGION_INFO, ri) < 0) {
            free(ri);
            continue;
        }
        vf.region[i] = ri;
    }

    /* guest RAM → IOMMU，IOVA = GPA 1:1 */
    struct vfio_iommu_type1_dma_map dma = {
        .argsz = sizeof(dma),
        .vaddr = (uint64_t)(uintptr_t)guest_mem,
        .iova = 0,
        .size = mem_size,
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
    };
    if (ioctl(vf.container, VFIO_IOMMU_MAP_DMA, &dma) < 0)
        DIE_ERRNO("VFIO_IOMMU_MAP_DMA");

    /* BAR 尺寸/类型：尺寸取 VFIO region size，类型读真实 config 低 4 位 */
    for (int b = 0; b < PCI_NUM_BARS; b++) {
        struct vfio_region_info *ri = vf.region[VFIO_PCI_BAR0_REGION_INDEX + b];
        if (!ri || ri->size == 0)
            continue;
        vf.bar_size[b] = ri->size;
        uint32_t raw = 0;
        if (pread(vf.device, &raw, 4,
                  vf.region[VFIO_PCI_CONFIG_REGION_INDEX]->offset + 0x10 + b * 4) < 0)
            raw = 0;
        vf.bar_type[b] = raw & 0xf;
        vf.bar_is64[b] = ((raw >> 1) & 0x3) == 0x2;
    }

    /* host bridge config：vendor 0x8086 / device 0x1237 / class 06:00 */
    memset(vf.hostbridge_cfg, 0, sizeof(vf.hostbridge_cfg));
    vf.hostbridge_cfg[0x00] = 0x86; vf.hostbridge_cfg[0x01] = 0x80; /* 8086 */
    vf.hostbridge_cfg[0x02] = 0x37; vf.hostbridge_cfg[0x03] = 0x12; /* 1237 */
    vf.hostbridge_cfg[0x04] = 0x06;                                  /* cmd: mem+io */
    vf.hostbridge_cfg[0x0a] = 0x00;
    vf.hostbridge_cfg[0x0b] = 0x06;                                  /* class 06:00 */
    vf.hostbridge_cfg[0x0e] = 0x00;                                  /* hdr type 0 */

    /* 定位 MSI-X cap 并读 Table/PBA 几何 */
    uint64_t cfg_off = vf.region[VFIO_PCI_CONFIG_REGION_INDEX]->offset;
    ssize_t rr;
    vf.msix_cap = 0;
    uint8_t cap_ptr = 0;
    rr = pread(vf.device, &cap_ptr, 1, cfg_off + 0x34);
    (void)rr;
    for (int i = 0, off = cap_ptr & 0xfc; off && i < 16; i++) {
        uint8_t id = 0, nxt = 0;
        rr = pread(vf.device, &id, 1, cfg_off + off);
        rr = pread(vf.device, &nxt, 1, cfg_off + off + 1);
        (void)rr;
        if (id == 0x11) { vf.msix_cap = off; break; }
        off = nxt & 0xfc;
    }
    if (vf.msix_cap) {
        uint16_t msgctl = 0;
        uint32_t tbl = 0;
        rr = pread(vf.device, &msgctl, 2, cfg_off + vf.msix_cap + 2);
        rr = pread(vf.device, &tbl, 4, cfg_off + vf.msix_cap + 4);
        (void)rr;
        vf.msix_nr = (msgctl & 0x7ff) + 1;
        vf.msix_msgctl = msgctl;        /* 影子初值=设备真实 Message Control */
        vf.msix_table_off = tbl & ~0x7u;
        vf.msix_table_bar = tbl & 0x7;
        for (int v = 0; v < 32; v++)
            vf.msix_efd[v] = -1;
    }

    vf.ready = 1;
    printf("[vfio] 直通 %s (group %d): device fd=%d, BAR0=%llu BAR2=%llu\n",
           VFIO_BDF, VFIO_GROUP_ID, vf.device,
           (unsigned long long)vf.bar_size[0],
           (unsigned long long)vf.bar_size[2]);
    printf("[vfio] MSI-X cap@%#x nr=%d table=BAR%d+%#x\n",
           vf.msix_cap, vf.msix_nr, vf.msix_table_bar, vf.msix_table_off);
}

static int msix_setup_irqs(void);
static int msix_update_routes(void);

/* 直通设备 config 读：BAR 用影子值，其余转发 VFIO */
static uint32_t pt_config_read(uint32_t reg, int len)
{
    /* MSI-X Message Control 用影子（guest 写 Enable 后读回要能看到） */
    if (vf.msix_cap && reg >= (uint32_t)vf.msix_cap + 2 &&
        reg + (uint32_t)len <= (uint32_t)vf.msix_cap + 4) {
        uint32_t val = 0, mc = vf.msix_msgctl;
        for (int i = 0; i < len; i++)
            val |= ((mc >> ((reg - (vf.msix_cap + 2) + i) * 8)) & 0xff) << (i * 8);
        return val;
    }
    if (reg >= 0x10 && reg < 0x28) {
        int bar_idx = -1, hi = 0;
        for (int i = 0; i < PCI_NUM_BARS; i++) {
            uint32_t lo_reg = 0x10 + i * 4;
            if (reg >= lo_reg && reg < lo_reg + 4) { bar_idx = i; hi = 0; break; }
            if (vf.bar_is64[i] && reg >= lo_reg + 4 && reg < lo_reg + 8) {
                bar_idx = i; hi = 1; break;
            }
        }
        if (bar_idx >= 0 && vf.bar_size[bar_idx]) {
            uint64_t v = vf.bar_val[bar_idx];
            uint32_t r = hi ? (uint32_t)(v >> 32)
                            : (((uint32_t)v & ~(uint32_t)(vf.bar_size[bar_idx] - 1)) |
                               vf.bar_type[bar_idx]);
            if (len == 4)
                return r;
            return (r >> ((reg & 3) * 8)) & (len == 2 ? 0xffffu : 0xffu);
        }
    }
    uint32_t val = 0;
    if (pread(vf.device, &val, len,
              vf.region[VFIO_PCI_CONFIG_REGION_INDEX]->offset + reg) < 0)
        val = 0xffffffff;
    return val;
}

static void pt_config_write(uint32_t reg, int len, uint32_t val)
{
    if (reg >= 0x10 && reg < 0x28 && len == 4) {
        int bar_idx = -1, hi = 0;
        for (int i = 0; i < PCI_NUM_BARS; i++) {
            uint32_t lo_reg = 0x10 + i * 4;
            if (reg == lo_reg) { bar_idx = i; hi = 0; break; }
            if (vf.bar_is64[i] && reg == lo_reg + 4) { bar_idx = i; hi = 1; break; }
        }
        if (bar_idx >= 0 && vf.bar_size[bar_idx]) {
            if (hi)
                vf.bar_val[bar_idx] = ((uint64_t)val << 32) |
                                      (vf.bar_val[bar_idx] & 0xffffffffULL);
            else
                vf.bar_val[bar_idx] = (vf.bar_val[bar_idx] & 0xffffffff00000000ULL) |
                                      (val & 0xfffffff0u);
            return;
        }
    }
    /* MSI-X Message Control(cap+2)：Enable=bit15、Function Mask=bit14 可写，
     * 其余(表大小)只读。写入只更新影子并供读回，不转发物理设备——物理侧
     * 的 MSI-X 使能由 VFIO 在 SET_IRQS 时经内核 PCI MSI 代码完成。
     * 触发时机依据 6.12 msix_capability_init 实测顺序（msi.c:725 先置
     * ENABLE|MASKALL 使表可访问 → :740 编程表项 → :756 msix_mask_all →
     * :758 清 MASKALL 才算真启用）：清 MASKALL 那一刻影子表已编程完毕，
     * 此时建 eventfd/KVM 路由并 SET_IRQS 武装。 */
    if (vf.msix_cap && reg >= (uint32_t)vf.msix_cap + 2 &&
        reg < (uint32_t)vf.msix_cap + 4) {
        uint32_t base = vf.msix_cap + 2;
        for (int i = 0; i < len; i++) {
            uint8_t b = (val >> (i * 8)) & 0xff;
            int bitpos = (reg + i - base) * 8;      /* 在 u16 内的位偏移 */
            if (bitpos >= 0 && bitpos < 16) {
                uint16_t mask = 0xc000 & (0xff << bitpos);   /* 只允许可写位 */
                vf.msix_msgctl = (vf.msix_msgctl & ~mask) |
                                 ((uint16_t)(b << bitpos) & mask);
            }
        }
        int enable = !!(vf.msix_msgctl & 0x8000);
        int funcmask = !!(vf.msix_msgctl & 0x4000);
        if (enable && !funcmask && !vf.msix_routing_done)
            msix_setup_irqs();
        return;
    }
    if (pwrite(vf.device, &val, len,
               vf.region[VFIO_PCI_CONFIG_REGION_INDEX]->offset + reg) < 0)
        fprintf(stderr, "[vfio] config write reg=%x 失败\n", reg);
}

/* 配置机制 1 的 PIO 分发；返回 0 表示已处理 */
static int pci_config_io(uint16_t port, int is_write, uint32_t *val, int size)
{
    if (!vf.ready)
        return -1;
    if (port == PCI_CFG_ADDR_PORT && size == 4) {
        if (is_write)
            pci_cfg_addr = *val;
        else
            *val = pci_cfg_addr;
        return 0;
    }
    if (port >= PCI_CFG_DATA_PORT && port < PCI_CFG_DATA_PORT + 4) {
        if (!(pci_cfg_addr & 0x80000000)) {
            if (!is_write)
                *val = 0xffffffff;
            return 0;
        }
        uint32_t bus = (pci_cfg_addr >> 16) & 0xff;
        uint32_t devfn = (pci_cfg_addr >> 8) & 0xff;
        uint32_t reg = (pci_cfg_addr & 0xfc) | (port - PCI_CFG_DATA_PORT);

        if (bus != PT_BUS) {
            if (!is_write)
                *val = 0xffffffff;
            return 0;
        }
        if (devfn == 0x00) {            /* host bridge */
            if (is_write) {
                for (int i = 0; i < size && reg + i < 256; i++)
                    vf.hostbridge_cfg[reg + i] = (*val >> (i * 8)) & 0xff;
            } else {
                uint32_t v = 0;
                for (int i = 0; i < size && reg + i < 256; i++)
                    v |= (uint32_t)vf.hostbridge_cfg[reg + i] << (i * 8);
                *val = v;
            }
            return 0;
        }
        if (devfn == PT_DEVFN) {        /* 直通设备 */
            if (is_write)
                pt_config_write(reg, size, *val);
            else
                *val = pt_config_read(reg, size);
            return 0;
        }
        if (!is_write)
            *val = 0xffffffff;          /* 无设备 */
        return 0;
    }
    return -1;
}

#define MSIX_GSI_BASE 24

/* 从影子 MSI-X 表重建 KVM 中断路由(默认 IOAPIC/PIC + 各有效向量的 MSI 路由)。
 * 每次 guest 改 MSI-X 表都重设一遍，保证路由始终反映最新表项。 */
static int msix_update_routes(void)
{
    int nr = vf.msix_nr > 32 ? 32 : vf.msix_nr;
    int maxent = 24 + 16 + nr + 4;
    struct kvm_irq_routing *rt =
        calloc(1, sizeof(*rt) + maxent * sizeof(struct kvm_irq_routing_entry));
    if (!rt)
        return -1;
    int n = 0;

    for (int g = 0; g < 24; g++) {          /* IOAPIC pin 0-23 */
        struct kvm_irq_routing_entry *e = &rt->entries[n++];
        e->gsi = g;
        e->type = KVM_IRQ_ROUTING_IRQCHIP;
        e->u.irqchip.irqchip = KVM_IRQCHIP_IOAPIC;
        e->u.irqchip.pin = g;
    }
    for (int g = 0; g < 16; g++) {          /* 8259 PIC pin 0-15 */
        struct kvm_irq_routing_entry *e = &rt->entries[n++];
        e->gsi = g;
        e->type = KVM_IRQ_ROUTING_IRQCHIP;
        e->u.irqchip.irqchip = (g < 8) ? KVM_IRQCHIP_PIC_MASTER
                                       : KVM_IRQCHIP_PIC_SLAVE;
        e->u.irqchip.pin = (g < 8) ? g : g - 8;
    }

    int active = 0;
    for (int v = 0; v < nr; v++) {
        uint8_t *ent = &vf.msix_shadow[v * 16];
        uint32_t addr_lo, addr_hi, data, ctrl;
        memcpy(&addr_lo, ent + 0, 4);
        memcpy(&addr_hi, ent + 4, 4);
        memcpy(&data,    ent + 8, 4);
        memcpy(&ctrl,    ent + 12, 4);
        (void)ctrl;
        if (!addr_lo && !addr_hi)
            continue;                        /* 未编程(无地址) */
        struct kvm_irq_routing_entry *e = &rt->entries[n++];
        e->gsi = MSIX_GSI_BASE + v;
        e->type = KVM_IRQ_ROUTING_MSI;
        e->u.msi.address_lo = addr_lo;
        e->u.msi.address_hi = addr_hi;
        e->u.msi.data = data;
        active++;
    }
    rt->nr = n;
    if (ioctl(vm_fd, KVM_SET_GSI_ROUTING, rt) < 0)
        fprintf(stderr, "[msix] KVM_SET_GSI_ROUTING: %s\n", strerror(errno));
    free(rt);
    return active;
}

/* 慢速路径代理线程：只在 msix_slowpath 下启用。此时没有 KVM_IRQFD consumer，
 * irq_bypass 不会撮合、IRTE 留在 Remapped 模式，设备每个 MSI-X 必走
 * vfio_msihandler（vfio_pci_intrs.c:373）→ eventfd_signal，这里收计数并
 * 经 KVM_IRQ_LINE（gsi=24+v 的 MSI 路由条目）注入。 */
static uint64_t slow_cnt[32];
static void *msix_slowpath_thread(void *arg)
{
    (void)arg;
    int nr = vf.msix_nr > 32 ? 32 : vf.msix_nr;
    struct pollfd pfds[32];
    int idx[32], n = 0;
    for (int v = 0; v < nr; v++) {
        if (vf.msix_efd[v] < 0)
            continue;
        pfds[n].fd = vf.msix_efd[v];
        pfds[n].events = POLLIN;
        idx[n++] = v;
    }
    if (!n)
        return NULL;
    time_t last_fire = time(NULL), last_msg = last_fire;
    while (running) {
        int rc = poll(pfds, n, 1000);
        if (rc <= 0) {
            time_t t = time(NULL);
            if (t - last_fire >= 5 && t - last_msg >= 5) {
                fprintf(stderr, "[msix-slow] 已 %lds 无设备中断触发"
                        "（notifies=%llu）\n", (long)(t - last_fire),
                        (unsigned long long)vf.pt_notifies);
                last_msg = t;
            }
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (!(pfds[i].revents & POLLIN))
                continue;
            uint64_t c;
            if (read(pfds[i].fd, &c, sizeof(c)) != (ssize_t)sizeof(c))
                continue;
            int v = idx[i];
            slow_cnt[v] += c;
            struct kvm_irq_level lvl = { .irq = MSIX_GSI_BASE + v, .level = 1 };
            if (ioctl(vm_fd, KVM_IRQ_LINE, &lvl) < 0)
                fprintf(stderr, "[msix-slow] KVM_IRQ_LINE vec%d: %s\n",
                        v, strerror(errno));
            if (slow_cnt[v] <= 8 || slow_cnt[v] % 256 == 0)
                fprintf(stderr, "[msix-slow] vec%d 设备中断触发 #%llu\n", v,
                        (unsigned long long)slow_cnt[v]);
        }
        last_fire = time(NULL);
    }
    return NULL;
}

/* 直通模式统计（SIGUSR1 / 退出时）：数据面与中断面观测点 */
static void pt_stats_dump(void)
{
    fprintf(stderr, "[stats] pt: QUEUE_NOTIFY 写=%llu（最后队列=%u） ctrl 转发=%llu",
            (unsigned long long)vf.pt_notifies, vf.pt_notify_last,
            (unsigned long long)vf.pt_ctrl_fwd);
    if (msix_slowpath) {
        int nr = vf.msix_nr > 32 ? 32 : vf.msix_nr;
        for (int v = 0; v < nr; v++)
            if (slow_cnt[v])
                fprintf(stderr, " msix_vec%d=%llu", v,
                        (unsigned long long)slow_cnt[v]);
    }
    fprintf(stderr, "\n");
}

/* 一次性：为各有效向量建 eventfd + KVM_IRQFD，并 VFIO_DEVICE_SET_IRQS(MSIX)
 * 让 VFIO 用这些 eventfd 接管向量。对照 QEMU hw/vfio/pci.c
 * vfio_msix_vector_do_use()/vfio_connect_kvm_msi_virq()。 */
static int msix_setup_irqs(void)
{
    int nr = vf.msix_nr > 32 ? 32 : vf.msix_nr;

    int active = msix_update_routes();       /* 先把路由建好 */

    for (int v = 0; v < nr; v++) {
        uint8_t *ent = &vf.msix_shadow[v * 16];
        uint32_t addr_lo, addr_hi, ctrl;
        memcpy(&addr_lo, ent + 0, 4);
        memcpy(&addr_hi, ent + 4, 4);
        memcpy(&ctrl,    ent + 12, 4);
        (void)ctrl;
        if (!addr_lo && !addr_hi)
            continue;
        vf.msix_efd[v] = eventfd(0, 0);
        if (!msix_slowpath) {
            struct kvm_irqfd kifd = { .fd = vf.msix_efd[v],
                                      .gsi = MSIX_GSI_BASE + v };
            if (ioctl(vm_fd, KVM_IRQFD, &kifd) < 0)
                fprintf(stderr, "[msix] KVM_IRQFD vec%d: %s\n",
                        v, strerror(errno));
        }
    }

    struct {
        struct vfio_irq_set hdr;
        int fds[32];
    } irqset;
    memset(&irqset, 0, sizeof(irqset));
    irqset.hdr.argsz = sizeof(struct vfio_irq_set) + nr * sizeof(int);
    irqset.hdr.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irqset.hdr.index = VFIO_PCI_MSIX_IRQ_INDEX;
    irqset.hdr.start = 0;
    irqset.hdr.count = nr;
    for (int v = 0; v < nr; v++)
        irqset.fds[v] = vf.msix_efd[v];     /* 未编程向量保持 -1 */
    if (ioctl(vf.device, VFIO_DEVICE_SET_IRQS, &irqset) < 0) {
        fprintf(stderr, "[msix] VFIO_DEVICE_SET_IRQS: %s\n", strerror(errno));
        return -1;
    }

    vf.msix_routing_done = 1;

    /* 同步 mask 状态。武装时内核经 request_irq → irq_startup →
     * pci_msix_unmask（msi.h:51，pci_msix_write_vector_ctrl 直写物理表）
     * 已把武装向量全部放行（vfio_pci_intrs.c:510 request_irq flags=0）；
     * 而 guest 清 MASKALL 那一刻各向量恰好是 msix_mask_all 写的全 mask
     * （msix_capability_init 顺序 msi.c:740→756→758：编程表 → 全 mask →
     * 清 MASKALL），随后驱动的 request_irq 才逐向量写 ctrl=0 解禁
     * （pci_msix_unmask）。故这里把影子表的 ctrl 写回物理表（只写条目内
     * 偏移 12 的 Vector Control；addr/data 是内核写的宿主 MSI 消息，
     * 不能动），使设备侧与 guest 一致。之后 guest 的解禁写在
     * handle_pt_mmio 直接转发物理表。
     * 注意：VFIO 对 MSI-X 没有 ACTION_MASK/UNMASK（vfio_pci_intrs.c:854
     * "XXX Need masking support exported"），直写表是唯一路径，QEMU 同款。
     * 该设备物理表读回恒为全 0xff（msixdump 实测），不读回验证。 */
    struct vfio_region_info *tri =
        vf.region[VFIO_PCI_BAR0_REGION_INDEX + vf.msix_table_bar];
    if (tri) {
        for (int v = 0; v < nr; v++) {
            uint8_t *ent = &vf.msix_shadow[v * 16];
            uint32_t a0, a1, ctrl;
            memcpy(&a0, ent + 0, 4);
            memcpy(&a1, ent + 4, 4);
            if (!a0 && !a1)
                continue;
            memcpy(&ctrl, ent + 12, 4);
            if (pwrite(vf.device, &ctrl, 4,
                       tri->offset + vf.msix_table_off + v * 16 + 12) == 4)
                vf.pt_ctrl_fwd++;
        }
    }

    printf("[msix] 路由就绪: %d/%d 向量已编程（%s 路径）\n", active, nr,
           msix_slowpath ? "慢速用户态代理" : "KVM_IRQFD/PI");
    for (int v = 0; v < nr; v++) {          /* 打印 guest 编程的表项 */
        uint8_t *ent = &vf.msix_shadow[v * 16];
        uint32_t a0, a1, d;
        memcpy(&a0, ent + 0, 4);
        memcpy(&a1, ent + 4, 4);
        memcpy(&d,  ent + 8, 4);
        if (!a0 && !a1)
            continue;
        fprintf(stderr, "[msix] vec%d: addr=%08x:%08x data=%08x\n",
                v, a1, a0, d);
    }
    if (msix_slowpath) {
        pthread_t stid;
        if (pthread_create(&stid, NULL, msix_slowpath_thread, NULL) != 0)
            DIE_ERRNO("pthread_create msix_slowpath");
    }
    return 0;
}

/* legacy virtio BAR0 直通（实测定论，见 corrections.md F 附带实验）：
 * 该设备是标准 legacy 布局，且随**自身** MSI-X Enable 状态切换——
 * 未启用时 config@0x14；经 VFIO_DEVICE_SET_IRQS 启用后 0x14/0x16 变为
 * config/queue 向量寄存器、config 移到 0x18（msixdump -a 实测：arm 后
 * [0x14]=0x0000ffff(NO_VECTOR×2)、[0x18]/[0x1c]=capacity 0/1，写 0x14/0x16
 * 可读写回）。guest 启用 MSI-X 后的视图（VIRTIO_PCI_CONFIG_OFF(msix)=0x18、
 * 向量寄存器@0x14/0x16，virtio_pci.h:74-80）与设备 arm 后的布局完全一致，
 * 直接透传即可；此前截留向量寄存器导致设备不知队列对应哪个 MSI-X 表项、
 * 完成请求也不发中断（guest I/O 挂起的根因），`-4` 换算与
 * capacity 读成 0x1FFFFFFFF 也同源于此。 */
static int handle_bar0_legacy(uint64_t off, int is_write, void *data, int len)
{
    struct vfio_region_info *ri = vf.region[VFIO_PCI_BAR0_REGION_INDEX];

    /* QUEUE_NOTIFY（virtio_pci.h:42）：计数，用于判定 notify 是否到达 */
    if (off == 0x10 && is_write) {
        uint16_t q = 0;
        memcpy(&q, data, len < 2 ? len : 2);
        vf.pt_notify_last = q;
        vf.pt_notifies++;
    }
    /* 向量寄存器影子（仅供统计；真值在设备里，读写都透传） */
    if (off >= 0x14 && off < 0x18 && is_write) {
        uint8_t regs[4];
        regs[0] = vf.msix_cfg_vec & 0xff; regs[1] = vf.msix_cfg_vec >> 8;
        regs[2] = vf.msix_q_vec & 0xff;   regs[3] = vf.msix_q_vec >> 8;
        memcpy(regs + (off - 0x14), data, len);
        vf.msix_cfg_vec = regs[0] | (regs[1] << 8);
        vf.msix_q_vec   = regs[2] | (regs[3] << 8);
    }
    ssize_t rc = is_write
        ? pwrite(vf.device, data, len, ri->offset + off)
        : pread(vf.device, data, len, ri->offset + off);
    (void)rc;
    return 0;
}

/* BAR MMIO 转发：guest 访问已分配 BAR 的 GPA → VFIO region pread/pwrite */
static int handle_pt_mmio(void)
{
    if (!vf.ready)
        return -1;
    uint64_t pa = run->mmio.phys_addr;

    for (int b = 0; b < PCI_NUM_BARS; b++) {
        uint64_t base = vf.bar_val[b] & ~0xfULL;
        if (!base || !vf.bar_size[b])
            continue;
        if (pa >= base && pa < base + vf.bar_size[b]) {
            uint64_t off = pa - base;
            if (b == 0)
                return handle_bar0_legacy(off, run->mmio.is_write,
                                          run->mmio.data, run->mmio.len);
            /* MSI-X 表：影子表截留（读回与统计），只转发 Vector Control。
             * 物理表里 addr/data 是内核经 SET_IRQS 写的宿主 MSI 消息，
             * mask 位（Vector Control bit0）只能直写：VFIO 对 MSI-X 没有
             * ACTION_MASK/UNMASK ioctl（vfio_pci_intrs.c:854）。 */
            if (b == vf.msix_table_bar && vf.msix_cap &&
                off >= vf.msix_table_off &&
                off + run->mmio.len <= vf.msix_table_off + (uint64_t)vf.msix_nr * 16) {
                uint32_t pos = (uint32_t)(off - vf.msix_table_off);
                if (run->mmio.is_write) {
                    memcpy(vf.msix_shadow + pos, run->mmio.data, run->mmio.len);
                    /* Vector Control（条目内偏移 12，mask=bit0）必须落到物理
                     * 表：它是设备屏蔽/放行中断的开关，截留会导致 guest 的
                     * unmask（request_irq → pci_msix_unmask，msi.h:51）永远
                     * 不生效——这正是此前 guest I/O 挂死的根因之一。 */
                    uint32_t vec = pos / 16, fld = pos % 16;
                    if (fld >= 12 || fld + run->mmio.len > 12) {
                        uint32_t ctrl;
                        memcpy(&ctrl, vf.msix_shadow + vec * 16 + 12, 4);
                        struct vfio_region_info *ri =
                            vf.region[VFIO_PCI_BAR0_REGION_INDEX + b];
                        if (ri && pwrite(vf.device, &ctrl, 4,
                                         ri->offset + vf.msix_table_off +
                                         vec * 16 + 12) == 4)
                            vf.pt_ctrl_fwd++;
                    }
                    if (vf.msix_routing_done && fld < 12)
                        msix_update_routes();   /* addr/data 变了，同步 KVM 路由 */
                } else {
                    memcpy(run->mmio.data, vf.msix_shadow + pos, run->mmio.len);
                }
                return 0;
            }
            struct vfio_region_info *ri = vf.region[VFIO_PCI_BAR0_REGION_INDEX + b];
            ssize_t rc = run->mmio.is_write
                ? pwrite(vf.device, run->mmio.data, run->mmio.len,
                         ri->offset + off)
                : pread(vf.device, run->mmio.data, run->mmio.len,
                        ri->offset + off);
            (void)rc;
            return 0;
        }
    }
    return -1;
}

/* ---------- main ---------- */

static void usage(void)
{
    fprintf(stderr,
        "用法: minivmm -k <bzImage> -i <initramfs> [-m MB] [-c cmdline] [-d disk.img] [-e]\n"
        "  -m   guest 内存, 默认 256MB\n"
        "  -d   virtio-blk 后端文件, 默认 blk.img (不存在则新建 64MB)\n"
        "  -e   项目2 M3: 用 KVM_IOEVENTFD + KVM_IRQFD 把 virtio 数据面挪出\n"
        "       KVM_EXIT_MMIO（默认关，用 KVM_IRQ_LINE，便于前后对比 exit 计数）\n"
        "  -p   项目3: VFIO 直通 4b:00.0（设备须已绑 vfio-pci）\n"
        "       环境变量 MINIVMM_MSIX_SLOW=1：跳过 KVM_IRQFD/PI 链路，用户态线程\n"
        "       计数设备 MSI-X 并经 KVM_IRQ_LINE 注入（诊断用，见 corrections.md E）\n"
        "  -c   内核命令行, 默认 \"console=ttyS0 earlyprintk=serial rdinit=/init\"\n"
        "       VMM 会在其后自动追加两条 virtio_mmio.device=（console@0xd0000000:5、\n"
        "       blk@0xd0000200:6），因为这两个设备是固定模拟的\n"
        "       cmdline 里带 autotest 时，guest 起来后会打印 MINIVMM_READY 并\n"
        "       reboot，VMM 随即退出并报告启动耗时（项目 4 的测量入口）\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *kernel = NULL, *initrd = NULL;
    const char *cmdline = "console=ttyS0 earlyprintk=serial rdinit=/init";
    const char *blk_path = "blk.img";
    unsigned long mem_mb = 256;
    int opt;

    t_start = now_sec();

    while ((opt = getopt(argc, argv, "k:i:m:c:d:beph")) != -1) {
        switch (opt) {
        case 'k': kernel = optarg; break;
        case 'i': initrd = optarg; break;
        case 'm': mem_mb = strtoul(optarg, NULL, 10); break;
        case 'c': cmdline = optarg; break;
        case 'd': blk_path = optarg; break;
        case 'b': blob_mode = 1; break;
        case 'e': use_eventfd = 1; break;
        case 'p': passthrough_mode = 1; break;
        default: usage();
        }
    }
    if (!blob_mode && (!kernel || !initrd))
        usage();

    mem_size = mem_mb << 20;
    if (mem_size < 64 << 20)
        DIE("内存至少 64MB");

    msix_slowpath = getenv("MINIVMM_MSIX_SLOW") != NULL;

    guest_mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (guest_mem == MAP_FAILED)
        DIE_ERRNO("mmap guest RAM");

    vm_setup();
    struct boot_image img = { 0 };
    if (blob_mode) {
        img.kernel_gpa = load_selftest_blob();
        build_gdt();
    } else {
        /* 追加 virtio-mmio 设备发现参数（vm_cmdline_set() @
         * drivers/virtio/virtio_mmio.c:718 解析），GPA/IRQ 与上面常量一致 */
        static char full_cmdline[CMDLINE_MAX];
        snprintf(full_cmdline, sizeof(full_cmdline),
                 "%s virtio_mmio.device=0x%llx@0x%llx:%u "
                 "virtio_mmio.device=0x%llx@0x%llx:%u",
                 cmdline,
                 (unsigned long long)VIRTIO_REGION_SIZE,
                 (unsigned long long)VIRTIO_CONSOLE_BASE,
                 VIRTIO_CONSOLE_GSI,
                 (unsigned long long)VIRTIO_REGION_SIZE,
                 (unsigned long long)VIRTIO_BLK_BASE,
                 VIRTIO_BLK_GSI);
        blk_open(blk_path);
        if (use_eventfd)
            virtio_eventfds_setup();
        img = load_bzimage(kernel, initrd, full_cmdline);
        if (passthrough_mode) {
            build_mptable();
            vfio_setup();
        }
    }
    vcpu_setup(&img);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_stats);

    termios_raw();
    pthread_t tid;
    if (pthread_create(&tid, NULL, input_thread, NULL) != 0)
        DIE_ERRNO("pthread_create");
    if (use_eventfd) {
        pthread_t wtid;
        if (pthread_create(&wtid, NULL, virtio_worker, NULL) != 0)
            DIE_ERRNO("pthread_create virtio_worker");
    }

    printf("[vmm] 进入 KVM_RUN 循环 +%.3f ms (Ctrl-C 退出, SIGUSR1 打印 exit 统计)\n",
           (now_sec() - t_start) * 1e3);
    run_loop();
    dump_exit_stats();
    running = 0;
    termios_restore();
    fprintf(stderr, "[vmm] 总运行时长 %.3f ms\n", (now_sec() - t_start) * 1e3);
    printf("\n[vmm] 退出\n");
    return 0;
}
