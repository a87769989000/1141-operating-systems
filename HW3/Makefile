
obj-m := kfetch_mod_A141064.o

KDIR ?= $(PWD)/linux-v6.8-devkit
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
