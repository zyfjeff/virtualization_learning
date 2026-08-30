# Q2：为什么直通必须"整组一起"？组是怎么划出来的？

> **问题**：一个设备想单独直通，内核却说"它和隔壁那设备在一个组里，必须一起走"。
> 组（`iommu_group`）的边界由什么决定？边界定了之后，core 又如何把设备装进组、
> 把域装到组上？
>
> **为什么值得问**：主流程（[README 三](README.md#三主流程一次设备-dma-的完整旅程)）的
> 每一步都发生在某个 group 之内——选域按 group 选（[Q3](domains.md)），IOVA 分配器按
> group 发（[Q4](iova.md)），直通时 VFIO 也按 group 整体移交所有权。不理解组的构造，
> 就无法解释"为什么这台网卡不能单独直通"，也无法解释 [Q3](domains.md#d7-推论acs-决定了域的形状)
> 里那条"ACS 改组成员 → 域的形状跟着变"的推论。
>
> **分工**：**判定规则在 [phase5 §1.4/§1.5](../phase5-vfio/README.md#14-iommu-组是怎么划出来的)，
> 构造机制在本篇。**

---

## 📖 目录

- [G.1 为什么是"整组一起"](#g1-为什么是整组一起)
- [G.2 probe 路径：设备如何进入 group](#g2-probe-路径设备如何进入-group)
- [G.3 attach 路径：域如何装到设备上](#g3-attach-路径域如何装到设备上)
- [G.4 与 phase5 的接缝](#g4-与-phase5-的接缝)
- [G.5 自检问题](#g5-自检问题)

---

## G.1 为什么是"整组一起"

`iommu_group` 的定义（`drivers/iommu/iommu.c:47`）就一句话能说清：**不可分割的域绑定
单位——这一组设备必须共享同一个域**。域是"设备能看到的地址空间"（[README 二](README.md#二概念四个核心对象)），
所以同组设备互相之间**没有地址隔离可言**：组内任何一台设备发出的 DMA 地址，都会被同一张
页表（或同一个 pass-through 模式）解释。

为什么必须这样？因为组边界的判定依据是**硬件上能否隔离**：两台设备之间如果存在
不经过 IOMMU 就能互相发起请求的物理路径（典型是没有 ACS 的桥下游设备互发
peer-to-peer），那么把它们放进不同域就是自欺——域 A 的页表管不住从域 B 的设备
绕过来的请求。判定细节（四步判定、ACS 有效能力、三种真正会并组的情形）全部在
[phase5 §1.4](../phase5-vfio/README.md#14-iommu-组是怎么划出来的)实测核过，本篇不重复。

对直通的直接后果：VFIO 把设备交给用户态之前要独占整个组
（[phase5 §1.4.5](../phase5-vfio/README.md#145-这对-vfio-意味着什么)）——组里只要还有一台
设备被别的驱动占着，直通就会失败。这就是"整组一起"的用户可见形态。

本篇接下来讲的是边界定了之后的两件事：**设备怎么进组**（G.2）与**域怎么装到组上**（G.3）。

---

## G.2 probe 路径：设备如何进入 group

入口是总线通知，不是驱动调用：

```
设备注册到总线
  └─ iommu_bus_notifier()                    drivers/iommu/iommu.c:1662
       BUS_NOTIFY_ADD_DEVICE
     └─ iommu_probe_device()                 drivers/iommu/iommu.c:600   ← 公开入口，带全局锁
          └─ __iommu_probe_device()          drivers/iommu/iommu.c:513
               ├─ iommu_fwspec_ops()  取 ops  drivers/iommu/iommu.c:528   （没有 fwspec 就 -ENODEV）
               ├─ iommu_init_device()         drivers/iommu/iommu.c:544   分配 dev_iommu、关联 iommu 设备
               ├─ iommu_group_alloc_device()  drivers/iommu/iommu.c:549   取/建 group + 建 group_device
               │    ├─ bus->iommu_ops->device_group()   ← 分组判定在这里（phase5 §1.4）
               │    └─ iommu_group_alloc()    drivers/iommu/iommu.c:963   （新 group 时）
               ├─ list_add_tail(&gdev->list)  drivers/iommu/iommu.c:560   ★ 必须先入链表
               ├─ iommu_create_device_direct_mappings() drivers/iommu/iommu.c:563  FW 要求的恒等映射
               ├─ iommu_setup_default_domain(group, 0)  drivers/iommu/iommu.c:569  ★ 选域 + attach（Q3）
               └─ iommu_setup_dma_ops(dev)              drivers/iommu/iommu.c:583  ★ 决定 dev->dma_iommu
```

三处值得停下来的地方：

**(1) "有 group 就算已 probe"**：`__iommu_probe_device()` 一拿到 ops 就做幂等判断
（`drivers/iommu/iommu.c:540-542`，注释只有一句 "Device is probed already if in a group"）。它上面
`drivers/iommu/iommu.c:531-537` 那段 "Serialise to avoid races between IOMMU drivers registering in
parallel and/or the 'replay' calls from ACPI/OF code" 是解释**为什么用全局
`iommu_probe_device_lock` 而不是 `device_lock`** 的，不要跟幂等判断混为一谈。

这个幂等判断的实际后果是：**设备在 sysfs 里出现的 group 链接，不代表它的驱动已经
attach 完成**。这跟 [phase5 corrections](../phase5-vfio/corrections.md) 里"VFIO 接管在
sysfs 上看不见"是同一类观测盲区，成因不同但结论一致：sysfs 反映的是 core 的记账，
不反映硬件状态。

**(2) 顺序是硬的**：`list_add_tail()` 必须在 `iommu_setup_default_domain()` 之前，源码
里专门写了注释（`drivers/iommu/iommu.c:556-559`）。原因是选域要遍历 group 里的全部设备
（见 [Q3 D.1](domains.md#d1-决策链粒度是-group不是-device)）——一个还没进链表的设备不会
参与投票，就会被漏掉。

**(3) `iommu_setup_dma_ops()` 是有前提的**：`if (group->default_domain)`
（`drivers/iommu/iommu.c:582`）。默认域没建成，DMA ops 就不会设置，设备退回到
`dma-direct`。这是排查"某设备为什么没走 IOMMU 映射"时最容易忽略的一层。

---

## G.3 attach 路径：域如何装到设备上

`iommu_attach_device()`（`drivers/iommu/iommu.c:2106`）是给"手工管域"的用户（VFIO 老路径）
用的，它带一层 group 互斥检查；core 内部统一走 `__iommu_attach_device()`
（`drivers/iommu/iommu.c:2078`），最终落到 `domain->ops->attach_dev()`。

attach 的语义差别值得单独记：**map 是改页表，attach 是改"谁能看到这张页表"**。
`iommu_setup_default_domain()` 里有一段很直白的注释说明了顺序上的历史包袱：

```c
/* 来源: drivers/iommu/iommu.c:3007-3012 */
	/*
	 * Drivers are supposed to allow mappings to be installed in a domain
	 * before device attachment, but some don't. Hack around this defect by
	 * trying again after attaching. If this happens it means the device
	 * ...
```

也就是说"先建表再上线"才是正确顺序，部分驱动做不到，core 用重试兜。写文档时不要把它
描述成一个严格保证。"装路由"在硬件层面的落点（context entry / PASID table entry / STE
里写的是表根指针，不是映射）在 [Q1](translation.md)。

PASID 维度是**另一条独立的路**：`iommu_attach_device_pasid()`（`drivers/iommu/iommu.c:3390`）。
同一个 RID 可以同时挂着 `IOMMU_NO_PASID` 的一套地址空间和 N 个 PASID 各自的地址空间，
SVA 走的就是后者。`struct dev_pasid_info` 因此和 `group_device` 是两个链表。

---

## G.4 与 phase5 的接缝

一句话分工：**group 的判定在 phase5，group 的构造在这里。**

`bus->iommu_ops->device_group()` 对 PCI 设备就是 `pci_device_group()`，它回答"这台机器上
这个设备的隔离边界在哪里"，判据是 ACS 能力位与拓扑——那整段推理在
[phase5 §1.4](../phase5-vfio/README.md#14-iommu-组是怎么划出来的)
和 [§1.5.1–1.5.3](../phase5-vfio/README.md#15-acs-与-ats直通依赖的两个-pcie-能力)。
本阶段接手的是**边界定了之后**：这个 group 会拿到哪种域、域对象从哪来、失效由谁发起。

反过来说，phase5 里"把 GPU 和 DDI 并进同一个 group"这类 ACS 配置，其内核侧的全部效果
只有一个：让 [Q3](domains.md) 的选择矩阵在**这个 group 上**返回 `IOMMU_DOMAIN_IDENTITY`
（或让一个 DMA 域覆盖两个设备）。这就是 [Q3 D.7](domains.md#d7-推论acs-决定了域的形状)
的推论。

---

## G.5 自检问题

1. 为什么"同组设备共享一个域"不是软件偷懒，而是硬件约束的直接结果？
   （G.1：组边界判的就是"硬件上能否隔离"；无 ACS 的 peer 路径绕过页表）
2. `list_add_tail()` 与 `iommu_setup_default_domain()` 的顺序能换吗？换了会漏什么？
   （G.2 (2)：选域要遍历组成员投票，未入链表的设备不参与，`drivers/iommu/iommu.c:556-559` 注释）
3. 设备在 sysfs 里有了 group 链接，它的驱动就绑定完了吗？
   （G.2 (1)：幂等判断只看"在不在 group"；sysfs 反映 core 记账，不反映硬件/驱动状态）
4. 默认域没建成时，设备的 `dma_map_*()` 走哪条路？
   （G.2 (3)：`iommu_setup_dma_ops()` 有 `if (group->default_domain)` 前提，退回 `dma-direct`）
5. attach 和 map 改的是同一样东西吗？
   （G.3：map 改页表，attach 改"谁能看到这张页表"）
