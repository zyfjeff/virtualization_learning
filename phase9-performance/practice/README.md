# 第九阶段实践：五个可跑的开销测量

> 基于 Linux 6.12.93 源码。测量规范只有一份，在 [`../measurement.md`](../measurement.md)；
> 本目录**不重复规范**，每份文档只管"这个实验特有的前置检查、判据与派生量"。
> 跨 phase 的性能结论索引在 [`../index.md`](../index.md)。

本轮（按项目决定）**不上机**：五个实验的结果表全部标"待实测"，但驱动脚本是齐备且自测过的
—— 五个脚本一律支持 `--preflight`（只读检查）与 `--dry-run`（打印将执行的动作、不碰系统）。

| 编号 | 文档 | 回答什么 | 脚本 |
|---|---|---|---|
| **E1** | [bench-ple.md](bench-ple.md) | 超卖场景下 PLE / directed yield 到底值多少钱 | [bench-ple.sh](bench-ple.sh) |
| **E2** | [bench-huge-dirty.md](bench-huge-dirty.md) | 大页 vs 4K、脏页日志的两笔代价（建表级 vs 写保护）分开量 | [bench-huge-dirty.sh](bench-huge-dirty.sh) |
| **E3** | [bench-migrate.md](bench-migrate.md) | vCPU 线程换一次物理核付的**四笔分开的账** | [bench-migrate.sh](bench-migrate.sh) |
| **E4** | [bench-clock-master.md](bench-clock-master.md) | 宿主 clocksource 一拨，KVM 主时钟快路径为什么**回不去** | [bench-clock-master.sh](bench-clock-master.sh) |
| **E5** | [bench-observer-cost.md](bench-observer-cost.md) | 每档观测手段自身的扰动量 —— 填 `../measurement.md` §4(b) 那张表 | [bench-observer-cost.sh](bench-observer-cost.sh) |

跑法统一是三步（`--arm` / `--all` 这些具体参数以各脚本 `-h` 为准；默认结果目录
`bench/<实验>-<时间戳>/`，E5 可用 `--out` 改）：

```bash
./bench-<X>.sh --preflight             # 只读，确认满足本文 §"前置" 的硬条件
./bench-<X>.sh --all --dry-run         # 打印时间线，确认它要写哪些地方
sudo ./bench-<X>.sh --all --repeat 3   # 真跑
```

## 通用前置（每个脚本都会查，写在这里是为了让人先看见）

1. **确认那台 VM 真的走了 KVM**：`ls -l /proc/<qemu-pid>/fd | grep -c kvm` 必须 > 0。
   缺 `-enable-kvm` 时 QEMU **静默**回退 TCG、不报错，宿主侧 `kvm:*` 零事件，
   每一档都会得出"没有 VM-Exit"的错误结论（`AGENTS.md` 陷阱 7）。
   `../../scripts/vm/boot-vm.sh` 默认带上 `-enable-kvm -cpu host` 并自检。
2. **ftrace 状态是全局的，跑前跑后都要还原**。`current_tracer` / `set_ftrace_filter` /
   `set_event` / `tracing_on` 四个出口**互相独立**，只清 `kprobe_events` 不算清干净
   （`AGENTS.md` 陷阱 9）。五个脚本都带 `clear_ftrace`；E5 另有 `restore_ftrace`，
   按开跑前 `orig-ftrace.txt` 的快照逐名回写。
3. **同一时刻只允许一个人在 trace**。E5 的 preflight 数 `/proc/*/fd` 里指向 `/dev/kvm`
   的**其它**引用、复读 `set_event` 与 `enabled_functions`、再用 `bpftool` 查外部 BPF
   有没有挂在 `kvm` 组上 —— 这三条才是"有没有别人在 trace"的真实出口。
   ★ tracefs 里**没有** `enabled_events` 这个文件，别照抄流传的写法
   （[`../corrections.md`](../corrections.md) D6）。
4. **算开销用绝对量，不用百分比**。本机 96 线程，1~2 CPU-s 的观测成本摊成百分比看着
   是 0，可它压在哪一个核上就让那一档吞吐掉 —— 见
   [`../measurement.md`](../measurement.md) §4(b)。

## guest 负载模块：`ple-load/`

E1 / E3 / E4 共用这一个模块：`workload=0` 是 N 个绑核线程抢一把 spinlock（E1 要的就是
锁争抢），`workload=1` 是每线程扫自己的私有缓冲区、无锁（E3 / E4 / E5 用它，理由见
[bench-migrate.md](bench-migrate.md) §3.1）。

```bash
cd ple-load && make && make install    # → ../../scripts/shared/ → guest /mnt/shared/
# guest 内： insmod /mnt/shared/ple_load.ko workload=1 nr_threads=4
# 读吞吐：   cat /sys/module/ple_load/parameters/completed      # 只读累计计数（0444）
```

★ `KDIR` 必须指向 guest 实际跑的那棵内核树（默认 `/root/code/linux-6.12.93`，
即 `../../scripts/vm/boot-vm.sh:178` 取 bzImage 的那棵），否则 vermagic 不匹配、
`insmod` 直接被拒。guest rootfs 里没有编译器，所以只能在宿主编译再经 9p 送进去。

## 两份旧文档：保留作反面记录

它们已被上面的实验取代，留在原地只为让 [`../corrections.md`](../corrections.md) 与
[`../measurement.md`](../measurement.md) 里"旧版错在哪"的引用还能翻到原文。
**两份文件顶部都有告示，不要照着跑**：

| 文档 | 状态 | 取代者 | 已知错误 |
|---|---|---|---|
| [ept-bench.md](ept-bench.md) | 已废弃 | E2 | 实验 1 用 function tracer 测 µs 量级的缺页路径，观测成本与被测量同级（`../measurement.md` §4 规则 2）；"`kvm:kvm_page_fault` 携带 `level`" 是假的（D2）；实验 3 把两笔代价记成一笔 |
| [timer-bench.md](timer-bench.md) | 部分失效 | E4 | 实验 6 那两行 `cat /sys/module/kvm/parameters/lapic_timer_advance_ns` 在 6.12.93 上不存在（D1 —— 那是宿主 6.8.0-51 的布局）。guest 侧的 clocksource / cyclictest 手段仍有效，但机制与结论一律看 `../../phase7-timer-virt/` |
