# Phase 10 KVM 调试与测试 — 场景实验

> 基于 Linux 6.12.93 源码 | 运行时诊断实战

---

## 📋 实验列表

| 实验 | 场景 | 工具 | 难度 | 预计时间 |
|------|------|------|------|---------|
| `e1-ftrace-diagnosis` | VM 启动失败诊断 | ftrace + debugfs | ★★☆ | 30min |
| `e2-perf-bottleneck` | 运行时性能瓶颈定位 | perf kvm stat + bpftrace | ★★★ | 35min |
| `e3-vcpu-exit-analysis` | vCPU 异常退出分析 | ftrace + bpftrace + debugfs | ★★★ | 40min |

---

## 🔧 运行方式

每个实验是一个独立目录，包含：
- `README.md` — 场景描述、诊断步骤、预期输出、常见问题
- `run.sh` — 自动化诊断脚本（支持 `--preflight` 检查环境 + `--dry-run` 模拟）

```bash
# 进入实验目录
cd e1-ftrace-diagnosis/

# 1. 检查环境（不执行诊断）
sudo ./run.sh --preflight

# 2. 模拟诊断流程（不实际运行 VM）
sudo ./run.sh --dry-run

# 3. 执行完整诊断
sudo ./run.sh
```

---

## 📖 实验详解

### E1: ftrace 诊断 VM 启动失败

**场景**：VMM 调用 `KVM_RUN` 后立即返回 `KVM_EXIT_FAIL_ENTRY` 或 `KVM_EXIT_INTERNAL_ERROR`，需要定位根因。

**诊断思路**：
1. 用 ftrace 跟踪 `kvm_dev_ioctl` → `kvm_vm_ioctl` → `kvm_vcpu_ioctl` 链路
2. 观察 `kvm_entry` / `kvm_exit` 事件序列
3. 检查 VMCS 状态（`dump_invalid_vmcs` 模块参数）
4. 分析 `kvm_exit` 的 `exit_reason` 和 `error_code`

**关键 tracepoints**：
```
kvm:kvm_entry          — VM-Entry 尝试
kvm:kvm_exit           — VM-Exit 原因（exit_reason, error_code）
kvm:kvm_inj_exception  — 异常注入
```

**预期诊断路径**：
```
KVM_RUN 失败
    ├─ exit_reason = FAIL_ENTRY → 检查 VMCS 配置（real mode / protected mode）
    ├─ exit_reason = INTERNAL_ERROR → 检查 KVM 内部错误（dump_invalid_vmcs）
    └─ exit_reason = SHUTDOWN → 检查 triple fault（异常注入失败）
```

**对应文档**：`../launch-failures.md` §2

---

### E2: perf + bpftrace 定位性能瓶颈

**场景**：VM 运行时 CPU 占用高、响应慢，需要定位是 VM-Exit 过多、中断延迟、还是内存问题。

**诊断思路**：
1. `perf kvm stat report` 分析 VM-Exit 分布
2. `bpftrace` 测量 VM-Exit 处理延迟
3. `kvm_page_fault` tracepoint 分析 EPT 缺页热点
4. `kvm_halt_poll_ns` 跟踪 halt-polling 窗口自适应

**关键工具**：
```bash
# VM-Exit 分布
sudo perf kvm stat record -a -- sleep 10
sudo perf kvm stat report

# VM-Exit 延迟直方图
sudo bpftrace -e '
kprobe:vmx_handle_exit { @start[tid] = nsecs; }
kretprobe:vmx_handle_exit {
    @latency_us = hist((nsecs - @start[tid]) / 1000);
    delete(@start[tid]);
}
interval:s:5 { print(@latency_us); clear(@latency_us); }
'

# EPT 缺页热点（按 2MB 对齐）
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @gpa[args->fault_address >> 21] = count();
}
interval:s:10 { print(@gpa, 20); clear(@gpa); }
'
```

**预期诊断路径**：
```
性能差
    ├─ VM-Exit 过多 → perf kvm stat 看分布 → 针对性优化（大页 / APICv / vhost）
    ├─ Exit 延迟高 → bpftrace 测延迟 → 检查 halt-polling / 调度抖动
    └─ 内存慢 → kvm_page_fault 热点 → 检查大页配置
```

**对应文档**：`../performance-analysis.md` §3

---

### E3: vCPU 异常退出分析

**场景**：VM 运行中突然停止，QEMU 报错 "KVM internal error" 或 "vCPU stopped"，需要定位退出原因。

**诊断思路**：
1. 检查 QEMU 日志（`-D /tmp/qemu.log`）中的 `KVM_EXIT_*` 原因
2. 用 `kvm_exit` tracepoint 分析退出模式（频率、原因分布）
3. 检查 `kvm_inj_exception` 是否注入异常
4. 分析 `kvm_userspace_exit` 确认退出到用户态的原因

**关键 tracepoints**：
```
kvm:kvm_exit              — 退出原因（exit_reason）
kvm:kvm_inj_exception     — 异常注入（exception, error_code）
kvm:kvm_userspace_exit    — 退到用户态（exit_reason）
kvm:kvm_inj_virq          — 中断注入
```

**预期诊断路径**：
```
vCPU 停止
    ├─ KVM_EXIT_INTERNAL_ERROR → 检查 VMCS 状态（dump_invalid_vmcs）
    ├─ KVM_EXIT_SHUTDOWN → triple fault → 检查异常注入链路
    ├─ 高频 EPT_VIOLATION → 内存配置问题 → 检查 mmu_notifier
    └─ 高频 EXTERNAL_INTERRUPT → 中断风暴 → 检查 APICv / Posted Interrupts
```

**对应文档**：`../vcpu-exit-diagnosis.md` §2

---

## ⚠️ 注意事项

1. **需要 root 权限**：所有实验需要 `sudo` 运行
2. **需要 KVM 模块**：确保 `kvm` 和 `kvm_intel` 已加载
3. **tracefs 是全局状态**：实验结束后脚本会自动清理，但中途 Ctrl+C 需要手动清
4. **实验 VM**：建议用 `scripts/vm/boot-vm.sh` 启动测试 VM

```bash
# 检查 KVM 模块
lsmod | grep kvm

# 启动测试 VM
cd ../../scripts/vm/
./boot-vm.sh ubuntu --memory 2G --cpus 2
```

---

## 📚 参考资料

- Phase 10 主文档: `../README.md`
- 源码注释: `../annotations.md`
- 勘误: `../corrections.md`
- 启动失败诊断: `../launch-failures.md`
- vCPU 退出诊断: `../vcpu-exit-diagnosis.md`
- 性能分析: `../performance-analysis.md`
- 实战案例: `../case-studies.md`
