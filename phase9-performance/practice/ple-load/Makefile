# out-of-tree 编译：在**宿主**上做，目标内核必须与 guest 里跑的那个一致。
#
# 本仓的 guest 内核就是 /root/code/linux-6.12.93（scripts/vm/boot-vm.sh:178
# 直接取该树的 arch/x86_64/boot/bzImage），所以 KDIR 默认指向它。
# 换 guest 内核时改 KDIR，否则 vermagic 不匹配、insmod 直接被拒。
#
# guest rootfs 里没有编译器（scripts/images/rootfs/ 下无 gcc），
# 只能在宿主编译，再经 9p 共享区送进去：
#     make && make install      # → scripts/shared/ → guest /mnt/shared/

KDIR   ?= /root/code/linux-6.12.93
SHARE  ?= /root/code/kvm-study/scripts/shared

obj-m += ple_load.o

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install: all
	mkdir -p $(SHARE)
	cp -f ple_load.ko $(SHARE)/
	@echo "→ $(SHARE)/ple_load.ko   guest 内： insmod /mnt/shared/ple_load.ko ..."
