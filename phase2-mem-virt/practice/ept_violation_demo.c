/*
 * EPT Violation 处理流程可视化
 *
 * 演示从 Guest 访问到 EPT 建立的完整过程
 */

#include <stdio.h>
#include <stdint.h>

// 模拟 EPT 页表结构
typedef uint64_t sp_t;

#define SPTE_PRESENT    (1ULL << 0)
#define SPTE_WRITABLE   (1ULL << 1)
#define SPTE_USER       (1ULL << 2)
#define SPTE_WB         (6ULL << 3)  // Write-Back
#define SPTE_UC         (0ULL << 3)  // Uncacheable
#define SPTE_ACCESSED   (1ULL << 8)
#define SPTE_DIRTY      (1ULL << 9)

// 模拟 EPT 页表（简化版）
sp_t ept_root[512] = {0};
sp_t ept_pdpt[512] = {0};
sp_t ept_pd[512] = {0};
sp_t ept_pt[512] = {0};

// 模拟物理页
uint64_t physical_pages[10][512];  // 10 个页面，每个 512 字节

void print_separator() {
    printf("─────────────────────────────────────────────────────────────\n");
}

void print_step(int step, const char *description) {
    printf("\n");
    print_separator();
    printf("Step %d: %s\n", step, description);
    print_separator();
}

// 模拟 make_spte()
uint64_t make_spte(uint64_t gfn, uint64_t pfn, int is_mmio, int writable) {
    uint64_t spte = SPTE_PRESENT | SPTE_USER;

    if (writable)
        spte |= SPTE_WRITABLE;

    if (is_mmio)
        spte |= SPTE_UC;  // Uncacheable for MMIO
    else
        spte |= SPTE_WB;  // Write-Back for RAM

    spte |= (pfn << 12);  // 物理页帧号

    return spte;
}

// 模拟 EPT 遍历
void walk_ept(uint64_t gpa) {
    uint64_t gfn = gpa >> 12;
    int pml4_index = (gfn >> 27) & 0x1FF;
    int pdpt_index = (gfn >> 18) & 0x1FF;
    int pd_index = (gfn >> 9) & 0x1FF;
    int pt_index = gfn & 0x1FF;

    printf("  GFN: 0x%lx\n", gfn);
    printf("  EPT 遍历:\n");
    printf("    PML4[%d] = 0x%lx\n", pml4_index, ept_root[pml4_index]);
    printf("    PDPT[%d] = 0x%lx\n", pdpt_index, ept_pdpt[pdpt_index]);
    printf("    PD[%d]   = 0x%lx\n", pd_index, ept_pd[pd_index]);
    printf("    PT[%d]   = 0x%lx\n", pt_index, ept_pt[pt_index]);
}

// 模拟 EPT Violation 处理
void handle_ept_violation(uint64_t gpa, int is_write, int is_mmio) {
    uint64_t gfn = gpa >> 12;
    uint64_t pfn = gfn;  // 简化：假设 PFN = GFN

    printf("  触发异常: GPA = 0x%lx (%s)\n", gpa, is_mmio ? "MMIO" : "RAM");
    printf("  访问类型: %s\n", is_write ? "写" : "读");

    // Step 1: VM-Exit
    print_step(1, "硬件触发 VM-Exit");
    printf("  EXIT_REASON = 48 (EPT_VIOLATION)\n");
    printf("  GUEST_PHYSICAL_ADDRESS = 0x%lx\n", gpa);
    printf("  EXIT_QUALIFICATION = 0x%x\n", is_write ? 0x2 : 0x1);

    // Step 2: handle_ept_violation()
    print_step(2, "handle_ept_violation()");
    printf("  从 VMCS 读取 GPA: 0x%lx\n", gpa);
    printf("  从 VMCS 读取 exit_qualification\n");

    // Step 3: __vmx_handle_ept_violation()
    print_step(3, "__vmx_handle_ept_violation()");
    printf("  解析 exit_qualification:\n");
    if (is_write)
        printf("    - 写访问 → PFERR_WRITE_MASK\n");
    else
        printf("    - 读访问 → PFERR_USER_MASK\n");
    printf("  调用 kvm_mmu_page_fault()\n");

    // Step 4: kvm_mmu_page_fault()
    print_step(4, "kvm_mmu_page_fault()");
    printf("  检查是否是 MMIO: %s\n", is_mmio ? "是" : "否");
    printf("  调用 kvm_mmu_do_page_fault()\n");

    // Step 5: kvm_mmu_do_page_fault()
    print_step(5, "kvm_mmu_do_page_fault()");
    printf("  初始化 kvm_page_fault 结构体:\n");
    printf("    - addr = 0x%lx\n", gpa);
    printf("    - gfn = 0x%lx\n", gfn);
    printf("    - slot = memslot[%d]\n", is_mmio ? 1 : 0);
    printf("  调用 kvm_tdp_page_fault()\n");

    // Step 6: kvm_tdp_page_fault()
    print_step(6, "kvm_tdp_page_fault()");
    printf("  TDP MMU 启用: 是\n");
    printf("  调用 kvm_tdp_mmu_page_fault()\n");

    // Step 7: direct_page_fault()
    print_step(7, "direct_page_fault()");
    printf("  快速路径检查: 跳过（首次访问）\n");
    printf("  分配物理页: kvm_faultin_pfn()\n");
    printf("    → PFN = 0x%lx\n", pfn);
    printf("  获取 mmu_lock (write_lock)\n");
    printf("  调用 direct_map()\n");

    // Step 8: kvm_tdp_mmu_map()
    print_step(8, "kvm_tdp_mmu_map()");
    printf("  调整大页级别: PG_LEVEL_4K\n");
    printf("  遍历 EPT 页表:\n");
    walk_ept(gpa);
    printf("  创建中间页表（如果需要）\n");
    printf("  调用 tdp_mmu_map_handle_target_level()\n");

    // Step 9: tdp_mmu_map_handle_target_level()
    print_step(9, "tdp_mmu_map_handle_target_level()");
    printf("  调用 make_spte() 构建 SPTE:\n");

    uint64_t spte = make_spte(gfn, pfn, is_mmio, is_write);

    printf("    SPTE = 0x%lx\n", spte);
    printf("      - Present: %s\n", (spte & SPTE_PRESENT) ? "是" : "否");
    printf("      - Writable: %s\n", (spte & SPTE_WRITABLE) ? "是" : "否");
    printf("      - Memory Type: %s\n",
           (spte & (7ULL << 3)) == SPTE_WB ? "WB (Write-Back)" : "UC (Uncacheable)");
    printf("      - PFN: 0x%lx\n", spte >> 12);

    printf("  原子写入 SPTE: tdp_mmu_set_spte_atomic()\n");
    printf("    PT[%d] = 0x%lx\n", (int)(gfn & 0x1FF), spte);

    // 更新 EPT 页表
    ept_pt[gfn & 0x1FF] = spte;

    // Step 10: VM-Resume
    print_step(10, "VM-Resume");
    printf("  释放 mmu_lock\n");
    printf("  Guest 继续执行\n");
    printf("  下次访问 GPA 0x%lx 时，EPT 命中\n", gpa);
}

int main() {
    printf("═════════════════════════════════════════════════════════════\n");
    printf("  EPT Violation 处理流程可视化\n");
    printf("═════════════════════════════════════════════════════════════\n");

    // 场景 1: 普通 RAM 访问
    printf("\n\n");
    printf("┌───────────────────────────────────────────────────────────┐\n");
    printf("│  场景 1: 普通 RAM 访问 (GPA = 0x10000)                  │\n");
    printf("└───────────────────────────────────────────────────────────┘\n");

    handle_ept_violation(0x10000, 1, 0);  // 写访问，RAM

    printf("\n\n");
    printf("✅ 结果: RAM 页面映射成功，使用 WB 内存类型\n");
    printf("   性能: 缓存命中时访问延迟 ~1ns\n");

    // 场景 2: GPU BAR (MMIO) 访问
    printf("\n\n");
    printf("┌───────────────────────────────────────────────────────────┐\n");
    printf("│  场景 2: GPU BAR 访问 (GPA = 0xF0000000)                │\n");
    printf("└───────────────────────────────────────────────────────────┘\n");

    handle_ept_violation(0xF0000000, 1, 1);  // 写访问，MMIO

    printf("\n\n");
    printf("✅ 结果: MMIO 页面映射成功，使用 UC 内存类型\n");
    printf("   性能: 每次访问延迟 ~100-500ns（无缓存）\n");

    // 第二次访问（EPT 命中）
    printf("\n\n");
    printf("┌───────────────────────────────────────────────────────────┐\n");
    printf("│  第二次访问: EPT 命中                                     │\n");
    printf("└───────────────────────────────────────────────────────────┘\n");

    printf("\n");
    print_separator();
    printf("Guest 再次访问 GPA 0x10000\n");
    print_separator();
    printf("  EPT 页表已建立:\n");
    walk_ept(0x10000);
    printf("  EPT 命中 → 直接访问物理页，无需 VM-Exit\n");
    printf("  性能提升: ~1000 倍（相比 VM-Exit）\n");

    // 总结
    printf("\n\n");
    printf("═════════════════════════════════════════════════════════════\n");
    printf("  总结\n");
    printf("═════════════════════════════════════════════════════════════\n");
    printf("1. EPT Violation 处理需要 10 个步骤\n");
    printf("2. 每次 VM-Exit 开销约 2-5 μs\n");
    printf("3. RAM 使用 WB 内存类型（缓存）\n");
    printf("4. MMIO 使用 UC 内存类型（不缓存）\n");
    printf("5. 第二次访问时 EPT 命中，无需 VM-Exit\n");
    printf("6. 优化策略: 使用大页、预取、快速路径\n");
    printf("\n");

    return 0;
}
