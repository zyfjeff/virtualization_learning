/*
 * trace-vmexit.c - BCC-style BPF 程序: 追踪 KVM VM-Exit
 * KVM 深度学习项目 - 系统篇
 *
 * ======================== 功能说明 ========================
 *   1. 挂载到 kvm:kvm_exit 追踪点 (tracepoint)
 *   2. 按退出原因 (exit_reason) 分组统计 VM-Exit 次数
 *   3. 计算每秒退出速率
 *   4. 显示 Top 10 最频繁的退出原因
 *
 * ======================== 内核源码映射 ========================
 *   追踪点定义: arch/x86/kvm/trace.h
 *       TRACE_EVENT(kvm_exit,
 *           TP_PROTO(unsigned int exit_reason, unsigned long guest_rip),
 *           ...
 *       )
 *
 *   触发位置:   arch/x86/kvm/x86.c
 *       vcpu_enter_guest()
 *       -> kvm_x86_ops.handle_exit()  [vmx_handle_exit() in vmx/vmx.c]
 *       -> trace_kvm_exit(exit_reason, guest_rip)
 *
 *   exit_reason 含义 (Intel SDM Vol.3C Appendix C):
 *     0  = EXCEPTION_NMI       异常或 NMI
 *     1  = EXTERNAL_INTERRUPT  外部中断 (最常见原因之一)
 *     7  = INTERRUPT_WINDOW    等待中断窗口打开
 *     10 = CPUID               guest 执行 CPUID
 *     12 = HLT                 guest 执行 HLT (进入空闲)
 *     18 = VMCALL              guest 发起 hypercall
 *     24 = EPT_VIOLATION       EPT 页表违规 (缺页异常, 最高频)
 *     25 = EPT_MISCONFIG       EPT 配置错误
 *     48 = MSR_WRITE           guest 执行 WRMSR
 *
 * ======================== bpftrace 等效命令 ========================
 *   bpftrace -e 'tracepoint:kvm:kvm_exit {
 *       @exits[args->exit_reason] = count();
 *   }'
 *
 *   # 带名称的增强版:
 *   bpftrace -e '
 *   tracepoint:kvm:kvm_exit {
 *       @exits[str(args->exit_reason)] = count();
 *   }
 *   interval:s:1 {
 *       print(@exits, 10);
 *       clear(@exits);
 *   }'
 *
 * ======================== ftrace 等效命令 ========================
 *   # 方法1: 直接查看原始事件流
 *   echo kvm:kvm_exit > /sys/kernel/debug/tracing/set_event
 *   cat /sys/kernel/debug/tracing/trace_pipe
 *
 *   # 方法2: 统计各退出原因次数
 *   cat /sys/kernel/debug/tracing/trace | grep kvm_exit | \
 *       grep -oP 'reason=\K[0-9]+' | sort | uniq -c | sort -rn | head
 *
 *   # 方法3: 使用 trace-cmd 工具
 *   trace-cmd record -e kvm:kvm_exit sleep 10
 *   trace-cmd report | grep kvm_exit | \
 *       awk '{print $NF}' | sort | uniq -c | sort -rn
 *
 * ======================== 用法 ========================
 *   sudo python3 run-trace-vmexit.sh        # 使用 BCC Python 包装器
 *   sudo ./trace-vmexit [vm_pid]            # 编译后运行
 *   编译: gcc -I/usr/include/bcc trace-vmexit.c -lbcc -o trace-vmexit
 */

#include <uapi/linux/ptrace.h>
#include <linux/sched.h>

/*
 * BPF Hash Map: exit_reason(u32) -> count(u64)
 * 每个 VM-Exit 原因对应一个计数器
 * 128 个桶足够覆盖所有已知的 exit reason
 *
 * 对应内核数据结构:
 *   arch/x86/kvm/x86.c 中 vcpu->arch.exit_reason (内核内部)
 *   include/uapi/linux/kvm.h 中 kvm_run->exit_reason (用户态 API)
 */
BPF_HASH(exit_counts, u32, u64, 128);

/*
 * BPF Hash Map: 每秒速率统计
 * key = exit_reason, value = 本秒内出现次数
 * 由用户态每秒读取后清零, 用于实时速率显示
 *
 * 对应: bpftrace 中 interval:s:1 { print(@rate); clear(@rate); }
 * 对应: ftrace 中需要外部脚本定时读取 trace 并计算差值
 */
BPF_HASH(exit_rate, u32, u64, 128);

/*
 * 全局原子计数器: 总 VM-Exit 次数
 * 用于快速查看整体退出频率
 *
 * 对应: bpftrace @total = count()
 * 对应: ftrace grep -c kvm_exit trace
 */
BPF_ARRAY(total_exits, u64, 1);

/*
 * PID 过滤器: target_pid[0] = 目标 VM 进程 PID
 * 设置为 0 表示追踪所有 KVM VM
 *
 * 对应: bpftrace -p <pid>
 * 对应: ftrace echo <pid> > set_event_pid
 */
BPF_ARRAY(target_pid, u32, 1);

/*
 * VM-Exit 追踪点处理函数
 *
 * 内核调用路径:
 *   arch/x86/kvm/x86.c: vcpu_enter_guest()
 *     -> vmx_vcpu_run()           [vmx/vmx.c]
 *       -> __vmx_handle_exit()    [vmx/vmx.c]
 *         -> trace_kvm_exit()     <-- 此处触发追踪点
 *
 * 追踪点参数 (struct trace_event_raw_kvm_exit):
 *   args->exit_reason  : VMCS Exit Reason 字段 (Basic VM-Exit Reason)
 *   args->guest_rip    : Guest 线性地址 (CS:RIP)
 *   args->info1        : Exit Information Field 1 (VMCS Exit Qualification)
 *   args->info2        : Exit Information Field 2
 *
 * VMCS (Virtual Machine Control Structure) 字段说明:
 *   Exit Reason    : VMCS offset 0x4402, 16-bit 基本退出原因
 *   Exit Qualification : VMCS offset 0x6400, 退出的详细信息
 *   Guest RIP      : VMCS offset 0x681C, guest 线性地址
 *
 * Intel SDM Vol.3C Appendix C 定义了所有退出原因的编码
 */
TRACEPOINT_PROBE(kvm, kvm_exit) {
    u64 one = 1;
    u32 reason = (u32)args->exit_reason;
    u64 *cnt;
    u32 key = 0;

    /* === PID 过滤 ===
     * 如果设置了目标 PID, 仅统计匹配的 QEMU/KVM 进程
     * bpf_get_current_pid_tgid() 返回 (tgid << 32 | pid)
     * tgid 即用户态可见的进程 ID (getpid() 返回值)
     * pid  即内核线程 ID (gettid() 返回值)
     *
     * QEMU 中每个 vCPU 是一个线程, 它们共享同一个 tgid
     * 所以按 tgid 过滤可以追踪一个 QEMU 进程的所有 vCPU
     */
    u32 *tpid = target_pid.lookup(&key);
    if (tpid && *tpid != 0) {
        u32 pid = bpf_get_current_pid_tgid() >> 32;
        if (pid != *tpid)
            return 0;
    }

    /* === 按退出原因分组计数 ===
     * lookup_or_init: 如果 key 不存在则初始化为 &one 指向的值
     * 否则返回已有值的指针
     *
     * 注意: BPF_HASH 的 value 存储在内核 BPF map 中
     * 多个 CPU 可能并发更新, 所以需要用原子操作
     *
     * 对应: bpftrace @exits[args->exit_reason] = count()
     * 对应: ftrace grep -oP 'reason=\K\d+' | sort | uniq -c
     */
    cnt = exit_counts.lookup_or_init(&reason, &one);
    if (cnt) {
        lock_xadd(cnt, 1);
    }

    /* === 总计数递增 ===
     * 用 BPF_ARRAY 存储, 因为只有一个全局计数器
     * BPF_ARRAY 比 BPF_HASH 更高效 (直接数组索引 vs 哈希查找)
     *
     * 对应: bpftrace @total = count()
     * 对应: ftrace grep -c kvm_exit /sys/kernel/debug/tracing/trace
     */
    cnt = total_exits.lookup(&key);
    if (cnt) {
        lock_xadd(cnt, 1);
    }

    return 0;
}

/*
 * ======================== 用户态程序说明 ========================
 *
 * 本文件是 BPF C 内核代码, 需要用户态程序加载和驱动
 *
 * BCC 模式 (推荐):
 *   使用 run-trace-vmexit.sh 中的内嵌 Python 程序:
 *     1. 加载本 C 代码 (BCC 自动编译为 BPF 字节码)
 *     2. 挂载到 kvm:kvm_exit 追踪点
 *     3. 每秒轮询 exit_counts BPF map
 *     4. 格式化输出 + exit_reason 名称映射
 *     5. 计算速率 (与上次快照的差值)
 *
 * bpftrace 等效 (一行命令, 无需本文件):
 *   bpftrace -e 'tracepoint:kvm:kvm_exit { @exits[args->exit_reason] = count(); }'
 *
 * ftrace 等效 (无需 BPF):
 *   echo kvm:kvm_exit > /sys/kernel/debug/tracing/set_event
 *   cat /sys/kernel/debug/tracing/trace_pipe
 *
 * ======================== 内核数据结构 ========================
 *
 * 本程序涉及的内核数据结构:
 *
 * 1. struct kvm_vcpu (arch/x86/include/asm/kvm_host.h)
 *    - arch.exit_reason : 退出原因 (内核内部使用)
 *    - run              : 指向 kvm_run (用户态共享)
 *    - vcpu_id          : vCPU 编号
 *
 * 2. struct kvm_run (include/uapi/linux/kvm.h)
 *    - exit_reason      : 退出原因 (用户态 API)
 *    - 通过 mmap 共享给 QEMU
 *
 * 3. VMCS (Intel SDM Vol.3C)
 *    - Exit Reason (0x4402)    : 硬件退出原因编码
 *    - Exit Qualification (0x6400) : 退出详细信息
 *    - Guest Physical Address  : EPT 缺页时的 GPA
 */
