#ifndef PCI_DRIVER_MODEL_H
#define PCI_DRIVER_MODEL_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/pci_ids.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/version.h>
#include <linux/dma-mapping.h>
#include <linux/eventfd.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/uaccess.h>

/* Needed by virt.c (the vfio-mdev back end) and, for the prototypes at the
 * bottom of this header, by any caller that embeds a struct mdev_parent.
 */
#include <linux/vfio.h>
#include <linux/mdev.h>

/* The IDs the soc-sim vfio-user / virt.c device models actually report
 * (soc-sim/vfio_user_ip.cc); keep both sides in sync. */
#define PCI_VENDOR_ID_MODEL 0x1234
#define PCI_DEVICE_ID_MODEL 0x1111
#define PCI_MODEL_BASE_CLASS 0x05	/* memory controller */

/* Number of /dev/pci_dev_<idx> nodes the user-space enumerator probes; must
 * stay in sync with MAX_PCI_DEV in pci_driver_user.c. */
#define PCI_MODEL_MAX_DEVICES 255

/* Upper bound accepted by PCI_MODEL_IOCTL_ALLOC_DMA (single buffer). */
#define PCI_MODEL_DMA_MAX_SIZE (64UL << 20)

/*
 * ABI: these two structures are handed to/from user space verbatim
 * (GET_BAR_INFO copies the whole bar_regs array, ALLOC_DMA/FREE_DMA exchange a
 * dma_region). The kernel-side pointers are kept only so the layout stays
 * compatible with pci_driver_user.c, which mirrors them with placeholders;
 * they are always zeroed before being copied out.
 *
 * The mirror assumes a 64-bit target, where dma_addr_t is u64; on a 32-bit
 * build dma_addr_t shrinks to u32 and pci_driver_user.c would have to follow.
 */
struct pci_bar_reg {
	u64 base;
	size_t size;
	u32 __iomem *virt_kernel;	/* never exposed to user space */
	u32 __user *virt_user;		/* unused, layout only */
};

struct dma_region {
	dma_addr_t dma_buffer_phys;
	void *dma_buffer_virt_kernel;	/* never exposed to user space */
	void *dma_buffer_virt_user;		/* unused, layout only */
	size_t dma_buffer_size;
};

struct dma_region_node {
	struct dma_region *region;
	struct list_head list;
};

struct pci_driver_model {
	struct pci_dev *pdev;
	struct pci_bar_reg bar_regs[6];
	u32 irq_cnt;
	u32 reserved;

	u32 device_idx; //for multiple devices.

	int irq_count;
	int *irq_vec;	//vector numbers actually requested

	/* for eventfd: installed by PCI_MODEL_IOCTL_SET_IRQFD. The MSI/INTx
	 * vectors are torn down and re-requested around every change, so the
	 * handler never races with a swap and needs no locking here. */
	struct eventfd_ctx *efd_ctx;

	/* work queue for irq buttom half */
	struct work_struct irq_wq;

	/* for char dev file */
	struct cdev cdev;
	dev_t dev;
	struct device *dev_node;

	/* Guards dma_regions_list, the mmap lookups and the `removed` flag.
	 * Only process-context users (ioctl/mmap/release/remove), so a mutex
	 * is enough and lets us keep the region pinned across a mapping. */
	struct mutex ops_lock;
	bool removed;

	/* DMA buffer list.*/
	struct list_head dma_regions_list;

	/* drv_data outlives remove() while user-space fds are still open. */
	struct kref kref;
};

/* basic ioctls */
#define PCI_MODEL_IOCTL_MAGIC 0x5536
#define PCI_MODEL_IOCTL_GET_BAR_INFO	_IOR(PCI_MODEL_IOCTL_MAGIC, 1, void *)
#define PCI_MODEL_IOCTL_SET_IRQFD		_IOW(PCI_MODEL_IOCTL_MAGIC, 2, void *)
#define PCI_MODEL_IOCTL_SET_IRQ			_IOW(PCI_MODEL_IOCTL_MAGIC, 3, void *)
#define PCI_MODEL_IOCTL_ALLOC_DMA		_IOW(PCI_MODEL_IOCTL_MAGIC, 4, void *)
#define PCI_MODEL_IOCTL_FREE_DMA		_IOW(PCI_MODEL_IOCTL_MAGIC, 5, void *)

/*
 * vfio-mdev interfaces (virt.c, built as pci_driver_virt.ko).
 *
 * The old pci_driver_model_mdev_init(dev, ops) shape is gone: the 7.0 kernel
 * removed struct mdev_parent_ops and mdev_register_device(). An mdev parent
 * is now a struct mdev_parent the caller owns, registered against an
 * mdev_driver that the vfio-mdev module registers itself. A caller that
 * wants its PCI device to host mediated instances embeds the struct
 * mdev_parent (typically in struct pci_driver_model) and does:
 *
 *	pci_driver_model_mdev_register_parent(&pdev->dev, &drv_data->mdev_parent);
 *	... pci_driver_model_mdev_unregister_parent(&drv_data->mdev_parent);
 */
int pci_driver_model_mdev_register_parent(struct device *dev,
		struct mdev_parent *parent);
void pci_driver_model_mdev_unregister_parent(struct mdev_parent *parent);

#endif /* PCI_DRIVER_MODEL_H */
