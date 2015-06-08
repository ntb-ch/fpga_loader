ifeq ($(KERNELRELEASE),)

EXTRAVERSION = +

	PWD := $(shell pwd)
modules:
	$(CHROOT_CMD) $(MAKE) -C $(KERNELDIR) M=$(PWD) modules
clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions modules.order Module.symvers

.PHONY: modules clean

else
	ccflags-y := -std=gnu99
	obj-m := fpga_loader.o
	
endif
