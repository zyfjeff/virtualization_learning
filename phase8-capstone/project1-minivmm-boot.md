# 项目 1：可启动的最小 VMM

> 目标：不借助任何现成 VMM，只用 `/dev/kvm` ioctl，把 Linux bzImage +
> initramfs 引导到 shell。

**前置**：phase0（KVM API）、phase1（vCPU/VMCS）、phase2（内存虚拟化）、
phase4（in-kernel irqchip）、phase7（时钟）。起步代码见
`../examples/kvm-api-demo/kvm-demo.c` 与 `../phase7-timer-virt/practice/common.h`。

---

## 里程碑

### M1：解析并加载 bzImage（boot protocol）

Linux x86 引导协议见
`/root/code/linux-6.12.93/Documentation/arch/x86/boot.rst`。VMM 走
**32-bit boot protocol**（不做实模式）：

1. **读 setup header**：bzImage = setup 段 + protected-mode 内核。setup 头
   从镜像偏移 `0x01f1` 开始；末尾 = `0x0202 + 镜像偏移 0x0201 处的字节值`
   （boot.rst "32-bit Boot Protocol" 一节）。`setup_sects` 在 `0x1f1`
   （boot.rst:189），为 0 时按 4 处理；protected-mode 内核从
   `(setup_sects + 1) * 512` 偏移开始。
2. **构造 boot_params（zero page）**：分配一页清零，把 setup header 复制
   进去，然后填（字段位置见 `arch/x86/include/uapi/asm/bootparam.h`）：
   - `type_of_loader`（:52）— 自定义值即可（如 `0xff`）
   - `loadflags`（:53）— 置 `LOADED_HIGH`（`1<<0`，bootparam.h:13），
     表示内核载入高端内存；协议 2.06+ 还要求设置 `CAN_USE_HEAP` 并填
     `heap_end_ptr`（boot.rst 中 0224 字段）
   - `cmd_line_ptr`（:62，偏移 0228）+ `cmdline_size`（偏移 0238，
     协议 2.06+ 必填）
   - `ramdisk_image` / `ramdisk_size`（:56-57，偏移 0218/021C）；
     initrd 超过 4 GB 时用 `setup_data` 的 `ext_ramdisk_image/size`
     （bootparam.h:128-130）
3. **加载内核**：载入 `pref_address`（bootparam.h:74，协议 2.10+，可重定位
   内核的首选地址），空间不足再按 `kernel_alignment`（:64）对齐下移；
   大小参考 `init_size`（:75）。
4. **填 e820 内存表**：`boot_params.e820_table` 必须如实描述 guest RAM
   （含保留区），内核完全依赖它建页表与分配器。
5. **入口约定**（boot.rst "32-bit Boot Protocol"，条目见该行下方清单）：
   - 入口 = 载入后内核的起始地址
   - 32 位保护模式、**分页关闭**
   - GDT 已加载：`__BOOT_CS = 0x10`、`__BOOT_DS = 0x18`，均为 4G 平坦段
   - 中断关闭；`%esi` = boot_params 基址；`%ebp/%edi/%ebx` = 0

cmdline 建议：
`console=ttyS0 earlyprintk=serial rdinit=/bin/sh quiet` 之外的参数先不加。

### M2：VM 骨架（注意初始化顺序）

在 `kvm-demo.c` 的骨架上调整：

1. `KVM_CREATE_VM` → `KVM_SET_TSS_ADDR` / `KVM_SET_IDENTITY_MAP_ADDR`
   （VMX 需要这两块宿主保留页，**不能落在 guest RAM 里**）。
2. **`KVM_CREATE_IRQCHIP` 必须在 `KVM_CREATE_VCPU` 之前**。内核硬拒反序：
   `kvm->created_vcpus` 非零直接 `-EINVAL`
   （`arch/x86/kvm/x86.c:7090-7099`，case 主体依次
   `kvm_pic_init()` :7101 → `kvm_ioapic_init()` :7105 →
   `kvm_setup_default_irq_routing()` :7111）。
3. `KVM_CREATE_PIT`（case @ `x86.c:7125`）可选：guest 使用 TSC-deadline
   定时器后不依赖 i8254（PIT 与 TSC-deadline 的关系见
   `../phase7-timer-virt/README.md`）。先不创建，跑不通再回头加。
4. `KVM_SET_USER_MEMORY_REGION`：一块连续 region 覆盖全部 guest RAM
   （内核、initrd、boot_params、cmdline 都必须在内，且与 e820 一致）。

### M3：vCPU 初始状态（两个静默拒绝陷阱在这一步）

1. `KVM_CREATE_VCPU`（case @ `virt/kvm/kvm_main.c:5170`）→
   `KVM_GET_VCPU_MMAP_SIZE`（`kvm_main.c:5552`）→ mmap `struct kvm_run`。
2. `KVM_SET_CPUID2`（case @ `x86.c:5957`）。推荐直接以
   `KVM_GET_SUPPORTED_CPUID` 的返回为底，只改下面几处，别整表重写：
   - **leaf1 ECX bit24 `X86_FEATURE_TSC_DEADLINE_TIMER` 必须保留**：
     `kvm_vcpu_after_set_cpuid()` 依它把 `timer_mode_mask` 设成 `3<<17`，否则只给
     `1<<17`（`cpuid.c:399-402`）——guest 写 LVT Timer 的 deadline 位(bit18)
     会被 `lapic.c:2391` 掩掉，TSC-deadline 模式设不上。这是
     `../phase7-timer-virt/practice/` 实验 3 实测踩过的坑（该 README
     §前置条件 ②，细节勘误见 `../phase7-timer-virt/corrections.md`）。
   - **leaf1 ECX bit31 `X86_FEATURE_HYPERVISOR` 必须自己置位**：
     `KVM_GET_SUPPORTED_CPUID` 的返回里没有它（本机实测 ECX=`0x76fab223`，
     bit31=0），而 guest 的 `__kvm_cpuid_base()`（`kvm.c:877`）拿它当准入
     门槛。缺了就没有 `Hypervisor detected: KVM`，kvmclock/PV 全灭。
     定义 `cpufeatures.h:144`。guest 内核侧另需 `CONFIG_HYPERVISOR_GUEST` /
     `CONFIG_KVM_GUEST`（见 `scripts/vm/kernel-config`）。
   - **leaf1 EBX[31:24] 初始 APIC ID 归零、EBX[23:16] 改 1**：宿主透传值
     （本机 0x60）与 in-kernel LAPIC 的 ID 0 不符，guest 会打
     `[Firmware Bug]: ... APIC ID mismatch`（`topology_common.c:174-176`）。
   - **leaf1 ECX bit21 x2APIC 第一版清掉**（见"已知陷阱"3）。
   - KVM 半虚拟化叶 `0x40000000`/`0x40000001`：宿主 KVM 已在
     `KVM_GET_SUPPORTED_CPUID` 里填好（本机 feature 叶 eax=`0x01007efb`，
     含 CLOCKSOURCE2/PV_EOI/ASYNC_PF 等），**照抄即可，别覆盖成最小集**；
     只有缺叶的老内核才需补 `KVM_FEATURE_CLOCKSOURCE=0`、
     `CLOCKSOURCE2=3`（`arch/x86/include/uapi/asm/kvm_para.h:19,25`）。
3. `KVM_SET_SREGS`（case @ `kvm_main.c:4542`）：按 M1 第 5 条设置
   `CR0.PE=1`、`CR0.PG=0`、平坦段（CS=0x10、DS/ES/SS=0x18，base 0、
   limit 0xFFFFFFFF）、EFLAGS 中断关。
4. `KVM_SET_REGS`（case @ `kvm_main.c:4516`）：`rip` = 内核载入基址，
   `rsi` = boot_params GPA，`rbp/rdi/rbx` = 0。

### M4：串口控制台（KVM_EXIT_IO 循环）

- cmdline 里 `console=ttyS0 earlyprintk=serial` 后，内核早期日志走
  16550 端口 `0x3f8`。Guest 每次 `out` 触发 `KVM_EXIT_IO`
  （`include/uapi/linux/kvm.h:148`）：`run->io.direction`
  （`KVM_EXIT_IO_IN=0` / `OUT=1`）、`size`、`port`、`count`，
  数据在 `run` 起始 + `data_offset` 处（`kvm.h:252-258`）。
- **只做到"LSR 读回 TX-empty"不够**。那只让 console 不卡死；要拿到
  `ttyS0 ... (irq = 4 ...) is a 16550A`（打印在 `serial_core.c:2574`），
  8250 驱动的探测还要求（寄存器位定义对齐
  `include/uapi/linux/serial_reg.h`）：
  1. **LSR（0x3fd）TX-empty**：bit5/6 置位（`UART_LSR_THRE|TEMT`，
     `serial_reg.h:141-142`），否则驱动认为发送忙。
  2. **THRI 中断**：`autoconfig_irq()`（`8250_port.c:1305`）靠"开 IER 后
     写 THR 能拉起中断"反查 IRQ 号；给不出 THRI，guest 报 `irq = 0`、
     退化成轮询。
  3. **IIR[7:6] = 0b11**：`autoconfig()` 的 FIFO 类型 switch
     （`8250_port.c:1241`）用它判端口型号，给错报 `is a 16450`。
  4. **loop 测试的 MSR 回环映射**：`autoconfig()` 在
     `MCR = LOOP|OUT2|RTS` 下期望 `MSR == DCD|CTS`
     （`8250_port.c:1215-1219`），即回环时 OUT2→DCD、RTS→CTS
     （另 OUT1→RI、DTR→DSR）。
  5. **IER（0x3f9）按 16550A 只实现低 4 位**：`autoconfig()` 会写/回读
     IER（`8250_port.c:1175`）。
  `practice/minivmm.c` 的 16550A 模型即按这五条实现，语义对照 QEMU
  `hw/char/serial.c`。

### M5：kvmclock 与 APIC timer（大部分由内核代劳）

- Guest Linux 通过 `KVM_FEATURE_CLOCKSOURCE2` 发现 kvmclock，写
  `MSR_KVM_SYSTEM_TIME_NEW`（`0x4b564d01`）与
  `MSR_KVM_WALL_CLOCK_NEW`（`0x4b564d00`）
  （`arch/x86/include/uapi/asm/kvm_para.h:52-53`，值为 GPA | enable 位）。
  **这两个 MSR 由 KVM 内核侧处理**（`kvm_guest_time_update()` @
  `x86.c:3215`），VMM 只需保证目标 GPA 在 guest RAM 内。
- APIC timer：in-kernel LAPIC 全程处理，TSC-deadline 写 `0x6e0` 走
  `kvm_set_msr_common()`（case @ `x86.c:3890`）→
  `kvm_set_lapic_tscdeadline_msr()`（`lapic.c:2585`）。VMM 无需插手。
- **实测提醒**：宿主暴露 CONSTANT_TSC + NONSTOP_TSC 时，`kvmclock_init()`
  把 `kvm_clock.rating` 自降到 299（`kvmclock.c:342-345`），低于 tsc 的 300
  （`tsc.c:1189`），dmesg 会先 `Switched to clocksource kvm-clock` 再
  `Switched to clocksource tsc`。最终 clocksource 是 tsc 不代表 kvmclock
  没生效（它仍提供 sched_clock / pvclock）。

### M6：启动到 shell 与观察

- initramfs 载入到高端内存（`ramdisk_image/size`），`rdinit=/init`
  （`/init` 负责挂 proc/sysfs/devtmpfs 再 exec shell；`scripts/vm/
  build-rootfs-minimal.sh` 生成的 initramfs 即如此）。
- 观察点：serial 输出内核日志直到 `#` 提示符；guest 内
  `cat /proc/cpuinfo`（应见 KVM 半虚拟化签名）、
  `dmesg | grep -i 'Hypervisor detected'`（应有 `Hypervisor detected: KVM`）、
  `cat /proc/cmdline`。
- 宿主侧用 `perf kvm stat live` 观察 exit 分布：启动早期以
  EPT violation 与 IO 为主，稳定后 halt/external interrupt 为主。

---

## 内核侧代码路径对照表

所有行号基于 6.12.93：

| 步骤 | ioctl / 事件 | 内核侧入口 |
|------|--------------|-----------|
| VM 创建 | `KVM_CREATE_VM` | `kvm_dev_ioctl_create_vm()` @ `virt/kvm/kvm_main.c` |
| TSS / identity map | `KVM_SET_TSS_ADDR` / `KVM_SET_IDENTITY_MAP_ADDR` | `x86.c:7069` / `x86.c:7072` |
| irqchip | `KVM_CREATE_IRQCHIP` | case @ `x86.c:7090`（vCPU 先建则 `-EINVAL` @ :7098-7099） |
| PIT（可选） | `KVM_CREATE_PIT` | case @ `x86.c:7125` |
| 内存 | `KVM_SET_USER_MEMORY_REGION` | `kvm_vm_ioctl_set_memory_region()` @ `kvm_main.c` |
| vCPU | `KVM_CREATE_VCPU` | case @ `kvm_main.c:5170` |
| kvm_run | `KVM_GET_VCPU_MMAP_SIZE` | case @ `kvm_main.c:5552` |
| CPUID | `KVM_SET_CPUID2` | case @ `x86.c:5957` → `kvm_set_cpuid()`（`cpuid.c:457`）→ `kvm_vcpu_after_set_cpuid()`（`cpuid.c:399-402` 决定 `timer_mode_mask`） |
| 寄存器 | `KVM_SET_REGS` / `KVM_SET_SREGS` | case @ `kvm_main.c:4516` / `kvm_main.c:4542` |
| 串口退出 | `KVM_EXIT_IO` | `include/uapi/linux/kvm.h:148`；io 结构 :252-258 |
| kvmclock | `WRMSR 0x4b564d01/0x4b564d00` | `kvm_guest_time_update()` @ `x86.c:3215`（常量 @ `kvm_para.h:52-53`） |
| TSC-deadline | `WRMSR 0x6e0` | `kvm_set_msr_common()` case @ `x86.c:3890` → `kvm_set_lapic_tscdeadline_msr()` @ `lapic.c:2585` |

---

## 已知陷阱（本项目实测/源码核证）

1. **irqchip 晚于 vCPU → `-EINVAL`**：`KVM_CREATE_IRQCHIP` 开头检查
   `kvm->created_vcpus`（`x86.c:7098-7099`）。顺序必须是
   VM → TSS/identity → irqchip → vCPU。
2. **CPUID 缺 TSC-deadline 位，deadline 模式设不上**：
   `kvm_vcpu_after_set_cpuid()` 是 if/else（`cpuid.c:399-402`）——有该位
   `timer_mode_mask = 3<<17`，没有则 `1<<17`（不是 0）。mask 只有
   `1<<17` 时，guest 写 LVT Timer 的 deadline 位(bit18)被 `lapic.c:2391`
   掩掉，TSC-deadline 模式设不上。只有**从未给 leaf1 建 CPUID 条目**
   （`cpuid.c:398` `kvm_find_cpuid_entry` 返回 NULL）mask 才保持 0。
   现象都是"定时器不对劲、无报错"，靠 serial 里 `tsc-deadline timer`
   缺失发现。
3. **x2APIC 的写不是"静默丢弃"**：若 CPUID 暴露 x2APIC，guest 用
   `WRMSR 0x80b`（EOI）等 MSR 访问 LAPIC。但 `kvm_set_apic_base()`
   （`x86.c:671`）在 guest CPUID **没有** `X86_FEATURE_X2APIC` 时把
   `X2APIC_ENABLE`（`BIT(10)`，`apicdef.h:153`）并入保留位
   （`x86.c:675-676`）并在 `:678-679` 拒绝写入；且
   `kvm_x2apic_msr_write()`（`lapic.c:3308`）在非 x2APIC 模式下
   `return 1`（`:3312-3313` 检查 `apic_x2apic_mode()`）——guest 发起的
   WRMSR 由此经 `complete_emulated_insn_gp()` 注入 **#GP**，host 发起的
   `KVM_SET_MSRS` 则是拒绝该 MSR。第一版保持 xAPIC（in-kernel LAPIC
   处理 MMIO），x2APIC 留作进阶选项。详见
   `../phase7-timer-virt/corrections.md`。
4. **boot_params 字段漏填**：`cmdline_size`（协议 2.06+）、`heap_end_ptr`、
   e820 表缺一不可；e820 缺 RAM 区域时内核启动早期直接崩，串口只打出
   几行解压日志。
5. **GPA 布局冲突**：boot_params/cmdline/initrd 必须落在 e820 声明的
   RAM 内，且避开 TSS/identity map 保留页，否则三重故障（`KVM_EXIT_SHUTDOWN`）。
6. **LSR 读返回 0 卡死 console**：8250 驱动以 `LSR.THRE` 判断可发送。
   但仅这一条不够，见 M4 的完整探测清单。
7. **leaf1 ECX bit31（HYPERVISOR）要自己置位**：`KVM_GET_SUPPORTED_CPUID`
   不含该位（本机实测 ECX=`0x76fab223`），缺了 guest 的
   `__kvm_cpuid_base()`（`kvm.c:877`）门槛不过，没有
   `Hypervisor detected: KVM`，kvmclock/PV 全灭。
8. **leaf1 EBX[31:24] 初始 APIC ID 要归零**：宿主透传值与 in-kernel
   LAPIC 的 ID 0 不符会触发 `[Firmware Bug]: ... APIC ID mismatch`
   （`topology_common.c:174-176`）；EBX[23:16] 逻辑处理器数也应改 1。

---

## 验收标准

- [ ] 从 `KVM_CREATE_VM` 到 shell 提示符，全程自己实现，不依赖
      QEMU/Cloud Hypervisor 等
- [ ] guest `dmesg` 有 `Hypervisor detected: KVM` 与 `kvm-clock: Using msrs`
      （最终 clocksource 可能是 tsc，见 M5 提醒，不算失败）；串口显示
      `ttyS0 ... (irq = 4 ...) is a 16550A`
- [ ] `/proc/interrupts` 里 **IRQ0 `XT-PIC timer`** 与 **IRQ4 `ttyS0`**
      计数增长。注意：本项目无 MP 表/ACPI MADT，guest 走 virtual-wire +
      in-kernel 8259，`LOC`（local timer）恒 0 属正常；`LOC` 要等装上
      IOAPIC（项目 3）后才会涨
- [ ] 宿主 `perf kvm stat` 能观察到 exit 分布，并能解释启动期
      EPT violation 峰值的来源（对照 phase2 的 EPT 建表过程）
- [ ] 能画出本项目每个 ioctl 到内核函数的调用链（用上表核对）

---

## 参考资料

- boot protocol：`/root/code/linux-6.12.93/Documentation/arch/x86/boot.rst`；
  zero page：`Documentation/arch/x86/zero-page.rst`
- KVM API：`/root/code/linux-6.12.93/Documentation/virt/kvm/api.rst`
- 起步代码：`../examples/kvm-api-demo/kvm-demo.c`、
  `../phase7-timer-virt/practice/common.h`
- QEMU 对照（它如何加载 bzImage）：
  `/root/code/qemu-10.1.0-rc2/hw/i386/x86-common.c:633`（`x86_load_linux()`）
