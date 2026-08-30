# pi-demo.ko Makefile
# Posted Interrupts 机制演示内核模块

obj-m += pi-demo.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 加载模块
load: all
	sudo insmod pi-demo.ko
	@echo ""
	@echo "模块已加载，查看结果:"
	@echo "  cat /proc/pi-demo"
	@echo "  dmesg | tail -20"

# 卸载模块
unload:
	sudo rmmod pi-demo 2>/dev/null || true
	@echo "模块已卸载"

# 查看结果
show:
	cat /proc/pi-demo

.PHONY: all clean load unload show
