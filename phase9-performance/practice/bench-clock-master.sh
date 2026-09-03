#!/usr/bin/env bash
# E4 · 主时钟决策树驱动器 —— 设计文档见 bench-clock-master.md，机制在 ../annotations.md §3.1
#
# 一条**不可乱序**的时间线（每条臂的输入是前一条留下的状态）：
#   A0 无 VM 切一个来回 → A1 起 VM1 → A2 切走 → A3 切回 → A4 再起 VM2 → A5 迁移代价对照
#
# ★ 观测窗必须**跨过触发点**：稳态窗口里本来就不会有 master_clock 事件
#   （只有六个重算点会发，见 bench-clock-master.md §6.8），窗内 0 条不等于机制没跑。
# ★ 真跑必须带 --i-accept-clocksource-risk：clocksource 看门狗可能把宿主 TSC
#   永久判死（只能重启恢复），理由与缓解见 bench-clock-master.md §2.1。
# ★ 写 current_clocksource 之后一律**复读**判定，写入返回成功不代表切成了（§6.1）。
#
# 用法：
#   ./bench-clock-master.sh --preflight                    只读检查
#   ./bench-clock-master.sh --all --dry-run                打印整条时间线，不碰系统
#   sudo ./bench-clock-master.sh --until A4 --i-accept-clocksource-risk
#   sudo ./bench-clock-master.sh --all --i-accept-clocksource-risk --repeat 3
set -u
cd "$(dirname "$0")"

# ---------- 路径与常量 ----------
KERNEL=/root/code/linux-6.12.93/arch/x86_64/boot/bzImage   # 与 boot-vm.sh:178 同源
INITRD=../../scripts/images/initramfs.img
KO_DIR=ple-load
SHARE=../../scripts/shared
TR=/sys/kernel/tracing
DBK=/sys/kernel/debug/kvm
CS=/sys/devices/system/clocksource/clocksource0

VCPU=4                  # guest vCPU 数
PRIV_KB=256             # 每线程私有缓冲区（workload=1，无锁）
SAMPLE_S=20             # 每个观测窗长度；A1/A3/A4 必须等长（§4.2 第三条对照）
WARM_S=3
STEP_MS=200             # A5 的注入节奏
NODE=${NODE:-0}
BUF_KB=8192
FALLBACK=""             # 非 TSC 基的备随时钟源，留空则自动挑 hpet → acpi_pm
REPEAT=1                # 只作用于 A5（A0–A4 是过/不过的判据，重复不改变结论）

TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/clock-master-$TS
DRY=0; ONLY_PREFLIGHT=0; ACCEPT_RISK=0
UNTIL=A5
PHASES=(A0 A1 A2 A3 A4 A5)

log()  { printf '%s\n' "$*"; }
warn() { printf '!! %s\n' "$*" >&2; }
die()  { warn "$*"; exit 1; }
usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

# ---------- 结果落盘 ----------
# --dry-run 时 $OUT 根本没创建（脚本不碰系统），所有写文件都必须短路，否则判据
# 打印会带着 "No such file or directory" 失败、看不出是真判据不过还是没落盘
out_append() {                         # $1=文件 $2=整行
    [ "$DRY" = 1 ] && return 0
    printf '%s\n' "$2" >> "$1"
}

# ---------- 状态 ----------
VM1_PID=""; VM2_PID=""
VM1_TIDS=""; VM2_TIDS=""
VM1_TAG=vm1; VM2_TAG=vm2
ORIG_CS=""; CUR_CS=""; FALLBACK_PICK=""; OFFS_BASE=""
declare -a A B                       # A5 的两组核
PASS=""; FAIL=""

verdict() {                          # $1=臂 $2=0通过/非0不通过 $3=通过说明 $4=失败说明
    if [ "$2" = 0 ]; then PASS="$PASS $1"; log "  ✓ $1：$3"
    else FAIL="$FAIL ${1}(FAILED)"; warn "  ✗ $1：$4"; fi
}

# ---------- 恢复 ----------
restore_cs() {
    [ -n "$ORIG_CS" ] || return 0
    if [ "$DRY" = 1 ]; then log "  [dry] echo $ORIG_CS > $CS/current_clocksource"; return 0; fi
    if [ "$(cs_cur)" = "$ORIG_CS" ]; then CUR_CS=$ORIG_CS; return 0; fi
    echo "$ORIG_CS" > $CS/current_clocksource 2>/dev/null || true
    if [ "$(cs_cur)" = "$ORIG_CS" ]; then
        CUR_CS=$ORIG_CS; log "  已恢复宿主 clocksource=$ORIG_CS"
    else
        CUR_CS=$(cs_cur)
        printf '\n!!!! 宿主 clocksource 没能恢复：当前 %s，应为 %s\n' "${CUR_CS:-UNKNOWN}" "$ORIG_CS" >&2
        printf '!!!! 手工执行：echo %s > %s/current_clocksource\n\n' "$ORIG_CS" "$CS" >&2
    fi
}
kill_cat() {                         # $1=tag
    local p
    p=$(cat "$OUT/cat-$1.pid" 2>/dev/null) || true
    [ -n "$p" ] && kill "$p" 2>/dev/null || true
    rm -f "$OUT/cat-$1.pid"
}
kill_vm() {                          # $1=slot
    local pid tag
    if [ "$1" = 1 ]; then pid=$VM1_PID; tag=$VM1_TAG; else pid=$VM2_PID; tag=$VM2_TAG; fi
    [ -z "$pid" ] && return 0
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    kill_cat "$tag"
    if [ "$1" = 1 ]; then
        VM1_PID=""; VM1_TIDS=""; exec 7>&- 2>/dev/null || true
    else
        VM2_PID=""; VM2_TIDS=""; exec 8>&- 2>/dev/null || true
    fi
    rm -f "$OUT/ser-$tag.in" "$OUT/ser-$tag.out"
}
cleanup() {
    if [ "$DRY" != 1 ]; then
        kill_vm 2 || true
        kill_vm 1 || true
        restore_cs || true
        clear_ftrace || true
    fi
    return 0
}
trap cleanup EXIT INT TERM

# ---------- ftrace ----------
clear_ftrace() {
    [ -e $TR/current_tracer ] || return 0
    if [ "$DRY" = 1 ]; then log "  [dry] 清 ftrace: current_tracer=nop、set_event、tracing_on=0"; return 0; fi
    echo 0 > $TR/function_profile_enabled 2>/dev/null || true
    echo nop > $TR/current_tracer 2>/dev/null || true
    echo > $TR/set_ftrace_filter 2>/dev/null || true
    echo > $TR/set_event 2>/dev/null || true
    echo 0 > $TR/tracing_on 2>/dev/null || true
}
EV_LIST="kvm:kvm_update_master_clock kvm:kvm_track_tsc kvm:kvm_pvclock_update kvm:kvm_write_tsc_offset"

setup_events() {
    local e miss=""
    [ "$DRY" = 1 ] && { log "  [dry] buffer_size_kb=$BUF_KB; set_event=$EV_LIST"; return 0; }
    : > $TR/set_event 2>/dev/null || warn "写 set_event 失败"
    echo $BUF_KB > $TR/buffer_size_kb 2>/dev/null || warn "buffer_size_kb 设置失败"
    for e in $EV_LIST; do
        [ -d "$TR/events/${e%:*}/${e#*:}" ] && echo "$e" >> $TR/set_event 2>/dev/null || miss="$miss $e"
    done
    [ -n "$miss" ] && warn "这几个事件不存在，相应判据无从核对：$miss（bench-clock-master.md §2.4）"
    echo 1 > $TR/tracing_on 2>/dev/null || warn "tracing_on 打不开"
}
# 一个观测窗 = 先清 buffer 再放行，避免"上一臂的事件算进这一臂"
open_win() {
    [ "$DRY" = 1 ] && { log "  [dry]   开观测窗：tracing_on=0 → 清 trace_buf → tracing_on=1"; return 0; }
    echo 0 > $TR/tracing_on; : > $TR/trace; echo 1 > $TR/tracing_on
}
close_win() {                        # $1=tag
    local ovr
    [ "$DRY" = 1 ] && return 0
    cp $TR/trace "$OUT/win-$1.txt" 2>/dev/null || : > "$OUT/win-$1.txt"
    cat $TR/per_cpu/cpu*/stats > "$OUT/bufstats-$1.txt" 2>/dev/null || true
    ovr=$(grep -h 'overrun:' "$OUT/bufstats-$1.txt" 2>/dev/null | awk '{s+=$NF} END{print s+0}')
    [ "${ovr:-0}" = 0 ] || warn "  窗口 $1 有 $ovr 次 ring buffer overrun，事件计数偏低（../measurement.md §4(c)）"
}
tid_re() { [ -n "$1" ] && printf '%s' "$1" | tr ' ' '|' || printf '%s' ""; }
# 按 tid 过滤计数：主时钟事件在 vCPU 线程上下文发出（建 VM 那条例外在 QEMU 主线程，§6.4）
ev_count() {                         # $1=窗口tag $2=事件名 $3=tid 正则（空=不过滤） $4=行内附加 pattern
    local f="$OUT/win-$1.txt" n
    [ -f "$f" ] || { echo 0; return; }
    if [ -n "$3" ]; then
        n=$(grep -E -- "-($3) \[" "$f" 2>/dev/null | grep -c "$2:.*$4" || true)
    else
        n=$(grep -c "$2:.*$4" "$f" 2>/dev/null || true)
    fi
    echo "${n:-0}"
}
# kvm_track_tsc 里的 offsetmatched 是**计数**、nr_online 是总数，判据是 前者+1==后者（§2.5）
track_ok() {                         # $1=窗口tag $2=tid 正则
    local f="$OUT/win-$1.txt"
    [ -f "$f" ] && [ -n "$2" ] || { echo 0; return; }
    grep -E -- "-($2) \[" "$f" 2>/dev/null | \
        awk '{ m=""; n="";
               for (i=1;i<=NF;i++) { if ($i=="offsetmatched") m=$(i+1); if ($i=="nr_online") n=$(i+1) }
               if (m!="" && n!="" && m+1==n) ok++ }
            END{print ok+0}'
}
pvclock_per_s() {                    # $1=窗口tag $2=tid 正则
    local n; n=$(ev_count "$1" kvm_pvclock_update "$2" "")
    awk -v n="$n" -v s="$SAMPLE_S" 'BEGIN{printf "%.2f", (s>0? n/s : 0)}'
}

# ---------- debugfs tsc-offset 阴性对照（§4.2 第二条） ----------
# 定位 per-vCPU 目录有两条路，先走可靠的那条：
#   (a) 每个 vcpuN 目录里有个 pid 文件，印的是 vcpu->pid（virt/kvm/kvm_main.c:4184-4194），
#       而 vcpu->pid 在 KVM_RUN 时被设成跑这个 vCPU 的线程 tid（:4484-4486）→ 按 tid 精确反查；
#   (b) 兜底按 VM 进程号拼目录名（6.12.93 是 "<pid>-<fdname>"，:1060；更早的内核只有裸 pid）。
# 读到的值是 vcpu->arch.tsc_offset（arch/x86/kvm/debugfs.c:33-38），不是 l1_tsc_offset ——
# 主时钟翻转**不该**动它（x86.c:3016-3044 里没有任何写 offset 的路径）。
tsc_offset_of() {                      # $1=vcpu 序号 $2=该 vCPU 线程 tid $3=VM pid → 值或 NA
    local i=$1 tid=$2 pid=$3 f
    [ -n "$tid" ] || { echo NA; return; }
    for f in "$DBK"/*/vcpu"$i"/pid; do
        [ -r "$f" ] || continue
        [ "$(cat "$f" 2>/dev/null | tr -d '[:space:]')" = "$tid" ] || continue
        f="${f%pid}tsc-offset"
        [ -r "$f" ] && { cat "$f" 2>/dev/null | tr -d '[:space:]'; return; }
    done
    for f in "$DBK/$pid/vcpu$i/tsc-offset" "$DBK/$pid-"*/vcpu"$i/tsc-offset"; do
        [ -r "$f" ] && { cat "$f" 2>/dev/null | tr -d '[:space:]'; return; }
    done
    echo NA
}
tsc_offsets() {                        # $1=tid 串（按 vcpu id 升序）$2=VM pid
    local tids=($1) i out=""
    for ((i = 0; i < VCPU; i++)); do out="$out vcpu$i=$(tsc_offset_of "$i" "${tids[$i]:-}" "$2")"; done
    echo "${out# }"
}
offset_control() {                     # $1=臂 $2=tid 串 $3=VM pid $4=基线串 → 0一致/1变了/2无从核对
    local now; now=$(tsc_offsets "$2" "$3")
    log "  tsc-offset $1: $now"
    out_append "$OUT/$VM1_TAG.txt" "$1 tsc_offsets=$now"
    case "$4" in *NA*|'') log "  （基线不可用，本臂对照跳过）"; return 2 ;; esac
    case "$now" in *NA*) log "  （§4.2 第二条无从核对：debugfs 里没有该 VM 的 tsc-offset，不算通过也不算失败）"; return 2 ;; esac
    [ "$now" = "$4" ] || { warn "  tsc-offset 与基线不同：基线 $4 → 现在 $now（主时钟翻转不该动它，../annotations.md §3.2）"; return 1; }
    return 0
}

# ---------- 宿主 clocksource ----------
cs_cur() { cat $CS/current_clocksource 2>/dev/null | tr -d '[:space:]'; }
cs_avail() { cat $CS/available_clocksource 2>/dev/null | tr -s ' \n' ' '; }
pick_fallback() {
    local c avail; avail=$(cs_avail)
    for c in hpet acpi_pm; do
        case " $avail " in *" $c "*) echo $c; return 0 ;; esac
    done
    for c in $avail; do
        [ "$c" != tsc ] && [ "$c" != tsc-early ] && { echo $c; return 0; }
    done
    return 1
}
cs_guard() {
    [ "$DRY" = 1 ] && return 0
    [ "$ACCEPT_RISK" = 1 ] || die "要真写 current_clocksource 必须带 --i-accept-clocksource-risk（bench-clock-master.md §2.1 的不可逆风险）"
}
# ★ 写入返回成功 ≠ 切换生效（名字拼错、已被判 unstable 都会静默失败，§6.1）
cs_set() {                           # $1=目标时钟源
    [ -n "$1" ] || { warn "没有可用的非 TSC 基时钟源，跳过切换（§2.2）"; return 1; }
    [ "$DRY" = 1 ] && { log "  [dry]   echo $1 > $CS/current_clocksource → 复读确认为 $1"; return 0; }
    echo "$1" > $CS/current_clocksource 2>/dev/null || true
    local end=$((SECONDS + 5)) got=""
    while [ $SECONDS -lt $end ]; do
        got=$(cs_cur); [ "$got" = "$1" ] && break
        sleep 0.2
    done
    [ "$got" = "$1" ] || { warn "  切换后 current_clocksource=${got:-?}，期望 $1（§6.1 的静默失败）"; return 1; }
    CUR_CS=$got; log "  宿主 clocksource 现为 $got"
    dmesg 2>/dev/null | grep -i "marking clocksource" | tail -2 | sed 's/^/    dmesg: /' >&2 || true
}
# A3 期间每秒复读，确认整窗都停在期望值上（§4.1-A3 第二条）
cs_hold() {                          # $1=期望值 $2=秒
    local i bad=0
    [ "$DRY" = 1 ] && { log "  [dry]   每 1 s 复读 current_clocksource 共 $2 次，期望一直是 $1"; return 0; }
    for i in $(seq 1 "$2"); do
        [ "$(cs_cur)" = "$1" ] || bad=$((bad + 1))
        sleep 1
    done
    [ "$bad" = 0 ] || { warn "  有 $bad 秒 current_clocksource 不是 $1，这一窗不干净"; return 1; }
    return 0
}

# ---------- 核选取与亲和性（A5） ----------
expand_list() {
    local IFS=',' c out=""
    for c in $1; do
        case $c in
            *-*) out="$out $(seq "${c%-*}" "${c#*-}")" ;;
            *)   out="$out $c" ;;
        esac
    done
    echo $out
}
pick_cores() {                       # $1=需要多少个不同物理核 → 每行 "本核 兄弟"
    local want=$1 got=0 seen=" " c sib n first second
    for c in $(seq 0 $(( $(nproc) - 1 ))); do
        [ "$got" -ge "$want" ] && break
        [ -r "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list" ] || continue
        n=$(cat "/sys/devices/system/cpu/cpu$c/node" 2>/dev/null)
        [ "${n:-$NODE}" = "$NODE" ] || continue
        # shellcheck disable=SC2086
        set -- $(expand_list "$(cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list")")
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
    rows=$(pick_cores $((VCPU * 2))) || die "node$NODE 上凑不出 $((VCPU * 2)) 个不同物理核，减小 --vcpu 或换 NODE"
    mapfile -t A < <(printf '%s\n' "$rows" | head -n "$VCPU" | awk '{print $1}')
    mapfile -t B < <(printf '%s\n' "$rows" | tail -n "$VCPU" | awk '{print $1}')
    log "  A 组核=${A[*]}  B 组核=${B[*]}（node$NODE、互不同核）"
}
pin_vcpus() {                        # $1=tids
    local tids=($1) i
    for i in "${!tids[@]}"; do
        if [ "$DRY" = 1 ]; then log "  [dry]   taskset -pc ${A[$((i % ${#A[@]}))]} ${tids[$i]}"; continue; fi
        taskset -pc "${A[$((i % ${#A[@]}))]}" "${tids[$i]}" >/dev/null 2>&1 || \
            warn "  绑核失败 ${tids[$i]} → ${A[$((i % ${#A[@]}))]}，A5 的注入次数与迁移次数会对不上"
    done
}
inject_step() {                      # $1=tids $2=step
    local tids=($1) i p cpu
    p=$(( $2 % 2 ))
    for i in "${!tids[@]}"; do
        if [ "$p" = 0 ]; then cpu=${A[$((i % ${#A[@]}))]}; else cpu=${B[$((i % ${#B[@]}))]}; fi
        if [ "$DRY" = 1 ]; then
            [ "$2" = 1 ] && log "  [dry]   每步 taskset -pc $cpu ${tids[$i]}（共 ${#tids[@]} 个线程/步）"
            continue
        fi
        taskset -pc "$cpu" "${tids[$i]}" >/dev/null 2>&1 || true
    done
}

# ---------- guest 交互 ----------
run_guest() {                        # $1=slot $2..=命令
    local slot=$1; shift
    if [ "$DRY" = 1 ]; then log "  [dry]   guest#$slot: $*"; return 0; fi
    if [ "$slot" = 1 ]; then printf '%s\n' "$*" >&7; else printf '%s\n' "$*" >&8; fi
}
wait_marker() {
    for _ in $(seq 1 400); do
        grep -q '^-------------------' "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
guest_val() { grep -E "^$2 [0-9]+[[:space:]]*$" "$1" 2>/dev/null | tail -1 | cut -d' ' -f2; }

verify_kvm() {
    local n
    n=$(ls -l /proc/$1/fd 2>/dev/null | grep -c '/dev/kvm') || true
    [ "${n:-0}" -gt 0 ] || die "PID $1 未持有 /dev/kvm —— 走的是 TCG（AGENTS.md 陷阱 7）"
    log "  KVM 确认：$n 个 /dev/kvm fd"
}
find_vcpu_tids() {                   # $1=pid → 按 **vcpu id 升序** 输出 tid（缺号则失败）
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

# ---------- 起一台 VM 并挂上无锁负载 ----------
# ★ 调用方决定观测窗是否跨过 boot —— 主时钟事件只在触发点出现，稳态窗里天然为 0（§6.8）
boot_vm() {                          # $1=slot $2=tag
    # ★ 不要写成 local slot=$1 tag=$2 serial=...-$tag...：bash 先展开完整条 local 的参数
    #   再赋值，同一句里引用刚声明的局部变量必定 unbound（实测 bash 5.2）
    local slot=$1 tag=$2 fd pidv
    local serial="$OUT/serial-$tag.log"
    if [ "$slot" = 1 ]; then fd=7; VM1_TAG=$tag; else fd=8; VM2_TAG=$tag; fi
    if [ "$DRY" = 1 ]; then
        log "  [dry]   qemu -enable-kvm -cpu host -m 2G -smp $VCPU -serial pipe:$OUT/ser-$tag"
        log "  [dry]   guest: insmod /mnt/shared/ple_load.ko workload=1 nr_threads=$VCPU priv_kb=$PRIV_KB"
        local ph i; ph=""; for i in $(seq 0 $((VCPU - 1))); do ph="$ph <tid$i>"; done; ph="${ph# }"
        if [ "$slot" = 1 ]; then VM1_PID="<pid-$tag>"; VM1_TIDS="$ph"; else VM2_PID="<pid-$tag>"; VM2_TIDS="$ph"; fi
        return 0
    fi
    mkfifo "$OUT/ser-$tag.in" "$OUT/ser-$tag.out"
    qemu-system-x86_64 -enable-kvm -cpu host -m 2G -smp "$VCPU" \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "console=ttyS0 earlyprintk=serial rdinit=/init" \
        -virtfs "local,path=$SHARE,mount_tag=hostshare,security_model=passthrough,id=hostshare" \
        -display none -monitor none -no-reboot \
        -serial pipe:"$OUT/ser-$tag" > "$OUT/qemu-$tag.err" 2>&1 &
    if [ "$slot" = 1 ]; then VM1_PID=$!; else VM2_PID=$!; fi
    pidv=$( [ "$slot" = 1 ] && echo "$VM1_PID" || echo "$VM2_PID" )
    cat "$OUT/ser-$tag.out" > "$serial" &
    echo $! > "$OUT/cat-$tag.pid"
    # ★ 用 <> 而不是 >：只写打开 FIFO 会阻塞到读端出现，QEMU 没起来时这里会永久挂住
    eval "exec $fd<> \"$OUT/ser-$tag.in\""
    wait_marker "$serial" || die "$tag: guest 未就绪（见 $serial）"
    verify_kvm "$pidv"
    local tids n
    tids=$(find_vcpu_tids "$pidv"); n=$(echo $tids | wc -w)
    [ "$n" = "$VCPU" ] || die "$tag: 只认出 $n 个 'CPU <n>/KVM' 线程，期望 $VCPU（QEMU 线程名可能变了，bench-migrate.md §2.2）"
    if [ "$slot" = 1 ]; then VM1_TIDS=$tids; else VM2_TIDS=$tids; fi
    log "  $tag: pid=$pidv vcpu tids=$tids"
    run_guest "$slot" "insmod /mnt/shared/ple_load.ko workload=1 nr_threads=$VCPU priv_kb=$PRIV_KB"
    sleep "$WARM_S"
    run_guest "$slot" "echo S0 \$(cat /sys/module/ple_load/parameters/completed)"
    out_append "$OUT/$tag.txt" "tag=$tag pid=$pidv tids=$tids vcpu=$VCPU sample_s=$SAMPLE_S step_ms=$STEP_MS"
}
kill_vm_slot() {
    [ "$DRY" = 1 ] && { log "  [dry]   关机 slot$1"; return 0; }
    kill_vm "$1"
}

# ============================================================
#  A0：不开 VM，只切一个来回 —— 阴性对照
# ============================================================
phase_A0() {
    log "--- A0：无 VM 时切一个来回，期望 kvm:* 时钟事件为 0 ---"
    setup_events; open_win
    if [ "$DRY" = 1 ]; then
        log "  [dry]   echo $FALLBACK_PICK → 等 2s → echo $ORIG_CS（全程无 VM）"
    else
        cs_set "$FALLBACK_PICK" || true
        sleep 2
        cs_set "$ORIG_CS" || true
    fi
    close_win A0
    local n; n=$(ev_count A0 kvm_update_master_clock "" "")
    log "  master_clock 事件（不过滤 tid）=$n"
    out_append "$OUT/env.txt" "A0 master_clock_events=$n cs=$(cs_cur)"
    if [ "$DRY" = 1 ]; then verdict A0 0 "dry-run 只打印计划" ""
    else verdict A0 "$([ "${n:-0}" = 0 ] && echo 0 || echo 1)" \
        "全为 0 —— 事件确实只来自我们的 VM" \
        "出现了 $n 条：宿主上有别的 VM 在跑（§4.2）"; fi
}

# ============================================================
#  A1–A4：一条连续时间线（VM1 全程活着）
# ============================================================
phase_A1() {
    log "--- A1：起 VM1，主时钟应当是开的（窗口跨过 boot，§6.8） ---"
    setup_events
    open_win
    boot_vm 1 "$VM1_TAG"
    [ "$DRY" = 1 ] && log "  [dry]   采样 ${SAMPLE_S}s（guest 有负载，vCPU 才会消费请求，§2.3）" || sleep "$SAMPLE_S"
    close_win A1
    local tr one ok; tr=$(tid_re "$VM1_TIDS")
    one=$(ev_count A1 kvm_update_master_clock "$tr" " masterclock 1")
    ok=$(track_ok A1 "$tr")
    log "  VM1: masterclock 1=${one:-0} 条，kvm_track_tsc 满足 (matched+1==online)=${ok:-0} 条"
    out_append "$OUT/$VM1_TAG.txt" "A1 masterclock1=$one track_ok=$ok"
    OFFS_BASE=$(tsc_offsets "$VM1_TIDS" "$VM1_PID")
    log "  tsc-offset 基线：$OFFS_BASE（A2/A3 拿它做 §4.2 第二条对照）"
    case "$OFFS_BASE" in
        *NA*) warn "  读不到该 VM 的 tsc-offset，§4.2 第二条对照在本机不可用（debugfs 布局随版本变，bench-migrate.md §2.5）" ;;
    esac
    if [ "$DRY" = 1 ]; then verdict A1 0 "dry-run 只打印计划" ""
    else verdict A1 "$([ "${one:-0}" -gt 0 ] && [ "${ok:-0}" -gt 0 ] && echo 0 || echo 1)" \
        "基线成立：主时钟开着" \
        "看不到 masterclock 1 或 track_tsc 不满足式 —— 基线不成立，后面三臂无意义"; fi
}
phase_A2() {
    log "--- A2：VM1 活着，切到 ${FALLBACK_PICK:-<无>}（关边，P1） ---"
    open_win
    cs_set "$FALLBACK_PICK" || true
    [ "$DRY" = 1 ] && log "  [dry]   采样 ${SAMPLE_S}s（VM1 的 vCPU 必须在跑，否则没人消费请求）" || sleep "$SAMPLE_S"
    close_win A2
    local tr zero withm one; tr=$(tid_re "$VM1_TIDS")
    zero=$(ev_count A2 kvm_update_master_clock "$tr" " masterclock 0")
    withm=$(ev_count A2 kvm_update_master_clock "$tr" " masterclock 0 hostclock none offsetmatched 1")
    one=$(ev_count A2 kvm_update_master_clock "$tr" " masterclock 1")
    log "  VM1: masterclock 0=${zero:-0}（其中 hostclock none + offsetmatched 1 = ${withm:-0}） masterclock 1=${one:-0}"
    out_append "$OUT/$VM1_TAG.txt" "A2 masterclock0=$zero none_and_matched=$withm masterclock1_after_off=$one"
    offset_control A2 "$VM1_TIDS" "$VM1_PID" "$OFFS_BASE" || true
    log "  本窗 kvm_write_tsc_offset=$(ev_count A2 kvm_write_tsc_offset "$tr" '')（预期 0：x86.c:3016-3044 不写 offset）"
    # ★ 必须"0 + offsetmatched 1"同现：这才证明掉的是宿主时钟源条件而不是 vCPU 匹配条件（§2.5）
    if [ "$DRY" = 1 ]; then verdict A2 0 "dry-run 只打印计划" ""
    else verdict A2 "$([ "${withm:-0}" -gt 0 ] && echo 0 || echo 1)" \
        "P1 成立：不重启 VM 主时钟就翻了，且翻的是 host 条件" \
        "没看到 masterclock 0 hostclock none offsetmatched 1（切换没成？guest 没负载？§2.2/§2.3）"; fi
}
phase_A3() {
    log "--- A3：切回 $ORIG_CS，什么都不做（开边，P2 的核心预测=零） ---"
    cs_set "$ORIG_CS" || true
    open_win
    if [ "$DRY" = 1 ]; then log "  [dry]   cs_hold $ORIG_CS ${SAMPLE_S}s"
    else cs_hold "$ORIG_CS" "$SAMPLE_S" || true; fi
    close_win A3
    local tr one zero; tr=$(tid_re "$VM1_TIDS")
    one=$(ev_count A3 kvm_update_master_clock "$tr" " masterclock 1")
    zero=$(ev_count A3 kvm_update_master_clock "$tr" " masterclock 0")
    log "  VM1: masterclock 1=${one:-0}（期望 0） masterclock 0=${zero:-0}，窗口内 clocksource 恒为 $(cs_cur)"
    out_append "$OUT/$VM1_TAG.txt" "A3 masterclock1=$one masterclock0=$zero"
    offset_control A3 "$VM1_TIDS" "$VM1_PID" "$OFFS_BASE" || true
    log "  本窗 kvm_write_tsc_offset=$(ev_count A3 kvm_write_tsc_offset "$tr" '')（预期 0）"
    if [ "$DRY" = 1 ]; then verdict A3 0 "dry-run 只打印计划" ""
    else verdict A3 "$([ "${one:-0}" = 0 ] && echo 0 || echo 1)" \
        "P2 成立：换回 tsc 之后老 VM 自己回不去" \
        "出现 ${one} 条 masterclock 1 —— P2 被否证，还有一条没找到的 on-edge 触发路径，登记 ../corrections.md"; fi
}
phase_A4() {
    log "--- A4：VM1 还活着时再起 VM2（per-VM 分叉，P3） ---"
    open_win                             # ★ 窗口必须跨过 VM2 的创建，否则一条都看不到
    boot_vm 2 "$VM2_TAG"
    [ "$DRY" = 1 ] && log "  [dry]   采样 ${SAMPLE_S}s，两台 VM 同时在跑" || sleep "$SAMPLE_S"
    close_win A4
    local v1 v2
    v1=$(ev_count A4 kvm_update_master_clock "$(tid_re "$VM1_TIDS")" " masterclock 1")
    v2=$(ev_count A4 kvm_update_master_clock "$(tid_re "$VM2_TIDS")" " masterclock 1")
    log "  同窗口：VM1(应仍关)=${v1:-0} 条 masterclock 1，VM2(新建应开)=${v2:-0} 条"
    out_append "$OUT/$VM1_TAG.txt" "A4 vm1_masterclock1=$v1 vm2_masterclock1=$v2"
    out_append "$OUT/$VM2_TAG.txt" "A4 vm1_masterclock1=$v1 vm2_masterclock1=$v2"
    if [ "$DRY" = 1 ]; then verdict A4 0 "dry-run 只打印计划" ""
    else verdict A4 "$([ "${v2:-0}" -gt 0 ] && [ "${v1:-0}" = 0 ] && echo 0 || echo 1)" \
        "P3 成立：同一台宿主上两台 VM 的快路径就此分叉" \
        "VM2=${v2:-0} VM1=${v1:-0}，与'新 VM 开、老 VM 仍关'不符（注意 §6.4 的建 VM 噪声）"; fi
    kill_vm_slot 2
}

# ============================================================
#  A5：主时钟 off vs on 的迁移代价（唯一有结果侧的臂）
# ============================================================
# ★ 两臂的注入窗内宿主都在 tsc 上，唯一系统差是 use_master_clock —— 借 A2/A3 造出来的
#   "回不去"状态当对照组，否则会把"主时钟关"与"宿主时钟源变慢"混成一团（§3 末）
cost_run() {                         # $1=off|on
    local mode=$1 steps tids tr n_on n_off n_one pv rate c0 c1 i upper line
    local tag="C-$mode"
    steps=$(( SAMPLE_S * 1000 / STEP_MS ))
    upper=$(( steps * VCPU / SAMPLE_S + 10 * VCPU ))
    log "--- A5/$tag：主时钟应为 $mode，注入 $steps 步 × $VCPU 线程 ---"
    setup_events
    plan_cpus
    open_win                             # 窗口跨过 boot，用来确认这台 VM 起步时的主时钟状态
    boot_vm 1 "$tag"
    [ "$DRY" = 1 ] || sleep "$SAMPLE_S"
    close_win "$tag-boot"
    tids=$VM1_TIDS; tr=$(tid_re "$tids")
    n_on=$(ev_count "$tag-boot" kvm_update_master_clock "$tr" " masterclock 1")
    log "  起步：masterclock 1=${n_on:-0} 条（两臂都要求 >0，证明 C-off 的关闭是后发生的）"
    [ "${n_on:-0}" -gt 0 ] || [ "$DRY" = 1 ] || { warn "  $tag 起步就没开主时钟，本臂作废"; kill_vm_slot 1; return 1; }
    if [ "$mode" = off ]; then
        cs_set "$FALLBACK_PICK" || true
        open_win; [ "$DRY" = 1 ] || sleep "$SAMPLE_S"; close_win "$tag-offedge"
        n_off=$(ev_count "$tag-offedge" kvm_update_master_clock "$tr" " masterclock 0")
        cs_set "$ORIG_CS" || true
        open_win; [ "$DRY" = 1 ] || sleep "$SAMPLE_S"; close_win "$tag-assert"
        n_one=$(ev_count "$tag-assert" kvm_update_master_clock "$tr" " masterclock 1")
        log "  前置断言：off 边 ${n_off:-0} 条 masterclock 0；切回后 ${n_one:-0} 条 masterclock 1（应为 0）"
        if [ "$DRY" != 1 ] && { [ "${n_off:-0}" = 0 ] || [ "${n_one:-0}" != 0 ]; }; then
            warn "  前置不成立（A2/A3 的结论没复现），本臂作废"; kill_vm_slot 1; return 1
        fi
    else
        open_win; [ "$DRY" = 1 ] || sleep "$SAMPLE_S"; close_win "$tag-assert"
        n_one=$(ev_count "$tag-assert" kvm_update_master_clock "$tr" " masterclock 1")
        log "  前置断言：全程 tsc，本窗 masterclock 1=${n_one:-0}（稳态窗为 0 属正常，见 §6.8）"
    fi
    pin_vcpus "$tids"
    run_guest 1 "echo S0 \$(cat /sys/module/ple_load/parameters/completed)"
    [ "$DRY" = 1 ] || sleep 1
    open_win
    for i in $(seq 1 "$steps"); do
        inject_step "$tids" "$i"
        if [ "$DRY" = 1 ]; then log "  [dry]   ……$steps 步同上，共 ${SAMPLE_S}s"; break; fi
        sleep "$(awk -v ms="$STEP_MS" 'BEGIN{printf "%.3f", ms/1000}')"
    done
    close_win "$tag"
    run_guest 1 "echo S1 \$(cat /sys/module/ple_load/parameters/completed)"
    [ "$DRY" = 1 ] || sleep 2
    pv=$(ev_count "$tag" kvm_pvclock_update "$tr" "")
    rate=$(pvclock_per_s "$tag" "$tr")
    c0=$(guest_val "$OUT/serial-$tag.log" S0); c1=$(guest_val "$OUT/serial-$tag.log" S1)
    log "  $tag: pvclock=${pv:-0}（${rate}/s，模型上界≈${upper}/s）completed=${c0:-?}→${c1:-?}"
    line="$tag mode=$mode pvclock=${pv:-0} pvclock_per_s=$rate model_upper_per_s=$upper plan_inject=$((steps * VCPU))"
    if [ -n "$c0" ] && [ -n "$c1" ]; then
        line="$line completed_s0=$c0 completed_s1=$c1 completed_per_s=$(awk -v a="$c0" -v b="$c1" -v s="$SAMPLE_S" 'BEGIN{printf "%.1f",(b-a)/s}')"
    fi
    out_append "$OUT/A5.txt" "$line"
    kill_vm_slot 1
    if [ "$DRY" != 1 ]; then restore_cs || true; clear_ftrace || true; fi
    return 0
}
phase_A5() {
    local r
    # ★ 先关掉 A1–A4 那台 VM1：cost_run 复用 slot1，不清会让 boot_vm 覆盖 VM1_PID，
    #   老 QEMU 既没人杀也一直在产生 pvclock 事件，A5 的按-tid 计数就废了
    kill_vm_slot 1
    for r in $(seq 1 "$REPEAT"); do
        log "--- A5 repeat $r/$REPEAT ---"
        cost_run off || true
        cost_run on  || true
    done
    log "  A5 是数值臂不打下钩：两臂的 pvclock_per_s 与 model_upper_per_s 见 $OUT/A5.txt，按 §4.3 的模型核对"
}

# ---------- 前置检查 ----------
preflight() {
    local rc=0 e avail cur fb hist ncore
    log "=== preflight ==="
    log "--- 1) 内核与文档版本 ---"
    log "  uname -r = $(uname -r)（宿主内核；文档行号基于 6.12.93，../measurement.md §5.2）"
    log "  ★ 本实验会**改写宿主 clocksource**，影响整机与所有 VM，不是只读观测"
    lsmod | grep -q '^kvm_intel' || { warn "kvm_intel 未加载"; rc=1; }

    log "--- 2) 时钟源（§2.2） ---"
    if [ ! -e $CS/current_clocksource ]; then
        warn "没有 $CS —— 本实验无法执行"; rc=1
    else
        avail=$(cs_avail); cur=$(cs_cur)
        log "  available =$avail"
        log "  current   =$cur  perm=$(stat -c %a $CS/current_clocksource 2>/dev/null)"
        fb=$FALLBACK
        [ -n "$fb" ] || fb=$(pick_fallback) || { warn "available 里除 tsc* 之外没有别的时钟源，P1 无从验证"; rc=1; }
        [ -n "$fb" ] && log "  将用的非 TSC 基时钟源 = $fb${FALLBACK:+（--fallback 指定）}"
        [ "$cur" = "$fb" ] && { warn "当前就是 $fb，得先让宿主回到 tsc 基，否则 A2 没有'关'可测"; rc=1; }
        case " $avail " in
            *" tsc "*) log "  tsc 仍在 available 里（可选）" ;;
            *) warn "available 里没有 tsc —— 很可能已被看门狗判死，本实验只会加重"; rc=1 ;;
        esac
        [ "$cur" = tsc ] || warn "当前不是 tsc 而是 $cur：A1 的基线（masterclock 1）大概率不成立，先查为什么"
    fi

    log "--- 3) 看门狗风险（§2.1） ---"
    if dmesg >/dev/null 2>&1; then
        hist=$(dmesg 2>/dev/null | grep -ci "marking clocksource.*unstable" || true)
        log "  dmesg 历史 unstable 记录 = ${hist:-0}"
        [ "${hist:-0}" = 0 ] || warn "有 ${hist} 条 unstable 记录：这台机器的 TSC 已在悬崖边，跑之前想清楚能否接受重启"
        dmesg 2>/dev/null | grep -i "clocksource: tsc\|tsc: Detected" | tail -2 | sed 's/^/    dmesg: /' || true
    else
        warn "读不到 dmesg（非 root 或 dmesg_restrict），无法核历史 unstable 记录"
    fi
    grep -q "nowatchdog" /proc/cmdline 2>/dev/null \
        && log "  cmdline 含 nowatchdog：TSC 不进看门狗名单，§2.1 的不可逆风险已消除" \
        || log "  cmdline 无 tsc=nowatchdog → 每次切换都在赌看门狗不误判；反复跑建议加（需重启）"
    log "  cmdline = $(cat /proc/cmdline 2>/dev/null)"

    log "--- 4) 观测出口（§2.4） ---"
    for e in $EV_LIST; do
        [ -d "$TR/events/${e%:*}/${e#*:}" ] && log "  ${e} 存在" || { warn "缺 $e"; rc=1; }
    done
    for e in kvm_update_master_clock kvm_track_tsc; do
        grep -q "offsetmatched" "$TR/events/kvm/$e/format" 2>/dev/null \
            && log "  $e 的 print fmt 含 offsetmatched（§2.5：两处同名不同义，判据按各自读法）" \
            || warn "$e 的 format 里没有 offsetmatched —— 文本 grep 判据要改"
    done
    log "  要按 §2.4 核对宿主 6.8 里那个单向门：nm/objdump 跑在 /lib/modules/$(uname -r)/kernel/arch/x86/kvm/kvm.ko[.zst]"

    log "--- 5) debugfs 与产物 ---"
    if ls $DBK/*/vcpu0/tsc-offset >/dev/null 2>&1; then
        log "  per-vCPU debugfs 布局存在（§4.2 的 tsc-offset 阴性对照可用）"
    else
        log "  当前无 VM，per-vCPU 布局尚不可见 —— 开跑后再判阴性对照是否可用"
    fi
    for e in "$KERNEL" "$INITRD" "$KO_DIR/ple_load.ko" "$SHARE/ple_load.ko"; do
        [ -f "$e" ] || { warn "缺 $e"; rc=1; }
    done
    if [ -f "$SHARE/ple_load.ko" ] && [ -f "$KO_DIR/ple_load.ko" ]; then
        cmp -s "$KO_DIR/ple_load.ko" "$SHARE/ple_load.ko" \
            && log "  共享区 ple_load.ko 与本地构建一致" \
            || warn "共享区的 ple_load.ko 与 $KO_DIR 不一致，guest 会加载旧版本"
    fi
    modinfo -p "$KO_DIR/ple_load.ko" 2>/dev/null | grep -q workload \
        && log "  模块支持 workload=1（无锁私有缓冲区，理由见 bench-migrate.md §3.1）" \
        || warn "模块里没有 workload 参数，先重建 ple_load.ko"
    command -v taskset >/dev/null && log "  taskset 可用" || { warn "缺 taskset（A5 无法注入迁移）"; rc=1; }

    log "--- 6) 核与拓扑（A5） ---"
    ncore=$(pick_cores $((VCPU * 2)) >/dev/null 2>&1 && echo OK || echo FAIL)
    log "  nproc=$(nproc) 需要 $((VCPU * 2)) 个 node$NODE 上的不同物理核 → $ncore"
    [ "$ncore" = OK ] || warn "凑不出核，A5 会失败（A0–A4 不受影响）"
    log "  当前负载：$(uptime | sed 's/.*load average/load/')"

    log "--- 7) ftrace 残留与独占 ---"
    e=$(cat $TR/current_tracer 2>/dev/null)
    [ -z "$e" ] || [ "$e" = nop ] || warn "current_tracer=$e，上一轮没清（AGENTS.md 陷阱 9）"
    e=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c '/dev/kvm') || true
    if [ "${e:-0}" != 0 ]; then warn "已有 ${e} 个 /dev/kvm fd 在跑：A0 的阴性对照与 A4 的按-tid 归属都会被污染"; rc=1; fi
    if [ "$ACCEPT_RISK" = 1 ]; then log "  已带 --i-accept-clocksource-risk：真跑会实际写 current_clocksource"
    else log "  未带 --i-accept-clocksource-risk：真跑在第一次切换前会停手（§2.1）"; fi

    if [ "$rc" = 0 ]; then log "== 前置检查通过 =="; else log "== 前置检查有未通过项，先修再跑 =="; fi
    return $rc
}

# ---------- 参数解析 ----------
while [ $# -gt 0 ]; do
    case $1 in
        --preflight)   ONLY_PREFLIGHT=1; shift ;;
        --dry-run)     DRY=1; shift ;;
        --until)       UNTIL="$2"; shift 2 ;;
        --all)         UNTIL=A5; shift ;;
        --fallback)    FALLBACK="$2"; shift 2 ;;
        --repeat)      REPEAT="$2"; shift 2 ;;
        --sample-s)    SAMPLE_S="$2"; shift 2 ;;
        --step-ms)     STEP_MS="$2"; shift 2 ;;
        --vcpu)        VCPU="$2"; shift 2 ;;
        --priv-kb)     PRIV_KB="$2"; shift 2 ;;
        --node)        NODE="$2"; shift 2 ;;
        --kernel)      KERNEL="$2"; shift 2 ;;
        --i-accept-clocksource-risk) ACCEPT_RISK=1; shift ;;
        -h|--help)     usage ;;
        *)             warn "未知参数 $1"; usage ;;
    esac
done
case $UNTIL in A0|A1|A2|A3|A4|A5) ;; *) die "--until 只能取 A0..A5（时间线不可乱序）";; esac
case $STEP_MS in ''|*[!0-9]*) die "--step-ms 要正整数";; esac
[ "$STEP_MS" -ge 10 ] || die "--step-ms 太小（<10ms）会让注入自身成为瓶颈"
[ "$VCPU" -ge 2 ] || die "--vcpu 至少 2"
[ "$REPEAT" -ge 1 ] || die "--repeat 至少 1"

if [ "$ONLY_PREFLIGHT" = 1 ]; then preflight; exit $?; fi
[ "$(id -u)" = 0 ] || [ "$DRY" = 1 ] || die "需要 root（tracefs / debugfs / clocksource / taskset）"

ORIG_CS=$(cs_cur); [ -n "$ORIG_CS" ] || ORIG_CS=tsc
CUR_CS=$ORIG_CS
FALLBACK_PICK=$FALLBACK
[ -n "$FALLBACK_PICK" ] || FALLBACK_PICK=$(pick_fallback) || FALLBACK_PICK=""
if [ "$DRY" != 1 ]; then
    # ★ 不可逆风险（§2.1）不允许"跳过检查直接跑"：preflight 不过就不碰任何东西
    preflight || die "前置检查未通过，不动系统。先修完再看 §2"
    cs_guard
    [ -n "$FALLBACK_PICK" ] || die "挑不出非 TSC 基的备随时钟源，A2/A3/A5 无从做起（§2.2）"
    mkdir -p "$OUT"
    printf 'orig_clocksource=%s fallback=%s vcpu=%s sample_s=%s step_ms=%s until=%s repeat=%s\n' \
        "$ORIG_CS" "$FALLBACK_PICK" "$VCPU" "$SAMPLE_S" "$STEP_MS" "$UNTIL" "$REPEAT" > "$OUT/env.txt"
fi
[ "$FALLBACK_PICK" = "$ORIG_CS" ] && die "备随时钟源与当前的相同（$ORIG_CS），换不出'非 TSC 基'状态"

log "=== E4 时间线 A0..$UNTIL：原 clocksource=$ORIG_CS 备随时钟源=${FALLBACK_PICK:-<未挑出>} ==="
for p in "${PHASES[@]}"; do
    "phase_$p"
    [ "$p" = "$UNTIL" ] && break
done

log ""
log "判定：${PASS:- 无}${FAIL:+ / 不通过：$FAIL}"
[ -z "$FAIL" ] || warn "A3 不通过=本章结论被否证，必须登记 ../corrections.md（见 §4.1-A3）"
[ "$DRY" = 1 ] && log "输出目录: ${OUT}（--dry-run：未执行任何改动动作，目录未创建）" \
                || log "输出目录: $OUT"
