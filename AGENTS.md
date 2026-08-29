# AGENTS.md — KVM 深度学习项目的 AI 协作指南

> 面向任意编码/写作 Agent，是本项目 AI 协作规范的唯一来源（`CLAUDE.md` 已指向此文件）。

## 项目定位

面向 **有用户态 VMM 经验（QEMU/crosvm）的读者**，基于 Linux 6.12.93 源码深入 KVM 内核态实现。产出物主要是**技术文档 + 可运行示例**，不是产品代码。因此**准确性 > 产出速度**。

```
phase0-kvm-framework/   KVM 框架层
phase1-vtx-basics/      VT-x + CPU 虚拟化
phase2-mem-virt/        内存虚拟化 (EPT/TDP MMU)
phase3-interrupts/      中断虚拟化 + VT-d IR
phase4-virtio/          virtio / vhost / vhost-user
phase5-vfio/            VFIO 设备直通
phase6-timer-virt/      时钟虚拟化
phase7-projects/        综合实践
phase8-performance/     性能优化
phase9-debugging/       调试与测试
phase10-microvm/        MicroVM 架构
examples/  notes/  scripts/  shared/
```

各 phase 下的 `practice/` 存放实践练习。实验 VM 环境在 `scripts/`：

| 目录 | 用途 |
|---|---|
| `scripts/vm/` | 构建内核与 rootfs、启动实验 VM（`boot-vm.sh` 默认启用 KVM） |
| `scripts/trace/` | 宿主侧观测脚本（ftrace + perf） |
| `scripts/images/` | 构建产物，已 gitignore |
| `scripts/shared/` | 9p 共享暂存区 → guest `/mnt/shared` |
| `scripts/archive/` | 已弃用脚本与历史文档，不要用于新实验 |

入口文档：`scripts/README.md`。

## 强制要求：事实核查

**任何技术结论都必须有源码或规范支撑，禁止凭记忆或推断作答。**

参考资料（路径均已验证存在）：

| 资料 | 路径 | 用途 |
|---|---|---|
| Linux 内核源码 | `/root/code/linux-6.12.93/` | 实现、数据结构、调用流程 |
| QEMU 源码（基线） | `/root/code/qemu-10.1.0-rc2/` | VMM 侧实现、设备模拟、KVM API 调用 |
| QEMU 源码（新版） | `/root/code/qemu-11.1.0/` | vhost-user 新特性等需要较新行为的核查 |
| DPDK 源码 | `/root/code/dpdk/` | vhost-user 后端实现、用户态数据面 |
| Intel VMX 规范 | `intel-vmx.pdf` | VMCS、VM-Exit/Entry、EPT/VPID、Posted Interrupt |
| Intel VT-d 规范 | `intel-vtd.pdf` | IOMMU、中断重映射、IRTE/PI Descriptor |
| Virtio 规范 | `virtio-v1.3-csd01.pdf` | virtqueue、设备类型、feature bits |

已有文档大多以 QEMU 10.1.0-rc2 为基线；phase4 的 `vhost-user-new-features-factcheck-v2.md` 与 `-usecases.md` 基于 11.1.0 + DPDK。**引用时必须写明版本**，两个版本行为不一致时说明差异。

核查流程：

1. **查源码** — grep 函数/结构定义，读实际代码，确认实现细节。
2. **查规范** — `pdftotext` 提取后 grep 章节，确认硬件行为。
3. **交叉验证** — 源码 vs 规范 vs 已有文档，三者必须一致。
4. **标注来源** — 文件路径 + 行号；规范章节号 + 图表号。差异要解释原因。

### 核心原则：硬件优化可能在软件里看不见

**代码只是硬件行为的一个子集。以规范为准，代码为辅。**

典型案例 —— Posted Interrupts 的零 VM-Exit：只看 KVM 的 `handle_external_interrupt_irqoff` 会误以为通知中断总是触发 VM-Exit。但 Intel SDM Section 30.6 明确：当 "process posted interrupts" = 1 且外部中断向量等于 posted-interrupt notification vector 时，硬件以不可中断的方式自动完成「清 ON → PIR→VIRR → 更新 RVI → 评估中断」，**不产生 VM-Exit**，KVM 代码根本不执行。那段 KVM 代码只处理向量不匹配、PI 未启用、嵌套虚拟化、vCPU 迁移等旁路情况。

## 已知陷阱（不要重犯）

1. **IRTE 模式选择位与 DM/DLM** — 区分 Remapped / Posted 的是 `IM` (IRTE Mode) @ bit 15，两种格式都有该位：Remapped 是 `IM=0`，Posted 是 `IM=1`。**不要写成 `DM=0`/`DM=1`**。Remapped 格式里 `DM` (Destination Mode) @ bit 2 与 `DLM` (Delivery Mode) @ bits 7:5 是两个不同字段，别把 DLM 叫成 DM。（VT-d Spec 9.9 / Figure 9-9 与 9.10 / Figure 9-10）Linux 源码里这个位**不叫 `IM`**，叫 `pst`（Posted 联合体里是 `p_pst`），见 `include/linux/dmar.h:212` 与 `:240` —— 搜不到 `IM` 不代表内核用了别的位。
2. **向量字段命名** — IRTE 里是 `VV` (Virtual Vector) @ bits 23:16；PI Descriptor 里才是 `NV` (Notification Vector) @ bits 279:272。值相同，命名不同。（VT-d Spec 9.10 vs 9.11）
3. **向量空间** — 不存在"全局共享 256 个 vector"。每 CPU 独立 LAPIC、各约 230 个可用 vector，唯一性是 `(CPU_ID, vector)`。
4. **PDA 位范围** — `PDAL` bits 63:38（对应地址 bits 31:6），`PDAH` bits 127:96（对应地址 bits 63:32），共 58 位，64 字节对齐。（VT-d Spec 9.10）
5. **VFIO MSI-X 直通流程** — QEMU 始终在中间协调：Guest 写 MSI-X 表 → QEMU 拦截 (`msix_table_mmio_write`) → `KVM_SET_GSI_ROUTING` + `VFIO_DEVICE_SET_IRQS` → VFIO 内核驱动设置 IRTE。参考 `hw/pci/msix.c:221`、`hw/vfio/pci.c:487`、`accel/kvm/kvm-all.c:2198`、`drivers/iommu/intel/irq_remapping.c:1352`。
6. **PI 零 VM-Exit** — 见上文核心原则，正常 PI 路径 0 次 VM-Exit、延迟 <1μs。关键词 "without transferring control to the VMM"。（SDM 30.6、VT-d Spec 5.2.5）
7. **启动 VM 必须显式传 `-enable-kvm -cpu host`** — 缺 `-enable-kvm` 时 QEMU **静默**回退 TCG 纯软件模拟，不报任何错，但宿主侧 `kvm:kvm_exit` / `kvm:kvm_entry` 等 tracepoint 零事件，所有 KVM 追踪实验都会得出"没有 VM-Exit"的错误结论；缺 `-cpu host` 则 guest 内看不到 VMX（`VMX: 0 CPUs with VMX support`）。写文档给出 qemu 命令时不要漏掉这两个参数。判断某次运行是否真的走了 KVM：数 `/proc/<qemu-pid>/fd` 里指向 `/dev/kvm` 的引用，走 KVM 时 >0，TCG 时为 0。`scripts/vm/boot-vm.sh` 已默认带上并会在启动前自检。
8. **`CONFIG_X86_POSTED_MSI` 不是 KVM 的 PI** — 内核里有两条都叫 "posted" 的路径，别混：`CONFIG_X86_POSTED_MSI`（6.11+）是**宿主自己**的 MSI 合并优化，与 Guest 无关，在 alloc 阶段由 `posted_msi_supported()` 门控走 `prepare_irte_posted()`（`drivers/iommu/intel/irq_remapping.c:1111`、`:1377`），PDA 指向宿主 per-CPU PI Descriptor；KVM 的 Guest VT-d PI 走 `intel_ir_set_vcpu_affinity()`（`:1248`），PDA 指向 **vCPU 的** PI Descriptor。两者在 `struct irq_2_iommu` 里是**两个独立标志** `posted_msi` 与 `posted_vcpu`（`:48-49`）。写文档时必须说清是哪一条。
9. **对 `.isra` / `.constprop` / `.part` 符号下 kprobe，取参不可信** — 这些后缀表示编译器改过函数签名（`.isra` = 参数被标量化替换），**寄存器与源码形参的对应关系不能照原型推断**，而且可能存在多个同名符号（实测 `modify_irte.isra.0` 在本机有两个地址）。必须先在**已知答案的场景**上做对照验证再采信，例如抓 IRTE 时先检查 `SID` 字段是否等于设备 BDF。另外清理 ftrace 状态时 `kprobe_events` 与 `current_tracer` / `set_ftrace_filter` 是**独立**的，只清前者会让下一轮实验被上一轮残留的 `function` tracer 淹没。

## 文档规范

引用格式：

```markdown
**源码引用**: `arch/x86/kvm/vmx/vmx.c:6912` → `vmx_sync_pir_to_irr()`
**规范引用**: intel-vtd.pdf, Section 9.10 (IRTE for Posted Interrupts), Figure 9-10
```

术语一律采用 Intel 规范写法：

- ✅ Posted 模式 / Remapped 模式 / IRTE Mode (IM) / Virtual Vector (VV) / Handle / Interrupt Format
- ❌ PI 模式（应为 Posted 模式）/ 在 Posted 上下文用 DM / 在 IRTE 上下文用 NV / "IRTE 索引" / "dmar_format"

代码示例必须摘自实际源码，并在首行注明来源：

```c
/* 来源: arch/x86/kvm/vmx/vmx.c:6912 */
int vmx_sync_pir_to_irr(struct kvm_vcpu *vcpu)
{
    ...
}
```

发现已有文档有错：在对应 phase 目录写 `corrections.md`，说明错误、给出正确信息与引用，并同步修正原文。

## 常用命令

```bash
# 内核源码
grep -rn "function_name" /root/code/linux-6.12.93/
cd /root/code/linux-6.12.93 && git grep "struct kvm_vcpu {"

# QEMU 源码
grep -rn "VFIO_DEVICE_SET_IRQS" /root/code/qemu-10.1.0-rc2/hw/vfio/
grep -rn "KVM_SET_GSI_ROUTING" /root/code/qemu-10.1.0-rc2/accel/kvm/

# 规范 PDF（poppler-utils）
pdftotext intel-vmx.pdf /tmp/vmx-spec.txt && grep -n "Posted-Interrupt Processing" /tmp/vmx-spec.txt
pdftotext intel-vtd.pdf /tmp/vtd-spec.txt
pdftotext virtio-v1.3-csd01.pdf /tmp/virtio-spec.txt

# 实验 VM 环境（详见 scripts/README.md）
cd scripts/vm
./build-kernel.sh
sudo ./build-rootfs-ubuntu.sh
./boot-vm.sh ubuntu --memory 4G --cpus 4 --queues 4   # 默认 -enable-kvm -cpu host
./boot-vm.sh ubuntu --tcg                             # 显式回退纯软件模拟

# 确认某次运行真的走了 KVM（0 表示走的是 TCG）
ls -l /proc/$(pgrep -f '^qemu-system-x86_64')/fd | grep -c kvm

# 观测脚本（宿主侧，ftrace + perf）
ls scripts/trace/   # trace-vmexit.sh, kvm-overview.sh 等
```

## 提交前检查清单

- [ ] 查过 Linux 内核源码
- [ ] 涉及 VMM 侧时查过 QEMU 源码
- [ ] 查过对应规范（VMX / VT-d / Virtio）
- [ ] 字段命名与规范一致
- [ ] 位范围准确
- [ ] 术语符合规范
- [ ] 引用（路径:行号、章节号）完整
- [ ] 代码示例来自实际源码
- [ ] 未重犯上文已知陷阱
- [ ] 用户态与内核态交互描述正确

## 学习原则

1. **准确性优先** — 宁可慢，也要保证准确。
2. **规范为准** — Intel / Virtio 规范是最终标准。
3. **源码为证** — 用实际代码验证理论。
4. **持续改进** — 发现错误立即纠正并回写文档。
