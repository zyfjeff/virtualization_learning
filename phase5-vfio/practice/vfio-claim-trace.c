/*
 * vfio-claim-trace —— 观察 VFIO 认领 IOMMU group 的时机与域切换
 *
 * 每一步 ioctl 前后都读一次 /sys/kernel/iommu_groups/<N>/type，
 * 用于定位 iommu_group_claim_dma_owner() 究竟在哪一步被调用。
 *
 * 相关源码：
 *   drivers/vfio/container.c:437   VFIO_GROUP_SET_CONTAINER 里认领 DMA ownership
 *   drivers/iommu/iommu.c:3214     iommu_group_claim_dma_owner()
 *   drivers/iommu/iommu.c:3184     __iommu_take_dma_ownership() 切 blocking_domain
 *   drivers/vfio/group.c:373       container 路径与 iommufd 路径的认领时机差异
 *
 * 用法: sudo ./vfio-claim-trace <iommu_group_id>
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void show_domain_type(int group_id, const char *stage)
{
	char path[128], buf[64] = "?";
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "/sys/kernel/iommu_groups/%d/type", group_id);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			if (buf[n - 1] == '\n')
				buf[n - 1] = '\0';
		}
		close(fd);
	}
	printf("    [域类型] %-34s %s\n", stage, buf);
}

int main(int argc, char **argv)
{
	char group_path[64];
	int container, group, api, ext, group_id;
	struct vfio_group_status status = { .argsz = sizeof(status) };

	if (argc != 2) {
		fprintf(stderr, "用法: %s <iommu_group_id>\n", argv[0]);
		return 1;
	}
	group_id = atoi(argv[1]);

	show_domain_type(group_id, "初始状态");

	container = open("/dev/vfio/vfio", O_RDWR);
	if (container < 0) {
		perror("open /dev/vfio/vfio");
		return 1;
	}
	printf("[1] 打开 container fd\n");
	show_domain_type(group_id, "打开 container 之后");

	api = ioctl(container, VFIO_GET_API_VERSION);
	printf("[2] VFIO_GET_API_VERSION = %d (期望 %d)\n", api, VFIO_API_VERSION);

	ext = ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU);
	printf("[3] VFIO_CHECK_EXTENSION(TYPE1v2) = %d\n", ext);
	if (ext <= 0) {
		fprintf(stderr, "内核不支持 Type1v2 IOMMU\n");
		return 1;
	}

	snprintf(group_path, sizeof(group_path), "/dev/vfio/%d", group_id);
	group = open(group_path, O_RDWR);
	if (group < 0) {
		fprintf(stderr, "open %s: %s\n", group_path, strerror(errno));
		fprintf(stderr, "组内设备是否都已绑定到 vfio-pci？\n");
		return 1;
	}
	printf("[4] 打开 group fd: %s\n", group_path);
	show_domain_type(group_id, "打开 group 之后");

	if (ioctl(group, VFIO_GROUP_GET_STATUS, &status) < 0) {
		perror("VFIO_GROUP_GET_STATUS");
		return 1;
	}
	printf("[5] VFIO_GROUP_GET_STATUS flags = 0x%llx  VIABLE=%s CONTAINER_SET=%s\n",
	       (unsigned long long)status.flags,
	       (status.flags & VFIO_GROUP_FLAGS_VIABLE) ? "是" : "否",
	       (status.flags & VFIO_GROUP_FLAGS_CONTAINER_SET) ? "是" : "否");

	/* 认领 DMA ownership 就发生在这一步，见 drivers/vfio/container.c:437 */
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0) {
		fprintf(stderr, "VFIO_GROUP_SET_CONTAINER: %s\n", strerror(errno));
		if (errno == EPERM)
			fprintf(stderr, "EPERM 表示 owner_cnt 非零：同组仍有设备绑在普通驱动上\n");
		return 1;
	}
	printf("[6] VFIO_GROUP_SET_CONTAINER 成功\n");
	show_domain_type(group_id, "SET_CONTAINER 之后");

	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) < 0) {
		perror("VFIO_SET_IOMMU");
		return 1;
	}
	printf("[7] VFIO_SET_IOMMU(TYPE1v2) 成功\n");
	show_domain_type(group_id, "SET_IOMMU 之后");

	printf("\n持有 fd 中，可在另一个终端观察内核状态。回车释放并退出。\n");
	getchar();

	close(group);
	close(container);
	show_domain_type(group_id, "关闭全部 fd 之后");
	return 0;
}
