# 第九阶段实践：性能基准方法

> 基于 Linux 6.12.93 内核源码

本目录收录两份可直接执行的性能基准方法文档，均改编自早期
`phase7-projects` 的观察型项目，保留其中经 6.12.93 源码核实的
tracepoint、函数与方法，**删除了所有未经实测的"典型值"数据** ——
所有数字以你自己的实测为准。

| 文档 | 内容 | 理论背景 |
|------|------|---------|
| [ept-bench.md](ept-bench.md) | EPT 缺页延迟、4K/2M 大页、脏页日志开销 | `../README.md`、`../../phase2-mem-virt/` |
| [timer-bench.md](timer-bench.md) | 时钟源读取延迟、cyclictest、TSC 稳定性、lapic_timer_advance | `../../phase7-timer-virt/README.md` |

通用要求：

- 宿主加载 `kvm`/`kvm_intel`，挂载 `/sys/kernel/debug`（debugfs）
- 实验 VM 建议用 `../../scripts/vm/boot-vm.sh`（默认
  `-enable-kvm -cpu host`，并自检确实走了 KVM）
- 每轮实验前后清理 ftrace 状态：`echo > trace`、
  `echo nop > current_tracer`、`echo > set_ftrace_filter`、
  `echo > set_event`（残留的 function tracer 会淹没下一轮事件）
