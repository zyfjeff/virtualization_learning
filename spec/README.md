# spec/ — 硬件与协议规范

> 事实核查的规范依据（AGENTS.md「事实核查」流程：查源码 → 查规范 → 交叉验证）。
> 引用格式：`<规范名>, Section X.Y (标题), Figure/Table N`。 prose 引用用裸文件名即可，
> 文件路径在本目录。提取文本：`pdftotext spec/<name>.pdf /tmp/<name>.txt`。

| 文件 | 规范 | 版本 | 覆盖 | 主要使用 phase |
|------|------|------|------|---------------|
| [`intel-vmx.pdf`](intel-vmx.pdf) | Intel VMX / SDM Vol.3 | 6.12.93 配套 | VMCS、VM-Exit/Entry、EPT/VPID、Posted Interrupt（§30.6） | phase1/2/5 |
| [`intel-vtd.pdf`](intel-vtd.pdf) | Intel VT-d | Rev 4.1 (D51397-016) | IOMMU、中断重映射、IRTE/PI Descriptor（§9.9/9.10/9.11） | phase3/5/7 |
| [`virtio-v1.3-csd01.pdf`](virtio-v1.3-csd01.pdf) | Virtio | 1.3 CSD01 | virtqueue、设备类型、feature bits | phase6/9 |
| [`amd-virtualization.pdf`](amd-virtualization.pdf) | AMD-Vi / AMD 虚拟化 | — | AMD-Vi IOMMU、AVIC、IRTE 格式、GA Log | phase3/5 |
| [`arm-smmuv3.pdf`](arm-smmuv3.pdf) | ARM SMMUv3 | — | Stage-1/2 翻译、STE/CD 格式、CMDQ 命令 | phase3 |
| [`pcie-base-spec-r6.0.pdf`](pcie-base-spec-r6.0.pdf) | PCIe Base | 6.0 | TLP/DLLP、ACS/ATS/AER、MSI/MSI-X、配置空间 | phase4/7 |

> ⚠️ AMD-Vi 规范缺失说明见 phase3 README「规范来源说明」：AMD 相关只陈述代码事实并标注。
> 规范与源码不一致时**以规范为准**（AGENTS.md 核心原则：硬件优化可能在软件里看不见）。
