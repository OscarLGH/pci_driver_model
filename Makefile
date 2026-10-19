obj-m += pci_driver_model.o
pci_driver_model-y += pci_driver.o

# vfio-mdev back end of the same device (QEMU: -device vfio-pci,sysfsdev=...);
# needs CONFIG_VFIO_MDEV and CONFIG_VFIO, and depends on pci_driver_virt.ko
# being loaded first if pci_driver.c ever calls its registration helpers.
obj-m += pci_driver_virt.o
pci_driver_virt-y += virt.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

# riscv is the target of this harness; override for a native check, e.g.
#   make ARCH=x86_64 CROSS_COMPILE= CC=gcc all
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-
CC_USER ?= $(CROSS_COMPILE)gcc

# riscv modules need the unwind tables off (objtool chokes otherwise)
KCFLAGS_EXTRA = -fno-asynchronous-unwind-tables -fno-unwind-tables
USER_CFLAGS = -O2 -Wall -Wextra -std=gnu11

all:
	make ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNELDIR) M=$(PWD) modules KCFLAGS="$(KCFLAGS_EXTRA)"
	$(CC_USER) $(USER_CFLAGS) pci_driver_user.c -o pci_driver_user.out

# module only, against the headers of the running kernel
x86:
	make ARCH=x86_64 -C $(KERNELDIR) M=$(PWD) modules

# user program only, with the host compiler
user:
	$(CC) $(USER_CFLAGS) pci_driver_user.c -o pci_driver_user.out

clean:
	make -C $(KERNELDIR) M=$(PWD) clean
	rm -f pci_driver_user.out
