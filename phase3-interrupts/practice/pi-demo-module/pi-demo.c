// SPDX-License-Identifier: GPL-2.0
/*
 * pi-demo.c - Posted Interrupts 机制演示内核模块
 *
 * 功能:
 *   1. 分配 PI Descriptor，演示 PIR/ON/SN 操作
 *   2. 模拟 IOMMU 的 PI posting 流程
 *   3. 使用 kprobe 追踪 KVM 的 PI 相关函数
 *   4. 通过 /proc/pi-demo 展示结果
 *
 * 使用方法:
 *   make && sudo insmod pi-demo.ko
 *   cat /proc/pi-demo
 *   sudo rmmod pi-demo
 *
 * 知识点:
 *   · PI Descriptor 结构 (64字节对齐)
 *   · PIR (Posted Interrupt Request) 位图
 *   · ON (Outstanding Notification) 防重复通知
 *   · SN (Suppress Notification) 抑制通知
 *   · NV (Notification Vector) 通知向量
 *   · NDST (Notification Destination) 目标 pCPU
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/kprobes.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <asm/posted_intr.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KVM Study");
MODULE_DESCRIPTION("Posted Interrupts mechanism demo");

/* ============================================================
 * 第一部分: PI Descriptor 操作演示
 * ============================================================ */

static struct pi_desc *demo_pi_desc;
static char demo_log_buf[4096];
static int demo_log_len;

static void demo_log_clear(void)
{
	demo_log_len = 0;
	demo_log_buf[0] = '\0';
}

static void dlog(const char *fmt, ...)
{
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(demo_log_buf + demo_log_len,
			sizeof(demo_log_buf) - demo_log_len, fmt, args);
	va_end(args);

	if (len > 0)
		demo_log_len += len;
}

/*
 * 演示 PI Descriptor 的基本操作
 */
static void demo_pi_descriptor(void)
{
	struct pi_desc *pi = demo_pi_desc;
	int vector;

	demo_log_clear();
	dlog("=== PI Descriptor 操作演示 ===\n\n");

	/* 初始化 */
	memset(pi, 0, sizeof(*pi));
	pi->nv = POSTED_INTR_VECTOR;  /* 通知向量 0xf7 */
	pi->ndst = 0;                 /* 目标 pCPU 0 */

	dlog("1. 初始化 PI Descriptor:\n");
	dlog("   地址: %px (64字节对齐: %s)\n", pi,
		 ((unsigned long)pi % 64 == 0) ? "是" : "否");
	dlog("   NV (通知向量): 0x%02x\n", pi->nv);
	dlog("   NDST (目标pCPU): %u\n", pi->ndst);
	dlog("   ON: %d, SN: %d\n", pi_test_on(pi), pi_test_sn(pi));
	dlog("   PIR: 全零\n\n");

	/* 模拟 IOMMU 写入第一个中断 */
	vector = 32;
	dlog("2. 模拟 IOMMU 写入 vector=%d:\n", vector);
	pi_test_and_set_pir(vector, pi);
	dlog("   PIR[%d] = 1\n", vector);
	dlog("   PIR 非空: %s\n", pi_is_pir_empty(pi) ? "否" : "是");

	/* 检查 ON，决定是否发送通知 */
	if (!pi_test_and_set_on(pi)) {
		dlog("   ON=0 → 设置 ON=1，发送通知中断\n");
		dlog("   (通知向量=0x%02x, 目标=CPU%u)\n",
			 pi->nv, pi->ndst);
	} else {
		dlog("   ON=1 → 不发送通知（已有 pending）\n");
	}
	dlog("\n");

	/* 模拟第二个中断（ON 已为 1） */
	vector = 64;
	dlog("3. 模拟 IOMMU 写入 vector=%d (ON已为1):\n", vector);
	pi_test_and_set_pir(vector, pi);
	dlog("   PIR[%d] = 1\n", vector);

	if (!pi_test_and_set_on(pi)) {
		dlog("   ON=0 → 发送通知\n");
	} else {
		dlog("   ON=1 → 不发送通知（搭便车）\n");
		dlog("   ★ 这就是 ON 字段的作用：合并通知 ★\n");
	}
	dlog("\n");

	/* 模拟 CPU 处理通知 */
	dlog("4. 模拟 CPU 处理通知中断:\n");
	if (pi_test_and_clear_on(pi)) {
		dlog("   清除 ON=0\n");
		dlog("   同步 PIR → VIRR (硬件自动)\n");
		dlog("   PIR[32]=1, PIR[64]=1 → VIRR[32]=1, VIRR[64]=1\n");
		dlog("   更新 RVI = 64 (最高 pending 向量)\n");
		dlog("   评估虚拟中断，投递给 Guest\n");
	}
	dlog("\n");

	/* 演示 SN 字段 */
	dlog("5. 演示 SN (Suppress Notification):\n");
	pi_set_sn(pi);
	dlog("   设置 SN=1 (vCPU 被调度出去)\n");

	vector = 96;
	dlog("   模拟写入 vector=%d (URG=0):\n", vector);
	pi_test_and_set_pir(vector, pi);
	dlog("   PIR[%d] = 1\n", vector);
	dlog("   X = ((ON==0) & (URG | (SN==0)))\n");
	dlog("     = ((1) & (0 | (0))) = 0\n");
	dlog("   ★ SN=1 且 URG=0 → 不发送通知 ★\n");
	dlog("\n");

	/* 紧急中断不受 SN 影响 */
	dlog("6. 模拟紧急中断 (URG=1, SN=1):\n");
	dlog("   X = ((ON==0) & (URG | (SN==0)))\n");
	dlog("     = ((1) & (1 | (0))) = 1\n");
	dlog("   ★ URG=1 → 即使 SN=1 也发送通知 ★\n");
	dlog("\n");

	dlog("=== 演示完成 ===\n");
}

/* ============================================================
 * 第二部分: Kprobe 追踪 KVM PI 函数
 * ============================================================ */

static atomic_t probe_sync_pir_count = ATOMIC_INIT(0);
static atomic_t probe_pi_load_count = ATOMIC_INIT(0);
static atomic_t probe_pi_put_count = ATOMIC_INIT(0);

static int probe_sync_pir_to_irr(struct kprobe *p, struct pt_regs *regs)
{
	atomic_inc(&probe_sync_pir_count);
	return 0;
}

static int probe_vmx_vcpu_pi_load(struct kprobe *p, struct pt_regs *regs)
{
	atomic_inc(&probe_pi_load_count);
	return 0;
}

static int probe_vmx_vcpu_pi_put(struct kprobe *p, struct pt_regs *regs)
{
	atomic_inc(&probe_pi_put_count);
	return 0;
}

static struct kprobe kp_sync_pir = {
	.symbol_name = "vmx_sync_pir_to_irr",
	.pre_handler = probe_sync_pir_to_irr,
};

static struct kprobe kp_pi_load = {
	.symbol_name = "vmx_vcpu_pi_load",
	.pre_handler = probe_vmx_vcpu_pi_load,
};

static struct kprobe kp_pi_put = {
	.symbol_name = "vmx_vcpu_pi_put",
	.pre_handler = probe_vmx_vcpu_pi_put,
};

static int probes_registered;

static int register_probes(void)
{
	int ret;

	ret = register_kprobe(&kp_sync_pir);
	if (ret < 0) {
		pr_info("pi-demo: vmx_sync_pir_to_irr 不存在 (可能非VMX或KVM未加载)\n");
	} else {
		pr_info("pi-demo: kprobe 注册 vmx_sync_pir_to_irr 成功\n");
		probes_registered++;
	}

	ret = register_kprobe(&kp_pi_load);
	if (ret < 0) {
		pr_info("pi-demo: vmx_vcpu_pi_load 不存在\n");
	} else {
		pr_info("pi-demo: kprobe 注册 vmx_vcpu_pi_load 成功\n");
		probes_registered++;
	}

	ret = register_kprobe(&kp_pi_put);
	if (ret < 0) {
		pr_info("pi-demo: vmx_vcpu_pi_put 不存在\n");
	} else {
		pr_info("pi-demo: kprobe 注册 vmx_vcpu_pi_put 成功\n");
		probes_registered++;
	}

	return 0;
}

static void unregister_probes(void)
{
	if (kp_sync_pir.symbol_name && probes_registered > 0)
		unregister_kprobe(&kp_sync_pir);
	if (kp_pi_load.symbol_name && probes_registered > 1)
		unregister_kprobe(&kp_pi_load);
	if (kp_pi_put.symbol_name && probes_registered > 2)
		unregister_kprobe(&kp_pi_put);
}

/* ============================================================
 * 第三部分: /proc/pi-demo 接口
 * ============================================================ */

static int pi_demo_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s", demo_log_buf);

	seq_printf(m, "\n=== Kprobe 追踪统计 ===\n\n");

	if (probes_registered > 0) {
		seq_printf(m, "  vmx_sync_pir_to_irr 调用次数: %d\n",
			   atomic_read(&probe_sync_pir_count));
		seq_printf(m, "  vmx_vcpu_pi_load 调用次数:    %d\n",
			   atomic_read(&probe_pi_load_count));
		seq_printf(m, "  vmx_vcpu_pi_put 调用次数:     %d\n",
			   atomic_read(&probe_pi_put_count));
		seq_printf(m, "\n");
		seq_printf(m, "  说明:\n");
		seq_printf(m, "    sync_pir_to_irr: PIR→IRR 同步 (进入Guest前)\n");
		seq_printf(m, "    pi_load: vCPU加载到pCPU时更新NDST\n");
		seq_printf(m, "    pi_put: vCPU从pCPU卸载时设置SN\n");
		seq_printf(m, "\n");
		seq_printf(m, "  运行 VM 后再次查看本文件，计数会增加\n");
	} else {
		seq_printf(m, "  (kprobe 未注册，可能需要加载 kvm_intel 模块)\n");
	}

	seq_printf(m, "\n=== PI Descriptor 当前状态 ===\n\n");
	if (demo_pi_desc) {
		seq_printf(m, "  ON: %d\n", pi_test_on(demo_pi_desc));
		seq_printf(m, "  SN: %d\n", pi_test_sn(demo_pi_desc));
		seq_printf(m, "  NV: 0x%02x\n", demo_pi_desc->nv);
		seq_printf(m, "  NDST: %u\n", demo_pi_desc->ndst);
		seq_printf(m, "  PIR 非空: %s\n",
			   pi_is_pir_empty(demo_pi_desc) ? "否" : "是");
	}

	return 0;
}

static int pi_demo_open(struct inode *inode, struct file *file)
{
	return single_open(file, pi_demo_show, NULL);
}

static const struct proc_ops pi_demo_proc_ops = {
	.proc_open = pi_demo_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ============================================================
 * 模块初始化/退出
 * ============================================================ */

static int __init pi_demo_init(void)
{
	pr_info("pi-demo: Posted Interrupts 演示模块加载\n");

	/* 分配 64 字节对齐的 PI Descriptor */
	demo_pi_desc = kzalloc(sizeof(struct pi_desc), GFP_KERNEL);
	if (!demo_pi_desc) {
		pr_err("pi-demo: 无法分配 PI Descriptor\n");
		return -ENOMEM;
	}

	/* 确保 64 字节对齐 */
	if ((unsigned long)demo_pi_desc % 64 != 0) {
		kfree(demo_pi_desc);
		demo_pi_desc = kmalloc(sizeof(struct pi_desc) + 64, GFP_KERNEL);
		if (!demo_pi_desc)
			return -ENOMEM;
		demo_pi_desc = PTR_ALIGN(demo_pi_desc, 64);
	}

	pr_info("pi-demo: PI Descriptor 分配成功: %px\n", demo_pi_desc);

	/* 运行演示 */
	demo_pi_descriptor();

	/* 注册 kprobes */
	register_probes();

	/* 创建 /proc/pi-demo */
	proc_create("pi-demo", 0444, NULL, &pi_demo_proc_ops);
	pr_info("pi-demo: /proc/pi-demo 已创建\n");

	return 0;
}

static void __exit pi_demo_exit(void)
{
	remove_proc_entry("pi-demo", NULL);
	unregister_probes();

	if (demo_pi_desc)
		kfree(demo_pi_desc);

	pr_info("pi-demo: 模块卸载\n");
}

module_init(pi_demo_init);
module_exit(pi_demo_exit);
