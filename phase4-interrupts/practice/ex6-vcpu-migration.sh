#!/bin/bash
# 实验 6: vCPU 迁移与 NDST 更新
# 观察 vCPU 迁移时 PI Descriptor 的 NDST 字段更新
#
# 知识点:
#   · NDST = 物理 CPU 的 APIC ID（不是 vCPU ID）
#   · vCPU 迁移时，KVM 更新 NDST 到新 pCPU
#   · 所有设备中断自动路由到新 pCPU
#   · 无需更新 IRTE（PDA 不变）
#
# 参考源码:
#   arch/x86/kvm/vmx/posted_intr.c - vmx_vcpu_pi_load()
#   arch/x86/kvm/vmx/vmx.c - vmx_vcpu_load()

set -e

echo "=========================================="
echo " 实验 6: vCPU 迁移与 NDST 更新"
echo "=========================================="
echo ""

TRACE_DIR="/sys/kernel/debug/tracing"

# 读某个线程当前所在的物理 CPU（/proc/<pid>/task/<tid>/stat 的 processor 字段）。
# ★ 这一件小事原来在本脚本里写错了三遍，两个坑：
#   ① `awk '{print $39}'` 对 QEMU vCPU 线程**数错字段**。/proc/<pid>/stat 的第 2 项是
#      comm，QEMU 给 vCPU 线程起的名字是 "CPU %d/KVM"（accel/kvm/kvm-accel-ops.c:70），
#      于是 comm 展开成 "(CPU 0/KVM)" —— **里面有一个空格**。awk 按空白切分时 comm 占了
#      两个字段，后面所有字段号整体右移 1：$39 读到的是 exit_signal（内核输出顺序
#      fs/proc/array.c:641）而不是 processor（:642 的 task_cpu()）。exit_signal 对 QEMU
#      线程通常是 17(SIGCHLD)，看着像个合理的 CPU 号，所以错得毫无痕迹。
#      正确做法：先贪婪剥掉最后一个 ") "，剥完 processor 落在第 37 个字段。
#   ② `... || echo "?"` **兜不住**：awk 读空输入时不打印任何东西、但退出码是 0，
#      `||` 因此不触发，调用方拿到的是空串，一旦进 `[ -eq ]` 就报
#      "integer expression expected"。兜底要用 ${VAR:-?}。
#   出处：../../phase9-performance/corrections.md D13。
task_cpu_of() {   # $1 = qemu pid, $2 = tid；取不到时输出 "?"
    local v
    v=$(sed 's/.*) //' "/proc/$1/task/$2/stat" 2>/dev/null | awk '{print $37}' || true)
    printf '%s' "${v:-?}"
}

# --------------------------------------------------
# 理论知识展示函数
# --------------------------------------------------
show_theory() {
    echo "--- NDST 更新机制（理论知识） ---"
    echo ""
    echo "  vCPU 迁移流程:"
    echo ""
    echo "  ① vCPU 在 pCPU-0 上运行:"
    echo "     pi_desc->ndst = pCPU-0 的 APIC ID"
    echo "     设备中断 → 通知到 pCPU-0"
    echo ""
    echo "  ② 调度器决定迁移 vCPU 到 pCPU-3:"
    echo "     vmx_vcpu_put(vcpu)"
    echo "       → 如果 vCPU 被抢占，设置 SN=1"
    echo ""
    echo "     vmx_vcpu_load(vcpu, 3)"
    echo "       → vmx_vcpu_pi_load(vcpu, 3)"
    echo "         → dest = cpu_physical_id(3)"
    echo "         → pi_desc->ndst = dest  // 更新为新 pCPU"
    echo "         → pi_desc->sn = 0       // 清除 SN"
    echo "         → pi_desc->nv = POSTED_INTR_VECTOR"
    echo ""
    echo "  ③ 后续设备中断:"
    echo "     设备 MSI → IOMMU → 查 IRTE → PI Descriptor"
    echo "     → 读取 pi_desc->ndst = pCPU-3 的 APIC ID"
    echo "     → 发送通知到 pCPU-3"
    echo "     → vCPU 在 pCPU-3 上收到中断"
    echo ""
    echo "  关键代码 (posted_intr.c:53-117):"
    echo ""
    echo "    void vmx_vcpu_pi_load(struct kvm_vcpu *vcpu, int cpu)"
    echo "    {"
    echo "        struct pi_desc *pi_desc = vcpu_to_pi_desc(vcpu);"
    echo "        unsigned int dest;"
    echo ""
    echo "        /* 获取新 pCPU 的物理 APIC ID */"
    echo "        dest = cpu_physical_id(cpu);"
    echo ""
    echo "        /* xAPIC 模式需要移位 */"
    echo "        if (!x2apic_mode)"
    echo "            dest = (dest << 8) & 0xFF00;"
    echo ""
    echo "        /* 原子更新 PI Descriptor */"
    echo "        do {"
    echo "            old.control = READ_ONCE(pi_desc->control);"
    echo "            new = old;"
    echo "            new.ndst = dest;    // ★ 更新 NDST ★"
    echo "            new.sn = 0;         // 清除 SN"
    echo "            new.nv = POSTED_INTR_VECTOR;"
    echo "        } while (cmpxchg64(&pi_desc->control,"
    echo "                           old.control, new.control) != old.control);"
    echo "    }"
    echo ""
    echo "=========================================="
    echo " 实验 6 完成（理论部分）"
    echo "=========================================="
}

# --------------------------------------------------
# 1. 查找运行中的 VM 和 vCPU 线程
# --------------------------------------------------
echo "--- 1. 查找运行中的 VM ---"
echo ""

# 查找 QEMU 进程（排除 tmux 等包装进程）
QEMU_PID=$(pgrep -x "qemu-system-x86" 2>/dev/null | head -1 || true)

if [ -z "$QEMU_PID" ]; then
    # 备选: 通过 /proc 查找
    QEMU_PID=$(ls /proc/*/exe 2>/dev/null | while read f; do
        if readlink "$f" 2>/dev/null | grep -q "qemu-system"; then
            echo "$f" | cut -d/ -f3
            break
        fi
    done)
fi

if [ -z "$QEMU_PID" ]; then
    echo "  未找到运行中的 QEMU 进程"
    echo "  请先启动 VM: sudo bash setup-vfio-vm.sh start"
    echo ""
    echo "  跳过实验 6 的实操部分，仅展示理论知识"
    echo ""
    show_theory
    exit 0
fi

echo "  找到 QEMU 进程: PID=$QEMU_PID"

# 查找 vCPU 线程（QEMU 的 vCPU 线程通常 CPU 时间最多）
echo ""
echo "  vCPU 线程:"

# 方法: 列出所有线程，排除主线程，按 CPU 时间排序取前 N 个
VCPU_THREADS=""
THREADS=$(ps -T -p "$QEMU_PID" 2>/dev/null | awk 'NR>1 {print $2}' | grep -v "^$QEMU_PID$")

if [ -n "$THREADS" ]; then
    for tid in $THREADS; do
        # vCPU 线程的特征: 名字包含 "CPU" 或 CPU 时间较多
        COMM=$(cat /proc/$QEMU_PID/task/$tid/comm 2>/dev/null || echo "?")
        if echo "$COMM" | grep -qi "CPU\|vcpu\|kvm"; then
            VCPU_THREADS="$VCPU_THREADS $tid"
        fi
    done

    # 如果没找到带 CPU 名字的，取除主线程外的所有线程
    if [ -z "$VCPU_THREADS" ]; then
        VCPU_THREADS="$THREADS"
    fi

    for tid in $VCPU_THREADS; do
        COMM=$(cat /proc/$QEMU_PID/task/$tid/comm 2>/dev/null || echo "?")
        CPU_TIME=$(ps -T -p "$QEMU_PID" 2>/dev/null | awk -v t="$tid" '$2==t {print $4}')
        echo "    TID=$tid ($COMM, CPU time=$CPU_TIME)"
    done
else
    echo "    未找到 vCPU 线程"
fi

echo ""

# --------------------------------------------------
# 2. 观察 vCPU 的 CPU 亲和性
# --------------------------------------------------
echo "--- 2. vCPU CPU 亲和性 ---"
echo ""

if [ -n "$VCPU_THREADS" ]; then
    echo "$VCPU_THREADS" | while read tid name; do
        AFFINITY=$(taskset -p "$tid" 2>/dev/null | awk -F: '{print $2}' || echo "unknown")
        CURRENT_CPU=$(task_cpu_of "$QEMU_PID" "$tid")
        echo "    $name (TID=$tid): 当前 CPU=$CURRENT_CPU, 亲和性=$AFFINITY"
    done
fi

echo ""

# --------------------------------------------------
# 3. 追踪 vCPU 迁移
# --------------------------------------------------
echo "--- 3. 追踪 vCPU 迁移 (10s) ---"
echo ""
echo "  使用 taskset 强制 vCPU 迁移..."
echo ""

# 选择第一个 vCPU 线程进行迁移测试
FIRST_VCPU_TID=$(echo "$VCPU_THREADS" | tr ' ' '\n' | grep -v '^$' | head -1)

if [ -n "$FIRST_VCPU_TID" ]; then
    echo "  选择 vCPU 线程 TID=$FIRST_VCPU_TID 进行迁移测试"
    echo ""

    # 获取当前 CPU（解析的两个坑见文件开头 task_cpu_of() 的注释）
    ORIG_CPU=$(task_cpu_of "$QEMU_PID" "$FIRST_VCPU_TID")
    [ "$ORIG_CPU" = "?" ] && ORIG_CPU=0
    echo "  原始 CPU: $ORIG_CPU"

    # 计算目标 CPU（选择一个不同的 CPU）
    NUM_CPUS=$(nproc)
    if [ "$ORIG_CPU" -eq 0 ]; then
        TARGET_CPU=1
    else
        TARGET_CPU=0
    fi

    if [ "$TARGET_CPU" -lt "$NUM_CPUS" ]; then
        echo "  目标 CPU: $TARGET_CPU"
        echo ""

        # 启用追踪
        echo 0 > "$TRACE_DIR/tracing_on"
        echo > "$TRACE_DIR/trace"
        echo 1 > "$TRACE_DIR/events/kvm/kvm_entry/enable" 2>/dev/null || true
        echo 1 > "$TRACE_DIR/events/sched/sched_migrate_task/enable" 2>/dev/null || true

        echo 1 > "$TRACE_DIR/tracing_on"

        # 执行迁移
        echo "  执行迁移: CPU $ORIG_CPU → CPU $TARGET_CPU"
        taskset -p "$TARGET_CPU" "$FIRST_VCPU_TID" 2>/dev/null || true

        sleep 3

        # 迁移回来
        echo "  执行迁移: CPU $TARGET_CPU → CPU $ORIG_CPU"
        taskset -p "$ORIG_CPU" "$FIRST_VCPU_TID" 2>/dev/null || true

        sleep 3

        echo 0 > "$TRACE_DIR/tracing_on"

        # 分析结果
        echo ""
        echo "  迁移追踪结果:"
        TRACE_OUTPUT=$(cat "$TRACE_DIR/trace" 2>/dev/null || true)

        MIGRATE_EVENTS=$(echo "$TRACE_OUTPUT" | grep "sched_migrate_task" | head -5)
        if [ -n "$MIGRATE_EVENTS" ]; then
            echo "    任务迁移事件:"
            echo "$MIGRATE_EVENTS" | while IFS= read -r line; do
                echo "      $line"
            done
        else
            echo "    未捕获到迁移事件"
        fi

        # 显示迁移后 vCPU 所在 CPU
        NEW_CPU=$(task_cpu_of "$QEMU_PID" "$FIRST_VCPU_TID")
        echo ""
        echo "  迁移后 vCPU 所在 CPU: $NEW_CPU"

        # 清理
        echo 0 > "$TRACE_DIR/events/kvm/kvm_entry/enable" 2>/dev/null || true
        echo 0 > "$TRACE_DIR/events/sched/sched_migrate_task/enable" 2>/dev/null || true
    else
        echo "  目标 CPU $TARGET_CPU 超出范围 (共 $NUM_CPUS 个 CPU)"
    fi
else
    echo "  未找到可用的 vCPU 线程"
fi

echo ""

# --------------------------------------------------
# 4. 总结
# --------------------------------------------------
echo "--- 4. 总结 ---"
echo ""
echo "  NDST 更新的关键点:"
echo ""
echo "  1. NDST 存储的是物理 CPU 的 APIC ID"
echo "     · 不是 vCPU ID"
echo "     · 通过 cpu_physical_id() 获取"
echo ""
echo "  2. vCPU 迁移时自动更新"
echo "     · vmx_vcpu_pi_load() 中更新"
echo "     · 使用 cmpxchg64 原子操作"
echo "     · 同时清除 SN 位"
echo ""
echo "  3. 无需更新 IRTE"
echo "     · IRTE.PDA 指向 PI Descriptor（不变）"
echo "     · 只需更新 PI Descriptor 中的 NDST"
echo "     · 所有设备中断自动路由到新 pCPU"
echo ""
echo "  4. 设计优势"
echo "     · 集中管理：每个 vCPU 一个 NDST"
echo "     · 更新高效：迁移只更新一个字段"
echo "     · 自动路由：设备中断自动跟随 vCPU"

echo ""
echo "=========================================="
echo " 实验 6 完成"
echo "=========================================="
echo ""
echo "  所有实验完成！"
echo "  回顾: 运行 cat README.md 查看实验列表"
