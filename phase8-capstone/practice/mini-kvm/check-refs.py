#!/usr/bin/env python3
"""把文档与本模块源码注释里的 `file.c:NN[-MM]` 引用逐条解析并打印被引用的原文。

行号引用会随代码改动漂移，肉眼核对必漏（见 phase8-capstone/corrections.md
J10(a′)、J11(6)）。本脚本不做语义判断，只保证两件事：

  1. 引用的文件找得到、行号不越界（越界一定错）；
  2. 把被引用的那几行原文打出来，让人一眼看出上下文对不对得上。

用法：
    ./check-refs.py                # 只扫本目录 README.md 与 stages/*.md
    ./check-refs.py --kernel       # 内核树引用也一起解析
    ./check-refs.py --quiet        # 只打印有问题的条目
    ./check-refs.py --kernel ../../corrections.md
    ./check-refs.py --kernel --src # 连模块自己的源码注释一起扫（那里面的引用
                                   # 几乎都是内核树引用，所以要配 --kernel）

已知边界：报"找不到文件"多半只是该文件名不在下面的 KERNEL_DIRS 里，加一行即
可；而"本目录同名文件越界 → 退到内核树同名文件"这一条会被标成"歧义"，因为本
地引用一旦漂出文件末尾就会被内核树接住 —— 打印出的路径就是最终采信的那个。
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KERNEL = "/root/code/linux-6.12.93"
STAGES = os.path.join(HERE, "stages")

REF = re.compile(r"(?P<file>(?:[\w+.-]+/)*[\w+.-]+\.(?:c|h|S|cpp|ld))"
                 r":(?P<start>\d+)(?:-(?P<end>\d+))?")


def default_docs():
    docs = ["README.md"]
    if os.path.isdir(STAGES):
        docs += ["stages/" + f for f in sorted(os.listdir(STAGES)) if f.endswith(".md")]
    return docs


def module_sources():
    """本模块自己的源码（注释里也全是 file:line 引用，同样会漂移）。"""
    skip = (".mod.c",)
    return [f for f in sorted(os.listdir(HERE))
            if f.endswith((".c", ".h", ".S")) and not f.endswith(skip)]


REPO_PREFIX = "practice/mini-kvm/"
# 内核树里文档常用的前缀（"vmx/vmx.c" 这种相对 arch/x86/kvm 的写法靠它兜住）
KERNEL_DIRS = ("", "arch/x86/kvm/", "arch/x86/kvm/vmx/", "arch/x86/kvm/mmu/",
               "arch/x86/include/", "arch/x86/include/asm/",
               "arch/x86/kernel/", "arch/x86/kernel/cpu/",
               "include/linux/", "include/uapi/linux/",
               "virt/kvm/", "kernel/", "mm/", "drivers/iommu/",
               "drivers/iommu/intel/", "drivers/pci/", "drivers/pci/msi/",
               "drivers/vfio/pci/", "drivers/tty/serial/", "drivers/tty/serial/8250/")


def candidates(name):
    """返回该文件名的所有可能解释，本目录在前：[(绝对路径, 是否内核树)]。"""
    out = []

    def add(path, is_kernel):
        entry = (path, is_kernel)
        if os.path.isfile(path) and entry not in out:
            out.append(entry)

    tail = name.split(REPO_PREFIX, 1)[1] if REPO_PREFIX in name else name
    add(os.path.join(HERE, name), False)
    add(os.path.join(HERE, tail), False)
    for d in KERNEL_DIRS:
        add(os.path.join(KERNEL, d, name), True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", action="store_true", help="也解析内核树引用")
    ap.add_argument("--src", action="store_true", help="连本模块源码注释一起扫")
    ap.add_argument("--quiet", action="store_true", help="只报问题")
    ap.add_argument("--context", type=int, default=4, help="每条最多打印几行")
    ap.add_argument("docs", nargs="*", default=None)
    args = ap.parse_args()

    files = args.docs or default_docs()
    if args.src:
        files = list(files) + module_sources()

    bad = printed = total = fallback = 0
    for doc in files:
        doc_path = doc if os.path.isabs(doc) else os.path.join(HERE, doc)
        if not os.path.isfile(doc_path):
            print(f"!! 文档不存在: {doc}")
            bad += 1
            continue
        cache = {}
        for lineno, text in enumerate(open(doc_path, errors="replace"), 1):
            for m in REF.finditer(text):
                name, start = m.group("file"), int(m.group("start"))
                end = int(m.group("end") or start)
                ref = f"{name}:{start}" + (f"-{end}" if m.group("end") else "")
                total += 1
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
                        chosen = (path, is_kernel, lines, i > 0)
                        break
                if chosen is None:
                    path, is_kernel = cands[-1]
                    nlines = len(open(path, errors="replace").read().splitlines())
                    tag = "内核" if is_kernel else "本模块"
                    print(f"!! {doc}:{lineno} 行号越界（{tag} 文件 {nlines} 行）  {ref}")
                    bad += 1
                    continue
                path, is_kernel, lines, ambiguous = chosen
                if is_kernel and not args.kernel:
                    continue
                tag = "内核" if is_kernel else "本模块"
                note = "（裸引用按内核树解析）" if ambiguous else ""
                if ambiguous:
                    fallback += 1
                if args.quiet:
                    continue
                printed += 1
                print(f"== {doc}:{lineno} -> {tag}{note} "
                      f"{os.path.relpath(path, HERE)}:{start}-{end}")
                stop = min(end, start + args.context - 1)
                for n in range(start - 1, stop):
                    print(f"   {n + 1}\t{lines[n]}")
                if stop < end:
                    print("   …")
    extra = f"（{fallback} 条裸引用按内核树解析）" if fallback else ""
    print(f"\n共 {total} 条引用，打印 {printed} 条，{bad} 条有问题{extra}",
          file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
