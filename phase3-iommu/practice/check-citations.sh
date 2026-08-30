#!/usr/bin/env bash
# 校验文档里每个 `path:line` / `path:line-line` 引用是否落在内核源码树中，
# 并打印该行实际内容供人工核对。
#
# 用法: ./check-citations.sh <doc.md ...>
#   默认内核树 /root/code/linux-6.12.93，可用 KERNEL_SRC 覆盖。
#   带 `/` 的路径在内核树找不到时，会回退到 QEMU 树
#   （默认 /root/code/qemu-10.1.0-rc2，可用 QEMU_SRC 覆盖；置空关闭）。
#
# 识别形式：`drivers/iommu/iommu.c:1234`、`arch/x86/kernel/pci-dma.c:161-164`、
#           `iova.c:963`（简写**只在全树同名文件唯一时**解析）
#
# 解析规则（重要）：
#   带 `/` 的引用当作 KERNEL_SRC 下的字面路径，找不到即 MISSING。
#   不带 `/` 的简写按 basename 全树查找：唯一才解析；多个同名 → AMBIGUOUS 报错。
#   早期版本按 "drivers/iommu/ → arch/x86/kernel/ → include/linux/" 顺序试探，
#   这会让 `iommu.c:1794`（想指 drivers/iommu/intel/iommu.c）静默命中
#   drivers/iommu/iommu.c:1794 而判为 ok —— 错误文件、行号在范围内、内容风马牛
#   不相及，正是引用校验最不该有的失败模式。写文档时请一律写全路径。

set -u
KERNEL_SRC="${KERNEL_SRC:-/root/code/linux-6.12.93}"
# 带 `/` 的引用在内核树落空时的回退树（QEMU 源码，置空可关闭）
QEMU_SRC="${QEMU_SRC-/root/code/qemu-10.1.0-rc2}"
# 文档里 `intel/pasid.c:529` 这类简写的解析基准目录
BASE="${CITATION_BASE:-drivers/iommu}"
DOCS=("$@")
[ ${#DOCS[@]} -eq 0 ] && { echo "usage: $0 <doc.md>..." >&2; exit 2; }
[ -d "$KERNEL_SRC" ] || { echo "KERNEL_SRC=$KERNEL_SRC 不是目录" >&2; exit 2; }

fail=0
total=0

# basename -> 以空格分隔的候选路径列表（相对 KERNEL_SRC）
declare -A BY_BASENAME=()
build_index() {
    local rel
    while IFS= read -r rel; do
        rel="${rel#./}"
        BY_BASENAME["$(basename "$rel")"]+="$rel"$'\n'
    done < <(cd "$KERNEL_SRC" && find . \( -name '*.c' -o -name '*.h' -o -name '*.S' \) \
                -not -path './tools/*' -not -path './scripts/*' | sort)
}
build_index

resolve() {
    # $1 = 引用里的 path；输出 "OK<TAB>真实相对路径" 或 "ERR<TAB>说明"
    local p="$1"
    case "$p" in
        */*)
            # 带 `/` 的引用：字面路径优先，其次相对 BASE 的子路径简写（`intel/iommu.c`）。
            # 注意**不允许**对裸文件名走这条路，否则又回到静默猜文件的老毛病。
            [ -f "$KERNEL_SRC/$p" ] && { printf 'OK\t%s\n' "$p"; return; }
            [ -f "$KERNEL_SRC/$BASE/$p" ] && { printf 'OK\t%s/%s\n' "$BASE" "$p"; return; }
            # 内核树落空再试 QEMU 树（hw/i386/... 这类引用）
            if [ -n "$QEMU_SRC" ] && [ -f "$QEMU_SRC/$p" ]; then
                printf 'OKQ\t%s\n' "$p"; return
            fi
            printf 'ERR\t文件不存在\n'; return ;;
    esac
    local all count first
    all=$(printf '%s' "${BY_BASENAME[$p]:-}" | grep -v '^$' | sort -u)
    count=$(printf '%s\n' "$all" | grep -c . )
    [ "$count" -eq 0 ] && { printf 'ERR\t全树无同名文件\n'; return; }
    [ "$count" -eq 1 ] && { printf 'OK\t%s\n' "$all"; return; }
    first=$(printf '%s\n' "$all" | head -1)
    printf 'ERR\t简写有 %s 个同名文件(%s 等)，必须写全路径\n' "$count" "$first"
}

extract() {
    # 从 markdown 中抽出 path:line 形式的引用，输出去重后的 "path<TAB>linespec"。
    # 不要求反引号完整包裹：文档里存在 `path:line（说明）` 这种带中文尾注的写法。
    grep -ohE '([A-Za-z0-9_./-]+\.(c|h|S)|[A-Za-z0-9_./-]*Kconfig):[0-9]+(-[0-9]+)?' "${DOCS[@]}" \
      | sed -E 's/:([0-9]+)(-[0-9]+)?$/\t\1\2/' \
      | sort -u
}

while IFS=$'\t' read -r path linespec; do
    [ -z "${path:-}" ] && continue
    total=$((total+1))
    start="${linespec%%-*}"
    end="${linespec#*-}"
    [ "$end" = "$linespec" ] && end="$start"

    resolved=$(resolve "$path")
    status="${resolved%%$'\t'*}"
    real="${resolved#*$'\t'}"
    if [ "$status" = "ERR" ]; then
        printf '%-9s %-40s :%-10s %s\n' "${real%%(*}" "$path" "$linespec" "$real"
        fail=$((fail+1)); continue
    fi

    root="$KERNEL_SRC"; tag="ok "
    if [ "$status" = "OKQ" ]; then root="$QEMU_SRC"; tag="ok(qemu)"; fi
    file="$root/$real"
    maxline=$(wc -l < "$file")
    if [ "$start" -gt "$maxline" ]; then
        printf 'OUT-RANGE %-57s :%s 但文件只有 %s 行\n' "$real" "$start" "$maxline"
        fail=$((fail+1)); continue
    fi

    snippet=$(sed -n "${start},${end}p" "$file" | tr -d '\t' | tr '\n' ' ' | cut -c1-96)
    printf '%-8s  %-56s :%-8s %s\n' "$tag" "$real" "$linespec" "$snippet"
done < <(extract)

echo
echo "checked=$total  problems=$fail"
[ "$fail" -eq 0 ]
