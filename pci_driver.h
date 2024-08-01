#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/pci_ids.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/proc_fs.h>
#include <asm/cacheflush.h>

#include <linux/dma-mapping.h>

#include <linux/vfio.h>
#include <linux/mdev.h>

#include <linux/eventfd.h>
#include <linux/workqueue.h>
#include <linux/list.h>


#define PCI_VENDOR_ID_MODEL 0x1b36
#define PCI_DEVICE_ID_MODEL 0x1100
#define PCI_MODEL_BASE_CLASS 0xff

struct pci_bar_reg {
	u64 base;
	size_t size;
	u32 __iomem *virt_kernel;
	u32 __user *virt_user;
};

struct dma_region {
    dma_addr_t dma_buffer_phys;
    void *dma_buffer_virt_kernel;
    void *dma_buffer_virt_user;
    size_t dma_buffer_size;
};

struct dma_region_node {
    struct dma_region* region;
    struct list_head list;
};

struct pci_driver_model {
	struct pci_dev *pdev;
	spinlock_t lock;
	struct pci_bar_reg bar_regs[6];
	void *oprom;
	u32 irq_cnt;
	u32 reserved;

	u32 device_idx; //for multiple devices.

	int irq_count;

	/* for eventfd */
	struct eventfd_ctx *efd_ctx;

	/* work queue for irq buttom half */
	struct work_struct irq_wq;

	/* for char dev file */
	struct cdev cdev;
	dev_t dev;

	/* for proc dev file */
	struct proc_dir_entry *device_proc_entry;

	/* DMA buffer list.*/
	struct list_head dma_regions_list;
};

/* basic ioctls */
#define PCI_MODEL_IOCTL_MAGIC 0x5536
#define PCI_MODEL_IOCTL_GET_BAR_INFO	_IOR(PCI_MODEL_IOCTL_MAGIC, 1, void *)
#define PCI_MODEL_IOCTL_SET_IRQFD		_IOW(PCI_MODEL_IOCTL_MAGIC, 2, void *)
#define PCI_MODEL_IOCTL_SET_IRQ			_IOW(PCI_MODEL_IOCTL_MAGIC, 3, void *)
#define PCI_MODEL_IOCTL_ALLOC_DMA		_IOW(PCI_MODEL_IOCTL_MAGIC, 4, void *)
#define PCI_MODEL_IOCTL_FREE_DMA		_IOW(PCI_MODEL_IOCTL_MAGIC, 5, void *)

/* vfio-mdev interfaces */
extern int pci_driver_model_mdev_init(struct device *dev, const void *ops);
