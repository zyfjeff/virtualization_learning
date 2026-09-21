/*
 * test-mini-kvm.c —— mini-kvm 用户空间测试程序
 *
 * 用法:
 *   make guest user          # 先构建 guest.bin 与本程序
 *   sudo insmod mini-kvm.ko  # 见 README 的 SDL 流程
 *   sudo ./test-mini-kvm
 *
 * 完整验收链路：
 *   1. open /dev/mini-kvm，KVM_GET_API_VERSION == 12
 *   2. KVM_CREATE_VM
 *   3. mmap 2MB 匿名内存 + memset（填充物理页）
 *   4. KVM_SET_USER_MEMORY_REGION（slot 0，GPA 0 起）
 *   5. 把 guest/guest.bin 拷到 GPA 0x1000
 *   6. 搭建 guest 页表（恒等映射，见 build_guest_pagetables 注释）
 *   7. KVM_CREATE_VCPU + mmap kvm_run
 *   8. KVM_RUN → 期望 KVM_EXIT_HLT，串口含 "Hello from Mini-KVM Guest!"
 *   9. MINI_KVM_VCPU_INJECT_IRQ(0x21) → KVM_RUN → 期望再次 HLT，
 *      串口含 "[IRQ 0x21 handled]"
 *
 * Guest 内存布局（与模块侧 mini-kvm.h 的常量对应）：
 *   0x1000        guest 代码（guest.bin）
 *   0x2000-0x2fff guest 自建的中断门 IDT（256 × 16B）
 *   0x6000/0x7000/0x8000  PML4 / PDPT / PD（本程序搭建）
 *   0x100000      guest 栈顶（向下生长）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

#define DEV_PATH		"/dev/mini-kvm"
#define GUEST_MEM_SIZE		(2 * 1024 * 1024)
#define GUEST_CODE_GPA		0x1000
#define GUEST_PML4_GPA		0x6000
#define GUEST_PDPT_GPA		0x7000
#define GUEST_PD_GPA		0x8000

/* mini-kvm 私有 ioctl（与模块侧 mini-kvm.h 保持一致） */
#define MINI_KVM_VM_GET_SERIAL		_IOR('M', 0x01, char[256])
#define MINI_KVM_VCPU_INJECT_IRQ	_IOW('M', 0x02, int)

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "[失败] %s: %s (errno=%d)\n",	\
				(msg), strerror(errno), errno);		\
			exit(1);					\
		}							\
	} while (0)

#define INFO(fmt, ...)	printf("[信息] " fmt "\n", ##__VA_ARGS__)

/*
 * 搭建 guest 的 64 位页表（恒等映射，前 2MB 用一张 2MB 大页覆盖）：
 *   PML4 @ 0x6000 : [0] → PDPT@0x7000 (P|W)
 *   PDPT @ 0x7000 : [0] → PD@0x8000   (P|W)
 *   PD   @ 0x8000 : [0] → GPA 0, 2MB 大页 (P|W|PS)
 * guest 的 CR3 = 0x6000（模块侧常量，见模块头文件）。
 */
static void build_guest_pagetables(uint8_t *mem)
{
	uint64_t *pml4 = (uint64_t *)(mem + GUEST_PML4_GPA);
	uint64_t *pdpt = (uint64_t *)(mem + GUEST_PDPT_GPA);
	uint64_t *pd = (uint64_t *)(mem + GUEST_PD_GPA);

	pml4[0] = GUEST_PDPT_GPA | 0x3;
	pdpt[0] = GUEST_PD_GPA | 0x3;
	pd[0] = 0x0ULL | 0x83;	/* P|W|PS：2MB 大页映射 GPA 0-2MB */

	INFO("guest 页表: PML4@0x%x → PDPT@0x%x → PD@0x%x (2MB 大页)",
	     GUEST_PML4_GPA, GUEST_PDPT_GPA, GUEST_PD_GPA);
}

static size_t load_guest_image(uint8_t *mem)
{
	FILE *f = fopen("guest/guest.bin", "rb");
	size_t n;

	CHECK(f, "打开 guest/guest.bin（先执行 make guest）");
	n = fread(mem + GUEST_CODE_GPA, 1, 0x1000, f);
	fclose(f);
	CHECK(n > 0 && n < 0x1000, "guest.bin 大小应在 (0, 4KB) 内");
	INFO("guest 镜像加载: %zu 字节 @ GPA 0x%x", n, GUEST_CODE_GPA);
	return n;
}

/* KVM_RUN 直到拿到一个"退出到用户态"的结果 */
static void run_until_exit(int vcpu_fd, struct kvm_run *run)
{
	for (;;) {
		int r = ioctl(vcpu_fd, KVM_RUN, 0);
		int err = errno;

		if (r) {
			/*
			 * 内核侧的失败路径都会先填好 run->exit_reason 再返回
			 * -1（多数是 -EIO/-EPROTO），细节在 dmesg 里。
			 */
			fprintf(stderr,
				"[失败] KVM_RUN: errno=%d (%s), exit_reason=%u"
				"（内核侧详情见 dmesg）\n",
				err, strerror(err), run->exit_reason);
			exit(1);
		}
		if (run->exit_reason != 0)
			return;
	}
}

static void check_serial(int vm_fd, const char *expect, const char *what)
{
	char buf[256];

	memset(buf, 0, sizeof(buf));
	CHECK(ioctl(vm_fd, MINI_KVM_VM_GET_SERIAL, buf) == 0,
	      "MINI_KVM_VM_GET_SERIAL");
	INFO("串口缓冲: %s", buf);
	if (!strstr(buf, expect)) {
		fprintf(stderr, "[失败] %s: 串口输出不含 \"%s\"\n",
			what, expect);
		exit(1);
	}
	INFO("✓ %s 验证通过", what);
}

int main(void)
{
	int kvm_fd, vm_fd, vcpu_fd, mmap_size;
	struct kvm_run *run;
	struct kvm_userspace_memory_region region;
	uint8_t *mem;

	printf("========================================\n");
	printf("  mini-kvm 完整链路测试（KVM 内核侧）\n");
	printf("========================================\n\n");

	/* 1. 打开设备 + 版本握手 */
	kvm_fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);
	CHECK(kvm_fd >= 0, "open /dev/mini-kvm（模块是否已 insmod?）");
	CHECK(ioctl(kvm_fd, KVM_GET_API_VERSION, 0) == KVM_API_VERSION,
	      "KVM_GET_API_VERSION == 12");
	CHECK(ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY) == 1,
	      "KVM_CAP_USER_MEMORY");
	INFO("✓ /dev/mini-kvm 打开, API 版本 12");

	/* 2. 创建 VM */
	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	CHECK(vm_fd >= 0, "KVM_CREATE_VM");
	INFO("✓ VM fd = %d", vm_fd);

	/* 3. guest 内存：匿名共享 2MB，memset 填充物理页 */
	mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(mem != MAP_FAILED, "mmap guest 内存");
	memset(mem, 0, GUEST_MEM_SIZE);

	/* 4. 注册内存槽（内核侧 pin_user_pages 钉住） */
	memset(&region, 0, sizeof(region));
	region.slot = 0;
	region.guest_phys_addr = 0;
	region.memory_size = GUEST_MEM_SIZE;
	region.userspace_addr = (uint64_t)(uintptr_t)mem;
	CHECK(ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) == 0,
	      "KVM_SET_USER_MEMORY_REGION");
	INFO("✓ memslot: GPA 0 → HVA %p, %d KB", mem, GUEST_MEM_SIZE / 1024);

	/* 5-6. guest 镜像与页表 */
	load_guest_image(mem);
	build_guest_pagetables(mem);

	/* 7. vCPU 与 kvm_run */
	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	CHECK(vcpu_fd >= 0, "KVM_CREATE_VCPU");
	mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	CHECK(mmap_size > 0, "KVM_GET_VCPU_MMAP_SIZE");
	run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   vcpu_fd, 0);
	CHECK(run != MAP_FAILED, "mmap kvm_run");
	INFO("✓ vCPU fd = %d, kvm_run %d 字节 @ %p", vcpu_fd, mmap_size, run);

	/* 8. 首次运行：guest 打印欢迎语后 hlt */
	printf("\n--- 第一次 KVM_RUN（期望 KVM_EXIT_HLT）---\n");
	run_until_exit(vcpu_fd, run);
	CHECK(run->exit_reason == KVM_EXIT_HLT, "首次退出应为 KVM_EXIT_HLT");
	INFO("✓ 退出原因 = KVM_EXIT_HLT (%d)", KVM_EXIT_HLT);
	check_serial(vm_fd, "Hello from Mini-KVM Guest!", "guest 欢迎语");

	/* 9. 注入外部中断 0x21，再运行 */
	printf("\n--- 注入 vector 0x21 并第二次 KVM_RUN ---\n");
	{
		int vector = 0x21;
		CHECK(ioctl(vcpu_fd, MINI_KVM_VCPU_INJECT_IRQ, &vector) == 0,
		      "MINI_KVM_VCPU_INJECT_IRQ(0x21)");
	}
	run_until_exit(vcpu_fd, run);
	CHECK(run->exit_reason == KVM_EXIT_HLT, "二次退出应为 KVM_EXIT_HLT");
	check_serial(vm_fd, "[IRQ 0x21 handled]", "中断注入处理");

	printf("\n========================================\n");
	printf("  全部通过: VMX 进入/退出 + EPT 按需映射 +\n");
	printf("  IO 模拟 + 中断注入均工作正常!\n");
	printf("========================================\n");

	munmap(run, mmap_size);
	munmap(mem, GUEST_MEM_SIZE);
	close(vcpu_fd);
	close(vm_fd);
	close(kvm_fd);
	return 0;
}
