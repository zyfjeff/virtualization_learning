# Phase 4 实践练习 — 理论取证链

> 统一学习循环第 5 步。实验不是工具观光，是**对课程结论取证**：
> 每个实验按五段式（COURSE_FRAMEWORK §3.5）——理论 → 预测 → 操作 → 对照 → 反馈。
> 先押注预测，再看证据，脚本自动给 PASS / FAIL / INCONCLUSIVE。
> 测量纪律遵循 `../../phase10-performance/measurement.md`。

## 📋 实验总览（取证链）

四个实验串成一条链，exN 的结论引出 exN+1 的问题：

| 编号 | 验证的理论（来源） | 预测 | 对照判据 | 入口 |
|------|------------------|------|---------|------|
| ex1 | bus 号递归层级分配（annotations §1-2） | primary<secondary≤subordinate；子树最远 bus==subordinate；secondary 唯一 | 逐 bridge 读 0x18/19/1a，BFS 子树自动比对 | `ex1-pcie-topology.sh` |
| ex2 | Type 0/1 布局 + BAR 编码（basics §3-4） | BAR0 bit0↔区域类型；bridge 0x18-1a 是 bus 号；64 位 BAR 占两槽 | 解码 BAR0 与 lspci 区域对照 | `ex2-config-space.sh` |
| ex3 | ACS 三层判定（annotations §3 + AGENTS.md #11/#13） | Ctl⊇(Cap∩REQ)；Intel RCiEP 无 cap 独占 group；quirk 优先于多功能聚合 | 扫 ACS cap/ctl + group 对照，P3a/P3b 双向 | `ex3-acs-analysis.sh` |
| ex4 | group=最小隔离集（phase3 group.md） | 多成员 group 可归因到隔离缺口；单成员可单独直通 | 逐 group 归因（同 slot/同 bus/非 ACS bridge 下游） | `ex4-iommu-group.sh` |

**链式衔接**：ex1 拓扑就位 → ex2 配置空间承载能力 → ex3 ACS 怎么判定隔离 → ex4 判定怎么落成 group 边界 → phase7 VFIO 真正直通。

## 环境准备

```bash
# 工具
command -v lspci setpci || sudo apt install pciutils
# IOMMU（ex3/ex4 需要）：grub 加 intel_iommu=on 或 amd_iommu=on
ls /sys/kernel/iommu_groups >/dev/null && echo "IOMMU 已启用"
# 全部实验需 root（读配置空间 / ACS 扩展能力 / iommu_group）
```

> 本实验在**宿主物理机**执行（不是 VM）——取证对象是真实 PCIe 拓扑与 IOMMU 分组。
> 宿主设备差异会让某些预测 INCONCLUSIVE（如缺多功能非 ACS 设备），这是诚实结果，不是失败。

## 运行

```bash
sudo ./ex1-pcie-topology.sh
sudo ./ex2-config-space.sh          # 可加 BDF 指定样本：sudo ./ex2-config-space.sh 01:00.0
sudo ./ex3-acs-analysis.sh
sudo ./ex4-iommu-group.sh
```

## 安全纪律

- **只读取证**：四个实验都不写寄存器。绝不写 BAR、不 unbind 驱动、不动 ACS Control
  （写 BAR/unbind 的挂死风险见 AGENTS.md 陷阱 #15）。
- BAR 大小探测（写全 1 读回掩码）只讲原理，不在活设备执行。
- 要改 ACS 配置做对照实验，用内核参数 `pci=config_acs=`（AGENTS.md #13，注意从右往左解析），
  不要 setpci 直写活设备。

## 本机实测发现（2026-11-25，96 线程宿主）

- **ex1**：142 PASS / 0 FAIL，bus 号递归分配不变量成立。
  **发现**：17:00.0 / 02:00.0 class=0604（bridge）但 config[0x0e] bit7=0
  （setpci+sysfs 双路核实）——"Type1 头=bridge" 是简化，header type bit 不是可靠 bridge 判据。
  内核如何调和 hdr_type 与 class 待核查（corrections.md 勘误 12）。
- **ex3**：P1/P2/P3a/P3b 全 PASS。50 个 ACS cap 设备 Ctl⊇(Cap∩REQ)（#13 实证）；
  大量无 cap Intel RCiEP 独占 group（#11 quirk 实证）；无 ACS 多功能本该同 group，
  命中 quirk 的被单独隔离（P3a，quirk 优先于多功能聚合的实证）。
- **ex4**：29 个多成员 group 全部归因到隔离缺口；193 个单成员 group。
  **核心问题的答案**：157 个设备落在多成员 group，必须整组直通——隔离粒度由拓扑+ACS 物理决定。
  group 101 = ASPEED PCI-to-PCI bridge + 下游设备（PCI-to-PCI bridge 永不实现 ACS，Spec §6.12.1）。

## 报告

实验记录与结论放 [`reports/`](reports/)；实测性能数字按 AGENTS.md 分级规则汇入
`../../phase10-performance/index.md`，此处只留过程记录与指针。
