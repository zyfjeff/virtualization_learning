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

VMCS 是一个 4KB 的内存区域，保存了：
- **Host-State Area**: VM-Exit 后恢复的 Host 状态
- **Guest-State Area**: VM-Entry 加载的 Guest 状态
- **VM-Execution Control Fields**: 控制哪些事件触发 VM-Exit
- **VM-Exit Information Fields**: VM-Exit 的原因和上下文

VMCS 通过 VMWRITE/VMREAD 指令访问，不能直接内存访问。

## 🔧 实现（`main.c` + `vmx.c` + `vmx_entry.S`）

### 1. 把整机带进 VMX 操作模式（`main.c`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/main.c:139-173（on_each_cpu 回调，错误分支略） */
static void mini_vmx_enable_one(void *info)
{
	struct page *page = per_cpu(mini_vmxon_page, raw_smp_processor_id());
	u64 phys = page_to_phys(page);
	u64 cr4;
	u8 err;

	/* 已有 VMX 用户（典型是没卸载的 kvm_intel）—— 拒绝，避免互踩 */
	if (cr4_read_shadow() & X86_CR4_VMXE) { ... -EBUSY ... }

	asm volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= X86_CR4_VMXE;
	asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

	/* VMXON 的操作数是 VMXON 区域的**物理地址**（内存操作数），SDM 23.7 */
	asm volatile("vmxon %1; setna %0" : "=qm"(err) : "m"(phys) : "cc");
	if (err) { /* 回滚 CR4.VMXE */ }
}
```

三处容易漏：
- **`MSR_IA32_FEAT_CTL` 预检**（`main.c:70-76`）。BIOS 没锁定并开放 VMX 时
  VMXON 直接 `#GP`，对照 `__kvm_is_vmx_supported()`（`vmx.c:2782-2795`）。
- **VMXON 区域首页写 revision id**（`main.c:258`），值取
  `IA32_VMX_BASIC[30:0]`（SDM 23.6）。VMCS 页同样要写（本模块 `vmx.c:574`）。
- **`vmxon` 的成功/失败只看 CF/ZF**，用 `setna` 取标志；失败必须回滚
  `CR4.VMXE`。KVM 是另一套写法：`asm goto("1: vmxon ...")` 配
  `_ASM_EXTABLE(1b, %l[fault])`，把 `#GP` 导向 fault 分支再清
  `CR4.VMXE`（`kvm_cpu_vmxon()`，`vmx.c:2833-2851`）。

`cr4_set_bits()`/`cr4_clear_bits()` 没有导出给模块，所以这里裸读写 CR4；
调用点在 `on_each_cpu()` 的 IPI 上下文里，中断关闭，本机读-改-写安全。

### 2. 控制域协商（`vmx.c::mini_compute_controls()`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/vmx.c:247-295（错误处理略） */
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

- 每组的"想要的位"都要过一遍能力 MSR：`adjust` 按 `IA32_VMX_*CTLS` 的
  "必须为 1 / 允许为 1" 两段做交集（SDM Appendix A.3-A.5）。`BASIC[55]=1`
  时用 `TRUE_*` 那三个 MSR，否则用旧的（本模块 `vmx.c:249`）。次级控制只有一个
  MSR，没有 `TRUE_*` 变体（SDM Appendix A.3.3）。
- **协商结果必须回读校验**：`adjust` 只削位不报错，硬件不支持时想要的位会
  静默消失。EPT 位没保住就直接拒绝建 vCPU（本模块 `vmx.c:287-292`）—— mini-kvm 没有
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
/* 来源: phase8-capstone/practice/mini-kvm/vmx.c:460-521（部分字段与注释略） */
mini_vmwrite(GUEST_RIP, MINI_KVM_GUEST_RIP);	/* 0x1000 */
mini_vmwrite(GUEST_RSP, MINI_KVM_GUEST_RSP);	/* 0x100000 */
mini_vmwrite(GUEST_CR3, MINI_KVM_GUEST_CR3);	/* 0x6000，用户态建的页表 */
mini_vmwrite(GUEST_RFLAGS, X86_EFLAGS_FIXED);	/* 0x2，bit1 恒 1 */
mini_vmwrite(GUEST_CR0, MINI_GUEST_CR0);	/* PE|MP|ET|NE|WP|PG */
mini_vmwrite(GUEST_CR4, MINI_GUEST_CR4);	/* PAE（IA-32e 必需） */
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
- IA-32e 的进入检查（SDM 27.3.1.2）要求 CS 可用且 `L=1, D=0`、TR 可用且类型
  是 9 或 0xB；SS/DS/ES/FS/GS/LDTR **允许**不可用（AR 的 bit16 = 1），这是
  64 位平坦模型最省事的做法。
- **FS/GS 的 base 是独立字段，选择子为空也照样参与寻址**，VMCLEAR 之后 VMCS
  字段初值不可依赖，必须显式清零（`vmx.c:508-509`）。
- guest 自己 `lidt` 到 0x2000 装 IDT；`GUEST_IDTR_BASE` 初始写 0 就够了。
  GDTR 完全不用（没有远跳转）。

### 4. VM-Entry / VM-Exit 的世界切换（`vmx_entry.S`）

C 里写 `asm volatile("vmresume")` 是不够的：VM-Exit 之后 CPU 只保证按
host-state 区恢复 RSP/RIP/CR*/FS.base/GS.base，**通用寄存器全是 guest 的
值**，必须自己把它们换回来并把宿主的被调用者保存寄存器恢复。本模块照
`vmenter.S::__vmx_vcpu_run()` 手写：

```
进入：保存 rbp/rbx/r12-r15 + vcpu 指针 + launched 标志到栈
      VMWRITE HOST_RSP（字段编码必须是寄存器操作数，SDM 31.3）
      bt 取 launched → CF；按 regs[] 装载 guest 寄存器，**RAX 最后装**
      （装载 RAX 的那条 mov 用的正是 regs[] 的指针，装完指针就没了）
      jnc VMRESUME / VMLAUNCH
退出（HOST_RIP = mini_vmx_vmexit）：
      把宿主寄存器存进 regs[] → pop 出暂存的 guest RAX 那一格
      xor %ebx,%ebx（返回值 0）→ 丢弃 16 字节 → 清零全部 GPR →
      逆序 pop rbx/r12-r15/rbp → RET
entry 失败（CF=1）：mov $1,%ebx 后并入同一条收栈路径
```

`launched` 决定用 VMLAUNCH 还是 VMRESUME：VMCS 的 launch state 是 "clear"
时执行 VMRESUME 会得到 VM-instruction error 5（SDM 31.4 Table 31-1）。
VMCLEAR 会把 launch state 置回 "clear"（SDM 25.11.3），所以每次迁移都要连带
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

## 📝 检查清单

完成 Stage 1 后，确认能回答：
- [ ] VMX Root Mode 和 Non-Root Mode 的区别
- [ ] VMCS 的四大区域 (Host/Guest/Control/Exit Info)
- [ ] VMXON 区域的作用和格式
- [ ] VM-Entry 时 CPU 做了哪些事
- [ ] VM-Exit 时 CPU 做了哪些事
- [ ] 为什么需要 CR4.VMXE

## 🔗 下一步

Stage 2: 内存虚拟化 (EPT)
