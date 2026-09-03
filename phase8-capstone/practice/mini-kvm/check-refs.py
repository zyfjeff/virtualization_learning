#!/usr/bin/env python3
"""把文档与本模块源码注释里的 `file.c:NN[-MM]` 引用逐条解析并打印被引用的原文。

行号引用会随代码改动漂移，肉眼核对必漏（见 phase8-capstone/corrections.md
J10(a′)、J11(6)）。本脚本核对三件事：

  1. 引用的文件找得到、行号不越界（越界一定错）；省略文件名的续引用（`vmx.c:234、
     :249`）也算，按"同段最近一个文件名"归位 —— 这种写法占本文档集的两成上下
     （汇总里打出条数），不认它等于这两成没查；
  2. 把被引用的那几行原文打出来，让人一眼看出上下文对不对得上；
  3. 引用写的**函数名**（``foo()`` 这种带空括号的）与行号是否互相印证，见下。

第 3 件事是给 J13(5) 那类错兜底的："`x86.c:7111` → `inject_pending_event()`"行号
完全正确，而 6.12.93 里根本没有 `inject_pending_event()` 这个函数（同一件事在
本树叫 `kvm_check_and_inject_events()`，`arch/x86/kvm/x86.c:10342`）—— 前两件事
对这种错完全无感。判据（--no-fn 可关）：

  * 名字所在文件 = **整段**被引文件的并集。文档一句话常跨两三行，引用写在上半句、
    函数名写在下半句是正常写法，按单行配对全是误报；
  * 只有"紧贴引用、中间不夹别的功能名"的名字才算断言（`x86.c:7111` → `foo()`），
    散文里顺带提到的 `schedule()`、`preempt_disable()` 不参与；比对表格（`|` 开
    头的行）整行跳过，那一格里是上一列的文件、下一列的名字；
  * 断言成立后再量**邻近度**：被引行号与这个名字在这个文件里出现的位置（自己的函
    数体内、或 ±WINDOW 行之内）对不上就是漂了。不能用"必须落在函数体内"当判据 ——
    引用调用点、引用一段讲这个函数的注释都是正常写法。

三档结果：`!!` 计入问题数、决定退出码（文件名都找不到 / 行号越界 / 名字在 6.12.93
里根本不存在 / 只剩注释与字符串里的残留）；`??` 只提示（名字有定义但不在本段被引
文件里、被引行号离名字太远），加 --fn-strict 才升级成 `!!`。
要**故意**提一个已经死掉的名字（复述旧错、"`foo()` 在此版本已不存在"），在那一行
加行内标记 `<!-- check-refs:ignore -->`，该行不参与第 3 项核对。

用法：
    ./check-refs.py                    # 只扫本目录 README.md 与 stages/*.md
    ./check-refs.py --kernel           # 内核树引用也打印原文
    ./check-refs.py --quiet            # 不打印原文，只报结果（含 ?? 提示）
    ./check-refs.py --kernel ../../corrections.md
    ./check-refs.py --kernel --src     # 连模块自己的源码注释一起扫（那里面的引用
                                       # 几乎都是内核树引用，所以要配 --kernel）
    ./check-refs.py --no-fn            # 关掉函数名核对（快速回归）
    ./check-refs.py --fn-strict        # 邻近度也判错
    ./check-refs.py --context 12       # 每条引用最多打印几行原文

已知边界：报"找不到文件"多半只是该文件名不在下面的 KERNEL_DIRS 里，加一行即可。
本目录与内核树同名（`vmx.c`/`main.c`/`ept.c`/…）时，候选按"本目录在前"排，**第一
个行号容得下的文件**胜出：`vmx.c:4320` 本目录那份只有 828 行，于是落到内核树的
11000 行那份；反过来 `vmx.c:500` 两份都容得下，就停在本目录。所以想指名内核那一
份必须写全路径（`arch/x86/kvm/vmx/vmx.c:3199`）；脚本把这类"落到非首选同名文件"
的条数打进汇总（不判错），核对时扫一眼即可。写成绝对路径的引用（`/root/code/qemu…/x86-common.c:633`
指 QEMU/DPDK 那几棵树）原样取文件，不拼本目录也不拼内核树。**带斜杠**的名字还会多试
两个位置：当前文档自己的目录、以及本仓根目录（`ple-load/ple_load.c:48-51` 这种兄弟文件
写法靠它）。裸名不补这两级，免得凭空多出一批同名候选打乱上面的择定顺序。
函数名核对的写法约定：`foo()` 这种带空括号的形式**只留给内核函数**。脚本自带的
shell helper 一律不带括号写（`event_on` / `filter_add`），否则 `--fn-strict` 会把它们
当成对 6.12.93 的断言而判错。
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = "/root/code/linux-6.12.93"
# 文档里也引用 QEMU/DPDK，写法是相对各自根目录的路径（`hw/i386/kvm/clock.c:163`）
OTHER_TREES = ("/root/code/qemu-10.1.0-rc2", "/root/code/qemu-11.1.0", "/root/code/dpdk")
STAGES = os.path.join(HERE, "stages")

REF = re.compile(r"(?P<file>/?(?:[\w+.-]+/)*[\w+.-]+\.(?:c|h|S|cpp|ld))"
                 r":(?P<start>\d+)(?:-(?P<end>\d+))?")
# 续引用：`vmx.c:234、:249`、`→ :9733` 这类省略文件名的写法，靠"同一段落里最近
# 一次出现的文件名"归位。不认这种写法的话，文档里近三分之一的引用等于没核对。
CONT = re.compile(r"(?<![\w.:]):(?P<start>\d+)(?:-(?P<end>\d+))?")

# 文档里写成 `foo()` 的函数名（空括号是它"是个函数"的唯一标记）
FN_NAME = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]{2,})\s*\(\s*\)")
# 函数定义行：要么"至少一个 类型 token + 空白"再可选星号然后 name(，要么
# `#define name(`（内核里 lockdep_assert_irqs_disabled()、__free_page() 这类
# 是宏，只认前一种会把它们报成"函数不存在"）。
FN_DEF = re.compile(r"^(?:[A-Za-z_][A-Za-z0-9_]*[ \t]+)+\*{0,3}[ \t]*"
                    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\("
                    r"|^[ \t]*#[ \t]*define[ \t]+(?P<name2>[A-Za-z_][A-Za-z0-9_]*)[ \t]*\(")
# 汇编里的符号入口
ASM_DEF = re.compile(r"^(?:SYM_FUNC_START|SYM_FUNC_START_NO_ALIASES|"
                     r"SYM_CODE_START|SYM_ENTRY|ENTRY)\s*\(\s*"
                     r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)")
WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]+")
# 函数名离引用多远才算"这个引用就是它"的断言（字符数）。紧贴着的是断言
# （`x86.c:7111` → `inject_pending_event()`，J13(5) 那类错），离得远的是散文里
# 顺带提到的 callee（`schedule()`、`pin_user_pages()`），配错文件无从判断。
ADJ = 40
# 函数名与被引行的允许距离（行）。文档引用调用点、引用讲这个函数的注释都算正常，
# 但漂到几百行外就是另外一件事了。
WINDOW = 40
# 明显不是内核函数的 token（控制流/取址/日志一类，写文档时经常顺带出现）
FN_SKIP = {"if", "for", "while", "switch", "sizeof", "typeof", "return",
           "case", "do", "else", "printf", "sprintf", "printk", "pr_debug",
           "pr_info", "pr_err", "pr_warn", "WARN", "WARN_ON", "BUG", "BUG_ON",
           "READ_ONCE", "WRITE_ONCE", "READ_ONCE", "static_call", "EXPORT_SYMBOL",
           "EXPORT_SYMBOL_GPL", "MODULE_LICENSE", "MODULE_DESCRIPTION", "MODULE_AUTHOR",
           "module_init", "module_exit", "container_of", "offsetof", "ARRAY_SIZE",
           "min", "max", "clamp", "unlikely", "likely", "define", "ifdef", "include"}
# 行内豁免标记：corrections.md 复述旧错、`examples/` 里"`foo()` 在此版本已不存在"
# 这类句子必须原样写下那个死掉的名字，行尾加 `<!-- check-refs:ignore -->` 即可。
IGNORE_NAME = "check-refs:ignore"


def default_docs():
    docs = ["README.md"]
    if os.path.isdir(STAGES):
        docs += ["stages/" + f for f in sorted(os.listdir(STAGES)) if f.endswith(".md")]
    return docs


def module_sources():
    """本模块自己的源码（注释里也全是 file:line 引用，同样会漂移）。"""
    return [f for f in sorted(os.listdir(HERE))
            if f.endswith((".c", ".h", ".S")) and not f.endswith(".mod.c")]


REPO_PREFIX = "practice/mini-kvm/"
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DOC_DIR = HERE   # 正在扫的那份文档自己的目录，main() 每份文档重设
# 内核树里文档常用的前缀（"vmx/vmx.c" 这种相对 arch/x86/kvm 的写法靠它兜住）
KERNEL_DIRS = ("", "arch/x86/kvm/", "arch/x86/kvm/vmx/", "arch/x86/kvm/mmu/",
               "arch/x86/kvm/svm/",
               "arch/x86/include/", "arch/x86/include/asm/",
               "arch/x86/include/uapi/asm/", "include/uapi/asm-generic/",
               "arch/x86/kernel/", "arch/x86/kernel/apic/", "arch/x86/kernel/cpu/",
               "include/linux/", "include/uapi/linux/",
               "virt/kvm/", "kernel/", "kernel/trace/", "mm/", "drivers/iommu/",
               "drivers/iommu/intel/", "drivers/iommu/iommufd/",
               "drivers/iommu/arm/arm-smmu-v3/", "drivers/irqchip/",
               "drivers/pci/", "drivers/pci/msi/",
               "drivers/vfio/", "drivers/vfio/pci/",
               "drivers/tty/serial/", "drivers/tty/serial/8250/")


def candidates(name):
    """返回该文件名的所有可能解释，本目录在前：[(绝对路径, 是否内核树)]。

    本目录与内核树同名是**约定内**的写法：源码注释与 stage 文档里裸写
    `vmx.c:4320` 指的就是 KVM 的 arch/x86/kvm/vmx/vmx.c，讲自己的文件一律带
    "本模块"限定（如 `本模块 vmx.c:576-583`）。所以内核树必须留在候选里 —— 把它
    挡掉只会让 9 条正常引用变成假的"行号越界"。名字与行号配错文件靠邻近度核对
    兜（见 check_fn_name），不靠文件名猜。
    """
    out = []

    def add(path, is_kernel):
        entry = (path, is_kernel)
        if os.path.isfile(path) and entry not in out:
            out.append(entry)

    tail = name.split(REPO_PREFIX, 1)[1] if REPO_PREFIX in name else name
    if name.startswith("/"):
        # 写全路径的引用指的多半是内核树之外的那几份（QEMU/DPDK），原样取
        add(name, False)
        return out
    add(os.path.join(HERE, name), False)
    add(os.path.join(HERE, tail), False)
    if "/" in name:
        # 兄弟文件的写法（`ple-load/ple_load.c:48-51`）相对文档自己或仓根
        add(os.path.join(DOC_DIR, name), False)
        add(os.path.join(REPO_ROOT, name), False)
    for d in KERNEL_DIRS:
        add(os.path.join(KERNEL, d, name), True)
    if "/" in name:                          # 带目录的写法才去 QEMU/DPDK 那几棵树找
        for root in OTHER_TREES:
            add(os.path.join(root, name), False)
    return out


DEF_INDEX = {}   # path -> {name: 定义行}
OCCURRENCES = {}  # path -> {标识符: [出现的行号]}
TREE_HITS = {}   # 函数名 -> (kind, [(相对路径, 行号)])，只在全树搜索时填
MODULE_WORDS = None   # 本模块自己定义的函数名，首次用到时建


def def_index(path):
    """一个文件里所有"看起来像函数定义"的行：{name: [定义行, …]}。"""
    idx = DEF_INDEX.get(path)
    if idx is not None:
        return idx
    lines = open(path, errors="replace").read().splitlines()
    idx = {}
    is_asm = path.endswith(".S")
    for i, line in enumerate(lines):
        if not line or line[0] in " \t/*;,{}":
            continue
        m = ASM_DEF.match(line) if is_asm else FN_DEF.match(line)
        if m is None:
            if not is_asm:
                m = ASM_DEF.match(line)      # .h 里也有 SYM_*? 顺手兜一下
            if m is None:
                continue
        name = m.group("name") or m.groupdict().get("name2")
        idx.setdefault(name, []).append(i + 1)
    DEF_INDEX[path] = idx
    return idx


def nearby_lines(path, ident):
    """在 `path` 里，"算这个 ident 的地盘"的行号集合。

    两处来源：名字出现的每一行（定义、调用、讲它的注释），外加它作为函数定义时
    的那一段 —— 内核代码风格里一个函数体到下一个定义之间不会掺进别的函数，所以
    用"下一个定义行"当上界就够，不必真去配花括号（原先的 _body_end 就是这么写的，
    又慢又会在宏和 `__visible` 之类的行上出错）。
    """
    rows = set(occurrences(path).get(ident, ()))
    starts = sorted({n for lines in def_index(path).values() for n in lines})
    for d in def_index(path).get(ident, ()):
        later = [n for n in starts if n > d]
        rows.update(range(d, (later[0] if later else starts[-1] + 1)))
    return rows


def occurrences(path):
    """标识符 -> 在该文件里出现过的行号。行号漂没漂，看这个名字离被引行多远。"""
    idx = OCCURRENCES.get(path)
    if idx is None:
        idx = {}
        for n, line in enumerate(open(path, errors="replace").read().splitlines(), 1):
            for w in set(WORD.findall(line)):
                idx.setdefault(w, []).append(n)
        OCCURRENCES[path] = idx
    return idx


def tree_find_definition(name):
    """内核树里 `name` 是什么状态。只在被引文件里彻底搜不到时才调用一次。

    返回 (kind, hits)，kind 四取一：
      "def"  找得到定义式的一行（函数/宏/汇编入口都算）；
      "use"  代码行里出现过，但没有任何定义式 —— 定义由宏拼接或生成代码产生，
             不能据此判定名字已失效，所以只给 ??；
      "word" 只剩注释/字符串里的残留，等于这个函数在 6.12.93 里已经不存在；
      "none" 整棵树里搜不到。
    """
    if name in TREE_HITS:
        return TREE_HITS[name]

    def run(pattern):
        try:
            return subprocess.run(
                ["grep", "-rn", "--include=*.c", "--include=*.h", "--include=*.S",
                 "-m", "3", "-E", pattern, KERNEL],
                capture_output=True, text=True, timeout=300).stdout
        except (OSError, subprocess.TimeoutExpired):
            return ""

    def is_comment(text):
        s = text.lstrip()
        return "*/" in text or s.startswith(("*", "//"))

    def collect(out):
        code, cmt = [], []
        for line in out.splitlines()[:200]:
            parts = line.split(":", 2)
            if len(parts) < 3 or not parts[1].isdigit():
                continue
            hit = (os.path.relpath(parts[0], KERNEL), int(parts[1]))
            (cmt if is_comment(parts[2]) else code).append(hit)
        # grep -r 按目录字母序走，先命中的常常是 alpha/arm/csky 那份；本文档讲
        # x86，打印时把 x86 与 KVM 共用的那份排在前面。
        key = lambda h: (not h[0].startswith(("arch/x86/", "virt/kvm/",
                                              "include/linux/")), h[0], h[1])
        return sorted(code, key=key), sorted(cmt, key=key)

    esc = re.escape(name)
    # 注意：grep -E 用的是 POSIX ERE，既不认 (?:…) 也不认 \s/\t（会警告
    # "? at start of expression" 然后一条都不中）。只能用普通分组和 [[:blank:]]。
    B = "[[:blank:]]"
    defpat = (f"^([A-Za-z_][A-Za-z0-9_]*{B}+)+\\*{{0,3}}{B}*{esc}{B}*\\("
              f"|^{B}*#{B}*define{B}+{esc}{B}*\\("
              f"|^(SYM_FUNC_START|SYM_CODE_START|ENTRY){B}*\\({B}*{esc}\\b")
    # 定义式：第 0 列开始的"类型 token + name("，或 `#define name(`，或汇编入口
    code, cmt = collect(run(defpat))
    hits = code or cmt
    kind = "def"
    if not hits:
        code, cmt = collect(run(r"\b" + esc + r"\b"))
        hits = code or cmt
        kind = "use" if code else ("word" if cmt else "none")
    hits = hits[:6]
    TREE_HITS[name] = (kind, hits)
    return kind, hits


def module_defs():
    """本模块自己定义的函数名（文档里 `mini_*()` 这类名字不该去内核树找）。"""
    global MODULE_WORDS
    if MODULE_WORDS is None:
        MODULE_WORDS = set()
        for f in module_sources() + ["guest/guest.S"]:
            path = os.path.join(HERE, f)
            if os.path.isfile(path):
                MODULE_WORDS |= set(def_index(path))
    return MODULE_WORDS


DOC_DEFS = {}


def doc_defs(doc):
    """被扫文档自己定义的名字：拿它扫别的工程（phase7 的 experiment3-lapic.c）时，
    那文件里的 `static` 函数同样不该去内核树找。"""
    path = doc if os.path.isabs(doc) else os.path.join(HERE, doc)
    names = DOC_DEFS.get(path)
    if names is None:
        names = set(def_index(path)) if path.endswith((".c", ".h", ".S")) \
            and os.path.isfile(path) else set()
        DOC_DEFS[path] = names
    return names


DIR_DEFS = {}


def dir_defs():
    """文档**同目录**的 C 源码里定义的名字。

    工程代码通常就放在讲它的那份文档旁边（`phase8-capstone/practice/minivmm.c`
    与 `practice/README.md`）。README 里 `service_vq()`、`build_mptable()` 这种
    是自己 VMM 的函数，去内核树找定义只会产出"函数名不存在"的假错。
    """
    names = DIR_DEFS.get(DOC_DIR)
    if names is None:
        names = set()
        if os.path.isdir(DOC_DIR):
            for f in sorted(os.listdir(DOC_DIR)):
                path = os.path.join(DOC_DIR, f)
                if f.endswith((".c", ".h", ".S")) and os.path.isfile(path):
                    names |= set(def_index(path))
        DIR_DEFS[DOC_DIR] = names
    return names


def gap_to_ref(pos, refs):
    """同一行里，函数名与最近一条引用之间隔了多少个字符。"""
    a, b = pos
    return min((max(0, a - e, s - b) for s, e in (r["span"] for r in refs)),
               default=10 ** 9)


def is_assertion(pos, refs, others=()):
    """这个名字是不是在断言"`file:NN` 讲的就是它"。

    条件是同一条行内紧贴（≤ADJ 字符）**且中间不夹别的函数名**：README 表格里
    `kvm_vm_ioctl()`、`kvm_vcpu_ioctl()`、`vcpu_run()`（`x86.c:11343`）连着写，
    只有最后那个名字与引用构成断言；散文里顺带提到的 `schedule()` 同理排除。
    """
    if not refs:
        return False
    a, b = pos
    for r in refs:
        s, e = r["span"]
        if max(0, a - e, s - b) > ADJ:
            continue
        lo, hi = (e, a) if a >= e else (b, s)
        if any(lo < c and d < hi for c, d in others):
            continue
        return True
    return False


def nearest_ident(refs, ident):
    """被引区间到"该文件里出现这个名字的最近一行"还差几行，取所有引用的最小值。

    返回 (距离, 引用条目, 名字所在行)，距离 0 = 被引行本身就提到这个名字。
    """
    best = None
    for r in refs:
        rows = nearby_lines(r["path"], ident)
        if not rows:
            continue
        d = min(max(r["start"] - n, n - r["end"], 0) for n in rows)
        cand = (d, r, min(rows, key=lambda n, r=r: max(r["start"] - n,
                                                       n - r["end"], 0)))
        if best is None or cand[0] < best[0]:
            best = cand
    return best


def check_fn_name(doc, lineno, pos, ident, line_refs, para_refs, bad_list,
                  suspect_list, strict, table=False, others=()):
    """核对 `ident()`：本段被引文件里得出现这个名字，且被引行得离它够近。"""
    if not any(ident in occurrences(r["path"]) for r in para_refs):
        if ident in module_defs() or ident in doc_defs(doc) or ident in dir_defs():
            return
        kind, hits = tree_find_definition(ident)
        where = ", ".join(f"{p}:{n}" for p, n in hits[:2])
        near = (line_refs or para_refs)[0]["ref"]
        if kind == "none":
            bad_list.append(f"!! {doc}:{lineno} 函数名在 6.12.93 里不存在  "
                            f"{ident}()（{near}）")
        elif kind == "word":
            bad_list.append(f"!! {doc}:{lineno} 函数名已不是函数，内核里只剩注释/字符串 "
                            f"{ident}()（出现在 {where}）")
        elif is_assertion(pos, line_refs, others):
            note = ("定义在别处" if kind == "def"
                    else "树里只有调用/宏拼接，找不到定义式")
            suspect_list.append(f"?? {doc}:{lineno} 函数名紧贴引用却不在本段被引文件里  "
                                f"{ident}() {note}：{where}")
        return
    # 名字在这个文件里，接着看被引行离它近不近 —— 行号漂到隔壁函数是这类文档最常
    # 见的失效方式。这里不能用"必须落在函数体内"当判据：引用调用点、引用一段讲
    # 这个函数的注释都是正常写法，那样只会满屏误报。同样只对"紧贴引用"的名字判，
    # 散文里顺带提到的 preempt_disable() 这类不算断言。
    if table or not is_assertion(pos, line_refs, others):
        return
    dist, r, n = nearest_ident(para_refs, ident)
    if dist <= WINDOW:
        return
    msg = (f"?? {doc}:{lineno} 被引行号离 {ident}() 有 {dist} 行远  "
           f"{os.path.relpath(r['path'], HERE)}:{r['start']}-{r['end']}，"
           f"这个名字最近出现在第 {n} 行")
    suspect_list.append(msg)
    if strict:
        bad_list.append(msg.replace("?? ", "!! "))


def main():
    global DOC_DIR
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", action="store_true",
                    help="把指向内核树的引用也打印出来（文件与行号照常核对）")
    ap.add_argument("--src", action="store_true", help="连本模块源码注释一起扫")
    ap.add_argument("--quiet", action="store_true", help="不打印被引原文，只报结果")
    ap.add_argument("--context", type=int, default=4, help="每条最多打印几行")
    ap.add_argument("--no-fn", action="store_true", help="关掉函数名核对")
    ap.add_argument("--fn-strict", action="store_true",
                    help="把\"函数名离被引行太远\"也算进问题数")
    ap.add_argument("docs", nargs="*", default=None)
    args = ap.parse_args()

    files = args.docs or default_docs()
    if args.src:
        files = list(files) + module_sources()

    bad = printed = total = used_line = n_cont = 0
    bad_lines, suspect_lines = [], []
    for doc in files:
        doc_path = doc if os.path.isabs(doc) else os.path.join(HERE, doc)
        if not os.path.isfile(doc_path):
            print(f"!! 文档不存在: {doc}")
            bad += 1
            continue
        DOC_DIR = os.path.dirname(os.path.abspath(doc_path))
        cache = {}
        para_file = None
        para = []            # 当前段落：[(行号, 原文, 本行解析出的引用)]

        def flush():
            """整段读完再核对函数名：文档里引用写在上一行、函数名写在这一行是
            正常写法（一句话跨行），只按单行配对会满屏误报。"""
            if args.no_fn:
                return
            union = [r for _, _, rs in para for r in rs]
            if not union:
                return
            for lno, body, rs in para:
                if IGNORE_NAME in body:
                    continue
                idents = [m for m in FN_NAME.finditer(body)
                          if m.group(1) not in FN_SKIP]
                spans = [(m.start(), m.end()) for m in idents]
                table = body.lstrip().startswith("|")
                for k, m in enumerate(idents):
                    check_fn_name(doc, lno, m.span(), m.group(1), rs, union,
                                  bad_lines, suspect_lines, args.fn_strict,
                                  table, spans[:k] + spans[k + 1:])

        for lineno, text in enumerate(open(doc_path, errors="replace"), 1):
            if not text.strip():
                flush()
                para, para_file = [], None
                continue
            full = list(REF.finditer(text))
            toks = [{"pos": m.start(), "text": m.group(0), "file": m.group("file"),
                     "start": int(m.group("start")),
                     "end": int(m.group("end") or m.group("start")),
                     "cont": False} for m in full]
            used = [m.span() for m in full]
            for m in CONT.finditer(text):
                if any(a < m.end() and m.start() < b for a, b in used):
                    continue
                toks.append({"pos": m.start(), "text": m.group(0), "file": None,
                             "start": int(m.group("start")),
                             "end": int(m.group("end") or m.group("start")),
                             "cont": True})
            toks.sort(key=lambda t: t["pos"])
            resolved = []
            line_file = None
            for t in toks:
                if t["cont"]:
                    name = line_file or para_file
                    if name is None:
                        continue
                else:
                    name = line_file = para_file = t["file"]
                start, end = t["start"], t["end"]
                ref = t["text"] + (f"[{name}]" if t["cont"] else "")
                total += 1
                n_cont += t["cont"]
                if name not in cache:
                    cache[name] = candidates(name)
                cands = cache[name]
                if not cands:
                    print(f"!! {doc}:{lineno} 找不到文件  {ref}")
                    bad += 1
                    continue
                chosen = None
                for i, (path, is_kernel) in enumerate(cands):
                    lines = open(path, errors="replace").read().splitlines()
                    if start >= 1 and end <= len(lines):
                        chosen = (path, is_kernel, lines, i)
                        break
                if chosen is None:
                    detail = "、".join(
                        f"{os.path.relpath(p, HERE)}({len(open(p, errors='replace').read().splitlines())} 行)"
                        for p, _ in cands)
                    print(f"!! {doc}:{lineno} 行号越界  {ref} -> {detail}")
                    bad += 1
                    continue
                path, is_kernel, lines, rank = chosen
                used_line += rank > 0                       # 靠行号挑中非首选文件
                tag = "内核" if is_kernel else ("本模块" if not path.startswith("/") else "")
                note = "（同名文件按行号择定）" if rank else ""
                resolved.append({"span": (t["pos"], t["pos"] + len(t["text"])),
                                 "path": path, "start": start, "end": end,
                                 "ref": ref, "is_kernel": is_kernel})
                # --kernel 与 --quiet 只决定打不打印原文；解析必须照做，否则函数名
                # 核对看不到内核引用，等于给最该查的那批开了免检
                if args.quiet or (is_kernel and not args.kernel):
                    continue
                printed += 1
                shown = path if path.startswith("/") else os.path.relpath(path, HERE)
                print(f"== {doc}:{lineno} -> {tag}{note} {shown}:{start}-{end}")
                stop = min(end, start + args.context - 1)
                for n in range(start - 1, stop):
                    print(f"   {n + 1}\t{lines[n]}")
                if stop < end:
                    print("   …")
            para.append((lineno, text, resolved))
        flush()
    for msg in bad_lines:
        print(msg)
        bad += 1
    for msg in suspect_lines:          # ?? 只提示，不影响退出码
        print(msg)
    extra = f"（{used_line} 条落到非首选同名文件）" if used_line else ""
    extra += f"，{n_cont} 条是省略文件名的续引用" if n_cont else ""
    susp = f"，{len(suspect_lines)} 条函数名可疑" if suspect_lines else ""
    print(f"\n共 {total} 条引用，打印 {printed} 条，{bad} 条有问题{extra}{susp}",
          file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
