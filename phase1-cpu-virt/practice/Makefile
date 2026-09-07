# Phase 1 实战练习 Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -static
TARGETS = ex1-vmx-verify ex2-cpuid-fault ex3-msr-test ex5-vmexit-overhead

all: $(TARGETS)

ex1-vmx-verify: ex1-vmx-verify.c
	$(CC) $(CFLAGS) -o $@ $<

ex2-cpuid-fault: ex2-cpuid-fault.c
	$(CC) $(CFLAGS) -o $@ $<

ex3-msr-test: ex3-msr-test.c
	$(CC) $(CFLAGS) -o $@ $<

ex5-vmexit-overhead: ex5-vmexit-overhead.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
