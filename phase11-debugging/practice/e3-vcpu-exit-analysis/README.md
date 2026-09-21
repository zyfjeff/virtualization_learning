# E3: vCPU 异常退出分析

> 场景：VM 运行中突然停止，QEMU 报错 "KVM internal error" 或 "vCPU stopped"

---

## 🎯 目标

学会分析 vCPU 异常退出的原因，从 trace 数据定位根因。

**你将学会**：
1. 检查 QEMU 日志中的 `KVM_EXIT_*` 原因
2. 用 `kvm_exit` tracepoint 分析退出模式
3. 用 `kvm_inj_exception` 检查异常注入
4. 用 `kvm_userspace_exit` 确认退出到用户态的原因

---

## 📋 前置条件

```bash
# 启动 VM 时启用日志
qemu-system-x86_64 ... -D /tmp/qemu.log -d kvm

# 或已有 VM，检查日志位置
ls -l /tmp/qemu.log
```

---

## 🔬 诊断步骤

### Step 1: 检查 QEMU 日志

```bash
echo "=== QEMU 日志最后 50 行 ==="
tail -50 /tmp/qemu.log

# 查找 KVM_EXIT_* 原因
echo ""
echo "=== KVM_EXIT 原因 ==="
grep -E 'KVM_EXIT_|kvm_exit' /tmp/qemu.log | tail -20

# 查找内部错误
echo ""
echo "=== 内部错误 ==="
grep -i 'internal error\|kvm internal' /tmp/qemu.log
```

**常见 KVM_EXIT 原因**：
- `KVM_EXIT_IO` — 正常 I/O 退出（PIO）
- `KVM_EXIT_MMIO` — 正常 MMIO 退出
- `KVM_EXIT_FAIL_ENTRY` — VM-Entry 失败
- `KVM_EXIT_INTERNAL_ERROR` — KVM 内部错误
- `KVM_EXIT_SHUTDOWN` — triple fault（guest 崩溃）

### Step 2: ftrace 跟踪退出模式

```bash
TRACEFS=/sys/kernel/debug/tracing

# 清场
: > "$TRACEFS/set_event"
echo 1 > "$TRACEFS/tracing_on"

# 启用退出相关事件
echo kvm:kvm_exit >> "$TRACEFS/set_event"
echo kvm:kvm_inj_exception >> "$TRACEFS/set_event"
echo kvm:kvm_inj_virq >> "$TRACEFS/set_event"
echo kvm:kvm_userspace_exit >> "$TRACEFS/set_event"

echo "收集数据（10 秒）..."
sleep 10

echo 0 > "$TRACEFS/tracing_on"
```

### Step 3: 分析退出模式

```bash
echo "=== 退出原因分布 ==="
grep 'kvm_exit' "$TRACEFS/trace" | \
    grep -oP 'reason \w+' | sort | uniq -c | sort -rn | head -15

echo ""
echo "=== 异常注入记录 ==="
grep 'kvm_inj_exception' "$TRACEFS/trace" | tail -20

echo ""
echo "=== 退到用户态记录 ==="
grep 'kvm_userspace_exit' "$TRACEFS/trace" | tail -20
```

### Step 4: bpftrace 分析退出频率

```bash
sudo bpftrace -e '
tracepoint:kvm:kvm_exit {
    @exits[args->exit_reason] = count();
    @total = count();
}
interval:s:5 {
    printf("=== VM-Exit 统计（5 秒）===\n");
    printf("Total exits: %d\n", @total);
    print(@exits, 10);
    clear(@exits);
    clear(@total);
}
'
```

### Step 5: 诊断 triple fault

```bash
# triple fault = 3 次异常注入失败 → KVM_EXIT_SHUTDOWN
echo "=== 检查 triple fault ==="
grep 'kvm_inj_exception' "$TRACEFS/trace" | \
    awk '{print $NF}' | sort | uniq -c | sort -rn

# 如果有异常注入，检查是否成功
echo ""
echo "=== 异常注入详情 ==="
grep 'kvm_inj_exception' "$TRACEFS/trace" | head -10

# 检查是否退到用户态
echo ""
echo "=== SHUTDOWN 退出 ==="
grep 'kvm_userspace_exit' "$TRACEFS/trace" | grep -i shutdown
```

**Triple fault 诊断路径**：
1. `kvm_inj_exception` 显示注入了什么异常（#PF / #GP / #UD）
2. 检查 guest IDT 是否正确设置（guest 内核问题）
3. 检查异常注入时的 guest 状态（CR0 / CR4 / EFER）

### Step 6: 诊断高频退出

```bash
# 如果某种退出特别多，深入分析
echo "=== 高频退出详细分析 ==="

# EPT_VIOLATION 多 → 内存问题
if grep -q 'EPT_VIOLATION' "$TRACEFS/trace"; then
    echo "EPT_VIOLATION 记录:"
    grep 'EPT_VIOLATION' "$TRACEFS/trace" | head -5
    echo "→ 检查 KVM_SET_USER_MEMORY_REGION 配置"
fi

# EXTERNAL_INTERRUPT 多 → 中断风暴
if grep -q 'EXTERNAL_INTERRUPT' "$TRACEFS/trace"; then
    echo ""
    echo "EXTERNAL_INTERRUPT 记录:"
    grep 'EXTERNAL_INTERRUPT' "$TRACEFS/trace" | wc -l
    echo "→ 检查 APICv / Posted Interrupts 配置"
fi

# IO_INSTRUCTION 多 → PIO 频繁
if grep -q 'IO_INSTRUCTION' "$TRACEFS/trace"; then
    echo ""
    echo "IO_INSTRUCTION 记录:"
    grep 'IO_INSTRUCTION' "$TRACEFS/trace" | head -5
    echo "→ 考虑 vhost 优化"
fi
```

---

## 📊 诊断决策树

```
vCPU 停止
    │
    ├─ QEMU 日志: KVM_EXIT_INTERNAL_ERROR
    │   → KVM 内部错误 → 检查 dmesg → dump_invalid_vmcs
    │
    ├─ QEMU 日志: KVM_EXIT_SHUTDOWN
    │   → triple fault → kvm_inj_exception 看注入了什么异常
    │   → 检查 guest IDT / CR0 / 内核状态
    │
    ├─ QEMU 日志: KVM_EXIT_FAIL_ENTRY
    │   → VM-Entry 失败 → dump_invalid_vmcs → 检查 VMCS 配置
    │
    ├─ 高频 EPT_VIOLATION
    │   → 内存映射问题 → 检查 KVM_SET_USER_MEMORY_REGION
    │   → mmu_notifier 失效 → 检查宿主内存管理
    │
    ├─ 高频 EXTERNAL_INTERRUPT
    │   → 中断风暴 → 检查 APICv / Posted Interrupts
    │   → 设备配置错误 → 检查中断路由
    │
    └─ 高频 IO_INSTRUCTION
        → PIO 频繁 → 考虑 vhost 优化
        → 设备模拟慢 → 检查 QEMU 设备实现
```

---

## ⚠️ 注意事项

1. **QEMU 日志必须启用**：`-D /tmp/qemu.log -d kvm`
2. **tracefs 全局状态**：实验结束清理
3. **KVM_EXIT 数字 vs 字符串**：QEMU 日志里是字符串，tracepoint 里 `exit_reason` 是数字
4. **triple fault 不总是 guest 问题**：可能是 KVM 异常注入逻辑 bug

---

## 📚 参考资料

- vCPU 退出诊断: `../../vcpu-exit-diagnosis.md` §2
- KVM trace events: `../../annotations.md` §1
- 异常注入路径: `../../annotations.md` §1.2
- 实战案例: `../../case-studies.md`
