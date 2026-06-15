# vmx-info.ko Makefile (安全版 - 只读取MSR)
obj-m += vmx-info.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load: all
	sudo insmod vmx-info.ko
	sudo dmesg | tail -40

unload:
	sudo rmmod vmx-info 2>/dev/null || true

.PHONY: all clean load unload
