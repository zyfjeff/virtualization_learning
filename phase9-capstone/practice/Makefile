CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
LDLIBS   = -lpthread

all: minivmm

minivmm: minivmm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f minivmm

.PHONY: all clean
