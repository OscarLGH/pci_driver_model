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

static int pci_driver_model_probe(struct pci_dev *pdev, const struct pci_device_id *pent)
{
	u16 vid, did;
	u32 *bar_ptr[6];
	u64 bar_base, bar_len;
	int i;
	printk("%s\n", __FUNCTION__);
	pci_enable_device(pdev);
	pci_set_master(pdev);
	pci_read_config_word(pdev, 0x0, &vid);
	pci_read_config_word(pdev, 0x2, &did);
	printk("PCI vendor ID:%04x, PCI device ID:%04x\n", vid, did);
	for (i = 0; i < 6; i++) {
		bar_base = pci_resource_start(pdev, i);
		bar_len = pci_resource_len(pdev, i);
		printk("BAR%d:base = 0x%08llx, len = 0x%llx\n", i, bar_base, bar_len);
		if (pci_resource_start(pdev, i) != 0) {
			bar_ptr[i] = ioremap(bar_base, bar_len);
			printk("BAR%d[0x0] = %08x\n", i, bar_ptr[i][0]);
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
