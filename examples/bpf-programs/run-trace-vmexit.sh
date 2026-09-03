#!/bin/bash
#
# run-trace-vmexit.sh - BCC Python 用户态包装器 + 多工具等效命令
# KVM 深度学习项目
#
# ======================== 功能说明 ========================
#   本脚本包含:
#     1. BCC Python 用户态程序 (内嵌): 加载 BPF C 代码并格式化输出
#     2. bpftrace 等效一行命令
#     3. ftrace 等效命令
#
# ======================== 用法 ========================
#   sudo bash run-trace-vmexit.sh              # 追踪所有 VM
#   sudo bash run-trace-vmexit.sh --pid 12345  # 仅追踪指定 PID 的 VM
#   sudo bash run-trace-vmexit.sh --bpftrace   # 使用 bpftrace 替代 BCC
#   sudo bash run-trace-vmexit.sh --ftrace     # 使用 ftrace (无聚合)
#
# ======================== 内核源码映射 ========================
#   BPF C 内核部分: 见同目录 trace-vmexit.c
#   追踪点: kvm:kvm_exit (arch/x86/kvm/trace.h:336)
#   触发链 (6.12.93): vcpu_enter_guest()
#     -> kvm_x86_call(vcpu_run)(vcpu, run_flags)  arch/x86/kvm/x86.c:11079
#     -> vmx_vcpu_run()                          arch/x86/kvm/vmx/vmx.c:7344
#        同一函数体内 trace_kvm_entry() 在 :7372、VM-entry 返回后 trace_kvm_exit() 在 :7489
#     ★ 两个事件都打在 irqoff 段内；vmx_handle_exit_irqoff() (vmx.c:7032) 是另一个函数,
#       它不打印 kvm_exit。
#
# ======================== bpftrace 等效命令 ========================
#   bpftrace -e 'tracepoint:kvm:kvm_exit { @exits[args->exit_reason] = count(); }
#   interval:s:5 { print(@exits, 10); clear(@exits); }'
#
# ======================== ftrace 等效命令 ========================
#   # set_event 上加事件用 >>: 带 O_TRUNC 的写会先清掉全部已启用事件
#   # (kernel/trace/trace_events.c:2411 -> :2422-2423 调 ftrace_clear_events()),
#   # 详见 ../../phase9-performance/measurement.md §5 第 3 条
#   echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event
#   cat /sys/kernel/debug/tracing/trace_pipe

set -euo pipefail

# ======================== 颜色定义 ========================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# ======================== 参数解析 ========================
MODE="bcc"
TARGET_PID="0"

while [[ $# -gt 0 ]]; do
    case $1 in
        --pid)      TARGET_PID="$2"; shift 2 ;;
        --bpftrace) MODE="bpftrace"; shift ;;
        --ftrace)   MODE="ftrace"; shift ;;
        --help|-h)
            echo "用法: sudo $0 [--pid PID] [--bpftrace] [--ftrace]"
            echo "  --pid PID      仅追踪指定 QEMU 进程"
            echo "  --bpftrace     使用 bpftrace 替代 BCC"
            echo "  --ftrace       使用 ftrace 原始事件流"
            exit 0
            ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

# ======================== 权限检查 ========================
if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    echo "请使用: sudo $0 $*"
    exit 1
fi

# ======================== 模式: ftrace ========================
run_ftrace() {
    echo -e "${CYAN}=== ftrace 模式: 追踪 kvm_exit 事件 ===${NC}"
    echo -e "${YELLOW}等效 bpftrace: bpftrace -e 'tracepoint:kvm:kvm_exit { @exits[args->exit_reason] = count(); }'${NC}"
    echo ""

    # 确保 debugfs 已挂载
    mount -t debugfs none /sys/kernel/debug/tracing 2>/dev/null || true

    # 清除之前的追踪配置
    # ★ `: > set_event` 是**显式**清空全部事件（带 O_TRUNC 的写会先走
    #   ftrace_clear_events()，kernel/trace/trace_events.c:2411 → :2422-2423）。
    #   下面挂事件一律用 `>>`，否则又会把别人的探针顺手关掉。
    : > /sys/kernel/debug/tracing/set_event
    echo > /sys/kernel/debug/tracing/trace

    # 设置 PID 过滤 (ftrace 使用 pid 过滤器)
    if [[ "$TARGET_PID" != "0" ]]; then
        echo "$TARGET_PID" > /sys/kernel/debug/tracing/set_event_pid
        echo -e "${GREEN}已设置 PID 过滤: $TARGET_PID${NC}"
    fi

    # 启用 kvm_exit 事件
    echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event

    echo -e "${GREEN}开始追踪... 按 Ctrl+C 停止${NC}"
    echo -e "${YELLOW}--- 事件流 (按 exit_reason 统计请 Ctrl+C 后查看下方汇总) ---${NC}"
    echo ""

    # 捕获 Ctrl+C 以显示汇总
    trap cleanup_ftrace EXIT

    cat /sys/kernel/debug/tracing/trace_pipe
}

cleanup_ftrace() {
    echo ""
    echo -e "${CYAN}=== 汇总统计 ===${NC}"

    # 从 trace 文件提取统计
    # ★ trace 行里**没有数字** reason：TRACE_EVENT_KVM_EXIT 的 TP_printk 打的是
    #   `reason %s`，字符串由 kvm_print_exit_reason() 用 __print_symbolic() 译出
    #   （arch/x86/kvm/trace.h:289-295、:325-328）。所以只能按符号名聚合，
    #   `grep -oP 'reason=\K[0-9]+'` 永远抓不到东西（本仓 corrections 有登记）。
    if [[ -f /sys/kernel/debug/tracing/trace ]]; then
        echo "各 exit_reason 出现次数 (Top 10):"
        grep kvm_exit /sys/kernel/debug/tracing/trace 2>/dev/null | \
            sed -n 's/.* reason \([^ ]*\).*/\1/p' | \
            sort | uniq -c | sort -rn | head -10 || true
    fi

    # 清理
    # ★ 这一句清的是**全宿主**的 set_event，不只是本脚本挂的那一个；
    #   同机别人（或你上一轮）挂的探针会一起停，需要各自重新 `>>` 挂回。
    : > /sys/kernel/debug/tracing/set_event
    echo > /sys/kernel/debug/tracing/set_event_pid
    echo -e "${GREEN}追踪已停止${NC}"
}

# ======================== 模式: bpftrace ========================
run_bpftrace() {
    echo -e "${CYAN}=== bpftrace 模式: 追踪 KVM VM-Exit ===${NC}"
    echo -e "${YELLOW}等效 ftrace: echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event${NC}"
    echo ""

    if ! command -v bpftrace &>/dev/null; then
        echo -e "${RED}错误: bpftrace 未安装${NC}"
        echo "安装: apt install bpftrace"
        exit 1
    fi

    # bpftrace 脚本: 按退出原因统计, 每 5 秒刷新
    # 内核追踪点: kvm:kvm_exit (arch/x86/kvm/trace.h)
    # exit_reason 字段对应 VMCS Exit Reason (Intel SDM Vol.3C)
    local BPFTRACE_SCRIPT='
/*
 * bpftrace 脚本: KVM VM-Exit 追踪
 *
 * KVM 内核路径 (6.12.93):
 *   vcpu_enter_guest() -> kvm_x86_call(vcpu_run) (arch/x86/kvm/x86.c:11079)
 *   -> vmx_vcpu_run() (arch/x86/kvm/vmx/vmx.c:7344)，trace_kvm_exit() 在 vmx.c:7489
 *
 * exit_reason 含义 (arch/x86/include/uapi/asm/vmx.h:32-95):
 *   0=EXCEPTION_NMI  1=EXTERNAL_INTERRUPT  10=CPUID  12=HLT  18=VMCALL
 *   30=IO_INSTRUCTION  31=MSR_READ  32=MSR_WRITE  40=PAUSE_INSTRUCTION
 *   48=EPT_VIOLATION  49=EPT_MISCONFIG  52=PREEMPTION_TIMER  62=PML_FULL
 *   ★ args->exit_reason 是**数字**；trace 文本里打的是符号名（trace.h:289）
 *
 * ftrace 等效: echo kvm:kvm_exit >> set_event && cat trace_pipe
 */

tracepoint:kvm:kvm_exit
{
    @exits[args->exit_reason] = count();
    @total = count();
}

interval:s:5
{
    printf("\n===== VM-Exit 统计 (最近 5 秒) =====\n");
    printf("总退出次数: %lld\n\n", @total);
    printf("%-8s %-20s %s\n", "原因码", "名称", "次数");
    printf("%-8s %-20s %s\n", "------", "----", "----");
    print(@exits, 10);
    clear(@exits);
    clear(@total);
}
'

    # PID 过滤
    local FILTER=""
    if [[ "$TARGET_PID" != "0" ]]; then
        FILTER="-p $TARGET_PID"
        echo -e "${GREEN}PID 过滤: $TARGET_PID${NC}"
    fi

    echo -e "${GREEN}开始追踪... 按 Ctrl+C 停止${NC}"
    echo ""
    echo "$BPFTRACE_SCRIPT" | bpftrace $FILTER -
}

# ======================== 模式: BCC (Python) ========================
run_bcc() {
    echo -e "${CYAN}=== BCC 模式: 追踪 KVM VM-Exit ===${NC}"
    echo -e "${YELLOW}等效 bpftrace: bpftrace -e 'tracepoint:kvm:kvm_exit { @exits[args->exit_reason] = count(); }'${NC}"
    echo -e "${YELLOW}等效 ftrace:   echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event${NC}"
    echo ""

    if ! python3 -c "import bcc" 2>/dev/null; then
        echo -e "${RED}错误: BCC Python 模块未安装${NC}"
        echo "安装: apt install python3-bpfcc"
        exit 1
    fi

    # ===== 内嵌 BCC Python 用户态程序 =====
    # 此 Python 脚本加载 BPF C 内核代码 (trace-vmexit.c) 并格式化输出
    #
    # 架构说明:
    #   用户态 (Python)          内核态 (BPF C)
    #   BPF.load_text()    ->    TRACEPOINT_PROBE(kvm, kvm_exit)
    #   exit_counts.items() <-   BPF_HASH(exit_counts, u32, u64)
    #   每秒轮询读取             原子计数器递增

    python3 - "$TARGET_PID" <<'PYTHON_EOF'
#!/usr/bin/env python3
"""
BCC Python 用户态: KVM VM-Exit 追踪
====================================

内核源码映射:
  追踪点: kvm:kvm_exit (arch/x86/kvm/trace.h:336)
  触发 (6.12.93): vcpu_enter_guest() -> kvm_x86_call(vcpu_run) (arch/x86/kvm/x86.c:11079)
                  -> vmx_vcpu_run() (arch/x86/kvm/vmx/vmx.c:7344)，trace_kvm_exit() 在 :7489

bpftrace 等效:
  bpftrace -e 'tracepoint:kvm:kvm_exit { @exits[args->exit_reason] = count(); }'

ftrace 等效:
  echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event
  cat /sys/kernel/debug/tracing/trace_pipe
"""

import sys
import signal
import time
from bcc import BPF

# ======================== BPF C 内核代码 ========================
# 此代码运行在内核态, 挂载到 kvm:kvm_exit 追踪点
# 对应内核函数: vcpu_enter_guest() -> vmx_vcpu_run() (arch/x86/kvm/vmx/vmx.c:7344)
BPF_PROGRAM = r"""
#include <uapi/linux/ptrace.h>
#include <linux/sched.h>

/* exit_reason -> 计数 (对应 VMCS Exit Reason, Intel SDM Vol.3C) */
BPF_HASH(exit_counts, u32, u64, 128);

/* 总退出次数 */
BPF_ARRAY(total_exits, u64, 1);

/* PID 过滤器 */
BPF_ARRAY(target_pid, u32, 1);

TRACEPOINT_PROBE(kvm, kvm_exit) {
    u64 one = 1;
    u32 reason = (u32)args->exit_reason;
    u64 *cnt;
    u32 key = 0;

    /* PID 过滤: tgid = 用户态 PID */
    u32 *tpid = target_pid.lookup(&key);
    if (tpid && *tpid != 0) {
        u32 pid = bpf_get_current_pid_tgid() >> 32;
        if (pid != *tpid)
            return 0;
    }

    cnt = exit_counts.lookup_or_init(&reason, &one);
    if (cnt)
        lock_xadd(cnt, 1);

    cnt = total_exits.lookup(&key);
    if (cnt)
        lock_xadd(cnt, 1);

    return 0;
}
"""

# ======================== Exit Reason 名称映射 ========================
# 全表抄自 arch/x86/include/uapi/asm/vmx.h:32-95（与内核 VMX_EXIT_REASONS 字符串表
# :96-158 同名），trace 文本里打的就是这些名字，两边对得上。
EXIT_REASON_NAMES = {
    0:  "EXCEPTION_NMI",
    1:  "EXTERNAL_INTERRUPT",
    2:  "TRIPLE_FAULT",
    3:  "INIT_SIGNAL",
    4:  "SIPI_SIGNAL",
    7:  "INTERRUPT_WINDOW",
    8:  "NMI_WINDOW",
    9:  "TASK_SWITCH",
    10: "CPUID",
    12: "HLT",
    13: "INVD",
    14: "INVLPG",
    15: "RDPMC",
    16: "RDTSC",
    18: "VMCALL",
    19: "VMCLEAR",
    20: "VMLAUNCH",
    21: "VMPTRLD",
    22: "VMPTRST",
    23: "VMREAD",
    24: "VMRESUME",
    25: "VMWRITE",
    26: "VMOFF",
    27: "VMON",
    28: "CR_ACCESS",
    29: "DR_ACCESS",
    30: "IO_INSTRUCTION",
    31: "MSR_READ",
    32: "MSR_WRITE",
    33: "INVALID_STATE",
    34: "MSR_LOAD_FAIL",
    36: "MWAIT_INSTRUCTION",
    37: "MONITOR_TRAP_FLAG",
    39: "MONITOR_INSTRUCTION",
    40: "PAUSE_INSTRUCTION",
    41: "MCE_DURING_VMENTRY",
    43: "TPR_BELOW_THRESHOLD",
    44: "APIC_ACCESS",
    45: "EOI_INDUCED",
    46: "GDTR_IDTR",
    47: "LDTR_TR",
    48: "EPT_VIOLATION",
    49: "EPT_MISCONFIG",
    50: "INVEPT",
    51: "RDTSCP",
    52: "PREEMPTION_TIMER",
    53: "INVVPID",
    54: "WBINVD",
    55: "XSETBV",
    56: "APIC_WRITE",
    57: "RDRAND",
    58: "INVPCID",
    59: "VMFUNC",
    60: "ENCLS",
    61: "RDSEED",
    62: "PML_FULL",
    63: "XSAVES",
    64: "XRSTORS",
    67: "UMWAIT",
    68: "TPAUSE",
    74: "BUS_LOCK",
    75: "NOTIFY",
}

# tracepoint 的 exit_reason 是 VMCS 原始值，高位可能带标志
# （arch/x86/include/uapi/asm/vmx.h:29-30）。内核译名时先 `& 0xffff`，再用
# __print_flags() 把高位按 VMX_EXIT_REASON_FLAGS 附加（arch/x86/kvm/trace.h:289-295）。
# ★ VMX_EXIT_REASON_FLAGS 里**只有** FAILED_VMENTRY（vmx.h:160-161）；
#   SGX_ENCLAVE_MODE 那个位虽在 vmx.h:30 定义，但没进这张表，__print_flags()
#   对不认识的位直接打十六进制 —— 这里照内核的行为做，不自己发明名字。
EXIT_REASON_FLAGS = {
    0x80000000: "FAILED_VMENTRY",
}

def get_exit_name(reason):
    """将 exit_reason 数值映射为可读名称（与 trace 文本同格式）"""
    name = EXIT_REASON_NAMES.get(reason & 0xffff, f"UNKNOWN({reason & 0xffff})")
    high = reason & ~0xffff
    if not high:
        return name
    # trace_print_flags_seq()（kernel/trace/trace_output.c:65，由 __print_flags 宏
    # include/trace/stages/stage3_trace_output.h:67-72 调用）：命中的位打名字并清掉
    # （:74-87），剩下的位打十六进制（:89-94）
    parts = []
    for bit, flag_name in EXIT_REASON_FLAGS.items():
        if high & bit == bit:
            parts.append(flag_name)
            high &= ~bit
    if high:
        parts.append(hex(high))
    return " ".join([name] + parts)

# ======================== 主程序 ========================
running = True

def signal_handler(sig, frame):
    global running
    running = False
    print("\n\n正在停止追踪...")

signal.signal(signal.SIGINT, signal_handler)

def main():
    target_pid = int(sys.argv[1]) if len(sys.argv) > 1 else 0

    print("=" * 60)
    print("  KVM VM-Exit 追踪器 (BCC BPF)")
    print("=" * 60)
    print()

    # 加载 BPF 程序到内核
    # BCC 会:
    #   1. 编译 C 代码为 BPF 字节码 (clang)
    #   2. 加载到内核 (bpf() 系统调用)
    #   3. 挂载到 kvm:kvm_exit 追踪点
    print("[*] 加载 BPF 程序...")
    b = BPF(text=BPF_PROGRAM)
    print("[+] BPF 程序加载成功")

    # 设置 PID 过滤
    if target_pid > 0:
        pid_map = b.get_table("target_pid")
        pid_map[0] = target_pid
        print(f"[+] PID 过滤: {target_pid}")
    else:
        print("[*] 追踪所有 KVM VM (未设置 PID 过滤)")

    print(f"\n[*] 开始追踪 kvm:kvm_exit... 按 Ctrl+C 停止")
    print(f"{'='*60}")
    print(f"  等效 bpftrace: bpftrace -e 'tracepoint:kvm:kvm_exit {{ @exits[args->exit_reason] = count(); }}'")
    print(f"  等效 ftrace:   echo kvm:kvm_exit >> /sys/kernel/debug/tracing/set_event")
    print(f"{'='*60}\n")

    # 每秒轮询 BPF map 并输出统计
    # 用户态通过 perf_event_open / bpf() 系统调用读取 BPF map
    prev_counts = {}

    while running:
        try:
            time.sleep(1)
        except KeyboardInterrupt:
            break

        # 读取 BPF Hash Map: exit_reason -> count
        exit_counts = b.get_table("exit_counts")
        total_table = b.get_table("total_exits")
        total = total_table[0].value if total_table else 0

        # 计算速率 (与上次快照的差值)
        current_counts = {}
        for k, v in exit_counts.items():
            reason = k.value
            count = v.value
            current_counts[reason] = count

        # 按计数排序
        sorted_reasons = sorted(current_counts.items(), key=lambda x: x[1], reverse=True)

        # 输出表头
        print(f"\n--- VM-Exit 统计 (总计: {total:,}) ---")
        print(f"{'退出原因':<8} {'名称':<24} {'累计次数':>12} {'说明'}")
        print("-" * 70)

        for reason, count in sorted_reasons[:15]:  # Top 15
            name = get_exit_name(reason)
            rate = count - prev_counts.get(reason, 0)
            rate_str = f"{rate:,}/s" if rate > 0 else ""

            # 为常见退出原因添加说明（原因码见 arch/x86/include/uapi/asm/vmx.h:32-95；
            # 高位标志先掩掉，只比 basic exit reason）
            desc = ""
            basic = reason & 0xffff
            if basic == 48:
                desc = "EPT_VIOLATION → 补二级映射（TDP MMU）"
            elif basic == 1:
                desc = "EXTERNAL_INTERRUPT → 宿主处理 IRQ 后重新 VM-Entry"
            elif basic == 12:
                desc = "HLT → guest 空闲；轮询档位见 phase9-performance/parameters.md §1"
            elif basic == 10:
                desc = "CPUID → KVM 模拟 CPU 特性"
            elif basic == 18:
                desc = "VMCALL → guest 主动 hypercall"
            elif basic == 30:
                desc = "IO_INSTRUCTION → 端口 I/O 被拦截"
            elif basic == 31:
                desc = "MSR_READ → RDMSR 被拦截"
            elif basic == 32:
                desc = "MSR_WRITE → WRMSR 被拦截"
            elif basic == 40:
                desc = "PAUSE_INSTRUCTION → PLE 触发，handle_pause (vmx.c:5911)"
            elif basic == 52:
                desc = "PREEMPTION_TIMER → VMX preemption timer 到期"

            print(f"  {reason:<6} {name:<24} {count:>12,}  {rate_str:>10}  {desc}")

        prev_counts = current_counts.copy()

    # 最终汇总
    print(f"\n{'='*60}")
    print("  最终汇总")
    print(f"{'='*60}")

    exit_counts = b.get_table("exit_counts")
    total_table = b.get_table("total_exits")
    total = total_table[0].value if total_table else 0

    print(f"\n总 VM-Exit 次数: {total:,}")
    print(f"\n各退出原因最终统计:")
    print(f"{'退出原因':<8} {'名称':<24} {'次数':>12} {'占比'}")
    print("-" * 55)

    final_counts = [(k.value, v.value) for k, v in exit_counts.items()]
    final_counts.sort(key=lambda x: x[1], reverse=True)

    for reason, count in final_counts:
        name = get_exit_name(reason)
        pct = (count / total * 100) if total > 0 else 0
        print(f"  {reason:<6} {name:<24} {count:>12,}  {pct:>5.1f}%")

    print(f"\n{'='*60}")
    print("追踪结束")

if __name__ == "__main__":
    main()
PYTHON_EOF
}

# ======================== 主入口 ========================
echo -e "${CYAN}"
echo "╔══════════════════════════════════════════════════════════╗"
echo "║        KVM VM-Exit 追踪器 - 多工具统一入口             ║"
echo "║        KVM 深度学习项目 - BPF 追踪系列                 ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo -e "${NC}"

case "$MODE" in
    bcc)       run_bcc ;;
    bpftrace)  run_bpftrace ;;
    ftrace)    run_ftrace ;;
esac
