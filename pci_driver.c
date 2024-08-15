#include "pci_driver.h"

struct class *pci_driver_model_class;
u64 device_cnt = 0;
static struct pci_device_id
pci_driver_model_id_table[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_MODEL, PCI_DEVICE_ID_MODEL),
	}	
};

static irqreturn_t pci_driver_irq(int irq, void *data)
{
	irqreturn_t result = IRQ_HANDLED;//IRQ_NONE
	struct pci_driver_model *drv_data = data;
	drv_data->irq_cnt++;
	printk("IRQ:%d\n", irq);

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 19, 0)
	if (drv_data->efd_ctx)
		eventfd_signal(drv_data->efd_ctx);
#else
	if (drv_data->efd_ctx)
		eventfd_signal(drv_data->efd_ctx, 1);
#endif

	schedule_work(&drv_data->irq_wq);
	return result;
}

static irqreturn_t pci_driver_irq_check(int irq, void *data)
{
	struct pci_driver_model *drv_data = data;
	printk("IRQ:%d\n", irq);
	printk("IRQ cnt:%d\n", drv_data->irq_cnt);
	return IRQ_WAKE_THREAD;//IRQ_NONE
}

static int pci_driver_model_char_open(struct inode *inode, struct file *file)
{
	struct pci_driver_model *pdev;
	pdev = container_of(inode->i_cdev, struct pci_driver_model, cdev);
	file->private_data = pdev;

	return 0;
}

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
static loff_t pci_driver_model_char_lseek(struct file *file, loff_t offset, int origin)
{
	int index;
	loff_t retval = 0;
	struct pci_driver_model *dev = file->private_data;
	//sscanf(filp->f_path.dentry->d_iname, "bar%d", &index);

	switch (origin) {
		case SEEK_SET:
			file->f_pos = offset;
			break;
		case SEEK_END:
			//retval = dev->regs[index].size;
			file->f_pos = retval;
			break;
		default:
			break;
	}
	return retval;
}
static ssize_t pci_driver_model_char_read(struct file *file, char __user *buf,
		size_t count, loff_t *pos)
{
	struct pci_driver_model *dev = file->private_data;
	return 0;
}

static ssize_t pci_driver_model_char_write(struct file *file, const char __user *buf,
                size_t count, loff_t *pos)
{
	struct pci_driver_model *dev = file->private_data;
	return 0;
}

static long pci_driver_model_char_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct pci_driver_model *dev = file->private_data;
	int ret = 0;
	struct dma_region *dma_usr_region;

	switch (cmd) {
		case PCI_MODEL_IOCTL_GET_BAR_INFO:
			copy_to_user((void *)arg, dev->bar_regs, sizeof(dev->bar_regs));
			ret = 0;
			break;
		case PCI_MODEL_IOCTL_SET_IRQFD:
			dev->efd_ctx = eventfd_ctx_fdget(arg);
			ret = 0;
			break;
		case PCI_MODEL_IOCTL_SET_IRQ:
			if (arg) {
			/* enable IRQ */

			} else {
			/* disable IRQ */

			}
			ret = 0;
			break;
		case PCI_MODEL_IOCTL_ALLOC_DMA:
            dma_usr_region = kmalloc(sizeof(*dma_usr_region), GFP_KERNEL);
            if (!dma_usr_region)
                return -ENOMEM;
            copy_from_user(dma_usr_region, (void * __user)arg, sizeof(*dma_usr_region));
            dma_usr_region->dma_buffer_virt_kernel =
                dma_alloc_coherent(&dev->pdev->dev, dma_usr_region->dma_buffer_size, &dma_usr_region->dma_buffer_phys, GFP_KERNEL);
            if (!dma_usr_region->dma_buffer_virt_kernel) {
                dev_err(&dev->pdev->dev, "Failed to allocate DMA buffer.");
                kfree(dma_usr_region);
                return -ENOMEM;
            }
            copy_to_user((void * __user)arg, dma_usr_region, sizeof(*dma_usr_region));
            struct dma_region_node *new_dma_node = kmalloc(sizeof(*new_dma_node), GFP_KERNEL);
            if (!new_dma_node) {
                kfree(dma_usr_region);
                return -ENOMEM;
            }
            new_dma_node->region = dma_usr_region;
            list_add_tail(&new_dma_node->list, &dev->dma_regions_list);
            ret = 0;
            break;
        case PCI_MODEL_IOCTL_FREE_DMA:
            struct dma_region dma_usr_region_tmp;
            copy_from_user(&dma_usr_region_tmp, (void * __user)arg, sizeof(dma_usr_region_tmp));
            struct dma_region_node *node, *tmp;
            list_for_each_entry_safe(node, tmp, &dev->dma_regions_list, list) {
                if (node->region->dma_buffer_phys == dma_usr_region_tmp.dma_buffer_phys &&
                    node->region->dma_buffer_size == dma_usr_region_tmp.dma_buffer_size) {
                        dma_free_coherent(&dev->pdev->dev,
                            node->region->dma_buffer_size,
                            node->region->dma_buffer_virt_kernel,
                            node->region->dma_buffer_phys);
                        kfree(node->region);
                        list_del(&node->list);
                        kfree(node);
                    }
            }
            ret = 0;
		default:
			ret = 0;
			break;
	}

	return ret;
}

static int pci_driver_model_char_mmap(struct file *file, struct vm_area_struct *vma)
{
	size_t vma_size = vma->vm_end - vma->vm_start;
	int i = 0;
	long current_offset = 0;
	long current_size = 0;
	struct pci_driver_model *dev = file->private_data;

	// mmap bar space
	for (i = 0; i < 6; i++) {
		if (dev->bar_regs[i].size < 0x1000) {
			continue;
		}

		if (vma->vm_pgoff * PAGE_SIZE >= dev->bar_regs[i].base &&
		vma->vm_pgoff * PAGE_SIZE + vma_size <= dev->bar_regs[i].base + dev->bar_regs[i].size) {
			pgprot_noncached(vma->vm_page_prot);
			if (remap_pfn_range(vma,
				vma->vm_start,
				vma->vm_pgoff,
				vma_size,
				vma->vm_page_prot)) {
				return -EAGAIN;
			} else {
				return 0;
			}
		}
	}

	bool can_map = false;
	struct dma_region_node *node;
    list_for_each_entry(node, &dev->dma_regions_list, list) {
        if (vma->vm_pgoff * PAGE_SIZE >= node->region->dma_buffer_phys &&
            vma->vm_pgoff * PAGE_SIZE + vma_size <= node->region->dma_buffer_phys + node->region->dma_buffer_size) {
                can_map = true;
                break;
            }
    }

    if (!can_map) {
        dev_err(&dev->pdev->dev, "mmap not allowed!");
        return -ENOMEM;
    }

	if (remap_pfn_range(vma,
		vma->vm_start,
		vma->vm_pgoff,
		vma_size,
		vma->vm_page_prot)) {
		return -EAGAIN;
	}

	return 0;
}

static const struct file_operations pci_driver_model_char_fops = {
	.owner = THIS_MODULE,
	.open = pci_driver_model_char_open,
	.write = pci_driver_model_char_write,
	.read = pci_driver_model_char_read,
	.mmap = pci_driver_model_char_mmap,
	.unlocked_ioctl = pci_driver_model_char_ioctl,
	.llseek = pci_driver_model_char_lseek,
};

int pci_driver_model_device_fd_create(struct pci_driver_model *pdm_dev)
{
	int major, minor;
	int ret;
	char buffer[256] = {0};
	struct pci_dev *pdev = pdm_dev->pdev;
	sprintf(buffer,
		"pci_dev_%d",
		pdm_dev->device_idx
	);
	alloc_chrdev_region(&pdm_dev->dev, 0, 255, "pci_dev");

	cdev_init(&pdm_dev->cdev, &pci_driver_model_char_fops);
	pdm_dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&pdm_dev->cdev, pdm_dev->dev, 1);

	device_create(pci_driver_model_class, NULL, pdm_dev->dev, NULL, buffer);
	printk("add char file %d:%d for %s\n", major, minor, buffer);

	return 0;
}

int pci_driver_model_device_fd_destory(struct pci_driver_model *pdm_dev)
{
	device_destroy(pci_driver_model_class, pdm_dev->dev);
	return 0;
}

void irq_work_queue_func(struct work_struct *wq)
{
	struct pci_driver_model *drv_data = container_of(wq, struct pci_driver_model, irq_wq);
	printk("work queue wakeup.\n");
}

static int pci_driver_model_probe(struct pci_dev *pdev, const struct pci_device_id *pent)
{
	u64 bar_base, bar_len;
	int ret;
	struct pci_driver_model *drv_data = NULL;
	int i;

	drv_data = kmalloc(sizeof(*drv_data), GFP_KERNEL);
	if (drv_data == NULL) {
		return -ENOMEM;
	}
	memset(drv_data, 0, sizeof(*drv_data));

	pci_set_drvdata(pdev, drv_data);
	drv_data->pdev = pdev;

	ret = pci_enable_device(pdev);
	if (ret) {
		printk("enabling pci device failed.\n");
		return -ENODEV;
	}

	pci_set_master(pdev);

	drv_data->irq_count = 1;
	ret = pci_alloc_irq_vectors(pdev, 1, drv_data->irq_count, PCI_IRQ_MSIX);
	if (ret < 0) {
		printk("allocate IRQs failed.\n");
		//return ret;
	}
	printk("allocated %d IRQ vectors.\n", ret);

	for (i = 0; i < drv_data->irq_count; i++) {
		ret = request_irq(pci_irq_vector(pdev, i), pci_driver_irq, 0,
			"pci_driver_irq %d", pci_get_drvdata(pdev));
	}

	INIT_WORK(&drv_data->irq_wq, irq_work_queue_func);

	drv_data->device_idx = device_cnt++;
	pci_driver_model_device_fd_create(drv_data);

	for (i = 0; i < 6; i++) {
		bar_base = pci_resource_start(pdev, i);
		bar_len = pci_resource_len(pdev, i);
		printk("BAR%d:base = 0x%08llx, len = 0x%llx\n", i, bar_base, bar_len);
		drv_data->bar_regs[i].base = bar_base;
		drv_data->bar_regs[i].size = bar_len;
		if (bar_len != 0)
			drv_data->bar_regs[i].virt_kernel = ioremap(bar_base, bar_len);
	}

	//pci_driver_model_mdev_init(&drv_data->pdev->dev, NULL);
	INIT_LIST_HEAD(&drv_data->dma_regions_list);

	return 0;
}

static void pci_driver_model_remove(struct pci_dev *pdev)
{
	int i;
	struct pci_driver_model *drv_data = pci_get_drvdata(pdev);
	for (i = 0; i < drv_data->irq_count; i++) {
		free_irq(pci_irq_vector(pdev, i), pci_get_drvdata(pdev));
	}
	pci_driver_model_device_fd_destory(drv_data);
	struct dma_region_node *node, *tmp;
	list_for_each_entry_safe(node, tmp, &drv_data->dma_regions_list, list) {
		dma_free_coherent(&pdev->dev,
			node->region->dma_buffer_size,
			node->region->dma_buffer_virt_kernel,
			node->region->dma_buffer_phys);
		kfree(node->region);
		list_del(&node->list);
		kfree(node);
	}
	kfree(drv_data);
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

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 19, 0)
	pci_driver_model_class = class_create("pci_driver_model");
#else
	pci_driver_model_class = class_create(THIS_MODULE, "pci_driver_model");
#endif
	
	return pci_register_driver(&pci_driver_model_driver);
}

static void __exit pci_driver_exit(void)
{
	pci_unregister_driver(&pci_driver_model_driver);
	class_destroy(pci_driver_model_class);
}

module_init(pci_driver_init);
module_exit(pci_driver_exit);
MODULE_LICENSE("GPL");
