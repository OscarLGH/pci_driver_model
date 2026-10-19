#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <inttypes.h>
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
#define MAX_PCI_DEV 255		/* must match PCI_MODEL_MAX_DEVICES */

/*
 * Mirror of the kernel ABI (pci_driver.h). The kernel-side pointer slots are
 * layout placeholders only -- the driver always reports them as zero.
 */
struct pci_bar_reg {
	uint64_t base;
	size_t size;
	uint64_t *virt_kernel;	//always zero, do not touch.
	uint64_t *virt_user;		//filled in by open_pci_device() from mmap()
};

typedef struct pci_device {
    int index;
    int device_fd;
    int event_fd;
    struct pci_bar_reg bar_regs[6];
} pci_device;

typedef struct dma_region {
    uint64_t dma_buffer_phys;
	void *unused; //always zero, do not touch!
    void *dma_buffer_virt_user;
	size_t dma_buffer_size;
} dma_region;

/*
 * Register map of BAR0, byte offsets -- must match the device model under
 * test. Offsets are used through bar_read64()/bar_write64() so an offset
 * outside the BAR is reported instead of faulting.
 */
#define REG_DMA_SRC	0x30
#define REG_DMA_DST	0x38
#define REG_DMA_LEN	0x40
#define REG_DMA_CTRL	0x48
#define REG_DMA_STAT	0x50
#define DMA_CTRL_TO_DEV	1
#define DMA_CTRL_FROM_DEV	2

/* byte offset inside BAR2 where the serial-string test writes */
#define BAR2_SERIAL_OFF	0x0

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
        printf("open %s failed: %s\n", buf, strerror(errno));
        return NULL;
    }
    ret = malloc(sizeof(*ret));
    if (ret == NULL) {
        printf("out of memory\n");
        close(device_fd);
        return NULL;
    }
    /* zeroed so a skipped BAR keeps virt_user == NULL instead of garbage */
    memset(ret, 0, sizeof(*ret));
    ret->index = index;
    ret->device_fd = device_fd;
    ret->event_fd = eventfd(0, EFD_CLOEXEC);
    if (ret->event_fd < 0) {
        printf("eventfd creation failed: %s\n", strerror(errno));
        close(ret->device_fd);
        free(ret);
        return NULL;
    }
    ret_int = ioctl(ret->device_fd, PCI_MODEL_IOCTL_SET_IRQFD, ret->event_fd);
    if (ret_int < 0) {
        printf("setting eventfd failed: %s\n", strerror(errno));
        close(ret->event_fd);
        close(ret->device_fd);
        free(ret);
        return NULL;
    }

    ret_int = ioctl(ret->device_fd, PCI_MODEL_IOCTL_GET_BAR_INFO, ret->bar_regs);
    if (ret_int < 0) {
        printf("get bar info failed: %s\n", strerror(errno));
        close(ret->event_fd);
        close(ret->device_fd);
        free(ret);
        return NULL;
    }

    for (i = 0; i < 6; i++) {
        printf("BAR%d Base = 0x%" PRIx64 " size = 0x%zx\n", i,
            ret->bar_regs[i].base, ret->bar_regs[i].size);
        if (ret->bar_regs[i].size < 0x1000)
            continue;
        ret->bar_regs[i].virt_user = mmap(NULL, ret->bar_regs[i].size, PROT_READ | PROT_WRITE, MAP_SHARED, ret->device_fd, (off_t)ret->bar_regs[i].base);
        if (ret->bar_regs[i].virt_user == MAP_FAILED) {
            printf("mmap BAR%d failed: %s\n", i, strerror(errno));
            ret->bar_regs[i].virt_user = NULL;
            goto err_unmap;
        }
    }

    return ret;

err_unmap:
    for (i = 0; i < 6; i++) {
        if (ret->bar_regs[i].virt_user) {
            munmap(ret->bar_regs[i].virt_user, ret->bar_regs[i].size);
            ret->bar_regs[i].virt_user = NULL;
        }
    }
    close(ret->event_fd);
    close(ret->device_fd);
    free(ret);
    return NULL;
}

int close_pci_device(pci_device *dev)
{
    int i;

    if (dev == NULL)
        return -1;
    for (i = 0; i < 6; i++) {
        if (dev->bar_regs[i].virt_user) {
            munmap(dev->bar_regs[i].virt_user, dev->bar_regs[i].size);
            dev->bar_regs[i].virt_user = NULL;
        }
    }
    if (dev->event_fd >= 0)
        close(dev->event_fd);
    if (dev->device_fd >= 0)
        close(dev->device_fd);
    free(dev);
    return 0;
}

/* BAR access helpers: offset is a byte offset inside the BAR, bounds checked */
static volatile uint64_t *bar_addr64(pci_device *dev, int bar, size_t offset)
{
    if (!dev || bar < 0 || bar >= 6)
        return NULL;
    if (!dev->bar_regs[bar].virt_user)
        return NULL;
    if (offset + sizeof(uint64_t) > dev->bar_regs[bar].size)
        return NULL;
    return (volatile uint64_t *)((volatile char *)dev->bar_regs[bar].virt_user + offset);
}

static volatile uint8_t *bar_addr8(pci_device *dev, int bar, size_t offset)
{
    if (!dev || bar < 0 || bar >= 6)
        return NULL;
    if (!dev->bar_regs[bar].virt_user)
        return NULL;
    if (offset + sizeof(uint8_t) > dev->bar_regs[bar].size)
        return NULL;
    return (volatile uint8_t *)((volatile char *)dev->bar_regs[bar].virt_user + offset);
}

int bar_write64(pci_device *dev, int bar, size_t offset, uint64_t value)
{
    volatile uint64_t *addr = bar_addr64(dev, bar, offset);
    if (!addr) {
        printf("BAR%d write at 0x%zx out of range or BAR not mapped\n", bar, offset);
        return -1;
    }
    *addr = value;
    return 0;
}

uint64_t bar_read64(pci_device *dev, int bar, size_t offset, int *err)
{
    volatile uint64_t *addr = bar_addr64(dev, bar, offset);
    if (err)
        *err = addr ? 0 : -1;
    return addr ? *addr : 0;
}

/*
 * P2P target BAR: BAR4 is the mmap-ed prefetchable window of the soc-sim
 * vfio-user device (memfd backed, guest side of the P2P target); BAR2 is the
 * trapped MMIO window. Prefer BAR4, fall back to BAR2.
 */
static int p2p_target_bar(pci_device *dev)
{
    if (dev->bar_regs[4].virt_user && dev->bar_regs[4].size >= 0x1000)
        return 4;
    if (dev->bar_regs[2].virt_user && dev->bar_regs[2].size >= 0x1000)
        return 2;
    return -1;
}

dma_region *allocate_dma(pci_device *dev, size_t dma_size)
{
    int ret;
    dma_region *dma_desc;

    if (dev == NULL)
        return NULL;

    dma_desc = malloc(sizeof(*dma_desc));
    if (dma_desc == NULL)
        return NULL;
    /* ALLOC_DMA copies the whole struct in, then fills it back in */
    memset(dma_desc, 0, sizeof(*dma_desc));
    dma_desc->dma_buffer_size = dma_size;
    ret = ioctl(dev->device_fd, PCI_MODEL_IOCTL_ALLOC_DMA, dma_desc);
    if (ret < 0) {
        printf("allocate dma error: %s\n", strerror(errno));
        free(dma_desc);
        return NULL;
    }
    dma_desc->dma_buffer_virt_user =
            mmap(NULL, dma_desc->dma_buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->device_fd, (off_t)dma_desc->dma_buffer_phys);
    if (dma_desc->dma_buffer_virt_user == MAP_FAILED) {
        printf("mmap dma error: %s\n", strerror(errno));
        dma_desc->dma_buffer_virt_user = NULL;
        ioctl(dev->device_fd, PCI_MODEL_IOCTL_FREE_DMA, dma_desc);
        free(dma_desc);
        return NULL;
    }

    return dma_desc;
}

int free_dma(pci_device *dev, dma_region * dma_desc)
{
    if (dev == NULL)
        return -1;

    if (!dma_desc)
        return -1;

    munmap(dma_desc->dma_buffer_virt_user, dma_desc->dma_buffer_size);
    if (ioctl(dev->device_fd, PCI_MODEL_IOCTL_FREE_DMA, dma_desc) < 0) {
        printf("free dma error: %s\n", strerror(errno));
        free(dma_desc);
        return -1;
    }
    free(dma_desc);
    return 0;
}

/*
 * The char device's write path is not implemented in the driver (it returns
 * -EOPNOTSUPP), so commands have to go through the BAR window; this wrapper is
 * kept only to show the failure.
 */
int send_command(pci_device *dev, uint64_t *cmd, int size)
{
    int ret = 0;

    if (dev == NULL)
        return -1;
    ret = write(dev->device_fd, cmd, size);
    if (ret < 0) {
        printf("write not supported: %s\n", strerror(errno));
    }
    return ret;
}

int send_command_fast(pci_device *dev, uint64_t *cmd, int size)
{
    int i;

    if (dev == NULL)
        return -1;

    /* the command block starts at BAR0 offset 0: index, not the pointer */
    for (i = 0; i < size / 8; i++) {
        if (bar_write64(dev, 0, (size_t)i * 8, cmd[i]) < 0)
            return -1;
    }
    return 0;
}

/* wait for one IRQ notification on the eventfd; timeout_ms < 0 waits forever */
int wait_command_finish(pci_device *dev, int timeout_ms)
{
    struct pollfd pfd = { .fd = dev->event_fd, .events = POLLIN };
    uint64_t event;
    ssize_t rd;
    int ret;

    ret = poll(&pfd, 1, timeout_ms);
    if (ret == 0) {
        printf("timeout waiting for IRQ\n");
        return -1;
    }
    if (ret < 0) {
        printf("poll failed: %s\n", strerror(errno));
        return -1;
    }

    rd = read(dev->event_fd, &event, sizeof(event));
    if (rd != (ssize_t)sizeof(event)) {
        printf("eventfd read failed: %s\n", strerror(errno));
        return -1;
    }
    printf("eventfd counter = %" PRIu64 "\n", event);
    return 0;
}

int main()
{
    int i, dev_idx;
    int ret;
    int cnt;
    int err;
    struct dma_region *dma_regions[8] = {0};
    uint64_t *dma_region_ptr;
    /* no command block here: main drives BAR0's DMA registers directly,
     * send_command()/send_command_fast() are left for other callers */

    pci_device *devs[MAX_PCI_DEV] = {0};

    cnt = enum_devices();

    printf("device num:%d\n", cnt);

    for (dev_idx = 0; dev_idx < cnt; dev_idx++) {
        printf("opening device:%d\n", dev_idx);
        pci_device *dev_ptr = open_pci_device(dev_idx);
        if (dev_ptr == NULL) {
            printf("open device error!\n");
            ret = -1;
            goto out;
        }

        devs[dev_idx] = dev_ptr;

        //allocate 8 4K dma buffers.
        for (i = 0; i < 8; i++) {
            dma_regions[i] = allocate_dma(dev_ptr, 0x1000);
            if (!dma_regions[i]) {
                printf("allocate dma error!\n");
                ret = -1;
                goto out;
            }
            printf("allocated dma:phys addr = 0x%" PRIx64 " virt addr = %p size = %zu\n",
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

        printf("bar2 write.\n");
        // bar2 write test for noc space.
        char * remote_serial_test_string = "This string is from qemu pcie driver.\n";
        for (i = 0; i < (int)strlen(remote_serial_test_string); i++) {
            volatile uint8_t *dst = bar_addr8(dev_ptr, 2, BAR2_SERIAL_OFF + i);
            if (!dst) {
                printf("BAR2 too small for the serial test\n");
                ret = -1;
                goto out;
            }
            *dst = (uint8_t)remote_serial_test_string[i];
        }

        printf("dma to device.\n");
        //dma test: dma to device.
        uint64_t src_addr = dma_regions[0]->dma_buffer_phys;
        uint64_t dst_addr = 0;
        uint64_t length = 0x1000;
        bar_write64(dev_ptr, 0, REG_DMA_SRC, src_addr);
        bar_write64(dev_ptr, 0, REG_DMA_DST, dst_addr);
        bar_write64(dev_ptr, 0, REG_DMA_LEN, length);
        bar_write64(dev_ptr, 0, REG_DMA_CTRL, DMA_CTRL_TO_DEV);
        bar_write64(dev_ptr, 0, REG_DMA_STAT, 0);

        //dma test: dma from noc space.
        src_addr = 0x80000000;
        dst_addr = dma_regions[0]->dma_buffer_phys;
        length = 0x1000;
        //bar_write64(dev_ptr, 0, REG_DMA_SRC, src_addr);
        //bar_write64(dev_ptr, 0, REG_DMA_DST, dst_addr);
        //bar_write64(dev_ptr, 0, REG_DMA_LEN, length);
        //bar_write64(dev_ptr, 0, REG_DMA_CTRL, DMA_CTRL_FROM_DEV);
        //bar_write64(dev_ptr, 0, REG_DMA_STAT, 0);

        //printf("DMA from noc:\n");
        //char *ptr = dma_regions[0]->dma_buffer_virt_user;
        //for (i = 0; i < 16; i++) {
        //    printf("%02x ", ptr[i]);
        //}

        printf("p2p.\n");
        // PCIE P2P test: device 1 DMA-writes into device 0's window.
        if (dev_idx == 1 && devs[0]) {
            int bar = p2p_target_bar(devs[0]);
            if (bar < 0) {
                printf("device 0 has no usable P2P window BAR\n");
            } else {
                printf("P2P target: BAR%d of device 0\n", bar);
                src_addr = 0;
                dst_addr = devs[0]->bar_regs[bar].base;
                length = 0x1000;
                bar_write64(dev_ptr, 0, REG_DMA_SRC, src_addr);
                bar_write64(dev_ptr, 0, REG_DMA_DST, dst_addr);
                bar_write64(dev_ptr, 0, REG_DMA_LEN, length);
                bar_write64(dev_ptr, 0, REG_DMA_CTRL, DMA_CTRL_FROM_DEV);
                bar_write64(dev_ptr, 0, REG_DMA_STAT, 0);

                printf("waiting for command to finish...\n");
                if (wait_command_finish(dev_ptr, 1000) < 0)
                    printf("no IRQ seen, reading the window anyway\n");

                printf("P2P test: dev0 BAR%d[0] = 0x%" PRIx64 "\n", bar,
                    bar_read64(devs[0], bar, 0, &err));
            }
        }
    }

    ret = 0;

out:
    for (dev_idx = 0; dev_idx < MAX_PCI_DEV; dev_idx++) {
        if (!devs[dev_idx])
            continue;
        for (i = 0; i < 8; i++) {
            if (dma_regions[i]) {
                if (free_dma(devs[dev_idx], dma_regions[i]) < 0)
                    printf("free dma error\n");
                dma_regions[i] = NULL;
            }
        }
        close_pci_device(devs[dev_idx]);
        devs[dev_idx] = NULL;
    }
    return ret;
}
