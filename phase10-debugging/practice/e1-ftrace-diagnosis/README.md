# E1: ftrace 诊断 VM 启动失败

> 场景：VMM 调用 `KVM_RUN` 后立即返回 `KVM_EXIT_FAIL_ENTRY` 或 `KVM_EXIT_INTERNAL_ERROR`

---

## 🎯 目标

学会使用 ftrace 跟踪 KVM ioctl 链路，定位 VM 启动失败的根因。

**你将学会**：
1. 用 ftrace function tracer 跟踪 `kvm_dev_ioctl` → `kvm_vm_ioctl` → `kvm_vcpu_ioctl` 调用链
2. 用 `kvm_entry` / `kvm_exit` tracepoints 观察 VM-Entry/Exit 序列
3. 根据 `exit_reason` 和 `error_code` 判断失败类型
4. 使用 `dump_invalid_vmcs` 模块参数导出 VMCS 状态

---

## 📋 前置条件

```bash
# 检查 KVM 模块
lsmod | grep kvm
# 应看到 kvm 和 kvm_intel（或 kvm_amd）

# 检查 tracefs
ls /sys/kernel/debug/tracing/
# 如果不存在，mount -t debugfs none /sys/kernel/debug/

# 启动测试 VM（在另一个终端）
cd ../../scripts/vm/
./boot-vm.sh ubuntu --memory 2G --cpus 2
```

---

## 🔬 诊断步骤

### Step 1: 启用 ftrace function tracer

```bash
TRACEFS=/sys/kernel/debug/tracing

# 清场（★ 用 : > 显式截断，不是 echo "" >）
: > $TRACEFS/set_event
: > $TRACEFS/set_ftrace_filter
echo none > $TRACEFS/current_tracer

# 跟踪 KVM ioctl 链路
echo function > $TRACEFS/current_tracer
{
  echo kvm_dev_ioctl              # /dev/kvm 入口
  echo kvm_create_vm              # VM 创建
  echo kvm_vm_ioctl               # VM 级 ioctl（KVM_CREATE_VCPU 等）
  echo kvm_vcpu_ioctl             # vCPU 级 ioctl（KVM_RUN）
  echo kvm_arch_vcpu_ioctl_run    # vCPU 运行入口
  echo vcpu_enter_guest           # VM-Entry 准备
  echo vmx_vcpu_run               # VMX 运行循环
} > $TRACEFS/set_ftrace_filter

echo 1 > $TRACEFS/tracing_on
echo "✓ ftrace enabled, monitoring KVM ioctl chain"
```

### Step 2: 启用 KVM tracepoints

```bash
# 追加事件（★ 用 >> 不是 >，否则会清掉已有事件）
echo kvm:kvm_entry >> $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_inj_exception >> $TRACEFS/set_event
echo kvm:kvm_userspace_exit >> $TRACEFS/set_event

echo "✓ KVM tracepoints enabled"
```

### Step 3: 触发 VM 启动（或观察现有 VM）

```bash
# 如果有运行中的 VM，直接观察
QEMU_PID=$(pgrep -f '^qemu-system-x86_64' | head -1)
if [ -z "$QEMU_PID" ]; then
    echo "未检测到运行中的 VM，请先启动"
    exit 1
fi

echo "Monitoring QEMU PID: $QEMU_PID"
sleep 5  # 收集 5 秒数据
```

### Step 4: 分析 trace 输出

```bash
echo 0 > $TRACEFS/tracing_on

# 查看 ioctl 调用序列
echo "=== KVM ioctl 调用链 ==="
cat $TRACEFS/trace | grep -E 'kvm_(dev_|vm_|vcpu_)ioctl' | head -20

# 查看 VM-Entry/Exit 事件
echo ""
echo "=== VM-Entry / VM-Exit 事件 ==="
cat $TRACEFS/trace | grep -E 'kvm_(entry|exit)' | head -30

# 分析 exit_reason 分布
echo ""
echo "=== VM-Exit 原因分布 ==="
cat $TRACEFS/trace | grep 'kvm_exit' | \
    grep -oP 'reason \w+' | sort | uniq -c | sort -rn | head -10
```

### Step 5: 诊断启动失败

```bash
# 检查是否有 FAIL_ENTRY 或 INTERNAL_ERROR
echo "=== 启动失败检查 ==="
cat $TRACEFS/trace | grep 'kvm_userspace_exit' | grep -E 'FAIL_ENTRY|INTERNAL_ERROR'

# 如果有失败，启用 VMCS dump
if cat $TRACEFS/trace | grep -q 'FAIL_ENTRY'; then
    echo "检测到 FAIL_ENTRY，启用 VMCS dump..."
    echo 1 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs
    echo "✓ dump_invalid_vmcs 已启用，下次失败会打印 VMCS 到 dmesg"
    dmesg | tail -50 | grep -i vmcs
fi
```

### Step 6: 清理

```bash
# 恢复默认
echo none > $TRACEFS/current_tracer
: > $TRACEFS/set_ftrace_filter
: > $TRACEFS/set_event
echo 0 > $TRACEFS/tracing_on

# 关闭 VMCS dump
echo 0 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs

echo "✓ Cleanup done"
```

---

## 📊 预期输出

### 正常启动序列

```
=== KVM ioctl 调用链 ===
kvm_dev_ioctl: KVM_CREATE_VM
kvm_vm_ioctl: KVM_CREATE_VCPU
kvm_vcpu_ioctl: KVM_RUN
kvm_arch_vcpu_ioctl_run: enter vcpu_run
vcpu_enter_guest: prepare VM-Entry
vmx_vcpu_run: VM-Entry → Guest → VM-Exit

=== VM-Entry / VM-Exit 事件 ===
kvm_entry: vcpu 0 rip 0xfffffff0
kvm_exit: reason EXTERNAL_INTERRUPT rip 0xfffffff0
kvm_entry: vcpu 0 rip 0x1000
kvm_exit: reason EPT_VIOLATION rip 0x1000
...
```

### 启动失败序列（FAIL_ENTRY）

```
=== 启动失败检查 ===
kvm_userspace_exit: vcpu 0 exit_reason KVM_EXIT_FAIL_ENTRY

=== dmesg 输出 ===
[ 1234.567] KVM: VMCS invalid, dumping:
[ 1234.568] VM_EXIT_CONTROLS: 0x000583e6
[ 1234.569] VM_ENTRY_CONTROLS: 0x000083e6
[ 1234.570] GUEST_CR0: 0x00000010  (real mode, PE=0)
[ 1234.571] GUEST_RIP: 0x0000fff0
```

**根因分析**：
- `GUEST_CR0 = 0x10` 表示 real mode（PE=0），但 `VM_EXIT_CONTROLS` 的 `host address-space size` 位可能配置错误
- 检查 VMM 是否正确设置了 `KVM_SET_SREGS` 的 CR0/CR4

---

## 🔍 常见失败模式

| 现象 | exit_reason | 可能根因 | 排查 |
|------|-------------|---------|------|
| `KVM_EXIT_FAIL_ENTRY` | — | VMCS 配置错误 | 启用 `dump_invalid_vmcs`，检查 VMCS 字段 |
| `KVM_EXIT_INTERNAL_ERROR` | — | KVM 内部错误 | 检查 dmesg，可能是内存损坏 |
| `KVM_EXIT_SHUTDOWN` | — | triple fault | 检查异常注入链路（`kvm_inj_exception`） |
| 高频 `EPT_VIOLATION` | EPT_VIOLATION | 内存映射问题 | 检查 `KVM_SET_USER_MEMORY_REGION` 参数 |
| 高频 `EXTERNAL_INTERRUPT` | EXTERNAL_INTERRUPT | 中断风暴 | 检查 APICv 配置，考虑启用 Posted Interrupts |

---

## ⚠️ 注意事项

1. **tracefs 是全局状态**：多个工具同时使用会互相干扰
2. **`>` vs `>>`**：写 `set_event` 用 `>>`，用 `>` 会清掉所有已有事件
3. **性能影响**：function tracer 有显著开销，诊断完立即关闭
4. **dump_invalid_vmcs 是 0644**：可以运行时修改，但只在下次失败时生效

---

## 📚 参考资料

- 启动失败诊断: `../../launch-failures.md` §2
- KVM trace events: `../../annotations.md` §1
- ftrace 高级用法: `../../annotations.md` §4
- VMCS 字段含义: intel-vmx.pdf Appendix B
