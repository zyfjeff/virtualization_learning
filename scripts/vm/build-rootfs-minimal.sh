#!/bin/bash
# 构建极简 rootfs（仅用于 KVM 测试）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGES_DIR="$PROJECT_ROOT/images"
ROOTFS_DIR="$IMAGES_DIR/rootfs"

# 颜色输出
log_info() { echo -e "\033[0;32m[INFO]\033[0m $1"; }
log_warn() { echo -e "\033[0;33m[WARN]\033[0m $1"; }
log_error() { echo -e "\033[0;31m[ERROR]\033[0m $1"; }

# 检查依赖
check_dependencies() {
    log_info "检查依赖..."

    if ! command -v busybox &>/dev/null; then
        log_error "busybox 未安装"
        echo "  Ubuntu/Debian: sudo apt install busybox-static"
        exit 1
    fi

    if ! command -v cpio &>/dev/null; then
        log_error "cpio 未安装"
        echo "  Ubuntu/Debian: sudo apt install cpio"
        exit 1
    fi

    log_info "✓ 依赖检查通过"
}

# 创建最小目录结构
create_minimal_structure() {
    log_info "创建最小目录结构..."

    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"/{bin,sbin,proc,sys,dev,tmp,mnt/shared}

    # 复制静态编译的 busybox
    cp "$(which busybox)" "$ROOTFS_DIR/bin/"
    cd "$ROOTFS_DIR/bin"
    # applet 全部指向同一个 busybox 二进制，靠 argv[0] 分发。
    # 这里列的是 phase8 各验收项真正要用的：
    #   项目1 串口控制台交互 + /proc/interrupts 计数
    #   项目2 virtio-console(/dev/hvc0)、virtio-blk(/dev/vda: dd / mke2fs / mount)
    #   项目3 PCI 直通设备可见性与读写
    #   项目4 启动延迟与负载脚本
    # 名单以 `busybox --list` 实际输出为准，不在名单里的不要加（会是死链）。
    for applet in \
        sh mount umount mkdir rmdir ls cat echo cp mv rm touch stat \
        dmesg grep head tail wc sort tr cut uniq tee xargs find \
        dd hexdump od printf seq uname stty sleep date uptime time \
        ps kill free top \
        mke2fs md5sum fallocate insmod poweroff reboot halt; do
        ln -sf busybox "$applet"
    done
    cd - >/dev/null

    log_info "✓ 目录结构创建完成"
}

# 创建极简 /init
create_init() {
    log_info "创建 /init..."

    cat > "$ROOTFS_DIR/init" <<'EOF'
#!/bin/sh
# 最小 initramfs 的 PID 1。被内核以 /init 或 rdinit=/init 调用。
# 交互用：exec /bin/sh；自动化用：cmdline 里加 autotest。

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mount -t tmpfs tmpfs /tmp 2>/dev/null || true

# 9p 共享目录只有 QEMU (-virtfs) 才有；自研 VMM (phase8) 里必然失败，忽略
mkdir -p /mnt/shared
# busybox mount 语法：先检查是否支持 9p
if grep -q "9p" /proc/filesystems 2>/dev/null; then
    mount -t 9p -o "trans=virtio,version=9p2000.L" hostshare /mnt/shared 2>/dev/null || true
fi

echo "---- initramfs ----"
echo "kernel       : $(uname -r)"
echo "cmdline      : $(cat /proc/cmdline)"
echo "hypervisor   : $(dmesg | grep -m1 'Hypervisor detected' || echo '(none — PV 未生效)')"
echo "clocksource  : $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null)"
echo "timer source : $(dmesg | grep -m1 'Switched to clocksource')"
echo "console      : $(dmesg | grep -m1 'ttyS0 at I/O' || echo '(无 8250)')"
echo "block devs   : $(ls /dev/vd* /dev/sd* 2>/dev/null | tr '\n' ' ')"
echo "virtio devs  : $(ls /sys/bus/virtio/devices/ 2>/dev/null | tr '\n' ' ')"
echo "irq counts   : $(grep -c . /proc/interrupts 2>/dev/null) 行"
echo "-------------------"

case " $(cat /proc/cmdline) " in
    *" autotest "*)
        # 自动化收尾：打印宿主侧可识别的就绪标记，然后强制重启。
        # phase8 项目 4 用它循环测量启动延迟（minivmm 与 QEMU 同一判据）。
        echo "MINIVMM_READY"
        reboot -f
        exit 0
        ;;
esac

exec /bin/sh
EOF

    chmod +x "$ROOTFS_DIR/init"
    log_info "✓ /init 创建完成"
}

# 打包为 initramfs
create_initramfs() {
    log_info "打包为 initramfs..."

    local img="$IMAGES_DIR/initramfs.img"

    cd "$ROOTFS_DIR"
    # set -e 不覆盖管道中间命令，缺 pipefail 时 cpio/gzip 失败会写出截断镜像而构建仍报成功
    local rc=0
    ( set -o pipefail; find . | cpio -o -H newc 2>/dev/null | gzip > "$img" ) || rc=$?
    cd - >/dev/null

    if [ "$rc" -ne 0 ] || ! gzip -t "$img" 2>/dev/null; then
        rm -f "$img"
        log_error "initramfs 打包失败（rc=$rc）或 gzip -t 校验未通过，已删除损坏产物"
        exit 1
    fi

    local size=$(du -h "$img" | cut -f1)
    log_info "✓ initramfs 创建完成 ($size)"
}

# 主函数
main() {
    echo ""
    echo "========================================"
    echo "  KVM Study - Minimal Rootfs Builder"
    echo "========================================"
    echo ""

    check_dependencies
    create_minimal_structure
    create_init
    create_initramfs

    echo ""
    echo "========================================"
    echo "  ✓ Rootfs 构建完成!"
    echo "========================================"
    echo ""
    echo "  输出: $IMAGES_DIR/initramfs.img"
    echo ""
    echo "  使用方法:"
    echo "    ./boot-vm.sh"
    echo ""
}

main "$@"
