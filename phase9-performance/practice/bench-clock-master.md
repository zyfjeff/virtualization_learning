# E4 · 主时钟决策树：切宿主 clocksource 会发生什么，以及为什么回不去

> 机制全文在 [`../annotations.md`](../annotations.md) §3.1.1（失效单向）与 §3.1.2
> （关掉之后迁移多付一笔 pvclock 账），本文**不重讲机制**，只给可判伪的预测、
> 前置检查与判据。测量规范见 [`../measurement.md`](../measurement.md)。
> **本轮不上机**，所有数字待实测。

---

## 1. 要回答的问题

`../../phase7-timer-virt/` 讲"guest 怎么读时间"，本章讲的是另一件事：
**KVM 自己什么时候敢走快路径**。它是四个条件相与（`arch/x86/kvm/x86.c:3034-3036`），
其中唯一能从外部随手拨动的开关是"宿主 clocksource 是不是 TSC 基"。
旧版 phase9 把这条写成了"CPUID.80000007:EDX[8] 决定"（已勘误，
见 [`../corrections.md`](../corrections.md)），所以本章需要一个真做过的实验。

四条预测，每条都可被一次运行否证：

| # | 预测 | 否证意味着 | 判据所在 |
|---|---|---|---|
| **P1** | 宿主 `tsc → hpet`，**不重启 VM**，几毫秒内该 VM 的 `use_master_clock` 翻成 0 | 说明失效链不在 notifier 上，`../../annotations.md` §3.1.1 的链路读错了 | §4.1-A2 |
| **P2** | 宿主 `hpet → tsc`，同一台 VM **永远不会**自己翻回 1（哪怕跑满一个采样窗） | 说明还有一条我没找到的 on-edge 触发路径 | §4.1-A3 |
| **P3** | P2 之后新建的第二台 VM **立刻能拿到** masterclock 1，而第一台仍是 0 | 说明主时钟不是全局状态而是 per-VM 状态，§3.1.1 的六个重算点分类错 | §4.1-A4 |
| **P4** | "主时钟 off + 宿主在 tsc 上"的 VM，其 `kvm:kvm_pvclock_update` 速率显著高于"主时钟 on"的同构 VM，且**上界符合 §3.1.2 的限流模型** | 说明 `x86.c:5034` 那个乘积项其实不发生，或被别的路径吞掉 | §4.1-A5 |

P2 + P3 是本章真正的收获：**同一台宿主上两台 VM 的时钟快路径可以永久分叉**，
而且这个分叉态恰好是 A5 的干净对照组（见 §3 末尾）。

---

## 2. 前置检查

### 2.1 ★ 唯一的不可逆风险：clocksource 看门狗可能把 TSC 判死

x86 上只有 `tsc-early` 与 `tsc` 两个时钟源的 `vdso_clock_mode` 是
`VDSO_CLOCKMODE_TSC`（`arch/x86/kernel/tsc.c:1174`、`:1197`），
`hpet` / `acpi_pm` 都是 `VDSO_CLOCKMODE_NONE`。`tsc` 带
`CLOCK_SOURCE_MUST_VERIFY`（`tsc.c:1194`），所以它**一直挂在看门狗名单里被比对**，
不管它是不是当前时钟源：

- `clocksource_enqueue_watchdog()`（`kernel/time/clocksource.c:623-636`）把
  MUST_VERIFY 的都塞进 `watchdog_list`；
- `clocksource_watchdog()`（`:424` 起）周期性拿 `watchdog`（一个非 MUST_VERIFY
  的时钟源，本机是 hpet/acpi_pm）做基准比斜率，超了就
  `__clocksource_unstable()`（`:194-201`）—— 清 `CLOCK_SOURCE_VALID_FOR_HRES`、
  置 `CLOCK_SOURCE_UNSTABLE`、`rating` 归零。

**这一步没有运行时逆操作**。TSC 一旦被判死：本机的 `tsc` 时钟源永久不可选，
宿主退回 hpet，**之后所有 VM 的主时钟都再也开不起来**，直到重启。
也就是说 A2 之后想复现 A1，靠的可能只有 `reboot`。

三条降低风险的做法，脚本按此执行：

1. **切换前先看 `dmesg` 里有没有既有的 unstable 记录**，有就别跑（说明这台机器的
   TSC 已经在悬崖边）。
2. **切换期间宿主必须近乎空载**。读回延迟过大时走的是
   `WD_READ_SKIP`/`WD_READ_UNSTABLE` 分支（`:245-294`），重负载正是它误判的原因；
   所以本实验不与 E1/E2/E5 同时跑，且 `--sample-s` 期间不要另起负载。
3. **要反复跑就用内核 cmdline `tsc=nowatchdog`**（`arch/x86/kernel/tsc.c:327-333`
   → `tsc_disable_clocksource_watchdog()`，注册前调用点在 `:1560-1562`），
   TSC 不进看门狗名单，就没有"被比死"这条路。它需要重启才能生效，
   所以脚本只能在 preflight 里**提示**这条，不能替你改。

真实执行还要显式加 `--i-accept-clocksource-risk`，没这个标志脚本拒绝写
`current_clocksource`。

### 2.2 必须有一个"非 TSC 基"的备随时钟源

```bash
cat /sys/devices/system/clocksource/clocksource0/available_clocksource
# 本机（6.8.0-51）实测：tsc hpet acpi_pm
cat /sys/devices/system/clocksource/clocksource0/current_clocksource   # tsc
stat -c %a /sys/devices/system/clocksource/clocksource0/current_clocksource  # 644，仅 root 可写
```

可用项里除 `tsc*` 之外一个都不剩 → **本实验无法执行**（`--fallback` 也救不了）。
脚本默认按 `hpet` → `acpi_pm` 顺序挑，可用 `--fallback` 覆盖。
两个坑：`hpet` 可能被 BIOS 或内核 cmdline `hpet=disable` 干掉
（`arch/x86/kernel/hpet.c:109` 认这个选项，`__setup("hpet=", …)` 在 `:118`；
本机实测 `available_clocksource` 里有 `hpet`，换机器时要先查）；`acpi_pm` 只有
24/32 位且不能容忍长看门狗间隔 —— `kernel/time/clocksource.c:492-500`
的注释专门点了它（`:498-499`）。

### 2.3 事件消费要求 guest 真的有负载

关边那条链每一跳都是异步的（`../../annotations.md` §3.1.1 的调用图），最后一跳是
**置请求**而不是重算：`pvclock_gtod_update_fn()`（`arch/x86/kvm/x86.c:9643`）
只给每个 vCPU `kvm_make_request(KVM_REQ_MASTERCLOCK_UPDATE, vcpu)`（`:9652`），
真正重算并打 trace 发生在该 vCPU **下一次进 guest** 时
（`x86.c:10809-10810`）。一个睡着不动的 vCPU 不会消费请求 —— 采样窗里 guest
必须跑东西，否则"没事件"既可能是主时钟没关，也可能是没人消费。
脚本用 E3 同一个负载模块：`insmod ple_load.ko workload=1`（无锁私有缓冲区，
不会把 PLE 的账混进来，理由见 `bench-migrate.md` §3.1）。

### 2.4 观测出口的实存性（宿主 6.8 ≠ 文档 6.12.93）

```bash
ls -d /sys/kernel/tracing/events/kvm/{kvm_update_master_clock,kvm_track_tsc,kvm_pvclock_update,kvm_write_tsc_offset}
```

四个都有（本机实测）。**print 格式逐字核过**：
`cat /sys/kernel/tracing/events/kvm/<ev>/format` 里的 `print fmt` 与
6.12.93 `arch/x86/kvm/trace.h` 的 `TP_printk` 完全一致 ——
这条核对很重要，因为脚本的判据全靠 grep 文本。

本机跑的是 6.8 而文档行号是 6.12.93，`../../measurement.md` §5.2 要求查"逻辑是否也在
6.8 上"。P2 的判据恰好**不需要源码也能验**：反汇编运行中的 `kvm.ko` 就能看到
那个单向门：

```bash
zstd -d /lib/modules/$(uname -r)/kernel/arch/x86/kvm/kvm.ko -o /tmp/kvm.ko
nm /tmp/kvm.ko | grep -w pvclock_gtod_notify
objdump -d -M intel --no-show-raw-insn /tmp/kvm.ko | \
    awk '/<pvclock_gtod_notify>:/,/^$/' | tail -14
```

本机实测尾部落在 `and eax,0xfffffffd; cmp eax,0x1; je <return>` 上 ——
即 `(mode & ~2) == 1` 时**直接返回**，只有不等时才继续查
`kvm_guest_has_master_clock` 并 `irq_work_queue`。x86 的枚举是
NONE/TSC/PVCLOCK/HVCLOCK = 0/1/2/3（`arch/x86/include/asm/vdso/clocksource.h:5-8`
+ `include/vdso/clocksource.h:12-18`），`{1,3}` 正是
`gtod_is_based_on_tsc()` 的两个值。结论：**单向门在宿主 6.8 上一模一样**，
P2 不是"只在文档版本里成立"的结论。

### 2.5 两个 tracepoint 里的 `offsetmatched` 不是同一个东西

| tracepoint | `offsetmatched` 实际是什么 | 源码 |
|---|---|---|
| `kvm_update_master_clock` | **布尔** —— `vcpus_matched` 的整体结果 | `arch/x86/kvm/x86.c:3042`，`trace.h:906-926` |
| `kvm_track_tsc` | **计数** —— `nr_vcpus_matched_tsc`，不含基准 vCPU | `x86.c:2540-2542`，`trace.h:928-955` |

同一份 trace 里两种含义共用一个标签，是 `../measurement.md` §7
"统计量的名字不像它量的是那个东西"那条陷阱的又一实例。
`kvm_track_tsc` 的正确读法是把两个数对起来看：
`offsetmatched 3 nr_online 4` ⇔ `3 + 1 == 4` ⇔ `vcpus_matched` 为真。

**同一条行里 `masterclock` 字段也不是同一时刻的值**：

| tracepoint | `masterclock` 是什么 | 依据 |
|---|---|---|
| `kvm_update_master_clock` | **本次重算的新决定** | `x86.c:3034` 赋值 → `:3042` 打印，同一个函数体内相邻 |
| `kvm_track_tsc` | **翻转前的旧值** | `kvm_track_tsc_matching()` 只算出一个**局部**变量（`:2526`）并发请求（`:2537-2538`），打的 `ka->use_master_clock` 还没被改过；该字段全树只在 `:3034` 赋值 |

对判据的直接影响：**翻边的证据只能取自 `kvm_update_master_clock`**，
`kvm_track_tsc` 在本章只用来核对 vCPU 匹配条件（A1 那条满足式）。
拿 `kvm_track_tsc` 的 `masterclock` 读翻转方向会稳定地慢一拍。

还有一处必须提前说明的标签误导：`hostclock` 的符号表只映射了
`NONE → "none"` 和 `TSC → "tsc"`（`trace.h:902-905`）。所以切到 hpet 后 trace 里
印的是 **`hostclock none`，意思是"不是 TSC 基"，不是"没有时钟源"**。

---

## 3. 实验矩阵

臂是一条**不可乱序的时间线**（每条臂的输入是前一条留下的状态），
脚本用 `--until` 截断，不接受乱序选择：

| 臂 | 动作 | 预期 |
|---|---|---|
| A0 | **不开 VM**，只切一个来回 `tsc→hpet→tsc` | `kvm:*` 时钟事件全为 0（阴性对照：证明事件来自我们的 VM，而不是宿主里别的什么） |
| A1 | 起 VM1（4 vCPU，`workload=1` 负载）→ 采样 `--sample-s` | 出现 `masterclock 1 hostclock tsc offsetmatched 1` |
| A2 | VM1 活着，`echo hpet > current_clocksource` → 采样 | 出现 `masterclock 0 hostclock none offsetmatched 1`（**P1**） |
| A3 | `echo tsc > current_clocksource`，VM1 继续跑同样长的窗，**什么都不额外做** | **零**条 `masterclock 1`（**P2**） |
| A4 | VM1 活着时再起 VM2 → 采样 → 关掉 VM2 | VM2 有 `masterclock 1`，同一窗口 VM1 仍一条没有（**P3**） |
| A5 | 两次独立 boot：`C-off` = 起 VM → 切 hpet 逼 off → 切回 tsc → 注入迁移；`C-on` = 起 VM（全程 tsc）→ 同样的注入 | 两臂**采样期宿主都在 `tsc` 上**，唯一差别是 `use_master_clock` → `kvm_pvclock_update` 速率差可直接归因（**P4**） |

A5 的设计要点是**借用 A3 造出来的分叉态**：直接"在 hpet 上跑 vs 在 tsc 上跑"
比的是两件事（宿主时钟源变慢 + 主时钟关掉），不可分；而"主时钟已 off 的 VM"与
"主时钟 on 的 VM"可以在**同一个宿主时钟源（tsc）**下对比，混淆项被消掉了。
代价是 `C-off` 必须先走一遍 A2→A3，所以 A5 依赖 A2/A3 的结论成立。

---

## 4. 观测点与判据

### 4.1 每臂判据

| 臂 | 通过条件（全部满足才算过） |
|---|---|
| A1 | 窗口内 VM1 的 tid 上有 `masterclock 1`；且 `kvm_track_tsc` 至少一条满足 `nr_vcpus_matched_tsc + 1 == online_vcpus` |
| A2 | 窗口内 VM1 tid 上出现 `masterclock 0`；**同一条里 `offsetmatched 1`** —— 这两个数同时出现才证明掉的是"宿主时钟源"条件而不是"vCPU 匹配"条件（§2.5） |
| A3 | 窗口内 VM1 tid 上 `masterclock 1` 计数 = 0，且**同窗口 `current_clocksource` 确实回到 `tsc`**（脚本每 1 s 复读一次）。若 >0 → P2 被否证，立刻登记 `../corrections.md` |
| A4 | VM2 的 tid 上有 `masterclock 1`；VM1 的 tid 上同窗口没有；两台 VM 的 pid 都从 `/proc/<pid>/fd` 的 `/dev/kvm` 计数确认过（AGENTS.md 陷阱 7） |
| A5 | 先看 A2/A3 式的前置断言在 `C-off` 里成立（off 且回到 tsc），再比 `kvm_pvclock_update`/秒；`C-on` 的速率必须接近 0（只有 boot 期几条） |

### 4.2 阴性对照

| 对照 | 期望 | 破了说明什么 |
|---|---|---|
| A0 全程 `kvm_update_master_clock` = 0 | 事件与我们的 VM 绑定 | 宿主上有别人的 VM 在跑（preflight 会报 `/dev/kvm` fd 数） |
| A2/A3 的 `tsc-offset` 逐 vCPU 与 A1 基线完全相同 | 切时钟源不改 offset | 改了就说明踩到了 `tsc_offset_adjustment` 或 `kvm_check_tsc_unstable()` 那条重写路径（`arch/x86/kvm/x86.c:5007-5025`），是异常信号（机制见 `../annotations.md` §3.2）。脚本按 per-vCPU debugfs 的 `pid` 文件反查归属（`virt/kvm/kvm_main.c:4184-4194` + `:4484-4486`），**读不到时报"无从核对"，不算通过** |
| 同上两窗的 `kvm:kvm_write_tsc_offset` 计数 = 0 | 与上一行互为备份的第二条出口 | 6.12.93 里这个 trace 只有一个发出点（`x86.c:2614`，在 `kvm_vcpu_write_tsc_offset()` 内），而该函数全树只有三个调用点：`x86.c:2688`（`__kvm_synchronize_tsc()`，显式同步 TSC）、`:2791`（`adjust_tsc_offset_guest/host`）、`:5023`（宿主 TSC 被判不稳后的 catchup）。**主时钟重算不在这个集合里**，所以两窗内任何一条都要单独解释 |
| A3 与 A1 的窗口长度相同 | "零事件"不是"没给够时间" | A1 已经证明这个长度的窗口里事件是能出现的 |

### 4.3 A5 的上界模型（必须先写预期再测）

`../../annotations.md` §3.1.2 的两条相加：

- **每次迁移直接一条**：`kvm_gen_kvmclock_update()` 先给本 vCPU 置
  `KVM_REQ_CLOCK_UPDATE`（`arch/x86/kvm/x86.c:3438`）；
- **广播部分被限流**：`schedule_delayed_work(..., KVMCLOCK_UPDATE_DELAY)`
  （`:3439-3440`，`#define` 在 `:3417` = 100 ms）对同一 work 不重复排队，
  100 ms 后 `kvmclock_update_fn()` 给**全部** vCPU 各置一次并 kick（`:3428-3430`）
  → 上界 `10 × vCPU 数 / 秒`。

所以 `C-off` 的预期是 `≈ 注入次数/秒 + 10 × V`，`C-on` 预期 `≈ 0`
（主时钟开着时 `x86.c:5034` 的条件不成立，只剩 boot / `KVM_SET_CLOCK` 等零星来源）。
测出来若与注入次数无关而与 `10 × V` 有关，说明限流把直接项也吃了 —— 那是新事实，
要回写 §3.1.2。

结果侧（guest `completed` 速率差）在 A5 里**只作方向参考**：两臂唯一的系统差是
`use_master_clock`，但 kick 的数量随负载变化，样本量小的时候不足以给倍数结论。

---

## 5. 执行

```bash
./bench-clock-master.sh --preflight                 # 只读，含 2.1/2.2/2.4 的核对
./bench-clock-master.sh --all --dry-run             # 打印整条时间线，不碰系统
sudo ./bench-clock-master.sh --until A4 --i-accept-clocksource-risk
sudo ./bench-clock-master.sh --all --i-accept-clocksource-risk --repeat 3
sudo ./bench-clock-master.sh --until A3 --fallback acpi_pm --sample-s 30
```

真跑**必先自动跑一遍 `preflight`**，不过就停手 —— §2.1 的风险不可逆，脚本不给
"跳过检查直接切"留后门。`--repeat N` **只作用于 A5**：A0–A4 是过/不过的判据，
重复不改变结论，只有 A5 的速率需要多次重复才允许写方向性结论。

窗口与收尾：每臂的观测窗都开在**动作之前**、关在动作之后（§6.8），
开窗时先 `tracing_on=0 → 清 trace → tracing_on=1`，避免把上一臂的事件算进这一臂；
关窗时顺手读 `per_cpu/cpu*/stats` 并把 `overrun:` 求和，非 0 就告警
（`../measurement.md` §4(c)）。`trap` 里无条件关机、写回原 `current_clocksource`
并清 ftrace（`current_tracer` / `set_event` / `set_ftrace_filter` /
`function_profile_enabled` 各自独立，AGENTS.md 陷阱 9）。写回失败会把
**原值和命令**大字打出来，让人工兜底 —— 那个 override 会一直粘住，不能靠下一轮
实验顺手改掉。

---

## 6. 已知坑

1. **`echo <cs> > current_clocksource` 不是原子切换**：它写的是
   `override_name` 然后 `clocksource_select()`（`kernel/time/clocksource.c:1401-1416`）。
   名字拼错、或该时钟源已被判 unstable，都会**静默失败** —— 文件写返回成功，
   `current_clocksource` 一动不动。判据必须在切换后**复读**该文件，不能信写入返回码。
   这个 override 会一直粘住，直到有人再写一次；所以退出恢复不是"礼貌"，是必须。
2. **切换走 `stop_machine()`**（`kernel/time/timekeeping.c:1531-1537`）：
   全 CPU 停一瞬间。这一下会污染任何亚毫秒级计时（ guest 的 `clock_gettime` 尖刺、
   E3/E5 的分布尾部），所以本实验不与它们共用一轮数据，A5 的注入循环也
   **不跨越切换点**。
3. **`masterclock 0` 可能出现多条**：`pvclock_gtod_update_fn()` 给每个 vCPU 都置请求，
   第一个消费的重算并翻标志，后面的重算是幂等的但仍各打一条 trace。
   所以 A2 的判据是"至少一条"，不要拿条数当迁移数。
4. **`kvm_update_master_clock` 也会以 QEMU 主线程 tid 出现**：`kvm_arch_init_vm()`
   里的 `pvclock_update_vm_gtod_copy()`（`arch/x86/kvm/x86.c:12844`）在建 VM 时
   就发一条，此时 `online_vcpus` 还是 0 → 印出来的必然是
   `masterclock 0 offsetmatched 0`。**这条是正常噪声**，把它算进 A2 的计数会让
   "新建 VM"看起来像"主时钟被关掉"。脚本按 tid 集合过滤，只认该 VM 的
   vCPU 线程 + 建 VM 时刻之后的事件。
5. **`boot_vcpu_runs_old_kvmclock` 不是我们能拨的条件**：guest 只在写
   `MSR_KVM_SYSTEM_TIME`（旧版）时才翻它（`x86.c:2354-2361`，还要求
   `vcpu_id == 0 && !host_initiated`）。本仓 guest 走的是
   `MSR_KVM_SYSTEM_TIME_NEW`（`arch/x86/kernel/kvmclock.c:295-302`），
   所以它恒为 0，A2 的翻转只能归因到 `host_tsc_clocksource` 那一项。
6. **A4 之后 VM1 的时钟仍然在走，只是慢路径**：判据不要写成"guest 时间错乱"，
   主时钟关掉不影响正确性（pvclock 页每 100 ms 被宿主重写），
   影响的只有读时间快路径与 §3.1.2 那笔迁移账。
7. **别把 A5 的 `C-off` 与 A2 混为一谈**：`C-off` 的采样期宿主已经回到 `tsc`，
   它测的是"宿主在 TSC 上但主时钟被永久性地留在关闭态"这个**只有 P2 成立才存在**的状态。
8. **★ 观测窗必须跨过触发点，稳态窗里的 0 没有任何信息量**：`kvm_update_master_clock`
   只在六个重算点发出（`../annotations.md` §3.1.1 的表），VM 起来之后就**不再周期性地打**。
   所以"VM 已经跑了 20 s，这窗里 0 条 `masterclock 1`"完全不能证明主时钟是关的 ——
   它可能只是没人触发重算。脚本因此把每一臂的窗都开在**动作之前**、关在动作之后：
   A1 跨 boot、A2 跨切换、A4 跨 VM2 创建、A5 另开 boot / off-edge / 断言三个窗。
   A3 的"零"之所以算判据，前提正是 A1 用**同样长度且跨触发点**的窗拿到了非零（§4.2 第三条）；
   两个条件少一个，P2 就既不能被证实也不能被否证。

---

## 7. 结果

**待实测**。

| 臂 | 重复 | `masterclock 1` 条数 | `masterclock 0` 条数 | `kvm_track_tsc` 满足式? | `pvclock/秒` | guest 速率 | `tsc-offset` 变化 |
|---|---|---|---|---|---|---|---|
| A0 | | 0（预期） | 0（预期） | — | — | — | — |
| A1 | | 待实测 | | | | | 无（预期） |
| A2 | | | 待实测 | | | | |
| A3 | | **0（预期）** | | | | | |
| A4（VM2） | | 待实测 | | | | | |
| A4（VM1） | | 0（预期） | | | | | |
| A5 `C-off` | | 0（预期） | | 待实测 | 待实测 | |
| A5 `C-on` | | ≥1 | | ≈0（预期） | 待实测 | |

失效声明：本轮不上机，A2/A3 的切换动作未在真机执行过，§2.1 的风险评估来自源码
而非事故经验；A5 的速率差需要至少 3 次重复才允许写方向性结论，
样本不足时只报计数不报倍数。
