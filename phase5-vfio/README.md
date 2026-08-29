# 第五阶段：VFIO 设备直通

> 基于 Linux 6.12.93 内核源码 | 预计学习时间：2-3 周
>
> **前置依赖**:
> - 第三阶段（中断虚拟化 + VT-d IR）：直通设备的中断投递依赖 PI + IRTE 机制
> - 第四阶段（Virtio）：理解半虚拟化 I/O 与直通的区别和取舍

---

## 📋 学习目标

本阶段聚焦 **VFIO（Virtual Function I/O）** 框架和 KVM 设备直通机制。VFIO 是
Linux 中实现安全设备直通的标准框架，它允许虚拟机直接访问物理硬件设备，同时通过
IOMMU 保证隔离性。

完成本阶段后，你应该能够：
1. 理解 VFIO 的分层架构（设备驱动 → 核心框架 → IOMMU 驱动）
2. 掌握 DMA 映射的完整路径（设备 DMA → IOMMU 翻译 → 物理内存）
3. 理解 KVM-VFIO 桥接层如何将 VFIO 组与虚拟机关联
4. 使用工具调试设备直通问题

---

## 📂 本章文件

| 文件 | 内容 |
|------|------|
| `README.md` | 本文件：VFIO 架构全景 + IOMMU 组划分规则 + DMA 映射路径 + KVM-VFIO 桥接 + 常见陷阱 |
| `annotations.md` | 源码精读：VFIO 分层实现、container/group/device、Type1 IOMMU 后端 |
| `corrections.md` | ★ 勘误：DMA ownership 认领时机、ACS 认知误区、sysfs 观测盲区、IRTE Posted 化的真实触发路径（均有实测印证） |
| `practice/` | ★ 3 个实验程序：ownership 认领时机追踪 / IOVA→HPA 映射验证 / MSI-X 中断直通与 IRTE Posted 化 |

> 陷阱1、陷阱2、陷阱4 的表述已按 `corrections.md` 修正过，两者冲突时以 `corrections.md` 为准。

---

## 🏗️ VFIO 架构总览

### 1.1 为什么需要 VFIO？

```
┌──────────────────────────────────────────────────────────────────┐
│                  设备直通 vs 设备模拟                              │
│                                                                  │
│  设备模拟（如 virtio, emulated NIC）:                             │
│    Guest I/O → VM-Exit → QEMU 模拟 → 物理设备                    │
│    ✓ 安全性好   ✗ 性能差（每次 I/O 都触发 VM-Exit）              │
│                                                                  │
│  VFIO 设备直通:                                                  │
│    Guest I/O → 设备直接 DMA → 物理内存                            │
│    ✓ 性能接近裸机   ✓ 设备功能完整                                │
│    △ 需要 IOMMU 支持   △ 设备被独占（不能多 VM 共享）            │
│                                                                  │
│  安全性保证:                                                      │
│    IOMMU 确保设备只能访问 Guest 分配的内存区域                     │
│    设备无法 DMA 到其他 Guest 或宿主机的内存                        │
│    DMA 地址翻译由 IOMMU 硬件完成，对设备透明                      │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 VFIO 分层架构

```
┌──────────────────────────────────────────────────────────────────┐
│                    VFIO 分层架构                                  │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │                    用户态 (QEMU)                            │ │
│  │                                                             │ │
│  │   /dev/vfio/vfio ───── /dev/vfio/$GROUP_ID                 │ │
│  │   (容器控制)            (组设备访问)                          │ │
│  │                                                             │ │
│  │   ioctl:                                                    │ │
│  │     VFIO_GET_API_VERSION                                    │ │
│  │     VFIO_GROUP_SET_CONTAINER                                │ │
│  │     VFIO_GROUP_GET_DEVICE_FD                                │ │
│  │     VFIO_IOMMU_MAP_DMA                                     │ │
│  │     VFIO_DEVICE_RESET                                      │ │
│  └──────────────────┬──────────────────────────────────────────┘ │
│                     │ ioctl                                      │
│  ┌──────────────────▼──────────────────────────────────────────┐ │
│  │                VFIO 核心框架                                 │ │
│  │             (drivers/vfio/vfio_main.c)                      │ │
│  │                                                             │ │
│  │  ┌─────────────┐  ┌──────────────┐  ┌────────────────┐     │ │
│  │  │ VFIO 设备   │  │  VFIO 组     │  │  VFIO 容器     │     │ │
│  │  │ 管理        │  │  管理        │  │  管理          │     │ │
│  │  │             │  │              │  │                │     │ │
│  │  │ vfio_device │  │ vfio_group   │  │ vfio_container │     │ │
│  │  └──────┬──────┘  └──────┬───────┘  └───────┬────────┘     │ │
│  └─────────┼────────────────┼──────────────────┼───────────────┘ │
│            │                │                  │                 │
│  ┌─────────▼────────────────▼──────────────────▼───────────────┐ │
│  │              VFIO 设备驱动层                                  │ │
│  │           (drivers/vfio/pci/vfio_pci_core.c)                │ │
│  │                                                             │ │
│  │  提供设备特定的操作:                                          │ │
│  │    - open/release                                            │ │
│  │    - read/write (配置空间访问)                                │ │
│  │    - mmap (MMIO 区域映射)                                    │ │
│  │    - ioctl (设备特定操作)                                     │ │
│  │    - irq (中断管理: INTx, MSI, MSI-X)                       │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │              IOMMU 驱动层                                    │ │
│  │         (drivers/vfio/vfio_iommu_type1.c)                   │ │
│  │                                                             │ │
│  │  Type 1 IOMMU 驱动:                                         │ │
│  │    - DMA 映射/解映射                                         │ │
│  │    - IOMMU 域管理                                            │ │
│  │    - 页表管理                                                │ │
│  │    - 支持 Intel VT-d 和 AMD-Vi                               │ │
│  │                                                             │ │
│  │  ┌───────────────────────────────────────────────────────┐  │ │
│  │  │  IOMMU 核心 (drivers/iommu/)                          │  │ │
│  │  │  - iommu_domain 管理                                   │  │ │
│  │  │  - IOVA 分配                                           │  │ │
│  │  │  - 页表操作                                            │  │ │
│  │  │  - 硬件特定驱动 (intel/, amd/)                         │  │ │
│  │  └───────────────────────────────────────────────────────┘  │ │
│  └─────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 1.3 核心概念

| 概念 | 说明 |
|------|------|
| **VFIO 容器** | 最高层抽象，对应 `/dev/vfio/vfio`，管理 IOMMU 域 |
| **VFIO 组** | IOMMU 组的用户态接口，对应 `/dev/vfio/$GROUP_ID` |
| **VFIO 设备** | 具体设备的接口，通过组的 `DEVICE_FD` ioctl 获取 |
| **IOMMU 域** | 一组设备的 DMA 地址空间，由 IOMMU 硬件隔离 |
| **IOMMU 组** | IOMMU 拓扑中不可分割的设备集合 |
| **DMA 映射** | 将 Guest 物理地址映射到设备可见的 IOVA |
| **MSI-X** | 设备的中断机制，通过 MSIX 表配置 |

### 1.4 IOMMU 组是怎么划出来的

**组不是 VFIO 划的。** 划分发生在设备 probe 阶段的 IOMMU core 里，VFIO 只是消费既成结果 ——
启动时读 `/sys/kernel/iommu_groups/`，无法改变分组。

```c
/* 来源: drivers/iommu/iommu.c:427 —— __iommu_probe_device() */
	group = ops->device_group(dev);
```

Intel IOMMU 的回调按总线类型分流：

```c
/* 来源: drivers/iommu/intel/iommu.c:4085 */
static struct iommu_group *intel_iommu_device_group(struct device *dev)
{
	if (dev_is_pci(dev))
		return pci_device_group(dev);
	return generic_device_group(dev);
}
```

> **判据不是拓扑位置，而是硬件能否保证 peer-to-peer DMA 隔离。**
> 常见误解是「同一个 switch 下的设备属于一个组」—— 这只在 switch 下游端口缺 ACS 时才成立。
> ACS 齐备时，同一个 switch 下每个设备各自独占一个组（下文有实测）。

#### 1.4.1 `pci_device_group()` 的四步判定

`drivers/iommu/iommu.c:1515-1576`，按序尝试，任一步撞到既有组就复用，全落空才新建：

| 步 | 行号 | 做什么 |
|---|---|---|
| 1 | `:1532` | `pci_for_each_dma_alias()` 沿 DMA alias 向上找，途中遇到已有组就用它 |
| 2 | `:1543-1555` | **核心**：向上走 PCI 层级，直到 ACS 能保证隔离 |
| 3 | `:1561` | 查 DMA alias 关系的既有组（quirk 声明的 requester ID 伪装） |
| 4 | `:1570` | 查同 slot 上未隔离的其他 function |
| — | `:1575` | 都没有 → `iommu_group_alloc()` 独占一组 |

第 2 步是决定性的：

```c
/* 来源: drivers/iommu/iommu.c:1543 —— pci_device_group() */
for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
	if (!bus->self)
		continue;

	if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
		break;                 /* 隔离成立 → 就此收手 */

	pdev = bus->self;          /* 不成立 → 把这个桥拉进同一组 */

	group = iommu_group_get(&pdev->dev);
	if (group)
		return group;
}
```

注意 `pci_acs_path_enabled(bus->self, NULL, ...)` 检查的是**整条路径**
（`drivers/pci/pci.c:3693`，任一跳不满足即返回 false），所以第一轮迭代就把
「设备的父桥一直到 root bus」全查了。全通过 → 立即 `break`，`pdev` 仍是设备本身
→ 走到 `:1575` 独占一组。

要求的四个 flag：

```c
/* 来源: drivers/iommu/iommu.c:1383 */
#define REQ_ACS_FLAGS   (PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)
```

即 Source Validation、Request Redirect、Completer Redirect、Upstream Forwarding。
**不含** `TransBlk` / `EgressCtrl` / `DirectTrans` —— 所以 `lspci` 里看到 `TransBlk-`
不影响判定，不要误以为隔离不成立。

#### 1.4.2 关键坑：`pci_acs_enabled()` 查的是「有效能力」不是「实际能力」

`drivers/pci/pci.c:3620` 对不同 PCIe 类型有三类完全不同的处理：

| 设备类型 | 行号 | 行为 |
|---|---|---|
| Downstream Port / Root Port | `:3657-3659` | **必须真的置了 flag**，读配置空间校验 |
| 单功能 Endpoint / Upstream Port / Leg End / RC End | `:3667-3672` → `:3681` | **没有 ACS capability 也返回 true** |
| PCIe-to-PCI 桥、PCI 桥、RC-EC | `:3642-3651` | 无条件 `return false` |
| 非 PCIe（传统 PCI / PCI-X） | `:3633` | `return false` |

第二类的理由写在注释里：

> most single function endpoints are not required to support ACS because they have no
> opportunity for peer-to-peer access. We therefore return 'true' regardless of whether
> the device exposes an ACS capability.
>
> —— `drivers/pci/pci.c:3612-3618`

所以 `lspci` 里某一跳**完全没有** ACS capability，并不等于隔离不成立 —— 要先看它的 PCIe 类型。

#### 1.4.3 实测：一个 switch，30+ 个组

宿主上 `48:00.0`（Upstream Port）带 32 个 Downstream Port `49:00.0`–`49:1f.0`，
构成一个完整的 PCIe switch。实际划分结果：

| 设备 | 角色 | group |
|---|---|---|
| `48:00.0` | switch 上游端口 | 1 |
| `49:00.0` … `49:1f.0` | 32 个下游端口 | **2 … 33，各自一组** |
| `4a:00.0` | 端点（挂 `49:00.0`） | 34 |
| `4b:00.0` | 端点（挂 `49:01.0`） | 35 |
| `5b:00.0` | 端点（挂 `49:11.0`） | 36 |
| `69:00.0` | 端点（挂 `49:1f.0`） | 37 |

同一个 switch 下拆出了 30 多个独立组。以 `4b:00.0` 为例逐跳验证：

| BDF | PCIe 类型 | ACS 实际情况 | `pci_acs_enabled()` |
|---|---|---|---|
| `49:01.0` | Downstream Port | `ACSCtl: SrcValid+ ReqRedir+ CmpltRedir+ UpstreamFwd+` 四个全置 | true（走 `pci.c:3659`） |
| `48:00.0` | Upstream Port | **完全没有 ACS capability** | true（单功能，走 `pci.c:3681`） |
| `47:00.0` | Root Port | `ACSCtl` 四个全置 | true（走 `pci.c:3659`） |

三跳全通 → `pci_acs_path_enabled()` = true → 循环第一轮就 `break` → 独占 group 35。

复现命令：

```bash
# 列出所有多设备组，一眼看出哪些设备无法单独直通
cd /sys/kernel/iommu_groups
for g in $(ls -v); do
    n=$(ls $g/devices | wc -l)
    [ "$n" -gt 1 ] && echo "group $g ($n): $(ls $g/devices | tr '\n' ' ')"
done

# 沿上游链路逐跳看 PCIe 类型与 ACS
lspci -vvv -s 49:01.0 | grep -A2 "Access Control Services"
```

#### 1.4.4 三种真正会并组的情形

**① 上游有 PCIe-to-PCI 桥** —— 同机 group 101 = `02:00.0` + `03:00.0`：

```
02:00.0 PCI bridge: ASPEED AST1150 PCI-to-PCI Bridge
        Capabilities: [80] Express (v2) PCI-Express to PCI/PCI-X Bridge
```

命中 `PCI_EXP_TYPE_PCIE_BRIDGE` → `pci.c:3642` 无条件 false → 桥被 `iommu.c:1550` 拉进组。

**② 多功能设备各 function 之间无 ACS** —— 同机 group 96 = `00:16.0/.1/.4`
（Intel MEI Controller，ACS capability 数量为 0），由第 4 步归并：

```c
/* 来源: drivers/iommu/iommu.c:1397 —— get_pci_function_alias_group() */
	if (!pdev->multifunction || pci_acs_enabled(pdev, REQ_ACS_FLAGS))
		return NULL;

	for_each_pci_dev(tmp) {
		if (tmp == pdev || tmp->bus != pdev->bus ||
		    PCI_SLOT(tmp->devfn) != PCI_SLOT(pdev->devfn) ||
		    pci_acs_enabled(tmp, REQ_ACS_FLAGS))
			continue;
```

条件是**同 bus 同 slot**、且双方都没有 ACS。同机 group 105–114、168–177 的
「8 个 function 一组」都走这条路径。

**③ DMA alias** —— 设备用别的 requester ID 发起 DMA（桥后的传统设备、或 quirk 中声明的），
`pci_for_each_dma_alias()` 会把 alias 双方绑到一起（`iommu.c:1532` / `:1561`）。

#### 1.4.5 这对 VFIO 意味着什么

组是**所有权的最小单位**。`VFIO_GROUP_SET_CONTAINER` 会调 `iommu_group_claim_dma_owner()`，
组内只要还有设备绑在普通驱动上就直接拒绝：

```c
/* 来源: drivers/iommu/iommu.c:3214 —— iommu_group_claim_dma_owner() */
	if (group->owner_cnt) {
		ret = -EPERM;
		goto unlock_out;
	}
```

失败点是 `SET_CONTAINER` 而**不是** bind —— `vfio-pci` 声明了 `.driver_managed_dma = true`，
绑定阶段刻意不动 `owner_cnt`，所以绑定一定成功。详见
[corrections.md](corrections.md) 勘误 1（含 kprobe 实测的完整接管时序）。

ACS 是硬件能力，**没有内核参数能把它「打开」**；硬件不支持时唯一正规解法是整组一起直通。
详见 [陷阱2](#陷阱2acs未启用) 与 [corrections.md](corrections.md) 勘误 2。

---

## 🔍 DMA 映射详解

### 2.1 DMA 地址翻译

```
┌──────────────────────────────────────────────────────────────────┐
│                DMA 地址翻译流程                                   │
│                                                                  │
│  Guest 中:                                                       │
│    设备驱动发起 DMA:                                              │
│      DMA 地址 = GPA (Guest 物理地址)                             │
│                                                                  │
│  IOMMU 翻译:                                                     │
│    ┌──────────┐    IOVA     ┌──────────────┐    HPA             │
│    │  设备    │ ──────────▶ │   IOMMU      │ ─────────▶ ┌─────┐ │
│    │ (PCIe)  │  DMA请求    │  页表翻译     │  物理地址   │ 内存 │ │
│    └──────────┘             └──────────────┘             └─────┘ │
│                                                                  │
│  两层映射:                                                        │
│    Guest: GPA → IOVA (由 QEMU/KVM 在 IOMMU 中设置)              │
│    设备:  看到的是 IOVA (设备认为自己 DMA 到 GPA)                 │
│                                                                  │
│  或者直接使用 GPA 作为 IOVA:                                      │
│    Guest GPA = IOVA (当 Guest 不使用虚拟 IOMMU 时)               │
│    KVM 将 GPA 映射到 HPA 到 IOMMU 页表中                         │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 DMA 映射路径

```
用户态 (QEMU) DMA 映射流程:

  ioctl(VFIO_IOMMU_MAP_DMA)
      │
      ▼
  vfio_iommu_type1.c: vfio_dma_do_map()
      │
      ├── 验证参数（IOVA, 大小, 用户空间 VADDR）
      │
      ▼
  vfio_find_dma_valid() — 检查 IOVA 范围是否已映射
      │
      ├── 如果已映射 → 返回错误
      │
      ▼
  vfio_pin_pages_remote() — 固定用户空间页面
      │
      ├── get_user_pages_fast() 固定物理页
      ├── 防止页面被换出
      │
      ▼
  iommu_map() — 写入 IOMMU 页表
      │
      ├── IOVA → PFN 映射
      ├── 权限设置 (Read/Write)
      │
      ▼
  IOMMU 硬件生效
      │
      └── 设备 DMA 现在可以通过 IOMMU 访问这些物理页
```

---

## 🔗 KVM-VFIO 桥接

### 3.1 KVM 如何与 VFIO 交互

```
┌──────────────────────────────────────────────────────────────────┐
│                  KVM-VFIO 桥接架构                                │
│                                                                  │
│  ┌──────────────────┐              ┌──────────────────┐          │
│  │    QEMU          │              │    KVM           │          │
│  │                  │              │                  │          │
│  │  /dev/vfio/5     │              │  /dev/kvm        │          │
│  │  (设备文件)      │              │  (KVM 设备文件)  │          │
│  └────────┬─────────┘              └────────┬─────────┘          │
│           │                                  │                    │
│           │ KVM_DEV_VFIO_GROUP               │                    │
│           │ (将 VFIO 组关联到 KVM VM)        │                    │
│           └──────────────┬───────────────────┘                    │
│                          │                                        │
│                          ▼                                        │
│              ┌───────────────────────┐                            │
│              │  virt/kvm/vfio.c     │                            │
│              │  kvm_vfio 模块       │                            │
│              │                      │                            │
│              │  功能:               │                            │
│              │  - 管理 VFIO 组列表  │                            │
│              │  - DMA coherency     │                            │
│              │  - 中断路由关联      │                            │
│              │  - 设备生命周期管理  │                            │
│              └───────────────────────┘                            │
│                                                                  │
│  关键交互点:                                                      │
│    1. QEMU 将 VFIO 设备/组的 fd 传给 KVM (KVM_DEV_VFIO_FILE_ADD)  │
│    2. KVM 取引用，并递增 assigned_device_count                    │
│    3. 根据设备是否 DMA coherent，注册/注销 noncoherent DMA        │
│                                                                  │
│  注意: IRTE 改成 Posted 模式**不走这条路**。                      │
│  组列表只提供 PI 的前提条件之一(assigned_device_count > 0)，       │
│  真正的触发是 irq_bypass 的 token 配对，见 3.3 节。               │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 KVM VFIO 操作

```c
/* 来源: virt/kvm/vfio.c */

/*
 * KVM VFIO 桥接支持的操作:
 *
 * KVM_DEV_VFIO_GROUP_ADD:
 *   将 VFIO 组添加到 KVM VM
 *   - 获取 vfio_group 引用
 *   - 加入 kvm->vfio_groups 列表
 *   - 用于后续 PI 配置
 *
 * KVM_DEV_VFIO_GROUP_DEL:
 *   从 KVM VM 移除 VFIO 组
 *   - 释放 vfio_group 引用
 *   - 从列表中删除
 *
 * KVM_DEV_VFIO_GROUP_SET_SPAPR_TCE:
 *   (PowerPC 特有) 设置 TCE 表
 *
 * KVM_DEV_VFIO_FILE_ADD / DEL:
 *   与 GROUP_ADD/DEL 类似，但使用文件描述符
 */
```

### 3.3 MSI-X 中断直通：从 Remapped 到 Posted

这是 phase3（中断虚拟化）与 phase5 真正接壤的地方。要点是**职责分工**：

| 阶段 | 谁做 | IRTE 状态 |
|---|---|---|
| `VFIO_DEVICE_SET_IRQS` 启用 MSI-X | VFIO | Remapped（`IM=0`），中断打进宿主 |
| irqfd 注册 + token 配对 | irq_bypass | 不变 |
| `__connect()` 回调进 KVM | KVM | 改写为 Posted（`IM=1`），硬件直投 vCPU |

**VFIO 自己永远建不出 Posted IRTE** —— 它不知道 vCPU 的 PI Descriptor 在哪。
Posted 化只能由 KVM 发起，而 KVM 要发起就必须先和 VFIO 对上。

#### 接缝：eventfd 指针当 token

VFIO 在装配每个向量的最后一步注册 producer：

```c
/* 来源: drivers/vfio/pci/vfio_pci_intrs.c:515 */
	ctx->producer.token = trigger;
	ctx->producer.irq = irq;
	ret = irq_bypass_register_producer(&ctx->producer);
```

KVM 在建立 irqfd 时注册 consumer：

```c
/* 来源: virt/kvm/eventfd.c:444 */
		irqfd->consumer.token = (void *)irqfd->eventfd;
		irqfd->consumer.add_producer = kvm_arch_irq_bypass_add_producer;
		irqfd->consumer.del_producer = kvm_arch_irq_bypass_del_producer;
		irqfd->consumer.stop = kvm_arch_irq_bypass_stop;
		irqfd->consumer.start = kvm_arch_irq_bypass_start;
		ret = irq_bypass_register_consumer(&irqfd->consumer);
```

两个 `token` 都是**同一个 eventfd 上下文的指针** —— QEMU 把同一个 eventfd 分别交给
`VFIO_DEVICE_SET_IRQS` 和 `KVM_IRQFD`，内核两侧便自动认亲。注册**无先后要求**，
producer 和 consumer 两侧都会扫描对面的链表（`virt/lib/irqbypass.c:108` 与 `:204`）。

#### 配对成功后的调用链

```
irq_bypass_register_{producer,consumer}()
  └─ __connect()                                  virt/lib/irqbypass.c:30
       └─ cons->add_producer()
            = kvm_arch_irq_bypass_add_producer()   arch/x86/kvm/x86.c:13665
                 ├─ kvm_arch_start_assignment()    :13673
                 └─ pi_update_irte()
                      = vmx_pi_update_irte()       arch/x86/kvm/vmx/posted_intr.c:272
                           └─ irq_set_vcpu_affinity()
                                = intel_ir_set_vcpu_affinity()
                                                   drivers/iommu/intel/irq_remapping.c:1248
                                     └─ modify_irte()   ← IRTE 落盘为 Posted
```

KVM 提供的两个关键信息就是 Posted IRTE 里 VFIO 拿不到的部分：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:319 */
		vcpu_info.pi_desc_addr = __pa(vcpu_to_pi_desc(vcpu));
		vcpu_info.vector = irq.vector;
```

`pi_desc_addr` 变成 IRTE 的 `PDAL`/`PDAH`，`vector` 变成 `VV`（Virtual Vector）。

#### 不是所有中断都会被 Posted 化

`vmx_pi_update_irte()` 先查四个全局前提：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:135 */
static bool vmx_can_use_vtd_pi(struct kvm *kvm)
{
	return irqchip_in_kernel(kvm) && enable_apicv &&
		kvm_arch_has_assigned_device(kvm) &&
		irq_remapping_cap(IRQ_POSTING_CAP);
}
```

再逐条路由表项筛，只有单目标 vCPU 的普通中断能过：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:314 */
		kvm_set_msi_irq(kvm, e, &irq);
		if (!kvm_intr_is_single_vcpu(kvm, &irq, &vcpu) ||
		    !kvm_irq_is_postable(&irq))
			continue;
```

被筛掉的（多播、广播、多目标低优先级）会走 `posted_intr.c:338` 的
`irq_set_vcpu_affinity(host_irq, NULL)` **退回 Remapped 模式**。
所以一台机器上同一个设备的不同向量，完全可能一部分 Posted、一部分 Remapped。

#### 实测：一个向量的 IRTE 前后对比

在 `0000:4b:00.0` 上抓 `modify_irte` 的原始 128 位（完整过程见
[practice/README.md](practice/README.md) 练习3）：

| | `pst` (spec **IM**) | 向量 | 目标信息 |
|---|---|---|---|
| VFIO 建好时 | **0** = Remapped | `0x21` = 33 | `dest_id` = 宿主 APIC ID |
| KVM 改写后 | **1** = Posted | `VV` = 33 | `PDAL=0x2e5210b` `PDAH=0x29` → PI Desc `0x29b94842c0` |

`SID` 两次都是 `0x4b00`，即设备 BDF `4b:00.0`，可用来校验探针取参是否正确。
从 KVM 注册 consumer 到 Posted IRTE 落盘实测约 53 µs。

**Posted 化之后宿主彻底看不到这个中断了**：Guest 内压 80 MB 只读 I/O，
Guest 侧中断计数 +629，而宿主 `/proc/interrupts` 上对应的三个 IRQ 计数**全程为 0** ——
`vfio_msihandler()` 根本没被调用，也就没有 `eventfd_signal()`，更没有 VM-Exit。

> ⚠️ 别把它和 `CONFIG_X86_POSTED_MSI` 搞混。后者（6.11+）是**宿主自己**的
> posted MSI 优化，与 Guest 无关，走的是 `prepare_irte_posted()`
> （`irq_remapping.c:1111`）+ `posted_msi_supported()` 门控。
> 本节讲的是 KVM 的 Guest VT-d PI，走 `intel_ir_set_vcpu_affinity()`，两条路互不相干。

---

## 📖 源码阅读路线

### 推荐阅读顺序

```
┌─────────────────────────────────────────────────────────────┐
│                    源码阅读路线                                │
│                                                             │
│  Step 1: VFIO 核心框架                                       │
│  ├── drivers/vfio/vfio_main.c       ← VFIO 核心            │
│  │   ├── vfio_group 管理                                    │
│  │   ├── vfio_device 管理                                   │
│  │   └── ioctl 分发                                          │
│  └── include/linux/vfio.h           ← 接口定义              │
│                                                             │
│  Step 2: IOMMU Type 1 驱动                                   │
│  ├── drivers/vfio/vfio_iommu_type1.c ← DMA 映射核心        │
│  │   ├── vfio_dma_do_map()                                   │
│  │   ├── vfio_pin_pages_remote()                             │
│  │   └── iommu_map()                                         │
│  └── drivers/iommu/iommu.c          ← IOMMU 核心           │
│                                                             │
│  Step 3: VFIO PCI 驱动                                       │
│  ├── drivers/vfio/pci/vfio_pci_core.c ← PCI 设备直通       │
│  │   ├── 配置空间模拟                                         │
│  │   ├── MMIO 区域映射                                        │
│  │   └── 中断管理 (INTx/MSI/MSI-X)                          │
│  └── drivers/vfio/pci/vfio_pci_priv.h                       │
│                                                             │
│  Step 4: KVM-VFIO 桥接                                       │
│  └── virt/kvm/vfio.c                ← KVM VFIO 桥接        │
│      ├── kvm_vfio_group 管理                                 │
│      └── kvm_dev_vfio_ops                                    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 关键函数索引

| 函数名 | 文件 | 作用 |
|--------|------|------|
| `pci_device_group()` | `drivers/iommu/iommu.c:1515` | PCI 设备的组划分主逻辑，四步判定 |
| `pci_acs_path_enabled()` | `drivers/pci/pci.c:3693` | 检查整条上游路径的 ACS，任一跳不满足即 false |
| `pci_acs_enabled()` | `drivers/pci/pci.c:3620` | 单设备 ACS 判定，按 PCIe 类型分三类处理 |
| `get_pci_function_alias_group()` | `drivers/iommu/iommu.c:1391` | 归并同 slot 上未隔离的多功能 function |
| `iommu_group_claim_dma_owner()` | `drivers/iommu/iommu.c:3214` | 认领组的 DMA ownership，组内有普通驱动则 `-EPERM`（判断在 `:3222`） |
| `vfio_file_iommu_group()` | `vfio_main.c` | 获取文件的 IOMMU 组 |
| `vfio_dma_do_map()` | `vfio_iommu_type1.c` | DMA 映射操作 |
| `vfio_pin_pages_remote()` | `vfio_iommu_type1.c` | 固定用户页面 |
| `vfio_pci_core_enable()` | `vfio_pci_core.c` | 启用 PCI 设备直通 |
| `vfio_pci_mmap()` | `vfio_pci_core.c` | 映射设备 MMIO |
| `kvm_vfio_group_add()` | `virt/kvm/vfio.c` | 添加 VFIO 组到 KVM |
| `kvm_vfio_update_coherency()` | `virt/kvm/vfio.c` | 更新 DMA 一致性 |
| `vfio_pci_set_msi_trigger()` | `vfio_pci_intrs.c` | `SET_IRQS` 的 MSI/MSI-X 入口 |
| `vfio_msi_set_vector_signal()` | `vfio_pci_intrs.c:447` | 单向量装配：`request_irq` + 注册 producer |
| `vfio_msihandler()` | `vfio_pci_intrs.c:373` | 宿主中断处理，仅 Remapped 模式下执行 |
| `irq_bypass_register_producer()` | `virt/lib/irqbypass.c:84` | VFIO 侧注册，按 token 找 consumer |
| `irq_bypass_register_consumer()` | `virt/lib/irqbypass.c:179` | KVM 侧注册，按 token 找 producer |
| `kvm_arch_irq_bypass_add_producer()` | `arch/x86/kvm/x86.c:13665` | 配对成功回调，转发到 PI 更新 |
| `vmx_pi_update_irte()` | `vmx/posted_intr.c:272` | 判定可否 Posted，取 PI Desc 地址 |
| `intel_ir_set_vcpu_affinity()` | `intel/irq_remapping.c:1248` | 把 IRTE 实际改写为 Posted 格式 |

---

## 🔬 实践练习

> 下面是手工验证步骤。带实测输出的可运行实验（ownership 认领时机、IOVA→HPA 映射验证、
> MSI-X 中断直通与 IRTE Posted 化）见 [practice/README.md](practice/README.md)。

### 练习 1：基本设备直通

```bash
# 1. 检查 IOMMU 是否启用
dmesg | grep -i iommu
# 应看到类似: DMAR: IOMMU enabled

# 2. 检查 VFIO 模块是否加载
lsmod | grep vfio

# 3. 加载 VFIO 模块（如果未加载）
modprobe vfio
modprobe vfio-pci
modprobe vfio_iommu_type1

# 4. 查看设备的 IOMMU 组
readlink /sys/bus/pci/devices/0000:03:00.0/iommu_group
# 输出类似: ../../../../kernel/iommu_groups/5

# 5. 绑定设备到 VFIO 驱动
echo "8086 1533" > /sys/bus/pci/drivers/vfio-pci/new_id
# 或使用 vendor:device ID

# 6. 在 QEMU 中使用设备直通
# qemu-system-x86_64 ... \
#   -device vfio-pci,host=0000:03:00.0
```

### 练习 2：DMA 映射跟踪

```bash
# 跟踪 IOMMU DMA 映射操作
echo iommu_map > /sys/kernel/debug/tracing/set_event
echo iommu_unmap >> /sys/kernel/debug/tracing/set_event
echo vfio_iommu_type1 >> /sys/kernel/debug/tracing/set_event

echo 1 > /sys/kernel/debug/tracing/tracing_on

# 启动带设备直通的虚拟机
# 然后在 trace 中观察 DMA 映射活动

echo 0 > /sys/kernel/debug/tracing/tracing_on
cat /sys/kernel/debug/tracing/trace
```

### 练习 3：分析 IOMMU 域

```bash
# 查看 IOMMU 域信息
ls /sys/kernel/iommu_groups/

# 查看特定组的设备
ls /sys/kernel/iommu_groups/5/devices/

# 查看 IOMMU 设备信息
dmesg | grep -E "iommu|DMAR|AMD-Vi"

# 查看 DMAR 表
cat /sys/firmware/acpi/tables/DMAR 2>/dev/null | hexdump -C | head
```

> 拿到本机的分组结果后，对照 [1.4 IOMMU 组是怎么划出来的](#14-iommu-组是怎么划出来的)
> 逐条推导：哪些组是因为 PCIe-to-PCI 桥而并的，哪些是同 slot 多功能设备，
> 哪些设备独占一组、以及它上游每一跳的 ACS 状况。

### 练习 4：性能测试

```bash
# 使用 iperf3 测试直通网卡性能
# 在 Host 上:
iperf3 -s

# 在 Guest 上:
iperf3 -c <host_ip> -t 30

# 对比:
# 1. 纯模拟网卡 (virtio) 的吞吐量
# 2. VFIO 直通的吞吐量
# 3. 裸机（无虚拟化）的吞吐量

# 使用 perf 分析 IOMMU 开销
perf record -e intel_iommu:dtlb_walk -a -- sleep 10
```

### 练习 5：中断分析

```bash
# 1. 查看直通设备的宿主 IRQ（名字形如 vfio-msix[0](0000:4b:00.0)）
grep vfio /proc/interrupts
cat /sys/bus/pci/devices/0000:4b:00.0/msi_irqs

# 2. 判断有没有走中断重映射（IR- 前缀）
for n in $(ls /sys/bus/pci/devices/0000:4b:00.0/msi_irqs); do
  echo "irq $n -> $(cat /sys/kernel/irq/$n/chip_name)"
done

# 3. 观察 KVM 侧的 IRTE 更新（这个 tracepoint 真实存在，
#    定义在 arch/x86/kvm/trace.h:1080）
sudo bash -c 'cd /sys/kernel/tracing
echo 1 > events/kvm/kvm_pi_irte_update/enable
echo 1 > tracing_on'
# 启动带直通设备的 VM，然后看 trace
sudo cat /sys/kernel/tracing/trace | grep kvm_pi_irte_update
```

> 注意：内核里**没有** `vfio_irq_set` tracepoint（`vfio_irq_set` 只是
> `include/uapi/linux/vfio.h:584` 的一个结构体名），本仓早期版本写错过。
> 要观测 VFIO 侧的装配过程只能下 kprobe，做法见
> [practice/README.md](practice/README.md) 练习3。

**判读要点**：Posted 模式生效后，第 1 步里那些 IRQ 的计数应当**始终为 0**。
计数在涨说明中断仍走 Remapped 路径，排查见 [陷阱4](#陷阱4中断停在-remapped-模式性能达不到预期)。

---

## 🔧 常见问题排查

### 设备直通失败

```bash
# 1. 检查 IOMMU 启用状态
cat /proc/cmdline | grep -E "intel_iommu|amd_iommu"
# 需要: intel_iommu=on 或 amd_iommu=on

# 2. 检查 ACS (Access Control Services)
# ACS 确保设备间 DMA 隔离
setpci -s 00:1c.0 ECAP_ACS+6.b  # 读取 ACS 能力

# 3. 检查 IOMMU 组完整性
# 如果多个设备在同一组且不能分离，可能无法直通单个设备

# 4. 检查 dmesg 错误
dmesg | grep -i vfio
dmesg | grep -i iommu
```

---

## 📚 参考资料

- Linux kernel: `Documentation/driver-api/vfio.rst`
- Intel SDM Vol 3, Chapter 10: Advanced Programmable Interrupt Controller
- Intel VT-d Specification: IOMMU 硬件细节
- KVM Forum talks on VFIO and device passthrough
- 论文: *"VFIO: The Virtual Function I/O Framework"*

---

## ✅ 阶段检验清单

- [ ] VFIO 容器、组、设备三者的关系是什么？
- [ ] IOMMU 组为什么有时包含多个设备？如何处理？
- [ ] `vfio_dma_do_map()` 的完整路径是什么？从 ioctl 到 IOMMU 页表更新
- [ ] KVM-VFIO 桥接的作用是什么？为什么 KVM 需要知道 VFIO 组？
- [ ] 直通设备的 MSI-X 中断是如何路由到 Guest 的？
- [ ] 设备直通的 DMA 一致性如何保证？

---

## 🔍 VMM视角对比

### 用户态VMM vs KVM内核态设备直通

| 方面 | 用户态VMM (QEMU) | KVM内核态 |
|------|------------------|-----------|
| **设备管理** | 通过VFIO ioctl管理 | VFIO框架直接集成 |
| **DMA映射** | ioctl(VFIO_IOMMU_MAP_DMA) | 通过KVM memslot自动映射 |
| **中断路由** | ioctl配置中断路由 | KVM-VFIO桥接自动处理 |
| **一致性管理** | 手动处理 | KVM自动处理coherency |

### 关键差异：DMA映射路径

```
用户态VMM:
  Guest访问GPA → EPT翻译 → IOMMU翻译 → HPA
  DMA映射: ioctl(VFIO_IOMMU_MAP_DMA) → IOMMU页表

KVM内核态:
  Guest访问GPA → EPT翻译 → IOMMU翻译 → HPA
  DMA映射: KVM memslot自动映射 → IOMMU页表
```

---

## ⚡ 性能优化技术

### 1. IOTLB缓存

**问题**：IOMMU翻译开销大

**解决**：使用IOTLB缓存IOMMU翻译结果

```c
/* intel_iommu.c 中配置 */
if (iommu->iommu_caps & VTD_CAP_IOTLB_CACHE) {
    /* 启用IOTLB缓存 */
    iommu_enable_iotlb_cache(iommu);
}
```

**效果**：
- 减少IOMMU翻译延迟
- DMA密集负载性能提升20-30%

### 2. DMA批处理

**问题**：频繁的DMA映射/解映射开销大

**解决**：批量处理DMA映射

```c
/* vfio_dma_do_map() 中优化 */
/* 批量映射多个页面 */
for (i = 0; i < nr_pages; i++) {
    iommu_map(domain, iova + i * PAGE_SIZE, pfn[i] * PAGE_SIZE, PAGE_SIZE, prot);
}
```

**效果**：
- 减少IOMMU页表更新次数
- 性能提升10-15%

### 3. 中断亲和性

**问题**：中断在多个pCPU间迁移

**症状**：中断延迟不稳定

**解决**：
```bash
# 绑定中断到特定pCPU
echo 2 > /proc/irq/<irq_num>/smp_affinity
# 绑定到pCPU 1
```

**效果**：
- 减少TLB刷新
- 中断延迟稳定

---

## ⚠️ 常见陷阱

### 陷阱1：IOMMU组包含多个设备

**场景**：无法直通单个设备

**症状**：绑定 `vfio-pci` **会成功**，但随后 `VFIO_GROUP_SET_CONTAINER` 返回 `-EPERM`；
`VFIO_GROUP_GET_STATUS` 中 `VIABLE` 标志为 0。

**原因**：IOMMU组包含多个设备，组内仍有设备绑在普通驱动上，DMA ownership 无法认领。

`vfio-pci` 声明了 `.driver_managed_dma = true`（`drivers/vfio/pci/vfio_pci.c:205`），
probe 阶段刻意不动 `owner_cnt`，所以绑定不会失败。认领发生在 container 附加时
（`drivers/vfio/container.c:437`），此时才检查：

```c
/* 来源: drivers/iommu/iommu.c:3214 —— iommu_group_claim_dma_owner() */
	if (group->owner_cnt) {
		ret = -EPERM;
		goto unlock_out;
	}
```

**解决**：
```bash
# 列出组内全部成员
ls /sys/kernel/iommu_groups/<N>/devices/

# 组内所有设备都必须绑到 vfio-pci（或处于 unbound），然后整组一起直通
qemu-system-x86_64 ... \
  -device vfio-pci,host=0000:03:00.0 \
  -device vfio-pci,host=0000:03:00.1  # 同组的其他设备
```

> 详见 [corrections.md](corrections.md) 勘误 1，含 kprobe 实测的完整接管时序。

### 陷阱2：ACS未启用

**场景**：设备与上游桥被划入同一个 IOMMU group，无法单独直通

**症状**：`/sys/kernel/iommu_groups/<N>/devices/` 里除目标设备外还有上游桥

**原因**：上游链路某一跳不满足 `REQ_ACS_FLAGS`，内核认为无法保证 peer-to-peer DMA 隔离，
于是把桥拉进同一组：

```c
/* 来源: drivers/iommu/iommu.c:1543 —— pci_device_group() */
	if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
		break;

	pdev = bus->self;          /* 隔离不成立 → 把桥拉进来 */
```

**排查**：
```bash
# 看上游链路每一跳的 PCIe 类型与 ACS 能力
lspci -vvv -s 49:01.0 | grep -A2 "Access Control Services"
```

**注意**：ACS 是硬件能力，**没有内核参数能把它"打开"**。社区流传的
`pcie_acs_override=downstream,multifunction` 来自第三方补丁，**不在上游内核中**
（6.12.93 的 `drivers/pci/` 与 `kernel-parameters.txt` 均无此参数），
在原生内核上写了会被静默忽略；`pci=noaer` 关的是 AER，与 ACS 无关。
硬件不支持时唯一正规解法是整组一起直通 —— 强行拆组会真实破坏 DMA 隔离。

> 单功能设备"没有 ACS 能力"反而算隔离成立，这一点极易误判，
> 详见 [corrections.md](corrections.md) 勘误 2。
>
> 完整的划分规则（四步判定、`pci_acs_enabled()` 的三类处理、一个 switch 拆出 30+ 组的实测）
> 见 [1.4 IOMMU 组是怎么划出来的](#14-iommu-组是怎么划出来的)。

### 陷阱3：DMA一致性未处理

**场景**：Guest和Host看到不一致的数据

**症状**：Guest读取到过时的数据

**原因**：DMA一致性未处理

**解决**：
```c
// KVM-VFIO桥接中自动处理
kvm_vfio_update_coherency(kvm, vfio_group);
// 确保DMA操作使用正确的内存类型
```

### 陷阱4：中断停在 Remapped 模式，性能达不到预期

**场景**：直通设备中断能到 Guest，但延迟偏高、宿主 CPU 有可观的中断开销

**症状**：宿主 `/proc/interrupts` 上该设备的 IRQ 计数**在涨**。
Posted 模式下这些计数应当**始终为 0**，因为 `vfio_msihandler()` 不会被调用。

```bash
# 找出设备的宿主 IRQ 号，观察计数是否增长
cat /sys/bus/pci/devices/0000:4b:00.0/msi_irqs
watch -n1 'grep -E "^ *(112|113|114):" /proc/interrupts'
```

**原因**：IRTE 没有被改写成 Posted 模式（`IM=0`），中断仍然先进宿主、再经 eventfd
唤醒 irqfd，多绕一整圈。常见诱因：

1. `enable_apicv=N`，或硬件不支持 IRQ posting；
2. 该向量的目标不是单个 vCPU（多播/广播/多目标低优先级），被
   `posted_intr.c:315` 主动排除，这是**设计如此，不是故障**；
3. Guest 里做了 irqbalance，把向量的 affinity 改成多 CPU 掩码。

**排查**：
```bash
cat /sys/module/kvm_intel/parameters/enable_apicv    # 需要 Y
dmesg | grep -i "posting\|Posted-Interrupt"

# 区分 Remapped / Posted 需要看 IRTE 原始位，chip_name 只能告诉你有没有重映射
cat /sys/kernel/irq/112/chip_name                   # IR- 前缀 = 有重映射
```

`/sys/kernel/debug/kvm/` 下**没有** `irq_routing` 这个文件（本仓早期版本写错过）。
可靠的观测手段是 `kvm_pi_irte_update` tracepoint 或对 `modify_irte` 下 kprobe。

> 完整机制见 [3.3 节](#33-msi-x-中断直通从-remapped-到-posted)，
> 含实测数据的动手实验见 [practice/README.md](practice/README.md) 练习3。
