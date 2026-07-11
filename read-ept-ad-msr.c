/*
 * 通过 /dev/cpu/0/msr 读取 IA32_VMX_EPT_VPID_CAP MSR
 * 检查 EPT A/D 位支持
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#define MSR_IA32_VMX_EPT_VPID_CAP 0x48C

int main() {
    int fd;
    uint64_t ept_vpid_cap;

    printf("=== 通过 /dev/cpu/0/msr 读取 MSR ===\n\n");

    // 打开 MSR 设备文件
    fd = open("/dev/cpu/0/msr", O_RDONLY);
    if (fd < 0) {
        perror("无法打开 /dev/cpu/0/msr");
        printf("提示: 需要 root 权限，运行: sudo ./read-ept-ad-msr\n");
        return 1;
    }

    // 读取 IA32_VMX_EPT_VPID_CAP MSR
    if (pread(fd, &ept_vpid_cap, sizeof(ept_vpid_cap), MSR_IA32_VMX_EPT_VPID_CAP) != sizeof(ept_vpid_cap)) {
        perror("读取 MSR 失败");
        close(fd);
        return 1;
    }

    close(fd);

    printf("IA32_VMX_EPT_VPID_CAP (0x48C) = 0x%016llx\n\n", ept_vpid_cap);

    // 解析各个位
    printf("=== EPT 能力解析 ===\n");

    // Bit 0: EPT 执行权限支持
    if (ept_vpid_cap & (1ULL << 0)) {
        printf("✓ Bit 0:  EPT 执行权限支持 (Execute-only)\n");
    }

    // Bit 6: EPT 页表遍历长度 4
    if (ept_vpid_cap & (1ULL << 6)) {
        printf("✓ Bit 6:  EPT 支持 4 级页表遍历\n");
    }

    // Bit 7: EPT 支持 Uncacheable 内存类型
    if (ept_vpid_cap & (1ULL << 7)) {
        printf("✓ Bit 7:  EPT 支持 Uncacheable (UC) 内存类型\n");
    }

    // Bit 8: EPT 支持 Write-Back 内存类型
    if (ept_vpid_cap & (1ULL << 8)) {
        printf("✓ Bit 8:  EPT 支持 Write-Back (WB) 内存类型\n");
    }

    // Bit 14: EPT 支持 2MB 大页
    if (ept_vpid_cap & (1ULL << 14)) {
        printf("✓ Bit 14: EPT 支持 2MB 大页\n");
    }

    // Bit 15: EPT 支持 1GB 大页
    if (ept_vpid_cap & (1ULL << 15)) {
        printf("✓ Bit 15: EPT 支持 1GB 大页\n");
    }

    // Bit 20: INVEPT 支持
    if (ept_vpid_cap & (1ULL << 20)) {
        printf("✓ Bit 20: INVEPT 指令支持\n");
    }

    // Bit 21: EPT Accessed/Dirty 位支持 ★
    if (ept_vpid_cap & (1ULL << 21)) {
        printf("★ Bit 21: EPT Accessed/Dirty 位支持 ★★★\n");
        printf("         → 硬件自动跟踪访问和脏页\n");
        printf("         → 减少 VM-Exit 次数\n");
        printf("         → 提升热迁移性能\n");
    } else {
        printf("✗ Bit 21: EPT A/D 位不支持\n");
        printf("         → KVM 需要使用软件模拟\n");
    }

    // Bit 22: INVEPT 支持所有上下文
    if (ept_vpid_cap & (1ULL << 22)) {
        printf("✓ Bit 22: INVEPT 支持所有上下文\n");
    }

    // Bit 25: INVEPT 支持单上下文
    if (ept_vpid_cap & (1ULL << 25)) {
        printf("✓ Bit 25: INVEPT 支持单上下文\n");
    }

    // Bit 26: INVVPID 支持
    if (ept_vpid_cap & (1ULL << 26)) {
        printf("✓ Bit 26: INVVPID 指令支持\n");
    }

    // Bit 32: INVVPID 支持单个地址
    if (ept_vpid_cap & (1ULL << 32)) {
        printf("✓ Bit 32: INVVPID 支持单个地址\n");
    }

    // Bit 33: INVVPID 支持单上下文
    if (ept_vpid_cap & (1ULL << 33)) {
        printf("✓ Bit 33: INVVPID 支持单上下文\n");
    }

    // Bit 34: INVVPID 支持所有上下文
    if (ept_vpid_cap & (1ULL << 34)) {
        printf("✓ Bit 34: INVVPID 支持所有上下文\n");
    }

    // Bit 35: INVVPID 支持单上下文 + 全局
    if (ept_vpid_cap & (1ULL << 35)) {
        printf("✓ Bit 35: INVVPID 支持单上下文 + 全局\n");
    }

    printf("\n");

    // 总结
    if (ept_vpid_cap & (1ULL << 21)) {
        printf("=== 结论 ===\n");
        printf("✓ 你的 CPU 支持硬件 EPT A/D 位！\n");
        printf("✓ KVM 可以使用硬件自动跟踪脏页\n");
        printf("✓ 热迁移性能将获得显著提升\n");
    } else {
        printf("=== 结论 ===\n");
        printf("✗ 你的 CPU 不支持硬件 EPT A/D 位\n");
        printf("⚠ KVM 将使用软件模拟 A/D 位\n");
        printf("⚠ 热迁移性能会有一定损失\n");
    }

    return 0;
}
