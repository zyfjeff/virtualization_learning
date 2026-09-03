# 时钟源性能与精度基准

> 目标：对比 guest 内各 clocksource（kvm-clock / tsc / hpet / acpi_pm）
> 的读取延迟与定时器精度，验证 TSC 稳定性与 `lapic_timer_advance` 效果。
> 理论背景见 `../../phase7-timer-virt/README.md`（clocksource vs
> clockevent、kvmclock/pvclock、TSC-deadline）。
> 本文只给方法与观测点，**所有数字以实测为准**（旧版文档曾给出编造的
> "典型值"表，已全部删除）。

> **⚠ 实验 6 在 6.12.93 上不可执行，别照抄。** 6.12.93 只有
> `lapic_timer_advance`（`bool`，`arch/x86/kvm/lapic.c:70-71`，0444 只读）与
> per-vCPU 的 debugfs 只读文件 `lapic_timer_advance_ns`
> （`debugfs_create_file("lapic_timer_advance_ns", 0444, …)`，
> `arch/x86/kvm/debugfs.c:67`）—— **手动设固定提前量做不到**，
> 那两行 `cat /sys/module/kvm/parameters/lapic_timer_advance_ns` 是宿主
> 6.8.0-51 的布局（详见 [`../corrections.md`](../corrections.md) D1）。
> 主时钟启停与 timer advance 的可执行版本是
> [`bench-clock-master.md`](bench-clock-master.md)（E4，含驱动脚本）。
> 其余实验（clocksource 读取延迟、cyclictest、TSC 多 CPU 同步）仍是有效的
> **guest 侧手工手段**；机制与已实测的结论一律看 `../../phase7-timer-virt/`，
> 本文不重复。

---

## 实验环境

```bash
qemu-system-x86_64 \
    -enable-kvm -m 2G -smp 2 -cpu host \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &
QEMU_PID=$!
```

Guest 内：

```bash
cat /sys/devices/system/clocksource/clocksource0/available_clocksource
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
grep -oE "constant_tsc|nonstop_tsc|tsc_deadline_timer" /proc/cpuinfo | sort -u
```

注意：PIT 不作为 clocksource 出现在列表中（它是 clockevent，
概念区分见 `../../phase7-timer-virt/README.md`）。

## 实验 1：时钟源切换验证

```bash
for src in kvm-clock tsc hpet acpi_pm; do
    echo $src > /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null
    actual=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)
    [ "$actual" = "$src" ] && echo "✓ $src" || echo "✗ 请求=$src 实际=$actual"
    date; uptime -p   # 时间不应跳变
done
echo kvm-clock > /sys/devices/system/clocksource/clocksource0/current_clocksource
```

## 实验 2：读取延迟基准（clock_gettime 微基准）

```c
/* clock_gettime_bench.c — 测当前 clocksource 的读取成本 */
#include <stdio.h>
#include <time.h>
int main(void) {
    int n = 10000000;
    struct timespec start, end, ts;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < n; i++) clock_gettime(CLOCK_MONOTONIC, &ts);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double s = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("%.2f ns/call\n", s * 1e9 / n);
    return 0;
}
```

对每个可切换的 clocksource 编译运行（`gcc -O2`），记录表格：
`时钟源 × ns/call × 是否 VM-Exit`。判断是否退出的方法见实验 4；
kvm-clock/tsc 走 vDSO 不退出，hpet/acpi_pm 每次读都是真实访问。

## 实验 3：定时器精度（cyclictest）

```bash
# Guest 内（apt install rt-tests）
cyclictest -t1 -p 80 -n -i 1000 -l 10000 -q
```

对每个 clocksource 跑一遍，记录 avg/max latency 与直方图形状。
注意：cyclictest 的精度主要取决于 **clockevent 设备**（lapic timer
TSC-deadline 模式）而非 clocksource，实验设计时把两个概念分开记录
（对照 `../../phase7-timer-virt/README.md` 的概念区分）。

## 实验 4：定时器相关 VM-Exit 追踪

```bash
# 宿主侧
TRACEFS=/sys/kernel/debug/tracing
echo > $TRACEFS/trace
echo kvm:kvm_exit > $TRACEFS/set_event
echo kvm:kvm_entry >> $TRACEFS/set_event
echo $QEMU_PID > $TRACEFS/set_event_pid 2>/dev/null || true
echo 1 > $TRACEFS/tracing_on
# Guest 内跑负载: stress-ng --cpu 1 --timeout 5
sleep 6
echo 0 > $TRACEFS/tracing_on

# 退出原因分布
cat $TRACEFS/trace | grep kvm_exit | awk '{print $NF}' | \
    sort | uniq -c | sort -rn | head -15
```

（`kvm_exit` 是 6.12.93 的标准 tracepoint，见
`../../phase10-debugging/annotations.md` §1.1。）

## 实验 5：TSC 多 CPU 同步稳定性

用 `sched_setaffinity` 把读 `rdtsc` 的线程逐个绑到各 CPU，比较读数差；
同时记录内核自检结论：

```bash
dmesg | grep -iE "tsc|clocksource" | tail -20
grep -E "constant_tsc|nonstop_tsc|tsc_deadline_timer" /proc/cpuinfo | sort -u
```

判定：差异持续增长或 `tsc unstable` 出现 → 该环境不应以 tsc 为
clocksource（迁移场景同理，见下文决策树）。

## 实验 6：lapic_timer_advance 调参

`lapic_timer_advance` 让 KVM 提前触发 lapic timer 以抵消注入延迟
（背景见 `../../phase7-timer-virt/README.md` 的 timer 章节）：

```bash
ls /sys/module/kvm/parameters/ | grep lapic_timer_advance
cat /sys/module/kvm/parameters/lapic_timer_advance_ns 2>/dev/null

# 对比: 默认(自适应) / 手动设固定值 / 关闭
# 每档跑实验 3 的 cyclictest，记录 avg/max 变化
```

---

## 时钟源选择决策树（结论框架）

```
Guest 内核支持 kvm-clock（CPUID 0x40000001 bit3）?
  ├─ 是 → 用 kvm-clock（迁移友好、多 vCPU 一致，由 pvclock 共享页供数）
  └─ 否 → constant_tsc?
        ├─ 有 → tsc（无 VM-Exit 读取；注意迁移后需 offset 重同步）
        └─ 无 → hpet / acpi_pm（兼容后备，读取有退出）
实时低延迟负载 → 在上述基础上确保 clockevent 走 TSC-deadline 模式
```

## 报告要求

1. 实测的 `时钟源 × 读取延迟 × 是否退出` 表
2. 各 clocksource 下的 cyclictest 结果（区分 clocksource 与
   clockevent 的贡献）
3. TSC 同步测试结论
4. `lapic_timer_advance` 三档对比数据
5. 遇到时间跳变/降级时的排查记录（清单见
   `../../phase10-debugging/README.md` 场景 6）
