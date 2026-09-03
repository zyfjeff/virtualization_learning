# 全仓库性能结论索引

> 本仓库散落在各 phase 的性能数字，**统一登记在这里**。
> 规则：别处引用性能数字时只写指针，不复制数字 —— 数字会随重测漂移，复制一份就
> 制造一个会过期的副本。测量是否可信的判据见 [measurement.md](measurement.md)。
> 数据来源永远是各章自己的 `bench/` 目录或文档正文，本文只做**目录 + 评级**。

---

## 0. 评级定义

| 级 | 含义 | 可否作为结论引用 |
|---|---|---|
| **A** | 本仓实测，有原始数据目录、有对照组、样本量达 `measurement.md` §2 门槛 | ✅ 可 |
| **B** | 本仓实测，但样本量小 / 环境特殊 / 无严格对照组 —— 量级可信，精确值不可 | ⚠️ 只能引用量级与趋势 |
| **C** | **无本仓实测**，来自机制推算或教科书值 | ❌ 不可当结论，只能当"待验证假设" |
| **D** | 已被实测或源码核查**证伪**，本仓已修正 | ❌ 仅作反面记录 |

---

## 1. A 级 —— 可放心引用

### 1.1 启动延迟

| 项 | 内容 |
|---|---|
| 结论 | guest 就绪时间 **qemu-microvm 546.0 ms < qemu-q35 987.5 ms < minivmm 1289.0 ms**；差距的根因是**固件信息是否给全**，不是设备模型大小 |
| 数字 | N=10 取中位数，ready 范围见原表（minivmm 1286.1–1300.0） |
| 条件 | 单 vCPU；M1/M2 内存 512 MB；guest 内核 6.12.93-kvm-study（`CONFIG_HZ=250`）；宿主 96 线程裸金属 6.8.0-51；QEMU 10.1.0-rc2 |
| 出处 | `../phase8-capstone/practice/README.md` 项目4 M1（约 `:301-344`） |
| 数据 | `../phase8-capstone/practice/bench/boot-20260901-095559/boot.csv` |
| 反直觉点 | minivmm 设备面最小（无 PCI/ACPI/BIOS、首字节 32.9 ms 最快），boot 却比 q35 **慢 300 ms** |
| 阶段拆分 | minivmm 的 1289 ms 里 801.7 ms（62%）是两段 legacy 探测：8250 `autoconfig_irq()` 253.4 ms + `i8042_probe()` 直接探测 548.3 ms |
| 阴性对照 | `tuned` 档（cmdline 加 `8250.nr_uarts=1 i8042.nokbd i8042.noaux`）→ 1291.6 ms，**无效**。证明 cmdline 补不回缺失的固件信息 |
| 关联 | 与 `../phase11-microvm/README.md` 的启动路径结论一致 |

### 1.2 halt-polling 收益曲线

| 项 | 内容 |
|---|---|
| 结论 | ① 空闲负载上 polling **无效也无害**（halt ~4 ms ≫ 200 µs 上限，窗口停在 0）；② 短 halt 负载上窗口能自适应命中但**买不到延迟**：RTT 165.9 vs 165.7 µs 无差别，代价 **+16 pp CPU**（74.9% vs 58.7%）；③ 曲线在"盖住典型 halt"处**饱和**，窗口 50→200 µs 延迟与 CPU 都不变 |
| 判据 | 唤醒事件到达时刻**早于** polling 窗口起点时，polling 无法让它更早 —— 只有"唤醒源随机且大概率落在窗口内"才收益 |
| 数字 | poll-on/off × idle/flood + 固定窗口 0/50/100/200 µs 四档扫描；冷唤醒 n=20、flood n=800，取中位数 |
| 出处 | `../phase8-capstone/practice/README.md` 项目4 M3（约 `:424-478`） |
| 数据 | `bench/halt-20260901-110933/`、`bench/halt-sweep-20260901-111527/` |
| 脚本 | `bench-halt.sh`、`bench-halt-sweep.sh` |
| 注意 | 该实验用的 `halt_poll_ns` 是 6.12.93 源码默认 200000；本机 6.8 的 `halt_poll_ns_shrink` 实读为 0，与源码默认 2 不同（[parameters.md](parameters.md) §1） |

### 1.3 VM-Exit 分布与单次退出成本

| 项 | 内容 |
|---|---|
| 结论 | VMM 之间**没有数量级差距**，差距在**退出次数**上，而次数由"缺什么"决定 |
| 单次均值 | minivmm idle `IO_INSTRUCTION` **1.80 µs**；QEMU `MSR_WRITE` **1.48 µs**；`HLT` 占时 99.8%+（idle 均值 3982 µs，直通透载 653.8 µs）；用户态 BAR MMIO 的 `EPT_MISCONFIG` 均值 **11.1 µs**；`PREEMPTION_TIMER` 均值 **0.77 µs** |
| busy 15 s 计数 | minivmm `EXTERNAL_INTERRUPT` 22415 + `IO_INSTRUCTION` 15004；q35 `EXTERNAL_INTERRUPT` 15041 + `MSR_WRITE` 3753 + `PREEMPTION_TIMER` 3753 |
| boot 计数 | minivmm `IO_INSTRUCTION` 40568 + `EPT_VIOLATION` 16691；q35 `IO_INSTRUCTION` 72247 + `EPT_MISCONFIG` 5142；microvm `IO_INSTRUCTION` 35277 |
| 直通负载 | `EPT_MISCONFIG` 14541（13.9%，均值 11.1 µs）全部来自用户态 BAR MMIO（`QUEUE_NOTIFY` 写 19248、`ctrl` 转发 23） |
| 条件 | 单 vCPU；`perf kvm stat -a`（**必须 `-a`**，见 `measurement.md` §7） |
| 出处 | `../phase8-capstone/practice/README.md` 项目4 M2（约 `:346-422`）、M4（`:474-478`） |
| 数据 | `bench/exits-20260901-101641/`、`bench/exits-pt-20260901-112353/` |
| 关键推论 | 补上 in-kernel 设备模拟后这些退出**逐项消失**（M2 直通表），说明它们全部由"缺表"引起、可逐项消除 |

### 1.4 VFIO 设备接管与中断装配耗时

| 项 | 内容 |
|---|---|
| 结论 | 接管一个组全程约 **160 µs**，其中 `intel_iommu_domain_alloc` 占 **88 µs**；关 fd 归还约 **5 µs** |
| 数字 | MSI-X 装配 4 个向量约 **345 µs**；Posted 化 IRTE ②→⑦ 约 **53 µs**，④→⑦ 仅 **5 µs** |
| 方法 | 宿主 ftrace function tracer 时间戳（**未用 kprobe 取参**，规避 `AGENTS.md` 陷阱 9） |
| 条件 | 本机 Intel 平台，直通存储设备；绑定 `vfio-pci` 时**一个事件都没有**，认领发生在 `SET_CONTAINER`（`drivers/vfio/container.c:437`），因 `vfio-pci` 声明 `.driver_managed_dma = true` |
| 出处 | `../phase6-vfio/practice/README.md`（约 `:144-163`、`:342`、`:485`） |
| 评级说明 | 定为 A：有原始 ftrace 输出与时间戳，但**单次采样、无重复** → 精确值引用时降级按 B 处理 |

### 1.5 LAPIC Timer 中断投递延迟

| 项 | 内容 |
|---|---|
| 结论 | TSC-deadline 设 2 ms → 实际观察到注入完成 **2.003–2.010 ms**，即到期到投递的额外开销约 **3–10 µs** 量级 |
| 数字 | 5 轮，2.5 GHz host：2.010 / 2.003 / 2.003 / 2.003 / 2.003 ms |
| 方法 | 自建 KVM API 程序 + `rdtsc` cycles；最小实模式 guest（`sti; jmp $` + handler 用 `OUT` 退出并发 EOI） |
| 前置 | 必须先 `KVM_CREATE_IRQCHIP`（无 irqchip 则 `KVM_GET/SET_LAPIC` 返回 EINVAL，`lapic.c:2904`）；必须 `KVM_SET_CPUID2` 声明 `TSC_DEADLINE_TIMER` + X2APIC；必须切 x2APIC |
| 出处 | `../phase7-timer-virt/practice/README.md` Experiment 3（`:142-264`，数字在 `:244-262`） |
| 评级说明 | n=5 偏少，但离散度极小（±0.007 ms），量级与"额外开销个位数 µs"可信 |

---

## 2. B 级 —— 只可引用量级与趋势

### 2.1 vhost=on/off 网络吞吐

| 项 | 内容 |
|---|---|
| 结论 | TCP 单流差异不大（**vhost=on 20.0 / off 22.1 Gbps**，已饱和内存带宽）；UDP 方向相反：vhost=on 发送 2.39 Gbps（213k pps）、接收丢包 **46%**，vhost=off 发送 3.79 Gbps（338k pps）、接收丢包 **78%** → vhost 的内核态处理**更稳定**但峰值 pps 更低 |
| 条件 | **单队列**、guest 未调优、TAP 成为瓶颈；guest 内核 6.12.93-kvm-study |
| 出处 | `../phase5-virtio/practice/README.md`（约 `:55-98`），脚本 `vhost-perf-test.sh` |
| **为什么只给 B** | ① 该表记录的宿主是 **"Host: 128 CPU"**，与当前实验宿主（96 线程）**不是同一台**，环境已不可复现；② 单队列 + TAP 瓶颈下，"vhost=off 反而更快"很可能是拓扑假象而非机制结论；③ 无重复采样 |
| 复用要求 | 引用时必须带上这三条限制。要重做 → 多队列 + 直连对端 + ≥3 次重复 |

### 2.2 VM-Exit / 指令开销微基准

| 项 | 内容 |
|---|---|
| 数字 | CPUID 约 **1522 ns**、RDTSC 约 **10 ns**（`clock_gettime` 包裹的 ns-per-op 微基准） |
| 出处 | `../shared/ex5-vmexit-overhead.c`；结论记录在 `../phase1-vtx-basics/practice/SUMMARY.md` |
| 评级说明 | 真跑过，但**无对照组、单次运行、无离散度**，且把"VM-Exit 往返"与"指令本身开销"混在一个数里 → 只可引用量级 |

---

## 3. C 级 —— 无实测，仅机制推算（禁止当结论引用）

这一节的每一条都是"看起来像数据但没有数据"。列出来是为了**标明缺口**，
每一条都对应 `practice/` 里一个待跑实验。

| 主题 | 流传的说法 | 现状 | 待验证于 |
|---|---|---|---|
| Posted Interrupts 收益 | "延迟 <1 µs、吞吐提升 10–100 倍" | `../phase4-interrupts/posted-interrupts.md` 有机制详解（机制部分有规范支撑，见下 D 节勘误），但**收益倍数无任何实测** | `../phase4-interrupts/practice/ex4-pi-vs-remapped.sh` **已有脚本但结果未回写** → 优先补这个，不用新写 |
| EPT 缺页快慢路径 | "快速路径 ~200 ns / 慢速 2–5 µs / RCU ~50 ns" | 反复出现在 `../phase2-mem-virt/tdp-mmu-concurrency.md` 与 `archive/` 多处，**互相复制、无出处** | `practice/bench-huge-dirty.md` + `bench-observer-cost.md` |
| A/D 位与写保护 | "A/D 硬件置位 ~1 ns vs 写保护 ~200 ns" | `../phase2-mem-virt/ept-violation-handling.md`，推算值 | 同上 |
| 内存类型延迟 | "WB / WT / UC 延迟表" | `../phase2-mem-virt/mmio-identification.md`，无实测 | — |
| 各时钟源读取成本 | "PIT 1000 ns / HPET 500 / APIC 300 / TSC 20 / kvmclock 30" | `../phase7-timer-virt/README.md:1076-1114`，**无实测** | 移交 `../phase7-timer-virt/practice/`（时钟基准归 phase7） |
| IOMMU strict/lazy 延迟 | 用 `dmar_perf_latency` 直方图量化 | `../phase3-iommu/practice/README.md` I.15：**方法已核实但无数据** | phase3 自己补 |
| `ptp_kvm` 对时精度 | "~1 µs" | `../phase7-timer-virt/README.md:1369`，无实测 | phase7 |

**PI 的机制结论例外**：`AGENTS.md` 核心原则已用规范支撑过 —— Intel SDM
Section 30.6 与 VT-d Spec 5.2.5 明确"process posted interrupts"=1 且向量匹配时，
硬件以不可中断方式完成清 ON → PIR→VIRR → 更新 RVI → 评估中断，
**"without transferring control to the VMM"**，即正常路径零 VM-Exit。
这条是**规范事实**不是推算，可直接引用；**不可引用的是"提升 N 倍"这类量化**。

---

## 4. D 级 —— 已证伪，保留作反面记录

| 曾经的"数据" | 错在哪 | 修正 |
|---|---|---|
| `halt_poll_ns` 默认 400000 ns = 400 µs | 源码 `KVM_HALT_POLL_NS_DEFAULT = 200000`（`arch/x86/include/asm/kvm_host.h:71`），本机实读 200000 | 全仓 10 处已改，见 `corrections.md` |
| 网络密集 500K → 50K VM-Exit/s（减少 90%）等五档"优化前后对比表" | **完全无出处**，本仓没有任何一次测到过 10⁵ 量级的退出率（实测最大单窗口是 boot 期 7.2 万次，见 §1.3） | 已从 `annotations.md` 删除 |
| halt-polling 调优"效果 -50% / +200 µs / CPU +10-20%" | 无出处。实测结论形态完全不同：**空闲场景零收益、flood 场景零延迟收益换 +16 pp CPU**（§1.2） | 已删除，改为指向 §1.2 |
| "无 A/D 每次写退出 ~500 ns / 有 A/D ~0 ns"；"PML 后正常写入 ~0 ns、无 PML 每次写 ~500 ns" | 方向都讲错了。有 PML 时正常写入**不退出**是硬件行为，但把开销写成"~500 ns/次"是凭空给的 | 已删除，机制重讲见 [annotations.md](annotations.md) §2 |
| `cat /sys/module/kvm_intel/parameters/halt_poll_ns` | `halt_poll_ns` 属 **kvm** 模块，该路径不存在（本机实测） | 已改 |
| `ple_window_shrink` 默认 2、`ple_window_max` 默认 16384 | 实为 **0** 与 **UINT_MAX**（本机实读 `4294967295`） | 已改，见 [parameters.md](parameters.md) §2 |
| `cat /sys/module/kvm/parameters/lapic_timer_advance_ns` | 6.12.93 无此模块参数（只有 `lapic_timer_advance` bool + per-vCPU debugfs 文件）；phase7 早已写明 | 已删，见 [parameters.md](parameters.md) §3 |
| "TSC-deadline 到期由硬件比较、零 VM-Exit" | vLAPIC 的 deadline 是**宿主 hrtimer**（`arch/x86/kvm/lapic.c`），到期要 VM-Exit 注入；phase7 Experiment 3 实测的就是这条退出 | 已在 [annotations.md](annotations.md) §3 更正 |
| "EPT A/D 位支持是大页启用条件之一" | 真实闸门是 `disallow_lpage` 计数 + `host_pfn_mapping_level`（`arch/x86/kvm/mmu/mmu.c:3138`），与 `eptad` 无关 | 已改 |
| "开脏页日志的开销 = 每次写入触发 EPT Violation" | 混淆了两件事：脏跟踪本身还会**让新建映射退到 4K**（`mmu.c:3185-3186`），且 PML 开启时正常写入不退出 | 拆开归因见 `practice/bench-huge-dirty.md` |

---

## 5. 本仓空白（关心但完全没数据）

phase9 重写后识别出的真实缺口，逐条对应一个待跑实验：

| 缺口 | 为什么是空白 | 实验 |
|---|---|---|
| **PLE / directed yield 的实际收益** | 全仓零数据。phase8 已定性说明"单 vCPU、无超卖自旋测不出"，并把超卖实验推给 phase9 | `practice/bench-ple.md`（E1） |
| **大页 vs 4K 的可测差异** | phase2 只有机制与推算值；phase8 说"本规模无独立可测收益"但那是单 vCPU 512 MB 场景 | `practice/bench-huge-dirty.md`（E2） |
| **脏页日志 / 热迁移开销** | 只有机制文档，从无 A/B 数据 | 同上（E2） |
| **vCPU 迁移的代价** | `../phase4-interrupts/practice/ex6-vcpu-migration.sh` 只观测未落结论 | `practice/bench-migrate.md`（E3） |
| **主时钟启停的实际触发条件** | 旧文档写的是 CPUID invariant 位，与源码判据不符（[annotations.md](annotations.md) §3） | `practice/bench-clock-master.md`（E4） |
| **各观测手段自身的扰动量** | `measurement.md` §4(b) 的扰动预算表**整张待填** | `practice/bench-observer-cost.md`（E5） |

---

## 6. 引用规则

1. 其它章节要引性能数字 → 写 `[<数字> 的实测见 index.md §1.x](../phase9-performance/index.md)`
   或直接指向该结论**所属 phase 的原文与 `bench/` 目录**。不要复制数字。
2. 新实测落地后 → 在本文加/改条目，**必须**同时填"条件"和"数据目录"两栏，
   缺任一栏不得进 A 级。
3. 环境变了（换宿主、换内核版本）→ 该条目降一级并在"评级说明"写明，
   不要保留过期精度。§2.1 的 phase5 数据就是现例。
4. **"开 trace 的代价可以忽略"这句在本文档是禁止说的**，直到 E5 把
   `measurement.md` §4(b) 那张表填上（§5 现列其为空白）。同理不得引用别人机器上的
   "perf 开销约 X%"。判"有没有人在 trace"也不能只看 `tracing_on` / `set_event` ——
   见 `corrections.md` C17（`tracing_on=0` 不关掉任何开销）与 D6（tracefs 里
   不存在 `enabled_events`，bpf/perf 侧走的是同一个 tracepoint 的另一个 probe）。
