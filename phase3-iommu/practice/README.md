# 第3阶段实践：4 个实验

> 每个实验对应主文档里的一个问题，按"目的 → 环境 → 步骤 → 观察点 → 结论回链"
> 组织。观测工具统一用 [`../../scripts/trace/iommu-analysis.sh`](../../scripts/trace/iommu-analysis.sh)
> 加 ftrace，不新增脚本。
>
> 文档中的 `path:line` 可用 [`check-citations.sh`](check-citations.sh) 复核：
> `./check-citations.sh`（需内核源码树，默认 `/root/code/linux-6.12.93`）。

## 环境与前置

| 条件 | 说明 |
|---|---|
| root 权限 | sysfs/tracefs/debugfs 读写都要 |
| ftrace 可用 | `/sys/kernel/tracing` 或 `/sys/kernel/debug/tracing` 存在 |
| 实验 1/3 | 任意有 IOMMU 的 x86 主机（Intel VT-d 最佳） |
| 实验 2 | 可跑本仓库实验 VM（`scripts/vm/boot-vm.sh`）即可，**vIOMMU 在 guest 里做** |
| 实验 4 | 需要能向设备发 DMA 的手段（驱动加载/直通）；观察手段对普通主机也适用 |

实验 VM 的启动按 [AGENTS.md 陷阱 7](../../AGENTS.md)：`boot-vm.sh` 默认带
`-enable-kvm -cpu host`，不要用 `--tcg` 做 IOMMU 实验。

---

## 实验 1：域类型可见性（对应 [Q3](../domains.md)）

**目的**：亲眼看到"命令行要的域类型"与"实际生效的域类型"可以不一致，并验证
`DMA → DMA-FQ` 是唯一可在线切换的方向。

**步骤**：

1. 记录命令行诉求：
   ```bash
   cat /proc/cmdline | grep -o 'iommu[^ ]*'   # 如 iommu.passthrough=0 iommu.strict=1
   ```
2. 记录实际类型（每个组一条）：
   ```bash
   for g in /sys/kernel/iommu_groups/*/type; do echo "$g: $(cat $g)"; done | sort -t/ -k5 -n
   ```
3. 挑一个 `DMA` 类型的组，在线切到 `DMA-FQ`（**不需要解绑驱动**）：
   ```bash
   echo DMA-FQ > /sys/kernel/iommu_groups/<N>/type
   cat /sys/kernel/iommu_groups/<N>/type        # 应变为 DMA-FQ
   ```
4. 再试反向：`echo DMA > .../type`，预期失败并提示需要先解绑驱动
   （`dmesg` 可见原因）。
5. 用分析脚本做快照对照：
   ```bash
   sudo ../../scripts/trace/iommu-analysis.sh -s
   ```

**观察点**：

- 步骤 2 的打印与步骤 1 的诉求不一致时，解释差异：`iommu.strict=0` 会请求
  `DMA_FQ`，但若 `iommu_dma_init_fq()` 失败会降级回 `DMA`（`drivers/iommu/dma-iommu.c:721-724`，
  [Q5 I.11](../invalidation.md)）；开机日志里的 "Falling back to IOMMU_DOMAIN_DMA"
  则是分配回落（`drivers/iommu/iommu.c:1637-1639`，[annotations A.2.2](../annotations.md)）。
- 步骤 3 能成而步骤 4 不能，源码依据是 `iommu_group_store_type()` 的特判
  （`drivers/iommu/iommu.c:3071-3074`）。
- 注意 sysfs 打印的 `DMA/identity/blocked` 与 `iommu_domain_type_str()` 的
  "Translated/Passthrough" 是**两套命名**（`drivers/iommu/iommu.c:172` vs `:890`）。

**结论回链**：[Q3](../domains.md)（sysfs 类型 ≠ 命令行诉求）、[Q5 I.11](../invalidation.md)。

---

## 实验 2：总线地址 ≠ 物理地址（对应 [Q4](../iova.md)）

**目的**：用 `iommu:map` tracepoint 直接对比同一次映射的 `iova` 与 `paddr`，
证明"DMA 地址是分配出来的"。

**环境**：实验 VM + vIOMMU（QEMU 属性已按 10.1.0-rc2 源码核实：`intremap`
`hw/i386/x86-iommu.c:129`、`device-iotlb` `:131`、`caching-mode`
`hw/i386/intel_iommu.c:3832`）：

```bash
cd ../../scripts/vm
./boot-vm.sh ubuntu --qemu "-machine q35,kernel-irqchip=split \
  -device intel-iommu,intremap=on,caching-mode=on \
  -append 'console=ttyS0 intel_iommu=on'"
```

（`--qemu` 透传的参数排在最后、会覆盖默认 `-append`，所以要把
`console=ttyS0` 一并带上。若 QEMU 报参数错，先去掉 `caching-mode=on` 重试。）

**步骤**（以下在 **guest 内**）：

1. 确认 vIOMMU 生效：
   ```bash
   dmesg | grep -i dmar            # 应有 DMAR: Intel(R) Virtualization ...
   ls /sys/kernel/iommu_groups/    # 非空
   ```
2. 开 tracepoint，制造一次 DMA（任意块设备/网卡读写即可，如 `dd if=/dev/vda of=/dev/null bs=1M count=16`）：
   ```bash
   cd /sys/kernel/tracing
   echo 1 > events/iommu/map/enable
   echo 1 > tracing_on
   dd if=/dev/vda of=/dev/null bs=1M count=16
   echo 0 > tracing_on
   grep iova trace | head
   ```

**观察点**：

- 每条事件形如
  `IOMMU: iova=0x... - 0x... paddr=0x... size=...`
  （格式来自 `include/trace/events/iommu.h:79-101`，调用点
  `drivers/iommu/iommu.c:2505`）。**对比同一行的 `iova` 与 `paddr`：绝大多数
  不相等**——这就是"总线地址不是物理地址"的直接证据。
- 在 guest 里把 `intel_iommu=` 去掉重启，同样操作会怎样？（`iommu:map` 事件
  消失，因为走了 `dma_direct`，README §3.2 的总分支。）

**结论回链**：[Q4 IO.2](../iova.md)（IOVA 是分配出来的）、[annotations A.4](../annotations.md)。

---

## 实验 3：失效的代价——strict 与 lazy（对应 [Q5](../invalidation.md)）

**目的**：对照 `DMA`（strict，逐次范围失效）与 `DMA-FQ`（lazy，攒批全域失效）
的失效行为差异，并用 Intel 的延迟直方图量化。

**环境**：带 Intel VT-d 的主机；需要 `CONFIG_INTEL_IOMMU_DEBUGFS`
（`drivers/iommu/intel/Kconfig:33`，[Q5 I.5](../invalidation.md) 讨论过依赖链）。

**步骤**：

1. 打开延迟直方图开关：
   ```bash
   echo 1 > /sys/kernel/debug/iommu/intel/dmar_perf_latency
   ```
2. 选一个有活跃块设备 DMA 的组（如系统盘所在组**以外的**组，避免影响运行），
   记录其当前类型，然后：
   ```bash
   cat /sys/kernel/iommu_groups/<N>/type          # 期望 DMA
   # 制造持续 DMA 负载（对该组设备的读写）约 10 秒
   cat /sys/kernel/debug/iommu/intel/dmar_perf_latency   # 记录 strict 分布
   echo DMA-FQ > /sys/kernel/iommu_groups/<N>/type        # 在线切到 lazy
   # 同样的负载再来 10 秒
   cat /sys/kernel/debug/iommu/intel/dmar_perf_latency   # 记录 lazy 分布
   ```
3. 用脚本交叉验证组的拓扑与域状态：
   ```bash
   sudo ../../scripts/trace/iommu-analysis.sh -g <N>
   ```

**观察点**：

- 直方图三列对应三类失效（IOTLB / DevTLB / IEC，`drivers/iommu/intel/dmar.c:1384-1397`
  的分类逻辑，[Q5 I.5](../invalidation.md)）。**切到 `DMA-FQ` 后，失效次数应显著
  下降**——因为 10 ms 一批、每批一次全域失效（`drivers/iommu/dma-iommu.c:105-111/:179-184`，
  [Q5 I.11](../invalidation.md)）。
- 若切换后计数几乎不变：查 `cap_caching_mode()`——caching-mode 硬件上连 map
  都要失效（[Q5 I.2](../invalidation.md)），lazy 的收益会被吃掉。
- 队列满时当场同步刷（`drivers/iommu/dma-iommu.c:233-237`），高负载下 lazy 会
  退化回最坏情况的 strict——观察负载加压时的计数反弹。

**结论回链**：[Q5 I.11](../invalidation.md)、[实验 1](#实验-1域类型可见性对应-q3) 的在线切换。

---

## 实验 4：bypass 与 fault（对应 [Q8](../backends.md)）

**目的**：观察"设备访问了没映射的地址"时内核的报错现场，理解不同后端的
故障上报粒度差异；验证保留区/恒等映射的边界行为。

**步骤**：

1. 订阅 fault 事件并盯住日志：
   ```bash
   cd /sys/kernel/tracing
   echo 1 > events/iommu/io_page_fault/enable
   echo 1 > tracing_on
   dmesg -w &      # 另开一窗
   ```
   （调用点 `drivers/iommu/iommu.c:2714`，事件定义
   `include/trace/events/iommu.h:153`。）
2. 制造一次 fault。两个安全的选择（**不要拿系统盘做实验**）：
   - 给 `vfio-pci` 绑定的设备开 ATS/直通后，在 guest 内访问未映射地址；
   - 或者用本仓库 phase6 的直通实验环境，在 guest 里对一个未做
     `VFIO_IOMMU_MAP_DMA` 的 IOVA 发 DMA。
3. 在 Intel 主机上，`dmesg` 里应看到 `DMAR: DRHD: handling fault status reg ...`
   一类**已解码**的日志（带源设备、地址、原因），出自 `dmar_fault()`
   （`drivers/iommu/intel/dmar.c:1947`）。

**观察点**：

- Intel 的日志给出 BDF + 地址 + 原因字符串；同样语义的事件在 ARM SMMUv3
  主机上是**8 个裸十六进制字**（`drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.c:1856-1859`，
  [Q8 B.6](../backends.md)）——跨平台排障手册必须分开写。
- `iommu=pt`（IDENTITY 默认域）下，同样的越界访问**不会产生 fault**：
  地址恒等直达，写坏的是物理内存本身（[Q3](../domains.md)：passthrough
  是正确性开关）。这就是"为什么直通场景默认不用 `iommu=pt` 给不可信设备"。

**结论回链**：[Q8 B.6](../backends.md)、[Q3](../domains.md)、[Q5 I.10](../invalidation.md)（PRI 是唯一把缺页变可恢复的路）。

---

## 记录要求

每个实验完成后，把以下三样东西记在自己的笔记里（不要提交进本仓库）：

1. 关键命令与完整输出（至少一条能复现观察点的原始记录）；
2. 观察与预期的差异——差异本身就是下一个问题的种子；
3. 对应的自检问题回答：实验 1 → [Q3](../domains.md) 自检；实验 2 → [Q4](../iova.md)
   自检；实验 3 → [Q5 I.16](../invalidation.md)；实验 4 → [Q8](../backends.md) 自检。
