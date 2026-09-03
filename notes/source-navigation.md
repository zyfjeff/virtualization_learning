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

★ 本页只列**开机时能传什么**。逐个参数的默认值、运行时权限（多数是 `0444` 只读，
`echo` 会失败）、能不能在一轮实验里连续扫 —— 权威表只有一份，在
[`../phase9-performance/parameters.md`](../phase9-performance/parameters.md)。

### kvm_intel 关键参数
```
ept=1           ← 启用EPT (Extended Page Tables)
eptad=1         ← 启用EPT A/D位
vpid=1          ← 启用VPID (Virtual Processor ID)
enable_apicv=1  ← 启用APIC虚拟化
enable_ipiv=1   ← 启用IPI虚拟化
nested=1        ← 启用嵌套虚拟化
pml=1           ← 启用脏页日志 (Page Modification Logging)
ple_gap/ple_window ← PLE窗口参数（★ 开机后全只读，运行时改不了）
```

### kvm 关键参数
```
halt_poll_ns=200000     ← HLT轮询窗口上限(ns)，KVM_HALT_POLL_NS_DEFAULT
halt_poll_ns_grow=2    ← 增长倍数
halt_poll_ns_grow_start=10000 ← 增长起始值(10μs)
halt_poll_ns_shrink=2  ← 缩小除数（★ 0 表示"一次失手就归零"）
nx_huge_pages=auto     ← NX大页缓解；★ 不是布尔，只收 off/force/auto/never
                         （arch/x86/kvm/mmu/mmu.c:87 module_param_cb；解析在 :7259，
                          四个字符串分支 :7268-7284，最后才 `kstrtobool` 兜底）
mmio_caching=1         ← MMIO缓存（`0444` 只读，arch/x86/kvm/mmu/spte.c:24）
```

---

## ★ Phase 9: 性能优化相关源码

★ 本节只留"去哪找符号"的坐标。机制走读各有归属：halt-polling 在
[`../phase0-kvm-framework/annotations.md`](../phase0-kvm-framework/annotations.md) §9，
PLE / PML 与脏页 / 主时钟三块在
[`../phase9-performance/annotations.md`](../phase9-performance/annotations.md) §1/§2/§3，
参数表在 [`../phase9-performance/parameters.md`](../phase9-performance/parameters.md)，
测量规范在 [`../phase9-performance/measurement.md`](../phase9-performance/measurement.md)。

### halt-polling
```
virt/kvm/kvm_main.c:78-97    ← halt_poll_ns 模块参数定义
virt/kvm/kvm_main.c:3670-3706 ← grow_halt_poll_ns() / shrink_halt_poll_ns()
virt/kvm/kvm_main.c:3811-3882 ← kvm_vcpu_halt() (含自适应算法)
include/trace/events/kvm.h    ← kvm_halt_poll_ns trace event
```

### PLE (Pause Loop Exiting)
```
arch/x86/kvm/vmx/vmx.c:194-219 ← ple_* 模块参数定义
arch/x86/kvm/vmx/vmx.c:1417   ← grow_ple_window()
arch/x86/kvm/vmx/vmx.c:5911-5924 ← handle_pause() (PLE 处理)
virt/kvm/kvm_main.c:4037-4099 ← kvm_vcpu_on_spin() (自旋锁检测)
arch/x86/kvm/trace.h          ← kvm_ple_window_update trace event
```

### PML (Page Modification Logging)
```
arch/x86/kvm/vmx/vmx.c:127-128 ← enable_pml 参数
arch/x86/kvm/vmx/vmx.h:336    ← PML_ENTITY_NUM (512)
arch/x86/kvm/vmx/vmx.c:4656   ← PML 检查逻辑
arch/x86/kvm/trace.h          ← kvm_pml_full trace event
```

### TSC 同步
```
arch/x86/kvm/x86.c:2670-2783  ← __kvm_synchronize_tsc() / kvm_synchronize_tsc()
arch/x86/kvm/vmx/vmx.c:1951-1960 ← vmx_write_tsc_offset() / vmx_write_tsc_multiplier()
                                 （★ 只把 arch.tsc_offset / arch.tsc_scaling_ratio
                                  写进 VMCS 两个字段，偏移的**计算**不在这里）
arch/x86/kvm/trace.h          ← kvm_track_tsc, kvm_write_tsc_offset trace events
```

---

## ★ Phase 10: 调试与测试相关源码

### Trace Events 定义位置
```
arch/x86/kvm/trace.h           ← x86 特定 KVM trace events (70+ 个)
include/trace/events/kvm.h     ← 通用 KVM trace events (20+ 个)
include/trace/events/irq.h     ← 中断相关 trace events
```

### debugfs 接口
```
virt/kvm/kvm_main.c            ← KVM 通用 debugfs 接口
arch/x86/kvm/debugfs.c         ← x86 特定 debugfs 接口
  - guest_mode                 ← vCPU 是否在 guest 模式
  - tsc-offset                 ← 当前 TSC 偏移
  - lapic_timer_advance_ns     ← LAPIC 定时器提前量
  - mmu_rmaps_stat             ← MMU rmap 统计
```

### KVM Selftests
```
tools/testing/selftests/kvm/   ← 完整测试框架目录
  dirty_log_test.c             ← 脏页日志正确性测试
  dirty_log_perf_test.c        ← ★ 脏页日志性能测试
  demand_paging_test.c         ← ★ 按需分页性能测试
  guest_memfd_test.c           ← ★ guest_memfd 测试 (6.12)
  memslot_perf_test.c          ← memslot 操作性能测试
  access_tracking_perf_test.c  ← 访问跟踪性能测试
  kvm_page_table_test.c        ← KVM 页表测试
  include/kvm_util.h           ← 测试工具库头文件
  lib/kvm_util.c               ← KVM 操作封装
```

---

## ★ Phase 11: MicroVM 相关源码

### VM 创建启动路径
```
virt/kvm/kvm_main.c:5492-5533 ← kvm_dev_ioctl_create_vm()
virt/kvm/kvm_main.c:1146-1265 ← kvm_create_vm()
virt/kvm/kvm_main.c:4217      ← kvm_vm_ioctl_create_vcpu()
virt/kvm/kvm_main.c:2124      ← kvm_vm_ioctl_set_memory_region()
arch/x86/kvm/x86.c:11579-11697 ← kvm_arch_vcpu_ioctl_run() (首次 VM-Entry)
```

### guest_memfd (6.12 新增)
```
virt/kvm/guest_memfd.c         ← ★ guest_memfd 完整实现
  kvm_gmem_create()            ← KVM_CREATE_GUEST_MEMFD 实现
  kvm_gmem_populate()          ← 内存填充回调
virt/kvm/kvm_mm.h:40           ← kvm_gmem_create() 声明
arch/x86/kvm/Kconfig:88        ← KVM_PRIVATE_MEM 配置选项
```

### 嵌套虚拟化 (MicroVM 参考)
```
arch/x86/kvm/vmx/nested.c      ← ★ L1/L2 嵌套虚拟化
arch/x86/kvm/vmx/nested.h      ← 嵌套虚拟化数据结构
arch/x86/kvm/trace.h           ← kvm_nested_vmenter, kvm_nested_vmexit 等 trace events
```

### VFIO 安全模型
```
drivers/vfio/vfio_main.c       ← VFIO 主框架
drivers/vfio/pci/vfio_pci_core.c ← PCI 设备直通
  vfio_pci_core_enable()       ← 设备启用 (line 500)
virt/kvm/vfio.c:296-360        ← KVM-VFIO 桥接 (kvm_vfio_set_attr)
virt/kvm/vfio.c:352            ← kvm_vfio_ops 注册
```
