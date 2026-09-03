# E2 · 大页 × 脏页日志：把两笔代价拆开量

> 取代旧版 `ept-bench.md`（其"实验 3 脏页日志开销"把两笔账记成了一笔）。
> EPT/TDP MMU 的**机制**在 `../../phase2-mem-virt/`，本文只量**代价**。
> **本轮不上机**，数字全部待实测。

---

## 1. 要回答的问题

旧版文档（`../corrections.md` B3、E3）把"开了脏页日志之后变慢"当成一件事。
它在源码里其实是**两件事**，而且第一件会**污染**第二件的测量：

```
脏页日志开启
  ├─ (i) 粒度代价：kvm_mmu_hugepage_adjust() 遇到
  │       if (kvm_slot_dirty_track_enabled(slot)) return;
  │       （arch/x86/kvm/mmu/mmu.c:3185）
  │       → 直接不给 req_level，缺页按 4K 建表
  │       已有的大页在**开启那一刻**就被拆：
  │         kvm_mmu_slot_apply_flags()          x86.c:13185
  │           ├─ eager_page_split=true  → kvm_mmu_slot_try_split_huge_pages()  x86.c:13251
  │           └─ 两者都走 kvm_mmu_slot_remove_write_access(..., PG_LEVEL_4K)   x86.c:13257
  │       关闭时反向回收：kvm_mmu_zap_collapsible_sptes()                      x86.c:13241
  │       这是"映射变多变碎"的代价，和 PML 硬件无关。
  │
  └─ (ii) 记录代价：每次写脏页要记进 PML 缓冲，满 512 条触发
          kvm_pml_full → 批量消费。开了 pml 与没开（写保护 + EPT
          violation 逐页记录）成本形态完全不同。
          机制见 ../annotations.md §2。
```

**所以本实验的主问题是**：在**粒度固定**的前提下，脏页记录本身值多少？
以及反过来：只改粒度、不开脏页日志，值多少？

五个可判定的子问题：

| # | 问题 | 判据（臂名见 §3） |
|---|---|---|
| Q1 | 2M vs 4K 的纯粒度差 | **H0 vs H1**：同为"无脏日志"，负载耗时差 + 缺页次数比。注意这一对**同时换了后端**，见 §3.1 |
| Q2 | 粒度固定为 4K 时，脏日志的净代价 | **H3 vs H1**：只差"开不开脏日志"，粒度两臂都是 4K |
| Q3 | PML 开/关的差 | **H4 vs H3**：同为开脏日志，只差 `pml` |
| Q4 | `eager_page_split` 的影响 | 在 {H3, H4} 上各跑 `true`/`false` |
| Q5 | 已有大页被拆的代价 | **H5 vs H2**：同为 hugetlbfs + 脏日志，只差"日志开启前是否已建满 2M" |

---

## 2. 前置检查

### 2.1 先确认这次运行真的建立了大页 —— 没有 tracepoint 会直接告诉你

**旧文档的错**：`practice/ept-bench.md:85` 曾写"`kvm:kvm_page_fault` 事件携带
`level` 相关上下文，可从输出确认实际建立的映射级别"。实际
`TRACE_EVENT(kvm_page_fault, ...)`（`arch/x86/kvm/trace.h:402-424`）只有四个字段
`vcpu_id / guest_rip / fault_address / error_code`，**没有 level，也没有 npages**。
6.12.93 的 `trace.h` 里带 level 的只有 SEV-SNP 的 `rmp_level`（`:1848` 起），
与 Intel EPT 无关。已登记为 `../corrections.md` D2。

6.12.93 上也**没有** `kvm:mmu_*` 之类的 x86 专有 tracepoint 能报出映射级别。
所以级别只能**推断**，三路交叉验证：

| 手段 | 看到什么 | 局限 |
|---|---|---|
| `kvm:kvm_page_fault` 的地址分布 | 同一 2 MiB 块只出现 1 次 → 2M；出现 512 次且 4 KiB 递增 → 4K。用 `fault_address >> 21` 去重后的块数 vs 总事件数比值判定 | 需要写入覆盖足够广；一次性建表后不再缺页，采样窗口要卡在"首次触碰"阶段 |
| `/proc/<qemu>/smaps` 的 `AnonHugePages` | 宿主侧是否真给了 THP | **只反映宿主映射，不等于 EPT 级别**；hugetlbfs 后端不进这个字段 |
| `/proc/meminfo` 的 `AnonHugePages` / `HugePages_*` | 后端类型与总量 | 同上 |

**三路不一致时以第 1 路为准**（它才是 guest 侧真正发生的退出次数）。
报告里必须写"级别是推断的"，不要写成实测。

### 2.2 大页来源必须是**宿主**侧，且两条路要分开跑

```
guest RAM 的后端
├── THP（匿名页）        /sys/kernel/mm/transparent_hugepage/enabled
│    QEMU 默认后端；能不能拿到 2M 取决于内存对齐与碎片，**不保证**
└── hugetlbfs（预留大页） -object memory-backend-file,mem-path=/dev/hugepages,...
     要先 echo N > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
     可控、可复现，★ 本实验主用这条
```
两条路的 `AnonHugePages` 观测口径不同，**不能混在同一张表里比较**。

### 2.3 `kvm:kvm_page_fault` 的 `error_code` 字段**两种位含义**，别照 #PF 读

同一个字段名，两个调用点传的东西不同：

| 调用点 | 第三个实参 | 位含义 |
|---|---|---|
| `handle_ept_violation()` → `arch/x86/kvm/vmx/vmx.c:5799` | **原始 `exit_qualification`** | EPT violation 位：bit0 R / bit1 W / bit2 X 是**访问类型**；bits5:3 是**当时 EPT 表项的 RWX** |
| `kvm_mmu_do_page_fault()` → `arch/x86/kvm/mmu/mmu.c:4655` | 已合成的 #PF 风格 `error_code` | `PFERR_*`：bit0 present、bit1 write、bit2 user |

合成发生在 `__vmx_handle_ept_violation()`（`arch/x86/kvm/vmx/common.h:9`）
—— 也就是说**打完 tracepoint 才换算**，trace 里看到的是换算**前**的值。
EPT 路径上按 #PF 位读会把"写访问"读成"用户态"，把"表项存在"读错一位。

本实验要的区分正好落在 EPT 位上（`VMX_EPT_READABLE_MASK` `0x1` /
`WRITABLE` `0x2` / `EXECUTABLE` `0x4`，`arch/x86/include/asm/vmx.h:534-536`；
`EPT_VIOLATION_RWX_SHIFT` = 3，`arch/x86/include/asm/vmx.h:586`）：

| bits5:3 | 含义 | 属于哪类代价 |
|---|---|---|
| `0b000` | 该 GPA 当时**没有映射** → 建表型缺页 | 粒度代价（Q1） |
| `0b011`(R\|W) 之外的非零值，即 **bit4=0** | 已有映射但**不可写** → 写保护型缺页 | 脏日志记录代价（Q2/Q3）、H5 的拆页后重放开 |

所以 `error_code & 0x38` 与 `(error_code & 0x38) >> 3 & 0x2` 两个掩码就把两类缺页分开了，
这是 H5−H2 唯一能直接取证的量。

### 2.4 参数权限（决定要不要重启/重载）

| 参数 | 权限 | 生效方式 |
|---|---|---|
| `eager_page_split` | 0644（`arch/x86/kvm/x86.c:194`，默认 **true** `:193`） | `echo` 即可，新 VM 生效 |
| `nx_huge_pages` | 0644（`arch/x86/kvm/mmu/mmu.c:87`，内部值默认 `-1`=auto，`:64`） | `echo` 即可 |
| `pml` | **0444** | 重载 `kvm_intel` 或内核 cmdline |
| `ept` / `vpid` / `enable_apicv` / `tdp_mmu` | **0444** | 同上 |
| `nr_hugepages` | sysfs 可写，但**要物理上凑得出来**；碎片化时会静默少于请求值 | 改完**必须回读确认** |

完整口径见 [`../parameters.md`](../parameters.md)。

---

## 3. 实验矩阵

主表（其余维度做条件展开，不做全笛卡尔积 —— 见 `../measurement.md` §6）：

| 臂 | 后端 | 脏日志 | 粒度 | PML | 回答 |
|---|---|---|---|---|---|
| H0 | hugetlbfs 2M | 关 | 实测推断 | — | Q1 基线 |
| H1 | 普通匿名页（THP=never） | 关 | 4K | — | Q1 对照 / Q2 基线 |
| H2 | hugetlbfs 2M | **开** | 会被 `mmu.c:3185` 压成 4K | 1 | 真实迁移场景 |
| H3 | 普通匿名页（THP=never） | **开** | 4K | 1 | **Q2 主判据**：与 H1 只差脏日志 |
| H4 | 普通匿名页（THP=never） | **开** | 4K | 0（重载） | Q3：与 H3 只差 PML |
| H5 | hugetlbfs 2M | 关 → **开** | 先建满 2M，再拆 | 1 | 已有大页被拆的净代价（= H5 − H2） |

**H2 与 H3 应当统计上相同**（既然脏日志一开就掉到 4K）。
如果 H2 明显慢于 H3，说明"已有的大页被拆掉"这一笔额外代价真实存在，
那本身就是一个发现 —— 但要先排除后端差异（H2 与 H3 的宿主内存不同）。
H5 就是为这笔拆页代价准备的：先把整块 guest RAM 触碰一遍建立 2M 映射
（H2 是在"还没建表"的状态下直接开日志，两臂的差别只剩"拆不拆"）。

`eager_page_split` 在 {H3,H4} 上各跑 true/false；
`nx_huge_pages` 只在 guest 有执行缺口时才动，作为独立小表。

### 3.1 为什么没有"同后端、不同粒度"的对照臂

本可用一条 **H5′ = hugetlbfs 后端 + 强制 4K** 直接钉死"粒度而非后端"这个混淆，
但 6.12.93 上**做不到**：

- `max_huge_page_level`（`arch/x86/kvm/mmu/mmu.c:115`）是 `static int ... __read_mostly`，
  由 `kvm_configure_mmu()`（`:6297`，赋值块 `:6314-6319`）按硬件能力算出，
  **没有 `module_param`**；
- 往下的真实闸门 `__kvm_mmu_max_mapping_level()`（`:3138`）只接受
  `host_pfn_mapping_level()` 给的上限 —— 宿主给 2M，KVM 就用 2M；
- 唯一能把粒度压到 4K 的常规开关就是**脏页日志本身**（`:3185`）。

所以 Q1（H0 vs H1）**确实带了后端差异**，这一点必须在报告里写明，
靠 §2.1 的**逐臂粒度推断**来支撑"差值来自粒度"，而不是靠一个假想的对照臂。
唯一同族的粒度对是 **THP=always vs THP=never**（都走匿名页），记作可选的 **H6**，
代价是 THP 给不给 2M 不确定，必须每轮回读 `AnonHugePages` 才知道臂是否成立。


---

## 4. 观测点与判据

| 量 | 取法 |
|---|---|
| 缺页次数 | `kvm:kvm_page_fault` 计数（`arch/x86/kvm/trace.h:402`）；`scripts/trace/trace-page-fault.sh` 可直接复用 |
| 缺页速率 | 同上 / 采样窗口；**理论比值 512:1 只是上界**，实测要写实际值 |
| **建表型 vs 写保护型缺页** | 同一条 `kvm:kvm_page_fault`，按 §2.3 的 EPT 位掩码分桶：`err & 0x38` 为 0 → 建表；非 0 且 `err & 0x10` 为 0 → 写保护后重放开。H5−H2 的取证就在这里 |
| 粒度 | §2.1 三路推断 |
| guest 负载耗时 | guest 内**分轮 `dd if=/dev/zero of=/e2 bs=1M count=$CNT`**，每轮后 `echo RN <i> <epoch>`；宿主按标记落盘时刻算每轮时长，取中位数 + 离散度。**★ 写 `/dev/null` 不算负载**：目标端直接丢弃，只有 `dd` 自己那块反复复用的缓冲区会被映射，**建表量与写入量无关**，测不出粒度差 |
| 脏页日志是否真开着 | **唯一硬判据：数 `mark_page_dirty_in_slot` 的命中**（`virt/kvm/kvm_main.c:3604`），它命中即证明 `kvm_slot_dirty_track_enabled()` 为真（`:3617`）。用 §4.2 的函数统计器，别用 `current_tracer=function`。辅判据：H1→H3 的**缺页次数突变**。**不要用 `kvm:kvm_dirty_ring_*`**，见 §4.1 |
| 大页是否**真的被拆了** | 数 `kvm_mmu_slot_try_split_huge_pages` 的命中（调用点 `arch/x86/kvm/x86.c:13251`）：**命中即该臂确有 2M 映射被拆**，是 H5−H2 的机制侧证据。`kvm_mmu_slot_remove_write_access`（`:13257`）每次开日志都命中，只能证明"日志开了"，不能证明拆过页 —— 两者必须分开数 |
| PML 记录量 vs 逐页写保护量 | `kvm:kvm_pml_full`（`arch/x86/kvm/trace.h:963`，参数**只有** `vcpu_id`）+ 退出号 **62**（`EXIT_REASON_PML_FULL`，`arch/x86/include/uapi/asm/vmx.h:88`）。PML 缓冲一次装 **512** 条（`PML_ENTITY_NUM`，`arch/x86/kvm/vmx/vmx.h:336`），处理函数 `handle_pml_full()`（`arch/x86/kvm/vmx/vmx.c:5962`）→ `pml_full 次数 × 512` 是"经 PML 记下的脏页条目数"的**上界** |
| EPT violation 总量 | 退出号 **48**（`arch/x86/include/uapi/asm/vmx.h:74`）。H4（`pml=0`）的每一次脏页记录都要走一次 48，这是 Q3 的主对账量 |
| 宿主 CPU% | `/proc/<qemu>/stat` utime+stime |
| ring buffer 溢出 | `per_cpu/cpu*/stats` 的 `overrun`，非零则计数作废（`../measurement.md` §4(c)） |

**判据的与关系**：
Q2 要成立必须 (i) H3 的缺页次数与 H1 同量级（粒度没变）、
(ii) H3 的耗时显著高于 H1、(iii) 高出的部分能对上多出来的退出/函数开销。
三条缺一就只能写"测到了差值但没归因"。

### 4.1 陷阱：脏页日志没有任何 tracepoint，`kvm_dirty_ring_*` 是错的尺子

6.12.93 的 `arch/x86/kvm/trace.h` 里**没有** `kvm:kvm_dirty_log` 这样的追踪点。
宿主上能 `ls` 到 `events/kvm/` 里带 dirty 的三个 —— `kvm_dirty_ring_push` /
`kvm_dirty_ring_reset` / `kvm_dirty_ring_exit` —— 定义在通用侧
`include/trace/events/kvm.h:378`、`:405`、`:426`，**与它们同名同源**，量的是
KVM **dirty ring** 那套 API。而 `mark_page_dirty_in_slot()` 里两条路是二选一：

```c
/* 来源: virt/kvm/kvm_main.c:3617-3624 */
if (memslot && kvm_slot_dirty_track_enabled(memslot)) {
	...
	if (kvm->dirty_ring_size && vcpu)
		kvm_dirty_ring_push(vcpu, slot, rel_gfn);
	else if (memslot->dirty_bitmap)
		set_bit_le(rel_gfn, memslot->dirty_bitmap);
}
```

QEMU 只有给了 `-accel kvm,dirty-ring-size=N` 才会置 `s->kvm_dirty_ring_size`
（属性注册 `accel/kvm/kvm-all.c:4041`，setter 赋值 `:3951`；读侧
`kvm_dirty_ring_enabled()` `:2494`），本仓的 `scripts/vm/boot-vm.sh` 没给
—— 所以走的是 `dirty_bitmap` 分支，**`kvm_dirty_ring_push` 一次都不会命中，
而脏页日志确实在开着**。
拿它做"日志没开"的证据，就是 `../measurement.md` §5 说的"测不到 ≠ 没有"。

正确做法是给**汇聚点本身**下 `function` tracer：`mark_page_dirty_in_slot` 是
`EXPORT_SYMBOL_GPL`（`:3627`）的全局函数、不会被内联进调用方，命中即等价于
"有一个可写 SPTE 在开着脏日志的 slot 里被建立/放开"。

**下探针前的实存性检查有个坑**：`available_filter_functions` 里**模块符号带
`[kvm]` 后缀**（实测本机是 `mark_page_dirty_in_slot [kvm]`），
用 `grep -x "mark_page_dirty_in_slot"` 匹配不到，会误判成"函数不存在/已内联"。
必须用 `grep -E "^name( |\[|$)"`（`bench-ple.sh` 的 `traceable()` 已按此实现）。

### 4.2 数次数不要开 `current_tracer=function`，会淹掉 ring buffer

一轮 64 MiB 写就是上万次 `mark_page_dirty_in_slot`，用 `function` tracer 逐条入
缓冲会立刻溢出（`overrun` 非零 → 所有计数作废）。要**次数**不要**轨迹**时，
用 ftrace 的函数统计器：

```bash
echo 0 > $TR/function_profile_enabled
echo > $TR/set_ftrace_filter
for f in mark_page_dirty_in_slot kvm_mmu_slot_try_split_huge_pages \
         kvm_mmu_slot_remove_write_access; do
    echo "$f" >> $TR/set_ftrace_filter 2>/dev/null || echo "进不去：$f"
done
echo 1 > $TR/function_profile_enabled      # current_tracer 保持 nop
# ……跑负载……
echo 0 > $TR/function_profile_enabled
cat $TR/trace_stat/function*               # 每 CPU 一张 Hit/Time/Avg 表
```

★ 名字**必须一个个写**。三个空格分隔的名字放进同一条 `echo` 是危险的：内核每次
只解析一个 token，第一个匹配不上的名字会中止这次写入，其后的名字连带丢失；
若它在最前面，filter 干脆停在 `#### all functions enabled ####`，统计器于是把整机
所有函数都算进去而 Hit 数字看着仍然正常。实测表与源码链路见
[`bench-migrate.md`](bench-migrate.md) §4.2.1，`../measurement.md` §7 有对应陷阱条目。

它注册的是**自己的** ops：6.12.93 在有 `CONFIG_FUNCTION_GRAPH_TRACER` 时走
`register_ftrace_profiler()`（`kernel/trace/ftrace.c:887`），里面先
`ftrace_ops_set_global_filter(&fprofiler_ops.ops)`（`:889`）再
`register_ftrace_graph()` —— 所以 filter 是全局的、只需配一次（不必每 CPU 各配），
而计数走 per-CPU hash（`function_profile_call()` `:784`），
**根本不经 ring buffer**。重新从 0 打开还会顺带清零：
`ftrace_profile_init_cpu()`（`:668`）里已有 hash 时直接 `ftrace_profile_reset()`（`:677`）。

本机 6.8.0-51 实测：`current_tracer` 全程 `nop`，只
`echo schedule > set_ftrace_filter` + 开关 `function_profile_enabled` 两秒，
`trace_stat/function*` 各 CPU 都记到 `schedule` 的 Hit（2~22 次不等），
而 `per_cpu/cpu0/stats` 的 `entries` 仍是 **0** —— 计数与缓冲确实分离。

**前提**：`CONFIG_FUNCTION_PROFILER=y`。宿主 6.8.0-51 有；
**guest 内核没有**（`scripts/images/kernel.config` 里
`# CONFIG_FUNCTION_PROFILER is not set`）。本实验全部在宿主侧观测，所以够用；
要在 guest 内侧量就得退回 `current_tracer=function` 并把窗口缩到几百条事件。

代价：这是**带计时的统计**（还要读 `Time`/`Avg`），单次开销比纯计数大，
所以**它自己也在扰动被测系统**。E2 里只用它做"有没有发生"的机制判据，
不要同时拿它和性能数字做换算 —— 那一档的扰动量由 E5（`bench-observer-cost.sh`）标定。

---

## 5. 执行

```bash
./bench-huge-dirty.sh --preflight
./bench-huge-dirty.sh --arm H3 --dry-run
sudo ./bench-huge-dirty.sh --all --repeat 5 --allow-reload
```

脏日志窗口靠 QEMU **QMP**（`-qmp unix:` socket）触发一次到 `exec:` 的迁移：

```jsonc
{"execute":"migrate-set-parameters","arguments":{"max-bandwidth":12500000}}
{"execute":"migrate","arguments":{"uri":"exec:cat >/dev/null"}}
// ……采样……
{"execute":"migrate_cancel"}
{"execute":"query-migrate"}          // 原文落盘，作为"日志确实开过"的旁证
```

不追求迁完，只要求脏日志窗口完整覆盖采样区间。用 QMP 而不是 HMP 的理由：
URI 走 JSON 字符串，不会被 HMP 的参数切分规则二次处理。
每臂都打印 `migrate` 起止时刻与 `query-migrate` 原文落盘位置。

---

## 6. 已知坑

1. **THP 不可复现**：`enabled=always` 不保证每次真给 2M（碎片、对齐、
   madvise 语义都影响）。所以主用 hugetlbfs；用 THP 的臂必须每轮回读
   `AnonHugePages` 并在报告里写实际拿到的量。
2. **`nr_hugepages` 写不满**：预留失败是静默的，写后必回读。
   预留不足会让 QEMU 直接起不来，或退化到别的后端 —— 两臂后端就此不同，
   整组数据作废。脚本按"请求量回读校验"处理，不满足就拒绝开跑。
3. **脏日志只覆盖部分采样窗**：迁移结束后 `KVM_MEM_LOG_DIRTY_PAGES`
   关掉，粒度又开始往大页走（还会触发 `kvm_mmu_zap_collapsible_sptes()`，
   `arch/x86/kvm/x86.c:13241`）。采样窗口必须完整落在迁移期内。
4. **precopy 会自己收敛然后关机**：guest 写脏的速度若低于带宽，迁移在几轮后
   收敛 → QEMU 完成切换并 `shutdown`，采样窗被腰斩且**不报错**。
   对策：`max-bandwidth` 压到明显低于脏页产生速率，采样结束**立刻**
   `migrate_cancel`，并把 `query-migrate` 的最终状态落盘；状态里出现
   `completed` 而不是 `cancelled` 的轮次一律标注为可疑。
5. **`exec:` 后面是脚本，不是文件名**：QEMU 把 `exec:` 之后的整串交给
   `/bin/sh -c`（`migration/migration.c:673-681`）。写成
   `migrate exec:file /tmp/x` 不会落到 `/tmp/x`，而是执行 `file /tmp/x`
   这个**命令**，打印一行就退出，迁移流拿到 EPIPE。
   另外子进程的 stdin 与 stdout 是**两根独立管道**
   （`g_spawn_async_with_pipes()`，`io/channel-command.c:86-91`；`O_RDWR`
   既不等于 `O_RDONLY` 也不等于 `O_WRONLY`，所以两根都建，
   且**不会**像 `O_WRONLY` 那样把 stdout 接到 `/dev/null`，`:83-84`）。
    outgoing 路径上 QEMU 只写不读那根 stdout，所以裸 `cat`（不带 `>/dev/null`）
   会把 stdout 管道灌满（默认 64 KiB）后**卡住**迁移 —— 数据不会串台，
   但采样窗内根本没有脏页流。目标命令必须自己把 stdout 丢掉。
6. **H2/H3 的宿主内存不同**：一个预留大页一个不预留，NUMA/碎片状态不同。
   要比较就得让两组都预留同样多的物理内存（预留但不用）。
7. **`pml=0` 需要重载 `kvm_intel`**，且它要求宿主没有别的 VM 在跑
   （脚本会拒绝）。忘了带 `--allow-reload` 会让 H4 静默地按 `pml=1` 跑完 ——
   脚本在每臂开头都回读并打印 `pml` 实际值。
8. **别用 H0 与 H2 直接相减当作"PML 的成本"** —— 那是粒度差 + 拆页差 + 记录差之和，
   正是本实验要拆开的东西（`../measurement.md` §6）。
9. **给这些 kvm 函数下探针前先确认符号可跟踪**：`mark_page_dirty_in_slot` 一类
   在 `available_filter_functions` 里带 `[kvm]` 后缀，`grep -x` 会误判成不存在（§4.1）。

---

## 7. 结果

**待实测**。

| 臂 | 重复 | 缺页次数 | 推断粒度 | 负载耗时中位 | 宿主 CPU% | `try_split` 命中 | `pml_full` 次数 | EPT violation(48) |
|---|---|---|---|---|---|---|---|---|
| H0 | | 待实测 | | | | | | |
| H1 | | 待实测 | | | | | | |
| H2 | | 待实测 | | | | | | |
| H3 | | 待实测 | | | | | | |
| H4 | | 待实测 | | | | | | |
| H5 | | 待实测 | | | | | | |
