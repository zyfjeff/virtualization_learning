# Mini-KVM：自己动手写一个最小 KVM 内核模块

> Phase 8 毕业建造 · KVM 内核侧。用户态侧的对应项目是
> `phase8-capstone/practice/minivmm.c`（用官方 KVM 当后端的完整 VMM）。
>
> **构建目标**：正在运行的内核（`KDIR = /lib/modules/$(uname -r)/build`，
> vermagic 一致才能 `insmod`）。
> **参考实现**：Linux 6.12.93 —— `/root/code/linux-6.12.93/`，只用于阅读与
> 引用，不是构建目标。文中所有不带路径的 `文件:行号` 都指本目录。
>
> **同名歧义**：本模块与内核树的 `arch/x86/kvm/vmx/` 都有 `vmx.c`。约定是——
> 指本模块时一定带"本模块"或把 "`文件:行号`" 写在代码块的 `来源:` 行里；指 KVM
> 时要么带函数名（如"`vmx_set_cr4()` 在 `vmx.c:3470`"），要么写
> `arch/x86/kvm/...` 全路径。**行号只要落在本目录同名文件的长度之内（`vmx.c`
> 800 余行、`main.c` 500 余行）就必须写全路径**，光靠"4xxx 一看就是内核树"这种
> 量级提示，只在明显超出本地文件长度时才成立。
>
> **规范引用**：`intel-vmx.pdf` = Intel 文档号 **326019-083US**（2024 年 3
> 月），本仓库这份只含 **Volume 3C**，正文止于该卷第 33 章的 33-84 页。所以
> 两件事必须说清：(1) `§22.x` / `§23.x` 这类编号在这份 PDF 里**根本没有对应
> 章节** —— Vol.3C 从第 24 章开始，那些数字是 KVM 源码里化石级注释的遗留
> （`vmx_set_constant_host_state()` 里的 `/* 22.2.3 */`，
> `arch/x86/kvm/vmx/vmx.c:4328` 等，说明见本模块 `vmx.c` 写 host CR0/CR3/CR4
> 那段注释）；(2) Vol.3C 里以 "see Appendix
> A.x" / "see Table 12-7" 形式转引的内容（附录 A 的 VMX capability MSR、
> PAT/MTRR 的组合表、IDT 门描述符格式等）**都不在本仓库这份 PDF 里，本地无
> 法复核**。引用它们时只写"由 Vol.3C §x.y 转述"，不假装查过原文。

---

## 1. 这个项目做到什么

一个 `mini-kvm.ko`，注册 `/dev/mini-kvm`，用**和 KVM 一样的三层 fd 模型**
（kvm fd → VM fd → vCPU fd）跑起一个 64 位裸机 guest：

```
用户态 test-mini-kvm                     内核 mini-kvm.ko
────────────────────                     ──────────────────
open /dev/mini-kvm                       misc 设备
KVM_CREATE_VM          ──────────────→   struct mini_kvm（EPT 根 + 串口缓冲）
mmap 2MB + memset                        （用户态提供 guest 内存）
KVM_SET_USER_MEMORY_REGION ───────────→  pin_user_pages() → struct mini_kvm_memslot
写入 guest.bin 与 guest 页表              （直接写用户内存，内核不感知）
KVM_CREATE_VCPU        ──────────────→   struct mini_kvm_vcpu + 一页 VMCS
mmap(vcpu_fd)          ──────────────→   struct kvm_run 共享页
KVM_RUN                ──────────────→   运行循环：VMPTRLD → VMRESUME
                                         ├ EPT violation → 按需建 4KB 映射
                                         ├ IO 0x3f8      → 串口捕获
                                         └ HLT           → KVM_EXIT_HLT 回用户态
MINI_KVM_VM_GET_SERIAL ──────────────→   读回 guest 打印的字符
MINI_KVM_VCPU_INJECT_IRQ(0x21) ──────→   排队，下次进入前写 VM_ENTRY_INTR_INFO_FIELD
KVM_RUN                ──────────────→   guest 的 IDT handler 打印后 iretq → 再次 HLT
```

验收判据（`test-mini-kvm` 自动检查）：第一次 `KVM_RUN` 以 `KVM_EXIT_HLT`
收场且串口含 `Hello from Mini-KVM Guest!`；注入 vector `0x21` 后第二次
`KVM_RUN` 仍回到 HLT 且串口含 `[IRQ 0x21 handled]`。

**验证状态（2026-09-01）**：本目录代码只完成了编译（`make` 通过，
`objdump` 校验收栈序列）与逐条引用审计；**尚未真机 `insmod` 跑通验收**，
因为那要求先 `rmmod kvm_intel kvm`，会让本机所有 VM 下线。第 4 节的上机
流程是给具备条件的机器准备的。

## 2. 文件结构

| 文件 | Stage | 内容 | 对照的 KVM 实现 |
|---|---|---|---|
| `mini-kvm.h` | — | 数据结构、常量、私有 ioctl、`mini_rdmsr()/mini_wrmsr()` | `include/linux/kvm_host.h`、`arch/x86/kvm/vmx/vmx.h` |
| `main.c` | 1 | 能力 MSR 采集、per-CPU CR4.VMXE + VMXON/VMXOFF、模块生命周期 | `vmx_hardware_setup()`、`kvm_cpu_vmxon()`（`vmx.c:2833-2851`）、`kvm_cpu_vmxoff()`（`arch/x86/kvm/vmx/vmx.c:743-755`） |
| `vmx.c` | 1 | VMX 指令包装、控制域协商、VMCS guest/host 状态初始化、VMCS 迁移与 VMCLEAR | `init_vmcs()`、`vmx_set_constant_host_state()`（`vmx.c:4320-4385`）、`vmx_vcpu_load_vmcs()`（`vmx.c:1449-1514`） |
| `vmx_entry.S` | 1 | 手写世界切换：VMLAUNCH/VMRESUME 进入、VM-Exit 着陆点、寄存器换入换出与收栈 | `arch/x86/kvm/vmx/vmenter.S`（`__vmx_vcpu_run`） |
| `ept.c` | 2 | 4 级 EPT 手工建表、EPT violation 按需映射、INVEPT | TDP MMU（`arch/x86/kvm/mmu/tdp_mmu.c`）、`construct_eptp()`（`vmx.c:3411-3423`） |
| `interrupt.c` | 3 | 中断排队/取用、NMI 转注（与 KVM 分歧，见 stage3） | `vmx_inject_irq()`（`vmx.c:4958`）、`vmx_do_nmi_irqoff()`（`vmx.c:7336`）、`KVM_NMI` ioctl（`x86.c:5193`） |
| `device.c` | 4 | IO 退出解码（Table 28-5）+ COM1 串口捕获 | `handle_io()`（`vmx.c:5401`） |
| `vcpu.c` | 5 | 三层 fd 的 ioctl/mmap、运行循环与退出分发 | `kvm_dev_ioctl()`/`kvm_vm_ioctl()`/`kvm_vcpu_ioctl()`、`vcpu_run()`（`x86.c:11343`）、`vcpu_enter_guest()`（`x86.c:10777`） |
| `test-mini-kvm.c` | — | 用户态验收程序 | QEMU/crosvm 的 KVM 调用序列 |
| `guest/guest.S`、`guest/guest.ld` | 3-5 | guest 裸机镜像：自建 IDT、串口输出、sti+hlt | — |
| `stages/stage1..5.md` | — | 每个 Stage 的原理笔记（已按实现重写，`file:line` 由 `check-refs.py` 机械核对） | — |
| `check-refs.py` | — | 把文档里所有 `file:line` 解析成原文打印，报越界/找不到 | — |

## 3. 构建

```bash
cd phase8-capstone/practice/mini-kvm
make            # 内核模块 mini-kvm.ko
make guest      # guest 裸机镜像 guest/guest.bin（gcc + ld + objcopy）
make user       # 用户态测试程序 test-mini-kvm
make clean
```

`make` 只建模块（`all: modules`）；测试程序要先 `make guest` 才能跑，
因为它从 `guest/guest.bin` 读镜像。

## 4. 上机：安全流程

这个模块**直接操纵宿主的 VMX 状态**。写错的后果不是"测试失败"，而是宿主
宕机（`vmx_entry.S` 的收栈序列里任何一个 pop 错位都是这种后果）。按顺序做：

```bash
# 1) 确认没有任何 VM 在跑：kvm  fd 引用数为 0
sudo fuser /dev/kvm                 # 无输出才可以继续
ls -l /proc/$(pgrep -f '^qemu-system-x86_64')/fd 2>/dev/null | grep -c kvm   # 必须是 0

# 2) 卸掉标准 KVM（mini-kvm 的 CR4.VMXE 预检会拒绝与 kvm_intel 共存，报 -EBUSY）
sudo rmmod kvm_intel kvm

# 3) 加载。insmod 失败时先看 dmesg，模块会给出拒绝原因（BIOS 未开放 VMX /
#    EPT 能力不足 / 已离线 CPU 等）
sudo insmod mini-kvm.ko
dmesg | tail -30

# 4) 跑验收，另开一个终端跟踪
dmesg -w &
sudo ./test-mini-kvm; echo "rc=$?"

# 5) 收尾：先退出所有 test-mini-kvm 进程，再卸载，然后恢复标准 KVM
sudo rmmod mini_kvm      # 文件名带 "-"、模块名带 "_"，kbuild 做的转换（见 corrections.md J10）
sudo modprobe kvm_intel
```

`make load` / `make unload` / `make restore` 是这几步的简写（`load` 会先做
第 1、2 步检查，不通过就拒绝）。

为什么不能和 `kvm_intel` 并存：一台逻辑处理器同一时刻只有一份 VMXON 区域
在生效 —— SDM Vol.3C §31.3 的 VMXON 伪码只有 `ELSIF not in VMX operation`
这一支会真正进入，已经在 VMX root operation 时执行 VMXON 得到的是
`VMfail("VMXON executed in VMX root operation")`（§31.4 Table 31-1 错误号
15），不是成功换一份区域；而 `kvm_intel` 已经把 `CR4.VMXE` 置上了。这一条
指令自己**不会**替我们报错（VMfail 只置标志），所以模块在 VMXON 之前显式查
`cr4_read_shadow() & X86_CR4_VMXE` 并回 `-EBUSY`，对照 KVM 的
`vmx_enable_virtualization_cpu()`（`vmx.c:2859-2860`）。

## 5. 用户态 API

复用 `linux/kvm.h` 的标准 ioctl，另加两个私有命令（magic `'M'`）：

| ioctl | fd | 说明 |
|---|---|---|
| `KVM_GET_API_VERSION` | kvm | 固定返回 `KVM_API_VERSION`(12) |
| `KVM_CHECK_EXTENSION` | kvm | 只认 `KVM_CAP_USER_MEMORY` → 1 |
| `KVM_CREATE_VM` | kvm | 分配 `struct mini_kvm` + EPT 根 |
| `KVM_GET_VCPU_MMAP_SIZE` | kvm | 一页；真实 KVM 在 x86 上返回三页（run + pio data + coalesced MMIO 环，`kvm_main.c:5552-5561`），按 pgoff 分派 |
| `KVM_SET_USER_MEMORY_REGION` | VM | 单 slot；`pin_user_pages()` 钉住整段；**只能设一次**，重复设返回 `-EEXIST` |
| `KVM_CREATE_VCPU` | VM | 单 vCPU；分配 VMCS 页并完成 VMCS 初始化 |
| `KVM_RUN` | vCPU | 运行循环；结果写 `kvm_run->exit_reason` |
| `MINI_KVM_VM_GET_SERIAL` | VM | `_IOR('M',0x01,char[256])` = `0x81004d01`，取串口捕获缓冲（NUL 结尾，只回传前 255 字节） |
| `MINI_KVM_VCPU_INJECT_IRQ` | vCPU | `_IOW('M',0x02,int)` = `0x40044d02`，参数 = vector；下次进入前注入 |

两个私有命令的宏在 `mini-kvm.h` 和 `test-mini-kvm.c` 里各抄了一份（用户态程序
不能 include 内核头）。两侧必须逐字符一致，编号差一位就是 `-ENOTTY`；上表的
十六进制值是手抄的锚点，改任何一侧都要重新核对（`gcc -include linux/kvm.h`
编个三行程序打印 `_IOR(...)` 即可复核）。

`KVM_RUN` 返回后 guest 寄存器在 VMCS 里（`kvm_run` 的 `sregs`/`kvm_regs`
未实现，这是简化点之一，见第 8 节）。

## 6. 运行循环处理哪些 VM-Exit

`vcpu.c` 的 `mini_vcpu_run_loop()`，进入前 `preempt_disable()`、进入/退出
瞬间 `local_irq_disable()`：

| Exit reason | 处理 | 之后 |
|---|---|---|
| `EXTERNAL_INTERRUPT` | 开/关中断的"瞬间窗口"让 pending 的宿主中断走宿主 IDT（`PIN_BASED_EXT_INTR_MASK` 使所有外部中断退出，且未设 `VM_EXIT_ACK_INTR_ON_EXIT`，向量仍 pending 在 LAPIC IRR）。**sti 和 cli 之间必须夹一条指令**，否则中断影子没走完就又被 cli 关住 → 每次进入立刻又以原因 1 退出 | 继续运行 |
| `EXCEPTION_NMI` | NMI → `mini_vcpu_reinject_nmi()`；其它向量 → 打印 guest RIP + dump VMCS | NMI 继续运行；异常 → `KVM_EXIT_INTERNAL_ERROR` |
| `IO_INSTRUCTION` | `mini_handle_io_exit()` 解码 Table 28-5，0x3f8 记入串口缓冲，推进 RIP；串 IO（bit 4）拒绝。整个处理在显式 `local_irq_enable()` 的窗口里（与 KVM 一致：`handle_exit()` 也在开中断后执行） | 继续运行 / 报错 |
| `EPT_VIOLATION` | `mini_ept_handle_violation()`：GPA ← `GUEST_PHYSICAL_ADDRESS`，查 memslot → 4KB 映射（`GFP_ATOMIC`） | 继续运行 |
| `CPUID` | 返回 0 并跳过指令（guest 不该执行它） | 继续运行 |
| `HLT` | 不推进 RIP（与 KVM 一致，恢复时重执行） | `KVM_EXIT_HLT` 回用户态 |
| `TRIPLE_FAULT` | 打印 + dump VMCS | `KVM_EXIT_SHUTDOWN` |
| 其它 / VM-Entry 失败（`exit_reason` bit31） | `mini_vmx_report_error()` + `mini_dump_vmcs()` | `KVM_EXIT_INTERNAL_ERROR` |

## 7. 五条最容易写错的硬件规则（本项目都踩过）

1. **VMCLEAR 必须落在最后持有该 VMCS 的 CPU 上。** SDM 25.11.1：
   "the first logical processor should execute VMCLEAR for the VMCS (to make it
   inactive on that logical processor and to ensure that all VMCS data are in
   memory) before the other logical processor executes VMPTRLD for the VMCS" ——
   因为 "a logical processor may maintain some VMCS data of an active VMCS on
   the processor and not in the VMCS region"，在新 CPU 上补一发 VMCLEAR 只改内存
   里的状态，旧 CPU 的片上副本仍然有效，于是同一 VMCS 可能 active 在两个逻辑
   处理器上（同一节：such a VMCS "may become corrupted"）。因此
   `mini_vmcs_clear()` 在 `preempt_disable()` 保护下比较 `loaded_cpu`，不同 CPU
   就 `smp_call_function_single()` 投递过去执行（对照 `vmx_vcpu_load_vmcs()` 里的
   `loaded_vmcs_clear()`，`vmx.c:1457`）。VMCLEAR 之后 launch state 变成
   "clear"，下一次进入**必须**用 VMLAUNCH（SDM 25.11.3），所以 `launched`
   标志要一起清。
2. **迁移逻辑必须在开中断时做。** `mini_vmcs_clear()` 可能发 IPI，而
   `smp_call_function_single()` 的注释直接写着 "Can deadlock when called with
   interrupts disabled."（`kernel/smp.c:647-653`，那里还有对应的
   `WARN_ON_ONCE`）。运行循环里多条 `continue` 路径是带着关中断回到循环头的，
   所以"上机"整段被提到循环之外、每次 `KVM_RUN` 只做一次 —— 这正是 KVM 的
   `vcpu_load()` 位置（`kvm_arch_vcpu_ioctl_run()` 进主循环前，
   `x86.c:11590` → `kvm_main.c:205-213` → `kvm_arch_vcpu_load()`
   `x86.c:4982` → `kvm_x86_call(vcpu_load)` `x86.c:5002`）。
3. **收栈纪律。** VM-Exit 之后通用寄存器里全是 **guest** 的值，`%rbp` 也是
   （VM-Entry 前刚装载的 guest RBP），所以任何"用 `%rbp` 当帧指针恢复栈"的
   写法都等于把 RSP 交给 guest 控制的数据。`vmx_entry.S` 严格照
   `vmenter.S`：VM-Exit 只保证恢复 RSP（"RSP is restored by hardware during
   VM-Exit"），其余靠**逆序 pop**；VM-Entry 失败与正常退出合并到同一条收栈
   路径（对照 `vmenter.S:300-303` 的 `.Lvmfail` 跳到 `:227` 的 `.Lclear_regs`，
   先清寄存器再 pop）。
4. **开中断窗口里必须夹一条指令。** STI 的中断影子要到**下一条指令执行完**才
   解除，所以 `sti` 紧跟 `cli` 等于刚把门推开又关上，pending 的可屏蔽中断一个都
   收不到（这条规则的定义在 Vol.3A 的 RFLAGS/STI 说明里，本仓库这份 PDF 只有
   Vol.3C，给不出可核对的章节号，故只引下面的源码）。本模块又**没有**开
   `VM_EXIT_ACK_INTR_ON_EXIT`，向量还挂在 LAPIC IRR（SDM 28.1："An external
   interrupt does not acknowledge the interrupt controller and the interrupt
   remains pending"），于是下一次 VM entry 立刻又以原因 1 退出 —— VM-Exit 风暴：
   guest 不再推进，宿主这个 CPU 的 tick 永远进不来（soft lockup / RCU stall）。
   KVM 的同一段窗口里放的正是 `++vcpu->stat.exits`，理由写在它自己的注释里：
   "An instruction is required after local_irq_enable() to fully unblock
   interrupts on processors that implement an interrupt shadow"
   （`arch/x86/kvm/x86.c:11149-11158`）。我们放的是 `vcpu->n_extint_exits++`
   （`vcpu.c:290-292`）。
5. **`HOST_RSP` 指向哪一格，决定退出后取栈上参数的偏移。** 硬件在 VM-Exit 只把
   `%rsp` 恢复成 `HOST_RSP`，之后每个 `N(%rsp)` 都以那一格为基准。KVM 的
   `HOST_RSP` 指向它压的**最后一格，而那格就是 `@regs`**（`push @regs` 在
   `vmenter.S:103`，地址交给 `vmx_update_host_rsp()` 在 `:108-109`），所以退出后
   重载用 `WORD_SIZE(%rsp)` = 8（`vmenter.S:203`）。本模块的 `HOST_RSP` 指向
   `launched`，`vcpu` 在它上面一格，退出路径又先 `push %rax` 暂存 guest RAX ——
   正确的偏移是 **16 不是 8**。照抄 KVM 的数字会把 `launched`（0 或 1）当指针，
   `pop REG_RAX(%rax)` 往 VA 0/1 开写，第一次 VM-Exit 就把宿主 #PF 掉。现在两侧
   偏移都由 `STACK_LAUNCHED` / `STACK_VCPU` 宏算出来（`vmx_entry.S:58-59`），
   判据是反汇编：退出着陆点必须看到 `mov 0x10(%rsp),%rax`（stage1 静态自检第 4
   条）。

`guest/guest.S` 里也有一条同级的坑：`OUT`/`IN` 的立即数端口字段只有 8 位，
`out %al, $0x3f8` 会被汇编器**静默截断**成端口 `0xf8`，必须走 `%dx`。

## 8. 与真实 KVM 的差异（简化清单）

| 方面 | mini-kvm | KVM 6.12.93 |
|---|---|---|
| 架构 | 只 x86 VMX/EPT | x86 SVM/VMX/ARM |
| VM/vCPU | 单 vCPU、单 memslot、memslot 只能设一次 | 多 vCPU、多 slot、热变更 |
| 内存 | 4 级 EPT、叶一律 4KB、按需映射 | TDP MMU + 影子页表、2M/1G 大页、PML 脏页跟踪 |
| 地址类型 | MMIO 不实现：落在 memslot 外的 GPA 直接 `-EFAULT`（`ept.c:243-248`）→ `KVM_EXIT_INTERNAL_ERROR` | `handle_ept_misconfig`/`kvm_io_bus` 派发 MMIO |
| IO | `CPU_BASED_UNCOND_IO_EXITING`（无 IO bitmap）；`IN` 一律返回 0；串 IO 拒绝 | IO bitmap + 指令模拟（`handle_io()` 走 emulator） |
| 中断 | 用户态显式请求注入单个 vector；无虚拟 LAPIC、无 Posted Interrupt、无 PIR/VIRR | in-kernel LAPIC + `kvm_pic`/`kvm_ioapic` + VT-d Posted Interrupts（SDM 30.6） |
| 异常 | 位图捕获 #DB/#UD/#GP/#PF 后报错退出 | 注入回 guest + emulator |
| MSR | 不开 "use MSR bitmaps"，运行循环也没有 MSR 退出分支 → guest 的 RDMSR/WRMSR 一律无条件退出后以 `KVM_EXIT_INTERNAL_ERROR` 收场（SDM Table 25-6 bit 28） | TSC offset/scaling、PAT、PERF 等一整套 |
| 实时性 | 无 halt-polling、无 `KVM_MP_STATE`、无 `KVM_KVMCLOCK_CTRL` 配合、无抢占定时器 | 全有 |
| 寄存器接口 | guest 寄存器只在内核侧 `vcpu->regs[]` 与 VMCS 里，未实现 `KVM_GET_REGS`/`KVM_SET_VCPU_EVENTS` | 完整 |
| 嵌套 | 不支持 | `nested/` |

## 9. 明确不做的事：安全

**这是教学模块，不要拿它跑不可信的 guest 代码。** 具体缺口：

- **投机执行侧只做了世界切换的寄存器清零。** `vmx_entry.S` 在恢复宿主寄存器
  前把所有 GPR 异或清零（连随后要从栈上取回的那些也一样），这是照
  `vmenter.S:231-240` 的理由做的："an L1 cache miss when restoring registers
  could lead to speculative execution with the guest's values"。除此之外**一条
  都没有**：vCPU 换 pCPU 时不发 IBPB（KVM 有
  `indirect_branch_prediction_barrier()`，`vmx.c:1486`）、进入 guest 前不做
  L1D 缓冲冲洗（KVM 的 `vmentry_l1d_flush_param`/`vmx_l1d_flush_pages`，
  `arch/x86/kvm/vmx/vmx.c:234`、`:249`，由 `vmx_setup_l1d_flush()` `:251` 建立）、不保存/恢复
  `IA32_SPEC_CTRL`（KVM 在 MSR 未被拦截时置 `VMX_RUN_SAVE_SPEC_CTRL`，
  `vmx.c:960-961`）、也没有 MDS/GDS 缓冲冲洗。等于把宿主与同机其他 VM 暴露给
  Spectre v2 / L1TF / MDS 类的跨世界推断。
- **宿主 GS 影子只保住宿主侧一半。** `SWAPGS` 在 VMX non-root 下原生执行
  （SDM 26.1.2 的无条件退出清单里没有它，也没有对应控制位），VM-Exit 的
  host-state 区又不恢复 `IA32_KERNEL_GS_BASE`（SDM 28.5.1 的宿主清单只有
  FS.base/GS.base）。运行循环在退出后立刻把宿主原值写回（对照
  `vmx_prepare_switch_to_guest()`/`vmx_prepare_switch_to_host()`，
  `vmx.c:1338-1346`、`1358-1390`），但 guest 侧的影子值不保存 —— 单
  guest、guest 不动 GS 的前提下够用，不是通用实现。
- **IO 端口没有隔离**：guest 可以 `OUT` 任意端口，模块除 0x3f8 外一律丢弃并
  `pr_info_ratelimited`。在教学 VM 里无所谓，在任何真实机器上都等于把宿主
  端口空间交给 guest。
- CPU 热插拔期间上线的 CPU 不会自动 VMXON（KVM 用
  `CPUHP_AP_KVM_ONLINE` 状态回调堵住这个洞，`kvm_main.c:5618-5626`）；
  mini-kvm 的对策是运行循环先检查、不在 VMX 模式就干净拒绝，而不是崩宿主。

## 10. Stage 文档

`stages/` 下的 5 篇讲原理与"为什么"，已经按本目录的多文件实现重写：代码块
摘自真实源码，引用逐条核对过（Linux 6.12.93 的 `file:line` + Intel VMX 规范
的章节号/图表号）。早期版本是设计期草稿，里面有几处硬伤（EPT violation 的
GPA 取错字段、IO qualification 布局、编造的 dmesg 字符串等），已回改并记录在
`phase8-capstone/corrections.md` J 节。若以后再发现文档与 `.c/.S` 不一致，
**以实现为准**，并同样在 corrections.md 留痕。

| Stage | 文档 | 主题 |
|---|---|---|
| 1 | `stages/stage1-vmx.md` | VMXON / VMCS / 首次 VM-Entry |
| 2 | `stages/stage2-ept.md` | EPT 页表、EPT violation、INVEPT |
| 3 | `stages/stage3-interrupt.md` | 中断注入、NMI |
| 4 | `stages/stage4-device.md` | IO 退出与串口 |
| 5 | `stages/stage5-runloop.md` | 运行循环与退出分发 |

## 11. 排错速查

| 现象 | 先看 | 常见原因 |
|---|---|---|
| `insmod` 报 `Device or resource busy` | `dmesg \| grep mini-kvm` | `kvm_intel` 没卸干净（CR4.VMXE 预检） |
| `insmod` 报 `No such device` | 同上 | BIOS 未开放 VMX（`MSR_IA32_FEAT_CTL`）或 EPT/INVEPT 能力不足 |
| `open /dev/mini-kvm` 失败 | `ls -l /dev/mini-kvm` | 模块没加载 |
| `test-mini-kvm` 报 `KVM_RUN` 返回 `-EPROTO` | `dmesg` 有"CPU%d 未 VMXON" | 模块加载后才上线的 CPU（第 9 节） |
| 退出原因是 `KVM_EXIT_INTERNAL_ERROR` | `dmesg` 里的 VMCS dump 与 `exit_reason` | guest 执行了 MSR/异常，或运行循环遇到未处理退出 |
| 串口缓冲为空但 guest 确实在跑 | `dmesg \| grep '忽略 OUT'` | guest 的 `OUT` 端口写错（见第 7 节末尾的截断坑） |
| 宿主直接宕机 | 硬重启 + 串口/netconsole | `vmx_entry.S` 的 host-state 或收栈被改动 |
