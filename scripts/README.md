# scripts/ —— 构建并启动实验 VM

本目录是本项目**唯一**的实验环境入口：构建内核与 rootfs、启动一台可做 KVM 实验的 VM，并提供宿主侧观测脚本。各 phase 的 `practice/` 都假定 VM 由这里启动。

```
scripts/
├── vm/        构建与启动实验 VM
├── trace/     宿主侧观测（ftrace / perf）
├── images/    构建产物（已 gitignore）
├── shared/    9p 共享暂存区 → guest /mnt/shared
└── archive/   已弃用脚本与历史文档
```

---

## 快速开始

```bash
cd scripts/vm

# 1. 编译内核（约 5-15 分钟，配置见 kernel-config）
./build-kernel.sh

# 2. 构建 rootfs（选一个）
sudo ./build-rootfs-ubuntu.sh      # 推荐：Ubuntu，工具齐全
sudo ./build-rootfs-minimal.sh     # 最快：busybox，秒级

# 3. 启动
./boot-vm.sh ubuntu --memory 4G --cpus 4 --queues 4
```

退出 VM：`Ctrl-A` 然后 `X`。

---

## 关于 KVM 加速（重要）

`boot-vm.sh` **默认传 `-enable-kvm -cpu host`**。这两个参数不是可选项：

- 缺 `-enable-kvm`，QEMU 会静默回退到 TCG 纯软件模拟。此时宿主侧 `kvm:kvm_exit`、`kvm:kvm_entry` 等 tracepoint **不会产生任何事件**，`trace/` 下的脚本全部采不到数据。
- 缺 `-cpu host`，QEMU 用默认 `qemu64` 模型，guest 内看不到 VMX（表现为 `VMX: 0 CPUs with VMX support`），phase1 的 VT-x 实验无法进行。

脚本会在启动前自检 `/dev/kvm` 是否存在且可写，并检查宿主的嵌套虚拟化开关：

```bash
# guest 内要看到 VMX，宿主必须开启嵌套虚拟化
cat /sys/module/kvm_intel/parameters/nested        # 期望 Y
modprobe -r kvm_intel && modprobe kvm_intel nested=1
```

确认某次运行真的走了 KVM：

```bash
ls -l /proc/$(pgrep -f '^qemu-system-x86_64')/fd | grep -c kvm   # > 0 表示走 KVM，0 表示 TCG
```

需要跑 TCG 做对比时用 `--tcg` 显式声明，脚本会打印告警。

---

## vm/ 各脚本

| 脚本 | 功能 | 耗时 | 依赖 |
|------|------|------|------|
| `build-kernel.sh` | 编译内核，输出到 `../images/` | 5-15 分钟 | gcc, make, bc, flex, bison, libssl-dev |
| `build-rootfs-ubuntu.sh` | Ubuntu rootfs（推荐） | 5-10 分钟 | debootstrap（需联网） |
| `build-rootfs-allinone.sh` | busybox + 宿主机测试工具 | 1-2 分钟 | busybox-static |
| `build-rootfs-minimal.sh` | 最小 busybox initramfs | < 1 分钟 | busybox-static, cpio |
| `build-rootfs-iperf.sh` | 网络性能专用（phase4） | 1-2 分钟 | busybox-static, iperf3 |
| `boot-vm.sh` | 启动实验 VM | - | qemu-system-x86 |
| `setup-vfio-vm.sh` | 带 VFIO 设备直通的 VM（phase3/phase5） | - | qemu, vfio-pci |
| `test-in-vm.sh` | VM 内跑的测试脚本 | - | - |

安装依赖：

```bash
# Ubuntu / Debian
sudo apt install build-essential bc flex bison libssl-dev \
                 busybox-static cpio debootstrap qemu-system-x86

# CentOS / RHEL / Fedora
sudo yum install gcc make bc flex bison openssl-devel busybox qemu-kvm
```

---

## boot-vm.sh 用法

```bash
./boot-vm.sh [镜像类型] [选项]
```

**镜像类型**（决定读哪个 initramfs）

| 类型 | 镜像文件 | 构建脚本 |
|------|---------|---------|
| `ubuntu` | `images/initramfs-ubuntu.img` | `build-rootfs-ubuntu.sh` |
| `allinone` | `images/initramfs-allinone.img` | `build-rootfs-allinone.sh` |
| `minimal` | `images/initramfs.img` | `build-rootfs-minimal.sh` |

**选项**

| 选项 | 默认 | 说明 |
|------|------|------|
| `--memory <size>` | 2G | 内存大小 |
| `--cpus <num>` | 2 | vCPU 数量 |
| `--queues <num>` | 1 | virtio-net 队列数（仅 `--net tap`） |
| `--net <type>` | tap | `tap` / `user` / `none` |
| `--gui` | 关 | 图形界面（默认走串口） |
| `--debug` | 关 | 开 GDB server 并暂停等待连接 |
| `--tcg` | 关 | 回退 TCG 纯软件模拟 |
| `--qemu "<args>"` | - | 透传额外参数给 qemu |

示例：

```bash
./boot-vm.sh ubuntu --memory 8G --cpus 8 --queues 8   # 性能测试
./boot-vm.sh ubuntu --net user                        # 免 root 创建 TAP，SSH 走 localhost:2222
./boot-vm.sh minimal --debug                          # 另开终端 gdb → target remote :1234
./boot-vm.sh allinone --qemu "-machine q35"           # 透传任意 qemu 参数
```

`--net tap` 需要 root 创建 TAP 设备；`--net user` 不需要但性能低。

---

## 9p 共享目录

`boot-vm.sh` 总会把 `scripts/shared/` 以 `mount_tag=hostshare` 导出，rootfs 的 `/init` 会将它挂到 guest 的 `/mnt/shared`。

好处是测试程序在宿主机编译、VM 内直接运行，改完立刻生效，不用重建 initramfs：

```bash
# 宿主机
cp phase2-mem-virt/practice/memtest scripts/shared/

# guest 内
/mnt/shared/memtest
```

依赖内核选项 `CONFIG_NET_9P=y` 与 `CONFIG_9P_FS=y`（`vm/kernel-config` 已包含）。手工挂载：

```bash
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/shared
```

---

## 内核配置

`vm/kernel-config` 是一份最小化配置，关键项：

| 选项 | 用途 |
|------|------|
| `CONFIG_KVM=y` / `CONFIG_KVM_INTEL=y` / `CONFIG_KVM_AMD=y` | 嵌套 KVM（guest 内再跑 VM） |
| `CONFIG_KVM_VFIO=y` | VFIO 设备直通 |
| `CONFIG_SERIAL_8250_CONSOLE=y` | 串口控制台（ttyS0） |
| `CONFIG_VIRTIO_PCI/BLK/NET=y` | virtio 设备 |
| `CONFIG_NET_9P=y` / `CONFIG_9P_FS=y` | 9p 共享目录 |
| `CONFIG_X86_MSR=y` | 用户态读写 MSR |
| `CONFIG_FTRACE=y` | ftrace 追踪 |

为缩短编译时间，图形驱动（DRM/VGA）、USB、ext4/xfs/btrfs、无线网络均已关闭。

---

## trace/ 观测脚本

在**宿主机**上运行，需要 VM 正在走 KVM（见上文自检方法）。

| 脚本 | 观测对象 |
|------|---------|
| `trace-vmexit.sh` | VM-Exit 原因分布与频率 |
| `trace-page-fault.sh` | EPT violation / page fault |
| `trace-irq-inject.sh` | 中断注入路径 |
| `trace-vfio.sh` | VFIO 设备直通相关事件 |
| `kvm-overview.sh` | perf 视角的 KVM 整体开销 |
| `iommu-analysis.sh` | IOMMU / 中断重映射 |

```bash
sudo ./trace/trace-vmexit.sh -p $(pgrep -f '^qemu-system-x86_64') -d 10
```

---

## 故障排查

### Kernel panic: VFS: Unable to mount root fs

initramfs 里缺少可执行的 `/init`，内核找不到启动入口，于是退回去尝试挂载根文件系统并失败。

```bash
# 确认镜像里有 /init
zcat images/initramfs-allinone.img | cpio -t | grep '^init$'
```

没有输出说明镜像是旧的构建产物（各 `build-rootfs-*.sh` 现在都会生成 `/init`），重新构建即可。

### guest 内 `VMX: 0 CPUs with VMX support`

两种原因：启动时没传 `-cpu host`（用本目录的 `boot-vm.sh` 即可，它默认会传），或宿主机没开嵌套虚拟化（见上文「关于 KVM 加速」）。

### trace 脚本采不到任何事件

先确认 VM 真的走了 KVM 而不是 TCG，方法见上文。

### 无法创建 TAP 设备

`RTNETLINK answers: Operation not permitted` —— 用 `sudo` 运行，或改用 `--net user`。

### 镜像 / 内核不存在

按报错提示跑对应的构建脚本；`boot-vm.sh` 会直接告诉你该跑哪一个。

---

## archive/

存放已弃用的脚本与历史文档，**不要用于新实验**。弃用原因见 [archive/README.md](archive/README.md)。

---

## 参考

- [Phase 4 virtio 实验](../phase4-virtio/README.md)
- [Virtio 规范](https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.html)
- [QEMU 文档](https://www.qemu.org/docs/master/)
