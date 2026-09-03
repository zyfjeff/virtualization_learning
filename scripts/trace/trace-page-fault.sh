#!/bin/bash
#
# trace-page-fault.sh - 跟踪 KVM EPT 缺页处理
#
# 用法: sudo ./trace-page-fault.sh [选项]
#   -p PID     跟踪指定的 QEMU 进程 PID
#   -d SECS    跟踪持续时间（默认 10 秒）
#   -l LEVEL   过滤页表级别 (1=4K, 2=2M, 3=1G)
#   -a         显示全部详细信息
#   -s         只显示摘要
#   -h         显示帮助
#

set -euo pipefail

TRACEFS=""
PID=""
DURATION=10
LEVEL=""
ALL_DETAILS=false
SUMMARY_ONLY=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

usage() {
    echo "用法: sudo $0 [选项]"
    echo ""
    echo "跟踪 KVM EPT 缺页处理过程"
    echo ""
    echo "选项:"
    echo "  -p PID     跟踪指定的 QEMU 进程 PID"
    echo "  -d SECS    跟踪持续时间（默认 10 秒）"
    echo "  -l LEVEL   过滤页表级别 (1=4K, 2=2M, 3=1G)"
    echo "  -a         显示全部详细信息"
    echo "  -s         只显示摘要"
    echo "  -h         显示帮助"
    echo ""
    echo "示例:"
    echo "  sudo $0 -p 12345 -d 5"
    echo "  sudo $0 -p 12345 -l 2    # 只跟踪 2MB 大页"
    echo "  sudo $0 -p 12345 -s"
    exit 0
}

cleanup() {
    if [ -n "$TRACEFS" ] && [ -d "$TRACEFS" ]; then
        echo 0 > "$TRACEFS/tracing_on" 2>/dev/null || true
        echo > "$TRACEFS/set_event" 2>/dev/null || true
        echo nop > "$TRACEFS/current_tracer" 2>/dev/null || true
        echo > "$TRACEFS/set_ftrace_filter" 2>/dev/null || true
        # ★ 本次写的两处过滤也要收掉，否则下一次观测会带着上一次的过滤条件，
        #   得到"一个事件都没有"的假结论。set_event_pid 用 `: >` 就够：
        #   ftrace_event_set_pid_open()（kernel/trace/trace_events.c:2432）在 :2442-2444
        #   对带 O_TRUNC 的写打开调 ftrace_clear_event_pids()，而 event_pid_write()
        #   开头 :2167-2168 是 `if (!cnt) return 0;`，纯截断不会再写进任何东西。
        echo 0 > "$TRACEFS/events/kvmmmu/kvm_mmu_set_spte/filter" 2>/dev/null || true
        : > "$TRACEFS/set_event_pid" 2>/dev/null || true
    fi
}

trap cleanup EXIT

# 解析参数
while getopts "p:d:l:ash" opt; do
    case $opt in
        p) PID="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        l) LEVEL="$OPTARG" ;;
        a) ALL_DETAILS=true ;;
        s) SUMMARY_ONLY=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}错误: 需要 root 权限${NC}"
    exit 1
fi

# 查找 tracefs
for tfs in /sys/kernel/tracing /sys/kernel/debug/tracing; do
    if [ -d "$tfs/events/kvm" ]; then
        TRACEFS="$tfs"
        break
    fi
done

if [ -z "$TRACEFS" ]; then
    echo -e "${RED}错误: 找不到 tracefs${NC}"
    exit 1
fi

if [ -z "$PID" ]; then
    PID=$(pgrep -f "qemu-system" | head -1 || echo "")
    if [ -n "$PID" ]; then
        echo -e "${GREEN}自动检测到 QEMU PID: $PID${NC}"
    fi
fi

echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}  KVM EPT 缺页跟踪${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "跟踪参数:"
echo "  PID:      ${PID:-所有 KVM}"
echo "  持续时间: ${DURATION} 秒"
echo "  页级别:   ${LEVEL:-全部}"
echo ""

# 清空之前的跟踪
echo > "$TRACEFS/trace"
echo 0 > "$TRACEFS/tracing_on"

# 设置缺页相关事件
# ★ 用 `>` 打开 set_event 带 O_TRUNC，内核会先 ftrace_clear_events() 把宿主上**全部**已启用
#   事件清掉（kernel/trace/trace_events.c:2411 → :2422-2423，函数定义 :883）。tracefs 是
#   全局状态，别把别人挂的探针顺手清了：清场显式写 `: >`，之后一律 `>>` 追加。
#   规则唯一来源：../../phase9-performance/measurement.md §5 第 3 条。
: > "$TRACEFS/set_event"
echo kvm:kvm_page_fault >> "$TRACEFS/set_event"
# 页级别要靠 kvmmmu:kvm_mmu_set_spte —— 它**存在**，只是不在 `kvm` 这个 system 下：
#   TRACE_SYSTEM kvmmmu   arch/x86/kvm/mmu/mmutrace.h:9
#   kvm_mmu_set_spte      mmutrace.h:334-335，TP_printk "gfn %llx spte %llx (…) level %d at %llx"（:360）
#   触发点                arch/x86/kvm/mmu/tdp_mmu.c:1059（TDP MMU）、arch/x86/kvm/mmu/mmu.c:2949（shadow）
#   kvm_mmu_paging_element mmutrace.h:89-90
# ★ 旧版这里写"kvm:kvm_mmu_paging_element 和 kvm:kvm_mmu_set_spte 在 6.12 中不存在"，
#   是只在 events/kvm/ 下找过；宿主 events/kvmmmu/ 下实测两个都在。
echo kvmmmu:kvm_mmu_set_spte >> "$TRACEFS/set_event"

# 如果指定了页级别过滤
if [ -n "$LEVEL" ]; then
    # ★ 级别过滤只能挂在 kvmmmu:kvm_mmu_set_spte 上 —— 它有 `field:u8 level`
    #   （arch/x86/kvm/mmu/mmutrace.h:343，宿主 events/kvmmmu/kvm_mmu_set_spte/format 实测）。
    #   kvm:kvm_page_fault **没有** level 字段（arch/x86/kvm/trace.h:406-411），在它上面过滤不了。
    #   级别取值按 enum pg_level（arch/x86/include/asm/pgtable_types.h:548-556）。
    PF_FILTER="$TRACEFS/events/kvmmmu/kvm_mmu_set_spte/filter"
    if echo "level == $LEVEL" > "$PF_FILTER" 2>/dev/null; then
        case $LEVEL in
            1) LV_NAME="4KB (PG_LEVEL_4K)" ;;
            2) LV_NAME="2MB (PG_LEVEL_2M)" ;;
            3) LV_NAME="1GB (PG_LEVEL_1G)" ;;
            4) LV_NAME="512GB (PG_LEVEL_512G)" ;;
            5) LV_NAME="256TB (PG_LEVEL_256T)" ;;
            *) LV_NAME="level=$LEVEL" ;;
        esac
        echo "  过滤: $LV_NAME（已写 $PF_FILTER）"
    else
        echo -e "${YELLOW}  警告: 级别过滤没写进去（$PF_FILTER），本次统计包含全部级别${NC}"
    fi
fi

# 设置 PID 过滤
if [ -n "$PID" ]; then
    # ★ 同 set_event：带 O_TRUNC 的写打开会先 ftrace_clear_event_pids() 清掉**全部**已有
    #   PID（kernel/trace/trace_events.c:2442-2444）。清场显式写，追加用 `>>`。
    : > "$TRACEFS/set_event_pid" 2>/dev/null || true
    echo "$PID" >> "$TRACEFS/set_event_pid" 2>/dev/null || true
fi

# 如果需要详细函数跟踪
if [ "$ALL_DETAILS" = true ]; then
    echo ""
    echo "启用函数级跟踪..."
    echo function > "$TRACEFS/current_tracer"
    # ★ set_ftrace_filter 同理，只是机制在另一处：`>` 是"把 filter **换成**只有这些名字"
    #   （ftrace_regex_open() 的 O_TRUNC 分支从**空** hash 起步，kernel/trace/ftrace.c:4536、
    #   :4579-4581，收尾 :6438/:6478-6479）。清场显式写，追加用 `>>`。
    : > "$TRACEFS/set_ftrace_filter"
    # ★ 写进去之前先在 available_filter_functions 里核对：static inline 的函数会被内联掉、
    #   没有符号。旧版直接 echo __tdp_mmu_set_spte_atomic（arch/x86/kvm/mmu/tdp_mmu.c:533，
    #   static inline __must_check），宿主 available_filter_functions 里实测**零命中**。
    for fn in kvm_handle_page_fault kvm_tdp_page_fault kvm_tdp_mmu_map make_spte; do
        if grep -q "^${fn}\b" "$TRACEFS/available_filter_functions" 2>/dev/null; then
            echo "$fn" >> "$TRACEFS/set_ftrace_filter"
        else
            echo -e "${YELLOW}  跳过 $fn: 不在 available_filter_functions 里（可能被内联）${NC}"
        fi
    done
    # 注: kvm_mmu_get_page 是 kvmmmu 下的 **tracepoint**（arch/x86/kvm/mmu/mmutrace.h:158-159），
    #     不是可过滤的函数 —— 旧版写"在 6.12 中不存在"把两码事混了。
fi

# 开始跟踪
echo "开始跟踪..."
echo 1 > "$TRACEFS/tracing_on"
sleep "$DURATION"
echo 0 > "$TRACEFS/tracing_on"
echo "跟踪完成。"
echo ""

# 分析结果
TRACE_DATA=$(cat "$TRACEFS/trace")

# ★ 统计辅助：本脚本是 set -euo pipefail，grep 无匹配时退出码 1 会经 pipefail 传染给整条
#   管线 → 命令替换失败 → set -e 直接终止脚本。"一次都没抓到"是正常结果，不是错误。
#   另外别写 `|| echo 0`：grep -c 计数为 0 时**照样打印 0**、只是退出码 1，再 echo 一个 0
#   会捕到两行 "0\n0"，把后面的算术比较打成 "integer expression expected"。
count_re() {   # $1 = 扩展正则；统计 TRACE_DATA 里的匹配行数
    local n
    n=$(printf '%s\n' "$TRACE_DATA" | grep -Ec "$1" || true)
    printf '%s' "${n:-0}"
}

# ★ 缺页类型只能从 error_code 的位解出来。kvm_page_fault 的 TP_printk 是
#   "vcpu %u rip 0x%lx address 0x%016llx error_code 0x%llx"（arch/x86/kvm/trace.h:420-422），
#   文本里**没有** write / read / exec 这些词 —— 旧版 grep 这三个词，计数恒为 0。
#
# ★ 这里的 error_code 是 KVM 自己的 PFERR 字（arch/x86/include/asm/kvm_host.h:261-273），
#   **不是** EPT 违规的原始 exit qualification。VMX 侧的换算在
#   arch/x86/kvm/vmx/common.h:14-25（__vmx_handle_ept_violation）：
#     qual bit0 EPT_VIOLATION_ACC_READ  (vmx.h:589) → PFERR_USER_MASK  bit2  ← 读走的是 bit2！
#     qual bit1 EPT_VIOLATION_ACC_WRITE (vmx.h:590) → PFERR_WRITE_MASK bit1
#     qual bit2 EPT_VIOLATION_ACC_INSTR (vmx.h:591) → PFERR_FETCH_MASK bit4
#     EPT_VIOLATION_RWX_MASK                        → PFERR_PRESENT_MASK bit0
#   所以 VMX 上 bit0 的意思是"EPT 表项带了 RWX 权限位"，不是"页已存在"。
#
# ★★ 位计数**不能当分区用**，只能各算各的。Intel VMX 规范 Table 28-7 的 NOTES 1 写明：
#   开了 EPT accessed/dirty flags 后，处理器访问 guest 页表项按写处理，此时 exit
#   qualification 的 **bit0 与 bit1 会同时置位** → 换算成 PFERR 就是 bit2 与 bit1 同现。
#   写成 `if 写 / elif 取指 / else 读` 会把这类事件静默归掉一类；而"else"还会把
#   present-only、RSVD(bit3)、PK(bit5)、SGX(bit15)、guest-RMP(bit31) 全算成读。
#
# ★ 跨厂商警告：SVM 的 NPF 把 exit_info_1 **原样**当 error_code 传下去
#   （arch/x86/kvm/svm/svm.c:2125、trace 点 :2139），那一套位沿用 #PF 错误码语义，
#   bit2 是 U/S（用户态访问）而**不是**"读"。在 AMD 机器上跑本脚本，PF_USER 这一列
#   不能按"读缺页"解读。
pf_type_counts() {
    PF_PRESENT=0; PF_WRITE=0; PF_USER=0; PF_RSVD=0; PF_FETCH=0; PF_OTHER=0
    local ec
    while read -r ec; do
        [ -n "$ec" ] || continue
        (( ec & 0x1  )) && PF_PRESENT=$((PF_PRESENT + 1))
        (( ec & 0x2  )) && PF_WRITE=$((PF_WRITE + 1))
        (( ec & 0x4  )) && PF_USER=$((PF_USER + 1))
        (( ec & 0x8  )) && PF_RSVD=$((PF_RSVD + 1))
        (( ec & 0x10 )) && PF_FETCH=$((PF_FETCH + 1))
        # 低 5 位全空 = 既非读写取指、也非 present/RSVD，多半是 PK/SGX/RMP 等高位事件
        (( ec & 0x1f )) || PF_OTHER=$((PF_OTHER + 1))
    done < <(printf '%s\n' "$TRACE_DATA" | grep "kvm_page_fault" |
             sed -n 's/.*error_code \(0x[0-9a-fA-F]*\).*/\1/p')
}

if [ "$SUMMARY_ONLY" = true ]; then
    # 摘要统计
    echo -e "${BLUE}=== EPT 缺页统计 ===${NC}"
    echo ""

    TOTAL_FAULTS=$(count_re "kvm_page_fault")
    echo "总缺页次数: $TOTAL_FAULTS"
    echo ""

    if [ "$TOTAL_FAULTS" -gt 0 ]; then
        RATE=$(echo "scale=1; $TOTAL_FAULTS / $DURATION" | bc)
        echo "平均缺页率: ${RATE}/秒"
        echo ""

        # 分析缺页类型（按 error_code 位独立计数，理由与换算链路见 pf_type_counts 上方注释）
        pf_type_counts
        echo "缺页 error_code 位计数（各列**独立**，同一条事件可同时计入多列，不是分区）:"
        echo "  bit0 PFERR_PRESENT_MASK: $PF_PRESENT   ← VMX 上是\"EPT 表项带 RWX 权限\"，不是\"页已存在\""
        echo "  bit1 PFERR_WRITE_MASK:   $PF_WRITE    写访问"
        echo "  bit2 PFERR_USER_MASK:    $PF_USER     VMX EPT 违规=读访问；AMD NPF=用户态访问"
        echo "  bit3 PFERR_RSVD_MASK:    $PF_RSVD     保留位违规"
        echo "  bit4 PFERR_FETCH_MASK:   $PF_FETCH    取指"
        echo "  低 5 位全空:             $PF_OTHER    多半是 PK(bit5)/SGX(bit15)/RMP(bit31) 等"
        echo "  （VMX 上想按读/写/取指分类，直接读 bit2/bit1/bit4 三列；三者之和可以大于总缺页数，"
        echo "    因为 Table 28-7 NOTES 1 明确 bit0 与 bit1 会同时置位）"
        echo ""

        # 分析 SPTE 操作 (通过 function trace 捕获 make_spte 调用)
        SPTE_OPS=$(count_re "make_spte")
        echo "SPTE 构造次数 (make_spte): $SPTE_OPS"
        if [ "$SPTE_OPS" -eq 0 ] && [ "$ALL_DETAILS" != true ]; then
            echo "  （没加 -a：function tracer 未启用，trace 里根本不会有 make_spte 行，"
            echo "    这一项必然是 0，不代表没有构造 SPTE）"
        fi
    fi

else
    # 详细分析
    echo -e "${BLUE}=== EPT 缺页详细分析 ===${NC}"
    echo ""

    TOTAL_FAULTS=$(count_re "kvm_page_fault")
    echo "总缺页次数: $TOTAL_FAULTS"

    if [ "$TOTAL_FAULTS" -gt 0 ]; then
        RATE=$(echo "scale=1; $TOTAL_FAULTS / $DURATION" | bc)
        echo "平均缺页率: ${RATE}/秒"
        echo ""

        # 分析缺页的 GPA 分布
        echo -e "${BLUE}--- GPA 地址分布 (前 20 个热门区域) ---${NC}"
        # ★ 文本形式是 "address 0x%016llx"（arch/x86/kvm/trace.h:420-422），**不是** "address=0x…"。
        #   旧版 grep -oP 'address=0x[0-9a-f]+' 永远匹配不到；而且在 set -euo pipefail 下这条
        #   空结果让整条管线退出码非 0 → 脚本每次都死在这一行，详细分支从来没跑完过。
        printf '%s\n' "$TRACE_DATA" | grep "kvm_page_fault" |
            sed -n 's/.*address \(0x[0-9a-fA-F]*\).*/\1/p' |
            sort | uniq -c | sort -rn | head -20 || true
        echo ""

        # 分析页级别
        echo -e "${BLUE}--- 页级别分布 (kvmmmu:kvm_mmu_set_spte) ---${NC}"
        # ★ 级别信息只在 kvmmmu:kvm_mmu_set_spte 里（本脚本上面已启用），文本是 "… level %d at 0x…"
        #   （arch/x86/kvm/mmu/mmutrace.h:360）。旧版 grep "level=1" 两处都错：这个事件旧版
        #   根本没启用，而且形式是 "level 1" 不是 "level=1"。
        #   kvm:kvm_page_fault 没有 level 字段（arch/x86/kvm/trace.h:406-411），别指望从它统计。
        echo "  SPTE 写入事件总数: $(count_re "kvm_mmu_set_spte")"
        for lv in 1 2 3 4 5; do
            n=$(printf '%s\n' "$TRACE_DATA" | grep "kvm_mmu_set_spte" |
                grep -Ec "level ${lv} " || true)
            case $lv in
                1) lbl="4KB   (PG_LEVEL_4K)" ;;
                2) lbl="2MB   (PG_LEVEL_2M)" ;;
                3) lbl="1GB   (PG_LEVEL_1G)" ;;
                4) lbl="512GB (PG_LEVEL_512G)" ;;
                5) lbl="256TB (PG_LEVEL_256T)" ;;
            esac
            echo "  level $lv  $lbl: ${n:-0}"
        done
        echo "  （级别取值按 enum pg_level，arch/x86/include/asm/pgtable_types.h:548-556）"
        echo ""

        # SPTE 分析 (通过 function trace)
        echo -e "${BLUE}--- SPTE 构造 (make_spte 函数调用) ---${NC}"
        SPTE_OPS=$(count_re "make_spte")
        echo "  make_spte 调用次数: $SPTE_OPS"
        TDP_MAP=$(count_re "kvm_tdp_mmu_map")
        echo "  kvm_tdp_mmu_map 调用次数: $TDP_MAP"
        if [ "$SPTE_OPS" -eq 0 ] && [ "$ALL_DETAILS" != true ]; then
            echo "  （这两项要 -a 启用 function tracer 才有；没加 -a 时必然是 0）"
        fi
        echo ""

        # 显示前 30 个缺页事件
        echo -e "${BLUE}--- 缺页事件示例 (前 30 条) ---${NC}"
        printf '%s\n' "$TRACE_DATA" | grep "kvm_page_fault" | head -30 || true
        echo ""

        # 如果启用了函数跟踪，显示函数调用
        if [ "$ALL_DETAILS" = true ]; then
            echo -e "${BLUE}--- 函数调用跟踪 (前 50 条) ---${NC}"
            printf '%s\n' "$TRACE_DATA" |
                grep -E "kvm_tdp_mmu_map|make_spte|tdp_mmu_set" | head -50 || true
        fi
    else
        echo -e "${YELLOW}未观察到缺页事件。${NC}"
        echo "可能原因:"
        echo "  - 虚拟机未运行"
        echo "  - 内存已全部映射"
        echo "  - 跟踪时间太短"
    fi
fi

echo ""
echo -e "${GREEN}完成！${NC}"
echo ""
echo "提示:"
echo "  - 使用 trace-cmd 可以保存完整数据（要页级别就得带上 kvmmmu:kvm_mmu_set_spte）:"
echo "    trace-cmd record -e kvm:kvm_page_fault -e kvmmmu:kvm_mmu_set_spte ${PID:+-p $PID} -- sleep $DURATION"
echo "  - 分析大页效果: 在 kvm_mmu_set_spte 的行里对比 \"level 1\"(4KB) 与 \"level 2\"(2MB) 的条数"
echo "  - 跟踪脏页: 使用 -a 选项启用函数跟踪，观察 make_spte 的调用模式"
