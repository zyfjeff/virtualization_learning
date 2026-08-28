# KVM Study - 测试环境构建指南

> 快速构建用于 KVM 学习的最小测试环境
>
> 包含：Linux 内核编译、rootfs 构建、虚拟机启动

---

## 🎯 快速开始

### 推荐方式（统一脚本）

```bash
cd scripts/testing

# 1. 编译内核 (约 5-15 分钟)
./build-kernel.sh

# 2. 构建 Ubuntu rootfs (推荐，包含所有测试工具)
sudo ./build-rootfs-ubuntu.sh

# 3. 启动虚拟机（统一启动脚本）
./boot-vm-unified.sh ubuntu

# 或使用自定义配置
./boot-vm-unified.sh ubuntu --memory 4G --cpus 4 --queues 4
```

**详细说明请参考：** [scripts/README.md](../../README.md)

### 传统方式（最小化环境）

```bash
cd scripts/testing

# 1. 编译内核 (约 5-15 分钟)
./build-kernel.sh

# 2. 构建最小 rootfs (Busybox, 约 1 分钟)
./build-rootfs.sh

# 3. 启动虚拟机
./boot-vm.sh
```

### 手动步骤

详见下方各脚本说明。

---

## 📁 脚本说明

### 推荐脚本（统一方案）

| 脚本 | 功能 | 耗时 | 依赖 |
|------|------|------|------|
| `build-kernel.sh` | 编译最小 Linux 内核 | 5-15 分钟 | gcc, make, bc |
| `build-rootfs-ubuntu.sh` | 构建 Ubuntu rootfs（推荐） | 5-10 分钟 | debootstrap |
| `build-rootfs-allinone.sh` | 构建 All-in-One rootfs | 1-2 分钟 | busybox-static |
| `boot-vm-unified.sh` | 统一启动脚本 | - | qemu-system-x86 |

### 传统脚本（最小化环境）

| 脚本 | 功能 | 耗时 | 依赖 |
|------|------|------|------|
| `build-rootfs.sh` | 构建最小 rootfs (Busybox) | < 1 分钟 | busybox-static |
| `boot-vm.sh` | 启动 KVM 虚拟机 | - | qemu-system-x86 |

**详细说明请参考：** [scripts/README.md](../../README.md)

---

## 🔧 安装依赖

### Ubuntu / Debian

```bash
# 编译工具链
sudo apt install build-essential bc flex bison libssl-dev

# Busybox (静态编译版本)
sudo apt install busybox-static

# QEMU
sudo apt install qemu-system-x86
```

### CentOS / RHEL / Fedora

```bash
# 编译工具链
sudo yum install gcc make bc flex bison openssl-devel

# Busybox
sudo yum install busybox

# QEMU
sudo yum install qemu-kvm
```

---

## 📖 详细使用说明

### 1. 编译内核

```bash
./build-kernel.sh [内核源码目录]

# 默认使用: /root/code/linux-6.12.93
# 输出: ../images/bzImage
```

**内核配置特点：**

- ✓ KVM 支持（Intel VMX + AMD SVM）
- ✓ 串口控制台（ttyS0）
- ✓ Virtio 设备支持
- ✓ Ftrace 追踪支持
- ✓ 最小化配置（编译快，体积小）

**输出文件：**

```
images/
├── bzImage          # 内核镜像 (~10-15 MB)
├── System.map       # 符号表（调试用）
└── kernel.config    # 内核配置文件
```

### 2. 构建 rootfs

```bash
./build-rootfs.sh [输出目录]

# 默认输出: ../images/initramfs.img
```

**rootfs 包含：**

- Busybox（静态编译，包含常用命令）
- KVM 测试程序（自动复制）
- 系统初始化脚本
- KVM 模块自动加载

**输出文件：**

```
images/
├── initramfs.img    # initramfs 镜像 (~2-5 MB)
└── rootfs/          # rootfs 目录（调试用）
```

### 3. 启动虚拟机

```bash
./boot-vm.sh [选项]

# 选项:
#   -m, --memory <MB>   内存大小 (默认: 512)
#   -c, --cpus <N>      CPU 数量 (默认: 2)
#   -n, --nested        启用嵌套虚拟化
#   -g, --gui           启用图形界面
#   -d, --debug         调试模式
```

**示例：**

```bash
# 默认启动 (512MB, 2 CPU, 串口)
./boot-vm.sh

# 1GB 内存, 4 CPU
./boot-vm.sh -m 1024 -c 4

# 启用嵌套虚拟化 (Guest 中可以运行 KVM)
./boot-vm.sh -n

# 图形界面
./boot-vm.sh -g

# 调试模式
./boot-vm.sh -d
```

**退出虚拟机：**

- 串口模式：`Ctrl-A X`
- 图形模式：关闭窗口 或 `Ctrl-Alt-2` → `quit`

---

## 🎮 虚拟机内测试

启动虚拟机后，可以运行以下测试程序：

### CPUID Faulting 测试

```bash
# 用户态测试
test-cpuid-fault

# KVM 虚拟机内测试（需要嵌套虚拟化）
test-cpuid-fault-kvm
```

### KVM API 演示

```bash
kvm-demo
```

### Mini KVM

```bash
insmod /root/mini-kvm.ko
dmesg | tail -20
```

---

## 🔍 调试技巧

### 查看 VM-Exit 统计

```bash
# 在虚拟机内
cat /sys/kernel/debug/kvm/vcpu_stat 2>/dev/null || \
  echo "需要 debugfs 挂载"

# 在 Host 上
sudo perf kvm stat record -- sleep 10
sudo perf kvm stat report
```

### 追踪 KVM 事件

```bash
# 挂载 debugfs
mount -t debugfs debugfs /sys/kernel/debug

# 启用 KVM 追踪
echo 1 > /sys/kernel/debug/tracing/events/kvm/enable

# 查看追踪输出
cat /sys/kernel/debug/tracing/trace_pipe
```

### 检查 CPU 虚拟化支持

```bash
# 检查 VMX/SVM
grep -E "vmx|svm" /proc/cpuinfo

# 检查 KVM 模块
lsmod | grep kvm

# 检查 CPUID Faulting
grep cpuid_fault /proc/cpuinfo
```

---

## 🐛 常见问题

### 问题1: /dev/kvm 不存在

```bash
# 检查 CPU 是否支持虚拟化
grep -E "vmx|svm" /proc/cpuinfo

# 加载 KVM 模块
sudo modprobe kvm
sudo modprobe kvm_intel  # Intel CPU
# 或
sudo modprobe kvm_amd    # AMD CPU

# 检查 BIOS 设置
# 确保 "Intel VT-x" 或 "AMD-V" 已启用
```

### 问题2: 内核编译失败

```bash
# 安装缺失的依赖
sudo apt install build-essential bc flex bison libssl-dev

# 清理旧的编译产物
cd /root/code/linux-6.12.93
make mrproper

# 重新配置和编译
./build-kernel.sh
```

### 问题3: QEMU 启动失败

```bash
# 检查 QEMU 安装
qemu-system-x86_64 --version

# 检查权限
ls -l /dev/kvm

# 添加用户到 kvm 组
sudo usermod -aG kvm $USER
# 需要重新登录生效
```

### 问题4: 虚拟机无法启动

```bash
# 使用调试模式
./boot-vm.sh -d

# 检查内核命令行
# 确保包含 console=ttyS0

# 尝试增加内存
./boot-vm.sh -m 1024
```

---

## 📊 内核配置说明

使用的内核配置 (`kernel-config`) 包含：

| 特性 | 说明 |
|------|------|
| `CONFIG_KVM=y` | KVM 核心支持 |
| `CONFIG_KVM_INTEL=y` | Intel VMX 支持 |
| `CONFIG_KVM_AMD=y` | AMD SVM 支持 |
| `CONFIG_SERIAL_8250_CONSOLE=y` | 串口控制台 |
| `CONFIG_VIRTIO_*=y` | Virtio 设备 |
| `CONFIG_FTRACE=y` | Ftrace 追踪 |
| `CONFIG_X86_MSR=y` | MSR 访问支持 |

**禁用的特性（减少编译时间）：**

- 图形驱动（DRM, VGA）
- USB 支持
- 额外文件系统（ext4, xfs, btrfs）
- 无线网络
- 不必要的调试选项

---

## 🎓 对应课程

这些测试环境用于以下课程：

- **Phase 0**: KVM 框架层 → `kvm-demo`, `mini-kvm`
- **Phase 1**: VT-x 基础 → `test-cpuid-fault`, `vmx-info`
- **Phase 2-10**: 内存/中断/设备虚拟化 → 嵌套虚拟化测试

---

## 📚 参考资料

- [Linux Kernel Documentation](https://www.kernel.org/doc/html/latest/)
- [KVM Documentation](https://www.kernel.org/doc/html/latest/virt/kvm/)
- [QEMU Documentation](https://www.qemu.org/docs/master/)
- [Busybox Documentation](https://busybox.net/)

---

## 📝 许可证

GPL-2.0

---

**作者**: KVM Study Project  
**日期**: 2026-06-29
