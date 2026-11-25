# vCPU 异常退出诊断

> VM 运行中突然停止、vCPU 异常退出时的诊断方法。

---

## 📋 概述

vCPU 异常退出分为三大类：

1. **Triple fault（KVM_EXIT_SHUTDOWN）** — guest 内连续异常导致 CPU 三重故障
2. **KVM 内部错误（KVM_EXIT_INTERNAL_ERROR）** — KVM 检测到无法处理的异常状态
3. **VM-Entry 失败（KVM_EXIT_FAIL_ENTRY）** — VMCS 配置错误导致无法进入 guest

本文覆盖诊断方法、源码路径、常见原因。

---

## 1. 退出流程概览

```
Guest 执行
    │
    ↓ (VM-Exit)
vmx_vcpu_run()                     ← arch/x86/kvm/vmx/vmx.c:7344
    ├─ trace_kvm_entry()           ← vmx.c:7372
    ├─ __vmx_vcpu_run() (asm)      ← vmx.c:7311 → 捕获 exit_reason
    └─ trace_kvm_exit()            ← vmx.c:7489  ★ kvm_exit tracepoint
    ↓
vcpu_enter_guest()                 ← arch/x86/kvm/x86.c:10777
    ├─ handle_exit_irqoff()
    └─ handle_exit → vmx_handle_exit()  ← vmx.c:6615
        └─ __vmx_handle_exit()           ← vmx.c:6436
            ├─ invalid guest state → handle_invalid_guest_state()
            ├─ failed vmentry → KVM_EXIT_FAIL_ENTRY
            ├─ vectoring_info conflict → KVM_EXIT_INTERNAL_ERROR
            ├─ fastpath check
            └─ kvm_vmx_exit_handlers[exit_reason.basic](vcpu)
                ├─ [2] handle_triple_fault → KVM_EXIT_SHUTDOWN     ← vmx.c:5394
                ├─ [0] handle_exception_nmi → 异常注入 / SIMUL_EX  ← vmx.c:5214
                └─ ... (30+ handlers)
    ↓
vcpu_run() returns ≤ 0
    ↓
kvm_arch_vcpu_ioctl_run() → copy exit_reason to userspace
```

**源码引用**：
- VM-Exit 入口：`arch/x86/kvm/vmx/vmx.c:7344`（`vmx_vcpu_run`）
- kvm_exit tracepoint：`vmx.c:7489`（VMX 侧唯一调用点；SVM 在 `svm.c:4464` 另有一处）
- 退出分发：`vmx.c:6436`（`__vmx_handle_exit`）
- 退出处理表：`vmx.c:6095-6148`（`kvm_vmx_exit_handlers[]`）

---

## 2. KVM_EXIT_INTERNAL_ERROR

### 2.1 触发条件

`KVM_EXIT_INTERNAL_ERROR` 表示 KVM 检测到无法处理的内部状态。子错误码定义在 `include/uapi/linux/kvm.h:189-195`：

| suberror | 值 | 含义 | 触发位置 |
|----------|-----|------|---------|
| `KVM_INTERNAL_ERROR_EMULATION` | 1 | 指令模拟失败 | `x86.c:8812`, `:11969`, `:13835` |
| `KVM_INTERNAL_ERROR_SIMUL_EX` | 2 | 异常交付期间又发生异常 | `vmx.c:5283-5290` |
| `KVM_INTERNAL_ERROR_DELIVERY_EV` | 3 | 事件交付冲突 | `vmx.c:6542-6553` |
| `KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON` | 4 | 未知退出原因 | `vmx.c:6606-6611` |

### 2.2 诊断方法

**Step 1：检查 QEMU 日志**

```bash
# 注意：这条信息打在 **stderr**，不在 `-D` 指定的日志文件里
#（QEMU 的 fprintf(stderr, ...)，accel/kvm/kvm-all.c:2858）
grep -i 'internal error' /tmp/qemu.err
# 实际文本是（大小写/标点照源码）：
#   KVM internal error. Suberror: 3
#   extra data[0]: 0x0000000080000021
#   extra data[1]: 0x0000000000000030
#   ...
# suberror 的取值含义见 §2.1 表（3 = KVM_INTERNAL_ERROR_DELIVERY_EV）；
# QEMU 不会打印 KVM_INTERNAL_ERROR_* 这种符号名。
```

**Step 2：启用 ftrace 跟踪退出链路**

```bash
TRACEFS=/sys/kernel/debug/tracing
: > $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_inj_exception >> $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on

# 触发错误后查看
cat $TRACEFS/trace | grep -E 'kvm_exit|inj_exception' | tail -30
```

**Step 3：分析 exit_reason 和 error_code**

```bash
# kvm_exit 的字段（宏 TRACE_EVENT_KVM_EXIT，arch/x86/kvm/trace.h:297-331）：
# - exit_reason: VM-Exit 原因。★ 在 ftrace 文本里打印的是**符号名**
#   （TP_printk 是 "vcpu %u reason %s%s%s ..."，由 kvm_print_exit_reason()
#    翻译，trace.h:289-296/:325-330）；只有在 bpftrace 里
#    `args->exit_reason` 才是数字。见 corrections.md C6。
# - guest_rip: guest RIP
# - isa: KVM_ISA_VMX / KVM_ISA_SVM
# - intr_info: VM-Exit interruption info
# - error_code: 异常错误码
# - info1: exit qualification
# - info2: idt_vectoring_info
# - vcpu_id: vCPU 编号

# 查看最近的 kvm_exit（文本里 reason 是符号名，grep 'reason \w+' 才对）
cat $TRACEFS/trace | grep 'kvm_exit' | tail -5
```

### 2.3 常见子错误分析

#### KVM_INTERNAL_ERROR_SIMUL_EX（suberror=2）

**含义**：异常交付期间又发生新异常（两个异常碰撞）。

**源码路径**：
```
handle_exception_nmi()              ← vmx.c:5214
  ├─ 检查 idt_vectoring_info 是否有效（表示正在交付异常）
  └─ 如果新异常不是 #PF（无 RSVD 位），→ KVM_EXIT_INTERNAL_ERROR
     ← vmx.c:5281-5290
```

**诊断**：
```bash
# 检查 idt_vectoring_info（kvm_exit 的 info2 字段）
sudo bpftrace -e '
tracepoint:kvm:kvm_exit {
    if (args->exit_reason == 0) {  // EXCEPTION_NMI
        printf("intr_info=0x%x idt_vectoring=0x%x error_code=0x%x\n",
               args->intr_info, args->info2, args->error_code);
    }
}
'
```

**根因**：guest 的 IDT 配置错误，或异常处理程序本身触发异常。

#### KVM_INTERNAL_ERROR_DELIVERY_EV（suberror=3）

**含义**：事件交付冲突 —— `idt_vectoring_info` 有效，但退出原因不是预期的交付时退出。

**源码路径**：
```
__vmx_handle_exit()                 ← vmx.c:6436
  └─ 检查 idt_vectoring_info 有效且退出原因不在预期列表
     → KVM_EXIT_INTERNAL_ERROR      ← vmx.c:6542-6553
```

**预期退出原因**：`EXCEPTION_NMI`, `EPT_VIOLATION`, `PML_FULL`, `APIC_ACCESS`, `TASK_SWITCH`, `NOTIFY`

**诊断**：检查 `idt_vectoring_info` 和 `exit_reason` 的组合。

#### KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON（suberror=4）

**含义**：未知的退出原因 —— `exit_reason.basic >= kvm_vmx_max_exit_handlers` 或对应 handler 为 NULL。

**源码路径**：
```
__vmx_handle_exit() → unexpected_vmexit  ← vmx.c:6602-6612
  └─ vcpu->run->internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON
     data[0] = exit_reason.full
     data[1] = last_vmentry_cpu
```

**诊断**：检查 `exit_reason` 是否是合法的 VMX 退出原因。

### 2.4 启用 dump_invalid_vmcs

```bash
# 运行时可改（0644）
echo 1 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs

# 下次内部错误时，dmesg 会打印 VMCS 状态
dmesg | grep -A20 'VMCS'
```

---

## 3. KVM_EXIT_SHUTDOWN（Triple Fault）

### 3.1 硬件触发的 Triple Fault

CPU 在 guest 执行期间发生三重故障，VM-Exit 原因 `EXIT_REASON_TRIPLE_FAULT`（= 2）。

**处理函数**：
```c
/* arch/x86/kvm/vmx/vmx.c:5394-5399 */
static int handle_triple_fault(struct kvm_vcpu *vcpu)
{
    vcpu->run->exit_reason = KVM_EXIT_SHUTDOWN;
    vcpu->mmio_needed = 0;
    return 0;
}
```

### 3.2 软件合成的 Triple Fault

KVM 自身检测到三重故障条件，设置 `KVM_REQ_TRIPLE_FAULT` 请求：

| 触发位置 | 条件 | 源码 |
|---------|------|------|
| `x86.c:885` | `#DF` 已挂起时又发生新异常 | `kvm_multiple_exception()` |
| `x86.c:5306` | UC 型 #MC 无法交付（`CR4.MCE` 未开 / `MCG_STATUS.MCIP`） | `kvm_inject_mce()` |
| `x86.c:5545` | 用户态用 `KVM_SET_VCPU_EVENTS` 塞入 pending triple fault | `kvm_vcpu_ioctl_x86_set_vcpu_events()` |
| `x86.c:8619` | 指令模拟导致无法交付的故障 | `emulator_triple_fault()` |
| `x86.c:8787` | 实模式 INT 模拟失败 | `kvm_inject_realmode_interrupt()` |

> ★ `vmx.c:6495-6496`（L2 状态无效）**不走 `KVM_REQ_TRIPLE_FAULT`**：
> 那里直接 `nested_vmx_vmexit(vcpu, EXIT_REASON_TRIPLE_FAULT, 0, 0)`
> 把三重故障投递给 L1，本表旧版把它列成"设置请求"是错的。
> 全树 `grep -n KVM_REQ_TRIPLE_FAULT arch/x86/kvm/x86.c` 的写点只有上表 5 处
> （外加 `x86.c:10850/10854` 两个读点）。

**消耗点**：`vcpu_enter_guest()` 检查 `KVM_REQ_TRIPLE_FAULT`（`x86.c:10850-10860`）：
```c
if (kvm_test_request(KVM_REQ_TRIPLE_FAULT, vcpu)) {
    if (is_guest_mode(vcpu))
        kvm_x86_ops.nested_ops->triple_fault(vcpu);  // L2 → 转发给 L1
    if (kvm_check_request(KVM_REQ_TRIPLE_FAULT, vcpu)) {
        vcpu->run->exit_reason = KVM_EXIT_SHUTDOWN;
        vcpu->mmio_needed = 0;
        r = 0;
        goto out;
    }
}
```

### 3.3 诊断 Triple Fault

**Step 1：检查异常注入记录**

```bash
TRACEFS=/sys/kernel/debug/tracing
: > $TRACEFS/set_event
echo kvm:kvm_inj_exception >> $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo 1 > $TRACEFS/tracing_on

# 触发 triple fault 后
cat $TRACEFS/trace | grep 'inj_exception' | tail -20
```

**Step 2：分析异常链**

Triple fault 通常是异常链：
```
#PF（缺页）→ 交付 #PF 时又 #PF → #DF（双重故障）
→ 交付 #DF 时又异常 → Triple Fault → KVM_EXIT_SHUTDOWN
```

```bash
# 查看异常序列：TP_printk 是 "%s%s%s%s%s"，输出形如
#   kvm_inj_exception: #PF (0)   /   kvm_inj_exception: #DF   /   #GP (14)
# 所以取异常名要 grep 符号本身，不能用 awk '{print $NF}'
# （带错误码时 $NF 是 "(0)"，不是异常名），也不能用 'exception=\w+'（没有这个键）
cat $TRACEFS/trace | grep 'inj_exception' | \
    grep -oP '#[A-Z]{2}' | tail -10
# 预期看到连续的 #PF → #DF → shutdown
```

**Step 3：检查 guest IDT**

Triple fault 通常意味着 guest 的 IDT 配置错误：
- IDT 未初始化
- IDT 指向无效地址
- 异常处理程序本身触发异常

**Step 4：检查 guest 状态**

```bash
# 用 ftrace function tracer 跟踪异常注入路径
echo function > $TRACEFS/current_tracer
echo kvm_inject_exception > $TRACEFS/set_ftrace_filter
echo kvm_multiple_exception >> $TRACEFS/set_ftrace_filter

# 重启 VM，观察异常注入序列
```

### 3.4 常见原因

| 原因 | 症状 | 排查 |
|------|------|------|
| Guest IDT 未初始化 | 启动后立即 shutdown | 检查 VMM 是否正确设置 guest IDT |
| Guest 内核 bug | 随机 shutdown | 检查 guest dmesg，启用 guest kdump |
| 实模式中断模拟失败 | 实模式代码触发 | 检查 `kvm_inject_realmode_interrupt`（`x86.c:8787`） |
| 嵌套虚拟化 L2 状态无效 | 嵌套 VM shutdown | 检查 L1 VMM 的 VMCS 配置 |

---

## 4. 高频 VM-Exit 分析

### 4.1 EPT_VIOLATION 过多

```bash
# 统计 EPT 缺页分布
sudo bpftrace -e '
tracepoint:kvm:kvm_page_fault {
    @gpa[args->fault_address >> 21] = count();  // 2MB 对齐
}
interval:s:10 {
    print(@gpa, 20);
    clear(@gpa);
}
'
```

**根因**：
- 大页未启用 → 启用 THP 或 hugetlbfs
- mmu_notifier 频繁失效 → 检查宿主内存管理（THP compaction、page migration）
- 设备直通 DMA 映射问题 → 检查 VFIO IOMMU 配置

### 4.2 EXTERNAL_INTERRUPT 过多

```bash
# 统计外部中断频率
sudo bpftrace -e '
tracepoint:kvm:kvm_exit /args->exit_reason == 1/ {  // 1 = EXTERNAL_INTERRUPT
    @count = count();
}
interval:s:5 {
    printf("EXTERNAL_INTERRUPT: %d/s\n", @count / 5);
    clear(@count);
}
'
```

**根因**：
- APICv / Posted Interrupts 未启用 → 检查 `enable_apicv`（`vmx.c:114`，0444）
- 设备中断风暴 → 检查 guest 设备配置
- IPI 频繁 → 检查 `enable_ipiv`（`vmx.c:117`）

### 4.3 IO_INSTRUCTION 过多

```bash
# 统计端口访问分布
sudo bpftrace -e '
tracepoint:kvm:kvm_pio {
    @ports[args->port] = count();
}
interval:s:10 {
    print(@ports, 10);
    clear(@ports);
}
'
```

**优化**：
- 串口（0x3f8）→ `-serial null` 禁用
- 网卡（e1000）→ 切换到 virtio-net
- 存储（IDE）→ 切换到 virtio-blk

---

## 5. 异常注入路径

### 5.1 异常队列

**核心函数**：`kvm_multiple_exception()`（`x86.c:826-906`）

```
异常发生
    ↓
kvm_queue_exception() / kvm_queue_exception_e()
    ↓
kvm_multiple_exception()
    ├─ 无挂起异常 → 直接队列
    ├─ 前一个异常是 #DF → Triple Fault
    ├─ Contributory + Contributory → 合成 #DF
    └─ 其他 → 替换前一个异常
```

### 5.2 异常注入

**注入函数**：`kvm_inject_exception()`（`x86.c:10284-10301`）

```c
/* arch/x86/kvm/x86.c:10284-10301 */
static void kvm_inject_exception(struct kvm_vcpu *vcpu)
{
    // ... 实模式去掉错误码 ...
    trace_kvm_inj_exception(vector, has_error_code, error_code, injected);
    kvm_x86_call(inject_exception)(vcpu);  // VMX: 写 VM_ENTRY_INTR_INFO
}
```

**调用链**：
```
vcpu_enter_guest() → kvm_check_and_inject_events() → kvm_inject_exception()
```

### 5.3 trace_kvm_inj_exception

**定义**：`arch/x86/kvm/trace.h:372`
**字段**：`exception, has_error, error_code, reinjected`
（`trace.h:377-382` 的 `TP_STRUCT__entry`；宿主
`events/kvm/kvm_inj_exception/format` 实测同此 —— **没有 `vcpu_id`，
没有 `has_error_code`，也没有 `payload`**；调用点传的是
`vcpu->arch.exception.{vector,has_error_code,error_code,injected}`，
落到 trace 里的字段名是 `exception/has_error/...`，见 `x86.c:10295-10298`）
**调用点**：`kvm_inject_exception()`（`x86.c:10295`）

`TP_printk` 是 `"%s%s%s%s%s"`，即直接打印异常符号（`#PF`、`#DF (0)`…），
所以想按异常统计要用 `grep -oP '#[A-Z]{2}'`，**不能用 `exception=\w+`**（文本里没有这个键）。

```bash
# 跟踪异常注入
echo kvm:kvm_inj_exception >> /sys/kernel/debug/tracing/set_event
cat /sys/kernel/debug/tracing/trace_pipe | grep inj_exception
```

---

## 6. 诊断工具汇总

| 工具 | 用途 | 命令 |
|------|------|------|
| QEMU stderr | 查看 KVM_EXIT_* 原因 | 直接看标准错误（不要 `-d kvm`，见下方说明）|
| QEMU trace event | 把 exit 原因写进 `-D` 日志文件 | `-D /tmp/qemu.log -d trace:kvm_run_exit` |
| ftrace kvm_exit | 退出原因分布 | `echo kvm:kvm_exit >> set_event` |
| ftrace kvm_inj_exception | 异常注入记录 | `echo kvm:kvm_inj_exception >> set_event` |
| bpftrace | 按 exit_reason 统计 | `tracepoint:kvm:kvm_exit { @r[args->exit_reason] = count(); }` |
| dump_invalid_vmcs | VMCS 状态导出 | `echo 1 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs` |

> ★ **`-d kvm` 不是合法的 QEMU 日志项**。QEMU 10.1.0-rc2 的
> `qemu_log_items[]`（`util/log.c:476-527`）里没有 `kvm`，未知项会让
> `qemu_str_to_log_mask()` 返回 0，`system/vl.c:2609-2613` 随即打印 usage 并
> `exit(1)` —— VM 根本起不来（本机 QEMU 8.2.2 实测同样 exit 1）。
> KVM 退出信息本来就是直接打 **stderr** 的：
> `accel/kvm/kvm-all.c:2858` `"KVM internal error. Suberror: %d"`、
> `target/i386/kvm/kvm.c:6098` `"KVM: entry failed, hardware error 0x%"PRIx64`。
> 想落进文件有两条路：shell 层 `2> /tmp/qemu.err`，或用 trace event
> `-d trace:kvm_run_exit`（`accel/kvm/kvm-all.c:3242`，`trace:` 前缀由
> `util/log.c:543-545` 特殊处理）。

---

## 📚 参考

- VM-Exit 处理源码：`arch/x86/kvm/vmx/vmx.c:6436`（`__vmx_handle_exit`）
- 退出处理表：`vmx.c:6095-6148`（`kvm_vmx_exit_handlers[]`）
- Triple fault 处理：`vmx.c:5394`（`handle_triple_fault`）
- 异常队列：`x86.c:826`（`kvm_multiple_exception`）
- 异常注入：`x86.c:10284`（`kvm_inject_exception`）
- kvm_exit tracepoint：`trace.h:297-331`（宏 `TRACE_EVENT_KVM_EXIT`），实例化 `trace.h:336`
- KVM_EXIT_* 常量：`include/uapi/linux/kvm.h:189-195`
- 性能分析：`performance-analysis.md`
- 实战案例：`case-studies.md`
- 常见错误：`corrections.md`
