// SPDX-License-Identifier: GPL-2.0
/*
 * ple_load —— guest 内核态（CPL0）负载，两种形态：
 *
 *   workload=0  N 个绑核线程抢一把 spinlock（E1 · PLE）
 *   workload=1  N 个绑核线程各扫自己的私有缓冲区，无锁（E3 · vCPU 迁移）
 *
 * 为什么需要一个自建模块而不是 stress-ng：见 ../bench-ple.md §3。
 * 一句话 —— "PAUSE-loop exiting" 在 CPL > 0 时被硬件忽略
 * （arch/x86/kvm/vmx/vmx.c:5916-5921 引 Intel SDM vol3 ch-25.1.3），
 * 用户态的 mutex/futex 忙等根本触发不了 PLE。
 *
 * E3 为什么不能沿用 workload=0：锁争抢的吞吐由 **cacheline 在核间来回交接**
 * 主导，把它当"迁移代价"的指标会把两件事混在一起（见 ../bench-migrate.md §3）。
 * 私有缓冲区模式让每个线程只碰自己的内存，速率对"换物理核后 L2 变冷 +
 * 影子 TLB 失效"才敏感。
 *
 * 本模块不碰任何 KVM 代码，只在 guest 里造负载。
 */

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>

MODULE_DESCRIPTION("CPL0 load (spinlock contention / private cache sweep) for KVM perf experiments");
MODULE_AUTHOR("kvm-study");
MODULE_LICENSE("GPL");

static unsigned int nr_threads = 8;
module_param(nr_threads, uint, 0444);
MODULE_PARM_DESC(nr_threads, "线程数，上限 guest 的 num_possible_cpus()");

/* 临界区长度可运行时改：改它是为了扫描"多长才够让等待方跨过 PLE 窗口"，
 * 不是调优手段。 */
static unsigned int hold_loops = 2000;
module_param(hold_loops, uint, 0644);
MODULE_PARM_DESC(hold_loops, "临界区内 volatile 累加迭代次数（workload=0）");

/* 0444 是刻意的：一次采样里换形态会让 completed 的语义在中间断裂，
 * 前后两段的速率不可比。要换形态就重新 insmod。 */
static unsigned int workload;
module_param(workload, uint, 0444);
MODULE_PARM_DESC(workload, "0=spinlock 争抢（默认） 1=每线程私有缓冲区扫描");

#define WORK_SPIN	0
#define WORK_PRIVATE	1

/* 每线程私有字节数。默认 256 KiB —— 小于本机 Xeon 8163 的 1 MiB L2，
 * 因此"换物理核"会真的把这块数据从冷 L2 重新拉一遍，而对同核的线程无影响。
 * 太大就变成内存带宽测试，太小则整块常驻 L1，迁移代价被摊平。 */
static unsigned int priv_kb = 256;
module_param(priv_kb, uint, 0444);
MODULE_PARM_DESC(priv_kb, "workload=1 时每线程私有缓冲区大小（KiB，默认 256）");

#define PRIV_STRIDE	8	/* 每次跳 8 个 unsigned long = 64 B = 一条 cache line */

/* 计数器必须**每线程独占一条 cache line**：共享 atomic 本身就会让那条线在核间
 * 来回交接，private 模式下等于自己往被测负载里掺迁移开销。 */
struct tcounter {
	atomic64_t v;
	char pad[64 - sizeof(atomic64_t)];
} ____cacheline_aligned;
static struct tcounter *tcount;

static DEFINE_SPINLOCK(test_lock);

static struct task_struct **tasks;
static unsigned int actual_threads;
static unsigned long *priv_base;	/* nr_threads 片连续大数组 */
static unsigned int priv_n;		/* 每片多少个 unsigned long */

/* volatile 累加器强制每轮真的做 load/add/store，编译器无法整体消掉；
 * 刻意不含 PAUSE，否则持锁方自己也变成忙等退出源，归因就混了。 */
static void noinline hold_busy(unsigned int loops)
{
	volatile unsigned int acc = 0;
	unsigned int i;

	for (i = 0; i < loops; i++)
		acc += i;
}

/* 扫一遍自己的切片：每条 cache line 只碰一次，纯读写、无锁、无共享状态。
 * noinline 是为了让整块循环体不会被摊进调用者、优化掉一半。 */
static void noinline priv_sweep(unsigned long *p, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i += PRIV_STRIDE)
		p[i] += 1;
}

static int ple_fn(void *data)
{
	unsigned long idx = (unsigned long)data;
	unsigned int priv = (workload == WORK_PRIVATE);

	if (idx < nr_cpu_ids)
		set_cpus_allowed_ptr(current, cpumask_of(idx));

	while (!kthread_should_stop()) {
		if (priv) {
			priv_sweep(priv_base + idx * priv_n, priv_n);
			atomic64_inc(&tcount[idx].v);
			continue;
		}
		spin_lock(&test_lock);
		hold_busy(hold_loops);
		atomic64_inc(&tcount[idx].v);
		spin_unlock(&test_lock);
	}
	return 0;
}

static u64 total_completed(void)
{
	u64 sum = 0;
	unsigned int i;

	for (i = 0; i < actual_threads; i++)
		sum += (u64)atomic64_read(&tcount[i].v);
	return sum;
}

static int completed_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%llu\n", (unsigned long long)total_completed());
}

static const struct kernel_param_ops completed_ops = {
	.get = completed_get,
};
module_param_cb(completed, &completed_ops, NULL, 0444);
MODULE_PARM_DESC(completed, "只读：累计完成数（workload=0 是临界区次数，=1 是扫完一轮私有缓冲区的次数）");

static int __init ple_load_init(void)
{
	unsigned int i, cap = num_possible_cpus();
	int err = -ENOMEM;

	if (workload != WORK_SPIN && workload != WORK_PRIVATE) {
		pr_err("ple_load: workload=%u 未知（0=spin 1=private）\n", workload);
		return -EINVAL;
	}
	if (!nr_threads) {
		pr_err("ple_load: nr_threads 不能为 0\n");
		return -EINVAL;
	}
	if (nr_threads > cap) {
		pr_warn("ple_load: nr_threads=%u 超过 guest 可用 CPU 数 %u，按 %u 起\n",
			nr_threads, cap, cap);
		nr_threads = cap;
	}
	if (workload == WORK_PRIVATE && !priv_kb) {
		pr_err("ple_load: workload=1 时 priv_kb 不能为 0\n");
		return -EINVAL;
	}

	/* 必须在任何线程起来之前赋值：completed 读取时按它遍历计数器 */
	actual_threads = nr_threads;

	tcount = kcalloc(nr_threads, sizeof(*tcount), GFP_KERNEL);
	tasks = kcalloc(nr_threads, sizeof(*tasks), GFP_KERNEL);
	if (!tcount || !tasks)
		goto fail_err;

	if (workload == WORK_PRIVATE) {
		priv_n = round_up((priv_kb * 1024) / sizeof(unsigned long),
				  PRIV_STRIDE);
		priv_base = kvcalloc((size_t)nr_threads * priv_n,
				     sizeof(*priv_base), GFP_KERNEL);
		if (!priv_base)
			goto fail_err;
		/* 预碰一遍：否则第一次扫描里"建页 + EPT 缺页"会混进采样窗 */
		for (i = 0; i < nr_threads; i++)
			priv_sweep(priv_base + (size_t)i * priv_n, priv_n);
	}

	for (i = 0; i < nr_threads; i++) {
		char name[32];

		snprintf(name, sizeof(name), "ple_load/%u", i);
		tasks[i] = kthread_create(ple_fn, (void *)(unsigned long)i,
					  name);
		if (IS_ERR(tasks[i])) {
			err = PTR_ERR(tasks[i]);
			pr_err("ple_load: 线程 %u 创建失败 %d\n", i, err);
			while (i--)
				kthread_stop(tasks[i]);
			goto fail_err;
		}
		wake_up_process(tasks[i]);
	}

	if (workload == WORK_PRIVATE)
		pr_info("ple_load: %u 线程各扫 %u KiB 私有缓冲区（无锁）\n",
			actual_threads, priv_kb);
	else
		pr_info("ple_load: %u 线程抢一把 spinlock，hold_loops=%u\n",
			actual_threads, hold_loops);
	return 0;

fail_err:
	actual_threads = 0;
	kvfree(priv_base);
	priv_base = NULL;
	kfree(tasks);
	tasks = NULL;
	kfree(tcount);
	tcount = NULL;
	return err;
}

static void __exit ple_load_exit(void)
{
	unsigned int i;

	for (i = 0; i < actual_threads; i++)
		kthread_stop(tasks[i]);
	pr_info("ple_load: 停止，累计完成 %llu 次\n",
		(unsigned long long)total_completed());
	kvfree(priv_base);
	priv_base = NULL;
	kfree(tasks);
	tasks = NULL;
	kfree(tcount);
	tcount = NULL;
	actual_threads = 0;
}

module_init(ple_load_init);
module_exit(ple_load_exit);
