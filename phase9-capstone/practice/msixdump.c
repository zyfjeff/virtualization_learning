/* 物理 MSI-X 表 / BAR0 布局诊断：直通设备 4b:00.0（vfio-pci 持有）。
 * 用法: ./msixdump [-r] [-a]
 *   -r  先做 VFIO_DEVICE_RESET 再读
 *   -a  用 VFIO_DEVICE_SET_IRQS 武装 向量0/1（设备 MSI-X Enable 会置位），
 *       观察 BAR0 布局是否从「config@0x14」切换到「向量寄存器@0x14/0x16 +
 *       config@0x18」，并测试 0x14/0x16 向量寄存器读写，最后释放向量。 */
#include <errno.h>
#include <fcntl.h>
#include <linux/vfio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void dump_bar0(int d, uint64_t off, const char *tag)
{
    printf("BAR0 dump 0x00-0x3f (%s):\n", tag);
    for (int o = 0; o < 0x40; o += 4) {
        uint32_t x = 0;
        pread(d, &x, 4, off + o);
        printf("  [%02x] %08x", o, x);
        if ((o & 0xc) == 0xc) printf("\n");
    }
}

static void dump_table(int d, uint64_t off2, uint32_t tbl, int nr)
{
    for (int v = 0; v < nr && v < 8; v++) {
        uint8_t ent[16];
        if (pread(d, ent, 16, off2 + (tbl & ~7) + v * 16) != 16) {
            perror("pread entry"); return;
        }
        uint32_t a0, a1, data, ctrl;
        memcpy(&a0, ent, 4); memcpy(&a1, ent + 4, 4);
        memcpy(&data, ent + 8, 4); memcpy(&ctrl, ent + 12, 4);
        printf("vec%d: addr=%08x:%08x data=%08x ctrl=%08x %s\n",
               v, a1, a0, data, ctrl, (ctrl & 1) ? "[MASKED]" : "[unmasked]");
    }
}

int main(int argc, char **argv)
{
    int do_reset = 0, do_arm = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) do_reset = 1;
        if (!strcmp(argv[i], "-a")) do_arm = 1;
    }
    int c = open("/dev/vfio/vfio", O_RDWR);
    if (c < 0) { perror("open container"); return 1; }
    int g = open("/dev/vfio/35", O_RDWR);
    if (g < 0) { perror("open group"); return 1; }
    if (ioctl(g, VFIO_GROUP_SET_CONTAINER, &c) < 0) { perror("SET_CONTAINER"); return 1; }
    if (ioctl(c, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0) { perror("SET_IOMMU"); return 1; }
    int d = ioctl(g, VFIO_GROUP_GET_DEVICE_FD, "0000:4b:00.0");
    if (d < 0) { perror("GET_DEVICE_FD"); return 1; }
    if (do_reset)
        printf("VFIO_DEVICE_RESET rc=%d\n", ioctl(d, VFIO_DEVICE_RESET));

    struct vfio_region_info bar2 = { .argsz = sizeof(bar2),
                                     .index = VFIO_PCI_BAR2_REGION_INDEX };
    if (ioctl(d, VFIO_DEVICE_GET_REGION_INFO, &bar2) < 0) { perror("BAR2 info"); return 1; }
    struct vfio_region_info bar0 = { .argsz = sizeof(bar0),
                                     .index = VFIO_PCI_BAR0_REGION_INDEX };
    if (ioctl(d, VFIO_DEVICE_GET_REGION_INFO, &bar0) < 0) { perror("BAR0 info"); return 1; }
    struct vfio_region_info cfg = { .argsz = sizeof(cfg),
                                    .index = VFIO_PCI_CONFIG_REGION_INDEX };
    if (ioctl(d, VFIO_DEVICE_GET_REGION_INFO, &cfg) < 0) { perror("cfg info"); return 1; }

    uint8_t cap_ptr = 0;
    pread(d, &cap_ptr, 1, cfg.offset + 0x34);
    int msix_cap = 0;
    for (int i = 0, off = cap_ptr & 0xfc; off && i < 16; i++) {
        uint8_t id = 0, nxt = 0;
        pread(d, &id, 1, cfg.offset + off);
        pread(d, &nxt, 1, cfg.offset + off + 1);
        if (id == 0x11) { msix_cap = off; break; }
        off = nxt & 0xfc;
    }
    uint16_t msgctl = 0;
    uint32_t tbl = 0;
    pread(d, &msgctl, 2, cfg.offset + msix_cap + 2);
    pread(d, &tbl, 4, cfg.offset + msix_cap + 4);
    int nr = (msgctl & 0x7ff) + 1;
    printf("msix cap@0x%x msgctl=%04x (Enable=%d FuncMask=%d nr=%d) table=BAR%d+0x%x\n",
           msix_cap, msgctl, !!(msgctl & 0x8000), !!(msgctl & 0x4000),
           nr, tbl & 7, tbl & ~7);

    uint16_t cmd = 0;
    pread(d, &cmd, 2, cfg.offset + 0x04);
    printf("command=%04x (mem=%d busmaster=%d)\n", cmd, !!(cmd & 2), !!(cmd & 4));
    if (!(cmd & 2)) {
        cmd |= 2;
        pwrite(d, &cmd, 2, cfg.offset + 0x04);
        printf("command 置 Memory Space → %04x\n", cmd);
    }

    uint32_t feat = 0, cap_lo = 0, cap_hi = 0;
    pread(d, &feat, 4, bar0.offset + 0);
    pread(d, &cap_lo, 4, bar0.offset + 0x14);
    pread(d, &cap_hi, 4, bar0.offset + 0x18);
    printf("BAR0[0x00] features=%08x  [0x14]=%08x [0x18]=%08x\n",
           feat, cap_lo, cap_hi);
    dump_bar0(d, bar0.offset, "arm 前");
    dump_table(d, bar2.offset, tbl, nr);

    if (!do_arm)
        return 0;

    /* ---- arm：SET_IRQS(MSIX, TRIGGER, vec0/1) → 设备 MSI-X Enable 置位 ---- */
    int efd[2] = { eventfd(0, 0), eventfd(0, 0) };
    struct vfio_irq_set *set = calloc(1, sizeof(*set) + 2 * sizeof(int));
    set->argsz = sizeof(*set) + 2 * sizeof(int);
    set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    set->index = VFIO_PCI_MSIX_IRQ_INDEX;
    set->start = 0;
    set->count = 2;
    memcpy(set->data, efd, sizeof(efd));
    int rc = ioctl(d, VFIO_DEVICE_SET_IRQS, set);
    printf("SET_IRQS(MSIX START vec0-1) rc=%d errno=%d(%s)\n",
           rc, errno, rc < 0 ? strerror(errno) : "-");
    free(set);

    pread(d, &msgctl, 2, cfg.offset + msix_cap + 2);
    printf("arm 后 msgctl=%04x (Enable=%d FuncMask=%d)\n",
           msgctl, !!(msgctl & 0x8000), !!(msgctl & 0x4000));
    dump_bar0(d, bar0.offset, "arm 后");
    dump_table(d, bar2.offset, tbl, nr);

    /* 0x14/0x16 若为向量寄存器：写 0/1 应能读回 */
    uint16_t v = 0, w = 0;
    pwrite(d, &(uint16_t){0}, 2, bar0.offset + 0x14);
    pwrite(d, &(uint16_t){1}, 2, bar0.offset + 0x16);
    pread(d, &v, 2, bar0.offset + 0x14);
    pread(d, &w, 2, bar0.offset + 0x16);
    printf("arm 后 BAR0[0x14] 写0读回=%04x  BAR0[0x16] 写1读回=%04x\n", v, w);
    uint32_t lo18 = 0, hi1c = 0;
    pread(d, &lo18, 4, bar0.offset + 0x18);
    pread(d, &hi1c, 4, bar0.offset + 0x1c);
    printf("arm 后 [0x18]=%08x [0x1c]=%08x（若为 capacity 应为 0/1）\n", lo18, hi1c);

    /* 释放向量，看布局是否切回 */
    struct vfio_irq_set *rel = calloc(1, sizeof(*rel));
    rel->argsz = sizeof(*rel);
    rel->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
    rel->index = VFIO_PCI_MSIX_IRQ_INDEX;
    rel->start = 0;
    rel->count = 0;
    rc = ioctl(d, VFIO_DEVICE_SET_IRQS, rel);
    printf("SET_IRQS(MSIX 释放) rc=%d\n", rc);
    free(rel);
    pread(d, &msgctl, 2, cfg.offset + msix_cap + 2);
    printf("释放后 msgctl=%04x (Enable=%d)\n", msgctl, !!(msgctl & 0x8000));
    dump_bar0(d, bar0.offset, "释放后");
    return 0;
}
