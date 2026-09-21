#!/usr/bin/env bash
# 项目4 M1：启动延迟对照（minivmm vs minivmm-tuned vs QEMU q35 / microvm）
# 判据统一：guest /init 打印 MINIVMM_READY（cmdline 带 autotest）。
# minivmm 侧两个里程碑由 VMM 进程内 CLOCK_MONOTONIC 打点（精确）；
# QEMU 侧用宿主墙钟轮询 -serial file 输出（首字节/标记），
# 轮询步长 0.2ms，引入 ≤1ms 系统偏置（两组同法，中位数对比不受影响）。
#
# minivmm-tuned：guest cmdline 追加 8250.nr_uarts=1 i8042.nokbd i8042.noaux。
# 这是**阴性对照**：实测这两组参数都不能消除对应开销——
#   - 8250 窗口 254ms 的根因是 autoconfig_irq() 两次调 probe_irq_on()，
#     各含 msleep(20)+msleep(100)（8250_port.c:1322/1330，kernel/irq/autoprobe.c:61/81），
#     COM1 固定带 UPF_AUTO_IRQ（asm/serial.h:16），与端口数无关；
#   - i8042.nokbd/noaux 拦不住 i8042_probe 里无条件的
#     i8042_controller_init()（drivers/input/serio/i8042.c:1556），
#     CTR 读 500ms 超时（i8042.h I8042_CTL_TIMEOUT×udelay(50)）照付。
# QEMU microvm 免掉这两项靠的是固件信息：FADT IAPC_BOOT_ARCH bit1=0
# （iapc_boot_arch_8042()，qemu include/hw/input/i8042.h:103 → guest
# acpi_parse_fadt()，arch/x86/kernel/acpi/boot.c:983-988 置 FIRMWARE_ABSENT）
# 与 PNPACPI 串口枚举（绕开 ISA autoconfig）。minivmm 不提供 ACPI/PNP 表，
# 该变体用于证明"cmdline 调参补不回缺失的固件信息"。
#
# 用法: ./bench-boot.sh [每实现采样次数，默认 12]
set -u
cd "$(dirname "$0")"

N=${1:-12}
K=../../scripts/images/bzImage
I=../../scripts/images/initramfs.img
APPEND="console=ttyS0 earlyprintk=serial rdinit=/init autotest"
APPEND_TUNED="$APPEND 8250.nr_uarts=1 i8042.nokbd i8042.noaux"
TS=$(date +%Y%m%d-%H%M%S)
OUT=bench/boot-$TS
mkdir -p "$OUT"
CSV=$OUT/boot.csv

now_ns() { date +%s%N; }

run_minivmm() {   # $1=run序号 $2=实现名 $3=cmdline追加
    local err; err=$(mktemp)
    ./minivmm -k "$K" -i "$I" -m 256 -c "$3" \
        >/dev/null 2>"$err" </dev/null
    local init first ready
    init=$(sed -n 's/.*进入 KVM_RUN 循环 +\([0-9.]*\) ms.*/\1/p' "$err")
    first=$(sed -n 's/.*首个串口字节 +\([0-9.]*\) ms.*/\1/p' "$err")
    ready=$(sed -n 's/.*MINIVMM_READY) +\([0-9.]*\) ms.*/\1/p' "$err")
    if [ "$1" != 0 ]; then
        echo "$2,$1,${init:-NA},${first:-NA},${ready:-NA}" >> "$CSV"
        cp "$err" "$OUT/$2-run$1.log"
    fi
    rm -f "$err"
}

run_qemu() {      # $1=machine $2=run序号
    local log; log=$(mktemp)
    local t0 first ready
    t0=$(now_ns); first=; ready=
    timeout 60 qemu-system-x86_64 -enable-kvm -cpu host -m 256 \
        -machine "$1" -kernel "$K" -initrd "$I" -append "$APPEND" \
        -display none -monitor none -serial "file:$log" -no-reboot &
    local qpid=$!
    while kill -0 "$qpid" 2>/dev/null; do
        [ -z "$first" ] && [ -s "$log" ] && first=$(now_ns)
        [ -z "$ready" ] && grep -q MINIVMM_READY "$log" 2>/dev/null \
            && ready=$(now_ns)
        sleep 0.0002
    done
    wait "$qpid" 2>/dev/null
    local f_ms r_ms
    f_ms=$( [ -n "$first" ] && awk "BEGIN{printf \"%.3f\", ($first-$t0)/1e6}" || echo NA )
    r_ms=$( [ -n "$ready" ] && awk "BEGIN{printf \"%.3f\", ($ready-$t0)/1e6}" || echo NA )
    if [ "$2" != 0 ]; then
        echo "qemu-$1,$2,NA,$f_ms,$r_ms" >> "$CSV"
        cp "$log" "$OUT/qemu-$1-run$2.log"
    fi
    rm -f "$log"
}

echo "impl,run,vmminit_ms,first_ms,ready_ms" > "$CSV"
echo "每实现先热身 1 次、再采样 $N 次 → $CSV"

run_minivmm 0 minivmm "$APPEND"                      # 热身
for i in $(seq 1 "$N"); do run_minivmm "$i" minivmm "$APPEND"; done
echo "== minivmm 完成 =="

run_minivmm 0 minivmm-tuned "$APPEND_TUNED"
for i in $(seq 1 "$N"); do run_minivmm "$i" minivmm-tuned "$APPEND_TUNED"; done
echo "== minivmm-tuned 完成 =="

run_qemu q35 0
for i in $(seq 1 "$N"); do run_qemu q35 "$i"; done
echo "== qemu-q35 完成 =="

run_qemu microvm 0
for i in $(seq 1 "$N"); do run_qemu microvm "$i"; done
echo "== qemu-microvm 完成 =="

awk -F, 'NR>1 && $5!="NA" {
    k=$1; n[k]++; a[k,n[k]]=$5; f[k,n[k]]=$4
} END {
    printf "%-14s %6s %10s %10s %10s | %10s\n",
           "impl","n","first_med","ready_med","ready_min","ready_max"
    for (k in n) {
        m=n[k]
        for (i=1;i<=m;i++) for (j=i+1;j<=m;j++)
            if (a[k,j]<a[k,i]) {t=a[k,i];a[k,i]=a[k,j];a[k,j]=t}
        for (i=1;i<=m;i++) for (j=i+1;j<=m;j++)
            if (f[k,j]<f[k,i]) {t=f[k,i];f[k,i]=f[k,j];f[k,j]=t}
        fm=(m%2)? a[k,(m+1)/2] : (a[k,m/2]+a[k,m/2+1])/2
        fmed=(m%2)? f[k,(m+1)/2] : (f[k,m/2]+f[k,m/2+1])/2
        printf "%-14s %6d %10.1f %10.1f %10.1f | %10.1f\n",
               k,m,fmed,fm,a[k,1],a[k,m]
    }
}' "$CSV" | column -t
