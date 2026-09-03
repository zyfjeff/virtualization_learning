# KVM 性能参数权威表

> 本章所有文档只在这里给出参数默认值与权限，别处一律指向本文。
> 数据基线：Linux **6.12.93** 源码 + 本机（宿主 `6.8.0-51-generic`）实读复核。
> 本文只讲**参数本身**（作用域/默认值/权限/生效方式），算法原理见各机制所属章节
> （halt-polling 算法 → `../phase0-kvm-framework/annotations.md`，PLE 见
> [annotations.md](annotations.md) §1）。

---

## 0. 读表前必须知道的两件事

### (a) 权限位决定你能不能改 —— 多数性能参数是只读的

`module_param()` 的第三个参数是 sysfs 权限。KVM 的性能参数**绝大多数是 `0444`**，
即 `/sys/module/*/parameters/*` 只读，`echo` 会报 `Permission denied`。这不是
root 不够，是内核根本没提供写入口。

| 权限 | 含义 | 改法 |
|---|---|---|
| `0644` | 运行期可写 | `echo N > /sys/module/<mod>/parameters/<name>`，下一个 VM/下一次生效 |
| `0444` | **只读** | 只能 `modprobe <mod> <name>=N`（需先卸载模块）或内核 cmdline |

做 A/B 实验时这条决定了实验形态：`0644` 的参数可以在同一台机器上连续切换采样；
`0444` 的参数必须**重启宿主或卸载/重载 `kvm_intel`**，每个数据点是一次独立开机。
`../phase8-capstone/practice/README.md` 的 halt-polling 实验能做成"一轮脚本扫四档"，
正是因为它用的四个参数全是 `0644`。

本机实测（`stat -c %a`，与源码 `module_param` 逐条一致）：

```
/sys/module/kvm/parameters/halt_poll_ns            perm=644   ← 可运行时改
/sys/module/kvm_intel/parameters/ple_window        perm=444   ← 只读
```

### (b) 宿主内核与源码基线不是一个版本

本仓库文档基于 **6.12.93**，但实验宿主跑的是 **6.8.0-51-generic**（`uname -r`）。
宿主侧读到的参数集合反映的是 6.8 的 KVM。已经有两处实测到的真实差异：

| 参数 | 6.12.93 源码 | 本机 6.8.0-51 实读 | 影响 |
|---|---|---|---|
| `lapic_timer_advance` | **存在**，`bool`，0444（`arch/x86/kvm/lapic.c:70-71`） | **不存在** | 见 §3 |
| `lapic_timer_advance_ns` | **不是模块参数**，只是 per-vCPU debugfs 只读文件（`arch/x86/kvm/debugfs.c:67`） | 存在，是模块参数，值 `-1` | 见 §3 |
| `halt_poll_ns_shrink` | 默认 **2**（`virt/kvm/kvm_main.c:93`） | 实读 **0**（`/proc/cmdline` 无相关项） | 见 §1 |

所以任何"在本机跑出来的参数值"都必须连同内核版本一起记录，不能当成 6.12.93 的
事实写进文档。开跑前的实存性自检方法见 [measurement.md](measurement.md) §5。

---

## 1. halt-polling（`kvm` 模块）

**定义位置**：`virt/kvm/kvm_main.c:78-94`（四个参数依次在 `:78/79`、`:83/84`、
`:88/89`、`:93/94`，前者是变量定义、后者是 `module_param`）。

```c
/* 来源: virt/kvm/kvm_main.c:78-94 (Linux 6.12.93) */
unsigned int halt_poll_ns = KVM_HALT_POLL_NS_DEFAULT;   /* :78 */
module_param(halt_poll_ns, uint, 0644);                 /* :79 */
unsigned int halt_poll_ns_grow = 2;                     /* :83 */
module_param(halt_poll_ns_grow, uint, 0644);            /* :84 */
unsigned int halt_poll_ns_grow_start = 10000; /* 10us */ /* :88 */
module_param(halt_poll_ns_grow_start, uint, 0644);      /* :89 */
unsigned int halt_poll_ns_shrink = 2;                   /* :93 */
module_param(halt_poll_ns_shrink, uint, 0644);          /* :94 */
```

上限常量的真身在 `arch/x86/include/asm/kvm_host.h:71`：

```c
#define KVM_HALT_POLL_NS_DEFAULT 200000
```

| 参数 | 作用域 | 6.12.93 默认 | 权限 | 本机 6.8 实读 |
|---|---|---|---|---|
| `halt_poll_ns` | kvm | **200000 ns = 200 µs** | 0644 | 200000 |
| `halt_poll_ns_grow` | kvm | 2 | 0644 | 2 |
| `halt_poll_ns_grow_start` | kvm | 10000 ns = 10 µs | 0644 | 10000 |
| `halt_poll_ns_shrink` | kvm | 2 | 0644 | **0** |

> **勘误**：本章旧版与 `phase0` / `notes` / `phase10` 共 10 处把 `halt_poll_ns`
> 默认值写成 `400000ns = 400µs`，源码与本机都是 **200000 = 200µs**。已全仓修正。

**`halt_poll_ns_shrink = 0` 的语义不是"关掉 shrink"**，而是走另一条分支：

```c
/* 来源: virt/kvm/kvm_main.c:3689-3706 */
	shrink = READ_ONCE(halt_poll_ns_shrink);
	...
	if (shrink == 0)
		val = 0;                   /* 直接清零窗口 */
	else
		val /= shrink;             /* 除法缩小 */
```

即 6.8 宿主的行为是"poll 未命中即把窗口清零"，而 6.12.93 默认是"除以 2"。
本机 0 这个值无法从现有证据区分是 6.8 的默认还是曾被运行期改写过（`/proc/cmdline`
里没有 halt_poll 相关项，倾向于前者），实验时必须按 [measurement.md](measurement.md)
§5 记录原值并在结束时恢复。

**观测出口（不需要 ftrace，零扰动优先用这些）**：

| 出口 | 位置 | 说明 |
|---|---|---|
| `/sys/kernel/debug/kvm/<pid>-<vm>/halt_attempted_poll` | `include/linux/kvm_host.h:1986` | 尝试 poll 次数 |
| `…/halt_successful_poll` | 同上 `:1985` | poll 内命中次数 |
| `…/halt_poll_invalid` | 同上 `:1987` | 无效唤醒次数 |
| `…/halt_wakeup` | 同上 `:1988` | 唤醒总次数 |
| tracepoint `kvm:kvm_halt_poll_ns` | `include/trace/events/kvm.h:347` | grow/shrink 轨迹，参数 `grow, vcpu_id, new, old`（grow/shrink 是 `:373-376` 的两个包装宏） |

注意 `halt_*_poll` 是 **per-vCPU** 统计，debugfs 里每个统计项是**单独一个文件**，
不存在一个叫 `stats` 的聚合文件（`virt/kvm/kvm_main.c:6352,6363` 用
`debugfs_create_file(pdesc->name, …)` 逐项注册）。`cat .../stats` 是无效命令。

**收益形态**：halt-polling 换来的是**延迟 ↔ CPU 的交换，不是单方面提升**。
本仓已实测的曲线见 `../phase8-capstone/practice/README.md` 项目 4 M3（poll 开关 ×
idle/flood × 四档窗口扫描），索引见 [index.md](index.md)。

---

## 2. PLE / Pause-Loop-Exiting（`kvm_intel` 模块，**全部只读**）

**定义位置**：`arch/x86/kvm/vmx/vmx.c:203-220`。

| 参数 | 6.12.93 默认 | 权限 | 本机实读 |
|---|---|---|---|
| `ple_gap` | 128 | **0444** | 128 |
| `ple_window` | 4096 | **0444** | 4096 |
| `ple_window_grow` | 2 | **0444** | 2 |
| `ple_window_shrink` | **0** | **0444** | 0 |
| `ple_window_max` | **UINT_MAX = 4294967295** | **0444** | 4294967295 |

默认常量集中在 `arch/x86/kvm/x86.h:72-77`：

```c
/* 来源: arch/x86/kvm/x86.h:72-77 (Linux 6.12.93) */
#define KVM_DEFAULT_PLE_GAP		128
#define KVM_VMX_DEFAULT_PLE_WINDOW	4096
#define KVM_DEFAULT_PLE_WINDOW_GROW	2
#define KVM_DEFAULT_PLE_WINDOW_SHRINK	0
#define KVM_VMX_DEFAULT_PLE_WINDOW_MAX	UINT_MAX
#define KVM_SVM_DEFAULT_PLE_WINDOW_MAX	USHRT_MAX
```

> **勘误**：本章旧版 `README.md` 写 `ple_window_shrink` 默认 2（实为 **0**）、
> `ple_window_max` 默认 16384（实为 **UINT_MAX**，本机读到 4294967295）。
> 另：AMD 侧默认窗口是 `KVM_SVM_DEFAULT_PLE_WINDOW = 3000`，与 VMX 的 4096 不同，
> 跨厂商对比时别混用。

`ple_window_shrink = 0` 与 halt-polling 同构 —— 0 不是"关闭"，而是"直接回到基值
`ple_window`"而非除以 shrink（`__shrink_ple_window`，`arch/x86/kvm/x86.h:96`）。

**两个致命的实验陷阱**（写实验设计前先看，详见 `practice/bench-ple.md`）：

1. **改参数要重载模块**。五个参数全 0444，`echo` 无效。要么
   `modprobe -r kvm_intel && modprobe kvm_intel ple_window=…`（会踢掉宿主上所有 VM），
   要么内核 cmdline，要么走 per-VM 的 `KVM_X86_DISABLE_EXITS_PAUSE`
   （`arch/x86/kvm/x86.c` 的 `KVM_CAP_X86_DISABLE_EXITS` 路径）做"完全关掉"这一档。
2. **per-vCPU 窗口只增长不回落到全局值**。`grow_ple_window()`
   （`arch/x86/kvm/vmx/vmx.c:1417`）把 `vmx->ple_window` 几何放大，唯一的回落点是
   `vmx_vcpu_load()`（`arch/x86/kvm/vmx/vmx.c:1519-1523`）：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:1519-1523 */
void vmx_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	if (vcpu->scheduled_out && !kvm_pause_in_guest(vcpu->kvm))
		shrink_ple_window(vcpu);
```

即回落**只在"该 vCPU 被抢占过、且 PLE 生效"时**发生。`ple_window_shrink` 默认 0
意味着回落直接回基值。**这不影响"每一轮实验必须重启 VM"** —— 因为窗口状态是
per-vCPU 的进程内状态，同一 VM 的第二轮会继承第一轮的窗口，采样必须一次性做完。

---

## 3. 定时器与抢占（`kvm` 模块）

| 参数 | 作用域 | 6.12.93 默认 | 权限 | 本机 6.8 实读 |
|---|---|---|---|---|
| `lapic_timer_advance` | kvm | `true` | **0444** | **不存在** |
| `min_timer_period_us` | kvm | 200 | 0644 | — |
| `kvmclock_periodic_sync` | kvm | true | **0444** | Y |
| `pi_inject_timer` | kvm | 0（`bint`） | 0644 | 0 |
| `preemption_timer` | kvm_intel | 见下 | **0444** | Y |

**`lapic_timer_advance_ns` 不是模块参数** —— 这是本章旧版的一处硬错误。
`practice/timer-bench.md` 旧版给的
`cat /sys/module/kvm/parameters/lapic_timer_advance_ns` 在 6.12.93 上根本走不通，
而 `../phase7-timer-virt/annotations.md:581` 早就明确写过
"★ 不存在 `lapic_timer_advance_ns` 模块参数"。6.12.93 的真实形态是：

- 模块参数只有 `lapic_timer_advance`（`bool`，`arch/x86/kvm/lapic.c:70-71`，0444 只读），
  控制"要不要自适应调整"；
- 调整量本身是 per-vCPU 状态 `apic->lapic_timer.timer_advance_ns`，由
  `adjust_lapic_timer_advance()`（`arch/x86/kvm/lapic.c:1840`）按到期误差自动增减，
  并通过 **debugfs 只读文件**暴露：`arch/x86/kvm/debugfs.c:67`
  `debugfs_create_file("lapic_timer_advance_ns", 0444, …)`；
- 调整有死区与步长：`abs(delta)` 超出 `LAPIC_TIMER_ADVANCE_ADJUST_MAX` 或小于
  `LAPIC_TIMER_ADVANCE_ADJUST_MIN` 时不动（`lapic.c:1848-1851`）。

所以想"手动设固定提前量"在 6.12.93 上**做不到**，只能观测自适应结果。
宿主是 6.8 时反过来：有 `lapic_timer_advance_ns` 参数、没有 `lapic_timer_advance`
bool。跨版本抄命令必错。

---

## 4. TSC 相关（`kvm` 模块）

| 参数 | 6.12.93 默认 | 权限 | 本机实读 |
|---|---|---|---|
| `tsc_tolerance_ppm` | 250 | 0644 | 250 |

定义 `arch/x86/kvm/x86.c:167-168`（源码注释：*"tsc tolerance in parts per million -
default to 1/2 of the NTP threshold"*）。它决定主时钟能否维持：见
[annotations.md](annotations.md) §3。

---

## 5. MMU / 大页 / 脏页跟踪

| 参数 | 作用域 | 6.12.93 默认 | 权限 | 本机实读 |
|---|---|---|---|---|
| `ept` | kvm_intel | 1（`enable_ept`） | **0444** | Y |
| `eptad` | kvm_intel | 1（`enable_ept_ad_bits`） | **0444** | Y |
| `pml` | kvm_intel | 1（`enable_pml`） | **0444** | Y |
| `vpid` | kvm_intel | 1（`enable_vpid`） | **0444** | Y |
| `tdp_mmu` | kvm | 1（`tdp_mmu_enabled`） | **0444** | Y |
| `eager_page_split` | kvm | true | 0644 | Y |
| `nx_huge_pages` | kvm | **-1（auto）** | 0644（`module_param_cb`） | Y |
| `nx_huge_pages_recovery_ratio` | kvm | 60（`CONFIG_PREEMPT_RT` 下为 0） | 0644（cb） | — |
| `flush_on_reuse` | kvm | false（`force_flush_and_sync_on_reuse`） | 0644 | N |

定义位置：`ept` `arch/x86/kvm/vmx/vmx.c:99`、`eptad` `:106`、`pml` `:128`、
`vpid` `:90`、`enable_apicv` `:114`（均 0444）；`tdp_mmu`
`arch/x86/kvm/mmu/mmu.c:112`、`flush_on_reuse` `:97`；`eager_page_split`
`arch/x86/kvm/x86.c:193-194`；`nx_huge_pages` 变量 `arch/x86/kvm/mmu/mmu.c:64`、
注册 `:87`（`module_param_cb`，故显示值 `Y`/`N` 是 `get_nx_huge_pages()` 翻译后的
结果，不是原始 `-1`）；`nx_huge_pages_recovery_ratio` `:68/:70`。

**做 EPT 大页实验必须知道的三条因果**（源码依据见 [annotations.md](annotations.md) §2）：

1. 大页能不能建，**与 `eptad` 无关**。真实闸门是
   `__kvm_mmu_max_mapping_level()`（`arch/x86/kvm/mmu/mmu.c:3138`）里的
   `disallow_lpage` 计数与宿主侧 `host_pfn_mapping_level`。旧文档写"EPT A/D 位支持
   是启用条件"是错的。
2. **开启脏页跟踪会让新建映射退到 4K**，判据在
   `kvm_mmu_hugepage_adjust()`（`arch/x86/kvm/mmu/mmu.c:3185-3186`）：
   `if (kvm_slot_dirty_track_enabled(slot)) return;`（helper 定义
   `include/linux/kvm_host.h:628`）。这条让"脏日志开销"和"大页收益"两个变量**互相
   纠缠**，测量时必须拆开归因 —— 见 `practice/bench-huge-dirty.md`。
3. `nx_huge_pages` 是 **iTLB multihit 漏洞缓解**，不是性能开关，但会**实打实地砍掉
   可执行大页**：`fault->huge_page_disallowed = fault->exec &&
   fault->nx_huge_page_workaround_enabled`（`mmu.c:3177`）。它是 `-1/0/1` 三态
   cb 参数，不是 bool。

---

## 6. APIC 虚拟化与中断（`kvm_intel` 模块）

| 参数 | 权限 | 本机实读 |
|---|---|---|
| `enable_apicv` | **0444** | Y |
| `fasteoi` | **0444** | — |
| `enable_ipiv` | **0444** | — |
| `vector_hashing`（kvm） | **0444** | — |

定义 `arch/x86/kvm/vmx/vmx.c:114`、`:112`、`:117`；`vector_hashing`
`arch/x86/kvm/x86.c:171`。APICv / Posted Interrupt 的机制与实测在
`../phase4-interrupts/posted-interrupts.md`，本文只管参数。

---

## 7. 关掉退出：per-VM 而非模块级

想"消除某类 VM-Exit"，多数情况正确的开关不是模块参数，而是
`KVM_CAP_X86_DISABLE_EXITS`：

```
KVM_X86_DISABLE_EXITS_MWAIT / HLT / PAUSE / SHUTDOWN / PLE
```

QEMU 侧对应 `-overcommit cpu-pm=on`（禁 MWAIT/HLT）。与 `ple_gap=0` 的效果一致
（都落到 `kvm->arch.pause_in_guest = true`，见 `arch/x86/kvm/vmx/vmx.c:7639-7640`
的 `vmx_vm_init()`），但**不需要重载模块、只影响本 VM**，做对照实验时优先用它。

---

## 8. 速查：哪些参数能在一轮实验里连续扫

| 可以直接 `echo` 扫 | 必须重载模块 / 重启 |
|---|---|
| `halt_poll_ns`、`halt_poll_ns_grow`、`halt_poll_ns_grow_start`、`halt_poll_ns_shrink`、`eager_page_split`、`nx_huge_pages`、`nx_huge_pages_recovery_ratio`、`flush_on_reuse`、`min_timer_period_us`、`pi_inject_timer`、`tsc_tolerance_ppm`、`ignore_msrs`、`report_ignored_msrs` | `ple_*` 全部、`ept`、`eptad`、`pml`、`vpid`、`enable_apicv`、`preemption_timer`、`tdp_mmu`、`lapic_timer_advance`、`kvmclock_periodic_sync`、`vector_hashing`、`enable_pmu` |

实验设计里凡落在右列的参数，**每个数据点都要独立开机重跑**，样本量和耗时按
[measurement.md](measurement.md) §2 另算。
