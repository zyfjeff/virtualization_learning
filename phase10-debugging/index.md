# Phase 10 KVM 调试与测试 — 主题索引

> 快速定位：你想调试什么？从这里找到对应文档。

---

## 📂 文件导航

| 文件 | 内容 | 何时阅读 |
|------|------|---------|
| [`README.md`](README.md) | 场景驱动导航 + 快速参考 | 入口，了解整体结构 |
| [`annotations.md`](annotations.md) | KVM trace events 完整目录 + bpftrace 脚本集 | 需要具体 tracepoint 参数或 bpftrace 脚本时 |
| [`corrections.md`](corrections.md) | 11 条常见错误（模块参数、trace 字段、tracefs 用法） | 写脚本或读文档前先扫一遍 |
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
| `KVM_CREATE_VCPU` 返回 EINVAL | `launch-failures.md` §1 | — |
| `KVM_RUN` 返回 `KVM_EXIT_FAIL_ENTRY` | `launch-failures.md` §2 | `kvm:kvm_exit` |
| `KVM_RUN` 返回 `KVM_EXIT_INTERNAL_ERROR` | `launch-failures.md` §2 | `kvm:kvm_exit` |
| `KVM_RUN` 返回 `KVM_EXIT_SHUTDOWN` | `launch-failures.md` §3 | `kvm:kvm_inj_exception` |
| VMCS 配置错误 | `launch-failures.md` §4 | `dump_invalid_vmcs` |

### vCPU 异常退出

| 问题 | 文档 | 关键 tracepoint |
|------|------|----------------|
| vCPU stopped / KVM internal error | `vcpu-exit-diagnosis.md` §2 | `kvm:kvm_exit` |
| Triple fault (KVM_EXIT_SHUTDOWN) | `vcpu-exit-diagnosis.md` §3 | `kvm:kvm_inj_exception` |
| 高频 EPT_VIOLATION | `vcpu-exit-diagnosis.md` §4 | `kvm:kvm_page_fault` |
| 高频 EXTERNAL_INTERRUPT | `vcpu-exit-diagnosis.md` §5 | `kvm:kvm_exit` |
| 异常注入失败 | `vcpu-exit-diagnosis.md` §6 | `kvm:kvm_inj_exception` |

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

| 用途 | 文档 | 章节 |
|------|------|------|
| 跟踪 KVM ioctl 链路 | `launch-failures.md` | §2 |
| VM 生命周期跟踪 | `annotations.md` | §4.6 |
| function / function_graph | `annotations.md` | §4.1-4.2 |
| PID 过滤 | `annotations.md` | §4.4 |

### bpftrace

| 脚本 | 文档 | 用途 |
|------|------|------|
| VM-Exit 延迟分析 | `annotations.md` §6.1 | `vmx_handle_exit` kprobe/kretprobe |
| EPT 缺页热点 | `annotations.md` §6.2 | `kvm_page_fault` 按 GPA 2MB 聚合 |
| halt-polling 监控 | `annotations.md` §6.3 | `kvm_halt_poll_ns` tracepoint |
| vCPU 调度分析 | `annotations.md` §6.4 | `sched_switch` + `kvm_entry/exit` |
| MSR 热点统计 | `annotations.md` §6.6 | `kvm_msr` 按 MSR 号聚合 |
| 嵌套虚拟化分析 | `annotations.md` §6.7 | `kvm_nested_vmenter/vmexit` |
| MMU notifier 跟踪 | `annotations.md` §6.8 | `kvm_unmap_hva_range / kvm_age_hva` |
| 中断注入延迟 | `annotations.md` §6.9 | `kvm_inj_virq` → `kvm_apic_accept_irq` |
| vCPU 唤醒原因 | `annotations.md` §6.10 | `kvm_vcpu_wakeup` |
| 内存带宽监控 | `annotations.md` §6.11 | `kvm_page_fault` + `kvm_mmio` |
| 中断风暴检测 | `annotations.md` §6.12 | `kvm_exit` EXTERNAL_INTERRUPT 计数 |

### perf

| 命令 | 文档 | 用途 |
|------|------|------|
| `perf kvm stat record -a` | `annotations.md` §2 | VM-Exit 分布统计 |
| `perf kvm stat report` | `annotations.md` §2 | 查看退出原因分布 |
| `perf record -g` | `performance-analysis.md` §3 | 函数级火焰图 |

### debugfs / sysfs

| 接口 | 文档 | 用途 |
|------|------|------|
| `/sys/module/kvm*/parameters/` | `annotations.md` §3.1 | 模块参数（存在吗？能改吗？） |
| `/sys/kernel/debug/kvm/<pid>-<vmid>/` | `annotations.md` §3.2 | VM 运行时状态 |
| `stats` (二进制) | `annotations.md` §3.3 | VM/vCPU 统计 |
| `dump_invalid_vmcs` | `launch-failures.md` §4 | VMCS 状态导出 |

---

## 📖 推荐阅读顺序

### 快速入门（30 分钟）
1. 读 `README.md` 了解整体结构
2. 扫 `corrections.md` 避免常见错误
3. 做 `practice/e1-ftrace-diagnosis` 练习 ftrace 诊断

### 深入调试（2 小时）
4. 读 `launch-failures.md` 掌握启动失败诊断
5. 读 `vcpu-exit-diagnosis.md` 掌握退出分析
6. 做 `practice/e3-vcpu-exit-analysis` 练习退出诊断

### 性能分析（2 小时）
7. 读 `performance-analysis.md` 掌握性能瓶颈定位
8. 做 `practice/e2-perf-bottleneck` 练习 perf + bpftrace
9. 读 `annotations.md` §6 的 bpftrace 脚本集

### 全面掌握（4 小时+）
10. 读 `annotations.md` 完整 trace events 目录
11. 读 `case-studies.md` 端到端案例
12. 回顾 `corrections.md` 确保不重犯

---

## 🔗 跨 Phase 引用

| Phase | 关联内容 | phase10 中的位置 |
|-------|---------|-----------------|
| phase0 KVM 框架 | KVM 模块初始化 | `launch-failures.md` §1 |
| phase1 VT-x | VMCS 字段、VM-Entry/Exit | `launch-failures.md` §4 |
| phase2 内存虚拟化 | EPT、缺页处理 | `performance-analysis.md` §4 |
| phase3 IOMMU | 设备直通相关崩溃 | `case-studies.md` 案例 3 |
| phase4 中断虚拟化 | APICv、Posted Interrupts | `performance-analysis.md` §5 |
| phase5 virtio | vhost 优化 | `performance-analysis.md` §2 |
| phase7 时钟虚拟化 | TSC、kvmclock | `performance-analysis.md` §7 |
| phase8 最小 VMM | KVM API 调用 | `launch-failures.md` §2 |
| phase9 性能测量 | 测量纪律、观测者成本 | `performance-analysis.md` §1 |
