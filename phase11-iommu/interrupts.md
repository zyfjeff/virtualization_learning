# Q6：直通设备的中断怎么穿过 IOMMU？MSI 地址走不走普通翻译？

**短答**：中断和 DMA 走的是**两套独立的硬件表**——中断重映射（IR）查中断重映射表
（IRTE），DMA 重映射查页表，二者能力位、初始化路径、开关参数全部分开。而 MSI 地址
本身：**在 x86 上不走普通页表翻译**，IOMMU 为 `0xFEE00000` 的 LAPIC 窗口划出一段
"硬件 MSI 保留区"，这段地址既不会被分配器发出去、设备写它时也按恒等方式直达；
**在 ARM 上恰恰相反**，MSI 走的是一段由软件专门建映射的 IOVA 窗口。两种行为源自
同一个事实：x86 的 MSI 地址是固定基址的 LAPIC 写入，ARM ITS 的 MSI 地址是每个
ITS 实例自己的 doorbell 物理地址，没有统一基址可言。

**为什么值得问**：直通设备的中断是"设备写一个地址"这个动作，直觉上它应该和 DMA
一样过页表。如果真这么以为，就会想不通三件事：为什么 x86 上没人给 `0xFEE00000`
建过映射、中断却能送达；为什么 ARM 要专门有个 `iommu_dma_prepare_msi()`；以及为什么
VFIO / IOMMUFD 都要在 attach 时特判一把 "SW_MSI"。这一节把这三个"想不通"一次解开。

> 源码基线：`/root/code/linux-6.12.93`。IRTE / Posted Interrupt 的位级细节已在
> [第三阶段](../phase3-interrupts/README.md) 以 `intel-vtd.pdf` §9.9–9.11 为据核过，
> 本文不重复，只讲"中断怎么穿过 IOMMU 这一层"。

## 📖 目录

- [IR.1 IR 与 DMA 重映射是两套独立机制](#ir1-ir-与-dma-重映射是两套独立机制)
- [IR.2 x86：MSI 地址为什么需要一段"保留窗口"](#ir2-x86msi-地址为什么需要一段保留窗口)
- [IR.3 ARM 的反方向：软件管理的 MSI 翻译窗口](#ir3-arm-的反方向软件管理的-msi-翻译窗口)
- [IR.4 dma-iommu 怎么处理这两类保留区](#ir4-dma-iommu-怎么处理这两类保留区)
- [IR.5 msi cookie：UNMANAGED 域的另一个小分配器](#ir5-msi-cookieunmanaged-域的另一个小分配器)
- [IR.6 自检问题](#ir6-自检问题)

---

## IR.1 IR 与 DMA 重映射是两套独立机制

最容易犯的错是把"开了 IOMMU"当成"中断重映射也开了"。二者在硬件上是同一个
Remapping 单元里的两张表，但**能力位、初始化、开关全部分开**。

能力位各查各的：

```c
/* 来源: drivers/iommu/intel/iommu.h:222 */
#define ecap_ir_support(e)		(((e) >> 3) & 0x1)
```

IR 的初始化入口是 `intel_prepare_irq_remapping()`（`drivers/iommu/intel/irq_remapping.c:703`），
它在 **BSP APIC 初始化时**（`arch/x86/kernel/apic/apic.c:1903` 经
`drivers/iommu/irq_remapping.c:100-107` 派发）就被调用——**早于**
`intel_iommu_init()`，整条路径不读 `intel_iommu_enabled`：

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:719-725（节选） */
	if (dmar_table_init() < 0)
		return -ENODEV;
	...
	if (!dmar_ir_support())
		return -ENODEV;
```

随后逐单元检查 `ecap_ir_support(iommu->ecap)`（`:734-736`）并建 IR 表
（`intel_setup_irq_remapping()`，`:520`）。两条命令行也各自独立：

| 机制 | 开关参数 | 状态变量 |
|---|---|---|
| 中断重映射 | `intremap=` / `nointremap`（`drivers/iommu/irq_remapping.c:50-84`） | `irq_remapping_enabled` |
| DMA 重映射 | `intel_iommu=`（`drivers/iommu/intel/iommu.c:205,240`） | `intel_iommu_enabled` |

`dmar_in_use()`（`drivers/iommu/intel/dmar.c:2123-2126`）把两者并列 `||`，是"两套"
的直接代码证据。于是存在四种组合：只开 IR（最常见的"安全底线"，因为
x86 上开 IR 是防中断注入攻击的前提）、只开 DMA、都开、都不开。**排查直通中断
问题时先确认是哪一种**，`dmesg | grep -i 'interrupt remapping\|DMAR'` 能分别看到
两条使能日志。

对直通设备意味着什么：设备 MSI 写进来后，硬件先用地址里的 handle 查**中断重映射表**
（IRTE）得到投递目标——这一步归 IR 管；而 IRTE 本身放在内存里，硬件访问 IRTE 表
走的是自己的取表逻辑，与设备 DMA 用的页表无关。IRTE 格式、Remapped/Posted 两种
模式（`IM` 位）、PI Descriptor 见 [第三阶段](../phase3-interrupts/README.md)。

---

## IR.2 x86：MSI 地址为什么需要一段"保留窗口"

x86 的 MSI 是一次内存写：目标地址落在 Local APIC 寄存器空间，基址固定
`0xFEE00000`（`arch/x86/include/asm/apicdef.h:15` 的 `APIC_DEFAULT_PHYS_BASE`），
地址的其余位编码目标 LAPIC；开启 IR 时低位改为携带 IR handle（VT-d 兼容格式）。
也就是说，**每个设备的每个中断向量，写的都是同一个物理窗口里的某个地址**。

这个窗口必须满足两条互相咬合的要求：

1. **不能被 IOVA 分配器发给普通 DMA** —— 否则某个驱动 `dma_map` 拿到这段地址，
   设备 DMA 写进来就成了伪造中断（这正是中断重映射要防的攻击面的另一半）。
2. **设备写它时必须能到达** —— 直通设备的 MSI 写带着的总线地址就是
   `0xFEE000xx`，若页表里没有对应项、又被当成普通 IOVA 拦截，中断就丢了。

x86 两家 IOMMU 的解法一致：把窗口申报成 `IOMMU_RESV_MSI` 保留区，语义是
**"硬件 MSI 区：不翻译（untranslated）"**：

```c
/* 来源: drivers/iommu/intel/iommu.c:43-44 */
#define IOAPIC_RANGE_START	(0xfee00000)
#define IOAPIC_RANGE_END	(0xfeefffff)
```

```c
/* 来源: drivers/iommu/intel/iommu.c:4077-4080 */
	reg = iommu_alloc_resv_region(IOAPIC_RANGE_START,
				      IOAPIC_RANGE_END - IOAPIC_RANGE_START + 1,
				      0, IOMMU_RESV_MSI, GFP_KERNEL);
```

（宏名叫 `IOAPIC_RANGE_*` 纯属历史命名，实际圈的是 LAPIC/MSI 窗口。）
AMD 的写法逐字同构：

```c
/* 来源: drivers/iommu/amd/iommu.c:50-51 */
#define MSI_RANGE_START		(0xfee00000)
#define MSI_RANGE_END		(0xfeefffff)
```

使用点在 `drivers/iommu/amd/iommu.c:2943-2945`，同样用 `IOMMU_RESV_MSI`。

保留区类型枚举（[Q4 IO.5](iova.md) 已列过全表，这里只看关键两档）：

```c
/* 来源: include/linux/iommu.h:271-273 */
	IOMMU_RESV_MSI,		/* Hardware MSI region (untranslated) */
	IOMMU_RESV_SW_MSI,	/* Software-managed MSI translation window */
```

`IOMMU_RESV_MSI` 注释里 "untranslated" 一词是理解 x86 路径的钥匙：**这段窗口不靠
页表项打通，而是靠"不走翻译/恒等放行"打通**，和 identity 域是同族的硬件表达。

---

## IR.3 ARM 的反方向：软件管理的 MSI 翻译窗口

GICv3 ITS 没有统一基址：每个 ITS 实例有自己的 `GITS_TRANSLATER` doorbell **物理
地址**，MSI 写直接打到那个物理地址上，由 ITS 用 DeviceID+EventID 查表路由。于
是问题反过来了：**直通设备被关在 SMMU 后面，它发出的地址会被 stage-2 翻译**——
若不给 doorbell 建映射，MSI 写就是 fault。

SMMUv3 的解法是申报一段 `IOMMU_RESV_SW_MSI` 窗口：

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:3444-3445 */
	region = iommu_alloc_resv_region(MSI_IOVA_BASE, MSI_IOVA_LENGTH,
					 prot, IOMMU_RESV_SW_MSI, GFP_KERNEL);
```

```c
/* 来源: drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.h:480-481 */
#define MSI_IOVA_BASE			0x8000000
#define MSI_IOVA_LENGTH			0x100000
```

注意语义与 x86 **正好相反**：`SW_MSI` 是"由**软件**负责在里面建立翻译"的窗口，
窗口地址（`0x8000000` 起 1MB）是一段**人造的 IOVA**，与 doorbell 物理地址没有
恒等关系——映射要软件一条一条建（IR.4/IR.5）。

谁来建？中断子系统在分配 MSI 时给每个中断一个"设备看到的地址"，ARM 的 irqchip
把这个地址的生成委托给 IOMMU 层：

- `iommu_dma_prepare_msi()`（`drivers/iommu/dma-iommu.c:1810`）：为这条中断在
  SW_MSI 窗口里分配一个 IOVA，并把 `iova → doorbell 物理地址` 的映射建进页表；
- `iommu_dma_compose_msi_msg()`（`drivers/iommu/dma-iommu.c:1844`）：把 MSI
  message 的地址字段改写成那个 IOVA。

全树只有 `drivers/irqchip/` 下四个 ARM 系 irqchip 调用它们：
`irq-gic-v3-its.c:1738,3589`、`irq-gic-v2m.c:100,182`、`irq-gic-v3-mbi.c`、
`irq-ls-scfg-msi.c`。**x86 没有任何调用者**——因为 x86 根本不需要：窗口是
"不翻译"的，没有"建映射"这个动作。一句话对照：

| | x86（Intel/AMD） | ARM SMMUv3 |
|---|---|---|
| MSI 地址形态 | 固定基址 `0xFEE00000` + 编码 | 每 ITS 一个 doorbell 物理地址 |
| 保留区类型 | `IOMMU_RESV_MSI`（untranslated） | `IOMMU_RESV_SW_MSI` |
| 窗口与物理地址关系 | 恒等（设备写什么地址就到哪） | 无关，软件逐条建映射 |
| 谁动手建映射 | 无人（硬件直放） | `iommu_dma_prepare_msi()` 经 ARM irqchip |

---

## IR.4 dma-iommu 怎么处理这两类保留区

内核驱动的 DMA 域（[Q3](domains.md) 的 `IOMMU_DOMAIN_DMA`）初始化时，
`iommu_dma_init_domain()`（`drivers/iommu/dma-iommu.c:726`）会调
`iova_reserve_iommu_regions()`（`drivers/iommu/dma-iommu.c:563`）把组内所有设备
申报的保留区从 IOVA 分配器里挖掉：

```c
/* 来源: drivers/iommu/dma-iommu.c:583-592（节选） */
		/* We ARE the software that manages these! */
		if (region->type == IOMMU_RESV_SW_MSI)
			continue;

		lo = iova_pfn(iovad, region->start);
		hi = iova_pfn(iovad, region->start + region->length - 1);
		reserve_iova(iovad, lo, hi);

		if (region->type == IOMMU_RESV_MSI)
			ret = cookie_init_hw_msi_region(cookie, region->start,
					region->start + region->length);
```

三行里三个决定，逐条说：

1. `SW_MSI` 直接 `continue` —— **不挖**。因为那段窗口就是要留给软件（也就是
   即将分配中断的驱动/irqchip）去用的，挖掉反而坏事。注释 "We ARE the software
   that manages these!" 就是在回答"别人挖掉、你为什么不挖"。
2. 其余保留区一律 `reserve_iova()` 挖掉（[Q4 IO.2](iova.md) 讲过"永不发号"的
   语义）——`IOMMU_RESV_MSI` 窗口因此永远不会被 `dma_map` 撞上。
3. `IOMMU_RESV_MSI` 额外走 `cookie_init_hw_msi_region()`
   （`drivers/iommu/dma-iommu.c:483-506`）：给这段窗口建一张 `iova == phys` 的
   `msi_page` 记账表。**这不是建页表映射**（硬件反正不翻译它），记账是为了
   后面 `iommu_dma_get_msi_page()` 查询时能答出"这个 MSI 地址对应哪段物理窗口"。

`iommu_dma_get_msi_page()`（`drivers/iommu/dma-iommu.c:1765`）是两种情形的汇合
点：拿一个 `msi_addr` 来，先在 `msi_page_list`（hw 窗口的记账表）里查；查不到
（即 SW_MSI 场景）才现场 `iommu_dma_alloc_iova()`（`:1783`）+ `iommu_map()`
（`:1787`）建一条真映射。同一段代码，两种后端语义。

---

## IR.5 msi cookie：UNMANAGED 域的另一个小分配器

用户态直通（VFIO / IOMMUFD）的域是 `IOMMU_DOMAIN_UNMANAGED`，**不走 dma-iommu 的
默认域初始化**，上面那套 `iova_reserve_iommu_regions()` 不会替它跑。但直通设备
的中断还是要落地，于是有一个专门的小接口：

```c
/* 来源: drivers/iommu/dma-iommu.c:416-432 */
int iommu_get_msi_cookie(struct iommu_domain *domain, dma_addr_t base)
{
	struct iommu_dma_cookie *cookie;

	if (domain->type != IOMMU_DOMAIN_UNMANAGED)
		return -EINVAL;

	if (domain->iova_cookie)
		return -EEXIST;

	cookie = cookie_alloc(IOMMU_DMA_MSI_COOKIE);
	if (!cookie)
		return -ENOMEM;

	cookie->msi_iova = base;
	domain->iova_cookie = cookie;
	return 0;
}
```

三条性质：

1. **只接受 `IOMMU_DOMAIN_UNMANAGED`**——DMA 域的保留区已由 IR.4 处理，再来一次
   就是重复建设。
2. **一个域只能领一次**，重复返回 `-EEXIST`。注意 `drivers/iommu/iommufd/device.c:322-324`
   的注释写的是 "returns **-EBUSY** on later calls"——**注释与代码不符**，代码
   实际是 `-EEXIST`（`drivers/iommu/dma-iommu.c:424`）。写工具判错误码时以代码为准。
3. `base` 是 SW_MSI 窗口的起始地址：cookie 记下它之后，后续
   `iommu_dma_get_msi_page()` 就从这里线性发号（`drivers/iommu/dma-iommu.c:769-772`，
   `IOMMU_DMA_MSI_COOKIE` 分支不走树分配器）。

全树只有两个调用点，正好是两代用户态接口：

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c:2307-2311 */
	if (resv_msi) {
		ret = iommu_get_msi_cookie(domain->domain, resv_msi_base);
		if (ret && ret != -ENODEV)
			goto out_detach;
	}
```

- VFIO type1：`drivers/vfio/vfio_iommu_type1.c:2308`；
- IOMMUFD：`drivers/iommu/iommufd/device.c:317`。

而 VFIO 那个 `resv_msi` 从哪来？`vfio_iommu_has_sw_msi()`
（`drivers/vfio/vfio_iommu_type1.c:1888-1908`）：

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c:1895-1908（节选） */
		/*
		 * The presence of any 'real' MSI regions should take
		 * precedence over the software-managed one if the
		 * IOMMU driver happens to advertise both types.
		 */
		if (region->type == IOMMU_RESV_MSI) {
			ret = false;
			break;
		}

		if (region->type == IOMMU_RESV_SW_MSI) {
			*base = region->start;
			ret = true;
		}
```

**只有组里存在 `SW_MSI` 保留区（即 ARM 平台）时才领 cookie**；若存在"真"的
`IOMMU_RESV_MSI`（x86），直接否决——那里中断地址不需要任何页表动作。所以
"为什么 x86 上 VFIO 从不取 msi cookie"与"为什么 x86 没人给 `0xFEE00000` 建
映射"是同一个答案的两面。

把三种接口与 MSI 的关系合在一张表里，为 [Q7](userspace.md) 垫底：

| 接口 | 域类型 | MSI 处理 |
|---|---|---|
| 内核驱动（DMA API） | `DMA` / `DMA_FQ` | IR.4：挖掉 `RESV_MSI` + 记账；`SW_MSI` 留给 `prepare_msi` |
| VFIO type1 | `UNMANAGED` | 仅 `SW_MSI` 时取 cookie（`:2308`） |
| IOMMUFD | `UNMANAGED`（HWPT） | 同上（`drivers/iommu/iommufd/device.c:317`） |

---

## IR.6 自检问题

1. `intel_iommu=off` 但 `intremap=on` 可能吗？反过来呢？各自什么效果？
   （IR.1：可能。两套独立开关；只开 IR 时设备 DMA 不走翻译，但中断仍查
   中断重映射表）
2. 为什么 `0xFEE00000` 窗口既要"挖掉"又不会被设备的 MSI 写挡住？
   （IR.2/IR.4：挖掉是对分配器而言；`IOMMU_RESV_MSI` 语义是 untranslated，
   设备写它不走页表）
3. `iommu_dma_prepare_msi()` 在 x86 上会执行吗？
   （IR.3：不会。全树只有 ARM 系 irqchip 调用）
4. 一个 UNMANAGED 域第二次调 `iommu_get_msi_cookie()` 返回什么？代码与
   `iommufd/device.c` 的注释一致吗？
   （IR.5：`-EEXIST`；注释写的 `-EBUSY` 是错的）
5. VFIO 在 x86 平台取过 msi cookie 吗？判断依据在哪一行？
   （IR.5：没有；`vfio_iommu_has_sw_msi()` 遇 `IOMMU_RESV_MSI` 即返回
   `false`，`drivers/vfio/vfio_iommu_type1.c:1900-1903`）
