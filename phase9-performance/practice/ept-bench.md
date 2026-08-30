# EPT 性能基准

> 目标：测量 EPT 缺页处理路径、对比 4K/2M 页、量化脏页日志开销。
> 理论背景见 `../../phase2-mem-virt/`；本文只给方法与已核实的观测点。

**所用观测点在 6.12.93 中的位置**：

| 观测点 | 位置 |
|--------|------|
| `kvm:kvm_page_fault` tracepoint | `arch/x86/kvm/trace.h:402` |
| `kvm_handle_page_fault()` | `arch/x86/kvm/mmu/mmu.c:4628` |
| `kvm_tdp_page_fault()` | `arch/x86/kvm/mmu/mmu.c:4726` |
| `kvm_tdp_mmu_map()` | `arch/x86/kvm/mmu/tdp_mmu.c:1104` |
| `make_spte()` | `arch/x86/kvm/mmu/spte.c:157` |
| `__tdp_mmu_set_spte_atomic()` | `arch/x86/kvm/mmu/tdp_mmu.c:533` |

---

## 实验环境

```bash
qemu-system-x86_64 \
    -enable-kvm -m 4G -smp 2 -cpu host \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &
QEMU_PID=$!
```

Guest 内准备 `stress-ng`（`apt install stress-ng`）。

## 实验 1：EPT 缺页处理延迟

```bash
TRACEFS=/sys/kernel/debug/tracing
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

echo kvm:kvm_page_fault > $TRACEFS/set_event

# 函数级跟踪：缺页处理主链
echo function > $TRACEFS/current_tracer
{
  echo kvm_handle_page_fault
  echo kvm_tdp_page_fault
  echo kvm_tdp_mmu_map
  echo make_spte
  echo __tdp_mmu_set_spte_atomic
} > $TRACEFS/set_ftrace_filter

echo 1 > $TRACEFS/tracing_on
# Guest 内: stress-ng --vm 1 --vm-bytes 1G --vm-method all --timeout 5
sleep 10
echo 0 > $TRACEFS/tracing_on

# 观察缺页事件与函数耗时（function tracer 带时戳）
cat $TRACEFS/trace | grep "kvm_tdp_mmu_map" | head -20
```

报告记录：缺页事件的每秒次数、`kvm_tdp_mmu_map` 附近的时间戳间隔分布。

## 实验 2：4K 页 vs 2M 大页

大页来源是**宿主侧**：guest 内存由 hugepage 支撑时，EPT 建 2M 映射。
两条路径对比（任选其一控制变量）：

1. THP：`echo never|always > /sys/kernel/mm/transparent_hugepage/enabled`
   后重启 VM（THP 对 memfd/mmap 后端是否生效取决于 QEMU 内存后端，
   记录实际生效方式）
2. 显式大页（更可控）：
   `-object memory-backend-file,mem-path=/dev/hugepages,size=4G,id=m0 \
    -numa node,memdev=m0`

```bash
# 每轮: 清 trace → 开跟踪 → Guest 内跑相同负载 → 计数
echo > $TRACEFS/trace
echo kvm:kvm_page_fault > $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on
# Guest 内: stress-ng --vm 1 --vm-bytes 512M --timeout 5
sleep 8
echo 0 > $TRACEFS/tracing_on
cat $TRACEFS/trace | grep -c kvm_page_fault
```

对比指标：缺页次数比（理论上界约 512:1）、缺页期总耗时、
Guest 内负载完成时间。`kvm:kvm_page_fault` 事件携带 `level` 相关
上下文，可从输出确认实际建立的映射级别。

## 实验 3：脏页日志开销

脏页日志由 `KVM_SET_USER_MEMORY_REGION` 的 `KVM_MEM_LOG_DIRTY_PAGES`
开启（迁移场景触发）。最简触发方式：QEMU monitor 内发起迁移
（`migrate` 到 `exec:cat >/dev/null`）让脏日志生效：

```bash
# 基准（无迁移）与迁移中各跑一次相同写负载，对比:
# 1) kvm:kvm_page_fault 计数
# 2) Guest 内 dd/stress-ng 完成时间
# 3) 宿主 perf: perf stat -p $QEMU_PID -- sleep 10 观察 CPU 占用变化
```

报告记录两种状态的差异，并对照 `../../phase2-mem-virt/` 中
写保护/脏位跟踪机制解释开销来源。

## 实验 4：内存压力下的热点函数

```bash
perf record -e kvm:kvm_page_fault -p $QEMU_PID -g -- sleep 30 &
# Guest 内: stress-ng --vm 4 --vm-bytes 1G --vm-method all --timeout 25
sleep 30
perf report --sort=dso,sym --stdio | head -40
```

把热点符号与上表源码位置对应，指出瓶颈在哪一段（查表 / 建表 /
cmpxchg 竞争）。竞争路径观察：`__tdp_mmu_set_spte_atomic()` 的
重试（对应 `RET_PF_RETRY` 语义，见 `../../phase2-mem-virt/` 注释）。

---

## 报告要求

1. 不同页大小下的缺页次数与处理耗时（实测值）
2. 脏页日志前后的性能差异及机制解释
3. 热点函数与源码路径的对应关系
4. 优化建议：何时用大页、何时脏日志开销可接受
