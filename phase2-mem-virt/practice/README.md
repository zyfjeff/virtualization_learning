# Phase 2 实践练习

> EPT 和内存虚拟化实践练习

---

## 练习列表

| 编号 | 练习名称 | 需要 VM | 难度 | 预计时间 | 核心知识点 |
|------|---------|---------|------|---------|-----------|
| 1 | EPT Violation 演示 | 是 | ★★☆ | 20min | EPT 缺页处理 |
| 2 | 内存类型分析 | 是 | ★★☆ | 20min | 内存类型映射 |

---

## 快速开始

```bash
# 启动 VM（所有练习都需要）
sudo bash /root/code/kvm-study/scripts/setup-vm.sh start

# 在另一个终端运行练习
sudo bash ept_violation_demo      # EPT Violation 演示
sudo bash memtype_analysis        # 内存类型分析

# 清理
sudo bash /root/code/kvm-study/scripts/setup-vm.sh stop
```

---

## 练习详情

### 练习 1: EPT Violation 演示

**目标**: 理解 EPT Violation 的处理流程

**方法**:
```bash
# 运行演示程序
sudo ./ept_violation_demo

# 在另一个终端观察 EPT Violation
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_page_fault/enable
cat /sys/kernel/debug/tracing/trace_pipe | grep kvm_page_fault
```

**预期输出**:
```
kvm_page_fault: address 0x7f1234560000 error_code 0x2
kvm_mmu_get_page: gfn 0x123456 role 0x1
kvm_tdp_mmu_map: gfn 0x123456 spte 0x8000000123456707
```

**关注点**:
- EPT Violation 的触发条件
- 页表映射的建立过程
- SPTE 的值和含义

---

### 练习 2: 内存类型分析

**目标**: 理解不同内存类型的映射方式

**方法**:
```bash
# 运行分析程序
sudo ./memtype_analysis

# 在另一个终端观察内存类型
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_mmio/enable
cat /sys/kernel/debug/tracing/trace_pipe | grep kvm_mmio
```

**关注点**:
- MMIO 区域的识别
- 内存类型的设置（WB/UC/WC）
- MMIO 访问的性能影响

---

## 统一测试环境

所有练习使用统一的 VM 启动脚本：

- **启动脚本**: `/root/code/kvm-study/scripts/setup-vm.sh`
- **功能**: 自动构建内核和 rootfs，启动 VM
- **详细说明**: 参见 `/root/code/kvm-study/scripts/testing/README-UNIFIED.md`

---

## 故障排查

### 问题 1: 无法访问 debugfs

```bash
# 挂载 debugfs
sudo mount -t debugfs none /sys/kernel/debug
```

### 问题 2: 看不到 EPT Violation

```bash
# 确保 KVM 模块已加载
lsmod | grep kvm

# 确保 tracepoint 已启用
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_page_fault/enable

# 在 VM 内执行内存操作触发 EPT Violation
# 例如：mmap + 访问内存
```

### 问题 3: 编译错误

```bash
# 确保安装了内核头文件
sudo apt install linux-headers-$(uname -r)

# 重新编译
make clean
make
```

---

## 参考资料

- Phase 2 README: `/root/code/kvm-study/phase2-mem-virt/README.md`
- EPT 规范: Intel SDM Volume 3C Chapter 28
- KVM 源码: `/root/code/linux-6.12.93/arch/x86/kvm/mmu/`
