# Phase 2 实战练习 Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGETS = ept_violation_demo memtype_analysis

all: $(TARGETS)

ept_violation_demo: ept_violation_demo.c
	$(CC) $(CFLAGS) -o $@ $<

memtype_analysis: memtype_analysis.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
