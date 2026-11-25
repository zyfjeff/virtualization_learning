# Phase 2 实践练习 — 理论取证链

> 五段式（COURSE_FRAMEWORK §3.5）：理论 → 预测 → 操作 → 对照 → 反馈。
> 连接 `annotations.md` 源码精读。C 练习 `make` 构建，需 VM（-enable-kvm -cpu host）。

## 📋 取证总览

| 编号 | 验证的理论 | 预测 | 对照判据 |
|------|-----------|------|---------|
| ept_violation_demo | 缺页建表（ept-violation-handling） | 首次访问触发 EPT violation，后续命中不触发 | ftrace kvm_page_fault 计数对照 |
| concurrent_test | 并发原子更新（tdp-mmu-concurrency） | 多 vCPU 并发缺页无 SPTE 损坏 | 并发压测无 corruption |
| memtype_analysis | MMIO/IPAT（mmio-identification） | MMIO 区走 misconfig 模拟 | trace 对照 RAM/MMIO |
| check-ept-ad / read-ept-ad-msr | EPT A/D 硬件支持 | capability MSR 解出 A/D 位 | MSR 解码对照（宿主） |

---

## 📋 练习总览

| Exercise | 主题 | 核心问题 | 连接 annotations.md | 类型 |
|----------|------|---------|---------------------|------|
| 1 | SPTE 位布局深度解码 | SPTE 的 64 位中，哪些是硬件位？哪些是 KVM 软件位？ | §1 SPTE 位布局 | Guest |
| 2 | EPT Violation 完整追踪 | 一次 EPT Violation 从触发到映射完成，经历了哪些步骤？ | §2-5 缺页处理 | 宿主+Guest |
| 3 | MMIO 识别机制分析 | KVM 如何区分 RAM 和 MMIO？IPAT 位如何影响内存类型？ | mmio-identification.md | 宿主+Guest |
| 4 | TDP MMU 并发观测 | 多线程并发访问内存时，TDP MMU 如何保证 SPTE 更新的原子性？ | §7 原子 SPTE 更新 | 宿主+Guest |
| 5 | 内存类型与性能分析 | WB/UC/WT 内存类型对性能有多大影响？ | §6 make_spte() | Guest |

---

## 环境准备

```bash
# 编译练习程序
make

# 启动 VM（建议单独终端，前台运行）
cd ../../scripts/vm && ./boot-vm.sh ubuntu --memory 4G --cpus 4

# 练习程序在 /mnt/shared 中访问（需要先挂载）
sudo mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/shared
cd /mnt/shared
```

---

## Exercise 1: SPTE 位布局深度解码

### 目标

完整解码一个 SPTE 的 64 位，理解每个位的含义。

**核心问题**：SPTE 中哪些位是硬件 EPT 定义的？哪些是 KVM 软件元数据？

### 连接 annotations.md

- §1「SPTE 位布局」：硬件位 vs 软件位
- §1「软件位分布」：为什么 EPT 模式的软件位在高比特（57/58）？

### 步骤

#### 1.1 在 Guest 内运行 ept_violation_demo

```bash
./ept_violation_demo
```

程序会模拟 EPT 页表的建立过程，展示每个 SPTE 的值。

#### 1.2 对照源码解码 SPTE

读 `arch/x86/kvm/mmu/spte.h:18-91`（软件位）+ `arch/x86/kvm/mmu.h:15-17`（`PT_*_MASK`），
理解每个位的含义：

```c
/* 来源: arch/x86/kvm/mmu.h:15-17 —— 注意这三个不在 spte.h 里 */
#define PT_PRESENT_MASK (1ULL << 0)
#define PT_WRITABLE_MASK (1ULL << PT_WRITABLE_SHIFT)   /* = 1 */
#define PT_USER_MASK    (1ULL << PT_USER_SHIFT)        /* = 2 */
```

```c
/* 来源: arch/x86/kvm/mmu/spte.h:39-43 */
#ifdef CONFIG_DYNAMIC_PHYSICAL_MASK
#define SPTE_BASE_ADDR_MASK (physical_mask & ~(u64)(PAGE_SIZE-1))
#else
#define SPTE_BASE_ADDR_MASK (((1ULL << 52) - 1) & ~(u64)(PAGE_SIZE-1))
#endif
```

```c
/* 来源: arch/x86/kvm/mmu/spte.h:90-91 —— EPT 模式的软件可写位在高位 */
#define EPT_SPTE_HOST_WRITABLE		BIT_ULL(57)
#define EPT_SPTE_MMU_WRITABLE		BIT_ULL(58)
```

#### 1.3 用 GDB 查看实际 SPTE

在宿主上：

```bash
# 附加到 QEMU 进程
sudo gdb -p $(pgrep qemu-system)

# 查看 EPT 根页面
(gdb) p/x *(u64*)$rsp  # 根据上下文找到 SPTE 指针

# 解码 SPTE 值（EPT 叶条目视角；注意 bit 2 是 Execute，不是 User！）
# bit 0:     Read
# bit 1:     Write
# bit 2:     Execute（supervisor）—— legacy 页表里这一位才是 User
# bit 5:3    Memory Type（0=UC, 1=WC, 4=WT, 5=WP, 6=WB；其余保留）
# bit 6:     IPAT（Ignore PAT）
# bit 7:     Ignored（SDM Table 29-7；4K 叶条目没有 PS 位）
# bit 8:     Accessed   （仅当 EPTP bit6 = 1）
# bit 9:     Dirty      （仅当 EPTP bit6 = 1）
# bit 10:    Execute for user-mode（仅当 mode-based execute control = 1）
# bit 51:12  物理地址
# bit 53:52  A/D 类型（软件位，00/01/10）
# bit 55:54  access-tracking 保存的 R/X（软件位）
# bit 57:    Host Writable（软件位）
# bit 58:    MMU Writable （软件位）
# bit 63:    Suppress #VE（KVM 把它并入 shadow_present_mask，spte.c:439-440）
```

### 思考题

1. **为什么 EPT 模式的软件位在高比特（57/58），而非低比特（9/10）？**
   - EPT 低位几乎被硬件位占满：bit 0/1/2 (R/W/X)、bit 5:3 (Memory Type)、
     bit 6 (IPAT)、bit 8/9 (A/D)、bit 10 (user-mode execute，MBEC 开启时)。
     **bit 7 在 4K 叶条目里是 "Ignored"**（不是"第二个 IPAT"——旧稿这里写重了；
     只有大页条目里 bit 7 才是 PS=1），可用 ignore 位太少，所以 KVM 把软件位挪到高位。
   - 对照 `spte.h:85-91` 的注释原文：*"Low ignored bits are at a premium for EPT,
     use high ignored bits, taking care to not overlap the A/D type mask or the saved
     access bits of access-tracked SPTEs when A/D bits are disabled."*
     非 EPT（legacy）路径才用低位：`DEFAULT_SPTE_HOST_WRITABLE = BIT_ULL(9)`、
     `DEFAULT_SPTE_MMU_WRITABLE = BIT_ULL(10)`（`spte.h:81-83`，注释
     "Bits 9 and 10 are ignored by all non-EPT PTEs"）。
   - **规范引用**: `intel-vmx.pdf` Vol 3C, Table 29-7 (Format of an EPT Page-Table
     Entry that Maps a 4-KByte Page)

2. **`SPTE_TDP_AD_MASK` 的三种取值分别对应什么场景？**
   - `AD_ENABLED` (0)：默认，硬件维护 A/D
   - `AD_DISABLED` (1)：`role.ad_disabled = !kvm_ad_enabled()`，即 CPU 无 EPT A/D 能力
     或 `kvm_intel.eptad=0`。**与 PML 无关**——PML 反过来要求 A/D 可用
     （`vmx.c:8525-8529`）
   - `AD_WRPROT_ONLY` (2)：`cpu_dirty_log_size && role.guest_mode`，即
     **PML + L2（嵌套）**；不是快照/COW/迁移
   - 对照 `spte.h:32-37`（定义）与 `mmu_internal.h:148-159`（判据）
   - ⚠️ 这三个值住在 **bits 53:52**，不在 bit 9/8 —— 见本仓 `corrections.md` 勘误 4

3. **`shadow_present_mask` 是常量还是变量？**
   - 运行时初始化，EPT 和 shadow paging 取不同值
   - 对照 `spte.h:180`（声明）、`spte.c:37`（定义）

### 输出示例

> **勘误**：旧稿这里给的样例是 `0x8000000123456067`，并声称
> "MemType=6(WB)、IPAT=0、bit57=1、bit58=1"。**该十六进制按位拆开其实完全不同**
> （bits 5:3 = 4 = WT、bit 6 IPAT = 1、bit 57/58 都是 0），数字与解读互相矛盾。
> 下面换成一个自洽的样例，每一位都能反算。

```
SPTE 解码: 0x8600000123456373

反算（每一行都可用 (val >> n) & mask 验证）:
  bit 0   (R):            1  ← 可读
  bit 1   (W):            1  ← 可写
  bit 2   (X):            0  ← 不可执行（EPT 的这一位是 Execute，不是 User）
  bits 5:3(MT):         110 (6) = WB
  bit 6   (IPAT):         1  ← 忽略 Guest PAT，强制用这里的 MT
  bit 7               :   0  ← 4K 叶条目里是 Ignored 位
  bit 8   (A):            1  ← 已被访问（EPTP bit6=1 时硬件维护）
  bit 9   (D):            1
  bits 51:12 (PAddr): 0x123456000  ← 物理地址，PFN = 0x123456
  bits 53:52 (AD type):  00 = SPTE_TDP_AD_ENABLED   ← 软件位，不是 bit 9/8
  bits 55:54          :  00 （未做 access tracking）
  bit 57 (HostWritable):  1  ← KVM 软件位（EPT 模式用高位）
  bit 58 (MMUWritable) :  1  ← KVM 软件位
  bit 63 (SUPPRESS_VE):   1  ← KVM 恒定并入 shadow_present_mask
                             （spte.c:439-440：
                              shadow_present_mask = (has_exec_only ? 0 : VMX_EPT_READABLE_MASK)
                                                    | VMX_EPT_SUPPRESS_VE_BIT）
```

校验用一行代码（宿主任意用户可跑，不需要 VM 在跑）：

```bash
python3 -c 'v=0x8600000123456373; \
print("R",v&1,"W",(v>>1)&1,"X",(v>>2)&1,"MT",(v>>3)&7,"IPAT",(v>>6)&1,"A",(v>>8)&1, \
"D",(v>>9)&1,"pfn",hex((v>>12)&((1<<40)-1)),"ADtype",(v>>52)&3,"hostW",(v>>57)&1,"mmuW",(v>>58)&1,"VE",v>>63)'
```

---

## Exercise 2: EPT Violation 完整追踪

### 目标

追踪一次 EPT Violation 从触发到映射完成的完整流程。

**核心问题**：`kvm_page_fault()` → `kvm_tdp_mmu_map()` → SPTE 安装，中间经历了哪些步骤？

### 连接 annotations.md

- §2「缺页处理入口」：`kvm_mmu_page_fault()` 内部分发
- §3「struct kvm_page_fault」：缺页上下文封装
- §4「TDP 缺页路径」：`kvm_tdp_page_fault()` 选择
- §5「kvm_tdp_mmu_map()」：映射核心

### 步骤

#### 2.1 在宿主启用 ftrace

```bash
# 启用 KVM 页表相关 tracepoint
echo 1 > /sys/kernel/tracing/events/kvm/kvm_page_fault/enable
echo 1 > /sys/kernel/tracing/events/kvm/kvm_mmio/enable

# 清空 trace 缓冲区
echo > /sys/kernel/tracing/trace

# 设置 PID 过滤（使用所有 vCPU 线程）
PID=$(pgrep -f '^qemu-system')
QEMU_TIDS=$(ls /proc/$PID/task/ 2>/dev/null | tr '\n' ' ')
echo "$QEMU_TIDS" > /sys/kernel/tracing/set_event_pid
```

#### 2.2 在 Guest 内触发 EPT Violation

```bash
# 运行演示程序
./ept_violation_demo

# 或者手动触发：分配大内存并访问
python3 -c "
import mmap
m = mmap.mmap(-1, 1024*1024*100)  # 100 MB
m[0] = 1  # 触发首次访问
"
```

#### 2.3 在宿主查看 trace

```bash
# 查看 EPT Violation 处理流程
cat /sys/kernel/tracing/trace | grep -E "kvm_page_fault|kvm_mmu_get_page|kvm_tdp_mmu_map"
```

#### 2.4 对照源码理解流程

读 `arch/x86/kvm/mmu/mmu.c` 和 `arch/x86/kvm/mmu/tdp_mmu.c`：

```c
/* 来源: arch/x86/kvm/mmu/mmu.c — kvm_mmu_page_fault() */
static int kvm_mmu_page_fault(struct kvm_vcpu *vcpu, ...)
{
    // 1. 构造 struct kvm_page_fault
    // 2. 调用 kvm_mmu_do_page_fault()
    // 3. 根据返回值决定是否需要模拟 MMIO
}

/* 来源: arch/x86/kvm/mmu/tdp_mmu.c — kvm_tdp_mmu_map() */
int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    // 1. 遍历 EPT 页表，找到目标 GPA 对应的 SPTE 指针
    // 2. 调用 make_spte() 构造新 SPTE 值
    // 3. 原子写入 SPTE（cmpxchg）
    // 4. 如果成功，返回 RET_PF_FIXED；否则重试
}
```

### 思考题

1. **`kvm_mmu_do_page_fault()` 如何决定走 TDP MMU 还是旧路径？**
   - 检查 `vcpu->arch.mmu->page_fault` 函数指针
   - TDP MMU 启用时指向 `kvm_tdp_mmu_page_fault()`
   - 对照 `annotations.md §4`

2. **`struct kvm_page_fault` 封装了哪些关键信息？**
   - `addr`: Guest 物理地址（GPA）
   - `error_code`: 错误码（写/用户态/保留位等）
   - `max_level` / `req_level` / `goal_level`: 页表级别
   - 对照 `annotations.md §3`

3. **为什么 `kvm_tdp_mmu_map()` 使用原子操作（cmpxchg）而不是锁？**
   - TDP MMU 设计目标：无锁并发
   - 多个 vCPU 可以并发修改不同 SPTE
   - 对照 `annotations.md §7`

### 预期 trace 输出

```
# kvm_page_fault 格式: vcpu %u rip 0x%lx address 0x%016llx error_code 0x%llx
kvm_page_fault: vcpu 0 rip 0x7f1234560100 address 0x00007f1234560000 error_code 0x0000000000000002

# error_code 含义:
# bit 0: P (Present) - 页不存在
# bit 1: W (Write) - 写操作
# bit 2: U (User) - 用户态访问
```

**注意**：KVM 没有 `kvm_mmu_get_page` 和 `kvm_tdp_mmu_map` tracepoints。
要观察完整的页表映射过程，需要使用 ftrace function tracer 或 kprobe：

```bash
# 使用 function tracer（需要 debugfs）
echo function > /sys/kernel/tracing/current_tracer
echo kvm_tdp_mmu_map > /sys/kernel/tracing/set_ftrace_filter
echo 1 > /sys/kernel/tracing/tracing_on
```

---

## Exercise 3: MMIO 识别机制分析

### 目标

理解 KVM 如何区分 RAM 和 MMIO，以及 IPAT 位如何影响内存类型。

**核心问题**：为什么某些 GPA 被识别为 MMIO，而某些是 RAM？

### 连接文档

- `mmio-identification.md` — MMIO 识别完整流程
- `annotations.md §6` — `make_spte()` 内存类型设置

### 步骤

#### 3.1 在 Guest 内运行 memtype_analysis

```bash
./memtype_analysis
```

程序模拟 `kvm_is_mmio_pfn()` 的逻辑，展示如何区分 RAM 和 MMIO。

#### 3.2 在宿主追踪 MMIO 访问

```bash
# 启用 MMIO 相关 tracepoint
echo 1 > /sys/kernel/tracing/events/kvm/kvm_mmio/enable
echo 1 > /sys/kernel/tracing/events/kvm/kvm_page_fault/enable

# 设置 PID 过滤
PID=$(pgrep -f '^qemu-system')
QEMU_TIDS=$(ls /proc/$PID/task/ 2>/dev/null | tr '\n' ' ')
echo "$QEMU_TIDS" > /sys/kernel/tracing/set_event_pid

# 在 Guest 内访问 MMIO 区域（如 GPU BAR）
# 这需要先配置 VFIO 直通，或使用模拟的 MMIO 区域

# 查看 trace 输出
cat /sys/kernel/tracing/trace | grep kvm_mmio
```

#### 3.3 对照源码理解 MMIO 识别

读 `arch/x86/kvm/mmu/spte.c:108` — `kvm_is_mmio_pfn()`：

```c
/* 来源: arch/x86/kvm/mmu/spte.c:108 */
static bool kvm_is_mmio_pfn(kvm_pfn_t pfn)
{
    if (pfn_valid(pfn))
        return !is_zero_pfn(pfn) && PageReserved(pfn_to_page(pfn)) &&
            (!pat_enabled() || pat_pfn_immune_to_uc_mtrr(pfn));

    return !e820__mapped_raw_any(pfn_to_hpa(pfn), E820_TYPE_RAM);
}
```

#### 3.4 理解 IPAT 位的作用

读 `mmio-identification.md` 第 5 章：

- **IPAT = 0**: 使用 Guest 设置的内存类型（PAT）
- **IPAT = 1**: 忽略 Guest PAT，强制使用 UC（MMIO 场景）

### 思考题

1. **为什么 MMIO 区域必须映射为 UC（Uncacheable）？**
   - MMIO 设备寄存器不能缓存，否则读到过期值
   - 硬件要求 MMIO 访问必须直达设备

2. **IPAT 位在什么场景下被设置为 1？**
   - `kvm_is_mmio_pfn()` 返回 true 时
   - `make_spte()` 根据 `pte_access` 和 `pfn` 决定是否设置 IPAT
   - 对照 `mmio-identification.md §5`

3. **VFIO GPU 直通时，GPU BAR 的内存类型是什么？**
   - UC（Uncacheable）
   - IPAT = 1，忽略 Guest PAT
   - 对照 `mmio-identification.md §6`

### 预期输出

```
MMIO 识别结果:
  PFN 0x1000 (RAM):     is_mmio = false, memtype = WB
  PFN 0xF0000 (MMIO):   is_mmio = true,  memtype = UC, IPAT = 1
```

---

## Exercise 4: TDP MMU 并发观测

### 目标

观测多线程并发访问内存时，TDP MMU 如何保证 SPTE 更新的原子性。

**核心问题**：TDP MMU 不使用 mmu_lock 写锁，如何避免竞争？

### 连接 annotations.md

- §7「原子 SPTE 更新」：cmpxchg 机制
- §8「TDP MMU 根页面管理」：引用计数

### 步骤

#### 4.1 在 Guest 内运行并发测试

```bash
./concurrent_test
```

程序创建 4 个线程，每个线程分配 50MB 内存并反复访问。

#### 4.2 在宿主观测并发行为

```bash
# 启用 page fault 追踪
echo 1 > /sys/kernel/tracing/events/kvm/kvm_page_fault/enable

# 设置 PID 过滤
PID=$(pgrep -f '^qemu-system')
QEMU_TIDS=$(ls /proc/$PID/task/ 2>/dev/null | tr '\n' ' ')
echo "$QEMU_TIDS" > /sys/kernel/tracing/set_event_pid

# 观测并发映射
cat /sys/kernel/tracing/trace_pipe | grep kvm_page_fault

# 如果要观察 SPTE 设置，使用 function tracer
echo function > /sys/kernel/tracing/current_tracer
echo kvm_tdp_mmu_map >> /sys/kernel/tracing/set_ftrace_filter
echo 1 > /sys/kernel/tracing/tracing_on
```

#### 4.3 对照源码理解原子更新

读 `arch/x86/kvm/mmu/tdp_mmu.c` — `tdp_mmu_set_spte_atomic()`：

```c
/* 来源: arch/x86/kvm/mmu/tdp_mmu.c */
static int tdp_mmu_set_spte_atomic(struct kvm *kvm,
                                   struct tdp_iter *iter,
                                   u64 new_spte)
{
    u64 *sptep = rcu_dereference(*iter->sptep);
    u64 old_spte = *sptep;

    // 原子比较并交换
    if (cmpxchg64(sptep, old_spte, new_spte) != old_spte) {
        // 竞争失败，返回重试
        return -EBUSY;
    }
    return 0;
}
```

### 思考题

1. **如果两个 vCPU 同时 map 同一个 GPA，会发生什么？**
   - 两个都调用 `cmpxchg64()`
   - 只有一个成功，另一个返回 -EBUSY 重试
   - 重试时会看到已更新的 SPTE，可能直接返回成功

2. **TDP MMU 的根页面引用计数如何管理？**
   - `kvm_tdp_mmu_get_root()` 增加引用
   - `kvm_tdp_mmu_put_root()` 减少引用
   - 引用为 0 时释放根页面
   - 对照 `annotations.md §8`

3. **为什么 TDP MMU 比旧 MMU 并发性能更好？**
   - 旧 MMU：全局 mmu_lock 写锁
   - TDP MMU：原子操作，无锁并发
   - 对照 `tdp-mmu-concurrency.md`

---

## Exercise 5: 内存类型与性能分析

### 目标

测量不同内存类型（WB/UC/WT）对性能的影响。

**核心问题**：MMIO（UC）比 RAM（WB）慢多少？

### 连接 annotations.md

- §6「make_spte()」：内存类型编码
- `mmio-identification.md §5`：IPAT 与内存类型

### 步骤

#### 5.1 在 Guest 内运行内存测试

```bash
./memtest
```

程序测量顺序读写、随机读写的性能。

#### 5.2 对比不同内存类型

```bash
# WB（Write-Back）：正常 RAM
# UC（Uncacheable）：MMIO 区域
# WT（Write-Through）：写穿透

# 使用 perf 测量
perf stat -e cache-misses,cache-references ./memtest
```

#### 5.3 对照源码理解内存类型编码

读 `arch/x86/kvm/mmu/spte.h`：

```c
/* 来源: arch/x86/kvm/mmu/spte.h */
// EPT 内存类型编码（bit 3-5）
// 000 (0) = UC (Uncacheable)
// 001 (1) = WC (Write Combining)
// 100 (4) = WT (Write-Through)
// 101 (5) = WP (Write-Protect)
// 110 (6) = WB (Write-Back)
// 111 (7) = UC- (UC minus)
```

### 预期结果

```
内存类型性能对比:
  WB (RAM):    ~10 GB/s
  WT:          ~5 GB/s
  UC (MMIO):   ~100 MB/s（直接访问设备，无缓存）

缓存未命中率:
  WB:  < 1%
  UC:  ~100%（每次访问都直达内存/设备）
```

### 思考题

1. **为什么 UC 比 WB 慢 100 倍？**
   - WB 利用 L1/L2/L3 缓存
   - UC 每次访问都直达内存或设备
   - 缓存行 64 字节，UC 每次只访问 1 字节

2. **什么场景下必须使用 UC？**
   - MMIO 设备寄存器
   - DMA 缓冲区（避免缓存一致性）
   - 对照 `mmio-identification.md §5`

3. **WC（Write Combining）适用于什么场景？**
   - 帧缓冲区（GPU）
   - 连续写入，不关心读取
   - 合并多次写入为一次突发传输

---

## 统一测试环境

所有练习使用统一的 VM 启动脚本：

- **构建脚本**: `scripts/vm/build-kernel.sh` + `scripts/vm/build-rootfs-ubuntu.sh`
- **启动脚本**: `scripts/vm/boot-vm.sh`
- **详细说明**: 参见 `../../scripts/README.md`

---

## 故障排查

### 问题 1: 无法访问 tracefs

```bash
# 挂载 tracefs
sudo mount -t tracefs none /sys/kernel/tracing
```

### 问题 2: 看不到 EPT Violation

```bash
# 确保 KVM 模块已加载
lsmod | grep kvm

# 确保 tracepoint 已启用
echo 1 > /sys/kernel/tracing/events/kvm/kvm_page_fault/enable

# 在 VM 内执行内存操作触发 EPT Violation
```

### 问题 3: 编译错误

```bash
# 确保安装了 gcc 和 make
sudo apt install build-essential

# 重新编译
make clean
make
```

---

## 附：EPT A/D 诊断工具

两个宿主侧小工具（2026-11-25 自仓库根目录移入，`make` 一并构建）：

| 工具 | 用途 | 运行位置 |
|------|------|---------|
| `check-ept-ad` | 通过 CPUID + MSR 检查 CPU 是否支持 EPT A/D 位 | 宿主（需 VMX） |
| `read-ept-ad-msr` | 通过 `/dev/cpu/0/msr` 读 `IA32_VMX_EPT_VPID_CAP` (0x48C)，解码 EPT A/D 支持位 | 宿主（需 msr 模块） |

```bash
make check-ept-ad read-ept-ad-msr
./check-ept-ad
sudo modprobe msr && sudo ./read-ept-ad-msr
```

---

## 参考资料

- Phase 2 README: `../README.md`
- annotations.md: 源码精读（Q&A 模式）
- EPT 规范: Intel SDM Volume 3C Chapter 28
- KVM 源码: `arch/x86/kvm/mmu/`
- mmio-identification.md: MMIO 识别与 IPAT
