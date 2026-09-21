# Phase 10 实战案例

> 端到端诊断案例：从症状到根因，完整展示诊断思路与工具链。

---

## 案例 1：VM 启动后立即崩溃

### 症状

QEMU 启动后几秒内退出，日志显示：
```
qemu-system-x86_64: KVM_RUN failed: Argument list too long
qemu-system-x86_64: error: kvm run failed Bad address
```

`dmesg` 里无异常。

### 诊断过程

**Step 1：检查 KVM_RUN 返回值**

```bash
# 用 strace 跟踪 QEMU 的 KVM 调用
sudo strace -e trace=ioctl -p $(pgrep qemu) 2>&1 | grep KVM_RUN
```

输出：
```
ioctl(9, KVM_RUN, 0) = -1 EFAULT (Bad address)
```

`EFAULT` 表示 guest 内存映射问题。

**Step 2：启用 ftrace 跟踪 KVM ioctl 链路**

```bash
TRACEFS=/sys/kernel/debug/tracing
: > $TRACEFS/set_event
echo function > $TRACEFS/current_tracer
echo kvm_vcpu_ioctl > $TRACEFS/set_ftrace_filter
echo kvm_arch_vcpu_ioctl_run >> $TRACEFS/set_ftrace_filter
echo vcpu_enter_guest >> $TRACEFS/set_ftrace_filter
echo vmx_vcpu_run >> $TRACEFS/set_ftrace_filter
echo 1 > $TRACEFS/tracing_on
```

重启 VM，观察 trace：
```
kvm_vcpu_ioctl: KVM_RUN
kvm_arch_vcpu_ioctl_run: enter vcpu_run
vcpu_enter_guest: prepare VM-Entry
vmx_vcpu_run: VM-Entry failed
```

**Step 3：检查 VM-Entry 失败原因**

```bash
echo kvm:kvm_exit >> $TRACEFS/set_event
cat $TRACEFS/trace_pipe | grep -A5 'VM-Entry'
```

发现：
```
vmx_vcpu_run: VM-Entry failed, error=0
```

`error=0` 表示 VM-Entry 本身成功，但 guest 立即触发异常。

**Step 4：检查 guest 内存映射**

```bash
# 查看 QEMU 的内存映射
cat /proc/$(pgrep qemu)/maps | grep kvm
```

发现 guest RAM 区域标记为 `rw-s`（shared），但其中一个 memslot 的起始地址未对齐：
```
7f1234000000-7f1334000000 rw-s 00000000 00:00 0  /dev/kvm
```

正常应该对齐到 2MB 边界。

### 根因

VMM 设置 `KVM_SET_USER_MEMORY_REGION` 时，`userspace_addr` 未按 hugepage 对齐，导致 EPT 映射失败。Guest 访问未映射区域触发 EPT_VIOLATION，KVM 无法修复，返回 `EFAULT`。

### 修复

确保 `userspace_addr` 对齐到 2MB（如果启用了大页）或至少 4KB：
```c
region.userspace_addr = (uintptr_t)mmap(NULL, size, PROT_READ|PROT_WRITE,
                                         MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,
                                         -1, 0);
// 检查返回值是否对齐
assert((region.userspace_addr & (HPAGE_SIZE - 1)) == 0);
```

### 验证

```bash
# 重新启动 VM
sudo qemu-system-x86_64 -enable-kvm -m 2G ...

# 检查是否正常运行
pgrep qemu  # 应持续存在

# 检查 EPT 缺页
sudo bpftrace -e 'tracepoint:kvm:kvm_page_fault { @count = count(); } interval:s:5 { print(@count); clear(@count); }'
# 缺页数量应在合理范围（启动后几秒内几百到几千次）
```

---

## 案例 2：guest 时钟偶尔跳变 5 秒

### 症状

Guest 内运行 `date` 命令，偶尔时间跳变：
```
# date
Wed Sep  3 10:00:00 UTC 2026
# date  (0.1 秒后)
Wed Sep  3 10:00:05 UTC 2026   ← 跳变 5 秒
# date  (0.1 秒后)
Wed Sep  3 10:00:05 UTC 2026   ← 正常
```

`dmesg` 无异常。

### 诊断过程

**Step 1：检查 guest clocksource**

```bash
# Guest 内
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
# 输出: kvm-clock
```

Guest 使用 kvm-clock，理论上不应跳变。

**Step 2：宿主侧跟踪时钟相关 tracepoint**

```bash
TRACEFS=/sys/kernel/debug/tracing
: > $TRACEFS/set_event
echo kvm:kvm_track_tsc >> $TRACEFS/set_event
echo kvm:kvm_update_master_clock >> $TRACEFS/set_event
echo kvm:kvm_write_tsc_offset >> $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on

# 等待跳变发生，然后查看 trace
cat $TRACEFS/trace | grep -E 'track_tsc|update_master|write_tsc'
```

发现：
```
kvm_track_tsc: vcpu 0 nr_vcpus_matched_tsc 0 online_vcpus 2 use_master_clock 1 host_clock 0
kvm_update_master_clock: use_master_clock 0 host_clock 0   ← masterclock 翻转！
kvm_track_tsc: vcpu 1 nr_vcpus_matched_tsc 0 online_vcpus 2 use_master_clock 0 host_clock 0
```

`use_master_clock` 从 1 变成 0，表示 masterclock 被禁用。

**Step 3：分析 masterclock 翻转原因**

```bash
# 检查 TSC 稳定性
grep -o 'constant_tsc\|tsc_reliable\|nonstop_tsc' /proc/cpuinfo | sort -u
# 输出: constant_tsc, nonstop_tsc, tsc_reliable

# 检查是否有 CPU 热插拔或频率变化
dmesg | grep -i 'tsc\|cpufreq'
```

发现：
```
[ 1234.567] cpufreq: CPU 1 frequency changed from 3000 MHz to 1500 MHz
```

CPU 频率变化导致 TSC 不稳定，KVM 禁用 masterclock。

**Step 4：确认跳变时机**

```bash
# 查看 masterclock 翻转的时间戳
cat $TRACEFS/trace | grep 'update_master_clock.*use_master_clock 0'
# 时间戳: 1234.568

# 查看 cpufreq 变化的时间戳
dmesg | grep 'frequency changed'
# 时间戳: [ 1234.567]
```

两者相差 1ms，确认是 CPU 频率变化触发 masterclock 翻转。

### 根因

宿主 CPU 频率动态调整（cpufreq），导致 TSC 频率短暂不稳定。KVM 检测到 TSC 不同步，禁用 masterclock（`use_master_clock = 0`）。Guest 从 kvm-clock（基于 masterclock）切换到后备时钟源，时间基准变化导致跳变。

`kvm_track_tsc` 的 `masterclock` 字段打的是**翻转前的旧值**（见 `corrections.md` C8），所以看到的是 `use_master_clock 1`，但 `kvm_update_master_clock` 的 `use_master_clock 0` 才是新值。

### 修复

**方案 A**：禁用 cpufreq（推荐用于虚拟化宿主）
```bash
# 设置 CPU 性能模式为 performance
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

**方案 B**：启用 TSC scaling（如果 CPU 支持）
```bash
# 检查 CPU 是否支持 TSC scaling
grep tsc_adjust /proc/cpuinfo  # 如果有，支持 TSC scaling

# KVM 会自动启用 TSC scaling，无需额外配置
```

**方案 C**：Guest 内使用 tsc 而非 kvm-clock
```bash
# Guest 内核参数
clocksource=tsc tsc=reliable
```

### 验证

```bash
# 禁用 cpufreq 后
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# 重新跟踪
: > $TRACEFS/set_event
echo kvm:kvm_update_master_clock >> $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on

# 运行 1 小时，检查是否有 masterclock 翻转
cat $TRACEFS/trace | grep 'update_master_clock' | wc -l
# 应为 0 或极少

# Guest 内检测跳变
while true; do date +%s.%N; sleep 0.1; done | \
    awk 'NR>1{d=$1-prev; if(d<0||d>0.2) print "JUMP:", d} {prev=$1}'
# 应无 JUMP 输出
```

---

## 案例 3：VM-Exit 频率异常高（10 万次/秒）

### 症状

`perf kvm stat` 显示 VM-Exit 率异常高：
```
$ sudo perf kvm stat record -a -- sleep 5
$ sudo perf kvm stat report

  VM-Exit Reason        Count    %
  ──────────────────    ─────    ───
  EXTERNAL_INTERRUPT    450000   90.0%   ← 异常高
  EPT_VIOLATION          30000   6.0%
  CPUID                  20000   4.0%
```

正常 `EXTERNAL_INTERRUPT` 应 < 10%。

### 诊断过程

**Step 1：检查 APICv 是否启用**

```bash
cat /sys/module/kvm_intel/parameters/enable_apicv
# 输出: Y

cat /sys/module/kvm_intel/parameters/enable_ipiv
# 输出: Y
```

APICv 和 IPIv 都已启用，理论上外部中断不应触发 VM-Exit。

**Step 2：用 bpftrace 分析中断来源**

```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_exit /args->exit_reason == 1/ {  // 1 = EXTERNAL_INTERRUPT
    @count = count();
}
tracepoint:kvm:kvm_apic_accept_irq {
    @irq[args->vector] = count();
}
interval:s:5 {
    printf("EXTERNAL_INTERRUPT: %d\n", @count);
    print(@irq, 10);
    clear(@count);
    clear(@irq);
}
'
```

输出：
```
EXTERNAL_INTERRUPT: 90000
@irq[32]: 85000   ← 向量 32 的中断占 94%
@irq[48]: 3000
@irq[64]: 2000
```

向量 32 的中断占绝大多数。

**Step 3：检查向量 32 对应什么设备**

```bash
# Guest 内
cat /proc/interrupts | grep ' 32:'
# 输出: 32:  85000  IR-IO-APIC-32-edge  virtio0

# 是 virtio 网卡的中断
```

**Step 4：检查 Posted Interrupts 配置**

```bash
# 宿主侧
sudo bpftrace -e '
tracepoint:kvm:kvm_pi_irte_update {
    printf("vector=%d vcpu=%d\n", args->vector, args->vcpu_id);
}
' | grep 'vector=32'

# 无输出，说明 PI 未配置
```

Posted Interrupts 未启用，所有外部中断都触发 VM-Exit。

**Step 5：检查 VM 配置**

```bash
# QEMU 启动参数
ps aux | grep qemu
# 输出: qemu-system-x86_64 -enable-kvm -cpu host ... -device virtio-net-pci ...
```

发现 QEMU 启动时**没有** `-machine kernel_irqchip=on`（默认是 `split`），导致 Posted Interrupts 未启用。

### 根因

QEMU 使用 `kernel_irqchip=split` 模式（默认），Posted Interrupts 未启用。virtio 网卡的高频中断（每秒 9 万次）全部触发 VM-Exit，导致性能下降。

### 修复

**方案 A**：启用完整 in-kernel irqchip
```bash
qemu-system-x86_64 -machine kernel_irqchip=on ...
```

**方案 B**：减少 virtio 中断频率
```bash
# Guest 内启用网卡中断合并
ethtool -C eth0 rx-frames 64
```

**方案 C**：使用 vhost-net 减少中断
```bash
qemu-system-x86_64 -netdev tap,id=net0,vhost=on ...
```

### 验证

```bash
# 启用 kernel_irqchip=on 后重启 VM
qemu-system-x86_64 -machine kernel_irqchip=on -enable-kvm ...

# 检查 Posted Interrupts 是否启用
sudo bpftrace -e '
tracepoint:kvm:kvm_pi_irte_update {
    @pi[args->vector] = count();
}
interval:s:5 { print(@pi); clear(@pi); }
'
# 应看到向量 32 的 PI 更新

# 重新统计 VM-Exit
sudo perf kvm stat record -a -- sleep 5
sudo perf kvm stat report
# EXTERNAL_INTERRUPT 应降到 < 10%
```

---

## 总结

| 案例 | 症状 | 根因 | 工具 |
|------|------|------|------|
| VM 启动崩溃 | `KVM_RUN` 返回 EFAULT | 内存未对齐 | strace + ftrace |
| 时钟跳变 5 秒 | `date` 偶尔跳变 | masterclock 翻转 | `kvm_update_master_clock` trace |
| VM-Exit 10 万/秒 | `EXTERNAL_INTERRUPT` 90% | PI 未启用 | bpftrace + `perf kvm stat` |

**诊断思路**：
1. 先确认症状（QEMU 日志、guest 内观测）
2. 选择合适的 tracepoint（ftrace / bpftrace）
3. 分析数据模式（分布、频率、时序）
4. 交叉验证（多个工具、多个 tracepoint）
5. 定位根因并修复
6. 验证修复效果
