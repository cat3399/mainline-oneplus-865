obj-$(CONFIG_MHI_BUS) += mhi.o
mhi-y := init.o main.o pm.o boot.o
mhi-$(CONFIG_MHI_BUS_DEBUG) += debugfs.o

obj-$(CONFIG_MHI_BUS_PCI_GENERIC) += mhi_pci_generic.o
mhi_pci_generic-y += pci_generic.o

obj-$(CONFIG_MHI_BUS_SAHARA_MODEM) += mhi_sahara_modem.o
mhi_sahara_modem-y += sahara.o

obj-$(CONFIG_MHI_BUS_BL_LOGGER) += mhi_bl_logger.o
mhi_bl_logger-y += mhi_bl.o

obj-$(CONFIG_MHI_BUS_CHAN_KEEPALIVE) += mhi_chan_keepalive.o
