# Phase 6 实战练习 Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGETS = vfio-claim-trace vfio-dma-map vfio-msix-probe

all: $(TARGETS)

vfio-claim-trace: vfio-claim-trace.c
	$(CC) $(CFLAGS) -o $@ $<

vfio-dma-map: vfio-dma-map.c
	$(CC) $(CFLAGS) -o $@ $<

vfio-msix-probe: vfio-msix-probe.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
