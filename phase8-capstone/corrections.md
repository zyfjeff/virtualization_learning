# phase8-capstone 勘误（corrections.md）

> 依据 AGENTS.md：发现已有文档有错，在本目录写 corrections.md，说明错误、
> 给出正确信息与引用，并同步修正原文。所有行号基于 Linux 6.12.93、
> QEMU 10.1.0-rc2；"实测"均指本机（宿主是**裸金属**，96 线程 —— 见 H 节，
> 本文件旧版此处写反了）。A–I 节的结论都已在 `practice/minivmm.c` 里落地并
> 跑通；**J 节是纯静态勘误**（源码 / 规范比对），mini-kvm 的真机 `insmod`
> 验收尚未执行。

---

## A. `project1-minivmm-boot.md` 原文的错误

### A1. 陷阱 2："缺 TSC-deadline 位 → timer_mode_mask 置 0" 不准确

原文："`kvm_update_cpuid()` 把 `timer_mode_mask` 置 0（cpuid.c:398-403），
mask 为 0 时 `timer_mode = LVTT & mask` 恒 0"。

**正确**：`kvm_update_cpuid()` 是 if/else，不是只设或清零
（`arch/x86/kvm/cpuid.c:399-402`）：

```c
if (cpuid_entry_has(best, X86_FEATURE_TSC_DEADLINE_TIMER))
    apic->lapic_timer.timer_mode_mask = 3 << 17;
else
    apic->lapic_timer.timer_mode_mask = 1 << 17;
```

- guest CPUID **有** leaf1 但缺该位 → mask = `1<<17`，不是 0。后果是写
  LVT Timer 时 bit18 被掩掉（`lapic.c:2391`
  `val &= (apic_lvt_mask[0] | timer_mode_mask)`）：guest 写
  `APIC_LVT_TIMER_TSCDEADLINE`(2<<17，`apicdef.h:109`) 被掩成 0（one-shot），
  写 `3<<17` 被掩成 `1<<17`（periodic，`apicdef.h:108`）—— deadline 模式
  根本设不上。
- 只有**从未给 leaf1 建过 CPUID 条目**（`kvm_find_cpuid_entry(vcpu,1)`
  返回 NULL，`cpuid.c:398`）时，mask 才保持 kzalloc 初值 0。phase7 实验 3
  踩的是这一种（当时完全没调 KVM_SET_CPUID2 建 leaf1）。

引用：`cpuid.c:398-402`、`lapic.c:2391`、`lapic.c:1781-1782`
（`apic_update_lvtt()` 里 `timer_mode = LVTT & timer_mode_mask`）、
`apicdef.h:107-109`。

### A2. 陷阱 3："kvm_x2apic_msr_write 静默返回" 不准确

原文："`kvm_x2apic_msr_write()`（lapic.c:3308）在非 x2APIC 模式下静默返回
（:3313 检查 apic_x2apic_mode()）"。

**正确**：该函数 `return 1`（`lapic.c:3312-3313`），不是"静默丢弃"。这个 1
的去向取决于发起方：

- **guest 发起的 WRMSR**：`kvm_emulate_wrmsr()`（`x86.c:2079`）拿到非 0 →
  `complete_emulated_insn_gp()` → `kvm_inject_gp(vcpu, 0)`，即**给 guest 注入
  #GP**（`x86.c` `complete_emulated_insn_gp()`）。
- **宿主发起的 KVM_SET_MSRS**：`kvm_do_msr_access()`（`x86.c:500`）把 1
  原样返回用户态，该 MSR 写入被拒。phase7 实验用的是这条路径，所以观察到
  "没生效"，但机制是"拒绝"而非"静默"。

### A3. 陷阱 3 的行号：x86.c:675-676 → 675-679

`kvm_set_apic_base()`（`x86.c:671`）里 reserved_bits 的计算在 `:675-676`
（guest CPUID 无 X86_FEATURE_X2APIC 时把 `X2APIC_ENABLE` 并入保留位），
真正的拒绝判断在 `:678-679`（`if ((data & reserved_bits) != 0 ...) return 1;`）。
原文只引到 :675-676，漏了拒绝分支。

### A4. "cpuid.c:398-403" 一律改为 "cpuid.c:399-402"

M3 与"内核侧代码路径对照表"两处的 `cpuid.c:398-403`，精确范围是
`cpuid.c:399-402`（if 在 :399，两个赋值在 :400 / :402）。

### A5. M4 串口："读 LSR 返回 TX-empty" 远不够

原文把最小串口模拟归结为"LSR 读回 0x60"。实测这只够让 console 不卡死，
**不够让 8250 驱动把它认成可用端口**。要拿到
`ttyS0 ... (irq = 4 ...) is a 16550A`（打印在 `serial_core.c:2574`），至少还要：

1. **THRI 中断**：`autoconfig_irq()`（`8250_port.c:1305`）靠"开 IER 后写 THR
   能拉起中断"来反查 IRQ 号；给不出 THRI，guest 报 `irq = 0` 并退化成轮询。
2. **IIR[7:6] = 0b11**：`autoconfig()` 的 FIFO 类型 switch
   （`8250_port.c:1241`）用它判端口型号，给错会报 `is a 16450`。
3. **loop 测试的 MSR 回环映射**：`autoconfig()` 在
   `MCR = LOOP|OUT2|RTS` 下期望 `MSR == DCD|CTS`（`8250_port.c:1215-1219`），
   即回环时 OUT2→DCD、RTS→CTS（另 OUT1→RI、DTR→DSR）。
4. **IER 测试**：`autoconfig()` 会写/回读 IER（`8250_port.c:1175`），
   IER 需按 16550A 语义只实现低 4 位。

`practice/minivmm.c` 的 16550A 模型按 `include/uapi/linux/serial_reg.h`
逐位实现，语义对照 QEMU `hw/char/serial.c`。

### A6. 验收标准："LOC 计数增长" 在本项目不成立

原文验收项要求 `/proc/interrupts` 里 `LOC`（local timer）增长。本项目**没有
MP 表 / ACPI MADT**，guest 走 virtual-wire 模式、用 in-kernel 8259，本地
定时器中断不经过 LAPIC，`LOC` 恒 0。实测应看 **IRQ0 `XT-PIC timer`** 增长
（本机 2 秒内 803 → 1304，约 250 Hz，对应 CONFIG_HZ=250），以及
IRQ4 `ttyS0` 增长。`LOC` 要到装上 IOAPIC（项目 3 需要 MP 表）后才会涨。

---

## B. 原文档缺失、实测必需的项（已补进 minivmm.c 注释）

### B1. leaf1 ECX bit31（X86_FEATURE_HYPERVISOR）必须 VMM 自己置位

`KVM_GET_SUPPORTED_CPUID` 返回的 leaf1 ECX **不含** hypervisor-present 位
（本机实测 ECX = `0x76fab223`，bit31 = 0）。而 guest 的
`__kvm_cpuid_base()`（`arch/x86/kernel/kvm.c:872`，门槛在 `:877`
`if (boot_cpu_has(X86_FEATURE_HYPERVISOR))`）拿这一位当准入门槛。
缺了就没有 `Hypervisor detected: KVM`，整套 PV（kvmclock / PV EOI /
ASYNC_PF）全部失效。定义：`cpufeatures.h:144`（word 4 = leaf1 ECX，bit31）。
guest 内核侧还需 `CONFIG_HYPERVISOR_GUEST` / `CONFIG_KVM_GUEST`
（见 B4）。

### B2. leaf1 EBX[31:24] 初始 APIC ID 必须 = in-kernel LAPIC ID

宿主 leaf1 EBX[31:24] 是宿主自己的初始 APIC ID（本机 0x60），直接透传给
单 vCPU guest 会与 in-kernel LAPIC 的 ID 0 不一致，guest 在
`cpu_parse_topology()` 打 `[Firmware Bug]: ... APIC ID mismatch`
（`arch/x86/kernel/cpu/topology_common.c:174-176`，比较
`c->topo.initial_apicid != c->topo.apicid`）。修法：清 EBX[31:24]，并把
EBX[23:16]（每包逻辑处理器数，宿主 96 对单 vCPU 无意义）改成 1。

### B3. guest 内核要能 mount virtio-blk：CONFIG_EXT4_FS，且别被覆盖

项目 2 验收要 `mount /dev/vda /mnt`，guest 需要 `CONFIG_EXT4_FS=y`。
`scripts/vm/kernel-config` 里曾同时出现 `CONFIG_EXT4_FS=y`（:132）和后面的
`# CONFIG_EXT4_FS is not set`（原 :230），**kconfig 后写覆盖前写**，结果
ext4 被静默关掉。已删掉矛盾行（见该文件注释）。排查方法：对 fragment 里
"先 =y 后 is not set"的符号做交叉检查。

### B4. 有 kvm-clock 不等于 clocksource 停在 kvm-clock

宿主暴露 CONSTANT_TSC + NONSTOP_TSC 时，`kvmclock_init()` 主动把
`kvm_clock.rating` 降到 299（`arch/x86/kernel/kvmclock.c:342-345`），低于
`tsc` 的 300（`arch/x86/kernel/tsc.c:1189`），于是 dmesg 先
`Switched to clocksource kvm-clock` 再 `Switched to clocksource tsc`。
这是设计如此（invariant TSC 足够好），不是 VMM 缺陷；kvm-clock 仍在提供
sched_clock 与 pvclock MSR 更新。验收时别把"最终是 tsc"当成 kvmclock 没生效。

---

## C. 同步修正清单

- `project1-minivmm-boot.md`：按 A1–A6 改写陷阱 2/3、M3/M4、对照表行号、
  验收标准；按 B1/B2 在 M3 增加两条 CPUID 必需项。
- `../phase7-timer-virt/practice/README.md`：其"前置条件 ②/③"沿用了 A1/A2
  的错误表述（"mask 保持 0"、"静默返回"、`cpuid.c:398-403`、
  `x86.c:675-676`），已在 `../phase7-timer-virt/corrections.md` 单独记录并修正。

---

## D. 项目 3 实测新发现：legacy virtio 直通设备的 config 偏移错位

（非既有文档错误，是直通 `4b:00.0` 时实测踩到并定位的非显然陷阱，记录备查。）

**现象**：直通 virtio-blk（`[1af4:1001]`，legacy ID）probe 时
`virtblk_probe()` 在 `blk_validate_limits()`（`block/blk-settings.c:364`，
`max_segment_size < PAGE_SIZE`）WARN 并 `-EINVAL`，`/dev/vdX` 不出现。

**根因**：该设备把 device config 放在 BAR0+**0x14**（无 legacy MSI-X 向量
寄存器，`int_pin=0` 也说明它不打算用 INTx 布局）。但 guest 的 legacy
virtio-pci 驱动一旦启用 MSI-X，就按
`VIRTIO_PCI_CONFIG_OFF(msix_enabled)= msix?0x18:0x14`
（`include/uapi/linux/virtio_pci.h:80`）读 config，并把 0x14/0x16 当
`VIRTIO_MSI_CONFIG_VECTOR`/`VIRTIO_MSI_QUEUE_VECTOR`（`virtio_pci.h:74/76`）。
两者错位 4 字节：guest 把设备的 `seg_max`(=16) 当 `size_max` 读，
`max_segment_size=16 < PAGE_SIZE` → 校验失败。用 BAR0 MMIO 打点实测确认：
guest 写 0x14/0x16 向量寄存器后，从 0x20/0x24（config@0x18 布局）读
size_max/seg_max，拿到的是设备@0x20 的 16。

**修复**：`handle_bar0_legacy()` 以"guest 是否写 0x14/0x16 向量寄存器"判定
其采用 MSI-X 布局；是则把 config@0x18 的访问换算到设备@0x14、并截留
0x14/0x16。修复后 probe 成功，`/dev/vdb` 出现（4 TiB 容量读对）。

> ⚠️ **本节结论后续被推翻**：该"换算 + 截留"修复建立在"设备 config 固定在
> 0x14"的错误前提上。`msixdump -a` 实测证明设备布局随**自身** MSI-X Enable
> 切换，正确做法是纯透传；容量 `0x1_FFFFFFFF` 与 I/O 挂死的最终定论见
> **G 节**。本节的"修复"文字保留作排查史。

**遗留**（已解决，见 G 节）：~~MSI-X 中断尚未路由~~、~~I/O 挂起~~——
均已定位为截留向量寄存器所致并修复。另注意 guest 的 PCI MSI-X
Enable 位会先置后清（使能回退），不能拿它当"是否用 0x18 布局"的判据，要用
0x14/0x16 向量寄存器写。

---

## E. 项目 3 实测更正：撤回"环境限制"结论；VFIO 走 Posted Interrupt 时 `vfio_msihandler` 不在投递路径上

（本节整体是对前一版 E 节的更正。前一版有两处错误，均撤回。）

**错误 1（撤回）**：曾据 guest 运行期间 `/sys/kernel/iommu_groups/35/type`
显示 `identity` + 宿主 cmdline `iommu=pt`，推断"设备在 identity 域、DMA 未
翻译、本机是嵌套环境不支持直通"。**错**：(a) 本机是**裸金属物理机**，不是
嵌套；(b) 该 sysfs 文件读的是 `group->default_domain->type`
（`drivers/iommu/iommu.c:890-897` 的 `iommu_group_show_type`），即
`iommu=pt` 决定的**默认域**类型，不代表设备被 VFIO 接管后的当前域；
`VFIO_IOMMU_MAP_DMA` 成功本身就说明 VFIO 已为容器建立 DMA 翻译。

**错误 2（撤回）**：曾用 ftrace 抓到 `vfio_msihandler` 计数恒 0，作为"设备
从没发过 MSI-X"的证据。**该推论不成立**：本机开了 APICv + VT-d 中断重映射，
VFIO 直通默认建立 **Posted Interrupt** 链路，稳态投递**根本不经过
`vfio_msihandler`**，host 侧该 IRQ 的 `/proc/interrupts` 计数也恒 0——
设备正常发中断时这两个观测值同样是 0，不能作为"设备没发中断"的证据。

**正确的投递模型**（全部经 6.12.93 源码核实）：

1. `KVM_IRQFD`：若 `kvm_arch_has_irq_bypass()`（= `enable_apicv &&
   irq_remapping_cap(IRQ_POSTING_CAP)`，`arch/x86/kvm/x86.c:13660`），
   `kvm_irqfd_assign` 把 irqfd 注册为 irq_bypass **consumer**，
   token = `(void *)irqfd->eventfd`（`virt/kvm/eventfd.c:443-449`）。
2. `VFIO_DEVICE_SET_IRQS(MSI-X)`：每向量 `vfio_msi_set_vector_signal()`
   （`drivers/vfio/pci/vfio_pci_intrs.c:447`）做两件事：
   `request_irq(irq, vfio_msihandler, ..., trigger)`（`:510`，把
   `vfio_msihandler` 挂为 host IRQ 硬中断处理函数）+ 注册 irq_bypass
   **producer**，token = 同一个 trigger（eventfd_ctx 指针），
   `prod->irq = host irq`（`:515-517`）。
3. VMM 把**同一个 eventfd** 传给 `KVM_IRQFD` 与 `VFIO_DEVICE_SET_IRQS` →
   两侧 token 相同 → `virt/lib/irqbypass.c` 按 token 撮合 →
   `kvm_arch_irq_bypass_add_producer()`（`arch/x86/kvm/x86.c:13665`）→
   `vmx_pi_update_irte(kvm, prod->irq, gsi, 1)`（`:13678`）。
4. `vmx_pi_update_irte()`（`arch/x86/kvm/vmx/posted_intr.c:272`）在 GSI 的
   路由里找到 `KVM_IRQ_ROUTING_MSI` 条目，要求单 vCPU 目标 + 可 post
   （Fixed 投递）后 `irq_set_vcpu_affinity(host_irq, &vcpu_info)`（`:330`），
   把 PI descriptor 地址（vCPU 的）与 guest vector 交给 Intel IR 驱动 →
   IRTE 切 **Posted 模式（IM=1）**。
5. 稳态：设备 MSI-X → IOMMU IR 表 → Posted IRTE → 硬件直接置 vCPU PI
   descriptor（PIR[vector]、ON）并发 notification vector；VMX 硬件按
   SDM 30.6 就地处理，**0 次 VM-Exit**。`vfio_msihandler` 只是未建/退出
   PI 时的慢速后备（如 APICv 关闭、撮合失败、条目非 Fixed/多目标、
   `vmx_pi_update_irte(...,0)` 切回 Remapped：`x86.c:13689-13709`）。
6. 佐证：PI 建链失败时 `vfio_pci_intrs.c:518-523` 会打
   `irq bypass producer (token %p) registration fails`，可查 dmesg。

**结果**（慢速路径判据已执行，见 **G 节**）：`MINIVMM_MSIX_SLOW=1` 下
eventfd 计数 >0（vec1 触发 6 次、dd 完成）——设备确实发中断，卡点在
minivmm 自己的截留/换算逻辑，与 PI 投递链路无关。PI 正式链路随后也实测
通过：宿主 `vfio-msix[0/1]` 计数全程 0，guest 侧 queue 向量正常收中断。

---

## F. 操作事故复盘：2026-08-31 宿主挂死（我的操作链是直接诱因）

**事故**：调试项目 3 期间，宿主 2026-08-31 22:06 CST 进入大面积 D 状态、
不可操作，被迫硬重启；22:09 重启后（boot -1）23:15 再次无声死亡，23:19
再重启（boot 0）。boot 0 里设备完全恢复：`virtio_blk` probe 正常、
直接 `dd` 读 `/dev/vdb` 正常。

**时间线**（`journalctl -b -2` + 会话记录，均 CST；monotonic 12460661s ≈
22:00）：

| 时间 | 事件 |
|---|---|
| 21:51:20 | 为做"宿主驱动对照"把 `4b:00.0` 从 `vfio-pci` 绑回 `virtio-pci`；内核打出 `virtio_blk virtio1: 6/0/0 default/read/poll queues`；udev 随即对 `/dev/vdb` 发起扫描 I/O |
| 21:51:30-41 | 宿主 `dd` 验证成功（中断计数增长） |
| 21:52-21:53 | **我用 python+`/dev/mem` 直读这块正被驱动着的设备的 BAR2（MSI-X 表）与 BAR0 寄存器** |
| ~21:56:00 | `udev-worker:99693` 进 D 状态（首份报告 21:58:02 "blocked for more than 122 seconds"），栈：`do_exit→____fput→bdev_release→blkdev_flush_mapping→truncate_inode_pages_range→folio_wait_bit_common` —— 它发起的 I/O 永远没完成，folio 解不开锁 |
| 21:58:04 | **我发 unbind（想绑回 `vfio-pci`），bash:100010 卡在 `unbind_store→virtio_pci_remove→virtblk_remove→del_gendisk`**。`del_gendisk` 要等磁盘 opener/在途 I/O 归零，而卡死在 folio 上的 udev-worker 正是最后的 opener → 死等，且**持有 device_lock** |
| 22:00:19 | 我再跑 `ls`/`cat driver_override` 查状态 → 撞上同一把锁（cat:100155 卡 `driver_override_show→mutex_lock`） |
| 22:02-22:06 | hung task 连报（udev-worker 491s / bash 245s / cat 122s），任何触碰该设备 sysfs 的进程都进 D；22:06 后机器不可用，硬重启 |

**责任判定**：
- "绑 `virtio-pci` → `/dev/mem` 摸活着的设备 BAR → 在设备有在途 I/O 时
  unbind"整条链都是我的操作，**向 D 状态设备发 unbind 是挂死的直接触发点**。
- 设备的"数据面停止完成请求"最早由哪个 I/O 触发，证据无法区分：udev 扫描
  （21:51 起）与我的 `/dev/mem` 读（21:52-53）落在同一窗口。23:19 冷启动后
  设备立即恢复正常，说明是设备自身状态问题、非宿主损坏——这块设备
  （物理卡、`[1af4:1001]`、2 TiB）对异常访问模式/状态是敏感的。
- **第二次死亡（23:15，boot -1）与我的操作无已证关联**：journal 显示
  22:09:15 设备 probe 正常（`[vdb] 4294967296 512-byte logical blocks`，
  2.00 TiB），全程无 hung task / BUG / oops / AER 记录；我的会话记录在
  14:04:53Z–15:35:24Z（UTC）之间无任何命令。死因不明，如实记录。

**教训（防再犯规则已同步进 `AGENTS.md` 已知陷阱 15）**：
1. 存储设备被内核驱动挂着、udev 未 settle 时**禁止 unbind/复位**；先确认
   无在途 I/O、无 opener。
2. **禁止用 `/dev/mem` 读写正被驱动着的设备的 BAR**（读设备寄存器也可能
   扰动设备状态）；要看设备寄存器走 VFIO device fd 的可控路径。
3. 对可疑设备的一切 sysfs 访问加 `timeout` 包裹；发现 D 状态级联立刻停手，
   改用 `/proc/<pid>/stack` 定位与 PCI 级复位（FLR / bus reset）手段。
4. 实验结束前把直通设备恢复到安全绑定（`vfio-pci`）、无残留句柄，别把
   带病设备留给下一次启动。

**附带观察（项目 3 线索，已结案）**：宿主 `virtio_blk` 读到的容量是
`4294967296` 扇区（`0x1_00000000`，2.00 TiB）；而 minivmm guest 读到
`8589934591`（`0x1_FFFFFFFF`）——**低 32 位读成了全 1**。定论见 **G 节**：
guest 的 config 读被旧换算/截留逻辑错置，落到了与布局切换相关的无效窗口；
改纯透传后 guest 读回 `4294967296`（2.00 TiB），与宿主一致。

---

## G. 项目 3 最终定论：设备布局随自身 MSI-X Enable 切换；I/O 挂死根因是截留向量寄存器

（推翻 D 节的"固定 config@0x14 + 换算"模型。全部结论经 `practice/msixdump.c`
的 `-a`（arm 对照）实验与修复后运行实测双向验证。）

### G1. 设备 BAR0 布局随**自身** MSI-X Enable 切换（实测）

`4b:00.0` 是标准 legacy virtio-pci，但 BAR0 布局不是静态的：

| 设备自身状态 | 0x14 | 0x16 | config |
|---|---|---|---|
| MSI-X 未启用 | config（capacity_lo@0x14…） | — | @0x14 |
| `VFIO_DEVICE_SET_IRQS` 启用后 | config MSI-X vector（u16） | queue MSI-X vector（u16），默认均 `0xFFFF`=NO_VECTOR | @0x18 |

`msixdump -a` 实测：arm 前 `BAR0[0x14]`=capacity_lo；arm 后
`[0x14]=0x0000ffff`（两个 NO_VECTOR）、`[0x18]/[0x1c]`=capacity 0/1，
且 0x14/0x16 可读写回。启用后布局与 guest legacy 驱动的 MSI-X 视图
（`VIRTIO_PCI_CONFIG_OFF(msix)=0x18`、向量寄存器@0x14/0x16，
`include/uapi/linux/virtio_pci.h:74-80`）**完全一致**。

### G2. 三个根因

1. **I/O 挂死**：旧代码截留 guest 对 0x14/0x16 向量寄存器的写（从不转发），
   设备因此永远不知道队列对应哪个 MSI-X 表项（queue vector 停在
   NO_VECTOR）——请求完成也不发中断，guest 死等。
2. **capacity `0x1_FFFFFFFF`**：旧"条件换算"（`msix_enabled && !routing_done`
   时 0x18→0x14）与布局切换的时序窗口相互作用，把 guest 的 capacity_lo 读
   错置。设备向量寄存器默认恰为 `0xFFFF`，两个 16 位寄存器拼 32 位正好是
   全 1，与该设备"异常状态下读回大片 0xff"（F 节）的行为一致。
3. **教训**：不要给直通设备假设静态寄存器布局；对任何换算/截留逻辑，先用
   受控工具（VFIO device fd）做"启用前/后"对照 dump，再决定 VMM 是否需要
   介入——本例的正确答案是**纯透传**。

### G3. 修复后模型（已实现并实测）

1. **BAR0 纯透传**（`handle_bar0_legacy`）：读写全部 `pread/pwrite` 到
   VFIO BAR0 region，仅对 0x10 QUEUE_NOTIFY 与 0x14/0x16 做影子计数。
2. **arm 时机**：guest 清 MSI-X Function Mask（W2）时触发。依据 6.12
   `msix_capability_init`（`drivers/pci/msi/msi.c:714`）实测顺序：
   `:725` 先置 ENABLE|MASKALL（"Some devices require MSI-X to be enabled
   before the MSI-X registers can be accessed"，使表可访问）→ `:740`
   编程表项 → `:756` `msix_mask_all()` → `:758` 清 MASKALL。W2 时影子表已
   完整，据此建 `KVM_SET_GSI_ROUTING` + `KVM_IRQFD` 并 `VFIO_DEVICE_SET_IRQS`
   武装（未编程向量传 fd=-1）。
3. **mask 位直写物理表**：VFIO 对 MSI-X **没有** `ACTION_MASK/UNMASK`
   （`drivers/vfio/pci/vfio_pci_intrs.c:854-857`："XXX Need masking support
   exported"，func 指针留空）。宿主内核对自家设备也是直写
   （`pci_msix_write_vector_ctrl`，`drivers/pci/msi/msi.h:43-55`）。故
   guest 对表内 Vector Control（条目偏移 12）的写由 `handle_pt_mmio` 直接
   `pwrite` 到物理表；arm 后另把影子表 ctrl 同步写回一次——武装时内核
   `request_irq(flags=0)` → `irq_startup` → `pci_msix_unmask` 已把所有
   武装向量放行（`vfio_pci_intrs.c:510`），而 guest 在 W2 时刻的状态恰是
   `msix_mask_all` 写的全 mask（`msi.c:756` 在 `:758` 之前），须拉齐。
   addr/data 永不写物理表（那里是内核写的宿主 MSI 消息）。

### G4. 实测验收（2026-08-31）

- 慢速路径（`MINIVMM_MSIX_SLOW=1`，跳过 KVM_IRQFD 以可靠计数）：
  `[msix-slow] vec1 设备中断触发` 共 6 次；`dd if=/dev/vdb bs=512 count=4`
  完成；`[vdb] 4294967296 512-byte logical blocks (2.00 TiB)` 容量正确。
- 正式路径（KVM_IRQFD/PI）：guest `/proc/interrupts` 显示
  `25: 5 PCI-MSIX-0000:00:01.0 1-edge virtio2-req.0`，dd 完成；**运行期间
  宿主 `/proc/interrupts` 的 `IR-PCI-MSIX-0000:4b:00.0 vfio-msix[0/1]`
  计数全程为 0**（8 次采样）——中断未经 `vfio_msihandler`，经 Posted IRTE
  直投 vCPU、0 次 VM-Exit（SDM 30.6，投递模型见 E 节）。
- 路由 `2/7` 属正常：guest 单 vCPU → virtio_blk 只建 1 个请求队列
  （`1/0/0 default/read/poll queues`），只编程 config + queue 两个向量。
- exit 统计：`IO=46943 MMIO=443`、`QUEUE_NOTIFY 写=2`、`ctrl 转发=23`。

### G5. 工具

`practice/msixdump.c`：`./msixdump` 读 MSI-X cap/表/BAR0 前 64B；
`-r` 先 `VFIO_DEVICE_RESET`；`-a` 做 arm 对照（SET_IRQS 武装 2 个向量 →
dump 布局与向量寄存器读写 → `DATA_NONE` 解除）。本次定位全靠它。

## H. `practice/README.md` 项目 1 环境描述更正：宿主是裸金属，不是 KVM guest

**原文**：「宿主本身是一台 KVM guest（96 线程），Linux 6.12.93-kvm-study，
guest 512 MB、单 vCPU。」

**更正**（2026-09-01 复核）：宿主为**裸金属**：`systemd-detect-virt` 返回
`none`，`/proc/cpuinfo` flags 无 hypervisor 位，宿主 dmesg 无
"Hypervisor detected"。96 线程、内核 6.8.0-51-generic 属实；
6.12.93-kvm-study 是 **guest** 内核（`scripts/vm/build-kernel.sh` 产物），
原文把两件事混在了一句。已同步修正 `practice/README.md` 项目 1 环境段
与项目 4 环境段。

## I. 项目 4 M3 flood 停摆：丢字符在 guest 侧，不是 minivmm 串口模型

**现象**：`bench-halt.sh` flood 连发 3000 字符，约 1000+ 字符后回显永久
停止，后续全部 2 s 超时。

**排查与定论**：

1. gdb 抓停摆时刻的 minivmm 串口状态：`ier=0x05`（RDI|RLSI，
   `UART_IER_THRI`=0x02 不在内）、`lsr=0x60`（THRE|TEMT、无 DR）、
   `iir=0xC1`（NO_INT）、`rx_head==rx_tail`、`irq_level=0` —— 状态
   **完全自洽且静止**：没有待发中断、RX 环空。`rx_head==rx_tail==1327`
   说明送达的 1327 个字符**全部被 guest 读走**（RBR 读空了环）。
2. 因此丢字符发生在 guest 读完 RBR 之后，即 guest tty/shell 层。宿主
   pty 实验复现：向交互的 `busybox sh`（v1.36.1）喂 1400 个 'a'，只有
   ~1024 个被回显并进入行缓冲（执行的命令行长度 ~1024），超出部分
   **照收（被读走）但不回显、不保留** —— busybox lineedit 行缓冲上限。
3. 1327 ≈ 1024（行缓冲）+ 停摆后超时期间继续送达并被丢弃的字符，吻合。

**处置**：`bench-halt.sh` flood 限 800 字符（< 1024），并加"连续 3 次
超时即中止"看门狗（见 `practice/README.md` 测量陷阱备忘）。minivmm.c
未改动——串口模型无错，不引入调试代码。

## J. `practice/mini-kvm/`：设计期 stage 文档的技术错误（已回改）

mini-kvm 从单文件 `examples/mini-kvm/mini-kvm.c` 拆成多文件内核模块
（`main.c`/`vmx.c`/`ept.c`/`interrupt.c`/`device.c`/`vcpu.c`/`vmx_entry.S`）后，
`stages/stage1..5.md` 与 `README.md` 仍是拆分前写的**设计期草稿**：伪代码不是
真实代码，几处描述与硬件规范不符。下列错误全部已回改到原文，实现侧
（`.c`/`.S`）本来就是对的，本轮没有改行为，只改了 `interrupt.c`/`vcpu.c`/
`main.c` 的注释引用 —— 而这件"只改注释"的事本身让所有 `vcpu.c:行号` 引用失效，
见 J10。所有引用对 Linux 6.12.93 与 `intel-vmx.pdf` 逐条核对。

### J1. EPT violation 的 GPA 取自 `EXIT_QUALIFICATION`（stage4 概念块）

**原文**：「MMIO …… 内存读写触发 EPT_VIOLATION VM-Exit，
`EXIT_QUALIFICATION` 包含访问的 GPA」。

**正确**：GPA 在 **`GUEST_PHYSICAL_ADDRESS`**（编码 `0x00002400`，
`arch/x86/include/asm/vmx.h:261`；SDM 25.9.1："This field is used by VM exits
due to EPT violations and EPT misconfigurations."）。
`EXIT_QUALIFICATION`（`0x00006400`，`asm/vmx.h:349`）在 EPT violation 下的
格式是 **SDM Table 28-7**，只有权限/类型标志位（bit 0 read / bit 1 write /
bit 2 fetch / bits 5:3 本次行走条目 R/W/X 的 logical-AND / bit 7 GLA 有效 /
bit 8 翻译访问 / bits 9-11 advanced VM-exit info / bit 12 IRET 解除 NMI 阻塞 /
bit 13 shadow-stack / bit 14 SSS / bit 15 guest-paging verification /
bit 16 异步访问），**一个地址位都没有**。照原文写会拿权限位拼出来的假地址
当 GPA 去映射。

**KVM 佐证**：`handle_ept_violation()` 里
`gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);`（`arch/x86/kvm/vmx/vmx.c:5798`）。
mini-kvm 同位置见 `ept.c:240`。

### J2. EPT 条目 bit 7 被写成"Large Page 标志"（stage2「EPT 权限位」）

**原文**给了一张不分层级的「EPT 条目位」表，其中
`[7] Large Page - 大页标志`、`[12:51] Address`。

**正确**：EPT 里**没有** x86-64 页表那种 PS 位的统一说法，位段按条目角色分
三种表：

| 条目 | 规范表 | bit 7 |
|---|---|---|
| 非叶（PML4E / PDPTE→PD / PDE→PT） | Table 29-2 / 29-4 / 29-6 | 与 bits 6:3 一起 "Reserved (must be 0)" |
| PDE 映射 2MB 页 | Table 29-5 | "Must be 1 (otherwise, this entry references an EPT page table)" |
| PTE 映射 4KB 页 | Table 29-7 | **"Ignored."** |

也就是说：4KB 叶的 bit 7 根本不参与判定；把它当"大页标志"清零，在 2MB 叶上
会让这条 PDE 变成"指向下一级页表"，翻译直接走歪。另外 bit 8/9（A/D）只在
EPTP bit 6 = 1 时有效，bit 10（user-X）只在 MBEC = 1 时有效，bit 63 只在
"EPT-violation #VE" = 1 时有效（均在 Table 29-5/29-7 的原文里限定）。
mini-kvm 两侧都不开这些能力，`ept.c:147-157` 的 EPTP 因此不带
`VMX_EPTP_AD_ENABLE_BIT`。

### J3. IO 退出的 `EXIT_QUALIFICATION` 布局缺字段（stage4）

**原文**只列了 size / direction / string / port 四项，且把串口代码写成
"先解码再判 string"。

**正确**（SDM Table 28-5）：bits 2:0 size−1（合法编码只有 0/1/3，4 与 5 是
保留）、bit 3 方向（0=OUT，1=IN）、bit 4 string、bit 5 REP prefixed、
bit 6 操作数编码（0=DX，1=立即数端口）、bits 15:7 未定义、bits 31:16 端口号、
bits 63:32 未定义。指令长度**不在**这个字段里，在
`VM_EXIT_INSTRUCTION_LEN`（`0x440c`，SDM 28.2.4）。实现侧 `device.c:61-104`
与 KVM `handle_io()`（`vmx.c:5401-5420`）公式一致：`string = qual & 16`
（`:5408`）先判、`port = qual >> 16`（`:5415`）、`size = (qual & 7) + 1`
（`:5416`）、`in = qual & 8`（`:5417`）。

### J4. 五篇文档的「预期输出」是编造的

原文到处是 `=== Stage 1: VMX 初始化 ===`、`✓ CPU 支持 VMX`、
`!!! VM-Exit 发生 !!!`、`Exit reason: 30 (IO)`、`Guest says: H` 这类横幅，
模块里**一条都不存在**。stage1/2/4/5 与 README 的验收段已换成真实
`pr_info` 字符串（`VMX_BASIC=… revision=… true_ctls=…`、`VMXON 完成…`、
`memslot 注册 GPA=0x0 大小=512 页`、`VMCS 初始化完成 (vcpu=0 rev=…)`、
`mini-kvm guest: <字符>`）。

顺带两个数值错误（本轮新写文本时也犯了）：

- `KVM_EXIT_HLT` = **5**（`include/uapi/linux/kvm.h:151`），不是 1。
- `EXIT_REASON_EXCEPTION_NMI` = 0、`EXIT_REASON_EXTERNAL_INTERRUPT` = **1**
  （`arch/x86/include/uapi/asm/vmx.h:32-33`），stage5 的分发表一度把后者
  写成 0。这两个号挨着，最容易记反。
- `RUN 结束 …` 统计行是 `pr_debug`（`vcpu.c:366-369`），不开 dynamic debug
  根本看不到，文档必须写清 `echo -n 'module mini_kvm +p' | tee
  /sys/kernel/debug/dynamic_debug/control` 这一步。

### J5. 引用了 KVM 里不存在的函数

**原文**：README 的 `interrupt.c` 一行对照源码写 `vmx_reinject_nmi()`；
`interrupt.c` 头注释写「NMI 应当重新注入给 guest（对照 vmx.c
`handle_exception_nmi` 的 NMI 再注入路径）」。

**正确**：6.12.93 里 `vmx_reinject_nmi()` 不存在（全树 grep 无此符号）。
KVM 对"guest 执行期间到达的 NMI"的处理恰好**相反**：在 root 模式直接跳宿主
IDT 的 NMI 门消费掉（`vmx_vcpu_enter_exit()` → `vmx_do_nmi_irqoff()`，
`vmx/vmx.c:7330-7338`；声明 `:6978`），`handle_exception_nmi()` 见到
`is_nmi()` 直接 `return 1`，注释写明"NMIs are handled by
vmx_vcpu_enter_exit()"（`vmx/vmx.c:5225-5231`）。KVM 注入给 guest 的 NMI
来自用户态 `KVM_NMI` ioctl（`x86.c:5193-5197` → `kvm_inject_nmi()` →
`KVM_REQ_NMI` → `process_nmi()`）与"已注入未被消费"的重投
（`x86.c:10381-10388`）。

**处置**：README 换成真实锚点；`interrupt.c` 注释改写为「mini-kvm 与 KVM 在
这里是**分歧**而不是照抄」，并把代价（宿主自己的 NMI 被抢走，MCE 打印 /
NMI watchdog 场景下不可用）写进注释与 stage3。

同类：`CPU_BASED_IO_EXITING` 这个宏在 6.12.93 不存在，只有
`CPU_BASED_UNCOND_IO_EXITING` 与 `CPU_BASED_USE_IO_BITMAPS`
（`arch/x86/include/asm/vmx.h:43-44`）；`vmxon_1` 也不存在，
`kvm_cpu_vmxon()`（`vmx.c:2833-2851`）用的是
`asm goto("1: vmxon %[vmxon_pointer]") + _ASM_EXTABLE(1b, %l[fault])`。

### J6. 「unconditional I/O exiting 与 IO bitmap 不能同时置」说反了

stage1 原文：「IO 用 `CPU_BASED_UNCOND_IO_EXITING`，不是 `IO_EXITING` + IO
bitmap，**两者不能同时置**」。

**正确**：SDM Table 25-6 对 bit 25（use I/O bitmaps）写的是 "If the I/O
bitmaps are used, the setting of the 'unconditional I/O exiting' control is
**ignored**"；§26.1.3 给出完整真值表：两个都 0 → 指令正常执行；uncond=1 且
bitmap=0 → 一律退出；bitmap=1 → 按 bitmap 判定（端口空间回绕访问时强制退出）。
也就是**没有互斥检查**，只有优先级。mini-kvm 没有 bitmap 页，所以只留
bit 24（本模块 `vmx.c:282`）。

### J7. 事件注入约束的章节归属错了（`interrupt.c` 头注释 + stage3）

**原文**把「IF=1 且 interruptibility bit0/bit1=0」两条都记在 SDM 27.3.1.4。

**正确**：两条分属两处，且第二条对 NMI 同样生效：

- RFLAGS 检查在 **27.3.1.4**（Checks on Guest RIP, RFLAGS, and SSP）：
  "The IF flag (RFLAGS[bit 9]) must be 1 if the valid bit (bit 31) in the
  VM-entry interruption-information field is 1 and the interruption type
  (bits 10:8) is external interrupt."
- interruptibility 检查在 **27.3.1.5**（Checks on Guest Non-Register State）：
  "Bit 0 (blocking by STI) and bit 1 (blocking by MOV-SS) must both be 0 if
  the valid bit … is 1 and the interruption type … has value 0, indicating
  external interrupt, **or value 2, indicating non-maskable interrupt (NMI)**."
  同节还规定：只有 "virtual NMIs" = 1 时才要求 bit3（NMI 阻塞）为 0
  （NOTE 明说该控制为 0 时不作要求）——所以 `interrupt.c:96-99` 放弃转注
  纯属策略，不是硬件逼的。
- 字段自洽性另在 **27.2.1.3**：type=NMI ⇒ vector 必须为 2；type=硬件异常 ⇒
  vector ≤ 31；bits 30:12 保留必须为 0。违反任一条都是 VM-Entry failure。
- 另两条容易漏的：VM-Entry interruption-information 的 valid 位**每次
  VM-Exit 都会被清**（25.8.3），以及 HLT activity state 下允许注入外部中断
  与 NMI（27.3.1.5 的枚举）——这解释了 KVM `vmx_clear_hlt()`
  （`vmx.c:1817-1828`）为什么改 activity state 而不是改 RFLAGS。

### J8. 本轮重写时自己犯的错（记为流程教训）

- README 第 7 节一度**凭记忆"引用"规范原文**：写了 "Any of the following
  operations… must be executed on the logical processor that the VMCS is
  active on" —— 这句话在 §25.11.1 里不存在。已换成逐字核对过的原文，并补上
  真正的理由：§25.11.1 "a logical processor may maintain some VMCS data of an
  active VMCS on the processor and not in the VMCS region"。
- README 第 9 节一度写"完全没有投机执行侧的缓解"——过度概括。
  `vmx_entry.S` 在弹出寄存器前清零全部 GPR，正是 KVM 的 L1F 卫生
  （`vmenter.S:231-240`，理由原文含 "an L1 cache miss when restoring
  registers could lead to speculative execution with the guest's values"）。
  已改成"有 GPR 清零，缺 IBPB（`vmx.c:1486`）、L1D flush
  （`vmx.c:234/:249/:251`）、`VMX_RUN_SAVE_SPEC_CTRL`（`vmx.c:960-961`）、
  MDS/GDS 缓解"。
- README 与 stage4 一度写"memslot 外的 GPA 会一直 EPT violation"——与实现
  不符：`ept.c:238-243` 只报一次 `-EFAULT`，运行循环随即回
  `KVM_EXIT_INTERNAL_ERROR`，不会循环退出。
- "PIRL" 是我生造的词，规范里没有；写 PIR / VIRR（SDM 30.6）。
- stage2 的 Table 28-7 位段第一版里我自己编了 "bit 14 = GDT/IDT 使用、
  bit 15 = SN 页"，对照规范后改成 J1 里那份真实列表。

**教训**：新增引用与"引用既有代码的行为概括"必须同一次动作里核对，不能先写
后补；引号里的规范原文必须来自 `pdftotext` 的实际输出。

### J9. 本文件自相矛盾：文档头说宿主是 KVM guest

第 5 行原写「"实测"均指本机（宿主即一台 KVM guest，96 线程）」，与 H 节的
复核结论（裸金属）冲突。已改为裸金属并注明见 H 节，同时声明 A–I 与 J 的
验证强度不同：J 是静态勘误，**mini-kvm 的真机 `insmod` 与 `test-mini-kvm`
验收尚未执行**。

### J10. 本轮自己在引用上犯的三类错

**(a) "只改注释"让全部 `vcpu.c:行号` 引用偏移一行。**
本轮为 `interrupt.c`/`vcpu.c` 补注释时，`mini_vcpu_run_loop()` 内部的注入窗口
注释多了一行，于是引用运行循环的三处 stage 文档集体偏一。逐个按当前文件重新
推导后改成：

| 引用点 | 错 | 对 |
|---|---|---|
| 注入窗口代码（stage3） | `vcpu.c:146-156` | `147-157` |
| 循环头 + 刷新 Host（stage5） | `vcpu.c:138-156` | `138-157` |
| bit31 entry-failure 拦截（stage5） | `vcpu.c:210-219` | `211-220` |
| gs_shadow + 关中断窗口（stage5） | `vcpu.c:187-192` | `188-193` |
| 进入失败分支（stage5） | `vcpu.c:194-202` | `195-203` |
| `launched` 置位（stage5） | `vcpu.c:203-204` | `204-205` |
| NMI 转注分支（stage3） | `vcpu.c:229-236` | `230-237` |
| 收尾 `pr_debug`（stage3/stage5/本文件） | `318-321` / `316-322` | `319-322` / `318-322` |

教训：改完被引用的源文件，必须**重新 grep 定位**而不是在旧行号上加减；同一轮里
`interrupt.c:59-63`、`interrupt.c:91-105`、`interrupt.c:96-99`、
`guest/guest.S:39-62`、本模块 `vmx.c:718-719`、`test-mini-kvm.c:9-19` 都按此
重推过一遍。

**(a′) 内核侧引用同样有偏差 —— 用脚本全量扫了一遍。**
逐条读文档抓不到这类错，所以写了个小脚本：把 README、五篇 stage 与本节里所有
`` `file.c:NNNN[-MMMM]` `` 抽出来，逐个在 `/root/code/linux-6.12.93/` 里解析
路径（含 `vmx.c` 这种两处同名的歧义），打印被引用的那行原文，人工判是否对得上
上下文。扫出的问题：

| 引用点 | 错 | 对 |
|---|---|---|
| `kvm_online_cpu()`（本文件、`main.c`、README 第 9 节） | `kvm_main.c:5619-5626`（5619 是左花括号；上一轮"修正"改过头） | `5618-5626` |
| `handle_ept_violation()` 读 GPA（stage4、J1） | `vmx.c:5799`（那是 `trace_kvm_page_fault`） | `5798` |
| `__KVM_REQUIRED_VMX_VM_EXIT_CONTROLS`（stage3） | `vmx.h:514-516`（514 是空行） | `515-517` |
| `vcpu_load()`（README、stage5、`vcpu.c` 注释） | `kvm_main.c:205-214`（214 是 `EXPORT_SYMBOL_GPL`） | `205-213` |
| L1F 寄存器清零注释（README 第 9 节、J8） | `vmenter.S:231-239`（`*/` 在 240） | `231-240` |
| "已注入未被消费"重投（stage3、`interrupt.c`、J5） | `x86.c:10382-10389`（首末各偏一） | `10381-10388` |
| `handle_external_interrupt_irqoff()` 里的变量（stage3） | `host_idt` | `host_idt_base` |

同一趟扫描顺带修掉一个**可读性缺陷**：裸 `vmx.c:NNNN` 在本项目里同时可能指
`arch/x86/kvm/vmx/vmx.c` 和本模块自己的 `vmx.c`（`vmx.c:249`、`:256-261`、
`:287-292`、`:574`、`:600-601` 全是后者）。凡是模块侧的都改写成
「本模块 `vmx.c:…`」，内核侧写全路径。这一串数字只是当时的快照，`vmx.c` 之后
又被改过，现值一律以 J11(6) 的表为准。

**教训**：文档里 `file:line` 的数量上千，靠肉眼必然漏；这类引用要么写符号名，
要么用脚本机械核对。本节 A–I 是真机数据，J 的这些行号已按 6.12.93 与本机
`make` 产物逐条校过。

**(b) `mini-kvm.ko` 与 `mini_kvm` 混用。**
`Makefile` 写的是 `obj-m := mini-kvm.o`，产物文件名带连字符；kbuild 把
`-` 换成 `_` 才做成模块名（`scripts/Makefile.lib:108` 的
`name-fix-token = $(subst $(comma),_,$(subst -,_,$1))`，经 `modname_flags`
`:111` 变成 `-DKBUILD_MODNAME=`），modpost 生成的 `__this_module` 直接用它当
`.name`（`scripts/mod/modpost.c:1765` `MODULE_INFO(name, KBUILD_MODNAME)`、
`:1769` `.name = KBUILD_MODNAME`）。所以：

- `insmod` 用**文件路径** → `sudo insmod mini-kvm.ko`（stage5 一度写
  `mini_kvm.ko`，那个文件不存在）；
- `rmmod` / dyndbg / `/sys/module/` 用**模块名** → `rmmod mini_kvm`、
  `echo -n 'module mini_kvm +p' | tee /sys/kernel/debug/dynamic_debug/control`；
- `dmesg | grep mini-kvm` 匹配的是 `pr_fmt` 前缀 `"mini-kvm: "`，与前两者无关。

这条不止是读 kbuild 源码：本轮 `make` 实际产出的就是 `mini-kvm.ko`，而
`strings mini-kvm.ko | grep '^name='` 给出 `name=mini_kvm`，与上面的推导一致。
仍未验证的是模块的**运行行为**——真机 `insmod` + `test-mini-kvm` 九步验收
（见本文件开头对 J 节的说明）。

### J11. 静态审计轮：CR4 的"第二份副本"、guest CR4.VMXE，以及 §27.3.1.2 的两处过度概括

本轮仍然不能上机（卸载 `kvm_intel` 会打断宿主所有 VM；本机 `/dev/cpu/*/msr` 一律
EIO，能力 MSR 读不到），所以做的是"把只有真机会炸的东西挖出来"的静态审计。**结论
按危险程度排序。**

#### (1) 裸写 CR4 绕过内核的 CR4 影子 —— 会在进程切换里 #GP(0) 打崩宿主

`main.c` 的 `mini_vmx_enable_one()` / `mini_vmx_disable_one()` 原来这样开/关
VMX 能力：

```c
asm volatile("mov %%cr4, %0" : "=r"(cr4));
cr4 |= X86_CR4_VMXE;
asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
```

这在功能上"看起来对"，但它绕过了内核维护的第二份副本 —— per-CPU 影子
`cpu_tlbstate.cr4`：

```c
/* 来源: arch/x86/kernel/cpu/common.c:453-464（Linux 6.12.93） */
void cr4_update_irqsoff(unsigned long set, unsigned long clear)
{
	unsigned long newval, cr4 = this_cpu_read(cpu_tlbstate.cr4);

	lockdep_assert_irqs_disabled();

	newval = (cr4 & ~clear) | set;
	if (newval != cr4) {
		this_cpu_write(cpu_tlbstate.cr4, newval);
		__write_cr4(newval);
	}
}
```

**新值是从影子里算出来的**，不是从真实 CR4。而 `switch_mm_irqs_off()`
（`arch/x86/mm/tlb.c:499`）每换一次 mm 都会走
`cr4_update_pce_mm(next)`（`:658`，函数体 `:469-482`），里面是
`cr4_set_bits_irqsoff(X86_CR4_PCE)` / `cr4_clear_bits_irqsoff(X86_CR4_PCE)`
（`CONFIG_PERF_EVENTS` 下，RDPMC 权限随进程切换变化时）。于是：VMXE 只在真实
CR4 里、不在影子里 → 下一次 PCE 变化就会用"没有 VMXE 的影子值"执行 `MOV to
CR4`。

这一条 `MOV CR4` 在 VMX root operation 里是硬件禁止的 —— SDM Vol.3C §24.8
第一条：*"Any attempt to set one of these bits to an unsupported value while in
VMX operation (including VMX root operation) using any of the CLTS, LMSW, or MOV
CR instructions causes a general-protection exception"*，紧跟的 NOTE 列出
*"the following bits be 1 in VMX operation: CR0.PE, CR0.NE, CR0.PG, and
CR4.VMXE"*；§24.7 也有一句 *"Once in VMX operation, it is not possible to clear
CR4.VMXE (see Section 24.8)"*。所以症状是**宿主在进程切换路径上吃 #GP(0)**，
比"guest 起不来"严重一个量级。

第二个后果更隐蔽：`mini_vmx_enable_one()` 用来拦"已有 VMX 用户"的那句
`if (cr4_read_shadow() & X86_CR4_VMXE)`（对照 KVM
`arch/x86/kvm/vmx/vmx.c:2859-2860`）读的正是这个影子（`common.c:467-472` 导出）。
裸写 → 自己的 VMXE 在影子里不可见 → 第二次 `insmod` 拦不住；随后
`modprobe kvm_intel` 也拦不住（它查的同样是影子）→ 两个 VMXON 用户叠在同一台
CPU 上。

**修法**：改用 `cr4_set_bits()` / `cr4_clear_bits()`（`arch/x86/include/asm/
tlbflush.h:41` / `:51`），与 KVM 完全一致（`arch/x86/kvm/vmx/vmx.c:2837`、`:2848`，
`kvm_cpu_vmxoff()` 里 `:749`、`:753`）。调用点都在 `on_each_cpu()` 的 IPI 回调
（中断关闭），满足 `lockdep_assert_irqs_disabled()`。

#### (2) 上面这个错误的根因是一条假前提："cr4_set_bits() 没有导出给模块"

`main.c` 与 `vmx.c` 的注释都写着 *"`cr4_set_bits()`/`cr4_clear_bits()` 没有导出
给模块（见 6.8 的 Module.symvers，只有 `cr4_read_shadow` 导出），所以只能裸写
CR4"*。**假的**：这两个是 `asm/tlbflush.h` 里的 `static inline`，它们唯一的内核
调用 `cr4_update_irqsoff()` 在 `arch/x86/kernel/cpu/common.c:465` 有
`EXPORT_SYMBOL`。两条可复现的证据：

```
$ nm -u mini-kvm.ko | grep cr4
                 U cr4_read_shadow
                 U cr4_update_irqsoff
$ grep cr4_update_irqsoff /lib/modules/$(uname -r)/build/Module.symvers
0x0b637410	cr4_update_irqsoff	vmlinux	EXPORT_SYMBOL
```

对照：同一张表里 `vmcs_read*` / `vmcs_write*` 一个都没有 —— 那才是模块真的用
不了、必须自己包装 VMREAD/VMWRITE 的情形（`vmx.c:89` 那条注释是对的）。
教训：**"内核没导出所以只能裸来"这种前提，必须用 `Module.symvers` /
`nm -u` 验一次再写进注释**，它直接决定了要不要写一段危险代码。

#### (3) guest CR4 缺 VMXE（上一轮改对，本轮补全链条）

`MINI_GUEST_CR4` 原来只有 `X86_CR4_PAE`，注释还理由是"非根模式下 VMXE 该是
0"——架构上错的那句。VMX operation 含 root 与非 root 两种状态，§24.8 NOTE 要求
CR4.VMXE 在两种状态下都为 1；§27.3.1.1 对 guest CR4 字段直接做这条检查
（*"The CR4 field must not set any bit to a value not supported in VMX operation
(see Section 24.8)"* —— 该节起于 Vol.3C 27-8 页，CR4 这条 bullet 落在 27-9 页），
且同节只给 CR0.PE/CR0.PG 开了
unrestricted-guest 例外（*"Bit 0 … and bit 31 (PG) are not checked if the
'unrestricted guest' VM-execution control is 1"*），**CR4.VMXE 没有例外**。
漏掉的结果是 VM entry 直接失败，退出原因 33（§27.8）。KVM 侧同样强制：三个
`KVM_*_VM_CR4_ALWAYS_ON*` 常量都含 `X86_CR4_VMXE`
（`arch/x86/kvm/vmx/vmx.c:156-158`），`vmx_set_cr4()` 三条分支都会或上它
（`:3481-3487`），最后 `vmcs_writel(GUEST_CR4, hw_cr4)`（`:3528`）。
副作用（已写进注释）：本模块没开 "use CR4 shadows"，guest 读 CR4 会看到
VMXE=1；KVM 用 `CR4_READ_SHADOW` 字段（`:3527`）把它藏掉。

#### (4) §27.3.1.2 的两处过度概括（stage1 正文）

| 原文 | 规范实际怎么说（Vol.3C §27.3.1.2） |
|---|---|
| "要求 CS 可用且 `L=1, D=0`" | **没有任何一条要求 CS.L=1**。唯一的 L 相关检查是 *"For CS, D/B must be 0 if the guest will be IA-32e mode and the L bit (bit 13) in the access-rights field is 1"* —— 是"若 L=1 则 D 必须 0"。`IA-32e mode guest=1` 而 `CS.L=0` 合法，guest 以兼容模式开始执行；L=1 是本项目自己的选择 |
| "TR 可用且类型是 9 或 0xB" | IA-32e 下只允许一个值：*"If the guest will be IA-32e mode, the Type must be 11 (64-bit busy TSS)"*。3/11 之分属于**非** IA-32e 分支（3 = 16 位忙 TSS，11 = 32 位）；9 根本不是 TSS 类型（那是 32 位可用代码段） |

顺带补两条会真实咬人、原文没写的检查：**Type 为 9/11 时 CS.DPL 必须等于 SS 访问
权字段的 DPL**；**G 位与 limit 双向绑定**（*"If any bit in the limit field in
the range 11:0 is 0, G must be 0"* / *"If any bit in the range 31:20 is 1, G must
be 1"*）—— 本项目 CS limit=0xFFFFFFFF → 必须 G=1（`0xA09B` 的 bit15=1 ✓），
TR limit=0x67 → 必须 G=0（`0x8B` 的 bit15=0 ✓）。

#### (5) "entry 失败（CF=1）"这个判据只覆盖三档里的一档

`vmx_entry.S` 的注释与 stage1 都把失败写成 CF=1。按 §27.1/§27.2/§27.8，失败其实
分三档，**前两档都不装载宿主状态、都不产生 VM-Exit**，只是把控制交给下一条指令：

| 档 | 观测 | 错误号 |
|---|---|---|
| §27.1 基本检查第 3/4 条（无 current VMCS；当前是 shadow VMCS） | CF | 不写 |
| §27.1 第 5 条 a/b/c（MOV-SS 阻塞；launch state 与指令不匹配） | ZF | 写 |
| §27.2 控制域 / 宿主状态检查 | ZF | 写 |
| §27.3.1 guest 状态非法 / §27.4 MSR 装载失败 | **产生 VM-Exit**，exit reason 带 bit31 | — |

所以 `vmx_entry.S` 的正确判据是"**那条 VM 指令居然返回了**"（三条 fallthrough 到
同一个 `.Lvmfail`），CF/ZF 在这里分不开前两档，能分开的只有
`VM_INSTRUCTION_ERROR`。本轮据此把解码补上：`mini_vmx_report_error()` 按 §31.4
Table 31-1 解码 0/4/5/6/7/8/12/13/15/16/26，并新增被 `main.c` 的 VMXON
`vmfail` 分支复用（VMXON 在 root operation 里再执行 → 错误号 15，§31.3 伪码
*"ELSE VMfail("VMXON executed in VMX root operation")"*）；`vcpu.c` 的 bit31
分支按 §27.8 解码 basic 33/34/41 与 qualification 2/3/4。
Table 31-1 在 Vol.3C 31-31 页，本机 `pdftotext -layout` 抽取结果的 10897-10923
行是它的完整错误号列表，其中 15 号确认为 *"VMXON executed in VMX root operation"*。

一个顺带的事实对照：KVM 的 `kvm_cpu_vmxon()`（`arch/x86/kvm/vmx/vmx.c:2839-2843`）
只挂 `_ASM_EXTABLE` 的 fault 分支，**CF/ZF 根本不判**，VMfail 会被静默放过 —— 它
靠独占 VMX + 前置 CPUID/FEAT_CTL 检查兜底。mini-kvm 的定位是"和 `kvm_intel` 抢
同一台机器"，这两档必须收。

#### (6) 行号漂移：本轮两次全量重测

`main.c` 多了一行 `#include <asm/tlbflush.h>`、enable/disable 两个函数体改写；
`vmx.c` 的 host-CR4 与 §22.x 注释各加一行；`vcpu.c` 的 §27.8 解码块加了十几行。
结果是本节第一次写下的"对"列**自己又过期了** —— J10(a) 的教训第三次生效：
**改过被引用的源文件，必须重新 grep 定位，不能对旧行号做加减**。

下表是本轮最后一次全量重测的结果（`make` 之后逐条 grep；文件长度 `main.c` 504、
`vmx.c` 822、`vcpu.c` 793、`ept.c` 261、`device.c` 104 行）。凡 README、五篇
stage 与 J1–J11 里出现的本地行号，都已按这一列刷新过。

| 引用点 | 曾写 | 现为 |
|---|---|---|
| `mini_cpu_vmx_supported()` | `main.c:76` | `main.c:77` |
| `mini_cpu_vmxon()` | `main.c:129-155` | `main.c:130-157` |
| CR0/CR4 FIXED 打印 | `main.c:215-219` | `main.c:221-225` |
| `mini_vmx_enable_one()` | `main.c:236-278` | `main.c:264-300` |
| `mini_vmx_disable_one()` | （本轮新增） | `main.c:342-361` |
| VMXON 区域首页写 revision | `392` → `419` | `main.c:420` |
| "内核未导出 vmcs_read/write" 注释 | — | `vmx.c:89` |
| `mini_vmx_adjust_control()` | `vmx.c:249` | `vmx.c:251-260` |
| unconditional I/O exiting（J6） | `vmx.c:256-261` | `vmx.c:282` |
| `mini_compute_controls()` | `vmx.c:247-295` | `vmx.c:271-325` |
| `true_ctls` 选 MSR 那一行 | `vmx.c:249` | `vmx.c:273` |
| "次级控制只有一个 MSR" 注释 | `vmx.c:287-292` | 未变（同一区间） |
| EPT 协商回读校验 | `vmx.c:287-292` | `vmx.c:317-321` |
| `MINI_GUEST_CR4` 定义 / 使用 | `vmx.c:553` | `vmx.c:554` / `:570` |
| `mini_vmx_set_guest_state()` | `460-521` → `560-637` | `vmx.c:561-638` |
| FS/GS base 清零 | `508-509` → `617-618` | `vmx.c:618-619` |
| VMCS 首页写 revision | `667` → `691` | `vmx.c:692` |
| `EXCEPTION_BITMAP` 写入（stage3/J10） | `vmx.c:600-601` | `vmx.c:718-719` |
| `mini_vcpu_run_loop()` / `for (;;)` | `vcpu.c:62` / `132` | 未变 |
| 循环头 + 刷新 Host（stage5） | `138-156` → `138-157` | 未变（`138-157`） |
| 注入窗口代码（stage3） | `146-156` → `147-157` | 未变（`147-157`） |
| gs_shadow + 关中断窗口（stage5） | `187-192` → `188-193` | 未变（`188-193`） |
| 进入失败分支（stage5） | `194-202` → `195-203` | `vcpu.c:202-210` |
| `launched` 置位（stage5） | `203-204` → `204-205` | `vcpu.c:211-212` |
| bit31 entry-failure 拦截（stage5） | `210-219` → `211-220` | `vcpu.c:229-267` |
| NMI 转注分支（stage3） | `229-236` → `230-237` | `vcpu.c:277-284` |
| 收尾 `pr_debug`（stage3/stage5/J4） | `318-322` / `319-322` | `vcpu.c:366-369` |
| `EPT_ENTRY_RWX` / `EPT_MEMTYPE_WB` | `ept.c:59` / `61` | `ept.c:64` / `:66` |
| `mini_ept_init()`（stage2 来源块） | `ept.c:136-152` | `ept.c:137-159` |
| EPTP 构造（J2） | `ept.c:143-152` | `ept.c:147-157` |
| `mini_ept_walk()`（stage2 来源块） | `ept.c:187-222` | `ept.c:192-227` |
| `mini_ept_handle_violation()`（stage2） | `ept.c:228-255` | `ept.c:233-261` |
| GPA 取自 `GUEST_PHYSICAL_ADDRESS`（J1） | `ept.c:235` | `ept.c:240` |
| GPA 越界 `-EFAULT`（stage2/stage4/README/J4） | `ept.c:238-243` | `ept.c:243-248` |
| `mini_serial_out()`（stage4） | `device.c:31-54` | `device.c:33-56` |
| `mini_handle_io_exit()`（stage4/J3） | `device.c:59-102` | `device.c:61-104` |

`interrupt.c` 的四处（`55-71`、`59-63`、`91-105`、`96-99`）、`guest/guest.S:39-62`、
`test-mini-kvm.c:9-19`、`vmx_entry.S:78`（= `SYM_FUNC_START(mini_vmx_enter)`）本轮
没有改动，重测仍然对得上。

> ⚠️ 本表与上一句话都只对 J11 当时成立。下一轮（J12）又动了 `vcpu.c`、`device.c`
> 与 `vmx_entry.S` 的注释，`vcpu.c:202-210 / 211-212 / 229-267 / 277-284 / 366-369`、
> `device.c:33-56 / 61-104`、`vmx_entry.S:78` 全部再次漂移。当前有效值一律以
> **J12(7)** 的重测表为准 —— 这就是 J10(a′) 说的"漂移是本项目最容易复发的错"。

另外两处引用纪律的修正：`main.c:215-219` 那句原来写"打印**实测**的 CR0/CR4
FIXED0/1"，本机 MSR 一个都读不到，措辞改成"运行时读到"；`vmx.c:287` 附近原来把
"次级控制只有一个 MSR" 的出处写成 *SDM Appendix A.3.3*，附录不在本仓库这份 PDF
（只含 Vol.3C）里，改以内核头文件 `arch/x86/include/asm/msr-index.h:1182-1200`
的编号清单为据（`TRUE_*` 只有 0x48d-0x490 四个）。同理，`stage1` 里"能力 MSR
高低 32 位"的说法补成"正文只在 §24.8/§25.x 转引，逐位定义在未收录的 Appendix
A.3-A.5"。

#### (7) 引用核对工具化 + 这一趟又抓出的五处

J10(a′) 说"这类引用要么写符号名，要么用脚本机械核对"，但脚本当时是 `/tmp` 里的
一次性产物 —— 于是漂移第三次发生。本轮把它落成仓库里的
`practice/mini-kvm/check-refs.py`：扫 README 与五篇 stage 共 125 条、
`corrections.md` 共 163 条 `file:line`，逐条解析成本目录或 6.12.93 里的真实文件，
**行号越界或文件找不到就非零退出**，再把被引用的那几行原文打出来供人工判上下文。
现在两处都是 0 问题。stage1 的"不加载模块也能做的静态自检"加了这第 4 条。

全量重测顺带暴露 README 的歧义判据不够用：本目录 `vmx.c` 已经 822 行，
"行号 4xxx 必然是内核树"这种量级提示只在超出本地文件长度时成立。README 第 1 节
的规则据此收紧成"**行号落在本目录同名文件长度之内就必须写全路径**"，并照这一条
把五处裸 KVM 锚点改写成 `arch/x86/kvm/vmx/vmx.c:…`：`main.c` 头注释里的
`:743-755`、`mini_vmx_disable_one()` 上方的 `:743-755`/`:735-741`、本模块 `vmx.c`
里的 `:788-789` 与 `:809`、README 第 2 节表格与第 9 节的 `vmx.c:234`。

四处引用本身写错或写过头，都已改：

| 位置 | 错 | 依据 |
|---|---|---|
| 本模块 `vmx.c` §22.x 注释 | 只列 `vmx.c:4328`、`:4335`、`:4340` 就说覆盖 `"22.2.3"/"22.2.4"` | `:4340` 是 `/* 22.2.3, 22.2.5 */`，**22.2.4 在 `arch/x86/kvm/vmx/vmx.c:4343`**（`HOST_CS_SELECTOR`） |
| `mini_host_cr4()` 注释 | 说 KVM 只在 `:4338-4341` 写一次 HOST_CR4 | KVM 每次 VM entry 前还会 `cr4 = cr4_read_shadow(); vmcs_writel(HOST_CR4, cr4)`（`:7410-7413`）；且 KVM 的 HOST_CR4 **不含**额外的 `\| X86_CR4_VMXE`，它靠 `cr4_set_bits()` 已把这一位写进影子（`:2837`）—— 本模块那次 `\|` 只是保险，不是 KVM 的对等物 |
| 本模块 `vmx.c` 迁移注释 | "KVM 紧跟 `vmcs_clear()` 写 `launched = 0`" | VMCLEAR 在 `arch/x86/kvm/vmx/vmx.c:793`，`loaded_vmcs->launched = 0` 在 `:809`，中间隔着 `smp_wmb()`；两者都在 `__loaded_vmcs_clear()` 里，不是 `vmcs_clear()` 干的 |
| README 第 1 节 | 把 §22.x 的出处指向"corrections.md J10/J11" | J10/J11 从没讨论过这个编号；真实依据是 KVM 源码注释本身（`arch/x86/kvm/vmx/vmx.c:4328` 等）与本模块 `vmx.c` 的那段说明，指针已改对 |

两处规范页码的写法收紧（避免把抽取文本的行号当成 PDF 的东西）：§27.3.1.1 起于
Vol.3C 27-8 页、guest CR4 那条 bullet 落在 27-9 页；Table 31-1 在 31-31 页，
"10897-10923"是**本机 `pdftotext -layout` 抽取结果**的行区间，不是卷内行号。
本模块 `vmx.c` 里 msr-index.h 的范围写漏一行：`IA32_VMX_BASIC` 在 1182（不是
1183），与上文 J11(6) 的 `1182-1200` 对齐。

### J12. 用户态边界与世界切换的静态审计轮：一处会当场 oops 的栈偏移、一处 VM-Exit 风暴、四处过度声称

本轮范围仍然是**不加载模块**（约束见 `practice/mini-kvm/README.md` 第 4 节：本机
是共享裸金属，卸载 `kvm_intel` 会打断宿主上所有 VM），手段是 `make` +
`objdump`/`nm`/`grep` + 逐条读源码与规范。下面 (1)(2) 两条是**会打死宿主**的缺陷，已同时补进 `practice/mini-kvm/README.md` 第 7 节（该节从三条扩到五条）。所有"会怎样"的判断都标成静态推演 +
上游代码佐证，没有任何一条声称是本机实测到的运行时行为。

#### (1) VM-Exit 落地点按 `8(%rsp)` 取 `@regs` —— 把 `launched` 的值当指针解引用

`vmx_entry.S` 的退出路径原本是：

```asm
push %rax
mov  8(%rsp), %rax        /* ← 错 */
pop  REG_RAX(%rax)        /* 往 %rax 指向的地址写 8 字节 */
```

栈上的布局由**我们自己写进 `HOST_RSP` 的那个 `%rsp`** 决定。入口序言依次压入
`rbp / r15..rbx / rdi(vcpu) / rsi(launched)`，随后 `mov %rsp,%rax; vmwrite %rax`
（`HOST_RSP` 字段编码 `0x6c14`，见 `arch/x86/include/asm/vmx.h:381`），所以
`HOST_RSP` 指向的**最低一格是 `launched`**，`vcpu` 在它上面一格。硬件在 VM-Exit
时把 `%rsp` 恢复成 `HOST_RSP`，于是：

| | `[rsp+0]` | `[rsp+8]` | `[rsp+16]` |
|---|---|---|---|
| 入口路径（`vmresume` 返回） | `launched` | `vcpu` | — |
| 退出路径（刚 `push %rax` 暂存 guest RAX） | 暂存 RAX | `launched` | `vcpu` |

入口路径按 `8(%rsp)` 取 `vcpu` 是对的；退出路径多了一格暂存，同一个偏移取到的是
**`launched`（0 或 1）**，紧接着 `pop REG_RAX(%rax)` 就是往 `0x0 + offsetof(regs,
REG_RAX)` / `0x1 + ...` 写 8 字节 —— 内核态解引用接近 NULL 的地址，第一次退出
当场 #PF/oops。这不是"guest 数据写坏宿主内存"，是"把自己的栈槽位当成指针"。

**为什么照抄 KVM 会抄错**：KVM 的 `mov WORD_SIZE(%_ASM_SP), %_ASM_AX`
（`arch/x86/kvm/vmx/vmenter.S:203`，`WORD_SIZE` 定义在 `:12`）之所以是 8，是因为
KVM 的 `HOST_RSP` 指向**它自己压的最后一格，而那格就是 `@regs`**：`push
%_ASM_ARG2` 在 `:103`，紧接着 `lea (%_ASM_SP),%_ASM_ARG2; call
vmx_update_host_rsp` 在 `:108-109`（`vmx_update_host_rsp()` 本体在
`arch/x86/kvm/vmx/vmx.c:7231-7237`），所以暂存 RAX 之后 `@regs` 恰好在上面一格。
本模块多压了 `launched`/`vcpu` 两格，差的是**两格不是 8 字节**。

修法是把偏移写成宏并让两条路径各自显式引用，杜绝"同一个数字在两处含义不同"：
`STACK_LAUNCHED`(=0) / `STACK_VCPU`(=8) 在 `vmx_entry.S:58-59`，入口路径用
`STACK_VCPU(%rsp)`（`:119`），退出路径用 `STACK_VCPU+8(%rsp)`（`:172`，= 16），
`SYM_INNER_LABEL_ALIGN(mini_vmx_vmexit, …)` 在 `:158`。反汇编是这一条唯一的判据：

```
$ make && objdump -d --no-show-raw-insn mini-kvm.ko | sed -n '/<mini_vmx_vmexit>:/,/pop/p' | head
  90:	push   %rax
  91:	mov    0x10(%rsp),%rax      ← 修复前是 0x8
  96:	pop    (%rax)               ← regs[REG_RAX]，偏移 0
```

`git log -S 'mov  8(%rsp), %rax'` 显示这条错跟着拆分提交 `5ca02c1` 进来。拆分前的
单文件版本（`examples/mini-kvm/mini-kvm.c`）**根本没有独立的世界切换汇编**：
`HOST_RIP` 直接指向 C 函数里的 `vmx_exit_handler:` 标签，`asm volatile("vmresume")`
裸写在函数体中，一个宿主 GPR 都不保存，也就没有 `@regs` 数组可取 —— 所以这条
偏移是"新写的汇编第一次落地就带着的"，不存在"以前是对的、被改坏了"。同一段偏移的推演见 stage1「不加载模块也能做的静态自检」第 4 条与 `vmx_entry.S:165-171` 的注释。

#### (2) `sti` 紧跟 `cli`：一个中断都不消费，然后就是 VM-Exit 风暴

外部中断分支原本是：

```c
vcpu->n_extint_exits++;
local_irq_enable();
local_irq_disable();
continue;
```

`sti` 的**中断影子（interrupt shadow）要到下一条指令执行完**才解除，紧跟一条
`cli` 等于刚推开的门又关上 —— pending 在 LAPIC IRR 里的那个向量一个都没被
消费。而本模块**没有**开 `VM_EXIT_ACK_INTR_ON_EXIT`（SDM 28.1：ack=0 时
"the interrupt remains pending"，且 `VM_EXIT_INTR_INFO` 无效，28.2.2），于是
下一次 VM entry 立刻又以原因 1 退出，形成**本地循环**：guest 不再推进，宿主这个
CPU 的 tick 永远进不来（soft lockup / RCU stall）。这不是"性能差一点"，是把宿主
的一个 CPU 锁死在退出循环里。

上游的同一段代码已经把理由写在注释里：KVM 也是 `local_irq_enable()` +
`local_irq_disable()`，中间夹一条 `++vcpu->stat.exits`，注释原文
*"An instruction is required after local_irq_enable() to fully unblock interrupts
on processors that implement an interrupt shadow, the stat.exits increment will do
nicely"*（`arch/x86/kvm/x86.c:11149-11158`，三条语句分别在 `:11156`/`:11157`/
`:11158`）。mini-kvm 用同一条占位：把 `vcpu->n_extint_exits++` 移进窗口
（`vcpu.c:290-292`）。

两点补充，都是查出来的而不是推的：

- 计数器自增会不会被编译器挪出窗口？不会。`native_irq_enable()` /
  `native_irq_disable()` 是带 `"memory"` clobber 的 `asm volatile("sti"/"cli")`
  （`arch/x86/include/asm/irqflags.h:35-43`），窗口两边就是完整的内存屏障点，
  对 `vcpu->...` 的写不可能跨过去重排。
- **"中断影子"这条规则的原文不在本仓库这份 PDF 里。** `intel-vmx.pdf` 只含
  Vol.3C（24-33 章），STI 的 `IF` 影子定义在 Vol.3A §22.x，本地无法复核，所以
  stage3 与代码注释都**不写章节号**，只写"规则 + KVM 注释为证"（README 第 1 节
  的编号纪律）。

#### (3) `device.c` 声称"这里拿 sleeping lock 合法"——不合法

上一轮给 `mini_serial_out()` 写的上下文注释是："运行循环的 IO 分支显式开了中断
（对照 KVM 的 `handle_exit()`），所以这里 printk 与拿 sleeping lock 都合法"。
后半句是错的，而且错在把 KVM 的一半抄了过来：

| | 中断 | 抢占 | 结论 |
|---|---|---|---|
| KVM `vcpu_enter_guest()` 退出后 | `local_irq_enable()` `x86.c:11170` | `preempt_enable()` `x86.c:11171` | 完全回到进程上下文，**可以睡** |
| mini-kvm 运行循环 | IO 分支的窗口里开 | **全程 `preempt_disable()`**（`vcpu.c:74` → `:383`） | 仍属原子上下文，不能拿 sleeping lock、不能阻塞分配 |

也就是说 mini-kvm 只放开了 KVM 那两件事里的一半。顺带把 ept.c 只用 `GFP_ATOMIC`
的理由挂在同一处（不是"怕关中断"，是因为整个循环都不可睡）。`printk` 本身仍然
合法，但要用得上的证据说：控制台拿不到锁时走的是 trylock 路径
`down_trylock_console_sem()`（`kernel/printk/printk.c:315-334`），阻塞版
`down_console_sem()` 在 `:310-313` 且只在不处于原子上下文时被选到 —— 记录先进
lockless 的 log buffer，刷新推迟。

#### (4) 串口缓冲的三处过度声称

| 原话 | 实际 | 依据 |
|---|---|---|
| "环形缓冲" | **线性**缓冲，写满 `MINI_KVM_SERIAL_SIZE - 1`（=511）后新字节静默丢弃 | `device.c:54-55` 的 `if (kvm->serial_len < MINI_KVM_SERIAL_SIZE - 1)`；`mini-kvm.h:33` |
| "GET_SERIAL 可以把**整段**缓冲取回去" | `_IOR('M',0x01,char[256])` 只有 256 字节，且 `memcpy` 上限写死 255 → **只能拿到前 255 字节** | `mini-kvm.h:95`、`vcpu.c:699-709` |
| "写者持 `kvm->lock` 串行化"（隐含） | 写者**不持任何锁**：唯一写者是运行循环，靠 `vcpu->mutex` 天然单写者；`GET_SERIAL` 那把锁只互斥读者，所以并发读会看到撕裂的一帧（但不会 UAF：`struct file` 的引用计数保证 `filp->private_data` 在这次 ioctl 期间不被 `close()` 释放） | `vcpu.c:408`、`:703-705` |

`test-mini-kvm` 的 guest 总共只打两条字符串（`guest/guest.S:133`/`:135`）：
27 + 19 = 46 字节，所以 255 截断不影响验收；
这一点写进 stage4 的正文，避免读者以为"缓冲多大就能取回多少"。

#### (5) `mmap` / `KVM_GET_VCPU_MMAP_SIZE` 的对照原先只有一句空壳

原来 `mini_vcpu_mmap()` 上面的注释只写"对照 `kvm_vcpu_mmap()`：把 kvm_run 页映射
给用户态"，两处都是不完整的：

- 真实 KVM 的 `kvm_vcpu_mmap()`（`virt/kvm/kvm_main.c:4142-4154`）**不做任何映射**，
  只装一个 `vm_ops`；真正建映射的是按 pgoff 分派的 `kvm_vcpu_fault()`
  （`:4112-4136`：0 = `kvm_run`、`KVM_PIO_PAGE_OFFSET`、
  `KVM_COALESCED_MMIO_PAGE_OFFSET`、dirty ring 各页，其余落到
  `kvm_arch_vcpu_fault()`）。
- `KVM_GET_VCPU_MMAP_SIZE` 在 x86 上返回的**不是一页而是三页**（run + pio data +
  coalesced MMIO 环，`virt/kvm/kvm_main.c:5552-5561`）。用户态如果按"页"去 mmap
  真实 KVM 的 vCPU fd，是拿不到全部窗口的。
- 反面：越界 pgoff 在 x86 上得到的是 `VM_FAULT_SIGBUS`
  （`arch/x86/kvm/x86.c:6344-6347` 就这一句 `return VM_FAULT_SIGBUS;`），
  mini-kvm 因为一次性 `remap_pfn_range()`，只能在 `vcpu.c:442-445` 用 `-EINVAL`
  提前挡（`vm_pgoff != 0` 或长度不是一页）。

两条已补进 `vcpu.c` 的注释与 README 第 5 节的表格。

#### (6) ioctl 编号是手抄两份的，改一侧不会编译报错

`MINI_KVM_VM_GET_SERIAL` / `MINI_KVM_VCPU_INJECT_IRQ` 两个宏在 `mini-kvm.h:95`、
`:98` 与 `test-mini-kvm.c` 各有一份（用户态程序不能 include 内核头）。编号差一位
的失败模式是静默的 `-ENOTTY`，所以 README 第 5 节把实测编码写死作为锚点：
`_IOR('M',0x01,char[256])` = **`0x81004d01`**、`_IOW('M',0x02,int)` =
**`0x40044d02`**（用一个 `gcc -include linux/ioctl.h` 的三行宿主程序算出，与
`KVM_RUN`=`0xae80`、`KVM_CREATE_VM`=`0xae01`、`KVM_CREATE_VCPU`=`0xae41` 同法）。
另外 `KVM_RUN` 的 `arg` 非 0 必须拒（对照 `virt/kvm/kvm_main.c:4474-4475`），本模块
在 `vcpu.c:412-415` 已经照做。

测试程序的失败路径同时补了信息量：`run_until_exit()`（`test-mini-kvm.c:97-117`）
原来只 `CHECK(r == 0, "KVM_RUN")`，errno 之外什么都不印；现在连 `errno` 和
`run->exit_reason` 一起打印并提示看 dmesg —— 内核侧每条失败路径都会先填好
`exit_reason` 再返回。

#### (7) 行号漂移：本轮重测（第三次记录同一件事）

本轮动过 `vcpu.c`（793 → 829）、`device.c`（104 → 120）、`vmx_entry.S`（226 → 248）、
`test-mini-kvm.c`（207 → 219）。当前文件长度：`main.c` 504、`vmx.c` 822、`vcpu.c` 829、
`ept.c` 261、`interrupt.c` 105、`device.c` 120、`vmx_entry.S` 248、`mini-kvm.h` 300、
`test-mini-kvm.c` 219。

| 引用点 | J11 表里的"现值" | 现为 |
|---|---|---|
| `mini_vcpu_run_loop()` | `vcpu.c:62` | `vcpu.c:67` |
| `preempt_disable()` | `vcpu.c:69` | `vcpu.c:74`（配对的 `preempt_enable()` 在 `:383`） |
| 上机段（stage5 来源块） | `vcpu.c:69-130` | `vcpu.c:74-137` |
| 循环头 + 刷新 Host | `vcpu.c:138-157` | `vcpu.c:143-162` |
| 注入窗口（stage3 来源块） | `vcpu.c:147-157` | `vcpu.c:152-162` |
| `for (;;)` | `vcpu.c:132` | `vcpu.c:137` |
| gs_shadow + 关中断窗口 | `vcpu.c:188-193` | `vcpu.c:193-197`（RFLAGS 注释在 `:198-205`） |
| 进入失败分支 | `vcpu.c:202-210` | `vcpu.c:207-215` |
| `launched` 置位 | `vcpu.c:211-212` | `vcpu.c:216-217` |
| bit31 entry-failure 拦截 | `vcpu.c:229-267` | `vcpu.c:234-272` |
| 外部中断窗口（含新注释） | — | `vcpu.c:275-293` |
| NMI 转注分支 | `vcpu.c:277-284` | `vcpu.c:295-302` |
| 收尾 `pr_debug` | `vcpu.c:366-369` | `vcpu.c:384-387` |
| `mini_serial_out()` | `device.c:33-56` | `device.c:49-72`（上下文注释 `:31-48`） |
| `mini_handle_io_exit()` | `device.c:61-104` | `device.c:77-120` |
| `SYM_FUNC_START(mini_vmx_enter)` | `vmx_entry.S:78` | `vmx_entry.S:92` |

未动的文件（`main.c`、`vmx.c`、`ept.c`、`interrupt.c`、`guest/guest.S`）本轮逐条
重打原文核对，全部仍然对得上。核对手段仍然是脚本，且这轮给它补了两件事：

```bash
./check-refs.py --quiet                       # README + 五篇 stage：134 条，0 问题
./check-refs.py --quiet --kernel --src        # 再解析内核树 + 本模块 .c/.S：262 条，0 问题
./check-refs.py --quiet --kernel ../../corrections.md   # 228 条，0 问题
```

- 新增 `--src`：把被引用到的**本模块源文件**也一并解析打印，否则"只改文档没改
  代码"和"两边都改了"这两种漂移在输出里长得一样。
- 修掉一个自伤 bug：越界分支把"被引用文件的行数"赋给了累计计数器 `total`（现改名
  `nlines`），一次越界会让统计总数变成最后那个文件的长度。
- `--quiet` 下每条裸引用都刷一行"歧义"是噪音（63 行），改成在结尾汇总一次
  （"N 条裸引用按内核树解析"）。
