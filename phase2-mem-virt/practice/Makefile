# Phase 2 实战练习 Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGETS = ept_violation_demo memtype_analysis memtest concurrent_test \
          check-ept-ad read-ept-ad-msr

all: $(TARGETS)

ept_violation_demo: ept_violation_demo.c
	$(CC) $(CFLAGS) -o $@ $<

memtype_analysis: memtype_analysis.c
	$(CC) $(CFLAGS) -o $@ $<

memtest: memtest.c
	$(CC) $(CFLAGS) -o $@ $<

concurrent_test: concurrent_test.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

# EPT A/D 诊断工具（2026-11-25 自仓库根目录移入）
check-ept-ad: check-ept-ad.c
	$(CC) $(CFLAGS) -o $@ $<

read-ept-ad-msr: read-ept-ad-msr.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
