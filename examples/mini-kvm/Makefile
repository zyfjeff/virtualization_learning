# Mini-KVM Makefile
#
# 构建简化版 KVM 内核模块
#
# 用法:
#   make         - 构建内核模块
#   make user    - 构建用户空间测试程序
#   make clean   - 清理所有构建产物
#   make load    - 加载模块 (需先卸载 kvm)
#   make unload  - 卸载模块

# 内核源码路径 (根据实际环境调整)
KDIR ?= /root/code/linux-6.12.93

# 模块名
obj-m := mini-kvm.o

# 编译选项
ccflags-y := -DDEBUG -g

.PHONY: all modules user clean load unload test

all: modules user

# 构建内核模块
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# 构建用户空间测试程序
user: test-mini-kvm.c
	gcc -Wall -O2 -o test-mini-kvm test-mini-kvm.c

# 清理
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test-mini-kvm

# 加载模块 (必须先卸载 KVM)
load: modules
	@echo "检查 KVM 模块状态..."
	@lsmod | grep -q kvm_intel && { \
		echo "⚠ KVM 模块正在使用，请先执行: sudo rmmod kvm_intel kvm"; \
		exit 1; \
	} || true
	sudo insmod mini-kvm.ko
	@echo ""
	@echo "✓ Mini-KVM 模块已加载"
	@dmesg | tail -15

# 卸载模块
unload:
	sudo rmmod mini_kvm 2>/dev/null || true
	@echo "✓ Mini-KVM 模块已卸载"

# 运行测试
test: user
	sudo ./test-mini-kvm

# 恢复 KVM
restore: unload
	sudo modprobe kvm_intel
	@echo "✓ 标准 KVM 已恢复"
