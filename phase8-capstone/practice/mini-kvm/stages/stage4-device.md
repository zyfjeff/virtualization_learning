# Stage 4: 设备模拟

> 对应课程 Phase 5-6: vhost + VFIO
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

PIO 走独立端口空间，`IN`/`OUT` 触发 `IO_INSTRUCTION` 退出；MMIO 是普通内存
访问，靠 EPT violation 触发。两者"退出信息在哪个 VMCS 字段"的规则不同，
**别记混**：PIO 的端口/方向/大小在 `EXIT_QUALIFICATION`，MMIO 的地址在
`GUEST_PHYSICAL_ADDRESS`。字段的逐位定义见下一节。

## 🔧 实现（`device.c`）

### PIO 退出的解码字段

SDM Vol.3 Table 28-5（Exit Qualification for I/O Instructions）：

```
[2:0]   访问大小：0 = 1 字节，1 = 2 字节，3 = 4 字节（其他值不使用）
[3]     方向：0 = OUT（guest 写），1 = IN（guest 读）
[4]     是否串指令（INS/OUTS）
[5]     是否带 REP 前缀
[6]     操作数编码：0 = DX，1 = 立即数端口
[15:7]  未定义
[31:16] 端口号（来自 DX 或指令里的立即数）
[63:32] 未定义（64 位处理器上存在）
```

指令长度**不在**这个字段里，它是独立的 VMCS 字段
`VM_EXIT_INSTRUCTION_LEN`（编码 `0x440c`）—— IO 退出后 guest RIP 仍停在原
指令上，必须自己加上这个长度才能前进（SDM 28.2.4）。

### MMIO

MMIO 类退出（EPT violation）的 GPA 来自 `GUEST_PHYSICAL_ADDRESS`（`0x2400`），
不是 `EXIT_QUALIFICATION`（那里只有访问性质与权限位，SDM 25.9.1 / Table
28-7）。KVM 自己也是这么取的：`handle_ept_violation()` 里
`gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);`（`vmx.c:5798`）。详见
`stage2-ept.md` 第 3 节。

mini-kvm **没有任何 MMIO 设备**：落在 memslot 外的 GPA 不会被建映射，
`mini_ept_handle_violation()` 直接返回 `-EFAULT`（`ept.c:238-243`），运行循环
报 `KVM_EXIT_INTERNAL_ERROR`。

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

mini-kvm 只碰 **0x3f8 一个端口做字节捕获**，上面那组 16550A 寄存器一个都没
实现：`IN` 一律返回 0，所以 guest 里跑真实串口驱动（要轮询 LSR 的
TX-empty / RX-ready）是跑不通的 —— 完整的 16550A 模型是用户态
`practice/minivmm.c` 那一侧的课题。

### 代码

```c
/* 来源: phase8-capstone/practice/mini-kvm/device.c:59-102（注释略） */
int mini_handle_io_exit(struct mini_kvm_vcpu *vcpu)
{
	u64 qual, rip, len;
	u32 port, size;
	bool is_in;

	mini_vmread(EXIT_QUALIFICATION, &qual);

	if (qual & (1u << 4)) {		/* bit 4 = 串指令，本模块没有模拟器 */
		pr_err("mini-kvm: 不支持串 IO 退出 (qual=0x%llx)，交给用户态\n", qual);
		return -EIO;
	}

	size = (u32)(qual & 7) + 1;		/* bits 2:0 */
	is_in = qual & (1u << 3);		/* bit 3 = 方向 */
	port = (u32)(qual >> 16) & 0xffff;	/* bits 31:16 */

	if (is_in) {
		vcpu->regs[0] = 0;		/* 教学简化：所有 IN 返回 0 */
	} else {
		u32 mask = size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
		u32 val = (u32)vcpu->regs[0] & mask;	/* OUT 的值在 RAX */

		if (port == MINI_KVM_SERIAL_PORT)
			mini_serial_out(vcpu->kvm, (u8)val);
		else
			pr_info_ratelimited("mini-kvm: 忽略 OUT port=0x%x val=0x%x\n",
					    port, val);
	}

	mini_vmread(VM_EXIT_INSTRUCTION_LEN, &len);
	mini_vmread(GUEST_RIP, &rip);
	mini_vmwrite(GUEST_RIP, rip + len);	/* 不推进就会原地反复退出 */

	vcpu->n_io_exits++;
	return 0;
}
```

`size = (qual & 7) + 1`、`in = qual & 8`、`port = qual >> 16` 三个式子与 KVM
的 `handle_io()` 逐字一致（`arch/x86/kvm/vmx/vmx.c:5415-5417`）。差异在两处：
KVM 把串 IO 交给 x86 模拟器（`:5408` 测 bit 4 → `:5412-5413`
`kvm_emulate_instruction()`），把标量 IO 交给 `kvm_fast_pio()`（`:5419`），
后者再经 `kvm_io_bus_read/write()` 派发给注册在 `KVM_PIO_BUS` 上的设备；
mini-kvm 没有模拟器，也没有 PIO 总线，串 IO 直接拒绝、其余端口丢弃。

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
sudo insmod mini-kvm.ko && sudo ./test-mini-kvm
sudo dmesg | grep 'mini-kvm guest'
# mini-kvm guest: Hello from Mini-KVM Guest!
# mini-kvm guest: [IRQ 0x21 handled]
```

串口是**按行**打点的：`mini_serial_out()`（`device.c:31-54`）逐字节记进环形
缓冲，遇到 `'\n'` 才把最近一行 `pr_info` 出去。用户态另有
`MINI_KVM_VM_GET_SERIAL` 可以把整段缓冲取回去做断言，`test-mini-kvm` 用的
就是这条路。写了别的端口会看到
`mini-kvm: 忽略 OUT port=0x... val=0x...`（`pr_info_ratelimited`）。

## 📝 检查清单

- [ ] 解释 PIO 和 MMIO 的退出信息分别落在哪个 VMCS 字段
- [ ] 说出 IO 退出的 `EXIT_QUALIFICATION` 里大小/方向/端口各在哪些位
- [ ] 解释 IO 退出后为什么必须手动推进 RIP，长度从哪里取
- [ ] 说明 16550A 的 LSR/IER 为什么不能省（mini-kvm 用"guest 不轮询"这一约定换来简化）
- [ ] 对比 mini-kvm 和 QEMU 的设备模型

## 🔗 下一步

Stage 5: 运行循环
