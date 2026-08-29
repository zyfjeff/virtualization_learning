# Phase 5 实践练习

> VFIO 设备直通实践。练习 1、2 在**宿主上直接运行**，不需要 Guest ——
> 目的是看清 VFIO 与 IOMMU 子系统的交互，Guest 只是这套机制的使用者。
> 练习 3 分两段：前半段（VFIO 侧建立 Remapped IRTE）仍是纯宿主，
> 后半段（IRTE 转为 Posted 模式）**必须真起一个带直通设备的 VM**，因为
> Posted 化的触发方 KVM 只在有 irqfd 消费者时才出现。

---

## 练习列表

| 编号 | 练习名称 | 需要 VM | 难度 | 预计时间 | 核心知识点 |
|------|---------|---------|------|---------|-----------|
| 1 | DMA ownership 认领时机 | 否 | ★★☆ | 30min | IOMMU group、`driver_managed_dma`、blocking domain |
| 2 | IOVA → HPA 映射建立 | 否 | ★★★ | 40min | `VFIO_IOMMU_MAP_DMA`、页大小选择、与 EPT 的关系 |
| 3 | MSI-X 中断直通与 IRTE Posted 化 | 后半段是 | ★★★★ | 90min | `VFIO_DEVICE_SET_IRQS`、`irq_bypass`、IRTE 解码、零 VM-Exit |

三个练习都需要 root，且需要一个**可以安全接管的 PCI 设备**。

---

## 前置准备

### 1. 确认 IOMMU 已启用

```bash
ls /sys/kernel/iommu_groups/ | wc -l     # 应远大于 0
dmesg | grep -i "DMAR: IOMMU enabled"
cat /proc/cmdline                        # 关注 intel_iommu=on / iommu=pt
```

### 2. 挑一个可以安全接管的设备

**这一步不能马虎**：绑定 `vfio-pci` 会把设备从原驱动上摘下来，正在用的磁盘或网卡会立刻中断。

```bash
# 逐一排查候选设备
BDF=0000:4b:00.0

# 它当前是什么、归谁管
lspci -s ${BDF#0000:} -k

# 独占一个 IOMMU group 吗（只有一个符号链接才是独占）
GROUP=$(basename $(readlink /sys/bus/pci/devices/$BDF/iommu_group))
ls /sys/kernel/iommu_groups/$GROUP/devices/

# 如果是块设备：确认没挂载、没进 fstab、没被当 swap
lsblk; grep -r vdb /etc/fstab; swapon --show
```

本文档的实测设备是 `0000:4b:00.0`（一块未挂载的数据盘，独占 group 35）。
**下面命令里的 group 号和 BDF 请换成你自己的。**

### 3. 绑定到 vfio-pci

```bash
BDF=0000:4b:00.0
sudo modprobe vfio-pci
echo vfio-pci | sudo tee /sys/bus/pci/devices/$BDF/driver_override
echo $BDF | sudo tee /sys/bus/pci/devices/$BDF/driver/unbind
echo $BDF | sudo tee /sys/bus/pci/drivers_probe

lspci -s ${BDF#0000:} -k        # 确认 Kernel driver in use: vfio-pci
ls /dev/vfio/                   # 应出现以 group 号命名的字符设备
```

### 4. 结束后恢复原驱动

```bash
BDF=0000:4b:00.0
echo | sudo tee /sys/bus/pci/devices/$BDF/driver_override
echo $BDF | sudo tee /sys/bus/pci/devices/$BDF/driver/unbind
echo $BDF | sudo tee /sys/bus/pci/drivers_probe
```

### 5. 编译

```bash
cd /root/code/kvm-study/phase5-vfio/practice
make
```

---

## 练习 1: DMA ownership 认领时机

**目标**：搞清楚 VFIO 究竟在哪一步「接管」设备。绑定 `vfio-pci` 时？还是更晚？

**运行**：

```bash
sudo ./vfio-claim-trace 35
```

**实测输出**：

```
[域类型] 初始状态                       identity
[1] 打开 container fd
    [域类型] 打开 container 之后            identity
[2] VFIO_GET_API_VERSION = 0 (期望 0)
[3] VFIO_CHECK_EXTENSION(TYPE1v2) = 1
[4] 打开 group fd: /dev/vfio/35
    [域类型] 打开 group 之后                identity
[5] VFIO_GROUP_GET_STATUS flags = 0x1  VIABLE=是 CONTAINER_SET=否
[6] VFIO_GROUP_SET_CONTAINER 成功
    [域类型] SET_CONTAINER 之后               identity
[7] VFIO_SET_IOMMU(TYPE1v2) 成功
    [域类型] SET_IOMMU 之后                   identity

持有 fd 中，可在另一个终端观察内核状态。回车释放并退出。
    [域类型] 关闭全部 fd 之后             identity
```

**第一个反直觉的点**：`identity` 从头到尾没变过。

这不是程序没生效，而是 `/sys/kernel/iommu_groups/N/type` 读的是 `group->default_domain->type`
（`drivers/iommu/iommu.c:890`），而 VFIO 接管只改 `group->domain`。**sysfs 这个属性天生观测不到 VFIO 接管**，
细节见 `phase5-vfio/corrections.md` 勘误 3。

**换成 kprobe 才能看见**：

```bash
sudo bash -c '
cd /sys/kernel/tracing
echo 0 > tracing_on; echo 0 > events/kprobes/enable; echo > kprobe_events
for f in vfio_container_attach_group iommu_group_claim_dma_owner \
         blocking_domain_attach_dev vfio_iommu_type1_attach_group \
         intel_iommu_domain_alloc intel_iommu_attach_device \
         iommu_group_release_dma_owner; do
  echo "p:$f $f" >> kprobe_events
done
echo 1 > events/kprobes/enable; echo > trace; echo 1 > tracing_on'

sudo ./vfio-claim-trace 35 > /dev/null
sudo cat /sys/kernel/tracing/trace | grep -v '^#'

# 用完务必清理
sudo bash -c 'cd /sys/kernel/tracing
echo 0 > tracing_on; echo 0 > events/kprobes/enable; echo > kprobe_events; echo > trace'
```

**实测输出**（时间戳已保留，便于看耗时）：

```
vfio-claim-trac-2452944 [022] 12171512.272445: vfio_container_attach_group
vfio-claim-trac-2452944 [022] 12171512.272447: iommu_group_claim_dma_owner
vfio-claim-trac-2452944 [022] 12171512.272449: blocking_domain_attach_dev
vfio-claim-trac-2452944 [022] 12171512.272466: vfio_iommu_type1_attach_group
vfio-claim-trac-2452944 [022] 12171512.272572: intel_iommu_domain_alloc
vfio-claim-trac-2452944 [022] 12171512.272575: intel_iommu_attach_device
vfio-claim-trac-2452944 [022] 12171512.272620: blocking_domain_attach_dev
vfio-claim-trac-2452944 [022] 12171512.272627: iommu_group_release_dma_owner
vfio-claim-trac-2452944 [022] 12171512.272628: intel_iommu_attach_device
```

**关注点**：

1. **绑定 `vfio-pci` 时一个事件都没有**。认领发生在 `SET_CONTAINER`
   （`drivers/vfio/container.c:437` 调 `iommu_group_claim_dma_owner()`），因为 `vfio-pci`
   声明了 `.driver_managed_dma = true`（`drivers/vfio/pci/vfio_pci.c:205`），probe 阶段刻意不动 `owner_cnt`。
2. **前 6 行是接管，后 3 行是关 fd 时的归还**，全程约 160 µs，其中 `intel_iommu_domain_alloc` 占了 88 µs。
3. **`blocking_domain_attach_dev` 出现两次，各在一次 domain 切换之前**。这是安全联锁：
   ownership 转移期间先把 DMA 窗口关死，不存在「旧 domain 已解除、新 domain 未就绪」的空档。

**故意制造失败**：如果 group 里还有别的设备绑在普通驱动上，`SET_CONTAINER` 会返回 `-EPERM`
（`drivers/iommu/iommu.c:3214` 的 `if (group->owner_cnt)`），程序会打印提示。
注意失败点是 `SET_CONTAINER` 而不是 bind —— `README.md` 陷阱1 这里写错了，见勘误 1。

---

## 练习 2: IOVA → HPA 映射建立

**目标**：证明 IOMMU 页表里存的是**裸宿主物理地址**，与 EPT 完全无关。

**思路**：用户态自己用 `/proc/self/pagemap` 算出缓冲区的 HPA，再用 kprobe 抓
`intel_iommu_map_pages` 收到的 `paddr` 参数。两者相等就说明问题。

**运行**：

```bash
# 先挂 kprobe（%di/%si/%dx/%cx/%r8 是 x86-64 SysV 前五个整型参数）
sudo bash -c '
cd /sys/kernel/tracing
echo 0 > tracing_on; echo 0 > events/kprobes/enable; echo > kprobe_events
echo "p:map_pages intel_iommu_map_pages dom=%di iova=%si paddr=%dx pgsize=%cx pgcount=%r8" >> kprobe_events
echo 1 > events/kprobes/enable; echo > trace; echo 1 > tracing_on'

sudo ./vfio-dma-map 35 0000:4b:00.0 4
sudo cat /sys/kernel/tracing/trace | grep -v '^#'

sudo bash -c 'cd /sys/kernel/tracing
echo 0 > tracing_on; echo 0 > events/kprobes/enable; echo > kprobe_events; echo > trace'
```

对应的函数原型（确认参数顺序，别把寄存器对错）：

```c
/* 来源: drivers/iommu/intel/iommu.c */
static int intel_iommu_map_pages(struct iommu_domain *domain, unsigned long iova,
				 phys_addr_t paddr, size_t pgsize, size_t pgcount,
				 int prot, gfp_t gfp, size_t *mapped)
```

**实测输出**：

```
[1] container + group 就绪，IOMMU 后端 = Type1v2
[2] VFIO_IOMMU_GET_INFO: flags=0x3
    支持的页大小: 4KB 2MB 1GB
[3] 设备 fd 就绪: 0000:4b:00.0  regions=9 irqs=5 flags=0x3 (支持 reset)
[4] 测试缓冲区 4 页 (16384 字节)
    用户态 VA  = 0x7a6773a22000
    宿主 HPA   = 0x2ec310b000  (pagemap PFN=0x2ec310b)
[5] VFIO_IOMMU_MAP_DMA 成功
    IOVA 0x100000000 → HPA 0x2ec310b000，长度 16384
```

```
map_pages: dom=0xffff91e188d5da80 iova=0x0          paddr=0x2f85aca000 pgsize=0x1000 pgcount=0x2
map_pages: dom=0xffff91e188d5da80 iova=0x100000000  paddr=0x2ec310b000 pgsize=0x1000 pgcount=0x1
map_pages: dom=0xffff91e188d5da80 iova=0x100001000  paddr=0x2f4add5000 pgsize=0x1000 pgcount=0x1
map_pages: dom=0xffff91e188d5da80 iova=0x100002000  paddr=0x1c4a5d8000 pgsize=0x1000 pgcount=0x1
map_pages: dom=0xffff91e188d5da80 iova=0x100003000  paddr=0x2351fb0000 pgsize=0x1000 pgcount=0x1
```

**关注点**：

1. **`paddr=0x2ec310b000` 与用户态算出的 HPA 完全一致**（本文档三次独立运行都对上：
   `0x2e08e08000`、`0xe46766000`、`0x2ec310b000`）。这直接印证了：

   ```c
   /* 来源: drivers/vfio/vfio_iommu_type1.c:1428 */
   ret = iommu_map(d->domain, iova, (phys_addr_t)pfn << PAGE_SHIFT,
   		npage << PAGE_SHIFT, prot | IOMMU_CACHE,
   		GFP_KERNEL_ACCOUNT);
   ```

   页表项是 `pfn << PAGE_SHIFT`，中间没有任何 GPA 层次。EPT 管 CPU 侧的 GPA→HPA，
   IOMMU 页表管设备侧的 IOVA→HPA，两套结构互不相干，唯一交集是最终落到同一块 HPA。
   这也解释了直通为什么**必须 pin 内存**：HPA 是写死在页表里的，页一旦被换出或迁移，
   设备会 DMA 到错误的物理内存，而设备访问不走 CPU 缺页机制，没有任何补救机会。

2. **IOVA 连续，HPA 散落**：四页落在 `0x2ec310b000` / `0x2f4add5000` / `0x1c4a5d8000` / `0x2351fb0000`，
   跨了好几个 GB。这正是 IOMMU 对直通设备的核心价值 —— 设备只看到一段连续 IOVA。

3. **`pgcount` 全是 1，2MB/1GB 能力用不上**。`GET_INFO` 明明报了大页，但受限于 pin 阶段：

   ```c
   /* 来源: drivers/vfio/vfio_iommu_type1.c:1461 */
   while (size) {
   	/* Pin a contiguous chunk of memory */
   	npage = vfio_pin_pages_remote(dma, vaddr + dma->size,
   				      size >> PAGE_SHIFT, &pfn, limit,
   				      &batch);
   ```

   注释里的 "contiguous" 是**物理连续**。四页物理上互不相邻，每轮只能 pin 到 1 页，
   循环跑了 4 次。要用上大页，前提是宿主先给出物理连续的大块（hugetlb / THP），VFIO 自己不做搬移。

4. **第一条 `iova=0x0 pgcount=0x2` 不是我们的映射**。它 domain 指针与后续相同、
   时间上早约 100 ms（紧跟 attach），来自 `vfio_test_domain_fgsp()`
   （`drivers/vfio/vfio_iommu_type1.c:1823`，在 `:2300` 调用）—— 映射两页再 unmap 一页，
   探测硬件细粒度 superpage 能力并缓存进 `domain->fgsp`（`:84`）。

---

## 练习 3: MSI-X 中断直通与 IRTE Posted 化

**目标**：走完直通中断的完整链路，并亲眼看到同一个 IRTE 从 **Remapped 模式**
（`IM=0`，中断先进宿主）被改写成 **Posted 模式**（`IM=1`，硬件直接投递给 vCPU）。

这条链路有个关键事实：**VFIO 自己只能建到 Remapped**。把 IRTE 改成 Posted 的是 KVM，
而 KVM 要出场必须有 irqfd 消费者，也就是必须真有一个 VM。所以练习分两段做。

### 第一段（宿主侧）：VFIO 如何装配 MSI-X

**运行**：

```bash
sudo ./vfio-msix-probe 35 0000:4b:00.0 4
```

**实测输出**（节选）：

```
[3] VFIO_DEVICE_GET_INFO: num_regions=9 num_irqs=5 flags=0x3

    索引  名称     count  flags
    0     INTX     1      0x9  (EVENTFD MASKABLE)
    1     MSI      1      0xb  (EVENTFD MASKABLE NORESIZE)
    2     MSIX     7      0x9  (EVENTFD MASKABLE)
    3     ERR      1      0x9  (EVENTFD MASKABLE)
    4     REQ      1      0x5  (EVENTFD AUTOMASKED)

[4] 启用 4 个 MSI-X 向量 (VFIO_DEVICE_SET_IRQS)
    已分配的 Linux IRQ:
      irq    chip                               hwirq  重映射
      112    IR-PCI-MSIX-0000:4b:00.0           0      Remapped (IM=0)
      113    IR-PCI-MSIX-0000:4b:00.0           1      Remapped (IM=0)
      114    IR-PCI-MSIX-0000:4b:00.0           2      Remapped (IM=0)
      115    IR-PCI-MSIX-0000:4b:00.0           3      Remapped (IM=0)
```

**三个值得停下来看的点**：

1. **`MSIX count=7` 不是写死的，是现场读配置空间读出来的**：

   ```c
   /* 来源: drivers/vfio/pci/vfio_pci_core.c:786 */
   } else if (irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
   	pos = vdev->pdev->msix_cap;
   	if (pos) {
   		pci_read_config_word(vdev->pdev, pos + PCI_MSIX_FLAGS, &flags);
   		return (flags & PCI_MSIX_FLAGS_QSIZE) + 1;
   ```

2. **MSI 有 `NORESIZE`，MSI-X 没有**。这不是笔误：MSI 的向量数由配置空间的 log2 字段决定，
   一旦分配不能改；MSI-X 每个向量独立成表项，可以随时增减，所以 QEMU 能跟着 Guest
   逐个向量地开关。

3. **chip 名字带 `IR-` 前缀** —— 中断重映射已经在链路上，IRTE 已经建好了。
   此刻它是 Remapped 模式：中断照常打进宿主 CPU，由 `vfio_msihandler()` 收下。

**用 kprobe 看装配顺序**：

```bash
sudo bash -c '
cd /sys/kernel/tracing
echo 0 > tracing_on; echo 0 > events/kprobes/enable; echo > kprobe_events
for f in pci_alloc_irq_vectors_affinity intel_irq_remapping_alloc \
         request_threaded_irq irq_bypass_register_producer; do
  echo "p:$f $f" >> kprobe_events
done
echo 1 > events/kprobes/enable; echo > trace; echo 1 > tracing_on'

sudo ./vfio-msix-probe 35 0000:4b:00.0 4 > /dev/null
sudo cat /sys/kernel/tracing/trace | grep -v '^#'
```

**实测形态**（4 个向量共约 345 µs）：

```
pci_alloc_irq_vectors_affinity        ×1   ← 一次性向 PCI 层申请全部向量
intel_irq_remapping_alloc             ×4   ← 每向量建一个 IRTE
request_threaded_irq                  ×4   ┐ 交替出现
irq_bypass_register_producer          ×4   ┘ 每向量一对
```

对应代码，`vfio_msi_set_vector_signal()` 里这两步是紧挨着的：

```c
/* 来源: drivers/vfio/pci/vfio_pci_intrs.c:510 */
	ret = request_irq(irq, vfio_msihandler, 0, ctx->name, trigger);
```

```c
/* 来源: drivers/vfio/pci/vfio_pci_intrs.c:515 */
	ctx->producer.token = trigger;
	ctx->producer.irq = irq;
	ret = irq_bypass_register_producer(&ctx->producer);
```

**`token` 就是 eventfd 上下文指针** —— 这是 VFIO 与 KVM 之间唯一的接缝，记住它，第二段全靠它。

### 第二段（Guest 侧）：IRTE 被改写成 Posted 模式

#### 挂探针

`modify_irte` 是 `static`，编译后成了 `modify_irte.isra.0`，**且本机有两个同名符号**：

```bash
sudo grep " modify_irte" /proc/kallsyms
# ffffffffb155b2a0 t modify_irte.isra.0
# ffffffffb1578320 t modify_irte.isra.0    ← Intel IR 的那个
```

`.isra` 意味着编译器改过函数签名，寄存器与形参的对应关系**不能照原型推断**。
所以下面的裸地址只对本机这一次启动有效，**换机器、换内核甚至重启都必须重新取址并重新验证**。

```bash
sudo bash -c '
cd /sys/kernel/tracing
echo 0 > tracing_on; echo 0 > events/kprobes/enable; echo > kprobe_events
echo nop > current_tracer; echo > set_ftrace_filter
echo "p:mirte 0xffffffffb1578320 lo=+0(%si):x64 hi=+8(%si):x64"           >> kprobe_events
echo "p:bypass_cons irq_bypass_register_consumer"                          >> kprobe_events
echo "p:kvm_add_prod kvm_arch_irq_bypass_add_producer"                     >> kprobe_events
echo "p:pi_update vmx_pi_update_irte host_irq=%si guest_irq=%dx set=%cx"   >> kprobe_events
echo "p:ir_vcpu_aff intel_ir_set_vcpu_affinity info=%si pi_desc=+0(%si):x64 vvec=+8(%si):u32" >> kprobe_events
echo 1 > events/kprobes/enable; echo > trace; echo 1 > tracing_on'
```

参数取法的依据（`%si` 是 x86-64 SysV 第二个整型参数）：

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:156 */
static int modify_irte(struct irq_2_iommu *irq_iommu,
		       struct irte *irte_modified)
```

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:1248 */
static int intel_ir_set_vcpu_affinity(struct irq_data *data, void *info)
```

```c
/* 来源: arch/x86/include/asm/irq_remapping.h:29 —— info 的真实类型 */
struct vcpu_data {
	u64 pi_desc_addr;	/* Physical address of PI Descriptor */
	u32 vector;		/* Guest vector of the interrupt */
};
```

#### 先做对照验证，再相信探针

**这一步不能跳过**。`.isra` 符号的取参可能整个是错的，必须先在**已知答案的场景**上校验：
拿第一段的纯宿主流程跑一遍，检查 `hi` 的低 16 位是不是等于设备 BDF（IRTE 的 `SID` 字段）。

```
mirte: lo=0x100ef000d hi=0x44b00
```

`hi & 0xffff = 0x4b00`，正是 `4b:00.0`（bus=0x4b, dev=0x00, fn=0）。`%si` 取对了。
顺手把 `lo` 解一遍，也自洽：

| 字段 | 值 | 含义 |
|---|---|---|
| `present` bit 0 | 1 | 有效 |
| `pst` bit 15 | **0** | **Remapped 模式** |
| `vector` bits 23:16 | `0xEF` | 见下 |
| `dst_mode` bit 2 | 1 | logical |
| `dlvry_mode` bits 7:5 | 0 | FIXED |

`0xEF` 不是真向量，是**停放向量**：

```c
/* 来源: arch/x86/include/asm/irq_vectors.h:91 */
#define MANAGED_IRQ_SHUTDOWN_VECTOR	0xef
```

alloc 阶段中断还没 activate，先用它占位（`arch/x86/kernel/apic/vector.c:192`），
`arch/x86/kernel/apic/msi.c:61` 的注释也把 "old vector is `MANAGED_IRQ_SHUTDOWN_VECTOR`"
当作「中断正在启动」的判据。

#### 起 VM

仓库里有专为本实验准备的脚本（内置 `DEVICE="0000:4b:00.0"`，`stop` 会自动恢复原驱动）：

```bash
cd /root/code/kvm-study/scripts/vm
sudo ./setup-vfio-vm.sh start        # tmux 会话 pi-test-vm
sudo ./setup-vfio-vm.sh status
```

**务必确认真的走了 KVM**（AGENTS.md 陷阱7）：

```bash
for p in $(pgrep -f qemu-system-x86_64); do
  echo "pid=$p kvmfd=$(sudo ls -l /proc/$p/fd 2>/dev/null | grep -c kvm)"
done
# 实测 pid=2471100 kvmfd=7   （另一个 pid 是 tmux 包装进程，kvmfd=0，别看错）
```

#### 实测：每个向量的完整时序

```bash
sudo cat /sys/kernel/tracing/trace | grep -v '^#'
```

单个向量（host irq 112）的事件序列：

```
mirte      lo=0x100ef000d        hi=0x44b00        ← ① alloc 占位，vector 0xEF，pst=0
bypass_cons                                        ← ② KVM 注册 irqfd 消费者
mirte      lo=0x101000021000d    hi=0x44b00        ← ③ Remapped，vector 0x21，pst=0
mirte      lo=0x101000021000d    hi=0x44b00        ←    （同值写两次）
kvm_add_prod                                       ← ④ token 配对成功，__connect()
pi_update  host_irq=0x70 guest_irq=0x2 set=0x1     ← ⑤ KVM 请求 Posted 化
ir_vcpu_aff pi_desc=0x29b94842c0 vvec=33           ← ⑥ 带着 PI Desc 地址进 IR 驱动
mirte      lo=0xb94842c000218001 hi=0x2900044b00   ← ⑦ Posted IRTE 落盘
```

**耗时**：② → ⑦ 约 53 µs，④ → ⑦ 只有 5 µs。

②③ 的顺序说明 VFIO 和 KVM 是**各自独立**注册的，谁先谁后都行 ——
`irq_bypass_register_producer()` 和 `irq_bypass_register_consumer()` 两边都会扫对面的链表：

```c
/* 来源: virt/lib/irqbypass.c:108 —— producer 侧 */
		if (consumer->token == producer->token) {
			ret = __connect(producer, consumer);
```

```c
/* 来源: virt/lib/irqbypass.c:204 —— consumer 侧，逻辑对称 */
		if (producer->token == consumer->token) {
			ret = __connect(producer, consumer);
```

`__connect()` 里 `cons->add_producer` 就是 ④：

```c
/* 来源: virt/kvm/eventfd.c:444 */
		irqfd->consumer.token = (void *)irqfd->eventfd;
		irqfd->consumer.add_producer = kvm_arch_irq_bypass_add_producer;
```

**consumer 的 token 是 `irqfd->eventfd`，producer 的 token 是 `ctx->trigger`，
两者指向同一个 eventfd 上下文** —— 这就是 QEMU 把同一个 eventfd 分别交给
`VFIO_DEVICE_SET_IRQS` 和 `KVM_IRQFD` 之后，内核两侧能自动对上的原因。

#### 解码三个 Posted IRTE

抓到三个向量的 Posted IRTE，按 `struct irte`（`include/linux/dmar.h:201`）逐位解开：

| 向量 | host_irq | GSI | `pst` (spec: **IM**) | `p_vector` (spec: **VV**) | `SID` | `PDAL` | `PDAH` | 反推 PI Desc | 探针实抓 |
|---|---|---|---|---|---|---|---|---|---|
| v0 | 0x70 | 0x02 | **1** | 33 | 0x4b00 | 0x2e5210b | 0x29 | `0x29b94842c0` | `0x29b94842c0` ✓ |
| v1 | 0x71 | 0x18 | **1** | 32 | 0x4b00 | 0x2e5210b | 0x29 | `0x29b94842c0` | `0x29b94842c0` ✓ |
| v2 | 0x72 | 0x19 | **1** | 33 | 0x4b00 | 0x2a7b79a | 0x1c | `0x1ca9ede680` | `0x1ca9ede680` ✓ |

反推公式直接来自驱动（`PDA_LOW_BIT = 26`，故 `PDAL` 存的是地址 bits 31:6）：

```c
/* 来源: drivers/iommu/intel/irq_remapping.c:1272 —— intel_ir_set_vcpu_affinity() */
		irte_pi.p_pst = 1;
		irte_pi.p_urgent = 0;
		irte_pi.p_vector = vcpu_pi_info->vector;
		irte_pi.pda_l = (vcpu_pi_info->pi_desc_addr >>
				(32 - PDA_LOW_BIT)) & ~(-1UL << PDA_LOW_BIT);
		irte_pi.pda_h = (vcpu_pi_info->pi_desc_addr >> 32) &
				~(-1UL << PDA_HIGH_BIT);
```

```python
# 反推：addr = (pda_h << 32) | (pda_l << 6)
>>> hex((0x29 << 32) | (0x2e5210b << 6))
'0x29b94842c0'      # 与 ir_vcpu_aff 抓到的 pi_desc 完全一致
```

**两个已知陷阱在这里同时得到实测印证**：

- **陷阱3（向量空间）**：v0 和 v2 的 `VV` 都是 33，但 `PDAL/PDAH` 不同 ——
  它们投递到**两个不同的 vCPU**。同一个向量号在不同 PI Descriptor 上共存，
  再次说明唯一性是 `(CPU, vector)` 而非全局 256 个。
- **陷阱4（PDA 位范围）**：`PDAL` 26 位存地址 bits 31:6、`PDAH` 32 位存 bits 63:32，
  合起来 58 位、64 字节对齐 —— 三次反推全部对上。

#### 验证零 VM-Exit

Posted 模式下中断由硬件直接送进 vCPU，**`vfio_msihandler()` 根本不执行**：

```c
/* 来源: drivers/vfio/pci/vfio_pci_intrs.c:373 */
static irqreturn_t vfio_msihandler(int irq, void *arg)
{
	struct eventfd_ctx *trigger = arg;

	eventfd_signal(trigger);
	return IRQ_HANDLED;
}
```

它不跑，宿主的 IRQ 计数器就不会动。在 Guest 里压一轮**只读** I/O：

```bash
# Guest 内（直通设备在 Guest 里是 0000:00:04.0 / /dev/vda）
grep -E "^ *26:" /proc/interrupts        # 压测前
dd if=/dev/vda of=/dev/null bs=4k count=20000     # 80MB，只读，不碰数据
grep -E "^ *26:" /proc/interrupts        # 压测后
```

```bash
# 宿主侧，同时观察
grep -E "^ *(112|113|114):" /proc/interrupts
```

**实测结果**：

| 位置 | 计数器 | 压测前 | 压测后 | 增量 |
|---|---|---|---|---|
| Guest | irq 26 `virtio0-req.1` | 632 | 1261 | **+629** |
| 宿主 | irq 112 | 0 | 0 | **0** |
| 宿主 | irq 113 | 0 | 0 | **0** |
| 宿主 | irq 114 | 0 | 0 | **0** |

Guest 收了 629 个中断，宿主一个都没收到。中断走的是
「设备 → IOMMU 查 Posted IRTE → 写 PI Descriptor 的 PIR → 发 Notification Vector → 硬件直接置 RVI」，
全程不经过宿主内核，也不产生 VM-Exit（SDM 30.6，见 AGENTS.md 陷阱6）。

**顺带一个反直觉的观察**：Guest 内看到的 chip 名字是 `PCI-MSIX-0000:00:04.0`，
**没有 `IR-` 前缀**。因为本次没给 Guest 配 vIOMMU，Guest 眼里压根没有中断重映射 ——
重映射整个发生在宿主侧，对 Guest 完全透明。

#### 收尾（务必执行）

```bash
cd /root/code/kvm-study/scripts/vm
sudo ./setup-vfio-vm.sh stop         # 同时把设备恢复到原驱动

# ftrace 状态全清 —— 注意不只是 kprobe_events
sudo bash -c 'cd /sys/kernel/tracing
echo 0 > tracing_on; echo 0 > events/kprobes/enable; echo > kprobe_events
echo nop > current_tracer; echo > set_ftrace_filter; echo > trace'

# 自检
echo "探针 $(sudo cat /sys/kernel/tracing/kprobe_events | wc -l) 个, \
tracer=$(sudo cat /sys/kernel/tracing/current_tracer), \
tracing_on=$(sudo cat /sys/kernel/tracing/tracing_on)"
# 期望：探针 0 个, tracer=nop, tracing_on=0

lspci -s 4b:00.0 -k | grep "Kernel driver"    # 确认已还给原驱动
lsblk                                          # 确认盘回来了
```

---

## 故障排查

### `/dev/vfio/N` 不存在

设备还没绑上 `vfio-pci`，或 `vfio_iommu_type1` 没加载：

```bash
lsmod | grep -E "^vfio"
sudo modprobe vfio-pci vfio_iommu_type1
```

### `VFIO_GROUP_SET_CONTAINER: Operation not permitted` (`-EPERM`)

同组还有设备绑在普通驱动上。列出全组成员，把它们一并绑到 `vfio-pci`，或换一个独占 group 的设备：

```bash
ls /sys/kernel/iommu_groups/35/devices/
```

### `VFIO_GROUP_GET_STATUS` 里 VIABLE=否

同上：组内存在未被 VFIO（或 unbound 状态）覆盖的设备。

### pagemap 读出来 PFN=0

需要 root，且页必须已实际分配。程序里用了 `MAP_POPULATE` + `memset` 来保证这点。
另外 `/proc/<pid>/pagemap` 对非特权进程会屏蔽 PFN（返回 0），必须 `sudo` 运行。

### kprobe 写入报 `Device or resource busy`

探针还处于 enabled 状态，先关再清：

```bash
sudo bash -c 'cd /sys/kernel/tracing; echo 0 > events/kprobes/enable; echo > kprobe_events'
```

### kprobe 抓出来的参数是垃圾值

先看符号名带不带 `.isra` / `.constprop` / `.part`：

```bash
sudo grep " your_func" /proc/kallsyms
```

带这些后缀说明编译器改过签名（`.isra` = 参数被标量化替换），**寄存器与形参的对应
关系不能照源码原型推断**，而且可能存在多个同名符号。唯一可靠的做法是先在**已知答案的
场景**上做对照验证（例如练习 3 里用 IRTE 的 `SID` 必须等于设备 BDF 来校验 `%si`），
验证通过后才能相信这个探针的其他输出。

### trace 里全是无关事件，找不到自己的探针

大概率是之前的实验留下了 `function` tracer 和 `set_ftrace_filter`。**清理 kprobe 不等于
清理 ftrace**，两者是独立的：

```bash
sudo bash -c 'cd /sys/kernel/tracing
echo nop > current_tracer; echo > set_ftrace_filter; echo > trace'
```

临时的过滤办法是按探针名筛：`grep -E "mirte|pi_update|ir_vcpu_aff" trace`。

### IRTE 始终不转 Posted 模式

`pst` 一直是 0，说明 `vmx_pi_update_irte()` 在入口就返回了。四个前提缺一不可：

```c
/* 来源: arch/x86/kvm/vmx/posted_intr.c:135 */
static bool vmx_can_use_vtd_pi(struct kvm *kvm)
{
	return irqchip_in_kernel(kvm) && enable_apicv &&
		kvm_arch_has_assigned_device(kvm) &&
		irq_remapping_cap(IRQ_POSTING_CAP);
}
```

逐项排查：

```bash
cat /sys/module/kvm_intel/parameters/enable_apicv    # 需要 Y
dmesg | grep -i "Posted-Interrupt\|posting"          # 硬件 PI 能力
```

即使这四项都过了，`vmx_pi_update_irte()` 内部还会逐条路由表项检查
（`posted_intr.c:315`）：只有**单目标 vCPU**、且 `kvm_irq_is_postable()` 为真的中断才 Posted 化，
多播/广播/低优先级多目标仍然留在 Remapped 模式。

### `/sys/kernel/debug/iommu/intel/` 不存在

宿主内核没开 `CONFIG_INTEL_IOMMU_DEBUGFS`，看不到 `ir_translation_struct`。
替代手段有两个：`/sys/kernel/irq/<N>/chip_name` 看有没有 `IR-` 前缀（能区分有无重映射，
但**区分不了 Remapped 与 Posted**），以及本练习用的 `modify_irte` kprobe（能拿到原始位）。

---

## 参考资料

- Phase 5 README：`/root/code/kvm-study/phase5-vfio/README.md`
- 本 phase 勘误：`/root/code/kvm-study/phase5-vfio/corrections.md`
- Phase 3（中断虚拟化 + VT-d IR）：`/root/code/kvm-study/phase3-interrupts/`
- 实验 VM 环境：`/root/code/kvm-study/scripts/README.md`
- VFIO 内核源码：`/root/code/linux-6.12.93/drivers/vfio/`
- VFIO PCI 中断：`/root/code/linux-6.12.93/drivers/vfio/pci/vfio_pci_intrs.c`
- IOMMU 核心：`/root/code/linux-6.12.93/drivers/iommu/iommu.c`
- Intel IOMMU 驱动：`/root/code/linux-6.12.93/drivers/iommu/intel/`
- Intel 中断重映射：`/root/code/linux-6.12.93/drivers/iommu/intel/irq_remapping.c`
- `struct irte` 定义：`/root/code/linux-6.12.93/include/linux/dmar.h:201`
- irq_bypass 管理器：`/root/code/linux-6.12.93/virt/lib/irqbypass.c`
- KVM irqfd：`/root/code/linux-6.12.93/virt/kvm/eventfd.c`
- 规范：intel-vtd.pdf（IRTE 格式见 9.10）；intel-vmx.pdf（Posted-Interrupt 处理见 30.6）
