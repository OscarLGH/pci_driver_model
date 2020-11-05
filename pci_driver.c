#include "pci_driver.h"

struct class *pci_driver_model_class;
static struct pci_device_id
pci_driver_model_id_table[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_MODEL, PCI_DEVICE_ID_MODEL),
		.class = PCI_MODEL_BASE_CLASS << 16,
		.class_mask = 0xff << 16
	}	
};

static irqreturn_t pci_driver_irq(int irq, void *data)
{
	irqreturn_t result = IRQ_HANDLED;//IRQ_NONE
	struct pci_driver_model *drv_data = data;
	drv_data->irq_cnt++;
	printk("IRQ:%d\n", irq);

	if (drv_data->efd_ctx)
		eventfd_signal(drv_data->efd_ctx, 1);

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

	pgprot_noncached(vma->vm_page_prot);
	while (current_size < vma_size && i < 6) {
		if (dev->bar_regs[i].size < 0x1000 ||
			vma->vm_pgoff * PAGE_SIZE > current_offset + dev->bar_regs[i].size) {
			goto next;
		}

		if (remap_pfn_range(vma,
			vma->vm_start,
			vma->vm_pgoff + dev->bar_regs[i].phys / PAGE_SIZE,
			dev->bar_regs[i].size < vma_size ? dev->bar_regs[i].size : vma_size,
			vma->vm_page_prot)) {
			return -EAGAIN;
		}

		current_size += (dev->bar_regs[i].size < vma_size ? dev->bar_regs[i].size : vma_size);
next:
		current_offset += dev->bar_regs[i].size;
		i++;

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
		"pci_%02x:%02x.%02x_%04x%04x",
		pdev->bus->number,
		(pdev->devfn >> 3) & 0x1f,
		pdev->devfn & 0x7,
		pdev->vendor,
		pdev->device
	);
	alloc_chrdev_region(&pdm_dev->dev, 0, 255, "pci_driver_model");

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
		return ret;
	}
	printk("allocated %d IRQ vectors.\n", ret);

	for (i = 0; i < drv_data->irq_count; i++) {
		ret = request_irq(pci_irq_vector(pdev, i), pci_driver_irq, 0,
			"pci_driver_irq %d", pci_get_drvdata(pdev));
	}

	INIT_WORK(&drv_data->irq_wq, irq_work_queue_func);

	drv_data->dma_buffer_size = 0x1000;
	drv_data->dma_buffer_virt = pci_alloc_consistent(drv_data->pdev, drv_data->dma_buffer_size, &drv_data->dma_buffer);

	pci_driver_model_device_fd_create(drv_data);

	for (i = 0; i < 6; i++) {
		bar_base = pci_resource_start(pdev, i);
		bar_len = pci_resource_len(pdev, i);
		printk("BAR%d:base = 0x%08llx, len = 0x%llx\n", i, bar_base, bar_len);
		drv_data->bar_regs[i].phys = bar_base;
		drv_data->bar_regs[i].size = bar_len;
		if (bar_len != 0)
			drv_data->bar_regs[i].virt = ioremap(bar_base, bar_len);
	}

	pci_driver_model_mdev_init(&drv_data->pdev->dev, NULL);

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
	pci_free_consistent(drv_data->pdev, drv_data->dma_buffer_size, drv_data->dma_buffer_virt, drv_data->dma_buffer);
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
	pci_driver_model_class = class_create(THIS_MODULE, "pci_driver_model");
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
