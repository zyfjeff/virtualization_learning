# SPDX-License-Identifier: GPL-2.0
#
# mini-kvm 构建文件（Phase 8 毕业建造 · KVM 内核侧）
#
# 关键约束：模块必须针对"正在运行的内核"构建（vermagic 一致才能 insmod）：
#   KDIR = /lib/modules/$(shell uname -r)/build
# 不要指向 6.12.93 参考源码树——那棵树只用于阅读与引用，不是构建目标。
#
# 用法:
#   make            - 构建内核模块
#   make guest      - 构建 guest 裸机镜像 (guest/guest.bin)
#   make user       - 构建用户空间测试程序
#   make clean      - 清理所有构建产物
#   make load / unload / restore - 加载/卸载/恢复（见 README 的 SDL 流程，勿跳过检查）

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m := mini-kvm.o
mini-kvm-y := main.o vmx.o vmx_entry.o ept.o interrupt.o device.o vcpu.o

GUEST_ASFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone \
                 -fno-pie -fno-asynchronous-unwind-tables
GUEST_LDFLAGS := -nostdlib -static -T guest/guest.ld

.PHONY: all modules guest user clean load unload restore

all: modules

# ---- 内核模块 -------------------------------------------------------------
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ---- guest 裸机镜像 --------------------------------------------------------
# guest.S 汇编 → guest.ld 链接到 GPA 0x1000 → objcopy 提取 .bin 纯镜像
guest: guest/guest.bin

guest/guest.bin: guest/guest.S guest/guest.ld
	gcc $(GUEST_ASFLAGS) -c guest/guest.S -o guest/guest.o
	ld $(GUEST_LDFLAGS) guest/guest.o -o guest/guest.elf
	objcopy -O binary guest/guest.elf guest/guest.bin

# ---- 用户空间测试程序 -------------------------------------------------------
user: test-mini-kvm
test-mini-kvm: test-mini-kvm.c
	gcc -Wall -Wextra -O2 -o test-mini-kvm test-mini-kvm.c

# ---- 清理 ------------------------------------------------------------------
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f guest/guest.o guest/guest.elf guest/guest.bin test-mini-kvm

# ---- 便捷目标（生产环境请走 README 的完整 SDL 检查流程） ---------------------
load: modules
	@lsmod | grep -q '^kvm_intel' && { \
		echo "!! kvm_intel 仍在，先确认无 VM 再: rmmod kvm_intel kvm"; exit 1; } || true
	fuser /dev/kvm 2>/dev/null && { echo "!! /dev/kvm 被占用"; exit 1; } || true
	insmod mini-kvm.ko

unload:
	rmmod mini_kvm

restore: unload
	modprobe kvm_intel
	@lsmod | grep kvm_intel && echo "✓ 标准 KVM 已恢复"
