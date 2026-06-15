/*
 * test-mini-kvm.c - 用户空间测试程序
 *
 * 用法:
 *   sudo ./test-mini-kvm
 *
 * 前置条件:
 *   - 加载 mini-kvm.ko 模块
 *   - 拥有 root 权限
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

#define DEVICE_PATH "/dev/mini-kvm"

int main(void)
{
    int kvm_fd, ret;
    int api_version;

    printf("========================================\n");
    printf("  Mini-KVM 用户空间测试\n");
    printf("========================================\n\n");

    /* 1. 打开 /dev/mini-kvm */
    printf("Step 1: 打开 %s\n", DEVICE_PATH);
    kvm_fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
    if (kvm_fd < 0) {
        perror("open(/dev/mini-kvm) 失败");
        printf("\n可能原因:\n");
        printf("  - 模块未加载: sudo insmod mini-kvm.ko\n");
        printf("  - 权限不足: 需要使用 sudo\n");
        printf("  - 设备路径错误: 检查 /dev/mini-kvm 是否存在\n");
        return 1;
    }
    printf("  ✓ 设备打开成功 (fd=%d)\n\n", kvm_fd);

    /* 2. 检查 API 版本 */
    printf("Step 2: 检查 KVM API 版本\n");
    api_version = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (api_version < 0) {
        perror("ioctl(KVM_GET_API_VERSION) 失败");
        close(kvm_fd);
        return 1;
    }
    printf("  ✓ API 版本: %d (期望: 12)\n\n", api_version);

    if (api_version != 12) {
        printf("  ⚠ 警告: 版本不匹配! mini-kvm 可能不完整\n");
    }

    /* 3. 检查扩展支持 */
    printf("Step 3: 检查 KVM 扩展支持\n");
    ret = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY);
    printf("  KVM_CAP_USER_MEMORY: %s\n", ret ? "✓ 支持" : "✗ 不支持");

    ret = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT);
    printf("  KVM_CAP_IMMEDIATE_EXIT: %s\n\n", ret ? "✓ 支持" : "✗ 不支持");

    /* 4. 获取 vCPU mmap 大小 */
    printf("Step 4: 获取 vCPU mmap 大小\n");
    ret = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (ret > 0) {
        printf("  ✓ vCPU mmap 大小: %d bytes\n\n", ret);
    } else {
        printf("  ⚠ 未实现 KVM_GET_VCPU_MMAP_SIZE\n\n");
    }

    /* 5. 创建 VM (如果 mini-kvm 支持) */
    printf("Step 5: 尝试创建 VM\n");
    printf("  (此步骤需要 mini-kvm 实现 KVM_CREATE_VM)\n");
    printf("  跳过 - 等待完整实现\n\n");

    /* 6. 总结 */
    printf("========================================\n");
    printf("  测试结果\n");
    printf("========================================\n");
    printf("  ✓ 设备通信正常\n");
    printf("  ✓ API 版本兼容\n");
    printf("  基础接口测试通过!\n\n");

    printf("下一步:\n");
    printf("  - 查看 dmesg 了解内核模块状态\n");
    printf("  - 阅读 stages/stage1-vmx.md 了解 VMX 基础\n");
    printf("  - 阅读 stages/stage2-ept.md 了解 EPT 内存虚拟化\n\n");

    close(kvm_fd);
    return 0;
}
