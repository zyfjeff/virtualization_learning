#!/usr/bin/env bash
# E3 · vCPU 迁移代价驱动器 —— 设计文档见 bench-migrate.md，代价拆分见其 §1
#
# 一次测量 = 一次完整 boot：绑核策略与注入节奏在 VM 生命周期内必须恒定，
# 中途换策略会让"上一段的迁移"混进"这一段的基线"（vCPU 线程落点是持久状态）。
#
# 主判据是 ftrace function profiler 数 loaded_vmcs_clear 的命中次数
# （真 VMCS 换核次数，见 bench-migrate.md §2.1）；★ 统计必须在关机之前取，
# 否则 free_loaded_vmcs() 会给每个 vCPU 再加一次。
#
# 用法：
#   ./bench-migrate.sh --preflight                 只做 bench-migrate.md §2 的检查（只读）
#   ./bench-migrate.sh --arm M2 --dry-run          打印将执行的动作，不碰系统
#   sudo ./bench-migrate.sh --all --repeat 5 --step-ms 200 --sample-s 20
set -u
cd "$(dirname "$0")"

# ---------- 路径与常量 ----------
KERNEL=/root/code/linux-6.12.93/arch/x86_64/boot/bzImage   # 与 boot-vm.sh:178 同源
INITRD=../../scripts/images/initramfs.img
KO_DIR=ple-load
SHARE=../../scripts/shared
TR=/sys/kernel/tracing
DBK=/sys/kernel/debug/kvm
SYS=/sys/devices/system

VCPU=4                  # guest vCPU 数 = 被注入的线程数
PRIV_KB=256             # guest 每线程私有缓冲区（workload=1）
SAMPLE_S=20             # 采样窗
WARM_S=3
STEP_MS=200             # 注入节奏；★ 自变量，不同取值的 M2 之间不可比
NODE=${NODE:-0}         # 只在 node0 里选核，避免把 NUMA 效应混进来
BUF_KB=8192

TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/migrate-$TS
DRY=0; ONLY_PREFLIGHT=0; WITH_PLE=0
ARMS=(); REPEAT=1
ALL_ARMS=(M0 M1 M2 M3 M4)

log()  { printf '%s\n' "$*"; }
warn() { printf '!! %s\n' "$*" >&2; }
die()  { warn "$*"; exit 1; }

usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

# ---------- 实验臂：mode ----------
#   none   = M0 每线程独占一核，全程不动
#   wide   = M1 全部线程允许 A∪B，交给宿主调度器
#   toggle = M2 每步在 A[i] 与 B[i] 之间换（换物理核）
#   noop   = M3 每步重复设 A[i]（同形状 syscall，不搬家）= M2 的阴性对照
#   smt    = M4 每步在 A[i] 与其 SMT 兄弟之间换（换逻辑 CPU，不换物理核）
arm_mode() {
    case $1 in
        M0) echo none ;;  M1) echo wide ;;
        M2) echo toggle ;; M3) echo noop ;; M4) echo smt ;;
        *)  return 1 ;;
    esac
}
arm_injects() { case $1 in M2|M3|M4) return 0 ;; *) return 1 ;; esac; }

# ---------- 状态 ----------
QEMU_PID=""; CAT_PID=""; ORIG_AFF=(); TID_N=0
INJ_OK=0; INJ_FAIL=0
declare -a VTID A B S                      # VTID[i] = vCPU i 的宿主 tid；A/B/S[i] = 目标核

# ---------- 恢复 ----------
restore_aff() {
    local i
    [ "${#ORIG_AFF[@]}" = 0 ] && return 0
    for i in "${!ORIG_AFF[@]}"; do
        [ -n "${ORIG_AFF[$i]}" ] || continue
        taskset -pc "${ORIG_AFF[$i]}" "${VTID[$i]}" >/dev/null 2>&1 || \
            warn "恢复 tid ${VTID[$i]} 亲和性失败（原值 ${ORIG_AFF[$i]}），请手工执行 taskset -pc ${ORIG_AFF[$i]} ${VTID[$i]}"
    done
    log "  已恢复 ${#ORIG_AFF[@]} 个 vCPU 线程的原亲和性"
    ORIG_AFF=()
}
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$CAT_PID" ]  && kill "$CAT_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    [ "$DRY" = 1 ] && return 0
    restore_aff || true
    clear_ftrace || true
    return 0
}
trap cleanup EXIT INT TERM

# ---------- ftrace ----------
clear_ftrace() {
    [ -e $TR/current_tracer ] || return 0
    if [ "$DRY" = 1 ]; then log "  [dry] 清 ftrace: current_tracer=nop、set_ftrace_filter、set_event"; return 0; fi
    echo 0 > $TR/function_profile_enabled 2>/dev/null || true
    echo nop > $TR/current_tracer 2>/dev/null || true
    echo > $TR/set_ftrace_filter 2>/dev/null || true
    echo > $TR/set_event 2>/dev/null || true
}
# 模块符号在 available_filter_functions 里带 [kvm_intel]/[kvm] 后缀，grep -x 会误判
traceable() { grep -qE "^$1( |\[|$)" $TR/available_filter_functions 2>/dev/null; }
profiler_ok() { [ -w $TR/function_profile_enabled ] && [ -d $TR/trace_stat ]; }

# 迁移路径上的四个汇聚函数 + PI/steal 旁路（bench-migrate.md §2.4）
PROF_FUNCS="kvm_arch_vcpu_load vmx_vcpu_load loaded_vmcs_clear vmx_vcpu_pi_load record_steal_time"

start_profile() {
    local f list="" bad=""
    for f in $PROF_FUNCS; do traceable "$f" && list="$list $f"; done
    [ -n "$list" ] || { warn "迁移路径函数全都不可跟踪，机制侧没有出口（bench-migrate.md §2.4）"; return 1; }
    if [ "$DRY" = 1 ]; then
        log "  [dry] function_profile_enabled=0 → 逐个 echo 名字 >> set_ftrace_filter（$list）→ function_profile_enabled=1"
        log "  [dry] current_tracer 保持 nop；名字逐个写入，匹配不上的逐个告警（静默截断的风险见 §4.2.1）"
        return 0
    fi
    profiler_ok || { warn "本机没有 function profiler（CONFIG_FUNCTION_PROFILER=n），无法低开销数命中数"; return 1; }
    # 必须逐个写：一次写好几个名字时，走到第一个匹配不上的名字就中止本次 write，
    # 后面的名字连带丢失；若它在最前面，filter 停在"全部函数都开"—— 统计器照样
    # 给出看着合理的数字，其实已经把整机所有函数都统计进去了（bench-migrate.md §4.2.1）。
    echo 0 > $TR/function_profile_enabled 2>/dev/null || true
    echo > $TR/set_ftrace_filter 2>/dev/null || return 1
    for f in $list; do
        echo "$f" >> $TR/set_ftrace_filter 2>/dev/null || bad="$bad $f"
    done
    [ -n "$bad" ] && warn "这几个函数写不进 set_ftrace_filter：$bad（§4.2.1）"
    echo 1 > $TR/function_profile_enabled || { warn "function_profile_enabled 打不开"; return 1; }
    log "  统计器已开：${list# }${bad:+（未生效：$bad）}"
}
# ★ 必须在 kill QEMU 之前调用：free_loaded_vmcs()→loaded_vmcs_clear 会给每 vCPU 再加一次
stop_profile() {
    local f
    [ "$DRY" = 1 ] && return 0
    [ -e $TR/function_profile_enabled ] || return 0
    [ "$(cat $TR/function_profile_enabled 2>/dev/null)" = 1 ] || return 0
    echo 0 > $TR/function_profile_enabled 2>/dev/null || true
    : > "$OUT/prof-latest.txt"
    for f in $PROF_FUNCS; do
        grep -hE "^ *$f +[0-9]" $TR/trace_stat/function* 2>/dev/null \
            | awk -v n="$f" '{h+=$2} END{printf "%s %d\n", n, h+0}' >> "$OUT/prof-latest.txt"
    done
    # 读不到行 = 命中 0，必须显式补 0，否则后面按名字取值会拿到空串
    for f in $PROF_FUNCS; do
        grep -q "^$f " "$OUT/prof-latest.txt" || echo "$f 0" >> "$OUT/prof-latest.txt"
    done
    sed -i 's/^[[:space:]]*//' "$OUT/prof-latest.txt" 2>/dev/null || true
    {
        printf '  profiler:'
        while read -r n h; do printf ' %s=%s' "$n" "$h"; done < "$OUT/prof-latest.txt"
        printf '\n'
    } | tee -a "$OUT/$CURRENT_ARM.txt" 2>/dev/null
    echo > $TR/set_ftrace_filter 2>/dev/null || true
    return 0
}
prof_hit() { awk -v n="$1" '$1==n{print $2}' "$OUT/prof-latest.txt" 2>/dev/null; }

setup_events() {
    [ "$DRY" = 1 ] && { log "  [dry] set_event=kvm:kvm_vcpu_wakeup$([ "$WITH_PLE" = 1 ] && echo ' + kvm:kvm_ple_window_update')"; return 0; }
    : > $TR/set_event
    echo $BUF_KB > $TR/buffer_size_kb 2>/dev/null || warn "buffer_size_kb 设置失败"
    [ -d $TR/events/kvm/kvm_vcpu_wakeup ] && echo kvm:kvm_vcpu_wakeup >> $TR/set_event 2>/dev/null \
        || warn "缺 kvm:kvm_vcpu_wakeup（§4.2 条件 4 的 wait/poll 之分无从核对）"
    [ "$WITH_PLE" = 1 ] && [ -d $TR/events/kvm/kvm_ple_window_update ] && \
        echo kvm:kvm_ple_window_update >> $TR/set_event 2>/dev/null
    echo 1 > $TR/tracing_on
}
collect_events() {                            # $1=arm
    local a=$1 ovr n_wake n_wait n_poll
    [ "$DRY" = 1 ] && return 0
    cp $TR/trace "$OUT/trace-$a.txt" 2>/dev/null || true
    cat $TR/per_cpu/cpu*/stats > "$OUT/bufstats-$a.txt" 2>/dev/null || true
    n_wake=$(grep -c 'kvm_vcpu_wakeup' "$OUT/trace-$a.txt" 2>/dev/null || true)
    # print fmt 是 "%s time %lld ns, polling %s"，第一项 waited ? "wait" : "poll"
    n_wait=$(grep -c 'kvm_vcpu_wakeup: wait ' "$OUT/trace-$a.txt" 2>/dev/null || true)
    n_poll=$(grep -c 'kvm_vcpu_wakeup: poll ' "$OUT/trace-$a.txt" 2>/dev/null || true)
    log "  events: wakeup=${n_wake:-0}（wait=${n_wait:-0} poll=${n_poll:-0}） ple_window_update=$(grep -c 'kvm_ple_window_update' "$OUT/trace-$a.txt" 2>/dev/null || true)"
    echo "wakeup_total=${n_wake:-0} wakeup_wait=${n_wait:-0} wakeup_poll=${n_poll:-0}" >> "$OUT/$a.txt"
    # 总数 ≈ halt 次数是正常现象（每次 kvm_vcpu_halt 结尾无条件发一条），
    # 只有 wait 一侧非 0 才说明 vCPU 真睡过 → PI 的 wakeup 链表分支参与进来了。
    [ "${n_wait:-0}" = 0 ] || \
        warn "有 ${n_wait} 次真的阻塞过（waited=true），PI wakeup-链表分支参与，§4.2 条件 4 不成立"
    ovr=$(grep -h 'overrun:' "$OUT/bufstats-$a.txt" 2>/dev/null | awk '{s+=$NF} END{print s+0}')
    [ "${ovr:-0}" = 0 ] || warn "ring buffer 有 $ovr 次 overrun，事件计数偏低（../measurement.md §4(c)）"
}

# ---------- 核选取：不同物理核、同 NUMA node ----------
expand_list() {                              # "0,48" / "0-3" → 空格分隔
    local IFS=',' c out=""
    for c in $1; do
        case $c in
            *-*) out="$out $(seq "${c%-*}" "${c#*-}")" ;;
            *)   out="$out $c" ;;
        esac
    done
    echo $out
}
# $1 = 需要多少个**物理核** → 每行打印 "本核最小编号 兄弟编号(无则同前)"
pick_cores() {
    local want=$1 got=0 seen=" " c sib first second n
    for c in $(seq 0 $(( $(nproc) - 1 ))); do
        [ "$got" -ge "$want" ] && break
        [ -r "$SYS/cpu/cpu$c/topology/thread_siblings_list" ] || continue
        n=$(cat "$SYS/cpu/cpu$c/node" 2>/dev/null)
        [ "${n:-$NODE}" = "$NODE" ] || continue
        sib=$(expand_list "$(cat "$SYS/cpu/cpu$c/topology/thread_siblings_list")")
        # shellcheck disable=SC2086
        set -- $sib
        first=${1:-}; second=${2:-$first}
        [ -n "$first" ] || continue
        case $seen in *" $first "*) continue ;; esac
        seen="$seen$first "
        printf '%s %s\n' "$first" "$second"
        got=$((got + 1))
    done
    [ "$got" -ge "$want" ] || return 1
}
plan_cpus() {
    local rows
    rows=$(pick_cores $((VCPU * 2))) || die "node$NODE 上凑不出 $((VCPU * 2)) 个不同物理核，减小 VCPU 或换 NODE"
    mapfile -t A < <(printf '%s\n' "$rows" | head -n "$VCPU" | awk '{print $1}')
    mapfile -t B < <(printf '%s\n' "$rows" | tail -n "$VCPU" | awk '{print $1}')
    mapfile -t S < <(printf '%s\n' "$rows" | head -n "$VCPU" | awk '{print $2}')
    log "  A 组核=${A[*]}  B 组核=${B[*]}  A 的 SMT 兄弟=${S[*]}（全部 node$NODE、互不同核）"
}

# ---------- vCPU 线程发现与亲和性 ----------
find_vcpu_tids() {                           # $1=qemu pid
    local pid=$1 t n idx
    VTID=(); TID_N=0
    for t in /proc/$pid/task/*; do
        [ -r "$t/comm" ] || continue
        n=$(cat "$t/comm")
        case $n in
            "CPU "*/KVM)
                idx=${n#CPU }; idx=${idx%/KVM}
                [[ "$idx" =~ ^[0-9]+$ ]] || continue
                [ "$idx" -lt "$VCPU" ] || continue
                VTID[idx]=$(basename "$t"); TID_N=$((TID_N + 1)) ;;
        esac
    done
    [ "$TID_N" = "$VCPU" ] || { warn "只找到 $TID_N 个 'CPU <n>/KVM' 线程，期望 $VCPU（QEMU 版本可能改了线程名，§2.2）"; return 1; }
    log "  vCPU 线程：$(for i in "${!VTID[@]}"; do printf 'v%d=%s ' "$i" "${VTID[$i]}"; done)"
}
save_aff() {
    local i raw
    ORIG_AFF=()
    for i in "${!VTID[@]}"; do
        raw=$(taskset -pc "${VTID[$i]}" 2>/dev/null | sed 's/.*: //')
        ORIG_AFF[i]=$raw
    done
    [ "$DRY" = 1 ] || log "  已记录原亲和性：${ORIG_AFF[0]:-?}（退出时逐个恢复）"
}
tid_allowed() {                              # $1=tid → Cpus_allowed_list
    sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' "/proc/$1/status" 2>/dev/null
}
cpu_list_has() {                             # $1=list $2=cpu
    local c
    for c in $(expand_list "$1"); do [ "$c" = "$2" ] && return 0; done
    return 1
}
set_aff() {                                  # $1=tid $2=cpu $3=说明（用于计数）
    if [ "$DRY" = 1 ]; then log "  [dry] taskset -pc $2 $1"; return 0; fi
    if taskset -pc "$2" "$1" >/dev/null 2>&1; then
        [ "$3" = inj ] && INJ_OK=$((INJ_OK + 1))
    else
        warn "taskset -pc $2 $1 失败"; [ "$3" = inj ] && INJ_FAIL=$((INJ_FAIL + 1))
        return 1
    fi
}
init_aff() {                                 # $1=arm
    local i mode wide; mode=$(arm_mode "$1")
    wide=$(printf '%s,%s' "${A[*]}" "${B[*]}" | tr ' ' ',' | sed 's/,$//')
    if [ "$DRY" = 1 ]; then
        if [ "$mode" = wide ]; then log "  [dry] 每个 vCPU 线程 → $wide（交给宿主调度器）"; else
            for i in "${!VTID[@]}"; do log "  [dry] taskset -pc ${A[$i]} ${VTID[$i]}"; done
        fi
        return 0
    fi
    for i in "${!VTID[@]}"; do
        if [ "$mode" = wide ]; then
            set_aff "${VTID[$i]}" "$wide" "" || return 1
            continue
        fi
        cpu_list_has "$(tid_allowed "${VTID[$i]}")" "${A[$i]}" || \
            die "tid ${VTID[$i]} 的 Cpus_allowed_list 里没有核 ${A[$i]}，绑核会被静默裁剪（§6.1）"
        set_aff "${VTID[$i]}" "${A[$i]}" "" || return 1
    done
    return 0
}
inject_step() {                              # $1=arm $2=step
    local arm=$1 s=$2 i p cpu
    p=$(( s % 2 ))
    for i in "${!VTID[@]}"; do
        case $arm in
            M2) if [ "$p" = 0 ]; then cpu=${A[$i]}; else cpu=${B[$i]}; fi ;;
            M3) cpu=${A[$i]} ;;
            M4) if [ "$p" = 0 ]; then cpu=${A[$i]}; else cpu=${S[$i]}; fi ;;
            *)  return 0 ;;
        esac
        [ "$DRY" = 1 ] && { log "  [dry] step $s: taskset -pc $cpu ${VTID[$i]}（真跑时 ${#VTID[@]} 个线程各一次）"; return 0; }
        set_aff "${VTID[$i]}" "$cpu" inj || true
    done
}

# ---------- debugfs 统计（位置随内核版本变，§2.5） ----------
kvm_stat_file() {                            # $1=name → 第一个可读路径
    local c
    for c in $DBK/"$1" $DBK/*/"$1"; do
        [ -r "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
    return 1
}
read_stat() {                                # $1=name → 值或 NA
    local f; f=$(kvm_stat_file "$1") || { echo NA; return; }
    cat "$f" 2>/dev/null | tr -d '[:space:]' || echo NA
}
tsc_offset_all() {                           # → "vcpu0=<val> vcpu1=..."
    local i f out=""
    for ((i = 0; i < VCPU; i++)); do
        f=$(ls $DBK/*/vcpu$i/tsc-offset 2>/dev/null | head -1)
        if [ -n "$f" ]; then out="$out vcpu$i=$(cat "$f" 2>/dev/null | tr -d '[:space:]')"; else out="$out vcpu$i=NA"; fi
    done
    echo "${out# }"
}

# ---------- guest 交互 ----------
run_guest() {
    if [ "$DRY" = 1 ]; then log "  [dry] guest# $*"; return 0; fi
    printf '%s\n' "$*" >&7
}
wait_marker() {
    for _ in $(seq 1 300); do
        grep -q '^-------------------' "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
wait_for() {                                # $1=file $2=grep pattern $3=秒
    local end=$((SECONDS + ${3:-60}))
    while [ $SECONDS -lt $end ]; do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.2
    done
    return 1
}
# 只认真数字；guest 回显的那行命令以 "$(" 或 "echo" 开头，不会被匹配
guest_val() { grep -E "^$2 [0-9]+[[:space:]]*$" "$1" | tail -1 | cut -d' ' -f2; }

verify_kvm() {
    local n
    n=$(ls -l /proc/$1/fd 2>/dev/null | grep -c '/dev/kvm') || true
    [ "${n:-0}" -gt 0 ] || die "PID $1 未持有 /dev/kvm —— 走的是 TCG（AGENTS.md 陷阱 7）"
    log "  KVM 确认：$n 个 /dev/kvm fd"
}
verify_vermagic() {
    local ko_ver cur_ver
    ko_ver=$(modinfo -F vermagic "$KO_DIR/ple_load.ko" 2>/dev/null | awk '{print $1}')
    [ -n "$ko_ver" ] || { warn "读不到 $KO_DIR/ple_load.ko，先 cd $KO_DIR && make"; return 1; }
    cur_ver=$(grep -m1 '^kernel  *:' "$1" | awk '{print $3}')
    [ "$cur_ver" = "$ko_ver" ] || die "guest uname=$cur_ver ≠ 模块 vermagic=$ko_ver，insmod 必被拒"
    log "  guest/模块版本一致：$cur_ver"
}
# 剥 comm 后 processor 是第 37 个字段（原始行第 39 个），见 bench-migrate.md §4.1
tid_cpu() { awk '{gsub(/.*\) /, "", $0); print $37}' "/proc/$1/stat" 2>/dev/null; }
sample_tids() {                             # $1=tag
    local i out=""
    for i in "${!VTID[@]}"; do out="$out v$i@${VTID[$i]}=$(tid_cpu "${VTID[$i]}")"; done
    echo "$1${out# }" >> "$OUT/$CURRENT_ARM.txt"
    log "  落点 $1:${out# }"
}

# ---------- 单臂一轮 ----------
CURRENT_ARM=""
boot_and_measure() {                        # $1=arm
    local arm=$1 mode steps step c0 c1 st0 st1 rate o0 o1 i
    mode=$(arm_mode "$arm"); CURRENT_ARM=$arm
    steps=0
    arm_injects "$arm" && steps=$(( SAMPLE_S * 1000 / STEP_MS ))
    INJ_OK=0; INJ_FAIL=0
    log "-- $arm: mode=$mode vcpu=$VCPU priv=${PRIV_KB}KiB sample=${SAMPLE_S}s step=${STEP_MS}ms 计划注入=$steps 次/线程"

    plan_cpus
    if [ "$DRY" = 1 ]; then
        log "  [dry] qemu-system-x86_64 -enable-kvm -cpu host -m 2G -smp $VCPU \\"
        log "        -kernel $KERNEL -initrd $INITRD -append 'console=ttyS0 ...' \\"
        log "        -virtfs local,path=$SHARE,mount_tag=hostshare,... -serial pipe:$OUT/ser-$arm"
        # tid 只有 QEMU 跑起来才认得出（§2.2），dry-run 里用占位名把计划打印完整
        VTID=(); for i in $(seq 0 $((VCPU - 1))); do VTID[i]="<vcpu$i-tid>"; done
        log "  [dry] vCPU 线程 tid 由 find_vcpu_tids 现查，下面用占位名"
        init_aff "$arm"
        setup_events
        [ "$arm" = M4 ] && log "  [dry] M4 需要 SMT 兄弟，本机 A 组兄弟=${S[*]}"
        for step in 1 2; do inject_step "$arm" "$step"; done
        [ "$steps" -gt 2 ] && log "  [dry] ……其余 $((steps - 2)) 步同上……"
        log "  [dry] guest: insmod ple_load.ko workload=1 nr_threads=$VCPU priv_kb=$PRIV_KB"
        log "  [dry] guest: 两次读 completed 与 /proc/stat steal，中间夹注入循环"
        log "  [dry] 关统计器（★ 关机前）→ 读 tsc-offset 尾值 → 关机 → 恢复亲和性"
        return 0
    fi

    mkfifo "$OUT/ser-$arm.in" "$OUT/ser-$arm.out"
    qemu-system-x86_64 -enable-kvm -cpu host -m 2G -smp "$VCPU" \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "console=ttyS0 earlyprintk=serial rdinit=/init" \
        -virtfs "local,path=$SHARE,mount_tag=hostshare,security_model=passthrough,id=hostshare" \
        -display none -monitor none -no-reboot \
        -serial pipe:"$OUT/ser-$arm" > "$OUT/qemu-$arm.err" 2>&1 &
    QEMU_PID=$!
    cat "$OUT/ser-$arm.out" > "$OUT/serial-$arm.log" &
    CAT_PID=$!
    exec 7> "$OUT/ser-$arm.in"

    wait_marker "$OUT/serial-$arm.log" || die "$arm: guest 未就绪（见 $OUT/serial-$arm.log）"
    verify_kvm "$QEMU_PID"
    verify_vermagic "$OUT/serial-$arm.log"
    find_vcpu_tids "$QEMU_PID" || die "$arm: 认不出 vCPU 线程，别跑（§2.2）"
    save_aff
    init_aff "$arm" || die "$arm: 初始亲和性设置失败"

    {
        echo "arm=$arm mode=$mode vcpu=$VCPU priv_kb=$PRIV_KB sample_s=$SAMPLE_S step_ms=$STEP_MS plan_inject_per_thread=$steps"
        echo "cpus_A=${A[*]} cpus_B=${B[*]} smt_of_A=${S[*]}"
        echo "boot_at=$(date -Is)"
        echo "tsc_offset_before=$(tsc_offset_all)"
        echo "stat_tlb_flush_before=$(read_stat tlb_flush) stat_exits_before=$(read_stat exits)"
    } >> "$OUT/$arm.txt"
    sample_tids init

    run_guest "insmod /mnt/shared/ple_load.ko workload=1 nr_threads=$VCPU priv_kb=$PRIV_KB"
    sleep "$WARM_S"
    run_guest "echo S0 \$(cat /sys/module/ple_load/parameters/completed)"
    run_guest "echo T0 \$(busybox awk '/^cpu /{print \$9}' /proc/stat)"
    sleep 1
    setup_events
    start_profile || warn "统计器没开成，本臂只有结果侧数据（机制侧不可判）"

    # 采样窗：注入循环本身就撑满 SAMPLE_S，不需要额外 sleep
    for step in $(seq 1 "$steps"); do
        inject_step "$arm" "$step"
        [ "$DRY" = 1 ] && break
        sleep "$(awk -v ms="$STEP_MS" 'BEGIN{printf "%.3f", ms/1000}')"
    done
    [ "$steps" = 0 ] && sleep "$SAMPLE_S"
    sample_tids mid

    run_guest "echo T1 \$(busybox awk '/^cpu /{print \$9}' /proc/stat)"
    run_guest "echo S1 \$(cat /sys/module/ple_load/parameters/completed)"
    sleep 2                               # 等回显落盘

    stop_profile                          # ★ 关机之前
    collect_events "$arm"

    c0=$(guest_val "$OUT/serial-$arm.log" S0); c1=$(guest_val "$OUT/serial-$arm.log" S1)
    st0=$(guest_val "$OUT/serial-$arm.log" T0); st1=$(guest_val "$OUT/serial-$arm.log" T1)
    [ -n "$c0" ] && [ -n "$c1" ] || die "$arm: 没读到 completed（模块没加载成功？见 $OUT/serial-$arm.log）"
    rate=$(awk -v a="$c0" -v b="$c1" -v d="$SAMPLE_S" 'BEGIN{printf "%.1f",(b-a)/d}')
    o0=$(prof_hit loaded_vmcs_clear); o1=$(prof_hit kvm_arch_vcpu_load)
    log "  completed: S0=$c0 S1=$c1 rate=${rate}/s  steal_delta=$(( ${st1:-0} - ${st0:-0} ))"
    log "  机制: vmcs_clear=${o0:-?} arch_load=${o1:-?} injected_ok=$INJ_OK injected_fail=$INJ_FAIL"
    {
        echo "completed S0=$c0 S1=$c1 rate_per_s=$rate"
        echo "steal jiffies T0=${st0:-NA} T1=${st1:-NA} delta=$(( ${st1:-0} - ${st0:-0} ))"
        echo "injected_ok=$INJ_OK injected_fail=$INJ_FAIL vmcs_clear=${o0:-NA} arch_load=${o1:-NA} pi_load=$(prof_hit vmx_vcpu_pi_load) steal_store=$(prof_hit record_steal_time)"
        echo "tsc_offset_after=$(tsc_offset_all)"
        echo "stat_tlb_flush_after=$(read_stat tlb_flush) stat_exits_after=$(read_stat exits)"
    } >> "$OUT/$arm.txt"

    exec 7>&-
    restore_aff || true                   # ★ 关机之前把线程放回原掩码
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
    kill "$CAT_PID" 2>/dev/null || true
    wait "$CAT_PID" 2>/dev/null || true
    CAT_PID=""
    rm -f "$OUT/ser-$arm.in" "$OUT/ser-$arm.out"
}

# ---------- 前置检查 ----------
preflight() {
    local rc=0 p n_core need
    log "=== preflight ==="

    log "--- 1) 内核与模块 ---"
    lsmod | grep -q '^kvm_intel' || { warn "kvm_intel 未加载"; rc=1; }
    log "  uname -r = $(uname -r)（宿主内核；文档行号基于 6.12.93，见 parameters.md §0）"
    for p in enable_apicv ple_gap; do
        [ -e "/sys/module/kvm_intel/parameters/$p" ] && \
            log "  kvm_intel/$p = $(cat "/sys/module/kvm_intel/parameters/$p") perm=$(stat -c %a "/sys/module/kvm_intel/parameters/$p")" \
            || { warn "缺 /sys/module/kvm_intel/parameters/$p"; rc=1; }
    done
    log "  ★ 本臂不改 kvm_intel 参数；PLE 与迁移的纠缠只在 --with-ple 下观测（bench-migrate.md §4.4）"

    log "--- 2) 机制侧观测出口 ---"
    for p in $PROF_FUNCS; do
        traceable "$p" && log "  $p 可跟踪" || warn "$p 不在 available_filter_functions —— 该臂的机制侧计数缺一角（§2.4）"
    done
    profiler_ok && log "  function profiler 可用" || { warn "无 function profiler，主判据失效"; rc=1; }
    traceable kvm_vcpu_flush_tlb_all && log "  kvm_vcpu_flush_tlb_all 可跟踪" \
        || log "  kvm_vcpu_flush_tlb_all 不可跟踪（已内联）→ TLB 冲刷只能走 debugfs tlb_flush（§2.4）"
    [ -d $TR/events/kvm/kvm_vcpu_wakeup ] && log "  kvm:kvm_vcpu_wakeup 存在" || warn "缺 kvm:kvm_vcpu_wakeup"

    log "--- 3) debugfs 统计布局（§2.5） ---"
    if p=$(kvm_stat_file tlb_flush); then
        log "  tlb_flush → $p perm=$(stat -c %a "$p")（写 0 可清零；cumulative 才是 0644）"
    else
        warn "找不到 tlb_flush 统计文件，先跑一台 VM 再看 $DBK"
    fi
    if ls $DBK/*/vcpu0/tsc-offset >/dev/null 2>&1; then
        log "  per-vCPU debugfs 布局存在（<pid>-<fd>/vcpu0/tsc-offset）"
    else
        log "  当前无 VM，per-vCPU 布局尚不可见 —— 开跑后再判 tsc-offset 阴性对照是否可用"
    fi

    log "--- 4) 核与拓扑 ---"
    need=$((VCPU * 2))
    n_core=$(pick_cores "$need" >/dev/null 2>&1 && echo OK || echo FAIL)
    log "  nproc=$(nproc) 需要 $need 个 node$NODE 上的不同物理核 → $n_core"
    [ "$n_core" = OK ] || { warn "node$NODE 上凑不出 $need 个不同物理核；调小 VCPU 或改 NODE"; rc=1; }
    [ "$n_core" = OK ] && plan_cpus
    p=$(cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list 2>/dev/null)
    log "  cpu0 的兄弟=$p（M4 依赖 SMT；若所有核兄弟都只有自身，M4 与 M3 等价）"
    log "  所选核当前负载：$(uptime | sed 's/.*load average/load/')"

    log "--- 5) 产物 ---"
    for p in "$KERNEL" "$INITRD" "$KO_DIR/ple_load.ko" "$SHARE/ple_load.ko"; do
        [ -f "$p" ] || { warn "缺 $p"; rc=1; }
    done
    [ -f "$SHARE/ple_load.ko" ] && [ -f "$KO_DIR/ple_load.ko" ] && \
        cmp -s "$KO_DIR/ple_load.ko" "$SHARE/ple_load.ko" \
        && log "  共享区的 ple_load.ko 与本地构建一致" \
        || warn "共享区的 ple_load.ko 缺失或与 $KO_DIR 不一致，guest 会加载旧版本（没有 workload=1）"
    modinfo -p "$KO_DIR/ple_load.ko" 2>/dev/null | grep -q workload \
        && log "  模块支持 workload/priv_kb（E3 需要）" || warn "模块里没有 workload 参数，先重建 ple_load.ko"
    command -v taskset >/dev/null && log "  taskset=$(command -v taskset)" || { warn "缺 taskset（util-linux）"; rc=1; }
    command -v gawk >/dev/null && log "  gawk 可用" || warn "缺 gawk（本脚本只用 POSIX awk，影响小）"

    log "--- 6) ftrace 残留 ---"
    p=$(cat $TR/current_tracer 2>/dev/null)
    [ -z "$p" ] || [ "$p" = nop ] || warn "current_tracer=$p，上一轮没清（AGENTS.md 陷阱 9）"
    [ "$(cat $TR/function_profile_enabled 2>/dev/null)" = 0 ] || warn "function_profile_enabled 还开着"

    log "--- 7) 独占性 ---"
    p=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c '/dev/kvm')
    [ "${p:-0}" = 0 ] || warn "已有 $p 个 /dev/kvm fd 在跑：debugfs 统计若是全局聚合会被别人污染（§2.5）"

    if [ "$rc" = 0 ]; then log "== 前置检查通过 =="; else log "== 前置检查有未通过项，先修再跑 =="; fi
    return $rc
}

# ---------- 参数解析 ----------
while [ $# -gt 0 ]; do
    case $1 in
        --preflight)   ONLY_PREFLIGHT=1; shift ;;
        --dry-run)     DRY=1; shift ;;
        --with-ple)    WITH_PLE=1; shift ;;
        --arm)         ARMS+=("$2"); shift 2 ;;
        --all)         ARMS=("${ALL_ARMS[@]}"); shift ;;
        --repeat)      REPEAT="$2"; shift 2 ;;
        --step-ms)     STEP_MS="$2"; shift 2 ;;
        --sample-s)    SAMPLE_S="$2"; shift 2 ;;
        --vcpu)        VCPU="$2"; shift 2 ;;
        --priv-kb)     PRIV_KB="$2"; shift 2 ;;
        --node)        NODE="$2"; shift 2 ;;
        --kernel)      KERNEL="$2"; shift 2 ;;
        -h|--help)     usage ;;
        *)             warn "未知参数 $1"; usage ;;
    esac
done
[ ${#ARMS[@]} -gt 0 ] || ARMS=(M2)
for a in "${ARMS[@]}"; do arm_mode "$a" >/dev/null || die "未知实验臂 $a（可选 ${ALL_ARMS[*]}）"; done
case $STEP_MS in ''|*[!0-9]*) die "--step-ms 要正整数";; esac
[ "$STEP_MS" -ge 10 ] || die "--step-ms 太小（<10ms）会让注入自身成为瓶颈"
[ "$VCPU" -ge 2 ] || die "--vcpu 至少 2"

if [ "$ONLY_PREFLIGHT" = 1 ]; then preflight; exit $?; fi
[ "$(id -u)" = 0 ] || [ "$DRY" = 1 ] || die "需要 root（taskset / tracefs / debugfs）"
[ "$DRY" = 1 ] || mkdir -p "$OUT"

for a in "${ARMS[@]}"; do
    log "=== $a: mode=$(arm_mode "$a") step_ms=$STEP_MS sample_s=$SAMPLE_S ==="
    for r in $(seq 1 "$REPEAT"); do
        log "-- $a repeat $r/$REPEAT"
        boot_and_measure "$a"
    done
done

log "输出目录: ${OUT}$([ "$DRY" = 1 ] && echo '（--dry-run：未执行任何改动动作）')"
