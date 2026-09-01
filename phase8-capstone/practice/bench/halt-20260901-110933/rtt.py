# 唤醒延迟：guest shell 阻塞在 read(ttyS0)，vCPU halt；
# 经 fifo 写入一个字符 → minivmm input_thread 置 LSR.DR 并
# KVM_IRQ_LINE 注入 IRQ4 → guest 读走并回显 → 串口日志出现该字符。
# RTT = 唤醒路径 + guest 处理 + 串口回传。
# 模式（argv[4]）：
#   idle  —— ping 间睡 100ms + 每次先等日志安静 50ms，vCPU 必定已深睡
#            （halt ≫ 200µs 窗口，考察"冷唤醒"延迟）
#   flood —— 见到回显立即发下一字符，halt 间隔 ≈ RTT（~150µs），
#            落在 halt-polling 窗口内（考察窗口能否接住短 halt）。
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
