1. CR0 Shadowing
2. VPID (Virtual Processor ID)
3. CPUID Faulting
4. Page Modification Logging (脏页)
5. MSR Bitmap
6. EPTP Switching
7. TPR Shadow
8. 硬件A/D PML(Page Modification Logging)
9. MTRR（Memory Type Range Registers）
10. VMX_EPT_IPAT_BIT (bit 6) = Ignore PAT

  ┌─ EPT 性能开销 ────────────────────────────────────────┐
  │                                                         │
  │  不使用大页的情况:                                      │
  │    Guest 页表遍历: 4 次内存访问 (4 级页表)            │
  │    EPT 页表遍历:   4 次内存访问 (4 级 EPT)            │
  │    总计: 最多 4 × 4 = 16 次内存访问！                  │
  │                                                         │
  │  这就是为什么 EPT 需要硬件优化:                         │
  │    - EPT TLB 缓存翻译结果                              │
  │    - 大页映射减少页表层级                              │
  │    - EPT A/D 位减少 VM-Exit                            │
  │                                                         │
  └─────────────────────────────────────────────────────────┘


  VMCS 中的 EPTP 字段指向 EPT 根页面：

    63    52  51  48 47   12 11  7 6   3 2 0
   ┌──────┬─────┬─────┬──────┬─────┬───┬───┐
   │ 预留 │ WBT │ 预留 │ GPA  │ 预留 │Ad │ 4 │
   │      │     │     │ 偏移 │     │/D │ 级 │
   └──────┴─────┴─────┴──────┴─────┴───┴───┘
                                          │
                                          │
                                          └─ 0=4级, 1=5级
                                     Ad/D ── 1=启用 A/D 位

  关键字段：
  - GPA 偏移 (bit 12-47): EPT 根页面的物理地址（必须 4KB 对齐）
  - WBT (bit 51-48): 内存类型（Write-Back = 6）
  - Ad/D (bit 6): 是否启用 Accessed/Dirty 位跟踪
  - 4 级 (bit 2-0): EPT 页表层级（0 = 4 级，1 = 5 级）


  ┌─ KVM SPTE 64 位完整结构 ─────────────────────────────────┐
  │                                                           │
  │  63  62  61-12  11  10  9  8  7  6  5  4  3  2  1  0    │
  │ ┌───┬───┬─────┬───┬──┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┬─┐          │
  │ │ NX│ A │ PFN │ G │IG│P│P│L│D│A│C│U│W│R│P│ │          │
  │ │   │ / │     │ │ │ │ │ │ │ │ │ │ │ │ │ │ ││          │
  │ │   │ D │     │ │ │C│S│ │ │ │ │ │ │ │ │ │ ││          │
  │ │   │   │     │ │ │ │ │ │ │ │ │ │ │ │ │ │ ││          │
  │ └───┴───┴─────┴───┴──┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┘          │
  │  │   │   │     │   │  │ │ │ │ │ │ │ │ │ │ ││          │
  │  │   │   │     │   │  │ │ │ │ │ │ │ │ │ │ │└─ R: Read │
  │  │   │   │     │   │  │ │ │ │ │ │ │ │ │ │ └── W: Write│
  │  │   │   │     │   │  │ │ │ │ │ │ │ │ │ └──── P: Present│
  │  │   │   │     │   │  │ │ │ │ │ │ │ │ └────── U: User │
  │  │   │   │     │   │  │ │ │ │ │ │ │ └──────── W: Writable│
  │  │   │   │     │   │  │ │ │ │ │ │ └────────── R: Readable│
  │  │   │   │     │   │  │ │ │ │ │ └──────────── C: Cache │
  │  │   │   │     │   │  │ │ │ │ └────────────── A: Accessed│
  │  │   │   │     │   │  │ │ │ └──────────────── D: Dirty │
  │  │   │   │     │   │  │ │ └────────────────── L: Large │
  │  │   │   │     │   │  │ └──────────────────── P: Present│
  │  │   │   │     │   │  └────────────────────── IG: Ignore│
  │  │   │   │     │   └───────────────────────── G: Global│
  │  │   │   │     └───────────────────────────── PFN (40位)│
  │  │   │   └─────────────────────────────────── A/D (硬件)│
  │  │   └─────────────────────────────────────── NX (No-X) │
  │  └─────────────────────────────────────────── 软件位    │
  │                                                           │
  └───────────────────────────────────────────────────────────┘

- 透传指令的开销约 10 ns
- VM-Exit 的开销约 1500 ns（~1.5 μs）
- L1 Cache:  ~1 ns    (最快)
- L2 Cache:  ~4 ns 
- L3 Cache:  ~10 ns 
- 内存:      ~100 ns  (最慢)
- EPT Violation 需要 2-5 μs



快速路径（fast_page_fault）的使用条件：
- ✅ SPTE 已经存在（页面已映射）
- ✅ 只需要修改权限位（例如：脏页跟踪时恢复写权限）
- ❌ 不需要创建新的页表项



## 中断

1. PPR、TPR、EOI、ISR、IRR
2. VID：Virtual Interrupt Delivery， vmx_set_rvi
  VMCS.GUEST_INTR_STATUS (16位):
    ┌──────────────┬──────────────┐
    │ SVI [15:8]   │ RVI [7:0]    │
    │ Servicing    │ Requested    │
    │ Virtual      │ Virtual      │
    │ Interrupt    │ Interrupt    │
    └──────────────┴──────────────┘

    SVI = 当前正在服务的最高优先级 vector (硬件维护)
    RVI = 待投递的最高优先级 vector (KVM 软件写入!)

```
  /* 来源: arch/x86/kvm/vmx/vmx.c:6912 */

  int vmx_sync_pir_to_irr(struct kvm_vcpu *vcpu)
  {
      struct vcpu_vmx *vmx = to_vmx(vcpu);
      int max_irr;
      bool got_posted_interrupt;

      if (pi_test_on(&vmx->pi_desc)) {
          // ① PI 的 ON=1 → 有 Posted Interrupt 到达
          pi_clear_on(&vmx->pi_desc);
          smp_mb__after_atomic();   // 内存屏障: IOMMU 可能并发写 PIR

          // ② 把 PI 的 PIR 拷贝到 vLAPIC IRR
          got_posted_interrupt =
              kvm_apic_update_irr(vcpu, vmx->pi_desc.pir, &max_irr);
      } else {
          // ③ 没有 PI → 直接从 vLAPIC 找最高优先级 IRR
          max_irr = kvm_lapic_find_highest_irr(vcpu);
          got_posted_interrupt = false;
      }

      // ④ ★ VID 的关键：写 RVI 到 VMCS
      if (!is_guest_mode(vcpu) && kvm_vcpu_apicv_active(vcpu))
          vmx_set_rvi(max_irr);       // 告诉硬件 "最高优先级待投递向量"
      else if (got_posted_interrupt)
          kvm_make_request(KVM_REQ_EVENT, vcpu);  // 非 APICv 走传统路径

      return max_irr;
  }
```

* eoi bitmap的作用，配合水平触发，记录要vm exit的中断。
* auto eoi、eoi virtualization
* remote_irr 配合水平触发