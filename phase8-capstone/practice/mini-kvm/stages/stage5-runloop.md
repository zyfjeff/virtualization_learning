# Stage 5: 运行循环与优化

> 对应课程 Phase 0, 9: KVM Framework + Performance Optimization
>
> 关键源码: `arch/x86/kvm/x86.c::vcpu_run()`
>           `arch/x86/kvm/x86.c::vcpu_enter_guest()`
>           `virt/kvm/kvm_main.c::kvm_vcpu_halt()`

---

## 🎯 阶段目标

实现完整的 vCPU 运行循环：
- vcpu_run() 主循环
- Exit 分发和处理
- halt-polling 优化 (简化版)
- 返回用户空间的场景

## 📖 核心概念

### vcpu_run() 主循环

```
vcpu_run()
  └→ for (;;) {
       │
       ├── vcpu_enter_guest()
       │   └→ 处理 KVM_REQ_* 请求
       │   └→ 注入事件 (中断/异常)
       │   └→ VMENTER → Guest 执行 → VM-Exit
       │   └→ handle_exit() 分发
       │
       ├── if (r <= 0) break  // 返回用户空间或错误
       │
       ├── 检查信号 (signal_pending)
       │
       └── 继续循环
     }
```

### Exit 快速路径 vs 慢速路径

```
快速路径 (内核态直接处理, 重新进入 Guest):
  - EXTERNAL_INTERRUPT: 注入中断
  - HLT: 进入 halt 状态
  - EPT_VIOLATION: 建立映射
  - CPUID: 模拟返回

慢速路径 (返回用户空间, 由 VMM 处理):
  - IO_INSTRUCTION: QEMU 模拟
  - MSR_READ/WRITE: 用户空间处理
  - MMIO: 设备模型处理
  - SHUTDOWN: Triple fault
```

### halt-polling (简化版)

```c
/*
 * 当 Guest 执行 HLT 时:
 *   1. 短暂 polling (忙等 halt_poll_ns)
 *   2. 如果唤醒 → 快速返回 (无调度开销)
 *   3. 如果超时 → 真正阻塞 (schedule)
 */
if (mp_state == HALTED) {
    /* Phase 1: polling */
    start = ktime_get();
    while (ktime_get() - start < halt_poll_ns) {
        if (runnable(vcpu))
            return 0;  /* 唤醒 */
        cpu_relax();
    }
    /* Phase 2: 阻塞 */
    kvm_vcpu_block(vcpu);  /* schedule() */
}
```

## 🔧 mini-kvm.c 实现

### vcpu_run() 主循环

```c
int mini_kvm_vcpu_run_loop(struct mini_kvm_vcpu *vcpu)
{
    int ret;

    vcpu->running = true;

    while (vcpu->running) {
        /* 进入 Guest */
        ret = mini_kvm_vcpu_run(vcpu);

        /* 处理返回值 */
        if (ret <= 0)
            break;

        /* 检查信号 */
        if (signal_pending(current)) {
            ret = -EINTR;
            break;
        }

        /* 继续循环 */
    }

    vcpu->running = false;
    return ret;
}
```

### Exit 分发

```c
int mini_kvm_handle_exit(struct mini_kvm_vcpu *vcpu)
{
    switch (vcpu->exit_reason) {
    case EXIT_REASON_HLT:
        /* Stage 8 优化: 加入 halt-polling */
        vcpu->num_hlt_exits++;
        return MINI_KVM_EXIT_TO_USERSPACE;

    case EXIT_REASON_CPUID:
        /* 模拟 CPUID */
        vcpu->arch.regs[0] = 0;
        vcpu->arch.rip += 2;
        return MINI_KVM_EXIT_RESUME_GUEST;

    case EXIT_REASON_IO_INSTRUCTION:
        /* Stage 4: 设备模拟 */
        return mini_kvm_handle_io(vcpu, ...);

    case EXIT_REASON_EPT_VIOLATION:
        /* Stage 2: EPT 映射 */
        return mini_kvm_ept_handle_violation(vcpu);

    default:
        return MINI_KVM_EXIT_TO_USERSPACE;
    }
}
```

## 🔑 关键差异: mini-kvm vs 真实 KVM

| 特性 | mini-kvm | 真实 KVM |
|------|----------|---------|
| 运行循环 | 简单 for 循环 | 复杂状态机 |
| 请求处理 | 无 | KVM_REQ_* 请求 |
| 事件注入 | 无 | 完整事件队列 |
| halt-polling | 无 | 自适应 polling |
| Exit 快速路径 | 无 | REENTER_GUEST 优化 |
| 抢占通知 | 无 | preempt_notifier |

## 🧪 实验验证

运行完整测试：

```bash
# 构建并加载
make
sudo rmmod kvm_intel kvm 2>/dev/null
sudo insmod mini-kvm.ko

# 运行测试
sudo ./test-mini-kvm

# 查看完整日志
dmesg | grep mini-kvm

# 预期完整输出:
# mini-kvm: === Stage 1: VMX 初始化 ===
# mini-kvm:   ✓ CPU 支持 VMX
# mini-kvm:   ✓ VMXON 执行成功
# mini-kvm: === Stage 2: EPT 初始化 ===
# mini-kvm:   ✓ EPT Pointer 配置
# mini-kvm: === Stage 1: 配置 vCPU 0 的 VMCS ===
# mini-kvm:   ✓ VMCS 加载完成
# mini-kvm:   ✓ Guest 状态配置完成
# mini-kvm: !!! VM-Exit 发生 !!!
# mini-kvm:   Exit reason: 10 (CPUID)
# mini-kvm:   → CPUID (模拟返回 0)
# mini-kvm: !!! VM-Exit 发生 !!!
# mini-kvm:   Exit reason: 30 (IO)
# mini-kvm:   Guest says: H
# ...
# mini-kvm:   Exit reason: 12 (HLT)
# mini-kvm:   → HLT (Guest 停止)
```

## 📝 检查清单

- [ ] 描述 vcpu_run() 的主循环结构
- [ ] 区分 Exit 的快速路径和慢速路径
- [ ] 解释 halt-polling 的自适应算法
- [ ] 理解何时需要返回用户空间
- [ ] 对比 mini-kvm 和真实 KVM 的运行循环

## 🎉 项目完成

完成所有 5 个 Stage 后：
- ✓ 理解了 VMX 的基本操作
- ✓ 实现了 EPT 内存虚拟化
- ✓ 模拟了简单设备
- ✓ 构建了完整的 vCPU 运行循环

## 🔗 扩展方向

1. **添加 MSR 处理**: 实现 RDMSR/WRMSR 模拟
2. **添加第二个串口**: COM2 (0x2f8)
3. **实现中断控制器**: 简化版 LAPIC
4. **添加多 vCPU 支持**: 多线程 + 锁
5. **性能优化**: halt-polling, VPID, 大页 EPT
6. **与真实 KVM 对比**: 性能、功能差异分析
