# Phase 0 实践练习

> KVM 框架层实践练习

---

## 练习列表

> 下列练习均为**手工步骤**（无脚本可直接运行），且都需要一个运行中的 VM。点击练习名跳到具体命令。

| 编号 | 练习名称 | 主要工具 | 难度 | 预计时间 | 核心知识点 |
|------|---------|---------|------|---------|-----------|
| 1 | [跟踪 VM 生命周期](#练习-1-跟踪-vm-生命周期) | ftrace | ★☆☆ | 15min | KVM_RUN 调用链 |
| 2 | [分析 vCPU 调度](#练习-2-分析-vcpu-调度) | perf record/report | ★★☆ | 20min | vCPU 线程调度 |
| 3 | [调试 memslot](#练习-3-调试-memslot) | QEMU monitor / --trace / strace | ★★☆ | 20min | 内存 slot 管理 |
| 4 | [性能对比](#练习-4-性能对比) | perf stat | ★★★ | 30min | 用户态 vs 内核态 |

---

## 快速开始

> 本阶段的练习是**手工步骤**形式，没有封装脚本；每个练习的完整命令见下方「练习详情」。

```bash
# 所有练习都需要一个运行中的 VM
# 使用统一测试环境启动（前台运行，建议单独开一个终端）
cd ../../scripts/vm && ./boot-vm.sh ubuntu --memory 4G --cpus 4

# 回到本目录，按「练习详情」逐个执行
# 清理：在 Guest 内执行 poweroff
```

---

## 练习详情

### 练习 1: 跟踪 VM 生命周期

**目标**: 理解从 ioctl(KVM_RUN) 到 VMENTER 的完整调用链

**方法**:
```bash
# 使用 ftrace 跟踪 KVM_RUN
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_entry/enable
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable

# 运行 VM 并观察 trace
cat /sys/kernel/debug/tracing/trace_pipe
```

**预期输出**:
```
kvm_entry: vcpu 0
kvm_exit: reason 0 vcpu 0
kvm_entry: vcpu 0
...
```

---

### 练习 2: 分析 vCPU 调度

**目标**: 用 `perf` 量化三个问题——vCPU 线程各占多少 CPU、时间花在内核态还是用户态、上下文切换频率如何。

#### 前置条件

```bash
# 1. 线程命名：boot-vm.sh 已默认加 -name kvm-study,debug-threads=on
#    效果：vCPU 线程在 ps/perf 中显示为 CPU 0/KVM、CPU 1/KVM...
#    不加的话所有线程都叫 qemu-system-x86，无法按线程区分
ps -L -p $(pgrep qemu-system-x86) -o pid=,lwp=,comm=
#   79110   79128  CPU 0/KVM     ← 预期看到这种命名
#   79110   79129  CPU 1/KVM

# 2. 内核符号解析：kptr_restrict=1 会让 perf 看到掩码地址（全 0）
echo 0 | sudo tee /proc/sys/kernel/kptr_restrict

# 3. perf 权限（root 运行可跳过）
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

> 注意：`kptr_restrict` 和 `perf_event_paranoid` 每次重启重置。要持久化写 `/etc/sysctl.d/`。

#### 步骤 1: 采集 perf 数据

```bash
QEMU_PID=$(pgrep qemu-system-x86)

# 带调用栈采集 10 秒（-g = call graph）
sudo perf record -g -p $QEMU_PID -o /tmp/perf-vcpu.data sleep 10
# [ perf record: Captured ... (615 samples) ]
```

> `sleep 10` 只控制时长，`-p` 才是过滤条件。不加 `-p` 会采到整个系统。

#### 步骤 2: 每线程 CPU 占用率

```bash
sudo perf report -i /tmp/perf-vcpu.data --stdio --sort=comm --no-children --no-call-graph
```

**典型输出**（4 vCPU 空闲 VM）：

```
    25.36%  CPU 3/KVM
    25.10%  CPU 1/KVM
    25.00%  CPU 2/KVM
    24.54%  CPU 0/KVM
```

**解读方法**：
- 百分比是**相对于 QEMU 全部样本**的占比，不是绝对 CPU%。4 个 vCPU 各 ~25% 说明负载均匀。
- 如果在 guest 里跑 `stress --cpu 4`，预期每个 vCPU 接近 25%（4 核打满）。
- 如果某个 vCPU 明显高于其他，说明 guest 负载不均衡（常见于单线程应用）。

#### 步骤 3: 内核态 vs 用户态 vs 模块分布

```bash
sudo perf report -i /tmp/perf-vcpu.data --stdio --sort=comm,dso --no-children --no-call-graph
```

**典型输出**：

```
    13.92%  CPU 1/KVM  [kvm_intel]       ← VMX 特定代码（vmx_vcpu_run 等）
    11.79%  CPU 1/KVM  [kvm]             ← KVM 通用框架（vcpu_run 等）
    10.82%  CPU 1/KVM  [kernel.kallsyms] ← 调度器、MSR 写入等
     8.73%  CPU 3/KVM  [kvm_intel]
     8.34%  CPU 0/KVM  [kvm_intel]
     8.29%  CPU 2/KVM  [kvm_intel]
     ...
```

**解读方法**：
- vCPU 线程的样本 **100% 落在内核侧**（`kvm_intel` + `kvm` + `kernel.kallsyms`）。这是正常的——vCPU 线程的生命周期就是反复 `ioctl(KVM_RUN)` → 进入 guest → VM-Exit → 返回用户态再 `ioctl`，采样时几乎总在 kernel 里。
- 三个内核 DSO 的含义：
  - `kvm_intel`：Intel VMX 相关（`vmx_vcpu_run`、`vmx_vmexit`、`vmx_l1d_flush`）
  - `kvm`：架构无关的 KVM 框架（`vcpu_run`、`kvm_arch_vcpu_load`）
  - `kernel.kallsyms`：纯内核基础设施（`native_write_msr`、`schedule`、`newidle_balance`）
- 如果用户态（`[qemu]`）占比 > 5%，说明 VM-Exit 过于频繁，VMM 用户态处理成了瓶颈。

#### 步骤 4: 热点函数（需要 --kallsyms 解析模块符号）

```bash
# 关键：perf 默认不解析 kvm/kvm_intel 模块符号，需要显式传 --kallsyms
sudo perf report -i /tmp/perf-vcpu.data --kallsyms /proc/kallsyms \
     --stdio --sort=comm,sym --no-children --no-call-graph
```

**典型输出**（已解析）：

```
     2.48%  CPU 2/KVM  [k] vmx_l1d_flush              ← L1D 缓存冲刷（L1TF 缓解）
     1.99%  CPU 1/KVM  [k] vmx_l1d_flush
     1.99%  CPU 1/KVM  [k] vmx_spec_ctrl_restore_host  ← SPEC_CTRL MSR 恢复
     1.85%  CPU 1/KVM  [k] vmx_vmexit                  ← VM-Exit 处理入口
     1.80%  CPU 0/KVM  [k] vmx_l1d_flush
     1.70%  CPU 1/KVM  [k] __vmx_vcpu_run              ← VMENTER 核心循环
     1.38%  CPU 3/KVM  [k] vmx_vmexit
     0.90%  CPU 0/KVM  [k] native_write_msr            ← MSR 写入
     0.85%  CPU 1/KVM  [k] vcpu_run                    ← KVM 主循环
     0.71%  CPU 1/KVM  [k] vmx_vcpu_enter_exit         ← vcpu_enter_guest 包装
     ...
```

**解读方法**：
- 热点集中在 VM-Entry / VM-Exit 路径上，这是 vCPU 调度的核心：
  - `__vmx_vcpu_run` → `vmx_vcpu_enter_exit`：执行 VMENTER，进入 guest
  - `vmx_vmexit`：VM-Exit 处理入口
  - `vmx_l1d_flush`：每次 VM-Exit 后冲刷 L1D 缓存（L1TF 安全缓解，`arch/x86/kvm/vmx/vmx.c`）
  - `vmx_spec_ctrl_restore_host`：恢复宿主 SPEC_CTRL MSR（Spectre 缓解）
- 如果看到 `kvm_apic_has_interrupt`、`kvm_cpu_has_pending_timer` 等占比高，说明中断注入频繁。
- 如果看到 `schedule`、`newidle_balance` 占比高，说明 vCPU 存在调度等待。

> 💡 **不加 `--kallsyms` 时**，kvm_intel 符号会显示为 `0x0000000000044cea` 这样的地址。不是 bug——perf 需要 kallsyms 来解析模块符号。

#### 步骤 5: 上下文切换频率（用 perf sched）

`perf report` 看不到切换次数，要用调度事件专用工具：

```bash
# 必须系统级采集（-a），因为 sched_switch 是 per-CPU 的 tracepoint
# 用 -p 过滤会导致 "context switch bugs" 且数据全 0
sudo perf sched record -a -o /tmp/perf-sched.data sleep 10

# 查看每线程的切换次数、运行时、调度延迟
sudo perf sched latency -i /tmp/perf-sched.data | grep -E "CPU [0-9]/KVM|vhost|Task.*Runtime"
```

**典型输出**：

```
  Task                  |   Runtime ms  | Switches | Avg delay ms | Max delay ms |
  CPU 0/KVM:85053       |     32.251 ms |     2502 | avg: 0.004   | max: 0.009   |
  CPU 1/KVM:85054       |     31.162 ms |     2502 | avg: 0.004   | max: 0.009   |
  CPU 2/KVM:85055       |     31.188 ms |     2502 | avg: 0.004   | max: 0.007   |
  CPU 3/KVM:85056       |     31.791 ms |     2502 | avg: 0.004   | max: 0.005   |
```

**解读方法**：
- **Switches**：切换次数。2502 次 / 10 秒 = 250 Hz/线程。空闲 VM 里这主要是 guest 执行 `HLT` 触发的**自愿切换**（guest idle → 让出物理核）。
- **Avg delay**：被唤醒到实际上 CPU 的等待时间。0.004 ms 说明宿主几乎没有调度压力。如果 > 1ms，说明 vCPU overcommit 或宿主负载过高。
- **Max delay**：最坏情况延迟。0.009 ms 很健康。如果 > 10ms，用户会感知到卡顿。
- **对比实验**：在 guest 里跑 `stress --cpu 8`（超过物理核数），观察 Switches 激增、Avg delay 变大。

#### 常见陷阱

| 问题 | 症状 | 解法 |
|------|------|------|
| 线程名全是 `qemu-system-x86` | 无法区分 vCPU 线程 | QEMU 启动参数加 `-name xxx,debug-threads=on`（`boot-vm.sh` 已默认加上） |
| 模块符号显示为地址 | `0x0000000000044cea` | `echo 0 > /proc/sys/kernel/kptr_restrict`，perf report 加 `--kallsyms /proc/kallsyms` |
| perf sched 显示 0 switches + "context switch bugs" | `perf sched record -p $PID` | 改用 `-a`（系统级），再用 grep 过滤输出 |
| `perf_event_paranoid` 阻止采集 | `Error: perf_event_open failed` | `echo -1 > /proc/sys/kernel/perf_event_paranoid`（root 可跳过） |

---

### 练习 3: 调试 memslot

**目标**: 理解内存 slot 的管理机制

**方法**:

memslot 是内核侧 `struct kvm` 的成员，既不在 debugfs 暴露（KVM 的 debugfs 目录只由统计项生成，见 `virt/kvm/kvm_main.c:1092`），也无法用 gdb 附加 QEMU 进程读取。要观察它，得从建立 memslot 的用户态一侧入手：

```bash
# 方法 1: QEMU monitor 查看内存区域树（memslot 的来源）
# 启动 QEMU 时加 -monitor stdio，然后执行：
(qemu) info mtree -f

# 方法 2: QEMU 自带 trace 事件，直接打印每个 memslot 的参数
# 来源: qemu/accel/kvm/trace-events:22
qemu-system-x86_64 --trace 'kvm_set_user_memory' ...
# 输出含 AddrSpace#/Slot#/flags/gpa/size/ua/ret

# 方法 3: 从 ioctl 层面观察（来源: qemu/accel/kvm/kvm-all.c:377-387）
sudo strace -f -e trace=ioctl -p $(pgrep -f qemu-system-x86) 2>&1 | grep -i KVM_SET_USER_MEMORY
```

**关注点**:
- memslot 的数量和大小
- GPA 到 HVA 的映射关系
- 内存区域的类型（RAM、MMIO）

---

### 练习 4: 观测 KVM 统计计数器

**目标**: 用 KVM 在 debugfs 暴露的统计计数器 + `perf kvm stat`，建立对 VM-Exit 的**定量直觉**——不同操作触发什么类型的 VM-Exit、各花多少时间。这是后续 phase1 深入 VM-Exit 处理流程的铺垫。

#### 背景：debugfs 的两级结构

KVM 在每个 VM 创建时调用 `kvm_create_vm_debugfs()`（`virt/kvm/kvm_main.c:1076`）生成统计目录：

```
/sys/kernel/debug/kvm/                          ← 全局聚合（所有 VM 合计）
├── exits                                       ← 累计 VM-Exit 次数
├── halt_exits                                  ← HLT 触发的 VM-Exit 次数
├── io_exits / mmio_exits / irq_exits ...       ← 按类型分类
├── l1d_flush                                   ← L1D 缓存冲刷次数（L1TF 缓解）
├── <PID>-<fd>/                                 ← per-VM 目录（一个 VM 一个）
│   ├── exits, halt_exits, ...                  ← 该 VM 的累计
│   ├── vcpu0/                                  ← per-vCPU 目录
│   │   └── exits, halt_exits, ...
│   └── vcpu1/
```

> 💡 只有一个 VM 时，顶层文件和 per-VM 目录的数值相同。多 VM 时，顶层是所有 VM 的合计。

计数器在内核里都是 `__this_cpu_inc` 或 `vcpu->stat.xxx++` 的形式，**几乎零开销**——不会干扰测量结果。

#### 前置条件

```bash
# VM 必须正在运行
QEMU_PID=$(pgrep qemu-system-x86)
VM_DIR=$(ls -d /sys/kernel/debug/kvm/${QEMU_PID}-* | head -1)
echo "VM 计数器目录: $VM_DIR"
# /sys/kernel/debug/kvm/<PID>-9
```

#### 步骤 1: 看空闲 VM 的计数器基线

```bash
# 快照 1
for f in exits halt_exits io_exits mmio_exits irq_exits l1d_flush \
         host_state_reload insn_emulation pf_taken req_event; do
    printf "  %-25s %s\n" "$f" "$(cat $VM_DIR/$f)"
done > /tmp/snap1.txt

sleep 5

# 快照 2
for f in exits halt_exits io_exits mmio_exits irq_exits l1d_flush \
         host_state_reload insn_emulation pf_taken req_event; do
    printf "  %-25s %s\n" "$f" "$(cat $VM_DIR/$f)"
done > /tmp/snap2.txt

# 计算差值
paste /tmp/snap1.txt /tmp/snap2.txt | while read _ name1 val1 _ name2 val2; do
    delta=$((val2 - val1))
    [ "$delta" -gt 0 ] && printf "  %-25s +%6d  (每秒 ~%d)\n" "$name1" "$delta" "$((delta / 5))"
done
```

**典型输出**（2 vCPU 空闲 VM）：

```
  exits                     +  5181  (每秒 ~1036)
  halt_exits                +  2516  (每秒 ~503)
  irq_exits                 +   139  (每秒 ~28)
  l1d_flush                 +  2655  (每秒 ~531)
  host_state_reload         +  2516  (每秒 ~503)
```

**解读**：
- **halt_exits 占 exits 的一半**：空闲 VM 里 guest 内核跑 `HLT` 让出 CPU，触发 VM-Exit。
- **l1d_flush ≈ halt_exits**：每次 VM-Exit 后 KVM 都冲刷 L1D 缓存（`vmx_l1d_flush`），这是 L1TF 安全缓解的开销——即使是无害的 HLT exit 也逃不掉。
- **host_state_reload = halt_exits**：HLT exit 后 KVM 必须重新加载宿主状态（`host_state_reload` 计数精确等于 halt_exits，说明每次 HLT exit 都触发了 state reload）。
- **irq_exits ~28/s**：guest 内核的周期性时钟中断（典型 HZ=250 的 tick，但因为 halt-poll 合并了一些，实际 exit 数更低）。

#### 步骤 2: 用 `perf kvm stat` 看 VM-Exit reason 分布（关键）

debugfs 计数器只给**总数**，看不出每次 exit 的处理时间。`perf kvm stat` 按 exit reason 分类统计次数 + 耗时：

```bash
# live 模式，观察 5 秒
sudo perf kvm stat live
```

**典型输出**（空闲 VM）：

```
VM-EXIT              Samples  Samples%    Time%    Min Time    Max Time     Avg time
HLT                      500    49.90%   99.96%     1.71us   3993.78us   3982.81us
MSR_WRITE                500    49.80%    0.04%     1.42us      2.72us      1.72us
EXTERNAL_INTERRUPT         2     0.20%    0.00%     2.75us      3.12us      2.94us
MSR_READ                   2     0.20%    0.00%     1.86us      2.30us      2.08us
```

**这是本练习最重要的表——要读懂三个维度**：

| 维度 | 含义 | 怎么看 |
|------|------|--------|
| Samples% | 这种 exit 占总 exit 的比例 | 数量分布 |
| Time% | 这种 exit 占总 VM-Exit **时间**的比例 | **开销分布** |
| Avg time | 单次 exit 平均处理时间 | 单次代价 |

**关键发现**：
- HLT 和 MSR_WRITE **数量几乎相同**（各 ~500/sec），但 **HLT 占了 99.96% 的时间**。
- 这不是因为 HLT "处理慢"——HLT 的 "Avg time 3982us" 其实是 **guest 睡眠的时间**（vCPU 被 halt-poll 阻塞等待中断），不是真正的 VM-Exit 处理开销。
- MSR_WRITE 是 guest 内核周期性写时间戳 MSR（`X86_MSR_TSC_ADJUST` 等），每次只要 1.72us，非常快。
- 真正的 VM-Exit 处理开销（排除 sleep 时间）都在 **微秒级**，这正是 KVM 作为内核态方案的优势——比用户态 VMM 少了一次进程切换。

#### 步骤 3: 不同 workload 下的对比（在 guest 里操作）

登录 guest（在 VM 的串口里输入 root 登录），跑不同命令，然后回到 host 看计数器变化：

```bash
# 在 host 上开一个窗口，持续监控 exits 的增速
watch -n 1 "cat $VM_DIR/exits; echo; cat $VM_DIR/halt_exits; echo; cat $VM_DIR/io_exits"

# 在 guest 串口里分别执行以下命令，观察 host 上计数器变化：
```

**场景 A: CPU 密集型**（guest 内）

```bash
# guest 内执行：纯计算，理想情况下不触发 VM-Exit
stress --cpu 2 --timeout 10
```

预期变化：
- `exits` 几乎**不增长**（纯 guest 模式执行，没有 VM-Exit）
- `halt_exits` 不增长
- 只有时钟中断（`irq_exits`）缓慢增加

> 💡 这就是 KVM 的价值——CPU 密集型 workload 在 guest 里几乎以原生速度运行。

**场景 B: I/O 密集型**（guest 内）

```bash
# guest 内执行：PIO 读写 virtio 设备
dd if=/dev/vda of=/dev/null bs=4k count=10000 iflag=direct
```

预期变化：
- `io_exits` 显著增加（每次 PIO 触发一次 VM-Exit）
- 如果走 virtio 的 MMIO 通知路径，`mmio_exits` 也会涨

**场景 C: 内存分配**（guest 内）

```bash
# guest 内执行：分配大量内存，触发 page fault
python3 -c "x = bytearray(500 * 1024 * 1024)"
```

预期变化：
- `pf_taken`（缺页总数）增加
- `pf_fixed`（KVM 修复的缺页）增加
- 如果 page table 已经建立好，`pf_fast` 会比较高（快速路径）

**场景 D: MSR / CPUID 访问**（guest 内，需安装 msr-tools）

```bash
apt install msr-tools
# 读 MSR：触发 MSR_READ exit
rdmsr 0x10
# 写 MSR：触发 MSR_WRITE exit
wrmsr 0x10 0x1234
# CPUID 指令：触发 CPUID exit
cpuid -r | head
```

预期变化：
- 对应计数器（`insn_emulation` 等）逐个增加

#### 步骤 4: 综合对比表

完成上述实验后，你应该能填出这张表：

| Workload | 主要增长的计数器 | 主要 exit reason | Avg time |
|----------|-----------------|------------------|----------|
| 空闲 | `halt_exits`, `l1d_flush` | HLT, MSR_WRITE | HLT ~4ms（含 sleep），MSR_WRITE ~2μs |
| CPU 密集 | 几乎无 | 仅时钟中断 | ~3μs |
| I/O 密集 | `io_exits`, `mmio_exits` | PIO / MMIO | ~1-5μs |
| 内存分配 | `pf_taken`, `pf_fixed` | PAGE_FAULT | ~1-10μs |
| MSR 读写 | `insn_emulation` | MSR_READ/WRITE | ~2μs |

这张表的含义：**不同的 guest 操作对应不同类型的 VM-Exit，处理代价差 1000 倍**（HLT 的"处理"含睡眠是 ms 级；MSR_WRITE 只要 μs 级）。理解这一点后，再去看 phase1 里各种 VM-Exit 的处理流程，就能理解为什么 KVM 要做那么多优化（halt-poll、MSR bitmap、 Posted Interrupts 等）——都是在把贵的 exit 变便宜、或者干脆消除掉。

#### 源码对照

计数器在哪里递增？练习结束后可以 grep 几个关键位置：

```bash
# halt_exits 在哪里递增
grep -rn "halt_exits\|KVM_STAT.*halt" /root/code/linux-6.12.93/arch/x86/kvm/

# l1d_flush 在哪里递增
grep -rn "l1d_flush\|L1D_FLUSH" /root/code/linux-6.12.93/arch/x86/kvm/vmx/

# 计数器定义（每个 vCPU 有哪些 stat 字段）
grep -n "kvm_vcpu_stat" /root/code/linux-6.12.93/arch/x86/kvm/x86.c | head -5
```

#### 常见陷阱

| 问题 | 症状 | 解法 |
|------|------|------|
| 找不到 debugfs 目录 | `/sys/kernel/debug/kvm/` 不存在 | `mount -t debugfs none /sys/kernel/debug` |
| 顶层计数器全是 0 | 没有 VM 在运行 | 启动 VM 后再查；或进 per-VM 子目录 `<PID>-*/` |
| `perf kvm stat live` 没输出 | perf_event_paranoid 限制 | `echo -1 > /proc/sys/kernel/perf_event_paranoid` |
| 多 VM 时顶层数看不懂 | 多个 VM 的计数混在一起 | 进 per-VM 子目录 `<PID>-*/` 看单 VM 数据 |

---

## 统一测试环境

所有练习使用统一的 VM 启动脚本：

- **构建脚本**: `scripts/vm/build-kernel.sh` + `scripts/vm/build-rootfs-ubuntu.sh`
- **启动脚本**: `scripts/vm/boot-vm.sh`（前台运行 QEMU）
- **详细说明**: 参见 `../../scripts/README.md`

---

## 故障排查

### 问题 1: 无法访问 debugfs

```bash
# 挂载 debugfs
sudo mount -t debugfs none /sys/kernel/debug
```

### 问题 2: perf 无法attach到进程

```bash
# 调整 perf_event_paranoid 设置
echo -1 > /proc/sys/kernel/perf_event_paranoid
```

### 问题 3: 无法找到 KVM 调试信息

```bash
# 确保 KVM 模块已加载
lsmod | grep kvm

# 确保 debugfs 已挂载
ls /sys/kernel/debug/kvm/
```

---

## 参考资料

- Phase 0 README: `../README.md`
- Linux 内核源码: `arch/x86/kvm/`
- KVM 文档: `Documentation/virt/kvm/`
