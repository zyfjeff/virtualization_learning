# Phase 7 时钟虚拟化实验 Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGETS = experiment1-tsc experiment2-kvmclock experiment3-lapic

all: $(TARGETS)

experiment1-tsc: experiment1-tsc.c common.h
	$(CC) $(CFLAGS) -o $@ $<

experiment2-kvmclock: experiment2-kvmclock.c common.h
	$(CC) $(CFLAGS) -o $@ $<

experiment3-lapic: experiment3-lapic.c common.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
