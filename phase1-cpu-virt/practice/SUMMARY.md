# Phase 1 实战总结

> **用途**: 参考模板 — 展示各 Exercise 的预期输出和关键发现
>
> **如何使用**: 完成练习后，对比此文档检查是否遗漏重要观察点
>
> CPU 虚拟化深度练习完成，连接 `annotations.md` 源码精读

---

## 完成的练习

### Exercise 1: VMX Capability 深度解码

**目标**: 完整解码 KVM 读取的 7 个 VMX capability MSR，理解特性依赖链

**关键发现**:
- IA32_VMX_BASIC 的 true-CTLS 位 (bit 55) 决定是否提供更细粒度的控制 MSR
- `adjust_vmx_controls()` 逻辑: `ctl &= allowed1; ctl |= allowed0;`
- EPT 依赖: 4-level (bit 6) + WB (bit 14) + INVEPT all-context (bit 22 or 24)
- APICv 依赖: VIRTUAL_TPR + VIRT_APIC + APIC_REG_VIRT + VIRT_INTR_DELIVERY
- Posted Interrupts 依赖: APICv enabled

**源码对照**:
- `vmx.c:2590` — `setup_vmcs_config()`
- `vmx.c:2563` — `adjust_vmx_controls()`
- `vmxfeatures.h` — VMX_FEATURE 位定义

---

### Exercise 2: MSR Bitmap 可视化与热 MSR 分析

**目标**: 理解 MSR Bitmap 如何减少 VM-Exit

**关键发现**:
- Bitmap 初始全 1 (全部拦截)，运行时按需放开
- 透传 MSR 列表 (`vmx_possible_passthrough_msrs[]`, `vmx.c:171`):
  - IA32_TSC, IA32_SPEC_CTRL, IA32_PRED_CMD, IA32_FLUSH_CMD
  - MSR_FS/GS_BASE, MSR_KERNEL_GS_BASE
  - MSR_IA32_SYSENTER_CS/ESP/EIP
  - C-state residency counters
- Bitmap 布局: 4 个 128 字节区域（共 4KB）
  - 0x000-0x07F: MSR 0x0000-0x1FFF 读拦截
  - 0x080-0x0FF: MSR 0xC0000000-0xC0001FFF 读拦截
  - 0x100-0x17F: MSR 0x0000-0x1FFF 写拦截
  - 0x180-0x1FF: MSR 0xC0000000-0xC0001FFF 写拦截

**性能数据**:
- 透传 MSR (IA32_TSC): ~10 ns
- 拦截 MSR (IA32_EFER): ~1500 ns
- 开销差异: ~150x

---

### Exercise 3: VM-Exit Reason Profiling

**目标**: 分析不同负载的 VM-Exit 分布

**典型分布**:

| 负载类型 | 主要 Exit Reason | 占比 |
|---------|-----------------|------|
| Idle | PREEMPTION_TIMER | ~50% |
| Idle | EXTERNAL_INTERRUPT | ~27% |
| CPU-bound | EPT_VIOLATION | ~83% |
| IO-bound | IO_INSTRUCTION | ~67% |

**快速路径 vs 慢速路径**:
- 快速路径: EXTERNAL_INTERRUPT, PREEMPTION_TIMER, APIC_WRITE, HLT
- 慢速路径: CPUID, IO_INSTRUCTION, EPT_VIOLATION, MSR_READ/WRITE

---

### Exercise 4: CPUID 虚拟化双机制对比

| 机制 | 触发条件 | 触发方式 | 处理方式 |
|------|---------|---------|---------|
| KVM 拦截 | Guest 任何 ring | VM-Exit (硬件) | `kvm_emulate_cpuid()` |
| Faulting | Guest Ring 3 | #GP (软件异常) | 注入 #GP 给 Guest |

- CPUID 在 VMX non-root 下**总是**触发 VM-Exit
- KVM 从 `vcpu->arch.cpuid_entries` 返回过滤后的值 (`cpuid.c:1629`)

---

### Exercise 5: vCPU 调度与 Halt-Polling 观测

**三阶段算法** (`kvm_main.c:3811`):
- Phase 1: 忙等 → 有事件立即恢复
- Phase 2: 阻塞 → schedule() 让出 CPU
- Phase 3: 自适应调整 → grow/shrink

**参数**: `halt_poll_ns=200000` (200μs), grow=2, shrink=2

---

## 项目结构

```
phase1-cpu-virt/practice/
├── README.md                 # 5 个练习说明
├── Makefile                  # 编译脚本
├── SUMMARY.md                # 本文件
├── ex1-vmx-verify.c          # [增强] VMX Capability 完整解码
├── ex2-cpuid-fault.c         # [保留] CPUID Faulting 测试
├── ex3-msr-test.c            # [保留] MSR 访问时间测量
├── ex4-cpuid-analysis.c      # [新增] CPUID 遍历 + 双机制对比
├── ex5-vmexit-overhead.c     # [保留] VM-Exit 开销测量
├── trace-msr-access.sh       # [新增] ftrace MSR 追踪
├── profile-vmexit.sh         # [新增] perf kvm stat 分析
└── trace-vcpu-sched.sh       # [新增] vCPU 调度追踪
```

## 关键学习成果

1. VMX 特性依赖链 — EPT → unrestricted_guest, APICv → PI
2. MSR Bitmap — 透传 vs 拦截 150x 性能差异
3. VM-Exit 分发 — O(1) 查表 + 快速/慢速路径
4. CPUID 虚拟化 — 硬件强制 VM-Exit + KVM 过滤 + Faulting
5. Halt-Polling — 忙等 vs 阻塞权衡 + 自适应算法
