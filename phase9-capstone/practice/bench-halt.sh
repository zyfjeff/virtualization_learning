#!/usr/bin/env bash
# 项目4 M3：halt-polling 调参实验（virt/kvm/kvm_main.c:78-95 的四个参数）
#
# 配置：
#   A（polling 开）: halt_poll_ns=200000 grow=2 grow_start=10000 shrink=2
#       —— 6.12 源码默认（kvm_main.c:93 shrink 默认 2）
#   B（polling 关）: halt_poll_ns=0
# 本机实验前残留值是 200000/2/10000/0（shrink=0：poll 未命中即清零窗口，
# kvm_main.c:3696-3697），脚本结束按原样恢复。
#
# Guest：minivmm 常驻 shell（无 autotest），单 vCPU 空转 halt。
# 指标（每配置各采一遍，顺序执行避免互相污染）：
#   1) VMM 进程 CPU%（5s）—— 忙等代价
#   2) perf kvm stat -a 10s —— HLT / EXTERNAL_INTERRUPT 退出分布
#   3) kvm:kvm_halt_poll_ns tracepoint 3s —— 窗口 grow/shrink 轨迹
#   4) 串口唤醒延迟 ×20（冷，间隔 100ms）—— 唤醒路径 + guest 处理
#   5) flood：连发 800 字符（间隔 ≈ RTT，短 halt），同测 CPU% 与
#      窗口轨迹——只有短 halt 才能让窗口增长（kvm_main.c:3872 条件）。
#      字符数上限 800 < 1024：guest 的 busybox ash lineedit 行缓冲约
#      1024 字节，超出后字符照收但不回显（宿主 pty 实验复现），
#      3000 连发曾因此在中途停摆、全部超时。
set -u
cd "$(dirname "$0")"

K=../../scripts/images/bzImage
I=../../scripts/images/initramfs.img
APPEND="console=ttyS0 earlyprintk=serial rdinit=/init"
P=/sys/module/kvm/parameters
TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/halt-$TS
mkdir -p "$OUT"
CLK_TCK=$(getconf CLK_TCK)

ORIG="$(cat $P/halt_poll_ns) $(cat $P/halt_poll_ns_grow) $(cat $P/halt_poll_ns_grow_start) $(cat $P/halt_poll_ns_shrink)"
set_params() {
    echo "$1" > $P/halt_poll_ns
    echo "$2" > $P/halt_poll_ns_grow
    echo "$3" > $P/halt_poll_ns_grow_start
    echo "$4" > $P/halt_poll_ns_shrink
    echo "params: ns=$(cat $P/halt_poll_ns) grow=$(cat $P/halt_poll_ns_grow) start=$(cat $P/halt_poll_ns_grow_start) shrink=$(cat $P/halt_poll_ns_shrink)"
}
restore() {
    echo "== 恢复实验前参数: $ORIG =="
    set -- $ORIG
    set_params "$1" "$2" "$3" "$4"
}
trap restore EXIT

wait_marker() {
    for _ in $(seq 1 300); do
        grep -q "^-------------------" "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "!! 等待 $1 就绪超时" >&2
    return 1
}

cpu_pct() {
    local a b
    a=$(awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null) || { echo NA; return; }
    sleep "$2"
    b=$(awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null) || { echo NA; return; }
    awk "BEGIN{printf \"%.1f\", ($b-$a)*100/($2*$CLK_TCK)}"
}

clear_trace() {
    echo > /sys/kernel/tracing/trace
}

cat > "$OUT/rtt.py" <<'PYEOF'
# 唤醒延迟：guest shell 阻塞在 read(ttyS0)，vCPU halt；
# 经 fifo 写入一个字符 → minivmm input_thread 置 LSR.DR 并
# KVM_IRQ_LINE 注入 IRQ4 → guest 读走并回显 → 串口日志出现该字符。
# RTT = 唤醒路径 + guest 处理 + 串口回传。
# 模式（argv[4]）：
#   idle  —— ping 间睡 100ms + 每次先等日志安静 50ms，vCPU 必定已深睡
#            （halt ≫ 200µs 窗口，考察"冷唤醒"延迟）
#   flood —— 见到回显立即发下一字符，发送间隔 ≈ RTT；guest 两次字符间
#            halt ≤80µs（实测：自适应窗口涨到 80µs 后稳定），落在
#            halt-polling 窗口内（考察窗口能否接住短 halt）。
#            总量必须 < 1024：guest busybox ash lineedit 行缓冲约
#            1024 字节，超出后字符照收不回显，flood 会静默停摆。
# 看门狗：连续 3 次超时即中止，避免停摆后按 2s/次空耗。
import os, sys, time, statistics

logp, fifop, n, mode = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
log = open(logp, 'rb')
log.seek(0, 2)
fifo = os.open(fifop, os.O_WRONLY | os.O_NONBLOCK)
chars = b"abcdefghjkmnpqrstuvwxyz"
rtt, timeout, fails = [], 0, 0
for i in range(n):
    if mode == "idle":
        while True:                   # 等日志安静 50ms，确认 vCPU 已回落 halt
            pos = log.tell()
            time.sleep(0.05)
            log.seek(0, 2)
            if log.tell() == pos:
                break
    c = bytes([chars[i % len(chars)]])
    t0 = time.perf_counter_ns()
    os.write(fifo, c)
    buf = b''
    got = False
    while time.perf_counter_ns() - t0 < 2_000_000_000:
        r = log.read(64)
        if r:
            buf += r
            if c in buf:
                rtt.append((time.perf_counter_ns() - t0) / 1e3)
                got = True
                break
        time.sleep(0.00002 if mode == "flood" else 0.0001)
    if got:
        fails = 0
    else:
        timeout += 1
        fails += 1
        if fails >= 3:
            print(f"!! 连续 3 次超时，中止于 i={i}", file=sys.stderr)
            break
    if mode == "idle":
        time.sleep(0.1)
if rtt:
    rtt.sort()
    print(f"n={len(rtt)} timeout={timeout} med={statistics.median(rtt):.1f}us "
          f"min={rtt[0]:.1f}us p90={rtt[int(len(rtt)*0.9)]:.1f}us max={rtt[-1]:.1f}us")
else:
    print(f"n=0 timeout={timeout} (全部超时)")
PYEOF

run_config() {    # $1=配置名 $2..$5=四个参数
    local cfg=$1
    echo; echo "======== 配置 $cfg ========"
    set_params "$2" "$3" "$4" "$5"

    mkfifo "$OUT/mv-$cfg.in"
    ./minivmm -k "$K" -i "$I" -m 256 -c "$APPEND" \
        < "$OUT/mv-$cfg.in" > "$OUT/serial-$cfg.log" 2>"$OUT/err-$cfg.log" &
    local pid=$!
    exec 7> "$OUT/mv-$cfg.in"
    if ! wait_marker "$OUT/serial-$cfg.log"; then kill "$pid"; exec 7>&-; return 1; fi
    sleep 1

    # 1) CPU%
    local cpu=$(cpu_pct "$pid" 5)
    echo "$cfg cpu%=$cpu" | tee -a "$OUT/summary.txt"

    # 2) 退出分布（-a 原因同 bench-exits.sh：包裹的 sleep 不是 VMM 进程）
    clear_trace
    perf kvm stat record -a -- sleep 10 >/dev/null 2>&1
    perf kvm stat report --stdio > "$OUT/kvmstat-$cfg.txt" 2>&1
    grep -E "HLT|EXTERNAL_INTERRUPT|Total" "$OUT/kvmstat-$cfg.txt" | sed 's/^/    /'

    # 3) halt_poll_ns 轨迹
    echo 1 > /sys/kernel/tracing/events/kvm/kvm_halt_poll_ns/enable
    clear_trace
    sleep 3
    cat /sys/kernel/tracing/trace > "$OUT/haltpoll-$cfg.trace"
    echo 0 > /sys/kernel/tracing/events/kvm/kvm_halt_poll_ns/enable
    awk '/kvm_halt_poll_ns/ {if ($0 ~ /grow /) g++; else if ($0 ~ /shrink /) s++}
         END{printf "    haltpoll 轨迹: grow=%d shrink=%d\n", g+0, s+0}' \
        "$OUT/haltpoll-$cfg.trace"
    tail -5 "$OUT/haltpoll-$cfg.trace" | sed 's/^/    /'

    # 4) 冷唤醒延迟（ping 间隔 100ms，halt ≫ 窗口）
    printf '    唤醒RTT(idle): '
    python3 "$OUT/rtt.py" "$OUT/serial-$cfg.log" "$OUT/mv-$cfg.in" 20 idle \
        | tee /dev/stderr | sed "s/^/$cfg 冷唤醒 /" >> "$OUT/summary.txt"

    # 5) flood：短 halt（≈RTT）负载，窗口能否接住；同时测 CPU 代价。
    #    800 字符 < lineedit 行缓冲上限（见文件头注释）
    echo 1 > /sys/kernel/tracing/events/kvm/kvm_halt_poll_ns/enable
    clear_trace
    local a b t0 t1
    a=$(awk '{print $14+$15}' "/proc/$pid/stat")
    t0=$(date +%s%N)
    python3 "$OUT/rtt.py" "$OUT/serial-$cfg.log" "$OUT/mv-$cfg.in" 800 flood \
        | tee /dev/stderr | sed "s/^/$cfg flood RTT /" >> "$OUT/summary.txt"
    b=$(awk '{print $14+$15}' "/proc/$pid/stat")
    t1=$(date +%s%N)
    cat /sys/kernel/tracing/trace > "$OUT/haltpoll-$cfg-flood.trace"
    echo 0 > /sys/kernel/tracing/events/kvm/kvm_halt_poll_ns/enable
    awk "BEGIN{printf \"    flood cpu%%=%.1f 耗时=%.1fs\n\", ($b-$a)*100/(($t1-$t0)/1e9*$CLK_TCK), ($t1-$t0)/1e9}" \
        | tee /dev/stderr | sed "s/^ */$cfg /" >> "$OUT/summary.txt"
    awk '/kvm_halt_poll_ns/ {if ($0 ~ /grow /) g++; else if ($0 ~ /shrink /) s++}
         END{printf "    flood haltpoll 轨迹: grow=%d shrink=%d\n", g+0, s+0}' \
        "$OUT/haltpoll-$cfg-flood.trace" | tee /dev/stderr | sed "s/^ */$cfg /" >> "$OUT/summary.txt"

    exec 7>&-
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    rm -f "$OUT/mv-$cfg.in"
}

echo "输出目录: $OUT"
echo "实验前参数: $ORIG"

run_config poll-on  200000 2 10000 2
run_config poll-off 0      2 10000 2

echo; echo "== 汇总 =="; cat "$OUT/summary.txt"
