# Phase 1 实战练习 Makefile
#
# 编译所有练习程序 (Guest 内执行)
# 宿主侧脚本 (trace-*.sh, profile-*.sh) 不需要编译

CC = gcc
CFLAGS = -Wall -Wextra -O2 -static
TARGETS = ex1-vmx-verify ex2-cpuid-fault ex3-msr-test ex4-cpuid-analysis ex5-vmexit-overhead

all: $(TARGETS)

ex1-vmx-verify: ex1-vmx-verify.c
	$(CC) $(CFLAGS) -o $@ $<

ex2-cpuid-fault: ex2-cpuid-fault.c
	$(CC) $(CFLAGS) -o $@ $<

ex3-msr-test: ex3-msr-test.c
	$(CC) $(CFLAGS) -o $@ $<

ex4-cpuid-analysis: ex4-cpuid-analysis.c
	$(CC) $(CFLAGS) -o $@ $<

ex5-vmexit-overhead: ex5-vmexit-overhead.c
	$(CC) $(CFLAGS) -o $@ $<

# 宿主侧脚本需要可执行权限
scripts: trace-msr-access.sh profile-vmexit.sh trace-vcpu-sched.sh
	chmod +x trace-msr-access.sh profile-vmexit.sh trace-vcpu-sched.sh

clean:
	rm -f $(TARGETS)

.PHONY: all clean scripts
