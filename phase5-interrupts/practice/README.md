# Phase 5 实践练习 — 理论取证链

> 五段式（COURSE_FRAMEWORK §3.5）：理论 → 预测 → 操作 → 对照 → 反馈。
> 基于设备 0000:4b:00.0 (Virtio block, MSI-X)，需 root。
>
> ⚠️ **安全纪律（AGENTS.md #15/#16）**：4b:00.0 曾发生 unbind 挂死事故。
> unbind/复位前确认无在途 I/O、无 opener；绝不通过 /dev/mem 摸驱动着的设备 BAR；
> 可疑设备 sysfs 访问用 timeout 包裹；实验结束恢复到 vfio-pci 安全绑定。
> 该设备 BAR 布局随 MSI-X Enable 动态变化，寄存器换算先做启用前/后对照 dump。

## 📋 实验总览（取证链）

| 编号 | 验证的理论 | 预测（理论成立必然观察到） | 对照判据 |
|------|-----------|--------------------------|---------|
| ex1 | PI 环境支持（APICv+IR） | enable_apicv=Y，IOMMU intremap=on | 模块参数/dmesg 对照 |
| ex2 | IRTE Posted 格式（AGENTS.md #1） | 直通设备 IRTE pst(IM)=1，PDA 指向 vCPU pi_desc | IRTE dump 字段对照（SID=BDF 验证 kprobe 可信，#9） |
| ex3 | 零 VM-Exit（SDM 30.6） | Posted 通知向量不产生 kvm_exit | trace 计数对照（向量匹配 vs 不匹配） |
| ex4 | Posted vs Remapped 开销 | Posted exit 数/延迟低于 Remapped | A/B 对照（数字 → phase10 分级） |
| ex5 | SN 位抑制（vCPU 抢占） | vCPU 阻塞时 SN=1，通知被抑制 | pi_desc SN 位对照 vCPU 状态 |
| ex6 | 迁移只改 PI desc | vCPU 迁移后 NDST 更新、**IRTE 不变**（Posted IRTE 无 destination） | 迁移前后 IRTE/pi_desc 对照 |

**链式衔接**：ex1 环境 → ex2 IRTE 格式 → ex3 零 VM-Exit → ex4 开销 → ex5 SN 抑制 → ex6 迁移维护。

---

## 实验列表（难度/时间参考）

| 编号 | 实验名称 | 需要 VM | 难度 | 预计时间 | 核心知识点 |
|------|---------|---------|------|---------|-----------|
| 0 | 环境搭建 (VFIO 直通 VM) | - | ★☆☆ | 5min | VFIO 设备绑定 |
| 1 | PI 环境检查 | 否 | ★☆☆ | 10min | APICv/PI 支持检测 |
| 2 | IRTE 观察 | 否 | ★★☆ | 15min | IRTE 格式、Posted vs Remapped |
| 3 | PI 中断追踪 | **是** | ★★★ | 20min | 中断投递完整路径 |
| 4 | PI vs Remapped 性能对比 | **是** | ★★★ | 30min | 零 VM-Exit 性能优势 |
| 5 | ON/SN 行为观察 | **是** | ★★★ | 20min | 通知合并、抑制机制 |
| 6 | vCPU 迁移与 NDST 更新 | **是** | ★★★ | 20min | vCPU 调度、NDST 动态更新 |

---

## 快速开始

```bash
cd phase5-interrupts/practice/

# 步骤 1: 环境检查（不需要 VM）
sudo bash ex1-pi-env-check.sh

# 步骤 2: 启动带设备直通的 VM（实验 3-6 需要）
# 注意: setup-vfio-vm.sh 在公共目录 scripts/ 下
sudo bash ../../scripts/vm/setup-vfio-vm.sh start

# 步骤 3: 运行实验
sudo bash ex3-pi-trace.sh        # PI 中断追踪
sudo bash ex5-on-sn-observe.sh   # ON/SN 行为观察
sudo bash ex6-vcpu-migration.sh  # vCPU 迁移观察

# 步骤 4: 清理
sudo bash ../../scripts/vm/setup-vfio-vm.sh stop
```

---

## 环境搭建 (setup-vfio-vm.sh)

实验 3-6 需要一个运行中的 VM，且设备通过 VFIO 直通给 VM。

**脚本位置**: `../../scripts/vm/setup-vfio-vm.sh`

```bash
# 首次使用：构建内核和 rootfs
sudo bash ../../scripts/vm/setup-vfio-vm.sh build

# 启动 VM（自动绑定设备到 vfio-pci）
sudo bash ../../scripts/vm/setup-vfio-vm.sh start

# 查看状态
sudo bash ../../scripts/vm/setup-vfio-vm.sh status

# 停止 VM（自动恢复设备驱动）
sudo bash ../../scripts/vm/setup-vfio-vm.sh stop
```

**setup-vfio-vm.sh 做了什么：**
```
1. 将设备 0000:4b:00.0 从 virtio-pci 解绑
2. 绑定到 vfio-pci 驱动
3. 启动 QEMU，使用 -device vfio-pci,host=4b:00.0 直通设备
4. VM 内部可以看到直通设备并生成中断
5. Host 侧可以追踪 PI 中断投递过程
```

**在 VM 内部生成 I/O 负载：**
```bash
# 查看直通设备
lspci
cat /proc/interrupts

# 生成 I/O 负载（触发设备中断）
dd if=/dev/vda of=/dev/null bs=4k count=10000
```

---

## 前置条件

```bash
# 1. 确认 IOMMU 和中断重映射已启用
dmesg | grep -E "DMAR|remapping"

# 2. 确认 APICv 已启用
cat /sys/module/kvm_intel/parameters/enable_apicv

# 3. 确认测试设备存在
lspci -s 4b:00.0

# 4. 确认设备使用 MSI-X
cat /proc/interrupts | grep "4b:00.0"

# 5. 挂载 debugfs
mount | grep debugfs || sudo mount -t debugfs none /sys/kernel/debug

# 6. 确认内核和 initramfs 存在（VM 启动需要）
#    setup-vfio-vm.sh 优先用 scripts/images/ 下的镜像，内核缺失时回退到已编译的内核树
ls ../../scripts/images/initramfs.img
ls arch/x86/boot/bzImage
```

---

## 实验设备说明

```
设备: 0000:4b:00.0 (Virtio block device)
  · 类型: SCSI storage controller (virtio-blk)
  · 中断: MSI-X (7 个向量: config + 6 个 request queue)
  · IOMMU group: 35
  · 中断前缀: IR-PCI-MSIX (表示使用了中断重映射)
  
中断向量:
  IRQ 112: virtio1-config     (配置中断)
  IRQ 113: virtio1-req.0      (请求队列 0)
  IRQ 114: virtio1-req.1      (请求队列 1)
  IRQ 115: virtio1-req.2      (请求队列 2)
  IRQ 116: virtio1-req.3      (请求队列 3)
  IRQ 117: virtio1-req.4      (请求队列 4)
  IRQ 118: virtio1-req.5      (请求队列 5)
```

---

## 注意事项

- 所有实验需要 root 权限
- **实验 3-6 需要先运行 `setup-vfio-vm.sh start` 启动 VM**
- 设备直通会将设备从 Host 解绑，VM 停止后自动恢复
- 实验 4 会修改 kvm_intel 模块参数，需要重启 VM
- 实验 6 需要运行中的 VM 和 taskset 工具
- 如果 IOMMU debugfs 不可用，实验 2 会跳过 IRTE 表查看
