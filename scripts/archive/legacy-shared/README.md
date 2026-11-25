# legacy-shared — 旧 9p 暂存区归档（2026-06-29）

> 原位置：仓库根目录 `shared/`。已被 `scripts/shared/`（→ guest `/mnt/shared`）取代，
> 2026-11-25 整理时归档至此。**不要用于新实验**，仅作历史出处保留。

## 为什么保留这些源码

`phase10-performance/index.md` §2.2 的 B 级微基准数字（CPUID 约 1522 ns、RDTSC 约 10 ns）
出处是这里的 `ex5-vmexit-overhead.c`。**该版本与 `phase1-cpu-virt/practice/` 的同名文件不同**：
归档版多了 RDMSR IA32_TSC（透传）与 RDMSR IA32_EFER（拦截）两组测试。
引用实测数字时以本目录版本为准，不要用 phase1 版本顶替。

## 与 phase1 practice 版本的关系（2026-11-25 diff 核对）

| 文件 | 与 `phase1-cpu-virt/practice/` 同名源码 |
|---|---|
| `ex1-vmx-verify.c` | 不同（早期版本） |
| `ex2-cpuid-fault.c` | 相同 |
| `ex3-msr-test.c` | 不同（早期版本） |
| `ex5-vmexit-overhead.c` | 不同（多 RDMSR TSC/EFER 测试，见上） |

## 目录内容

- 4 个 `.c` 源码：git 跟踪，历史出处。
- 8 个 ELF（`check_vmx_basic`、`ex1-vmx-verify`、`ex2-cpuid-fault`、`ex3-msr-test`、
  `ex3-msr-test-dev`、`ex3-msr-test-simple`、`ex5-vmexit-overhead`、`test-cpuid-fault`）：
  2026-06 的编译产物，已 untrack（`.gitignore` 覆盖），仅磁盘保留。
  其中 `check_vmx_basic`、`ex3-msr-test-dev`、`ex3-msr-test-simple`、`test-cpuid-fault`
  在本目录没有对应源码，无法从此处重建。
