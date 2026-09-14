# Phase 2 实践练习

> 内存虚拟化（EPT）深度练习，连接 `annotations.md` 源码精读

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

读 `arch/x86/kvm/mmu/spte.h:18-91`，理解每个位的含义：

```c
/* 来源: arch/x86/kvm/mmu/spte.h */
#define PT_PRESENT_MASK         (1ULL << 0)     /* 存在位 */
#define PT_WRITABLE_MASK        (1ULL << 1)     /* 可写位 */
#define PT_USER_MASK            (1ULL << 2)     /* 用户态位 */
#define SPTE_BASE_ADDR_MASK     /* bits 51:12，物理地址 */

// 软件位（KVM 元数据）
#define EPT_SPTE_HOST_WRITABLE  (1ULL << 57)    /* 宿主认为可写 */
#define EPT_SPTE_MMU_WRITABLE   (1ULL << 58)    /* KVM MMU 认为可写 */
```

#### 1.3 用 GDB 查看实际 SPTE

在宿主上：

```bash
# 附加到 QEMU 进程
sudo gdb -p $(pgrep qemu-system)

# 查看 EPT 根页面
(gdb) p/x *(u64*)$rsp  # 根据上下文找到 SPTE 指针

# 解码 SPTE 值
# bit 0: Present
# bit 1: Writable
# bit 2: User
# bit 3-5: Memory Type (6=WB, 0=UC)
# bit 6: IPAT
# bits 12-51: Physical Address
# bit 57: Host Writable (软件位)
# bit 58: MMU Writable (软件位)
```

### 思考题

1. **为什么 EPT 模式的软件位在高比特（57/58），而非低比特（9/10）？**
   - EPT 低位几乎被硬件位占满：bit 0/1/2 (R/W/X)、bit 3:5 (Memory Type)、bit 6 (IPAT)、bit 7 (Ignore PAT)、bit 8/9 (A/D)
   - 对照 `spte.h:54-56` 的注释

2. **`SPTE_TDP_AD_MASK` 的三种取值分别对应什么场景？**
   - `AD_ENABLED` (0): 默认，硬件 A/D 位启用
   - `AD_DISABLED` (1): 硬件不支持 A/D，或嵌套虚拟化 L2 使用 PML
   - `AD_WRPROT_ONLY` (2): 仅写保护跟踪
   - 对照 `spte.h:33-36`

3. **`shadow_present_mask` 是常量还是变量？**
   - 运行时初始化，EPT 和 shadow paging 取不同值
   - 对照 `spte.h:180`（声明）、`spte.c:37`（定义）

### 输出示例

```
SPTE 解码: 0x8000000123456067

位布局:
  bit 0 (Present):     1  ← 页存在
  bit 1 (Writable):    1  ← 可写
  bit 2 (User):        1  ← 用户态可访问
  bit 3-5 (MemType):   110 (6) = WB
  bit 6 (IPAT):        0  ← 使用 PAT
  bit 7 (Ignore PAT):  0
  bits 12-51 (PAddr):  0x123456  ← 物理页帧号
  bit 57 (Host Writable): 1  ← KVM 软件位
  bit 58 (MMU Writable):  1  ← KVM 软件位
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

## 参考资料

- Phase 2 README: `../README.md`
- annotations.md: 源码精读（Q&A 模式）
- EPT 规范: Intel SDM Volume 3C Chapter 28
- KVM 源码: `arch/x86/kvm/mmu/`
- mmio-identification.md: MMIO 识别与 IPAT
