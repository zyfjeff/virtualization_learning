# Phase N 实践练习

> 统一学习循环第 5 步（动手验证）。实验编号 `exN-<slug>` 连续；
> 实验报告/记录放 `reports/`；被取代的文档直接删除（git 历史即归档，不设 archive/）。
> 测量纪律（重复/噪声/清场）遵循 `../../phase10-performance/measurement.md`。

## 📋 实验总览

| 编号 | 主题 | 验证什么 | 环境 | 预期现象 |
|------|------|---------|------|---------|
| ex1 | <主题> | <对应学习目标> | 宿主 / VM / 直通VM | <可判定的预期> |

## 环境准备

```bash
# 标准 VM（默认已带 -enable-kvm -cpu host，见 scripts/vm/boot-vm.sh）
cd ../../scripts/vm && ./boot-vm.sh ubuntu --memory 4G --cpus 4

# 需要 vIOMMU / 直通时追加（示例）：
# ./boot-vm.sh ubuntu --net user --qemu "-machine q35,kernel-irqchip=split -device intel-iommu,intremap=on"
```

<!-- 确认 KVM 加速（AGENTS.md 陷阱 #7）：
     ls -l /proc/$(pgrep -f '^qemu-system-x86_64')/fd | grep -c kvm   # >0 才是 KVM -->

## 运行

```bash
./ex1-<slug>.sh
# C 程序：make && ./exN-<slug>
```

## 清理纪律

<!-- tracefs 是全局状态（AGENTS.md 硬约束 2）：set_event / set_ftrace_filter /
     set_event_pid 三个文件带 O_TRUNC 的写会清掉全部已有配置。
     加用 >>，清场要显式写并注明。实验结束恢复设备安全绑定（陷阱 #15）。 -->

```bash
# 显式清场示例（按实验实际用到的状态写全）
echo 0 > /sys/kernel/tracing/tracing_on
: > /sys/kernel/tracing/set_event
: > /sys/kernel/tracing/set_ftrace_filter
```

## 报告

实验记录与结论放 [`reports/`](reports/)；实测性能数字按 AGENTS.md 分级规则
汇入 `../../phase10-performance/index.md`，此处只留过程记录与指针。
