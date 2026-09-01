# 项目 4：性能对标

> 目标：把自己造的 VMM 放进真实基准里 —— 与 QEMU（及可选的
> Firecracker/Cloud Hypervisor）对比启动延迟、VM-Exit 分布与
> halt-polling 行为，用数据解释架构差异。

**前置**：项目 1-3；阅读 `../phase9-performance/README.md`
（halt-polling/PLE/VPID）、`../phase10-debugging/README.md`
（trace 方法）、`../phase11-microvm/README.md`（MicroVM 启动路径）。

---

## M1：启动延迟

- 测量定义：从 VMM 进程启动到 guest shell 第一行输出
  （串口时间戳或宿主侧 `KVM_EXIT_IO` 首字节）。自制 VMM、
  QEMU `-machine microvm`（`scripts/vm/boot-vm.sh` 可作基线，
  注意它默认 `-enable-kvm -cpu host`）、有条件再加 Firecracker。
- 各跑 ≥10 次取中位数；按阶段拆分耗时：KVM 初始化 / 内核解压 /
  驱动探测 / initramfs。拆分手段：宿主侧
  `kvm:kvm_exit` tracepoint + guest `dmesg` 时间戳。
- 对照 `../phase11-microvm/README.md`：MicroVM 砍掉哪些设备、
  为什么能缩短启动路径；你的 VMM 天然精简，对比能说明
  "设备模型大小"与"启动延迟"的关系。

## M2：VM-Exit 分布

同一负载（如启动到 shell、guest 内 `stress-ng --cpu 1`）下：

```bash
perf kvm stat record -- sleep 30 && perf kvm stat report
# 或逐事件:
perf record -e kvm:kvm_exit -a -- sleep 10
```

- 对比自制 VMM（串口/virtio-mmio）与 QEMU（virtio-pci + 全套设备）：
  谁的 IO 退出多、halt 退出占比、外部中断占比
- 把 exit 类型映射回源码（`kvm:kvm_exit` 的 reason 枚举与
  `../phase10-debugging/` 中的 tracepoint 目录），解释每类退出
  在你的实现里对应哪段代码
- 若做了项目 3：直通设备负载下退出分布应接近"只剩调度相关退出"，
  验证数据面下沉的效果（与项目 2 M3 的 irqfd 前后对比呼应）

## M3：halt-polling 调参实验

KVM 在 vCPU halt 时先忙等再真正睡眠，窗口由模块参数控制
（`virt/kvm/kvm_main.c:78-95`）：

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `halt_poll_ns` | `KVM_HALT_POLL_NS_DEFAULT` | 忙等窗口上限 |
| `halt_poll_ns_grow` | 2 | 命中时窗口倍增系数 |
| `halt_poll_ns_grow_start` | 10000 (10μs) | 增长起点 |
| `halt_poll_ns_shrink` | 2 | 未命中时缩减系数 |

实验：

1. `echo 0 > /sys/module/kvm/parameters/halt_poll_ns` 与默认值对比：
   空转功耗（`halt` 退出计数、宿主 CPU 占用）与唤醒延迟
   （guest 内定时器/网络 ping 延迟）的此消彼长
2. 用 `kvm:kvm_halt_poll_ns` tracepoint 观察窗口增长/缩减轨迹
   （方法见 `../phase9-performance/README.md` 与
   `../phase10-debugging/` 的 trace 章节）
3. 结论写进报告：什么负载该开/关、窗口该多大

## M4：报告

按仓库报告模板（环境 / 方法 / 数据 / 源码对应 / 优化思考）产出，
重点回答：

- 你的 VMM 与 QEMU 的差距主要在哪个环节？是设备模型、
  退出处理路径，还是根本没差（因为数据面都下沉了）？
- 哪些 phase9 的优化（halt-polling、PLE、VPID、大页）在
  你的场景里实际可测出收益？

---

## 验收标准

- [x] 启动延迟对比表（≥3 组实现、≥10 次采样、中位数 + 阶段拆分）
      —— 4 实现 ×10 次 + 8250/i8042 两段拆分与 tuned 阴性对照，
      `practice/README.md` 项目 4 M1；数据 `practice/bench/boot-20260901-095559/`
- [x] 相同负载下至少 2 组实现的 VM-Exit 分布对比，每类退出
      有源码级解释 —— 3 实现 ×（boot/idle/busy）+ 直通负载，
      含 PIC tick 4 次 PIO 的 ftrace 解码；数据 `practice/bench/exits-20260901-101641/`
      与 `exits-pt-20260901-112353/`
- [x] halt-polling 调参曲线（窗口值 × 延迟/开销），结论有数据支撑
      —— poll-on/off ×（idle/flood）+ 50/100/200µs 固定窗口扫描，
      数据 `practice/bench/halt-20260901-110933/`、`halt-sweep-20260901-111527/`
- [x] 报告能指出"自己实现与成熟 VMM 的真实差距"，而不是复述文档
      —— 结论：差距在固件信息面（ACPI/PNP/MP 表），不在设备模型或
      退出处理路径；见 `practice/README.md` 项目 4 M4

---

## 参考资料

- `../phase9-performance/README.md`、`../phase10-debugging/README.md`、
  `../phase11-microvm/README.md`
- `perf kvm` 用法：`man perf-kvm`；tracepoint 目录
  `/sys/kernel/tracing/events/kvm/`
- QEMU microvm：`/root/code/qemu-10.1.0-rc2/hw/i386/microvm.c`
