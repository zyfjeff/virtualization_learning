# Phase 5 实践练习

> VFIO 设备直通实践。所有练习在**宿主上直接运行**，不需要启动 Guest VM ——
> 目的是看清 VFIO 与 IOMMU 子系统的交互，Guest 只是这套机制的使用者。

---

## 练习列表

| 编号 | 练习名称 | 需要 VM | 难度 | 预计时间 | 核心知识点 |
|------|---------|---------|------|---------|-----------|
| 1 | DMA ownership 认领时机 | 否 | ★★☆ | 30min | IOMMU group、`driver_managed_dma`、blocking domain |
| 2 | IOVA → HPA 映射建立 | 否 | ★★★ | 40min | `VFIO_IOMMU_MAP_DMA`、页大小选择、与 EPT 的关系 |

两个练习都需要 root，且需要一个**可以安全接管的 PCI 设备**。

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

---

## 参考资料

- Phase 5 README：`/root/code/kvm-study/phase5-vfio/README.md`
- 本 phase 勘误：`/root/code/kvm-study/phase5-vfio/corrections.md`
- VFIO 内核源码：`/root/code/linux-6.12.93/drivers/vfio/`
- IOMMU 核心：`/root/code/linux-6.12.93/drivers/iommu/iommu.c`
- Intel IOMMU 驱动：`/root/code/linux-6.12.93/drivers/iommu/intel/`
- 规范：intel-vtd.pdf
