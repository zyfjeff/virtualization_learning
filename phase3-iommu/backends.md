# Q8：Intel / ARM / AMD 三家硬件，怎么对上同一套软件抽象？

**短答**：core 只定义动词——`iommu_ops`（驱动级方法表）+ `iommu_domain_ops`
（域级方法表）——三家各自填表。真正的差异集中在三处：**页表归谁建**（Intel 手写
`dma_pte` 走表器，ARM/AMD 委托给共享的 `io-pgtable` 库）；**怎么走进框架**（x86
平台钩子、platform 驱动、x86 检测+延迟初始化，三条不同的启动链）；**"恒等映射"在
硬件上是什么**（Intel 是 PASID 表项 `PGTT=PT`，ARM 是 bypass STE，AMD 是
`DTE[Mode]=0`）。还有一处观测差异值得记住：fault 日志在 Intel 上是解码过的，在
ARM 上是裸十六进制。

**为什么值得问**：同一个 `iommu_map()`，在 Intel 上落到 `try_cmpxchg64()` 改一个
64 位表项，在 ARM 上落到 `io_pgtable_ops->map_pages()`——不知道这层分工，读源码时
会在两个完全不同的数据结构之间迷路。同样，"开直通"（identity 域）在三家硬件上写
下去的是三种不同的表项，但 core 只看见一个 `IOMMU_DOMAIN_IDENTITY`。这一篇把
"一套抽象、三副实现"的接缝逐条对齐。

> 源码基线：`/root/code/linux-6.12.93`。失效队列的三家对照在
> [Q5 I.12–I.14](invalidation.md)，翻译格式的三家对照在 [Q1](translation.md)，
> 本文不重复，只讲"软件抽象怎么对上硬件"。

## 📖 目录

- [B.1 一套动词，三张方法表](#b1-一套动词三张方法表)
- [B.2 页表归谁：手写 `dma_pte` 还是 `io-pgtable` 库](#b2-页表归谁手写-dma_pte-还是-io-pgtable-库)
- [B.3 三条启动链：怎么走进框架](#b3-三条启动链怎么走进框架)
- [B.4 identity 的三种硬件表达](#b4-identity-的三种硬件表达)
- [B.5 probe 路径：唯一与后端无关的一段](#b5-probe-路径唯一与后端无关的一段)
- [B.6 fault 来了：解码输出与裸十六进制](#b6-fault-来了解码输出与裸十六进制)
- [B.7 墓碑：6.12 里已经不存在的符号](#b7-墓碑612-里已经不存在的符号)
- [B.8 自检问题](#b8-自检问题)

---

## 规范可用性声明

| 后端 | 一手规范 | 本仓库是否具备 | 本文的处理 |
|---|---|---|---|
| Intel VT-d | `intel-vtd.pdf`（Rev 4.1） | ✅ | 表项语义可引章节号 |
| ARM SMMUv3 | Arm IHI 0070（SMMUv3 架构规范） | ✅ `arm-smmuv3.pdf` | STE/命令语义以代码与注释为主，规范核对字段 |
| AMD-Vi | AMD I/O Virtualization Technology 规范 | ❌ 无 | 只陈述代码事实，不给规范章节号 |

---

## B.1 一套动词，三张方法表

三家驱动注册的都是同一个 `struct iommu_ops`，但填法差异本身就是知识点：

```c
/* 来源: drivers/iommu/intel/iommu.c:4644-4650（节选） */
const struct iommu_ops intel_iommu_ops = {
	.blocked_domain		= &blocking_domain,
	.release_domain		= &blocking_domain,
	.identity_domain	= &identity_domain,
	...
	.domain_alloc		= intel_iommu_domain_alloc,
```

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:3526-3530（节选） */
static struct iommu_ops arm_smmu_ops = {
	.identity_domain	= &arm_smmu_identity_domain,
	.blocked_domain		= &arm_smmu_blocked_domain,
	...
	.domain_alloc_paging    = arm_smmu_domain_alloc_paging,
```

```c
/* 来源: drivers/iommu/amd/iommu.c:3058-3061（节选） */
const struct iommu_ops amd_iommu_ops = {
	.capable = amd_iommu_capable,
	.blocked_domain = &blocked_domain,
	.domain_alloc = amd_iommu_domain_alloc,
```

三处差异，每处都有后果：

1. **分配钩子的新旧两代并存。** `.domain_alloc`（按类型分配）是老接口，
   `.domain_alloc_paging`（按设备分配、只用于带页表的域）是新接口。core 在
   `__iommu_domain_alloc()` 里按序分派：

   ```c
   /* 来源: drivers/iommu/iommu.c:1951-1954 */
   	else if (type & __IOMMU_DOMAIN_PAGING && ops->domain_alloc_paging)
   		domain = ops->domain_alloc_paging(dev);
   	else if (ops->domain_alloc)
   		domain = ops->domain_alloc(alloc_type);
   ```

   Intel/AMD 还填老接口（`:4650` / `:3061`），ARM 已迁到新接口（`:3530`，
   没有 `.domain_alloc`）。[Q7 U.5.2](userspace.md) 里
   `iommu_paging_domain_alloc()` 最终就是走到这个分派。
2. **`.identity_domain` 不是三家都有。** Intel（`:4647`）与 ARM（`:3527`）
   指向一个**静态单例域对象**——`__iommu_domain_alloc()` 遇到 IDENTITY 时直接
   返回它、不分配内存；**AMD 的表里没有这个字段**，identity 对它来说是一种
   "分配出来的、类型为 IDENTITY 的普通域"（B.4 看后果）。
3. **`.release_domain` 只有 Intel 填了**（`:4646`，复用 `blocking_domain`）——
   设备解绑时挂到哪个域，三家语义因此不完全一致。

---

## B.2 页表归谁：手写 `dma_pte` 还是 `io-pgtable` 库

这是三家之间最实质的结构差异：**页表本体是谁建、谁改的**。

### B.2.1 Intel：自持走表器

Intel 不用公共库，自己维护一张 `dma_pte` 多级表：

```c
/* 来源: drivers/iommu/intel/iommu.h:835-837 */
struct dma_pte {
	u64 val;
};
```

一个表项就是一个 64 位字，格式由 VT-d 规范 §9.8（二级页表项）定义。建映射的
主函数 `__domain_mapping()`（`drivers/iommu/intel/iommu.c:1795`）在目标叶子项上
做原子替换：

```c
/* 来源: drivers/iommu/intel/iommu.c:857 */
			if (!try_cmpxchg64(&pte->val, &tmp, pteval))
```

`pfn_to_dma_pte()`（`drivers/iommu/intel/iommu.c:819`）就是它的走表器：按
AGAW 逐级索引、必要时分配下一级页。**为什么手写？** 因为二级表项的原子替换、
超级页切换（[Q5 I.2](invalidation.md) 的 `switch_to_super_page`）、以及
context entry 里 `SSPTPTR` 的装配都是 VT-d 特有的耦合点，公共库表达不了。

### B.2.2 ARM：io-pgtable 库 + 按 stage 选格式

SMMUv3 的域在 `arm_smmu_domain_finalise()`
（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:2419`）里定形，格式选择按
stage 分岔：

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:2452-2453,2461-2462（节选） */
		fmt = ARM_64_LPAE_S1;
		finalise_stage_fn = arm_smmu_domain_finalise_s1;
	...
		fmt = ARM_64_LPAE_S2;
		finalise_stage_fn = arm_smmu_domain_finalise_s2;
```

随后一行委托给公共库：

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:2468 */
	pgtbl_ops = alloc_io_pgtable_ops(fmt, &pgtbl_cfg, smmu_domain);
```

### B.2.3 AMD：同一个库，但直通强制 V1

`protection_domain_alloc()`（`drivers/iommu/amd/iommu.c:2496`）按域类型选
页表格式：

```c
/* 来源: drivers/iommu/amd/iommu.c:2516-2530（节选） */
	switch (type) {
	/* No need to allocate io pgtable ops in passthrough mode */
	case IOMMU_DOMAIN_IDENTITY:
	case IOMMU_DOMAIN_SVA:
		return domain;
	case IOMMU_DOMAIN_DMA:
		pgtable = amd_iommu_pgtable;
		break;
	/*
	 * Force IOMMU v1 page table when allocating
	 * domain for pass-through devices.
	 */
	case IOMMU_DOMAIN_UNMANAGED:
		pgtable = AMD_IOMMU_V1;
		break;
```

注释把结论写死了：**用户态直通域（UNMANAGED）强制用 V1 页表**——这是读
AMD 直通行为时最先要记住的约束。随后同样走公共库
（`alloc_io_pgtable_ops()`，`:2546-2547`）。

### B.2.4 公共库长什么样

`drivers/iommu/io-pgtable.c` 里只有一张格式→初始化函数的表：

```c
/* 来源: drivers/iommu/io-pgtable.c:16,20-21,32-33（节选） */
io_pgtable_init_table[IO_PGTABLE_NUM_FMTS] = {
	...
	[ARM_64_LPAE_S1] = &io_pgtable_arm_64_lpae_s1_init_fns,
	[ARM_64_LPAE_S2] = &io_pgtable_arm_64_lpae_s2_init_fns,
	...
	[AMD_IOMMU_V1] = &io_pgtable_amd_iommu_v1_init_fns,
	[AMD_IOMMU_V2] = &io_pgtable_amd_iommu_v2_init_fns,
```

`alloc_io_pgtable_ops()`（`:57`）查表（`:70`）后调 `fns->alloc()`。于是
"页表谁建"的答案：**Intel 一家自持；ARM 和 AMD 共用这一层，差异被压缩成
`enum io_pgtable_fmt` 的一个取值**。

---

## B.3 三条启动链：怎么走进框架

三家进入 IOMMU 子系统的时机和路径完全不同，排查"为什么这台机器上
`intel_iommu`/`arm-smmu-v3`/`amd_iommu` 没生效"时，先看的是各自的启动链。

| 后端 | 入口 | 注册点 | 特点 |
|---|---|---|---|
| Intel | `intel_iommu_init()`（`drivers/iommu/intel/iommu.c:3227`） | `iommu_device_register(..., &intel_iommu_ops, NULL)`（`:3334`） | `subsys_initcall` 级的 x86 平台初始化，依赖 ACPI DMAR 表 |
| ARM | `arm_smmu_device_probe()`（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:4570`） | 同文件 `:4667` | 标准 platform 驱动，随设备树/ACPI 枚举逐个 probe |
| AMD | `amd_iommu_detect()`（`drivers/iommu/amd/init.c:3439`） | `iommu_device_register(...)`（`drivers/iommu/amd/init.c:2137`） | 两段式：先检测并把 `amd_iommu_init` 挂上 `x86_init.iommu.iommu_init`（`:3455`），后经 `state_next()`（`:3251`）状态机分阶段初始化 |

两个值得记住的推论：

- **Intel/AMD 是"x86 平台初始化的一部分"**，早于大多数驱动；**ARM 是普通
  设备驱动**，它的 probe 顺序受设备树影响。所以"SMMU 比某个设备晚到"在 ARM
  上是真实存在的场景（`is_attach_deferred` 这类钩子就是为它生的，AMD 也填了，
  `amd/iommu.c:3068`）。
- **注册的都是同一个 `iommu_device_register()`**——core 从这里开始接管，
  三家汇进同一条 `iommu_device` 链表。

---

## B.4 identity 的三种硬件表达

"恒等映射（DMA 地址 = 物理地址）"在 core 只是一个域类型
`IOMMU_DOMAIN_IDENTITY`（[Q3](domains.md)），落到硬件上是三种完全不同的表项。

### B.4.1 Intel：PASID 表项的翻译类型字段

```c
/* 来源: drivers/iommu/intel/iommu.c:4599-4616（节选） */
static int identity_domain_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	...
	if (sm_supported(iommu)) {
		ret = intel_pasid_setup_pass_through(iommu, dev, IOMMU_NO_PASID);
		...
	} else {
		ret = device_setup_pass_through(dev);
	}
```

scalable mode 下写的是 PASID 表项：

```c
/* 来源: drivers/iommu/intel/pasid.c:550 */
	pasid_set_translation_type(pte, PASID_ENTRY_PGTT_PT);
```

`PASID_ENTRY_PGTT_PT`（值为 4，`drivers/iommu/intel/pasid.h:52`）= Pass-Through
翻译类型——**硬件看到设备请求就不走页表**。非 scalable 的老路径走
`device_setup_pass_through()`（`intel/iommu.c:4588`），写的是 context entry。
静态域对象本体的注册见 `intel/iommu.c:4636-4642`。

### B.4.2 ARM：一条 bypass STE

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:1581-1587（节选） */
void arm_smmu_make_bypass_ste(struct arm_smmu_device *smmu,
			      struct arm_smmu_ste *target)
{
	memset(target, 0, sizeof(*target));
	target->data[0] = cpu_to_le64(
		STRTAB_STE_0_V |
		FIELD_PREP(STRTAB_STE_0_CFG, STRTAB_STE_0_CFG_BYPASS));
```

`STRTAB_STE_0_CFG_BYPASS` 的值是 4（`arm-smmu-v3.h:241`）——Stream Table Entry
的 `Config` 字段置成 bypass，该设备的请求不经 stage-1/2 翻译。挂法在
`arm_smmu_attach_dev_identity()`（`arm-smmu-v3.c:3066`，`:3072` 调上面的
构造函数），域对象是 `:3081` 的静态 `arm_smmu_identity_domain`。

### B.4.3 AMD：什么都不写就是恒等

AMD 的表达最"消极"：`protection_domain_alloc()` 对 IDENTITY 类型**早退、不建
任何页表**（B.2.3 的 `return domain`）。设备表项里体现为：

```c
/* 来源: drivers/iommu/amd/iommu.c:2060-2064（节选） */
	if (domain->iop.mode != PAGE_MODE_NONE)
		pte_root = iommu_virt_to_phys(domain->iop.root);

	pte_root |= (domain->iop.mode & DEV_ENTRY_MODE_MASK)
		    << DEV_ENTRY_MODE_SHIFT;
```

`PAGE_MODE_NONE` 就是 0（`drivers/iommu/amd/amd_iommu_types.h:320`）——
**DTE[Mode]=0、无页表根，即恒等**。判定函数也只有这一行语义
（`amd/iommu.c:221-223`：`pdom_is_in_pt_mode()` 就是查
`domain.type == IOMMU_DOMAIN_IDENTITY`）。

两个 AMD 特有的限制，都因 SNP（Secure Nested Paging）：

```c
/* 来源: drivers/iommu/amd/iommu.c:2596-2601 */
	/*
	 * Since DTE[Mode]=0 is prohibited on SNP-enabled system,
	 * default to use IOMMU_DOMAIN_DMA[_FQ].
	 */
	if (amd_iommu_snp_en && (type == IOMMU_DOMAIN_IDENTITY))
		return ERR_PTR(-EINVAL);
```

- **SNP 开启时禁止 IDENTITY 域**（上面，`:2600-2601`）——"什么都不写"的表达
  在 SNP 下被硬件规范禁止；
- `amd_iommu_def_domain_type()`（`:2987`）里，untrusted 设备强制 DMA 域
  （`:3006-3007`），能 PASID 的设备在**无内存加密且非 SNP** 时才给 IDENTITY。

### B.4.4 一个结构性差异

Intel/ARM 的 identity 是**静态单例**（`.identity_domain` 指向文件内定义的
`iommu_domain`，分配时直接返回该指针）；AMD 没有 `.identity_domain` 字段，
每个 IDENTITY 域都是 `protection_domain_alloc()` 现分配的空壳。**后果**：
`/sys/kernel/iommu_groups/N/type` 打印 `identity` 时，三家域对象的身份不同——
排"这个组到底挂在哪张表"时，AMD 上没有一个全局唯一的 `identity_domain`
符号可以去比对。

---

## B.5 probe 路径：唯一与后端无关的一段

三家的分歧到"设备进场"这一步**反而消失**了：

```c
/* 来源: drivers/iommu/iommu.c:600 → :513 → :416（调用链） */
iommu_probe_device(dev)
  → __iommu_probe_device(dev, ...)
      → ops->probe_device(dev)      /* 三家各自实现 */
      ...
      → iommu_setup_dma_ops(dev)    /* drivers/iommu/iommu.c:583 */
```

`ops->probe_device` 是三家唯一要交的功课（建 `dev_iommu` 档案、登记进组）；
其后的默认域选择（[Q3](domains.md)）、`iommu_setup_dma_ops()`
（`drivers/iommu/dma-iommu.c:1747`，[Q7 U.2](userspace.md)）全是 core 与
dma-iommu 的事，与后端无关。**所以"驱动看到的 DMA 行为"在三家之间是一致的，
不一致的只有页表格式、失效命令与故障上报**——后两者在 [Q5](invalidation.md)
与下面 B.6。

---

## B.6 fault 来了：解码输出与裸十六进制

同一个"设备访问了没映射的地址"，两家给出的现场信息差别很大。

**Intel**：fault 由 `dmar_fault()`（`drivers/iommu/intel/dmar.c:1947`）这个
IRQ 处理，读 Fault Recording Register，打印出源设备（BDF）、访问地址、
读写方向与解码后的原因字符串。

**ARM**：事件队列线程 `arm_smmu_evtq_thread()`
（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:1838`）逐条取事件，先交给
`arm_smmu_handle_evt()`（调用在 `:1852`，内部经
`iommu_report_device_fault()`，`:1832`，走通用 fault 上报）；处理不了或需要
留痕时打印的是**原始事件字**：

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:1856-1859（节选） */
			dev_info(smmu->dev, "event 0x%02x received:\n", id);
			for (i = 0; i < ARRAY_SIZE(evt); ++i)
				dev_info(smmu->dev, "\t0x%016llx\n",
					 (unsigned long long)evt[i]);
```

**实践含义**：在 ARM 平台排 DMA fault，`dmesg` 里拿到的是 8 个十六进制字，
要自己按 SMMUv3 事件记录格式（事件类型在 `evt[0]` 的 `ID` 字段）去译；
在 Intel 平台日志直接给结论。写跨平台排查手册时这一点必须分开写。

---

## B.7 墓碑：6.12 里已经不存在的符号

按 5.x / 6.0 时代的教材与博客读代码时，会在两个符号上卡住：

- **`si_domain`**（静态全局的静态恒等域）；
- **`domain_add_dev_info()`**（设备→域关联的旧入口）。

在 6.12.93 的 `drivers/iommu/` 里两者都**不存在**（整词 grep 零命中）。它们
的职责分别被 `.identity_domain` 静态域指针 + 默认域机制（[Q3](domains.md)）
和 `iommu_group` 的 `iommu_attach_device/group` 路径（[Q2](group.md)）取代。
**读到旧符号不要怀疑自己的源码树，也不要按旧流程推演。**

---

## B.8 自检问题

1. 同一个 `iommu_map()` 请求，在 Intel 与 ARM 上分别最终改写的是什么
   数据结构？（B.2：Intel 是 `dma_pte.val` 上的 `try_cmpxchg64`，
   `intel/iommu.c:857`；ARM 是 io-pgtable 库的 LPAE 表，经
   `alloc_io_pgtable_ops` 获得，`arm-smmu-v3.c:2468`）
2. AMD 为什么不给直通域用 V2 页表？依据在哪行注释？
   （B.2.3：UNMANAGED 强制 `AMD_IOMMU_V1`，
   `amd/iommu.c:2524-2529` 注释 "Force IOMMU v1 page table..."）
3. `iommu.passthrough=1` 生效时，Intel 的硬件动作和 ARM 的硬件动作
   分别是什么？（B.4：Intel 写 PASID 表项 `PGTT_PT`，`drivers/iommu/intel/pasid.c:550`；
   ARM 写 bypass STE，`arm-smmu-v3.c:1581`）
4. SNP 开启的 AMD 平台上能建 IDENTITY 域吗？报错点在哪？
   （B.4.3：不能，`protection_domain_alloc` 返回 `-EINVAL`，
   `amd/iommu.c:2600-2601`）
5. 三家里谁没有 `.identity_domain`？它的 identity 域从哪来？
   （B.1/B.4.4：AMD；`protection_domain_alloc(type=IDENTITY)` 现分配，
   不建页表，`amd/iommu.c:2518-2520`）
6. `si_domain` 在 6.12 里还能搜到吗？它的职责由什么接替？
   （B.7：搜不到；`.identity_domain` 静态指针 + 默认域机制）
