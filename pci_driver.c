#include "pci_driver.h"

#include <linux/dma-buf.h>
#include <linux/pci-p2pdma.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>

/* the INTx flag was called PCI_IRQ_LEGACY before the rename in v6.x */
#ifndef PCI_IRQ_INTX
#define PCI_IRQ_INTX PCI_IRQ_LEGACY
#endif

static struct class *pci_driver_model_class;

/* Device index allocator for /dev/pci_dev_<idx>. atomic so concurrent probes
 * cannot hand out the same index; indices are not recycled on remove (the
 * user-space enumerator only ever looks for holes it can live with). */
static atomic_t device_cnt = ATOMIC_INIT(-1);

static struct pci_device_id
pci_driver_model_id_table[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_MODEL, PCI_DEVICE_ID_MODEL),
	},
	{ }	/* terminator: the PCI core scans until it sees an empty entry */
};

static irqreturn_t pci_driver_irq(int irq, void *data)
{
	struct pci_driver_model *drv_data = data;

	drv_data->irq_cnt++;
	dev_dbg(&drv_data->pdev->dev, "IRQ:%d\n", irq);

	/*
	 * No lock here: the vectors are free_irq()'ed and re-requested around
	 * every efd_ctx change (see pci_driver_model_set_efd()), so the ctx
	 * cannot disappear while this handler runs.
	 */
	if (drv_data->efd_ctx) {
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 19, 0)
		eventfd_signal(drv_data->efd_ctx);
#else
		eventfd_signal(drv_data->efd_ctx, 1);
#endif
	}

	schedule_work(&drv_data->irq_wq);
	return IRQ_HANDLED;
}

static void pci_driver_model_release(struct kref *kref)
{
	struct pci_driver_model *drv_data =
		container_of(kref, struct pci_driver_model, kref);

	kfree(drv_data->irq_vec);
	kfree(drv_data);
}

/*
 * Install (or clear, with new_ctx == NULL) the eventfd the ISR signals.
 *
 * The vectors are torn down and re-armed around the swap: free_irq() waits for
 * handlers already running, so pci_driver_irq() can dereference efd_ctx
 * without any locking, and the old ctx is never dropped while it may still be
 * in use. `new_ctx` is a reference the caller obtained (eventfd_ctx_fdget) and
 * ownership transfers here either way. Caller must hold ops_lock.
 */
static int pci_driver_model_set_efd(struct pci_driver_model *drv_data,
				    struct eventfd_ctx *new_ctx)
{
	struct eventfd_ctx *old_ctx = drv_data->efd_ctx;
	struct pci_dev *pdev = drv_data->pdev;
	int i, ret;

	if (old_ctx == new_ctx)
		return 0;

	for (i = 0; i < drv_data->irq_count; i++)
		free_irq(drv_data->irq_vec[i], drv_data);

	drv_data->efd_ctx = new_ctx;

	for (i = 0; i < drv_data->irq_count; i++) {
		ret = request_irq(drv_data->irq_vec[i], pci_driver_irq, 0,
				  dev_name(&pdev->dev), drv_data);
		if (ret) {
			dev_err(&pdev->dev, "re-request of IRQ %d failed: %d\n",
				drv_data->irq_vec[i], ret);
			goto err;
		}
	}

	if (old_ctx)
		eventfd_ctx_put(old_ctx);
	return 0;

err:
	if (new_ctx)
		eventfd_ctx_put(new_ctx);
	drv_data->efd_ctx = old_ctx;
	for (i = 0; i < drv_data->irq_count; i++)
		if (request_irq(drv_data->irq_vec[i], pci_driver_irq, 0,
				dev_name(&pdev->dev), drv_data))
			dev_err(&pdev->dev, "IRQ %d lost\n",
				drv_data->irq_vec[i]);
	return ret;
}

static int pci_driver_model_char_open(struct inode *inode, struct file *file)
{
	struct pci_driver_model *pdev;

	pdev = container_of(inode->i_cdev, struct pci_driver_model, cdev);
	/* the fd outlives remove(), which only drops its own reference */
	kref_get(&pdev->kref);
	file->private_data = pdev;

	return 0;
}

static int pci_driver_model_char_release(struct inode *inode, struct file *file)
{
	struct pci_driver_model *dev = file->private_data;

	file->private_data = NULL;
	if (dev) {
		mutex_lock(&dev->ops_lock);
		if (!dev->removed)
			pci_driver_model_set_efd(dev, NULL);
		mutex_unlock(&dev->ops_lock);
		kref_put(&dev->kref, pci_driver_model_release);
	}

	return 0;
}

static loff_t pci_driver_model_char_lseek(struct file *file, loff_t offset, int origin)
{
	loff_t new_pos;

	switch (origin) {
		case SEEK_SET:		/* from <linux/seek.h>, via <linux/fs.h> */
			if (offset < 0)
				return -EINVAL;
			new_pos = offset;
			break;
		case SEEK_CUR:
			if (file->f_pos + offset < 0)
				return -EINVAL;
			new_pos = file->f_pos + offset;
			break;
		default:
			/* no notion of an end here: the file is not a window
			 * onto a single object */
			return -EINVAL;
	}

	file->f_pos = new_pos;
	return new_pos;
}

static ssize_t pci_driver_model_char_read(struct file *file, char __user *buf,
		size_t count, loff_t *pos)
{
	/* commands go through ioctl()/mmap(); returning 0 here would look like
	 * a successful empty read to user space */
	return -EOPNOTSUPP;
}

static ssize_t pci_driver_model_char_write(struct file *file, const char __user *buf,
                size_t count, loff_t *pos)
{
	return -EOPNOTSUPP;
}

static int pci_driver_model_export_bar(struct pci_driver_model *drv,
				       const struct pci_model_export_req *req);

static long pci_driver_model_char_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct pci_driver_model *dev = file->private_data;
	void __user *uarg = (void __user *)(unsigned long)arg;
	struct dma_region_node *node, *tmp;
	struct pci_bar_reg bar_regs[6];
	struct dma_region dma_usr_region;
	struct pci_model_export_req req;
	struct eventfd_ctx *ctx;
	unsigned long size;
	int i, ret = 0;

	switch (cmd) {
		case PCI_MODEL_IOCTL_GET_BAR_INFO:
			/* hand out a sanitized copy: the kernel-side
			 * ioremap() addresses must not reach user space */
			for (i = 0; i < 6; i++) {
				bar_regs[i] = dev->bar_regs[i];
				bar_regs[i].virt_kernel = NULL;
				bar_regs[i].virt_user = NULL;
			}
			if (copy_to_user(uarg, bar_regs, sizeof(bar_regs)))
				return -EFAULT;
			ret = 0;
			break;
		case PCI_MODEL_IOCTL_SET_IRQFD:
			/* the ioctl argument is an eventfd number, not a pointer */
			ctx = eventfd_ctx_fdget((int)arg);
			if (IS_ERR(ctx))
				return PTR_ERR(ctx);
			mutex_lock(&dev->ops_lock);
			if (dev->removed) {
				eventfd_ctx_put(ctx);
				ret = -ENODEV;
			} else {
				ret = pci_driver_model_set_efd(dev, ctx);
			}
			mutex_unlock(&dev->ops_lock);
			return ret;
		case PCI_MODEL_IOCTL_SET_IRQ:
			/* not implemented: vector masking is left to the
			 * device-specific registers */
			ret = 0;
			break;
		case PCI_MODEL_IOCTL_ALLOC_DMA:
			if (copy_from_user(&dma_usr_region, uarg, sizeof(dma_usr_region)))
				return -EFAULT;

			size = dma_usr_region.dma_buffer_size;
			if (!size || (size & ~PAGE_MASK) || size > PCI_MODEL_DMA_MAX_SIZE) {
				dev_err(&dev->pdev->dev,
					"bad DMA request size 0x%lx\n", size);
				return -EINVAL;
			}

			node = kmalloc(sizeof(*node), GFP_KERNEL);
			if (!node)
				return -ENOMEM;
			node->region = kmalloc(sizeof(*node->region), GFP_KERNEL);
			if (!node->region) {
				kfree(node);
				return -ENOMEM;
			}
			*node->region = dma_usr_region;
			node->region->dma_buffer_size = size;
			node->region->dma_buffer_virt_user = NULL;
			node->region->dma_buffer_virt_kernel =
				dma_alloc_coherent(&dev->pdev->dev, size,
					&node->region->dma_buffer_phys, GFP_KERNEL);
			if (!node->region->dma_buffer_virt_kernel) {
				dev_err(&dev->pdev->dev, "Failed to allocate DMA buffer.");
				kfree(node->region);
				kfree(node);
				return -ENOMEM;
			}

			/* publish the answer before the buffer becomes freeable
			 * through FREE_DMA */
			if (copy_to_user(uarg, node->region, sizeof(*node->region))) {
				dma_free_coherent(&dev->pdev->dev, size,
					node->region->dma_buffer_virt_kernel,
					node->region->dma_buffer_phys);
				kfree(node->region);
				kfree(node);
				return -EFAULT;
			}

			mutex_lock(&dev->ops_lock);
			list_add_tail(&node->list, &dev->dma_regions_list);
			mutex_unlock(&dev->ops_lock);
			ret = 0;
			break;
		case PCI_MODEL_IOCTL_FREE_DMA:
			if (copy_from_user(&dma_usr_region, uarg, sizeof(dma_usr_region)))
				return -EFAULT;

			mutex_lock(&dev->ops_lock);
			ret = -ENOENT;
			list_for_each_entry_safe(node, tmp, &dev->dma_regions_list, list) {
				if (node->region->dma_buffer_phys ==
				    dma_usr_region.dma_buffer_phys &&
				    node->region->dma_buffer_size ==
				    dma_usr_region.dma_buffer_size) {
					dma_free_coherent(&dev->pdev->dev,
						node->region->dma_buffer_size,
						node->region->dma_buffer_virt_kernel,
						node->region->dma_buffer_phys);
					kfree(node->region);
					list_del(&node->list);
					kfree(node);
					ret = 0;
					break;
				}
			}
			mutex_unlock(&dev->ops_lock);
			break;
		case PCI_MODEL_IOCTL_EXPORT_BAR:
			/* the argument is a struct pci_model_export_req */
			if (copy_from_user(&req, uarg, sizeof(req)))
				return -EFAULT;
			if (req.bar >= 6)
				return -EINVAL;
			mutex_lock(&dev->ops_lock);
			if (dev->removed) {
				ret = -ENODEV;
			} else {
				ret = pci_driver_model_export_bar(dev, &req);
			}
			mutex_unlock(&dev->ops_lock);
			/* ret is a dma-buf fd (>= 0) or a negative errno */
			return ret;
		default:
			ret = -EINVAL;
			break;
	}

	return ret;
}

static int pci_driver_model_char_mmap(struct file *file, struct vm_area_struct *vma)
{
	size_t vma_size = vma->vm_end - vma->vm_start;
	unsigned long offset = (unsigned long)vma->vm_pgoff << PAGE_SHIFT;
	struct pci_driver_model *dev = file->private_data;
	struct dma_region_node *node;
	void *dma_kaddr = NULL;
	dma_addr_t dma_addr = 0;
	bool dma_hit = false;
	int i, ret;

	/* mapping device BARs or DMA buffers into a process is privileged */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!vma_size || offset + vma_size < offset)
		return -EINVAL;

	// mmap bar space: the mmap offset is the BAR's physical address, which
	// is also what GET_BAR_INFO reports and what user space passes back.
	for (i = 0; i < 6; i++) {
		u64 base = dev->bar_regs[i].base;
		size_t size = dev->bar_regs[i].size;

		if (!base || size < PAGE_SIZE) {
			continue;
		}

		if (offset >= base && offset + vma_size <= base + size) {
			/*
			 * pgprot_noncached() *returns* the adjusted protection,
			 * it does not modify its argument: without the
			 * assignment the BAR would be mapped cacheable (WB).
			 */
			vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
			if (io_remap_pfn_range(vma,
				vma->vm_start,
				offset >> PAGE_SHIFT,
				vma_size,
				vma->vm_page_prot)) {
				return -EAGAIN;
			} else {
				return 0;
			}
		}
	}

	// mmap DMA space: the offset is the *bus* address reported by
	// ALLOC_DMA. It only identifies the buffer -- with an IOMMU enabled it
	// is an IOVA and must never be used as a pfn, so the buffer is mapped
	// through its kernel address instead.
	mutex_lock(&dev->ops_lock);
	if (dev->removed) {
		mutex_unlock(&dev->ops_lock);
		return -ENODEV;
	}
	list_for_each_entry(node, &dev->dma_regions_list, list) {
		dma_addr_t start = node->region->dma_buffer_phys;
		size_t len = node->region->dma_buffer_size;

		if (offset >= start && offset + vma_size <= start + len) {
			dma_kaddr = node->region->dma_buffer_virt_kernel;
			dma_addr = start;
			dma_hit = true;
			break;
		}
	}
	if (!dma_hit) {
		dev_err(&dev->pdev->dev, "mmap not allowed!\n");
		mutex_unlock(&dev->ops_lock);
		return -ENOMEM;
	}
	/* dma_mmap_coherent() interprets vm_pgoff as a page index *inside the
	 * buffer* (a too large index is an -ENXIO), while our mmap offset is
	 * the bus address of the region's start. */
	vma->vm_pgoff = (offset - dma_addr) >> PAGE_SHIFT;

	/* the buffer stays alive as long as we hold ops_lock */
	ret = dma_mmap_coherent(&dev->pdev->dev, vma, dma_kaddr, dma_addr, vma_size);
	mutex_unlock(&dev->ops_lock);

	return ret;
}

/*
 * dma-buf export of a device BAR, so another device's driver (an RDMA driver,
 * for instance) can import the BAR and DMA into it directly (peer-to-peer).
 *
 * The kernel's own p2pdma provider backs the buffer:
 *  - pci_p2pdma_add_resource() creates the struct pages for the BAR range;
 *    that range must align to the memory-section size, hence the 2 MiB
 *    alignment checks on the ioctl's (offset, size). Only the registered
 *    range gets struct pages, so a large BAR whose peer-DMA part is small
 *    does not pay the ~64 B-per-4-KiB page-array cost for the whole BAR.
 *  - map_dma_buf must hand the importer an sgt already mapped into the
 *    importer's DMA address space. dma_map_sgtable() on these p2pdma pages
 *    runs the provider/client policy check: a common upstream PCIe bridge
 *    classifies the mapping as PCI_P2PDMA_MAP_BUS_ADDR and fills in the
 *    BAR's bus address; without one the check fails with -EREMOTEIO.
 */
static int pci_bar_dmabuf_attach(struct dma_buf *dmabuf,
				 struct dma_buf_attachment *att)
{
	struct pci_bar_dmabuf *b = dmabuf->priv;

	mutex_lock(&b->drv->ops_lock);
	b->attached++;
	mutex_unlock(&b->drv->ops_lock);
	dev_info(&b->drv->pdev->dev, "BAR%d+0x%llx dma-buf attach from %s\n",
		 b->bar_nr, (unsigned long long)b->offset, dev_name(att->dev));
	return 0;
}

static void pci_bar_dmabuf_detach(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *att)
{
	struct pci_bar_dmabuf *b = dmabuf->priv;

	mutex_lock(&b->drv->ops_lock);
	b->attached--;
	mutex_unlock(&b->drv->ops_lock);
	dev_info(&b->drv->pdev->dev, "BAR%d+0x%llx dma-buf detach from %s\n",
		 b->bar_nr, (unsigned long long)b->offset, dev_name(att->dev));
}

static struct sg_table *pci_bar_dmabuf_map(struct dma_buf_attachment *att,
					   enum dma_data_direction dir)
{
	struct pci_bar_dmabuf *b = att->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);
	if (sg_alloc_table(sgt, 1, GFP_KERNEL)) {
		kfree(sgt);
		return ERR_PTR(-ENOMEM);
	}
	sg_set_page(sgt->sgl, b->page, b->len, 0);

	ret = dma_map_sgtable(att->dev, sgt, dir, 0);
	if (ret) {
		dev_info(&b->drv->pdev->dev,
			"BAR%d+0x%llx dma_map_sgtable for %s failed: %d\n",
			b->bar_nr, (unsigned long long)b->offset,
			dev_name(att->dev), ret);
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(ret);
	}

	dev_info(&b->drv->pdev->dev,
		"BAR%d+0x%llx mapped for %s dir=%d dma=0x%llx size=%zu\n",
		b->bar_nr, (unsigned long long)b->offset,
		dev_name(att->dev), dir,
		(unsigned long long)sg_dma_address(sgt->sgl), b->len);
	return sgt;
}

static void pci_bar_dmabuf_unmap(struct dma_buf_attachment *att,
				 struct sg_table *sgt,
				 enum dma_data_direction dir)
{
	dma_unmap_sgtable(att->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static void pci_bar_dmabuf_release(struct dma_buf *dmabuf)
{
	struct pci_bar_dmabuf *b = dmabuf->priv;
	struct pci_driver_model *drv = b->drv;

	mutex_lock(&drv->ops_lock);
	list_del(&b->list);
	mutex_unlock(&drv->ops_lock);
	dev_info(&drv->pdev->dev, "BAR%d+0x%llx dma-buf released\n",
		 b->bar_nr, (unsigned long long)b->offset);
	kfree(b);
}

static const struct dma_buf_ops pci_bar_dmabuf_ops = {
	.attach = pci_bar_dmabuf_attach,
	.detach = pci_bar_dmabuf_detach,
	.map_dma_buf = pci_bar_dmabuf_map,
	.unmap_dma_buf = pci_bar_dmabuf_unmap,
	.release = pci_bar_dmabuf_release,
};

/*
 * Hand out a dma-buf fd for one (BAR, offset, size) range (caller holds
 * ops_lock). The buffer is cached per range: repeat calls only add a
 * reference and a new fd. A p2pdma provider range is registered the first
 * time a range is requested that no already-registered range contains; it
 * lives until the driver unbinds (devm), even if every exported buffer is
 * closed meanwhile. A range that overlaps a registered one without being
 * inside it is rejected: its struct pages would collide.
 */
static int pci_driver_model_export_bar(struct pci_driver_model *drv,
				       const struct pci_model_export_req *req)
{
	struct pci_dev *pdev = drv->pdev;
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct pci_bar_dmabuf *b;
	struct pci_p2p_range *r;
	struct dma_buf *dmabuf;
	int bar_nr = req->bar;
	resource_size_t start = pci_resource_start(pdev, bar_nr);
	size_t bar_len = pci_resource_len(pdev, bar_nr);
	u64 off = req->offset;
	u64 len;
	int fd, ret;

	if (!(pci_resource_flags(pdev, bar_nr) & IORESOURCE_MEM)) {
		dev_err(&pdev->dev, "BAR%d is not a memory BAR\n", bar_nr);
		return -EINVAL;
	}
	if (req->flags) {
		dev_err(&pdev->dev, "BAR%d export: flags 0x%x must be 0\n",
			bar_nr, req->flags);
		return -EINVAL;
	}
	if (off >= bar_len) {
		dev_err(&pdev->dev,
			"BAR%d export: offset 0x%llx outside the BAR (size 0x%zx)\n",
			bar_nr, (unsigned long long)off, bar_len);
		return -EINVAL;
	}
	len = req->size ? req->size : (u64)bar_len - off;
	/* both ends of the range must sit on a 2 MiB memory-section boundary
	 * for pci_p2pdma_add_resource() to create the struct pages */
	if (off + len > bar_len || !IS_ALIGNED((u64)start + off, SZ_2M) ||
	    !IS_ALIGNED((u64)start + off + len, SZ_2M)) {
		dev_err(&pdev->dev,
			"BAR%d export off=0x%llx size=0x%llx: the range must fit in the BAR and start/end at 2 MiB boundaries\n",
			bar_nr, (unsigned long long)off,
			(unsigned long long)len);
		return -EINVAL;
	}

	list_for_each_entry(b, &drv->bar_dmabufs, list) {
		if (b->bar_nr == bar_nr && b->offset == off && b->len == len) {
			/* dma_buf_fd() consumes one reference per fd; the export
			 * reference was already handed to the first fd */
			get_dma_buf(b->dmabuf);
			goto have_buffer;
		}
	}

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->drv = drv;
	b->bar_nr = bar_nr;
	b->offset = off;
	b->len = len;

	/* register the range as a p2pdma provider unless an already
	 * registered one fully contains it */
	{
		bool covered = false;

		list_for_each_entry(r, &drv->p2p_ranges, list) {
			if (r->bar != bar_nr)
				continue;
			if (off >= r->offset && off + len <= r->offset + r->size) {
				covered = true;
				break;
			}
			if (off < r->offset + r->size && r->offset < off + len) {
				dev_err(&pdev->dev,
					"BAR%d export off=0x%llx size=0x%llx overlaps registered p2pdma range off=0x%llx size=0x%llx without being inside it\n",
					bar_nr, (unsigned long long)off,
					(unsigned long long)len,
					(unsigned long long)r->offset,
					(unsigned long long)r->size);
				kfree(b);
				return -EBUSY;
			}
		}
		if (!covered) {
			ret = pci_p2pdma_add_resource(pdev, bar_nr, len, off);
			if (ret) {
				dev_err(&pdev->dev,
					"pci_p2pdma_add_resource(BAR%d, off=0x%llx, size=0x%llx) failed: %d\n",
					bar_nr, (unsigned long long)off,
					(unsigned long long)len, ret);
				kfree(b);
				return ret;
			}
			r = kmalloc(sizeof(*r), GFP_KERNEL);
			if (!r) {
				kfree(b);
				return -ENOMEM;
			}
			r->bar = bar_nr;
			r->offset = off;
			r->size = len;
			list_add_tail(&r->list, &drv->p2p_ranges);
		}
	}

	b->page = pfn_to_page(PHYS_PFN((u64)start + off));

	info.ops = &pci_bar_dmabuf_ops;
	info.size = len;
	info.priv = b;
	/* DEFINE_DMA_BUF_EXPORT_INFO sets owner = THIS_MODULE: the
	 * dma-buf core holds a module reference until the buffer's
	 * release() runs, so rmmod is refused while an exported
	 * buffer (or an MR using it) is alive */
	dmabuf = dma_buf_export(&info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		kfree(b);
		return ret;
	}
	b->dmabuf = dmabuf;
	list_add_tail(&b->list, &drv->bar_dmabufs);

have_buffer:
	fd = dma_buf_fd(b->dmabuf, O_CLOEXEC);
	if (fd < 0) {
		/* drop the reference reserved for this fd */
		dma_buf_put(b->dmabuf);
		return fd;
	}
	dev_info(&pdev->dev,
		"BAR%d off=0x%llx size=0x%llx exported as dma-buf, fd=%d\n",
		bar_nr, (unsigned long long)off, (unsigned long long)len, fd);
	return fd;
}

static const struct file_operations pci_driver_model_char_fops = {
	.owner = THIS_MODULE,
	.open = pci_driver_model_char_open,
	.release = pci_driver_model_char_release,
	.write = pci_driver_model_char_write,
	.read = pci_driver_model_char_read,
	.mmap = pci_driver_model_char_mmap,
	.unlocked_ioctl = pci_driver_model_char_ioctl,
	.llseek = pci_driver_model_char_lseek,
};

static int pci_driver_model_device_fd_create(struct pci_driver_model *pdm_dev)
{
	struct pci_dev *pdev = pdm_dev->pdev;
	char buffer[64];
	struct device *dev_node;
	int ret;

	snprintf(buffer, sizeof(buffer), "pci_dev_%u", pdm_dev->device_idx);

	ret = alloc_chrdev_region(&pdm_dev->dev, 0, 1, buffer);
	if (ret) {
		dev_err(&pdev->dev, "alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	cdev_init(&pdm_dev->cdev, &pci_driver_model_char_fops);
	pdm_dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&pdm_dev->cdev, pdm_dev->dev, 1);
	if (ret) {
		dev_err(&pdev->dev, "cdev_add failed: %d\n", ret);
		goto err_region;
	}

	dev_node = device_create(pci_driver_model_class, &pdev->dev,
		pdm_dev->dev, NULL, "%s", buffer);
	if (IS_ERR(dev_node)) {
		ret = PTR_ERR(dev_node);
		dev_err(&pdev->dev, "device_create failed: %d\n", ret);
		goto err_cdev;
	}
	pdm_dev->dev_node = dev_node;

	dev_info(&pdev->dev, "add char file %d:%d for %s\n",
		MAJOR(pdm_dev->dev), MINOR(pdm_dev->dev), buffer);

	return 0;

err_cdev:
	cdev_del(&pdm_dev->cdev);
err_region:
	unregister_chrdev_region(pdm_dev->dev, 1);
	return ret;
}

static void pci_driver_model_device_fd_destory(struct pci_driver_model *pdm_dev)
{
	if (pdm_dev->dev_node) {
		device_destroy(pci_driver_model_class, pdm_dev->dev);
		pdm_dev->dev_node = NULL;
	}
	/* closes the door to new opens; already open fds keep their own
	 * reference to drv_data */
	cdev_del(&pdm_dev->cdev);
	unregister_chrdev_region(pdm_dev->dev, 1);
}

static void irq_work_queue_func(struct work_struct *wq)
{
	struct pci_driver_model *drv_data = container_of(wq, struct pci_driver_model, irq_wq);
	dev_dbg(&drv_data->pdev->dev, "work queue wakeup.\n");
}

static int pci_driver_model_probe(struct pci_dev *pdev, const struct pci_device_id *pent)
{
	u64 bar_base, bar_len;
	struct pci_driver_model *drv_data;
	int i, nvec, nreq = 0, ret;

	drv_data = kzalloc(sizeof(*drv_data), GFP_KERNEL);
	if (drv_data == NULL) {
		return -ENOMEM;
	}

	kref_init(&drv_data->kref);	/* the probe reference, dropped in remove */
	mutex_init(&drv_data->ops_lock);
	INIT_LIST_HEAD(&drv_data->dma_regions_list);
	INIT_LIST_HEAD(&drv_data->bar_dmabufs);
	INIT_LIST_HEAD(&drv_data->p2p_ranges);
	INIT_WORK(&drv_data->irq_wq, irq_work_queue_func);

	pci_set_drvdata(pdev, drv_data);
	drv_data->pdev = pdev;

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "enabling pci device failed: %d\n", ret);
		return ret;
	}

	ret = pci_request_regions(pdev, "pci model");
	if (ret) {
		dev_err(&pdev->dev, "requesting regions failed: %d\n", ret);
		goto err_disable;
	}

	pci_set_master(pdev);

	/* the DMA buffers handed to user space are bus addresses: without an
	 * explicit mask the default may be too narrow for a 64-bit device */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "no usable DMA addressing mode: %d\n", ret);
		goto err_regions;
	}

	drv_data->irq_count = 1;
	nvec = pci_alloc_irq_vectors(pdev, 1, drv_data->irq_count,
		PCI_IRQ_MSIX | PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (nvec < 0) {
		dev_err(&pdev->dev, "allocate IRQs failed: %d\n", nvec);
		ret = nvec;
		goto err_regions;
	}
	drv_data->irq_count = nvec;

	drv_data->irq_vec = kcalloc(nvec, sizeof(*drv_data->irq_vec), GFP_KERNEL);
	if (!drv_data->irq_vec) {
		ret = -ENOMEM;
		goto err_vectors;
	}
	for (i = 0; i < nvec; i++) {
		drv_data->irq_vec[i] = pci_irq_vector(pdev, i);
		ret = request_irq(drv_data->irq_vec[i], pci_driver_irq, 0,
			dev_name(&pdev->dev), drv_data);
		if (ret) {
			dev_err(&pdev->dev, "request of IRQ %d failed: %d\n",
				drv_data->irq_vec[i], ret);
			goto err_irqs;
		}
		nreq++;	/* only this many vectors are actually held */
	}
	dev_info(&pdev->dev, "allocated %d IRQ vector(s)\n", nvec);

	drv_data->device_idx = (u32)atomic_inc_return(&device_cnt);
	if (drv_data->device_idx >= PCI_MODEL_MAX_DEVICES) {
		dev_err(&pdev->dev, "too many model devices\n");
		ret = -ENOSPC;
		goto err_irqs;
	}

	ret = pci_driver_model_device_fd_create(drv_data);
	if (ret)
		goto err_idx;

	for (i = 0; i < 6; i++) {
		bar_base = pci_resource_start(pdev, i);
		bar_len = pci_resource_len(pdev, i);
		dev_info(&pdev->dev, "BAR%d:base = 0x%llx, len = 0x%llx\n",
			i, bar_base, bar_len);
		drv_data->bar_regs[i].base = bar_base;
		drv_data->bar_regs[i].size = bar_len;
		if (bar_len != 0) {
			drv_data->bar_regs[i].virt_kernel = pci_iomap(pdev, i, 0);
			if (!drv_data->bar_regs[i].virt_kernel) {
				dev_err(&pdev->dev, "ioremap of BAR%d failed\n", i);
				ret = -ENOMEM;
				goto err_bars;
			}
		}
	}

	/* Off by default. To also export this device through vfio-mdev, load
	 * pci_driver_virt.ko (which registers the mdev driver), embed a
	 * struct mdev_parent in struct pci_driver_model and register it here;
	 * QEMU then attaches it with
	 * -device vfio-pci,sysfsdev=/sys/bus/mdev/devices/<uuid>. */
	//pci_driver_model_mdev_register_parent(&pdev->dev, &drv_data->mdev_parent);

	return 0;

err_bars:
	for (i = 0; i < 6; i++)
		if (drv_data->bar_regs[i].virt_kernel)
			pci_iounmap(pdev, drv_data->bar_regs[i].virt_kernel);
	pci_driver_model_device_fd_destory(drv_data);
err_idx:
	atomic_dec(&device_cnt);
err_irqs:
	for (i = 0; i < nreq; i++)
		free_irq(drv_data->irq_vec[i], drv_data);
	kfree(drv_data->irq_vec);
	drv_data->irq_vec = NULL;
err_vectors:
	pci_free_irq_vectors(pdev);
err_regions:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kref_put(&drv_data->kref, pci_driver_model_release);
	return ret;
}

static void pci_driver_model_remove(struct pci_dev *pdev)
{
	struct pci_driver_model *drv_data = pci_get_drvdata(pdev);
	struct dma_region_node *node, *tmp;
	struct pci_bar_dmabuf *db;
	int i;

	/* stop the device from DMA-ing into buffers we are about to free */
	pci_clear_master(pdev);

	/*
	 * `removed` is published under ops_lock, the same lock
	 * pci_driver_model_set_efd() requires, so the vector teardown below
	 * cannot race with a SET_IRQFD re-arm.
	 */
	mutex_lock(&drv_data->ops_lock);
	drv_data->removed = true;
	for (i = 0; i < drv_data->irq_count; i++)
		free_irq(drv_data->irq_vec[i], drv_data);
	if (drv_data->efd_ctx) {
		eventfd_ctx_put(drv_data->efd_ctx);
		drv_data->efd_ctx = NULL;
	}
	/*
	 * NOTE: buffers still mapped by user space are not refcounted per
	 * mapping -- an application holding a mapping of a device that goes
	 * away keeps dangling ptes. Freeing here is what keeps a normal
	 * unbind/rmmod leak-free.
	 */
	list_for_each_entry_safe(node, tmp, &drv_data->dma_regions_list, list) {
		dma_free_coherent(&pdev->dev,
			node->region->dma_buffer_size,
			node->region->dma_buffer_virt_kernel,
			node->region->dma_buffer_phys);
		kfree(node->region);
		list_del(&node->list);
		kfree(node);
	}
	/* An exported buffer pins the module, so rmmod is refused while one is
	 * alive; reaching here with a live buffer means a manual sysfs unbind.
	 * The devm p2pdma provider (its struct pages) goes away when this
	 * returns, leaving any surviving importer with stale pages. */
	list_for_each_entry(db, &drv_data->bar_dmabufs, list)
		dev_warn(&pdev->dev,
			"BAR%d+0x%llx dma-buf still exported; its p2pdma pages go away with this unbind\n",
			db->bar_nr, (unsigned long long)db->offset);
	mutex_unlock(&drv_data->ops_lock);

	/* the bottom half touches drv_data, so it must be gone before we drop
	 * our own reference */
	cancel_work_sync(&drv_data->irq_wq);

	/* no new opens after this; existing fds keep their own reference */
	pci_driver_model_device_fd_destory(drv_data);

	for (i = 0; i < 6; i++)
		if (drv_data->bar_regs[i].virt_kernel)
			pci_iounmap(pdev, drv_data->bar_regs[i].virt_kernel);

	pci_free_irq_vectors(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);

	/* any fd still open keeps drv_data alive until it is closed */
	kref_put(&drv_data->kref, pci_driver_model_release);
}

static struct pci_driver
pci_driver_model_driver = {
	.name = "pci model",
	.id_table = pci_driver_model_id_table,
	.probe = pci_driver_model_probe,
	.remove = pci_driver_model_remove
};

static int __init pci_driver_init(void)
{
	int ret;

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 19, 0)
	pci_driver_model_class = class_create("pci_driver_model");
#else
	pci_driver_model_class = class_create(THIS_MODULE, "pci_driver_model");
#endif
	if (IS_ERR(pci_driver_model_class))
		return PTR_ERR(pci_driver_model_class);

	ret = pci_register_driver(&pci_driver_model_driver);
	if (ret)
		class_destroy(pci_driver_model_class);

	return ret;
}

static void __exit pci_driver_exit(void)
{
	pci_unregister_driver(&pci_driver_model_driver);
	class_destroy(pci_driver_model_class);
}

module_init(pci_driver_init);
module_exit(pci_driver_exit);
MODULE_DESCRIPTION("Generic PCI device test harness: BAR mmap, eventfd IRQs, coherent DMA buffers and BAR dma-buf export (p2pdma)");
MODULE_AUTHOR("OscarLGH");
MODULE_LICENSE("GPL");
/* dma_buf_export/dma_buf_fd/get/put are exported in the DMA_BUF namespace */
MODULE_IMPORT_NS("DMA_BUF");
