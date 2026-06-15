# KVM源码导航图

## 核心数据结构层次

```
┌─────────────────────────────────────────────────────────────────────┐
│ 用户空间 (QEMU/KVMTOOL)                                             │
│   ioctl(KVM_CREATE_VM)                                              │
│   ioctl(KVM_CREATE_VCPU)                                            │
│   ioctl(KVM_RUN)                                                    │
└─────────────────────┬───────────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────────────┐
│ include/linux/kvm_host.h                                            │
│   struct kvm              ← VM实例 (内存slot列表, vCPU列表, MMU)      │
│   struct kvm_vcpu         ← 虚拟CPU (寄存器, 中断, MMU context)       │
│   struct kvm_memory_slot  ← 内存区域 (GPA→PFN映射)                   │
└─────────────────────┬───────────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────────────┐
│ arch/x86/include/asm/kvm_host.h                                     │
│   struct kvm_x86_ops      ← x86回调表 (VMX/SVM实现)                  │
│   struct kvm_arch         ← x86架构扩展                              │
└─────────────────────┬───────────────────────────────────────────────┘
                      │
        ┌─────────────┼─────────────┐
        ▼             ▼             ▼
┌──────────┐  ┌──────────┐  ┌──────────────┐
│ VMX实现   │  │ MMU实现   │  │ 中断实现      │
│ vmx/     │  │ mmu/     │  │ lapic.c      │
│          │  │          │  │ posted_intr.c│
└──────────┘  └──────────┘  └──────────────┘
```

## 源码文件索引

### 核心框架层
```
virt/kvm/kvm_main.c          ← KVM框架核心 (VM/vCPU创建, ioctl处理)
include/linux/kvm_host.h     ← 核心数据结构定义
include/uapi/linux/kvm.h     ← 用户空间API (ioctl号定义)
```

### x86通用层
```
arch/x86/kvm/x86.c           ← x86通用KVM代码 (vcpu_run主循环, exit分发)
arch/x86/include/asm/kvm_host.h ← x86架构数据结构扩展
arch/x86/kvm/irq.c           ← IRQ路由基础
arch/x86/kvm/irq_comm.c      ← IRQ通信/投递
```

### Intel VMX层
```
arch/x86/kvm/vmx/vmx.c       ← ★ VMX主入口 (所有VMX操作实现)
arch/x86/kvm/vmx/main.c      ← KVM-x86回调注册 (vt_x86_ops, vt_init_ops)
arch/x86/kvm/vmx/vmx.h       ← vcpu_vmx结构定义 (VMCS, PI描述符)
arch/x86/kvm/vmx/vmcs.h      ← VMCS字段定义
arch/x86/kvm/vmx/vmx_ops.h   ← VMX指令封装 (vmread/vmwrite)
arch/x86/kvm/vmx/vmenter.S   ← 汇编VM-Entry/VM-Exit代码
arch/x86/kvm/vmx/posted_intr.c ← Posted Interrupts实现
arch/x86/kvm/vmx/posted_intr.h ← PI数据结构和辅助函数
arch/x86/kvm/vmx/nested.c    ← 嵌套虚拟化 (L2 guest支持)
```

### 内存虚拟化层
```
arch/x86/kvm/mmu/mmu.c       ← ★ MMU主逻辑 (page fault处理, MMU初始化)
arch/x86/kvm/mmu/tdp_mmu.c   ← ★ TDP MMU (EPT页表管理, 并发安全)
arch/x86/kvm/mmu/spte.c      ← SPTE位操作和初始化
arch/x86/kvm/mmu/spte.h      ← ★ SPTE格式定义 (关键!)
arch/x86/kvm/mmu/tdp_iter.c  ← EPT页表遍历迭代器
arch/x86/kvm/mmu/paging_tmpl.h ← 页表填充模板 (FNAME宏)
arch/x86/kvm/mmu/mmu_internal.h ← MMU内部接口
```

### 中断虚拟化 + VT-d中断重映射 (合并学习)
```
┌─ KVM 中断层 ─────────────────────────────────────────────┐
│ arch/x86/kvm/lapic.c         ← ★ 本地APIC实现 (vLAPIC)    │
│ arch/x86/kvm/lapic.h         ← vLAPIC数据结构和接口        │
│ arch/x86/kvm/ioapic.c        ← IOAPIC实现                 │
│ arch/x86/kvm/i8259.c         ← PIC实现 (兼容)              │
│ arch/x86/kvm/irq.c           ← IRQ基础路由 (kvm_set_irq)   │
│ arch/x86/kvm/irq_comm.c      ← IRQ通信和投递               │
│ arch/x86/include/asm/posted_intr.h ← ★ PI描述符硬件定义    │
└──────────────────────────┬────────────────────────────────┘
                           │ vmx_pi_update_irte() ← 桥梁函数
┌─ VT-d 中断重映射层 ──────┴───────────────────────────────┐
│ drivers/iommu/intel/irq_remapping.c                       │
│   ← ★ irq_2_iommu: IRQ→IOMMU映射                        │
│   ← ★ intel_ir_data: IRTE + MSI消息                     │
│   ← ★ prepare_irte() / modify_irte(): IRTE操作           │
│   ← ★ IRTE PI模式: PDA字段指向vCPU的pi_desc              │
│ drivers/iommu/intel/iommu.c  ← VT-d主实现                │
│ drivers/iommu/intel/dmar.c   ← DMAR表解析                │
│ drivers/iommu/intel/cache.c  ← IOTLB/IEC管理             │
└──────────────────────────────────────────────────────────┘
```

### VFIO设备直通层
```
drivers/vfio/vfio_main.c     ← VFIO主框架 (设备注册, ioctl)
drivers/vfio/container.c     ← Container管理 (IOMMU domain绑定)
drivers/vfio/vfio_iommu_type1.c ← Type1 IOMMU驱动 (DMA映射)
drivers/vfio/pci/vfio_pci_core.c ← PCI设备直通核心
drivers/vfio/pci/vfio_pci_intrs.c ← VFIO中断处理 (INTx/MSI/MSI-X)
virt/kvm/vfio.c              ← KVM-VFIO桥接 (一致性检测)
```

### Intel VT-d (IOMMU) — DMA重映射部分 (VFIO相关)
```
drivers/iommu/intel/iommu.c      ← ★ VT-d主实现 (DMA页表操作)
drivers/iommu/intel/dmar.c       ← DMAR表解析
drivers/iommu/intel/pasid.c      ← PASID支持 (ENQCMD/SVA)
drivers/iommu/intel/cache.c      ← IOTLB管理
```
注: 中断重映射部分见上方"中断虚拟化 + VT-d中断重映射"章节

## 关键函数调用链

### VM创建和运行
```
QEMU: ioctl(fd, KVM_CREATE_VM)
  → kvm_dev_ioctl_create_vm()     [kvm_main.c]
    → kvm_create_vm()             [kvm_main.c]
      → kvm_arch_init_vm()        [vmx/main.c → vmx_vm_init]
      → kvm_init_mmu()            [mmu/mmu.c]

QEMU: ioctl(vcpu_fd, KVM_RUN)
  → kvm_vcpu_ioctl()              [kvm_main.c]
    → kvm_arch_vcpu_ioctl_run()   [x86.c]
      → vcpu_run()                [x86.c]  ← 主循环
        → vcpu_enter_guest()      [x86.c]
          → kvm_x86_call(vcpu_run)[x86_ops.h]
            → vmx_vcpu_run()      [vmx/vmx.c]  ← ★ VM-Entry
              → vmx_vcpu_enter_exit()  [vmx/vmx.c]
                → __vmx_vcpu_run()    [vmenter.S] ← 汇编入口
                  → vmcs_writel(GUEST_RIP...)
                  → vmx_vmenter()     ← VMENTER指令!

VM-Exit发生:
  → vmx_vcpu_enter_exit() 返回
    → vmx_handle_exit()      [vmx/vmx.c]  ← ★ Exit分发
      → __vmx_handle_exit()  [vmx/vmx.c]
        → kvm_x86_exit_handlers_basic[exit_reason]()
```

### EPT页错误处理
```
VM-Exit: EPT_VIOLATION
  → vmx_handle_exit()
    → handle_ept_violation()   [vmx/vmx.c]
      → kvm_mmu_page_fault()   [mmu/mmu.c]
        → kvm_tdp_page_fault() [mmu/mmu.c]
          → kvm_tdp_mmu_map()  [mmu/tdp_mmu.c]  ← ★ EPT映射
            → tdp_mmu_iter_descend()
            → tdp_mmu_set_spTE_atomic()  ← 原子写入SPTE
```

### 中断注入路径
```
设备中断到达:
  → kvm_set_irq()              [irq.c]
    → kvm_irq_delivery_to_apic() [irq_comm.c]
      → apic->set_irq()        [lapic.c]
        → kvm_apic_set_irq()
        → kvm_lapic_set_irq()

APICv模式 (Posted Interrupt):
  → vmx_deliver_posted_interrupt()  [vmx/vmx.c]
    → pi_set_on(pi_desc)      ← 设置PI.ON位
    → pi_set_pir(vector)      ← 设置PIR[vector]
    → apic_send_IPI()         ← 发送通知中断 (NV向量)

vCPU调度时同步PIR→IRR:
  → vmx_sync_pir_to_irr()     [vmx/vmx.c]
    → pi_test_on(pi_desc)
    → pi_clear_on(pi_desc)
    → kvm_apic_update_irr(pi_desc.pir)  ← PIR拷贝到IRR
    → vmx_set_rvi(max_irr)    ← 更新RVI触发VID
```

### VFIO设备直通
```
QEMU: ioctl(vfio_fd, VFIO_GROUP_SET_CONTAINER)
  → vfio_group_set_container()  [vfio/group.c]
    → iommu_domain_alloc()
    → vfio_iommu_type1_attach_group()  [vfio_iommu_type1.c]

QEMU: ioctl(vfio_fd, VFIO_IOMMU_MAP_DMA)
  → vfio_dma_do_map()           [vfio_iommu_type1.c]
    → vfio_pin_pages_remote()   ← 固定用户页面
    → iommu_map()               ← IOMMU页表映射
      → intel_iommu_map()       [intel/iommu.c]
        → dma_pte_*()           ← 操作VT-d页表项

设备DMA:
  → 设备发出DMA请求 (IOVA)
  → VT-d硬件遍历Context Entry + DMA Page Table
  → 转换为HPA, 完成DMA
```

## 模块参数速查

### kvm_intel 关键参数
```
ept=1           ← 启用EPT (Extended Page Tables)
eptad=1         ← 启用EPT A/D位
vpid=1          ← 启用VPID (Virtual Processor ID)
enable_apicv=1  ← 启用APIC虚拟化
enable_ipiv=1   ← 启用IPI虚拟化
nested=1        ← 启用嵌套虚拟化
pml=1           ← 启用脏页日志 (Page Modification Logging)
ple_gap/ple_window ← PLE窗口参数
```

### kvm 关键参数
```
halt_poll_ns=400000    ← HLT轮询时间(ns)
nx_huge_pages=1        ← NX大页缓解
mmio_caching=1         ← MMIO缓存
```
