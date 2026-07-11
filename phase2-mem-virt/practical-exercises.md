# Phase 2 实战练习指南

> 动手实践，深入理解 EPT 内存虚拟化

---

## 实战 1：使用 ftrace 跟踪 EPT Violation

### 目标

观察真实虚拟机中的 EPT Violation 处理过程

### 步骤

#### 1. 准备环境

```bash
# 确保 debugfs 已挂载
mount -t debugfs none /sys/kernel/debug

# 检查 ftrace 是否可用
cat /sys/kernel/debug/tracing/available_tracers | grep function
```

#### 2. 设置跟踪点

```bash
# 清空之前的跟踪配置
echo > /sys/kernel/debug/tracing/set_ftrace_filter
echo > /sys/kernel/debug/tracing/trace

# 设置要跟踪的函数
echo kvm_handle_page_fault > /sys/kernel/debug/tracing/set_ftrace_filter
echo kvm_tdp_page_fault >> /sys/kernel/debug/tracing/set_ftrace_filter
echo kvm_tdp_mmu_map >> /sys/kernel/debug/tracing/set_ftrace_filter
echo make_spte >> /sys/kernel/debug/tracing/set_ftrace_filter

# 设置跟踪器为 function
echo function > /sys/kernel/debug/tracing/current_tracer

# 开启跟踪
echo 1 > /sys/kernel/debug/tracing/tracing_on
```

#### 3. 运行虚拟机

```bash
# 启动一个简单的虚拟机
cd /root/code/kvm-study/scripts/testing
./boot-vm.sh

# 在虚拟机中执行内存密集操作
# 例如：分配大量内存
stress-ng --vm 4 --vm-bytes 1G --timeout 10s
```

#### 4. 查看跟踪结果

```bash
# 关闭跟踪
echo 0 > /sys/kernel/debug/tracing/tracing_on

# 查看结果
cat /sys/kernel/debug/tracing/trace | head -100
```

#### 5. 分析结果

寻找以下模式：

```
kvm_handle_page_fault
  ↓
kvm_tdp_page_fault
  ↓
kvm_tdp_mmu_map
  ↓
make_spte
```

### 思考题

1. 一次 EPT Violation 调用了哪些函数？
2. 每个函数大约花费多少时间？
3. 有没有观察到快速路径（fast_page_fault）？

---

## 实战 2：使用 perf 分析 EPT 性能

### 目标

量化 EPT Violation 的性能开销

### 步骤

#### 1. 使用 perf record

```bash
# 记录 KVM 页错误事件
perf record -e kvm:kvm_page_fault -a -g -- sleep 10

# 查看报告
perf report
```

#### 2. 分析热点函数

```bash
# 查看调用图
perf report --stdio --sort=dso,symbol

# 查找 KVM 相关函数
perf report --stdio | grep kvm
```

#### 3. 使用 perf stat

```bash
# 统计 KVM 事件
perf stat -e kvm:* -a sleep 10
```

### 思考题

1. 每秒发生多少次 EPT Violation？
2. 哪个函数占用 CPU 时间最多？
3. 如何优化 EPT 性能？

---

## 实战 3：观察脏页跟踪

### 目标

观察脏页跟踪的实际工作过程

### 步骤

#### 1. 启动虚拟机并开启脏页跟踪

```bash
# 编写一个简单的测试程序
cat > test_dirty_tracking.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

int main() {
    int kvm_fd = open("/dev/kvm", O_RDWR);
    int vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    
    // 分配内存
    void *mem = malloc(4096);
    
    // 设置内存区域
    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x1000,
        .memory_size = 4096,
        .userspace_addr = (unsigned long)mem,
        .flags = KVM_MEM_LOG_DIRTY_PAGES,  // 开启脏页跟踪
    };
    ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region);
    
    // 获取脏页日志
    struct kvm_dirty_log log = {
        .slot = 0,
        .dirty_bitmap = calloc(1, 4096 / 8),
    };
    ioctl(vm_fd, KVM_GET_DIRTY_LOG, &log);
    
    // 检查脏页
    if (log.dirty_bitmap[0] & 1) {
        printf("Page 0x1000 is dirty!\n");
    }
    
    return 0;
}
EOF

gcc -o test_dirty_tracking test_dirty_tracking.c
./test_dirty_tracking
```

#### 2. 使用 QEMU 测试

```bash
# 启动 QEMU 并开启脏页跟踪
qemu-system-x86_64 \
    -enable-kvm \
    -m 512 \
    -monitor telnet::45454,server,nowait \
    -daemonize

# 在 QEMU monitor 中查看脏页
echo "info dirty" | nc localhost 45454
```

### 思考题

1. 脏页跟踪的开销有多大？
2. 如何优化脏页跟踪性能？
3. 硬件 A/D 位和软件写保护的区别是什么？

---

## 实战 4：内存类型性能测试

### 目标

测量 WB 和 UC 内存类型的性能差异

### 步骤

#### 1. 编写测试程序

```bash
cat > memtype_benchmark.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIZE (100 * 1024 * 1024)  // 100MB

void benchmark_memory(volatile char *ptr, size_t size, const char *type) {
    struct timespec start, end;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // 写入内存
    for (size_t i = 0; i < size; i += 4096) {
        ptr[i] = 0x55;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;
    
    printf("%s: %.2f ms (%.2f MB/s)\n", 
           type, elapsed, size / elapsed / 1000.0);
}

int main() {
    // 分配普通内存（WB）
    char *wb_mem = malloc(SIZE);
    benchmark_memory(wb_mem, SIZE, "WB (Write-Back)");
    
    // 分配 UC 内存（使用 mmap）
    char *uc_mem = mmap(NULL, SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_UNCACHED,
                        -1, 0);
    if (uc_mem != MAP_FAILED) {
        benchmark_memory(uc_mem, SIZE, "UC (Uncacheable)");
        munmap(uc_mem, SIZE);
    }
    
    free(wb_mem);
    return 0;
}
EOF

gcc -O2 -o memtype_benchmark memtype_benchmark.c
./memtype_benchmark
```

#### 2. 分析结果

预期结果：
- WB 内存：~10-50 ms（缓存命中）
- UC 内存：~1000-5000 ms（无缓存）

**性能差距：100-500 倍！**

### 思考题

1. 为什么 WB 和 UC 性能差距这么大？
2. 在实际虚拟机中，哪些内存是 UC 的？
3. 如何优化 MMIO 访问性能？

---

## 实战 5：EPT 大页测试

### 目标

观察大页（2M/1G）对性能的影响

### 步骤

#### 1. 配置大页

```bash
# 分配 2M 大页
echo 1024 > /proc/sys/vm/nr_hugepages

# 检查大页状态
grep Huge /proc/meminfo

# 挂载 hugepages
mount -t hugetlbfs none /dev/hugepages
```

#### 2. 使用大页启动虚拟机

```bash
qemu-system-x86_64 \
    -enable-kvm \
    -m 2G \
    -mem-path /dev/hugepages \
    -mem-prealloc \
    -kernel /path/to/vmlinuz \
    -append "root=/dev/sda1 console=ttyS0"
```

#### 3. 在虚拟机中测试性能

```bash
# 在虚拟机中编译运行内存测试程序
gcc -O2 -o memtest memtest.c
./memtest
```

#### 4. 在宿主机观察

```bash
# 使用 perf 观察大页使用情况
perf stat -e page_faults -a sleep 10

# 查看 EPT 页表
cat /sys/kernel/debug/kvm/*/ept
```

### 思考题

1. 大页减少了多少 EPT Violation？
2. 性能提升了多少？
3. 什么场景适合使用大页？

---

## 实战 6：MMIO 设备模拟

### 目标

理解 MMIO 设备的处理流程

### 步骤

#### 1. 创建简单的 MMIO 设备

```bash
cat > mmio_device.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>

int main() {
    // 打开 PCI 设备（例如网卡）
    int fd = open("/sys/bus/pci/devices/0000:00:03.0/resource0", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    
    // 映射 MMIO 区域
    void *mmio = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (mmio == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    
    // 读取设备寄存器（UC 内存）
    volatile unsigned int *reg = (unsigned int *)mmio;
    printf("Device ID: 0x%x\n", reg[0]);
    
    // 写入设备寄存器
    reg[1] = 0x12345678;
    
    munmap(mmio, 4096);
    close(fd);
    return 0;
}
EOF

gcc -o mmio_device mmio_device.c
sudo ./mmio_device
```

#### 2. 使用 ftrace 观察

```bash
# 跟踪 MMIO 处理
echo handle_mmio_page_fault > /sys/kernel/debug/tracing/set_ftrace_filter
echo 1 > /sys/kernel/debug/tracing/tracing_on

./mmio_device

echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace
```

### 思考题

1. MMIO 访问和普通内存访问有什么区别？
2. 为什么 MMIO 需要特殊处理？
3. 如何优化 MMIO 性能？

---

## 实战 7：并发压力测试

### 目标

测试多 vCPU 并发访问的性能

### 步骤

#### 1. 创建多 vCPU 虚拟机

```bash
qemu-system-x86_64 \
    -enable-kvm \
    -smp 4 \
    -m 2G \
    -kernel /path/to/vmlinuz
```

#### 2. 在虚拟机中运行并发测试

```bash
# 在虚拟机中
stress-ng --vm 4 --vm-bytes 512M --timeout 30s
```

#### 3. 在宿主机监控

```bash
# 监控 KVM 统计
watch -n 1 'cat /sys/kernel/debug/kvm/*/vcpu_*'

# 使用 perf 分析
perf record -e kvm:* -a -g -- sleep 30
perf report
```

### 思考题

1. 多 vCPU 并发时性能如何？
2. 有没有锁竞争？
3. 如何优化并发性能？

---

## 实战 8：源码修改练习

### 目标

通过修改源码加深理解

### 练习 1：添加日志

```c
// 在 arch/x86/kvm/mmu/tdp_mmu.c 中添加日志
int kvm_tdp_mmu_map(struct kvm_vcpu *vcpu, struct kvm_page_fault *fault)
{
    // 添加这行
    pr_info("EPT Violation: gfn=%llx, write=%d\n", 
            fault->gfn, fault->write);
    
    // ... 原有代码 ...
}
```

### 练习 2：统计快速路径

```c
// 在 fast_page_fault 中添加统计
static int fast_page_fault(struct kvm_vcpu *vcpu, 
                           struct kvm_page_fault *fault)
{
    static atomic_t fast_path_count = ATOMIC_INIT(0);
    
    // ... 原有代码 ...
    
    if (ret == RET_PF_FIXED) {
        atomic_inc(&fast_path_count);
        if (atomic_read(&fast_path_count) % 1000 == 0) {
            pr_info("Fast path used %d times\n", 
                    atomic_read(&fast_path_count));
        }
    }
    
    return ret;
}
```

### 练习 3：测量延迟

```c
// 添加延迟测量
ktime_t start, end;
start = ktime_get();

// ... EPT 处理代码 ...

end = ktime_get();
pr_info("EPT handling took %lld ns\n", ktime_to_ns(end - start));
```

### 思考题

1. 通过修改源码，你学到了什么？
2. 如何验证你的修改是正确的？
3. 修改对性能有什么影响？

---

## 实战 9：性能调优

### 目标

优化虚拟机的内存性能

### 步骤

#### 1. 基线测试

```bash
# 运行基准测试
./memtest  # 记录基线性能
```

#### 2. 优化 1：启用大页

```bash
echo 1024 > /proc/sys/vm/nr_hugepages
qemu-system-x86_64 -mem-path /dev/hugepages ...

# 再次测试
./memtest  # 对比性能
```

#### 3. 优化 2：调整 EPT 参数

```bash
# 查看当前参数
cat /sys/module/kvm/parameters/*

# 调整参数
echo 1 > /sys/module/kvm/parameters/ept_ad
```

#### 4. 优化 3：NUMA 优化

```bash
numactl --membind=0 qemu-system-x86_64 ...
```

### 思考题

1. 哪个优化效果最明显？
2. 为什么这些优化有效？
3. 还有什么优化方法？

---

## 实战 10：问题诊断

### 目标

诊断和解决内存虚拟化问题

### 场景 1：EPT Violation 频繁

```bash
# 症状：虚拟机性能差
# 诊断
perf record -e kvm:kvm_page_fault -a
perf report

# 解决：启用大页
echo 1024 > /proc/sys/vm/nr_hugepages
```

### 场景 2：内存泄漏

```bash
# 症状：宿主机内存持续增长
# 诊断
cat /proc/meminfo
slabtop

# 检查 KVM 内存使用
cat /sys/kernel/debug/kvm/*/pages
```

### 场景 3：锁竞争

```bash
# 症状：多 vCPU 性能差
# 诊断
perf record -e lock:lock_acquire -a
perf report

# 解决：使用快速路径
# 检查快速路径使用率
grep fast_path /sys/kernel/debug/kvm/*/vcpu_*
```

### 思考题

1. 如何快速定位问题？
2. 哪些工具最有用？
3. 如何预防问题？

---

## 实战总结

### 技能清单

完成这些实战练习后，你应该能够：

- [ ] 使用 ftrace 跟踪 EPT Violation
- [ ] 使用 perf 分析 KVM 性能
- [ ] 观察和测试脏页跟踪
- [ ] 测量内存类型性能差异
- [ ] 配置和使用大页
- [ ] 理解 MMIO 设备处理
- [ ] 测试多 vCPU 并发性能
- [ ] 修改和调试 KVM 源码
- [ ] 优化虚拟机内存性能
- [ ] 诊断和解决内存问题

### 工具清单

| 工具 | 用途 |
|------|------|
| ftrace | 函数调用跟踪 |
| perf | 性能分析 |
| stress-ng | 压力测试 |
| numactl | NUMA 优化 |
| slabtop | 内存 slab 分析 |
| kvm_stat | KVM 统计 |

### 下一步

1. 选择 2-3 个感兴趣的实战练习
2. 动手实践
3. 记录观察结果
4. 总结经验教训

---

**祝实战顺利！** 🚀
