# Phase 5 勘误

> 基线：Linux 6.12.93。以下问题在 `0000:4b:00.0`（独占 IOMMU group 35）上实测复现，并已回查源码确认。
> 宿主实测内核为 6.8.0-51-generic，涉及的代码路径与 6.12.93 一致。

---

## 勘误 1：同组多设备的失败点不是 `vfio-pci` 绑定

**原文**：`README.md` 陷阱1「症状：`vfio-pci` 绑定失败」

**实际**：绑定 `vfio-pci` 一定成功，失败发生在后面的 `VFIO_GROUP_SET_CONTAINER`，返回 `-EPERM`。

**原因**：DMA ownership 的认领时机与绑定解耦。

普通驱动 probe 成功后会占用 default domain，使 `owner_cnt` 自增：

```c
/* 来源: drivers/pci/pci-driver.c:1670 */
if (!ret && !driver->driver_managed_dma) {
	ret = iommu_device_use_default_domain(dev);
	if (ret)
		arch_teardown_dma_ops(dev);
}
```

而 `vfio-pci` 显式声明自己管理 DMA，因此 probe 阶段**不动** `owner_cnt`：

```c
/* 来源: drivers/vfio/pci/vfio_pci.c:205 */
	.driver_managed_dma	= true,
```

真正的认领在 container 附加时才发生：

```c
/* 来源: drivers/vfio/container.c:437 */
		ret = iommu_group_claim_dma_owner(group->iommu_group, group);
```

此时如果同组还有设备绑在普通驱动上，`owner_cnt` 非零，直接拒绝：

```c
/* 来源: drivers/iommu/iommu.c:3214 —— iommu_group_claim_dma_owner() */
	if (group->owner_cnt) {
		ret = -EPERM;
		goto unlock_out;
	}
```

**注意认领时机随路径不同**。iommufd 路径推迟到 `VFIO_GROUP_GET_DEVICE_FD`：

> With the container FD the iommu_group_claim_dma_owner() is done during
> SET_CONTAINER but for IOMMFD this is done during VFIO_GROUP_GET_DEVICE_FD.
>
> —— `drivers/vfio/group.c:373`

**修正后的排查方式**：不要盯着 bind 是否成功，要看 `SET_CONTAINER` 的 errno。`phase5-vfio/practice/vfio-claim-trace.c` 在 `-EPERM` 时会直接给出提示。

---

## 勘误 2：`pcie_acs_override=` 不是上游内核参数

**原文**：`README.md` 陷阱2

```bash
echo "pci=noaer pcie_acs_override=downstream,multifunction" >> /etc/default/grub
```

**实际**：6.12.93 中不存在 `pcie_acs_override` 这个参数。

```
grep -rn "pcie_acs_override" drivers/pci/ Documentation/admin-guide/kernel-parameters.txt
→ 无匹配
```

它来自社区长期流传的第三方 ACS override 补丁（常见于 Proxmox、Arch 的 `linux-vfio` 等打过补丁的发行版内核），从未合入上游。在原生内核上写这个参数不会报错，只会被静默忽略，从而给出「加了参数但组没变」的困惑。

另外 `pci=noaer` 关闭的是 AER（Advanced Error Reporting），与 ACS 隔离毫无关系，同时出现在这条命令里属于误传。

**修正后的正确认知**：ACS 是硬件能力，不能靠内核参数「打开」。内核的判断逻辑是：

```c
/* 来源: drivers/iommu/iommu.c:1383 */
#define REQ_ACS_FLAGS   (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)
```

```c
/* 来源: drivers/iommu/iommu.c:1543 —— pci_device_group() */
for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
	if (!bus->self)
		continue;

	if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
		break;

	pdev = bus->self;          /* 隔离不成立 → 把桥拉进同一组 */
	group = iommu_group_get(&pdev->dev);
	if (group)
		return group;
}
```

若上游链路的隔离不成立，唯一的正规解法就是把整组一起直通。使用第三方 override 补丁强行拆组会真实地破坏 DMA 隔离，等于放弃了 IOMMU 的安全保证。

### 附：单功能设备「没有 ACS 能力」反而算隔离成立

这一点极易误判。`pci_acs_enabled()` 对单功能端点/上游端口直接返回 true：

> most single function endpoints are not required to support ACS because they
> have no opportunity for peer-to-peer access. We therefore return 'true'
> regardless of whether the device exposes an ACS capability.
>
> —— `drivers/pci/pci.c:3612-3618` 注释

```c
/* 来源: drivers/pci/pci.c:3667 */
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_UPSTREAM:
	case PCI_EXP_TYPE_LEG_END:
	case PCI_EXP_TYPE_RC_END:
		if (!pdev->multifunction)
			break;          /* → 最终 return true */

		return pci_acs_flags_enabled(pdev, acs_flags);
```

**实测印证**：`4b:00.0` 独占 group 35，其上游链路是

| BDF | PCIe 类型 | ACS 情况 | `pci_acs_enabled()` |
|---|---|---|---|
| `49:01.0` | Downstream Port | 有 ACS，四个 `REQ_ACS_FLAGS` 全置 | true（走 `pci.c:3659` 检查标志） |
| `48:00.0` | Upstream Port | **无 ACS capability** | true（单功能，走 `pci.c:3681`） |
| `47:00.0` | Root Port | 有 ACS，标志已启用 | true |

三跳全部成立 → `pci_acs_path_enabled()` 返回 true → 循环第一轮就 `break`，`pdev` 仍是设备本身 → 走到 `iommu_group_alloc()`，独占一组。

反例 `03:00.0`：其父桥 `02:00.0` 的能力是 `[80] Express (v2) PCI-Express to PCI/PCI-X Bridge`，命中硬编码分支：

```c
/* 来源: drivers/pci/pci.c:3642 */
	case PCI_EXP_TYPE_PCIE_BRIDGE:
	...
		return false;
```

于是桥被拉进组，`03:00.0` 与 `02:00.0` 同处 group 101。

---

## 勘误 3：`/sys/kernel/iommu_groups/N/type` 观测不到 VFIO 接管

**问题**：直觉上会用这个文件判断设备是否已被 VFIO 接管，但它从头到尾都不变。实测 `4b:00.0` 在绑定 `vfio-pci`、`SET_CONTAINER`、`SET_IOMMU` 之后，group 35 的 `type` 始终是 `identity`（宿主 cmdline 带 `iommu=pt`）。

**原因**：这个属性读的是 `default_domain`，不是当前实际附加的 domain。

```c
/* 来源: drivers/iommu/iommu.c:890 */
static ssize_t iommu_group_show_type(struct iommu_group *group,
				     char *buf)
```

`struct iommu_group` 里有三个 domain 字段，含义完全不同：

| 字段 | 含义 |
|---|---|
| `default_domain` | 组的默认 domain，**sysfs `type` 暴露的就是它** |
| `domain` | 当前实际附加的 domain，VFIO 接管后指向 VFIO 分配的 unmanaged domain |
| `blocking_domain` | 阻断 domain，所有 DMA 被拒 |

VFIO 接管只改 `group->domain`，不碰 `default_domain`，所以 sysfs 毫无变化，dmesg 也完全安静。

**正确的观测手段是 kprobe**。实测一次完整接管（`SET_CONTAINER` + `SET_IOMMU`）耗时约 160 µs，顺序为：

```
vfio_container_attach_group
  → iommu_group_claim_dma_owner      (+2µs)   认领 ownership
  → blocking_domain_attach_dev       (+1µs)   ← 先阻断
  → vfio_iommu_type1_attach_group    (+17µs)
  → intel_iommu_domain_alloc         (+88µs)  分配 VFIO 自己的 domain
  → intel_iommu_attach_device        (+3µs)   ← 再附加
关闭 fd 时：
  → blocking_domain_attach_dev       (+40µs)  ← 先阻断
  → iommu_group_release_dma_owner    (+9µs)
  → intel_iommu_attach_device                 ← 恢复 default_domain
```

两次 `blocking_domain_attach_dev` 构成安全联锁：ownership 转移期间 DMA 窗口始终是关闭的，不存在「旧 domain 已解除、新 domain 未就绪」的空档。

```c
/* 来源: drivers/iommu/iommu.c:3184 —— __iommu_take_dma_ownership() */
	ret = __iommu_group_alloc_blocking_domain(group);
	if (ret)
		return ret;

	ret = __iommu_group_set_domain(group, group->blocking_domain);
```

Intel IOMMU 侧的实现：

```c
/* 来源: drivers/iommu/intel/iommu.c:4645 */
	.blocked_domain		= &blocking_domain,
```

`blocking_domain_attach_dev()` 最终调用 `device_block_translation(dev)`，把上下文项置为不允许翻译。

---

## 附：attach 时那次 `iova=0x0, pgcount=2` 的映射不是业务映射

追 `intel_iommu_map_pages` 时会看到一次先于用户 `VFIO_IOMMU_MAP_DMA` 的调用（实测早 107 ms），`iova=0x0 pgsize=4096 pgcount=2`，且 domain 指针与后续业务映射相同、紧跟在 `intel_iommu_attach_device` 之后。

它来自能力探测，不是遗留映射：

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c:1823 */
static void vfio_test_domain_fgsp(struct vfio_domain *domain, struct list_head *regions)
```

在 `vfio_iommu_type1_attach_group()`（`:2300` 处调用）中映射 `PAGE_SIZE * 2`、再 unmap 掉其中一页，用来试探硬件是否支持细粒度 superpage，结果缓存进：

```c
/* 来源: drivers/vfio/vfio_iommu_type1.c:84 */
	bool			fgsp : 1;	/* Fine-grained super pages */
```

该标志用于门控 `:1076` 处的连续块合并逻辑（`if (!domain->fgsp)` 时才走逐段试探的慢路径）。

---

## 参考

- 实验程序：`phase5-vfio/practice/`（说明见该目录 `README.md`）
- 源码基线：`/root/code/linux-6.12.93/`
- 规范：intel-vtd.pdf；PCIe Base Spec 6.12（ACS）
