# Stage 1: VMX 基础 - VMXON, VMCS, VM-Entry

> 对应课程 Phase 1: VT-x Basics
>
> 关键源码: `arch/x86/kvm/vmx/vmx.c::vmx_hardware_setup()`
>           `arch/x86/kvm/vmx/vmenter.S::__vmx_vcpu_run()`

---

## 🎯 阶段目标

实现 VMX 的基本操作：
- 检测 CPU 是否支持 VMX
- 执行 VMXON 启用 VMX 操作
- 配置 VMCS (Virtual Machine Control Structure)
- 执行首次 VM-Entry 进入 Guest 模式
- 处理第一次 VM-Exit 返回 Host

## 📖 核心概念

### VMX 操作模式

Intel VT-x 引入两种操作模式：
- **VMX Root Mode**: Host (VMM) 运行在 root 模式，拥有完全控制权
- **VMX Non-Root Mode**: Guest 运行在 non-root 模式，受限执行

模式切换通过 VM-Entry/VM-Exit 指令实现：
```
VMX Root Mode (Host)
    │
    ├── VMLAUNCH/VMRESUME ──→ VM-Entry
    │                              │
    │                              ▼
    │                    VMX Non-Root Mode (Guest)
    │                              │
    │                         Guest 执行...
    │                              │
    │                    VM-Exit (触发条件满足)
    │                              │
    └────── VM-Exit ───────────────┘
```

### VMCS (Virtual Machine Control Structure)

VMCS 是一个 4KB 的内存区域。SDM §25.3 "Organization of VMCS Data" 把里面的
数据分成**六组**（不是四组）：

| 组 | 作用（§25.3 原话的要义） |
|---|---|
| Guest-state area | VM-Exit 时把处理器状态存进来，VM-Entry 时从这里装载 |
| Host-state area | **VM-Exit 时从这里装载**处理器状态 |
| VM-execution control fields | 控制 non-root 下的处理器行为，并部分决定 VM-Exit 的原因 |
| VM-exit control fields | 控制 VM-Exit 本身 |
| VM-entry control fields | 控制 VM-Entry 本身 |
| VM-exit information fields | 接收 VM-Exit 的原因与上下文；某些处理器上只读 |

前三组之后统称为 "VMX controls"。VMCS 的内存布局是
implementation-specific（§25.2 "Format of the VMCS Region" 只规定了首页的
revision identifier 与大小），软件只能通过 §24.5 说的 VMCS pointer +
VMREAD / VMWRITE / VMCLEAR 去访问它，不能按结构体直接读内存。

## 🔧 实现（`main.c` + `vmx.c` + `vmx_entry.S`）

### 1. 把整机带进 VMX 操作模式（`main.c`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/main.c:264-300（on_each_cpu 回调，日志略） */
static void mini_vmx_enable_one(void *info)
{
	int cpu = raw_smp_processor_id();
	struct page *page = per_cpu(mini_vmxon_page, cpu);
	u64 phys = page_to_phys(page);

	if (atomic_read(&mk_global.enable_err))
		return;

	/* CPUID + 本 CPU 的 MSR_IA32_FEAT_CTL，见下面的第 1 条 */
	if (!mini_cpu_vmx_supported()) { ... -ENODEV ... }

	/* 已有 VMX 用户（典型是没卸载的 kvm_intel）—— 拒绝，避免互踩 */
	if (cr4_read_shadow() & X86_CR4_VMXE) { ... -EBUSY ... }

	/* 必须走 CR4 影子，不能裸写 CR4 —— 见下面的第 5 条 */
	cr4_set_bits(X86_CR4_VMXE);

	/* 见下面第 3 条：VMXON 会同步 fault，必须收进 exception table */
	if (mini_cpu_vmxon(phys)) {
		cr4_clear_bits(X86_CR4_VMXE);
		atomic_set(&mk_global.enable_err, -EIO);
		return;
	}

	per_cpu(mini_vmxon_done, cpu) = true;
	atomic_inc(&vmx_enable_count);
}
```

```c
/* 来源: phase8-capstone/practice/mini-kvm/main.c:130-157 */
static int mini_cpu_vmxon(u64 phys)
{
	asm goto("1: vmxon %[ptr]\n\t"
		 "jz  %l[vmfail]\n\t"
		 "jc  %l[vmfail]\n\t"
		 _ASM_EXTABLE(1b, %l[fault])
		 :
		 : [ptr] "m"(phys)
		 : "cc", "memory"
		 : vmfail, fault);

	return 0;

vmfail:	... 	return -EIO;
fault: 	... 	return -EIO;
}
```

五处容易漏：

1. **VMX 支持判定必须逐 CPU 做**（`main.c:77` 的 `mini_cpu_vmx_supported()`，
   在 IPI 回调里调用）。CPUID 与 `MSR_IA32_FEAT_CTL`(0x3A) 都是**每个逻辑
   处理器独立**的状态，而 §31.3 VMXON 的伪码把 "bit 0 (lock bit) of
   IA32_FEATURE_CONTROL MSR is clear" 和 "outside SMX operation and bit 2 …
   is clear" 直接写成 `#GP(0)` 条件 —— 在加载线程那台 CPU 上读一次，不能替
   别的 CPU 作保。对照 KVM 的两层：模块加载时给每个在线 CPU 下发一次
   `smp_call_function_single(cpu, kvm_x86_check_cpu_compat)`（`x86.c:9828`，
   回调 `x86.c:9736-9739` → `:9733` → `__kvm_is_vmx_supported()`，
   `arch/x86/kvm/vmx/vmx.c:2782-2798`），并且 `kvm_arch_hardware_enable()` 在真正要 VMXON 的
   那台 CPU 上、VMXON 之前又跑一遍（`x86.c:12694`）。

2. **VMXON 区域首页要写 revision identifier**（本模块 `main.c:420`），值取
   `IA32_VMX_BASIC[30:0]`；VMCS 页同样要写（`vmx.c:692`）。规范出处是
   §25.11.5 "VMXON Region"（"it should write the 31-bit VMCS revision
   identifier to bits 30:0 of the first 4 bytes … bit 31 should be cleared to
   0"）与 §25.2；写错不是静默失败，§31.3 的伪码是
   `rev[30:0] ≠ VMCS revision identifier → VMfailInvalid`。

3. **VMXON 会同步 fault，光看 CF/ZF 收不住。** §31.3 的 Operation 伪码 +
   Protected Mode Exceptions 列出：`#UD`（操作数是寄存器 / CR0.PE=0 /
   CR4.VMXE=0 / RFLAGS.VM=1 / LMA=1 且 CS.L=0）、`#GP(0)`（CPL>0、A20M、
   CR0/CR4 fixed 位不符、FEATURE_CONTROL 不支持，**以及访问内存操作数本身**
   ——有效地址越出段界限、段不可用、落在只执行段）、`#PF`/`#SS`（读操作数）。
   这个回调跑在 IPI 的硬中断上下文，任何一次 fault 都是宿主 die()，别的 CPU
   还会卡在 `smp_call_function_single` 的等待里。所以必须照 KVM 的
   `kvm_cpu_vmxon()`（`arch/x86/kvm/vmx/vmx.c:2833-2851`）用
   `asm goto(...) + _ASM_EXTABLE(1b, %l[fault])` 把 fault 导向修复分支，
   再回滚 `CR4.VMXE`。
   顺带一个反直觉的点：**已经在 VMX root operation 里再执行 VMXON 不 #UD**，
   伪码最后一条是 `VMfail("VMXON executed in VMX root operation")` =
   §31.4 Table 31-1 错误号 15。所以拦"和别的 VMM 互踩"不能指望指令自己报错，
   只能像上面那样先查 `CR4.VMXE`（对照 `arch/x86/kvm/vmx/vmx.c:2859-2860`）。
4. **ZF 和 CF 两档失败都要收。** §31.2 里 `VMfailInvalid` 是 CF=1/ZF=0，
   `VMfailValid` 是 CF=0/ZF=1 并写 `VM_INSTRUCTION_ERROR`；而 §31.2 的
   `VMfail` 伪函数按 "current VMCS 指针是否有效" 分流 —— 于是
   "已 VMXON 且已 VMPTRLD 过再 VMXON" 给出的是 **ZF** 而不是 CF。只写
   `setna`（= ZF|CF）或只判 `jc` 都会漏。
   对照 KVM：`kvm_cpu_vmxon()`（`arch/x86/kvm/vmx/vmx.c:2839-2843`）里
   `asm goto` 只挂了 `_ASM_EXTABLE` 的 fault 分支，指令正常返回就一律
   `return 0` —— **CF/ZF 根本不判，VMfail 会被静默放过**。KVM 敢这样是因为
   它独占 VMX 且前面有两层 CPUID/FEAT_CTL 兼容检查兜底；mini-kvm 的定位是
   "和 kvm_intel 抢同一台机器"，所以这两档失败必须收，并且要把
   `VM_INSTRUCTION_ERROR` 打出来（`vmx.c::mini_vmx_report_error()`）。

5. **CR4.VMXE 只能走内核的 CR4 影子，裸写 `MOV to CR4` 会在进程切换里炸宿主。**
   上一版本这里写的是"`cr4_set_bits()`/`cr4_clear_bits()` 没有导出给模块，只能
   裸读写 CR4" —— **这句话本身是错的**：两个函数是 `asm/tlbflush.h:41`/`:51` 的
   `static inline`，唯一的内核调用 `cr4_update_irqsoff()` 在
   `arch/x86/kernel/cpu/common.c:453-465` 有 `EXPORT_SYMBOL`。两条可复现的证据：
   `nm -u mini-kvm.ko | grep cr4` → `U cr4_update_irqsoff` + `U cr4_read_shadow`；
   `grep cr4_update_irqsoff /lib/modules/$(uname -r)/build/Module.symvers` →
   `EXPORT_SYMBOL`（同一张表里 `vmcs_read*` / `vmcs_write*` 一个都没有，那才是
   真的用不了，见 `vmx.c` 里 VMCS 访问包装的说明）。而裸写的后果是致命的：

   - 内核把 CR4 的权威副本记在 per-CPU 影子 `cpu_tlbstate.cr4` 里，
     `cr4_update_irqsoff()` 的动作是"读影子 → 算新值 → 写回影子 → `MOV CR4`"
     （`common.c:455-462`）。`switch_mm_irqs_off()`（`arch/x86/mm/tlb.c:499`）
     每次换 mm 都调 `cr4_update_pce_mm(next)`（`:658` → `:469-482`），里面有
     `cr4_set_bits_irqsoff(X86_CR4_PCE)` / `cr4_clear_bits_irqsoff(...)`。
     VMXE 只进了真实 CR4、没进影子，下一次 PCE 变化就会拿"没有 VMXE 的影子值"
     去 `MOV CR4`。
   - 这一条 `MOV CR4` 在 VMX root operation 里是被硬件拦住的：§24.8 第一条
     "Any attempt to set one of these bits to an unsupported value while in VMX
     operation (including VMX root operation) using any of the CLTS, LMSW, or
     MOV CR instructions causes a general-protection exception"，紧跟的 NOTE
     列出 VMX operation 下必须为 1 的位含 **CR4.VMXE**。于是宿主在进程切换路径
     上吃一个 #GP(0) —— 比 guest 起不来严重得多。§24.7 那句
     "Once in VMX operation, it is not possible to clear CR4.VMXE" 说的就是这个
     状态。
   - 影子还是上面那句 `cr4_read_shadow()` 预检的数据来源（`common.c:467-472`
     导出）：裸写让我们自己的 VMXE 在影子里不可见，第二次 `insmod` 拦不住，
     随后 `kvm_intel` 加载也拦不住（它查的同样是影子，
     `arch/x86/kvm/vmx/vmx.c:2859-2860`），两个 VMXON 用户叠在同一台 CPU 上。

   KVM 的写法就是答案：`cr4_set_bits(X86_CR4_VMXE)`（`arch/x86/kvm/vmx/vmx.c:2837`）、
   `cr4_clear_bits(X86_CR4_VMXE)`（`:2848`，以及 `kvm_cpu_vmxoff()` 的 `:749`/`:753`）。
   这两个 inline 内部 `local_irq_save/restore` + `lockdep_assert_irqs_disabled()`，
   我们的调用点都在 `on_each_cpu()` 的 IPI 回调里，条件成立。

### 2. 控制域协商（`vmx.c::mini_compute_controls()`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/vmx.c:271-325（错误处理略） */
c->pin_based  = mini_vmx_adjust_control(PIN_BASED_EXT_INTR_MASK | PIN_BASED_NMI_EXITING, ...);
c->cpu_based  = mini_vmx_adjust_control(CPU_BASED_HLT_EXITING |
					CPU_BASED_UNCOND_IO_EXITING |
					CPU_BASED_ACTIVATE_SECONDARY_CONTROLS, ...);
c->secondary  = mini_vmx_adjust_control(SECONDARY_EXEC_ENABLE_EPT,
					MSR_IA32_VMX_PROCBASED_CTLS2);
c->exit       = mini_vmx_adjust_control(VM_EXIT_HOST_ADDR_SPACE_SIZE |
					VM_EXIT_LOAD_IA32_EFER, ...);
c->entry      = mini_vmx_adjust_control(VM_ENTRY_IA32E_MODE |
					VM_ENTRY_LOAD_IA32_EFER, ...);
```

- 每组的"想要的位"都要过一遍能力 MSR：`adjust` 先把它与"允许为 1"段取交集、
  再并上"必须为 1"段（MSR 低 32 位 = 必须为 1，高 32 位 = 允许为 1，
  `vmx.c:251-260`；规范正文只在 §24.8/§25.x 里转引，逐位定义在 Appendix A.3-A.5
  ——**附录不在本仓库这份 PDF（只含 Vol.3C）里，这里以内核头文件为准**）。
  `BASIC[55]=1` 时用 `TRUE_*` 那四个 MSR，否则用旧的（本模块 `vmx.c:273`；
  位定义 `arch/x86/include/asm/vmx.h:134`）。次级控制只有一个 MSR，没有
  `TRUE_*` 变体，判据是能直接读到的编号清单
  `arch/x86/include/asm/msr-index.h:1182-1200`：`TRUE_*` 只有 0x48d-0x490
  四个（pin/proc/exit/entry），0x48b 的 `PROCBASED_CTLS2` 与 0x492 的
  `CTLS3` 都是单一编号。
- **协商结果必须回读校验**：`adjust` 只削位不报错，硬件不支持时想要的位会
  静默消失。EPT 位没保住就直接拒绝建 vCPU（本模块 `vmx.c:317-321`）—— mini-kvm 没有
  "不用 EPT 也能跑"的第二条路（KVM 可以退回影子页表，所以它只是
  `enable_ept = 0`，`arch/x86/kvm/vmx/vmx.c:8434-8438`）。
- IO 用的是 `CPU_BASED_UNCOND_IO_EXITING`（bit 24，所有 IO 一律退出），不开
  `CPU_BASED_USE_IO_BITMAPS`（bit 25）。这两个不是互斥关系：SDM Table 25-6 对
  bit 25 写的是 "If the I/O bitmaps are used, the setting of the
  'unconditional I/O exiting' control is **ignored**" —— bitmap 一旦启用就
  接管判定（§26.1.3）。mini-kvm 没有 bitmap 页，所以只留 bit 24。
  （6.12.93 里根本没有 `CPU_BASED_IO_EXITING` 这个名字，只有
  `arch/x86/include/asm/vmx.h:43-44` 这两个。）

### 3. Guest 状态（`vmx.c::mini_vmx_set_guest_state()`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/vmx.c:561-638（部分字段与注释略） */
mini_vmwrite(GUEST_RIP, MINI_KVM_GUEST_RIP);	/* 0x1000 */
mini_vmwrite(GUEST_RSP, MINI_KVM_GUEST_RSP);	/* 0x100000 */
mini_vmwrite(GUEST_CR3, MINI_KVM_GUEST_CR3);	/* 0x6000，用户态建的页表 */
mini_vmwrite(GUEST_RFLAGS, X86_EFLAGS_FIXED);	/* 0x2，bit1 恒 1 */
mini_vmwrite(GUEST_CR0, MINI_GUEST_CR0);	/* PE|MP|ET|NE|WP|PG */
mini_vmwrite(GUEST_CR4, MINI_GUEST_CR4);	/* PAE|VMXE（vmx.c:554） */
mini_vmwrite(GUEST_IA32_EFER, MINI_GUEST_EFER);	/* SCE|LME|LMA */

mini_vmwrite(GUEST_CS_SELECTOR, 0x8);
mini_vmwrite(GUEST_CS_AR_BYTES, MINI_SEG_AR_CS64);	/* 0xA09B：P=1 L=1 D=0 */
mini_vmwrite(GUEST_SS_AR_BYTES, MINI_SEG_AR_LDT_UNUSABLE);	/* 0x10000 */
/* DS/ES/FS/GS/LDTR 同样标记不可用 */
mini_vmwrite(GUEST_FS_BASE, 0);
mini_vmwrite(GUEST_GS_BASE, 0);
mini_vmwrite(GUEST_TR_SELECTOR, 0x10);
mini_vmwrite(GUEST_TR_LIMIT, 0x67);			/* 64 位 TSS 大小 */
mini_vmwrite(GUEST_TR_AR_BYTES, MINI_SEG_AR_TSS64);	/* 0x8B：64 位忙 TSS */
```

要点：
- **CR4 必须带 VMXE**（`MINI_GUEST_CR4 = X86_CR4_PAE | X86_CR4_VMXE`，
  `vmx.c:554`）。"非根模式下 VMXE 该是 0"是错的：§24.8 的 NOTE 把 CR4.VMXE 列为
  **VMX operation 期间必须为 1** 的位，而 VMX operation 含 root 与 non-root 两种
  状态；§27.3.1.1 直接对 guest CR4 字段做这条检查（"The CR4 field must not set any
  bit to a value not supported in VMX operation (see Section 24.8)"）。
  unrestricted guest 只豁免 CR0.PE / CR0.PG，不豁免 CR4.VMXE。对照 KVM：三个
  `KVM_*_VM_CR4_ALWAYS_ON*` 常量全都含 `X86_CR4_VMXE`
  （`arch/x86/kvm/vmx/vmx.c:156-158`），`vmx_set_cr4()` 无论走哪条分支都或上它
  （`:3481-3487`），最后 `vmcs_writel(GUEST_CR4, hw_cr4)`（`:3528`）。漏这一位 =
  VM entry 直接失败，退出原因 33（§27.8），见 Stage 5。
- 段寄存器的进入检查在 §27.3.1.2，几条会真实咬人的规则：
  - **CS 的 `L` 位没有任何"必须为 1"的检查**。规范里唯一涉及 L 的是
    "For CS, D/B must be 0 if the guest will be IA-32e mode and the L bit (bit 13)
    in the access-rights field is 1"。`L=1` 是我们要跑 64 位 guest 自己选的
    （`MINI_SEG_AR_CS64 = 0xA09B`：P=1 L=1 D=0 S=1 Type=0xB DPL=0 G=1）；
    `IA-32e mode guest=1` 而 `CS.L=0` 是合法的，guest 会以兼容模式开始执行。
  - CS 必须**可用**（unusable 位是 AR 的 bit16，而 §27.3.1.2 对 CS 无条件要求
    "Bits 31:17 (reserved) ... must all be 0"）；Type 必须是 9/11/13/15
    （unrestricted guest=0 时）；**Type 为 9 或 11 时 CS.DPL 必须等于 SS 访问权字段的
    DPL** —— 我们的 SS 标成不可用且 AR=0x10000（DPL=0），所以 CS.DPL 必须是 0。
  - **TR 必须可用**（"Bit 16 (Unusable). The unusable bit must be 0"），且
    "If the guest will be IA-32e mode, the Type must be 11 (64-bit busy TSS)" ——
    只有 0xB 一个合法值（16/32 位忙 TSS 的 3/11 之分属于**非** IA-32e 分支）。
    另外 S 必须 0、P 必须 1。
  - G 位与 limit 是双向约束："If any bit in the limit field in the range 11:0 is
    0, G must be 0" / "If any bit in the limit field in the range 31:20 is 1, G
    must be 1"。CS limit=0xFFFFFFFF → 必须 G=1；TR limit=0x67 → 必须 G=0
    （`MINI_SEG_AR_TSS64 = 0x8B`，bit15=0 ✓）。
  - SS/DS/ES/FS/GS/LDTR **允许**不可用（AR 的 bit16 = 1），这是 64 位平坦模型
    最省事的做法。
- **FS/GS 的 base 是独立字段，选择子为空也照样参与寻址**，VMCLEAR 之后 VMCS
  字段初值不可依赖，必须显式清零（`vmx.c:618-619`）。
- guest 自己 `lidt` 到 0x2000 装 IDT；`GUEST_IDTR_BASE` 初始写 0 就够了。
  GDTR 完全不用（没有远跳转）。IDTR/GDTR 基址要 canonical、limit 高 16 位必须为 0
  （§27.3.1.3）。

### 4. VM-Entry / VM-Exit 的世界切换（`vmx_entry.S`）

C 里写 `asm volatile("vmresume")` 是不够的：VM-Exit 之后 CPU 只保证按
host-state 区恢复 RSP/RIP/CR*/FS.base/GS.base，**通用寄存器全是 guest 的
值**，必须自己把它们换回来并把宿主的被调用者保存寄存器恢复。本模块照
`vmenter.S::__vmx_vcpu_run()` 手写：

```
进入：保存 rbp/rbx/r12-r15 + vcpu 指针 + launched 标志到栈
      VMWRITE HOST_RSP（字段编码必须是寄存器操作数，§31.3 VMWRITE）
      bt 取 launched → CF；按 regs[] 装载 guest 寄存器，**RAX 最后装**
      （装载 RAX 的那条 mov 用的正是 regs[] 的指针，装完指针就没了）
      jnc VMRESUME / VMLAUNCH
退出（HOST_RIP = mini_vmx_vmexit）：
      把宿主寄存器存进 regs[] → pop 出暂存的 guest RAX 那一格
      xor %ebx,%ebx（返回值 0）→ 丢弃 16 字节 → 清零全部 GPR →
      逆序 pop rbx/r12-r15/rbp → RET
entry 失败：mov $1,%ebx 后并入同一条收栈路径
```

最后一条的判据是"**那条 VM 指令居然返回了**"，不是"CF=1"。VM entry 的失败分三
档，前两档落到这里：§27.1 的基本检查（无 current VMCS / 当前是 shadow VMCS →
CF；被 MOV-SS 阻塞、launch state 与指令不匹配 → ZF + 错误号）与 §27.2 的控制域
和宿主状态检查（ZF + 错误号），它们**都不装载宿主状态、都不产生 VM-Exit**，只是
把控制交给下一条指令。第三档（§27.3.1 guest 状态非法、§27.4 进入时 MSR 装载
失败）才按 §27.8 装载宿主状态并记录带 bit31 的 exit reason，走的是 `vcpu.c` 的
bit 31 分支。前两档在 `vmx_entry.S` 里落到同一个 `.Lvmfail`，CF/ZF 分不开它们，
能分开它们的只有 `VM_INSTRUCTION_ERROR`（解码见 `mini_vmx_report_error()`）。

`launched` 决定用 VMLAUNCH 还是 VMRESUME：VMCS 的 launch state 是 "clear"
时执行 VMRESUME 会得到 VM-instruction error 5（§31.4 Table 31-1）。
VMCLEAR 会把 launch state 置回 "clear"（§25.11.3），所以每次迁移都要连带
清掉这个标志。完整理由与踩过的坑见 README 第 7 节与 `vmx_entry.S` 头部注释。

## 🧪 实验验证

```bash
make && sudo insmod mini-kvm.ko      # 上机前的检查见 ../README.md 第 4 节
sudo dmesg | grep mini-kvm | tail
# mini-kvm: VMX_BASIC=0x... revision=0x... true_ctls=1
# mini-kvm: EPT_VPID_CAP=0x... INVEPT(context=1 global)
# mini-kvm: CR0 FIXED0=... FIXED1=... CR4 FIXED0=... FIXED1=...
# mini-kvm: VMXON 完成，N/N 个 CPU 进入 VMX 操作模式
# mini-kvm: VMCS 初始化完成 (vcpu=0 rev=0x... pin=... cpu=... sec=... exit=... entry=...)
# mini-kvm: vCPU 上机 CPU-1→CPU5, VMCS 重新加载        ← 第一次进入前也算"迁移"
```

最后一行的五组控制域值就是第 2 节协商的结果；`insmod` 失败时的三种拒绝原因
（BIOS 未开放 / EPT 能力不足 / CR4.VMXE 已被占用）也都在 dmesg 里。

### 不加载模块也能做的静态自检

共享机器上不能随便 `insmod`（要先卸载 `kvm_intel`，会打断宿主上所有 VM），而
本机 `/dev/cpu/*/msr` 全部返回 EIO，能力 MSR 一个都读不到 —— 下面四条是不上机
也能验的部分：

```bash
# 1. CR4 走的是内核影子，不是裸写（第 5 条）。看到 cr4_update_irqsoff 说明
#    tlbflush.h 的 cr4_set_bits() 真的能链进模块
nm -u mini-kvm.ko | grep cr4

# 2. 全模块只剩"读"CR4 的地方，没有任何裸 MOV to CR4
grep -n 'mov %0,%%cr4' *.c *.S			# 应当无输出

# 3. "VM 指令居然返回了"一定被收口：两条进入指令后面都紧跟同一个 jmp
objdump -d --no-show-raw-insn mini-kvm.ko | grep -A1 -E "\bvmresume\b|\bvmlaunch\b"
#   75:  vmresume
#   78:  jmp    10d <mini_vmx_vmexit+0x7d>     ← .Lvmfail（`vmresume` 返回=失败）
#   7d:  vmlaunch
#   80:  jmp    10d <mini_vmx_vmexit+0x7d>     ← 同一处

# 4. 本文档与 README/corrections.md 里每一条 `file:line` 都落在真实代码上
#    （行号漂移是本项目最容易复发的错，见 corrections.md J10(a′)/J11(6)）
./check-refs.py --quiet				# README + 五篇 stage
./check-refs.py --quiet ../../corrections.md
./check-refs.py --context 2			# 想看每条引用的原文就去掉 --quiet
```

能力 MSR 的实际数值（`VMX_BASIC`、`EPT_VPID_CAP`、`CR0/CR4_FIXED0/1`）只能等
上机时由模块自己在 `insmod` 里打印，本文档不引用任何"作者在本机量到"的数字。
（上面第 3 条的地址随构建变化，稳定的判据是"两条进入指令各自紧跟同一个 `jmp`"
这个结构。）

## 📝 检查清单

完成 Stage 1 后，确认能回答：
- [ ] VMX Root Mode 和 Non-Root Mode 的区别
- [ ] §25.3 把 VMCS 数据分成哪六组（guest-state / host-state / 三类控制域 /
      VM-exit information）
- [ ] VMXON 区域的作用和格式
- [ ] VM-Entry 时 CPU 做了哪些事
- [ ] VM-Exit 时 CPU 做了哪些事
- [ ] 为什么需要 CR4.VMXE —— 既要宿主的 CR4.VMXE（还要进内核影子，见第 5 条），
      也要 VMCS 里 GUEST_CR4 的 CR4.VMXE（§24.8 NOTE + §27.3.1.1，见第 3 节）
- [ ] VM entry 失败的三档分别怎么观测：CF、ZF + `VM_INSTRUCTION_ERROR`、
      bit31 的 exit reason（§27.1 / §27.2 / §27.8）

## 🔗 下一步

Stage 2: 内存虚拟化 (EPT)
