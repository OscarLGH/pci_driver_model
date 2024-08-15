obj-m +=pci_driver_model.o
pci_driver_model-y += pci_driver.o

#all:
#	make ARCH=x86_64 -C $(KERNELDIR) M=$(PWD) modules KCFLAGS="-Wno-error"
#	gcc pci_driver_user.c -o pci_driver_user.out
#all_riscv:
all:
	make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -C $(KERNELDIR) M=$(PWD) modules KCFLAGS="-fno-asynchronous-unwind-tables -fno-unwind-tables"
	riscv64-linux-gnu-gcc pci_driver_user.c -o pci_driver_user.out
clean:
	make -C $(KERNELDIR) M=$(PWD) clean
