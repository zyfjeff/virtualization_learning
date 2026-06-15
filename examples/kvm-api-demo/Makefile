# KVM API Demo Makefile
# 编译环境: Linux + KVM 支持 (需要 /dev/kvm)

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=gnu11
TARGETS = kvm-demo kvm-demo-regs

# 默认构建所有目标
all: $(TARGETS)

kvm-demo: kvm-demo.c
	$(CC) $(CFLAGS) -o $@ $<

kvm-demo-regs: kvm-demo-regs.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

# 快速运行 (需要 root 权限或 kvm 组)
run: kvm-demo
	sudo ./kvm-demo

run-regs: kvm-demo-regs
	sudo ./kvm-demo-regs

.PHONY: all clean run run-regs
