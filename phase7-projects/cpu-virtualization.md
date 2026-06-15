# 实践项目 6：CPU 虚拟化深度分析

> 目标：通过实验深入理解 CPUID、MSR、指令虚拟化的实际行为

---

## 🎯 项目目标

通过实际操作验证 KVM 的 CPU 虚拟化机制：
1. 观察 Guest 看到的 CPUID 信息与 Host 的差异
2. 测量 MSR 拦截对性能的影响
3. 追踪指令虚拟化相关的 VM-Exit
4. 验证半虚拟化特性（kvmclock、PV EOI 等）

---

## 📋 前置知识

- 第一阶段：VT-x 基础 + CPU 虚拟化 (cpu-virtualization.md)
- CPUID 指令和叶号概念
- MSR 基本概念

---

## 🔧 实验环境

```bash
# 启动测试 VM
qemu-system-x86_64 \
    -enable-kvm \
    -m 2G \
    -smp 2 \
    -cpu host \
    -drive file=test.qcow2,format=qcow2 \
    -nographic -serial mon:stdio &

QEMU_PID=$!

# 在 Guest 内安装工具
# apt install cpuid msr-tools linux-tools-generic
```

---

## 📊 实验步骤

### 步骤 1：CPUID 对比分析

```bash
#!/bin/bash
# cpuid-compare.sh
# 对比 Host 和 Guest 的 CPUID

echo "=== CPUID 对比分析 ==="

echo ""
echo "--- Host CPUID ---"
echo "Vendor: $(cat /proc/cpuinfo | grep 'vendor_id' | head -1 | cut -d: -f2)"
echo "Model name: $(cat /proc/cpuinfo | grep 'model name' | head -1 | cut -d: -f2)"
echo "Flags: $(cat /proc/cpuinfo | grep 'flags' | head -1 | cut -d: -f2)" | tr ' ' '\n' | wc -l
echo " 个特性标志"

echo ""
echo "--- Guest CPUID (在 Guest 内执行) ---"
echo "Vendor: $(cat /proc/cpuinfo | grep 'vendor_id' | head -1 | cut -d: -f2)"
echo "Model name: $(cat /proc/cpuinfo | grep 'model name' | head -1 | cut -d: -f2)"
echo "Flags: $(cat /proc/cpuinfo | grep 'flags' | head -1 | cut -d: -f2)" | tr ' ' '\n' | wc -l
echo " 个特性标志"

echo ""
echo "--- Hypervisor CPUID (0x40000000) ---"
# 应看到 "KVMKVMKVM"
cpuid -1 -l 0x40000000 2>/dev/null || \
    echo "  (安装 cpuid 工具: apt install cpuid)"

echo ""
echo "--- KVM 特性 CPUID (0x40000001) ---"
cpuid -1 -l 0x40000001 2>/dev/null

echo ""
echo "--- 关键差异分析 ---"
echo "检查 VMX 位 (Guest 应该看不到 VMX, 除非启用嵌套):"
echo "  Host: $(grep -c 'vmx' /proc/cpuinfo) 个 CPU 有 VMX"
echo "  (Guest 内执行): grep -c 'vmx' /proc/cpuinfo"

echo ""
echo "检查 hypervisor 位:"
echo "  Host: $(grep -c 'hypervisor' /proc/cpuinfo) 个 CPU 有 hypervisor 标志"
echo "  Guest: 应该有 hypervisor 标志 (告诉 OS 在虚拟化环境中)"
```

### 步骤 2：MSR 访问追踪

```bash
#!/bin/bash
# msr-trace.sh
# 追踪 MSR 拦截和访问

echo "=== MSR 访问追踪 ==="

TRACEFS=/sys/kernel/debug/tracing

# 设置 MSR 追踪
echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

echo kvm:kvm_msr > $TRACEFS/set_event

# 按 QEMU PID 过滤
if [ -n "$QEMU_PID" ]; then
    echo $QEMU_PID > $TRACEFS/set_event_pid 2>/dev/null
fi

echo 1 > $TRACEFS/tracing_on

# 在 Guest 内触发 MSR 访问
# 方法1: 读 TSC
#   rdtsc (通过内联汇编)
# 方法2: 读/写 EFER
#   rdmsr -a 0xC0000080
# 方法3: 运行一个工作负载
#   ssh vm "stress --cpu 1 --timeout 5"

echo "追踪 5 秒..."
sleep 5

echo 0 > $TRACEFS/tracing_on

echo ""
echo "=== MSR 访问统计 ==="
echo "总 MSR VM-Exit 次数: $(cat $TRACEFS/trace | grep kvm_msr | wc -l)"

echo ""
echo "--- 按 MSR 号统计 ---"
# 提取 MSR 号并统计
cat $TRACEFS/trace | grep kvm_msr | \
    grep -oP 'msr=0x[0-9a-f]+' | \
    sort | uniq -c | sort -rn | head -15

echo ""
echo "--- 按读写方向统计 ---"
echo "读 (rdmsr): $(cat $TRACEFS/trace | grep kvm_msr | grep -c 'write=0')"
echo "写 (wrmsr): $(cat $TRACEFS/trace | grep kvm_msr | grep -c 'write=1')"
```

### 步骤 3：指令 VM-Exit 分析

```bash
#!/bin/bash
# instruction-exit-trace.sh
# 分析各类指令触发的 VM-Exit

echo "=== 指令 VM-Exit 分析 ==="

TRACEFS=/sys/kernel/debug/tracing

echo > $TRACEFS/trace
echo 0 > $TRACEFS/tracing_on

# 启用所有 KVM exit 追踪
echo kvm:kvm_exit > $TRACEFS/set_event

if [ -n "$QEMU_PID" ]; then
    echo $QEMU_PID > $TRACEFS/set_event_pid 2>/dev/null
fi

echo 1 > $TRACEFS/tracing_on

# 在 Guest 内运行工作负载
echo "追踪 5 秒..."
sleep 5

echo 0 > $TRACEFS/tracing_on

echo ""
echo "=== VM-Exit 原因分布 ==="
cat $TRACEFS/trace | grep kvm_exit | \
    sed 's/.*reason //' | awk '{print $1}' | \
    sort | uniq -c | sort -rn | head -20

echo ""
echo "--- CPU 虚拟化相关 Exit ---"
echo "CPUID:      $(cat $TRACEFS/trace | grep kvm_exit | grep -c 'CPUID')"
echo "MSR_READ:   $(cat $TRACEFS/trace | grep kvm_exit | grep -c 'MSR_READ')"
echo "MSR_WRITE:  $(cat $TRACEFS/trace | grep kvm_exit | grep -c 'MSR_WRITE')"
echo "IO:         $(cat $TRACEFS/trace | grep kvm_exit | grep -c 'IO_INSTRUCTION')"
echo "HLT:        $(cat $TRACEFS/trace | grep kvm_exit | grep -c 'HLT')"
echo "CR_ACCESS:  $(cat $TRACEFS/trace | grep kvm_exit | grep -c 'CR_ACCESS')"
echo "VMCALL:     $(cat $TRACEFS/trace | grep kvm_exit | grep -c 'VMCALL')"

echo ""
echo "--- 分析 ---"
TOTAL=$(cat $TRACEFS/trace | grep kvm_exit | wc -l)
CPU_EXIT=$(cat $TRACEFS/trace | grep kvm_exit | \
    grep -E 'CPUID|MSR|IO_INSTRUCTION|HLT|CR_ACCESS|VMCALL|MONITOR|PAUSE' | wc -l)

echo "总 VM-Exit: $TOTAL"
echo "CPU 虚拟化相关: $CPU_EXIT ($(echo "scale=1; $CPU_EXIT*100/$TOTAL" | bc)%)"
echo ""
echo "IO_INSTRUCTION 高: 可能是 IO 端口密集操作 (PIT/PIO磁盘)"
echo "MSR_READ/WRITE 高: MSR 拦截频繁, 检查哪些 MSR 需要透传"
echo "HLT 高: Guest 空闲时间多, 考虑 halt-polling 优化"
echo "VMCALL 高: 半虚拟化调用频繁 (kvmclock/PV EOI/kick)"
```

### 步骤 4：半虚拟化特性验证

```bash
#!/bin/bash
# pv-features.sh
# 验证 KVM 半虚拟化特性

echo "=== KVM 半虚拟化特性验证 ==="

echo ""
echo "--- 1. kvmclock ---"
echo "当前时钟源: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
# 预期: kvm-clock

echo ""
echo "--- 2. PV EOI (半虚拟化 EOI) ---"
echo "  检查 CPU flags: $(grep 'kvm_pv_eoi' /proc/cpuinfo | head -1)"
# 如果有 kvm_pv_eoi: Guest 使用半虚拟化 EOI, 减少 APIC EOI 的 VM-Exit

echo ""
echo "--- 3. PV Unhalt (半虚拟化唤醒) ---"
echo "  检查 CPU flags: $(grep 'kvm_pv_unhalt' /proc/cpuinfo | head -1)"
# 如果有 kvm_pv_unhalt: Guest PLE (Pause Loop Exiting) 优化

echo ""
echo "--- 4. PV TLB flush ---"
echo "  检查 CPU flags: $(grep 'kvm_pv_tlb_flush' /proc/cpuinfo | head -1)"

echo ""
echo "--- 5. Steal time (CPU 偷取时间) ---"
cat /proc/stat | grep '^cpu' | head -1
echo "  最后一列是 steal time (被 Host 抢占的时间)"

echo ""
echo "--- 6. Async PF (异步缺页) ---"
echo "  检查 CPU flags: $(grep 'kvm_asyncpf' /proc/cpuinfo | head -1)"

echo ""
echo "--- 总结 ---"
echo "KVM CPUID 特性叶 (0x40000001):"
cpuid -1 -l 0x40000001 2>/dev/null || echo "  (需要 cpuid 工具)"
```

### 步骤 5：MSR 透传优化验证

```bash
#!/bin/bash
# msr-optimization.sh
# 对比不同 MSR Bitmap 配置的性能

echo "=== MSR 透传优化验证 ==="

echo ""
echo "--- 1. 基准: 当前 MSR VM-Exit 频率 ---"
TRACEFS=/sys/kernel/debug/tracing

echo > $TRACEFS/trace
echo kvm:kvm_msr > $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on
sleep 3
echo 0 > $TRACEFS/tracing_on

BASELINE=$(cat $TRACEFS/trace | grep kvm_msr | wc -l)
echo "3秒内 MSR VM-Exit 次数: $BASELINE ($(echo "scale=1; $BASELINE/3" | bc)/秒)"

echo ""
echo "--- 2. 分析主要 MSR ---"
cat $TRACEFS/trace | grep kvm_msr | \
    grep -oP 'msr=0x[0-9a-f]+' | \
    sort | uniq -c | sort -rn | head -5

echo ""
echo "--- 3. 常见优化 ---"
echo ""
echo "a) IA32_SPEC_CTRL (0x48): Spectre 缓解, 通常透传"
echo "   检查: grep 'spec_ctrl' /proc/cpuinfo"

echo ""
echo "b) x2APIC MSRs (0x800-0x8FF): APICv 启用时部分透传"
echo "   检查 APICv: cat /sys/module/kvm_intel/parameters/enable_apicv"

echo ""
echo "c) TSC (0x10): 如果 Constant TSC, 可以用 TSC offsetting"
echo "   检查: grep 'constant_tsc' /proc/cpuinfo"

echo ""
echo "--- 4. 验证 VMCS MSR Bitmap ---"
echo "  通过 vmcs dump (需要调试工具) 或 ftrace 观察哪些 MSR 被拦截"
echo "  KVM 自动优化的 MSR:"
echo "    - x2APIC: 当 APICv 启用时, 自动更新 MSR Bitmap"
echo "    - 函数: vmx_update_msr_bitmap_x2apic()"
```

### 步骤 6：CPUID 隐藏特性测试

```bash
#!/bin/bash
# cpuid-stealth.sh
# 测试 KVM 如何隐藏特定 CPU 特性

echo "=== CPUID 特性隐藏测试 ==="

echo ""
echo "--- 1. VMX 位隐藏 (无嵌套虚拟化时) ---"
echo "  在 Guest 内:"
echo "  grep vmx /proc/cpuinfo"
echo "  预期: 无输出 (VMX 被隐藏, 防止 Guest 尝试嵌套 VMX)"

echo ""
echo "--- 2. 嵌套虚拟化启用后 ---"
echo "  在 Host 上启用嵌套:"
echo "  echo 1 > /sys/module/kvm_intel/parameters/nested"
echo "  (需要重启 VM)"
echo "  然后:"
echo "  grep vmx /proc/cpuinfo"
echo "  预期: 有输出 (VMX 现在对 Guest 可见)"

echo ""
echo "--- 3. SMAP/SMEP 隐藏测试 ---"
echo "  Guest 可见特性受 QEMU -cpu 参数控制:"
echo "  -cpu host        → 透传所有 Host 特性"
echo "  -cpu qemu64      → 最小特性集"
echo "  -cpu max         → 所有 KVM 支持的特性"
echo "  -cpu host,-vmx   → 透传所有但隐藏 VMX"

echo ""
echo "--- 4. 查看 QEMU 配置的 CPUID ---"
echo "  在 QEMU monitor 中:"
echo "  (qemu) info cpuid"
echo "  或通过 HMP:"
echo "  echo 'info cpuid' | socat STDIO UNIX-CONNECT:/tmp/qemu-monitor.sock"
```

---

## 📈 预期分析结果

### MSR VM-Exit 典型分布

```
MSR 号           名称                  频率(次/秒)   说明
──────────────  ─────────────────────  ────────────  ─────────────────
0xC0000080      IA32_EFER             ~500-2000     长模式控制, 频繁读写
0xC0000103      IA32_TSC_AUX          ~100-500      RDTSCP 辅助值
0x00000010      IA32_TIME_STAMP_COUNTER ~100-300     TSC 写入 (同步)
0x00000277      IA32_PAT              ~50-200       页属性表
0xC0000081-83   STAR/LSTAR/CSTAR      ~20-100       SYSCALL 配置
0x00000048      IA32_SPEC_CTRL        ~0-10         透传 (通常不拦截)
```

### VM-Exit 原因典型分布 (CPU 虚拟化相关)

```
Exit 原因              占比      主要触发源
───────────────────  ────────  ──────────────────────────
EXTERNAL_INTERRUPT   ~30%     设备中断 (取决于工作负载)
EPT_VIOLATION        ~20-40%  内存访问 (最大来源!)
HLT                  ~10-20%  Guest 空闲
IO_INSTRUCTION       ~5-15%   PIO 设备 (串口/磁盘)
VMCALL               ~3-8%    半虚拟化调用 (kvmclock/PV)
MSR_READ/WRITE       ~2-5%    MSR 拦截
CR_ACCESS            ~1-3%    CR0/CR3/CR4 修改
CPUID                ~0-1%    通常不拦截 (除非嵌套)
PREEMPTION_TIMER     ~1-3%    抢占定时器到期
```

### 半虚拟化特性效果

```
特性               VM-Exit 减少    场景
────────────────  ──────────────  ──────────────────────────
kvmclock          ~1000/s         替代 PIT/HPET 读取
PV EOI            ~100-500/s      替代 APIC EOI 写
PV TLB flush      ~50-200/s       替代 INVLPG 广播
PV unhalt         ~10-50/s        PLE 优化
Async PF          ~100-500/s      内存超分配时减少 stall
```

---

## 🔍 深入分析

### 分析1: 为什么 CPUID 通常不拦截？

```bash
echo "=== CPUID 性能分析 ==="

# 启用 CPUID exit (如果 QEMU 启用了)
TRACEFS=/sys/kernel/debug/tracing
echo > $TRACEFS/trace
echo kvm:kvm_cpuid > $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on
sleep 3
echo 0 > $TRACEFS/tracing_on

COUNT=$(cat $TRACEFS/trace | grep kvm_cpuid | wc -l)
echo "3秒内 CPUID VM-Exit: $COUNT"

# 如果为 0: QEMU 使用静态过滤, CPUID 不触发 VM-Exit
# 如果有值: QEMU 启用了 CPUID exiting

echo ""
echo "CPUID 频率分析:"
echo "  Linux 内核启动: ~1000+ 次 CPUID"
echo "  稳态运行: ~10-100 次/秒"
echo "  如果每次都 VM-Exit: ~1-10 μs/次 = ~100-1000 μs/秒"
echo "  使用静态过滤: 0 VM-Exit = 0 开销"
echo "  → 静态过滤是性能最优选择"
```

### 分析2: 优化 MSR 拦截

```bash
echo "=== MSR 优化建议 ==="

echo ""
echo "场景: 如果某个 MSR 的 VM-Exit 频率很高但不需要拦截:"
echo "  1. 检查 MSR Bitmap 配置"
echo "  2. 如果安全, 可以设置 MSR Bitmap 对应位为 0 (透传)"
echo "  3. KVM 已有自动优化:"
echo "     - x2APIC MSR: APICv 启用时自动透传"
echo "     - SPEC_CTRL/PRED_CMD: 通常透传"
echo ""
echo "自定义优化 (高级, 需要修改 KVM):"
echo "  文件: arch/x86/kvm/vmx/vmx.c"
echo "  函数: vmx_disable_intercept_for_msr()"
echo "  添加需要透传的 MSR 到 bitmap"
```

---

## 📝 报告要求

1. **CPUID 对比表**: 列出 Host 和 Guest 的 5 个关键 CPUID 叶差异
2. **MSR 访问统计**: 记录 Top-10 最频繁的 MSR 访问
3. **VM-Exit 分布图**: 按原因分类统计, 标注 CPU 虚拟化相关的占比
4. **半虚拟化效果**: 列出检测到的 PV 特性及其预期收益
5. **优化建议**: 基于测量数据, 提出 MSR/CPUID 拦截优化建议

---

## 📚 延伸阅读

- Phase 1 (phase1-vtx-basics/cpu-virtualization.md) — CPU 虚拟化理论
- Phase 6 项目2 (ept-performance.md) — 内存性能分析（对比方法论）
- `arch/x86/kvm/cpuid.c` — CPUID 虚拟化源码
- `arch/x86/kvm/vmx/vmx.c` — MSR Bitmap 和 vmx_get/set_msr
