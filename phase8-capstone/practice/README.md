# Phase 8 毕业建造 — 最小 VMM 实践

> 不借助 QEMU/Firecracker，直接用 `/dev/kvm` ioctl 造一个能启动 Linux 的
> VMM（`minivmm.c`），再逐步加 virtio-mmio、VFIO 直通，最后与 QEMU 做
> 性能对标。本页记录**实测结果**；设计依据与陷阱见
> `../project1..4-*.md` 与 `../corrections.md`。

---

## 📋 进度

| 项目 | 内容 | 状态 |
|------|------|------|
| 1 | 可启动的最小 VMM（bzImage → shell，16550A 串口） | ✅ 实测通过 |
| 2 | 自制 virtio-mmio console + blk（`KVM_IRQ_LINE` 中断） | ✅ 实测通过 |
| 3 | VFIO 直通 `4b:00.0`（MP 表/VFIO/PCI 枚举/盘 I/O + MSI-X Posted 投递） | ✅ 实测通过 |
| 4 | 启动延迟 / VM-Exit 分布 / halt-polling 对标 QEMU | ✅ 实测通过 |

---

## 🔧 编译和运行

```bash
cd phase8-capstone/practice
make                      # gcc -O2 -g -Wall -Wextra -o minivmm minivmm.c -lpthread

# 交互启动（Ctrl-C 退出，SIGUSR1 打印 exit 统计）
sudo ./minivmm -k ../../scripts/images/bzImage \
               -i ../../scripts/images/initramfs.img -m 512

# 自动化（guest /init 打印 MINIVMM_READY 后 reboot，VMM 报告启动耗时后退出）
sudo ./minivmm -k ../../scripts/images/bzImage \
               -i ../../scripts/images/initramfs.img -m 512 \
               -c "console=ttyS0 earlyprintk=serial rdinit=/init autotest" < /dev/null
```

依赖的 guest 镜像由 `scripts/vm/` 生成：`./build-kernel.sh`（已含
`CONFIG_KVM_GUEST` / `CONFIG_VIRTIO_MMIO` / `CONFIG_EXT4_FS`，见
`scripts/vm/kernel-config`）+ `./build-rootfs-minimal.sh`（busybox initramfs，
`/init` 挂 proc/sys/devtmpfs，cmdline 带 `autotest` 时打印就绪标记并 reboot）。

> ⚠️ 用 `scripts/images/bzImage`，不是仓库根 `images/` 下的旧产物。

---

## 项目 1：实测结果

**环境**：宿主为裸金属（96 线程，内核 6.8.0-51-generic；2026-09-01 用
`systemd-detect-virt`=none 与 CPUID 无 hypervisor 位复核——此前"宿主是
KVM guest"为笔误，见 `corrections.md` H 节），guest 512 MB、单 vCPU、
内核 6.12.93-kvm-study。

### 启动判据（autotest 模式，一次典型运行）

| 里程碑 | 相对 VMM 进程启动 |
|--------|------------------|
| 首个发往宿主的串口字节 | **+33.6 ms** |
| guest 就绪（`MINIVMM_READY`） | **+1308.7 ms** |

一次完整 boot 的用户态 exit 计数：`IO=38625`（几乎全是 8250 寄存器
PIO；项目 4 会拆解）。

### guest 侧关键 dmesg（均为实测抓取）

```
Hypervisor detected: KVM
kvm-clock: Using msrs 4b564d01 and 4b564d00
clocksource: Switched to clocksource kvm-clock
...
clocksource: Switched to clocksource tsc          # invariant TSC，见下
serial8250: ttyS0 at I/O 0x3f8 (irq = 4, base_baud = 115200) is a 16550A
Run /init as init process
```

- **最终 clocksource 是 tsc，不是 bug**：宿主暴露 CONSTANT_TSC +
  NONSTOP_TSC，`kvmclock_init()` 把 kvm_clock rating 自降到 299
  （`kvmclock.c:342-345`）< tsc 的 300（`tsc.c:1189`）。kvm-clock 仍在
  提供 sched_clock / pvclock。
- **无 `[Firmware Bug]: APIC ID mismatch`**：leaf1 EBX[31:24] 已归零。

### `/proc/interrupts`（两次采样，相隔 ~2 s）

```
           CPU0
  0:        803    XT-PIC      timer      →  1304   (~250 Hz, CONFIG_HZ=250)
  4:        112    XT-PIC      ttyS0      →   253
LOC:          0   Local timer interrupts      （恒 0，见下）
```

本项目无 MP 表 / ACPI MADT，guest 走 **virtual-wire + in-kernel 8259**，
本地定时器不经过 LAPIC，所以 `LOC` 恒 0 属正常；定时器中断体现在
IRQ0 `XT-PIC timer`。`LOC` 要等项目 3 装上 IOAPIC 后才会涨。

### 让 8250 驱动认成 `16550A` 且 `irq = 4` 的五要素

只做到"LSR 读回 TX-empty"会得到 `irq = 0` / `is a 16450`。`minivmm.c`
按 `include/uapi/linux/serial_reg.h` 实现了（行号见
`../project1-minivmm-boot.md` M4）：

1. LSR TX-empty（`THRE|TEMT`）；
2. **THRI 中断**——`autoconfig_irq()`（`8250_port.c:1305`）靠它反查 IRQ；
3. **IIR[7:6]=0b11**——`autoconfig()` FIFO 类型 switch（`8250_port.c:1241`）；
4. **loop 测试 MSR 回环映射**（OUT2→DCD、RTS→CTS，`8250_port.c:1215-1219`）；
5. IER 只实现低 4 位（`8250_port.c:1175`）。

### guest 复位 → VMM 干净退出

模拟了两条硬复位路径，guest `reboot -f` 能真正收掉 VMM（否则只能靠
timeout）：i8042 命令寄存器 `0x64` 写 `0xFE`（`reboot.c:667`）与 CF9
端口 `0xcf9` 写 bit1（`reboot.c:698`）。实测 `reboot -f` 后 VMM 打印
`guest 复位请求 (port 0x64 val 0xfe)` 并退出。

### CPUID 三处必改（相对 `KVM_GET_SUPPORTED_CPUID` 原样透传）

| leaf1 字段 | 处理 | 原因 |
|-----------|------|------|
| ECX bit24 `TSC_DEADLINE_TIMER` | 保留 | 缺则 `timer_mode_mask=1<<17`，deadline 设不上（`cpuid.c:399-402`） |
| ECX bit31 `HYPERVISOR` | **置位** | `KVM_GET_SUPPORTED_CPUID` 不含它（实测 ECX=`0x76fab223`），缺了 PV 全灭（`kvm.c:877`） |
| ECX bit21 `x2APIC` | 清零 | 第一版保持 xAPIC MMIO |
| EBX[31:24] 初始 APIC ID | 归零 | 与 in-kernel LAPIC ID 0 对齐，否则 APIC ID mismatch（`topology_common.c:174-176`） |
| EBX[23:16] 逻辑处理器数 | 改 1 | 宿主 96 对单 vCPU 无意义 |

KVM 半虚拟化叶 `0x40000000/1` 宿主已填好（feature 叶 eax=`0x01007efb`），
照抄即可，不要覆盖成最小集。

---

## 项目 2：virtio-mmio console + blk

实现：`minivmm.c` 里的 virtio-mmio（**modern / VERSION=2**）设备模型。
寄存器偏移对齐 `include/uapi/linux/virtio_mmio.h`，split virtqueue 布局对齐
`include/uapi/linux/virtio_ring.h`。feature 只谈 `VIRTIO_F_VERSION_1`(bit32)。
中断目前用 `KVM_IRQ_LINE` 电平模型（置位 → guest 读 `INTERRUPT_STATUS`、
写 `INTERRUPT_ACK` → 清位落线），M3 再换 irqfd/ioeventfd。

**设备发现**：VMM 在内核 cmdline 后自动追加
`virtio_mmio.device=0x200@0xd0000000:5 virtio_mmio.device=0x200@0xd0000200:6`
（`vm_cmdline_set()` @ `drivers/virtio/virtio_mmio.c:718` 解析）。GPA 取
`0xd0000000` 起的空洞——在 512MB RAM 之外、e820 不声明，访问才触发 EPT
violation → `KVM_EXIT_MMIO`。实测 guest dmesg：

```
virtio-mmio: Registering device virtio-mmio.0 at 0xd0000000-0xd00001ff, IRQ 5.
virtio-mmio: Registering device virtio-mmio.1 at 0xd0000200-0xd00003ff, IRQ 6.
virtio_blk virtio1: [vda] 131072 512-byte logical blocks (67.1 MB/64.0 MiB)
```

GSI 5/6 能送达是因为默认路由表把 GSI 0-15 同时挂到 IOAPIC 与 8259
（`ROUTING_ENTRY2` @ `irq_comm.c:375-385`），本项目 guest 走 virtual-wire +
8259，`KVM_IRQ_LINE` 经 PIC pin 投递。

### virtio-blk：desc→avail→后端→used→中断 全链路（实测）

后端是宿主普通文件（`-d <img>`，缺省自建 64MB），`pread/pwrite` 实现
`VIRTIO_BLK_T_IN/OUT/FLUSH`（`virtio_blk.h:165/166/174`），请求头
`virtio_blk_outhdr` 用 `chain_read` 跨段取，末段 1 字节写状态
（`VIRTIO_BLK_S_OK/IOERR/UNSUPP` = 0/1/2，`virtio_blk.h:317-319`）。

用宿主预置的 ext4 镜像（`mkfs.ext4` + 塞一个文件）经 `-d` 传入，guest 内实测：

```
# mount -t ext4 /dev/vda /mnt        → EXT4-fs (vda): mounted ... r/w ordered
# cat /mnt/from-host.txt             → host-file-content      (T_IN 读)
# echo guest-wrote-this > /mnt/from-guest.txt                 (T_OUT 写)
# umount /mnt                        → 正常
```

卸载后宿主 `mount -o loop` 该镜像能读到 `guest-wrote-this`，证明写路径真正
落盘。一次 boot 的 exit 计数：`IO≈39765 MMIO≈342`（MMIO 即 virtio 寄存器
访问 + QUEUE_NOTIFY；M3 会把 notify 相关退出降下去）。

### virtio-console：echo 模型（实测双向）

- **transmit(q1) → 宿主 stdout**：`echo hello > /dev/hvc0` 直接在宿主终端
  显示（guest→host）。
- **receive(q0) ← echo**：把发送过的字节回灌进 receive 队列，演示 host→guest。
  注意 hvc 的 tty 缓冲只在**有读者挂着**时才留得住数据，boot 早期投递的输入
  晚开的读者读不到；所以双向测试要先挂一个读写 fd：

```sh
exec 3<>/dev/hvc0; stty -echo <&3     # 先开读者，关 tty 回显避免自激
echo PING >&3                          # 宿主终端出现 PING（guest→host）
read -r line <&3; echo "recv=[$line]"  # recv=[PING]（echo 经 receiveq 回来）
```

实测 `recv=[PING]`，收发两个方向的 virtqueue 都通。

### M3：ioeventfd + irqfd 数据面下沉（实测）

`-e` 打开后：
- `KVM_IOEVENTFD`（`kvm_ioeventfd()` @ `virt/kvm/eventfd.c:999`）按
  `datamatch=队列号` 把各队列 `QUEUE_NOTIFY` 绑到 eventfd——guest 写 notify
  **不再触发 `KVM_EXIT_MMIO`**，由一个 epoll worker 线程收 eventfd 后跑
  `service_vq()`。
- `KVM_IRQFD`（`kvm_irqfd()` @ `eventfd.c:579`）把每个设备的 GSI 绑到
  eventfd，`virtio_raise_irq()` 改成写 eventfd；无 resample 时内核自动做一次
  assert→deassert 脉冲（`eventfd.c:48-52`），正好匹配 PIC 边沿中断。

同一负载（boot + `dd if=/dev/vda of=/tmp/ddout bs=512 count=512`）实测：

| 模式 | IO（串口） | MMIO |
|------|-----------|------|
| 默认（`KVM_IRQ_LINE`，notify 走 MMIO 退出） | 42919 | **360** |
| `-e`（ioeventfd + irqfd） | 42920 | **95** |

MMIO 下降的 ~265 次就是被 ioeventfd 接走的 `QUEUE_NOTIFY` 退出；剩下的 95
次是 probe 阶段的寄存器/配置空间访问（FEATURES、QUEUE_*、STATUS、config），
这部分无法下沉。串口 IO 不变（未动 8250 路径）。复现：

```bash
sudo ./minivmm -k ../../scripts/images/bzImage -i ../../scripts/images/initramfs.img \
     -m 512 -d blk.img          # 然后在 guest 里跑上面的 dd，Ctrl-C 看 [stats]
sudo ./minivmm ... -d blk.img -e   # 对照
```

## 项目 3：VFIO 直通 `4b:00.0`（已完成）

设备：`0000:4b:00.0` virtio-blk（`[1af4:1001]`，**legacy 设备 ID**、revision 0、
subsystem_device=0x0002、64-bit 内存 BAR0/BAR2 各 4KB、MSI-X Count=7 表在
BAR2），已绑 `vfio-pci`、独占 IOMMU group 35。用 `-p` 打开直通模式。

**已实现并实测通过**（`-p`）：

1. **MP 表 → guest 进 APIC/IOAPIC 模式**（MSI 的前提）。`build_mptable()`
   按 Intel MP 1.4 在 639KB 处放浮动指针 + 配置表（1 CPU、ISA/PCI 总线、
   IOAPIC@0xfec00000 id=0、ISA IRQ 恒等映射）。实测 guest dmesg：
   `found SMP MP-table at [mem 0x0009fc00]`、`IOAPIC[0]: apic_id 0 ... GSI 0-23`、
   `APIC: Switch to symmetric I/O mode`，`/proc/interrupts` 从 `XT-PIC` 变
   `IO-APIC`，且串口 IRQ4、virtio-mmio IRQ5/6 经 IOAPIC 仍正常。
2. **VFIO fd 栈**：`/dev/vfio/vfio` → group 35 → device fd，
   `VFIO_SET_IOMMU(TYPE1)`、`VFIO_IOMMU_MAP_DMA`（guest RAM，IOVA=GPA 1:1）、
   枚举 region（`vfio_setup()`）。
3. **PCI 配置机制 1**（PIO 0xcf8/0xcfc）：host bridge@00:00.0 用本地 config，
   直通设备@00:01.0 的 config 读写转发 `VFIO_PCI_CONFIG_REGION`。实测 guest
   枚举到两设备：`pci 0000:00:00.0 [8086:1237]`、`pci 0000:00:01.0 [1af4:1001]`。
4. **64-bit BAR sizing/分配**：影子寄存器实现"写全 1 读回 size mask"，guest
   正确算出 4KB 并分配 `BAR0 [mem 0x100000000-0x100000fff 64bit pref]`、
   `BAR2 [mem 0x100001000-...]`。
5. **BAR MMIO 转发**：guest 访问已分配 BAR 的 GPA → `KVM_EXIT_MMIO` →
   `pread/pwrite` 到 VFIO BAR region（`handle_pt_mmio()`）。
6. **legacy virtio-pci 驱动绑定**：设备无 modern VIRTIO_PCI_CAP（能力链只有
   PCIe+MSI-X），guest 走 `virtio_pci_legacy`（`vp_legacy_probe` 要求
   device 0x1000-0x103f、revision 0、subsystem_device 定设备类型，均满足）。
7. **BAR0 纯透传（最终方案）**：`msixdump -a` 实测证明该设备是标准
   legacy 布局，且随**自身** MSI-X Enable 状态切换——未启用时 config@0x14；
   经 `VFIO_DEVICE_SET_IRQS` 启用后 0x14/0x16 变为 config/queue 向量寄存器
   （默认 `0xFFFF`=NO_VECTOR）、config 移到 0x18。启用后的布局与 guest
   legacy 驱动的 MSI-X 视图（`VIRTIO_PCI_CONFIG_OFF(msix)=0x18`、向量寄存器
   @0x14/0x16，`virtio_pci.h:74-80`）完全一致，`handle_bar0_legacy()` 直接
   透传即可（仅对 QUEUE_NOTIFY 与向量寄存器留影子计数）。
   > 此前按"设备 config 固定@0x14"做过 `-4` 偏移换算 + 截留 0x14/0x16，
   > 在布局切换的时序窗口里把 guest 的 config 读错置（容量低 32 位读成
   > 全 1 的 `0x1_FFFFFFFF`，与设备向量寄存器默认 `0xFFFF` 的取值吻合），
   > 且设备永远学不到队列→MSI-X 表项映射（完成请求也不发中断、I/O 挂死）。
   > 完整排查与证据链见 `../corrections.md` D/G 节。
8. **MSI-X 武装时机**：guest 清 MSI-X Function Mask 时（`pt_config_write`）
   触发 `msix_setup_irqs()`。依据 6.12 `msix_capability_init` 顺序
   （`drivers/pci/msi/msi.c:725` 先置 ENABLE|MASKALL 使表可访问 → `:740`
   编程表项 → `:756` 全 mask → `:758` 清 MASKALL），那一刻影子表已完整：
   据此 `KVM_SET_GSI_ROUTING` + `KVM_IRQFD` + `VFIO_DEVICE_SET_IRQS`
   （未编程向量 fd=-1）。
9. **mask 位直写物理表**：VFIO 对 MSI-X 没有 `ACTION_MASK/UNMASK`
   （`vfio_pci_intrs.c:854` "XXX Need masking support exported"），宿主内核
   自家路径也是直写（`pci_msix_write_vector_ctrl`，`msi.h:43-55`）。guest
   对表内 Vector Control 的写由 `handle_pt_mmio()` 直接落物理表；arm 后把
   影子表 ctrl 同步写回一次（武装会无条件放行所有向量，而 guest 此刻是全
   mask），之后由 guest 驱动的 `request_irq → pci_msix_unmask` 逐向量解禁。

**实测验收**（2026-08-31）：

- **慢速路径**（`MINIVMM_MSIX_SLOW=1`：跳过 `KVM_IRQFD`，用户态线程收
  eventfd 经 `KVM_IRQ_LINE` 注入，是"设备是否发中断"的可靠判据）：
  `[msix-slow] vec1 设备中断触发` 6 次，`dd if=/dev/vdb bs=512 count=4`
  完成，容量 `[vdb] 4294967296 512-byte logical blocks (2.00 TiB)`。
- **正式路径**（KVM_IRQFD → Posted Interrupt）：guest `/proc/interrupts`
  `25: 5 PCI-MSIX-0000:00:01.0 1-edge virtio2-req.0`，dd 完成；**运行期间
  宿主 `IR-PCI-MSIX-0000:4b:00.0 vfio-msix[0/1]` 计数全程 0**——中断未经
  `vfio_msihandler`，硬件按 Posted IRTE 直写 vCPU PI descriptor，0 次
  VM-Exit（SDM 30.6；投递模型与源码链见 `../corrections.md` E 节）。
- 路由 `[msix] 路由就绪: 2/7` 属正常：单 vCPU → virtio_blk 只建 1 个请求
  队列（`1/0/0 default/read/poll queues`），只编程 config+queue 两向量。
- exit 统计：`IO=46943 MMIO=443`、`QUEUE_NOTIFY 写=2`、`ctrl 转发=23`。

> 排查史备注：期间曾两次误判（"identity 域环境不支持直通"、
> "vfio_msihandler 计数 0 = 设备没发中断"），撤回与正确模型见
> `../corrections.md` E 节；一次把宿主搞挂的操作事故复盘与安全规则见
> F 节（并沉淀为 `AGENTS.md` 已知陷阱 15）。
> 诊断工具 `msixdump.c`（`-r` 复位、`-a` arm 对照）是本项目的关键抓手。

## 项目 4：性能对标（已完成）

**环境**：宿主裸金属（`systemd-detect-virt`=none、CPUID 无 hypervisor 位；
项目 1 环境描述里"宿主是 KVM guest"为笔误，见 `corrections.md` H 节），
96 线程、内核 6.8.0-51-generic；被测 guest 统一单 vCPU、
内核 6.12.93-kvm-study（`scripts/vm/kernel-config`，CONFIG_HZ=250），
M1/M2 内存 512 MB、M3 为 256 MB。QEMU 基线 10.1.0-rc2（`-accel kvm -cpu host`），
q35 为默认 machine，microvm 为 `-machine microvm`。
脚本：`bench-boot.sh`（M1）、`bench-exits.sh`（M2）、`bench-halt.sh` +
`bench-halt-sweep.sh`（M3）；原始数据在 `bench/` 各输出目录。

### M1：启动延迟（N=10，中位数）

判据统一：guest `/init`（cmdline 带 `autotest`）打印 `MINIVMM_READY` 即就绪。
minivmm 两个里程碑在 VMM 进程内用 CLOCK_MONOTONIC 打点；QEMU 侧宿主墙钟轮询
`-serial file` 输出（步长 0.2ms，≤1ms 系统偏置，两组同法、对比时抵消）。
数据：`bench/boot-20260901-095559/boot.csv`。

| 实现 | 首个串口字节 | guest 就绪 | ready 范围 |
|------|-------------|-----------|-----------|
| minivmm | 32.9 ms | **1289.0 ms** | 1286.1–1300.0 |
| minivmm-tuned | 33.1 ms | 1291.6 ms | 1283.1–1301.1 |
| qemu-q35 | 191.6 ms | **987.5 ms** | 971.4–1002.0 |
| qemu-microvm | 90.2 ms | **546.0 ms** | 534.7–551.9 |

**阶段拆分**（guest 内核时间戳，`minivmm-run*.log` 与各 `-serial.log`）：

minivmm 的 1289 ms 里有 **801.7 ms（62%）是两段 legacy 探测**：

| 窗口 | guest dmesg 界标 | 耗时 | 根因 |
|------|-----------------|------|------|
| 8250 自动配线 | `Serial: 8250/16550 driver` [0.281688] → `ttyS0 at I/O 0x3f8` [0.535106] | 253.4 ms | `autoconfig_irq()`（`8250_port.c:1305`）两次 `probe_irq_on()`，各含 msleep(20)+msleep(100)（`kernel/irq/autoprobe.c:61,81`）；COM1 恒带 `UPF_AUTO_IRQ`（`asm/serial.h:16`） |
| i8042 直接探测 | `Probing ports directly` [0.545087] → `Can't read CTR` [1.093362] | 548.3 ms | 无 ACPI/PNP 信息 → `i8042_probe()` 直接探端口，CTR 读 500 ms 超时（`I8042_CTL_TIMEOUT`×udelay(50)，`drivers/input/serio/i8042.h`） |

QEMU 两侧都靠**固件信息**绕开：

- **q35**：ttyS0 由 PNPACPI 枚举（`00:03:`，[0.682619]，IRQ 直接给出，不做
  `probe_irq_on()`）；i8042 由 `PNP0303/PNP0f13` 发现（[0.687529]），不直接探测。
- **microvm**：ttyS0 [0.260070]（IRQ 49，走 IOAPIC）；i8042 只打印
  `PNP: No PS/2 controller found` [0.263513] 就结束——FADT `IAPC_BOOT_ARCH`
  bit1=0（`iapc_boot_arch_8042()`，qemu `include/hw/input/i8042.h:103`）→ guest
  `acpi_parse_fadt()` 置 `X86_LEGACY_I8042_FIRMWARE_ABSENT`
  （`arch/x86/kernel/acpi/boot.c:983-988`），探测整个跳过。

**minivmm-tuned 阴性对照**：cmdline 追加 `8250.nr_uarts=1 i8042.nokbd
i8042.noaux`，ready 1291.6 vs 1289.0 ms，无效。8250 那 253 ms 是
`autoconfig_irq()` 逐端口探测、与端口数无关；`i8042.nokbd/noaux` 拦不住
`i8042_probe()` 里无条件的 `i8042_controller_init()`（`drivers/input/serio/i8042.c:1556`）。
结论：**cmdline 调参补不回缺失的固件信息**。

反直觉点：minivmm 设备模型远小于 QEMU（无 PCI/ACPI/BIOS，首字节最快
32.9 ms），boot 却比 q35 慢 300 ms——瓶颈不在模拟量，在缺固件信息把
guest 推上了 msleep 密集的 legacy 探测路径。与
`../phase11-microvm/README.md` 的结论一致：microvm 快既因砍设备，
更因把该给的固件信息给全了。

### M2：VM-Exit 分布

方法：`perf kvm stat record -a -- sleep 15` + `report`。`-a` 必须：不加时
record 只跟踪被包裹进程（`tools/perf/builtin-kvm.c:1959-1960`），vCPU 线程的退出全丢；
数据写入 `perf.data.guest` 文件由 report 读取（`tools/perf/builtin-kvm.c:602-613`，
分支在 `:609`）。
负载：boot（包裹 VMM 进程全程）、idle（shell 空转 15 s）、busy（guest 内
`while :; do :; done` 15 s）。数据：`bench/exits-20260901-101641/`。

idle（15 s，CPU%：minivmm 0.7 / q35 0.0 / microvm 0.3）：

| VM-EXIT | minivmm | qemu-q35 | qemu-microvm |
|---------|---------|----------|--------------|
| IO_INSTRUCTION | 15008（79.9%） | – | – |
| MSR_WRITE | – | 3752（49.4%） | 3753（49.4%） |
| HLT | 3751（Time% 99.82，均值 3982 µs） | 3751（99.96%） | 3752（99.96%） |

busy（15 s，CPU% 三者均 ~100）：

| VM-EXIT | minivmm | qemu-q35 | qemu-microvm |
|---------|---------|----------|--------------|
| EXTERNAL_INTERRUPT | 22415（59.3%） | 15041（65.8%） | 15134（66.6%） |
| IO_INSTRUCTION | 15004（39.7%） | – | – |
| MSR_WRITE | – | 3753（16.4%） | 3753（16.5%） |
| PREEMPTION_TIMER | – | 3753（16.4%） | 3753（16.5%） |

boot（perf 包裹整个 VMM 进程）：

| VM-EXIT | minivmm | qemu-q35 | qemu-microvm |
|---------|---------|----------|--------------|
| IO_INSTRUCTION | 40568（66.9%） | 72247（86.1%） | 35277（90.2%） |
| EPT_VIOLATION | 16691（27.5%） | 3472（4.1%） | 519（1.3%） |
| EPT_MISCONFIG | 341（0.6%） | 5142（6.1%） | 638（1.6%） |

**源码级解释**（逐类）：

1. **minivmm 空闲的 1000/s IO = 250 Hz PIC tick × 4 次端口访问**。本项目
   （`-p` 以外模式）无 MP 表/ACPI MADT，guest dmesg：`APIC: ACPI MADT or MP
   tables are not detected` → `Switch to virtual wire mode`。时钟中断走 8259
   `handle_level_irq`（`kernel/irq/chip.c`），每个 tick 都要
   mask/ack/unmask：IN 0x21、OUT 0x21、OUT 0x20、OUT 0x21
   （`mask_and_ack_8259A`/unmask，`drivers/irqchip/irq-i8259.c:48,134`，
   用 `/tmp/mvidle.kvmexit15` 的 `kvm_exit` info1>>16 解码核对过序列与
   rip 模式）。4×250 Hz=1000/s，且 15008/4≈3752=HLT 计数，自洽。
   QEMU guest 有 MADT → LAPIC timer，每 tick **零** PIO，改为 250/s 的
   MSR_WRITE（x2APIC 寄存器经 MSR 访问，per-tick 重编程定时器）。
   同一个 250 Hz 时钟，投递载体决定了退出形态。
2. **qemu busy 的 PREEMPTION_TIMER 3753**：LAPIC timer 由 VMX preemption
   timer 模拟，到期以此 reason 退出（均值 0.77 µs）。minivmm busy 没有
   LAPIC，tick 继续走 PIC，IO 保持 15004——缺固件表的直接代价。
3. **EXTERNAL_INTERRUPT**：vCPU 运行期间到达的宿主硬件中断，与 VMM 实现
   无关，随宿主背景噪声起伏（两侧同量级）。
4. **EPT_VIOLATION vs EPT_MISCONFIG**：前者主要是 guest RAM 首次触按的
   按需建页（EPT violation），KVM **内核态**解决、不下用户态——minivmm
   boot 期间自身统计的用户态 `MMIO=342` 只对应那 341 个
   EPT_MISCONFIG 采样（MMIO SPTE 建好后的重复访问，即两台 virtio-mmio
   设备的寄存器探测）；q35 的 5142 则是全套 PCI 设备 MMIO 空间的探测。
5. **boot IO 排序**（q35 72247 > minivmm 40568 > microvm 35277）：IO 退出
   总量随模拟设备面（被探测的端口数）增长。

**直通负载（`-p` + guest 内循环 `dd if=/dev/vdb`，perf 15 s）**，
数据：`bench/exits-pt-20260901-112353/`：

| VM-EXIT | Samples | 说明 |
|---------|---------|------|
| EPT_VIOLATION | 68694（65.7%） | dd 循环反复 fork/读盘的按需建页，内核态解决 |
| HLT | 16549（Time% 97.3，均值 653.8 µs） | guest 等设备完成 |
| EPT_MISCONFIG | 14541（13.9%，均值 11.1 µs） | 用户态 BAR MMIO：`QUEUE_NOTIFY 写=19248`、`ctrl 转发=23` |
| EXTERNAL_INTERRUPT | 4712（4.5%） | 宿主背景中断 |

两个要点：**IO_INSTRUCTION 从榜上消失**——`-p` 装了 MP 表，guest 进
APIC/IOAPIC 模式（`found SMP MP-table`、`Switch to symmetric I/O mode`），
时钟 tick 不再走 PIC 的 4 次 PIO；**完成中断零退出**——宿主侧
`IR-PCI-MSIX-0000:4b:00.0 vfio-msix` 计数全程 0，MSI-X 经 Posted IRTE
直写 vCPU PI descriptor（SDM 30.6，项目 3 已验证）。数据面下沉后，
直通负载的退出只剩"调度相关 + notify MMIO"；notify 这一项若再学
QEMU 用 `KVM_IOEVENTFD` 绑 BAR 写（项目 2 M3 的同款手法），用户态退出
还能再降一个量级。

### M3：halt-polling 调参

机制要点（`virt/kvm/kvm_main.c`）：`halt_poll_ns` 等四参数见 `:78-95`；
per-vCPU 窗口 `vcpu->halt_poll_ns` 初值 0，模块参数只是上限
（`kvm_vcpu_max_halt_poll_ns()` `:3787-3803`）；命中（事件在窗口内到达）
才增长，且要求 `halt_ns < max`（`:3872-3874`）；未命中按 `shrink` 除
（`:3689-3706`，shrink=0 直接清零 `:3696-3697`）。本机实验前残留
`200000/2/10000/0`（shrink=0），脚本结束按原样恢复。
配置：**poll-on** = 200000/2/10000/2（源码默认）、**poll-off** = ns=0。
负载：idle = shell 空转（4 ms tick 深 halt）；flood = 见回显即发下一字符，
发送间隔 ≈ RTT，guest 两次字符间的 halt 是短 halt——实测 ≤80 µs
（判据：自适应窗口涨到 80 µs 后稳定，即 `halt_ns ≤ halt_poll_ns` 走
no-op 分支，`kvm_main.c:3865`）。数据：`bench/halt-20260901-110933/`。

| 配置 | idle CPU% | 冷唤醒 RTT（n=20） | flood RTT（n=800） | flood CPU% | flood 窗口轨迹 |
|------|----------|-------------------|-------------------|-----------|---------------|
| poll-on | 0.2 | med 175.7 µs | med 165.9 µs | **74.9%** | 0→10k→20k→40k→80k（4 grow，0.7 ms 内），负载停后 4 次 shrink 归零 |
| poll-off | 0.4 | med 179.7 µs | med 165.7 µs | **58.7%** | 恒 0（无事件） |

**固定窗口扫描**（`bench-halt-sweep.sh`，`bench/halt-sweep-20260901-111527/`）：
用 `grow=1 + grow_start=NS + shrink=1` 把窗口钉在 NS——首次短 halt 时
0<grow_start 被抬到 grow_start（`kvm_main.c:3680-3682`），之后 ×1、÷1 均不变。

| 窗口 | flood RTT med | flood CPU% |
|------|--------------|-----------|
| 0（关） | 165.7 µs | 58.7% |
| 50 µs | 167.0 µs | 74.8% |
| 100 µs | 165.8 µs | 74.6% |
| 200 µs | 165.6 µs | 74.9% |

**结论**：

1. **空闲负载上 polling 无效也无害**：halt ~4 ms ≫ 200 µs 上限，增长条件
   （`:3872`）永不成立，窗口停在 0；两配置 CPU%≈0、冷唤醒 RTT 相同。
   关 polling 对空转 VM 不损失任何东西。
2. **短 halt 负载上窗口自适应命中但买不到延迟**：0.7 ms 内 4 步涨到
   80 µs 并稳定（此后 `halt_ns ≤ halt_poll_ns` 走 `;` 分支）。本负载的
   唤醒事件是"回显到达宿主 → 宿主再发下一字符"，间隔由 RTT 闭环本身
   决定，polling 只能把 block+unblock 换成忙等、不能让事件更早到达：
   两配置 RTT 相同（165.9 vs 165.7 µs），代价是 **+16 pp CPU**。
3. **曲线在"盖住典型 halt"处饱和**：窗口从 50 µs 加到 200 µs，延迟与
   CPU 都不变。成本台阶是"是否轮询"，不是窗口大小；窗口只需 ≥ 典型
   halt 长度，再大纯属浪费。
4. **该开/该关的负载画像**：唤醒源随机且大概率落在窗口内（高频网卡/块
   设备中断）→ 开，用 CPU 换唤醒延迟与 block/unblock 开销；唤醒间隔恒
   大于上限（空转），或像本实验一样间隔由外部闭环锁定 → 关，省 CPU 不亏
   延迟。

### M4：真实差距在哪

1. **不在设备模型的模拟效率**。minivmm idle IO 退出均值 1.80 µs、HLT
   占时 99.8%+，与 QEMU（MSR_WRITE 1.48 µs、HLT 99.96%）同量级；处理
   路径都是"退出 → 用户态几个寄存器操作 → 注入 → 回注"，没有结构性差距。
2. **不在退出数量级上"输给"QEMU 的地方，恰是固件信息**。minivmm 每 tick
   4 次 PIO（PIC mask/ack/unmask）vs QEMU guest 0 次 PIO（LAPIC）；
   boot 多付 801.7 ms 的 8250/i8042 探测。装上 MP 表（`-p`）后 PIC tick
   立刻消失（M2 直通表），证明这些退出全部由"缺表"引起，可逐项消除。
3. **启动延迟差距的本质是固件信息面，不是设备模型大小**：设备面最小的
   minivmm boot 最慢（1289 ms），设备面砍到最狠且固件信息给全的
   microvm 最快（546 ms），q35 居中。tuned 阴性对照排除了
   "cmdline 可以替代固件信息"的可能。给 minivmm 补 ACPI（FADT
   `IAPC_BOOT_ARCH`、PNPACPI 串口/键盘枚举）即可消掉 800 ms 大头——
   这是本项目找到的**唯一显著的 boot 差距**，且与代码模拟质量无关。
4. **phase9 优化在本场景的可测性**：
   - **halt-polling**：可测（M3）。本场景的正确结论是"默认参数即可，
     空闲场景可关"；它的收益形态是延迟↔CPU 的交换，不是单方面提升。
     收益曲线的实测数据在 `../../phase9-performance/index.md` §1.2（本仓规则：
     数字只放一处，这里只给指针），参数默认值/权限在
     `../../phase9-performance/parameters.md` §1。
   - **PLE**：单 vCPU、无超卖自旋，测不出；需要多 vCPU 超卖实验 ——
     那一套的设计与判据在 `../../phase9-performance/practice/bench-ple.md`（E1），
     且 `ple_*` 五个参数**全部 `0444` 只读**
     （`arch/x86/kvm/vmx/vmx.c:204-219`），运行时改不了，只能重新 insmod + 重启 VM。
   - **VPID/大页**：KVM 默认开启/按需，对 VMM 透明，本规模无独立可测收益。

### 测量陷阱备忘（已固化进脚本）

- `perf kvm stat record` 必须 `-a`，否则只跟踪被包裹进程、vCPU 线程退出
  全丢（`tools/perf/builtin-kvm.c:1959-1960` 仅 target 为空才默认 system-wide）；
  数据走 `perf.data.guest` 文件（`tools/perf/builtin-kvm.c:602-613`，分支在 `:609`），
  与 tracefs 缓冲无关。
- flood 字符总量必须 < guest busybox ash lineedit 行缓冲（约 1024 字节，
  宿主 pty 实测：超限时字符照收不回显），否则 flood 中途静默停摆、后续
  全部超时。`bench-halt.sh` 限 800 字符，且连续 3 次超时即中止。
- QEMU pipe chardev 只打开已存在的 `<path>.in/.out`（qemu
  `chardev/char-pipe.c:121-154`），须先 `mkfifo`。
