#!/usr/bin/env bash
# 项目4 M2：VM-Exit 分布对照（minivmm vs QEMU q35 / microvm）
# 场景：
#   boot  —— 启动全程（autotest 判据，perf kvm stat record 包住整个进程）
#   idle  —— 无 autotest 常驻 shell，稳定后采样 DUR 秒
#   busy  —— 向 guest shell 注入 `while :; do :; done`，采样 DUR 秒
# 采样工具：perf kvm stat（record 结束后立即 report；期间宿主上只允许这一个
# KVM guest 在跑，否则计数混入其他 VM）。
# 用法: ./bench-exits.sh [稳态采样秒数，默认 15]
set -u
cd "$(dirname "$0")"

DUR=${1:-15}
K=../../scripts/images/bzImage
I=../../scripts/images/initramfs.img
APPEND="console=ttyS0 earlyprintk=serial rdinit=/init"
TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/exits-$TS
mkdir -p "$OUT"

CLK_TCK=$(getconf CLK_TCK)

wait_marker() {   # $1=串口日志，等 /init 的信息横幅出现（shell 就绪）
    for _ in $(seq 1 300); do
        grep -q "^-------------------" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "!! 等待 $1 就绪超时" >&2
    return 1
}

cpu_pct() {       # $1=pid $2=秒：采样区间内进程 CPU 占用%
    local a b
    a=$(awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null) || { echo NA; return; }
    sleep "$2"
    b=$(awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null) || { echo NA; return; }
    awk "BEGIN{printf \"%.1f\", ($b-$a)*100/($2*$CLK_TCK)}"
}

clear_trace() {   # perf kvm stat 的数据走 perf.data.guest 文件（record 写、
    echo > /sys/kernel/tracing/trace   # report 读，builtin-kvm.c:602-614），
}                       # 与 tracefs 缓冲无关；这里清空只是给其他 ftrace
                        # 使用者留干净起点

kvm_report() {    # $1=输出文件
    perf kvm stat report --stdio > "$1" 2>&1
}

# ---------- 场景1：启动全程 ----------
run_boot() {      # $1=impl $2...=VMM 命令
    local impl=$1; shift
    echo "== boot: $impl =="
    clear_trace
    perf kvm stat record -- "$@" >/dev/null 2>&1 </dev/null
    kvm_report "$OUT/boot-$impl.txt"
    head -6 "$OUT/boot-$impl.txt" | tail -3
}

# ---------- 场景2/3：稳态 ----------
run_steady() {    # $1=impl $2=mode(idle|busy)
    local impl=$1 mode=$2
    local log=$OUT/$impl-$mode-serial.log
    local pid cpu
    echo "== $mode: $impl =="
    case $impl in
    minivmm)
        mkfifo "$OUT/mv-$mode.in"
        ./minivmm -k "$K" -i "$I" -m 256 -c "$APPEND" \
            < "$OUT/mv-$mode.in" > "$log" 2>"$OUT/$impl-$mode.err" &
        pid=$!
        exec 7> "$OUT/mv-$mode.in"     # 保持写端，供注入
        ;;
    qemu-*)
        local mach=${impl#qemu-}
        # -serial pipe: POSIX 后端只 O_RDWR 打开已存在的 <path>.in/.out，
        # 不会创建（qemu chardev/char-pipe.c:132-150），必须先 mkfifo
        mkfifo "$OUT/qp-$mode.in" "$OUT/qp-$mode.out"
        qemu-system-x86_64 -enable-kvm -cpu host -m 256 -machine "$mach" \
            -kernel "$K" -initrd "$I" -append "$APPEND" \
            -display none -monitor none -no-reboot \
            -serial pipe:"$OUT/qp-$mode" > "$OUT/$impl-$mode.err" 2>&1 &
        pid=$!
        cat "$OUT/qp-$mode.out" > "$log" &
        local catpid=$!
        exec 7> "$OUT/qp-$mode.in"
        ;;
    esac
    wait_marker "$log" || { kill "$pid" 2>/dev/null; return 1; }
    sleep 1
    if [ "$mode" = busy ]; then
        printf 'while :; do :; done\n' >&7
        sleep 2
    fi
    cpu=$(cpu_pct "$pid" 3)            # 先测 3s CPU，与退出采样错开
    clear_trace
    # -a 必须：不加时 record 只跟踪被包裹的 sleep 进程（builtin-kvm.c:1959
    # 仅在 target 为空时才默认 system_wide），vCPU 线程的退出全丢
    perf kvm stat record -a -- sleep "$DUR" >/dev/null 2>&1
    kvm_report "$OUT/$mode-$impl.txt"
    echo "$impl $mode cpu%=$cpu" >> "$OUT/cpu.txt"
    exec 7>&-
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    [ -n "${catpid:-}" ] && { kill "$catpid" 2>/dev/null; catpid=; }
    rm -f "$OUT/mv-$mode.in" "$OUT/qp-$mode.in" "$OUT/qp-$mode.out"
}

echo "输出目录: $OUT (DUR=${DUR}s)"

run_boot minivmm ./minivmm -k "$K" -i "$I" -m 256 -c "$APPEND autotest"
run_boot qemu-q35 qemu-system-x86_64 -enable-kvm -cpu host -m 256 \
    -machine q35 -kernel "$K" -initrd "$I" -append "$APPEND autotest" \
    -display none -monitor none -serial file:"$OUT/boot-qemu-q35-serial.log" -no-reboot
run_boot qemu-microvm qemu-system-x86_64 -enable-kvm -cpu host -m 256 \
    -machine microvm -kernel "$K" -initrd "$I" -append "$APPEND autotest" \
    -display none -monitor none -serial file:"$OUT/boot-qemu-microvm-serial.log" -no-reboot

for m in idle busy; do
    for impl in minivmm qemu-q35 qemu-microvm; do
        run_steady "$impl" "$m"
    done
done

echo "== CPU 占用 =="; cat "$OUT/cpu.txt"
echo "== 报告文件 =="; ls "$OUT"/*.txt
