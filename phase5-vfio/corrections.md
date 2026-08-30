# Phase 5 勘误

> 基线：Linux 6.12.93。以下问题在 `0000:4b:00.0`（独占 IOMMU group 35）上实测复现，并已回查源码确认。
> 宿主实测内核为 6.8.0-51-generic，涉及的代码路径与 6.12.93 一致。
>
> 勘误 1~3 在纯宿主侧复现；勘误 4 需要真起一个带该设备直通的 VM
> （`scripts/vm/setup-vfio-vm.sh`），因为 IRTE 的 Posted 化只有 KVM 能触发。
> 勘误 5 纠正的是本仓文档自身的引用错误（README 1.4），纯宿主侧可复现。

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

**修正后的正确认知**：要分清 ACS 的**能力位**（`ACSCap`）与**控制位**（`ACSCtl`）。能力位是硬件属性，不能靠内核参数「打开」；控制位是可写的，而且内核检测到 IOMMU 后默认就会全部置上（`pci_request_acs()` → `pci_std_enable_acs()`），上游另有 `pci=config_acs=` 与 `pci=disable_acs_redir=` 两个参数，但都只能进一步**关掉**隔离，不能凭空造出硬件没有的能力。详见 `README.md` 1.5.2。内核的判断逻辑是：

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
/* 来源: drivers/pci/pci.c:3649 */
	case PCI_EXP_TYPE_PCI_BRIDGE:
	...
		return false;
```

于是 `03:00.0` 与 `02:00.0` 同处 group 101。注意常量名是 `PCI_EXP_TYPE_PCI_BRIDGE`（0x7），
不是 `PCI_EXP_TYPE_PCIE_BRIDGE`（0x8）；且本机实际的并组路径是第 1 步 DMA alias 而非这里的
ACS 判定 —— 详见 [勘误 5](#勘误-5readme-14-里两处引用错误与一处过度简化)。

> 组划分的完整规则 —— `pci_device_group()` 的四步判定、`pci_acs_enabled()` 按 PCIe 类型的三类处理、
> 以及「一个 switch 下 32 个下游端口各自独占一组」的实测拓扑 —— 见
> [README.md 1.4 IOMMU 组是怎么划出来的](README.md#14-iommu-组是怎么划出来的)。

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

## 勘误 4：IRTE 的 Posted 化不由 KVM-VFIO 组列表触发

**原文**：`README.md` 3.1 节「关键交互点」

```
3. 当设备使用 Posted Interrupts 时，KVM 需要知道哪些 VFIO 组属于 VM
4. KVM 更新 IRTE 以支持 PI
```

**实际**：这两条把因果关系接错了。`KVM_DEV_VFIO_FILE_ADD` **不会**触发任何 IRTE 改写。

`kvm_vfio_file_add()` 做的事只有三件，没有一件与 IRTE 有关：

```c
/* 来源: virt/kvm/vfio.c:178 */
	kvm_arch_start_assignment(dev->kvm);
	kvm_vfio_file_set_kvm(kvf->file, dev->kvm);
	kvm_vfio_update_coherency(dev);
```

真正触发 Posted 化的是 **`irq_bypass` 的 token 配对**，走的是完全另一条路：

```
VFIO:  irq_bypass_register_producer()   token = ctx->trigger
KVM:   irq_bypass_register_consumer()   token = irqfd->eventfd
         ↓ 两个 token 是同一个 eventfd 上下文指针
       __connect()                       virt/lib/irqbypass.c:30
         └─ kvm_arch_irq_bypass_add_producer()   arch/x86/kvm/x86.c:13665
              └─ vmx_pi_update_irte()            vmx/posted_intr.c:272
                   └─ intel_ir_set_vcpu_affinity()  intel/irq_remapping.c:1248
                        └─ modify_irte()          ← IRTE 写成 Posted
```

配对与 VFIO 组列表毫无关系，只看 token 是否相等：

```c
/* 来源: virt/lib/irqbypass.c:108 */
		if (consumer->token == producer->token) {
			ret = __connect(producer, consumer);
```

**组列表确实有作用，但只是间接的**：`kvm_arch_start_assignment()` 递增
`assigned_device_count`，而它是 PI 的前提条件之一：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:135 */
static bool vmx_can_use_vtd_pi(struct kvm *kvm)
{
	return irqchip_in_kernel(kvm) && enable_apicv &&
		kvm_arch_has_assigned_device(kvm) &&
		irq_remapping_cap(IRQ_POSTING_CAP);
}
```

不过这个计数**在 irq_bypass 路径里自己也会被加上**，且就在检查之前：

```c
/* 来源: arch/x86/kvm/x86.c:13673 —— kvm_arch_irq_bypass_add_producer() */
	kvm_arch_start_assignment(irqfd->kvm);

	spin_lock_irq(&kvm->irqfds.lock);
	irqfd->producer = prod;

	ret = kvm_x86_call(pi_update_irte)(irqfd->kvm,
					   prod->irq, irqfd->gsi, 1);
```

所以即便退化到没有 `KVM_DEV_VFIO_FILE_ADD`，条件依然满足。**组列表不是 Posted 化的
必要环节**，它的正职是 DMA coherency（`kvm_vfio_update_coherency()`）与设备生命周期。

**实测印证**：kprobe 抓到的顺序是 `bypass_cons` → `kvm_add_prod` → `pi_update` →
`ir_vcpu_aff` → `modify_irte(Posted)`，全程 53 µs，其中 `kvm_add_prod` 到 IRTE 落盘只有 5 µs。
详见 `practice/README.md` 练习3。

### 附 1：两处不存在的观测接口

原文给出的两条命令都会失败，属于凭印象写的：

| 原文 | 问题 |
|---|---|
| `cat /sys/kernel/debug/kvm/<vm_id>/irq_routing`（陷阱4） | KVM debugfs 下没有 `irq_routing` 这个文件 |
| `echo vfio_irq_set >> .../set_event`（练习5） | 没有这个 tracepoint。`vfio_irq_set` 只是 `include/uapi/linux/vfio.h:584` 的结构体名 |

可用的替代品：

- `kvm_pi_irte_update` tracepoint —— 真实存在，定义在 `arch/x86/kvm/trace.h:1080`，
  在 `posted_intr.c:322` 处触发；
- `/sys/kernel/irq/<N>/chip_name` —— `IR-` 前缀表示走了中断重映射，
  但**区分不了 Remapped 与 Posted**；
- 对 `modify_irte` 下 kprobe 读原始 128 位 —— 唯一能直接看到 `IM` 位的办法
  （宿主未开 `CONFIG_INTEL_IOMMU_DEBUGFS` 时尤其如此）。

### 附 2：Linux 把规范里的 `IM` 位叫 `pst`

读代码时容易找不到 `IM`。Linux 的 `struct irte` 里，规范的 **IRTE Mode (IM) @ bit 15**
被命名为 `pst`（posted 的缩写）：

```c
/* 来源: include/linux/dmar.h:207 —— remapped/posted 共享部分 */
				__u64	present		: 1,  /*  0      */
					fpd		: 1,  /*  1      */
					__res0		: 6,  /*  2 -  6 */
					avail		: 4,  /*  8 - 11 */
					__res1		: 3,  /* 12 - 14 */
					pst		: 1,  /* 15      */
					vector		: 8,  /* 16 - 23 */
```

Posted 联合体里叫 `p_pst`（`dmar.h:240`）。位置、语义与规范一致（VT-d Spec 9.9 / 9.10），
只是名字不同 —— **不要因为搜不到 `IM` 就以为内核用了别的位**。

对照表：

| VT-d 规范 | Linux 字段 | 位 |
|---|---|---|
| IRTE Mode (IM) | `pst` / `p_pst` | 15 |
| Virtual Vector (VV) | `p_vector` | 23:16 |
| PDAL | `pda_l` | 63:38 |
| PDAH | `pda_h` | 127:96 |
| SID | `sid` / `p_sid` | 79:64 |

### 附 3：`CONFIG_X86_POSTED_MSI` 不是 KVM 的 PI

内核里有**两条**都叫「posted」的路径，容易混为一谈：

| | 宿主 posted MSI | Guest VT-d PI（本 phase 的主题） |
|---|---|---|
| 配置项 | `CONFIG_X86_POSTED_MSI`（6.11+） | `CONFIG_IRQ_REMAP` + KVM |
| 目的 | 宿主自己合并 MSI 中断，降低宿主 IRQ 开销 | 中断直投 vCPU，零 VM-Exit |
| 与 Guest 的关系 | **无关** | 核心 |
| 写 IRTE 的函数 | `prepare_irte_posted()`（`irq_remapping.c:1111`） | `intel_ir_set_vcpu_affinity()`（`:1248`） |
| 触发点 | alloc 阶段，`posted_msi_supported()` 门控（`:1377`） | irq_bypass 配对后 |
| PDA 指向 | 宿主 per-CPU PI Descriptor | **vCPU 的** PI Descriptor |

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:1377 */
		if (posted_msi_supported()) {
			prepare_irte_posted(irte);
			data->irq_2_iommu.posted_msi = 1;
		}
```

注意 `struct irq_2_iommu` 里是**两个不同的标志**（`irq_remapping.c:48-49`）：
`posted_msi`（宿主）与 `posted_vcpu`（Guest，在 `irq_remapping.c:1278` 置位）。
本次实测宿主未开 `CONFIG_X86_POSTED_MSI`，所以 alloc 阶段拿到的是纯 Remapped IRTE
（`pst=0`，向量为占位的 `MANAGED_IRQ_SHUTDOWN_VECTOR = 0xef`），
Posted 位完全由 KVM 后续写入 —— 因果链干净可辨。

---

## 勘误 5：README 1.4 里两处引用错误与一处过度简化

**原文**：`README.md` 1.4.2 表格、1.4.4 ①，以及本文件勘误 2 的附节反例。这三处都是本次
补写 1.4 节时引入的新错误，写完后核查 DMA alias 机制才发现。

### 错误 1：PCIe 类型常量写反了

原文说 `02:00.0` 命中 `PCI_EXP_TYPE_PCIE_BRIDGE`（`pci.c:3642`）。**这两个常量的名字与含义正好相反**：

```c
/* 来源: include/uapi/linux/pci_regs.h:482 */
#define   PCI_EXP_TYPE_PCI_BRIDGE  0x7	/* PCIe to PCI/PCI-X Bridge */
#define   PCI_EXP_TYPE_PCIE_BRIDGE 0x8	/* PCI/PCI-X to PCIe Bridge */
```

`lspci` 印的 "PCI-Express to PCI/PCI-X Bridge" 是 **0x7 = `PCI_EXP_TYPE_PCI_BRIDGE`**，
对应 `pci.c:3649`，不是 0x8 / `:3642`。实测读配置空间确认：

```bash
$ setpci -s 02:00.0 0x82.w     # PCIe Cap @0x80，+2 = PCI_EXP_FLAGS
0072                           # bits 7:4 = 0x7
```

两个分支都落到同一个 `return false`（`pci.c:3651`），所以**结论没变，错的是常量名与行号**。

### 错误 2：group 101 的成因说错了

原文说桥被第 2 步的 ACS 循环（`iommu.c:1550`）拉进组。实际走的是**第 1 步 DMA alias**。

关键在于 `pci_for_each_dma_alias()` 对纯 PCIe 的桥根本不回调：

```c
/* 来源: drivers/pci/search.c:84 */
		case PCI_EXP_TYPE_ROOT_PORT:
		case PCI_EXP_TYPE_UPSTREAM:
		case PCI_EXP_TYPE_DOWNSTREAM:
			continue;
		case PCI_EXP_TYPE_PCI_BRIDGE:
			ret = fn(tmp,
				 PCI_DEVID(tmp->subordinate->number,
					   PCI_DEVFN(0, 0)), data);
```

`03:00.0` 是传统 PCI 设备（`lspci -vvv` 里 Express capability 数量为 0），
其父桥 `02:00.0` 是 `PCI_EXP_TYPE_PCI_BRIDGE`、`subordinate` = bus 03，命中 `search.c:88`
这一支，真的回调了 `get_pci_alias_or_group()`：

```c
/* 来源: drivers/iommu/iommu.c:1470 */
	data->pdev = pdev;
	data->group = iommu_group_get(&pdev->dev);
	return data->group != NULL;
```

它只看 `pdev`（=桥）有没有组，不看 `alias` 值。`02:00.0` 先被 probe 且已独占 group 101，
所以返回非 0 → `pci_device_group()` 在 `iommu.c:1533` 就 `return data.group`，
**ACS 循环一行都没执行**。

> 桥的 `pci_acs_enabled()` 也确实是无条件 false，若 probe 顺序反过来，第 2 步同样会并组 ——
> 两条路结果一致，但把实际路径写成第 2 步是错的。

顺带解释了 `02:00.0` 自己为什么独占 group 101 而不是并入其上游 root port `00:1c.4` 所在的
group 99：`02:00.0` 自身的别名枚举只遇到 root port（`search.c:84` → `continue`，不回调），
第 2 步 `pci_acs_path_enabled(00:1c.4, ...)` 又返回 true，于是新建一组。

### 错误 3（过度简化）：Root Port / Downstream Port 并非「flag 必须真的置位」

原文 1.4.2 表格写「必须真的置了 flag，读配置空间校验」。实际校验前有一次按设备**声明**的
`ACSCap` 做的掩码：

```c
/* 来源: drivers/pci/pci.c:3597 —— pci_acs_flags_enabled() */
	pci_read_config_word(pdev, pos + PCI_ACS_CAP, &cap);
	acs_flags &= (cap | PCI_ACS_EC);
```

**没声明的能力被视为「硬连线已启用」直接跳过检查。** 所以 `ACSCap` 里的 `-` 不算失败，
只有「Cap 里声明了、Ctl 里没开」才算。实测 `00:1c.4`：

```
ACSCap: SrcValid+ TransBlk+ ReqRedir+ CmpltRedir+ UpstreamFwd- ...
ACSCtl: SrcValid+ TransBlk- ReqRedir+ CmpltRedir+ UpstreamFwd- ...
```

`UpstreamFwd` 在 Cap 里是 `-`，被 `:3598` 从 `REQ_ACS_FLAGS` 里掩掉；`TransBlk` 不在
`REQ_ACS_FLAGS` 内（`iommu.c:1383`），Ctl 里没开也无所谓。剩下 `SrcValid` / `ReqRedir` /
`CmpltRedir` 三个 Cap、Ctl 都置位 → 返回 true。这正是 `02:00.0` 没并进 group 99 的原因。

**修正**：README 1.4.2 表格该行改为「读配置空间校验，但要求集先被 ACSCap 掩掉」，并补出
`:3597-3598` 的掩码代码与 `00:1c.4` 的实测；1.4.4 ① 改为按 DMA alias 叙述并给出正确常量；
1.4.4 ③ 补齐 DMA alias 的四个来源。本文件勘误 2 附节的反例代码块同步改为 `pci.c:3649` /
`PCI_EXP_TYPE_PCI_BRIDGE`。

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

## 勘误 6：`has_assigned_device` 只限制 VT-d PI，不限制 KVM-side PI

**原文**：讨论 `vmx_can_use_vtd_pi()` 时容易得出"没有直通设备就不能用 Posted Interrupt"的结论。

**实际**：PI Descriptor 机制有**两种触发源**，`has_assigned_device` 只门控其中一种。

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:135 */
static bool vmx_can_use_vtd_pi(struct kvm *kvm)
{
    return irqchip_in_kernel(kvm) && enable_apicv &&
        kvm_arch_has_assigned_device(kvm) &&     // ← 只门控 VT-d PI
        irq_remapping_cap(IRQ_POSTING_CAP);
}
```

| | VT-d PI (IOMMU 路径) | KVM-side PI (APICv 路径) |
|---|---|---|
| **谁写 PI Descriptor** | IOMMU 硬件 | KVM 软件 |
| **入口** | `vmx_pi_update_irte()` | `vmx_deliver_interrupt()` |
| **需要 `has_assigned_device`** | ✅ | ❌ |

`vmx_deliver_interrupt()` 注册为 `kvm_x86_ops.deliver_interrupt`（`main.c:107`），
由 `lapic.c:1352` 对所有中断源调用。`vmx_deliver_posted_interrupt()` 只检查
`apicv_active`，不检查 `has_assigned_device`。

**影响范围**：即使纯模拟 VM，只要 APICv 开启，中断也能走 PI Descriptor 路径
（PIR→IRR 硬件同步），减少 VM-Exit。

**修正**：README §3.3 已补充「两种不同的 Posted Interrupt」小节澄清此问题。

---

## 参考

- 实验程序：`phase5-vfio/practice/`（说明见该目录 `README.md`）
- 相关章节：`phase3-interrupts/`（Posted Interrupts 与 VT-d IR 机制）
- 源码基线：`/root/code/linux-6.12.93/`
  - `virt/lib/irqbypass.c` —— producer/consumer 配对
  - `virt/kvm/eventfd.c` —— irqfd 与 consumer 注册
  - `drivers/vfio/pci/vfio_pci_intrs.c` —— MSI-X 装配与 producer 注册
  - `drivers/iommu/intel/irq_remapping.c` —— IRTE 读写
  - `include/linux/dmar.h:201` —— `struct irte` 位域定义
- 规范：intel-vtd.pdf（Section 9.9 Remapped IRTE / 9.10 Posted IRTE）；
  intel-vmx.pdf（Section 30.6 Posted-Interrupt Processing）；PCIe Base Spec（ACS）
