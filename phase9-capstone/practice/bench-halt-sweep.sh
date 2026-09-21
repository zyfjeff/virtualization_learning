#!/usr/bin/env bash
# 项目4 M3 补充：halt-polling 窗口扫描（flood 短 halt 负载下，窗口 × CPU/延迟）
#
# 固定窗口技巧：per-vCPU 窗口初始 0，首次短 halt 走 grow 分支
# （kvm_main.c:3872-3874）；grow=1 时 val = val*1 不变，
# 但 0 < grow_start 会被抬到 grow_start（kvm_main.c:3680-3682），
# shrink=1 时 val/1 也不变 —— 于是 (grow=1, grow_start=NS, shrink=1)
# 把窗口钉死在 NS。NS=0 即纯关。
#
# 与 bench-halt.sh 的关系：那边测了 poll 关（0）与自适应（0→80µs）
# 两个端点；这里补 50k/100k/200k 三个固定点，得到完整的
# "窗口 × 代价" 曲线。负载同为 800 字符 flood（< busybox lineedit
# 行缓冲 1024，见 bench-halt.sh 头注）。
set -u
cd "$(dirname "$0")"

K=../../scripts/images/bzImage
I=../../scripts/images/initramfs.img
APPEND="console=ttyS0 earlyprintk=serial rdinit=/init"
P=/sys/module/kvm/parameters
TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/halt-sweep-$TS
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

cat > "$OUT/rtt.py" <<'PYEOF'
# flood：见回显即发下一字符；总量 < 1024（busybox lineedit 行缓冲上限）。
# 连续 3 次超时中止。用法: rtt.py <log> <fifo> <n>
import os, sys, time, statistics

logp, fifop, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
log = open(logp, 'rb')
log.seek(0, 2)
fifo = os.open(fifop, os.O_WRONLY | os.O_NONBLOCK)
chars = b"abcdefghjkmnpqrstuvwxyz"
rtt, timeout, fails = [], 0, 0
for i in range(n):
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
        time.sleep(0.00002)
    if got:
        fails = 0
    else:
        timeout += 1
        fails += 1
        if fails >= 3:
            print(f"!! 连续 3 次超时，中止于 i={i}", file=sys.stderr)
            break
if rtt:
    rtt.sort()
    print(f"n={len(rtt)} timeout={timeout} med={statistics.median(rtt):.1f}us "
          f"min={rtt[0]:.1f}us p90={rtt[int(len(rtt)*0.9)]:.1f}us max={rtt[-1]:.1f}us")
else:
    print(f"n=0 timeout={timeout} (全部超时)")
PYEOF

run_one() {    # $1=窗口值(ns)
    local ns=$1 cfg="w$ns"
    echo; echo "======== 窗口 $ns ns ========"
    if [ "$ns" -eq 0 ]; then
        set_params 0 2 10000 2
    else
        set_params "$ns" 1 "$ns" 1
    fi

    mkfifo "$OUT/mv-$cfg.in"
    ./minivmm -k "$K" -i "$I" -m 256 -c "$APPEND" \
        < "$OUT/mv-$cfg.in" > "$OUT/serial-$cfg.log" 2>"$OUT/err-$cfg.log" &
    local pid=$!
    exec 7> "$OUT/mv-$cfg.in"
    if ! wait_marker "$OUT/serial-$cfg.log"; then kill "$pid"; exec 7>&-; return 1; fi
    sleep 1

    local a b t0 t1
    a=$(awk '{print $14+$15}' "/proc/$pid/stat")
    t0=$(date +%s%N)
    python3 "$OUT/rtt.py" "$OUT/serial-$cfg.log" "$OUT/mv-$cfg.in" 800 \
        | tee /dev/stderr | sed "s/^/w$ns /" >> "$OUT/summary.txt"
    b=$(awk '{print $14+$15}' "/proc/$pid/stat")
    t1=$(date +%s%N)
    awk "BEGIN{printf \"flood cpu%%=%.1f 耗时=%.1fs\n\", ($b-$a)*100/(($t1-$t0)/1e9*$CLK_TCK), ($t1-$t0)/1e9}" \
        | tee /dev/stderr | sed "s/^ */w$ns /" >> "$OUT/summary.txt"

    exec 7>&-
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    rm -f "$OUT/mv-$cfg.in"
}

echo "输出目录: $OUT"
echo "实验前参数: $ORIG"

for ns in 50000 100000 200000; do
    run_one "$ns"
done

echo; echo "== 汇总 =="; cat "$OUT/summary.txt"
