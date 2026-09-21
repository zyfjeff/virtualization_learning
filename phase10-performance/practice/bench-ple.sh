#!/usr/bin/env bash
# E1 · PLE 超卖实验驱动器 —— 设计文档见 bench-ple.md，机制见 ../annotations.md §1
#
# 一次测量 = 一次完整 boot，不是"同一台 VM 里换参数"。两条理由：
#   1) ple_* 全是 0444 只读（arch/x86/kvm/vmx/vmx.c:204 等），改档必须重载 kvm_intel；
#   2) PLE 窗口 per-vCPU 只增不减（vmx.c:1417-1431），回落只在 vmx_vcpu_load
#      （vmx.c:1519-1523）—— 同进程内跨档比较一定被上一档污染。
#   （guest 的 minimal initramfs 里也没有 rmmod，负载无法热替换。）
#
# 用法：
#   ./bench-ple.sh --preflight                  只做 bench-ple.md §2 的检查（只读）
#   ./bench-ple.sh --arm A1 --dry-run           打印将执行的动作，不碰系统
#   sudo ./bench-ple.sh --arm A1 --repeat 3
#   sudo ./bench-ple.sh --all --repeat 3 --allow-reload
set -u
cd "$(dirname "$0")"

# ---------- 路径与常量 ----------
KERNEL=/root/code/linux-6.12.93/arch/x86_64/boot/bzImage   # 与 boot-vm.sh:178 同源
INITRD=../../scripts/images/initramfs.img
KO_DIR=ple-load
SHARE=../../scripts/shared
P_I=/sys/module/kvm_intel/parameters
TR=/sys/kernel/tracing
CG_ROOT=/sys/fs/cgroup
CG=$CG_ROOT/kvm-study-ple
CGNAME=kvm-study-ple

VCPU=16                 # guest vCPU 数
CPUS_OVER=0-7           # 2:1 超卖：8 个互不相同的物理核（拓扑核实见 bench-ple.md §2.3）
CPUS_FLAT=0-15          # 1:1 对照：16 个互不相同的物理核
MEMS=0                  # 两组都在 NUMA node0；不写死会让对照/实验的内存落点不同
HOLD=${HOLD:-2000}
THREADS=${THREADS:-16}
SAMPLE_S=${SAMPLE_S:-10}
WARM_S=${WARM_S:-3}

TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/ple-$TS
DRY=0; ALLOW_RELOAD=0; ONLY_PREFLIGHT=0; WITH_EXITS=0
ARMS=(); REPEAT=1
ALL_ARMS=(A0 A1 A2 A3 A4 A5)

log()  { printf '%s\n' "$*"; }
warn() { printf '!! %s\n' "$*" >&2; }
die()  { warn "$*"; exit 1; }

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

# ---------- 实验臂定义 ----------
arm_params() {                      # → "ple_gap ple_window"
    case $1 in
        A0|A1) echo "128 4096" ;;
        A2|A5) echo "0 4096"   ;;   # vmx_vm_init: !ple_gap → pause_in_guest → 只清 PLE 位
        A3)    echo "128 128" ;;
        A4)    echo "128 16777216" ;;
        *)     return 1 ;;
    esac
}
arm_mode()       { case $1 in A0|A5) echo flat ;; *) echo over ;; esac; }   # A0/A5 = 1:1 不超卖
arm_default()    { [ "$(arm_params "$1")" = "128 4096" ]; }

# ---------- 状态与恢复 ----------
ORIG_GAP=""; ORIG_WIN=""; QEMU_PID=""; CAT_PID=""; CG_READY=0

save_params() {
    ORIG_GAP=$(cat $P_I/ple_gap 2>/dev/null)
    ORIG_WIN=$(cat $P_I/ple_window 2>/dev/null)
}
reload_kvm_intel() {                       # $1 = "ple_gap=.. ple_window=.."
    if [ "$DRY" = 1 ]; then
        log "  [dry] modprobe -r kvm_intel && modprobe kvm_intel $1"; return 0
    fi
    # 拆模块会连带干掉别人的 VM，先确认没有进程持有 /dev/kvm
    local holders
    holders=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c '/dev/kvm')
    [ "$holders" = 0 ] || { warn "/dev/kvm 被 $holders 个 fd 持有，拒绝重载"; return 1; }
    modprobe -r kvm_intel || { warn "modprobe -r kvm_intel 失败（有 VM 在跑？）"; return 1; }
    # shellcheck disable=SC2086
    modprobe kvm_intel $1 || { warn "modprobe kvm_intel $1 失败"; return 1; }
    log "  已重载 kvm_intel: $1"
}
restore_params() {
    [ -n "$ORIG_GAP" ] || return 0
    [ "$(cat $P_I/ple_gap 2>/dev/null)" = "$ORIG_GAP" ] && \
    [ "$(cat $P_I/ple_window 2>/dev/null)" = "$ORIG_WIN" ] && return 0
    log "== 恢复 ple_gap=$ORIG_GAP ple_window=$ORIG_WIN =="
    reload_kvm_intel "ple_gap=$ORIG_GAP ple_window=$ORIG_WIN" || \
        warn "自动恢复失败，请手工执行：modprobe -r kvm_intel && modprobe kvm_intel ple_gap=$ORIG_GAP ple_window=$ORIG_WIN"
}
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$CAT_PID" ]  && kill "$CAT_PID"  2>/dev/null || true
    wait 2>/dev/null || true
    [ "$DRY" = 1 ] && return 0
    [ "$CG_READY" = 1 ] && rmdir "$CG" 2>/dev/null || true
    clear_ftrace || true
    restore_params || true
}
trap cleanup EXIT INT TERM

# ---------- ftrace ----------
clear_ftrace() {
    [ -e $TR/current_tracer ] || return 0
    if [ "$DRY" = 1 ]; then log "  [dry] 清 ftrace: current_tracer=nop、set_ftrace_filter、set_event"; return 0; fi
    echo nop > $TR/current_tracer 2>/dev/null || true
    echo > $TR/set_ftrace_filter 2>/dev/null || true
    echo > $TR/set_event 2>/dev/null || true
}
traceable() { grep -qE "^$1( |\[|$)" $TR/available_filter_functions 2>/dev/null; }

setup_ftrace() {
    local funcs="" f
    for f in kvm_vcpu_on_spin kvm_vcpu_yield_to; do
        traceable "$f" && funcs="$funcs $f"
    done
    [ -n "$funcs" ] || warn "on_spin/yield_to 都不可跟踪，PLE 机制侧没有观测出口（bench-ple.md §6 排查 2）"
    if [ "$DRY" = 1 ]; then
        log "  [dry] set_ftrace_filter 清空后逐个 >> 名字（$funcs）；current_tracer=function"
        log "  [dry] set_event=kvm:kvm_ple_window_update$([ "$WITH_EXITS" = 1 ] && echo ' + kvm:kvm_exit')"
        log "  [dry] 名字逐个写入，匹配不上的逐个告警（静默截断的风险见 bench-migrate.md §4.2.1）"
        return 0
    fi
    clear_ftrace
    # 逐个写：`echo function > set_ftrace_filter` 是无效的（实测 EINVAL），而一次写
    # 多个名字时第一个匹配不上的名字会中止本次 write，后面的名字连带丢失。
    local bad=""
    echo > $TR/set_ftrace_filter 2>/dev/null || warn "清空 set_ftrace_filter 失败"
    # shellcheck disable=SC2086
    for f in $funcs; do
        echo "$f" >> $TR/set_ftrace_filter 2>/dev/null || bad="$bad $f"
    done
    [ -n "$bad" ] && warn "这几个函数写不进 set_ftrace_filter：$bad（bench-migrate.md §4.2.1）"
    echo function > $TR/current_tracer
    # 清场要显式：`>` 打开 set_event 带 O_TRUNC，会在 ftrace_event_set_open() 里
    # 先 ftrace_clear_events() 清掉**全部**已启用事件（measurement.md §5 第 3 条）。
    # 加事件一律 `>>`，别让它顺手动到别人的探针。
    : > $TR/set_event 2>/dev/null || warn "清空 set_event 失败"
    [ -d $TR/events/kvm/kvm_ple_window_update ] && \
        echo kvm:kvm_ple_window_update >> $TR/set_event 2>/dev/null
    [ "$WITH_EXITS" = 1 ] && echo kvm:kvm_exit >> $TR/set_event 2>/dev/null
    echo 1 > $TR/tracing_on
}
# stats 里 "overrun:" 和 "commit overrun:" 每 CPU 都有这一行（值为 0 时也在），
# 所以只能把数值加总，grep 'overrun' 会永远命中。
overrun_total() { grep -h 'overrun:' "$1" 2>/dev/null | awk '{s+=$NF} END{print s+0}'; }
entries_total() { grep -h '^entries:' "$1" 2>/dev/null | awk '{s+=$NF} END{print s+0}'; }
collect_trace() {                          # $1=arm
    local a=$1 n_pause=0 n_spin n_yield n_win ovr
    [ "$DRY" = 1 ] && return 0
    cp $TR/trace "$OUT/trace-$a.txt" 2>/dev/null || true
    cat $TR/per_cpu/cpu*/stats > "$OUT/bufstats-$a.txt" 2>/dev/null || true
    n_pause=$(grep -c 'PAUSE_INSTRUCTION' "$OUT/trace-$a.txt" 2>/dev/null) || true
    n_spin=$(grep -c 'kvm_vcpu_on_spin'  "$OUT/trace-$a.txt" 2>/dev/null) || true
    n_yield=$(grep -c 'kvm_vcpu_yield_to' "$OUT/trace-$a.txt" 2>/dev/null) || true
    n_win=$(grep -c 'kvm_ple_window_update' "$OUT/trace-$a.txt" 2>/dev/null) || true
    log "  trace: PAUSE_exit=$n_pause on_spin=$n_spin yield_to=$n_yield ple_window_update=$n_win" | tee -a "$OUT/$a.txt"
    ovr=$(overrun_total "$OUT/bufstats-$a.txt")
    log "  buffer: entries=$(entries_total "$OUT/bufstats-$a.txt") overrun=$ovr"
    [ "${ovr:-0}" = 0 ] || \
        warn "ring buffer 有 $ovr 次 overrun，以上计数偏低（../measurement.md §4(c)）"
    clear_ftrace
}

# ---------- cgroup v2 cpuset ----------
setup_cg() {                               # $1 = over|flat
    local cpus=$CPUS_FLAT
    [ "$1" = over ] && cpus=$CPUS_OVER
    if [ "$DRY" = 1 ]; then
        log "  [dry] mkdir $CG; echo $cpus > cpuset.cpus; echo $MEMS > cpuset.mems"; return 0
    fi
    mkdir -p "$CG" || die "创建 $CG 失败"
    echo "$cpus" > "$CG/cpuset.cpus" || die "写 $CG/cpuset.cpus 失败"
    echo "$MEMS" > "$CG/cpuset.mems" 2>/dev/null || true
    CG_READY=1
    log "  cgroup: cpuset.cpus=$(cat $CG/cpuset.cpus) effective=$(cat $CG/cpuset.cpus.effective) mems=$(cat $CG/cpuset.mems.effective 2>/dev/null)"
}
move_to_cg() {                             # $1 = pid；v2 写 cgroup.procs 搬整个线程组
    if [ "$DRY" = 1 ]; then
        log "  [dry] echo <qemu-pid> > $CG/cgroup.procs"; return 0
    fi
    echo "$1" > "$CG/cgroup.procs" || die "移入 $CG 失败"
    local total inside t
    total=$(ls "/proc/$1/task" | wc -l)
    inside=0
    for t in /proc/$1/task/*/cgroup; do
        grep -q "/$CGNAME\$" "$t" 2>/dev/null && inside=$((inside+1))
    done
    [ "$inside" = "$total" ] || die "只有 $inside/$total 个线程进了 $CGNAME，超卖没做到位"
    log "  $total 个线程已全部在 $CGNAME 内（vCPU 线程=$VCPU）"
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
# 只认真数字；guest 回显的那行命令以 "$(" 开头，不会被匹配
guest_val() { grep -E "^$2 [0-9]+[[:space:]]*$" "$1" | tail -1 | cut -d' ' -f2; }

verify_kvm() {
    local n
    n=$(ls -l /proc/$1/fd 2>/dev/null | grep -c '/dev/kvm') || true
    [ "${n:-0}" -gt 0 ] || die "PID $1 未持有 /dev/kvm —— 走的是 TCG，kvm:* 全为零（AGENTS.md 陷阱 7）"
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

# ---------- 单臂一轮 ----------
boot_and_measure() {                       # $1=arm
    local arm=$1 cpus mode
    mode=$(arm_mode "$arm")
    [ "$mode" = over ] && cpus=$CPUS_OVER || cpus=$CPUS_FLAT
    log "-- $arm: $mode($cpus) threads=$THREADS hold=$HOLD sample=${SAMPLE_S}s"

    if [ "$DRY" = 1 ]; then
        setup_cg "$mode"
        log "  [dry] qemu-system-x86_64 -enable-kvm -cpu host -m 2G -smp $VCPU \\"
        log "        -kernel $KERNEL -initrd $INITRD -append 'console=ttyS0 ...' \\"
        log "        -virtfs local,path=$SHARE,mount_tag=hostshare,... \\"
        log "        -display none -monitor none -no-reboot -serial pipe:$OUT/ser-$arm"
        log "  [dry] echo <qemu-pid> > $CG/cgroup.procs 并校验全部线程都在组内"
        setup_ftrace
        log "  [dry] insmod /mnt/shared/ple_load.ko nr_threads=$THREADS hold_loops=$HOLD"
        log "  [dry] 采 completed 两次，间隔 ${SAMPLE_S}s"
        return 0
    fi

    setup_cg "$mode"
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
    move_to_cg "$QEMU_PID"
    setup_ftrace                          # 观测从这之后才生效，boot 期退出不计入

    {
        echo "arm=$arm mode=$mode cpus=$cpus threads=$THREADS hold=$HOLD sample=${SAMPLE_S}s"
        echo "boot_at=$(date -Is)"
    } >> "$OUT/$arm.txt"

    run_guest "insmod /mnt/shared/ple_load.ko nr_threads=$THREADS hold_loops=$HOLD"
    sleep "$WARM_S"
    run_guest "echo S0 \$(cat /sys/module/ple_load/parameters/completed)"
    sleep "$SAMPLE_S"
    run_guest "echo S1 \$(cat /sys/module/ple_load/parameters/completed)"
    sleep 1                               # 等 S1 回显落盘
    local c0 c1
    c0=$(guest_val "$OUT/serial-$arm.log" S0)
    c1=$(guest_val "$OUT/serial-$arm.log" S1)
    [ -n "$c0" ] && [ -n "$c1" ] || die "$arm: 没读到 completed 计数（模块没加载成功？见 $OUT/serial-$arm.log）"
    local rate
    rate=$(awk -v a="$c0" -v b="$c1" -v d="$SAMPLE_S" 'BEGIN{printf "%.1f",(b-a)/d}')
    log "  completed: S0=$c0 S1=$c1 rate=$rate/s"
    echo "completed S0=$c0 S1=$c1 rate_per_s=$rate" >> "$OUT/$arm.txt"

    grep -m1 'ple_load:' "$OUT/serial-$arm.log" >> "$OUT/$arm.txt" 2>/dev/null || true
    collect_trace "$arm"

    exec 7>&-
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
    kill "$CAT_PID" 2>/dev/null || true
    wait "$CAT_PID" 2>/dev/null || true
    CAT_PID=""
    rm -f "$OUT/ser-$arm.in" "$OUT/ser-$arm.out"
    rmdir "$CG" 2>/dev/null || true; CG_READY=0
}

# ---------- 前置检查 ----------
preflight() {
    local rc=0 p a
    log "=== preflight ==="

    log "--- 1) 参数实存性与权限（内核版本漂移检查）---"
    lsmod | grep -q '^kvm_intel' || { warn "kvm_intel 未加载"; rc=1; }
    for p in ple_gap ple_window ple_window_grow ple_window_shrink ple_window_max; do
        if [ -e "$P_I/$p" ]; then
            log "  $(printf '%-20s' "$p") = $(cat "$P_I/$p") perm=$(stat -c %a "$P_I/$p")"
        else
            warn "$P_I/$p 不存在 —— 与 parameters.md 记的 6.12.93 不一致，先核对内核版本"
            rc=1
        fi
    done

    log "--- 2) 观测出口 ---"
    for p in kvm_vcpu_on_spin kvm_vcpu_yield_to; do
        traceable "$p" && log "  $p 可跟踪" || warn "$p 不在 available_filter_functions"
    done
    for p in grow_ple_window shrink_ple_window; do
        traceable "$p" && log "  $p 可跟踪" || log "  $p 不可跟踪（已内联）→ 窗口只有 kvm:kvm_ple_window_update 一条出口"
    done
    [ -d $TR/events/kvm/kvm_ple_window_update ] \
        && log "  kvm:kvm_ple_window_update 存在" || { warn "缺 kvm:kvm_ple_window_update"; rc=1; }

    log "--- 3) 超卖手段 ---"
    [ "$(stat -fc %T $CG_ROOT 2>/dev/null)" = cgroup2fs ] \
        || { warn "$CG_ROOT 不是 cgroup2fs，cpuset 方案要改"; rc=1; }
    grep -qw cpuset "$CG_ROOT/cgroup.controllers" 2>/dev/null \
        || { warn "根 cgroup 无 cpuset controller"; rc=1; }
    local n_over n_flat
    n_over=$(echo "$CPUS_OVER" | awk -F'[-,]' '{print $2-$1+1}')
    n_flat=$(echo "$CPUS_FLAT" | awk -F'[-,]' '{print $2-$1+1}')
    log "  nproc=$(nproc) 超卖组 $n_over 核 / 对照 $n_flat 核 / guest vCPU=$VCPU → 超卖比 $(awk -v v=$VCPU -v c=$n_over 'BEGIN{printf "%.1f",v/c}')x"
    [ "$n_over" -lt "$VCPU" ] || { warn "超卖组核数 >= vCPU 数，根本没有超卖"; rc=1; }
    [ "$n_flat" -ge "$VCPU" ] || { warn "对照组核数 < vCPU 数，对照组也被超卖了"; rc=1; }
    log "  cpu0 SMT 兄弟=$(cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list 2>/dev/null)（若与所选核重叠，说明混进了 SMT 效应）"

    log "--- 4) 产物 ---"
    for p in "$KERNEL" "$INITRD" "$KO_DIR/ple_load.ko" "$SHARE/ple_load.ko"; do
        [ -f "$p" ] || { warn "缺 $p"; rc=1; }
    done
    [ -f "$SHARE/ple_load.ko" ] && [ -f "$KO_DIR/ple_load.ko" ] && \
        cmp -s "$KO_DIR/ple_load.ko" "$SHARE/ple_load.ko" \
        && log "  共享区的 ple_load.ko 与本地构建一致" \
        || warn "共享区的 ple_load.ko 缺失或与 $KO_DIR 下不一致，guest 会加载旧版本"
    log "  uname -r = $(uname -r)（宿主内核；文档行号基于 6.12.93，见 parameters.md §0）"

    log "--- 5) ftrace 残留 ---"
    p=$(cat $TR/current_tracer 2>/dev/null)
    [ -z "$p" ] || [ "$p" = nop ] || warn "current_tracer=$p，上一轮没清（AGENTS.md 陷阱 9）"

    log "--- 6) 其它 VM ---"
    p=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c '/dev/kvm')
    [ "${p:-0}" = 0 ] || warn "已有 $p 个 /dev/kvm fd 在跑：需要重载 kvm_intel 的臂（A2/A3/A4/A5）会被拒绝"

    if [ "$rc" = 0 ]; then log "== 前置检查通过 =="; else log "== 前置检查有未通过项，先修再跑 =="; fi
    return $rc
}

# ---------- 参数解析 ----------
while [ $# -gt 0 ]; do
    case $1 in
        --preflight)    ONLY_PREFLIGHT=1; shift ;;
        --dry-run)      DRY=1; shift ;;
        --allow-reload) ALLOW_RELOAD=1; shift ;;
        --with-exits)   WITH_EXITS=1; shift ;;
        --arm)          ARMS+=("$2"); shift 2 ;;
        --all)          ARMS=("${ALL_ARMS[@]}"); shift ;;
        --repeat)       REPEAT="$2"; shift 2 ;;
        --kernel)       KERNEL="$2"; shift 2 ;;
        --hold)         HOLD="$2"; shift 2 ;;
        --threads)      THREADS="$2"; shift 2 ;;
        -h|--help)      usage ;;
        *)              warn "未知参数 $1"; usage ;;
    esac
done
[ ${#ARMS[@]} -gt 0 ] || ARMS=(A1)
for a in "${ARMS[@]}"; do arm_params "$a" >/dev/null || die "未知实验臂 $a（可选 ${ALL_ARMS[*]}）"; done

if [ "$ONLY_PREFLIGHT" = 1 ]; then preflight; exit $?; fi
[ "$(id -u)" = 0 ] || [ "$DRY" = 1 ] || die "需要 root（cgroup 写入 / 模块重载 / tracefs）"
[ "$DRY" = 1 ] || mkdir -p "$OUT"
[ "$DRY" = 1 ] || save_params

for a in "${ARMS[@]}"; do
    gap=$(arm_params "$a" | cut -d' ' -f1)
    win=$(arm_params "$a" | cut -d' ' -f2)
    log "=== $a: mode=$(arm_mode "$a") ple_gap=$gap ple_window=$win ==="
    if ! arm_default "$a"; then
        [ "$ALLOW_RELOAD" = 1 ] || { warn "$a 需要重载 kvm_intel，未给 --allow-reload，跳过"; continue; }
        reload_kvm_intel "ple_gap=$gap ple_window=$win" || { warn "$a 重载失败，跳过"; continue; }
    fi
    for r in $(seq 1 "$REPEAT"); do
        log "-- $a repeat $r/$REPEAT"
        boot_and_measure "$a"
    done
done

log "输出目录: ${OUT}$([ "$DRY" = 1 ] && echo '（--dry-run：未执行任何改动动作）')"
