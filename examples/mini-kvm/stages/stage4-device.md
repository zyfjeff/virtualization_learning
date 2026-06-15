# Stage 4: 设备模拟

> 对应课程 Phase 4-5: vhost + VFIO
>
> 关键源码: `arch/x86/kvm/vmx/vmx.c::handle_io()`
>           `arch/x86/kvm/vmx/vmx.c::handle_ept_violation()` (MMIO)

---

## 🎯 阶段目标

实现简单的设备模拟：
- 处理 IO_INSTRUCTION VM-Exit (PIO 设备)
- 模拟串口设备 (0x3f8)
- 理解 MMIO 处理 (通过 EPT Violation)

## 📖 核心概念

### PIO vs MMIO

```
PIO (Port I/O):
  - 独立地址空间 (x86 IO 端口)
  - in/out 指令触发 IO_INSTRUCTION VM-Exit
  - EXIT_QUALIFICATION 包含端口号、方向、大小

MMIO (Memory Mapped I/O):
  - 映射到内存地址空间
  - 内存读写触发 EPT_VIOLATION VM-Exit
  - EXIT_QUALIFICATION 包含访问的 GPA
```

### 串口模拟

标准 COM1 串口端口: 0x3f8-0x3ff
- 0x3f8 (THR/RBR): 发送/接收缓冲
- 0x3f9 (IER): 中断启用
- 0x3fa (IIR): 中断识别
- 0x3fb (LCR): 线路控制
- 0x3fc (MCR): 调制解调器控制
- 0x3fd (LSR): 线路状态
- 0x3fe (MSR): 调制解调器状态
- 0x3ff (SCR): 暂存

## 🔧 mini-kvm.c 实现

```c
/*
 * 处理 IO 指令 (PIO)
 *
 * EXIT_QUALIFICATION 格式:
 *   [2:0]  = Size - 1 (0=1B, 1=2B, 3=4B)
 *   [3]    = Direction (0=OUT, 1=IN)
 *   [4]    = String instruction
 *   [5]    = REP prefixed
 *   [6]    = Operand size
 *   [15:16]= Port number
 *   [31:16]= Instruction length
 */
int mini_kvm_handle_io(struct mini_kvm_vcpu *vcpu, u16 port,
                       bool is_write, u32 size, u32 value)
{
    if (port == 0x3f8) {  /* COM1: 串口 */
        if (is_write) {
            /* 写 THR: 输出字符 */
            char ch = value & 0xFF;
            kvm->serial_buffer[kvm->serial_pos++] = ch;
            printk(KERN_INFO "Guest says: %c", ch);

            /* 如果输出换行符, 打印整行 */
            if (ch == '\n') {
                kvm->serial_buffer[kvm->serial_pos] = 0;
                printk(KERN_INFO ">>> %s", kvm->serial_buffer);
                kvm->serial_pos = 0;
            }
        } else {
            /* 读 RBR: 返回 0 (无输入) */
            vcpu->arch.regs[0] = 0;
        }
        return MINI_KVM_EXIT_RESUME_GUEST;
    }

    /* 未知端口: 返回到用户空间 */
    return MINI_KVM_EXIT_TO_USERSPACE;
}
```

## 🔑 关键差异: mini-kvm vs 真实 KVM

| 特性 | mini-kvm | 真实 KVM |
|------|----------|---------|
| 设备类型 | 仅串口 | PIC, PIT, LAPIC, IOAPIC, ... |
| PIO 处理 | 简单 switch | 复杂设备模型 |
| MMIO 处理 | 无 | EPT Violation + 用户空间模拟 |
| 设备直通 | 无 | VFIO + IOMMU |
| 虚拟设备 | 无 | virtio + vhost |

## 🧪 实验验证

```bash
# 加载模块并运行 Guest
sudo insmod mini-kvm.ko
# 预期 Guest 输出:
# mini-kvm: Guest says: H
# mini-kvm: Guest says: e
# mini-kvm: Guest says: l
# mini-kvm: Guest says: l
# mini-kvm: Guest says: o
# mini-kvm: Guest says: !
# mini-kvm: >>> Hello!
```

## 📝 检查清单

- [ ] 解释 PIO 和 MMIO 的区别
- [ ] 理解 EXIT_QUALIFICATION 的格式
- [ ] 描述串口设备的寄存器布局
- [ ] 对比 mini-kvm 和 QEMU 的设备模型

## 🔗 下一步

Stage 5: 运行循环与优化
