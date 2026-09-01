# Stage 2: EPT 内存虚拟化

> 对应课程 Phase 2: Memory Virtualization (EPT/TDP MMU)
>
> 关键源码: `arch/x86/kvm/mmu/tdp_mmu.c::kvm_tdp_mmu_map()`
>           `arch/x86/kvm/mmu/spte.c::make_spte()`

---

## 🎯 阶段目标

实现 EPT (Extended Page Tables)：
- 建立 4 级 EPT 页表结构
- 实现 GPA → HPA 地址映射
- 处理 EPT Violation (按需映射)

## 📖 核心概念

### EPT 页表结构

```
4KB 页映射 (4 级):
  PML4 [512] → PDPT [512] → PD [512] → PT [512] → 物理页

2MB 大页映射 (3 级):
  PML4 [512] → PDPT [512] → PD [512] → 2MB 物理页

1GB 大页映射 (2 级):
  PML4 [512] → PDPT [512] → 1GB 物理页
```

### EPT 权限位

**叶条目与非叶条目的布局不同**，这是最容易写错的地方（SDM Vol.3C
Table 29-7 = 4KB 叶、Table 29-5 = PDE 直接映射 2MB、Table 29-4 = 指向下一级的
PDPTE）：

```
所有条目共有:
  [0]     Read
  [1]     Write
  [2]     Execute（"mode-based execute control for EPT" = 0 时就是普通执行位；
          = 1 时表示 supervisor-mode 执行位，Table 29-7）
  [8]     accessed —— 仅当 EPTP bit 6 = 1 时有效，否则 Ignored
  [9]     dirty    —— 同上
  [10]    user-mode Execute —— 仅当 mode-based execute control = 1
  [63]    Suppress #VE —— 仅当 "EPT-violation #VE" = 1

只有叶条目才有:
  [5:3]   EPT memory type（6 = Write-Back）
  [6]     Ignore PAT
  地址在 [51:12]（4KB 叶；2MB/1GB 叶的保留区间更宽，见 Table 29-5 的 20:12）

非叶条目（Table 29-2 PML4E / 29-4 PDPTE / 29-6 PDE）:
  [7:3]   Reserved（must be 0）—— 中间层没有内存类型字段

bit 7 的唯一含义:
  PDPTE/PDE **直接映射大页**时必须为 1（Table 29-5: "Must be 1
  (otherwise, this entry references an EPT page table)"）；
  4KB 叶条目里 bit 7 是 Ignored，不是"大页标志"。
```

mini-kvm 的实现：叶一律 4KB，EPTP bit 6（A/D flags）不开，所以硬件忽略
accessed 位（`ept.c` 的 `mini_ept_init()`，对照 `construct_eptp()`
`arch/x86/kvm/vmx/vmx.c:3411-3423`）。

### EPT Violation

当 Guest 访问一个未映射的 GPA 时：
1. MMU 无法翻译 GPA → HPA
2. 触发 EPT Violation VM-Exit
3. VMM 在 VM-Exit 处理中建立 EPT 映射
4. 重新进入 Guest，重试原指令

## 🔧 实现（`ept.c`）

### 1. EPT 初始化（`mini_ept_init()`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/ept.c:136-152（注释略） */
pml4 = alloc_page(GFP_KERNEL | __GFP_ZERO);
kvm->ept.pml4 = pml4;
/* construct_eptp() 对照: arch/x86/kvm/vmx/vmx.c:3411-3423 */
kvm->ept.eptp = page_to_phys(pml4) | VMX_EPTP_MT_WB | VMX_EPTP_PWL_4;
```

EPTP（SDM 25.6.11，Table 25-9）：`[2:0]` 页表访问的内存类型 = WB(6)、
`[5:3]` 行走长度-1 = 3（4 级）、`[6]` A/D flags = 0、`[51:12]` PML4 页帧。

### 2. 4 级行走与中间层分配（`mini_ept_walk()`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/ept.c:187-222（注释略） */
static u64 *mini_ept_walk(struct mini_kvm_ept *ept, u64 gpa, bool alloc)
{
	static const int shifts[4] = {39, 30, 21, 12};
	u64 *table = (u64 *)page_address(ept->pml4);
	int level;

	for (level = 0; level < 4; level++) {
		int idx = (gpa >> shifts[level]) & 511;
		u64 *entry = &table[idx];

		if (level == 3)
			return entry;			/* 叶 */
		if (!(*entry & EPT_PTE_READ)) {		/* SDM 29.3.2: bits 2:0 全 0 = not present */
			struct page *np = alloc_page(GFP_ATOMIC | __GFP_ZERO);

			ept->tracked[ept->nr_tracked++] = np;
			/* 非叶条目：bits 7:3 保留必须为 0（Table 29-2/29-4/29-6） */
			*entry = page_to_phys(np) | EPT_ENTRY_RWX;
		}
		table = (u64 *)page_address(pfn_to_page(
				(*entry & EPT_PTE_ADDR_MASK) >> PAGE_SHIFT));
	}
	return NULL;
}
```

两个实现要点：
- 中间层只写 RWX，不写内存类型位（`EPT_ENTRY_RWX`，`ept.c:59`），叶条目才
  追加 `EPT_MEMTYPE_WB = 6ULL << 3`（`ept.c:61`）。
- 分配标志是 `GFP_ATOMIC`：本函数在 EPT-violation 路径上被调用，那时运行循环
  处于 `preempt_disable()` + 关中断。

### 3. EPT Violation 处理（`mini_ept_handle_violation()`）

```c
/* 来源: phase8-capstone/practice/mini-kvm/ept.c:228-255（错误分支略） */
int mini_ept_handle_violation(struct mini_kvm_vcpu *vcpu)
{
	struct mini_kvm_memslot *slot = &vcpu->kvm->slot;
	u64 gpa, *entry;
	unsigned long idx;

	mini_vmread(GUEST_PHYSICAL_ADDRESS, &gpa);
	gpa &= PAGE_MASK;
	/* GPA 不在 slot 内 → -EFAULT（ept.c:238-243） */

	idx = (gpa - slot->base_gpa) >> PAGE_SHIFT;
	entry = mini_ept_walk(&vcpu->kvm->ept, gpa, true);
	*entry = page_to_phys(slot->pages[idx]) | EPT_ENTRY_RWX | EPT_MEMTYPE_WB;
	return 0;		/* 运行循环重新进入，硬件重放那条访问 */
}
```

**GPA 从哪个字段读，是这一段唯一的考点，也是骨架版写错的地方：**

| | 错（骨架版） | 对 |
|---|---|---|
| GPA 来源 | `EXIT_QUALIFICATION` | `GUEST_PHYSICAL_ADDRESS`（编码 `0x2400`，`arch/x86/include/asm/vmx.h:261`） |

- SDM 25.9.1 对这个字段的说明就是一句话："This field is used by VM exits due
  to EPT violations and EPT misconfigurations."
- 而 `EXIT_QUALIFICATION`（`0x6400`）在 EPT violation 下的格式是 SDM Table
  28-7，**一个地址位都没有**：bit 0 = data read、bit 1 = data write、
  bit 2 = instruction fetch、bits 5:3 = 本次行走用到的各条目 R/W/X 位的
  logical-AND、bit 7 = guest-linear-address 字段是否有效、bit 8 = 该访问是否
  是某个线性地址的翻译、bits 9-11 = 用户态/可写/可执行提示（需处理器支持
  advanced VM-exit information）、bit 12 = IRET 解除 NMI 阻塞、
  bit 13 = shadow-stack 访问、bit 14 = 条目里的 supervisor-shadow-stack 位
  （需 EPTP bit 7）、bit 15 = guest-paging verification、bit 16 = PT/PEBS/
  用户中断投递一类的异步访问，bits 63:17 未定义。把它当 GPA 用，等于每次
  缺页都映射到一个由权限位拼出来的假地址上。
- HPA 也不是这里现分配的：`KVM_SET_USER_MEMORY_REGION` 时 `pin_user_pages()`
  已经把用户态那 2MB 钉住，violation 只是从 `slot->pages[]` 取（对照 KVM 的
  `hva_to_pfn()` + `make_spte()`）。

**建映射时不需要 INVEPT。** SDM 29.4.3.4："it is not necessary to execute
INVEPT following modification of an EPT paging-structure entry that had been
not present or misconfigured" —— 处理器不缓存 not-present 条目（§29.3.2）。
mini-kvm 唯一的失效点是运行循环每次重新 VMPTRLD 之后那一发 all-context
INVEPT（对照 `vmx_flush_tlb_all()` 在 enable_ept 时下发 INVEPT global，
`vmx.c:3204-3216`）。

**对应课程**:
- Phase 2, Section 4: `kvm_tdp_mmu_map()` / `make_spte()`
- 关键源码: `arch/x86/kvm/mmu/tdp_mmu.c`、`arch/x86/kvm/mmu/spte.c`

## 🔑 关键差异: mini-kvm vs 真实 KVM

| 特性 | mini-kvm | 真实 KVM |
|------|----------|---------|
| 并发安全 | 靠 vCPU mutex 串行化，改 EPT 时不可能有第二个行走者 | TDP MMU 有无锁行走者，拆根要 `call_rcu()` 等宽限期（`tdp_mmu.c:91`、`:422`） |
| 大页支持 | 仅 4KB 叶 | 2MB / 1GB 叶 + 升降级（`kvm_mmu_get_page` / `tdp_mmu`） |
| 按需映射 | 从 `pin_user_pages()` 钉住的 `slot->pages[]` 取 HPA | `hva_to_pfn()` + `make_spte()`，含 refcount/异常处理 |
| 脏页跟踪 | 无 | PML + 软件位 |
| MMIO | 不实现，memslot 外的 GPA → `-EFAULT` → `KVM_EXIT_INTERNAL_ERROR` | `kvm_io_bus_read/write()` 派发 |
| A/D flags | 不开（EPTP bit 6 = 0） | 按硬件能力与需要开启 |

## 🧪 实验验证

```bash
# 加载 + 跑通完整链路（安全流程见 ../README.md 第 4 节）
sudo insmod mini-kvm.ko
sudo ./test-mini-kvm

# EPT 能力与根的建立都在 dmesg 里
sudo dmesg | grep mini-kvm
# mini-kvm: VMX_BASIC=0x... revision=0x... true_ctls=1
# mini-kvm: EPT_VPID_CAP=0x... INVEPT(context=1 global)
# mini-kvm: CR0 FIXED0=... CR4 FIXED1=...          ← 决定 guest/host CR0/CR4 取值
# mini-kvm: VMXON 完成，N/N 个 CPU 进入 VMX 操作模式
# mini-kvm: memslot 注册 GPA=0x0 大小=512 页 (HVA=0x...)   ← pin_user_pages 全量成功
# mini-kvm: VMCS 初始化完成 (vcpu=0 rev=0x... pin=... cpu=... sec=... exit=... entry=...)
# mini-kvm guest: Hello from Mini-KVM Guest!       ← 第一次 HLT 前经串口打出来

# 想看每次 RUN 的退出统计（含 ept=? 计数），要开 dynamic debug：
echo -n 'module mini_kvm +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
# mini-kvm: RUN 结束 exits=... io=... ept=... hlt=... extint=... nmi=... inj=...
```

`test-mini-kvm` 注册用户态那 2MB 时内核就把页全钉住了，所以一次干净运行
必然会出现若干次 EPT violation（guest 代码/IDT/页表/栈各踩一页）——
统计行里的 `ept=` 就是这个数。

## 📝 检查清单

- [ ] 解释 EPT 4 级页表结构，并说出非叶条目为什么不能写内存类型位
- [ ] 描述 EPT Violation 的完整处理流程，指明 GPA 来自哪个 VMCS 字段
- [ ] 对比 EPT 与普通页表的区别
- [ ] 理解 EPTP 的格式和作用
- [ ] 解释为什么需要 A/D 位，以及不开它会影响什么
- [ ] 解释"建映射不用 INVEPT、换 pCPU 必须 INVEPT"的规范依据

## 🔗 下一步

Stage 3: 中断处理
