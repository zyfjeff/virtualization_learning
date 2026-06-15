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
│    1. QEMU 将 VFIO 组的 fd 传递给 KVM                           │
│    2. KVM 获取 VFIO 组的引用                                     │
│    3. 当设备使用 Posted Interrupts 时，                           │
│       KVM 需要知道哪些 VFIO 组属于 VM                            │
│    4. KVM 更新 IRTE 以支持 PI                                    │
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
| `vfio_file_iommu_group()` | `vfio_main.c` | 获取文件的 IOMMU 组 |
| `vfio_dma_do_map()` | `vfio_iommu_type1.c` | DMA 映射操作 |
| `vfio_pin_pages_remote()` | `vfio_iommu_type1.c` | 固定用户页面 |
| `vfio_pci_core_enable()` | `vfio_pci_core.c` | 启用 PCI 设备直通 |
| `vfio_pci_mmap()` | `vfio_pci_core.c` | 映射设备 MMIO |
| `kvm_vfio_group_add()` | `virt/kvm/vfio.c` | 添加 VFIO 组到 KVM |
| `kvm_vfio_update_coherency()` | `virt/kvm/vfio.c` | 更新 DMA 一致性 |

---

## 🔬 实践练习

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
# 查看直通设备的中断分布
cat /proc/interrupts | grep vfio

# 分析 MSI-X 中断数量
# 通过 QEMU monitor:
info pci
# 查看设备的 MSI-X 配置

# 跟踪 VFIO 中断
echo vfio_irq_set >> /sys/kernel/debug/tracing/set_event
echo kvm_set_irq >> /sys/kernel/debug/tracing/set_event
```

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

**症状**：`vfio-pci`绑定失败

**原因**：IOMMU组包含多个设备，无法隔离

**解决**：
```bash
# 查看IOMMU组
ls /sys/kernel/iommu_groups/

# 如果多个设备在同一组，需要直通整个组
qemu-system-x86_64 ... \
  -device vfio-pci,host=0000:03:00.0 \
  -device vfio-pci,host=0000:03:00.1  # 同组的其他设备
```

### 陷阱2：ACS未启用

**场景**：设备间DMA隔离失败

**症状**：设备DMA访问到其他设备的内存

**原因**：ACS (Access Control Services)未启用

**解决**：
```bash
# 检查ACS
setpci -s 00:1c.0 ECAP_ACS+6.b

# 如果未启用，在GRUB中添加
echo "pci=noaer pcie_acs_override=downstream,multifunction" >> /etc/default/grub
update-grub && reboot
```

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

### 陷阱4：MSI-X表未正确配置

**场景**：直通设备中断无法到达Guest

**症状**：Guest设备无响应

**原因**：MSI-X表未正确配置

**解决**：
```bash
# 查看MSI-X表
cat /sys/bus/pci/devices/0000:03:00.0/msi_irqs

# 确保中断路由正确
cat /sys/kernel/debug/kvm/<vm_id>/irq_routing
```
