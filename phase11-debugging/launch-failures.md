# VM 启动失败诊断

> VMM 调用 `KVM_CREATE_VM` / `KVM_CREATE_VCPU` / `KVM_RUN` 失败时的诊断方法。

---

## 📋 概述

VM 启动过程分为三步，每步可能失败：

| 步骤 | ioctl | 常见错误 |
|------|-------|---------|
| 1. 创建 VM | `KVM_CREATE_VM` | `-ENOMEM`、`-EINVAL`、`-EBUSY`、`-EIO` |
| 2. 创建 vCPU | `KVM_CREATE_VCPU` | `-EINVAL`（超限/重复）、`-ENOMEM`、`-EEXIST` |
| 3. 运行 vCPU | `KVM_RUN` | `KVM_EXIT_FAIL_ENTRY`、`KVM_EXIT_INTERNAL_ERROR`、`KVM_EXIT_SHUTDOWN` |

本文覆盖每一步的错误路径、诊断方法、常见根因。

---

## 1. KVM_CREATE_VM 失败

### 1.1 调用链

```
ioctl(fd, KVM_CREATE_VM, type)
    ↓
kvm_dev_ioctl()                          ← virt/kvm/kvm_main.c:5535
    └─ case KVM_CREATE_VM: → kvm_dev_ioctl_create_vm(arg)  ← kvm_main.c:5546
        ├─ get_unused_fd_flags(O_CLOEXEC)  ← kvm_main.c:5499
        ├─ kvm_create_vm(type, fdname)     ← kvm_main.c:1146-1287
        │   ├─ kvm_arch_alloc_vm()         ← kvm_main.c:1148   (-ENOMEM)
        │   ├─ kvm_arch_init_vm()          ← kvm_main.c:1224  (x86.c:12803)
        │   │   ├─ 检查 type                ← x86.c:12808      (-EINVAL)
        │   │   └─ kvm_page_track_init()    ← x86.c:12819      (-ENOMEM)
        │   ├─ kvm_enable_virtualization() ← kvm_main.c:1228  (kvm_main.c:5693)
        │   │   ├─ 系统关机中？              ← kvm_main.c:5722  (-EBUSY)
        │   │   └─ cpuhp_setup_state → VMXON ← kvm_main.c:5611 (-EIO)
        │   └─ ... (MMU notifier, debugfs 等)
        └─ anon_inode_getfile("kvm-vm")    ← kvm_main.c:5511
```

### 1.2 错误码分析

| errno | 触发位置 | 根因 |
|-------|---------|------|
| `-ENOMEM` | `kvm_main.c:1152` | `kvm_arch_alloc_vm()` 返回 NULL — 内存不足 |
| `-ENOMEM` | `kvm_main.c:1188` | `init_srcu_struct()` 失败 |
| `-ENOMEM` | `kvm_main.c:1217-1221` | `kvm_io_bus` kzalloc 失败 |
| `-EINVAL` | `x86.c:12808` | 不支持的 VM type（只接受 `KVM_X86_DEFAULT_VM` 和 `KVM_X86_SW_PROTECTED_VM`） |
| `-ENOMEM` | `x86.c:12819` | `kvm_page_track_init()` 失败 |
| `-EBUSY` | `kvm_main.c:5722-5724` | 系统正在关机。★ 判据是 `system_state == SYSTEM_HALT \|\| SYSTEM_POWER_OFF \|\| SYSTEM_RESTART`，内核里**没有 `SYSTEM_HALTING` 这个枚举值**（`include/linux/kernel.h:183-192`） |
| `-EIO` | `kvm_main.c:5611` | VMXON 在某个 CPU 上失败 — VT-x 未启用或 BIOS 禁用 |

### 1.3 诊断方法

**Step 1：检查 KVM 模块**

```bash
# 模块是否加载
lsmod | grep kvm
# 应看到 kvm 和 kvm_intel（或 kvm_amd）

# 如果 kvm_intel 加载失败，检查 dmesg
dmesg | grep -i 'kvm\|vmx'
# 常见错误: "VMXON failed" → BIOS 未启用 VT-x
```

**Step 2：检查 /dev/kvm**

```bash
# 设备节点存在吗
ls -l /dev/kvm
# 预期: crw-rw-rw- 1 root kvm ... /dev/kvm

# 权限正确吗
groups  # 当前用户是否在 kvm 组
# 如果不在: sudo usermod -aG kvm $USER
```

**Step 3：strace 跟踪 ioctl**

```bash
sudo strace -e trace=ioctl -p $(pgrep qemu) 2>&1 | grep KVM_CREATE_VM
# 或新建 VM 时
sudo strace -e trace=ioctl qemu-system-x86_64 -enable-kvm ... 2>&1 | grep KVM_CREATE_VM
```

**Step 4：检查系统状态**

```bash
# 系统是否在关机：★ 没有 /proc/sys/kernel/system_state 这个文件
#（内核没把它注册成 sysctl；本机实测 No such file or directory）。
# system_state 是内核内部变量（include/linux/kernel.h:183-192），用户态只能间接判断：
dmesg | tail -20 | grep -i 'reboot\|halt\|power off'   # 是否已进入关机流程
cat /proc/loadavg                                      # 辅助判断系统是否还在正常调度

# 检查 VT-x 是否启用
grep -o 'vmx' /proc/cpuinfo | head -1
# 如果有 vmx → CPU 支持 VT-x
# 如果没有 → BIOS 禁用或 CPU 不支持

# 检查 VMXON 是否成功
dmesg | grep -i 'vmxon'
```

### 1.4 VMXON 失败（-EIO）

`kvm_enable_virtualization()` 在每个 CPU 上调用 `kvm_enable_virtualization_cpu()`（`kvm_main.c:5611`），后者调用 `kvm_arch_enable_virtualization_cpu()` 执行 VMXON。

**常见原因**：
- BIOS 禁用 VT-x → 进 BIOS 启用 Intel Virtualization Technology
- VT-x 被 hypervisor 占用 → 检查是否运行在嵌套虚拟化环境
- CPU 不支持 VMX → `grep vmx /proc/cpuinfo` 无输出

---

## 2. KVM_CREATE_VCPU 失败

### 2.1 调用链

```
ioctl(vm_fd, KVM_CREATE_VCPU, id)
    ↓
kvm_vm_ioctl()                           ← virt/kvm/kvm_main.c:5160
    └─ case KVM_CREATE_VCPU: → kvm_vm_ioctl_create_vcpu(kvm, arg)  ← kvm_main.c:4217
        ├─ id >= KVM_MAX_VCPU_IDS ?        ← kvm_main.c:4232  (-EINVAL)
        ├─ kvm->created_vcpus >= max ?     ← kvm_main.c:4236  (-EINVAL)
        ├─ kvm_arch_vcpu_precreate()       ← kvm_main.c:4241  (x86.c:12349)
        │   ├─ id >= max_vcpu_ids ?         ← x86.c:12358    (-EINVAL)
        │   └─ vmx_vcpu_precreate()        ← x86.c:12361    (-ENOMEM)
        ├─ kmem_cache_zalloc(kvm_vcpu_cache) ← kvm_main.c:4250  (-ENOMEM)
        ├─ alloc_page(vcpu->run)           ← kvm_main.c:4257  (-ENOMEM)
        ├─ kvm_arch_vcpu_create()          ← kvm_main.c:4266  (x86.c:12364)
        │   ├─ kvm_mmu_create()            ← x86.c:12380    (-ENOMEM)
        │   ├─ kvm_create_lapic()          ← x86.c:12384    (-ENOMEM)
        │   └─ vmx_vcpu_create()           ← x86.c:12434    (vmx.c:7514)
        │       ├─ allocate_vpid()          ← vmx.c:7527     (-ENOMEM)
        │       ├─ alloc_page(PML)          ← vmx.c:7536     (-ENOMEM)
        │       └─ alloc_loaded_vmcs()      ← vmx.c:7554     (-ENOMEM)
        │           └─ alloc_vmcs()         ← vmx.c:2947     (-ENOMEM)
        ├─ kvm_dirty_ring_alloc()          ← kvm_main.c:4271
        └─ kvm_get_vcpu_by_id(duplicate) ? ← kvm_main.c:4285  (-EEXIST)
```

### 2.2 限制常量

```c
/* 来源: arch/x86/include/asm/kvm_host.h:49-54（KVM_MAX_VCPUS）与 :65-66（比例/ID 上限）
 * 注意这两组 define 不相邻，不能写成 :51-53 一段。 */
#ifdef CONFIG_KVM_MAX_NR_VCPUS
#define KVM_MAX_VCPUS CONFIG_KVM_MAX_NR_VCPUS      /* :51 */
#else
#define KVM_MAX_VCPUS 1024                         /* :53，KVM=n 时的占位 */
#endif

#define KVM_VCPU_ID_RATIO 4                        /* :65 */
#define KVM_MAX_VCPU_IDS (KVM_MAX_VCPUS * KVM_VCPU_ID_RATIO)  /* :66 */
```

`CONFIG_KVM_MAX_NR_VCPUS` 的 Kconfig 默认值是 **1024**（`MAXSMP` 时为 4096），
范围 1024-4096（`arch/x86/kvm/Kconfig`，`config KVM_MAX_NR_VCPUS`）。
也就是说默认 `KVM_MAX_VCPU_IDS = 1024 * 4 = 4096`。

### 2.3 错误码分析

| errno | 触发位置 | 根因 |
|-------|---------|------|
| `-EINVAL` | `kvm_main.c:4232` | vCPU ID >= `KVM_MAX_VCPU_IDS` |
| `-EINVAL` | `kvm_main.c:4236` | 已创建 vCPU 数 >= `max_vcpus`（默认 1024） |
| `-EINVAL` | `x86.c:12358` | ID >= `kvm->arch.max_vcpu_ids`（可通过 `KVM_CAP_MAX_VCPU_ID` 调整） |
| `-EEXIST` | `kvm_main.c:4285` | 重复的 vCPU ID |
| `-ENOMEM` | `kvm_main.c:4250` | vCPU 结构体分配失败 |
| `-ENOMEM` | `kvm_main.c:4257` | `vcpu->run` 页分配失败 |
| `-ENOMEM` | `vmx.c:2947` | VMCS 分配失败 — VMXON region 不可用或内存不足 |
| `-ENOMEM` | `vmx.c:7527` | VPID 耗尽 — 已创建过多 vCPU |
| `-ENOMEM` | `x86.c:12381-12435` | MMU / LAPIC / FPU / emulate context 分配失败 |

### 2.4 诊断方法

**Step 1：检查 vCPU 数量**

```bash
# QEMU 启动时指定了多少 vCPU
ps aux | grep qemu | grep -o '\-smp [0-9]*'

# KVM 已创建多少 vCPU
ls /sys/kernel/debug/kvm/*/vcpu* | wc -l
```

**Step 2：检查 vCPU ID 重复**

```bash
# strace 跟踪
sudo strace -e trace=ioctl -p $(pgrep qemu) 2>&1 | grep KVM_CREATE_VCPU
# 看 id 参数是否重复
```

**Step 3：检查内存**

```bash
# 每个 vCPU 需要约 1MB（vcpu 结构 + run 页 + VMCS + LAPIC + MMU）
free -h
# 如果内存不足，减少 vCPU 数量
```

---

## 3. KVM_RUN 失败

`KVM_RUN` 失败有三种 KVM_EXIT 原因：

| exit_reason | 含义 | 触发位置 |
|-------------|------|---------|
| `KVM_EXIT_FAIL_ENTRY` | VM-Entry 失败 | `vmx.c:6508-6514`, `:6517-6524` |
| `KVM_EXIT_INTERNAL_ERROR` | KVM 内部错误 | `vmx.c:6542-6553`, `:6606-6611`, `:5283-5290` |
| `KVM_EXIT_SHUTDOWN` | Triple fault | `x86.c:10850-10859`, `vmx.c:5394-5399` |

详细诊断见 `vcpu-exit-diagnosis.md`。

### 3.1 KVM_EXIT_FAIL_ENTRY

**触发条件**：

1. **`failed_vmentry`**（`vmx.c:6508-6514`）：CPU 的 VM-entry 检查失败（guest 状态无效、控制字段错误等）。`hardware_entry_failure_reason` 包含完整的 exit-reason 字段。

2. **`vmx->fail`**（`vmx.c:6517-6524`）：`VMLAUNCH`/`VMRESUME` 指令本身失败（VMCS 指针错误、host 状态无效等）。`hardware_entry_failure_reason` 来自 `VM_INSTRUCTION_ERROR` VMCS 字段。

**诊断**：

```bash
# 启用 VMCS dump
echo 1 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs

# 重启 VM，检查 dmesg
dmesg | grep -A30 'VMCS'
```

`dump_vmcs()`（`vmx.c:6239-6430`）在 `dump_invalid_vmcs=1` 时打印：
- Guest 状态：CR0/CR3/CR4、RSP/RIP/RFLAGS、所有段选择子、GDTR/IDTR、EFER
- Host 状态：RIP/RSP、段选择子、CR0/CR3/CR4
- 控制字段：CPU-based controls、VM-entry/exit controls、exception bitmap

**常见根因**：
- Guest CR0/CR4 配置错误（如 real mode 下 PE=0 但 VM-entry 要求 protected mode）
- 段选择子无效（如 CS.selector 未对齐）
- VM-entry control 字段错误（如 `entry_ctls` 的 `IA-32e mode guest` 位与 guest 模式不匹配）

### 3.2 KVM_EXIT_INTERNAL_ERROR

详细分析见 `vcpu-exit-diagnosis.md` §2。

### 3.3 KVM_EXIT_SHUTDOWN

详细分析见 `vcpu-exit-diagnosis.md` §3。

---

## 4. dump_invalid_vmcs 模块参数

### 4.1 定义

```c
/* arch/x86/kvm/vmx/vmx.c:133-134 */
static bool __read_mostly dump_invalid_vmcs = 0;
module_param(dump_invalid_vmcs, bool, 0644);
```

默认**关闭**。运行时可启用：
```bash
echo 1 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs
```

### 4.2 门控

```c
/* arch/x86/kvm/vmx/vmx.c:6248-6251 */
static void dump_vmcs(struct kvm_vcpu *vcpu)
{
    if (!dump_invalid_vmcs) {
        pr_warn_ratelimited("set kvm_intel.dump_invalid_vmcs=1 to dump internal KVM state.\n");
        return;
    }
    // ... 打印 VMCS ...
}
```

### 4.3 调用点

| 位置 | 触发条件 |
|------|---------|
| `vmx.c:6509` | `exit_reason.failed_vmentry`（CPU VM-entry 检查失败） |
| `vmx.c:6518` | `vmx->fail`（VMLAUNCH/VMRESUME 指令失败） |
| `vmx.c:6605` | `unexpected_vmexit`（未知退出原因） |

### 4.4 输出内容

启用后，`dump_vmcs` 通过 `pr_err` 打印（可从 `dmesg` 查看）。
**下面的标签逐条照 `vmx.c` 的格式串抄**，grep 时请照这些字符串写，
别照旧文档里那些"看着像"的名字（`host_mask` / `ProcBased` / `Exit reason` 都印不出来）：

```
VMCS <ptr>, last attempted VM-entry on CPU <n>       ← vmx.c:6269
*** Guest State ***                                   ← vmx.c:6271
CR0: actual=0x..., shadow=0x..., gh_mask=0x...        ← vmx.c:6272（★ 是 gh_mask，不是 host_mask）
CR4: actual=0x..., shadow=0x..., gh_mask=0x...        ← vmx.c:6274
CR3 = 0x...                                           ← vmx.c:6276
RSP = 0x...  RIP = 0x...                              ← vmx.c:6284
RFLAGS=0x...         DR7 = 0x...                      ← vmx.c:6286
CS:   sel=0x..., attr=0x..., limit=0x..., base=0x...  ← vmx_dump_sel(), vmx.c:6213-6214
GDTR: limit=0x..., base=0x...                         ← vmx_dump_dtsel(), vmx.c:6222-6223
EFER= 0x...                                           ← vmx.c:6303（Guest 段）
*** Host State ***                                    ← vmx.c:6335
RIP = 0x...  RSP = 0x...
CS=... SS=... DS=... ES=... FS=... GS=... TR=...
CR0=... CR3=... CR4=...
EFER= 0x...                                           ← vmx.c:6356（Host 段）
*** Control State ***                                 ← vmx.c:6366
CPUBased=0x... SecondaryExec=0x... TertiaryExec=0x... ← ★ 不是 ProcBased/Proc2Based
PinBased=0x... EntryControls=... ExitControls=...     ← ★ 不是 Entry=/Exit=
ExceptionBitmap=... PFECmask=... PFECmatch=...        ← ★ 不是 "Exception bitmap="
VMEntry: intr_info=... errcode=... ilen=...
VMExit: intr_info=... errcode=... ilen=...
        reason=... qualification=...                  ← ★ 只有数字，不印 EXIT_REASON_* 符号名
IDTVectoring: info=... errcode=...
TSC Offset = 0x...
```

> ★ 想按符号名看退出原因，`dump_vmcs` 里没有；要么用 `kvm:kvm_exit` tracepoint
> （`TP_printk` 才翻译成符号名，见 `annotations.md` §1），要么自己拿
> `reason=0x...` 的低 16 位去查 `arch/x86/include/uapi/asm/vmx.h:32-95`。

---

## 5. ftrace 诊断启动失败

### 5.1 跟踪 ioctl 链路

```bash
TRACEFS=/sys/kernel/debug/tracing
: > $TRACEFS/set_event
echo function > $TRACEFS/current_tracer
{
  echo kvm_dev_ioctl
  echo kvm_create_vm
  echo kvm_vm_ioctl
  echo kvm_vcpu_ioctl
  echo kvm_arch_vcpu_ioctl_run
  echo vcpu_enter_guest
  echo vmx_vcpu_run
} > $TRACEFS/set_ftrace_filter
echo 1 > $TRACEFS/tracing_on

# 启动 VM
qemu-system-x86_64 -enable-kvm ...

# 查看 trace
cat $TRACEFS/trace | grep -E 'kvm_(dev_|vm_|vcpu_)ioctl' | head -20
```

### 5.2 跟踪 VM-Entry/Exit

```bash
echo kvm:kvm_entry >> $TRACEFS/set_event
echo kvm:kvm_exit >> $TRACEFS/set_event
echo kvm:kvm_inj_exception >> $TRACEFS/set_event
echo kvm:kvm_userspace_exit >> $TRACEFS/set_event

# 查看事件
cat $TRACEFS/trace | grep -E 'kvm_(entry|exit)' | head -30
```

### 5.3 分析 exit_reason

```bash
# kvm_exit 的 exit_reason 在 trace 文本里是符号名（见 corrections.md C6）
cat $TRACEFS/trace | grep 'kvm_exit' | \
    grep -oP 'reason \w+' | sort | uniq -c | sort -rn | head -10
```

---

## 6. 常见启动失败场景

### 6.1 /dev/kvm 不存在

**症状**：`ioctl(-1, KVM_CREATE_VM, 0) = -1 ENODEV`

**根因**：KVM 模块未加载或 BIOS 禁用 VT-x

**排查**：
```bash
lsmod | grep kvm          # 模块是否加载
grep vmx /proc/cpuinfo    # CPU 是否支持 VT-x
dmesg | grep -i vmx       # VMXON 是否成功
```

### 6.2 VM-Entry 失败（KVM_EXIT_FAIL_ENTRY）

**症状**：QEMU stderr 打印 `KVM: entry failed, hardware error 0x...`

> ★ 旧文档写的是 `KVM_RUN failed: Argument list too long`，这不对：
> `KVM_EXIT_FAIL_ENTRY` 是经 `run->exit_reason` 上报的**正常 ioctl 返回**
> （`vmx.c:6510-6515` 里 ioctl 返回 0），不是 ioctl 失败，
> 也就不会有 `strerror(-E2BIG)` = "Argument list too long"。
> QEMU 侧的打印在 `target/i386/kvm/kvm.c:6096-6099`
> （`fprintf(stderr, "KVM: entry failed, hardware error 0x%" PRIx64 "\n", ...)`），
> 且当 reason 是 `VMX_INVALID_GUEST_STATE` 时还会补一段 unrestricted-mode 提示。

**根因**：Guest 状态无效（CR0/CR4/段选择子错误）

**排查**：
```bash
echo 1 > /sys/module/kvm_intel/parameters/dump_invalid_vmcs
# 重启 VM，检查 dmesg
dmesg | grep -A30 'Guest State'
```

### 6.3 Triple Fault（KVM_EXIT_SHUTDOWN）

**症状**：VM 启动后立即退出，QEMU 报错 `KVM internal error: suberror 2` 或 `vCPU stopped`

**根因**：Guest 连续异常导致三重故障（IDT 未初始化、异常处理程序触发异常）

**排查**：
```bash
# 跟踪异常注入
echo kvm:kvm_inj_exception >> /sys/kernel/debug/tracing/set_event
# 查看异常链
cat /sys/kernel/debug/tracing/trace | grep inj_exception | tail -20
```

### 6.4 vCPU 数量超限（-EINVAL）

**症状**：`ioctl(vm_fd, KVM_CREATE_VCPU, 1024) = -1 EINVAL`

**根因**：超过 `KVM_MAX_VCPUS`（默认 1024）

**排查**：
```bash
# 检查限制
grep KVM_MAX_VCPUS /root/code/linux-6.12.93/arch/x86/include/asm/kvm_host.h
# 减少 vCPU 数量或重新编译内核提高限制
```

### 6.5 VMXON 失败（-EIO）

**症状**：`ioctl(fd, KVM_CREATE_VM, 0) = -1 EIO`

**根因**：VT-x 未启用（BIOS 禁用或被其他 hypervisor 占用）

**排查**：
```bash
grep vmx /proc/cpuinfo    # 无输出 → CPU 不支持或 BIOS 禁用
dmesg | grep -i vmxon     # "VMXON failed" → 检查 BIOS
```

---

## 📚 参考

- KVM_CREATE_VM 源码：`virt/kvm/kvm_main.c:5492-5533`（`kvm_dev_ioctl_create_vm`）
- kvm_create_vm 源码：`virt/kvm/kvm_main.c:1146-1287`
- kvm_arch_init_vm 源码：`arch/x86/kvm/x86.c:12803-12862`
- KVM_CREATE_VCPU 源码：`virt/kvm/kvm_main.c:4217-4327`
- kvm_arch_vcpu_create 源码：`arch/x86/kvm/x86.c:12364-12443`
- vmx_vcpu_create 源码：`arch/x86/kvm/vmx/vmx.c:7514-7632`
- dump_invalid_vmcs 参数：`arch/x86/kvm/vmx/vmx.c:133-134`
- dump_vmcs 函数：`arch/x86/kvm/vmx/vmx.c:6239-6430`
- KVM_EXIT_FAIL_ENTRY 设置：`arch/x86/kvm/vmx/vmx.c:6508-6524`
- vCPU 退出诊断：`vcpu-exit-diagnosis.md`
- 性能分析：`performance-analysis.md`
- 常见错误：`corrections.md`
