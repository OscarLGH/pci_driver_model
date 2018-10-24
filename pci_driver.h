#define PCI_VENDOR_ID_MODEL 0x8086
#define PCI_DEVICE_ID_MODEL 0x7110
#define PCI_MODEL_BASE_CLASS 0x6

struct pci_driver_model {
	u32 *regs[6];
	void *oprom;
	void *dma_buffer_in;
	void *dma_buffer_out;
	u32 irq_cnt;
	u32 reserved;
};
