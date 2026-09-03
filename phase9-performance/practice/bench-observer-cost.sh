#!/usr/bin/env bash
# E5 · 观测开销自校准驱动器 —— 设计文档见 bench-observer-cost.md，表在 ../measurement.md §4(b)
#
# 一条臂 = 一档观测强度下的一个独立采样窗，顺序固定、不可乱序（§3.2）：
#   O0 真零 → O1 挂着不记 → O2 kvm_entry → O3 kvm_exit → O4 kvm:* → O5 function
#   → O6 bpftrace → O7 O3+trace_pipe 读者 → O0e 末次基线
#
# ★ 基线是"没有任何 probe 挂在 tracepoint 上"，不是 `tracing_on=0`（§2.1，
#   upstream 在 kernel/trace/trace.c:1592-1599 自己写明 tracing_off 不关掉任何开销）。
# ★ 档位在窗外切换：写 set_ftrace_filter / current_tracer 会触发一批 text_poke，
#   记进 sum_exec_runtime 就污染这一臂（§2.3、§6.7）。
# ★ 每臂结束必须把 ftrace 恢复到"什么都没开"再进下一臂，且退出时整表回滚原值。
#
# 用法：
#   ./bench-observer-cost.sh --preflight
#   ./bench-observer-cost.sh --all --dry-run
#   sudo ./bench-observer-cost.sh --arms O0,O2,O3,O0e --sample-s 20
#   sudo ./bench-observer-cost.sh --all --repeat 3
#   sudo ./bench-observer-cost.sh --all --out /data/e5-run1     # 结果目录自己定
#   ./bench-observer-cost.sh --report-from /data/e5-run1        # 只重算汇总，不碰系统
set -u
ORIG_PWD=$PWD             # 调用者的 cwd；下面立刻 cd 进脚本目录（KERNEL/INITRD 是相对路径）
cd "$(dirname "$0")"

# ---------- 路径与常量 ----------
KERNEL=/root/code/linux-6.12.93/arch/x86_64/boot/bzImage   # 与 boot-vm.sh 同源
INITRD=../../scripts/images/initramfs.img
KO_DIR=ple-load
SHARE=../../scripts/shared
TR=/sys/kernel/tracing
DBK=/sys/kernel/debug/kvm

VCPU=4                  # guest vCPU 数
PRIV_KB=256             # ple_load workload=1 每线程私有缓冲区
SAMPLE_S=20             # 采样窗长（所有臂同一值，否则不可比）
WARM_S=3                # 档位生效后的稳定期，**不记进窗口**
TICK_S=2                # 窗内采样节拍（只用于核对单调性/掉核，不参与差值计算）
BUF_KB=8192             # 每 CPU ring buffer
WIN_MAX=2000000         # 存档快照上限（字节）；只影响落盘，不影响计数
PIPE_CAP_MB=256         # O7 读者落盘硬上限（ulimit -f），写满即被 SIGXFSZ 杀掉
REPEAT=1
ARMS_WANT="O0,O1,O2,O3,O4,O5,O6,O7,O0e"
FUNCS="vcpu_enter_guest vmx_vcpu_run vmx_handle_exit handle_ept_violation kvm_mmu_page_fault"
EVENT_ENTRY=kvm_entry
EVENT_EXIT=kvm_exit
KNOW_OTHER_VMS=0

TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/observer-cost-$TS
DRY=0; ONLY_PREFLIGHT=0; REPORT_FROM=""
declare -a SEQ=()

log()  { printf '%s\n' "$*"; }
warn() { printf '!! %s\n' "$*" >&2; }
die()  { warn "$*"; exit 1; }
usage() { awk 'NR>1 { if (!/^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 1; }

# ---------- 状态 ----------
VM_PID=""; VM_TIDS=""; VM_TAG=vm
CAT_PID=""; BPF_PID=""; BPF_OUT=""; RD_PID=""; CONS_PID=""
STAT_NAME=""; STAT_KIND=""
PASS=""; FAIL=""; VOID=""; DRIFT=""; NOBASE=0
USERHZ=$(getconf CLK_TCK 2>/dev/null || echo 100)
declare -A F=()          # 原值快照

out_append() {           # $1=文件 $2=整行
    [ "$DRY" = 1 ] && return 0
    printf '%s\n' "$2" >> "$1"
}
verdict() {              # $1=臂 $2=0通过/非0不通过 $3=通过说明 $4=失败说明
    if [ "$2" = 0 ]; then PASS="$PASS $1"; log "  ✓ $1：$3"
    else FAIL="$FAIL ${1}(FAILED)"; warn "  ✗ $1：$4"; fi
}
# ★ 值没变就不写：写 current_tracer / set_ftrace_filter 即使内容相同也会走一遍
#   ftrace_run_update_code()（kernel/trace/ftrace.c:2954）→ 一批 text_poke（§2.3）
tf_write() {             # $1=路径 $2=期望值 → 0 现值已等于期望值
    local cur; cur=$(cat "$1" 2>/dev/null) || return 1
    [ "$cur" = "$2" ] && return 0
    echo "$2" > "$1" 2>/dev/null || return 1
    [ "$(cat "$1" 2>/dev/null)" = "$2" ]
}

# ---------- ftrace 快照与恢复 ----------
# set_event 的复读格式（6.12.93）：**只列已启用的事件**，一行一个 `system:name`，
# 没有 [+] 前缀也没有 # 注释头 —— `t_show()` kernel/trace/trace_events.c:1445-1453
# 打 "%s:%s\n"，而 `s_next()` `:1413-1423` 只在 `:1421` 往下走带 EVENT_FILE_FL_ENABLED 的
# file；ops 注册在 `:2251-2256`（show_set_event_seq_ops）。
set_event_list() { cat $TR/set_event 2>/dev/null | grep -v '^$'; }
# filter 的复读里模块函数印成 `名字 [模块]`：print_rec() kernel/trace/ftrace.c:4303-4320
# 的 `if (modname) seq_printf(m, " [%s]", modname)`。回写时必须剥掉后缀，否则
# `[kvm]` 会被当成第二个名字并匹配失败（bench-migrate.md §4.2.1(a)）。
filter_list() {
    cat $TR/set_ftrace_filter 2>/dev/null | grep -v '^#' | grep -v '^$' | \
        grep -v 'all functions enabled' | sed 's/ \[.*\]$//' | paste -sd ' '
}
snap_ftrace() {
    [ -e $TR/current_tracer ] || return 0
    F[tracer]=$(cat $TR/current_tracer 2>/dev/null)
    F[on]=$(cat $TR/tracing_on 2>/dev/null)
    F[buf]=$(cat $TR/buffer_size_kb 2>/dev/null)
    F[clock]=$(cat $TR/trace_clock 2>/dev/null)
    F[prof]=$(cat $TR/function_profile_enabled 2>/dev/null)
    F[setevent]=$(set_event_list | tr '\n' ' ')
    F[filter]=$(filter_list)
    [ "$DRY" = 1 ] && return 0
    { for k in tracer on buf clock prof setevent filter; do printf '%s=%s\n' "$k" "${F[$k]:-}"; done; } \
        > "$OUT/orig-ftrace.txt" 2>/dev/null || true
}
clear_ftrace() {
    [ -e $TR/current_tracer ] || return 0
    if [ "$DRY" = 1 ]; then log "  [dry] 清 ftrace：tracer=nop、set_event 空、filter 空、tracing_on=0"; return 0; fi
    tf_write $TR/function_profile_enabled 0 || true
    tf_write $TR/current_tracer nop || true
    [ -n "$(set_event_list)" ] && : > $TR/set_event 2>/dev/null
    [ -n "$(filter_list)" ] && : > $TR/set_ftrace_filter 2>/dev/null
    tf_write $TR/tracing_on 0 || true
    : > $TR/trace 2>/dev/null || true
}
restore_ftrace() {
    [ "$DRY" = 1 ] && return 0
    [ -e $TR/current_tracer ] || return 0
    tf_write $TR/function_profile_enabled "${F[prof]:-0}" || true
    tf_write $TR/current_tracer nop || true
    : > $TR/set_ftrace_filter 2>/dev/null || true
    : > $TR/set_event 2>/dev/null || true
    local e
    for e in ${F[setevent]:-}; do
        echo "$e" >> $TR/set_event 2>/dev/null || warn "  原事件 $e 挂不回去"
    done
    for e in ${F[filter]:-}; do
        echo "$e" >> $TR/set_ftrace_filter 2>/dev/null || warn "  原函数 $e 挂不回 filter"
    done
    [ -n "${F[tracer]:-}" ] && [ "${F[tracer]}" != nop ] && \
        { echo "${F[tracer]}" > $TR/current_tracer 2>/dev/null || warn "  原 tracer ${F[tracer]} 回不去"; }
    [ -n "${F[buf]:-}" ] && echo "${F[buf]}" > $TR/buffer_size_kb 2>/dev/null
    [ -n "${F[clock]:-}" ] && echo "${F[clock]}" > $TR/trace_clock 2>/dev/null
    tf_write $TR/tracing_on "${F[on]:-0}" || true
}
kill_side() {                            # 读者 / bpftrace
    local p
    for p in "${RD_PID:-}" "${BPF_PID:-}" "${CAT_PID:-}"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null || true
    done
    RD_PID=""; BPF_PID=""; CAT_PID=""; CONS_PID=""
}
kill_vm() {
    [ -n "$VM_PID" ] || return 0
    kill "$VM_PID" 2>/dev/null || true
    wait "$VM_PID" 2>/dev/null || true
    VM_PID=""; exec 7>&- 2>/dev/null || true
}
cleanup() {
    # ★ --report-from 只是重算已有数据，退出时**绝不能**碰 tracefs
    [ -n "$REPORT_FROM" ] && return 0
    if [ "$DRY" != 1 ]; then
        stop_sampler; kill_side; kill_vm
        clear_ftrace; restore_ftrace
    fi
    return 0
}
trap cleanup EXIT INT TERM

# ---------- 系统指纹（证明零副作用） ----------
# ★ tracefs 里**没有 enabled_events 这个文件**（6.12.93 全树无此名，本机亦无）。
#   残留探针的正确看法：set_event 复读（只列已启用事件）+ enabled_functions + 进程表。
fingerprint() {                          # $1=输出文件
    {
        echo "tracer=$(cat $TR/current_tracer 2>/dev/null)"
        echo "tracing_on=$(cat $TR/tracing_on 2>/dev/null)"
        echo "buffer_size_kb=$(cat $TR/buffer_size_kb 2>/dev/null)"
        echo "set_event=$(set_event_list | tr '\n' ',')"
        echo "filter=$(filter_list)"
        echo "enabled_functions=$(cat $TR/enabled_functions 2>/dev/null | wc -l)"
        echo "profile=$(cat $TR/function_profile_enabled 2>/dev/null)"
        echo "kvmfds=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c '/dev/kvm')"
        echo "tracepiped=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c 'trace_pipe')"
        # ★ `|| true` 而不是 `|| echo 0`：pgrep -c / grep -c 计数为 0 时照样打印 0、
        #   只是退出码为 1，再 echo 一个 0 会让快照多出一行裸 `0`，破坏 key=value 格式
        echo "qemu=$(pgrep -c -f 'qemu-system' 2>/dev/null || true)"
        echo "bpftrace=$(pgrep -c -x bpftrace 2>/dev/null || true)"
        echo "perf=$(pgrep -c -x perf 2>/dev/null || true)"
        echo "kvmdirs=$(ls -d $DBK/[0-9]* 2>/dev/null | wc -l)"
        echo "tracelen=$(cat $TR/trace 2>/dev/null | wc -l)"
    } > "$1" 2>/dev/null || true
}

# ---------- debugfs 统计发现（§2.2：per-VM 优先） ----------
find_stat_path() {                       # $1=统计名 → 路径；不可用返回 1
    local name=$1 pervm
    pervm=$(ls -d "$DBK/${VM_PID}-"* "$DBK/${VM_PID}" 2>/dev/null | head -1)
    if [ -n "$pervm" ] && [ -r "$pervm/$name" ]; then STAT_NAME="$pervm/$name"; STAT_KIND=per-VM; return 0; fi
    if [ -r "$DBK/$name" ]; then STAT_NAME="$DBK/$name"; STAT_KIND=global; return 0; fi
    return 1
}
read_exits() {                           # → 数值或 NA
    local v=""
    [ -n "$STAT_NAME" ] || { printf 'NA'; return; }
    v=$(cat "$STAT_NAME" 2>/dev/null | tr -d '[:space:]')
    [ -n "$v" ] && printf '%s' "$v" || printf 'NA'
}

# ---------- tracepoint / filter 可用性 ----------
traceable() {                            # $1=函数名 → 0 可跟踪
    [ -r $TR/available_filter_functions ] || return 1
    grep -qE -- "^$1( |\[|$)" $TR/available_filter_functions
}
event_on() {                             # $1=subsys:event → 0 已在 set_event 复读里
    set_event_list | grep -qxF -- "$1"
}
enable_one() {                           # $1=subsys:event → 0 成功且已复读确认
    local e=$1
    [ -d "$TR/events/${e%:*}/${e#*:}" ] || return 1
    echo "$e" >> $TR/set_event 2>/dev/null || return 1
    event_on "$e"
}
# filter 必须一个名字一次 >>（坏名会吞掉它后面的全部名字，bench-migrate.md §4.2.1(b)）
filter_add() {                           # $1=空格分隔的名字 → 0 全部真装进去了
    local f miss="" got
    : > $TR/set_ftrace_filter 2>/dev/null || true
    for f in $1; do
        echo "$f" >> $TR/set_ftrace_filter 2>/dev/null || miss="$miss $f"
    done
    [ -n "$miss" ] && warn "  这些名字写入被拒：$miss"
    # ★ 判据是逐个名字复读，不是数个数：空 filter 在 hash_contains_ip() 里被当作
    #   恒匹配（ftrace.c:1513），"一个都没装成 + filter 空 = 全开"照样能骗过计数
    got=$(filter_list)
    for f in $1; do
        case " $got " in *" $f "*) ;; *) warn "  filter 复读里没有 $f（实际：${got:-空=全开}）"; return 1 ;; esac
    done
    return 0
}

# ---------- VM ----------
wait_marker() { for _ in $(seq 1 400); do grep -q '^-------------------' "$1" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
verify_kvm() {
    local n
    n=$(ls -l /proc/$1/fd 2>/dev/null | grep -c '/dev/kvm') || true
    [ "${n:-0}" -gt 0 ] || die "PID $1 未持有 /dev/kvm —— 走的是 TCG（AGENTS.md 陷阱 7）"
    log "  KVM 确认：$n 个 /dev/kvm fd"
}
find_vcpu_tids() {                       # $1=pid → 按 vcpu id 升序输出 tid
    local pid=$1 t n idx out="" count=0
    local -a slot=()
    for t in /proc/$pid/task/*; do
        [ -r "$t/comm" ] || continue
        n=$(cat "$t/comm")
        case $n in
            "CPU "*/KVM)
                idx=${n#CPU }; idx=${idx%/KVM}
                [[ "$idx" =~ ^[0-9]+$ ]] || continue
                [ "$idx" -lt "$VCPU" ] || continue
                [ -z "${slot[idx]:-}" ] || { warn "vcpu $idx 出现两个线程，按名字归属不可靠"; return 1; }
                slot[idx]=$(basename "$t"); count=$((count + 1)) ;;
        esac
    done
    [ "$count" = "$VCPU" ] || return 1
    for ((idx = 0; idx < VCPU; idx++)); do out="$out ${slot[idx]}"; done
    echo "${out# }"
}
run_guest() {
    local slot=$1; shift
    if [ "$DRY" = 1 ]; then log "  [dry] guest: $*"; return 0; fi
    printf '%s\n' "$*" >&7
}
boot_vm() {
    local serial
    if [ "$DRY" = 1 ]; then
        log "  [dry] qemu -enable-kvm -cpu host -m 2G -smp $VCPU + insmod ple_load workload=1 priv_kb=$PRIV_KB"
        VM_PID="<pid>"; VM_TIDS="<tid0> <tid1> <tid2> <tid3>"; return 0
    fi
    serial="$OUT/serial-$VM_TAG.log"
    mkfifo "$OUT/ser-$VM_TAG.in" "$OUT/ser-$VM_TAG.out"
    qemu-system-x86_64 -enable-kvm -cpu host -m 2G -smp "$VCPU" \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "console=ttyS0 earlyprintk=serial rdinit=/init" \
        -virtfs "local,path=$SHARE,mount_tag=hostshare,security_model=passthrough,id=hostshare" \
        -display none -monitor none -no-reboot \
        -serial pipe:"$OUT/ser-$VM_TAG" > "$OUT/qemu.err" 2>&1 &
    VM_PID=$!
    cat "$OUT/ser-$VM_TAG.out" > "$serial" &
    CAT_PID=$!
    # ★ 用 <>：只写打开 FIFO 会阻塞到读端出现，QEMU 没起来时这里永久挂住
    eval "exec 7<> \"$OUT/ser-$VM_TAG.in\""
    wait_marker "$serial" || die "guest 未就绪（见 $serial）"
    verify_kvm "$VM_PID"
    VM_TIDS=$(find_vcpu_tids "$VM_PID") || die "认不出 $VCPU 个 'CPU <n>/KVM' 线程（bench-migrate.md §2.2）"
    log "  vm: pid=$VM_PID vcpu tids=$VM_TIDS"
    run_guest 1 "insmod /mnt/shared/ple_load.ko workload=1 nr_threads=$VCPU priv_kb=$PRIV_KB"
    sleep 2
}

# ---------- 每档观测状态 ----------
# 返回 0 = 档位已生效；非 0 = 该臂作废（不是"代价为 0"，是"没有数据"）
describe_arm() {
    case $1 in
      O0|O0e) echo '真零基线：set_event 空 + tracer=nop + tracing_on=0' ;;
      O1)     echo "挂着不记：set_event 挂 kvm:$EVENT_EXIT + tracing_on=0" ;;
      O2)     echo "记录中：单事件 kvm:$EVENT_ENTRY（TP_fast_assign 只读 RIP）" ;;
      O3)     echo "记录中：单事件 kvm:$EVENT_EXIT（TP_fast_assign 读 VMCS）" ;;
      O4)     echo '记录中：echo 1 > events/kvm/enable（整组）' ;;
      O5)     echo "function tracer + filter=[$FUNCS]" ;;
      O6)     echo "bpftrace -e 't:kvm:$EVENT_EXIT { @c = count(); }'（set_event 保持空）" ;;
      O7)     echo "O3 + 常驻 cat trace_pipe > 文件（消费侧）" ;;
    esac
}
apply_arm() {                            # $1=臂
    local arm=$1 n dir on d
    [ "$DRY" = 1 ] && { log "  [dry] 设档 $arm：$(describe_arm "$arm")"; return 0; }
    clear_ftrace
    CONS_PID=""
    echo $BUF_KB > $TR/buffer_size_kb 2>/dev/null || warn "  buffer_size_kb 设不了"
    case $arm in
    O0|O0e) : > $TR/trace; tf_write $TR/tracing_on 0 || return 1
            [ -z "$(set_event_list)" ] || { warn "  基线要求 set_event 为空，实际：$(set_event_list | tr '\n' ' ')"; return 1; }
            [ "$(cat $TR/current_tracer)" = nop ] || { warn "  基线要求 tracer=nop，实际 $(cat $TR/current_tracer)"; return 1; } ;;
    O1)     enable_one "kvm:$EVENT_EXIT" || { warn "  事件 kvm:$EVENT_EXIT 挂不上"; return 1; }
            : > $TR/trace; tf_write $TR/tracing_on 0 || return 1     # ★ 故意不开 tracing_on
            event_on "kvm:$EVENT_EXIT" || { warn "  O1 要求事件挂着，复读里没有它"; return 1; } ;;
    O2)     enable_one "kvm:$EVENT_ENTRY" || { warn "  事件 kvm:$EVENT_ENTRY 挂不上"; return 1; }
            : > $TR/trace; tf_write $TR/tracing_on 1 || return 1 ;;
    O3)     enable_one "kvm:$EVENT_EXIT" || { warn "  事件 kvm:$EVENT_EXIT 挂不上"; return 1; }
            : > $TR/trace; tf_write $TR/tracing_on 1 || return 1 ;;
    O4)     n=0
            for dir in $TR/events/kvm/*/; do [ -d "$dir" ] && n=$((n + 1)); done
            echo 1 > $TR/events/kvm/enable 2>/dev/null || { warn "  写 events/kvm/enable 失败"; return 1; }
            on=0
            for d in $TR/events/kvm/*/; do [ "$(cat "$d/enable" 2>/dev/null)" = 1 ] && on=$((on + 1)); done
            log "  O4：$on/$n 个 kvm 事件 enable；set_event 复读里 kvm 行数 $(set_event_list | grep -c '^kvm:')"
            out_append "$OUT/arm-$arm.txt" "kvm_events_enabled=$on/$n"
            [ "$on" = "$n" ] || { warn "  只装上 $on/$n，O4 定义不成立"; return 1; }
            : > $TR/trace; tf_write $TR/tracing_on 1 || return 1 ;;
    O5)     filter_add "$FUNCS" || return 1
            echo function > $TR/current_tracer 2>/dev/null || { warn "  current_tracer=function 失败"; return 1; }
            [ "$(cat $TR/current_tracer)" = function ] || { warn "  tracer 实际是 $(cat $TR/current_tracer)"; return 1; }
            : > $TR/trace; tf_write $TR/tracing_on 1 || return 1 ;;
    O6)     command -v bpftrace >/dev/null || { warn "  没有 bpftrace"; return 1; }
            BPF_OUT="$OUT/bpftrace-$arm.txt"
            bpftrace -e "t:kvm:$EVENT_EXIT { @c = count(); }" > "$BPF_OUT" 2>&1 &
            BPF_PID=$!; CONS_PID=$BPF_PID
            echo "$BPF_PID" > "$OUT/bpftrace-$arm.pid" 2>/dev/null || true
            for _ in $(seq 1 50); do grep -q 'Attached 1 probe' "$BPF_OUT" 2>/dev/null && break; sleep 0.1; done
            grep -q 'Attached 1 probe' "$BPF_OUT" || { warn "  bpftrace 没挂上：$(head -3 "$BPF_OUT")"; kill "$BPF_PID" 2>/dev/null; BPF_PID=""; CONS_PID=""; return 1; }
            out_append "$OUT/arm-$arm.txt" "set_event_during_bpftrace=$(set_event_list | tr '\n' ',')" ;;
    O7)     enable_one "kvm:$EVENT_EXIT" || { warn "  事件 kvm:$EVENT_EXIT 挂不上"; return 1; }
            : > $TR/trace; tf_write $TR/tracing_on 1 || return 1
            # ★ 必须有界：trace_pipe 的产出 = 退出率 × 行长，20s 窗在 1e5 exits/s 下是几百 MB。
            #   ulimit -f 的单位是 512 字节块，写满时内核给进程发 SIGXFSZ 杀掉它。
            #   用 `exec cat` 是为了让 $! 就是 cat 自己（子 shell 被替换掉），
            #   否则 CONS_PID 量到的是一个几乎不耗 CPU 的外壳 → 消费侧成本全部漏计。
            #   代价写在 §6.13：cap 提前用完 = 消费没覆盖全窗，该臂不能与 O3 相减。
            ( ulimit -f "$((PIPE_CAP_MB * 2048))" 2>/dev/null || true
              exec cat $TR/trace_pipe > "$OUT/pipe-$arm.txt" ) &
            RD_PID=$!; CONS_PID=$RD_PID
            echo "$RD_PID" > "$OUT/reader-$arm.pid" 2>/dev/null || true ;;
    *)      warn "  未知臂 $arm"; return 1 ;;
    esac
    return 0
}
unapply_arm() {                          # $1=臂
    local p
    [ "$DRY" = 1 ] && return 0
    case $1 in
    O6)  p=$BPF_PID; [ -n "$p" ] && { kill -INT "$p" 2>/dev/null; wait "$p" 2>/dev/null || true; } ;;
    O7)  p=$RD_PID;  [ -n "$p" ] && { kill "$p" 2>/dev/null; wait "$p" 2>/dev/null || true; } ;;
    esac
    BPF_PID=""; RD_PID=""; CONS_PID=""
    clear_ftrace
}

# ---------- 读数 ----------
# /proc/stat 首行按 user nice system idle iowait irq softirq steal guest guest_nice
# 打印（fs/proc/stat.c:128-137），即 $2..$11。
# ★ guest 已经并进 user/nice：kernel/sched/cputime.c:157-158 先
#   task_group_account_field(p, CPUTIME_USER, cputime) 再 cpustat[CPUTIME_GUEST] += cputime。
#   所以 busy 里**不能再加** $10/$11，否则重复计数；而我们的 vCPU 时间正是记在 guest，
#   已经含在 busy 的 user 项里。idle = $5(idle) + $6(iowait)。
cpu_busy_raw() {                         # → "busy<TAB>idle"（jiffies）
    awk '/^cpu /{printf "%s\t%s\n", $2+$3+$4+$7+$8+$9, $5+$6}' /proc/stat 2>/dev/null \
        || printf 'NA\tNA\n'
}
# 四个 vCPU 线程之和：sum_exec_runtime / run_delay（ns）+ stime（ticks）
sched_all() {
    [ -n "$VM_TIDS" ] || { printf 'NA\tNA\tNA'; return; }
    local t a b line rest stv rt=0 rd=0 st=0 n=0 want=0
    for t in $VM_TIDS; do
        want=$((want + 1))
        [ -r "/proc/$VM_PID/task/$t/schedstat" ] || continue
        # fs/proc/base.c:511-522 proc_pid_schedstat(): "sum_exec_runtime run_delay pcount"
        read -r a b _ < "/proc/$VM_PID/task/$t/schedstat" 2>/dev/null || continue
        n=$((n + 1))
        rt=$(awk -v x="$rt" -v y="${a:-0}" 'BEGIN{printf "%.0f", x+y}')
        rd=$(awk -v x="$rd" -v y="${b:-0}" 'BEGIN{printf "%.0f", x+y}')
        # /proc/<tid>/stat 第 15 列 = stime（fs/proc/array.c:604-605，单位 clock ticks）。
        # ★ 第 2 列 comm 可以含空格（"CPU 0/KVM"）→ 不能按空白直接数下标，先剥掉第一个 ") " 之前
        #   的部分。★ 必须用**最短**匹配：贪婪的 ##*)  会切到行内最后一个 ") "，而第 28 列是
        #   可执行文件路径，路径里带 ") " 就会把 stime 静默截错（procps 也是按第一个 ")" 切的）
        line=$(cat "/proc/$VM_PID/task/$t/stat" 2>/dev/null) || continue
        rest=${line#*) }
        # shellcheck disable=SC2086
        set -- $rest
        stv=${13:-0}                 # rest 的 $1=state(第3列) → stime(第15列)=rest 的第 13 个
        st=$(awk -v x="$st" -v y="$stv" 'BEGIN{printf "%.0f", x+y}')
    done
    # ★ 少读到一个线程就必须整体报 NA：部分求和会系统性低估 Δ，看起来"这一档更便宜"，
    #   比 NA 危险得多 —— NA 会被原样打出来，低估不会
    [ "$n" = "$want" ] && [ "$want" -gt 0 ] || { printf 'NA\tNA\tNA'; return; }
    printf '%s\t%s\t%s' "$rt" "$rd" "$st"
}
# 消费侧（bpftrace / cat trace_pipe）自己那个进程的 sum_exec_runtime —— §4.1 最后一行
cons_rt() {
    local a
    [ -n "$CONS_PID" ] || { printf 'NA'; return; }
    read -r a _ < "/proc/$CONS_PID/schedstat" 2>/dev/null || { printf 'NA'; return; }
    printf '%s' "${a:-NA}"
}
snap_all() {                             # → 1rt 2rd 3stime 4busy 5idle 6exits 7cons
    local a b
    a=$(sched_all); b=$(cpu_busy_raw)
    printf '%s\t%s\t%s\t%s' "$a" "$b" "$(read_exits)" "$(cons_rt)"
}
SAMP_PID=""
tick_loop() {                            # $1=臂
    local i t arm=$1
    for ((i = 0; i * TICK_S < SAMPLE_S + 2; i++)); do
        t=$(date +%s.%N)
        printf '%s\t%s\n' "$t" "$(snap_all)" >> "$OUT/tick-$arm.tsv" 2>/dev/null || true
        sleep "$TICK_S"
    done
}
start_sampler() { [ "$DRY" = 1 ] && return 0; tick_loop "$1" & SAMP_PID=$!; }
stop_sampler()  { [ -n "$SAMP_PID" ] && { kill "$SAMP_PID" 2>/dev/null; wait "$SAMP_PID" 2>/dev/null || true; }; SAMP_PID=""; }

read_completed() {                       # → guest 侧 completed 计数（一次串口往返）
    local m="cmp-$RANDOM$RANDOM" line="" i
    [ "$DRY" = 1 ] && { printf '<c>'; return; }
    printf 'echo COMP %s $(cat /sys/module/ple_load/parameters/completed)\n' "$m" >&7
    for i in $(seq 1 100); do
        line=$(grep -m1 "^COMP $m " "$OUT/serial-$VM_TAG.log" 2>/dev/null) || true
        [ -n "$line" ] && break
        sleep 0.1
    done
    [ -n "$line" ] || { printf 'NA'; return; }
    printf '%s' "$line" | awk '{print $3}'
}

# 一次读 pass：数事件行 + 存前 WIN_MAX 字节快照 + 数 [LOST 行
# 打印 "cnt rec kept lost"：cnt=命中 pat 的行数；rec=非注释头行数（真记到多少条）
scan_trace() {                           # $1=源 $2=存档 $3=pattern
    awk -v w="$2" -v pat="$3" -v lim="$WIN_MAX" '
        /^#/ { next }
        { lines++
          if (kept < lim && w != "") { print > w; kept += length($0) + 1 }
          if (pat != "" && index($0, pat)) cnt++
          if (index($0, "[LOST")) lost++ }
        END { printf "%d %d %d %d\n", cnt+0, lines+0, kept+0, lost+0 }' "$1" 2>/dev/null
}

# ---------- 一臂 ----------
run_arm() {                              # $1=臂
    local arm=$1 c0 c1 s0 s1 pat="" f line
    f="$OUT/arm-$arm.txt"
    log "--- 臂 $arm（窗 ${SAMPLE_S}s，稳定期 ${WARM_S}s）：$(describe_arm "$arm") ---"
    if [ "$DRY" = 1 ]; then
        log "  [dry] apply_arm → warm ${WARM_S}s → t0 → 读 completed/计数 → 开计时窗 ${SAMPLE_S}s → 读计数 → t1 → 停记录并统计 → 撤档"
        return 0
    fi
    apply_arm "$arm" || { warn "  档位没能生效，$arm 作废"; VOID="$VOID $arm"; return 1; }
    sleep "$WARM_S"
    # ★ completed 要一次串口往返：t0 取在"发出读命令之前"、c1 的读命令取在 t1 之前发出，
    #   两端各自的往返延迟就落在 [t0,t1] 之外（§4.1）
    local t0 t1
    t0=$(date +%s.%N); c0=$(read_completed); s0=$(snap_all)
    start_sampler "$arm"
    sleep "$SAMPLE_S"
    stop_sampler
    s1=$(snap_all)
    t1=$(date +%s.%N); c1=$(read_completed)
    dur=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')

    # 收窗：先停记录，再统计（读 trace 是读端成本，不能算进窗口，§6.8）
    tf_write $TR/tracing_on 0 || true
    case $arm in
      O2)  pat="$EVENT_ENTRY:" ;;
      O3|O7) pat="$EVENT_EXIT:" ;;
      O4)  pat=" kvm_" ;;
      O5)  pat="" ;;   # ★ 不能按 "+0x" 数：TRACE_DEFAULT_FLAGS（kernel/trace/trace.c:479-486）
                       #   里没有 TRACE_ITER_SYM_OFFSET，函数行只印裸函数名（本机
                       #   options/sym-offset=0 实测）。本档 events 全关 + 窗首清过缓冲
                       #   → 函数记录是缓冲里唯一的写者，命中数直接取 rec_lines（见下）
    esac
    local scan n_ev=0 n_rec=0 n_kept=0 lost=0 ovr=0 src="$TR/trace"
    [ "$arm" = O7 ] && src="$OUT/pipe-$arm.txt"
    scan=$(scan_trace "$src" "$OUT/win-$arm.txt" "$pat")
    n_ev=$(printf '%s' "$scan" | awk '{print $1}'); n_rec=$(printf '%s' "$scan" | awk '{print $2}')
    [ "$arm" = O5 ] && n_ev="$n_rec"      # 见上面 pat="" 的注释：这一档命中数 = 全部记录行
    n_kept=$(printf '%s' "$scan" | awk '{print $3}'); lost=$(printf '%s' "$scan" | awk '{print $4}')
    cat $TR/per_cpu/cpu*/stats > "$OUT/bufstats-$arm.txt" 2>/dev/null || true
    ovr=$(grep -h 'overrun:' "$OUT/bufstats-$arm.txt" 2>/dev/null | awk '{s+=$NF} END{print s+0}')
    ovr=${ovr:-0}
    [ "$ovr" = 0 ] || warn "  $arm 有 $ovr 次 ring buffer overrun，事件计数偏低（../measurement.md §4(c)）"
    unapply_arm "$arm"

    local rt0 rd0 st0 busy0 idle0 e0 cons0 rt1 rd1 st1 busy1 idle1 e1 cons1
    rt0=$(printf '%s' "$s0" | cut -f1); rd0=$(printf '%s' "$s0" | cut -f2); st0=$(printf '%s' "$s0" | cut -f3)
    busy0=$(printf '%s' "$s0" | cut -f4); idle0=$(printf '%s' "$s0" | cut -f5)
    e0=$(printf '%s' "$s0" | cut -f6);   cons0=$(printf '%s' "$s0" | cut -f7)
    rt1=$(printf '%s' "$s1" | cut -f1); rd1=$(printf '%s' "$s1" | cut -f2); st1=$(printf '%s' "$s1" | cut -f3)
    busy1=$(printf '%s' "$s1" | cut -f4); idle1=$(printf '%s' "$s1" | cut -f5)
    e1=$(printf '%s' "$s1" | cut -f6);   cons1=$(printf '%s' "$s1" | cut -f7)
    line=$(awk -v a="$arm" -v dur="$dur" -v c0="$c0" -v c1="$c1" -v e0="$e0" -v e1="$e1" \
              -v rt0="$rt0" -v rt1="$rt1" -v rd0="$rd0" -v rd1="$rd1" -v st0="$st0" -v st1="$st1" \
              -v b0="$busy0" -v b1="$busy1" -v i0="$idle0" -v i1="$idle1" \
              -v cs0="$cons0" -v cs1="$cons1" \
              -v nev="$n_ev" -v nrec="$n_rec" -v kept="$n_kept" -v ovr="$ovr" -v lost="$lost" '
        function d(x, y,  r) { if (x == "NA" || y == "NA") return "NA"; r = y - x; return r < 0 ? "NA" : r }
        BEGIN {
            cd = d(c0, c1); ed = d(e0, e1)
            printf "arm=%s dur=%s completed_delta=%s completed_per_s=%s exits_delta=%s exits_per_s=%s", \
                   a, dur, cd, (cd=="NA"||dur+0<=0?"NA":sprintf("%.2f", cd/dur)), \
                   ed, (ed=="NA"||dur+0<=0?"NA":sprintf("%.2f", ed/dur))
            printf " rt_ns=%s run_delay_ns=%s stime_ticks=%s busy_jif=%s idle_jif=%s cons_rt_ns=%s", \
                   d(rt0,rt1), d(rd0,rd1), d(st0,st1), d(b0,b1), d(i0,i1), d(cs0,cs1)
            printf " ev_lines=%s rec_lines=%s win_kept=%s overrun=%s lost_lines=%s", nev, nrec, kept, ovr, lost
        }')
    log "  $line"
    out_append "$f" "$line"
    out_append "$f" "stat_path=$STAT_NAME kind=$STAT_KIND"
    if [ "$arm" = O6 ]; then
        local bc; bc=$(awk '/^@c:/{print $2}' "$BPF_OUT" 2>/dev/null | tail -1)
        out_append "$f" "bpf_aggregate=${bc:-NA}"
        # 生效自证（§4.2）：@c>0 且 set_event 仍为空 —— §2.5 的"看不见"必须现场成立
        if [ "$(awk -v v="${bc:-0}" 'BEGIN{print (v+0 > 0) ? 1 : 0}')" = 1 ]; then verdict O6 0 "bpftrace @c=$bc（此时 set_event 仍为空）"
        else verdict O6 1 "@c 读不到或为 0，见 $BPF_OUT"; fi
    fi
    # ★ O7 撞到捕获上限 = 读者后半窗不在场，"记录 vs 消费"的差值不可比（§6.13）
    local psz=0 capped=0
    if [ "$arm" = O7 ]; then
        psz=$(stat -c %s "$OUT/pipe-$arm.txt" 2>/dev/null || echo 0)
        [ "${psz:-0}" -ge $(( (PIPE_CAP_MB - 2) * 1024 * 1024 )) ] && capped=1
        out_append "$f" "pipe_bytes=$psz cap_mb=$PIPE_CAP_MB"
    fi
    case $arm in
    O0|O0e) verdict "$arm" "$( [ "$n_rec" = 0 ] && [ "$ovr" = 0 ] && echo 0 || echo 1)" \
                "基线窗内零记录（rec_lines=0，overrun=0）" \
                "基线窗里 rec_lines=$n_rec overrun=$ovr，基线不干净" ;;
    O1)   verdict O1 "$( [ "$n_rec" = 0 ] && [ "$ovr" = 0 ] && echo 0 || echo 1)" \
                "事件挂着但确实没记（set_event 有 kvm:$EVENT_EXIT，rec_lines=0）" \
                "tracing_on=0 却记到 $n_rec 行 / overrun=$ovr，档位定义没落地" ;;
    O2)   [ "${n_ev:-0}" -gt 0 ] && verdict O2 0 "记录 $n_ev 条 $EVENT_ENTRY" \
            || verdict O2 1 "零事件：要么走的是 TCG（陷阱 7），要么事件没生效" ;;
    O3)   [ "${n_ev:-0}" -gt 0 ] && verdict O3 0 "记录 $n_ev 条 $EVENT_EXIT" || verdict O3 1 "零事件" ;;
    O4)   [ "${n_ev:-0}" -gt 0 ] && verdict O4 0 "记录 $n_ev 条 kvm_*" || verdict O4 1 "零事件" ;;
    O5)   [ "${n_ev:-0}" -gt 0 ] && verdict O5 0 "命中 $n_ev 次函数条目" || verdict O5 1 "零命中（filter 没生效或函数没被调用）" ;;
    O7)   if [ "$capped" = 1 ]; then verdict O7 1 "捕获撞顶 ${PIPE_CAP_MB}MB（$psz 字节）→ 读者中途被杀，消费未覆盖全窗"
          elif [ "${n_ev:-0}" -gt 0 ]; then verdict O7 0 "读者收到 $n_ev 条 $EVENT_EXIT（$psz 字节，未撞顶）"
          else verdict O7 1 "读者零收到（见 $OUT/pipe-$arm.txt）"; fi ;;
    esac
    # 摘要行（列固定：1臂 2dur 3comp/s 4exits/s 5rt_ns 6busy 7rd_ns 8stime 9cons_rt 10ev 11ovr 12lost 13rec 14kind）
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$arm" "$dur" \
        "$(printf '%s' "$line" | sed -n 's/.*completed_per_s=\([^ ]*\).*/\1/p')" \
        "$(printf '%s' "$line" | sed -n 's/.*exits_per_s=\([^ ]*\).*/\1/p')" \
        "$(printf '%s' "$line" | sed -n 's/.* rt_ns=\([^ ]*\).*/\1/p')" \
        "$(printf '%s' "$line" | sed -n 's/.*busy_jif=\([^ ]*\).*/\1/p')" \
        "$(printf '%s' "$line" | sed -n 's/.*run_delay_ns=\([^ ]*\).*/\1/p')" \
        "$(printf '%s' "$line" | sed -n 's/.*stime_ticks=\([^ ]*\).*/\1/p')" \
        "$(printf '%s' "$line" | sed -n 's/.*cons_rt_ns=\([^ ]*\).*/\1/p')" \
        "$n_ev" "$ovr" "$lost" "$n_rec" "${STAT_KIND:-none}" >> "$OUT/summary.tsv"
    return 0
}

# ---------- 汇总 ----------
# 每臂先对 repeat 取**中位数**（§3.2），再以 O0 为基线算四个派生量（§4.1），
# 最后按 §4.3 条件 2 判漂移：|O0e − O0|（completed/s 绝对量）< 1/3 × min(相邻臂间差的绝对量)。
report() {
    [ "$DRY" = 1 ] && { log "  [dry] 按 summary.tsv 算：吞吐损失% / 额外宿主 CPU-s / 每百万退出 Δrt_ns / 退出率漂移% + §4.3 漂移对照"; return 0; }
    [ -s "$OUT/summary.tsv" ] || return 0
    log ""
    log "== 汇总（同一负载、不同观测档；派生量以 O0 为基线，每臂取 ${REPEAT} 轮中位数） =="
    [ "${STAT_KIND:-}" = per-VM ] || warn "  退出计数取自全局聚合文件（${STAT_KIND:-无}），混进别的 VM 就会失真（§2.2）"
    local out
    out=$(awk -F'\t' -v hz="$USERHZ" '
        function nums(s, arr,   t, k, i, c) {
            k = split(s, t, ","); c = 0
            for (i = 1; i <= k; i++) if (t[i] ~ /^-?[0-9]/) arr[++c] = t[i] + 0
            return c
        }
        function med(s,   v, c, i, j, x) {
            c = nums(s, v)
            if (c == 0) return "NA"
            for (i = 2; i <= c; i++) { x = v[i]; j = i - 1; while (j >= 1 && v[j] > x) { v[j+1] = v[j]; j-- } v[j+1] = x }
            return (c % 2) ? v[(c + 1) / 2] : (v[c / 2] + v[c / 2 + 1]) / 2
        }
        function rng(s,   v, c, i, lo, hi) {
            c = nums(s, v)
            if (c == 0) return "NA"
            lo = v[1]; hi = v[1]
            for (i = 2; i <= c; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i] }
            return hi - lo
        }
        { a = $1
          if (!(a in seen)) { seen[a] = 1; ord[++no] = a; n[a] = 0 }
          n[a]++
          # ★ 用逗号串累积，不用二维数组的子数组：本机 awk 是 mawk，没有 array-of-array
          C[a] = C[a] "," $3;  E[a] = E[a] "," $4;  RTS[a] = RTS[a] "," $5
          B[a] = B[a] "," $6;  CS[a] = CS[a] "," $9; EV[a] = EV[a] "," $10
          OV[a] = OV[a] "," $11; D[a] = D[a] "," $2
        }
        END {
            if (!("O0" in seen)) { print "  没有 O0 基线行，派生量算不出来（先看上面的作废/FAILED）"; print "#NOBASE"; exit }
            base = med(C["O0"]); bex = med(E["O0"]); bbb = med(B["O0"]); brt = med(RTS["O0"])
            printf "  %-4s %11s %8s %8s %11s %9s %10s %12s %10s %8s %s\n", \
                   "臂", "completed/s", "极差", "损失%", "exits/s", "漂移%", "额外CPU-s", "ns/百万退出", "每次命中ns", "overrun", "消费CPU-s"
            gap = ""; prevC = "NA"
            for (i = 1; i <= no; i++) { a = ord[i]
                mC = med(C[a]); mE = med(E[a]); mRT = med(RTS[a]); mB = med(B[a])
                mCS = med(CS[a]); mEV = med(EV[a]); mD = med(D[a]); rC = rng(C[a]); ovr = med(OV[a])
                fC = (mC == "NA") ? "NA" : sprintf("%.1f", mC)
                frC = (rC == "NA") ? "NA" : sprintf("%.1f", rC)
                fE = (mE == "NA") ? "NA" : sprintf("%.1f", mE)
                loss  = (mC != "NA" && base != "NA" && base + 0 > 0) ? sprintf("%.2f", (base - mC) / base * 100) : "NA"
                drift = (mE != "NA" && bex != "NA" && bex + 0 > 0) ? sprintf("%.2f", (mE - bex) / bex * 100) : "NA"
                ecpu  = (mB != "NA" && bbb != "NA") ? sprintf("%.2f", (mB - bbb) / hz) : "NA"
                # ★ 分母是本窗的退出次数（exits/s 中位 × 窗长中位），不是速率：
                #   rt_ns 是窗内增量，两者相除才是"每次退出多少 ns"
                pern = "NA"
                if (mE != "NA" && mD != "NA" && mRT != "NA" && brt != "NA" && mE * mD > 0) \
                    pern = sprintf("%.0f", (mRT - brt) / (mE * mD) * 1e6)
                perhit = (mEV != "NA" && mEV + 0 > 0 && mRT != "NA" && brt != "NA" && a != "O0" && a != "O0e") \
                    ? sprintf("%.2f", (mRT - brt) / mEV) : "NA"
                ccpu = (mCS != "NA") ? sprintf("%.2f", mCS / 1e9) : "NA"
                printf "  %-4s %11s %8s %8s %11s %9s %10s %12s %10s %8s %s\n", \
                    a, fC, frC, loss, fE, drift, ecpu, pern, perhit, (ovr == "NA" ? "NA" : sprintf("%d", ovr)), ccpu
                if (i > 1 && mC != "NA" && prevC != "NA") { g = mC - prevC; if (g < 0) g = -g; if (gap == "" || g < gap) gap = g }
                prevC = mC
            }
            if ("O0e" in seen) {
                oc = med(C["O0e"])
                if (oc == "NA" || base == "NA" || base + 0 <= 0) { print "  §4.3 条件 2：O0/O0e 的 completed 不可用，漂移无从判定"; print "#DRIFT=NA" }
                else {
                    d = oc - base; if (d < 0) d = -d
                    # ★ 两边都得是 completed/s 的**绝对量**：拿 |Δ|/O0（无量纲）去比
                    #   1/3 × 臂间差（有量纲）永远成立，这条判据就形同虚设
                    if (gap == "") { print "  §4.3 条件 2：只有 O0/O0e 一档，臂间差无从计算"; print "#DRIFT=NA" }
                    else {
                        printf "  §4.3 条件 2 漂移核对：O0=%.1f O0e=%.1f → |Δ|=%.1f（基线的 %.2f%%），min(相邻臂间差)=%.1f → 阈值 %.2f → %s\n", \
                               base, oc, d, d / base * 100, gap, gap / 3, (d < gap / 3 ? "通过" : "★ 不通过 → 整轮作废")
                        print (d < gap / 3 ? "#DRIFT=PASS" : "#DRIFT=FAIL")
                    }
                }
            } else { print "  §4.3 条件 2：缺 O0e，漂移无从判定（--arms 里带上 O0e）"; print "#DRIFT=NA" }
        }' "$OUT/summary.tsv" 2>"$OUT/report.err") \
        || { warn "  汇总失败（summary.tsv 格式？）"; [ -s "$OUT/report.err" ] && sed 's/^/    awk: /' "$OUT/report.err" >&2; }
    [ -n "$out" ] && printf '%s\n' "$out" | grep -v -e '^#DRIFT=' -e '^#NOBASE' || true
    DRIFT=$(printf '%s\n' "$out" | sed -n 's/^#DRIFT=//p' | tail -1)
    NOBASE=$(printf '%s\n' "$out" | grep -c '^#NOBASE')
    [ "${DRIFT:-}" = FAIL ] && warn "  ★ 漂移对照不通过：上面所有派生量都不可引用（§4.3 条件 2，整轮作废）"
    [ -n "$VOID" ] && warn "  作废的臂：$VOID"
    [ -n "$FAIL" ] && warn "  未通过的臂：$FAIL"
}

# ---------- preflight ----------
preflight() {
    local rc=0 e miss="" n
    log "== E5 前置检查（只读） =="
    log "--- 1) 文件系统 ---"
    [ -d $TR ] || { warn "没有 $TR（tracefs 未挂？）"; return 1; }
    log "  tracefs OK：current_tracer=$(cat $TR/current_tracer) tracing_on=$(cat $TR/tracing_on) buffer_size_kb=$(cat $TR/buffer_size_kb)/cpu"
    [ -d $DBK ] && log "  debugfs kvm 目录存在" || warn "没有 $DBK（debugfs 未挂）—— 退出计数真值取不到"
    log "  USER_HZ=$USERHZ nproc=$(nproc) 负载：$(uptime | sed 's/.*load average/load/')"
    log "  trace_clock=$(cat $TR/trace_clock)"
    # ★ buffer_size_kb 是**每 CPU**的：总量 = BUF_KB × nproc。96 核 × 8 MiB = 768 MiB，
    #   小内存机器上这一写就是 OOM 诱因，且内核还可能按 max_buffer_size 截断
    n=$(( BUF_KB * $(nproc) / 1024 ))          # MiB
    awk -v want="$n" '/^MemTotal:/{ if (want*1024 > $2/4) print "  ★ ring buffer 总量约 " want " MiB，超过本机内存的 1/4（MemTotal " int($2/1024) " MiB）→ 用 --buf-kb 调小"; else print "  ring buffer 总量约 " want " MiB（" BUFKB "kB/cpu × " NPROC " cpu），内存占比可接受" }' \
        BUFKB="$BUF_KB" NPROC="$(nproc)" /proc/meminfo 2>/dev/null | sed 's/^/  /' || true
    log "  O7 读者落盘硬上限 ${PIPE_CAP_MB} MiB（撞顶则该臂判失败，§6.13）"

    log "--- 2) 独占：有没有别人在跑 VM / 在 trace ---"
    n=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c '/dev/kvm') || true
    if [ "${n:-0}" != 0 ]; then
        warn "已有 ${n} 个 /dev/kvm fd 在跑：全局 debugfs 统计与 function tracer 都会串台（§2.2、§3.1）"
        [ "$KNOW_OTHER_VMS" = 1 ] && log "  已带 --i-know-other-vms-exist：继续，但派生列只能当上界看" \
            || rc=1
    else log "  无其它 VM 在跑"; fi
    n=$(ls -d $DBK/[0-9]* 2>/dev/null | wc -l)
    [ "${n:-0}" = 0 ] || warn "debugfs 里已有 ${n} 个 VM 目录（跑起来之后应当只剩我们这一台，§2.2）"
    n=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c 'trace_pipe') || true
    [ "${n:-0}" = 0 ] || { warn "有 ${n} 个进程在读 trace_pipe —— 消费侧成本会被别人混进来（§4.1 O7）"; rc=1; }
    for e in current_tracer tracing_on function_profile_enabled; do
        case "$(cat $TR/$e 2>/dev/null)" in nop|0|'') ;; *) warn "残留 $e=$(cat $TR/$e)（AGENTS.md 陷阱 9）"; rc=1 ;; esac
    done
    # ★ tracefs 没有 enabled_events；"谁挂着事件"只看 set_event 复读（只列已启用事件，
    #   §2.5），ftrace 侧的 ops 残留看 enabled_functions，bpf 侧看进程表/bpftool。
    n=$(set_event_list | grep -c .)
    if [ "${n:-0}" != 0 ]; then warn "set_event 里已有 ${n} 个事件：$(set_event_list | tr '\n' ' ')"; rc=1; else log "  set_event 为空"; fi
    n=$(filter_list | wc -w)
    [ "${n:-0}" = 0 ] || { warn "set_ftrace_filter 非空：$(filter_list)"; rc=1; }
    n=$(cat $TR/enabled_functions 2>/dev/null | wc -l)
    if [ "${n:-0}" != 0 ]; then
        log "  enabled_functions 有 $n 个函数挂着 ftrace ops（宿主常态，kprobe/bpf 也在这里）："
        cat $TR/enabled_functions 2>/dev/null | head -3 | sed 's/^/    /'
        for e in $FUNCS; do
            if grep -qE -- "^$e( |\()" $TR/enabled_functions 2>/dev/null; then
                warn "退出路径上的 $e 已被别人占用 → O5 的命中数不是本负载造成的"; rc=1
            fi
        done
    fi
    if command -v bpftool >/dev/null; then
        nbpf=$(bpftool -jp prog 2>/dev/null | grep -c '"type": "tracepoint"')
        # bpftool link list 里 tracepoint 挂载点印成一行 `tracepoint <事件名>`（不带 system 前缀）
        tps=$(bpftool link list 2>/dev/null | awk '/tracepoint/{print $2}' | sort -u)
        ntps=$(printf '%s\n' $tps | grep -c .)
        if [ "${nbpf:-0}" != 0 ]; then
            hit=$(comm -12 <(printf '%s\n' $tps) <(ls $TR/events/kvm 2>/dev/null | sort -u))
            if [ -n "$hit" ]; then
                warn "外部 BPF 已挂在这些 kvm tracepoint 上：$(printf '%s ' $hit)→ O0 不是真零基线（§2.1），必须先卸掉"
                rc=1
            elif [ "${ntps:-0}" = 0 ]; then
                warn "有 ${nbpf} 个 tracepoint 型 BPF 程序，但本机 bpftool 列不出挂点名 → 无法证明它没挂 kvm 组"
                rc=1
            else
                log "  tracepoint 型 BPF 程序 ${ntps} 个：$(printf '%s ' $tps)（不在 kvm 组 → O0 仍成立；O6 只认自己那个 @c）"
            fi
        fi
    fi
    pgrep -x bpftrace >/dev/null && { warn "已有 bpftrace 在跑"; rc=1; }
    pgrep -x perf >/dev/null && { warn "已有 perf 在跑（perf 也走同一个 tracepoint 的另一个 probe，§2.5）"; rc=1; }

    log "--- 3) 事件与函数存在性（宿主 $(uname -r) ≠ 文档 6.12.93） ---"
    for e in "$EVENT_ENTRY" "$EVENT_EXIT"; do
        [ -d "$TR/events/kvm/$e" ] && log "  事件 kvm:$e 存在" || { warn "缺事件 kvm:$e，O1/O2/O3/O7 判据无从核对"; miss="$miss kvm:$e"; rc=1; }
    done
    n=$(ls -d $TR/events/kvm/*/ 2>/dev/null | wc -l)
    log "  kvm 组共 $n 个事件目录（O4 就是全开这些）"
    for e in $FUNCS; do
        traceable "$e" && log "  可跟踪：$e" || { warn "不可跟踪：$e（被内联或改名，§3.1 要求现场核对）"; rc=1; }
    done

    log "--- 4) bpftrace ---"
    if command -v bpftrace >/dev/null; then
        log "  $(bpftrace --version 2>&1 | head -1)"
        if [ "$ONLY_PREFLIGHT" = 1 ]; then
            log "  实挂测试（几秒，无 VM 时 0 事件，只验能力）"
            timeout 6 bpftrace -e "t:kvm:$EVENT_EXIT { @c = count(); }" 2>&1 | head -2 | sed 's/^/    /'
        else log "  （真跑时 O6 自己会挂，并等 'Attached 1 probe' 出现）"; fi
    else warn "没有 bpftrace → O6 无从做起"; rc=1; fi

    log "--- 5) schedstat / 负载材料 ---"
    n=$(awk '{print $1}' /proc/self/schedstat 2>/dev/null)
    if [ -z "$n" ]; then warn "读不到 /proc/<tid>/schedstat → §4.1 的主归因量没有"; rc=1
    elif [ "$n" = 0 ]; then warn "schedstat 第 1 列恒为 0（CONFIG_SCHED_INFO 未开）→ 主归因量没有"; rc=1
    else log "  schedstat 可用（本进程 sum_exec_runtime=${n}ns）"
         [ "$(awk '{print $2}' /proc/self/schedstat)" = 0 ] && \
             log "  注意：run_delay 本机当前为 0，被抢过核的线程才有值 → 不可用时该列记 NA（§4.1）"; fi
    [ -f "$KERNEL" ] && [ -f "$INITRD" ] && log "  内核/initrd 在" || { warn "缺 $KERNEL 或 $INITRD"; rc=1; }
    for p in "$KERNEL" "$INITRD" "$KO_DIR/ple_load.ko" "$SHARE/ple_load.ko"; do
        [ -e "$p" ] || warn "  缺 $p"
    done
    cmp -s "$KO_DIR/ple_load.ko" "$SHARE/ple_load.ko" \
        && log "  共享区 ple_load.ko 与本地构建一致" \
        || warn "  共享区 ple_load.ko 缺失或与 $KO_DIR 不一致，guest 会加载旧版本"

    log "--- 6) 时间预算 ---"
    n=$(echo "$ARMS_WANT" | tr ',' '\n' | grep -c .)
    log "  ${n} 臂 × (warm ${WARM_S}s + sample ${SAMPLE_S}s) × repeat ${REPEAT} ≈ $(( n * (WARM_S + SAMPLE_S) * REPEAT / 60 )) 分钟"
    log "  另有档间恢复与统计读取消耗（读 \$TR/trace 要格式化整块缓冲，慢但在窗外）"

    if [ "$rc" = 0 ]; then log "== 前置检查通过 =="; else log "== 前置检查有未通过项，先修再跑 =="; fi
    return $rc
}

# ---------- 参数解析 ----------
# ★ 脚本开头已 cd 进自己的目录（为了 INITRD 这类相对路径），所以**用户传的相对路径
#   必须按调用者 cwd 展开**，否则 --out tmp/x 会莫名其妙落到 practice/bench/tmp/x。
_abspath() { case $1 in /*) printf '%s\n' "$1" ;; *) printf '%s/%s\n' "$ORIG_PWD" "$1" ;; esac; }
while [ $# -gt 0 ]; do
    case $1 in
        --preflight)   ONLY_PREFLIGHT=1; shift ;;
        --dry-run)     DRY=1; shift ;;
        --all)         ARMS_WANT="O0,O1,O2,O3,O4,O5,O6,O7,O0e"; shift ;;
        --arms)        ARMS_WANT="$2"; shift 2 ;;
        --funcs)       FUNCS="$2"; shift 2 ;;
        --repeat)      REPEAT="$2"; shift 2 ;;
        --sample-s)    SAMPLE_S="$2"; shift 2 ;;
        --warm-s)      WARM_S="$2"; shift 2 ;;
        --tick-s)      TICK_S="$2"; shift 2 ;;
        --buf-kb)      BUF_KB="$2"; shift 2 ;;
        --vcpu)        VCPU="$2"; shift 2 ;;
        --priv-kb)     PRIV_KB="$2"; shift 2 ;;
        --kernel)      KERNEL="$2"; shift 2 ;;
        --out)         OUT=$(_abspath "$2"); shift 2 ;;
        --report-from) REPORT_FROM=$(_abspath "$2"); shift 2 ;;
        --i-know-other-vms-exist) KNOW_OTHER_VMS=1; shift ;;
        -h|--help)     usage ;;
        *)             warn "未知参数 $1"; usage ;;
    esac
done
for v in SAMPLE_S TICK_S WARM_S REPEAT VCPU BUF_KB PRIV_KB; do
    case "${!v}" in ''|*[!0-9]*) die "$v 需要整数，实际是 '${!v}'" ;; esac
done
[ "$SAMPLE_S" -ge 5 ] || die "--sample-s 太小（<5s），tick 都跑不满一轮"
[ "$TICK_S" -ge 1 ] || die "--tick-s 要 >=1"
[ "$WARM_S" -ge 1 ] || die "--warm-s 要 >=1（档位的 text_poke 必须落在窗外，§2.3）"
[ "$REPEAT" -ge 1 ] || die "--repeat 至少 1"
[ "$VCPU" -ge 1 ] || die "--vcpu 至少 1"
# buffer_size_kb 写 0 会被内核拒（tracing_entries_write() kernel/trace/trace.c:6797-6801
# "must have at least 1 entry" → -EINVAL），且 preflight 的内存估算会变成 0
[ "$BUF_KB" -ge 1 ] || die "--buf-kb 至少 1（写 0 内核直接 -EINVAL）"
IFS=',' read -r -a SEQ <<< "$ARMS_WANT"
for e in "${SEQ[@]}"; do case $e in O0|O0e|O1|O2|O3|O4|O5|O6|O7) ;; *) die "--arms 里不认的臂：$e";; esac; done

if [ -n "$REPORT_FROM" ]; then
    OUT="$REPORT_FROM"
    [ -s "$OUT/summary.tsv" ] || die "$OUT/summary.tsv 不存在或为空，没有可重算的数据"
    REPEAT=$(awk -F'\t' '{ c[$1]++ } END { m = 0; for (a in c) if (c[a] > m) m = c[a]; print m + 0 }' "$OUT/summary.tsv")
    STAT_KIND=$(awk -F'\t' 'END { print $14 }' "$OUT/summary.tsv")
    report
    [ "${NOBASE:-0}" != 0 ] && die "summary.tsv 里没有 O0 基线行，重算失败"
    [ "${DRIFT:-}" = FAIL ] && exit 2
    exit 0
fi
if [ "$ONLY_PREFLIGHT" = 1 ]; then preflight; exit $?; fi
[ "$(id -u)" = 0 ] || [ "$DRY" = 1 ] || die "需要 root（tracefs / debugfs / 起 VM）"

if [ "$DRY" != 1 ]; then
    preflight || die "前置检查未通过，不动系统（§2.6）"
    mkdir -p "$OUT"
    fingerprint "$OUT/fingerprint-start.txt"
    printf 'arms=%s sample_s=%s warm_s=%s tick_s=%s vcpu=%s priv_kb=%s buf_kb=%s repeat=%s funcs=%s\n' \
        "$ARMS_WANT" "$SAMPLE_S" "$WARM_S" "$TICK_S" "$VCPU" "$PRIV_KB" "$BUF_KB" "$REPEAT" "$FUNCS" \
        > "$OUT/params.txt"
    snap_ftrace
    boot_vm
    find_stat_path exits || warn "找不到 exits 统计文件，退出次数真值无从核对（§2.2）"
    log "  统计文件：${STAT_NAME:-无}（${STAT_KIND:-无}）"
    [ "${STAT_KIND:-}" = per-VM ] || warn "用的是全局聚合文件：混进别的 VM 就会失真（§2.2）"
    log ""
    r=1
    while [ "$r" -le "$REPEAT" ]; do
        log "===== 第 $r/$REPEAT 轮 ====="
        for a in "${SEQ[@]}"; do run_arm "$a"; done
        r=$((r + 1))
    done
    fingerprint "$OUT/fingerprint-end.txt"
    report
    log ""
    if cmp -s "$OUT/fingerprint-start.txt" "$OUT/fingerprint-end.txt"; then
        log "收尾指纹一致：ftrace / VM / 读者全部回到原状"
    else
        warn "收尾指纹与开始时不同，逐项对比："
        diff "$OUT/fingerprint-start.txt" "$OUT/fingerprint-end.txt" | sed 's/^/    /' >&2
    fi
    log "数据目录：$OUT"
    [ -n "$FAIL" ] && exit 1
    [ "${DRIFT:-}" = FAIL ] && exit 2
    exit 0
fi

# ---------- dry-run ----------
log "== E5 dry-run：只打印时间线，不碰系统 =="
preflight || log "  （preflight 在 dry-run 下不阻塞时间线打印）"
log "  [dry] mkdir -p $OUT；fingerprint-start"
log "  [dry] snap_ftrace：记录 current_tracer / set_event 复读 / tracing_on / buffer_size_kb 原值"
log "  [dry] boot_vm：$VCPU vCPU + ple_load workload=1 priv_kb=$PRIV_KB（VM 只起一次，档间不重启）"
log "  [dry] find_stat_path exits → 优先 $DBK/<pid>-<fd>/exits（§2.2）"
r=1
while [ "$r" -le "$REPEAT" ]; do
    log "  ----- 第 $r/$REPEAT 轮 -----"
    for a in "${SEQ[@]}"; do run_arm "$a"; done
    r=$((r + 1))
done
log "  [dry] fingerprint-end + report + restore_ftrace（写回原值）+ 关 VM"
report
exit 0
