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
