# Phase N 实践练习 — 理论取证链

> 统一学习循环第 5 步。实验是对课程结论**取证**，不是工具观光。
> 每个实验按五段式（COURSE_FRAMEWORK §3.5）：理论 → 预测 → 操作 → 对照 → 反馈。
> 脚本先打印理论与预测，再输出观察，自动给 PASS / FAIL / INCONCLUSIVE。
> 实验编号 `exN-<slug>` 连续；报告放 `reports/`；被取代文档直接删除（git 历史即归档）。
> 测量纪律遵循 `../../phase10-performance/measurement.md`。

## 📋 实验总览（取证链）

<!-- 每个实验聚焦一条可证伪的课程结论；exN 的结论引出 exN+1 的问题 -->

| 编号 | 验证的理论（来源） | 预测 | 对照判据 | 入口 |
|------|------------------|------|---------|------|
| ex1 | <结论>（annotations §n / 规范章节） | <理论成立必然观察到什么> | <如何自动/人工判定 PASS/FAIL> | `ex1-<slug>.sh` |

**链式衔接**：ex1 <结论> → ex2 <问题> → … → 下一 phase。

## 环境准备

```bash
# 标准 VM（默认已带 -enable-kvm -cpu host，见 scripts/vm/boot-vm.sh）
cd ../../scripts/vm && ./boot-vm.sh ubuntu --memory 4G --cpus 4
# 宿主物理机实验（如 PCIe 拓扑取证）注明在宿主执行，不是 VM
```

<!-- 确认 KVM 加速（AGENTS.md 陷阱 #7）：
     ls -l /proc/$(pgrep -f '^qemu-system-x86_64')/fd | grep -c kvm   # >0 才是 KVM -->

## 运行

```bash
sudo ./ex1-<slug>.sh        # 需要 root 时注明
# C 程序：make && ./exN-<slug>
```

## 安全与清理纪律

<!-- 只读取证优先；危险写操作遵守 AGENTS.md 陷阱 #15（不写活设备 BAR、不随意 unbind）。
     tracefs 是全局状态（AGENTS.md 硬约束 2）：set_event / set_ftrace_filter /
     set_event_pid 带 O_TRUNC 的写会清掉全部已有配置；加用 >>，清场显式写并注明。
     实验结束恢复设备安全绑定。 -->

```bash
# 显式清场示例（按实验实际用到的状态写全）
echo 0 > /sys/kernel/tracing/tracing_on
: > /sys/kernel/tracing/set_event
: > /sys/kernel/tracing/set_ftrace_filter
```

## 本机实测发现

<!-- 记录 PASS/FAIL 汇总与真实发现（理论简化的边界、quirk 实证等）。
     概念性边界/待核查 → corrections.md；实测性能数字 → phase10 index 分级。 -->

## 报告

实验记录与结论放 [`reports/`](reports/)；实测性能数字按 AGENTS.md 分级规则汇入
`../../phase10-performance/index.md`，此处只留过程记录与指针。
