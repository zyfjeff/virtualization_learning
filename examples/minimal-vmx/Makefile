# vmx-demo.ko Makefile (完整版 - 执行VMXON/VMCS/VMLAUNCH)
obj-m += vmx-demo.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load: all
	@echo "★ 警告: 确保KVM已卸载"
	@echo "  sudo rmmod kvm_intel kvm"
	sudo insmod vmx-demo.ko
	sudo dmesg | tail -50

unload:
	sudo rmmod vmx-demo 2>/dev/null || true

.PHONY: all clean load unload
