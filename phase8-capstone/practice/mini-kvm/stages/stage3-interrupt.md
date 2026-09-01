# Stage 3: 中断处理

> 对应课程 Phase 4: Interrupts (APIC, Posted Interrupts)
>
> 关键源码: `arch/x86/kvm/lapic.c`
>           `arch/x86/kvm/vmx/posted_intr.c`

---

## 🎯 阶段目标

实现简化的中断注入：
- 理解外部中断如何到达 vCPU
- 通过 VMCS 注入中断到 Guest
- 处理中断相关的 VM-Exit

## 📖 核心概念

### 中断注入路径

```
外部设备中断
    │
    ├── 传统路径:
    │   └→ Host 内核 → KVM → vLAPIC IRR → VM-Entry 注入
    │
    └── Posted Interrupts 路径 (PI):
        └→ IOMMU → PI 描述符 PIR → VM-Entry VID 自动注入
```

### VMCS 中断注入字段

VM-Entry 时，通过 VMCS 的 `VM_ENTRY_INTR_INFO` 字段注入中断：
- Vector: 中断向量号 (0-255)
- Type: 中断类型 (External, NMI, Hardware Exception, etc.)
- Deliver Error Code: 是否传递错误码
- Valid: 是否注入

## 🔧 mini-kvm.c 实现 (简化版)

```c
/* 注入外部中断到 Guest */
int mini_kvm_inject_irq(struct mini_kvm_vcpu *vcpu, int vector)
{
    u32 intr_info;

    /* 构造中断信息 */
    intr_info = vector |                        /* 向量号 */
                INTR_TYPE_EXT_INTR |            /* 外部中断 */
                INTR_INFO_VALID_MASK;           /* 有效 */

    /* 写入 VMCS VM-Entry 中断信息字段 */
    vmcs_write(VMCS_ENTRY_INTR_INFO, intr_info);

    return 0;
}
```

## 🔑 关键差异: mini-kvm vs 真实 KVM

| 特性 | mini-kvm | 真实 KVM |
|------|----------|---------|
| 中断控制器 | 无 | vLAPIC + PIC + IOAPIC |
| 中断路由 | 直接注入 | 复杂路由表 |
| Posted Interrupts | 无 | 完整 PI 实现 |
| 优先级 | 无 | 完整优先级评估 |
| 中断窗口 | 无 | 中断窗口退出 |

## 📝 检查清单

- [ ] 描述中断注入的完整路径
- [ ] 理解 VMCS VM_ENTRY_INTR_INFO 字段格式
- [ ] 对比传统中断路径和 Posted Interrupts
- [ ] 解释为什么需要中断窗口

## 🔗 下一步

Stage 4: 设备模拟
