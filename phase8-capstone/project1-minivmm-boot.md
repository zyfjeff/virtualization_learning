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
2. `KVM_SET_CPUID2`（case @ `x86.c:5957`）。最小集合：
   - 基本叶（leaf 1）：必须含 `X86_FEATURE_TSC_DEADLINE_TIMER`，否则
     `kvm_update_cpuid()` 把 `apic->lapic_timer.timer_mode_mask` 置 0
     （`cpuid.c:398-403`），APIC timer 静默失效 —— 这是
     `../phase7-timer-virt/practice/` 实验 3 实测踩过的坑（该
     README §前置条件与勘误 26）
   - KVM 半虚拟化叶：`0x40000000`（签名 "KVMKVMKVM\0\0\0"）+
     `0x40000001` feature 位：`KVM_FEATURE_CLOCKSOURCE=0`、
     `KVM_FEATURE_CLOCKSOURCE2=3`（`arch/x86/include/uapi/asm/kvm_para.h:19,25`）；
     可选 `KVM_FEATURE_PV_EOI=6`、`KVM_FEATURE_PV_TLB_FLUSH=9`（:28,30）
   - 第一版**不要**暴露 x2APIC（见"已知陷阱"3）
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
- 最小模拟：`0x3f8` 写 → 打到宿主 stdout。但 Linux 8250 驱动会轮询
  **LSR（0x3fd）**：读 LSR 必须返回 TX-empty（bit5/6 置位，如 `0x60`），
  否则驱动认为发送忙，console 卡死。这是最小串口模拟的第一个必答题。
- 进阶：处理 `0x3f9-0x3ff` 的读回默认值，使 `8250` 驱动探测稳定。

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

### M6：启动到 shell 与观察

- initramfs 载入到高端内存（`ramdisk_image/size`），`rdinit=/bin/sh`。
- 观察点：serial 输出内核日志直到 `#` 提示符；guest 内
  `cat /proc/cpuinfo`（应见 KVM 半虚拟化签名）、
  `dmesg | grep -i kvm-clock`（clocksource 应为 kvm-clock）、
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
| CPUID | `KVM_SET_CPUID2` | case @ `x86.c:5957` → `kvm_update_cpuid()`（`cpuid.c:398-403` 决定 `timer_mode_mask`） |
| 寄存器 | `KVM_SET_REGS` / `KVM_SET_SREGS` | case @ `kvm_main.c:4516` / `kvm_main.c:4542` |
| 串口退出 | `KVM_EXIT_IO` | `include/uapi/linux/kvm.h:148`；io 结构 :252-258 |
| kvmclock | `WRMSR 0x4b564d01/0x4b564d00` | `kvm_guest_time_update()` @ `x86.c:3215`（常量 @ `kvm_para.h:52-53`） |
| TSC-deadline | `WRMSR 0x6e0` | `kvm_set_msr_common()` case @ `x86.c:3890` → `kvm_set_lapic_tscdeadline_msr()` @ `lapic.c:2585` |

---

## 已知陷阱（本项目实测/源码核证）

1. **irqchip 晚于 vCPU → `-EINVAL`**：`KVM_CREATE_IRQCHIP` 开头检查
   `kvm->created_vcpus`（`x86.c:7098-7099`）。顺序必须是
   VM → TSS/identity → irqchip → vCPU。
2. **CPUID 缺 TSC-deadline 位，APIC timer 静默失效**：
   `kvm_update_cpuid()` 依据 `X86_FEATURE_TSC_DEADLINE_TIMER` 设置
   `timer_mode_mask`（`cpuid.c:398-403`）；mask 为 0 时
   `timer_mode = LVTT & mask` 恒 0（`lapic.c:1781`），guest 卡在定时器
   校准。无任何报错，靠 serial 日志里 `tsc-deadline timer` 缺失发现。
3. **x2APIC 的静默丢写**：若 CPUID 暴露 x2APIC，guest 用 `WRMSR 0x80b`
   （EOI）等 MSR 访问 LAPIC。但 `kvm_set_apic_base()`（`x86.c:671-690`）
   在 guest CPUID **没有** `X86_FEATURE_X2APIC` 时把 `X2APIC_ENABLE`
   （`BIT(10)`，`apicdef.h:153`）当保留位拒绝（:675-676）；且
   `kvm_x2apic_msr_write()`（`lapic.c:3308`）在非 x2APIC 模式下**静默
   返回**（:3313 检查 `apic_x2apic_mode()`）。第一版保持 xAPIC
   （in-kernel LAPIC 处理 MMIO），x2APIC 留作进阶选项。详见
   `../phase7-timer-virt/practice/` 勘误 27。
4. **boot_params 字段漏填**：`cmdline_size`（协议 2.06+）、`heap_end_ptr`、
   e820 表缺一不可；e820 缺 RAM 区域时内核启动早期直接崩，串口只打出
   几行解压日志。
5. **GPA 布局冲突**：boot_params/cmdline/initrd 必须落在 e820 声明的
   RAM 内，且避开 TSS/identity map 保留页，否则三重故障（`KVM_EXIT_SHUTDOWN`）。
6. **LSR 读返回 0 卡死 console**：8250 驱动以 `LSR.THRE` 判断可发送。

---

## 验收标准

- [ ] 从 `KVM_CREATE_VM` 到 shell 提示符，全程自己实现，不依赖
      QEMU/Cloud Hypervisor 等
- [ ] guest `dmesg` 显示 `clocksource: kvm-clock`，`/proc/interrupts`
      可见 `LOC`（local timer）计数增长
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
