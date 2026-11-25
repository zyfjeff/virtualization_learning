# Phase 11 KVM 调试与诊断 — 主题索引

> 快速定位：你想调试什么？从这里找到对应文档。

---

## 📂 文件导航

| 文件 | 内容 | 何时阅读 |
|------|------|---------|
| [`README.md`](README.md) | 场景驱动导航 + 快速参考 | 入口，了解整体结构 |
| [`annotations.md`](annotations.md) | KVM trace events 完整目录 + bpftrace 脚本集 | 需要具体 tracepoint 参数或 bpftrace 脚本时 |
| [`corrections.md`](corrections.md) | 26 条常见错误（模块参数、trace 字段、tracefs 用法、QEMU 用法） | 写脚本或读文档前先扫一遍 |
| [`launch-failures.md`](launch-failures.md) | VM 启动失败诊断（KVM_CREATE_VM/VCPU/RUN 失败） | VM 启不起来、KVM_RUN 立即返回 |
| [`vcpu-exit-diagnosis.md`](vcpu-exit-diagnosis.md) | vCPU 异常退出诊断（VM-Exit 原因分析、triple fault） | VM 运行中崩溃、vCPU stopped |
| [`performance-analysis.md`](performance-analysis.md) | 运行时性能瓶颈定位（VM-Exit 分布、延迟、内存、调度） | VM 性能不及预期 |
| [`case-studies.md`](case-studies.md) | 3 个端到端诊断案例 | 想看完整诊断流程示例 |
| [`practice/`](practice/README.md) | 3 个场景驱动实验 | 动手练习 |

---

## 🔍 按问题类型索引

### VM 启动失败

| 问题 | 文档 | 关键 tracepoint |
|------|------|----------------|
| `KVM_CREATE_VM` 返回 ENODEV | `launch-failures.md` §1 | — |
| `KVM_CREATE_VCPU` 返回 EINVAL | `launch-failures.md` §2 | — |
| `KVM_RUN` 返回 `KVM_EXIT_FAIL_ENTRY` | `launch-failures.md` §3.1 | `kvm:kvm_exit` |
| `KVM_RUN` 返回 `KVM_EXIT_INTERNAL_ERROR` | `launch-failures.md` §3.2 | `kvm:kvm_exit` |
| `KVM_RUN` 返回 `KVM_EXIT_SHUTDOWN` | `launch-failures.md` §3.3 | `kvm:kvm_inj_exception` |
| VMCS 配置错误 | `launch-failures.md` §4 | `dump_invalid_vmcs` |

### vCPU 异常退出

| 问题 | 文档 | 关键 tracepoint |
|------|------|----------------|
| vCPU stopped / KVM internal error | `vcpu-exit-diagnosis.md` §2 | `kvm:kvm_exit` |
| Triple fault (KVM_EXIT_SHUTDOWN) | `vcpu-exit-diagnosis.md` §3 | `kvm:kvm_inj_exception` |
| 高频 EPT_VIOLATION | `vcpu-exit-diagnosis.md` §4.1 | `kvm:kvm_page_fault` |
| 高频 EXTERNAL_INTERRUPT | `vcpu-exit-diagnosis.md` §4.2 | `kvm:kvm_exit` |
| 异常注入链路 | `vcpu-exit-diagnosis.md` §5 | `kvm:kvm_inj_exception` |
| 诊断工具速查 | `vcpu-exit-diagnosis.md` §6 | — |

### 性能问题

| 问题 | 文档 | 关键工具 |
|------|------|---------|
| VM-Exit 过多 | `performance-analysis.md` §2 | `perf kvm stat` |
| VM-Exit 处理延迟高 | `performance-analysis.md` §3 | bpftrace `vmx_handle_exit` |
| 内存性能差（EPT 缺页多） | `performance-analysis.md` §4 | bpftrace `kvm_page_fault` |
| vCPU 调度抖动 | `performance-analysis.md` §5 | bpftrace `sched_switch` |
| halt-polling 低效 | `performance-analysis.md` §6 | `kvm:kvm_halt_poll_ns` |
| TSC 不同步 / 时钟跳变 | `performance-analysis.md` §7 | `kvm:kvm_track_tsc` |

---

## 🛠️ 按工具索引

### ftrace

> ★ 本节指针按 `annotations.md` 的**实际章节**改写（§1 入口退出 / §2 中断 /
> §3 内存 / §4 时钟 / §5 性能调优 / §6 tracefs 全局状态 / §7 CPU 操作 /
> §8 perf kvm stat / §9 debugfs / §10 bpftrace 脚本集 / §11 嵌套 / §12 调用链）。
> 旧版指向的 §4.x（ftrace 工具专章）与 §6.x（bpftrace 编号小节）在文中并不存在。

| 用途 | 文档 | 章节 |
|------|------|------|
| 跟踪 KVM ioctl 链路 | `launch-failures.md` | §5.1 |
| function tracer 输出格式 | `case-studies.md` | 案例 1 Step 2 |
| tracefs 全局状态 / `set_event_pid` 过滤 | `annotations.md` | §6 |
| 事件字段与 print fmt | `annotations.md` | §1-§5、§7 |

### bpftrace

| 脚本 | 文档 | 用途 |
|------|------|------|
| VM-Exit 延迟分析 | `annotations.md` §10 | `vmx_handle_exit` kprobe/kretprobe |
| EPT 缺页热点 | `annotations.md` §10 | `kvm_page_fault` 按 GPA 2MB 聚合 |
| 按退出原因统计 | `annotations.md` §10 | `args->exit_reason`（数字） |
| vCPU 调度分析 | `annotations.md` §10 | `sched_switch` + `kvm_entry/exit` |
| 中断风暴检测 | `annotations.md` §10 | `kvm_exit` EXTERNAL_INTERRUPT 计数 |
| halt-polling 监控 | `performance-analysis.md` §6.1 | `kvm_halt_poll_ns` tracepoint |
| MSR / PIO / MMIO 热点 | `performance-analysis.md` §2.4、§4.1 | `kvm_msr` / `kvm_pio` / `kvm_page_fault` |
| MMU notifier 跟踪 | `performance-analysis.md` §4.4 | `kvm_unmap_hva_range / kvm_age_hva` |
| 真实映射级别 | `performance-analysis.md` §4.2 | `kvmmmu:kvm_mmu_set_spte` 的 `level` |
| 中断注入延迟 | `performance-analysis.md` §5.2 | `kvm_apic_accept_irq` → `kvm_inj_virq` |

### perf

| 命令 | 文档 | 用途 |
|------|------|------|
| `perf kvm stat record -a` | `annotations.md` §8 | VM-Exit 分布统计 |
| `perf kvm stat report --key=time` | `annotations.md` §8 | 查看退出原因分布/排序（无 `--sort`）|
| `perf record -g -a -e kvm:kvm_exit` | `performance-analysis.md` §3.3 | 函数级火焰图（`perf kvm stat record` 禁用了 `-g`）|

### debugfs / sysfs

| 接口 | 文档 | 用途 |
|------|------|------|
| `/sys/module/kvm*/parameters/` | `../phase10-performance/parameters.md`（默认值唯一来源）；形态见 `annotations.md` §9 | 模块参数（存在吗？能改吗？）|
| `/sys/kernel/debug/kvm/<pid>-<vm fd>/` | `annotations.md` §9 | VM 运行时状态（第二段是 VM fd 号，不是 vmid）|
| 逐项统计文件（`exits`、`halt_successful_poll`…） | `annotations.md` §9 | VM/vCPU 统计。★ 6.12.93 的 debugfs 里**没有**名为 `stats` 的文件；二进制 blob 走 `KVM_GET_STATS_FD` 的 anon fd（`kvm-vm-stats`）|
| `dump_invalid_vmcs` | `launch-failures.md` §4 | VMCS 状态导出 |

---

## 📖 推荐阅读顺序

### 快速入门（30 分钟）
1. 读 `README.md` 了解整体结构
2. 扫 `corrections.md` 避免常见错误
3. 做 `practice/ex1-ftrace-diagnosis` 练习 ftrace 诊断

### 深入调试（2 小时）
4. 读 `launch-failures.md` 掌握启动失败诊断
5. 读 `vcpu-exit-diagnosis.md` 掌握退出分析
6. 做 `practice/ex3-vcpu-exit-analysis` 练习退出诊断

### 性能分析（2 小时）
7. 读 `performance-analysis.md` 掌握性能瓶颈定位
8. 做 `practice/ex2-perf-bottleneck` 练习 perf + bpftrace
9. 读 `annotations.md` §6 的 bpftrace 脚本集

### 全面掌握（4 小时+）
10. 读 `annotations.md` 完整 trace events 目录
11. 读 `case-studies.md` 端到端案例
12. 回顾 `corrections.md` 确保不重犯

---

## 🔗 跨 Phase 引用

| Phase | 关联内容 | 本章（phase11）中的位置 |
|-------|---------|-----------------|
| phase0 KVM 框架 | KVM 模块初始化 | `launch-failures.md` §1 |
| phase1 VT-x | VMCS 字段、VM-Entry/Exit | `launch-failures.md` §4 |
| phase2 内存虚拟化 | EPT、缺页处理 | `performance-analysis.md` §4 |
| phase3 IOMMU | 设备直通相关崩溃 | `case-studies.md` 案例 3 |
| phase5 中断虚拟化 | APICv、Posted Interrupts | `performance-analysis.md` §5 |
| phase6 virtio | vhost 优化 | `performance-analysis.md` §2 |
| phase8 时钟虚拟化 | TSC、kvmclock | `performance-analysis.md` §7 |
| phase9 最小 VMM | KVM API 调用 | `launch-failures.md` §2 |
| phase10 性能测量 | 测量纪律、观测者成本 | `performance-analysis.md` §1 |
