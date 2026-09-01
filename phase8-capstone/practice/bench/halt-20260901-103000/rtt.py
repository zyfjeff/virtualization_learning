# 唤醒延迟：guest shell 阻塞在 read(ttyS0)，vCPU halt；
# 经 fifo 写入一个字符 → minivmm input_thread 置 LSR.DR 并
# KVM_IRQ_LINE 注入 IRQ4 → guest 读走并回显 → 串口日志出现该字符。
# RTT = 唤醒路径 + guest 处理 + 串口回传。
import os, sys, time, statistics

logp, fifop, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
log = open(logp, 'rb')
log.seek(0, 2)
fifo = os.open(fifop, os.O_WRONLY | os.O_NONBLOCK)
chars = b"abcdefghjkmnpqrstuvwxyz"
rtt, timeout = [], 0
for i in range(n):
    while True:                       # 等日志安静 50ms，确认 vCPU 已回落 halt
        pos = log.tell()
        time.sleep(0.05)
        log.seek(0, 2)
        if log.tell() == pos:
            break
    c = bytes([chars[i % len(chars)]])
    t0 = time.perf_counter_ns()
    os.write(fifo, c)
    buf = b''
    while time.perf_counter_ns() - t0 < 2_000_000_000:
        r = log.read(64)
        if r:
            buf += r
            if c in buf:
                rtt.append((time.perf_counter_ns() - t0) / 1e3)
                break
        time.sleep(0.0001)
    else:
        timeout += 1
    time.sleep(0.1)
if rtt:
    rtt.sort()
    print(f"n={len(rtt)} timeout={timeout} med={statistics.median(rtt):.1f}us "
          f"min={rtt[0]:.1f}us p90={rtt[int(len(rtt)*0.9)]:.1f}us max={rtt[-1]:.1f}us")
else:
    print(f"n=0 timeout={timeout} (全部超时)")
