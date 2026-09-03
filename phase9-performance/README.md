# Phase 9：性能测量与跨机制开销

> 基于 Linux 6.12.93 源码 | 实测宿主：96 线程裸金属，内核 6.8.0-51-generic，
> QEMU 10.1.0-rc2。★ 宿主与参考内核**不是同一棵树**，参数默认值可能不同 ——
> 每次引用默认值前先做 [`measurement.md`](measurement.md) §5 第 2 条的"参数实存性自检"。

---

## 0. 本章只负责三件事

| 职责 | 载体 | 为什么归这里 |
|---|---|---|
| **① 测量的可信性规范** | [`measurement.md`](measurement.md) | 全仓唯一一份"这个性能结论能不能信"的判据：对照组、样本量、噪声控制、观测手段的分辨率与**自身扰动**、归因纪律、报告规范。工具用法与排查树在 `../phase10-debugging/`，两章的分工见 `measurement.md` §0 |
| **② 三个独占机制** | [`annotations.md`](annotations.md) §1/§2/§3 + [`parameters.md`](parameters.md) | 只收"**只有跨机制权衡视角才看得清**"的东西：PLE 的价值 = 超卖调度、PML 的成本 = 迁移与退出的交叉、主时钟 = 调度迁移与时钟的耦合。判据与"为什么别的机制不在这"见 `annotations.md` §4 |
| **③ 跨 phase 性能结论索引** | [`index.md`](index.md) | 各章实测数字统一登记 + A/B/C/D 评级。规则是**别处只写指针、不复制数字**（副本会随重测过期），见 `index.md` §6 |

发现本章或别处写错的性能事实，一律记进 [`corrections.md`](corrections.md)
（A 机制错 / B 数字无据 / C 源码核查纠正 / D 命令在本版本上不可执行 / E 结构性问题），
并同步改原文。

## 1. 文件清单与建议读序

| 顺序 | 文件 | 内容 |
|---|---|---|
| 1 | [`measurement.md`](measurement.md) | §1 一个结论的最小可辩护形态 → §2 重复与统计纪律 → §3 噪声控制 → §4 观测分辨率与扰动预算（★ 表待 E5 填）→ §5 开跑前自检 → §6 归因纪律 → §7 本仓踩过的陷阱 → §8 报告规范 |
| 2 | [`parameters.md`](parameters.md) | halt-polling / PLE / 定时器 / TSC / MMU 与大页 / APICv 的参数、默认值、**权限**（PLE 一组全只读）、能否在一轮实验里连续扫 |
| 3 | [`annotations.md`](annotations.md) | 三个独占机制的 6.12.93 源码走读；§4 是"旧版内容 → 现在归哪个 phase"的去处对照 |
| 4 | [`practice/`](practice/README.md) | 五个实验 E1–E5，各带一个可跑脚本；目录、通用前置与负载模块见该页 |
| 5 | [`index.md`](index.md) | 全仓性能结论的索引与评级，§5 列出"关心但完全没有数据"的空白 |

## 2. 本章的当前状态

**本轮决定不上机**：五个实验全部产出"设计 + 可跑脚本"，效果数字一律标 *待实测*。
已经落地的非实测成果有两类 —— 源码核查纠正（`corrections.md` C16/C17、D1–D8），
以及 preflight / 自测能自己抓住的那批静默错数缺陷。

三条硬规矩，在实测补齐之前不得绕过：

1. **禁止写"开 trace 的代价可以忽略"** —— 扰动预算表（`measurement.md` §4(b)）
   还整张待填，E5 就是为填它而设计的（`index.md` §6 规则 4）。
2. **`tracing_on=0` 不是零开销基线** —— static key 只在注册/注销探针时翻，
   记录路径照样跑一遍触发检查与缓冲预留（`corrections.md` C17）。
3. **引用默认值前先实读** —— 本仓已被三个"看着像默认值"的错绊倒
   （`halt_poll_ns` 400000、`ple_window_shrink` 2、`ple_window_max` 16384，
   见 `corrections.md` D 级与 `index.md` §4）。

## 3. 上手

```bash
cd practice
./bench-observer-cost.sh --preflight     # 只读：环境是否满足硬前置
./bench-observer-cost.sh --all --dry-run # 打印整条时间线，不碰系统
sudo ./bench-observer-cost.sh --arms O0,O2,O3,O0e --sample-s 20
```

五个脚本一律支持 `--preflight` / `--dry-run`，参数看 `-h`；guest 负载模块
`ple-load/` 在宿主编译后经 9p 送进 guest。VM 用
`../scripts/vm/boot-vm.sh`（默认带 `-enable-kvm -cpu host` 并自检 ——
缺 `-enable-kvm` 时 QEMU **静默**回退 TCG，所有退出类测量都会归零）。
