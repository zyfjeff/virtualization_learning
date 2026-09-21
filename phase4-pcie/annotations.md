# Phase 4：源码精读注释 - PCIe 总线

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号已经过验证，基于 Linux 6.12.93。

---

## 1. PCIe 设备发现与枚举

### Q: PCI 子系统如何发现总线上的设备？

**文件**: `drivers/pci/probe.c:2963` — `pci_scan_child_bus_extend()`

```c
/* 来源: drivers/pci/probe.c:2963 */
static unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
                                              unsigned int available_buses)
{
    unsigned int used_buses, normal_bridges = 0, hotplug_bridges = 0;
    unsigned int start = bus->busn_res.start;
    unsigned int devfn, cmax, max = start;
    struct pci_dev *dev;

    /* 扫描当前总线上的所有设备（Device 0-31，每 Device 8 个 Function） */
    for (devfn = 0; devfn < 256; devfn += 8)
        pci_scan_slot(bus, devfn);

    /* 为 SR-IOV 保留总线号 */
    used_buses = pci_iov_bus_range(bus);
    max += used_buses;

    /* 扫描 Bridge 的下游总线 */
    for_each_pci_bridge(dev, bus) {
        cmax = max;
        /* 递归扫描下游总线 */
        cmax = pci_scan_child_bus_extend(dev->subordinate,
                                         available_buses - used_buses);
        max = max(cmax, max);
    }

    return max;
}
```

**扫描过程**：
1. 遍历 Device 0-31（每个 Device 最多 8 个 Function）
2. 对每个 Function 调用 `pci_scan_slot()`
3. 创建 `pci_dev` 结构体
4. 如果是 Bridge，递归扫描下游总线

### Q: `pci_scan_slot()` 如何检测 Function？

**文件**: `drivers/pci/probe.c:2747` — `pci_scan_slot()`

```c
/* 来源: drivers/pci/probe.c:2747 */
int pci_scan_slot(struct pci_bus *bus, int devfn)
{
    struct pci_dev *dev;
    int fn = 0, nr = 0;

    if (only_one_child(bus) && (devfn > 0))
        return 0; /* 单功能设备，已扫描 */

    do {
        dev = pci_scan_single_device(bus, devfn + fn);
        if (dev) {
            if (!pci_dev_is_added(dev))
                nr++;
            if (fn > 0)
                dev->multifunction = 1;
        } else if (fn == 0) {
            /* Function 0 不存在，停止扫描 */
            if (!hypervisor_isolated_pci_functions())
                break;
        }
        fn = next_fn(bus, dev, fn);
    } while (fn >= 0);

    return nr;
}
```

**关键点**：
- Function 0 必须存在
- 使用 `do-while` 循环遍历 Function
- `next_fn()` 决定下一个 Function 号

---

## 2. Bridge 与 Bus Number 分配

### Q: Bridge 的三个 Bus Number 字段有什么作用？

**文件**: `include/uapi/linux/pci_regs.h:132-134`

```c
/* 来源: include/uapi/linux/pci_regs.h:132-134 */
#define PCI_PRIMARY_BUS         0x18    /* Primary bus number */
#define PCI_SECONDARY_BUS       0x19    /* secondary bus number */
#define PCI_SUBORDINATE_BUS     0x1a    /* furthest downstream bus number */
#define PCI_SEC_LATENCY_TIMER   0x1b    /* Secondary bus latency timer */
```

**Bus Number 的作用**：
- **Primary Bus**：Bridge 的上游总线号
- **Secondary Bus**：Bridge 的下游总线号（直接连接的设备）
- **Subordinate Bus**：Bridge 下游的最远总线号（包含所有下游 Bridge）

### Q: 如何确定 Subordinate Bus Number？

**计算规则**：
```
Subordinate = max(所有下游 Bus Number)

示例：
Bus 01: Switch Upstream Port
  └─ Bus 02: Switch Downstream Port 0
      ├─ Bus 03: Endpoint (无 Bridge)
      └─ Bus 04: Switch Downstream Port 1
          └─ Bus 05: Endpoint

对于 Downstream Port 0 (Bus 02):
  Secondary = 02
  Subordinate = 03（最远的下游 Bus）

对于 Downstream Port 1 (Bus 04):
  Secondary = 04
  Subordinate = 05（最远的下游 Bus）
```

---

## 3. ACS（Access Control Services）检查

### Q: ACS 如何影响 IOMMU group 划分？

**文件**: `drivers/pci/pci.c:3583` — `pci_acs_flags_enabled()`

```c
/* 来源: drivers/pci/pci.c:3583 */
static bool pci_acs_flags_enabled(struct pci_dev *pdev, u32 acs_flags)
{
    int pos;
    u16 acs_ctrl;

    /* 使用缓存的 ACS Capability 位置 */
    pos = pdev->acs_cap;
    if (!pos)
        return false;  /* 不支持 ACS */

    /* 读取 ACS Control 寄存器 */
    pci_read_config_word(pdev, pos + PCI_ACS_CTRL, &acs_ctrl);

    /* 检查请求的 ACS 标志是否启用 */
    acs_flags &= PCI_ACS_FLAGS_MASK;
    return (acs_ctrl & acs_flags) == acs_flags;
}
```

**ACS 标志含义**：
```c
/* 来源: include/uapi/linux/pci_regs.h:991-997 */
#define PCI_ACS_SV      0x0001  /* Source Validation */
#define PCI_ACS_TB      0x0002  /* Translation Blocking */
#define PCI_ACS_RR      0x0004  /* P2P Request Redirect */
#define PCI_ACS_CR      0x0008  /* P2P Completion Redirect */
#define PCI_ACS_UF      0x0010  /* Upstream Forwarding */
#define PCI_ACS_EC      0x0020  /* P2P Egress Control */
#define PCI_ACS_DT      0x0040  /* Direct Translated P2P */
```

### Q: 内核如何检查 ACS 隔离性？

**文件**: `drivers/pci/pci.c:3620` — `pci_acs_enabled()`

```c
/* 来源: drivers/pci/pci.c:3620 */
bool pci_acs_enabled(struct pci_dev *pdev, u32 acs_flags)
{
    int ret;

    /* 检查 PCIe 设备 */
    if (!pci_is_pcie(pdev))
        return false;

    /* 检查设备特定的 ACS quirk */
    ret = pci_dev_specific_acs_enabled(pdev, acs_flags);
    if (ret >= 0)
        return ret > 0;  /* 有 quirk，直接使用 quirk 结果 */

    /* 检查 Root Complex Integrated Endpoint */
    if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_END)
        return true;  /* RC 集成端点，总是隔离的 */

    /* 检查 PCIe 类型 */
    switch (pci_pcie_type(pdev)) {
    case PCI_EXP_TYPE_ENDPOINT:
    case PCI_EXP_TYPE_UPSTREAM:
    case PCI_EXP_TYPE_LEG_END:
        return false;  /* Endpoint 不提供隔离 */
    
    case PCI_EXP_TYPE_PCI_BRIDGE:
    case PCI_EXP_TYPE_PCIE_BRIDGE:
    case PCI_EXP_TYPE_DOWNSTREAM:
    case PCI_EXP_TYPE_ROOT_PORT:
        /* Bridge 需要提供隔离 */
        return pci_acs_flags_enabled(pdev, acs_flags);
    }

    return false;
}
```

**关键逻辑**：
1. 先检查设备特定的 quirk（某些设备有特殊处理）
2. RC 集成端点（如集成显卡）总是隔离的
3. Endpoint 不提供隔离（它们是被隔离的对象）
4. Bridge（Downstream Port / Root Port）需要提供隔离

### Q: IOMMU group 如何由 ACS 决定？

**文件**: `drivers/iommu/iommu.c:1515` — `pci_device_group()`

```c
/* 来源: drivers/iommu/iommu.c:1515 */
struct iommu_group *pci_device_group(struct device *dev)
{
    struct pci_dev *pdev = to_pci_dev(dev);
    struct group_for_pci_data data;
    struct pci_bus *bus;
    struct iommu_group *group = NULL;

    /* 查找上游 DMA 别名 */
    if (pci_for_each_dma_alias(pdev, get_pci_alias_or_group, &data))
        return data.group;

    pdev = data.pdev;

    /* 向上遍历，直到找到 ACS 隔离点 */
    for (bus = pdev->bus; !pci_is_root_bus(bus); bus = bus->parent) {
        if (!bus->self)
            continue;

        /* 检查 Bridge 是否提供 ACS 隔离 */
        if (pci_acs_path_enabled(bus->self, NULL, REQ_ACS_FLAGS))
            break;

        pdev = bus->self;

        /* 如果找到已有的 group，使用它 */
        group = iommu_group_get(&pdev->dev);
        if (group)
            return group;
    }

    /* 查找别名 group */
    group = get_pci_alias_group(pdev, ...);
    if (group)
        return group;

    /* 没有共享 group，分配新的 */
    return iommu_group_alloc();
}
```

**算法逻辑**：
```
从设备向上遍历：
  ├─ 如果 Bridge 启用 ACS → 设备可以独立成组
  └─ 如果 Bridge 未启用 ACS → 继续向上，直到找到 ACS Bridge 或 Root

示例：
Bus 01: Root Port (ACS 启用)
  └─ Bus 02: Switch Downstream Port (ACS 未启用)
      ├─ Device A (Endpoint)
      └─ Device B (Endpoint)

结果：
  Device A 和 Device B 必须在同一 IOMMU group
  （因为它们之间的 Bridge 没有 ACS 隔离）
```

---

## 4. PCIe 配置访问实现

### Q: 内核如何访问 PCIe 配置空间？

**文件**: `drivers/pci/access.c:560` — `pci_read_config_byte()`

```c
/* 来源: drivers/pci/access.c:560 */
int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
    if (pci_dev_is_disconnected(dev)) {
        PCI_SET_ERROR_RESPONSE(val);
        return PCIBIOS_DEVICE_NOT_FOUND;
    }
    return pci_bus_read_config_byte(dev->bus, dev->devfn, where, val);
}
```

**访问机制**：
```c
/* 来源: arch/x86/pci/direct.c:21 — pci_conf1_read() */
/* Legacy 方式：通过 IO 端口 0xCF8/0xCFC */
static int pci_conf1_read(unsigned int seg, unsigned int bus,
                          unsigned int devfn, int reg, int len, u32 *value)
{
    unsigned long flags;

    if (seg || (bus > 255) || (devfn > 255) || (reg > 4095)) {
        *value = -1;
        return -EINVAL;
    }

    raw_spin_lock_irqsave(&pci_config_lock, flags);

    /* 构造配置地址 */
    outl(PCI_CONF1_ADDRESS(bus, devfn, reg), 0xCF8);

    /* 读取数据 */
    switch (len) {
    case 1: *value = inb(0xCFC + (reg & 3)); break;
    case 2: *value = inw(0xCFC + (reg & 2)); break;
    case 4: *value = inl(0xCFC); break;
    }

    raw_spin_unlock_irqrestore(&pci_config_lock, flags);

    return 0;
}
```

---

## 5. 关键数据结构

### Q: `struct pci_dev` 的关键字段？

**文件**: `include/linux/pci.h:336`

```c
/* 来源: include/linux/pci.h:336 */
struct pci_dev {
    /* 基本标识 */
    unsigned int devfn;           /* BDF 中的 Device/Function */
    struct pci_bus *bus;          /* 所在的总线 */
    struct pci_bus *subordinate;  /* 下游总线（如果是 Bridge） */
    
    /* 设备信息 */
    unsigned short vendor;        /* Vendor ID */
    unsigned short device;        /* Device ID */
    unsigned short subsystem_vendor; /* Subsystem Vendor ID */
    unsigned short subsystem_device; /* Subsystem Device ID */
    unsigned int class;           /* Class Code */
    u8 revision;                  /* Revision ID */
    
    /* PCIe 特定 */
    u8 pcie_cap;                  /* PCIe Capability 偏移 */
    u16 pcie_flags_reg;           /* PCIe Flags */
    u8 msi_cap;                   /* MSI capability offset */
    u8 msix_cap;                  /* MSI-X capability offset */
    
    /* 中断 */
    u8 pin;                       /* Interrupt pin */
    
    /* 资源映射 */
    struct resource resource[DEVICE_COUNT_RESOURCE];  /* BAR 资源 */
    
    /* 驱动 */
    struct pci_driver *driver;    /* 绑定的驱动 */
};
```

**注意**：
- Bus Number 通过 `dev->bus->number` 访问
- IOMMU group 通过 `dev->dev.iommu_group` 访问

---

## 6. 总结

### 6.1 关键函数速查

| 函数 | 文件 | 作用 |
|------|------|------|
| `pci_scan_child_bus_extend()` | `probe.c:2963` | 递归扫描总线 |
| `pci_scan_slot()` | `probe.c:2747` | 扫描设备的 Function |
| `pci_acs_enabled()` | `pci.c:3620` | 检查 ACS 隔离性 |
| `pci_device_group()` | `iommu.c:1515` | 确定 IOMMU group |
| `pci_read_config_byte()` | `access.c:560` | 读配置空间 |
| `pci_conf1_read()` | `direct.c:21` | Legacy 配置读取 |

### 6.2 关键数据结构

| 结构体 | 文件 | 作用 |
|--------|------|------|
| `struct pci_dev` | `pci.h:336` | PCI 设备描述 |
| `struct pci_bus` | `pci.h:500` | PCI 总线描述 |
| `struct pci_ops` | `pci.h:450` | 配置访问操作 |
| `struct resource` | `ioport.h:18` | BAR 资源描述 |

### 6.3 核心算法

1. **设备发现**：遍历 Device 0-31，调用 `pci_scan_slot()`
2. **Bus Number 分配**：递归扫描，计算 Subordinate Bus
3. **IOMMU group 划分**：向上遍历，检查 ACS 隔离性
4. **配置访问**：Legacy (IO 端口 0xCF8/0xCFC) 或 Enhanced (MMIO)
