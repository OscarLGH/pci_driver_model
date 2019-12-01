#include <linux/proc_fs.h>

#define PCI_VENDOR_ID_MODEL 0x8086
#define PCI_DEVICE_ID_MODEL 0x100f
#define PCI_MODEL_BASE_CLASS 0x2

struct pci_bar_reg {
	u64 phys;
	u32 *virt;
	long size;
	struct proc_dir_entry *bar_proc_entry;
};
struct pci_driver_model {
	struct pci_dev *pdev;
	struct pci_bar_reg regs[6];
	void *oprom;
	u32 irq_cnt;
	u32 reserved;
	struct proc_dir_entry *device_proc_entry;
};
