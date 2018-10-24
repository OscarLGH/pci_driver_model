#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>

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

static int pci_driver_model_probe(struct pci_dev *pdev, const struct pci_device_id *pent)
{
	u16 vid, did;
	u64 bar_base, bar_len;
	int ret;
	struct pci_driver_model *drv_data = NULL;
	int i;

	drv_data = kmalloc(sizeof(*drv_data), GFP_KERNEL);
	if (drv_data == NULL) {
		return -ENOMEM;
	}

	pci_set_drvdata(pdev, drv_data);

	printk("%s\n", __FUNCTION__);
	ret = pci_enable_device(pdev);
	if (ret) {
		printk("enabling pci device failed.\n");
		return -ENODEV;
	}

	pci_set_master(pdev);

	ret = pci_enable_msi(pdev);
	if (ret) {
		printk("enabling MSI failed.\n");
	}

	ret = pci_request_irq(pdev, pdev->irq, pci_driver_irq_check, pci_driver_irq, pci_get_drvdata(pdev), "pci_driver_irq %d", pdev->irq);
	if (ret) {
		printk("request irq failed.\n");
		return ret;
	}

	pci_read_config_word(pdev, 0x0, &vid);
	pci_read_config_word(pdev, 0x2, &did);
	printk("PCI vendor ID:%04x, PCI device ID:%04x\n", vid, did);
	for (i = 0; i < 6; i++) {
		bar_base = pci_resource_start(pdev, i);
		bar_len = pci_resource_len(pdev, i);
		printk("BAR%d:base = 0x%08llx, len = 0x%llx\n", i, bar_base, bar_len);
		if (pci_resource_start(pdev, i) != 0) {
			drv_data->regs[i] = ioremap(bar_base, bar_len);
			printk("BAR%d[0x0] = %08x\n", i, drv_data->regs[i][0]);
		}
	}

	return 0;
}

static void pci_driver_model_remove(struct pci_dev *pdev)
{
	printk("%s\n", __FUNCTION__);
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
