// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal MHI boot logger client for Qualcomm modem SBL boot messages.
 *
 * Some SDX55 fusion setups expose a DL-only "BL" channel in SBL EE in
 * parallel with SAHARA. Vendor kernels bind a small consumer to drain that
 * traffic. Keep the channel serviced even if the messages themselves are only
 * used for debug.
 */

#include <linux/device.h>
#include <linux/mhi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/slab.h>

#define MHI_BL_RX_SIZE 0x1000

struct mhi_bl_dev {
	struct mhi_device *mhi_dev;
	u8 *rx_buf;
};

static int mhi_bl_queue_rx(struct mhi_bl_dev *bdev)
{
	return mhi_queue_buf(bdev->mhi_dev, DMA_FROM_DEVICE, bdev->rx_buf,
			     MHI_BL_RX_SIZE, MHI_EOT);
}

static int mhi_bl_probe(struct mhi_device *mhi_dev,
			const struct mhi_device_id *id)
{
	struct mhi_bl_dev *bdev;
	int ret;

	bdev = devm_kzalloc(&mhi_dev->dev, sizeof(*bdev), GFP_KERNEL);
	if (!bdev)
		return -ENOMEM;

	bdev->rx_buf = devm_kzalloc(&mhi_dev->dev, MHI_BL_RX_SIZE, GFP_KERNEL);
	if (!bdev->rx_buf)
		return -ENOMEM;

	bdev->mhi_dev = mhi_dev;
	dev_set_drvdata(&mhi_dev->dev, bdev);

	ret = mhi_prepare_for_transfer(mhi_dev);
	if (ret)
		return ret;

	ret = mhi_bl_queue_rx(bdev);
	if (ret) {
		mhi_unprepare_from_transfer(mhi_dev);
		return ret;
	}

	dev_info(&mhi_dev->dev, "BL: boot logger channel ready\n");
	return 0;
}

static void mhi_bl_remove(struct mhi_device *mhi_dev)
{
	mhi_unprepare_from_transfer(mhi_dev);
}

static void mhi_bl_dl_xfer_cb(struct mhi_device *mhi_dev,
			      struct mhi_result *result)
{
	struct mhi_bl_dev *bdev = dev_get_drvdata(&mhi_dev->dev);
	size_t n;

	if (result->transaction_status || !result->bytes_xferd)
		return;

	n = min_t(size_t, result->bytes_xferd, MHI_BL_RX_SIZE - 1);
	bdev->rx_buf[n] = '\0';

	/* SBL boot logs are best-effort debug; keep them short in dmesg. */
	dev_info(&mhi_dev->dev, "BL: %s\n", bdev->rx_buf);

	if (mhi_bl_queue_rx(bdev))
		dev_err(&mhi_dev->dev, "BL: failed to requeue RX buffer\n");
}

static void mhi_bl_ul_xfer_cb(struct mhi_device *mhi_dev,
			      struct mhi_result *result)
{
}

static const struct mhi_device_id mhi_bl_match_table[] = {
	{ .chan = "BL" },
	{}
};
MODULE_DEVICE_TABLE(mhi, mhi_bl_match_table);

static struct mhi_driver mhi_bl_driver = {
	.id_table = mhi_bl_match_table,
	.probe = mhi_bl_probe,
	.remove = mhi_bl_remove,
	.ul_xfer_cb = mhi_bl_ul_xfer_cb,
	.dl_xfer_cb = mhi_bl_dl_xfer_cb,
	.driver = {
		.name = "mhi_bl_logger",
	},
};
module_mhi_driver(mhi_bl_driver);

MODULE_DESCRIPTION("MHI SBL boot logger client");
MODULE_LICENSE("GPL");
