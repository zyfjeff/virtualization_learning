# Phase 1 深度实践 — CPU 虚拟化源码精读配套练习

> 基于 Linux 6.12.93 源码。每个练习连接 `../annotations.md` 的源码精读，
> 要求**先读懂源码，再做实验验证**。
>
> **执行环境**：
> - ex1/ex2/ex3/ex4/ex5 在 **Guest 内**执行（读取虚拟化的硬件状态）
> - trace/profile 脚本在**宿主侧**执行（观测 KVM 行为）
> - VM 启动必须带 `-enable-kvm -cpu host`（`boot-vm.sh` 默认已带）

---

## 练习总览

| # | 名称 | 核心问题 | 连接 annotations.md | 执行位置 |
|---|------|---------|-------------------|---------|
| 1 | VMX Capability 深度解码 | KVM 为什么只启用这些 VMX 特性？依赖链是什么？ | §5 VMX 特性检测 | Guest |
| 2 | MSR Bitmap 可视化与热 MSR 分析 | MSR Bitmap 如何减少 VM-Exit？哪些 MSR 最热？ | §5, §6 | 宿主 + Guest |
| 3 | VM-Exit Reason Profiling | 不同工作负载的 VM-Exit 分布有何差异？ | §3 VM-Exit 分发 | 宿主 |
| 4 | CPUID 虚拟化双机制对比 | 静态过滤 vs 动态拦截，KVM 如何选择？ | §5, annotations §2 | Guest + 宿主 |
| 5 | vCPU 调度与 Halt-Polling 观测 | halt-polling 如何在延迟和 CPU 占用间权衡？ | phase0 §7 halt-polling | 宿主 |

---

## 环境准备

```bash
# 宿主侧：启动 VM（默认 -enable-kvm -cpu host）
cd ../../scripts/vm
./boot-vm.sh ubuntu --memory 4G --cpus 4

# 确认走的是 KVM（不是 TCG）
ls -l /proc/$(pgrep -f '^qemu-system-x86_64')/fd | grep -c kvm
# 输出应 >0；=0 说明走的是 TCG，所有 KVM 追踪实验结论都无效

# 宿主侧：编译练习程序（如果还没有编译）
cd practice
make clean && make

# 将编译好的程序复制到共享目录，供 Guest 访问
cp ex* /root/code/kvm-study/scripts/shared/

# Guest 侧：手动挂载 9p 共享目录（ubuntu rootfs 不会自动挂载）
sudo mkdir -p /mnt/shared
sudo mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/shared

# 验证挂载
ls /mnt/shared/ex*
# 应该能看到 ex1-vmx-verify, ex2-cpuid-fault 等

# 宿主侧：确认 tracefs 可用
ls /sys/kernel/tracing/events/kvm/ | head
# 应有 kvm_exit, kvm_entry, kvm_cpuid, kvm_msr 等
```

---

## Exercise 1: VMX Capability 深度解码

### 目标

完整解码 KVM 读取的所有 VMX capability MSR，理解特性依赖链。

**核心问题**：`vmx_hardware_setup()` 读完 MSR 后为什么只启用这些特性？如果某位不支持，
KVM 怎么回退？

### 连接 annotations.md

- §5「VMX 特性检测」：`setup_vmcs_config()` 读取哪些 MSR
- §5「特性依赖链」：EPT → unrestricted_guest, APICv → Posted Interrupts
- §6「模块参数」：`ept`, `vpid`, `apicv` 等参数如何影响决策

### 步骤

#### 1.1 运行增强版 ex1-vmx-verify

```bash
./ex1-vmx-verify
```

程序会解码 7 个 VMX MSR：
- IA32_VMX_BASIC (0x480)
- IA32_VMX_PINBASED_CTLS (0x481)
- IA32_VMX_PROCBASED_CTLS (0x482)
- IA32_VMX_EXIT_CTLS (0x483)
- IA32_VMX_ENTRY_CTLS (0x484)
- IA32_VMX_PROCBASED_CTLS2 (0x48B)
- IA32_VMX_EPT_VPID_CAP (0x48C)

每个 MSR 的 allowed-0（必须为 1）和 allowed-1（可以为 1）位都会解码。

#### 1.2 对照 KVM 源码

读 `arch/x86/kvm/vmx/vmx.c:2590` — `setup_vmcs_config()`：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:2590 */
static int setup_vmcs_config(struct vmcs_config *vmcs_conf,
                             struct vmx_capability *vmx_cap)
{
    /* 读 MSR，取交集合 */
    if (adjust_vmx_controls(KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL,
                            KVM_OPTIONAL_VMX_CPU_BASED_VM_EXEC_CONTROL,
                            MSR_IA32_VMX_PROCBASED_CTLS,
                            &_cpu_based_exec_control))
        return -EIO;
    /* ... */
}
```

`adjust_vmx_controls()` 的逻辑（`vmx.c:2563`）：
- `ctl &= vmx_msr_high;` — 硬件不支持的位（high=0）强制清零
- `ctl |= vmx_msr_low;` — 硬件强制要求的位（low=1）强制置 1
- `if (ctl_min & ~ctl)` — 如果 KVM 必须的位硬件不支持，返回 -EIO

#### 1.3 验证特性依赖链

对照 `vmx_hardware_setup()`（`vmx.c:8404`）：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:8404 */
__init int vmx_hardware_setup(void)
{
    if (setup_vmcs_config(&vmcs_config, &vmx_capability) < 0)
        return -EIO;

    /* EPT 依赖：4 级页表 + WB 内存类型 + INVEPT 全局 */
    if (!cpu_has_vmx_ept() || !cpu_has_vmx_ept_4levels() ||
        !cpu_has_vmx_ept_mt_wb() || !cpu_has_vmx_invept_global())
        enable_ept = 0;

    /* unrestricted_guest 依赖 EPT */
    if (!cpu_has_vmx_unrestricted_guest() || !enable_ept)
        enable_unrestricted_guest = 0;

    /* APICv 是 Posted Interrupts 的前提 */
    if (!cpu_has_vmx_apicv())
        enable_apicv = 0;
    if (!enable_apicv)
        vt_x86_ops.sync_pir_to_irr = NULL;  /* 禁用回调 */
}
```

### 思考题

1. **如果 CPU 不支持 EPT_AD_BITS（EPT A/D 位），KVM 会怎样回退？**
   - 读 `vmx.c` 找 `enable_ept_ad_bits` 的决策逻辑
   - 没有 A/D 位时，KVM 必须用更昂贵的页表 walk 来跟踪访问/脏位
   - 对照 Intel SDM Vol 3, Appendix A.3.3

2. **为什么 `PIN_BASED_ALWAYSON_WITHOUT_TRUE_MSR = 0x00000016`？**
   - 哪些位是「即使没有 true-CTLS MSR 也必须为 1」的？
   - 对照 SDM Vol 3, Appendix A.3.1

3. **PROCBASED_CTLS 的 allowed-0 里有哪些位是 KVM 必须启用的？**
   - 读 `KVM_REQUIRED_VMX_CPU_BASED_VM_EXEC_CONTROL` 定义
   - 为什么 MSR_BITMAP 是必须的？

### 输出示例

```
IA32_VMX_BASIC (0x480) = 0x0000000000000000
  VMCS revision: 1
  VMCS size: 4096 bytes
  True controls: supported
  ...

IA32_VMX_PINBASED_CTLS (0x481) = 0xXXXXXXXXXXXXXXXX
  allowed-0 (must-be-1): 0x00000016
    bit 1: 1 (Reserved)
    bit 2: 1 (Reserved)
    bit 4: 1 (Reserved)
  allowed-1 (can-be-1): 0x0000006F
    bit 0: 1 - External-interrupt exiting
    bit 3: 1 - NMI exiting
    bit 5: 1 - Virtual NMIs
    bit 6: 1 - Activate VMX-preemption timer
    bit 7: 1 - Process posted interrupts
  ...
```

---

## Exercise 2: MSR Bitmap 可视化与热 MSR 分析

### 目标

理解 MSR Bitmap 如何减少 VM-Exit，追踪哪些 MSR 最热。

**核心问题**：KVM 初始把所有 MSR 都拦截（bitmap 全 1），运行时按需放开。哪些 MSR 被放开？
为什么？

### 连接 annotations.md

- §5「VMX 特性检测」：MSR_BITMAP 控制位
- §6「模块参数」：动态调整 MSR 拦截

### 步骤

#### 2.1 理解正确的测量方法

**⚠️ 常见陷阱**：在 Guest 中通过 `/dev/cpu/0/msr` 测量 MSR 访问时间

```bash
# 错误方法（不推荐）
./ex3-msr-test
```

**问题**：
- 系统调用开销（~2000-3000 ns）淹没了 VM-Exit 差异
- 结果：透传 vs 拦截只有 1-2x 差异（误导）

**正确方法**：在宿主侧追踪 `kvm:kvm_msr` 事件

```bash
# 正确方法
sudo ./trace-msr-access.sh -d 5
```

**原理**：
- **透传 MSR**：**不出现**在 `kvm:kvm_msr` trace 中（无 VM-Exit，直接读物理 MSR）
- **拦截 MSR**：**出现**在 trace 中（每次 VM-Exit，开销 ~1500-3000 ns）

#### 2.2 在宿主侧用 ftrace 追踪 MSR 访问

```bash
sudo ./trace-msr-access.sh -p $(pgrep -f qemu-system) -d 10
```

脚本会启用 `kvm:kvm_msr` 事件，收集 10 秒内的 MSR 访问统计。

#### 2.3 分析 MSR Bitmap 实现

读 `arch/x86/kvm/vmx/vmx.c`：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:171 */
static u32 vmx_possible_passthrough_msrs[MAX_POSSIBLE_PASSTHROUGH_MSRS] = {
    MSR_IA32_SPEC_CTRL,      /* 0x48 - Spectre 缓解 */
    MSR_IA32_PRED_CMD,       /* 0x49 - Indirect branch prediction */
    MSR_IA32_FLUSH_CMD,      /* 0x10B - L1D flush */
    MSR_IA32_TSC,            /* 0x10 - Timestamp counter */
    MSR_FS_BASE,             /* 0xC0000100 - FS base */
    MSR_GS_BASE,             /* 0xC0000101 - GS base */
    MSR_KERNEL_GS_BASE,      /* 0xC0000102 - Kernel GS base */
    MSR_IA32_XFD,            /* 0x1C4 - Extended feature disable */
    MSR_IA32_XFD_ERR,        /* 0x1C5 - XFD error */
    MSR_IA32_SYSENTER_CS,    /* 0x174 - SYSENTER CS */
    MSR_IA32_SYSENTER_ESP,   /* 0x175 - SYSENTER ESP */
    MSR_IA32_SYSENTER_EIP,   /* 0x176 - SYSENTER EIP */
    MSR_CORE_C1_RES,         /* C1 residency */
    MSR_CORE_C3_RESIDENCY,   /* C3 residency */
    MSR_CORE_C6_RESIDENCY,   /* C6 residency */
    MSR_CORE_C7_RESIDENCY,   /* C7 residency */
};
```

MSR Bitmap 初始全 1（全部拦截）（`vmx.c:2963`）：
```c
memset(loaded_vmcs->msr_bitmap, 0xff, PAGE_SIZE);
```

运行时按需放开（`vmx.c:4000`）：
```c
void vmx_disable_intercept_for_msr(struct kvm_vcpu *vcpu, u32 msr, int type)
{
    /* 清 bitmap 对应位 → 硬件不再拦截该 MSR */
    if (type & MSR_TYPE_R)
        vmx_clear_msr_bitmap_read(msr_bitmap, msr);
    if (type & MSR_TYPE_W)
        vmx_clear_msr_bitmap_write(msr_bitmap, msr);
}
```

### 思考题

1. **为什么 IA32_TSC 被透传，但 IA32_EFER 被拦截？**
   - TSC 是只读的，Guest 读 TSC 不会改变状态
   - EFER 控制长模式（LME/LMA），Guest 写 EFER 必须被 KVM 拦截
   - 对照 `vmx_set_msr()` 里的处理逻辑

2. **MSR Bitmap 的布局是什么？**
   - 低 1024 字节：MSR 0x00000000 - 0x00001FFF 的读拦截
   - 接下来 1024 字节：MSR 0x00000000 - 0x00001FFF 的写拦截
   - 接下来 1024 字节：MSR 0xC0000000 - 0xC0001FFF 的读拦截
   - 最后 1024 字节：MSR 0xC0000000 - 0xC0001FFF 的写拦截
   - 对照 Intel SDM Vol 3, Section 24.6.9, Figure 24-11

3. **如果 Guest 访问不在 bitmap 范围内的 MSR（如 0x2000），会怎样？**
   - 硬件会触发 VM-Exit（因为 bitmap 只覆盖 0x0000-0x1FFF 和 0xC000-0xC001）
   - KVM 在 `handle_msr()` 里模拟

---

## Exercise 3: VM-Exit Reason Profiling

### 目标

分析不同工作负载的 VM-Exit 分布，理解快速路径 vs 慢速路径。

**核心问题**：idle VM 和 CPU-bound VM 的 VM-Exit 分布有何差异？哪些 Exit 走快速路径？

### 连接 annotations.md

- §3「VM-Exit 分发」：`kvm_vmx_exit_handlers[]` 表
- §3「exit_fastpath」：快速路径的 4 种返回值

### 步骤

#### 3.1 运行 perf kvm stat

```bash
sudo ./profile-vmexit.sh -p $(pgrep -f qemu-system) -d 30
```

脚本会收集 30 秒的 `perf kvm stat` 数据，并按 Exit reason 统计。

#### 3.2 运行 3 种负载

使用 `stress` 统一测试不同负载类型，观察 VM-Exit 分布差异。

**负载 1: idle**
```bash
# Guest 内什么都不做，或运行 sleep
sleep 30
```
预期：HLT + PREEMPTION_TIMER 主导

**负载 2: CPU-bound**
```bash
# Guest 内运行 stress（纯 CPU 计算）
stress --cpu 4 --timeout 30
```
预期：CPUID 主导（glibc 运行时检测 CPU 特性）

**负载 3: IO-bound**
```bash
# Guest 内运行 stress（I/O 密集，Buffered I/O）
# 注意：确保当前目录不是 tmpfs（如 /tmp），否则还是写内存
cd /root  # 或 df -T . 确认是真实磁盘
stress --io 4 --timeout 30

# 如果要测试 Direct I/O（绕过 page cache）：
stress --hdd 4 --hdd-bytes 1G --hdd-opts direct --timeout 30
```
预期：
- **Buffered I/O**：EPT_VIOLATION（写 page cache）+ 少量 IO_INSTRUCTION（fsync）
- **Direct I/O（virtio-blk）**：
  - **EPT_MISCONFIG 主导**（virtio MMIO 区域访问，内存类型配置触发）
  - 少量 IO_INSTRUCTION（legacy 端口 I/O）
  - MSR_WRITE（定时器）

**注意**：EPT_MISCONFIG 通常表示 EPT 页表内存类型配置问题，但对于 virtio MMIO 区域，这是**正常的 MMIO 处理机制**——KVM 故意将 MMIO 区域配置为特殊内存类型，Guest 访问时触发 VM-Exit 由 KVM 模拟。

**负载 4: Memory-bound**（可选）
```bash
# Guest 内运行 stress（内存密集）
stress --vm 4 --vm-bytes 512M --timeout 30
```
预期：EPT_VIOLATION 主导

**负载 5: 综合负载**（可选）
```bash
# 混合 CPU + IO + Memory
stress --cpu 2 --io 2 --vm 2 --timeout 30
```
预期：各种 Exit 均衡分布

#### 3.3 分析 VM-Exit 分布

对照 `vmx.c:6095` 的 `kvm_vmx_exit_handlers[]` 表：

| Exit reason | 处理函数 | 快速路径？ | 说明 |
|-------------|---------|-----------|------|
| EXTERNAL_INTERRUPT | `handle_external_interrupt()` | 可能 | Posted Interrupt |
| PREEMPTION_TIMER | `handle_preemption_timer()` | 是 | 定时器到期 |
| EPT_VIOLATION | `handle_ept_violation()` | 否 | 建页表 |
| MSR_WRITE | `kvm_emulate_wrmsr()` | 否 | 模拟 MSR 写 |
| CPUID | `handle_cpuid()` | 否 | 回用户空间 |
| IO_INSTRUCTION | `handle_io()` | 可能 | 回用户空间 |

### 思考题

1. **为什么 idle VM 的 PREEMPTION_TIMER 占比最高？**
   - idle VM 执行 HLT，KVM 设置 preemption timer 防止无限阻塞
   - timer 到期触发 VM-Exit，KVM 检查是否有中断，没有则继续 halt
   - 对照 `handle_preemption_timer()` 实现

2. **EXTERNAL_INTERRUPT 在什么情况下走快速路径？**
   - Posted Interrupts 启用时，外部中断不触发 VM-Exit
   - 但如果通知向量不匹配，或 PI 未启用，会触发 `EXTERNAL_INTERRUPT`
   - 对照 `handle_external_interrupt()` 和 SDM Vol 3, Section 30.6

3. **为什么 IO_INSTRUCTION 有时走快速路径，有时回用户空间？**
   - 如果 IO 端口被 QEMU 模拟（如 0x3f8 串口），回用户空间
   - 如果 IO 端口由内核处理（如 virtio MMIO），可能在内核完成
   - 对照 `handle_io()` 的返回值

### 输出示例

```
=== VM-Exit Reason Distribution (30s) ===

Idle VM:
  PREEMPTION_TIMER      15000 (50.0%)
  EXTERNAL_INTERRUPT     8000 (26.7%)
  EPT_VIOLATION          3000 (10.0%)
  HLT                    2000  (6.7%)
  ...

CPU-bound VM:
  EPT_VIOLATION         50000 (83.3%)
  PREEMPTION_TIMER       5000  (8.3%)
  MSR_WRITE              3000  (5.0%)
  ...

IO-bound VM:
  IO_INSTRUCTION        40000 (66.7%)
  EPT_VIOLATION         15000 (25.0%)
  PREEMPTION_TIMER       3000  (5.0%)
  ...
```

---

## Exercise 4: CPUID 虚拟化双机制对比

### 目标

对比 CPUID Faulting（用户态拦截）和 KVM CPUID 拦截（Guest 内核态拦截）。

**核心问题**：KVM 如何过滤 CPUID？Guest 内核执行 CPUID 时，是读物理 CPUID 还是 KVM 模拟的值？

### 连接 annotations.md

- §5「VMX 特性检测」：CPUID 指令总是触发 VM-Exit
- §2「VM-Entry/Exit 汇编路径」：CPUID 是 VM-Exit 的来源之一

### 步骤

#### 4.1 运行 ex4-cpuid-analysis

```bash
./ex4-cpuid-analysis
```

程序会：
- 遍历 CPUID leaf 0-1F，记录每次执行
- 对比 KVM 过滤前后的值
- 标注哪些 leaf 被修改（如 leaf 1 的 VMX 位）

#### 4.2 在宿主侧追踪 CPUID 拦截

```bash
# 宿主侧
TRACEFS=/sys/kernel/tracing
echo kvm:kvm_cpuid > $TRACEFS/set_event
echo $(pgrep -f qemu-system) > $TRACEFS/set_event_pid
echo 1 > $TRACEFS/tracing_on

# Guest 侧运行 ex4-cpuid-analysis

# 宿主侧收集数据
echo 0 > $TRACEFS/tracing_on
cat $TRACEFS/trace | grep kvm_cpuid | wc -l
```

#### 4.3 对比 CPUID Faulting

运行 ex2-cpuid-fault：

```bash
./ex2-cpuid-fault
```

对比：
- **CPUID Faulting**：用户态（Ring 3）执行 CPUID 触发 #GP，内核态不受影响
- **KVM CPUID 拦截**：Guest 内核态（Ring 0）执行 CPUID 触发 VM-Exit，KVM 返回过滤后的值

### 思考题

1. **为什么 Guest 内核执行 CPUID 也要触发 VM-Exit？**
   - Guest 内核可能读 CPUID 来检测特性（如 VMX、EPT）
   - KVM 必须过滤这些特性，避免 Guest 看到真实硬件能力
   - 对照 `handle_cpuid()` 和 `kvm_emulate_cpuid()`

2. **CPUID Faulting 和 KVM CPUID 拦截的区别是什么？**
   - CPUID Faulting：阻止用户态探测，安全特性
   - KVM 拦截：过滤返回值，虚拟化特性
   - 两者可以同时启用

3. **哪些 CPUID leaf 会被 KVM 修改？**
   - Leaf 1: VMX 位、APIC ID
   - Leaf 7: 特性标志（如 INVPCID、SMAP）
   - Leaf B: 拓扑信息
   - 对照 `kvm_get_cpuid()` 实现

---

## Exercise 5: vCPU 调度与 Halt-Polling 观测

### 目标

观测 halt-polling 的行为，理解延迟 vs CPU 占用的权衡。

**核心问题**：halt-polling 窗口多大会让 CPU 占用显著上升？收益在哪里？

### 连接 annotations.md

- phase0 `annotations.md` §7「halt-polling」：三阶段算法
- phase0 `README.md`「halt-polling 调优」：实测结论

### 步骤

#### 5.1 追踪 vCPU 调度

```bash
sudo ./trace-vcpu-sched.sh -p $(pgrep -f qemu-system) -d 10
```

脚本会用 `perf sched` 追踪 vCPU 线程的调度行为。

#### 5.2 调整 halt_poll_ns 参数

```bash
# 查看当前值（默认 200000 ns = 200 μs）
cat /sys/module/kvm/parameters/halt_poll_ns

# 四档对比
for v in 0 200000 400000 1000000; do
    echo "$v" > /sys/module/kvm/parameters/halt_poll_ns
    echo "=== halt_poll_ns = $v ns ==="
    # 运行工作负载，测量延迟和 CPU 占用
done

# 恢复原值
echo 200000 > /sys/module/kvm/parameters/halt_poll_ns
```

#### 5.3 分析 halt-polling 实现

读 `virt/kvm/kvm_main.c:3811` — `kvm_vcpu_halt()`：

```c
/* 来源: virt/kvm/kvm_main.c:3811 */
void kvm_vcpu_halt(struct kvm_vcpu *vcpu)
{
    unsigned int max_halt_poll_ns = kvm_vcpu_max_halt_poll_ns(vcpu);
    bool do_halt_poll = halt_poll_allowed && vcpu->halt_poll_ns;

    if (do_halt_poll) {
        /* Phase 1: 忙等 */
        do {
            if (kvm_vcpu_check_block(vcpu) < 0)
                goto out;  /* 有事件 → 立即恢复 */
            cpu_relax();
        } while (kvm_vcpu_can_poll(cur, stop));
    }

    /* Phase 2: 真正阻塞 */
    waited = kvm_vcpu_block(vcpu);

    /* Phase 3: 自适应调整 */
    if (!vcpu_valid_wakeup(vcpu))
        shrink_halt_poll_ns(vcpu);  /* 无效唤醒 → 缩小窗口 */
    else if (halt_ns > max_halt_poll_ns)
        shrink_halt_poll_ns(vcpu);  /* 远超窗口 → 缩小 */
    else if (halt_ns < max_halt_poll_ns)
        grow_halt_poll_ns(vcpu);    /* 稍超窗口 → 增大 */
}
```

### 思考题

1. **为什么 idle VM 调大 halt_poll_ns 会显著增加 CPU 占用？**
   - idle VM 频繁 halt，polling 窗口内没有事件
   - 忙等消耗 CPU，但没有收益（唤醒源是定时器，时间确定）
   - 对照 phase0 README.md 的实测结论

2. **halt-polling 的收益在哪里？**
   - 唤醒源随机且大概率落在窗口内时
   - 如网络包到达、中断触发
   - 对照 phase0 README.md「陷阱1」

3. **为什么窗口超过某个值后收益饱和？**
   - 窗口盖住了典型 halt 时间后，再大也无法让唤醒更早
   - 对照 phase9-performance/index.md §1.2 的实测数据

---

## 提交检查清单

完成练习后，确认：

- [ ] 读过 `../annotations.md` 对应章节
- [ ] 查过 Linux 内核源码（文件路径 + 行号）
- [ ] 查过 Intel SDM（章节号）
- [ ] 思考题已回答
- [ ] 输出数据已记录
- [ ] 未重犯 `CLAUDE.md` 已知陷阱

---

## 参考

- `../annotations.md` — 源码精读
- `../../scripts/trace/` — 宿主侧追踪脚本
- Intel SDM Vol 3, Chapter 24-31 — VMX 架构
- Linux 6.12.93 源码 — `arch/x86/kvm/vmx/`
