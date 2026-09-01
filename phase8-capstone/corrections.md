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
mini-kvm 同位置见 `ept.c:235`。

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
mini-kvm 两侧都不开这些能力，`ept.c:143-152` 的 EPTP 因此不带
`VMX_EPTP_AD_ENABLE_BIT`。

### J3. IO 退出的 `EXIT_QUALIFICATION` 布局缺字段（stage4）

**原文**只列了 size / direction / string / port 四项，且把串口代码写成
"先解码再判 string"。

**正确**（SDM Table 28-5）：bits 2:0 size−1（合法编码只有 0/1/3，4 与 5 是
保留）、bit 3 方向（0=OUT，1=IN）、bit 4 string、bit 5 REP prefixed、
bit 6 操作数编码（0=DX，1=立即数端口）、bits 15:7 未定义、bits 31:16 端口号、
bits 63:32 未定义。指令长度**不在**这个字段里，在
`VM_EXIT_INSTRUCTION_LEN`（`0x440c`，SDM 28.2.4）。实现侧 `device.c:59-102`
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
- `RUN 结束 …` 统计行是 `pr_debug`（`vcpu.c:319-322`），不开 dynamic debug
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
bit 24（本模块 `vmx.c:256-261`）。

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
`guest/guest.S:39-62`、本模块 `vmx.c:600-601`、`test-mini-kvm.c:9-19` 都按此
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
「本模块 `vmx.c:…`」，内核侧写全路径。

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
