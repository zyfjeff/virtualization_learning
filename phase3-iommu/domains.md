# Q3：`iommu.passthrough` 到底改了什么？

> **问题**：`iommu.passthrough=0/1` 是调直通、调 GDR 时最常被动过的开关。它常被称为
> "开/关 IOMMU"——这个说法是**错的**，而且错的方向足以让人排查问题时完全找错地方。
> 它到底改了内核里的哪一处决策？
>
> **为什么值得问**：主流程（[README 三](README.md#三主流程一次设备-dma-的完整旅程)）的
> 第 ② 步那条三分支——`dev->dma_iommu` 真/假——就是由本问题里的决策链设定的。
> 看不懂这个决策，就无法解释"同一台机器、同一个设备，改一个 cmdline 参数后打印的
> DMA 地址完全变了"，也无法解释为什么 `iommu.passthrough=1` 会让某些 peer/P2P 路径
> 直接消失（见 [Q4](iova.md) 末尾）。它是**正确性开关**，不只是性能开关。

---

## 📖 目录

- [D.1 决策链：粒度是 group，不是 device](#d1-决策链粒度是-group不是-device)
- [D.2 五种域类型：位组合，不是枚举](#d2-五种域类型位组合不是枚举)
- [D.3 命令行、Kconfig 与架构差异](#d3-命令行kconfig-与架构差异)
- [D.4 identity 域在硬件上是什么](#d4-identity-域在硬件上是什么)
- [D.5 一个反直觉的事实：identity 域是全驱动单例](#d5-一个反直觉的事实identity-域是全驱动单例)
- [D.6 观测手段](#d6-观测手段)
- [D.7 推论：ACS 决定了域的形状](#d7-推论acs-决定了域的形状)
- [D.8 自检问题](#d8-自检问题)

---

## D.1 决策链：粒度是 group，不是 device

`iommu_setup_default_domain()`（`drivers/iommu/iommu.c:2950`）是给 group 选默认域的
唯一入口，三步：

```
iommu_setup_default_domain(group, target_type)          drivers/iommu/iommu.c:2950
  ├─ req_type = iommu_get_default_domain_type(group, target_type)   drivers/iommu/iommu.c:1726
  │     └─ for_each_group_device(group, gdev)
  │           iommu_get_def_domain_type(group, gdev->dev, driver_type)   drivers/iommu/iommu.c:1684
  │                ├─ ops->default_domain ?  → 该驱动全局静态域的 type
  │                ├─ ops->def_domain_type(dev) → 驱动对"这一个设备"的建议
  │                └─ 冲突时：IDENTITY 优先，并打印
  │                   "IOMMU driver error, requesting conflicting def_domain_type"
  ├─ dom = iommu_group_alloc_default_domain(group, req_type)        drivers/iommu/iommu.c:1605
  │     ├─ ops->default_domain 存在 → 直接返回它（legacy，别用）
  │     ├─ req_type != 0 → __iommu_group_alloc_default_domain()     drivers/iommu/iommu.c:1593
  │     ├─ 否则先试全局 iommu_def_domain_type
  │     └─ 失败则回落 IOMMU_DOMAIN_DMA 并 pr_warn
  └─ group->default_domain = dom; __iommu_group_set_domain_*(group, dom)
```

**关键性质：粒度是 group，不是 device。** 看 `iommu_get_def_domain_type()` 的注释就明白
这是设计而非巧合：

```c
/* 来源: drivers/iommu/iommu.c:1680-1686 */
/*
 * Combine the driver's chosen def_domain_type across all the devices in a
 * group. Drivers must give a consistent result.
 */
static int iommu_get_def_domain_type(struct iommu_group *group,
				     struct device *dev, int cur_type)
```

一个 group 里所有设备的域类型**必须一致**，不一致时内核打错误日志并优先取 IDENTITY。
这条性质把 [Q2](group.md) 里 group 与 phase6 的接缝缝死了：**ACS 配置通过改变 group 的
成员，间接改变了域的形状**（D.7 展开）。

`iommu_get_default_domain_type()`（`drivers/iommu/iommu.c:1726`）里还有两个硬覆盖，
容易被忽略：

- `CONFIG_ARM_DMA_USE_IOMMU` 打开时强制 `IOMMU_DOMAIN_IDENTITY`（`drivers/iommu/iommu.c:1741-1748`）；
- `untrusted` 设备（Thunderbolt/USB4 这类 external-facing）强制 `IOMMU_DOMAIN_DMA`，且如果驱动
  想把它覆盖成别的类型，**直接拒绝 probe**（`drivers/iommu/iommu.c:1774-1783`，
  `"Device is not trusted, but driver is overriding group %u to %s, refusing to probe."`）。

---

## D.2 五种域类型：位组合，不是枚举

类型定义在 `include/linux/iommu.h:196-206`，是**位组合**而不是枚举：

```c
/* 来源: include/linux/iommu.h:165-206（节选） */
#define __IOMMU_DOMAIN_PAGING	(1U << 0)  /* Support for iommu_map/unmap */
#define __IOMMU_DOMAIN_DMA_API	(1U << 1)  /* Domain for use in DMA-API implementation */
#define __IOMMU_DOMAIN_PT	(1U << 2)  /* Domain is identity mapped   */
#define __IOMMU_DOMAIN_DMA_FQ	(1U << 3)  /* DMA-API uses flush queue    */
...
#define IOMMU_DOMAIN_BLOCKED	(0U)
#define IOMMU_DOMAIN_IDENTITY	(__IOMMU_DOMAIN_PT)
#define IOMMU_DOMAIN_UNMANAGED	(__IOMMU_DOMAIN_PAGING)
#define IOMMU_DOMAIN_DMA	(__IOMMU_DOMAIN_PAGING | __IOMMU_DOMAIN_DMA_API)
#define IOMMU_DOMAIN_DMA_FQ	(__IOMMU_DOMAIN_PAGING | __IOMMU_DOMAIN_DMA_API | \
				 __IOMMU_DOMAIN_DMA_FQ)
```

| 类型 | 谁分配 | 谁管映射 | 有页表吗 | 典型场景 |
|---|---|---|---|---|
| `BLOCKED` (0) | `ops->blocked_domain` 静态单例 | 没人，一切 DMA 都 fault | 无 | 设备空闲期、`release_domain`、隔离可疑设备 |
| `IDENTITY` | `ops->identity_domain` 静态单例 | 不需要映射 | **无**（pass-through 模式） | `iommu.passthrough=1`；某些驱动强制 |
| `UNMANAGED` | `iommu_paging_domain_alloc()` | 调用方逐页 `iommu_map()` | 有 | **VFIO 直通**、`iommu_dma` 以外的所有手工映射 |
| `DMA` | core（带 `iommu_get_dma_cookie()`，`drivers/iommu/iommu.c:1983`） | dma-iommu 自动 | 有 | 宿主侧驱动的正常 DMA |
| `DMA_FQ` | 同上 + flush queue | dma-iommu 自动、**延迟失效** | 有 | lazy 模式（x86/S390 默认） |
| `SVA` / `NESTED` | 驱动/用户 | 进程页表 / 嵌套 | 复用别家的表 | PASID/SVA、nested 直通 |

注意最后一行：`iommu_group_show_type()` 的 `switch` **只覆盖了前五种**
（`drivers/iommu/iommu.c:890-917`），所以 SVA 域被当成 default domain 时 sysfs 会打印
`unknown`——读到 `unknown` 不要以为内核坏了。

`iommu_domain_free()` 的行为也解释了静态单例为什么能安全复用：它会
`if (domain->ops->free) domain->ops->free(domain);`，而 `identity_domain`/`blocked_domain`
的 ops **根本没有 `.free`**（`intel/iommu.c:4636-4642`，
`arm/arm-smmu-v3/arm-smmu-v3.c:3077-3083`），于是"释放"是空操作。

各类型分别由谁消费（`UNMANAGED` 与 VFIO/IOMMUFD 的分工、`DMA`/`DMA_FQ` 与 dma-iommu 的
关系）在 [Q7](userspace.md) 展开。

---

## D.3 命令行、Kconfig 与架构差异

全局默认值由 `iommu_subsys_init()`（`drivers/iommu/iommu.c:191`）在 `subsys_initcall`
阶段决定：

```c
/* 来源: drivers/iommu/iommu.c:191-219 */
static int __init iommu_subsys_init(void)
{
	if (!(iommu_cmd_line & IOMMU_CMD_LINE_DMA_API)) {
		if (IS_ENABLED(CONFIG_IOMMU_DEFAULT_PASSTHROUGH))
			iommu_set_default_passthrough(false);
		else
			iommu_set_default_translated(false);

		if (iommu_default_passthrough() && cc_platform_has(CC_ATTR_MEM_ENCRYPT)) {
			pr_info("Memory encryption detected - Disabling default IOMMU Passthrough\n");
			iommu_set_default_translated(false);
		}
	}

	if (!iommu_default_passthrough() && !iommu_dma_strict)
		iommu_def_domain_type = IOMMU_DOMAIN_DMA_FQ;

	pr_info("Default domain type: %s%s\n", ...);
```

三条容易踩的规则：

**(1) `iommu.passthrough` 是跨架构的，`iommu=` 只有 x86 有。**
`early_param("iommu.passthrough", iommu_set_def_domain_type)` 在通用代码里
（`drivers/iommu/iommu.c:697`），而 `iommu=nopt` / `iommu=pt` 的解析器在
`arch/x86/kernel/pci-dma.c:161-164`，注册点是同文件
`early_param("iommu", iommu_setup)`（`arch/x86/kernel/pci-dma.c:174`）
——**arm64 上传 `iommu=nopt` 会被静默忽略**。跨架构脚本一律写 `iommu.passthrough=`。

**(2) 未显式指定时，各架构的默认不同。**
`drivers/iommu/Kconfig:98-99`：`IOMMU_DEFAULT_DMA` 在 `X86 || S390` 上默认 LAZY，
其他架构（含 arm64）默认 **STRICT**。推论很实用：在一台 arm64 机器上如果 dmesg 打的是
`Default domain type: Passthrough`，那**一定是** cmdline 或发行版配置改过，因为 Kconfig
默认不是它——这个反推在定位环境差异时很值钱。

顺带把日志字符串钉死，`iommu_domain_type_str()`（`drivers/iommu/iommu.c:172-189`）的映射是
`BLOCKED→"Blocked"`、`IDENTITY→"Passthrough"`、`UNMANAGED→"Unmanaged"`、
`DMA` 与 `DMA_FQ` **都→`"Translated"`**。也就是说 **dmesg 这一行区分不了 strict 与 lazy**，
必须看紧随其后的 `DMA domain TLB invalidation policy: strict|lazy mode` 那一行
（只在非 passthrough 时打印，`drivers/iommu/iommu.c:215-219`）。

**(3) 内存加密会自动否决 passthrough。** 上面 `cc_platform_has(CC_ATTR_MEM_ENCRYPT)`
那段就是：TDX/SME 之类场景下内核会强制改回 translated 并打日志。看到
"Memory encryption detected" 就知道有人在跟你抢这个开关。

---

## D.4 identity 域在硬件上是什么

这是本章最该被记住的一句话：**identity 域不是一张"恒等映射的页表"，而是硬件的
pass-through 翻译模式——页表根本没建。**

规范侧（`intel-vtd.pdf` Section 3.9, Pass-through Translation）：

> "When DMA remapping hardware is programmed to provide pass-through translations, the
> hardware takes the input address and provides it back as the output address. Remapping
> hardware does not perform any access rights checking for pass-through translations.
> The input address to pass-through translation is subject to Host Address Width (HAW)
> address checking and any violations are treated as a translation fault."

三个直接后果，一个比一个不直观：

1. **没有权限检查** ⇒ identity 域下 `IOMMU_READ/IOMMU_WRITE`、只读保护、SWIOTLB 的
   缓解作用全部不存在。它等价于没有 IOMMU，只多了一个 HAW 宽度检查。
2. **有 HAW 检查** ⇒ 这是唯一还能在 identity 域下观察到的 fault 类型。如果你在
   passthrough 模式下看到 stage-1 / 翻译类 fault，说明**这个设备的流实际不在
   pass-through 上**，别在错的方向上继续查。
3. **不能表达"两个地址空间"** ⇒ peer/P2P 的本质是"设备 A 发出的地址被解释成设备 B 的
   BAR"，而 pass-through 下输入地址直接就是输出地址，没有可以重解释的环节。
   见 [Q4](iova.md) 的 peer/P2P 一节。

内核侧的落地，VT-d scalable mode 与非 scalable mode 各一条路，两条都**只写表格条目、
不写页表**：

```c
/* 来源: drivers/iommu/intel/iommu.c:4599-4620 */
static int identity_domain_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct device_domain_info *info = dev_iommu_priv_get(dev);
	struct intel_iommu *iommu = info->iommu;

	device_block_translation(dev);

	if (dev_is_real_dma_subdevice(dev))
		return 0;

	if (sm_supported(iommu)) {
		ret = intel_pasid_setup_pass_through(iommu, dev, IOMMU_NO_PASID);
		if (!ret)
			iommu_enable_pci_caps(info);
	} else {
		ret = device_setup_pass_through(dev);
	}
```

`intel_pasid_setup_pass_through()`（`intel/pasid.c:529`）里可以数清楚它到底
写了什么：`pasid_set_domain_id(pte, FLPT_DEFAULT_DID)`、`pasid_set_address_width()`、
`pasid_set_translation_type(pte, PASID_ENTRY_PGTT_PT)`、`pasid_set_present()`——
**没有任何一行设置页表指针**。SMMUv3 那边是同构的：
`arm_smmu_attach_dev_identity()` 造一个 bypass STE
（`arm/arm-smmu-v3/arm-smmu-v3.c:3066-3074`，
`arm_smmu_make_bypass_ste()` + `STRTAB_STE_1_S1DSS_BYPASS`），连 context descriptor 都不下。
三家后端在这一点上的完整对照在 [Q8](backends.md)。

于是标题里那个"到底改了什么"有了完整答案：

> `iommu.passthrough` 改的不是"IOMMU 开不开"，而是**这个 group 的默认域是 pass-through
> 翻译模式还是一张真实页表**。它同时决定了三件事：硬件是否做权限检查、`dev->dma_iommu`
> 是否为真、以及有没有 IOVA 分配器可用。

---

## D.5 一个反直觉的事实：identity 域是全驱动单例

`__iommu_domain_alloc()` 里这一支：

```c
/* 来源: drivers/iommu/iommu.c:1945-1950 */
	unsigned int alloc_type = type & IOMMU_DOMAIN_ALLOC_FLAGS;

	if (alloc_type == IOMMU_DOMAIN_IDENTITY && ops->identity_domain)
		return ops->identity_domain;
	else if (alloc_type == IOMMU_DOMAIN_BLOCKED && ops->blocked_domain)
		return ops->blocked_domain;
```

返回的是驱动里那个 `static struct iommu_domain`。也就是说
**`iommu.passthrough=1` 时，全机器所有 IDENTITY 默认域的 group，其
`group->default_domain` 指向同一个对象**。它的直接后果是可证的：identity 域没有 per-group
的页表，也没有 `iommu_get_dma_cookie()` 分配的 IOVA 分配器（对照
`drivers/iommu/iommu.c:1980-1988` 那个 `if (iommu_is_dma_domain(domain))` 分支，identity 域
`type & __IOMMU_DOMAIN_DMA_API` 为 0，拿不到 cookie）。

注意不要把这条推过头：往某个 group 的 `type` 写 `identity` **只改这一个 group 的
`default_domain` 指针**，不会让别的 group 的 `type` 跟着变——共享的是域对象，不是记账。
"单例在外部还能观测到什么"留作 `practice/` 实验 1 的待验证问题，不要当结论写。

对照记忆：`DMA`/`UNMANAGED` 域每 group 一个（`__iommu_group_domain_alloc()` →
`__iommu_domain_alloc()` → `ops->domain_alloc_paging()`），各自有独立的 IOVA 分配器
cookie。这就是"域的形状"和"group 的边界"真正耦合的地方。

---

## D.6 观测手段

| 想看什么 | 怎么看 | 注意 |
|---|---|---|
| 系统默认域 | `dmesg \| grep "Default domain type"` | 来自 `iommu_subsys_init()`，`(set via kernel command line)` 后缀表示被 cmdline 抢过 |
| lazy/strict | `dmesg \| grep "TLB invalidation policy"` | 只在非 passthrough 时打印 |
| 每 group 的域类型 | `cat /sys/kernel/iommu_groups/N/type` | 读的是 `group->default_domain->type`；**0644 可写**，走 `iommu_group_store_type()` |
| group 成员 | `ls /sys/kernel/iommu_groups/N/devices/` | 见 phase6 §1.4 的实测方法 |
| 保留区 | `cat /sys/kernel/iommu_groups/N/reserved_regions` | `iommu_group_show_resv_regions`（`drivers/iommu/iommu.c:922`）；与 [Q4](iova.md) 的窗口偏移对得上 |
| 是否真的走了 IOMMU | 设备 `dma_map` 后读 `iommu_groups/*/devices` + `type`，或直接看失效计数 | sysfs 不反映 VFIO 接管，见 [phase6 corrections](../phase6-vfio/corrections.md) |

**两套字符串不要混用**：dmesg 走 `iommu_domain_type_str()` 印 `Passthrough` / `Translated`，
sysfs 的 `type` 走 `iommu_group_show_type()`（`drivers/iommu/iommu.c:890-917`）印 `identity` / `DMA` /
`DMA-FQ` / `blocked` / `unmanaged`。同一状态两个名字，写 grep 脚本时不能通用。

`type` 可写这件事常被忽略。写路径是 `iommu_group_store_type()`（`drivers/iommu/iommu.c:3047`），
三条约束都在代码里：

- 只接受 `identity` / `DMA` / `DMA-FQ` / `auto` 四个串（`drivers/iommu/iommu.c:3058-3066`），其余 `-EINVAL`；
- **`DMA → DMA-FQ` 可以原地升级**，直接 `iommu_dma_init_fq()` 后把 `default_domain->type`
  改掉，不拆域也不解绑设备（`drivers/iommu/iommu.c:3071-3081`，注释 "We can bring up a flush queue
  without tearing down the domain"）；
- 其余情况要求 group 里**没有绑定的驱动**：`if (list_empty(&group->devices) ||
  group->owner_cnt) return -EPERM;`（`drivers/iommu/iommu.c:3084-3088`）。

所以 `practice/` 实验 1 用"解绑驱动后改写 type"来免重启切 identity/DMA，
[Q5](invalidation.md) 的实验用"`DMA → DMA-FQ` 可在线切换"来做 strict/lazy 对照——后者连
驱动都不用解绑。

---

## D.7 推论：ACS 决定了域的形状

把 [Q2](group.md) 的接缝、D.1 的决策链、D.4 的硬件语义串起来，就能把
[phase6](../phase6-vfio/README.md) 里那套看起来"只是安全配置"的 ACS 讲清楚：

```
ACS 能力位/拓扑  ──(phase6 §1.4/§1.5)──▶  group 成员集合
                                              │
                          iommu_get_default_domain_type() 遍历成员投票
                                              │
                                              ▼
                              group->default_domain（一种类型、一份页表）
                                              │
                              __iommu_domain_alloc → 页表 or pass-through
                                              │
                                              ▼
                    dev->dma_iommu 真/假 ──▶ 有没有 IOVA 分配器、能不能表达 peer
```

所以 NVIDIA 在 GB300 + CX8 上做的 ACS 调整，从内核视角看**只发生了一件事**：把 GPU 和
CX8 的 Data Direct Interface 归进同一个 group，于是它们共享同一个默认域，于是一台设备的
DMA 可以合法地命中另一台设备的 BAR。中间没有任何"内核为 P2P 开了后门"。

这条推论也是本阶段与第六阶段最好的收口：**phase6 讲的是"边界怎么画"，phase3 讲的是
"边界里面装什么"**。

---

## D.8 自检问题

1. `iommu.passthrough=1` 与 `iommu=nopt` 哪个在 arm64 上有效？为什么？
   （D.3：`iommu=` 的解析器只在 `arch/x86/kernel/pci-dma.c:174` 注册）
2. 为什么 `iommu_get_def_domain_type()` 要遍历 group 里**所有**设备？单个设备说了不算吗？
   （D.1：粒度是 group；不一致时 IDENTITY 优先并打错误日志）
3. identity 域下设备发的地址还有没有任何检查？能看到什么 fault？
   （D.4：只有 HAW 宽度检查，没有权限检查；规范 3.9）
4. `iommu.passthrough=1` 时，两个不同 group 的 `default_domain` 指针相等吗？
   `DMA` 域呢？（D.5：identity/blocked 是 `ops->identity_domain` 静态单例，相等；
   `DMA`/`UNMANAGED` 每 group 各分配一个，不相等）
5. 为什么 dmesg 的 `Translated` 区分不了 strict 与 lazy？该看哪一行？
   （D.3：`iommu_domain_type_str()` 把 `DMA` 与 `DMA_FQ` 都印成 `Translated`；
   看 `DMA domain TLB invalidation policy` 行）
