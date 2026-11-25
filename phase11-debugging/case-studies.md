# Phase 11 实战案例

> 端到端诊断案例：从症状到根因，完整展示诊断思路与工具链。
>
> ★ 本文三个案例的**数值都是示意**（不是本机实测），只用来演示"怎么看"；
> 真正落地的数字必须按 `../phase10-performance/measurement.md` 的纪律重测。
> 命令、trace 字段名、机制描述则一律以 6.12.93 源码 + 宿主 `format` 为准。

---

## 案例 1：VM 启动后立即崩溃

### 症状

QEMU 启动后几秒内退出，stderr 显示：
```
error: kvm run failed Bad address
```

`dmesg` 里无异常。

> ★ 旧版这里还写了 `KVM_RUN failed: Argument list too long`（= `strerror(-E2BIG)`），
> 已删：KVM 的 `KVM_RUN` 不会用 `-E2BIG` 表示 VM-Entry 失败。
> VM-Entry 失败是经 `run->exit_reason = KVM_EXIT_FAIL_ENTRY` 上报的
> （`arch/x86/kvm/vmx/vmx.c:6510-6515`、`:6517-6524`，ioctl 返回 0），
> QEMU 对应打印是 `KVM: entry failed, hardware error 0x%…`
> （`target/i386/kvm/kvm.c:6096-6099`）。
> `-E2BIG` 在 KVM 里另有出处（如 `KVM_DIRTY_RING` 尺寸超限，
> `virt/kvm/kvm_main.c:4970-4972`），和 guest 状态无关。
> 本例的 `Bad address`（`-EFAULT`）来自 QEMU 侧
> `accel/kvm/kvm-all.c:3227` 的 `error: kvm run failed %s`。

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

重启 VM，观察 trace。**function tracer 的真实格式是"缩进 + 函数名();"，
带 `comm-PID [CPU] flags 时间戳:` 前缀**，不是旧文档里那种 `函数: 说明文字`：

```
 qemu-system-x86- 771290 [003] d..1  1234.567890: kvm_vcpu_ioctl <-__x64_sys_ioctl
 qemu-system-x86- 771290 [003] d..1  1234.567893: kvm_arch_vcpu_ioctl_run <-kvm_vcpu_ioctl
 qemu-system-x86- 771290 [003] d..1  1234.567896: vcpu_enter_guest <-kvm_arch_vcpu_ioctl_run
 qemu-system-x86- 771290 [003] d..1  1234.567902: vmx_vcpu_run <-vcpu_enter_guest
```

**Step 3：检查 VM-Entry 失败原因**

```bash
echo kvm:kvm_userspace_exit >> $TRACEFS/set_event
cat $TRACEFS/trace_pipe | grep kvm_userspace_exit
```

发现（`kvm_userspace_exit` 的 `TP_printk` 是 `reason %s (%d)`，
字段只有 `reason` + `errno`，**没有 vcpu_id**；见 `include/trace/events/kvm.h:22-41`）：

```
 kvm_vcpu: 771291: kvm_userspace_exit: reason KVM_EXIT_FAIL_ENTRY (9)
```

再看 QEMU stderr（**不是** `-D` 日志文件）：

```
KVM: entry failed, hardware error 0x80000021
```

`0x80000000` 位是 `exit_reason.failed_vmentry`（`vmx.h` 的 `VMX_EXIT_REASONS`/FLAGS），
低 16 位 `0x21` 是基本退出号 —— 这里对应
`EXIT_REASON_INVALID_STATE = 33`（`arch/x86/include/uapi/asm/vmx.h:62`）。

**Step 4：检查 guest 内存映射**

```bash
grep -E 'kvm|anon' /proc/$(pgrep -f '^qemu-system-x86_64' | head -1)/maps | head
```

★ 注意映射的**归属**（本机实测）：guest RAM 是普通的私有匿名/大页映射
（`rw-p ... 00:00 0`，无文件后缀），**`/dev/kvm` 不会出现在 maps 里**；
出现的是 `anon_inode:kvm-vcpu:<n>` 的 run 结构映射，每 vCPU 几页：

```
7474ff516000-7474ff519000 rw-s 00:00 0 anon_inode:kvm-vcpu:1     ← 3 页，vcpu->run
7474b7e00000-7474f7e00000 rw-p 00:00 0                           ← 1 GB guest RAM（匿名）
```

所以"看到 `rw-s … /dev/kvm` 就以为是 guest RAM"这个判据是错的。

### 根因

KVM 对 memslot 的**对齐要求只有 PAGE_SIZE**：`kvm_set_memory_region()` 显式检查
`memory_size`、`guest_phys_addr`、`userspace_addr` 三者是否
`& (PAGE_SIZE - 1)`（`virt/kvm/kvm_main.c:2007-2015`），不满足时
`KVM_SET_USER_MEMORY_REGION` 直接返回 `-EINVAL`。
**未 2MB 对齐不是错误**，只是那段 gfn 范围拿不到大页映射（走 4K SPTE）。
本例的真实故障是 memslot 覆盖了 GPA，但对应 HVA 区间没被 populate/权限不符，
KVM 在 fault 路径里拿不到 pfn，最终用户态看到 ioctl 失败。

### 修复

按 PAGE_SIZE 对齐是硬要求，按大页对齐是性能优化：
```c
region.userspace_addr = (uintptr_t)mmap(NULL, size, PROT_READ|PROT_WRITE,
                                        MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB,
                                        -1, 0);
assert(region.userspace_addr != (uintptr_t)MAP_FAILED);
/* 硬约束（kvm_main.c:2007-2015）：页对齐 */
assert((region.userspace_addr & (page_size - 1)) == 0);
/* 软约束：想吃大页才需要按 hugepage 对齐 */
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

发现（**照源码 `TP_printk` 的真实文本**，旧文档这里的字段写法是编的）：

```
kvm_track_tsc: vcpu_id 0 masterclock 1 offsetmatched 0 nr_online 2 hostclock tsc
kvm_update_master_clock: masterclock 0 hostclock tsc offsetmatched 0   ← masterclock 翻转！
kvm_track_tsc: vcpu_id 1 masterclock 0 offsetmatched 0 nr_online 2 hostclock tsc
```

- `kvm_track_tsc` 的格式串是
  `"vcpu_id %u masterclock %u offsetmatched %u nr_online %u hostclock %s"`
  （`arch/x86/kvm/trace.h:951-955`）——注意 **`offsetmatched` 这一列印的是
  `nr_vcpus_matched_tsc`（计数）**，不是布尔；宿主
  `events/kvm/kvm_track_tsc/format` 的字段名仍是
  `vcpu_id / nr_vcpus_matched_tsc / online_vcpus / use_master_clock / host_clock`。
- `kvm_update_master_clock` 的格式串是
  `"masterclock %d hostclock %s offsetmatched %u"`（`trace.h:922-925`），
  `hostclock` 是**符号**（`none` / `tsc`），不是数字 0。
- `kvm_write_tsc_offset` 打印 `"vcpu=%u prev=%llu next=%llu"`
  （`trace.h:896-897`），**没有 `offset=`**。

`masterclock` 从 1 变成 0，表示 masterclock 被禁用。

**Step 3：分析 masterclock 翻转原因**

```bash
# 检查 TSC 稳定性（invariant TSC 看这两个位；tsc_reliable 是内核 quirk，
# 本机 6.8.0-51 就没有置位，不能当成必要条件）
grep -o 'constant_tsc\|nonstop_tsc' /proc/cpuinfo | sort -u

# 看宿主当前 clocksource 是不是 TSC（masterclock 的硬前提）
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
```

★ 关键判据来自源码，不是 cpufreq 日志：
`kvm_track_tsc_matching()`（`arch/x86/kvm/x86.c:2515-2541`）里

```c
bool use_master_clock = (ka->nr_vcpus_matched_tsc + 1 ==
                         atomic_read(&vcpu->kvm->online_vcpus)) &&
                        gtod_is_based_on_tsc(gtod->clock.vclock_mode);
```

也就是**两个条件**：① 所有 online vCPU 的 TSC offset 匹配；② 宿主 vclock_mode 是
TSC 系（`VDSO_CLOCKMODE_TSC`/`HVCLOCK`）。上面 trace 里 `offsetmatched 0` 而
`nr_online 2` —— 说明**匹配数不够**（连 0+1 != 2），这才是翻转的原因。

**Step 4：确认跳变时机**

```bash
# 查看 masterclock 翻转的时间戳（文本里没有 "use_master_clock 0"，别这么 grep）
cat $TRACEFS/trace | grep 'kvm_update_master_clock' | grep 'masterclock 0'
# 时间戳: 1234.568

# 查看该时刻附近谁写了 TSC offset
cat $TRACEFS/trace | grep 'kvm_write_tsc_offset'
#   kvm_write_tsc_offset: vcpu=0 prev=0 next=-8372621 → offset 被改了
```

两者时间吻合：是 **guest/用户态改写了某个 vCPU 的 TSC**，导致
`nr_vcpus_matched_tsc` 掉下去，masterclock 被关闭。

### 根因

★ 旧版把原因写成"cpufreq 调频导致 TSC 不稳定"，这与源码不符：
invariant TSC（`constant_tsc` + `nonstop_tsc`）的定义就是 TSC 频率不随
P-state/C-state 变化，`cpufreq` 改的是核心时钟，不改 TSC 计数速率，
KVM 也没有任何"读 cpufreq 来决策 masterclock"的代码路径。

真实机制是上面那两个条件被破坏：某个 vCPU 的 TSC offset 与其他 vCPU 不再匹配
（典型诱因：guest 写 `IA32_TSC`、`KVM_SET_TSC`、CPU 热插拔/迁移导致的
`tsc_generate_kernel_gtod`变化，或宿主 clocksource 被换成了非 TSC），
于是 `use_master_clock` 置 0，pvclock 退回逐 vCPU 计算路径。

Guest 侧**不会**因此切换 clocksource（`current_clocksource` 仍是 kvm-clock）；
跳变来自 masterclock 关闭瞬间 guest 读到的 pvclock 基准被重算。

`kvm_track_tsc` 的 `masterclock` 字段打的是**翻转前的旧值**（见 `corrections.md` C8），所以看到的是 `use_master_clock 1`，但 `kvm_update_master_clock` 的 `use_master_clock 0` 才是新值。

### 修复

**方案 A**：别让宿主 clocksource 脱离 TSC（这才是 masterclock 的前提之一）
```bash
# ① 宿主 clocksource 必须是 tsc（gtod_is_based_on_tsc()，x86.c:2510-2513）
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
# 若被切走（例如踩到 unstable），可用内核参数固定：
#   tsc=reliable clocksource=tsc
# ② 别在运行期反复改 guest TSC（KVM_SET_TSC / guest 写 IA32_TSC 都会打破 offset 匹配）
```

> ★ 旧版把"禁用 cpufreq"当主因修复，这里保留为**通用调优建议**（减少噪声与迁移），
> 但要写清楚：invariant TSC 下 cpufreq **不改 TSC 计数速率**，
> 它不是 masterclock 翻转的原因，别用它来"验证根因"。
> ```bash
> echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
> ```

**方案 B**：TSC scaling —— 判据不是 `tsc_adjust`

```bash
# ★ tsc_adjust（/proc/cpuinfo）= X86_FEATURE_TSC_ADJUST，
#   指的是 IA32_TSC_ADJUST 这个 MSR（arch/x86/include/asm/cpufeatures.h:243），
#   与 TSC scaling 是两回事，不能用它判断是否支持 scaling。
# TSC scaling 在 Intel 上是 VMX 的 secondary exec control：
#   cpu_has_vmx_tsc_scaling() = vmcs_config.cpu_based_2nd_exec_ctrl &
#                               SECONDARY_EXEC_TSC_SCALING
#   （arch/x86/kvm/vmx/capabilities.h:262-266）
# KVM 检测到就自动启用，并在 VM-Enter 前写 TSC_MULTIPLIER（vmx.c:1958）。
# 用户态可查的结果是 per-vCPU debugfs 文件 tsc-scaling-ratio：
cat /sys/kernel/debug/kvm/<pid>-<fd>/vcpu0/tsc-scaling-ratio   # 存在即已启用
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

判读阈值以 `performance-analysis.md` §2.1 的经验表为准
（`EXTERNAL_INTERRUPT` < 20% 属正常、> 40% 为异常信号）；旧版这里写的 "< 10%"
与本章自己的表格冲突，已统一。注意这些是**经验值、非实测结论**。

### 诊断过程

**Step 1：检查 APICv 是否启用**

```bash
cat /sys/module/kvm_intel/parameters/enable_apicv
# 输出: Y

cat /sys/module/kvm_intel/parameters/enable_ipiv
# 输出: Y
```

APICv 和 IPIv 都已启用。

> ★ 但这推不出"外部中断不应触发 VM-Exit"。APICv 优化的是**guest 之间的中断投递**
> （vIRR / VID / posted-interrupt 通知），而 `EXTERNAL_INTERRUPT`（exit reason 1）
> 是**宿主自己的物理中断**落到了正在跑 guest 的那个 pCPU 上，
> 必然先 VM-Exit 才能进宿主中断处理。两者不是一回事，
> 所以"APICv=Y 还有一堆 EXTERNAL_INTERRUPT"本身不矛盾。
> 真正的缓解手段是宿主侧中断亲和（把设备/IRQ 从 vCPU 绑定的 pCPU 上挪开），
> 以及**直通设备**用 VT-d Posted Interrupts 让通知向量走硬件
> （`intel-vmx.pdf` Section 30.6：notification vector 命中时硬件处理、不产生 VM-Exit）。

**Step 2：用 bpftrace 分析中断来源**

```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_exit /args->exit_reason == 1/ {  // 1 = EXTERNAL_INTERRUPT
    @count = count();
}
tracepoint:kvm:kvm_apic_accept_irq {
    @irq[args->vec] = count();      // ★ 字段是 vec，不是 vector（trace.h:546-551）
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
# 宿主侧：★ kvm_pi_irte_update 的向量字段叫 gvec，不是 vector
#（arch/x86/kvm/trace.h:1080 起：host_irq / vcpu_id / gsi / gvec / pi_desc_addr / set）
sudo bpftrace -e '
tracepoint:kvm:kvm_pi_irte_update {
    printf("gvec=%d vcpu=%d gsi=%d host_irq=%d\n",
           args->gvec, args->vcpu_id, args->gsi, args->host_irq);
}
'

# 无输出 = 当前没有任何 IRTE 被配成 Posted 模式
```

> ★ 这个 tracepoint 只在**直通设备**（VFIO/mdev）把 host IRQ 绑到 vCPU 时触发
> （`arch/x86/kvm/vmx/posted_intr.c:322`、`arch/x86/kvm/svm/avic.c:1003`）。
> **模拟/vhost 的 virtio 设备根本没有 IRTE**，所以"没输出"对这个案例
> 既不能证明 PI 坏了，也不是问题所在——它是"这条路径压根不参与"的正常表现。

**Step 5：检查 VM 配置**

```bash
# QEMU 启动参数
ps aux | grep qemu
# 输出: qemu-system-x86_64 -enable-kvm -cpu host ... -device virtio-net-pci ...
```

```bash
# 先确认 irqchip 模式到底是什么（不要凭"默认是 split"下结论）
ps -ef | grep '[q]emu-system'          # 看有没有显式 -machine kernel-irqchip=...
grep -c "kvm-kernel-irqchip" /proc/$(pgrep -f '^qemu-system-x86_64')/cmdline 2>/dev/null
```

★ **QEMU 的 `kernel-irqchip` 默认是 `on`，不是 `split`**（QEMU 10.1.0-rc2）：

- 选项文档自己写明 `kernel-irqchip=on|off|split … (default=on)`
  （`qemu-options.hx:231`）；
- 未显式指定时取 `ON_OFF_AUTO_AUTO`（`accel/kvm/kvm-all.c:3995`），
  再由 `mc->default_kernel_irqchip_split` 决定（`kvm-all.c:2784-2786`）；
- q35 的 machine class 把该值设为 **false**（`hw/i386/pc_q35.c:364`），
  只有历史机型 `pc-q35-4.0` 才设 true（`pc_q35.c:589`）。

所以"没写 `kernel_irqchip=on` ⇒ 处于 split ⇒ PI 没开"这条链**每一环都不成立**。

### 根因

`EXTERNAL_INTERRUPT`（exit reason 1）占比高，原因是**宿主物理中断落到了正在
运行 guest 的 pCPU 上**： guest 在 non-root 时收到外部中断必然先 VM-Exit
（`vmx.c:7489` 打点后进 `handle_external_interrupt_irqoff`），
这与 `kernel-irqchip` 是 on 还是 split 无关，也和 APICv 是否开启无关。
本例的 virtio-net 是模拟/vhost 设备，中断由宿主内核线程经
`kvm_set_irq`/irqfd 注入，**全程不经过 VT-d IRTE**，因此
`kvm_pi_irte_update` 无输出是正常现象，不能作为"PI 未启用"的证据。

### 修复

**方案 A（对症）**：宿主侧把中断从 vCPU 所在 pCPU 挪走 + 降频
```bash
# ① 中断亲和：把宿主网卡队列 IRQ 绑到不跑 vCPU 的核
echo 4 > /proc/irq/<host_irq>/smp_affinity_list
# ② 减少 guest 侧中断次数（合并）
ethtool -C eth0 rx-frames 64 rx-usecs 200      # guest 内
# ③ 用 vhost-net 把数据面下沉，省掉 QEMU 线程往返
qemu-system-x86_64 ... -netdev tap,id=net0,vhost=on ...
```

**方案 B（如果确有直通设备）**：让直通设备走 Posted Interrupts
前提是宿主开 `intel_iommu=on` 且 interrupt remapping 生效、guest 侧配 vIOMMU
（`-device intel-iommu,intremap=on,caching-mode=off`）、`enable_apicv=Y`；
此时 IRTE 被置为 Posted 模式，通知向量由硬件消费，才出现"0 VM-Exit 投递"
（`intel-vmx.pdf` Section 30.6；`intel-vtd.pdf` Section 5.2.5、9.10）。
模拟 virtio 设备不在这条路上。

**方案 C（错误方向，保留以正视听）**：
单纯加 `-machine kernel-irqchip=on` **不会**降低 `EXTERNAL_INTERRUPT`，
因为它本来默认就是 `on`。

### 验证

```bash
# 调整宿主 IRQ 亲和 / 中断合并后重新统计
sudo perf kvm stat record -a -- sleep 5
sudo perf kvm stat report --key=sample --stdio
# 观察 EXTERNAL_INTERRUPT 次数是否下降（下降幅度需按 phase10 纪律多次测量取分布，
# 本文不给具体百分比结论）

# 若走的是直通设备，才用 PI 证据链验证：
sudo bpftrace -e '
tracepoint:kvm:kvm_pi_irte_update { @pi[args->gvec] = count(); }   # 字段是 gvec
interval:s:5 { print(@pi); clear(@pi); }'
# 同时看 dmesg/`/sys/kernel/iommu_groups/*/` 与 IRTE 配置（phase5/phase7）
```

---

## 总结

| 案例 | 症状 | 根因 | 工具 |
|------|------|------|------|
| VM 启动崩溃 | stderr `KVM: entry failed, hardware error 0x…` | VM-Entry 失败（guest 状态无效）；memslot 只要求页对齐 | strace + ftrace + dump_invalid_vmcs |
| 时钟跳变 5 秒 | `date` 偶尔跳变 | vCPU TSC offset 不再匹配 → masterclock 关闭 | `kvm_track_tsc` / `kvm_update_master_clock` |
| VM-Exit 10 万/秒 | `EXTERNAL_INTERRUPT` 占比高 | 宿主物理中断落在 vCPU 的 pCPU 上（与 irqchip 模式、与模拟设备的 PI 无关） | bpftrace + `perf kvm stat` + 宿主 IRQ 亲和 |

**诊断思路**：
1. 先确认症状（QEMU 日志、guest 内观测）
2. 选择合适的 tracepoint（ftrace / bpftrace）
3. 分析数据模式（分布、频率、时序）
4. 交叉验证（多个工具、多个 tracepoint）
5. 定位根因并修复
6. 验证修复效果
