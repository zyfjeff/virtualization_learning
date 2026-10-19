# KVM_CLOCK_REALTIME 深入分析

> 基于 Linux 6.12 源码，分析 KVM_CLOCK_REALTIME 标志的作用、使用场景和陷阱

---

## 📋 前置知识

阅读本文前，建议先掌握：
- ✅ kvmclock 的基本原理（pvclock 共享页、system_time、tsc_timestamp）
- ✅ KVM_GET_CLOCK / KVM_SET_CLOCK 的基本用法
- ✅ VM 快照/恢复的基本流程
- ✅ CLOCK_MONOTONIC vs CLOCK_REALTIME 的区别

---

## 1. 背景问题：VM 迁移中的时间同步难题

### 1.1 问题的本质

VM 热迁移时，需要将 guest 的时间状态从源主机迁移到目标主机。kvmclock 的核心公式是：

```
guest_time = host_tsc * tsc_to_system_mul + system_time
```

其中：
- `host_tsc`：宿主机的 TSC 值
- `tsc_to_system_mul`：TSC 到系统时间的缩放因子
- `system_time`：宿主机的系统时间（CLOCK_MONOTONIC）

**问题**：迁移时，我们需要确保 guest 看到的时间是连续的，不能跳变。

### 1.2 传统方案的局限

在 KVM_CLOCK_REALTIME 引入之前（Linux 5.16 之前），VMM 的典型做法是：

```c
// 源主机：保存时钟状态
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);
// data.clock 包含当前的 kvmclock 值
// data.flags = KVM_CLOCK_TSC_STABLE

// 迁移到目标主机

// 目标主机：恢复时钟状态
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
// 直接恢复 data.clock 的值
```

**问题**：
1. `data.clock` 只是一个绝对时间值，没有时间基准信息
2. 如果迁移过程中有延迟（比如网络传输、目标主机准备），guest 的时间会"停滞"
3. 恢复后，guest 的时间会比实际时间慢（慢了多少 = 迁移耗时）

### 1.3 更严重的问题：单调性破坏

如果 VMM 想"补偿"迁移耗时，可能会这样做：

```c
// 错误做法：手动补偿迁移耗时
data.clock += migration_duration_ns;
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
```

**后果**：
- 如果 `migration_duration_ns` 估计不准确，可能导致 kvmclock **回退**
- kvmclock 回退会破坏 CLOCK_MONOTONIC 的单调性保证
- Guest 内核的调度器、锁超时、 watchdog 等都会出问题

---

## 2. KVM_CLOCK_REALTIME 的解决方案

### 2.1 核心思想

引入两个新的标志和字段，提供**原子时间对**（atomic time pair）：

```c
struct kvm_clock_data {
    __u64 clock;      // kvmclock 当前值
    __u32 flags;      // 标志位
    __u32 pad0;
    __u64 realtime;   // 宿主机的 CLOCK_REALTIME（新增）
    __u64 host_tsc;   // 宿主机的 TSC 值（新增）
    __u32 pad[4];
};
```

新增标志：
- `KVM_CLOCK_REALTIME` (bit 2)：`realtime` 字段有效
- `KVM_CLOCK_HOST_TSC` (bit 3)：`host_tsc` 字段有效

### 2.2 工作原理

#### KVM_GET_CLOCK 的行为

```c
// arch/x86/kvm/x86.c:1661-1694 (简化)
static void __get_kvmclock(struct kvm *kvm, struct kvm_clock_data *data)
{
    data->flags = 0;
    
    if (ka->use_master_clock && /* TSC 稳定 */) {
        struct timespec64 ts;
        
        // 原子读取墙钟和 TSC
        if (kvm_get_walltime_and_clockread(&ts, &data->host_tsc)) {
            // 成功：提供 realtime 和 host_tsc
            data->realtime = ts.tv_nsec + NSEC_PER_SEC * ts.tv_sec;
            data->flags |= KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC;
        } else {
            // 失败：只提供 host_tsc
            data->host_tsc = rdtsc();
        }
        
        data->flags |= KVM_CLOCK_TSC_STABLE;
        // 计算 kvmclock 值
        data->clock = __pvclock_read_cycles(&hv_clock, data->host_tsc);
    } else {
        // TSC 不稳定：只提供 clock 值
        data->clock = get_kvmclock_base_ns() + ka->kvmclock_offset;
    }
}
```

**关键点**：
1. `realtime` 和 `host_tsc` 是**原子读取**的（同一个时刻）
2. 只有在 TSC 稳定时才会设置 `KVM_CLOCK_REALTIME` 标志
3. `realtime` 是宿主机的 `CLOCK_REALTIME`（墙钟）

#### KVM_SET_CLOCK 的行为

```c
// arch/x86/kvm/x86.c:4321-4358 (简化)
static int kvm_vm_ioctl_set_clock(struct kvm *kvm, void __user *argp)
{
    if (copy_from_user(&data, argp, sizeof(data)))
        return -EFAULT;
    
    // 检查标志位
    if (data.flags & ~KVM_CLOCK_VALID_FLAGS)
        return -EINVAL;
    
    // 核心：处理 KVM_CLOCK_REALTIME
    if (data.flags & KVM_CLOCK_REALTIME) {
        u64 now_real_ns = ktime_get_real_ns();
        
        /*
         * 避免 kvmclock 回退！
         * 如果当前真实时间 > 保存时的真实时间
         * 说明迁移过程中有时间流逝
         * 将这段时间加到 kvmclock 上
         */
        if (now_real_ns > data.realtime)
            data.clock += now_real_ns - data.realtime;
        // 否则：不做任何调整（保护单调性）
    }
    
    // 计算并设置 kvmclock_offset
    if (ka->use_master_clock)
        now_raw_ns = ka->master_kernel_ns;
    else
        now_raw_ns = get_kvmclock_base_ns();
    
    ka->kvmclock_offset = data.clock - now_raw_ns;
    
    return 0;
}
```

**关键机制**：

```
场景：快照恢复（快照 -> 恢复，中间经过了 T 秒）

1. 快照时：
   data.clock = 1000s
   data.realtime = 1000000s (宿主墙钟)
   data.flags = KVM_CLOCK_REALTIME | KVM_CLOCK_TSC_STABLE

2. 恢复时（经过了 10 秒）：
   now_real_ns = 1000010s (当前宿主墙钟)
   
3. KVM 检查：
   if (now_real_ns > data.realtime)  // 1000010 > 1000000 ✓
       data.clock += (now_real_ns - data.realtime)  // 1000 + 10 = 1010s
   
4. 最终：
   kvmclock_offset = 1010s - now_raw_ns
   guest 看到的时间 = 1010s（正确补偿了 10 秒的迁移时间）
```

### 2.3 单调性保护

**核心设计**：

```c
if (now_real_ns > data.realtime)
    data.clock += now_real_ns - data.realtime;
```

这个条件判断是**单向的**：
- ✅ 如果时间向前流逝：补偿（`data.clock` 增加）
- ❌ 如果时间向后回退：**不补偿**（`data.clock` 保持不变）

**为什么这样设计？**

假设一个错误场景：
```
快照时：realtime = 1000s
恢复时：realtime = 990s（宿主墙钟被手动调后了 10 秒）

如果不保护：
  data.clock += (990 - 1000) = -10s
  → kvmclock 回退 10 秒！
  → CLOCK_MONOTONIC 被破坏！

有保护：
  if (990 > 1000)  // false
  → 不调整
  → kvmclock 保持不变
  → 单调性得到保护
```

---

## 3. 使用场景

### 3.1 场景一：快照恢复（推荐）

**最佳实践**：

```c
// 保存快照
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);
assert(data.flags & KVM_CLOCK_REALTIME);  // 确保标志有效
save_to_snapshot(&data);

// ... 快照存储 ...

// 恢复快照
load_from_snapshot(&data);

// 关键：恢复时必须保留 KVM_CLOCK_REALTIME 标志
assert(data.flags & KVM_CLOCK_REALTIME);
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
// KVM 会自动补偿快照到恢复之间的时间流逝
```

**优势**：
- 自动补偿迁移耗时
- 保护单调性
- 无需 VMM 手动计算

### 3.2 场景二：热迁移（推荐）

```c
// 源主机
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);
send_to_destination(&data);

// 目标主机
recv_from_source(&data);
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
// KVM 会自动补偿迁移过程中的时间流逝
```

**优势**：
- 迁移对 guest 透明
- guest 看到的 CLOCK_MONOTONIC 不会跳变

### 3.3 场景三：冷启动（不适用）

冷启动时，没有"之前的时间状态"，所以不应该使用 `KVM_CLOCK_REALTIME`：

```c
// 冷启动：初始化 kvmclock
struct kvm_clock_data data = {0};
data.flags = 0;  // 不设置 KVM_CLOCK_REALTIME
data.clock = 0;  // 从 0 开始
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
```

### 3.4 场景四：TSC 不稳定（自动降级）

如果宿主机的 TSC 不稳定，KVM 不会设置 `KVM_CLOCK_REALTIME` 标志：

```c
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);

if (!(data.flags & KVM_CLOCK_REALTIME)) {
    // TSC 不稳定，无法使用 realtime 补偿
    // 只能使用传统的 clock 值恢复
    // 注意：这种方式无法自动补偿迁移耗时
}
```

---

## 4. 副作用和陷阱

### 4.1 陷阱一：Firecracker PR #5809 问题

**问题描述**：

Firecracker 在快照恢复时，直接将保存的 `kvm_clock_data` 原样传给 `KVM_SET_CLOCK`，包括 `flags` 字段。

**错误代码**：

```rust
// Firecracker 错误做法
let saved_data = load_from_snapshot();
// saved_data.flags 包含 KVM_CLOCK_REALTIME
ioctl(vcpu_fd, KVM_SET_CLOCK, &saved_data);
```

**后果**：

```
场景：
1. 快照时：realtime = 1000s, clock = 1000s
2. 快照存储了 10 秒
3. 恢复时：now_real_ns = 1010s

KVM 的行为：
  if (1010 > 1000)  // true
      data.clock += 10  // clock 变成 1010s

结果：
  guest 的 CLOCK_MONOTONIC 凭空向前跳了 10 秒！
  
影响：
  - sched_clock 跳变（kvmclock 被注册为 sched_clock）
  - printk 时间戳乱序
  - 调度统计错误
  - softlockup watchdog 误判
```

**修复方案**：

```rust
// Firecracker 修复后的做法
let saved_data = load_from_snapshot();
saved_data.flags = 0;  // 清除所有标志，包括 KVM_CLOCK_REALTIME
ioctl(vcpu_fd, KVM_SET_CLOCK, &saved_data);
// 现在 KVM 不会自动补偿，VMM 需要自己处理时间连续性
```

**教训**：
- `KVM_CLOCK_REALTIME` 会自动补偿时间流逝
- 如果不想让 KVM 自动补偿，必须清除 `flags`
- 快照恢复时，是否补偿取决于 VMM 的语义需求

### 4.2 陷阱二：误用 KVM_CLOCK_REALTIME 导致时间跳变

**错误场景**：

```c
// 错误：在不需要补偿的场景使用 KVM_CLOCK_REALTIME
struct kvm_clock_data data;
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);

// 做一些与时间无关的操作（比如暂停 VM 进行调试）
sleep(10);  // 暂停了 10 秒

// 恢复时错误地使用了 KVM_CLOCK_REALTIME
data.flags |= KVM_CLOCK_REALTIME;  // 错误！
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
// 结果：guest 时间向前跳了 10 秒！
```

**正确做法**：

```c
// 如果只是暂停/恢复，不需要补偿时间
data.flags = 0;  // 不使用 KVM_CLOCK_REALTIME
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
```

### 4.3 陷阱三：忽略 KVM_CLOCK_TSC_STABLE 标志

**问题**：

```c
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);

// 错误：没有检查 TSC 稳定性
if (data.flags & KVM_CLOCK_REALTIME) {
    // 假设 realtime 总是有效
    use_realtime(data.realtime);
}
```

**正确做法**：

```c
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);

// 正确：先检查 TSC 稳定性
if (data.flags & KVM_CLOCK_TSC_STABLE) {
    // TSC 稳定，realtime 才有效
    if (data.flags & KVM_CLOCK_REALTIME) {
        use_realtime(data.realtime);
    }
} else {
    // TSC 不稳定，降级处理
    fallback_to_monotonic();
}
```

### 4.4 陷阱四：手动修改 clock 值后仍使用 KVM_CLOCK_REALTIME

**错误场景**：

```c
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);

// 手动调整 clock 值
data.clock += 100;  // 手动增加 100ns

// 错误：仍然使用 KVM_CLOCK_REALTIME
data.flags |= KVM_CLOCK_REALTIME;
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);

// 结果：KVM 会在手动调整的基础上再自动补偿
// 最终时间 = 手动调整 + 自动补偿
// 可能不符合预期
```

**正确做法**：

```c
// 方案 1：手动调整，不使用 KVM_CLOCK_REALTIME
data.clock += 100;
data.flags = 0;  // 清除 KVM_CLOCK_REALTIME
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);

// 方案 2：不手动调整，让 KVM 自动补偿
// 不做任何修改，直接恢复
ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
```

---

## 5. 最佳实践

### 5.1 快照/恢复的标准流程

```c
// 保存快照
struct kvm_clock_data data;
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);

// 验证标志
if (!(data.flags & KVM_CLOCK_TSC_STABLE)) {
    // TSC 不稳定，警告用户
    log_warning("TSC unstable, snapshot may have time issues");
}

// 保存到快照文件
save_to_snapshot(&data, sizeof(data));

// ... 快照存储 ...

// 恢复快照
struct kvm_clock_data data;
load_from_snapshot(&data, sizeof(data));

// 决策：是否使用 KVM_CLOCK_REALTIME？
if (want_monotonic_clock) {
    // 想要单调性：清除标志，不自动补偿
    data.flags = 0;
} else if (want_realtime_compensation) {
    // 想要自动补偿迁移耗时：保留标志
    // 确保标志有效
    if (!(data.flags & KVM_CLOCK_REALTIME)) {
        log_error("Snapshot doesn't support realtime compensation");
        data.flags = 0;  // 降级
    }
} else {
    // 其他逻辑
    data.flags = 0;
}

ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
```

### 5.2 热迁移的标准流程

```c
// 源主机
struct kvm_clock_data data;
ioctl(vcpu_fd, KVM_GET_CLOCK, &data);
send_to_destination(&data);

// 目标主机
struct kvm_clock_data data;
recv_from_source(&data);

// 热迁移：总是使用 KVM_CLOCK_REALTIME
// 让 KVM 自动补偿迁移耗时
if (!(data.flags & KVM_CLOCK_REALTIME)) {
    log_warning("Source doesn't support realtime, migration may have time jump");
}

ioctl(vcpu_fd, KVM_SET_CLOCK, &data);
```

### 5.3 检查 KVM 支持

```c
// 检查 KVM 是否支持 KVM_CLOCK_REALTIME
int caps = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_ADJUST_CLOCK);

if (caps & KVM_CLOCK_REALTIME) {
    // KVM 支持 KVM_CLOCK_REALTIME
    printf("KVM_CLOCK_REALTIME supported\n");
} else {
    // KVM 不支持，需要降级处理
    printf("KVM_CLOCK_REALTIME not supported, using fallback\n");
}
```

---

## 6. 与其他技术的对比

### 6.1 KVM_CLOCK_REALTIME vs VMClock

| 特性 | KVM_CLOCK_REALTIME | VMClock |
|------|-------------------|---------|
| 引入时间 | Linux 5.16 (2021) | Linux 7.0 (2026) |
| 主要用途 | 快照/迁移时间补偿 | 精确时间同步 + 误差界 |
| 精度 | 毫秒级（依赖墙钟） | 纳秒级（依赖 TSC） |
| 跨架构 | 仅 x86 | x86 + ARM |
| 标准化 | KVM 专有 | UAPI 标准 |
| 误差界 | 无 | 有 |
| 迁移通知 | 无 | `disruption_marker` |

**结论**：
- `KVM_CLOCK_REALTIME` 是 KVM 特有的迁移辅助机制
- VMClock 是更通用的精确时间同步方案
- 两者可以共存：`KVM_CLOCK_REALTIME` 处理迁移，VMClock 提供精确时间

### 6.2 KVM_CLOCK_REALTIME vs PVCLOCK_GUEST_STOPPED

| 特性 | KVM_CLOCK_REALTIME | PVCLOCK_GUEST_STOPPED |
|------|-------------------|----------------------|
| 作用 | 补偿迁移时间流逝 | 通知 guest 被暂停 |
| 机制 | 调整 kvmclock 值 | 设置 pvclock 标志 |
| 影响 | guest 时间向前跳 | guest 时间不跳变 |
| guest 感知 | 无感知 | 通过 watchdog 感知 |
| 适用场景 | 热迁移 | 快照/暂停/调试 |

**结论**：
- `KVM_CLOCK_REALTIME`：让 guest 时间"看起来"连续（自动补偿）
- `PVCLOCK_GUEST_STOPPED`：告诉 guest "你被暂停过"（显式通知）
- 两者可以结合使用：迁移用 `KVM_CLOCK_REALTIME`，暂停用 `PVCLOCK_GUEST_STOPPED`

---

## 7. 源码分析

### 7.1 关键数据结构

```c
// include/uapi/linux/kvm.h:1085-1099

/* For KVM_CAP_ADJUST_CLOCK */
#define KVM_CLOCK_TSC_STABLE    (1 << 1)  // bit 1
#define KVM_CLOCK_REALTIME      (1 << 2)  // bit 2
#define KVM_CLOCK_HOST_TSC      (1 << 3)  // bit 3

#define KVM_CLOCK_VALID_FLAGS \
    (KVM_CLOCK_TSC_STABLE | KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC)

struct kvm_clock_data {
    __u64 clock;      // kvmclock 当前值
    __u32 flags;      // 标志位
    __u32 pad0;
    __u64 realtime;   // 宿主机的 CLOCK_REALTIME
    __u64 host_tsc;   // 宿主机的 TSC 值
    __u32 pad[4];
};
```

### 7.2 关键函数

```c
// arch/x86/kvm/x86.c

// 获取时钟（提供 realtime 和 host_tsc）
static void __get_kvmclock(struct kvm *kvm, struct kvm_clock_data *data)
{
    // 原子读取墙钟和 TSC
    if (kvm_get_walltime_and_clockread(&ts, &data->host_tsc)) {
        data->realtime = ts.tv_nsec + NSEC_PER_SEC * ts.tv_sec;
        data->flags |= KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC;
    }
}

// 设置时钟（处理 KVM_CLOCK_REALTIME 补偿）
static int kvm_vm_ioctl_set_clock(struct kvm *kvm, void __user *argp)
{
    if (data.flags & KVM_CLOCK_REALTIME) {
        u64 now_real_ns = ktime_get_real_ns();
        
        // 单向补偿：只向前，不回退
        if (now_real_ns > data.realtime)
            data.clock += now_real_ns - data.realtime;
    }
    
    ka->kvmclock_offset = data.clock - now_raw_ns;
}
```

---

## 8. 总结

### 8.1 KVM_CLOCK_REALTIME 的核心价值

1. **自动补偿迁移耗时**：VMM 无需手动计算
2. **保护单调性**：单向补偿，防止 kvmclock 回退
3. **简化 VMM 逻辑**：只需保留 flags，KVM 自动处理

### 8.2 使用原则

1. **快照/恢复**：根据语义需求决定是否使用
   - 想要单调性：清除 flags
   - 想要自动补偿：保留 flags
2. **热迁移**：总是使用 `KVM_CLOCK_REALTIME`
3. **冷启动**：不使用 `KVM_CLOCK_REALTIME`
4. **TSC 不稳定**：自动降级，无法使用

### 8.3 常见错误

1. ❌ 不信任快照中的 flags（Firecracker 错误）
2. ❌ 在不需要补偿的场景使用 `KVM_CLOCK_REALTIME`
3. ❌ 忽略 `KVM_CLOCK_TSC_STABLE` 标志
4. ❌ 手动修改 clock 后仍使用 `KVM_CLOCK_REALTIME`

### 8.4 检查清单

- [ ] 理解 `KVM_CLOCK_REALTIME` 的作用（自动补偿迁移耗时）
- [ ] 理解单调性保护机制（单向补偿）
- [ ] 检查 KVM 是否支持（`KVM_CHECK_EXTENSION`）
- [ ] 检查 TSC 稳定性（`KVM_CLOCK_TSC_STABLE`）
- [ ] 根据场景选择合适的 flags
- [ ] 避免 Firecracker PR #5809 的错误

---

## 📚 参考资料

- Linux 内核提交：`c68dc1b577ea` ("KVM: x86: Report host tsc and realtime values in KVM_GET_CLOCK")
- Linux 源码：`arch/x86/kvm/x86.c:1661-1694` (`__get_kvmclock`)
- Linux 源码：`arch/x86/kvm/x86.c:4321-4358` (`kvm_vm_ioctl_set_clock`)
- Linux 源码：`include/uapi/linux/kvm.h:1085-1099` (数据结构定义)
- Firecracker PR #5809：修复快照恢复时的时间跳变问题
- KVM API 文档：`Documentation/virt/kvm/api.rst`
