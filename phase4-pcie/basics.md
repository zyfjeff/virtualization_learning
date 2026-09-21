# PCIe 硬件基础

> PCIe（Peripheral Component Interconnect Express）是现代计算机系统中设备互连的标准。
> 理解 PCIe 的硬件基础是掌握设备直通的前提。

---

## 1. PCIe 拓扑结构

### 1.1 核心组件

PCIe 拓扑由三种核心组件构成：

```
┌─────────────────────────────────────────────────────────────┐
│                    PCIe 拓扑层次                              │
│                                                              │
│  ┌──────────────┐                                           │
│  │ Root Complex │  ← CPU 和内存的接口                         │
│  │  (RC)        │    - 包含 Host Bridge                      │
│  └──────┬───────┘    - 至少一个 PCIe Port                    │
│         │                                                     │
│    ┌────┴────┐                                               │
│    │ Switch  │  ← 可选的交换芯片                               │
│    │         │    - 包含多个 Port                             │
│    │  ┌──┬──┐│    -  upstream port (朝向 RC)                 │
│    │  │  │  ││    -  downstream port (朝向设备)              │
│    │  └──┴──┘│                                               │
│    └────┬───┘                                               │
│         │                                                     │
│    ┌────┴────┐                                               │
│    │Endpoint │  ← 终端设备（网卡、GPU、存储等）               │
│    │         │    - 包含一个或多个 Function                   │
│    └─────────┘                                               │
│                                                              │
│  关键概念：                                                    │
│  - Port：PCIe 连接的端点，包含一个 Link                       │
│  - Link：点对点连接，由多个 Lane 组成（x1/x2/x4/x8/x16）    │
│  - Function：设备内的逻辑单元（类似多网卡设备）               │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 Root Complex（RC）

**Root Complex** 是 PCIe 拓扑的根节点，连接 CPU/内存子系统：

```c
/* 源码位置: drivers/pci/probe.c:3058 */
/* PCI 子系统初始化时创建 Root Bus */
struct pci_bus *pci_create_root_bus(struct device *parent, int bus,
        struct pci_ops *ops, void *sysdata, struct list_head *resources)
{
    /* Root Bus 是 Bus 0，对应 Root Complex */
    b->number = b->secondary = bus;  /* Bus Number = 0 */
    ...
}
```

**Root Complex 的特点**：
- 不包含配置空间（对软件不可见）
- 至少包含一个 Root Port（downstream port）
- 负责生成配置请求和内存请求
- 通常集成在 CPU 芯片内（现代系统）

### 1.3 Switch

**PCIe Switch** 是可选的交换芯片，用于扩展 PCIe 端口数量：

```
┌─────────────────────────────────────┐
│         PCIe Switch                 │
│                                     │
│  ┌─────────────┐                   │
│  │ Upstream    │ ← 朝向 RC         │
│  │ Port        │                   │
│  └──────┬──────┘                   │
│         │                          │
│    ┌────┴────┐                     │
│    │ Internal│ ← 内部交叉开关      │
│    │ Fabric  │                     │
│    └────┬────┘                     │
│         │                          │
│  ┌──────┴──────┬──────┐           │
│  │             │      │           │
│  ▼             ▼      ▼           │
│ ┌────┐      ┌────┐ ┌────┐        │
│ │DS  │      │DS  │ │DS  │        │
│ │Port│      │Port│ │Port│        │
│ └────┘      └────┘ └────┘        │
│  ↓           ↓      ↓            │
│ EP1         EP2    EP3           │
└─────────────────────────────────────┘
```

**Switch 的特点**：
- 包含一个 upstream port 和多个 downstream port
- 每个 port 都是一个虚拟的 PCI-to-PCI Bridge
- Switch 本身有配置空间（Type 1）
- 用于扩展 PCIe 端口数量（如主板上的多个 PCIe 插槽）

### 1.4 Endpoint

**Endpoint** 是 PCIe 拓扑的终端设备：

```
┌─────────────────────────────────────┐
│         Endpoint Device             │
│                                     │
│  ┌─────────────────────────────┐   │
│  │ Function 0                  │   │  ← 网卡端口 1
│  │ - 配置空间（Type 0）        │   │
│  │ - BAR 0-5                   │   │
│  │ - MSI-X 表                  │   │
│  └─────────────────────────────┘   │
│                                     │
│  ┌─────────────────────────────┐   │
│  │ Function 1                  │   │  ← 网卡端口 2
│  │ - 配置空间（Type 0）        │   │
│  │ - BAR 0-5                   │   │
│  │ - MSI-X 表                  │   │
│  └─────────────────────────────┘   │
│                                     │
│  ┌──────────────┐                  │
│  │ Upstream     │ ← 连接到 Switch  │
│  │ Port         │    或 RC         │
│  └──────────────┘                  │
└─────────────────────────────────────┘
```

**Endpoint 的特点**：
- 配置空间为 Type 0（非 Bridge）
- 包含 BAR（Base Address Register）用于内存映射
- 可以包含多个 Function（多功能设备）
- Function 0 必须存在，其他 Function 可选

---

## 2. BDF 寻址机制

### 2.1 BDF 格式

每个 PCIe Function 由 **BDF（Bus/Device/Function）** 唯一标识：

```
┌─────────────────────────────────────┐
│         BDF 格式                    │
│                                     │
│  Bus:      8 bits  (0-255)         │
│  Device:   5 bits  (0-31)          │
│  Function: 3 bits  (0-7)           │
│                                     │
│  总计：16 bits = 256 * 32 * 8      │
│       = 65536 个 Function          │
│                                     │
│  示例：01:00.0                     │
│  - Bus 01：PCIe Switch 的下游总线  │
│  - Device 00：Switch 的第一个端口  │
│  - Function 0：设备的第一个功能    │
└─────────────────────────────────────┘
```

### 2.2 BDF 与拓扑的关系

BDF 编码反映了 PCIe 拓扑的层次结构：

```c
/* 源码位置: include/uapi/linux/pci.h:31-33 */
#define PCI_DEVFN(slot, func)   ((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn)         (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)         ((devfn) & 0x07)

/* 源码位置: include/linux/pci.h:73 */
#define PCI_BUS_NUM(devfn)      (((devfn) >> 8) & 0xff)  /* 从 devfn 提取 Bus */
```

**拓扑映射规则**：
- **Bus Number**：标识 PCIe 段（Segment）
  - Bus 0：Root Complex 的下游
  - Bus 1-N：Bridge 的 Secondary Bus
- **Device Number**：标识 Bridge 的下游端口（0-31）
- **Function Number**：标识设备内的功能单元（0-7）

### 2.3 BDF 示例

```
┌─────────────────────────────────────────────────────────────┐
│  PCIe 拓扑与 BDF 映射                                        │
│                                                              │
│  Bus 00 (Root Complex)                                      │
│  ├── 00:00.0 - Host Bridge (集成在 RC 内)                   │
│  ├── 00:01.0 - PCIe Root Port → Bus 01                     │
│  ├── 00:02.0 - PCIe Root Port → Bus 03                     │
│  └── 00:1f.0 - ISA Bridge (LPC)                            │
│                                                              │
│  Bus 01 (Switch 上游)                                       │
│  └── 01:00.0 - PCIe Switch Upstream Port                   │
│                                                              │
│  Bus 02 (Switch 下游)                                       │
│  ├── 02:00.0 - Switch DS Port → Endpoint (网卡)            │
│  │   └── 02:00.0 - Endpoint Function 0                     │
│  │   └── 02:00.1 - Endpoint Function 1                     │
│  ├── 02:01.0 - Switch DS Port → Endpoint (GPU)             │
│  └── 02:02.0 - Switch DS Port → Endpoint (NVMe)            │
│                                                              │
│  Bus 03 (直接连接的设备)                                    │
│  └── 03:00.0 - Endpoint (WiFi 网卡)                        │
└─────────────────────────────────────────────────────────────┘

lspci 输出示例：
$ lspci -s 02:00.0
02:00.0 Ethernet controller: Intel Corporation I210 Gigabit Network Connection
         Subsystem: Intel Corporation Device 0000
```

---

## 3. 配置空间

### 3.1 配置空间类型

PCIe 定义了三种配置空间类型：

```
┌─────────────────────────────────────────────────────────────┐
│  配置空间类型                                                │
│                                                              │
│  Type 0: Endpoint                                           │
│  ┌─────────────────────────────────────────┐               │
│  │ 0x00-0x3F: 标准头（64 字节）            │               │
│  │   - Device ID / Vendor ID               │               │
│  │   - Command / Status                    │               │
│  │   - Class Code / Revision ID            │               │
│  │   - BAR 0-5                             │               │
│  │   - Subsystem ID / Vendor ID            │               │
│  │   - Expansion ROM BAR                   │               │
│  │   - Capabilities Pointer                │               │
│  │   - Interrupt Line / Pin                │               │
│  └─────────────────────────────────────────┘               │
│                                                              │
│  Type 1: Bridge (PCIe-to-PCIe)                              │
│  ┌─────────────────────────────────────────┐               │
│  │ 0x00-0x3F: 标准头（64 字节）            │               │
│  │   - 与 Type 0 相同的前 16 字节          │               │
│  │   - Primary Bus Number                  │               │
│  │   - Secondary Bus Number                │               │
│  │   - Subordinate Bus Number              │               │
│  │   - Memory / IO Base & Limit            │               │
│  └─────────────────────────────────────────┘               │
│                                                              │
│  Type 2: CardBus Bridge (PCI 兼容，已过时)                 │
│  └── 基本不使用                                              │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 配置空间访问

内核通过 `pci_read_config_*` / `pci_write_config_*` 访问配置空间：

```c
/* 源码位置: drivers/pci/access.c:560 */
int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
    if (pci_dev_is_disconnected(dev)) {
        PCI_SET_ERROR_RESPONSE(val);
        return PCIBIOS_DEVICE_NOT_FOUND;
    }
    return pci_bus_read_config_byte(dev->bus, dev->devfn, where, val);
}

/* 源码位置: drivers/pci/access.c:570 */
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
    if (pci_dev_is_disconnected(dev)) {
        PCI_SET_ERROR_RESPONSE(val);
        return PCIBIOS_DEVICE_NOT_FOUND;
    }
    return pci_bus_read_config_word(dev->bus, dev->devfn, where, val);
}
```

**配置访问机制**：
- **Legacy 方式**：通过 IO 端口 0xCF8/0xCFC（仅支持 256 字节）
- **Enhanced 方式**：通过 MMIO（支持 4KB 配置空间）

### 3.3 关键配置寄存器

```c
/* 源码位置: include/uapi/linux/pci_regs.h */

/* 标准头（Type 0/1 共用） */
#define PCI_VENDOR_ID           0x00    /* 16 bits */
#define PCI_DEVICE_ID           0x02    /* 16 bits */
#define PCI_COMMAND             0x04    /* 16 bits */
#define PCI_STATUS              0x06    /* 16 bits */
#define PCI_CLASS_REVISION      0x08    /* 32 bits */
#define PCI_BASE_ADDRESS_0      0x10    /* 32 bits */
#define PCI_BASE_ADDRESS_1      0x14    /* 32 bits */
#define PCI_BASE_ADDRESS_2      0x18    /* 32 bits */
#define PCI_BASE_ADDRESS_3      0x1c    /* 32 bits */
#define PCI_BASE_ADDRESS_4      0x20    /* 32 bits */
#define PCI_BASE_ADDRESS_5      0x24    /* 32 bits */

/* Bridge 特有（Type 1） */
#define PCI_PRIMARY_BUS         0x18    /* 8 bits */
#define PCI_SECONDARY_BUS       0x19    /* 8 bits */
#define PCI_SUBORDINATE_BUS     0x1a    /* 8 bits */
#define PCI_SEC_LATENCY_TIMER   0x1b    /* 8 bits - Secondary bus latency timer */
```

---

## 4. BAR（Base Address Register）

### 4.1 BAR 的作用

BAR 用于设备向系统请求内存或 IO 映射区域：

```
┌─────────────────────────────────────────────────────────────┐
│  BAR 映射机制                                                │
│                                                              │
│  设备侧：                                                    │
│  ┌──────────────────────────────────────┐                   │
│  │ Endpoint Device                      │                   │
│  │  - BAR 0: 寄存器映射（MMIO）        │                   │
│  │  - BAR 1: 未使用                    │                   │
│  │  - BAR 2: 帧缓冲区（大内存区域）    │                   │
│  │  - BAR 3-5: 未使用                  │                   │
│  └──────────────────────────────────────┘                   │
│                                                              │
│  系统侧：                                                    │
│  ┌──────────────────────────────────────┐                   │
│  │ 物理地址空间                         │                   │
│  │  0x00000000_FEBF0000 - BAR 0 (4KB)  │ ← 寄存器         │
│  │  0x00000000_C0000000 - BAR 2 (256MB)│ ← 帧缓冲         │
│  └──────────────────────────────────────┘                   │
│                                                              │
│  CPU 访问：                                                  │
│  - writel(0xFEBF0000, value) → 写入设备寄存器               │
│  - 通过 PCIe 总线传输到设备                                  │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 BAR 的类型

BAR 支持三种类型：

```c
/* 源码位置: include/uapi/linux/pci_regs.h:88 */
#define PCI_BASE_ADDRESS_SPACE     0x01  /* 0 = memory, 1 = I/O */
#define PCI_BASE_ADDRESS_SPACE_IO  0x01
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0x00

/* 内存类型的子类型 */
#define PCI_BASE_ADDRESS_MEM_TYPE_32   0x00  /* 32-bit 地址 */
#define PCI_BASE_ADDRESS_MEM_TYPE_1M   0x02  /* Below 1MB (已过时) */
#define PCI_BASE_ADDRESS_MEM_TYPE_64   0x04  /* 64-bit 地址 */
#define PCI_BASE_ADDRESS_MEM_TYPE_MASK 0x06
```

**BAR 类型**：
1. **32-bit Memory BAR**：支持 4GB 以下地址
2. **64-bit Memory BAR**：支持任意地址（占用两个 BAR 槽位）
3. **IO BAR**：支持 IO 端口地址（已过时，推荐使用 MMIO）

### 4.3 BAR 枚举过程

系统启动时，BIOS/内核会枚举所有 BAR 并分配地址。**BAR 大小探测**和**地址分配**是两个独立步骤。

**步骤 1：BAR 大小探测**（设备枚举阶段）

```c
/* 来源: drivers/pci/probe.c:176 — __pci_read_base() */
/* 在设备枚举时调用，探测 BAR 大小 */
pci_read_config_dword(dev, pos, &l);        /* 1. 读取 BAR 原始值 */
pci_write_config_dword(dev, pos, l | mask); /* 2. 写入全 1 */
pci_read_config_dword(dev, pos, &sz);       /* 3. 读回值（低位为 0 表示大小） */
pci_write_config_dword(dev, pos, l);        /* 4. 恢复原始值 */
```

**步骤 2：地址分配**（资源分配阶段）

```c
/* 来源: drivers/pci/setup-res.c:329 — pci_assign_resource() */
int pci_assign_resource(struct pci_dev *dev, int resno)
{
    struct resource *res = dev->resource + resno;
    
    /* 资源大小已在枚举阶段通过 __pci_read_base() 确定 */
    /* 这里只负责分配地址并写入 BAR */
    ret = _pci_assign_resource(dev, resno, size, min);
    if (ret)
        ret = pci_revert_fw_address(res, dev, resno, size);
    
    return ret;
}
```

**BAR 枚举完整流程**：

| 阶段 | 函数 | 位置 | 作用 |
|------|------|------|------|
| 设备枚举 | `__pci_read_base()` | `probe.c:176` | 写全 1 读回，确定 BAR 大小 |
| 资源分配 | `pci_assign_resource()` | `setup-res.c:329` | 分配地址，写入 BAR |
| Bridge 窗口 | `__pci_bus_size_bridges()` | `setup-bus.c:1280` | 为 Bridge 下游分配地址范围 |

**BAR 大小探测原理**：
1. 读取 BAR 原始值（保存）
2. 写入全 1（0xFFFFFFFF）
3. 读回值：低位为 0 的位数表示 BAR 大小
4. 恢复 BAR 原始值

---

## 5. PCIe 能力（Capabilities）

### 5.1 能力结构

PCIe 能力是可选的扩展功能，通过 **Capability Pointer** 链接：

```
┌─────────────────────────────────────────────────────────────┐
│  配置空间中的能力链表                                        │
│                                                              │
│  0x00-0x3F: 标准头                                          │
│  └── 0x34: Capabilities Pointer ─┐                         │
│                                  ↓                         │
│  0x40: ┌──────────────────────┐                           │
│        │ Capability ID: 0x10  │ ← PCIe Capability         │
│        │ Next Pointer: 0x60   │ ─┐                        │
│        │ PCIe 特定字段...     │  │                        │
│        └──────────────────────┘  │                        │
│                                  ↓                         │
│  0x60: ┌──────────────────────┐                           │
│        │ Capability ID: 0x11  │ ← MSI Capability          │
│        │ Next Pointer: 0x80   │ ─┐                        │
│        │ MSI 特定字段...      │  │                        │
│        └──────────────────────┘  │                        │
│                                  ↓                         │
│  0x80: ┌──────────────────────┐                           │
│        │ Capability ID: 0x05  │ ← MSI-X Capability        │
│        │ Next Pointer: 0x00   │ ─┐ (链表结束)             │
│        │ MSI-X 特定字段...    │  │                        │
│        └──────────────────────┘  │                        │
│                                  ↓                         │
│  0x00: NULL (链表结束)                                   │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 关键能力 ID

```c
/* 源码位置: include/uapi/linux/pci_regs.h */
#define PCI_CAP_ID_PM         0x01  /* Power Management */
#define PCI_CAP_ID_AGP        0x02  /* AGP (已过时) */
#define PCI_CAP_ID_VPD        0x03  /* Vital Product Data */
#define PCI_CAP_ID_SLOTID     0x04  /* Slot Identification */
#define PCI_CAP_ID_MSI        0x05  /* Message Signaled Interrupts */
#define PCI_CAP_ID_CHSWP      0x06  /* CompactPCI HotSwap (已过时) */
#define PCI_CAP_ID_PCIX       0x07  /* PCI-X (已过时) */
#define PCI_CAP_ID_HT         0x08  /* HyperTransport (已过时) */
#define PCI_CAP_ID_VNDR       0x09  /* Vendor Specific */
#define PCI_CAP_ID_DBG        0x0A  /* Debug port */
#define PCI_CAP_ID_CCRC       0x0B  /* CompactPCI Central Resource Control */
#define PCI_CAP_ID_SHPC       0x0C  /* PCI Hot-plug */
#define PCI_CAP_ID_SSVID      0x0D  /* Bridge subsystem vendor/device ID */
#define PCI_CAP_ID_AGP3       0x0E  /* AGP 8x (已过时) */
#define PCI_CAP_ID_SECDEV     0x0F  /* Secure Device */
#define PCI_CAP_ID_EXP        0x10  /* PCI Express */
#define PCI_CAP_ID_MSIX       0x11  /* MSI-X */
#define PCI_CAP_ID_SATA       0x12  /* SATA Data/Index Conf */
#define PCI_CAP_ID_AF         0x13  /* PCI Advanced Features */
```

### 5.3 PCIe Capability 结构

```c
/* 源码位置: include/uapi/linux/pci_regs.h:586 */
/* PCIe Capability (ID = 0x10) */
#define PCI_EXP_FLAGS         2     /* Capabilities register */
#define PCI_EXP_FLAGS_VERS    0x000f /* Capability version */
#define PCI_EXP_FLAGS_TYPE    0x00f0 /* Device/Port type */
#define PCI_EXP_FLAGS_SLOT    0x0100 /* Slot implemented */
#define PCI_EXP_FLAGS_IRQ     0x3e00 /* Interrupt message number */

/* Device/Port types */
#define PCI_EXP_TYPE_ENDPOINT   0x0  /* Endpoint */
#define PCI_EXP_TYPE_LEG_END    0x1  /* Legacy Endpoint */
#define PCI_EXP_TYPE_ROOT_PORT  0x4  /* Root Port */
#define PCI_EXP_TYPE_UPSTREAM   0x5  /* Upstream Port */
#define PCI_EXP_TYPE_DOWNSTREAM 0x6  /* Downstream Port */
#define PCI_EXP_TYPE_PCI_BRIDGE 0x7  /* PCIe to PCI/PCI-X Bridge */
#define PCI_EXP_TYPE_PCIE_BRIDGE 0x8 /* PCI/PCI-X to PCIe Bridge */
#define PCI_EXP_TYPE_RC_END     0x9  /* Root Complex Integrated Endpoint */
#define PCI_EXP_TYPE_RC_EC      0xa  /* Root Complex Event Collector */
```

---

## 6. 实战：使用 lspci 探索 PCIe 拓扑

### 6.1 lspci 基本用法

```bash
# 列出所有设备（简短格式）
$ lspci
00:00.0 Host bridge: Intel Corporation Xeon E3-1200 v5/E3-1500 v5/6th Gen Core Processor Host Bridge/DRAM Registers (rev 08)
00:01.0 PCI bridge: Intel Corporation Xeon E3-1200 v5/E3-1500 v5/6th Gen Core Processor PCIe Controller (x16) (rev 08)
00:14.0 USB controller: Intel Corporation 100 Series/C230 Series Chipset Family USB 3.0 xHCI Controller (rev 31)
00:1f.0 ISA bridge: Intel Corporation H110 Chipset LPC/eSPI Controller (rev 31)
01:00.0 VGA compatible controller: NVIDIA Corporation GM206 [GeForce GTX 950] (rev a1)

# 树形显示（显示拓扑）
$ lspci -t
-[0000:00]-+-00.0  Intel Corporation Xeon E3-1200 v5/E3-1500 v5/6th Gen Core Processor Host Bridge/DRAM Registers
           +-01.0-[01]--+-00.0  NVIDIA Corporation GM206 [GeForce GTX 950]
           |            \-00.1  NVIDIA Corporation GM206 High Definition Audio Controller
           +-14.0  Intel Corporation 100 Series/C230 Series Chipset Family USB 3.0 xHCI Controller
           \-1f.0  Intel Corporation H110 Chipset LPC/eSPI Controller

# 详细显示（包含配置空间）
$ lspci -xxx -s 01:00.0
01:00.0 VGA compatible controller: NVIDIA Corporation GM206 [GeForce GTX 950] (rev a1)
00: de 10 06 00 07 04 10 00 a1 00 00 03 00 00 80 00
10: 00 00 00 f1 00 00 00 f0 00 00 00 f2 00 00 00 f3
20: 00 00 00 f4 00 00 00 f5 00 00 00 00 de 10 44 01
30: 00 00 00 00 60 00 00 00 00 00 00 00 0a 01 00 00
```

### 6.2 setpci 工具

`setpci` 可以直接读写配置空间：

```bash
# 读取 Vendor ID 和 Device ID（偏移 0x00，32 位）
$ setpci -s 01:00.0 0.l
f1000000  # 低 16 位 = Device ID, 高 16 位 = Vendor ID

# 读取 Command 寄存器（偏移 0x04，16 位）
$ setpci -s 01:00.0 4.w
0407  # Memory Space + IO Space + Bus Master + ...

# 读取 BAR0（偏移 0x10，32 位）
$ setpci -s 01:00.0 10.l
f1000000  # BAR0 的当前值

# 修改 Command 寄存器（危险操作！）
$ setpci -s 01:00.0 COMMAND=0407.w  # 写入新值
```

---

## 7. 总结

### 7.1 关键概念回顾

| 概念 | 要点 |
|------|------|
| **拓扑** | RC → Switch → Endpoint，层次化结构 |
| **BDF** | Bus/Device/Function，唯一标识设备 |
| **配置空间** | Type 0（Endpoint）/ Type 1（Bridge） |
| **BAR** | 设备请求内存/IO 映射的机制 |
| **能力** | 可选扩展功能，链表结构 |

### 7.2 与虚拟化的关系

- **BDF** → 设备标识，IOMMU group 划分
- **BAR** → 设备内存映射，MMIO 直通
- **能力** → ACS（隔离）、ATS（性能）、SR-IOV（虚拟化）
- **拓扑** → 决定设备直通的约束和可能性

### 7.3 下一步

- 深入学习 PCIe 拓扑枚举（下一章）
- 理解 PCIe 能力与虚拟化的关系
- 掌握 VFIO 设备直通的拓扑约束
