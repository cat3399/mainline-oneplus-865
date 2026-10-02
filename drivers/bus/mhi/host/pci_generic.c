// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MHI PCI driver - MHI over PCI controller driver
 *
 * This module is a generic driver for registering MHI-over-PCI devices,
 * such as PCIe QCOM modems.
 *
 * Copyright (C) 2020 Linaro Ltd <loic.poulain@linaro.org>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/devcoredump.h>
#include <linux/esoc_client.h>
#include <linux/iommu.h>
#include <linux/ktime.h>
#include <linux/mhi.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define MHI_PCI_DEFAULT_BAR_NUM 0

#define MHI_POST_RESET_DELAY_MS 2000
#define MHI_LINK_RETRAIN_DELAY_MS 2000

#if IS_REACHABLE(CONFIG_PCIE_QCOM)
int qcom_pcie_retrain_link(struct pci_dev *pdev);
int qcom_pcie_perst_toggle(struct pci_dev *pdev);
int qcom_pcie_relink(struct pci_dev *pdev);
#else
static inline int qcom_pcie_retrain_link(struct pci_dev *pdev)
{
	return -EOPNOTSUPP;
}
static inline int qcom_pcie_perst_toggle(struct pci_dev *pdev)
{
	return -EOPNOTSUPP;
}
static inline int qcom_pcie_relink(struct pci_dev *pdev)
{
	return -EOPNOTSUPP;
}
#endif

#define HEALTH_CHECK_PERIOD (HZ * 2)

/* PCI VID definitions */
#define PCI_VENDOR_ID_THALES	0x1269
#define PCI_VENDOR_ID_QUECTEL	0x1eac
#define PCI_VENDOR_ID_NETPRISMA	0x203e

#define MHI_EDL_DB			91
#define MHI_EDL_COOKIE			0xEDEDEDED

/**
 * struct mhi_pci_dev_info - MHI PCI device specific information
 * @config: MHI controller configuration
 * @name: name of the PCI module
 * @fw: firmware path (if any)
 * @edl: emergency download mode firmware path (if any)
 * @edl_trigger: capable of triggering EDL mode in the device (if supported)
 * @bar_num: PCI base address register to use for MHI MMIO register space
 * @dma_data_width: DMA transfer word size (32 or 64 bits)
 * @mru_default: default MRU size for MBIM network packets
 * @sideband_wake: Devices using dedicated sideband GPIO for wakeup instead
 *		   of inband wake support (such as sdx24)
 * @no_m3: M3 not supported
 * @async_power_up: Don't wait synchronously for mission mode during probe.
 *                  Some external-modem setups continue their boot handshake
 *                  asynchronously after the initial BHI upload.
 * @early_boot_recovery: Allow the generic health-check timer to force PCI
 *			 recovery immediately after power-up.
 * @amss_fw: AMSS firmware path for Sahara-based loading (Fusion modems without
 *           onboard flash, e.g. SDX55 on Xiaomi Mi 10T).  When set, the path
 *           is copied to mhi_controller::amss_image so the MHI Sahara driver
 *           can find it.
 * @needs_hyp_assign: Set for Fusion modems whose PCIe SID is protected by a
 *                    TrustZone stage-2 SMMU context bank (SM8250 PCIe2,
 *                    SID=0x1d01).  mhi_mem_protect() will be called after
 *                    mhi_prepare_for_power_up() to donate MHI DMA buffers to
 *                    VMID_MSS_MSA so the modem can write to event rings.
 * @disable_pm: Keep runtime PM and PCIe link power saving disabled.
 * @no_link_pm: Disable PCIe link power saving (ASPM) but leave Linux runtime
 *              PM enabled. Needed for devices whose firmware expects the host
 *              to cycle the MHI device into M3 when idle (SDX55 fusion) but
 *              that cannot tolerate autonomous PCIe link low-power states.
 * @rddm_size: Size in bytes of the RDDM (RAM dump debug mode) buffer that the
 *             host pre-allocates and exposes to the modem via BHIe RXVEC.
 *             When non-zero the MHI core auto-allocates rddm_image during
 *             mhi_prepare_for_power_up(); on a modem crash the host can pull
 *             the dump out via mhi_download_rddm_image().  Set this for
 *             devices where post-mortem firmware diagnostics are needed.
 */
struct mhi_pci_dev_info {
	const struct mhi_controller_config *config;
	const char *name;
	const char *fw;
	const char *edl;
	const char *amss_fw;
	dma_addr_t iova_start;
	dma_addr_t iova_stop;
	bool edl_trigger;
	unsigned int bar_num;
	unsigned int dma_data_width;
	unsigned int mru_default;
	bool sideband_wake;
	bool no_m3;
	bool async_power_up;
	bool early_boot_recovery;
	bool needs_hyp_assign;
	bool disable_pm;
	bool no_link_pm;
	size_t rddm_size;
	size_t rddm_seg_len;
};

#define MHI_CHANNEL_CONFIG_UL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}						\

#define MHI_CHANNEL_CONFIG_DL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_CHANNEL_CONFIG_DL_AUTOQUEUE(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
		.auto_queue = true,			\
	}

#define MHI_EVENT_CONFIG_CTRL(ev_ring, el_count) \
	{					\
		.num_elements = el_count,	\
		.irq_moderation_ms = 0,		\
		.irq = (ev_ring) + 1,		\
		.priority = 1,			\
		.mode = MHI_DB_BRST_DISABLE,	\
		.data_type = MHI_ER_CTRL,	\
		.hardware_event = false,	\
		.client_managed = false,	\
		.offload_channel = false,	\
	}

#define MHI_CHANNEL_CONFIG_HW_UL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_ENABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = true,		\
	}						\

#define MHI_CHANNEL_CONFIG_HW_DL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_ENABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = true,		\
	}

#define MHI_CHANNEL_CONFIG_OFFLOAD_UL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = true,		\
		.doorbell_mode_switch = false,		\
	}						\

#define MHI_CHANNEL_CONFIG_OFFLOAD_DL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_AMSS),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = true,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_CHANNEL_CONFIG_UL_SBL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_SBL),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}						\

#define MHI_CHANNEL_CONFIG_DL_SBL(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_SBL),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_CHANNEL_CONFIG_UL_FP(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_TO_DEVICE,			\
		.ee_mask = BIT(MHI_EE_FP),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}						\

#define MHI_CHANNEL_CONFIG_DL_FP(ch_num, ch_name, el_count, ev_ring) \
	{						\
		.num = ch_num,				\
		.name = ch_name,			\
		.num_elements = el_count,		\
		.event_ring = ev_ring,			\
		.dir = DMA_FROM_DEVICE,			\
		.ee_mask = BIT(MHI_EE_FP),		\
		.pollcfg = 0,				\
		.doorbell = MHI_DB_BRST_DISABLE,	\
		.lpm_notify = false,			\
		.offload_channel = false,		\
		.doorbell_mode_switch = false,		\
	}

#define MHI_EVENT_CONFIG_DATA(ev_ring, el_count) \
	{					\
		.num_elements = el_count,	\
		.irq_moderation_ms = 5,		\
		.irq = (ev_ring) + 1,		\
		.priority = 1,			\
		.mode = MHI_DB_BRST_DISABLE,	\
		.data_type = MHI_ER_DATA,	\
		.hardware_event = false,	\
		.client_managed = false,	\
		.offload_channel = false,	\
	}

#define MHI_EVENT_CONFIG_SW_DATA(ev_ring, el_count) \
	{					\
		.num_elements = el_count,	\
		.irq_moderation_ms = 0,		\
		.irq = (ev_ring) + 1,		\
		.priority = 1,			\
		.mode = MHI_DB_BRST_DISABLE,	\
		.data_type = MHI_ER_DATA,	\
		.hardware_event = false,	\
		.client_managed = false,	\
		.offload_channel = false,	\
	}

#define MHI_EVENT_CONFIG_HW_DATA(ev_ring, el_count, ch_num) \
	{					\
		.num_elements = el_count,	\
		.irq_moderation_ms = 1,		\
		.irq = (ev_ring) + 1,		\
		.priority = 1,			\
		.mode = MHI_DB_BRST_DISABLE,	\
		.data_type = MHI_ER_DATA,	\
		.hardware_event = true,		\
		.client_managed = false,	\
		.offload_channel = false,	\
		.channel = ch_num,		\
	}

#define MHI_EVENT_CONFIG_OFFLOAD_DATA(ev_ring, el_count, ch_num) \
	{					\
		.num_elements = el_count,	\
		.irq_moderation_ms = 1,		\
		.irq = (ev_ring) + 1,		\
		.priority = 1,			\
		.mode = MHI_DB_BRST_DISABLE,	\
		.data_type = MHI_ER_DATA,	\
		.hardware_event = true,		\
		.client_managed = false,	\
		.offload_channel = true,	\
		.channel = ch_num,		\
	}

static const struct mhi_channel_config mhi_qcom_qdu100_channels[] = {
	MHI_CHANNEL_CONFIG_UL(0, "LOOPBACK", 32, 2),
	MHI_CHANNEL_CONFIG_DL(1, "LOOPBACK", 32, 2),
	MHI_CHANNEL_CONFIG_UL_SBL(2, "SAHARA", 128, 1),
	MHI_CHANNEL_CONFIG_DL_SBL(3, "SAHARA", 128, 1),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 64, 3),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 64, 3),
	MHI_CHANNEL_CONFIG_UL(9, "QDSS", 64, 3),
	MHI_CHANNEL_CONFIG_UL(14, "NMEA", 32, 4),
	MHI_CHANNEL_CONFIG_DL(15, "NMEA", 32, 4),
	MHI_CHANNEL_CONFIG_UL(16, "CSM_CTRL", 32, 4),
	MHI_CHANNEL_CONFIG_DL(17, "CSM_CTRL", 32, 4),
	MHI_CHANNEL_CONFIG_UL(40, "MHI_PHC", 32, 4),
	MHI_CHANNEL_CONFIG_DL(41, "MHI_PHC", 32, 4),
	MHI_CHANNEL_CONFIG_UL(46, "IP_SW0", 256, 5),
	MHI_CHANNEL_CONFIG_DL(47, "IP_SW0", 256, 5),
};

static struct mhi_event_config mhi_qcom_qdu100_events[] = {
	/* first ring is control+data ring */
	MHI_EVENT_CONFIG_CTRL(0, 64),
	/* SAHARA dedicated event ring */
	MHI_EVENT_CONFIG_SW_DATA(1, 256),
	/* Software channels dedicated event ring */
	MHI_EVENT_CONFIG_SW_DATA(2, 64),
	MHI_EVENT_CONFIG_SW_DATA(3, 256),
	MHI_EVENT_CONFIG_SW_DATA(4, 256),
	/* Software IP channels dedicated event ring */
	MHI_EVENT_CONFIG_SW_DATA(5, 512),
	MHI_EVENT_CONFIG_SW_DATA(6, 512),
	MHI_EVENT_CONFIG_SW_DATA(7, 512),
};

static const struct mhi_controller_config mhi_qcom_qdu100_config = {
	.max_channels = 128,
	.timeout_ms = 120000,
	.num_channels = ARRAY_SIZE(mhi_qcom_qdu100_channels),
	.ch_cfg = mhi_qcom_qdu100_channels,
	.num_events = ARRAY_SIZE(mhi_qcom_qdu100_events),
	.event_cfg = mhi_qcom_qdu100_events,
};

static const struct mhi_pci_dev_info mhi_qcom_qdu100_info = {
	.name = "qcom-qdu100",
	.fw = "qcom/qdu100/xbl_s.melf",
	.edl_trigger = true,
	.config = &mhi_qcom_qdu100_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
	.no_m3 = true,
};

static const struct mhi_channel_config mhi_qcom_sa8775p_channels[] = {
	MHI_CHANNEL_CONFIG_UL(46, "IP_SW0", 2048, 1),
	MHI_CHANNEL_CONFIG_DL(47, "IP_SW0", 2048, 2),
};

static struct mhi_event_config mhi_qcom_sa8775p_events[] = {
	/* first ring is control+data ring */
	MHI_EVENT_CONFIG_CTRL(0, 64),
	/* Software channels dedicated event ring */
	MHI_EVENT_CONFIG_SW_DATA(1, 64),
	MHI_EVENT_CONFIG_SW_DATA(2, 64),
};

static const struct mhi_channel_config modem_qcom_v1_mhi_channels[] = {
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 16, 1),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 16, 1),
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 4, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 4, 0),
	MHI_CHANNEL_CONFIG_UL(14, "QMI", 4, 0),
	MHI_CHANNEL_CONFIG_DL(15, "QMI", 4, 0),
	MHI_CHANNEL_CONFIG_UL(20, "IPCR", 8, 0),
	MHI_CHANNEL_CONFIG_DL_AUTOQUEUE(21, "IPCR", 8, 0),
	MHI_CHANNEL_CONFIG_UL_FP(34, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_DL_FP(35, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_UL(46, "IP_SW0", 64, 2),
	MHI_CHANNEL_CONFIG_DL(47, "IP_SW0", 64, 3),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0", 128, 4),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0", 128, 5),
};

static struct mhi_event_config modem_qcom_v1_mhi_events[] = {
	/* first ring is control+data ring */
	MHI_EVENT_CONFIG_CTRL(0, 64),
	/* DIAG dedicated event ring */
	MHI_EVENT_CONFIG_DATA(1, 128),
	/* Software channels dedicated event ring */
	MHI_EVENT_CONFIG_SW_DATA(2, 64),
	MHI_EVENT_CONFIG_SW_DATA(3, 64),
	/* Hardware channels request dedicated hardware event rings */
	MHI_EVENT_CONFIG_HW_DATA(4, 1024, 100),
	MHI_EVENT_CONFIG_HW_DATA(5, 2048, 101)
};

static const struct mhi_controller_config mhi_qcom_sa8775p_config = {
	.max_channels = 128,
	.timeout_ms = 8000,
	.num_channels = ARRAY_SIZE(mhi_qcom_sa8775p_channels),
	.ch_cfg = mhi_qcom_sa8775p_channels,
	.num_events = ARRAY_SIZE(mhi_qcom_sa8775p_events),
	.event_cfg = mhi_qcom_sa8775p_events,
};

static const struct mhi_controller_config modem_qcom_v2_mhiv_config = {
	.max_channels = 128,
	.timeout_ms = 8000,
	.ready_timeout_ms = 50000,
	.num_channels = ARRAY_SIZE(modem_qcom_v1_mhi_channels),
	.ch_cfg = modem_qcom_v1_mhi_channels,
	.num_events = ARRAY_SIZE(modem_qcom_v1_mhi_events),
	.event_cfg = modem_qcom_v1_mhi_events,
};

static const struct mhi_controller_config modem_qcom_v1_mhiv_config = {
	.max_channels = 128,
	.timeout_ms = 8000,
	.num_channels = ARRAY_SIZE(modem_qcom_v1_mhi_channels),
	.ch_cfg = modem_qcom_v1_mhi_channels,
	.num_events = ARRAY_SIZE(modem_qcom_v1_mhi_events),
	.event_cfg = modem_qcom_v1_mhi_events,
};

/* Apollo vendor DT exposes SAHARA and QRTR on the external SDX55M. */
static const struct mhi_channel_config modem_qcom_sdx55_fusion_channels[] = {
	MHI_CHANNEL_CONFIG_UL_SBL(2, "SAHARA", 128, 1),
	MHI_CHANNEL_CONFIG_DL_SBL(3, "SAHARA", 128, 1),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 16, 1),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 16, 1),
	MHI_CHANNEL_CONFIG_UL(10, "EFS", 64, 1),
	{
		.num = 11,
		.name = "EFS",
		.num_elements = 64,
		.event_ring = 1,
		.dir = DMA_FROM_DEVICE,
		.ee_mask = BIT(MHI_EE_AMSS),
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_DISABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = false,
		.wake_capable = true,
	},
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 4, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 4, 0),
	MHI_CHANNEL_CONFIG_UL(14, "QMI", 4, 0),
	MHI_CHANNEL_CONFIG_DL(15, "QMI", 4, 0),
	MHI_CHANNEL_CONFIG_UL(16, "QMI1", 64, 3),
	MHI_CHANNEL_CONFIG_DL(17, "QMI1", 64, 3),
	MHI_CHANNEL_CONFIG_UL(18, "IP_CTRL", 64, 1),
	MHI_CHANNEL_CONFIG_DL_AUTOQUEUE(19, "IP_CTRL", 64, 1),
	MHI_CHANNEL_CONFIG_UL(20, "IPCR", 8, 0),
	MHI_CHANNEL_CONFIG_DL_AUTOQUEUE(21, "IPCR", 8, 0),
	{
		.num = 25,
		.name = "BL",
		.num_elements = 32,
		.event_ring = 1,
		.dir = DMA_FROM_DEVICE,
		.ee_mask = BIT(MHI_EE_SBL),
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_DISABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = false,
		.auto_queue = true,
	},
	MHI_CHANNEL_CONFIG_UL_FP(34, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_DL_FP(35, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_UL(46, "IP_SW0", 64, 2),
	MHI_CHANNEL_CONFIG_DL(47, "IP_SW0", 64, 3),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0", 512, 7),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0", 512, 8),
	/*
	 * Call audio. The modem DMAs voice frames straight into a mailbox in
	 * system memory that the ADSP reads, so the host never moves data here
	 * -- this only has to exist so the channel is set up and a driver can
	 * bind to it. See sound/soc/qcom/qdsp6/q6voice-mhi.c.
	 */
	MHI_CHANNEL_CONFIG_OFFLOAD_UL(80, "AUDIO_VOICE_0", 64, 0),
	/*
	 * Channels lent to the audio DSP so it can drive the modem itself:
	 * it supplies the ring contexts and rings the doorbells, we only
	 * proxy the control operations it cannot perform. All four share
	 * event ring 4, which the DSP owns. See drivers/bus/mhi/host/satellite.c.
	 */
	MHI_CHANNEL_CONFIG_OFFLOAD_DL(50, "ADSP_0", 64, 4),
	MHI_CHANNEL_CONFIG_OFFLOAD_DL(51, "ADSP_1", 64, 4),
	MHI_CHANNEL_CONFIG_OFFLOAD_DL(70, "ADSP_2", 64, 4),
	MHI_CHANNEL_CONFIG_OFFLOAD_DL(71, "ADSP_3", 64, 4),
	MHI_CHANNEL_CONFIG_OFFLOAD_UL(105, "IP_HW_MHIP_0", 512, 11),
	MHI_CHANNEL_CONFIG_OFFLOAD_DL(106, "IP_HW_MHIP_0", 512, 12),
	MHI_CHANNEL_CONFIG_OFFLOAD_UL(107, "IP_HW_MHIP_1", 512, 13),
	MHI_CHANNEL_CONFIG_OFFLOAD_DL(108, "IP_HW_MHIP_1", 512, 14),
	{
		.num = 104,
		.name = "IP_HW0_RSC",
		.num_elements = 512,
		.local_elements = 3078,
		.event_ring = 8,
		.dir = DMA_FROM_DEVICE,
		.type = MHI_CH_TYPE_INBOUND_COALESCED,
		.ee_mask = BIT(MHI_EE_AMSS),
		.pollcfg = 0,
		.doorbell = MHI_DB_BRST_ENABLE,
		.lpm_notify = false,
		.offload_channel = false,
		.doorbell_mode_switch = true,
	},
	MHI_CHANNEL_CONFIG_UL(109, "RMNET_CTL", 128, 15),
	MHI_CHANNEL_CONFIG_DL(110, "RMNET_CTL", 128, 16),
};

/*
 * SDX55 Fusion firmware is normally paired with the downstream kona MHI
 * topology. Keep the generic channels, but use vendor-like event rings for
 * the hardware RMNET path and RMNET_CTL.
 */
static struct mhi_event_config modem_qcom_sdx55_fusion_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 64),
	MHI_EVENT_CONFIG_DATA(1, 256),
	MHI_EVENT_CONFIG_SW_DATA(2, 64),
	MHI_EVENT_CONFIG_SW_DATA(3, 64),
	/*
	 * Event ring for the channels lent to the audio DSP. The DSP installs
	 * its own context here and consumes the events itself, so the host must
	 * stay out of it entirely -- offload_channel is what keeps the core from
	 * claiming an IRQ for it or walking it. Marking it merely client managed
	 * is not enough: the core would still process the ring and then trip
	 * over an rp that now points into the DSP's memory.
	 */
	{
		.num_elements = 256,
		.irq_moderation_ms = 0,
		.irq = 4 + 1,
		.priority = 1,
		.mode = MHI_DB_BRST_DISABLE,
		.data_type = MHI_ER_DATA,
		.hardware_event = false,
		.client_managed = false,
		.offload_channel = true,
	},
	MHI_EVENT_CONFIG_SW_DATA(5, 512),
	MHI_EVENT_CONFIG_SW_DATA(6, 512),
	MHI_EVENT_CONFIG_HW_DATA(7, 1024, 100),
	MHI_EVENT_CONFIG_HW_DATA(8, 2048, 101),
	MHI_EVENT_CONFIG_HW_DATA(9, 1024, 102),
	MHI_EVENT_CONFIG_HW_DATA(10, 1024, 103),
	MHI_EVENT_CONFIG_OFFLOAD_DATA(11, 1024, 105),
	MHI_EVENT_CONFIG_OFFLOAD_DATA(12, 1024, 106),
	MHI_EVENT_CONFIG_OFFLOAD_DATA(13, 1024, 107),
	MHI_EVENT_CONFIG_OFFLOAD_DATA(14, 1024, 108),
	MHI_EVENT_CONFIG_HW_DATA(15, 1024, 109),
	MHI_EVENT_CONFIG_HW_DATA(16, 1024, 110),
};

static const struct mhi_controller_config modem_qcom_sdx55_fusion_config = {
	.max_channels = 128,
	.timeout_ms = 60000,
	.ready_timeout_ms = 50000,
	.num_channels = ARRAY_SIZE(modem_qcom_sdx55_fusion_channels),
	.ch_cfg = modem_qcom_sdx55_fusion_channels,
	.num_events = ARRAY_SIZE(modem_qcom_sdx55_fusion_events),
	.event_cfg = modem_qcom_sdx55_fusion_events,
};

static const struct mhi_pci_dev_info mhi_qcom_sa8775p_info = {
	.name = "qcom-sa8775p",
	.edl_trigger = false,
	.config = &mhi_qcom_sa8775p_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx75_info = {
	.name = "qcom-sdx75m",
	.fw = "qcom/sdx75m/xbl.elf",
	.edl = "qcom/sdx75m/edl.mbn",
	.edl_trigger = true,
	.config = &modem_qcom_v2_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx65_info = {
	.name = "qcom-sdx65m",
	.fw = "qcom/sdx65m/xbl.elf",
	.edl = "qcom/sdx65m/edl.mbn",
	.edl_trigger = true,
	.config = &modem_qcom_v1_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx55_info = {
	.name = "qcom-sdx55m",
	.fw = "qcom/sdx55m/sbl1.mbn",
	.edl = "qcom/sdx55m/edl.mbn",
	.edl_trigger = true,
	.config = &modem_qcom_v1_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

/*
 * Qualcomm SDX55M in Fusion configuration (e.g. Xiaomi Mi 10T / Apollo).
 *
 * BHI loads sbl1.mbn. The vendor DT programs the modem's S1-translated IOVA
 * pool as <0x20000000 0x1fffffff>, but the downstream MHI controller sets the
 * advertised start address to 0 and the stop address to pool_end. Mirror that
 * here so the MHI context registers match the vendor SDX55 setup.
 *
 * qcom_scm_assign_mem() (needs_hyp_assign) returns -EINVAL on Apollo's SM8250
 * TZ firmware, so skip it; the SMMU stage-1 IOVA mapping is sufficient for
 * the modem to DMA-read the BHI image. Sahara on MHI channels 2/3 then
 * supplies AMSS and NV images from the host; ESOC owns the reset sequence.
 */
static const struct mhi_pci_dev_info mhi_qcom_sdx55_fusion_info = {
	.name = "qcom-sdx55m-fusion",
	.fw = "qcom/sdx55m/sbl1.mbn",
	.edl = "qcom/sdx55m/edl.mbn",
	.amss_fw = "qcom/sdx55m/multi_image.mbn",
	.iova_start = 0,
	.iova_stop = 0x3fffffff,
	.edl_trigger = true,
	.config = &modem_qcom_sdx55_fusion_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	/*
	 * Align with every other SDX55-family entry (regular sdx55, sdx65,
	 * sdx75, telit, foxconn, quectel): 32-bit DMA.  IOVA pool is capped
	 * at 0x3fffffff so addresses already fit in 32 bits; setting
	 * dma_data_width=64 was making MHI core write non-zero high-address
	 * halves to registers the modem treats as reserved.
	 */
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
	.async_power_up = true,
	/* Initial bring-up must fail closed, not reset the RC under live MHI
	 * polling. The imported post-BHI recovery path rebooted the AP after
	 * a rejected Sahara image on kebab. Recover only via explicit ESOC. */
	.early_boot_recovery = false,
	.needs_hyp_assign = false,
	/*
	 * Leave PCIe ASPM alone — all other mainline SDX55 variants allow it
	 * (no_link_pm = false).  The previous comment speculated the fusion
	 * firmware couldn't tolerate L1/L1SS, but the ERRFATAL fires at a
	 * variable ~12-15s regardless of link state, which doesn't match a
	 * link-PM-sensitivity bug.  Revert to the default so the idle
	 * autosuspend path runs like on working SDX55 boards.
	 */
	.no_link_pm = false,
	/*
	 * Enable RDDM (RAM dump debug mode) capture.  Was previously disabled
	 * because at the +15s ERRFATAL baseline (dummy EFS) the modem hadn't
	 * advanced far enough to complete a BHIe RDDM transfer and the dump
	 * pull stalled the ESOC crash path.  With real EFS in place the modem
	 * stays in MISSION until a deliberate ERRFATAL from a deeper code
	 * path (e.g. xiaomi_extend uninitialized SIM dispatcher), so the
	 * BHIe transfer completes and the dump is informative — it captures
	 * the firmware function that triggered the fault, callsite chain,
	 * and the dispatcher table state we suspect is uninitialized.
	 *
	 * 64 MB total in 64 KB segments (1024 mhi_buf allocations).  Typical
	 * SDX55 RDDM dumps are 30-100 MB; 64 MB gives partial coverage with
	 * a small enough total footprint to allocate reliably on the phone.
	 */
	.rddm_size = 64 * 1024 * 1024,
	.rddm_seg_len = 0x10000,
};

static const struct mhi_pci_dev_info mhi_qcom_sdx24_info = {
	.name = "qcom-sdx24",
	.edl = "qcom/prog_firehose_sdx24.mbn",
	.config = &modem_qcom_v1_mhiv_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = true,
};

static const struct mhi_channel_config mhi_quectel_em1xx_channels[] = {
	MHI_CHANNEL_CONFIG_UL(0, "NMEA", 32, 0),
	MHI_CHANNEL_CONFIG_DL(1, "NMEA", 32, 0),
	MHI_CHANNEL_CONFIG_UL_SBL(2, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_DL_SBL(3, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 32, 1),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 32, 1),
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_UL(32, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_DL(33, "DUN", 32, 0),
	/* The EDL firmware is a flash-programmer exposing firehose protocol */
	MHI_CHANNEL_CONFIG_UL_FP(34, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_DL_FP(35, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0_MBIM", 128, 2),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0_MBIM", 128, 3),
};

static struct mhi_event_config mhi_quectel_em1xx_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 128),
	MHI_EVENT_CONFIG_DATA(1, 128),
	MHI_EVENT_CONFIG_HW_DATA(2, 1024, 100),
	MHI_EVENT_CONFIG_HW_DATA(3, 1024, 101)
};

static const struct mhi_controller_config modem_quectel_em1xx_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.ready_timeout_ms = 50000,
	.num_channels = ARRAY_SIZE(mhi_quectel_em1xx_channels),
	.ch_cfg = mhi_quectel_em1xx_channels,
	.num_events = ARRAY_SIZE(mhi_quectel_em1xx_events),
	.event_cfg = mhi_quectel_em1xx_events,
};

static const struct mhi_pci_dev_info mhi_quectel_em1xx_info = {
	.name = "quectel-em1xx",
	.edl = "qcom/prog_firehose_sdx24.mbn",
	.config = &modem_quectel_em1xx_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = true,
};

static const struct mhi_pci_dev_info mhi_quectel_rm5xx_info = {
	.name = "quectel-rm5xx",
	.edl = "qcom/prog_firehose_sdx6x.elf",
	.config = &modem_quectel_em1xx_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = true,
};

static const struct mhi_channel_config mhi_foxconn_sdx55_channels[] = {
	MHI_CHANNEL_CONFIG_UL(0, "LOOPBACK", 32, 0),
	MHI_CHANNEL_CONFIG_DL(1, "LOOPBACK", 32, 0),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 32, 1),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 32, 1),
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_UL(32, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_DL(33, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_UL_FP(34, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_DL_FP(35, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0_MBIM", 128, 2),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0_MBIM", 128, 3),
};

static struct mhi_event_config mhi_foxconn_sdx55_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 128),
	MHI_EVENT_CONFIG_DATA(1, 128),
	MHI_EVENT_CONFIG_HW_DATA(2, 1024, 100),
	MHI_EVENT_CONFIG_HW_DATA(3, 1024, 101)
};

static const struct mhi_controller_config modem_foxconn_sdx55_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.num_channels = ARRAY_SIZE(mhi_foxconn_sdx55_channels),
	.ch_cfg = mhi_foxconn_sdx55_channels,
	.num_events = ARRAY_SIZE(mhi_foxconn_sdx55_events),
	.event_cfg = mhi_foxconn_sdx55_events,
};

static const struct mhi_controller_config modem_foxconn_sdx72_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.ready_timeout_ms = 50000,
	.num_channels = ARRAY_SIZE(mhi_foxconn_sdx55_channels),
	.ch_cfg = mhi_foxconn_sdx55_channels,
	.num_events = ARRAY_SIZE(mhi_foxconn_sdx55_events),
	.event_cfg = mhi_foxconn_sdx55_events,
};

static const struct mhi_pci_dev_info mhi_foxconn_sdx55_info = {
	.name = "foxconn-sdx55",
	.edl = "qcom/sdx55m/foxconn/prog_firehose_sdx55.mbn",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx55_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_foxconn_t99w175_info = {
	.name = "foxconn-t99w175",
	.edl = "qcom/sdx55m/foxconn/prog_firehose_sdx55.mbn",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx55_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_foxconn_dw5930e_info = {
	.name = "foxconn-dw5930e",
	.edl = "qcom/sdx55m/foxconn/prog_firehose_sdx55.mbn",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx55_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_foxconn_t99w368_info = {
	.name = "foxconn-t99w368",
	.edl = "qcom/sdx65m/foxconn/prog_firehose_lite.elf",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx55_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_foxconn_t99w373_info = {
	.name = "foxconn-t99w373",
	.edl = "qcom/sdx65m/foxconn/prog_firehose_lite.elf",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx55_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_foxconn_t99w510_info = {
	.name = "foxconn-t99w510",
	.edl = "qcom/sdx24m/foxconn/prog_firehose_sdx24.mbn",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx55_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_foxconn_dw5932e_info = {
	.name = "foxconn-dw5932e",
	.edl = "qcom/sdx65m/foxconn/prog_firehose_lite.elf",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx55_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_foxconn_t99w640_info = {
	.name = "foxconn-t99w640",
	.edl = "qcom/sdx72m/foxconn/edl.mbn",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx72_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_pci_dev_info mhi_foxconn_dw5934e_info = {
	.name = "foxconn-dw5934e",
	.edl = "qcom/sdx72m/foxconn/edl.mbn",
	.edl_trigger = true,
	.config = &modem_foxconn_sdx72_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_channel_config mhi_mv3x_channels[] = {
	MHI_CHANNEL_CONFIG_UL(0, "LOOPBACK", 64, 0),
	MHI_CHANNEL_CONFIG_DL(1, "LOOPBACK", 64, 0),
	/* MBIM Control Channel */
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 64, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 64, 0),
	/* MBIM Data Channel */
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0_MBIM", 512, 2),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0_MBIM", 512, 3),
};

static struct mhi_event_config mhi_mv3x_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 256),
	MHI_EVENT_CONFIG_DATA(1, 256),
	MHI_EVENT_CONFIG_HW_DATA(2, 1024, 100),
	MHI_EVENT_CONFIG_HW_DATA(3, 1024, 101),
};

static const struct mhi_controller_config modem_mv3x_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.num_channels = ARRAY_SIZE(mhi_mv3x_channels),
	.ch_cfg = mhi_mv3x_channels,
	.num_events = ARRAY_SIZE(mhi_mv3x_events),
	.event_cfg = mhi_mv3x_events,
};

static const struct mhi_pci_dev_info mhi_mv31_info = {
	.name = "cinterion-mv31",
	.config = &modem_mv3x_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
};

static const struct mhi_pci_dev_info mhi_mv32_info = {
	.name = "cinterion-mv32",
	.config = &modem_mv3x_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
};

static const struct mhi_channel_config mhi_sierra_em919x_channels[] = {
	MHI_CHANNEL_CONFIG_UL_SBL(2, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_DL_SBL(3, "SAHARA", 256, 0),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 32, 0),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 32, 0),
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 128, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 128, 0),
	MHI_CHANNEL_CONFIG_UL(14, "QMI", 32, 0),
	MHI_CHANNEL_CONFIG_DL(15, "QMI", 32, 0),
	MHI_CHANNEL_CONFIG_UL(32, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_DL(33, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0", 512, 1),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0", 512, 2),
};

static struct mhi_event_config modem_sierra_em919x_mhi_events[] = {
	/* first ring is control+data and DIAG ring */
	MHI_EVENT_CONFIG_CTRL(0, 2048),
	/* Hardware channels request dedicated hardware event rings */
	MHI_EVENT_CONFIG_HW_DATA(1, 2048, 100),
	MHI_EVENT_CONFIG_HW_DATA(2, 2048, 101)
};

static const struct mhi_controller_config modem_sierra_em919x_config = {
	.max_channels = 128,
	.timeout_ms = 24000,
	.num_channels = ARRAY_SIZE(mhi_sierra_em919x_channels),
	.ch_cfg = mhi_sierra_em919x_channels,
	.num_events = ARRAY_SIZE(modem_sierra_em919x_mhi_events),
	.event_cfg = modem_sierra_em919x_mhi_events,
};

static const struct mhi_pci_dev_info mhi_sierra_em919x_info = {
	.name = "sierra-em919x",
	.config = &modem_sierra_em919x_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
};

static const struct mhi_channel_config mhi_telit_fn980_hw_v1_channels[] = {
	MHI_CHANNEL_CONFIG_UL(14, "QMI", 32, 0),
	MHI_CHANNEL_CONFIG_DL(15, "QMI", 32, 0),
	MHI_CHANNEL_CONFIG_UL(20, "IPCR", 16, 0),
	MHI_CHANNEL_CONFIG_DL_AUTOQUEUE(21, "IPCR", 16, 0),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0", 128, 1),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0", 128, 2),
};

static struct mhi_event_config mhi_telit_fn980_hw_v1_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 128),
	MHI_EVENT_CONFIG_HW_DATA(1, 1024, 100),
	MHI_EVENT_CONFIG_HW_DATA(2, 2048, 101)
};

static const struct mhi_controller_config modem_telit_fn980_hw_v1_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.num_channels = ARRAY_SIZE(mhi_telit_fn980_hw_v1_channels),
	.ch_cfg = mhi_telit_fn980_hw_v1_channels,
	.num_events = ARRAY_SIZE(mhi_telit_fn980_hw_v1_events),
	.event_cfg = mhi_telit_fn980_hw_v1_events,
};

static const struct mhi_pci_dev_info mhi_telit_fn980_hw_v1_info = {
	.name = "telit-fn980-hwv1",
	.fw = "qcom/sdx55m/sbl1.mbn",
	.edl = "qcom/sdx55m/edl.mbn",
	.config = &modem_telit_fn980_hw_v1_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = false,
};

static const struct mhi_channel_config mhi_telit_fn990_channels[] = {
	MHI_CHANNEL_CONFIG_UL_SBL(2, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_DL_SBL(3, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 64, 1),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 64, 1),
	MHI_CHANNEL_CONFIG_UL(12, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_DL(13, "MBIM", 32, 0),
	MHI_CHANNEL_CONFIG_UL(32, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_DL(33, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_UL(92, "DUN2", 32, 1),
	MHI_CHANNEL_CONFIG_DL(93, "DUN2", 32, 1),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0_MBIM", 128, 2),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0_MBIM", 128, 3),
};

static struct mhi_event_config mhi_telit_fn990_events[] = {
	MHI_EVENT_CONFIG_CTRL(0, 128),
	MHI_EVENT_CONFIG_DATA(1, 128),
	MHI_EVENT_CONFIG_HW_DATA(2, 1024, 100),
	MHI_EVENT_CONFIG_HW_DATA(3, 2048, 101)
};

static const struct mhi_controller_config modem_telit_fn990_config = {
	.max_channels = 128,
	.timeout_ms = 20000,
	.num_channels = ARRAY_SIZE(mhi_telit_fn990_channels),
	.ch_cfg = mhi_telit_fn990_channels,
	.num_events = ARRAY_SIZE(mhi_telit_fn990_events),
	.event_cfg = mhi_telit_fn990_events,
};

static const struct mhi_pci_dev_info mhi_telit_fn990_info = {
	.name = "telit-fn990",
	.config = &modem_telit_fn990_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
	.mru_default = 32768,
};

static const struct mhi_pci_dev_info mhi_telit_fe990a_info = {
	.name = "telit-fe990a",
	.config = &modem_telit_fn990_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
	.mru_default = 32768,
};

static const struct mhi_channel_config mhi_telit_fn920c04_channels[] = {
	MHI_CHANNEL_CONFIG_UL_SBL(2, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_DL_SBL(3, "SAHARA", 32, 0),
	MHI_CHANNEL_CONFIG_UL(4, "DIAG", 64, 1),
	MHI_CHANNEL_CONFIG_DL(5, "DIAG", 64, 1),
	MHI_CHANNEL_CONFIG_UL(14, "QMI", 32, 0),
	MHI_CHANNEL_CONFIG_DL(15, "QMI", 32, 0),
	MHI_CHANNEL_CONFIG_UL(32, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_DL(33, "DUN", 32, 0),
	MHI_CHANNEL_CONFIG_UL_FP(34, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_DL_FP(35, "FIREHOSE", 32, 0),
	MHI_CHANNEL_CONFIG_UL(92, "DUN2", 32, 1),
	MHI_CHANNEL_CONFIG_DL(93, "DUN2", 32, 1),
	MHI_CHANNEL_CONFIG_HW_UL(100, "IP_HW0", 128, 2),
	MHI_CHANNEL_CONFIG_HW_DL(101, "IP_HW0", 128, 3),
};

static const struct mhi_controller_config modem_telit_fn920c04_config = {
	.max_channels = 128,
	.timeout_ms = 50000,
	.num_channels = ARRAY_SIZE(mhi_telit_fn920c04_channels),
	.ch_cfg = mhi_telit_fn920c04_channels,
	.num_events = ARRAY_SIZE(mhi_telit_fn990_events),
	.event_cfg = mhi_telit_fn990_events,
};

static const struct mhi_pci_dev_info mhi_telit_fn920c04_info = {
	.name = "telit-fn920c04",
	.config = &modem_telit_fn920c04_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
	.mru_default = 32768,
	.edl_trigger = true,
};

static const struct mhi_pci_dev_info mhi_telit_fn990b40_info = {
	.name = "telit-fn990b40",
	.config = &modem_telit_fn920c04_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.sideband_wake = false,
	.mru_default = 32768,
	.edl_trigger = true,
};

static const struct mhi_pci_dev_info mhi_netprisma_lcur57_info = {
	.name = "netprisma-lcur57",
	.edl = "qcom/prog_firehose_sdx24.mbn",
	.config = &modem_quectel_em1xx_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = true,
};

static const struct mhi_pci_dev_info mhi_netprisma_fcun69_info = {
	.name = "netprisma-fcun69",
	.edl = "qcom/prog_firehose_sdx6x.elf",
	.config = &modem_quectel_em1xx_config,
	.bar_num = MHI_PCI_DEFAULT_BAR_NUM,
	.dma_data_width = 32,
	.mru_default = 32768,
	.sideband_wake = true,
};

/* Keep the list sorted based on the PID. New VID should be added as the last entry */
static const struct pci_device_id mhi_pci_id_table[] = {
	{PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0116),
		.driver_data = (kernel_ulong_t) &mhi_qcom_sa8775p_info },
	/* Telit FN920C04 (sdx35) */
	{PCI_DEVICE_SUB(PCI_VENDOR_ID_QCOM, 0x011a, 0x1c5d, 0x2020),
		.driver_data = (kernel_ulong_t) &mhi_telit_fn920c04_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0304),
		.driver_data = (kernel_ulong_t) &mhi_qcom_sdx24_info },
	/* EM919x (sdx55), use the same vid:pid as qcom-sdx55m */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_QCOM, 0x0306, 0x18d7, 0x0200),
		.driver_data = (kernel_ulong_t) &mhi_sierra_em919x_info },
	/* Telit FN980 hardware revision v1 */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_QCOM, 0x0306, 0x1C5D, 0x2000),
		.driver_data = (kernel_ulong_t) &mhi_telit_fn980_hw_v1_info },
	/* This ID is shared by standalone modules and phone fusion modems.
	 * Select fusion at probe only when DT supplies the ESOC control link. */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_QCOM, 0x0306, PCI_VENDOR_ID_QCOM, 0x010c),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_sdx55_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0306),
		.driver_data = (kernel_ulong_t) &mhi_qcom_sdx55_info },
	/* Telit FN990 */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_QCOM, 0x0308, 0x1c5d, 0x2010),
		.driver_data = (kernel_ulong_t) &mhi_telit_fn990_info },
	/* Telit FE990A */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_QCOM, 0x0308, 0x1c5d, 0x2015),
		.driver_data = (kernel_ulong_t) &mhi_telit_fe990a_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0308),
		.driver_data = (kernel_ulong_t) &mhi_qcom_sdx65_info },
	/* Telit FN990B40 (sdx72) */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_QCOM, 0x0309, 0x1c5d, 0x201a),
		.driver_data = (kernel_ulong_t) &mhi_telit_fn990b40_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0309),
		.driver_data = (kernel_ulong_t) &mhi_qcom_sdx75_info },
	/* QDU100, x100-DU */
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, 0x0601),
		.driver_data = (kernel_ulong_t) &mhi_qcom_qdu100_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QUECTEL, 0x1001), /* EM120R-GL (sdx24) */
		.driver_data = (kernel_ulong_t) &mhi_quectel_em1xx_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QUECTEL, 0x1002), /* EM160R-GL (sdx24) */
		.driver_data = (kernel_ulong_t) &mhi_quectel_em1xx_info },
	/* RM520N-GL (sdx6x), eSIM */
	{ PCI_DEVICE(PCI_VENDOR_ID_QUECTEL, 0x1004),
		.driver_data = (kernel_ulong_t) &mhi_quectel_rm5xx_info },
	/* RM520N-GL (sdx6x), Lenovo variant */
	{ PCI_DEVICE(PCI_VENDOR_ID_QUECTEL, 0x1007),
		.driver_data = (kernel_ulong_t) &mhi_quectel_rm5xx_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QUECTEL, 0x100d), /* EM160R-GL (sdx24) */
		.driver_data = (kernel_ulong_t) &mhi_quectel_em1xx_info },
	{ PCI_DEVICE(PCI_VENDOR_ID_QUECTEL, 0x2001), /* EM120R-GL for FCCL (sdx24) */
		.driver_data = (kernel_ulong_t) &mhi_quectel_em1xx_info },
	/* T99W175 (sdx55), Both for eSIM and Non-eSIM */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0ab),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w175_info },
	/* DW5930e (sdx55), With eSIM, It's also T99W175 */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0b0),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_dw5930e_info },
	/* DW5930e (sdx55), Non-eSIM, It's also T99W175 */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0b1),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_dw5930e_info },
	/* T99W175 (sdx55), Based on Qualcomm new baseline */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0bf),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w175_info },
	/* T99W175 (sdx55) */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0c3),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w175_info },
	/* T99W368 (sdx65) */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0d8),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w368_info },
	/* T99W373 (sdx62) */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0d9),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w373_info },
	/* T99W510 (sdx24), variant 1 */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0f0),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w510_info },
	/* T99W510 (sdx24), variant 2 */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0f1),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w510_info },
	/* T99W510 (sdx24), variant 3 */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0f2),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w510_info },
	/* DW5932e-eSIM (sdx62), With eSIM */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0f5),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_dw5932e_info },
	/* DW5932e (sdx62), Non-eSIM */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe0f9),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_dw5932e_info },
	/* T99W640 (sdx72) */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe118),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w640_info },
	/* DW5934e(sdx72), With eSIM */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe11d),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_dw5934e_info },
	/* DW5934e(sdx72), Non-eSIM */
	{ PCI_DEVICE(PCI_VENDOR_ID_FOXCONN, 0xe11e),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_dw5934e_info },
	/* MV31-W (Cinterion) */
	{ PCI_DEVICE(PCI_VENDOR_ID_THALES, 0x00b3),
		.driver_data = (kernel_ulong_t) &mhi_mv31_info },
	/* MV31-W (Cinterion), based on new baseline */
	{ PCI_DEVICE(PCI_VENDOR_ID_THALES, 0x00b4),
		.driver_data = (kernel_ulong_t) &mhi_mv31_info },
	/* MV32-WA (Cinterion) */
	{ PCI_DEVICE(PCI_VENDOR_ID_THALES, 0x00ba),
		.driver_data = (kernel_ulong_t) &mhi_mv32_info },
	/* MV32-WB (Cinterion) */
	{ PCI_DEVICE(PCI_VENDOR_ID_THALES, 0x00bb),
		.driver_data = (kernel_ulong_t) &mhi_mv32_info },
	/* T99W175 (sdx55), HP variant */
	{ PCI_DEVICE(0x03f0, 0x0a6c),
		.driver_data = (kernel_ulong_t) &mhi_foxconn_t99w175_info },
	/* NETPRISMA LCUR57 (SDX24) */
	{ PCI_DEVICE(PCI_VENDOR_ID_NETPRISMA, 0x1000),
		.driver_data = (kernel_ulong_t) &mhi_netprisma_lcur57_info },
	/* NETPRISMA FCUN69 (SDX6X) */
	{ PCI_DEVICE(PCI_VENDOR_ID_NETPRISMA, 0x1001),
		.driver_data = (kernel_ulong_t) &mhi_netprisma_fcun69_info },
	{  }
};
MODULE_DEVICE_TABLE(pci, mhi_pci_id_table);

enum mhi_pci_device_status {
	MHI_PCI_DEV_STARTED,
	MHI_PCI_DEV_SUSPENDED,
	MHI_PCI_DEV_LINK_RETRAIN_PENDING,
	MHI_PCI_DEV_AUTOSUSPEND_SET,
};

/*
 * MSI polling interval: On Qualcomm SM8250, when the endpoint's IOMMU domain
 * is DMA-translated (not identity), DW PCIe iMSI-RX cannot catch MSI TLPs
 * because the SMMU TBU sits in the inbound path before iMSI-RX.  This timer
 * polls event rings to compensate for the missing MSI delivery.
 */
#define MHI_MSI_POLL_INTERVAL_MS	2

struct mhi_pci_device {
	struct mhi_controller mhi_cntrl;
	struct pci_saved_state *pci_state;
	struct work_struct recovery_work;
	struct delayed_work link_retrain_work;
	struct delayed_work pm_probe_work;
	int pm_probe_iter;
	struct timer_list health_check_timer;
	struct timer_list msi_poll_timer;
	bool msi_poll_enabled;
	bool msi_poll_saw_m0;
	unsigned long status;
	const struct mhi_pci_dev_info *info;
	struct esoc_desc *esoc_client;
	struct esoc_client_hook esoc_hook;
};

static int mhi_pci_power_up(struct mhi_pci_device *mhi_pdev);
static void mhi_pci_start_msi_poll(struct mhi_pci_device *mhi_pdev);

static int mhi_pci_read_reg(struct mhi_controller *mhi_cntrl,
			    void __iomem *addr, u32 *out)
{
	*out = readl(addr);
	return 0;
}

static void mhi_pci_write_reg(struct mhi_controller *mhi_cntrl,
			      void __iomem *addr, u32 val)
{
	writel(val, addr);
}

/*
 * Linearise the modem's RDDM (RAM dump debug mode) image into a vmalloc
 * buffer and hand it to devcoredump so the user can pull the dump from
 * /sys/class/devcoredump/.  Also log the first 256 bytes via hex dump so the
 * crash header is visible directly in dmesg without copying files.
 *
 * Caller must guarantee BHIe RX vector status is XFER_COMPL (i.e.
 * mhi_download_rddm_image() returned 0) before calling this.
 */
static void mhi_pci_collect_rddm(struct mhi_controller *mhi_cntrl)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	struct image_info *img = mhi_cntrl->rddm_image;
	size_t total = 0, copied = 0;
	struct mhi_buf *mhi_buf;
	void *dump;
	u32 i;

	if (!img || !img->entries) {
		dev_warn(&pdev->dev, "RDDM: no image buffer to collect\n");
		return;
	}

	/* Last entry is the BHI vector table itself, skip it. */
	for (i = 0; i < img->entries - 1; i++)
		total += img->mhi_buf[i].len;

	if (!total) {
		dev_warn(&pdev->dev, "RDDM: image buffer is empty\n");
		return;
	}

	dump = vmalloc(total);
	if (!dump) {
		dev_warn(&pdev->dev,
			 "RDDM: vmalloc(%zu) failed, dropping dump\n", total);
		return;
	}

	mhi_buf = img->mhi_buf;
	for (i = 0; i < img->entries - 1; i++, mhi_buf++) {
		memcpy((u8 *)dump + copied, mhi_buf->buf, mhi_buf->len);
		copied += mhi_buf->len;
	}

	dev_info(&pdev->dev, "RDDM: collected %zu bytes from modem\n", copied);
	print_hex_dump(KERN_INFO, "rddm-head: ", DUMP_PREFIX_OFFSET, 16, 1,
		       dump, min_t(size_t, copied, 256), true);

	/* dev_coredumpv takes ownership of the vmalloc'd buffer */
	dev_coredumpv(&pdev->dev, dump, copied, GFP_KERNEL);
}

static void mhi_pci_status_cb(struct mhi_controller *mhi_cntrl,
			      enum mhi_callback cb)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);

	/* Nothing to do for now */
	switch (cb) {
	case MHI_CB_EE_RDDM:
		dev_warn(&pdev->dev,
			 "modem entered RDDM, downloading crash dump\n");
		if (!mhi_download_rddm_image(mhi_cntrl, false))
			mhi_pci_collect_rddm(mhi_cntrl);
		else
			dev_warn(&pdev->dev,
				 "RDDM: BHIe download failed\n");
		pm_runtime_forbid(&pdev->dev);
		break;
	case MHI_CB_FATAL_ERROR:
	case MHI_CB_SYS_ERROR:
		dev_warn(&pdev->dev, "firmware crashed (%u)\n", cb);
		pm_runtime_forbid(&pdev->dev);
		break;
	case MHI_CB_EE_MISSION_MODE:
		/*
		 * Downstream mhi_qcom forces a suspend/resume cycle at mission
		 * mode so the modem can switch to AMSS PCIe PHY settings.
		 *
		 * This runs BEFORE mhi_create_devices() so pending_pkts == 0
		 * and the suspend won't be refused.
		 */
		if (mhi_pdev && mhi_pdev->info && mhi_pdev->info->async_power_up &&
		    mhi_pdev->info != &mhi_qcom_sdx55_fusion_info) {
			int ret, retries = 0;
			struct pci_saved_state *saved_state = NULL;

			do {
				ret = mhi_pm_suspend(mhi_cntrl);
				if (ret == -EBUSY) {
					msleep(100);
					retries++;
				}
			} while (ret == -EBUSY && retries < 10);

			if (!ret) {
				dev_info(&pdev->dev,
					 "mission mode: M3 OK (retries=%d), cycling D3hot\n",
					 retries);

				pci_clear_master(pdev);
				ret = pci_save_state(pdev);
				if (ret)
					dev_warn(&pdev->dev,
						 "mission mode: pci_save_state failed: %d\n",
						 ret);
				else
					saved_state = pci_store_saved_state(pdev);
				pci_disable_device(pdev);
				pci_set_power_state(pdev, PCI_D3hot);

				msleep(250);

				pci_set_power_state(pdev, PCI_D0);
				if (pci_enable_device(pdev))
					dev_warn(&pdev->dev,
						 "mission mode: pci_enable_device failed\n");
				if (saved_state) {
					ret = pci_load_and_free_saved_state(pdev,
									    &saved_state);
					if (ret)
						dev_warn(&pdev->dev,
							 "mission mode: pci_load_and_free_saved_state failed: %d\n",
							 ret);
				}
				pci_restore_state(pdev);
				pci_set_master(pdev);

				ret = mhi_pm_resume(mhi_cntrl);
				if (ret)
					dev_warn(&pdev->dev,
						 "mission mode: MHI resume failed: %d\n",
						 ret);
				else
					dev_info(&pdev->dev,
						 "mission mode: D3hot cycle complete\n");
			} else {
				dev_warn(&pdev->dev,
					 "mission mode: MHI suspend failed: %d (retries=%d)\n",
					 ret, retries);
			}
		}

		/*
		 * Some fusion setups need PCIe ASPM disabled even in mission
		 * mode. Respect the device policy from probe.
		 */
		if (mhi_pdev && mhi_pdev->info &&
		    (mhi_pdev->info->disable_pm || mhi_pdev->info->no_link_pm)) {
			pci_disable_link_state(pdev, PCIE_LINK_STATE_ALL);
			dev_info(&pdev->dev,
				 "mission mode: keeping PCIe link power saving disabled\n");
		}

		if (mhi_pdev && mhi_pdev->info && mhi_pdev->info->disable_pm) {
			pm_runtime_forbid(&pdev->dev);
			dev_info(&pdev->dev,
				 "mission mode: keeping runtime PM forbidden (disable_pm)\n");
		} else if (mhi_pdev && mhi_pdev->info == &mhi_qcom_sdx55_fusion_info) {
			/*
			 * SDX55 fusion modems crash with ERRFATAL ~5 s into
			 * mhi_pm_suspend (MCFG refresh timer watchdog fires
			 * while the host is blocked waiting for M3 acknowledgement).
			 * Forbid runtime PM entirely — the modem stays in M0.
			 */
			pm_runtime_forbid(&pdev->dev);
			dev_info(&pdev->dev,
				 "mission mode: forbidding runtime PM to prevent ERRFATAL on SDX55 fusion\n");
		} else if (mhi_pdev && mhi_pdev->info && !mhi_pdev->info->no_m3 &&
			   mhi_pdev->esoc_client &&
			   pci_pme_capable(pdev, PCI_D3hot) &&
			   !test_and_set_bit(MHI_PCI_DEV_AUTOSUSPEND_SET,
					     &mhi_pdev->status)) {
			/*
			 * ESOC-client path: the direct-probe autosuspend block
			 * at the bottom of mhi_pci_probe() is unreachable here
			 * because probe returns early when an ESOC client is
			 * registered.  Mirror that autosuspend setup now.
			 *
			 * Mirror the non-ESOC probe-end autosuspend setup now.
			 * We previously only called pm_runtime_allow() here,
			 * which left usage_count stuck at 1 on Apollo and
			 * prevented the idle M3 autosuspend transition from
			 * ever arming. Drop the count explicitly after allowing
			 * runtime PM, just like the normal probe path does.
			 */
			pm_runtime_set_autosuspend_delay(&pdev->dev, 2000);
			pm_runtime_use_autosuspend(&pdev->dev);
			pm_runtime_mark_last_busy(&pdev->dev);
			pm_runtime_allow(&pdev->dev);
			pm_runtime_put_noidle(&pdev->dev);
			dev_info(&pdev->dev,
				 "mission mode: enabled runtime PM autosuspend (2000ms), usage=%d\n",
				 atomic_read(&pdev->dev.power.usage_count));
			schedule_delayed_work(&mhi_pdev->pm_probe_work,
				      msecs_to_jiffies(100));
		}
		break;
	case MHI_CB_IDLE:
		/*
		 * Downstream mhi_qcom uses this callback to explicitly kick
		 * runtime PM autosuspend.  MHI core fires MHI_CB_IDLE when
		 * the device transitions M1→M2 and has no pending packets or
		 * wake assertions.  Without this, the firmware's idle watchdog
		 * fires because the host never drives the M3 suspend that the
		 * autosuspend timer would trigger.
		 */
		pm_runtime_mark_last_busy(&pdev->dev);
		pm_request_autosuspend(&pdev->dev);
		break;
	default:
		break;
	}
}

static void mhi_pci_wake_get_nop(struct mhi_controller *mhi_cntrl, bool force)
{
	/* no-op */
}

static void mhi_pci_wake_put_nop(struct mhi_controller *mhi_cntrl, bool override)
{
	/* no-op */
}

static void mhi_pci_wake_toggle_nop(struct mhi_controller *mhi_cntrl)
{
	/* no-op */
}

static bool mhi_pci_is_alive(struct mhi_controller *mhi_cntrl)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	u16 vendor = 0;

	if (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor))
		return false;

	if (vendor == (u16) ~0 || vendor == 0)
		return false;

	return true;
}

static int mhi_pci_claim(struct mhi_controller *mhi_cntrl,
			 unsigned int bar_num, u64 dma_mask)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "failed to enable pci device: %d\n", err);
		return err;
	}

	mhi_cntrl->regs = pcim_iomap_region(pdev, bar_num, pci_name(pdev));
	if (IS_ERR(mhi_cntrl->regs)) {
		err = PTR_ERR(mhi_cntrl->regs);
		dev_err(&pdev->dev, "failed to map pci region: %d\n", err);
		return err;
	}
	mhi_cntrl->reg_len = pci_resource_len(pdev, bar_num);

	err = dma_set_mask_and_coherent(&pdev->dev, dma_mask);
	if (err) {
		dev_err(&pdev->dev, "Cannot set proper DMA mask\n");
		return err;
	}

	pci_set_master(pdev);

	return 0;
}

/*
 * On Qualcomm SM8250, the SMMU TBU sits between the PCIe endpoint and the DW
 * PCIe iMSI-RX module.  When the endpoint's IOMMU domain is DMA-translated
 * (not identity), MSI TLPs carry an address the SMMU has no mapping for and
 * they fault.  Identity-map the MSI target page so the write reaches the
 * iMSI-RX address comparator.
 *
 * Must run after the endpoint's MSI capability has been programmed, i.e. after
 * pci_alloc_irq_vectors() -- the target address is read back out of it.
 */
static void mhi_pci_map_msi_page(struct pci_dev *pdev)
{
	struct iommu_domain *domain;
	int msi_cap;
	int err;

	domain = iommu_get_domain_for_dev(&pdev->dev);
	msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	if (domain && msi_cap) {
		u32 addr_lo = 0, addr_hi = 0;
		u16 msi_flags = 0;
		phys_addr_t msi_page;

		pci_read_config_dword(pdev, msi_cap + PCI_MSI_ADDRESS_LO,
				      &addr_lo);
		pci_read_config_word(pdev, msi_cap + PCI_MSI_FLAGS,
				     &msi_flags);
		if (msi_flags & PCI_MSI_FLAGS_64BIT)
			pci_read_config_dword(pdev,
					      msi_cap + PCI_MSI_ADDRESS_HI,
					      &addr_hi);

		msi_page = (((phys_addr_t)addr_hi << 32) | addr_lo) & PAGE_MASK;
		if (msi_page && iommu_iova_to_phys(domain, msi_page) == msi_page) {
			/* already identity-mapped, e.g. across a link rebuild */
			dev_dbg(&pdev->dev,
				"MSI target page 0x%pa already mapped\n",
				&msi_page);
		} else if (msi_page) {
			err = iommu_map(domain, msi_page, msi_page, PAGE_SIZE,
					IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO,
					GFP_KERNEL);
			if (err && err != -EEXIST)
				dev_warn(&pdev->dev,
					 "Failed to map MSI page 0x%pa in SMMU: %d\n",
					 &msi_page, err);
			else
				dev_info(&pdev->dev,
					 "MSI target page 0x%pa mapped in SMMU domain (type %d)\n",
					 &msi_page, domain->type);
		} else {
			dev_warn(&pdev->dev,
				 "endpoint MSI address is zero, MSIs will fault\n");
		}
	}
}

static int mhi_pci_get_irqs(struct mhi_controller *mhi_cntrl,
			    const struct mhi_controller_config *mhi_cntrl_config)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int nr_vectors, i;
	int *irq;

	/*
	 * Alloc one MSI vector for BHI + one vector per event ring, ideally...
	 * No explicit pci_free_irq_vectors required, done by pcim_release.
	 */
	mhi_cntrl->nr_irqs = 1 + mhi_cntrl_config->num_events;

	nr_vectors = pci_alloc_irq_vectors(pdev, 1, mhi_cntrl->nr_irqs, PCI_IRQ_MSIX | PCI_IRQ_MSI);
	if (nr_vectors < 0) {
		dev_err(&pdev->dev, "Error allocating MSI vectors %d\n",
			nr_vectors);
		return nr_vectors;
	}

	if (nr_vectors < mhi_cntrl->nr_irqs) {
		dev_warn(&pdev->dev, "using shared MSI\n");

		/* Patch msi vectors, use only one (shared) */
		for (i = 0; i < mhi_cntrl_config->num_events; i++)
			mhi_cntrl_config->event_cfg[i].irq = 0;
		mhi_cntrl->nr_irqs = 1;
	}

	irq = devm_kcalloc(&pdev->dev, mhi_cntrl->nr_irqs, sizeof(int), GFP_KERNEL);
	if (!irq)
		return -ENOMEM;

	for (i = 0; i < mhi_cntrl->nr_irqs; i++) {
		int vector = i >= nr_vectors ? (nr_vectors - 1) : i;

		irq[i] = pci_irq_vector(pdev, vector);
	}

	mhi_cntrl->irq = irq;

	return 0;
}

static int mhi_pci_runtime_get(struct mhi_controller *mhi_cntrl, void *priv)
{
	/* The runtime_get() MHI callback means:
	 *    Do whatever is requested to leave M3.
	 */
	return pm_runtime_get(mhi_cntrl->cntrl_dev);
}

static void mhi_pci_runtime_put(struct mhi_controller *mhi_cntrl, void *priv)
{
	/* The runtime_put() MHI callback means:
	 *    Device can be moved in M3 state.
	 */
	pm_runtime_mark_last_busy(mhi_cntrl->cntrl_dev);
	pm_runtime_put(mhi_cntrl->cntrl_dev);
}

static u64 mhi_pci_time_get(struct mhi_controller *mhi_cntrl, void *priv)
{
	return ktime_get_boottime_ns() / NSEC_PER_USEC;
}

static int mhi_pci_lpm_disable(struct mhi_controller *mhi_cntrl, void *priv)
{
	return 0;
}

static int mhi_pci_lpm_enable(struct mhi_controller *mhi_cntrl, void *priv)
{
	return 0;
}

static enum pci_bus_speed mhi_pci_to_pcie_speed(u32 target)
{
	switch (target) {
	case 1:
		return PCIE_SPEED_2_5GT;
	case 2:
		return PCIE_SPEED_5_0GT;
	case 3:
		return PCIE_SPEED_8_0GT;
	case 4:
		return PCIE_SPEED_16_0GT;
	case 5:
		return PCIE_SPEED_32_0GT;
	case 6:
		return PCIE_SPEED_64_0GT;
	default:
		return PCI_SPEED_UNKNOWN;
	}
}

static int mhi_pci_bw_scale(struct mhi_controller *mhi_cntrl,
			    struct mhi_link_info *link_info)
{
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	struct pci_dev *bridge = pci_upstream_bridge(pdev);
	enum pci_bus_speed speed;
	int ret;

	speed = mhi_pci_to_pcie_speed(link_info->target_link_speed);
	if (speed == PCI_SPEED_UNKNOWN) {
		dev_warn(&pdev->dev,
			 "BW_REQ: unsupported speed encoding %u (width %u)\n",
			 link_info->target_link_speed,
			 link_info->target_link_width);
		return -EINVAL;
	}

	if (!bridge) {
		dev_info(&pdev->dev,
			 "BW_REQ: no upstream bridge, ignoring request speed=%u width=%u\n",
			 link_info->target_link_speed,
			 link_info->target_link_width);
		return 0;
	}

	ret = pcie_set_target_speed(bridge, speed, false);
	if (ret)
		dev_warn(&pdev->dev,
			 "BW_REQ: failed to set target speed=%u width=%u on %s: %d\n",
			 link_info->target_link_speed, link_info->target_link_width,
			 pci_name(bridge), ret);
	else
		dev_info(&pdev->dev,
			 "BW_REQ: requested speed=%u width=%u via %s\n",
			 link_info->target_link_speed, link_info->target_link_width,
			 pci_name(bridge));

	return ret;
}

/*
 * Periodic diagnostic probe scheduled from mission-mode entry.
 *
 * Logs PCI runtime PM state, MHI dev_wake / pending_pkts, and current MHI
 * execution environment for several seconds after mission mode is entered,
 * so we can see why autosuspend isn't driving the device to M3 before the
 * firmware idle watchdog fires.
 */
static void mhi_pci_pm_probe_work(struct work_struct *work)
{
	struct mhi_pci_device *mhi_pdev = container_of(to_delayed_work(work),
						       struct mhi_pci_device,
						       pm_probe_work);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	struct device *dev = &pdev->dev;
	dev_info(dev,
		 "pm_probe[%d]: usage=%d disable_depth=%d runtime_status=%d "
		 "dev_wake=%d pending_pkts=%d ee=0x%x dev_state=0x%x\n",
		 mhi_pdev->pm_probe_iter,
		 atomic_read(&dev->power.usage_count),
		 dev->power.disable_depth,
		 dev->power.runtime_status,
		 atomic_read(&mhi_cntrl->dev_wake),
		 atomic_read(&mhi_cntrl->pending_pkts),
		 (unsigned int)mhi_cntrl->ee,
		 (unsigned int)mhi_cntrl->dev_state);

	/*
	 * Only force a single early M3 proof cycle. Repeating it during active
	 * IPCR/DIAG bring-up perturbs the very traffic we're trying to observe.
	 */
	if (mhi_pdev->pm_probe_iter == 0) {
		int ret = mhi_pm_suspend(mhi_cntrl);

		if (!ret) {
			msleep(50);
			ret = mhi_pm_resume(mhi_cntrl);
			dev_info(dev, "pm_probe[%d]: forced M3 cycle OK, resume=%d\n",
				 mhi_pdev->pm_probe_iter, ret);

			/*
			 * Keep exactly one proven M3 cycle then pin the link
			 * back in M0. Testing confirmed the post-mission
			 * ERRFATAL timing is invariant to runtime PM state
			 * (identical crash time with continuous M3 cycling or
			 * no further cycles), so long-lived M3 is neither
			 * required nor harmful — just leave the link in the
			 * simplest state.
			 */
			if (!ret && mhi_pdev->info == &mhi_qcom_sdx55_fusion_info) {
				pm_runtime_forbid(dev);
				dev_info(dev,
					 "pm_probe[%d]: disabled runtime PM after one successful M3 cycle on SDX55 fusion\n",
					 mhi_pdev->pm_probe_iter);
			}
		} else {
			dev_info(dev, "pm_probe[%d]: forced M3 failed: %d\n",
				 mhi_pdev->pm_probe_iter, ret);
		}
	}

	mhi_pdev->pm_probe_iter++;
	if (mhi_pdev->pm_probe_iter < 20)
		schedule_delayed_work(&mhi_pdev->pm_probe_work,
				      msecs_to_jiffies(1000));
}

/*
 * Link retrain work — runs ~3 seconds after BHI firmware loading.
 *
 * On Fusion modems (e.g. SDX55M on SM8250/Apollo) the modem drops the PCIe
 * link when SBL transitions to AMSS boot from internal NAND.  The vendor
 * kernel handles this by doing a full PCIe disable/enable cycle with PERST#
 * toggle ~2 s after BHI.  We replicate that here: check if the link died,
 * toggle PERST# via the Qcom PCIe driver, then re-initialise MHI.
 */
static void mhi_pci_link_retrain_work(struct work_struct *work)
{
	struct mhi_pci_device *mhi_pdev = container_of(work, struct mhi_pci_device,
						       link_retrain_work.work);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	clear_bit(MHI_PCI_DEV_LINK_RETRAIN_PENDING, &mhi_pdev->status);

	if (!mhi_pdev->info->early_boot_recovery) {
		dev_warn(&pdev->dev, "automatic link retrain disabled; leaving modem for diagnosis\n");
		return;
	}

	/* If device is alive, the modem booted without dropping the link */
	if (mhi_pci_is_alive(mhi_cntrl)) {
		dev_dbg(&pdev->dev, "link retrain: device is alive, nothing to do\n");
		return;
	}

	dev_info(&pdev->dev, "link retrain: device unreachable after BHI, retraining PCIe link\n");

	/* Clean up MHI state from the failed first attempt */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, false);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	/* Toggle PERST# and retrain the link */
	err = qcom_pcie_retrain_link(pdev);
	if (err) {
		dev_err(&pdev->dev, "link retrain: PCIe link retrain failed: %d\n", err);
		return;
	}

	/* Restore PCI state and re-enable the device */
	pci_load_saved_state(pdev, mhi_pdev->pci_state);
	pci_restore_state(pdev);

	if (!mhi_pci_is_alive(mhi_cntrl)) {
		dev_err(&pdev->dev, "link retrain: device still unreachable after retrain\n");
		return;
	}

	/* Dump raw MMIO registers via BAR0 to diagnose modem state */
	{
		void __iomem *base = mhi_cntrl->regs;
		u32 mhiver, mhicfg, mhictrl, mhistatus, bhioff, bhiever;
		u32 bhiee, bhiimgaddr_lo, bhiimgaddr_hi, bhiimgsize, bhistatus;

		mhiver    = base ? ioread32(base + 0x00) : 0xdead0000;
		mhicfg    = base ? ioread32(base + 0x10) : 0xdead0010;
		mhictrl   = base ? ioread32(base + 0x38) : 0xdead0038;
		mhistatus = base ? ioread32(base + 0x48) : 0xdead0048;
		bhioff    = base ? ioread32(base + 0x28) : 0xdead0028;
		dev_info(&pdev->dev,
			 "link retrain: MMIO regs: MHIVER=0x%x MHICFG=0x%x MHICTRL=0x%x MHISTATUS=0x%x BHIOFF=0x%x\n",
			 mhiver, mhicfg, mhictrl, mhistatus, bhioff);

		if (base && bhioff < 0x1000) {
			bhiee         = ioread32(base + bhioff + 0x28);
			bhiever       = ioread32(base + bhioff + 0x2c);
			bhiimgaddr_lo = ioread32(base + bhioff + 0x18);
			bhiimgaddr_hi = ioread32(base + bhioff + 0x1c);
			bhiimgsize    = ioread32(base + bhioff + 0x20);
			bhistatus     = ioread32(base + bhioff + 0x30);
			dev_info(&pdev->dev,
				 "link retrain: BHI regs: EXECENV=0x%x EXECENVVER=0x%x IMGADDR=0x%x%08x IMGSIZE=0x%x STATUS=0x%x\n",
				 bhiee, bhiever, bhiimgaddr_hi, bhiimgaddr_lo,
				 bhiimgsize, bhistatus);
		}
	}

	/*
	 * The modem is booting AMSS from internal NAND.  Wait for it to
	 * bring up its MHI endpoint and reach Mission Mode before we
	 * re-initialise the host side, otherwise MHI will try to BHI-load
	 * SBL again and fail.
	 */
	err = mhi_prepare_for_power_up(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "link retrain: failed to prepare MHI: %d\n", err);
		return;
	}

	{
		enum mhi_ee_type ee;
		int wait_ms = 10000;

		dev_info(&pdev->dev, "link retrain: waiting for modem to reach AMSS\n");
		do {
			msleep(100);
			wait_ms -= 100;
			ee = mhi_get_exec_env(mhi_cntrl);
		} while (ee != MHI_EE_AMSS && wait_ms > 0);

		if (ee != MHI_EE_AMSS) {
			dev_err(&pdev->dev,
				"link retrain: modem did not reach AMSS (EE=%d), powering up anyway\n",
				ee);
		} else {
			dev_info(&pdev->dev, "link retrain: modem reached AMSS\n");
		}
	}

	err = mhi_pci_power_up(mhi_pdev);
	if (err) {
		dev_err(&pdev->dev, "link retrain: failed to power up MHI: %d\n", err);
		mhi_unprepare_after_power_down(mhi_cntrl);
		return;
	}

	set_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status);
	dev_info(&pdev->dev, "link retrain: MHI re-initialised successfully\n");
}

/*
 * Wait up to 5 s for the modem's PCIe endpoint to answer config reads again
 * after a reset.  pci_enable_device() is retried until it succeeds because
 * the D3cold->D0 platform transition only completes once the endpoint is
 * alive.  Returns true once the endpoint responds.
 */
/*
 * Wait for the modem's config space to answer again.
 *
 * After a crash the PCIe link really is down, so the modem has to boot far
 * enough to bring its own PCIe up and re-train before anything here responds.
 * That takes considerably longer than the ~0 ms seen at cold boot, where a PON
 * warm reset only restarts the modem's compute side and leaves the link alone.
 * Being impatient here is actively harmful: the caller's fallback is to pulse
 * PERST#, which resets a modem that was merely still booting.
 */
static bool mhi_pci_wait_endpoint_timeout(struct pci_dev *pdev, int timeout_ms)
{
	bool enabled = false;
	int retries;
	u16 vendor;
	int err;

	for (retries = 0; retries < timeout_ms / 100; retries++) {
		/*
		 * Probe config space directly. Reads only need a trained link,
		 * whereas pci_enable_device() is about BAR decoding and can
		 * fail for its own reasons -- gating the probe on it would
		 * report a dark endpoint without ever having looked at one.
		 */
		vendor = 0xffff;
		pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor);
		if (vendor != 0xffff && vendor != 0x0000) {
			if (!enabled) {
				err = pci_enable_device(pdev);
				if (err) {
					dev_warn(&pdev->dev,
						 "endpoint answers (vendor %#06x) but enable failed: %d\n",
						 vendor, err);
					return false;
				}
				pci_set_master(pdev);
				enabled = true;
			}
			dev_info(&pdev->dev,
				 "modem PCIe endpoint ready after ~%d ms\n",
				 retries * 100);
			return true;
		}
		msleep(100);
	}

	/* Balance our own enable so the refcount stays sane on failure */
	if (enabled)
		pci_disable_device(pdev);

	dev_warn(&pdev->dev, "modem PCIe endpoint still dark after %d ms\n",
		 timeout_ms);

	return false;
}

/*
 * Bring MHI up on a modem whose sideband is already asserted.
 *
 * Shared by the ESOC power-on hook and by a probe that re-enumerated in the
 * middle of a power-on cycle: in both cases AP2MDM_STATUS is high and the
 * modem is waiting for the host to start talking to it.
 */
static int mhi_pci_esoc_bringup(struct mhi_pci_device *mhi_pdev)
{
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	err = mhi_prepare_for_power_up(mhi_cntrl);
	if (err)
		return err;

	if (mhi_cntrl->rddm_size)
		dev_info(&pdev->dev,
			 "RDDM buffer allocated: size=%zu seg_len=%zu image=%s\n",
			 mhi_cntrl->rddm_size, mhi_cntrl->rddm_seg_len,
			 mhi_cntrl->rddm_image ? "ok" : "MISSING");

	if (mhi_pdev->info->needs_hyp_assign) {
		err = mhi_mem_protect(mhi_cntrl);
		if (err)
			dev_warn(&pdev->dev,
				 "MHI DMA donation to modem VMID failed (%d), proceeding\n",
				 err);
	}

	err = mhi_pci_power_up(mhi_pdev);
	if (err) {
		mhi_unprepare_after_power_down(mhi_cntrl);
		return err;
	}

	set_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status);
	mhi_pci_start_msi_poll(mhi_pdev);

	/* A failed Sahara image is not a reason to reset a live host controller. */
	if (mhi_pdev->info->async_power_up && mhi_pdev->info->early_boot_recovery) {
		set_bit(MHI_PCI_DEV_LINK_RETRAIN_PENDING, &mhi_pdev->status);
		schedule_delayed_work(&mhi_pdev->link_retrain_work,
				      msecs_to_jiffies(MHI_LINK_RETRAIN_DELAY_MS));
	}

	return 0;
}

/*
 * Re-enumerate the modem after its PCIe link has been rebuilt.
 *
 * Removing and rescanning is the standard way to get a device fully
 * re-initialised: the PCI core tears down the old pci_dev and the subsequent
 * scan probes a fresh one, so MSI, the SMMU mappings and the MHI controller
 * are all built from scratch rather than patched up.
 *
 * Runs from a worker because the caller is the device's own ESOC power-on
 * callback: removing the device there would free the memory it is executing
 * against.
 */
struct mhi_pci_reprobe {
	struct work_struct work;
	struct pci_dev *pdev;
};

/* Slot currently being re-enumerated; only ever set across pci_rescan_bus(). */
static struct pci_bus *mhi_pci_reprobe_bus;
static unsigned int mhi_pci_reprobe_devfn;

/*
 * True for the one probe that the re-enumeration above is responsible for.
 * Consumed on read: a later probe of the same slot is an ordinary one.
 */
static bool mhi_pci_take_reprobe(struct pci_dev *pdev)
{
	if (pdev->bus != mhi_pci_reprobe_bus ||
	    pdev->devfn != mhi_pci_reprobe_devfn)
		return false;

	mhi_pci_reprobe_bus = NULL;
	return true;
}

static void mhi_pci_reprobe_work(struct work_struct *work)
{
	struct mhi_pci_reprobe *rp = container_of(work, struct mhi_pci_reprobe,
						  work);
	struct pci_dev *pdev = rp->pdev;
	struct pci_bus *bus = pdev->bus;
	unsigned int devfn = pdev->devfn;

	dev_info(&pdev->dev, "re-enumerating after link rebuild\n");

	pci_lock_rescan_remove();
	pci_stop_and_remove_bus_device(pdev);

	/*
	 * pci_rescan_bus() probes synchronously under this lock, so marking
	 * the slot here and clearing it right after is enough for probe to
	 * recognise its own re-enumeration without the flag ever outliving it.
	 */
	mhi_pci_reprobe_bus = bus;
	mhi_pci_reprobe_devfn = devfn;
	pci_rescan_bus(bus);
	mhi_pci_reprobe_bus = NULL;

	pci_unlock_rescan_remove();

	pci_dev_put(pdev);
	kfree(rp);
}

static void mhi_pci_schedule_reprobe(struct pci_dev *pdev)
{
	struct mhi_pci_reprobe *rp;

	rp = kzalloc(sizeof(*rp), GFP_KERNEL);
	if (!rp) {
		dev_err(&pdev->dev, "cannot schedule re-enumeration\n");
		return;
	}

	rp->pdev = pci_dev_get(pdev);
	INIT_WORK(&rp->work, mhi_pci_reprobe_work);
	schedule_work(&rp->work);
}

static int mhi_pci_esoc_power_on(void *priv, unsigned int flags)
{
	struct mhi_pci_device *mhi_pdev = priv;
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	if (test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status))
		return 0;

	dev_info(&pdev->dev, "ESOC requested MHI power on (flags=0x%x)\n", flags);

	/*
	 * Re-allow runtime PM that was forbidden in esoc_power_off to
	 * keep the device out of D3cold across the warm-reset cycle.
	 */
	pm_runtime_allow(&pdev->dev);

	/*
	 * Hold a PM reference so that autosuspend does not fire
	 * while we wait for the modem PBL to initialise.
	 */
	pm_runtime_get_noresume(&pdev->dev);

	/*
	 * The SPMI warm reset just rebooted the modem.  Before its
	 * PBL can initialise the PCIe endpoint, the host PCIe
	 * controller must be out of D3cold so the refclk is
	 * running.  Trigger runtime resume now — it may fail
	 * (D3cold→D0 verifies PMCSR which needs a live endpoint)
	 * but the platform power-up (clocks, PHY, genpd) succeeds
	 * regardless.
	 *
	 * Save whether the device was suspended before the resume
	 * attempt so we know whether the runtime-PM callback ran.
	 */
	{
		bool was_suspended = test_bit(MHI_PCI_DEV_SUSPENDED,
					      &mhi_pdev->status);

		pm_runtime_resume(&pdev->dev);

		/*
		 * The resume callback's error path queues recovery_work
		 * which would fight us.  Cancel it.
		 */
		cancel_work_sync(&mhi_pdev->recovery_work);

		if (was_suspended) {
			/*
			 * The runtime resume callback fired and cleared
			 * MHI_PCI_DEV_SUSPENDED, but pci_enable_device()
			 * inside it likely failed (endpoint not alive).
			 * The platform clocks/refclk are up though.
			 */
		} else {
			/*
			 * Device was not suspended — either first boot
			 * (still D0 from probe) or PM compiled out.
			 * Enable manually so clocks are on.
			 */
			err = pci_enable_device(pdev);
			if (!err)
				pci_set_master(pdev);
		}
	}

	/*
	 * Poll until the modem PBL boots and its PCIe endpoint responds.
	 *
	 * Only at the very first power-on is the link already up from PCI
	 * enumeration, and there this answers in ~0 ms. After any shutdown --
	 * graceful restart just as much as a crash -- the link is genuinely
	 * down and the modem has to boot far enough to bring its own PCIe back
	 * up and re-train, which takes seconds. Waiting is free when the
	 * endpoint is already alive, so be patient on every path: being
	 * impatient here just means resetting a modem that was still booting.
	 */
	if (!mhi_pci_wait_endpoint_timeout(pdev, 30000)) {
		/*
		 * Endpoint stayed dark.  As a last resort toggle PERST# to
		 * cold-reset the modem down to PBL so it re-enumerates like
		 * first boot.  That resets the endpoint's config space, so
		 * the saved BAR assignment has to be restored afterwards.
		 *
		 * This is worth trying however we got here: a graceful restart
		 * leaves the link just as dead as a crash does, and refusing to
		 * try the one recovery mechanism available simply because the
		 * modem did not crash means the port never comes back.
		 *
		 * Rebuilding the controller is what the vendor stack does here
		 * (msm_pcie_pm_control(MSM_PCIE_RESUME) followed by a fresh
		 * probe), and qcom_pcie_relink() is the mainline equivalent:
		 * the same host_deinit()/host_init() pair the controller's own
		 * resume path uses. It refuses unless the link is already
		 * down, which is what makes it safe -- tearing the resources
		 * out from under a live device is what hung the AP when this
		 * was attempted by hand via qcom_pcie_retrain_link().
		 */
		dev_info(&pdev->dev,
			 "endpoint dark after reset, rebuilding PCIe link\n");
		err = qcom_pcie_relink(pdev);
		if (err) {
			dev_err(&pdev->dev,
				"link rebuild failed: %d, falling back to PERST#\n",
				err);
			err = qcom_pcie_perst_toggle(pdev);
			if (err)
				dev_err(&pdev->dev,
					"PERST# toggle failed: %d, continuing anyway\n",
					err);
		}

		/* PERST# drops it all the way to PBL, so allow a full boot */
		if (!mhi_pci_wait_endpoint_timeout(pdev, 30000))
			goto err_no_endpoint;

		/*
		 * The link is back, but everything the endpoint learned at
		 * probe is gone with it: config space, MSI, and the DMA
		 * mappings the modem needs to fetch its own boot image. Trying
		 * to restore those one by one is a losing game -- fixing MSI
		 * just moves the failure to the next thing probe did.
		 *
		 * Do what the vendor stack does after resuming the link
		 * (msm_pcie_pm_control(MSM_PCIE_RESUME) then mhi_pci_probe):
		 * throw the device away and enumerate it again, so probe
		 * rebuilds all of it. That cannot happen from here -- we are
		 * running inside this device's own ESOC callback and the
		 * remove would free the structures underneath us -- so hand it
		 * to a worker and return.
		 */
		dev_info(&pdev->dev,
			 "link rebuilt, re-enumerating device to rebuild state\n");
		pm_runtime_put_noidle(&pdev->dev);
		mhi_pci_schedule_reprobe(pdev);
		return 0;
	}

	/* Release the extra PM reference */
	pm_runtime_put_noidle(&pdev->dev);

	return mhi_pci_esoc_bringup(mhi_pdev);

err_no_endpoint:
	dev_err(&pdev->dev, "modem PCIe endpoint did not respond after reset\n");
	pm_runtime_put_noidle(&pdev->dev);
	return -ETIMEDOUT;
}

static void mhi_pci_esoc_power_off(void *priv, unsigned int flags)
{
	struct mhi_pci_device *mhi_pdev = priv;
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	bool graceful = !(flags & ESOC_HOOK_MDM_CRASH);

	cancel_delayed_work_sync(&mhi_pdev->link_retrain_work);
	timer_delete_sync(&mhi_pdev->msi_poll_timer);
	mhi_pdev->msi_poll_enabled = false;

	if (!test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status))
		return;

	/*
	 * MDM2AP_ERRFATAL fired and ESOC is tearing the modem down. Pull the
	 * RDDM dump out via BHIe before we power the link off so we can see
	 * what the modem actually crashed on. Use the non-panic path: we run
	 * from the mdm_drv workqueue, so the panic path's udelay() polling
	 * loops (up to timeout_ms ~= 30s) would starve the CPU and trigger
	 * an RCU stall. wait_event_timeout() sleeps instead.
	 */
	if (!graceful) {
		if (mhi_cntrl->rddm_image) {
			dev_warn(&pdev->dev,
				 "ESOC reported modem crash, pulling RDDM dump\n");
			if (!mhi_download_rddm_image(mhi_cntrl, false))
				mhi_pci_collect_rddm(mhi_cntrl);
			else
				dev_warn(&pdev->dev,
					 "RDDM: BHIe download failed after ERRFATAL\n");
		} else {
			dev_warn(&pdev->dev,
				 "ESOC reported modem crash, skipping RDDM and powering down\n");
		}
	}

	mhi_power_down(mhi_cntrl, graceful);
	mhi_unprepare_after_power_down(mhi_cntrl);

	/*
	 * Prevent runtime PM autosuspend from calling pci_disable_device()
	 * while ESOC is about to warm-reset the modem.  If the device enters
	 * D3cold before the recovery reset, the D3cold→D0 transition will
	 * fail because the modem endpoint is dead — and on ARM64/DT platforms
	 * the platform power transition is a no-op, so there is no way to
	 * re-enable the link without a live endpoint.  Keeping the device in
	 * D0 across the reset cycle avoids this entirely.
	 */
	pm_runtime_forbid(&pdev->dev);
}

static int mhi_pci_register_esoc(struct mhi_pci_device *mhi_pdev)
{
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	struct esoc_desc *desc;
	int err;

	if (!pdev->dev.of_node || !of_property_present(pdev->dev.of_node, "esoc-0"))
		return 0;

	desc = devm_register_esoc_client(&pdev->dev, "mdm");
	if (IS_ERR(desc))
		return PTR_ERR(desc);
	if (!desc)
		return 0;

	mhi_pdev->esoc_client = desc;
	mhi_pdev->esoc_hook.priv = mhi_pdev;
	mhi_pdev->esoc_hook.prio = ESOC_MHI_HOOK;
	mhi_pdev->esoc_hook.esoc_link_power_on = mhi_pci_esoc_power_on;
	mhi_pdev->esoc_hook.esoc_link_power_off = mhi_pci_esoc_power_off;

	err = esoc_register_client_hook(desc, &mhi_pdev->esoc_hook);
	if (err)
		return err;

	pci_d3cold_disable(pdev);
	dev_info(&pdev->dev, "registered modem ESOC hook from DT node\n");

	return 0;
}

static void mhi_pci_apply_dt_pre_register(struct mhi_pci_device *mhi_pdev)
{
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	struct device_node *np = pdev->dev.of_node;
	struct device_node *iommu_np;
	const char *iommu_dma_type;
	u32 addr_win[2];
	bool use_s1 = true;

	if (!np)
		return;

	iommu_np = of_parse_phandle(np, "qcom,iommu-group", 0);
	if (!iommu_np)
		return;

	if (!of_property_read_string(iommu_np, "qcom,iommu-dma", &iommu_dma_type) &&
	    !strcmp(iommu_dma_type, "bypass"))
		use_s1 = false;

	if (use_s1 &&
	    !of_property_read_u32_array(iommu_np, "qcom,iommu-dma-addr-pool",
					addr_win, ARRAY_SIZE(addr_win))) {
		mhi_cntrl->iova_start = 0;
		mhi_cntrl->iova_stop = (dma_addr_t)addr_win[0] + addr_win[1];
		dev_info(&pdev->dev,
			 "using DT IOVA window start=0x%llx stop=0x%llx\n",
			 (unsigned long long)mhi_cntrl->iova_start,
			 (unsigned long long)mhi_cntrl->iova_stop);
	}

	of_node_put(iommu_np);
}

static void mhi_pci_apply_dt_post_register(struct mhi_pci_device *mhi_pdev)
{
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	struct device_node *np = pdev->dev.of_node;
	u32 val;

	if (!np)
		return;

	if (!of_property_read_u32(np, "mhi,timeout", &val)) {
		mhi_cntrl->timeout_ms = val;
		dev_info(&pdev->dev, "using DT MHI timeout %u ms\n", val);
	}

	if (!of_property_read_u32(np, "mhi,buffer-len", &val)) {
		mhi_cntrl->buffer_len = val;
		dev_info(&pdev->dev, "using DT MHI buffer length 0x%x\n", val);
	}
}

static void mhi_pci_recovery_work(struct work_struct *work)
{
	struct mhi_pci_device *mhi_pdev = container_of(work, struct mhi_pci_device,
						       recovery_work);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	dev_warn(&pdev->dev, "device recovery started\n");

	if (!mhi_pdev->info->early_boot_recovery) {
		dev_info(&pdev->dev, "skipping generic recovery for this controller\n");
		return;
	}

	timer_delete(&mhi_pdev->health_check_timer);
	pm_runtime_forbid(&pdev->dev);

	/* Clean up MHI state */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, false);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	pci_set_power_state(pdev, PCI_D0);
	pci_load_saved_state(pdev, mhi_pdev->pci_state);
	pci_restore_state(pdev);

	if (!mhi_pci_is_alive(mhi_cntrl))
		goto err_try_reset;

	err = mhi_prepare_for_power_up(mhi_cntrl);
	if (err)
		goto err_try_reset;

	if (mhi_pdev->info->needs_hyp_assign) {
		err = mhi_mem_protect(mhi_cntrl);
		if (err)
			dev_warn(&pdev->dev,
				 "MHI DMA donation to modem VMID failed (%d), proceeding\n",
				 err);
	}

	err = mhi_pci_power_up(mhi_pdev);
	if (err)
		goto err_unprepare;

	dev_dbg(&pdev->dev, "Recovery completed\n");

	set_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status);
	mhi_pci_start_msi_poll(mhi_pdev);
	mod_timer(&mhi_pdev->health_check_timer, jiffies + HEALTH_CHECK_PERIOD);
	return;

err_unprepare:
	mhi_unprepare_after_power_down(mhi_cntrl);
err_try_reset:
	err = pci_try_reset_function(pdev);
	if (err)
		dev_err(&pdev->dev, "Recovery failed: %d\n", err);
}

static void health_check(struct timer_list *t)
{
	struct mhi_pci_device *mhi_pdev = timer_container_of(mhi_pdev, t,
							     health_check_timer);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	if (!test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status) ||
			test_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status))
		return;

	if (!mhi_pci_is_alive(mhi_cntrl)) {
		dev_err(mhi_cntrl->cntrl_dev, "Device died\n");
		if (mhi_pdev->info->early_boot_recovery)
			queue_work(system_long_wq, &mhi_pdev->recovery_work);
		else
			dev_info(mhi_cntrl->cntrl_dev,
				 "Skipping generic recovery for transient modem disappearance\n");
		return;
	}

	/* reschedule in two seconds */
	mod_timer(&mhi_pdev->health_check_timer, jiffies + HEALTH_CHECK_PERIOD);
}

static void mhi_pci_msi_poll(struct timer_list *t)
{
	struct mhi_pci_device *mhi_pdev = timer_container_of(mhi_pdev, t,
							     msi_poll_timer);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	enum mhi_state hw_state;

	if (!mhi_pdev->msi_poll_enabled)
		return;

	/* Do not keep reading endpoint BARs after the endpoint disappears.
	 * In particular, never race a root-complex clock/PHY teardown. */
	if (!mhi_pci_is_alive(mhi_cntrl)) {
		mhi_pdev->msi_poll_enabled = false;
		mhi_pdev->msi_poll_saw_m0 = false;
		dev_warn(mhi_cntrl->cntrl_dev,
			 "modem endpoint disappeared; stopping MSI polling without automatic recovery\n");
		return;
	}

	/* Always reschedule first — ensures the timer never silently dies */
	mod_timer(&mhi_pdev->msi_poll_timer,
		  jiffies + msecs_to_jiffies(MHI_MSI_POLL_INTERVAL_MS));

	/*
	 * Check HW state BEFORE the STARTED gate — MHI core error handling
	 * may clear STARTED when it detects stale state, but we still need
	 * to detect modem reboot (M0->RESET) to trigger recovery.
	 */
	hw_state = mhi_get_mhi_state(mhi_cntrl);

	if (hw_state == MHI_STATE_M0)
		mhi_pdev->msi_poll_saw_m0 = true;

	if (hw_state == MHI_STATE_RESET && mhi_pdev->msi_poll_saw_m0) {
		dev_info(mhi_cntrl->cntrl_dev,
			 "MHI M0->RESET detected by poll timer, scheduling recovery\n");
		mhi_pdev->msi_poll_enabled = false;
		mhi_pdev->msi_poll_saw_m0 = false;
		queue_work(system_long_wq, &mhi_pdev->recovery_work);
		return;
	}

	if (!test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status) ||
	    test_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status))
		return;

	mhi_poll_events(mhi_cntrl);
}

static void mhi_pci_start_msi_poll(struct mhi_pci_device *mhi_pdev)
{
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct iommu_domain *domain;

	domain = iommu_get_domain_for_dev(mhi_cntrl->cntrl_dev);
	if (!domain || domain->type == IOMMU_DOMAIN_IDENTITY ||
	    domain->type == IOMMU_DOMAIN_BLOCKED) {
		dev_dbg(mhi_cntrl->cntrl_dev,
			"IOMMU domain type %d, MSI polling not needed\n",
			domain ? domain->type : -1);
		return;
	}

	dev_info(mhi_cntrl->cntrl_dev,
		 "IOMMU DMA domain detected (type %d), enabling MSI poll timer (%d ms)\n",
		 domain->type, MHI_MSI_POLL_INTERVAL_MS);
	mhi_pdev->msi_poll_enabled = true;
	mhi_pdev->msi_poll_saw_m0 = false;
	mod_timer(&mhi_pdev->msi_poll_timer,
		  jiffies + msecs_to_jiffies(MHI_MSI_POLL_INTERVAL_MS));
}

static int mhi_pci_generic_edl_trigger(struct mhi_controller *mhi_cntrl)
{
	void __iomem *base = mhi_cntrl->regs;
	void __iomem *edl_db;
	int ret;
	u32 val;

	ret = mhi_device_get_sync(mhi_cntrl->mhi_dev);
	if (ret) {
		dev_err(mhi_cntrl->cntrl_dev, "Failed to wakeup the device\n");
		return ret;
	}

	pm_wakeup_event(&mhi_cntrl->mhi_dev->dev, 0);
	mhi_cntrl->runtime_get(mhi_cntrl, mhi_cntrl->priv_data);

	ret = mhi_get_channel_doorbell_offset(mhi_cntrl, &val);
	if (ret)
		goto err_get_chdb;

	edl_db = base + val + (8 * MHI_EDL_DB);

	mhi_cntrl->write_reg(mhi_cntrl, edl_db + 4, upper_32_bits(MHI_EDL_COOKIE));
	mhi_cntrl->write_reg(mhi_cntrl, edl_db, lower_32_bits(MHI_EDL_COOKIE));

	mhi_soc_reset(mhi_cntrl);

err_get_chdb:
	mhi_cntrl->runtime_put(mhi_cntrl, mhi_cntrl->priv_data);
	mhi_device_put(mhi_cntrl->mhi_dev);

	return ret;
}

static int mhi_pci_power_up(struct mhi_pci_device *mhi_pdev)
{
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	struct pci_dev *pdev = to_pci_dev(mhi_cntrl->cntrl_dev);
	int err;

	if (mhi_pdev->info->async_power_up) {
		err = mhi_async_power_up(mhi_cntrl);
		if (!err)
			dev_info(&pdev->dev, "MHI controller started asynchronously\n");
		return err;
	}

	return mhi_sync_power_up(mhi_cntrl);
}

static int mhi_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	const struct mhi_pci_dev_info *info = (struct mhi_pci_dev_info *) id->driver_data;
	const struct mhi_controller_config *mhi_cntrl_config;
	struct mhi_pci_device *mhi_pdev;
	struct mhi_controller *mhi_cntrl;
	int err;

	if (info == &mhi_foxconn_sdx55_info &&
	    of_property_present(pdev->dev.of_node, "esoc-0")) {
		if (!IS_REACHABLE(CONFIG_QCOM_ESOC_CLIENT) ||
		    !IS_REACHABLE(CONFIG_PCIE_QCOM))
			return dev_err_probe(&pdev->dev, -EOPNOTSUPP,
					     "fusion modem requires ESOC and Qualcomm PCIe\n");
		info = &mhi_qcom_sdx55_fusion_info;
	}

	dev_info(&pdev->dev, "MHI PCI device found: %s\n", info->name);

	/* mhi_pdev.mhi_cntrl must be zero-initialized */
	mhi_pdev = devm_kzalloc(&pdev->dev, sizeof(*mhi_pdev), GFP_KERNEL);
	if (!mhi_pdev)
		return -ENOMEM;

	INIT_WORK(&mhi_pdev->recovery_work, mhi_pci_recovery_work);
	INIT_DELAYED_WORK(&mhi_pdev->link_retrain_work, mhi_pci_link_retrain_work);
	INIT_DELAYED_WORK(&mhi_pdev->pm_probe_work, mhi_pci_pm_probe_work);
	timer_setup(&mhi_pdev->health_check_timer, health_check, 0);
	timer_setup(&mhi_pdev->msi_poll_timer, mhi_pci_msi_poll, 0);

	mhi_cntrl_config = info->config;
	mhi_cntrl = &mhi_pdev->mhi_cntrl;
	mhi_pdev->info = info;

	mhi_cntrl->cntrl_dev = &pdev->dev;
	if (info->iova_stop) {
		mhi_cntrl->iova_start = info->iova_start;
		mhi_cntrl->iova_stop = info->iova_stop;
	} else {
		mhi_cntrl->iova_start = 0;
		mhi_cntrl->iova_stop = (dma_addr_t)DMA_BIT_MASK(info->dma_data_width);
	}
	mhi_pci_apply_dt_pre_register(mhi_pdev);
	mhi_cntrl->fw_image = info->fw;
	mhi_cntrl->edl_image = info->edl;
	mhi_cntrl->amss_image = info->amss_fw;

	mhi_cntrl->read_reg = mhi_pci_read_reg;
	mhi_cntrl->write_reg = mhi_pci_write_reg;
	mhi_cntrl->status_cb = mhi_pci_status_cb;
	mhi_cntrl->runtime_get = mhi_pci_runtime_get;
	mhi_cntrl->runtime_put = mhi_pci_runtime_put;
	mhi_cntrl->time_get = mhi_pci_time_get;
	mhi_cntrl->lpm_disable = mhi_pci_lpm_disable;
	mhi_cntrl->lpm_enable = mhi_pci_lpm_enable;
	mhi_cntrl->bw_scale = mhi_pci_bw_scale;
	mhi_cntrl->priv_data = mhi_pdev;
	mhi_cntrl->mru = info->mru_default;
	mhi_cntrl->name = info->name;
	mhi_cntrl->rddm_size = info->rddm_size;
	mhi_cntrl->rddm_seg_len = info->rddm_seg_len;

	if (info->edl_trigger)
		mhi_cntrl->edl_trigger = mhi_pci_generic_edl_trigger;

	if (info->sideband_wake) {
		mhi_cntrl->wake_get = mhi_pci_wake_get_nop;
		mhi_cntrl->wake_put = mhi_pci_wake_put_nop;
		mhi_cntrl->wake_toggle = mhi_pci_wake_toggle_nop;
	}

	err = mhi_pci_claim(mhi_cntrl, info->bar_num, DMA_BIT_MASK(info->dma_data_width));
	if (err)
		return err;

	/*
	 * Constrain the DMA mask to match the modem's IOVA window.
	 * The IOMMU DMA allocator uses the DMA mask to bound IOVA
	 * allocation.  Without this, a 64-bit mask lets the allocator
	 * place buffers at ~0xffffe000, far above the modem's
	 * [iova_start, iova_stop] window, causing the modem to crash.
	 */
	if (info->iova_stop) {
		err = dma_set_mask_and_coherent(&pdev->dev, info->iova_stop);
		if (err) {
			dev_err(&pdev->dev, "Cannot set DMA mask to 0x%llx: %d\n",
				(unsigned long long)info->iova_stop, err);
			return err;
		}
		dev_info(&pdev->dev, "DMA mask constrained to 0x%llx for modem IOVA window\n",
			 (unsigned long long)info->iova_stop);
	}

	/*
	 * NOTE: A previous 2MB identity mapping at 0xaf800000 was removed
	 * (it was added for SBL DMA faults, which are now fixed by the DMA
	 * mask constraint above).  A targeted per-page MSI identity mapping
	 * is created later, after mhi_pci_get_irqs() programs the EP's MSI
	 * capability registers with the iMSI-RX target address.
	 */

	if (info->disable_pm || info->no_link_pm) {
		pci_disable_link_state(pdev, PCIE_LINK_STATE_ALL);
		dev_info(&pdev->dev, "disabled PCIe link power saving\n");
	}
	if (info->disable_pm) {
		pm_runtime_forbid(&pdev->dev);
		dev_info(&pdev->dev, "forbade runtime PM (disable_pm)\n");
	}

	err = mhi_pci_get_irqs(mhi_cntrl, mhi_cntrl_config);
	if (err)
		return err;

	/*
	 * On Qualcomm SM8250, the SMMU TBU sits between the PCIe endpoint
	 * and the DW PCIe iMSI-RX module.  When the endpoint's IOMMU domain
	 * is DMA-translated (not identity), MSI TLPs carry an address that
	 * the SMMU has no mapping for, causing translation faults.  Create
	 * an identity mapping for the MSI target page so the SMMU passes
	 * the MSI write through to the iMSI-RX address comparator.
	 */
	mhi_pci_map_msi_page(pdev);

	pci_set_drvdata(pdev, mhi_pdev);

	/* Have stored pci confspace at hand for restore in sudden PCI error.
	 * cache the state locally and discard the PCI core one.
	 */
	pci_save_state(pdev);
	mhi_pdev->pci_state = pci_store_saved_state(pdev);
	pci_load_saved_state(pdev, NULL);

	err = mhi_register_controller(mhi_cntrl, mhi_cntrl_config);
	if (err)
		return err;

	mhi_pci_apply_dt_post_register(mhi_pdev);

	err = mhi_pci_register_esoc(mhi_pdev);
	if (err)
		goto err_unregister;

	/*
	 * When an ESOC client is registered the modem sideband (AP2MDM_STATUS)
	 * is controlled by the ESOC/remoteproc driver.  Powering up MHI here
	 * (before AP2MDM_STATUS=1) causes BHI to fail: the modem PBL rejects
	 * the firmware upload (error 0xef120700) because AP2MDM_STATUS is still
	 * low.  Defer the entire prepare+power_up sequence to the ESOC
	 * esoc_link_power_on hook, which fires after ESOC_PWR_ON has driven
	 * AP2MDM_STATUS high.
	 */
	if (mhi_pdev->esoc_client) {
		/*
		 * A re-enumeration triggered from the power-on hook is the
		 * exception: that hook has already run for this cycle and will
		 * not run again, so nothing else would ever start us.
		 */
		if (mhi_pci_take_reprobe(pdev)) {
			dev_info(&pdev->dev,
				 "re-probed during ESOC power-on, starting MHI now\n");
			err = mhi_pci_esoc_bringup(mhi_pdev);
			if (err)
				goto err_unregister;
			return 0;
		}

		dev_info(&pdev->dev,
			 "ESOC client registered, deferring MHI power-up to ESOC hook\n");
		return 0;
	}

	/* MHI bus does not power up the controller by default */
	err = mhi_prepare_for_power_up(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "failed to prepare MHI controller\n");
		goto err_unregister;
	}

	/*
	 * On SM8250 with external SDX55M on PCIe2, TZ stage-2 SMMU (CB=12,
	 * SID=0x1d01) may block modem DMA writes to host memory.  Attempt to
	 * donate MHI control-path DMA buffers to VMID_MSS_MSA.  Non-fatal:
	 * the call fails with -EINVAL on platforms where the stage-2 SMMU
	 * protection is not actually enabled and the modem works without it.
	 */
	if (info->needs_hyp_assign) {
		err = mhi_mem_protect(mhi_cntrl);
		if (err)
			dev_warn(&pdev->dev,
				 "MHI DMA donation to modem VMID failed (%d), proceeding\n",
				 err);
	}

	err = mhi_pci_power_up(mhi_pdev);
	if (err) {
		dev_err(&pdev->dev, "failed to power up MHI controller\n");
		goto err_unprepare;
	}

	set_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status);
	mhi_pci_start_msi_poll(mhi_pdev);

	/* start health check */
	if (info->early_boot_recovery)
		mod_timer(&mhi_pdev->health_check_timer, jiffies + HEALTH_CHECK_PERIOD);

	/* Allow runtime suspend only if both PME from D3Hot and M3 are supported */
	if (!info->disable_pm &&
	    pci_pme_capable(pdev, PCI_D3hot) && !(info->no_m3)) {
		pm_runtime_set_autosuspend_delay(&pdev->dev, 2000);
		pm_runtime_use_autosuspend(&pdev->dev);
		pm_runtime_mark_last_busy(&pdev->dev);
		pm_runtime_put_noidle(&pdev->dev);
	}

	return 0;

err_unprepare:
	mhi_unprepare_after_power_down(mhi_cntrl);
err_unregister:
	if (mhi_pdev->esoc_client)
		esoc_unregister_client_hook(mhi_pdev->esoc_client, &mhi_pdev->esoc_hook);
	mhi_unregister_controller(mhi_cntrl);

	return err;
}

static void mhi_pci_remove(struct pci_dev *pdev)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	timer_delete_sync(&mhi_pdev->health_check_timer);
	timer_delete_sync(&mhi_pdev->msi_poll_timer);
	cancel_work_sync(&mhi_pdev->recovery_work);
	cancel_delayed_work_sync(&mhi_pdev->link_retrain_work);
	cancel_delayed_work_sync(&mhi_pdev->pm_probe_work);

	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, true);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	/* Balance the usage_count decrement from probe/mission-mode.
	 * Non-ESOC: pm_runtime_put_noidle at probe end.
	 * ESOC: pm_runtime_allow at mission-mode entry.
	 */
	if (test_bit(MHI_PCI_DEV_AUTOSUSPEND_SET, &mhi_pdev->status))
		pm_runtime_forbid(&pdev->dev);
	else if (pci_pme_capable(pdev, PCI_D3hot))
		pm_runtime_get_noresume(&pdev->dev);

	if (mhi_pdev->esoc_client)
		esoc_unregister_client_hook(mhi_pdev->esoc_client, &mhi_pdev->esoc_hook);

	mhi_unregister_controller(mhi_cntrl);
}

static void mhi_pci_shutdown(struct pci_dev *pdev)
{
	mhi_pci_remove(pdev);
	pci_set_power_state(pdev, PCI_D3hot);
}

static void mhi_pci_reset_prepare(struct pci_dev *pdev)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	dev_info(&pdev->dev, "reset\n");

	timer_delete(&mhi_pdev->health_check_timer);

	/* Clean up MHI state */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, false);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	/* cause internal device reset */
	mhi_soc_reset(mhi_cntrl);

	/* Be sure device reset has been executed */
	msleep(MHI_POST_RESET_DELAY_MS);
}

static void mhi_pci_reset_done(struct pci_dev *pdev)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	int err;

	/* Restore initial known working PCI state */
	pci_load_saved_state(pdev, mhi_pdev->pci_state);
	pci_restore_state(pdev);

	/* Is device status available ? */
	if (!mhi_pci_is_alive(mhi_cntrl)) {
		dev_err(&pdev->dev, "reset failed\n");
		return;
	}

	err = mhi_prepare_for_power_up(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "failed to prepare MHI controller\n");
		return;
	}

	if (mhi_pdev->info->needs_hyp_assign) {
		err = mhi_mem_protect(mhi_cntrl);
		if (err)
			dev_warn(&pdev->dev,
				 "MHI DMA donation to modem VMID failed (%d), proceeding\n",
				 err);
	}

	err = mhi_pci_power_up(mhi_pdev);
	if (err) {
		dev_err(&pdev->dev, "failed to power up MHI controller\n");
		mhi_unprepare_after_power_down(mhi_cntrl);
		return;
	}

	set_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status);
	mhi_pci_start_msi_poll(mhi_pdev);
	if (mhi_pdev->info->early_boot_recovery)
		mod_timer(&mhi_pdev->health_check_timer, jiffies + HEALTH_CHECK_PERIOD);
}

static pci_ers_result_t mhi_pci_error_detected(struct pci_dev *pdev,
					       pci_channel_state_t state)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	dev_err(&pdev->dev, "PCI error detected, state = %u\n", state);

	if (state == pci_channel_io_perm_failure)
		return PCI_ERS_RESULT_DISCONNECT;

	/*
	 * For fusion modems (async_power_up), the PCIe link drop during
	 * SBL→AMSS transition is expected.  Prevent the error recovery from
	 * doing a secondary bus reset which would cold-reset the modem back
	 * to PBL.  The link_retrain_work will handle recovery.
	 */
	if (test_bit(MHI_PCI_DEV_LINK_RETRAIN_PENDING, &mhi_pdev->status)) {
		dev_info(&pdev->dev, "link retrain pending, skipping PCI error recovery\n");
		return PCI_ERS_RESULT_RECOVERED;
	}

	/* Clean up MHI state */
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, false);
		mhi_unprepare_after_power_down(mhi_cntrl);
	} else {
		/* Nothing to do */
		return PCI_ERS_RESULT_RECOVERED;
	}

	pci_disable_device(pdev);

	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t mhi_pci_slot_reset(struct pci_dev *pdev)
{
	if (pci_enable_device(pdev)) {
		dev_err(&pdev->dev, "Cannot re-enable PCI device after reset.\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}

	return PCI_ERS_RESULT_RECOVERED;
}

static void mhi_pci_io_resume(struct pci_dev *pdev)
{
	struct mhi_pci_device *mhi_pdev = pci_get_drvdata(pdev);

	dev_err(&pdev->dev, "PCI slot reset done\n");

	queue_work(system_long_wq, &mhi_pdev->recovery_work);
}

static const struct pci_error_handlers mhi_pci_err_handler = {
	.error_detected = mhi_pci_error_detected,
	.slot_reset = mhi_pci_slot_reset,
	.resume = mhi_pci_io_resume,
	.reset_prepare = mhi_pci_reset_prepare,
	.reset_done = mhi_pci_reset_done,
};

static int  __maybe_unused mhi_pci_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct mhi_pci_device *mhi_pdev = dev_get_drvdata(dev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	int err;

	dev_info(&pdev->dev,
		 "mhi_pci_runtime_suspend: ENTER usage=%d ee=0x%x started=%d\n",
		 atomic_read(&dev->power.usage_count),
		 (unsigned int)mhi_cntrl->ee,
		 test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status));

	/*
	 * Runtime PM is already forbidden for the SDX55 fusion once it reaches
	 * mission mode, but system suspend (s2idle) lands here directly and
	 * would still attempt the M3 transition. The modem never acknowledges
	 * it: the host blocks for 2 s, mhi_pm_suspend() fails with -EBUSY, and
	 * the aborted attempt leaves the link half-transitioned, so the retry
	 * a moment later reports "Could not enter M0/M1". Observed fallout is
	 * a fatal exception in the sensor DSP within 40 ms of the retry, an
	 * slpi crash/recovery loop that pins rproc_crash_handler_work on the
	 * freezable rproc_recovery_wq (every later suspend then dies in the
	 * freezer after 20 s), and eventually MDM2AP_ERRFATAL on the modem.
	 *
	 * There is nothing to gain by suspending it anyway: this modem is a
	 * separate chip on its own PMIC, so just leave it in M0/D0 across
	 * s2idle. Returning before MHI_PCI_DEV_SUSPENDED is set keeps the pair
	 * symmetric, as mhi_pci_runtime_resume() returns early when it is clear.
	 */
	if (mhi_pdev->info == &mhi_qcom_sdx55_fusion_info) {
		/*
		 * Saving the state here is what keeps the modem powered: the
		 * PCI core only calls pci_prepare_to_sleep() (D3hot) when the
		 * driver has not saved state itself. Without this the device
		 * is powered down behind our backs, the link does not come
		 * back, and the modem is lost on the first system suspend.
		 */
		pci_save_state(pdev);
		dev_dbg(&pdev->dev, "fusion modem stays in M0/D0 across suspend\n");
		return 0;
	}

	if (test_and_set_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status))
		return 0;

	timer_delete(&mhi_pdev->health_check_timer);
	cancel_work_sync(&mhi_pdev->recovery_work);

	if (!test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status) ||
			mhi_cntrl->ee != MHI_EE_AMSS)
		goto pci_suspend; /* Nothing to do at MHI level */

	/* Transition to M3 state */
	err = mhi_pm_suspend(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "failed to suspend device: %d\n", err);
		clear_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status);
		return -EBUSY;
	}

	dev_info(&pdev->dev, "mhi_pci_runtime_suspend: M3 OK\n");

pci_suspend:
	pci_disable_device(pdev);
	pci_wake_from_d3(pdev, true);

	return 0;
}

static int __maybe_unused mhi_pci_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct mhi_pci_device *mhi_pdev = dev_get_drvdata(dev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;
	int err;

	if (!test_and_clear_bit(MHI_PCI_DEV_SUSPENDED, &mhi_pdev->status))
		return 0;

	err = pci_enable_device(pdev);
	if (err)
		goto err_recovery;

	pci_set_master(pdev);
	pci_wake_from_d3(pdev, false);

	if (!test_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status) ||
			mhi_cntrl->ee != MHI_EE_AMSS)
		return 0; /* Nothing to do at MHI level */

	/* Exit M3, transition to M0 state */
	err = mhi_pm_resume(mhi_cntrl);
	if (err) {
		dev_err(&pdev->dev, "failed to resume device: %d\n", err);
		goto err_recovery;
	}

	/* Resume health check */
	mod_timer(&mhi_pdev->health_check_timer, jiffies + HEALTH_CHECK_PERIOD);

	/* It can be a remote wakeup (no mhi runtime_get), update access time */
	pm_runtime_mark_last_busy(dev);

	return 0;

err_recovery:
	/* Do not fail to not mess up our PCI device state, the device likely
	 * lost power (d3cold) and we simply need to reset it from the recovery
	 * procedure, trigger the recovery asynchronously to prevent system
	 * suspend exit delaying.
	 */
	queue_work(system_long_wq, &mhi_pdev->recovery_work);
	pm_runtime_mark_last_busy(dev);

	return 0;
}

static int  __maybe_unused mhi_pci_suspend(struct device *dev)
{
	pm_runtime_disable(dev);
	return mhi_pci_runtime_suspend(dev);
}

static int __maybe_unused mhi_pci_resume(struct device *dev)
{
	int ret;

	/* Depending the platform, device may have lost power (d3cold), we need
	 * to resume it now to check its state and recover when necessary.
	 */
	ret = mhi_pci_runtime_resume(dev);
	pm_runtime_enable(dev);

	return ret;
}

static int __maybe_unused mhi_pci_freeze(struct device *dev)
{
	struct mhi_pci_device *mhi_pdev = dev_get_drvdata(dev);
	struct mhi_controller *mhi_cntrl = &mhi_pdev->mhi_cntrl;

	/* We want to stop all operations, hibernation does not guarantee that
	 * device will be in the same state as before freezing, especially if
	 * the intermediate restore kernel reinitializes MHI device with new
	 * context.
	 */
	flush_work(&mhi_pdev->recovery_work);
	if (test_and_clear_bit(MHI_PCI_DEV_STARTED, &mhi_pdev->status)) {
		mhi_power_down(mhi_cntrl, true);
		mhi_unprepare_after_power_down(mhi_cntrl);
	}

	return 0;
}

static int __maybe_unused mhi_pci_restore(struct device *dev)
{
	struct mhi_pci_device *mhi_pdev = dev_get_drvdata(dev);

	/* Reinitialize the device */
	queue_work(system_long_wq, &mhi_pdev->recovery_work);

	return 0;
}

static const struct dev_pm_ops mhi_pci_pm_ops = {
	SET_RUNTIME_PM_OPS(mhi_pci_runtime_suspend, mhi_pci_runtime_resume, NULL)
#ifdef CONFIG_PM_SLEEP
	.suspend = mhi_pci_suspend,
	.resume = mhi_pci_resume,
	.freeze = mhi_pci_freeze,
	.thaw = mhi_pci_restore,
	.poweroff = mhi_pci_freeze,
	.restore = mhi_pci_restore,
#endif
};

static struct pci_driver mhi_pci_driver = {
	.name		= "mhi-pci-generic",
	.id_table	= mhi_pci_id_table,
	.probe		= mhi_pci_probe,
	.remove		= mhi_pci_remove,
	.shutdown	= mhi_pci_shutdown,
	.err_handler	= &mhi_pci_err_handler,
	.driver.pm	= &mhi_pci_pm_ops
};
module_pci_driver(mhi_pci_driver);

MODULE_AUTHOR("Loic Poulain <loic.poulain@linaro.org>");
MODULE_DESCRIPTION("Modem Host Interface (MHI) PCI controller driver");
MODULE_LICENSE("GPL");
