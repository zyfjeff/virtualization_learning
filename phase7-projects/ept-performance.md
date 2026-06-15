# 实践项目 2：EPT 性能分析

> 目标：分析 EPT 在真实工作负载下的性能特征

---

## 🎯 项目目标

通过测量和分析 EPT 缺页处理、大页映射、脏页日志等关键路径的性能，
深入理解 KVM 内存虚拟化的性能开销和优化空间。

---

## 📋 前置知识

- 第二阶段：EPT、SPTE、TDP MMU
- GPA → HPA 翻译流程
- EPT Violation 处理机制

---

## 🔧 实验环境

```bash
# 启动带 EPT 的测试虚拟机
qemu-system-x86_64 \
    -enable-kvm \
    -m 4G \
    -smp 2 \
    -cpu host \
    -numa node,memdev=mem0 -object memory-backend-ram,id=mem0,size=4G \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &

QEMU_PID=$!
```

---

## 📊 实验步骤

### 步骤 1：测量 EPT 缺页处理延迟

```bash
#!/bin/bash
# trace-ept-fault.sh

TRACEFS=/sys/kernel/debug/tracing

# 设置 EPT 缺页跟踪
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

# ★ trace event: kvm_page_fault (已验证存在于 6.12.93)
echo kvm:kvm_page_fault > $TRACEFS/set_event

# 开启函数级跟踪以测量延迟
# ★ 这些函数均存在于 6.12.93
echo function > $TRACEFS/current_tracer
echo kvm_handle_page_fault > $TRACEFS/set_ftrace_filter
echo kvm_tdp_page_fault >> $TRACEFS/set_ftrace_filter
echo kvm_tdp_mmu_map >> $TRACEFS/set_ftrace_filter
echo make_spte >> $TRACEFS/set_ftrace_filter
echo __tdp_mmu_set_spte_atomic >> $TRACEFS/set_ftrace_filter

# 启用延迟测量
echo 1 > $TRACEFS/tracing_on

# 在虚拟机内执行内存压力测试
# ssh vm "stress-ng --vm 1 --vm-bytes 1G --vm-method all --timeout 5"
sleep 10

echo 0 > $TRACEFS/tracing_on

# 分析函数调用延迟
cat $TRACEFS/trace | grep "kvm_tdp_mmu_map" | head -20

# 统计缺页处理时间分布
echo "缺页处理延迟统计:"
cat $TRACEFS/trace | grep -A2 "kvm_tdp_mmu_map" | head -30
```

### 步骤 2：对比 4K 页 vs 2MB 大页

```bash
#!/bin/bash

# --- 测试 1: 4K 页 ---
echo "=== 测试 4K 页 ==="

# 确保禁用大页
echo never > /sys/kernel/mm/transparent_hugepage/enabled

# 清空 EPT 缓存 (通过 VM 暂停/恢复触发)
# 在虚拟机内:
# echo 3 > /proc/sys/vm/drop_caches

# 跟踪缺页
echo 1 > /sys/kernel/debug/tracing/tracing_on
# ssh vm "stress-ng --vm 1 --vm-bytes 512M --timeout 5"
sleep 8
echo 0 > /sys/kernel/debug/tracing/tracing_on

# 统计缺页次数
FAULTS_4K=$(cat /sys/kernel/debug/tracing/trace | grep "kvm_page_fault" | wc -l)
echo "4K 页缺页次数: $FAULTS_4K"

# --- 测试 2: 2MB 大页 ---
echo "=== 测试 2MB 大页 ==="

# 启用大页
echo always > /sys/kernel/mm/transparent_hugepage/enabled

echo > /sys/kernel/debug/tracing/trace
echo 1 > /sys/kernel/debug/tracing/tracing_on
# ssh vm "stress-ng --vm 1 --vm-bytes 512M --timeout 5"
sleep 8
echo 0 > /sys/kernel/debug/tracing/tracing_on

FAULTS_2M=$(cat /sys/kernel/debug/tracing/trace | grep "kvm_page_fault" | wc -l)
echo "2MB 大页缺页次数: $FAULTS_2M"

echo "对比结果:"
echo "  4K 页缺页: $FAULTS_4K 次"
echo "  2MB 页缺页: $FAULTS_2M 次"
echo "  减少比例: $(echo "scale=1; (1-$FAULTS_2M/$FAULTS_4K)*100" | bc)%"
```

### 步骤 3：脏页日志性能分析

```bash
#!/bin/bash
# dirty-logging-test.sh

TRACEFS=/sys/kernel/debug/tracing

# 步骤 1: 基准测试（无脏页日志）
echo "=== 基准: 无脏页日志 ==="
echo > $TRACEFS/trace
echo kvm:kvm_page_fault > $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on

# 在虚拟机内运行内存写测试
# ssh vm "dd if=/dev/zero of=/tmp/test bs=1M count=512"
sleep 5
echo 0 > $TRACEFS/tracing_on

BASELINE=$(cat $TRACEFS/trace | grep "kvm_page_fault" | wc -l)
echo "基准缺页数: $BASELINE"

# 步骤 2: 启用脏页日志
echo "=== 测试: 启用脏页日志 ==="

# 通过 QEMU monitor 启用脏页日志
# 在 QEMU monitor 中:
#   info migrate  (查看迁移状态)
#   或通过 libvirt: virsh screenshot --memory-dump

echo > $TRACEFS/trace
echo 1 > $TRACEFS/tracing_on

# 再次运行相同的写测试
# ssh vm "dd if=/dev/zero of=/tmp/test2 bs=1M count=512"
sleep 5
echo 0 > $TRACEFS/tracing_on

DIRTY_LOG=$(cat $TRACEFS/trace | grep "kvm_page_fault" | wc -l)
echo "脏页日志缺页数: $DIRTY_LOG"

echo "脏页日志额外缺页: $((DIRTY_LOG - BASELINE))"
echo "性能影响: $(echo "scale=1; ($DIRTY_LOG/$BASELINE - 1)*100" | bc)%"
```

### 步骤 4：SPTE 分析

```bash
#!/bin/bash
# 分析 SPTE 的权限变化
# ★ 注意: kvm_mmu_set_spte/kvm_mmu_get_page 不是 trace events
# 使用 function trace 跟踪相关函数

TRACEFS=/sys/kernel/debug/tracing

echo > $TRACEFS/trace

# 跟踪 SPTE 相关函数
echo function > $TRACEFS/current_tracer
echo make_spte > $TRACEFS/set_ftrace_filter
echo __tdp_mmu_set_spte_atomic >> $TRACEFS/set_ftrace_filter
echo kvm_tdp_mmu_map >> $TRACEFS/set_ftrace_filter

# 也可以配合 kvm_page_fault trace event 使用
echo kvm:kvm_page_fault >> $TRACEFS/set_event

echo 1 > $TRACEFS/tracing_on
sleep 5
echo 0 > $TRACEFS/tracing_on

# 分析函数调用
echo "SPTE 相关函数调用:"
cat $TRACEFS/trace | grep -E "make_spte|tdp_mmu" | head -20

# 检查大页映射 (通过 kvm_page_fault trace event 的 level 字段)
echo ""
echo "大页映射统计 (从 kvm_page_fault 提取):"
cat $TRACEFS/trace | grep "kvm_page_fault" | head -20
```

### 步骤 5：内存压力下的性能

```bash
#!/bin/bash

# 使用 perf 分析内存压力下的 KVM 性能
echo "=== 内存压力测试 ==="

# 在 Host 端准备
echo 1 > /proc/sys/vm/drop_caches

# perf 记录
# ★ kvm:kvm_mmu_set_spte 不是真实 trace event，已移除
perf record -e kvm:kvm_page_fault \
    -p $QEMU_PID -g -- sleep 30 &

# 在虚拟机内运行压力测试
# ssh vm "stress-ng --vm 4 --vm-bytes 1G --vm-method all --timeout 25"
sleep 30

# 分析报告
perf report --stdio
perf report --sort=dso,sym --stdio | head -40
```

---

## 📈 预期分析结果

### 缺页处理延迟分布

```
期望观察:
  - 简单映射（页已分配）: ~1-5 μs
  - 需要分配页面:         ~5-20 μs
  - 大页映射:             ~2-10 μs
  - 竞争（cmpxchg 重试）: ~10-50 μs
```

### 大页 vs 4K 页性能对比

```
工作负载          4K 页缺页次数    2MB 页缺页次数    性能提升
────────────────  ──────────────   ──────────────   ────────
顺序读 512MB      ~131072          ~256             ~512x
随机读 512MB      ~131072          ~256             ~512x
内存分配密集      ~65536           ~128             ~512x
fork 密集型       ~32768           ~64              ~512x
```

### 脏页日志开销

```
操作类型           无日志         有日志          额外开销
─────────────────  ──────────     ──────────      ─────────
顺序写入 512MB     基准 T         ~1.05T          ~5%
随机写入 512MB     基准 T         ~1.5T           ~50%
混合读写           基准 T         ~1.1T           ~10%
```

---

## 🔍 深入分析方向

### 分析 EPT 缓存命中率

```bash
# 通过 MSR 读取 EPT 相关信息（如果支持）
# 注意：需要 root 权限，且某些 MSR 可能不可读

# 使用 perf 硬件事件
perf stat -e cache-misses,cache-references \
    -p $QEMU_PID -- sleep 10
```

### 分析 TDP MMU 并发

```bash
# 跟踪 cmpxchg 竞争
echo tdp_mmu_set_spte_atomic > /sys/kernel/debug/tracing/set_ftrace_filter
# 观察 RET_PF_RETRY 的频率
```

---

## 📝 报告要求

1. 记录不同页大小下的缺页次数和处理时间
2. 量化脏页日志对内存性能的影响
3. 分析 EPT 缺页处理中的热点函数
4. 提出优化建议（如：何时使用大页、如何减少脏页日志开销）
