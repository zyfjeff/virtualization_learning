# CPUID Faulting Demo Makefile
#
# 编译 CPUID Faulting 测试程序
#
# 用法:
#   make              # 编译所有程序
#   make test         # 编译并运行测试
#   make clean        # 清理编译产物
#   make help         # 显示帮助

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS =

# 目标程序
TARGETS = test-cpuid-fault test-cpuid-fault-kvm

# 默认目标
all: $(TARGETS)

# 编译用户态测试程序
test-cpuid-fault: test-cpuid-fault.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# 编译 KVM 测试程序
test-cpuid-fault-kvm: test-cpuid-fault-kvm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# 运行测试
test: test-cpuid-fault
	@echo "=== 运行用户态测试 ==="
	@./test-cpuid-fault
	@echo ""
	@echo "=== 运行 KVM 测试（需要在虚拟机中运行）==="
	@echo "请启动虚拟机后运行: ./test-cpuid-fault-kvm"

# 运行简单测试（不需要虚拟机）
test-simple: test-cpuid-fault
	./test-cpuid-fault

# 在 QEMU 虚拟机中测试
test-qemu: test-cpuid-fault test-cpuid-fault-kvm
	@echo "=== 在 QEMU 虚拟机中测试 ==="
	@echo "启动虚拟机..."
	qemu-system-x86_64 -enable-kvm -m 512 -nographic \
		-kernel /boot/vmlinuz-$$(uname -r) \
		-append "root=/dev/sda1 console=ttyS0 quiet" \
		-drive file=test.img,format=raw 2>/dev/null || \
	echo "请手动启动虚拟机并运行测试程序"

# 清理
clean:
	rm -f $(TARGETS) *.o

# 安装（可选）
install: $(TARGETS)
	install -m 755 $(TARGETS) /usr/local/bin/

# 卸载
uninstall:
	rm -f /usr/local/bin/test-cpuid-fault
	rm -f /usr/local/bin/test-cpuid-fault-kvm

# 帮助
help:
	@echo "CPUID Faulting Demo - Makefile 帮助"
	@echo ""
	@echo "目标:"
	@echo "  all            编译所有程序（默认）"
	@echo "  test           编译并运行测试"
	@echo "  test-simple    只运行用户态测试"
	@echo "  test-qemu      在 QEMU 虚拟机中测试"
	@echo "  clean          清理编译产物"
	@echo "  install        安装到 /usr/local/bin"
	@echo "  uninstall      卸载"
	@echo "  help           显示此帮助"
	@echo ""
	@echo "程序说明:"
	@echo "  test-cpuid-fault       用户态测试程序"
	@echo "  test-cpuid-fault-kvm   KVM 虚拟机测试程序"
	@echo ""
	@echo "示例:"
	@echo "  make                   # 编译"
	@echo "  ./test-cpuid-fault     # 运行用户态测试"
	@echo "  scp test-cpuid-fault-kvm vm:/root/"
	@echo "  ssh vm ./test-cpuid-fault-kvm"

# 声明伪目标
.PHONY: all test test-simple test-qemu clean install uninstall help
