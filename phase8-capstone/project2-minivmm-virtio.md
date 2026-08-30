# 项目 2：自制 virtio-mmio 设备

> 目标：在项目 1 的 VMM 里，用纯用户态代码实现 virtio-mmio 传输层的
> **virtio-console** 与 **virtio-blk**，替换/补充串口。

**前置**：项目 1（可启动的 VMM）、phase5（virtqueue / feature 协商 /
vhost 对照）。规范：`../virtio-v1.3-csd01.pdf` §2.7（Split Virtqueues）、
§3.1（Device Initialization）、§4.2（Virtio Over MMIO）、
§4.2.2（MMIO Device Register Layout）。

---

## 设计决策：x86 上如何让 guest 发现 virtio-mmio 设备

virtio-mmio 没有自发现机制；x86 上 QEMU/Firecracker 靠 ACPI 表描述设备。
为了不在本阶段手写 ACPI，用 guest 内核自带的**命令行手动注册**通道：

```
virtio_mmio.device=<size>@<base>:<irq>[:<id>]
```

由 `vm_cmdline_set()` 解析（`drivers/virtio/virtio_mmio.c:718`），
注册成 platform device（`platform_device_register_resndata()`，:768）；
语法示例见该文件头注释（:39-50）。本项目的 cmdline 追加形如：

```
virtio_mmio.device=0x200@0xd0000000:5 virtio_mmio.device=0x200@0xd0000200:6
```

（GPA 选 guest RAM 之外的空洞区域；两个设备一个 console、一个 blk。
前提：自编内核开 `CONFIG_VIRTIO_MMIO=y`、`CONFIG_VIRTIO_BLK=y`、
`CONFIG_VIRTIO_CONSOLE=y`，见 `scripts/vm/build-kernel.sh`。）

---

## 里程碑

### M1：virtio-mmio 寄存器组（先做 console）

MMIO 读/写落在设备基址区间，产生 `KVM_EXIT_MMIO`（与串口的
`KVM_EXIT_IO` 并列处理）。寄存器集见
`include/uapi/linux/virtio_mmio.h`：

| 偏移 | 寄存器 | 说明 |
|------|--------|------|
| 0x000 | `VIRTIO_MMIO_MAGIC_VALUE` | 必须返回 `0x74726976`（"virt"） |
| 0x004 | `VIRTIO_MMIO_VERSION` | 2 = modern（本项目用 modern 接口） |
| 0x008 | `VIRTIO_MMIO_DEVICE_ID` | console=3 / blk=2（`virtio_ids.h:33-34`） |
| 0x00c | `VIRTIO_MMIO_VENDOR_ID` | 任意，如 `0xffff` |
| 0x030/0x034 | `QUEUE_SEL` / `QUEUE_NUM_MAX` | 队列选择与最大深度 |
| 0x050 | `VIRTIO_MMIO_QUEUE_NOTIFY` | **数据面入口**：guest 写队列号通知设备 |
| 0x064 | `VIRTIO_MMIO_INTERRUPT_ACK` | guest 确认中断 |
| 0x070 | `VIRTIO_MMIO_STATUS` | 设备状态机（§3.1 的复位/协商序列） |

对照 §3.1 的初始化序列实现状态机：guest 依次写
ACKNOWLEDGE(1) → DRIVER(2) → FEATURES_OK(8) → 配队列 → DRIVER_OK(4)；
任何一步写 0 表示设备复位。

### M2：手写 virtqueue 处理循环（split 布局）

按 §2.7 在 guest 内存里实现三段结构：

- **Descriptor Table**：`addr/len/flags/next` 数组
- **Available Ring**：guest → 设备（`flags`、`idx`、`ring[]`）
- **Used Ring**：设备 → guest（`flags`、`idx`、`ring[id,len]`）

实现要点：

1. 读 guest 写下的队列地址（modern 接口经
   `QUEUE_DESC_LOW/HIGH` 等 0x80 起的寄存器，见 `virtio_mmio.h`；
   或退回 legacy 的 `QUEUE_PFN`（0x040）简化实现）
2. 内存屏障语义：`avail->idx` 与 `used->idx` 的可见性（对照
   `../phase5-virtio/virtio-queue.md` 中的内存序讨论）
3. **通知方向**：guest 写 `QUEUE_NOTIFY` → `KVM_EXIT_MMIO` → VMM 处理
4. **中断方向**：设备完成后置 `VIRTIO_MMIO_INTERRUPT_STATUS`（0x60）
   bit0 并投递中断（见 M4）
5. feature 协商：第一版只谈 `VIRTIO_F_VERSION_1`（blk 可再加
   `VIRTIO_BLK_F_SEG_MAX` 等，按 `virtio_blk.h` 逐个核对再开）

console：实现 input/output 两个队列的 echo/转发到宿主终端即可；
blk：后端用普通文件 + `pread/pwrite`，支持 `VIRTIO_BLK_T_IN/OUT/FLUSH`
三种请求（结构 `virtio_blk_outhdr`，`include/uapi/linux/virtio_blk.h`）。

### M3：用 ioeventfd/irqfd 把数据面搬出 KVM_EXIT（进阶）

裸 `KVM_EXIT_MMIO` 每次通知都是一次用户态往返。KVM 提供两个零退出通道：

- **`KVM_IOEVENTFD`**（case @ `virt/kvm/kvm_main.c:5266` →
  `kvm_ioeventfd()` @ `virt/kvm/eventfd.c:999`）：把
  `QUEUE_NOTIFY` 的写绑定到 eventfd，guest 写入直接触发 fd，
  **不再退出到 VMM**；VMM 用独立线程 `read()` eventfd 收通知
- **`KVM_IRQFD`**（case @ `kvm_main.c:5257` → `kvm_irqfd()` @
  `virt/kvm/eventfd.c:579`）：把"设备中断"绑定到 eventfd，VMM 处理完
  请求后 `write(irqfd, 1)` 即注入中断，无需再走
  `KVM_IRQ_LINE`（`kvm_vm_ioctl_irq_line()` @ `x86.c:6528`）

里程碑：先用 `KVM_IRQ_LINE` 跑通（简单、同步），再切到
ioeventfd + irqfd，用 `perf kvm stat` 对比前后退出计数 —— 这直接复现
phase5 讲的"数据面下沉"优化。

### M4：中断路由

`virtio_mmio.device=` 里的 `<irq>` 是 GSI。in-kernel irqchip 下：

- 电平中断用 `KVM_IRQ_LINE` 置位/清位（IOAPIC 路由由
  `KVM_CREATE_IRQCHIP` 时建立的默认路由表承担，
  `kvm_setup_default_irq_routing()` @ `x86.c:7111`，见项目 1）
- 注意 guest 的中断处理程序需要看到边沿/电平时序：blk 完成一个请求
  拉高 → guest ACK（写 `INTERRUPT_ACK`）→ VMM 清 `INTERRUPT_STATUS` → 拉低

---

## 内核侧代码路径对照表

| 步骤 | 机制 | 内核侧 |
|------|------|--------|
| MMIO 退出 | `KVM_EXIT_MMIO` | EPT violation → `handle_ept_violation()` → MMIO 仿真（见 `../phase2-mem-virt/`） |
| 设备发现 | `virtio_mmio.device=` | `vm_cmdline_set()` @ `drivers/virtio/virtio_mmio.c:718` |
| 通知下沉 | `KVM_IOEVENTFD` | case @ `kvm_main.c:5266` → `kvm_ioeventfd()` @ `eventfd.c:999` |
| 中断下沉 | `KVM_IRQFD` | case @ `kvm_main.c:5257` → `kvm_irqfd()` @ `eventfd.c:579` |
| 电平中断 | `KVM_IRQ_LINE` | `kvm_vm_ioctl_irq_line()` @ `x86.c:6528` |
| guest 驱动 | virtio-mmio 前端 | `drivers/virtio/virtio_mmio.c`；blk `drivers/block/virtio_blk.c` |

---

## 已知陷阱

1. **GPA 选在 RAM 内**：MMIO 区域必须不在 `KVM_SET_USER_MEMORY_REGION`
   覆盖的 EPT 可访问区，否则不产生 EPT violation、`KVM_EXIT_MMIO` 永不出现。
   布局：RAM 留出空洞（e820 里也不声明），设备放洞里。
2. **字节序与对齐**：virtqueue 结构全部小端；vring 尺寸按 §2.7 的公式
   对齐（desc/avail/used 各有对齐要求），错一位 guest 直接挂起且不报错。
3. **modern vs legacy 寄存器不混用**：`VERSION`=2 时 guest 走
   0x80 起的 modern 队列寄存器；实现哪个版本必须与 `VERSION` 返回值一致。
4. **忘记 DRIVER_OK 前不动数据面**：guest 在配置完成前可能就写
   `QUEUE_NOTIFY`（探测行为），设备实现必须容忍。

---

## 验收标准

- [ ] guest 内 `echo hello > /dev/hvc0` 与宿主终端双向通；
      `ls /dev/vda`、`mount /dev/vda /mnt` 读写正常
- [ ] 能逐字段解释一次块请求的 desc → avail → 后端 → used → 中断 →
      ACK 全链路（对照 §2.7 图示）
- [ ] 切换 `KVM_IRQ_LINE` → irqfd/ioeventfd 后，`perf kvm stat` 中
      对应退出计数显著下降，能解释下降的是哪类退出
- [ ] 与 `../phase5-virtio/README.md` 对照：说明自己的实现相比
      QEMU/vhost 缺了什么（vhost 内核态数据面、批处理、zero-copy）

---

## 参考资料

- Virtio 规范：`../virtio-v1.3-csd01.pdf`（§2.7、§3.1、§4.2）
- phase5：`../phase5-virtio/README.md`、`virtio-queue.md`、
  `vhost-architecture.md`
- QEMU 对照：`/root/code/qemu-10.1.0-rc2/hw/virtio/virtio-mmio.c`
- guest 驱动：`/root/code/linux-6.12.93/drivers/virtio/virtio_mmio.c`
