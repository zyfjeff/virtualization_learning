/*
 * vfio-msix-probe —— MSI-X 直通的宿主侧路径实测
 *
 * 目的：不启动 Guest，也能看清 VFIO_DEVICE_SET_IRQS 在内核里做了什么：
 *   1. 各 IRQ index 的 count / flags 从哪来（vfio_pci_get_irq_count）
 *   2. eventfd 与 Linux IRQ 号的绑定（vfio_msi_set_vector_signal → request_irq）
 *   3. 中断重映射是否在链路上（irq chip 名字的 "IR-" 前缀）
 *   4. irq_bypass producer 已注册，但没有 KVM consumer 时不会切 Posted 模式
 *
 * 源码依据:
 *   drivers/vfio/pci/vfio_pci_intrs.c:829  vfio_pci_set_irqs_ioctl()
 *   drivers/vfio/pci/vfio_pci_intrs.c:381  vfio_msi_enable() → pci_alloc_irq_vectors()
 *   drivers/vfio/pci/vfio_pci_intrs.c:447  vfio_msi_set_vector_signal()
 *   drivers/vfio/pci/vfio_pci_intrs.c:510  request_irq(irq, vfio_msihandler, ...)
 *   drivers/vfio/pci/vfio_pci_intrs.c:517  irq_bypass_register_producer()
 *   drivers/vfio/pci/vfio_pci_intrs.c:373  vfio_msihandler() → eventfd_signal()
 *   arch/x86/kvm/x86.c:13665               kvm_arch_irq_bypass_add_producer()
 *
 * 用法: sudo ./vfio-msix-probe <iommu_group_id> <bdf> [向量数]
 * 例:   sudo ./vfio-msix-probe 35 0000:4b:00.0 4
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <linux/vfio.h>

#define MAX_VEC 32

static const char *irq_index_name(unsigned int idx)
{
	switch (idx) {
	case VFIO_PCI_INTX_IRQ_INDEX:	return "INTX";
	case VFIO_PCI_MSI_IRQ_INDEX:	return "MSI";
	case VFIO_PCI_MSIX_IRQ_INDEX:	return "MSIX";
	case VFIO_PCI_ERR_IRQ_INDEX:	return "ERR";
	case VFIO_PCI_REQ_IRQ_INDEX:	return "REQ";
	default:			return "?";
	}
}

static void print_irq_flags(unsigned int flags)
{
	if (flags & VFIO_IRQ_INFO_EVENTFD)
		printf(" EVENTFD");
	if (flags & VFIO_IRQ_INFO_MASKABLE)
		printf(" MASKABLE");
	if (flags & VFIO_IRQ_INFO_AUTOMASKED)
		printf(" AUTOMASKED");
	if (flags & VFIO_IRQ_INFO_NORESIZE)
		printf(" NORESIZE");
}

/* 读一个 sysfs 单行属性 */
static int read_attr(const char *path, char *buf, size_t len)
{
	int fd;
	ssize_t n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[n - 1] = '\0';
	return 0;
}

/*
 * 列出设备已分配的 Linux IRQ 号及其 irq chip。
 * chip 名字带 "IR-" 前缀说明中断重映射在链路上（IRTE 已建立）。
 */
static void show_msi_irqs(const char *bdf, const char *stage)
{
	char dir[256], path[320], chip[128], hwirq[64];
	struct dirent *de;
	DIR *d;
	int n = 0;

	snprintf(dir, sizeof(dir), "/sys/bus/pci/devices/%s/msi_irqs", bdf);
	printf("\n  [%s] %s\n", stage, dir);

	d = opendir(dir);
	if (!d) {
		printf("      (不存在 —— 设备当前没有分配 MSI/MSI-X 中断)\n");
		return;
	}

	printf("      %-6s %-34s %-6s %s\n", "IRQ", "chip_name", "hwirq", "模式");
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "/sys/kernel/irq/%s/chip_name", de->d_name);
		if (read_attr(path, chip, sizeof(chip)) < 0)
			strcpy(chip, "?");

		snprintf(path, sizeof(path), "/sys/kernel/irq/%s/hwirq", de->d_name);
		if (read_attr(path, hwirq, sizeof(hwirq)) < 0)
			strcpy(hwirq, "?");

		printf("      %-6s %-34s %-6s %s\n", de->d_name, chip, hwirq,
		       strncmp(chip, "IR-", 3) == 0 ? "Remapped (IM=0)" : "无重映射");
		n++;
	}
	closedir(d);
	printf("      共 %d 个\n", n);
}

int main(int argc, char **argv)
{
	struct vfio_group_status gstatus = { .argsz = sizeof(gstatus) };
	struct vfio_device_info dinfo = { .argsz = sizeof(dinfo) };
	struct vfio_irq_set *set;
	int container, group, device;
	int efd[MAX_VEC];
	char path[64];
	unsigned int i, nvec = 4;
	size_t set_size;

	if (argc < 3) {
		fprintf(stderr, "用法: %s <iommu_group_id> <bdf> [向量数]\n", argv[0]);
		fprintf(stderr, "例:   %s 35 0000:4b:00.0 4\n", argv[0]);
		return 1;
	}
	if (argc >= 4) {
		nvec = (unsigned int)atoi(argv[3]);
		if (nvec < 1 || nvec > MAX_VEC) {
			fprintf(stderr, "向量数需在 1..%d 之间\n", MAX_VEC);
			return 1;
		}
	}

	container = open("/dev/vfio/vfio", O_RDWR);
	if (container < 0) {
		fprintf(stderr, "打开 /dev/vfio/vfio: %s\n", strerror(errno));
		return 1;
	}

	snprintf(path, sizeof(path), "/dev/vfio/%s", argv[1]);
	group = open(path, O_RDWR);
	if (group < 0) {
		fprintf(stderr, "打开 %s: %s\n", path, strerror(errno));
		return 1;
	}

	if (ioctl(group, VFIO_GROUP_GET_STATUS, &gstatus) < 0 ||
	    !(gstatus.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		fprintf(stderr, "group 不可用：组内仍有设备绑在普通驱动上\n");
		return 1;
	}
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		fprintf(stderr, "SET_CONTAINER: %s\n", strerror(errno));
		if (errno == EPERM)
			fprintf(stderr, "EPERM：owner_cnt 非零，见 corrections.md 勘误 1\n");
		return 1;
	}
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) < 0) {
		fprintf(stderr, "SET_IOMMU: %s\n", strerror(errno));
		return 1;
	}

	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, argv[2]);
	if (device < 0) {
		fprintf(stderr, "GET_DEVICE_FD(%s): %s\n", argv[2], strerror(errno));
		return 1;
	}
	if (ioctl(device, VFIO_DEVICE_GET_INFO, &dinfo) < 0) {
		fprintf(stderr, "GET_INFO: %s\n", strerror(errno));
		return 1;
	}
	printf("[1] 设备就绪: %s  num_irqs=%u num_regions=%u\n",
	       argv[2], dinfo.num_irqs, dinfo.num_regions);
	printf("    注意 num_irqs 是 index 槽位数（VFIO_PCI_NUM_IRQS=%d），不是向量数\n",
	       VFIO_PCI_NUM_IRQS);

	/* 每个 index 的 count 由 vfio_pci_get_irq_count() 从 PCI 能力寄存器读出 */
	printf("\n[2] 逐个 index 查 VFIO_DEVICE_GET_IRQ_INFO\n");
	printf("    %-6s %-6s %s\n", "index", "count", "flags");
	for (i = 0; i < dinfo.num_irqs; i++) {
		struct vfio_irq_info iinfo = { .argsz = sizeof(iinfo), .index = i };

		if (ioctl(device, VFIO_DEVICE_GET_IRQ_INFO, &iinfo) < 0) {
			printf("    %-6s %-6s %s\n", irq_index_name(i), "-",
			       strerror(errno));
			continue;
		}
		printf("    %-6s %-6u 0x%x", irq_index_name(i), iinfo.count,
		       iinfo.flags);
		print_irq_flags(iinfo.flags);
		printf("\n");
	}

	show_msi_irqs(argv[2], "SET_IRQS 之前");

	/*
	 * 挂 eventfd 就是「启用 MSI-X」。内核侧路径:
	 *   vfio_pci_set_msi_trigger → vfio_msi_enable → pci_alloc_irq_vectors
	 *                            → vfio_msi_set_block → vfio_msi_set_vector_signal
	 * 每个向量最终 request_irq(irq, vfio_msihandler, 0, name, trigger)。
	 */
	printf("\n[3] 为 %u 个向量创建 eventfd 并 SET_IRQS(MSIX, ACTION_TRIGGER)\n", nvec);
	for (i = 0; i < nvec; i++) {
		efd[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (efd[i] < 0) {
			fprintf(stderr, "eventfd: %s\n", strerror(errno));
			return 1;
		}
	}

	set_size = sizeof(*set) + nvec * sizeof(int);
	set = calloc(1, set_size);
	if (!set) {
		fprintf(stderr, "calloc 失败\n");
		return 1;
	}
	set->argsz = set_size;
	set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	set->index = VFIO_PCI_MSIX_IRQ_INDEX;
	set->start = 0;
	set->count = nvec;
	memcpy(set->data, efd, nvec * sizeof(int));

	if (ioctl(device, VFIO_DEVICE_SET_IRQS, set) < 0) {
		fprintf(stderr, "SET_IRQS: %s\n", strerror(errno));
		free(set);
		return 1;
	}
	printf("    成功。eventfd:");
	for (i = 0; i < nvec; i++)
		printf(" %d", efd[i]);
	printf("\n");

	show_msi_irqs(argv[2], "SET_IRQS 之后");

	printf("\n    chip 名字里的 \"IR-\" 前缀说明中断重映射在链路上，IRTE 已建立。\n");
	printf("    此时 irq_bypass producer 已注册（vfio_pci_intrs.c:517），但没有\n");
	printf("    KVM irqfd consumer 与之配对，所以 IRTE 停留在 Remapped 模式。\n");
	printf("    切到 Posted 模式需要 Guest：consumer 侧 kvm_arch_irq_bypass_add_producer\n");
	printf("    (arch/x86/kvm/x86.c:13665) 才会调 pi_update_irte 把 IM 置 1。\n");

	printf("\n回车关闭 fd 并禁用 MSI-X。");
	fflush(stdout);
	getchar();

	/* count=0 + DATA_NONE 即禁用整个 index，见 vfio_pci_set_msi_trigger() */
	memset(set, 0, set_size);
	set->argsz = sizeof(*set);
	set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
	set->index = VFIO_PCI_MSIX_IRQ_INDEX;
	set->start = 0;
	set->count = 0;
	if (ioctl(device, VFIO_DEVICE_SET_IRQS, set) < 0)
		fprintf(stderr, "禁用 SET_IRQS: %s\n", strerror(errno));
	else
		printf("[4] MSI-X 已禁用（pci_free_irq_vectors 释放全部向量）\n");

	show_msi_irqs(argv[2], "禁用之后");

	free(set);
	for (i = 0; i < nvec; i++)
		close(efd[i]);
	close(device);
	close(group);
	close(container);
	return 0;
}
