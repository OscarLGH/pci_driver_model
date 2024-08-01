#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#define PCI_MODEL_IOCTL_MAGIC 0x5536
#define PCI_MODEL_IOCTL_GET_BAR_INFO	_IOR(PCI_MODEL_IOCTL_MAGIC, 1, void *)
#define PCI_MODEL_IOCTL_SET_IRQFD		_IOW(PCI_MODEL_IOCTL_MAGIC, 2, void *)
#define PCI_MODEL_IOCTL_SET_IRQ			_IOW(PCI_MODEL_IOCTL_MAGIC, 3, void *)
#define PCI_MODEL_IOCTL_ALLOC_DMA		_IOW(PCI_MODEL_IOCTL_MAGIC, 4, void *)
#define PCI_MODEL_IOCTL_FREE_DMA		_IOW(PCI_MODEL_IOCTL_MAGIC, 5, void *)

#define DEVICE_NAME "/dev/pci_dev_%d"
#define MAX_PCI_DEV 255

struct pci_bar_reg {
	uint64_t base;
	size_t size;
	uint32_t *virt_kernel; //not used, do not touch.
	uint32_t *virt_user;
};

typedef struct pci_device {
    int index;
    int device_fd;
    int event_fd;
    struct pci_bar_reg bar_regs[6];
} pci_device;

typedef struct dma_region {
    uint64_t dma_buffer_phys;
	void *unused; //do not touch!
    void *dma_buffer_virt_user;
	size_t dma_buffer_size;
} dma_region;

int enum_devices()
{
    int device_fd = 0;
    int cnt = 0;
    char buf[256] = {0};
    for (int i = 0; i < MAX_PCI_DEV; i++) {
        sprintf(buf, DEVICE_NAME, i);
        device_fd = open(buf, O_RDWR);
        if (device_fd > 0) {
            cnt++;
            close(device_fd);
        }
    }
    return cnt;
}

pci_device *open_pci_device(int index)
{
    int device_fd;
    pci_device *ret;
    int ret_int;
    int i;
    char buf[256] = {0};
    sprintf(buf, DEVICE_NAME, index);
    device_fd = open(buf, O_RDWR);
    if (device_fd < 0) {
        printf("open device error!\n");
        return NULL;
    }
    ret = malloc(sizeof(*ret));
    ret->index = index;
    ret->device_fd = device_fd;
    ret->event_fd = eventfd(0, EFD_CLOEXEC);
    ret_int = ioctl(ret->device_fd, PCI_MODEL_IOCTL_SET_IRQFD, ret->event_fd);
    if (ret_int < 0) {
        printf("setting eventfd failed.\n", ret_int);
        free(ret);
        return NULL;
    }

    ret_int = ioctl(ret->device_fd, PCI_MODEL_IOCTL_GET_BAR_INFO, ret->bar_regs);
    if (ret_int < 0) {
        printf("get bar info failed.\n", ret_int);
        free(ret);
        return NULL;
    }

    for (i = 0; i < 6; i++) {
        printf("BAR%d Base = 0x%llx size = 0x%llx\n", i, ret->bar_regs[i].base, ret->bar_regs[i].size);
        if (ret->bar_regs[i].size < 0x1000)
            continue;
        ret->bar_regs[i].virt_user = mmap(NULL, ret->bar_regs[i].size, PROT_READ | PROT_WRITE, MAP_SHARED, ret->device_fd, ret->bar_regs[i].base);
        if (ret->bar_regs[i].virt_user == MAP_FAILED) {
            printf("mmap regs failed.\n", ret_int);
            free(ret);
            return NULL;
        }
    }

    return ret;
}

int close_pci_device(pci_device *dev)
{
    if (dev == NULL)
        return -1;
    close(dev->device_fd);
}

dma_region *allocate_dma(pci_device *dev, size_t dma_size)
{
    int ret;
    dma_region *dma_desc = malloc(sizeof(*dma_desc));
    if (dev == NULL)
        return NULL;

    dma_desc->dma_buffer_size = dma_size;
    ret = ioctl(dev->device_fd, PCI_MODEL_IOCTL_ALLOC_DMA, dma_desc);
    if (ret < 0) {
        printf("allocate dma error, ret = %d\n", ret);
        free(dma_desc);
        return NULL;
    }
    dma_desc->dma_buffer_virt_user =
            mmap(NULL, dma_desc->dma_buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->device_fd, dma_desc->dma_buffer_phys);
    if (!dma_desc->dma_buffer_virt_user) {
        ioctl(dev->device_fd, PCI_MODEL_IOCTL_FREE_DMA, dma_desc);
        printf("mmap dma error, ret = %d\n", ret);
        free(dma_desc);
        return NULL;
    }

    return dma_desc;
}

int free_dma(pci_device *dev, dma_region * dma_desc)
{
    if (dev == NULL)
        return NULL;

    if (!dma_desc)
        return -1;

    munmap(dma_desc->dma_buffer_virt_user, dma_desc->dma_buffer_size);
    ioctl(dev->device_fd, PCI_MODEL_IOCTL_FREE_DMA, dma_desc);
    return 0;
}

int send_command(pci_device *dev, uint64_t *cmd, int size)
{
    int ret = 0;

    if (dev == NULL)
        return -1;
    ret = write(dev->device_fd, cmd, size);
    if (ret < 0) {
        printf("write error, ret = %d\n", ret);
    }
    return ret;
}

int send_command_fast(pci_device *dev, uint64_t *cmd, int size)
{
    int ret = 0;
    int i;

    if (dev == NULL)
        return -1;
    
    for (i = 0; i < size / 8; i++) {
        dev->bar_regs[0].virt_user = cmd[i];
    }
    return ret;
}

int wait_command_finish(pci_device *dev)
{
    int ret;
    long event;
    ret = read(dev->event_fd, &event, sizeof(event));
    return 0;
}

int main()
{
    int i, dev_idx;
    int ret;
    int cnt;
    struct dma_region *dma_regions[8] = {0};
    uint64_t *dma_region_ptr;
    uint64_t cmd[0x100] = {0};

    cnt = enum_devices();

    printf("device num:%d\n", cnt);

    for (dev_idx = 0; dev_idx < cnt; dev_idx++) {
        printf("opening device:%d\n", dev_idx);
        pci_device *dev_ptr = open_pci_device(dev_idx);
        if (dev_ptr == NULL) {
            printf("open device error!\n");
            return -1;
        }

        //allocate 8 4K dma buffers.
        for (i = 0; i < 8; i++) {
            dma_regions[i] = allocate_dma(dev_ptr, 0x1000);
            if (!dma_regions[i]) {
                printf("allocate dma error!\n");
                return -1;
            }
            printf("allocated dma:phys addr = 0x%llx virt addr = 0x%llx size = %d\n",
                dma_regions[i]->dma_buffer_phys,
                dma_regions[i]->dma_buffer_virt_user,
                dma_regions[i]->dma_buffer_size
            );
        }

        dma_region_ptr = dma_regions[0]->dma_buffer_virt_user;

        dma_region_ptr[0] = 0x55aaaa55;
        dma_region_ptr[1] = 0xaa5555aa;
        dma_region_ptr[2] = 0x12345678;
        dma_region_ptr[3] = 0x87654321;


        cmd[0] = 0x0;
        cmd[1] = 0x1;
        cmd[2] = 0x2;
        cmd[3] = 0x3;

        dev_ptr->bar_regs[0].virt_user[0] = 0x12345678;
        printf("reg0:0x%llx\n", dev_ptr->bar_regs[0].virt_user[0]);

        printf("waiting for command to finish...\n");
        wait_command_finish(dev_ptr);
        printf("command finished.\n");

        // free dma buffers.
        for (i = 0; i < 8; i++) {
            ret = free_dma(dev_ptr, dma_regions[i]);
            if (ret < 0) {
                printf("free dma error, ret = %d\n", ret);
                return -1;
            }
        }

        close_pci_device(dev_ptr);

    }
    return 0;
}

