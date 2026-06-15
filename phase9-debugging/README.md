# Phase 9：KVM 调试与测试

> 基于 Linux 6.12.93 源码 | 面向 MicroVM 开发者的实战指南

---

## 📚 阶段概述

本阶段提供 KVM 调试和测试的完整参考，包括：

1. **完整 KVM trace events 目录** — 按分类列出所有可用事件及参数
2. **perf kvm stat** — VM-Exit 统计分析
3. **ftrace 高级用法** — 函数跟踪、过滤、组合
4. **debugfs 接口** — KVM 运行时状态查看
5. **KVM selftests** — 内核自带测试框架
6. **bpftrace 脚本集** — 高级性能分析

---

## 🎯 快速导航

| 问题 | 章节 | 工具 |
|------|------|------|
| VM-Exit 太多，原因不明？ | 1 + 2 | trace events + perf kvm stat |
| 中断延迟高？ | 1 + 6 | kvm_entry/kvm_exit + bpftrace |
| 内存性能差？ | 1 + 3 | kvm_page_fault + ftrace function_graph |
| vCPU 调度问题？ | 1 | kvm_vcpu_wakeup + sched:sched_switch |
| TSC 不同步？ | 1 | kvm_track_tsc + kvm_write_tsc_offset |
| 新功能测试？ | 5 | selftests |
| 热路径分析？ | 6 | bpftrace |
