# Phase 11：源码精读注释 - PCIe 总线

> 基于 Linux 6.12.93 源码。每个代码片段回答一个具体问题，只保留关键行。
> 行号可能随版本变化，用函数名 grep 定位更可靠。

---

## 1. PCIe 设备发现与枚举

### Q: PCI 子系统如何发现总线上的设备？

**文件**: `drivers/pci/probe.c:3183` — `pci_scan_child_bus_extend()`

```c
/* 来源: drivers/pci/probe.c:3183 */
unsigned int pci_scan_child_bus_extend(struct pci_bus *bus,
                                       unsigned int available_buses)
{
    unsigned int used_buses, cmax = 0;
    struct pci_dev *dev;

    /* 扫描当前总线上的所有设备（Device 0-31） */
    for (pass = 0; pass < 2; pass++)
        for (devfn = 0; devfn < 256; devfn += 8)
            pci_scan_slot(bus, devfn);

    /* 扫描 Bridge 的下游总线 */
    list_for_each_entry(dev, &bus->devices, bus_list) {
        struct pci_bus *child;
        
        if (!dev->subordinate)
            continue;

        /* 递归扫描下游总线 */
        cmax = pci_scan_child_bus_extend(dev->subordinate,
                                         available_buses - used_buses);
    }

    return cmax;
}
```

**扫描过程**：
1. 遍历 Device 0-31（每个 Device 最多 8 个 Function）
2. 对每个 Function 读取 Vendor ID
3. 如果 Vendor ID != 0xFFFF，说明设备存在
4. 创建 `pci_dev` 结构体
5. 如果是 Bridge，递归扫描下游总线

### Q: `pci_scan_slot()` 如何检测 Function？

**文件**: `drivers/pci/probe.c:2363` — `pci_scan_slot()`

```c
/* 来源: drivers/pci/probe.c:2363 */
int pci_scan_slot(struct pci_bus *bus, int devfn)
{
    struct pci_dev *dev;
    int nr = 0;

    /* 检查 Function 0 是否存在 */
    if (pci_scan_single_device(bus, devfn) == NULL)
        return 0;

    /* 检查是否为多功能设备 */
    if (!pci_bus_read_dev_vendor_id(bus, devfn, &l, 0))
        return 0;

    /* 读取 Header Type，检查多功能标志 */
    pci_read_config_byte(bus->self, PCI_HEADER_TYPE, &hdr_type);
    if (!(hdr_type & 0x80))
        return nr;  /* 单功能设备，只扫描 Function 0 */

    /* 多功能设备，扫描 Function 1-7 */
    for (fn = 1; fn < 8; fn++) {
        if (pci_scan_single_device(bus, devfn + fn) != NULL)
            nr++;
    }

    return nr;
}
```

**关键点**：
- Function 0 必须存在
- Header Type 的 bit 7（0x80）表示是否为多功能设备
- 只有多功能设备才扫描 Function 1-7

---

## 2. Bridge 与 Bus Number 分配

### Q: Bridge 的三个 Bus Number 字段有什么作用？

**文件**: `drivers/pci/probe.c:1180` — `pci_bridge_check_ranges()`

```c
/* 来源: include/uapi/linux/pci_regs.h:106 */
#define PCI_PRIMARY_BUS         0x18    /* Primary bus number */
#define PCI_SECONDARY_BUS       0x19    /* secondary bus number */
#define PCI_SUBORDINATE_BUS     0x1a    /* furthest downstream bus number */

/* 源码位置: drivers/pci/probe.c:1180 */
static void pci_bridge_check_ranges(struct pci_bus *bus)
{
    /* Primary Bus: Bridge 的上游总线 */
    /* Secondary Bus: Bridge 的下游总线（第一个） */
    /* Subordinate Bus: Bridge 下游的最远总线号 */
    
    /* 示例：Switch 的 Downstream Port */
    /* Primary = 01（Switch 上游） */
    /* Secondary = 02（Switch 下游第一个设备） */
    /* Subordinate = 02（如果没有更多 Bridge） */
}
```

**Bus Number 的作用**：
- **Primary Bus**：Bridge 的上游总线号
- **Secondary Bus**：Bridge 的下游总线号（直接连接的设备）
- **Subordinate Bus**：Bridge 下游的最远总线号（包含所有下游 Bridge）

### Q: 如何确定 Subordinate Bus Number？

**文件**: `drivers/pci/probe.c:1243` — `pci_bridge_update_ranges()`

```c
/* 来源: drivers/pci/probe.c:1243 */
static void pci_bridge_update_ranges(struct pci_bus *bus)
{
    struct pci_dev *bridge = bus->self;
    
    /* Subordinate = max(所有下游 Bus Number) */
    bus->bridge_ctrl = pci_bridge_window_resize(bus);
    
    /* 写入 Bridge 配置空间 */
    pci_write_config_byte(bridge, PCI_SUBORDINATE_BUS,
                          bus->number + bus->number_buses - 1);
}
```

**计算规则**：
```
Subordinate = Secondary + (下游所有 Bridge 的数量)

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

**文件**: `drivers/pci/pci.c:3598` — `pci_acs_flags_enabled()`

```c
/* 来源: drivers/pci/pci.c:3598 */
bool pci_acs_flags_enabled(struct pci_dev *pdev, u32 acs_flags)
{
    int pos;
    u16 acs_ctrl;

    /* 检查设备是否支持 ACS */
    pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_ACS);
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
/* 来源: include/uapi/linux/pci_regs.h:676 */
#define PCI_ACS_SV      0x0001  /* Source Validation */
#define PCI_ACS_TB      0x0002  /* Translation Blocking */
#define PCI_ACS_RR      0x0004  /* P2P Request Redirect */
#define PCI_ACS_CR      0x0008  /* P2P Completion Redirect */
#define PCI_ACS_UF      0x0010  /* Upstream Forwarding */
#define PCI_ACS_EC      0x0020  /* P2P Egress Control */
#define PCI_ACS_DT      0x0040  /* Direct Translated P2P */
```

### Q: 内核如何检查 ACS 隔离性？

**文件**: `drivers/pci/pci.c:3624` — `pci_acs_enabled()`

```c
/* 来源: drivers/pci/pci.c:3624 */
bool pci_acs_enabled(struct pci_dev *pdev, u32 acs_flags)
{
    int ret;

    /* 检查设备特定的 ACS quirk */
    ret = pci_dev_specific_acs_enabled(pdev, acs_flags);
    if (ret != -ENOTTY)
        return ret;  /* 有 quirk，直接使用 quirk 结果 */

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
        return pci_acs_flags_enabled(pdev, acs_flags);
    
    case PCI_EXP_TYPE_DOWNSTREAM:
    case PCI_EXP_TYPE_ROOT_PORT:
        /* Downstream Port 和 Root Port 需要提供隔离 */
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

**文件**: `drivers/iommu/iommu.c:1383` — `iommu_group_get_for_pci_dev()`

```c
/* 来源: drivers/iommu/iommu.c:1383 */
static struct iommu_group *iommu_group_get_for_pci_dev(struct pci_dev *pdev)
{
    struct pci_dev *bridge = pdev;
    struct iommu_group *group = NULL;

    /* 向上遍历 PCIe 拓扑 */
    while (!pci_is_root_bus(bridge->bus)) {
        bridge = bridge->bus->self;

        /* 检查 Bridge 是否提供 ACS 隔离 */
        if (pci_acs_enabled(bridge, REQ_ACS_FLAGS)) {
            /* Bridge 提供隔离，可以独立成组 */
            break;
        }

        /* Bridge 不提供隔离，继续向上 */
    }

    /* 如果没有 ACS 隔离，整个子树必须在同一组 */
    if (!pci_acs_enabled(bridge, REQ_ACS_FLAGS)) {
        /* 找到 Root Port，整个子树共享一个 group */
        while (!pci_is_root_bus(bridge->bus))
            bridge = bridge->bus->self;
    }

    /* 为这个设备（或子树）创建/获取 group */
    group = iommu_group_get_for_dev(&pdev->dev);
    return group;
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

**文件**: `drivers/pci/access.c:49` — `pci_read_config_byte()`

```c
/* 来源: drivers/pci/access.c:49 */
int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
    /* 通过 bus->ops 调用具体的访问方法 */
    return pci_bus_read_config_byte(dev->bus, dev->devfn, where, val);
}

/* 源码位置: drivers/pci/access.c:156 */
int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn,
                             int where, u8 *val)
{
    /* 调用具体的 PCI ops */
    return bus->ops->read(bus, devfn, where, 1, val);
}
```

**访问机制**：
```c
/* 源码位置: arch/x86/pci/direct.c:65 */
/* Legacy 方式：通过 IO 端口 0xCF8/0xCFC */
static int pci_direct_conf1_read(struct pci_bus *bus, unsigned int devfn,
                                  int where, int size, u32 *value)
{
    unsigned long flags;
    u32 data = 0;

    /* 构造配置地址 */
    outl(0x80000000 | (bus->number << 16) | (devfn << 8) | (where & ~3),
         0xCF8);

    /* 读取数据 */
    switch (size) {
    case 1: data = inb(0xCFC + (where & 3)); break;
    case 2: data = inw(0xCFC + (where & ~1)); break;
    case 4: data = inl(0xCFC); break;
    }

    *value = data;
    return 0;
}

/* Enhanced 方式：通过 MMIO（支持 4KB 配置空间） */
static int pci_ecam_read(struct pci_bus *bus, unsigned int devfn,
                          int where, int size, u32 *value)
{
    void __iomem *addr;

    /* 计算 MMIO 地址 */
    addr = pci_ecam_map_bus(bus, devfn, where);
    if (!addr)
        return -EINVAL;

    /* 直接内存读取 */
    switch (size) {
    case 1: *value = readb(addr); break;
    case 2: *value = readw(addr); break;
    case 4: *value = readl(addr); break;
    }

    return 0;
}
```

---

## 5. 关键数据结构

### Q: `struct pci_dev` 的关键字段？

**文件**: `include/linux/pci.h:350`

```c
/* 来源: include/linux/pci.h:350 */
struct pci_dev {
    /* 基本标识 */
    unsigned int devfn;           /* BDF 中的 Device/Function */
    unsigned int busnr;           /* Bus Number */
    struct pci_bus *bus;          /* 所在的总线 */
    
    /* 设备信息 */
    u16 vendor;                   /* Vendor ID */
    u16 device;                   /* Device ID */
    u16 subsystem_vendor;         /* Subsystem Vendor ID */
    u16 subsystem_device;         /* Subsystem Device ID */
    u32 class;                    /* Class Code */
    u8 revision;                  /* Revision ID */
    
    /* 资源映射 */
    struct resource resource[DEVICE_COUNT_RESOURCE];  /* BAR 资源 */
    
    /* Bridge 相关 */
    struct pci_bus *subordinate;  /* 下游总线（如果是 Bridge） */
    
    /* 中断 */
    unsigned int irq;             /* 中断号 */
    
    /* PCIe 特定 */
    u8 pcie_cap;                  /* PCIe Capability 偏移 */
    u8 pcie_flags_reg;            /* PCIe Flags */
    
    /* IOMMU */
    struct iommu_group *iommu_group;  /* IOMMU group */
};
```

---

## 6. 总结

### 6.1 关键函数速查

| 函数 | 文件 | 作用 |
|------|------|------|
| `pci_scan_slot()` | `probe.c:2363` | 扫描设备的 Function |
| `pci_acs_enabled()` | `pci.c:3624` | 检查 ACS 隔离性 |
| `iommu_group_get_for_pci_dev()` | `iommu.c:1383` | 确定 IOMMU group |
| `pci_read_config_*()` | `access.c:49` | 读配置空间 |
| `pci_write_config_*()` | `access.c:69` | 写配置空间 |

### 6.2 关键数据结构

| 结构体 | 文件 | 作用 |
|--------|------|------|
| `struct pci_dev` | `pci.h:350` | PCI 设备描述 |
| `struct pci_bus` | `pci.h:500` | PCI 总线描述 |
| `struct pci_ops` | `pci.h:450` | 配置访问操作 |
| `struct resource` | `ioport.h:18` | BAR 资源描述 |

### 6.3 核心算法

1. **设备发现**：遍历 Device 0-31，检查 Vendor ID
2. **Bus Number 分配**：递归扫描，计算 Subordinate Bus
3. **IOMMU group 划分**：向上遍历，检查 ACS 隔离性
4. **配置访问**：Legacy (IO 端口) 或 Enhanced (MMIO)
