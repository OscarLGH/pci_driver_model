// SPDX-License-Identifier: GPL-2.0
/*
 * virt.c -- vfio-mdev back end for the pci_driver_model harness.
 *
 * Rewritten for the 7.0 kernel. The API this file used to target is gone:
 * struct mdev_parent_ops, mdev_register_device() and the read/write/ioctl/
 * mmap file ops that hung off struct mdev_device no longer exist. On 7.0 a
 * mediated device is an ordinary device on the "mdev" bus, probed by a
 * struct mdev_driver, and the file operations QEMU talks to live in struct
 * vfio_device_ops of a struct vfio_device registered with
 * vfio_register_emulated_iommu_dev() -- i.e. this is now a plain emulated
 * vfio device whose parent happens to be an mdev type.
 *
 * The device modelled here is deliberately the same one the SoC exports
 * through the vfio-user back end (soc-sim/vfio_user_ip.cc), so the same
 * guest-side harness can be driven over an in-kernel transport instead of a
 * vfio-user socket:
 *
 *	insmod pci_driver_virt.ko
 *	UUID=$(cat /proc/sys/kernel/random/uuid)
 *	echo $UUID > /sys/devices/platform/pci-model/mdev_supported_types/pci-model/create
 *	qemu -device vfio-pci,sysfsdev=/sys/bus/mdev/devices/$UUID
 *
 *	vendor 0x1234 / device 0x1111, class 0x058000, rev 0x01 -- the SoC's
 *	    own IDs, the same ones the char-device driver (pci_driver.c)
 *	    matches on
 *	config space: 256 bytes plus per-byte write masks, including the
 *	    "writable bits are ~(size-1)" BAR sizing trick
 *	region 2 (BAR2): 1 MiB, no MMAP flag -> every guest access is
 *	    trapped through read()/write(), same convention as region 2 on
 *	    the vfio-user side
 *	region 4 (BAR4): 1 MiB, MMAP flag -> QEMU maps the window straight
 *	    into guest RAM (vfio_region_mmap), which is how host and guest
 *	    share memory here
 *	9 regions / 5 IRQ indices reported, so QEMU's vfio-pci realize path
 *	    matches what the vfio-user back end reports
 *
 * Not modelled, on purpose:
 *	- Interrupts: interrupt pin 0 in config space, count 0 for every IRQ
 *	  index. The interrupt tests belong to the char-device path in
 *	  pci_driver.c (eventfd + real MSI/INTx vectors).
 *	- BAR2 forwarding into the SoC bus. An mdev has no bus behind it, so
 *	  region 2 is a plain store; the vfio-user twin is what talks to the
 *	  interconnect.
 *	- Device DMA. An emulated device sits behind an emulated IOMMU with
 *	  nothing to pin, and the BAR4 window is the shared-memory path.
 */

#include "pci_driver.h"

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>

#define MVDEV_MTYPE_NAME		"pci-model"
#define MVDEV_VENDOR_ID			0x1234
#define MVDEV_DEVICE_ID			0x1111
#define MVDEV_CONFIG_SIZE		256
#define MVDEV_WINDOW_SIZE		SZ_1M	/* BAR2 and BAR4 each */
#define MVDEV_WINDOW_NR_PAGES		(MVDEV_WINDOW_SIZE / PAGE_SIZE)
/* Same marker the vfio-user P2P window pre-fills, so the guest-side check
 * ("BAR4[0] == 0xDEADBEEFCAFEBABE") works on either transport.
 */
#define MVDEV_WINDOW_MARKER		0xDEADBEEFCAFEBABEULL
#define MVDEV_MAX_INSTANCES		8

/*
 * Region addressing on the vfio device fd: the region index sits in the top
 * bits of the file offset. The uapi header exported these macros when this
 * file was first written; the 7.0 tree no longer does, so the ABI constants
 * are kept here.
 */
#define MVDEV_OFFSET_SHIFT		40
#define MVDEV_OFFSET_TO_INDEX(off)	((off) >> MVDEV_OFFSET_SHIFT)
#define MVDEV_INDEX_TO_OFFSET(idx)	((u64)(idx) << MVDEV_OFFSET_SHIFT)
#define MVDEV_OFFSET_MASK		(((u64)1 << MVDEV_OFFSET_SHIFT) - 1)
/* The same, in the page units vm_pgoff uses. */
#define MVDEV_PGOFF_SHIFT		(MVDEV_OFFSET_SHIFT - PAGE_SHIFT)

struct mvdev_device {
	struct vfio_device	vdev;	/* must be first for container_of */
	struct mdev_device	*mdev;

	u8			config[MVDEV_CONFIG_SIZE];
	u8			config_wmask[MVDEV_CONFIG_SIZE];

	void			*bar2;	/* trapped window, plain store */

	struct page		**window_pages;	/* mmap'able BAR4 window */
	void			*window_kv;	/* vmap() of the above */
	atomic_t		map_count;	/* live mmaps of window_pages */
	wait_queue_head_t	window_idle;
};

#define to_mvdev(vdev) container_of((vdev), struct mvdev_device, vdev)

static struct mvdev_parent {
	struct platform_device	*pdev;
	struct mdev_parent	parent;
} *mvdev_parent;

/* ---------------------------------------------------------------------- */
/* config space emulation, mirroring soc-sim/vfio_user_ip.cc closely enough
 * that a guest sees one device whichever transport it was attached through.
 */

static void mvdev_init_config(struct mvdev_device *mdev)
{
	u32 wmask = (u32)~(MVDEV_WINDOW_SIZE - 1);

	memset(mdev->config, 0, sizeof(mdev->config));

	/* Conventional PCI header: no capability chain (0x34 stays 0), so
	 * QEMU never looks for MSI/MSI-X on this device.
	 */
	mdev->config[0x00] = MVDEV_VENDOR_ID & 0xFF;
	mdev->config[0x01] = MVDEV_VENDOR_ID >> 8;
	mdev->config[0x02] = MVDEV_DEVICE_ID & 0xFF;
	mdev->config[0x03] = MVDEV_DEVICE_ID >> 8;
	mdev->config[0x08] = 0x01;		/* revision */
	mdev->config[0x09] = 0x00;		/* prog-if */
	mdev->config[0x0A] = 0x80;		/* subclass */
	mdev->config[0x0B] = 0x05;		/* base class: memory controller */
	mdev->config[0x0E] = 0x00;		/* header type: single function */
	/* BAR2 at 0x18: 64-bit memory BAR; BAR3 at 0x1C is its upper half. */
	mdev->config[0x18] = 0x04;
	/* BAR4 at 0x20: 64-bit prefetchable memory BAR; BAR5 at 0x24 is the
	 * upper half. This is the window QEMU maps directly.
	 */
	mdev->config[0x20] = 0x0C;
	/* No interrupt pin: nothing raises INTx here (see the header block). */
	mdev->config[0x3D] = 0x00;

	/* A set bit is a writable bit; the rest keeps its reset value. */
	memset(mdev->config_wmask, 0xFF, sizeof(mdev->config_wmask));
	memset(&mdev->config_wmask[0x00], 0, 4);	/* vendor/device ID */
	memset(&mdev->config_wmask[0x08], 0, 4);	/* class code */
	mdev->config_wmask[0x0E] = 0;			/* header type */
	memset(&mdev->config_wmask[0x10], 0, 8);	/* BAR0/BAR1: none */
	/* Only address bits at or above the region size are writable, so a
	 * sizing write of ~0 reads back ~(size-1)|type and firmware sizes
	 * the BAR to exactly MVDEV_WINDOW_SIZE.
	 */
	mdev->config_wmask[0x18] = wmask & 0x0F;	/* keep the type bits */
	mdev->config_wmask[0x19] = (wmask >> 8) & 0xFF;
	mdev->config_wmask[0x1A] = (wmask >> 16) & 0xFF;
	mdev->config_wmask[0x1B] = (wmask >> 24) & 0xFF;
	memset(&mdev->config_wmask[0x1C], 0xFF, 4);	/* BAR3 upper half */
	mdev->config_wmask[0x20] = wmask & 0x0F;	/* keep prefetch+64bit */
	mdev->config_wmask[0x21] = (wmask >> 8) & 0xFF;
	mdev->config_wmask[0x22] = (wmask >> 16) & 0xFF;
	mdev->config_wmask[0x23] = (wmask >> 24) & 0xFF;
	memset(&mdev->config_wmask[0x24], 0xFF, 4);	/* BAR5 upper half */
	memset(&mdev->config_wmask[0x2C], 0, 4);	/* subsystem IDs */
	memset(&mdev->config_wmask[0x30], 0, 4);	/* no expansion ROM */
	mdev->config_wmask[0x34] = 0;			/* no cap chain */
	mdev->config_wmask[0x3D] = 0;			/* interrupt pin */
}

/* Guest-visible address of a 64-bit BAR pair, for the log only. */
static u64 mvdev_bar_addr(struct mvdev_device *mdev, unsigned int slot)
{
	u32 low = le32_to_cpu(get_unaligned((const __le32 *)&mdev->config[slot]));
	u32 high = le32_to_cpu(get_unaligned((const __le32 *)&mdev->config[slot + 4]));

	return (((u64)high) << 32) | (low & PCI_BASE_ADDRESS_MEM_MASK);
}

static void mvdev_reset(struct mvdev_device *mdev)
{
	/* Config goes back to the template and the trapped window looks like
	 * a power-up. The BAR4 window is left alone on purpose: it is a
	 * mailbox shared with the SoC, exactly as on the vfio-user side.
	 */
	mvdev_init_config(mdev);
	if (mdev->bar2)
		memset(mdev->bar2, 0, MVDEV_WINDOW_SIZE);
}

/* ---------------------------------------------------------------------- */
/* the mmap'able BAR4 window */

static void mvdev_free_window(struct mvdev_device *mdev)
{
	unsigned int i;

	if (!mdev->window_pages)
		return;

	/*
	 * A mapping can outlive the vfio fd that created it (the vma holds
	 * its own reference on the open file), so vfio core's guarantee that
	 * close_device() ran is not enough: the pages have to stay alive
	 * until the last vm_operations_struct->close().
	 */
	if (!wait_event_timeout(mdev->window_idle,
			atomic_read(&mdev->map_count) == 0,
			msecs_to_jiffies(5000))) {
		dev_warn(mdev_dev(mdev->mdev),
			"%d mapping(s) of the BAR4 window are still live, leaking %lu pages\n",
			atomic_read(&mdev->map_count),
			MVDEV_WINDOW_NR_PAGES);
		return;
	}

	if (mdev->window_kv) {
		vunmap(mdev->window_kv);
		mdev->window_kv = NULL;
	}
	for (i = 0; i < MVDEV_WINDOW_NR_PAGES; i++)
		if (mdev->window_pages[i])
			__free_page(mdev->window_pages[i]);
	kvfree(mdev->window_pages);
	mdev->window_pages = NULL;
}

static int mvdev_alloc_window(struct mvdev_device *mdev)
{
	unsigned int i;

	mdev->window_pages = kvcalloc(MVDEV_WINDOW_NR_PAGES,
			sizeof(*mdev->window_pages), GFP_KERNEL);
	if (!mdev->window_pages)
		return -ENOMEM;

	/*
	 * One order-0 page per slot rather than one compound page: the pages
	 * go to vm_insert_page(), which only accepts pages that are not part
	 * of a compound page. vmap() gives the read()/write() path the same
	 * linear view the message path has on the vfio-user side.
	 */
	for (i = 0; i < MVDEV_WINDOW_NR_PAGES; i++) {
		mdev->window_pages[i] = alloc_page(GFP_KERNEL);
		if (!mdev->window_pages[i]) {
			mvdev_free_window(mdev);
			return -ENOMEM;
		}
	}

	mdev->window_kv = vmap(mdev->window_pages, MVDEV_WINDOW_NR_PAGES,
			VM_MAP, PAGE_KERNEL);
	if (!mdev->window_kv) {
		mvdev_free_window(mdev);
		return -ENOMEM;
	}

	memset(mdev->window_kv, 0, MVDEV_WINDOW_SIZE);
	*(u64 *)mdev->window_kv = MVDEV_WINDOW_MARKER;
	return 0;
}

static void mvdev_window_vma_open(struct vm_area_struct *vma)
{
	atomic_inc(&((struct mvdev_device *)vma->vm_private_data)->map_count);
}

static void mvdev_window_vma_close(struct vm_area_struct *vma)
{
	struct mvdev_device *mdev = vma->vm_private_data;

	if (atomic_dec_and_test(&mdev->map_count))
		wake_up(&mdev->window_idle);
}

static const struct vm_operations_struct mvdev_window_vmops = {
	.open = mvdev_window_vma_open,
	.close = mvdev_window_vma_close,
};

static int mvdev_mmap(struct vfio_device *vdev, struct vm_area_struct *vma)
{
	struct mvdev_device *mdev = to_mvdev(vdev);
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned int index;
	unsigned long i;
	int ret;

	index = vma->vm_pgoff >> MVDEV_PGOFF_SHIFT;
	if (index != VFIO_PCI_BAR4_REGION_INDEX)
		return -EINVAL;	/* region 2 is trapped on purpose */
	if (vma->vm_pgoff & ((1UL << MVDEV_PGOFF_SHIFT) - 1))
		return -EINVAL;	/* must start at the region base */
	if (size != MVDEV_WINDOW_SIZE)
		return -EINVAL;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;	/* a private copy is not a mailbox */

	for (i = 0; i < MVDEV_WINDOW_NR_PAGES; i++) {
		ret = vm_insert_page(vma, vma->vm_start +
				(i << PAGE_SHIFT), mdev->window_pages[i]);
		if (ret)
			return ret;
	}

	/*
	 * Only claimed once every page is in: when an insert fails, the mm
	 * core tears the half-built vma down and runs ->close() itself, so
	 * taking the reference earlier would be balanced twice. From here on
	 * the reference is ours, and it is what keeps window_pages alive
	 * past close_device() -- see mvdev_free_window().
	 */
	vma->vm_private_data = mdev;
	vma->vm_ops = &mvdev_window_vmops;
	atomic_inc(&mdev->map_count);

	dev_info(&mdev->vdev.device, "BAR4 window mapped at %#lx, %lu KiB\n",
		vma->vm_start, size / SZ_1K);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* vfio file operations */

static int mvdev_open_device(struct vfio_device *vdev)
{
	return 0;
}

static void mvdev_close_device(struct vfio_device *vdev)
{
}

static ssize_t mvdev_read(struct vfio_device *vdev, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct mvdev_device *mdev = to_mvdev(vdev);
	unsigned int index = MVDEV_OFFSET_TO_INDEX(*ppos);
	u64 off = (u64)*ppos & MVDEV_OFFSET_MASK;
	const void *src;
	size_t size, len;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		src = mdev->config;
		size = MVDEV_CONFIG_SIZE;
		break;
	case VFIO_PCI_BAR2_REGION_INDEX:
		src = mdev->bar2;
		size = MVDEV_WINDOW_SIZE;
		break;
	case VFIO_PCI_BAR4_REGION_INDEX:
		src = mdev->window_kv;
		size = MVDEV_WINDOW_SIZE;
		break;
	default:
		return -EINVAL;
	}

	if (!src || off >= size)
		return -EINVAL;
	len = min_t(size_t, count, size - off);
	if (copy_to_user(buf, (const char *)src + off, len))
		return -EFAULT;

	*ppos += len;
	return len;
}

static ssize_t mvdev_write_config(struct mvdev_device *mdev,
		const char __user *buf, size_t count, u64 off)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		u8 val;

		if (get_user(val, buf + i))
			return -EFAULT;
		mdev->config[off + i] =
			(mdev->config[off + i] & ~mdev->config_wmask[off + i]) |
			(val & mdev->config_wmask[off + i]);
	}

	/* The BAR writes are the interesting part of a guest boot, log them
	 * so the harness can line them up with the addresses it sees later.
	 */
	if (off + count > 0x18 && off < 0x20)
		dev_info(&mdev->vdev.device, "BAR2 = %#llx\n",
			mvdev_bar_addr(mdev, 0x18));
	if (off + count > 0x20 && off < 0x28)
		dev_info(&mdev->vdev.device, "BAR4 = %#llx\n",
			mvdev_bar_addr(mdev, 0x20));

	return count;
}

static ssize_t mvdev_write(struct vfio_device *vdev, const char __user *buf,
		size_t count, loff_t *ppos)
{
	struct mvdev_device *mdev = to_mvdev(vdev);
	unsigned int index = MVDEV_OFFSET_TO_INDEX(*ppos);
	u64 off = (u64)*ppos & MVDEV_OFFSET_MASK;
	void *dst;
	size_t size, len;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		dst = mdev->config;
		size = MVDEV_CONFIG_SIZE;
		break;
	case VFIO_PCI_BAR2_REGION_INDEX:
		dst = mdev->bar2;
		size = MVDEV_WINDOW_SIZE;
		break;
	case VFIO_PCI_BAR4_REGION_INDEX:
		dst = mdev->window_kv;
		size = MVDEV_WINDOW_SIZE;
		break;
	default:
		return -EINVAL;
	}

	if (!dst || off >= size)
		return -EINVAL;
	len = min_t(size_t, count, size - off);

	if (index == VFIO_PCI_CONFIG_REGION_INDEX) {
		ssize_t written = mvdev_write_config(mdev, buf, len, off);

		if (written < 0)
			return written;
		*ppos += written;
		return written;
	}

	if (copy_from_user((char *)dst + off, buf, len))
		return -EFAULT;

	*ppos += len;
	return len;
}

static long mvdev_ioctl(struct vfio_device *vdev, unsigned int cmd,
		unsigned long arg)
{
	struct mvdev_device *mdev = to_mvdev(vdev);
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO: {
		struct vfio_device_info info;

		if (copy_from_user(&info, uarg, sizeof(info)))
			return -EFAULT;
		if (info.argsz < sizeof(info))
			return -EINVAL;

		info.flags = VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET;
		info.num_regions = VFIO_PCI_NUM_REGIONS;
		info.num_irqs = VFIO_PCI_NUM_IRQS;
		info.argsz = sizeof(info);
		if (copy_to_user(uarg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	case VFIO_DEVICE_GET_REGION_INFO: {
		struct vfio_region_info info;

		if (copy_from_user(&info, uarg, sizeof(info)))
			return -EFAULT;
		if (info.argsz < sizeof(info))
			return -EINVAL;

		info.offset = MVDEV_INDEX_TO_OFFSET(info.index);
		info.cap_offset = 0;
		switch (info.index) {
		case VFIO_PCI_CONFIG_REGION_INDEX:
			info.size = MVDEV_CONFIG_SIZE;
			info.flags = VFIO_REGION_INFO_FLAG_READ |
				VFIO_REGION_INFO_FLAG_WRITE;
			break;
		case VFIO_PCI_BAR2_REGION_INDEX:
			/* Deliberately no MMAP: QEMU traps every access and
			 * the read()/write() paths are what model the
			 * device, as region 2 does on vfio-user.
			 */
			info.size = MVDEV_WINDOW_SIZE;
			info.flags = VFIO_REGION_INFO_FLAG_READ |
				VFIO_REGION_INFO_FLAG_WRITE;
			break;
		case VFIO_PCI_BAR4_REGION_INDEX:
			info.size = MVDEV_WINDOW_SIZE;
			info.flags = VFIO_REGION_INFO_FLAG_READ |
				VFIO_REGION_INFO_FLAG_WRITE |
				VFIO_REGION_INFO_FLAG_MMAP;
			break;
		default:
			/* BAR0/1/3/5 and ROM: unimplemented, zero sized. */
			info.size = 0;
			info.flags = 0;
			break;
		}
		info.argsz = sizeof(info);
		if (copy_to_user(uarg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	case VFIO_DEVICE_GET_IRQ_INFO: {
		struct vfio_irq_info info;

		if (copy_from_user(&info, uarg, sizeof(info)))
			return -EFAULT;
		if (info.argsz < sizeof(info))
			return -EINVAL;
		if (info.index >= VFIO_PCI_NUM_IRQS)
			return -EINVAL;

		/* No interrupts modelled: report every index as absent. */
		info.flags = 0;
		info.count = 0;
		info.argsz = sizeof(info);
		if (copy_to_user(uarg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	case VFIO_DEVICE_SET_IRQS: {
		struct vfio_irq_set hdr;

		if (copy_from_user(&hdr, uarg, sizeof(hdr)))
			return -EFAULT;
		if (hdr.argsz < sizeof(hdr) || hdr.index >= VFIO_PCI_NUM_IRQS)
			return -EINVAL;
		/* QEMU tears down interrupt routing it never got; accept
		 * that, refuse anything that actually asks for an IRQ.
		 */
		return hdr.count ? -EINVAL : 0;
	}

	case VFIO_DEVICE_RESET:
		mvdev_reset(mdev);
		dev_info(&mdev->vdev.device, "device reset\n");
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct vfio_device_ops mvdev_vfio_ops = {
	.name = MVDEV_MTYPE_NAME,
	.open_device = mvdev_open_device,
	.close_device = mvdev_close_device,
	.read = mvdev_read,
	.write = mvdev_write,
	.ioctl = mvdev_ioctl,
	.mmap = mvdev_mmap,
};

/* ---------------------------------------------------------------------- */
/* mdev driver: an instance is one emulated PCI device for QEMU to attach */

static int mvdev_mdev_probe(struct mdev_device *mdev)
{
	struct mvdev_device *vmdev;
	int ret;

	/* Note the bare type name: the macro supplies the "struct". */
	vmdev = vfio_alloc_device(mvdev_device, vdev, mdev_dev(mdev),
			&mvdev_vfio_ops);
	if (!vmdev)
		return -ENOMEM;

	vmdev->mdev = mdev;
	atomic_set(&vmdev->map_count, 0);
	init_waitqueue_head(&vmdev->window_idle);
	mvdev_init_config(vmdev);

	vmdev->bar2 = vzalloc(MVDEV_WINDOW_SIZE);
	if (!vmdev->bar2) {
		ret = -ENOMEM;
		goto err_put;
	}

	ret = mvdev_alloc_window(vmdev);
	if (ret)
		goto err_bar2;

	/* Emulated device: vfio core and iommufd own the DMA mapping, there
	 * is no physical IOMMU group and nothing for us to pin.
	 */
	ret = vfio_register_emulated_iommu_dev(&vmdev->vdev);
	if (ret)
		goto err_window;

	dev_set_drvdata(mdev_dev(mdev), vmdev);
	dev_info(mdev_dev(mdev),
		"emulated PCI %04x:%04x, region 2 and region 4 are %lu KiB windows, region 4 mmap'able\n",
		MVDEV_VENDOR_ID, MVDEV_DEVICE_ID,
		(unsigned long)(MVDEV_WINDOW_SIZE / SZ_1K));
	return 0;

err_window:
	mvdev_free_window(vmdev);
err_bar2:
	vfree(vmdev->bar2);
	vmdev->bar2 = NULL;
err_put:
	vfio_put_device(&vmdev->vdev);
	return ret;
}

static void mvdev_mdev_remove(struct mdev_device *mdev)
{
	struct mvdev_device *vmdev = dev_get_drvdata(mdev_dev(mdev));

	/* Returns once every open fd has run close_device(). */
	vfio_unregister_group_dev(&vmdev->vdev);
	/* Still not enough for a RAM-backed window: see mvdev_free_window(). */
	mvdev_free_window(vmdev);
	vfree(vmdev->bar2);
	vmdev->bar2 = NULL;
	/* vfio core owns the allocation made by vfio_alloc_device(); this
	 * drops our reference and the container is freed once the last
	 * reference to the embedded struct device goes away.
	 */
	vfio_put_device(&vmdev->vdev);
}

static ssize_t mvdev_show_description(struct mdev_type *mtype, char *buf)
{
	return sysfs_emit(buf,
		"SoC simulator emulated PCI device (%04x:%04x), attach with -device vfio-pci,sysfsdev=...\n",
		MVDEV_VENDOR_ID, MVDEV_DEVICE_ID);
}

/* The mdev types this parent offers; mdev_register_parent() takes an array of
 * pointers and the core puts one directory per type under
 * <parent>/mdev_supported_types/.
 */
static struct mdev_type mvdev_type = {
	.sysfs_name = MVDEV_MTYPE_NAME,
	.pretty_name = "SoC simulator PCI model",
};

static struct mdev_type *mvdev_types[] = {
	&mvdev_type,
};

static struct mdev_driver mvdev_mdev_driver = {
	.device_api = "vfio",
	.max_instances = MVDEV_MAX_INSTANCES,
	.probe = mvdev_mdev_probe,
	.remove = mvdev_mdev_remove,
	.show_description = mvdev_show_description,
	.driver = {
		/* device_register() keys the driver's sysfs node off this;
		 * the mdev core does not fill it in for us.
		 */
		.name = "pci_driver_model_virt",
	},
};

/*
 * Integration point for pci_driver.c: a caller that wants its real PCI
 * device to be the mdev parent embeds a struct mdev_parent next to its
 * per-device state and registers it from probe()/remove(). The mdev driver
 * itself is registered by this module, so the caller only needs to load
 * pci_driver_virt.ko first (the symbols below create the dependency).
 */
int pci_driver_model_mdev_register_parent(struct device *dev,
		struct mdev_parent *parent)
{
	return mdev_register_parent(parent, dev, &mvdev_mdev_driver,
			mvdev_types, ARRAY_SIZE(mvdev_types));
}
EXPORT_SYMBOL_GPL(pci_driver_model_mdev_register_parent);

void pci_driver_model_mdev_unregister_parent(struct mdev_parent *parent)
{
	mdev_unregister_parent(parent);
}
EXPORT_SYMBOL_GPL(pci_driver_model_mdev_unregister_parent);

/* ---------------------------------------------------------------------- */

static int __init pci_driver_model_virt_init(void)
{
	struct mvdev_parent *parent;
	int ret;

	ret = mdev_register_driver(&mvdev_mdev_driver);
	if (ret)
		return ret;

	/*
	 * Standalone parent so the module is testable on its own. To hang the
	 * mdev off a real device instead, call
	 * pci_driver_model_mdev_register_parent() from that device's driver
	 * and leave this platform device unused.
	 */
	parent = kzalloc_obj(*parent, GFP_KERNEL);
	if (!parent) {
		mdev_unregister_driver(&mvdev_mdev_driver);
		return -ENOMEM;
	}

	parent->pdev = platform_device_register_simple(MVDEV_MTYPE_NAME, -1,
			NULL, 0);
	if (IS_ERR(parent->pdev)) {
		ret = PTR_ERR(parent->pdev);
		goto err_free;
	}

	ret = pci_driver_model_mdev_register_parent(&parent->pdev->dev,
			&parent->parent);
	if (ret)
		goto err_unreg;

	mvdev_parent = parent;
	pr_info("pci_driver_model_virt: create an instance with `echo $UUID > /sys/devices/platform/%s/mdev_supported_types/%s/create`\n",
		MVDEV_MTYPE_NAME, MVDEV_MTYPE_NAME);
	return 0;

err_unreg:
	platform_device_unregister(parent->pdev);
err_free:
	kfree(parent);
	mdev_unregister_driver(&mvdev_mdev_driver);
	return ret;
}

static void __exit pci_driver_model_virt_exit(void)
{
	pci_driver_model_mdev_unregister_parent(&mvdev_parent->parent);
	platform_device_unregister(mvdev_parent->pdev);
	kfree(mvdev_parent);
	mvdev_parent = NULL;
	mdev_unregister_driver(&mvdev_mdev_driver);
}

module_init(pci_driver_model_virt_init);
module_exit(pci_driver_model_virt_exit);

MODULE_DESCRIPTION("vfio-mdev emulated PCI device matching the soc-sim vfio-user backend");
MODULE_LICENSE("GPL");
