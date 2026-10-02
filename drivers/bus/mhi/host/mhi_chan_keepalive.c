// SPDX-License-Identifier: GPL-2.0
/*
 * MHI channel keep-alive client for Qualcomm SDX55 Fusion modem.
 *
 * Hypothesis test: vendor DT exposes EFS/QMI1/IP_CTRL channels in AMSS
 * EE.  Mainline declares them in the channel config but no client
 * driver binds, so the channels are never STARTed.  Modem firmware
 * tasks waiting on host-side ring setup may starve and trigger the
 * per-task DOG watchdog at MISSION+15s (see ~/deepseek-findings.txt).
 *
 * This client opens each channel, drains DL traffic, and never queues
 * UL.  If the +15s ERRFATAL changes timing or disappears, channel
 * completeness was the gap.
 */

#include <linux/device.h>
#include <linux/mhi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/slab.h>

#define MHI_KEEPALIVE_RX_SIZE 0x1000

struct mhi_keepalive_dev {
	struct mhi_device *mhi_dev;
	u8 *rx_buf;
};

static int mhi_keepalive_queue_rx(struct mhi_keepalive_dev *kdev)
{
	return mhi_queue_buf(kdev->mhi_dev, DMA_FROM_DEVICE, kdev->rx_buf,
			     MHI_KEEPALIVE_RX_SIZE, MHI_EOT);
}

static int mhi_keepalive_probe(struct mhi_device *mhi_dev,
			       const struct mhi_device_id *id)
{
	struct mhi_keepalive_dev *kdev;
	int ret;

	kdev = devm_kzalloc(&mhi_dev->dev, sizeof(*kdev), GFP_KERNEL);
	if (!kdev)
		return -ENOMEM;

	kdev->rx_buf = devm_kzalloc(&mhi_dev->dev, MHI_KEEPALIVE_RX_SIZE,
				    GFP_KERNEL);
	if (!kdev->rx_buf)
		return -ENOMEM;

	kdev->mhi_dev = mhi_dev;
	dev_set_drvdata(&mhi_dev->dev, kdev);

	ret = mhi_prepare_for_transfer(mhi_dev);
	if (ret)
		return ret;

	ret = mhi_keepalive_queue_rx(kdev);
	if (ret) {
		mhi_unprepare_from_transfer(mhi_dev);
		return ret;
	}

	dev_info(&mhi_dev->dev, "keepalive: %s channel opened\n", id->chan);
	return 0;
}

static void mhi_keepalive_remove(struct mhi_device *mhi_dev)
{
	mhi_unprepare_from_transfer(mhi_dev);
}

static void mhi_keepalive_dl_xfer_cb(struct mhi_device *mhi_dev,
				     struct mhi_result *result)
{
	struct mhi_keepalive_dev *kdev = dev_get_drvdata(&mhi_dev->dev);

	if (result->transaction_status)
		return;

	if (mhi_keepalive_queue_rx(kdev))
		dev_err(&mhi_dev->dev, "keepalive: failed to requeue RX\n");
}

static void mhi_keepalive_ul_xfer_cb(struct mhi_device *mhi_dev,
				     struct mhi_result *result)
{
}

/*
 * EFS is deliberately absent here: the SDX55 firmware is built RMTEFS
 * ("MPSS...SDX55_RMTEFS_PACK..."), i.e. it expects the AP to host its
 * filesystem. Merely holding the channel open is not enough - it has to be
 * serviced. mhi_wwan_ctrl claims EFS instead and exposes /dev/wwan0efs0 so
 * an EFS sync daemon can implement the protocol in userspace, the way
 * Android's /vendor/bin/ks does on MHI channel 10.
 */
static const struct mhi_device_id mhi_keepalive_match_table[] = {
	{ .chan = "QMI1" },
	{ .chan = "IP_CTRL" },
	{}
};
MODULE_DEVICE_TABLE(mhi, mhi_keepalive_match_table);

static struct mhi_driver mhi_keepalive_driver = {
	.id_table = mhi_keepalive_match_table,
	.probe = mhi_keepalive_probe,
	.remove = mhi_keepalive_remove,
	.ul_xfer_cb = mhi_keepalive_ul_xfer_cb,
	.dl_xfer_cb = mhi_keepalive_dl_xfer_cb,
	.driver = {
		.name = "mhi_chan_keepalive",
	},
};
module_mhi_driver(mhi_keepalive_driver);

MODULE_DESCRIPTION("MHI channel keep-alive for SDX55 Fusion control channels");
MODULE_LICENSE("GPL");
