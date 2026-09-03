# E3 · vCPU 迁移：一次"换了个核跑"到底付多少钱

> 机制侧的 `vmx_vcpu_load()` 全链在 `../../phase1-vtx-basics/` 只讲到 VMCS 加载，
> 本文只量**代价**，不重讲机制。测量规范见 [`../measurement.md`](../measurement.md)。
> **本轮不上机**，数字全部待实测。

---

## 1. 要回答的问题

"vCPU 线程被宿主调度器换到另一个物理核"这件事，在源码里不是一个代价，而是
**四笔分开的账**，它们互相掩盖：

```
宿主把 vCPU 线程放到新 pCPU 上并调度进来
  └─ kvm_sched_in()                        virt/kvm/kvm_main.c:6375
       └─ kvm_arch_vcpu_load(vcpu, cpu)    arch/x86/kvm/x86.c:4982
            ├─ ① VMCS/TLB 账：vmx_vcpu_load_vmcs()   vmx.c:1449
            │     already_loaded = loaded_vmcs->cpu == cpu（:1453）
            │     不等时才：loaded_vmcs_clear()  :1457 → IPI 到旧核
            │                 indirect_branch_prediction_barrier()  :1486
            │                 kvm_make_request(KVM_REQ_TLB_FLUSH)   :1496
            │                 重写 HOST_TR_BASE / HOST_GDTR_BASE    :1504-1505
            │     → 下一次 vmentry 前影子 TLB 全清（x86.c:10828 服务该请求）
            ├─ ② PI 账：vmx_vcpu_pi_load(vcpu, cpu)  posted_intr.c:53
            │     快路径 :74  —— 没迁移且不在 wakeup 链表上时只清 SN 就返回
            │     真迁移时 :109 new.ndst = dest 重算 + cmpxchg64 重写 PID.control
            ├─ ③ 时钟账：x86.c:5014 的 `vcpu->cpu != cpu` 分支
            │     KVM_REQ_MIGRATE_TIMER      :5037（ LAPIC 定时器按新 CPU 重挂）
            │     KVM_REQ_GLOBAL_CLOCK_UPDATE :5035（仅 !use_master_clock 时）
            │     宿主 TSC 若被判 unstable → 重写 tsc offset（:5018/:5023）
            └─ ④ steal 账：x86.c:5041 每次 load 都置 KVM_REQ_STEAL_UPDATE，
                  put 时 kvm_steal_time_set_preempted()  :5044/:5106
```

**硬件侧还有一笔看不见账本的**：换了物理核 = L1/L2 私有缓存作废、
影子 TLB 作废（①已含）、以及 guest 数据在 LLC 里的落点变化。这一笔源码里
没有对应函数，只能靠"同样的注入节奏、只换 SMT 兄弟 vs 换物理核"反推（§3 的 M4）。

四个可判定的子问题：

| # | 问题 | 判据（臂名见 §3） |
|---|---|---|
| Q1 | 迁移是不是真的有代价（而不是"测不出"） | **M2 vs M0**，且必须先看 `loaded_vmcs_clear` 命中数确实随注入上升 |
| Q2 | 代价里有多少其实来自"注入动作本身"（syscall + 停止/重启开销） | **M3 vs M0**：M3 ≈ M0 才说明 M2 的差是真迁移造成的 |
| Q3 | 换核的软件账（VMCS/TLB/PI）与硬件账（缓存）各占多少 | **M4 vs M2**：M4 只换逻辑 CPU、同物理核，软件账照付、缓存账几乎不付 |
| Q4 | 自然调度（不干预）离最优有多远 | **M1 vs M0** |

---

## 2. 前置检查

### 2.1 "迁移"必须由宿主侧统计自证，不能靠"我改了亲和性"

`sched_setaffinity(2)` 成功 ≠ 发生了迁移。目标核正忙、任务不在运行、
`cpuset` 把允许集重新裁过，都会让它静默地"什么也没动"。
唯一算数的是**宿主 KVM 侧真的走了 not-already_loaded 分支**：

```
loaded_vmcs_clear()  arch/x86/kvm/vmx/vmx.c:812
   调用点只有两处：vmx_vcpu_load_vmcs() 的 !already_loaded 分支（:1457）
                   free 路径（:2937）
```

所以本实验用 ftrace **function profiler** 数 `loaded_vmcs_clear` 的命中数，
它是"真 VMCS 迁移次数"的下界（另加每 vCPU 一次关机时的 free，见 §6.2）。
配套的三个计数把分母与旁路补齐：

| 计数 | 含义 | 符号 |
|---|---|---|
| 注入次数 | 脚本主动改亲和性的次数（已知量） | `injected` |
| `loaded_vmcs_clear` | 真发生了 VMCS 换核的次数 | `vmcs_clear` |
| `kvm_arch_vcpu_load` | vCPU 线程被调度进来的总次数 | `arch_load` |
| `vmx_vcpu_pi_load` | PI load 被调用的总次数（含快路径） | `pi_load` |

`vmcs_clear / injected < 1` 就说明注入有落空的，M2 的差值不能全归给迁移；
`vmcs_clear / arch_load` 就是"每次调度进来有多大概率是真迁移"，M0 应接近 0。

### 2.2 只能碰 `CPU <n>/KVM` 线程

QEMU 的 vCPU 线程名是 `"CPU %d/KVM"`
（`accel/kvm/kvm-accel-ops.c:70`，QEMU 10.1.0-rc2；缓冲区
`VCPU_THREAD_NAME_SIZE 16`，`include/system/cpus.h:13`，故 comm 里最多 15 字符）。
主线程、`vhost-xxx`、`iohandler`、`kvm-reaper` 都不该动 —— 动了会把
设备模拟的放置变化混进"vCPU 迁移代价"里。脚本按
`/proc/<pid>/task/*/comm` 精确匹配 `CPU <i>/KVM` 反查 tid，
匹配不到就拒绝开跑（宁可少一组臂，也不要静默地没改到）。

### 2.3 两组核必须是"不同物理核、同 NUMA node"

本机 `lscpu`：96 逻辑 CPU / 24 核 × 2 SMT × 2 socket，
node0 = `0-23,48-71`，且 `cpu0` 的兄弟是 `0,48`（`thread_siblings_list` 实测）。
所以：

```
A 组 = 0,1,2,3     B 组 = 4,5,6,7     （都是 node0 的不同物理核）
SMT 对 = (i, i+48)                     （同一物理核的另一半）
```

选核时必须用 `thread_siblings_list` 现算，不能照抄本文编号 ——
换一台机器 `0-7` 很可能就是 4 个核的 8 个 SMT 逻辑 CPU，那样 M0 从一开始
就带着 SMT 争抢，整组数据失去意义。preflight 会把每个所选核的兄弟打出来。

### 2.4 观测出口的实存性（6.8 宿主 ≠ 6.12.93 文档）

宿主跑 `6.8.0-51`，本文行号基于 `6.12.93`；两侧的可用性必须现场查
（`../measurement.md` 的版本一致性硬规则）。本机
`available_filter_functions` 实测结论：

| 符号 | 6.8.0-51 宿主 | 备注 |
|---|---|---|
| `kvm_arch_vcpu_load` / `kvm_arch_vcpu_put` | ✅ 可跟踪 | |
| `vcpu_load` | ✅ 可跟踪 | |
| `vmx_vcpu_load` / `vmx_vcpu_put` | ✅ 可跟踪 | |
| `vmx_vcpu_pi_load` / `vmx_vcpu_pi_put` | ✅ 可跟踪 | |
| `loaded_vmcs_clear` | ✅ 可跟踪 | §2.1 的主判据 |
| `record_steal_time` | ✅ 可跟踪 | |
| `kvm_vcpu_flush_tlb_all` | ❌ **不在表里**（静态函数已内联） | 6.12.93 里它是 `x86.c:3612`，但别指望能挂上 |

`tlb_flush` 这项统计因此只能走 debugfs 计数器（`x86.c:267`，
`++vcpu->stat.tlb_flush` 在 `x86.c:3614/:3623/:3648`），不能走 ftrace 函数名。

### 2.5 debugfs 统计文件的位置随版本变，必须"发现"而不是硬编码

6.12.93 里 VM 级与 vCPU 级统计都挂在 **per-VM 目录**
`/sys/kernel/debug/kvm/<pid>-<fd>/`（`kvm_create_vm_debugfs()`
`virt/kvm/kvm_main.c:1047`，目录名格式 `"%d-%s"` 见 `:1061`）。
但**本机 6.8.0-51 实测**：没有任何 VM 在跑时，
`/sys/kernel/debug/kvm/` 根下直接就是 `tlb_flush`、`exits`、`halt_poll_invalid`…
一整批文件（读值为 0）。两者布局不同，脚本一律按
"`/sys/kernel/debug/kvm/<name>` 或 `/sys/kernel/debug/kvm/*/<name>` 谁可读用谁"
发现路径，并在 preflight 里打印实际用的是哪种。
per-vCPU 的文件则是 `<pid>-<fd>/vcpu<i>/`（`kvm_main.c:4204` 的 `"vcpu%d"`），
里面有 `tsc-offset`（`arch/x86/kvm/debugfs.c:63`，0444）与 `pid`（`kvm_main.c:4207`）。

---

## 3. 实验矩阵

负载固定：guest 4 vCPU，`ple_load.ko workload=1`（每线程只扫自己的私有缓冲区，
**无锁**）。用它而不用 `workload=0` 的理由见 §3.1。

| 臂 | 宿主放置策略 | 回答 |
|---|---|---|
| M0 | 每个 vCPU 线程独占一个不同物理核（A 组），全程不动 | 零迁移基线 |
| M1 | 全部 vCPU 线程允许在 A∪B 共 8 个物理核上自由调度 | Q4 自然调度 |
| M2 | 每 `--step-ms` 把 4 个线程整体 A→B→A…（1:1，不超卖） | Q1 真迁移 |
| M3 | 与 M2 同节奏调 `sched_setaffinity`，但目标 = 当前所在核 | Q2 阴性对照 |
| M4 | 每 `--step-ms` 在 `(i, i+48)` 之间来回（同一物理核） | Q3 软件账/硬件账分离 |

### 3.1 为什么 M 组不能沿用 E1 的 spinlock 负载

`workload=0` 的吞吐由**一条 cacheline 在核间接力**主导。把 vCPU 线程搬走会同时
改变接力距离，速率必然掉 —— 但那既不是 VMCS 重载、也不是 TLB 冲刷的代价，
而是"锁争抢路径变长"的代价，与 E1 的问题纠缠。E3 要的是
"同样的计算、同样的私有数据，只换落点"，所以每个线程只碰自己独占的缓冲区，
计数器也按线程独占 cache line（`ple-load/ple_load.c` 里的 `struct tcounter`）。

### 3.2 为什么 M2 不超卖

一旦允许核数 < 线程数，就同时引入了"被抢占"这一笔（那是 E1 的主问题），
迁移代价被吞进排队延迟里。M2 的注入是**等量核之间的整体搬家**：
任一时刻 4 个线程各自占一个物理核，只是核换了。

### 3.3 M4 的读法边界

M4 与 M2 之差 ≈ "换物理核"相对"只换逻辑 CPU"多出来的那部分（缓存/TLB 落点）。
但 Intel 上 L1/L2 TLB 与 SMT 兄弟的共享关系本文**不做断言**（未查规范），
所以 M4 的结论只能写成"上下界读法"，不能写成"SMT 共享 L2 TLB"。

---

## 4. 观测点与判据

### 4.1 因变量（结果侧）

| 量 | 采法 | 备注 |
|---|---|---|
| guest 完成速率 | 前后两次读 `/sys/module/ple_load/parameters/completed`，同 E1 | workload=1 时单位是"扫完一轮私有缓冲区的次数" |
| guest 侧 steal | guest 内 `grep '^cpu ' /proc/stat` 第 8 列差值 | 宿主把线程停在路上就会被计入 steal，是"迁移伤害"的 guest 视角 |
| 宿主 per-tid 落点 | `/proc/<tid>/stat` 的第 39 个字段 | 就是 `task_cpu()`（`fs/proc/array.c:642`）。comm 可能含空格，**先剥到最后一个 `) ` 再数**；剥完之后它变成剩余串的**第 37 个**字段，照抄 39 会读到别的列 |

### 4.2 自变量是否真生效（机制侧）

ftrace function profiler 数命中次数，"为什么不用 `current_tracer=function`"同 E2 §4.2，
不重复。但**名单必须一个个写**：

```bash
echo 0 > /sys/kernel/tracing/function_profile_enabled
echo > /sys/kernel/tracing/set_ftrace_filter
for f in kvm_arch_vcpu_load vmx_vcpu_load loaded_vmcs_clear \
         vmx_vcpu_pi_load record_steal_time; do
    echo "$f" >> /sys/kernel/tracing/set_ftrace_filter 2>/dev/null || echo "进不去：$f"
done
echo 1 > /sys/kernel/tracing/function_profile_enabled
# ……采样……
echo 0 > /sys/kernel/tracing/function_profile_enabled
cat /sys/kernel/tracing/trace_stat/function*      # 取各 CPU 的 Hit 求和
```

### 4.2.1 `set_ftrace_filter` 的三种静默失效（本机 6.8.0-51 实测，各重复三遍一致）

**(a) 名字要写裸名，不能照抄 `available_filter_functions` 的显示。** 模块里的函数
在那张表里印成 `kvm_vcpu_on_spin [kvm]`、`loaded_vmcs_clear [kvm_intel]` ——
所以判断"可跟踪"**不能** `grep -x 名字`，要用 `^名字( |\[|$)`
（三个脚本的 `traceable()` 就是这个式子）。把带 `[kvm]` 的整串原样写进 filter，
`[kvm]` 会被当成**第二个名字**并匹配失败，于是这条写入返回错误 —— 而裸名部分
其实已经装进去了，报错信息纯属噪音。

**(b) 一次写好几个名字时，坏名字会吞掉它后面的全部名字。**

| 写法（前面都已 `echo >` 清空） | write 报错 | 实际装进 filter 的 |
|---|---|---|
| `echo "good1 good2" > …` | 否 | 两个都在 |
| `echo "good1 bad good2" > …` | **是** | **只有 good1** —— 走到坏名字就停，good2 连带丢掉 |
| `echo "bad good1" > …` | **是** | **一个都没有**，filter 仍是 `#### all functions enabled ####` |

机制：`trace_get_user()` 每次只取**一个空白分隔的 token**
（`kernel/trace/trace.c:1790` 的 `while (cnt && !isspace(ch) && ch)`），
`match_records()` 拿这一个 glob 去全量 mcount 记录里找、一个都没命中就返回 0
（`kernel/trace/ftrace.c:4849-4856`），`ftrace_process_regex()` 把 0 变成
`-EINVAL`（`:5682-5684`），`ftrace_regex_write()` 见负值直接 `goto out`
（`:5737-5738`）—— **这一次 write 剩下的字节再也不解析**。已装入的不回滚。

**(c) ★ 最坑的是第三行：filter 空 = 全部函数都开。** 空 filter 在
`hash_contains_ip()` 里被当作**恒匹配**（`kernel/trace/ftrace.c:1513`
`ftrace_hash_empty(hash->filter_hash) || …`）。于是 function profiler 会统计
整台机器的每个函数（`trace_stat` 几千行、明显变慢），而我们要的几个名字**恰好在
"全部"里面**，打印出的 Hit 数字看着完全正常 —— 实验其实已经废了。
**所以判据不能只看返回值**：必须一个名字一次 `>>`，并把没装进去的名字逐个打印出来
（三个 bench 脚本的 `start_profile` 现在都这么写，`stop_profile` 再给没命中的名字
显式补 `0`，把"没装进去"和"装了但零命中"分开）。

顺带一条没追到机制的：同一串名字改用**换行**分隔时，实测坏行之后的好行**仍然**会装入。
别把它当稳定契约，逐名字单独写是唯一不用猜的写法。

四个必须同时成立的判定条件（缺一即"测不出"，不是"没有代价"）：

1. `M2.vmcs_clear ≈ injected`（M3 应≈0，M0 应≈0）。
2. `M2` 与 `M0` 的差值 **大于** E5 的扰动预算（[`../measurement.md`](../measurement.md) §4）。
3. `M3` 与 `M0` 无显著差 —— 否则 M2 的差里混了注入动作本身（syscall + IPI + 停止/重启）。
4. 每臂 `kvm:kvm_vcpu_wakeup` 里 `waited=true` 的那一侧（打印成 `wait time … ns`）≈ 0。
   ★ 这条 tracepoint 在 `kvm_vcpu_halt()` 结尾**无条件**发一条
   （`kvm_main.c:3880` → `trace_kvm_vcpu_wakeup(halt_ns, waited, vcpu_valid_wakeup(vcpu))`），
   轮询成功也发，所以**事件总数 ≈ halt 次数**，总数本身不构成判据；只有第二个字段
   `waited`（`kvm_main.c:3837` 的 `waited = kvm_vcpu_block(vcpu)`）才代表真的睡过去过。
   没有 `wait`，才说明采样窗内 vCPU 没进阻塞路径，PI 的 wakeup-链表分支
   （`posted_intr.c:91`，判据 `pi_desc->nv == POSTED_INTR_WAKEUP_VECTOR`；
   `:74` 是它的反面 —— 快路径守卫）没有参与，归因才干净。

### 4.3 三个阴性对照

| 对照 | 期望 | 不符说明什么 |
|---|---|---|
| 每 vCPU 的 `tsc-offset` 注入前后 | **不变** | 变了 = 走进 `x86.c:5014-5023` 的 TSC-unstable 分支，宿主 TSC 被判不稳定，本臂数据连同"迁移代价"一起作废（时钟也在被动调整） |
| M0 的 `vmcs_clear` | ≈ 0（只允许关机时那一次，见 §6.2） | 不为 0 → 绑核没生效（cgroup `cpuset` 覆盖、或 QEMU 自己又改了亲和性） |
| M3 的 `vmcs_clear` | ≈ 0 | 明显 > 0 → "设成当前核"在该内核上仍会触发迁移，M3 不再是合格对照 |

### 4.4 与 E1 的纠缠点

`scheduled_out` 为真时 `vmx_vcpu_load()` 会先调 `shrink_ple_window()`
（`vmx.c:1520-1523`）。注入迁移必然伴随 out/in，所以 M2 的每步都顺带缩了 PLE 窗口。
本实验**不控制**它（控制要重载 `kvm_intel` 关 PLE，代价是把 E3 变成 E1 的变体），
改为把它显式化：加 `--with-ple` 时同时开 `kvm:kvm_ple_window_update`，
把每臂的更新次数记下来，报告里作为已知干扰项列出。

---

## 5. 执行

```bash
./bench-migrate.sh --preflight
./bench-migrate.sh --arm M2 --dry-run
sudo ./bench-migrate.sh --all --repeat 5 --step-ms 200 --sample-s 20
sudo ./bench-migrate.sh --arm M0 --arm M2 --repeat 7 --smt-pair   # 只做 Q1/Q3
```

单臂时序（一次 boot 一个臂，绝不"同一台 VM 里换放置策略"）：

```
建 VM（4 vCPU，A 组核）→ 等 guest 就绪 → 校验 /dev/kvm fd 数
→ 反查 CPU <i>/KVM 的 tid，逐线程 taskset 绑到 A 组
→ 读 tsc-offset 基线、清 debugfs 统计（可写则 echo 0）
→ insmod ple_load.ko workload=1 priv_kb=256 nr_threads=4 → 预热
→ 开 profiler（+ 可选 tracepoint）
→ 跑注入循环 N 步（M0/M1 不注入，M3 空注入，M4 用 SMT 对）
→ 两次读 completed 夹一个采样窗
→ 关 profiler 并落盘（★ 必须在关机之前，见 §6.2）
→ 读 tsc-offset 尾值、抓 per-tid 落点、guest steal
→ 关机
```

---

## 6. 已知坑

1. **`sched_setaffinity` 到 cpuset 允许的核之外会被静默裁剪**。如果宿主上
   QEMU 落在某个 `cpuset` cgroup 里，注入会"成功返回但没搬家"。
   脚本在注入前先读 `/proc/<tid>/status` 的 `Cpus_allowed_list` 并校验目标核在内。
2. **profiler 统计必须在关机之前取**。`loaded_vmcs_clear()` 的第二个调用点在
   `free_loaded_vmcs()`（`vmx.c:2933`）里（`:2937`），VM 销毁时每个 vCPU 都会再命中
   一次；关机后再读就把这 N 次算进了"迁移次数"，M0 永远不是 0。
   `stop_profile` 因此放在 kill QEMU 之前，并在脚本注释里钉死顺序。
3. **每步注入会产生 IPI 与 `stop_one_cpu` 开销**，它随核数与宿主负载变。
   没有 M3 就分不开这笔与真迁移 —— M3 不是可选项。
4. **M1 的"自由调度"其实由 `kvm_preempt_ops` 决定统计口径**：
   `kvm_arch_vcpu_load()` 每次调度进来都会跑（`kvm_main.c:6383`），
   不止迁移时。所以 `arch_load` 是分母不是分子，别把它的增量当迁移数。
5. **steal time 只在 `preempted` 为真时写入**（`x86.c:5044`，由
   `kvm_sched_out()` 的 `task_is_runnable` 判定，`kvm_main.c:6395-6397`）。
   绑核臂与注入臂的 steal 差可能是"被抢占概率"差而不是"迁移"差，
   所以 §4.2 条件 4 要求先看阻塞/抢占量级。
6. **注入节奏是本实验的自变量，不是调优参数**。报告里必须写 `--step-ms`，
   不同 `--step-ms` 的 M2 结果不可互相比较（代价 ≈ 每次迁移的固定成本 × 次数，
   次数与 1/step 成正比）。
7. **别在 M4 里把两个线程挤到同一逻辑 CPU**。同一物理核的两个线程会退化
   成 SMT 争抢，而 M4 想测的是"换逻辑 CPU 不换物理核"。
8. **给 `vmx_vcpu_pi_load()` 一类的 kvm_intel 符号下 kprobe 前**，先确认
   它没被 `.isra`/`.constprop` 改名（AGENTS.md 陷阱 9）。本实验只用 function
   profiler 数命中，不取参数，正是为了绕开这条。
9. **宿主上任何别的负载都会同时污染四个臂**（96 个核不等于"有 8 个空闲核"）。
   preflight 会打印所选核的当前 `load`，跑之前人工确认这段时间没有别人在用。

---

## 7. 结果

**待实测**。

| 臂 | 重复 | guest 速率中位 | 相对 M0 | guest steal | `arch_load` | `vmcs_clear` | `pi_load` | injected | `wakeup` wait/poll | `tsc-offset` 变化 |
|---|---|---|---|---|---|---|---|---|---|---|
| M0 | | 待实测 | 1.00 | | | | | 0 | 待实测 | 无（预期） |
| M1 | | 待实测 | | | | | | 0 | | |
| M2 | | 待实测 | | | | | | | | |
| M3 | | 待实测 | | | | | | （=M2） | | |
| M4 | | 待实测 | | | | | | | | |

`injected` 记的是**整臂合计**（每步 × 每 vCPU 线程各一次），与 `vmcs_clear` 同量纲；
`wakeup wait/poll` 是 §4.2 条件 4 的证据，`wait` 非 0 的那一臂要在报告里单独说明。
