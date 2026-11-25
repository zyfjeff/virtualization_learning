# Phase 4 勘误记录

> 本文件记录 phase4 文档事实核查中发现的错误。
> 基于 Linux 6.12.93 源码验证，2026-09-21 更新。

---

## 已修复错误

### ✅ 1. BAR 枚举流程描述错误（严重）

**位置**: `basics.md` §4.3

**错误**: 
1. 使用了不存在的函数 `pci_assign_resource_fixup()`
2. 错误地将 BAR 大小探测放在 `pci_assign_resource()` 中
3. 行号错误（setup-res.c:291 → 实际 329）

**修正**: 
- BAR 大小探测在 `__pci_read_base()` (`probe.c:176`)
- 地址分配在 `pci_assign_resource()` (`setup-res.c:329`)
- 这两个是独立的步骤，不应混为一谈

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 2. PCI_CAP_ID_CSIV 名称错误

**位置**: `basics.md` §5.2

**错误**: `PCI_CAP_ID_CSIV` 不存在

**修正**: 正确名称是 `PCI_CAP_ID_CHSWP`（CompactPCI HotSwap）

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 3. PCI_SEC_LIMIT 名称错误

**位置**: `basics.md` §3.3

**错误**: `PCI_SEC_LIMIT` 不存在

**修正**: 正确名称是 `PCI_SEC_LATENCY_TIMER`

**源码引用**: `include/uapi/linux/pci_regs.h:134`

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 4. BDF 宏文件路径错误

**位置**: `basics.md` §2.2

**错误**: 声称 `PCI_DEVFN`/`PCI_SLOT`/`PCI_FUNC` 在 `include/linux/pci.h:32-36`

**修正**: 
- `PCI_DEVFN`/`PCI_SLOT`/`PCI_FUNC` 在 `include/uapi/linux/pci.h:31-33`
- `PCI_BUS_NUM` 在 `include/linux/pci.h:73`

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 5. pci_read_config_byte 行号错误

**位置**: `basics.md` §3.2, `annotations.md` §4

**错误**: 声称在 `drivers/pci/access.c:49`

**修正**: 实际在 `drivers/pci/access.c:560`，且包含 `pci_dev_is_disconnected()` 检查

**状态**: ✅ 已修复 (2026-09-21)

---

### ✅ 6. annotations.md 全面修正

**位置**: `annotations.md` 全文

**修正内容**:
1. **虚构函数名**：
   - `pci_bridge_check_ranges()` → 移除（函数不存在）
   - `pci_bridge_update_ranges()` → 移除（函数不存在）
   - `iommu_group_get_for_pci_dev()` → `pci_device_group()` (`iommu.c:1515`)
   - `pci_direct_conf1_read()` → `pci_conf1_read()` (`direct.c:21`)

2. **行号修正**：
   - `pci_scan_child_bus_extend()`: probe.c:3183 → probe.c:2963
   - `pci_scan_slot()`: probe.c:2363 → probe.c:2747
   - `pci_acs_flags_enabled()`: pci.c:3598 → pci.c:3583
   - `pci_acs_enabled()`: pci.c:3624 → pci.c:3620
   - `pci_read_config_byte()`: access.c:49 → access.c:560
   - `struct pci_dev`: pci.h:350 → pci.h:336

3. **代码体修正**：
   - 所有函数代码体替换为实际源码
   - 移除虚构的函数实现
   - 添加缺失的检查逻辑

**状态**: ✅ 已修复 (2026-09-21)

---

## 待修复错误

### ✅ 7. basics.md 剩余行号错误（2026-11-25 已修）

复核并修正三处：
- `pci_create_root_bus`：probe.c:3058 → **3113**（3058 实为
  `pci_scan_child_bus_extend` 内部 `max = start + used_buses;`）
- `PCI_BASE_ADDRESS_*`：pci_regs.h:88 → **102-108**（88 实为 PCI_BIST 区）
- `PCI_EXP_FLAGS*`：pci_regs.h:586 → **474 起**（586 实为 PCI_EXP_SLTCAP）

字段值本身（0x01/0x04/0x06、0x00f0、0x3e00 等）与 pci_regs.h 逐项核对无误。

**状态**: ✅ 已修复 (2026-11-25)

---

### ⚠️ 11. ats.md 性能数字无出处（2026-11-25 登记）

`ats.md`「📊 ATS 的性能分析」含 "IOMMU 查询减少 80-95%"、"100-500 ns"、
"10-50 ns"、"5-10x"、"1-10 μs" 等数字，无规范引用、无本仓实测记录，
按 AGENTS.md 性能单一来源规则属机制推算（至多 C 级），不得当实测结论引用。

**已做**: 原文节首加"机制推算"警示注记；README「性能与测量」注明现状。
**待做**: 按 `../COURSE_FRAMEWORK.md` §1.3 审计流程逐条核对出处，
有出处的补引用，无出处的在 `../phase10-performance/index.md` §3 登记或标"待实测"。

**状态**: ⚠️ 待审计

---

### ⚠️ 12. "Type 1 头 = bridge" 是简化——header type bit 不是可靠 bridge 判据（2026-11-25 ex1 发现）

`basics.md` 用 Header Type bit7（0=Type0/Endpoint，1=Type1/bridge）区分设备类型。
ex1 实测发现反例：`17:00.0`（Intel Sky Lake-E Root Port）与 `02:00.0`（ASPEED AST1150
PCI-to-PCI Bridge）class=0x0604（bridge），但 config[0x0e] bit7=0（setpci 与
`/sys/bus/pci/devices/*/config` 双路核实，非读取 bug）。

**结论**：可靠的 bridge 判据是 class code 0x0604（及 PCIe Cap 的 Port Type），
不是 header type bit。ex1/ex3/ex4 已改用 class code 识别 bridge。

**待核查**：内核 `pci_setup_device()` 读 hdr_type 决定头布局，与 class 判 bridge
如何调和这些 bit7=0 的 bridge？（涉及 `drivers/pci/probe.c` hdr_type 处理）——
查清后回写 basics.md §3 的简化说法。

**状态**: ⚠️ 待核查（概念边界 + 待办，符合勘误瘦身策略）

---

### ✅ 13. annotations.md §3 ACS 代码块为伪代码（2026-11-25，严重）

**位置**: `annotations.md` §3（`pci_acs_flags_enabled()` / `pci_acs_enabled()`）

**错误**（行号对、代码体错）：
1. `pci_acs_flags_enabled`：写 `acs_flags &= PCI_ACS_FLAGS_MASK` —— 内核**不存在**
   `PCI_ACS_FLAGS_MASK`；真实判据是先读 `PCI_ACS_CAP` 再
   `acs_flags &= (cap | PCI_ACS_EC)`（pci.c:3583-3602，形参 `u16`）——
   "未声明的能力视为硬连线已启用"（AGENTS.md #11）恰恰被伪代码抹掉了，
   与同节问题链 #3 的正文自相矛盾。
2. `pci_acs_enabled`：① 顺序错——旧块先 `!pci_is_pcie` 再查 quirk，真实是
   **quirk 最先**（pci.c:3624）；② `PCI_BRIDGE`/`PCIE_BRIDGE`/`RC_EC` 真实
   **return false**（:3647-3649，PCIe 6.12.1 排除其 ACS），旧块归入"查寄存器"；
   ③ 旧块写"RC_END 总是隔离 return true"，真实：RCiEP 判隔离来自 **quirk**
   （quirks.c:5122 `pci_quirk_rciep_acs`，AGENTS #11）+ 单功能 fall-through，
   `pci_acs_enabled` 里没有 RC_END→true 分支；④ 旧块写"Endpoint 不提供隔离
   return false"，真实是**单功能端点返回 true**（effective ACS，函数头注释）。

**修复**：两段代码块替换为 `drivers/pci/pci.c:3583-3602 / 3620-3681` 真实节选，
"关键逻辑"列表同步重写。

**状态**: ✅ 已修复 (2026-11-25)

---

### ✅ 14. annotations.md §1 枚举代码块的 bridge 循环与真实调用结构不符（2026-11-25）

**位置**: `annotations.md` §1（`pci_scan_child_bus_extend()` 代码块）

**错误**: bridge 循环写成直接递归
`pci_scan_child_bus_extend(dev->subordinate, available_buses - used_buses)`。

**核实**: 真实代码两遍循环调用的是 `pci_scan_bridge_extend(bus, dev, max, 0, 0)`
（第一遍，不动固件配置，probe.c:3006-3018）与
`pci_scan_bridge_extend(bus, dev, cmax, buses, 1)`（第二遍重配+分配余量，:3020-3046）；
`pci_scan_child_bus_extend()` 的递归只发生在 `pci_scan_bridge_extend()` 内部
（probe.c:1444）。正文"bridge 扫两遍"的说法本身正确。

**修复**: 代码块改为两遍循环的真实节选并注明递归位置。

**状态**: ✅ 已修复 (2026-11-25)

---

### ✅ 15. ats.md 寄存器位图错误 + 虚构文件/函数 + 不存在的 trace 事件（2026-11-25）

**位置**: `ats.md`「ATS 的配置和启用」「ATS 的性能测量」

**错误与核实**：
1. ATS 控制寄存器旧写"bit15 Enable / **bit14 Invalidate Queue Depth** /
   bits13-0 保留"——真实（pci_regs.h:912-921，PCIe 6.0 §7.8.11）：
   Control：bit15=Enable、**bits14:10=STU**（旧图整个遗漏，正是 AGENTS #14a 的坑）；
   Capability：bits4:0=Invalidate Queue Depth、bit5=Page Aligned Request（#14b/#14c）。
2. `drivers/iommu/intel/ats.c` 在 6.12.93 **不存在**；
   `intel_enable_ats(dev, queue_depth)` 不存在。真实路径：
   `iommu.c:1295-1297 → pci_enable_ats(pdev, VTD_PAGE_SHIFT)`
   （`drivers/pci/ats.c:90`，形参是页位移 `ps`，写 Enable+STU）。
3. `events/amd_iommu/ats/enable`：6.12.93 无 `amd_iommu` trace 事件类
   （drivers/iommu/amd 下无 TRACE_EVENT）；`iommu:map/unmap` 存在
   （include/trace/events/iommu.h:79/103）。已改为 `qi_flush_dev_iotlb`
   （dmar.c:1534）kprobe 思路并按 AGENTS #9 标注取参验证要求。

**修复**: 三处均已按真实源码改写，附 file:line。

**状态**: ✅ 已修复 (2026-11-25)

---

### ✅ 16. acs-upstream-forwarding.md 把 UF 控制位画在 Switch Upstream Port（2026-11-25）

**错误**: 拓扑图/分工表写"UF 工作位置：Switch Upstream Ports + Root Ports"。

**核实**（PCIe Base r6.0 §6.12.1, p.753）：
"ACS Upstream Forwarding: must be implemented by **Root Ports** if the RC
supports Redirected Request Validation; must be implemented by **Switch
Downstream Ports**"；行为定义也在 DSP（"When ACS Upstream Forwarding is
enabled in a Switch Downstream Port, and its Ingress Port receives an Upstream
Request or Completion TLP targeting the Port's own Egress Port, the Port must
instead forward the TLP Upstream"）。§6.12.2 的"中间 Switch 必须支持 UF"指这些
Switch 的下游端口；多功能设备/RCiEP 侧 UF "must not be implemented"（§6.12.1.2）。
"不使能 UF 时重定向 TLP 处理 undefined"的结论本身有 §6.12.2 支撑，保留。

**修复**: 拓扑图、分工表、场景 1/3、性能节路径全部改到 DSP/Root Port，附勘误注记。

**状态**: ✅ 已修复 (2026-11-25)

---

## 2026-11-25 统一课程框架试点（P1）修正记录

### ✅ 8. README 本章文件表列出不存在的文件

**错误**: 文件表与推荐阅读顺序引用 `topology.md` / `capabilities.md` /
`vfio-integration.md` 三个**不存在**的文件；实际存在的两个深入文档未列出。

**修正**: 文件表按磁盘实际文件重写；学习路线改为映射实际材料
（basics.md 覆盖拓扑+能力概览，VFIO 集成指向 phase7 README §1.4–1.5）。

### ✅ 9. 实践练习计数错误

**错误**: README 写"3 个动手实验"，practice/ 实际有 ex1–ex4 四个脚本。
**修正**: 改为 4 个，列表与 practice/README.md 总览表对齐。

### ✅ 10. 旧待修项 K（README 前置编号）复核

2026-09-21 登记的"README 前置知识 phase 编号待更新"：复核现行 README
前置引用（Phase 1-3、Phase 3→4→5→7）已是正确编号，无需再改。标记已核实。

### 📝 改名与结构重组（非错误，留档）

- `ACS_UF_EXPLANATION.md` → `acs-upstream-forwarding.md`、
  `ATS_EXPLANATION.md` → `ats.md`（COURSE_FRAMEWORK §5 命名规范；
  改名前 grep 全库确认零外部引用）。
- `practice/PHASE4_SUMMARY.md` → `practice/reports/`（脚本与报告分离）。
- README 按 COURSE_FRAMEWORK §4 统一骨架重组：新增 VMM视角对比 /
  源码精读入口 / 性能与测量（指针）/ 陷阱与勘误 / 下一步；
  原有学习目标、技术全景、检验清单、参考资料原文保留。

---

## 总结

**已修复**：
- ✅ 4 个虚构函数名
- ✅ 13 个行号引用
- ✅ 多个命名错误
- ✅ annotations.md 全面重写
- ✅（2026-11-25 二轮）basics.md 剩余行号（条目 7）；annotations §3 ACS 伪代码、
  §1 扫描调用结构（条目 13、14）；ats.md 位图/虚构文件/trace 事件（条目 15）；
  acs-upstream-forwarding.md UF 位置（条目 16）

**待修复**：
- ⚠️ 条目 11（ats.md 性能数字审计）、条目 12（header type 边界待核查）

**事实核查完成度**：~95%（第二轮已对照 6.12.93 逐行核过 ACS/ATS/枚举代码体）

**实际情况**: 根据 git log，phase 编号经历过重组（commit `4c19f41`），PCIe 现在是 Phase 4。README 中的前置知识引用需要更新为正确的 phase 编号。

**状态**: ❌ 待修正

---

## 总结

### 严重程度分类

| 级别 | 数量 | 说明 |
|---|---|---|
| 🔴 严重 | 4 | 函数名虚构（A1-A4） |
| 🟠 重要 | 5 | 代码体错误（C1-C7）、字段不存在（D1-D2）、BAR 流程错误（E1-E2） |
| 🟡 中等 | 3 | 行号错误（B）、文件路径错误（H）、宏名错误（F、G） |
| 🟢 轻微 | 1 | README 前置知识编号（K） |

### 修正优先级

1. **立即修正**: 虚构函数名（A1-A4）— 这些会误导读者 grep 源码时找不到
2. **尽快修正**: 代码体错误（C1-C8）— 代码示例必须来自实际源码
3. **计划修正**: 行号和文件路径更新（B、H、I）
4. **补充修正**: BAR 枚举重写（E）、README 编号（K）

### 待修正文件清单

- [x] `annotations.md` — 修正所有函数名、行号、代码体、struct 字段
      （2026-11-25 二轮补修 §1/§3 代码体，见条目 13、14）
- [x] `basics.md` — 修正行号、函数名、BAR 枚举流程、宏名
      （2026-11-25 修 probe.c:3113 / pci_regs.h:102-108 / 474，见条目 7）
- [x] `README.md` — 修正前置知识 phase 编号引用（条目 10 复核无需再改）
