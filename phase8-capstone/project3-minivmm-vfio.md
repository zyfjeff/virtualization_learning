# 项目 3：VFIO 设备直通

> 目标：把一块真实 PCI 设备（网卡或 NVMe）直通进项目 2 的 VMM，
> guest 内用原生驱动驱动它。

**前置**：项目 2；**强烈建议先完成阅读**：
`../phase3-iommu/`（group/domain/ACS 判定，phase6 的地基）、
`../phase6-vfio/README.md`（VFIO 全栈：§1.4 group 划分、§1.5 ACS/ATS、
DMA 映射、MSI-X 直通流程）。硬件要求：启用 `intel_iommu=on`，
设备位于**可独立直通的 IOMMU group**。

---

## M0：选设备与宿主侧准备

1. **确认 group 可直通**：按 `../phase6-vfio/README.md` §1.4 的方法
   （`/sys/kernel/iommu_groups/` + `pci_device_group()` 四步判定）确认
   目标设备独占一个 group，或同 group 设备都能一起直通。
   `../phase3-iommu/group.md` 解释判定依据（ACS、quirk、RCiEP）。
2. **解绑原驱动、绑定 `vfio-pci`**（`scripts/` 与
   `../phase6-vfio/practice/` 里有现成的认领/跟踪实验可复用）。
3. 记录设备 BDF、BAR 布局（`lspci -vv`）、是否支持 MSI/MSI-X。

## M1：VFIO fd 层级

VFIO 的用户态接口（`include/uapi/linux/vfio.h`）分三层：

```
/dev/vfio/vfio   → container fd: VFIO_GET_API_VERSION / VFIO_CHECK_EXTENSION
                   / VFIO_SET_IOMMU(VFIO_TYPE1_IOMMU)
/dev/vfio/<grp>  → group fd:     VFIO_GROUP_GET_DEVICE_FD(bdf)
                 → device fd:   一切设备操作
```

（完整序列见 `../phase6-vfio/README.md` 的架构总览一节。）
注意：**container 在打开任何 group 之前设好 IOMMU 类型**；
group 加入 container 后才能拿到 device fd。

## M2：DMA 映射（guest RAM → IOMMU）

- `VFIO_IOMMU_MAP_DMA`：把项目 1 的 guest RAM 以 **IOVA = GPA** 的
  1:1 方式映射进 IOMMU，设备 DMA 地址即 guest 物理地址。
- 对照 `../phase3-iommu/translation.md`：这一步建立的是
  二级翻译（IOVA→HPA），直通设备与 EPT 走的是两套独立机制。
- 陷阱：映射必须覆盖所有 guest RAM region；`vaddr` 是宿主虚拟地址，
  注意与 `KVM_SET_USER_MEMORY_REGION` 用的是同一段内存。

## M3：让 guest 看见设备（本项目的硬骨头）

直通只解决数据面；**guest 首先得枚举到这块设备**。最小方案：

1. VMM 模拟一个最小 PCI 根复合体：实现配置空间访问通道
   （传统 `0xcf8`/`0xcfc` PIO，或 ECAM MMIO），在某个 BDF 上
   暴露直通设备。
2. 配置空间读写转发：`VFIO_DEVICE_READ/WRITE` 作用于
   `VFIO_PCI_CONFIG_REGION`（region 0）；写命令寄存器
   （Bus Master / Memory Space Enable）必须真正落到设备。
3. BAR：`VFIO_DEVICE_GET_REGION_INFO` 枚举各 BAR region；
   `mmap()` region fd 后可把 BAR 直接作为
   `KVM_SET_USER_MEMORY_REGION` 挂进 guest（guest 对 BAR 的普通
   访问不再退出），不可 mmap 的区域仍走 `KVM_EXIT_MMIO` 转发。
   对照 `../phase6-vfio/README.md` 中 BAR mmap 与剥页的讨论。

## M4：中断（MSI/MSI-X）

复用 `../phase6-vfio/README.md` 已核实的流程（QEMU 始终在中间协调）：

1. guest 写 MSI-X 表 → VMM 在自己模拟的 BAR 里拦截（对照
   `../phase6-vfio/README.md` 中 `msix_table_mmio_write` 流程）
2. VMM 调 `KVM_SET_GSI_ROUTING`（case @ `virt/kvm/kvm_main.c:5311`）
   建立 MSI 路由（`gsi → 地址/数据`）
3. VMM 调 `VFIO_DEVICE_SET_IRQS`（`VFIO_PCI_MSIX_IRQ_INDEX`），
   传入一组 eventfd；中断触发时 VFIO 写 eventfd
4. 用 `KVM_IRQFD`（case @ `kvm_main.c:5257`）把同一组 eventfd 绑到
   路由 —— eventfd 成为 VFIO 与 KVM 之间的桥（两者注册**无先后
   要求**，见 `../phase6-vfio/README.md` 的相应讨论）
5. 中断重映射（IRTE）由 VFIO 内核驱动在 `intel/irq_remapping.c`
   侧自动建立；若走 Posted 模式，`intel_ir_set_vcpu_affinity()` 会把
   IRTE 指向 vCPU 的 PI Descriptor（`../phase3-iommu/interrupts.md`
   与 `../phase4-interrupts/` 的 PI 章节）。**先跑通 Remapped 模式
   再谈 Posted**。

---

## 已知陷阱

1. **group 不独占**：多设备 group 必须全部直通，否则
   `VFIO_GROUP_GET_DEVICE_FD` 之后 DMA 隔离不成立（§1.4）。
2. **忘记 Bus Master**：配置空间转发漏写 Command 寄存器，
   设备"活着但不出活"（无 DMA、无中断）。
3. **MSI-X 表的 PBA 与 table 页**：拦截范围要覆盖整个
   table BIR 对应的页，写错掩码位会导致中断风暴或丢中断。
4. **中断路由冲突**：`KVM_SET_GSI_ROUTING` 的 gsi 号不能与
   IOAPIC 默认路由重叠，直通设备用高位 gsi（如 24+）最省事。

---

## 验收标准

- [ ] guest `lspci -vv` 看到真实设备；原生驱动加载（网卡：链路通；
      NVMe：`fio` 读写正常）
- [ ] 能画出一次设备中断从硬件到 guest 的完整路径
      （设备 → MSI-X 写 → IOMMU 中断重映射 → LAPIC → vCPU），
      并说明 Remapped 与 Posted 模式各自在哪一段不同
- [ ] 与项目 2 的 virtio 路径对比：相同负载下的吞吐/延迟差异，
      用宿主侧 `perf`/ftrace 解释（方法见项目 4）

---

## 参考资料

- `../phase6-vfio/README.md` + `annotations.md` + `practice/`
- `../phase3-iommu/`（group.md / translation.md / interrupts.md）
- VFIO 内核实现：`/root/code/linux-6.12.93/drivers/vfio/`
- QEMU 对照：`/root/code/qemu-10.1.0-rc2/hw/vfio/pci.c`
