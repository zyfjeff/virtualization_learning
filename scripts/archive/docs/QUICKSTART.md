# KVM Study 测试环境

> 一键构建用于 KVM 学习的最小测试环境

---

## 🚀 快速开始

```bash
# 一键构建（推荐）
cd scripts/testing
./setup.sh

# 启动虚拟机
./boot-vm.sh
```

---

## 📁 文件说明

| 文件 | 说明 |
|------|------|
| `setup.sh` | 一键设置脚本（安装依赖 + 编译 + 构建） |
| `build-kernel.sh` | 编译最小 Linux 内核 |
| `build-rootfs.sh` | 构建最小 rootfs (Busybox) |
| `boot-vm.sh` | 启动 KVM 虚拟机 |
| `kernel-config` | 最小内核配置文件 |

---

## 🎯 输出文件

构建完成后生成：

```
images/
├── bzImage          # Linux 内核 (~10-15 MB)
├── initramfs.img    # 最小 rootfs (~2-5 MB)
├── System.map       # 内核符号表（调试用）
└── kernel.config    # 内核配置备份
```

---

## 🔧 手动构建

### 步骤 1: 编译内核

```bash
./build-kernel.sh /path/to/linux-6.12.93

# 耗时: 5-15 分钟
# 输出: ../images/bzImage
```

### 步骤 2: 构建 rootfs

```bash
./build-rootfs.sh

# 耗时: < 1 分钟
# 输出: ../images/initramfs.img
```

### 步骤 3: 启动虚拟机

```bash
./boot-vm.sh              # 默认: 512MB, 2 CPU
./boot-vm.sh -m 1024      # 1GB 内存
./boot-vm.sh -n           # 启用嵌套虚拟化
./boot-vm.sh -g           # 图形界面
```

---

## 📚 详细文档

查看 [scripts/README.md](../../README.md) 获取完整使用说明。

---

## 🎓 对应课程

- Phase 0: KVM 框架层测试
- Phase 1: VT-x 基础测试
- Phase 2-10: 内存/中断/设备虚拟化测试

---

## 📝 许可证

GPL-2.0
