/*
 * vfio-dma-map —— 验证 VFIO DMA 映射建立的是 IOVA → HPA 的直接映射
 *
 * 思路：分配并 populate 一段匿名内存，从 /proc/self/pagemap 读出它的宿主物理页帧号，
 * 再对同一段内存做 VFIO_IOMMU_MAP_DMA。若用 kprobe 抓 intel_iommu_map_pages 的
 * paddr 实参，应当等于 pagemap 报告的 HPA —— 即 IOMMU 页表里存的是裸宿主物理地址，
 * 与 CPU 侧的 EPT 是两套独立的翻译结构。
 *
 * 这也解释了为什么直通必须 pin 内存：页表里是裸 HPA，宿主一旦换出或迁移页面，
 * 设备就会 DMA 到错误的物理内存。
 *
 * 相关源码：
 *   drivers/vfio/vfio_iommu_type1.c:1548  vfio_dma_do_map()
 *   drivers/vfio/vfio_iommu_type1.c:1448  vfio_pin_map_dma() 循环 pin + map
 *   drivers/vfio/vfio_iommu_type1.c:1428  iommu_map(domain, iova, pfn << PAGE_SHIFT, ...)
 *   drivers/iommu/intel/iommu.c           intel_iommu_map_pages()
 *
 * 用法: sudo ./vfio-dma-map <iommu_group_id> <bdf> [页数]
 * 例如: sudo ./vfio-dma-map 35 0000:4b:00.0 4
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_IOVA 0x100000000UL /* 4GB 处，避开常见的保留区间 */

/* 从 /proc/self/pagemap 读出虚拟页对应的宿主物理页帧号，需要 root */
static int va_to_pfn(void *va, uint64_t *pfn)
{
	uint64_t entry;
	off_t offset;
	int fd;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		return -1;

	offset = ((uintptr_t)va / sysconf(_SC_PAGESIZE)) * sizeof(uint64_t);
	if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry)) {
		close(fd);
		return -1;
	}
	close(fd);

	if (!(entry & (1ULL << 63))) /* bit 63: page present */
		return -1;

	*pfn = entry & ((1ULL << 55) - 1);
	return 0;
}

static void print_pgsizes(uint64_t bitmap)
{
	int bit;

	printf("    支持的页大小:");
	for (bit = 0; bit < 64; bit++) {
		if (!(bitmap & (1ULL << bit)))
			continue;
		if (bit < 20)
			printf(" %lluKB", 1ULL << (bit - 10));
		else if (bit < 30)
			printf(" %lluMB", 1ULL << (bit - 20));
		else
			printf(" %lluGB", 1ULL << (bit - 30));
	}
	printf("\n");
}

int main(int argc, char **argv)
{
	struct vfio_iommu_type1_dma_map map = { .argsz = sizeof(map) };
	struct vfio_iommu_type1_dma_unmap unmap = { .argsz = sizeof(unmap) };
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(iommu_info) };
	struct vfio_device_info dev_info = { .argsz = sizeof(dev_info) };
	struct vfio_group_status status = { .argsz = sizeof(status) };
	char group_path[64];
	int container, group, device, pages;
	long page_size = sysconf(_SC_PAGESIZE);
	size_t len;
	void *buf;
	uint64_t pfn;

	if (argc < 3) {
		fprintf(stderr, "用法: %s <iommu_group_id> <bdf> [页数]\n", argv[0]);
		return 1;
	}
	pages = (argc > 3) ? atoi(argv[3]) : 4;
	len = (size_t)pages * page_size;

	/* --- 1. container + group + SET_IOMMU --- */
	container = open("/dev/vfio/vfio", O_RDWR);
	if (container < 0) {
		perror("open /dev/vfio/vfio");
		return 1;
	}

	snprintf(group_path, sizeof(group_path), "/dev/vfio/%s", argv[1]);
	group = open(group_path, O_RDWR);
	if (group < 0) {
		fprintf(stderr, "open %s: %s\n", group_path, strerror(errno));
		return 1;
	}

	if (ioctl(group, VFIO_GROUP_GET_STATUS, &status) < 0) {
		perror("VFIO_GROUP_GET_STATUS");
		return 1;
	}
	if (!(status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		fprintf(stderr, "group 不可用：组内仍有设备绑在普通驱动上\n");
		return 1;
	}

	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		fprintf(stderr, "VFIO_GROUP_SET_CONTAINER: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) < 0) {
		perror("VFIO_SET_IOMMU");
		return 1;
	}
	printf("[1] container + group 就绪，IOMMU 后端 = Type1v2\n");

	if (ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info) == 0) {
		printf("[2] VFIO_IOMMU_GET_INFO: flags=0x%x\n", iommu_info.flags);
		print_pgsizes(iommu_info.iova_pgsizes);
	}

	/* --- 2. 拿设备 fd --- */
	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, argv[2]);
	if (device < 0) {
		fprintf(stderr, "VFIO_GROUP_GET_DEVICE_FD(%s): %s\n", argv[2],
			strerror(errno));
		return 1;
	}
	if (ioctl(device, VFIO_DEVICE_GET_INFO, &dev_info) < 0) {
		perror("VFIO_DEVICE_GET_INFO");
		return 1;
	}
	printf("[3] 设备 fd 就绪: %s  regions=%u irqs=%u flags=0x%x%s\n",
	       argv[2], dev_info.num_regions, dev_info.num_irqs, dev_info.flags,
	       (dev_info.flags & VFIO_DEVICE_FLAGS_RESET) ? " (支持 reset)" : "");

	/* --- 3. 准备一段已 populate 的内存，取出它的 HPA --- */
	buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(buf, 0xa5, len); /* 确保真实分配了物理页 */

	if (va_to_pfn(buf, &pfn) < 0) {
		fprintf(stderr, "读取 pagemap 失败（需要 root）\n");
		return 1;
	}
	printf("[4] 测试缓冲区 %d 页 (%zu 字节)\n", pages, len);
	printf("    用户态 VA  = 0x%lx\n", (unsigned long)buf);
	printf("    宿主 HPA   = 0x%llx  (pagemap PFN=0x%llx)\n",
	       (unsigned long long)(pfn << 12), (unsigned long long)pfn);

	/* --- 4. 建立 IOVA → HPA 映射 --- */
	map.vaddr = (uintptr_t)buf;
	map.iova = TEST_IOVA;
	map.size = len;
	map.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	if (ioctl(container, VFIO_IOMMU_MAP_DMA, &map) < 0) {
		fprintf(stderr, "VFIO_IOMMU_MAP_DMA: %s\n", strerror(errno));
		return 1;
	}
	printf("[5] VFIO_IOMMU_MAP_DMA 成功\n");
	printf("    IOVA 0x%llx → HPA 0x%llx，长度 %llu\n",
	       (unsigned long long)map.iova, (unsigned long long)(pfn << 12),
	       (unsigned long long)map.size);
	printf("    ↑ 若 kprobe 抓到的 intel_iommu_map_pages paddr 等于上面的 HPA，\n");
	printf("      即证明 IOMMU 页表存的是裸宿主物理地址，与 EPT 无关\n");

	printf("\n映射已建立，回车解除映射并退出。\n");
	getchar();

	unmap.iova = TEST_IOVA;
	unmap.size = len;
	if (ioctl(container, VFIO_IOMMU_UNMAP_DMA, &unmap) < 0)
		perror("VFIO_IOMMU_UNMAP_DMA");
	else
		printf("[6] VFIO_IOMMU_UNMAP_DMA 成功，实际解除 %llu 字节\n",
		       (unsigned long long)unmap.size);

	munmap(buf, len);
	close(device);
	close(group);
	close(container);
	return 0;
}
