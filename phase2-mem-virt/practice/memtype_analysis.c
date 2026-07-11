/*
 * KVM 内存类型处理分析
 *
 * 演示 KVM 如何区分 RAM 和 MMIO，以及如何设置内存类型
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

// 模拟 kvm_is_mmio_pfn 的逻辑
int is_mmio_region(unsigned long pfn, unsigned long max_pfn) {
    // 实际内核中的逻辑：
    // 1. 如果 pfn_valid(pfn) 为真（有对应的 struct page）
    //    - 检查是否是 PageReserved
    //    - 检查是否是零页
    //    - 检查 PAT 属性
    // 2. 如果 pfn_valid(pfn) 为假
    //    - 检查 e820 映射表，看是否是 E820_TYPE_RAM

    // 简化示例：假设高地址区域是 MMIO
    // GPU BAR 通常在 0xF0000000 以上
    if (pfn >= (0xF0000000 >> 12)) {
        return 1;  // MMIO
    }
    return 0;  // RAM
}

// 模拟 vmx_get_mt_mask 的逻辑
unsigned char get_memory_type(unsigned long pfn, int is_mmio) {
    // 实际内核代码：
    // if (is_mmio)
    //     return MTRR_TYPE_UNCACHABLE << VMX_EPT_MT_EPTE_SHIFT;
    // return (MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT);

    #define VMX_EPT_MT_EPTE_SHIFT  3
    #define MTRR_TYPE_UNCACHABLE   0
    #define MTRR_TYPE_WRBACK       6

    if (is_mmio) {
        printf("  → 内存类型: UC (Uncacheable)\n");
        printf("  → 值: 0x%02x (bit 3-5 = 000)\n",
               MTRR_TYPE_UNCACHABLE << VMX_EPT_MT_EPTE_SHIFT);
        return MTRR_TYPE_UNCACHABLE << VMX_EPT_MT_EPTE_SHIFT;
    } else {
        printf("  → 内存类型: WB (Write-Back)\n");
        printf("  → 值: 0x%02x (bit 3-5 = 110)\n",
               MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT);
        return MTRR_TYPE_WRBACK << VMX_EPT_MT_EPTE_SHIFT;
    }
}

int main() {
    printf("========================================\n");
    printf("  KVM 内存类型处理分析\n");
    printf("========================================\n\n");

    // 场景 1: 普通 RAM
    printf("场景 1: 普通 RAM (GPA = 0x10000)\n");
    printf("----------------------------------------\n");
    unsigned long ram_pfn = 0x10000 >> 12;  // PFN = 0x10
    int ram_is_mmio = is_mmio_region(ram_pfn, 0x100000);
    printf("  PFN: 0x%lx\n", ram_pfn);
    printf("  HPA: 0x%lx\n", ram_pfn << 12);
    printf("  是 MMIO? %s\n", ram_is_mmio ? "是" : "否");
    unsigned char ram_mt = get_memory_type(ram_pfn, ram_is_mmio);
    printf("\n");

    // 场景 2: GPU BAR (MMIO)
    printf("场景 2: GPU BAR (GPA = 0xF0000000)\n");
    printf("----------------------------------------\n");
    unsigned long gpu_pfn = 0xF0000000 >> 12;  // PFN = 0xF0000
    int gpu_is_mmio = is_mmio_region(gpu_pfn, 0x100000);
    printf("  PFN: 0x%lx\n", gpu_pfn);
    printf("  HPA: 0x%lx\n", gpu_pfn << 12);
    printf("  是 MMIO? %s\n", gpu_is_mmio ? "是" : "否");
    unsigned char gpu_mt = get_memory_type(gpu_pfn, gpu_is_mmio);
    printf("\n");

    // 场景 3: 构建 SPTE
    printf("场景 3: 构建 SPTE\n");
    printf("----------------------------------------\n");

    // RAM 的 SPTE
    unsigned long ram_spte = 0;
    ram_spte |= (1ULL << 0);   // Present
    ram_spte |= (1ULL << 1);   // Writable
    ram_spte |= (1ULL << 2);   // User
    ram_spte |= ((unsigned long)ram_mt << 0);  // 内存类型 (bit 3-5)
    ram_spte |= ((unsigned long)ram_pfn << 12); // PFN

    printf("  RAM SPTE: 0x%016lx\n", ram_spte);
    printf("    bit 0-2:   0x%lx (R|W|U)\n", ram_spte & 0x7);
    printf("    bit 3-5:   0x%lx (WB = 110)\n", (ram_spte >> 3) & 0x7);
    printf("    bit 12-51: 0x%lx (PFN)\n", ram_spte >> 12);
    printf("\n");

    // MMIO 的 SPTE
    unsigned long mmio_spte = 0;
    mmio_spte |= (1ULL << 0);   // Present
    mmio_spte |= (0ULL << 1);   // 不可写 (MMIO 通常只读)
    mmio_spte |= (1ULL << 2);   // User
    mmio_spte |= ((unsigned long)gpu_mt << 0);  // 内存类型 (bit 3-5)
    mmio_spte |= ((unsigned long)gpu_pfn << 12); // PFN

    printf("  MMIO SPTE: 0x%016lx\n", mmio_spte);
    printf("    bit 0-2:   0x%lx (R|U)\n", mmio_spte & 0x7);
    printf("    bit 3-5:   0x%lx (UC = 000)\n", (mmio_spte >> 3) & 0x7);
    printf("    bit 12-51: 0x%lx (PFN)\n", mmio_spte >> 12);
    printf("\n");

    // 场景 4: GPU BAR 注册流程
    printf("场景 4: GPU BAR 注册流程\n");
    printf("----------------------------------------\n");
    printf("  1. QEMU 通过 pci_device_register() 注册 GPU 设备\n");
    printf("  2. GPU 驱动探测 BAR 区域 (通常在 0xF0000000+)\n");
    printf("  3. QEMU 调用 ioremap() 映射 GPU BAR 到宿主虚拟地址\n");
    printf("  4. QEMU 调用 KVM_SET_USER_MEMORY_REGION:\n");
    printf("     - slot: 1 (GPU BAR)\n");
    printf("     - guest_phys_addr: 0xF0000000 (GPA)\n");
    printf("     - memory_size: 256MB (BAR 大小)\n");
    printf("     - userspace_addr: 0x7f1234560000 (HVA)\n");
    printf("     - flags: KVM_MEM_READONLY (如果只读)\n");
    printf("  5. KVM 内部创建 kvm_memory_slot\n");
    printf("  6. Guest 访问 GPU BAR 时触发 EPT Violation\n");
    printf("  7. kvm_is_mmio_pfn(pfn) 检查:\n");
    printf("     - pfn = 0xF0000 >= 0x3C000 (0xF0000000 >> 12)\n");
    printf("     - 返回 true → 识别为 MMIO\n");
    printf("  8. vmx_get_mt_mask() 返回 UC (0x00)\n");
    printf("  9. EPT 页表项设置为 UC，防止缓存\n");
    printf("\n");

    printf("========================================\n");
    printf("  总结\n");
    printf("========================================\n");
    printf("1. KVM 通过 kvm_is_mmio_pfn() 区分 RAM 和 MMIO\n");
    printf("2. MMIO 区域使用 UC (Uncacheable) 内存类型\n");
    printf("3. RAM 区域使用 WB (Write-Back) 内存类型\n");
    printf("4. GPU BAR 被识别为 MMIO，使用 UC 防止缓存\n");
    printf("5. 内存类型在 EPT SPTE 的 bit 3-5 中编码\n");
    printf("\n");

    return 0;
}
