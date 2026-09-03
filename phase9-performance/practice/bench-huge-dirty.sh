#!/usr/bin/env bash
# E2 · 大页 × 脏页日志驱动器 —— 设计文档见 bench-huge-dirty.md，机制见 ../annotations.md §2
#
# 一次测量 = 一次完整 boot：
#   1) pml 是 0444，改它必须重载 kvm_intel；
#   2) 脏日志开启那一刻就把已有大页拆掉（x86.c:13251/13257），关掉又会回收
#      （x86.c:13241）—— 同一台 VM 里前后两段的粒度状态互相污染。
#
# 脏日志窗口 = 一次真实的 precopy 迁移到 exec:cat >/dev/null，采样结束立刻
#   migrate_cancel。走 QMP 而不是 HMP：URI 里的空格与 > 不被二次切分。
#
# 用法：
#   ./bench-huge-dirty.sh --preflight                 只做 bench-huge-dirty.md §2 的检查（只读）
#   ./bench-huge-dirty.sh --arm H3 --dry-run          打印将执行的动作，不碰系统
#   sudo ./bench-huge-dirty.sh --arm H3 --repeat 3
#   sudo ./bench-huge-dirty.sh --all --repeat 5 --allow-reload
set -u
cd "$(dirname "$0")"

# ---------- 路径与常量 ----------
KERNEL=/root/code/linux-6.12.93/arch/x86_64/boot/bzImage   # 与 boot-vm.sh:178 同源
INITRD=../../scripts/images/initramfs.img
SHARE=../../scripts/shared
P_K=/sys/module/kvm/parameters
P_I=/sys/module/kvm_intel/parameters
TR=/sys/kernel/tracing
HP2M=/sys/kernel/mm/hugepages/hugepages-2048kB
THPE=/sys/kernel/mm/transparent_hugepage/enabled
HUGEDIR=/dev/hugepages

MEM_G=2                 # guest 内存 GiB
VCPU=4
CNT=64                  # 每轮 dd 写入 MiB
ROUNDS=5
WARM_S=3
BW=12500000             # max-bandwidth B/s ≈ 100 Mbps，故意压到远低于脏页产生速率
BUF_KB=8192             # 每 CPU ring buffer

TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/huge-dirty-$TS
DRY=0; ALLOW_RELOAD=0; ONLY_PREFLIGHT=0
ARMS=(); REPEAT=1; EAGER=""
ALL_ARMS=(H0 H1 H2 H3 H4 H5)

log()  { printf '%s\n' "$*"; }
warn() { printf '!! %s\n' "$*" >&2; }
die()  { warn "$*"; exit 1; }

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

# ---------- 实验臂：backend dirty pretouch pml ----------
arm_spec() {
    case $1 in
        H0) echo "hugetlb off none 1" ;;
        H1) echo "anon off none 1" ;;
        H2) echo "hugetlb on none 1" ;;
        H3) echo "anon on none 1" ;;
        H4) echo "anon on none 0" ;;   # pml=0，需重载 kvm_intel
        H5) echo "hugetlb on pre 1" ;;  # 先建满 2M 再开日志
        *)  return 1 ;;
    esac
}
arm_field() { arm_spec "$1" | awk -v n="$2" '{print $n}'; }
arm_want_pml() { [ "$(arm_field "$1" 4)" = 1 ]; }

# ---------- 状态与恢复 ----------
ORIG_PML=""; ORIG_THPE=""; ORIG_HPNR=""; ORIG_EAGER=""
QEMU_PID=""; CAT_PID=""; MON_SOCK=""; HP_RESERVED=0; THP_SET=0; EAGER_SET=0

reload_kvm_intel() {                       # $1 = "pml=0" 之类
    if [ "$DRY" = 1 ]; then
        log "  [dry] modprobe -r kvm_intel && modprobe kvm_intel $1"; return 0
    fi
    local holders
    holders=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c '/dev/kvm')
    [ "$holders" = 0 ] || { warn "/dev/kvm 被 $holders 个 fd 持有，拒绝重载"; return 1; }
    modprobe -r kvm_intel || { warn "modprobe -r kvm_intel 失败（有 VM 在跑？）"; return 1; }
    # shellcheck disable=SC2086
    modprobe kvm_intel $1 || { warn "modprobe kvm_intel $1 失败"; return 1; }
    log "  已重载 kvm_intel: $1"
}

save_state() {
    ORIG_PML=$(cat "$P_I/pml" 2>/dev/null)
    ORIG_EAGER=$(cat "$P_K/eager_page_split" 2>/dev/null)
    ORIG_HPNR=$(cat "$HP2M/nr_hugepages" 2>/dev/null)
    ORIG_THPE=$(sed -n 's/.*\[\(.*\)\].*/\1/p' "$THPE" 2>/dev/null)
}

restore_state() {
    if [ "$DRY" = 1 ]; then return 0; fi
    [ "$THP_SET" = 1 ] && [ -n "$ORIG_THPE" ] && { echo "$ORIG_THPE" > "$THPE" 2>/dev/null || true; log "  已恢复 THP=$ORIG_THPE"; }
    [ "$HP_RESERVED" = 1 ] && { echo "$ORIG_HPNR" > "$HP2M/nr_hugepages" 2>/dev/null || true; log "  已恢复 nr_hugepages=$ORIG_HPNR"; }
    [ "$EAGER_SET" = 1 ] && [ -n "$ORIG_EAGER" ] && { echo "$ORIG_EAGER" > "$P_K/eager_page_split" 2>/dev/null || true; log "  已恢复 eager_page_split=$ORIG_EAGER"; }
    local now_want
    now_want=$(cat "$P_I/pml" 2>/dev/null)
    [ -n "$ORIG_PML" ] && [ "$now_want" != "$ORIG_PML" ] && {
        log "== 恢复 pml=$ORIG_PML =="
        reload_kvm_intel "pml=$ORIG_PML" || \
            warn "自动恢复失败，请手工执行：modprobe -r kvm_intel && modprobe kvm_intel pml=$ORIG_PML"
    }
    return 0
}

cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$CAT_PID" ]  && kill "$CAT_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    [ "$DRY" = 1 ] && return 0
    stop_profile || true
    clear_ftrace || true
    restore_state || true
    [ -n "$MON_SOCK" ] && rm -f "$MON_SOCK"
    return 0
}
trap cleanup EXIT INT TERM

# ---------- ftrace：ring buffer 计数 + 函数统计器 ----------
clear_ftrace() {
    [ -e $TR/current_tracer ] || return 0
    if [ "$DRY" = 1 ]; then log "  [dry] 清 ftrace: current_tracer=nop、set_ftrace_filter、set_event"; return 0; fi
    echo nop > $TR/current_tracer 2>/dev/null || true
    echo > $TR/set_ftrace_filter 2>/dev/null || true
    echo > $TR/set_event 2>/dev/null || true
}
# 模块符号在 available_filter_functions 里带 [kvm] 后缀，grep -x 会误判
traceable() { grep -qE "^$1( |\[|$)" $TR/available_filter_functions 2>/dev/null; }
# stats 里 "overrun:" 和 "commit overrun:" 每 CPU 都有这一行，所以只能加和数值
overrun_total() { grep -h 'overrun:' "$1" 2>/dev/null | awk '{s+=$NF} END{print s+0}'; }
entries_total() { grep -h '^entries:' "$1" 2>/dev/null | awk '{s+=$NF} END{print s+0}'; }
profiler_ok() { [ -w $TR/function_profile_enabled ] && [ -d $TR/trace_stat ]; }

PROF_FUNCS="mark_page_dirty_in_slot kvm_mmu_slot_try_split_huge_pages kvm_mmu_slot_remove_write_access"

start_profile() {
    local f list="" bad=""
    for f in $PROF_FUNCS; do traceable "$f" && list="$list $f"; done
    [ -n "$list" ] || { warn "三个建表/拆页函数都不可跟踪，机制侧没有出口（bench-huge-dirty.md §4.1）"; return 1; }
    if [ "$DRY" = 1 ]; then
        log "  [dry] function_profile_enabled=0 → 逐个 echo 名字 >> set_ftrace_filter（$list）→ function_profile_enabled=1"
        log "  [dry] current_tracer 保持 nop；名字逐个写入，匹配不上的逐个告警（静默截断的风险见 bench-migrate.md §4.2.1）"
        return 0
    fi
    if ! profiler_ok; then
        warn "本机没有 function profiler（CONFIG_FUNCTION_PROFILER=n），退回 current_tracer=function 会淹掉 ring buffer"
        return 1
    fi
    # 必须逐个写：一次写好几个名字时，走到第一个匹配不上的名字就中止本次 write，
    # 后面的名字连带丢失；若它在最前面，filter 停在"全部函数都开"—— 统计器照样
    # 给出看着合理的数字，其实已经把整机所有函数都统计进去了（bench-migrate.md §4.2.1）。
    echo 0 > $TR/function_profile_enabled 2>/dev/null || true
    echo > $TR/set_ftrace_filter 2>/dev/null || return 1
    for f in $list; do
        echo "$f" >> $TR/set_ftrace_filter 2>/dev/null || bad="$bad $f"
    done
    [ -n "$bad" ] && warn "这几个函数写不进 set_ftrace_filter：$bad（bench-huge-dirty.md §4.2）"
    echo 1 > $TR/function_profile_enabled || { warn "function_profile_enabled 打不开"; return 1; }
    log "  统计器已开：${list# }${bad:+（未生效：$bad）}"
}
stop_profile() {
    local f
    [ "$DRY" = 1 ] && return 0
    [ -e $TR/function_profile_enabled ] || return 0
    [ "$(cat $TR/function_profile_enabled)" = 1 ] || return 0
    echo 0 > $TR/function_profile_enabled 2>/dev/null || true
    grep -hE "^ *($(echo "$PROF_FUNCS" | tr ' ' '|')) +[0-9]" $TR/trace_stat/function* 2>/dev/null \
        | awk '{h[$1]+=$2} END{for(k in h) printf "  %-38s hit=%d\n", k, h[k]}' | sort > "$OUT/prof-latest.txt"
    # 没命中的函数在统计表里根本没有行，"读不到" 与 "命中 0" 必须区分开显式补 0
    for f in $PROF_FUNCS; do
        grep -q "^[[:space:]]*$f " "$OUT/prof-latest.txt" || echo "  $f hit=0" >> "$OUT/prof-latest.txt"
    done
    sort -o "$OUT/prof-latest.txt" "$OUT/prof-latest.txt" 2>/dev/null || true
    cat "$OUT/prof-latest.txt" 2>/dev/null || true
    echo > $TR/set_ftrace_filter 2>/dev/null || true
    return 0
}

setup_trace() {                             # ring buffer 只收 tracepoint 事件
    if [ "$DRY" = 1 ]; then
        log "  [dry] buffer_size_kb=$BUF_KB; set_event=kvm:kvm_page_fault kvm:kvm_pml_full"
        return 0
    fi
    clear_ftrace
    echo $BUF_KB > $TR/buffer_size_kb 2>/dev/null || warn "buffer_size_kb 设置失败"
    : > $TR/set_event
    [ -d $TR/events/kvm/kvm_page_fault ] && echo kvm:kvm_page_fault >> $TR/set_event 2>/dev/null \
        || warn "缺 kvm:kvm_page_fault，粒度无从推断"
    [ -d $TR/events/kvm/kvm_pml_full ] && echo kvm:kvm_pml_full >> $TR/set_event 2>/dev/null \
        || warn "缺 kvm:kvm_pml_full（pml=0 时本来就不该有）"
    echo 1 > $TR/tracing_on
}
reset_trace() { [ "$DRY" = 1 ] || echo > $TR/trace 2>/dev/null || true; }

collect_trace() {                           # $1=arm
    local a=$1 nf nb n_new n_wp n_pml
    [ "$DRY" = 1 ] && return 0
    cp $TR/trace "$OUT/trace-$a.txt" 2>/dev/null || true
    cat $TR/per_cpu/cpu*/stats > "$OUT/bufstats-$a.txt" 2>/dev/null || true
    # EPT 路径上 error_code 是原始 exit qualification：bits5:3 = 当时表项的 RWX
    read -r nf nb n_new n_wp <<< "$(gawk '
        /kvm_page_fault/ {
            if (match($0, /address 0x[0-9a-f]+/)) ad = strtonum(substr($0, RSTART+8, RLENGTH-8)); else next
            ec = 0
            if (match($0, /error_code 0x[0-9a-f]+/)) ec = strtonum(substr($0, RSTART+11, RLENGTH-11))
            total++
            blk[int(ad / 2097152)] = 1
            if (and(ec, 56) == 0) new++; else if (and(ec, 16) == 0) wp++
        }
        END { n=0; for (k in blk) n++; printf "%d %d %d %d", total, n, new, wp }
    ' "$OUT/trace-$a.txt")"
    n_pml=$(grep -c 'kvm_pml_full' "$OUT/trace-$a.txt" 2>/dev/null) || true
    log "  trace: page_fault=$nf 覆盖2M块=$nb 建表型=${n_new:-0} 写保护型=${n_wp:-0} pml_full=${n_pml:-0}"
    log "  粒度推断: 每块平均 $(awk -v f="${nf:-0}" -v b="${nb:-0}" 'BEGIN{printf "%.1f", (b>0? f/b : 0)}') 次缺页（≈1 → 2M；≈512 → 4K）"
    echo "faults=$nf blocks2m=$nb faults_new=${n_new:-0} faults_wp=${n_wp:-0} pml_full=${n_pml:-0}" >> "$OUT/$a.txt"
    ovr=$(overrun_total "$OUT/bufstats-$a.txt")
    log "  buffer: entries=$(entries_total "$OUT/bufstats-$a.txt") overrun=$ovr"
    [ "${ovr:-0}" = 0 ] || \
        warn "ring buffer 有 $ovr 次 overrun，以上计数偏低（../measurement.md §4(c)）"
    clear_ftrace
}

# ---------- 大页 / THP ----------
reserve_huge() {
    local need=$((MEM_G * 512 + 64)) got
    if [ "$DRY" = 1 ]; then
        log "  [dry] echo $need > $HP2M/nr_hugepages 并回读校验"; return 0
    fi
    echo "$need" > "$HP2M/nr_hugepages" || die "写 $HP2M/nr_hugepages 失败"
    HP_RESERVED=1
    got=$(cat "$HP2M/nr_hugepages")
    [ "$got" -ge $((MEM_G * 512)) ] || die "只预留到 $got 页（需 $((MEM_G*512))），后端凑不齐就别跑了（bench-huge-dirty.md §6.2）"
    log "  预留 2M 大页：$got 页（请求 $need）"
}
set_thp_never() {
    if [ "$DRY" = 1 ]; then log "  [dry] echo never > $THPE"; return 0; fi
    echo never > "$THPE" || die "写 $THPE 失败"
    THP_SET=1
    log "  THP=$(cat $THPE)"
}

# ---------- QMP ----------
qmp() {                                     # $1 = 要落盘的文件名，其余 = JSON 命令
    local dst=$1; shift
    if [ "$DRY" = 1 ]; then
        while [ $# -gt 0 ]; do log "  [dry] QMP $1"; shift; done; return 0
    fi
    [ -S "$MON_SOCK" ] || { warn "$MON_SOCK 不是 socket"; return 1; }
    { printf '%s\n' '{"execute":"qmp_capabilities"}' "$@"; sleep 1; } \
        | timeout 15 socat -t 3 - "UNIX-CONNECT:$MON_SOCK" >> "$dst" 2>&1
}
start_migration() {                          # $1=arm
    local f="$OUT/qmp-$1-migrate.txt"
    : > "$f"
    qmp "$f" '{"execute":"migrate-set-parameters","arguments":{"max-bandwidth":'"$BW"'}}' || return 1
    qmp "$f" '{"execute":"migrate","arguments":{"uri":"exec:cat >/dev/null"}}' || return 1
    log "  migrate 已发起（max-bandwidth=$BW B/s），原文见 $f"
}
stop_migration() {
    local f="$OUT/qmp-$1-cancel.txt" st
    : > "$f"
    qmp "$f" '{"execute":"migrate_cancel"}' || true
    qmp "$f" '{"execute":"query-migrate"}' || true
    st=$(grep -o '"status":"[a-z-]*"' "$f" 2>/dev/null | tail -1)
    log "  迁移终态 ${st:-未取到}（出现 completed 说明提前收敛，本轮标可疑，bench-huge-dirty.md §6.4）"
    echo "migrate_final=${st:-unknown}" >> "$OUT/$1.txt"
    case $st in *completed*) warn "$1: 迁移已完成而非取消，采样窗可能被腰斩" ;; esac
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
    local end=$((SECONDS + ${3:-120}))
    while [ $SECONDS -lt $end ]; do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

verify_kvm() {
    local n
    n=$(ls -l /proc/$1/fd 2>/dev/null | grep -c '/dev/kvm') || true
    [ "${n:-0}" -gt 0 ] || die "PID $1 未持有 /dev/kvm —— 走的是 TCG（AGENTS.md 陷阱 7）"
    log "  KVM 确认：$n 个 /dev/kvm fd"
}
snap_backing() {                            # $1=arm $2=pid
    [ "$DRY" = 1 ] && return 0
    {
        echo "# backing 快照 $(date -Is)"
        grep -E '^(AnonHugePages|FileHugetlb|ShmemPmdMapped)' /proc/meminfo
        awk '/AnonHugePages/{s+=$2} END{printf "smaps AnonHugePages 合计 %d kB\n", s}' "/proc/$2/smaps" 2>/dev/null
        echo "nr_hugepages=$(cat $HP2M/nr_hugepages) free=$(cat $HP2M/free_hugepages) rsvd=$(cat $HP2M/resv_hugepages 2>/dev/null)"
    } > "$OUT/backing-$1.txt" 2>/dev/null || true
    log "  backing 快照 → $OUT/backing-$1.txt"
}

# ---------- 单臂一轮 ----------
boot_and_measure() {                        # $1=arm
    local arm=$1 backend dirty pretouch obj ser
    backend=$(arm_field "$arm" 1); dirty=$(arm_field "$arm" 2)
    pretouch=$(arm_field "$arm" 3)
    ser="$OUT/ser-$arm"
    log "-- $arm: backend=$backend dirty=$dirty pretouch=$pretouch pml=$(cat $P_I/pml 2>/dev/null) eager=$(cat $P_K/eager_page_split 2>/dev/null)"

    if [ "$backend" = hugetlb ]; then
        reserve_huge
        obj="memory-backend-file,id=ram0,size=${MEM_G}G,mem-path=$HUGEDIR,prealloc=on"
    else
        set_thp_never
        obj="memory-backend-ram,id=ram0,size=${MEM_G}G"
    fi

    if [ "$DRY" = 1 ]; then
        log "  [dry] qemu-system-x86_64 -enable-kvm -cpu host -m ${MEM_G}G -smp $VCPU \\"
        log "        -machine memory-backend=ram0 -object $obj \\"
        log "        -kernel $KERNEL -initrd $INITRD -append 'console=ttyS0 ...' \\"
        log "        -qmp unix:$OUT/qmp-$arm,server,nowait -serial pipe:$ser"
        [ "$pretouch" = pre ] && log "  [dry] guest: 先 dd 铺满 ${MEM_G}G 建立 2M 映射，再开日志"
        [ "$dirty" = on ] && log "  [dry] QMP migrate-set-parameters + migrate exec:cat >/dev/null"
        log "  [dry] 开统计器 + kvm:kvm_page_fault/kvm_pml_full，然后清 trace"
        log "  [dry] guest: $ROUNDS 轮 dd if=/dev/zero of=/e2 bs=1M count=$CNT，每轮回显 RN <i> <epoch>"
        [ "$dirty" = on ] && log "  [dry] QMP migrate_cancel + query-migrate"
        log "  [dry] 收 trace → 统计缺页/块数/两类缺页 → 关机"
        return 0
    fi

    MON_SOCK="$OUT/qmp-$arm"
    mkfifo "$ser.in" "$ser.out"
    qemu-system-x86_64 -enable-kvm -cpu host -m "${MEM_G}G" -smp "$VCPU" \
        -machine memory-backend=ram0 -object "$obj" \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "console=ttyS0 earlyprintk=serial rdinit=/init" \
        -virtfs "local,path=$SHARE,mount_tag=hostshare,security_model=passthrough,id=hostshare" \
        -display none -monitor none -no-reboot \
        -qmp "unix:$MON_SOCK,server,nowait" \
        -serial pipe:"$ser" > "$OUT/qemu-$arm.err" 2>&1 &
    QEMU_PID=$!
    cat "$ser.out" > "$OUT/serial-$arm.log" &
    CAT_PID=$!
    exec 7> "$ser.in"

    wait_marker "$OUT/serial-$arm.log" || die "$arm: guest 未就绪（见 $OUT/serial-$arm.log）"
    verify_kvm "$QEMU_PID"
    snap_backing "$arm" "$QEMU_PID"
    {
        echo "arm=$arm backend=$backend dirty=$dirty pretouch=$pretouch"
        echo "pml=$(cat $P_I/pml) eager_page_split=$(cat $P_K/eager_page_split) nx_huge_pages=$(cat $P_K/nx_huge_pages)"
        echo "mem=${MEM_G}G vcpu=$VCPU cnt=${CNT}MiB rounds=$ROUNDS bw=$BW"
        echo "boot_at=$(date -Is)"
    } >> "$OUT/$arm.txt"

    # H5：先把整块 RAM 铺满，让 2M 映射真实建立，再开日志
    if [ "$pretouch" = pre ]; then
        log "  预铺 ${MEM_G}G 建立大页映射…"
        run_guest "dd if=/dev/zero of=/pre bs=1M count=$((MEM_G * 1024)) 2>/dev/null; echo PRE DONE"
        wait_for "$OUT/serial-$arm.log" '^PRE DONE' 600 || die "$arm: 预铺超时"
        snap_backing "pre-$arm" "$QEMU_PID"
    fi

    if [ "$dirty" = on ]; then start_migration "$arm" || die "$arm: migrate 发起失败，脏日志没开成"; fi

    setup_trace
    start_profile
    reset_trace
    sleep "$WARM_S"

    local rounds
    rounds=$(seq 1 "$ROUNDS")
    run_guest "for i in $rounds; do dd if=/dev/zero of=/e2 bs=1M count=$CNT 2>/dev/null; echo RN \$i \$(date +%s); done"
    wait_for "$OUT/serial-$arm.log" "^RN $ROUNDS " $((ROUNDS * 120 + 60)) \
        || die "$arm: 只跑完 $(grep -c '^RN ' "$OUT/serial-$arm.log")/$ROUNDS 轮（见 $OUT/serial-$arm.log）"

    stop_profile
    collect_trace "$arm"
    snap_backing "end-$arm" "$QEMU_PID"
    [ "$dirty" = on ] && stop_migration "$arm"

    exec 7>&-
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
    kill "$CAT_PID" 2>/dev/null || true
    wait "$CAT_PID" 2>/dev/null || true
    CAT_PID=""
    rm -f "$ser.in" "$ser.out" "$MON_SOCK"
    MON_SOCK=""
    # 每臂收尾都回落到不预留大页，避免上一轮的物理占用影响下一轮
    if [ "$backend" = hugetlb ] && [ "$HP_RESERVED" = 1 ]; then
        echo "$ORIG_HPNR" > "$HP2M/nr_hugepages" 2>/dev/null || true
        HP_RESERVED=0
    fi
}

# ---------- 前置检查 ----------
preflight() {
    local rc=0 p need
    log "=== preflight ==="

    log "--- 1) 参数实存性与权限（内核版本漂移检查）---"
    lsmod | grep -q '^kvm_intel' || { warn "kvm_intel 未加载"; rc=1; }
    for p in "$P_I/pml" "$P_I/ept" "$P_K/eager_page_split" "$P_K/nx_huge_pages"; do
        if [ -e "$p" ]; then
            log "  $(printf '%-46s' "${p##*/}") = $(cat "$p") perm=$(stat -c %a "$p")"
        else
            warn "$p 不存在 —— 与 parameters.md 记的 6.12.93 不一致，先核对内核版本"
            rc=1
        fi
    done

    log "--- 2) 后端 ---"
    if [ -d "$HUGEDIR" ]; then
        mount | grep -q "type hugetlbfs" && log "  $HUGEDIR 已挂 hugetlbfs" \
            || { warn "$HUGEDIR 存在但没挂 hugetlbfs：mount -t hugetlbfs nodev $HUGEDIR"; rc=1; }
    else
        warn "$HUGEDIR 不存在：mount -t hugetlbfs nodev $HUGEDIR"; rc=1
    fi
    need=$((MEM_G * 512))
    log "  当前 nr_hugepages=$(cat $HP2M/nr_hugepages)，本实验要 ${need}+ 页（${MEM_G}G）"
    log "  THP=$(cat $THPE 2>/dev/null)（anon 臂会临时改成 never 并在退出时恢复）"
    p=$(awk '/^MemAvailable/{print int($2/1048576)}' /proc/meminfo)
    log "  MemAvailable=${p}G —— 预留 ${MEM_G}G 大页要真的凑得出来，碎片化会静默失败"

    log "--- 3) 观测出口 ---"
    for p in $PROF_FUNCS; do
        traceable "$p" && log "  $p 可跟踪" || warn "$p 不在 available_filter_functions"
    done
    profiler_ok && log "  function profiler 可用（CONFIG_FUNCTION_PROFILER=y）" \
        || warn "无 function profiler → 机制侧只能退回 current_tracer=function（bench-huge-dirty.md §4.2）"
    [ -d $TR/events/kvm/kvm_page_fault ] && log "  kvm:kvm_page_fault 存在" || { warn "缺 kvm:kvm_page_fault"; rc=1; }
    [ -d $TR/events/kvm/kvm_pml_full ] && log "  kvm:kvm_pml_full 存在" || warn "缺 kvm:kvm_pml_full"
    log "  ★ 6.12.93 没有任何 tracepoint 会直接报出映射级别，也没有 kvm:kvm_dirty_log（§2.1/§4.1）"

    log "--- 4) 迁移手段 ---"
    command -v socat >/dev/null && log "  socat=$(command -v socat)（QMP 走 unix socket）" || { warn "缺 socat"; rc=1; }
    command -v gawk >/dev/null && log "  gawk 可用（strtonum 解 trace 里的十六进制）" || warn "缺 gawk，粒度推断要改写"
    local qv
    qv=$(qemu-system-x86_64 --version 2>/dev/null | head -1)
    [ -n "$qv" ] || { warn "qemu-system-x86_64 不在 PATH"; rc=1; }
    log "  运行期 QEMU: ${qv:-无}；本文引用行号基线 qemu-10.1.0-rc2"
    case $qv in *10.1*) ;; "") ;; *) warn "运行期与基线版本不同，§5 的 exec:/QMP 语义要在本机复核后再采信（AGENTS.md 要求写明版本）" ;; esac
    # 只探测选项能否被解析，不会真起 VM
    timeout 10 qemu-system-x86_64 -machine memory-backend=bogus -m 256M -display none -nographic 2>&1 \
        | grep -q "Memory backend 'bogus' not found" \
        && log "  -machine memory-backend= 可用" || { warn "运行期 QEMU 不认 -machine memory-backend="; rc=1; }
    timeout 10 qemu-system-x86_64 -qmp unix:/nonexistent-dir/x,server,nowait -m 256M -display none 2>&1 \
        | grep -qi "qmp" && log "  -qmp unix: 可用" || warn "-qmp 探测失败，检查 QEMU"

    log "--- 5) 产物 ---"
    for p in "$KERNEL" "$INITRD"; do
        [ -f "$p" ] || { warn "缺 $p"; rc=1; }
    done
    log "  uname -r = $(uname -r)（宿主内核；文档行号基于 6.12.93，见 parameters.md §0）"

    log "--- 6) ftrace 残留 ---"
    p=$(cat $TR/current_tracer 2>/dev/null)
    [ -z "$p" ] || [ "$p" = nop ] || warn "current_tracer=$p，上一轮没清（AGENTS.md 陷阱 9）"
    [ "$(cat $TR/function_profile_enabled 2>/dev/null)" = 0 ] || warn "function_profile_enabled 还开着"

    log "--- 7) 其它 VM ---"
    p=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c '/dev/kvm')
    [ "${p:-0}" = 0 ] || warn "已有 $p 个 /dev/kvm fd 在跑：H4（pml=0）需要重载 kvm_intel 会被拒绝"

    if [ "$rc" = 0 ]; then log "== 前置检查通过 =="; else log "== 前置检查有未通过项，先修再跑 =="; fi
    return $rc
}

# ---------- 参数解析 ----------
while [ $# -gt 0 ]; do
    case $1 in
        --preflight)    ONLY_PREFLIGHT=1; shift ;;
        --dry-run)      DRY=1; shift ;;
        --allow-reload) ALLOW_RELOAD=1; shift ;;
        --arm)          ARMS+=("$2"); shift 2 ;;
        --all)          ARMS=("${ALL_ARMS[@]}"); shift ;;
        --repeat)       REPEAT="$2"; shift 2 ;;
        --rounds)       ROUNDS="$2"; shift 2 ;;
        --cnt)          CNT="$2"; shift 2 ;;
        --mem)          MEM_G="$2"; shift 2 ;;
        --eager)        EAGER="$2"; shift 2 ;;
        --kernel)       KERNEL="$2"; shift 2 ;;
        -h|--help)      usage ;;
        *)              warn "未知参数 $1"; usage ;;
    esac
done
[ ${#ARMS[@]} -gt 0 ] || ARMS=(H3)
for a in "${ARMS[@]}"; do arm_spec "$a" >/dev/null || die "未知实验臂 $a（可选 ${ALL_ARMS[*]}）"; done

if [ "$ONLY_PREFLIGHT" = 1 ]; then preflight; exit $?; fi
[ "$(id -u)" = 0 ] || [ "$DRY" = 1 ] || die "需要 root（大页预留 / THP / 模块重载 / tracefs）"
[ "$DRY" = 1 ] || mkdir -p "$OUT"
[ "$DRY" = 1 ] || save_state

# eager_page_split 是 0644，只影响"开日志那一刻拆不拆"，对 {H2,H3,H4,H5} 有意义
if [ -n "$EAGER" ]; then
    [ "$EAGER" = on ] || [ "$EAGER" = off ] || die "--eager 只接受 on|off"
    if [ "$DRY" = 1 ]; then log "[dry] echo $EAGER > $P_K/eager_page_split"; else
        echo "$EAGER" > "$P_K/eager_page_split" || die "写 eager_page_split 失败"
        EAGER_SET=1
    fi
fi

for a in "${ARMS[@]}"; do
    want=$(arm_want_pml "$a" && echo Y || echo N)
    have=$(cat "$P_I/pml" 2>/dev/null)
    log "=== $a: backend=$(arm_field "$a" 1) dirty=$(arm_field "$a" 2) pretouch=$(arm_field "$a" 3) pml 期望=$want 实际=$have ==="
    if [ "$want" = Y ] && [ "$have" != Y ]; then
        [ "$ALLOW_RELOAD" = 1 ] || { warn "$a 需要 pml=1 而当前是 $have，未给 --allow-reload，跳过"; continue; }
        reload_kvm_intel "pml=1" || { warn "$a 重载失败，跳过"; continue; }
    elif [ "$want" = N ] && [ "$have" != N ]; then
        [ "$ALLOW_RELOAD" = 1 ] || { warn "$a 需要 pml=0 而当前是 $have，未给 --allow-reload，跳过"; continue; }
        reload_kvm_intel "pml=0" || { warn "$a 重载失败，跳过"; continue; }
    fi
    for r in $(seq 1 "$REPEAT"); do
        log "-- $a repeat $r/$REPEAT"
        boot_and_measure "$a"
    done
done

log "输出目录: ${OUT}$([ "$DRY" = 1 ] && echo '（--dry-run：未执行任何改动动作）')"
