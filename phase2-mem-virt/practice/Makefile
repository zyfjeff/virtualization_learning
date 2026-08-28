# Phase 2 实战练习 Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGETS = ept_violation_demo memtype_analysis memtest concurrent_test

all: $(TARGETS)

ept_violation_demo: ept_violation_demo.c
	$(CC) $(CFLAGS) -o $@ $<

memtype_analysis: memtype_analysis.c
	$(CC) $(CFLAGS) -o $@ $<

memtest: memtest.c
	$(CC) $(CFLAGS) -o $@ $<

concurrent_test: concurrent_test.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
