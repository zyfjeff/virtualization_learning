# archive/ —— 已弃用的脚本与文档

这里的东西**不要用于新实验**，保留仅为追溯历史。当前可用的路径见 [../README.md](../README.md)。

## 脚本

| 文件 | 原路径 | 弃用原因 |
|------|--------|---------|
| `boot-vm-initramfs.sh` | `scripts/testing/boot-vm.sh` | 用 `-kernel bzImage` 相对路径，依赖 `images/bzImage`（需先跑 `build-kernel.sh` 才会生成）；还会拷贝已删除的 phase1 练习二进制。被 `vm/boot-vm.sh` 取代 |
| `boot-vm-ext4.sh` | 同上目录 | 同样依赖 `images/bzImage`；ext4 根文件系统方案已不再使用 |
| `boot-vm-9p.sh` | 同上目录 | 依赖不存在的 `images/rootfs.ext4`；9p 共享已并入 `vm/boot-vm.sh` 默认行为 |
| `build-rootfs.sh` | 同上目录 | 输出路径 `images/initramfs.img` 与 `build-rootfs-minimal.sh` **冲突**，两者互相覆盖。功能被 `build-rootfs-ubuntu.sh` / `-allinone.sh` 取代 |
| `build-rootfs-ext4.sh` | 同上目录 | 配套 ext4 方案，一并弃用 |
| `setup.sh` | 同上目录 | 一键脚本，内部调用已弃用的 `build-rootfs.sh`。改为按 `../README.md` 的三步手工执行 |

## 文档

`docs/` 下 5 份文档内容大量重叠且互相矛盾（`MIGRATION-GUIDE.md` 与 `README-old.md` 各自建议删掉对方提到的脚本），已合并为 [../README.md](../README.md)：

| 文件 | 说明 |
|------|------|
| `README-old.md` | 原 `testing/README.md`，依赖清单与内核配置表已并入新 README |
| `QUICKSTART.md` | 描述的是 `boot-vm.sh` 旧用法 |
| `MIGRATION-GUIDE.md` | 旧脚本 → 统一脚本的迁移说明，迁移已完成 |
| `TEST_REPORT.md` | 早期测试记录；其中「initramfs 缺 /init」与「需要 `-cpu host`」两条排查记录已并入新 README 的故障排查 |
| `FINAL_VERIFICATION.md` | 早期验证记录；9p 共享配方已并入新 README |

## 命名变更

| 旧 | 新 |
|----|----|
| `scripts/testing/` | `scripts/vm/` |
| `scripts/testing/boot-vm-unified.sh` | `scripts/vm/boot-vm.sh` |
| `scripts/testing/build-rootfs-simple.sh` | `scripts/vm/build-rootfs-minimal.sh` |
| `scripts/setup-vfio-vm.sh` | `scripts/vm/setup-vfio-vm.sh` |
| `scripts/ftrace/` + `scripts/perf/` | `scripts/trace/` |
| `scripts/testing/README-UNIFIED.md` | `scripts/README.md` |
