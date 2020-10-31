#include "pci_driver.h"

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
		case 1:
			break;
		default:
			break;
	}

	return ret;
}

static int pci_driver_model_char_mmap(struct file *filp, struct vm_area_struct *vma)
{
	/*
	size_t size = vma->vm_end - vma->vm_start;
	int index;
	struct pci_driver_model *dev = PDE_DATA(file_inode(filp));
	sscanf(filp->f_path.dentry->d_iname, "bar%d", &index);
	pgprot_noncached(vma->vm_page_prot);
	if (remap_pfn_range(vma,
			vma->vm_start,
			vma->vm_pgoff + dev->regs[index].phys / PAGE_SIZE,
			size,
			vma->vm_page_prot)) {
		return -EAGAIN;
	}
	return 0;
	*/
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
	major = MAJOR(pdm_dev->dev);
	minor = 0x20;

	cdev_init(&pdm_dev->cdev, &pci_driver_model_char_fops);
	pdm_dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&pdm_dev->cdev, pdm_dev->dev, 1);

	pdm_dev->class = class_create(THIS_MODULE, "pci_driver_model");
	device_create(pdm_dev->class, NULL, pdm_dev->dev, NULL, buffer);
	printk("add char file %d:%d for %s\n", major, minor, buffer);

	return 0;
}

int pci_driver_model_device_fd_destory(struct pci_driver_model *pdm_dev)
{
	device_destroy(pdm_dev->class, pdm_dev->dev);
	class_destroy(pdm_dev->class);

	return 0;
}


static int pci_driver_model_proc_open(struct inode *inode, struct file *filp)
{
	int index;
	struct pci_driver_model *dev = PDE_DATA(file_inode(filp));
	sscanf(filp->f_path.dentry->d_iname, "bar%d", &index);
	inode->i_size = dev->regs[index].size;
	return 0;
}

static ssize_t pci_driver_model_proc_write(struct file *filp, const char __user *buffer,
			size_t count, loff_t *ppos)
{
	int index;
	u32 *bar_ptr;
	u32 *bar_ptr_offset;
	struct pci_driver_model *dev = PDE_DATA(file_inode(filp));
	sscanf(filp->f_path.dentry->d_iname, "bar%d", &index);
	bar_ptr = dev->regs[index].virt;

	if (*ppos > dev->regs[index].size)
		count = dev->regs[index].size;

	if (bar_ptr == NULL)
		return -EFAULT;

	if (filp->f_pos + count > dev->regs[index].size)
		return 0;

	bar_ptr_offset = bar_ptr + filp->f_pos / 4;
	copy_from_user(bar_ptr_offset, buffer, count);
	*ppos += count;
	return count;
}

static ssize_t pci_driver_model_proc_read(struct file *filp, char __user *buffer,
			size_t count, loff_t *ppos)
{
	int index;
	u32 *bar_ptr;
	u32 *bar_ptr_offset;
	struct pci_driver_model *dev = PDE_DATA(file_inode(filp));
	sscanf(filp->f_path.dentry->d_iname, "bar%d", &index);
	bar_ptr = dev->regs[index].virt;

	if (*ppos > dev->regs[index].size)
		count = dev->regs[index].size;

	if (bar_ptr == NULL)
		return -EFAULT;

	if (filp->f_pos + count > dev->regs[index].size)
		return 0;

	bar_ptr_offset = bar_ptr + filp->f_pos / 4;

	copy_to_user(buffer, bar_ptr_offset, count);
	*ppos += count;
	filp->f_pos += count;
	return count;
}

static loff_t pci_driver_model_proc_lseek(struct file *filp, loff_t offset, int origin)
{
	int index;
	loff_t retval = 0;
	struct pci_driver_model *dev = PDE_DATA(file_inode(filp));
	sscanf(filp->f_path.dentry->d_iname, "bar%d", &index);

	switch (origin) {
		case SEEK_SET:
			filp->f_pos = offset;
			break;
		case SEEK_END:
			retval = dev->regs[index].size;
			filp->f_pos = retval;
			break;
		default:
			break;
	}
	return retval;
}

static int pci_driver_model_proc_mmap(struct file *filp, struct vm_area_struct *vma)
{
	size_t size = vma->vm_end - vma->vm_start;
	int index;
	struct pci_driver_model *dev = PDE_DATA(file_inode(filp));
	sscanf(filp->f_path.dentry->d_iname, "bar%d", &index);
	pgprot_noncached(vma->vm_page_prot);
	if (remap_pfn_range(vma,
			vma->vm_start,
			vma->vm_pgoff + dev->regs[index].phys / PAGE_SIZE,
			size,
			vma->vm_page_prot)) {
		return -EAGAIN;
	}
	return 0;
}

static const struct file_operations pci_driver_model_fops = {
	.open = pci_driver_model_proc_open,
	.read = pci_driver_model_proc_read,
	.write = pci_driver_model_proc_write,
	.llseek = pci_driver_model_proc_lseek,
	.mmap = pci_driver_model_proc_mmap,
};


static int pci_driver_model_probe(struct pci_dev *pdev, const struct pci_device_id *pent)
{
	u64 bar_base, bar_len;
	int ret;
	struct pci_driver_model *drv_data = NULL;
	int i;
	char buffer[256];

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

	if (0) {
		ret = pci_enable_msi(pdev);
		if (ret) {
			printk("enabling MSI failed.\n");
		}

		ret = pci_request_irq(pdev, pdev->irq, pci_driver_irq_check, pci_driver_irq, pci_get_drvdata(pdev), "pci_driver_irq %d", pdev->irq);
		if (ret) {
			printk("request irq failed.\n");
			return ret;
		}
	}

	pci_driver_model_device_fd_create(drv_data);

	printk("Enabling %02x:%02x.%02x (%04x %04x) /proc reading.\n",
		pdev->bus->number,
		pdev->devfn >> 8,
		pdev->devfn & 0xff,
		pdev->vendor,
		pdev->device);

	sprintf(buffer, "pci_%02x:%02x.%02x_%04x%04x", pdev->bus->number, (pdev->devfn >> 3) & 0x1f, pdev->devfn & 0x7, pdev->vendor, pdev->device);
	drv_data->device_proc_entry = proc_mkdir(buffer, NULL);
	for (i = 0; i < 6; i++) {
		bar_base = pci_resource_start(pdev, i);
		bar_len = pci_resource_len(pdev, i);
		printk("BAR%d:base = 0x%08llx, len = 0x%llx\n", i, bar_base, bar_len);
		drv_data->regs[i].phys = bar_base;
		drv_data->regs[i].size = bar_len;
		if (bar_len != 0)
			drv_data->regs[i].virt = ioremap(bar_base, bar_len);

		sprintf(buffer, "bar%d", i);
		drv_data->regs[i].bar_proc_entry = proc_create_data(buffer, 0755, drv_data->device_proc_entry, &pci_driver_model_fops, drv_data);
	}

	return 0;
}


static void pci_driver_model_cleanup(struct pci_driver_model *drv_data)
{
	int i;
	char buffer[256];
	struct pci_dev *pdev = drv_data->pdev;
	for (i = 0; i < 6; i++) {
		if (drv_data->regs[i].bar_proc_entry != NULL) {
			sprintf(buffer, "bar%d", i);
			remove_proc_entry(buffer, drv_data->device_proc_entry);
		}
	}
	sprintf(buffer, "pci_%02x:%02x.%02x_%04x%04x", pdev->bus->number, (pdev->devfn >> 3) & 0x1f, pdev->devfn & 0x7, pdev->vendor, pdev->device);
	remove_proc_entry(buffer, NULL);
	device_destroy(drv_data->class, drv_data->dev);
	class_destroy(drv_data->class);
}

static void pci_driver_model_remove(struct pci_dev *pdev)
{
	struct pci_driver_model *drv_data = pci_get_drvdata(pdev);
	pci_driver_model_cleanup(drv_data);
	pci_driver_model_device_fd_destory(drv_data);
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
	return pci_register_driver(&pci_driver_model_driver);
}

static void __exit pci_driver_exit(void)
{
	pci_unregister_driver(&pci_driver_model_driver);
}

module_init(pci_driver_init);
module_exit(pci_driver_exit);
MODULE_LICENSE("GPL");
