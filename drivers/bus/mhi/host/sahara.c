// SPDX-License-Identifier: GPL-2.0
/*
 * MHI Sahara firmware loader for Qualcomm modems without onboard flash.
 *
 * Implements the Sahara v2 protocol over MHI channels 2/3 (SAHARA) to
 * load AMSS firmware after SBL EE.  The firmware path is read from
 * mhi_controller::amss_image, set by the MHI PCI controller driver.
 */

#include <linux/firmware.h>
#include <linux/mhi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/workqueue.h>

/* Sahara v2 protocol commands */
#define SAHARA_HELLO_CMD		0x01
#define SAHARA_HELLO_RESP_CMD		0x02
#define SAHARA_READ_DATA_CMD		0x03
#define SAHARA_END_OF_IMAGE_CMD		0x04
#define SAHARA_DONE_CMD			0x05
#define SAHARA_DONE_RESP_CMD		0x06
#define SAHARA_RESET_CMD		0x07
#define SAHARA_CMD_READY_CMD		0x0B
#define SAHARA_CMD_SWITCH_MODE_CMD	0x0C

#define SAHARA_VERSION			2
#define SAHARA_SUCCESS			0

#define SAHARA_MODE_IMAGE_TX_PENDING	0x0
#define SAHARA_MODE_IMAGE_TX_COMPLETE	0x1

/* Packet lengths (bytes, LE) */
#define SAHARA_HELLO_LENGTH		0x30
#define SAHARA_READ_DATA_LENGTH		0x14
#define SAHARA_END_OF_IMAGE_LENGTH	0x10
#define SAHARA_DONE_LENGTH		0x08
#define SAHARA_RESET_LENGTH		0x08
#define SAHARA_CMD_SWITCH_MODE_LENGTH	0x0C

/*
 * Maximum single MHI TRE payload.  Matches MHI_TRE_DATA_DWORD0() which uses
 * a 16-bit length field, giving 64 KiB - 1 per TRE.
 */
#define SAHARA_MAX_PKT_SZ		0xffffU

/*
 * Largest READ_DATA request we will accept.  We send the data in
 * SAHARA_MAX_PKT_SZ chunks linked with MHI_CHAIN.
 *
 * SDX55 SBL may request near-whole-image transfers for large firmware blobs
 * such as qdsp6sw, so keep this comfortably above the largest observed
 * request size.
 */
#define SAHARA_MAX_XFER_SZ		0x6000000U	/* 96 MiB */

struct sahara_packet {
	__le32 cmd;
	__le32 length;
	union {
		struct {
			__le32 version;
			__le32 version_compat;
			__le32 max_length;
			__le32 mode;
		} hello;
		struct {
			__le32 version;
			__le32 version_compat;
			__le32 status;
			__le32 mode;
		} hello_resp;
		struct {
			__le32 image;
			__le32 offset;
			__le32 length;
		} read_data;
		struct {
			__le32 image;
			__le32 status;
		} end_of_image;
		struct {
			__le32 mode;
		} switch_mode;
	};
};

struct mhi_sahara_dev {
	struct mhi_device	*mhi_dev;
	struct work_struct	work;
	struct delayed_work	xfer_work;
	struct sahara_packet	*rx;
	struct sahara_packet	*ctrl_tx;
	u8			*data_tx;
	const struct firmware	*fw;
	size_t			rx_size;
	u32			xfer_offset;
	u32			xfer_remaining;
	bool			xfer_active;
};

static void sahara_xfer_retry_work(struct work_struct *work);

static void sahara_schedule_xfer_retry(struct mhi_sahara_dev *sdev)
{
	queue_delayed_work(system_wq, &sdev->xfer_work, msecs_to_jiffies(1));
}

static const char *sahara_image_to_fw(struct mhi_sahara_dev *sdev, u32 image_id)
{
	/* Wire IDs from kebab's stock libmdmimgload.so (SDX55M base map).
	 * OnePlus subsequently remaps image 20 through liboemnvbk_img_helper.
	 * SBL security SW_IDs are a different namespace: do not infer these
	 * numbers from the signed image metadata or SBL boot table. */
	switch (image_id) {
	case 6:
		return "qcom/sdx55m/apps.mbn";
	case 23:
		return "qcom/sdx55m/aop.mbn";
	case 16:
		return "qcom/sdx55m/efs1.bin";
	case 17:
		return "qcom/sdx55m/efs2.bin";
	case 20:
		/* Stock OnePlus oemnvbk_add_map_config(..., 40, 1) replaces
		 * "20:" with mdm1oemnvbktmp for SDX55M/SDXPRAIRIE. Use only a
		 * verified, own-device host copy; never regenerate/write raw NV.
		 * Keep Apollo and other machines on their original EFS3 policy. */
		if (of_machine_is_compatible("oneplus,kebab") &&
		    sdev->mhi_dev->mhi_cntrl->name &&
		    !strcmp(sdev->mhi_dev->mhi_cntrl->name, "qcom-sdx55m-fusion"))
			return "qcom/sdx55m/oemnvbktmp.bin";
		return "qcom/sdx55m/efs3.bin";
	case 8:
		return "qcom/sdx55m/qdsp6sw.mbn";
	case 21:
		return "qcom/sdx55m/sbl1.mbn";
	case 34:
		/* Read-only host copy of this phone's mdmddr partition. */
		return "qcom/sdx55m/mdmddr.bin";
	case 36:
		return "qcom/sdx55m/multi_image_qti.mbn";
	case 37:
		return sdev->mhi_dev->mhi_cntrl->amss_image;
	case 38:
		/* Stock map calls this xbl_config.elf; our modem image uses xbl_cfg.elf. */
		return "qcom/sdx55m/xbl_cfg.elf";
	case 25:
		return "qcom/sdx55m/tz.mbn";
	case 29:
		return "qcom/sdx55m/acdb.mbn";
	case 33:
		return "qcom/sdx55m/hyp.mbn";
	case 40:
		return "qcom/sdx55m/apdp.mbn";
	case 41:
		return "qcom/sdx55m/devcfg.mbn";
	case 42:
		return "qcom/sdx55m/sec.elf";
	default:
		return NULL;
	}
}

static void sahara_send_reset(struct mhi_sahara_dev *sdev)
{
	sdev->ctrl_tx->cmd    = cpu_to_le32(SAHARA_RESET_CMD);
	sdev->ctrl_tx->length = cpu_to_le32(SAHARA_RESET_LENGTH);
	mhi_queue_buf(sdev->mhi_dev, DMA_TO_DEVICE, sdev->ctrl_tx,
		      SAHARA_RESET_LENGTH, MHI_EOT);
}

static void sahara_hello(struct mhi_sahara_dev *sdev)
{
	struct device *dev = &sdev->mhi_dev->dev;

	if (le32_to_cpu(sdev->rx->length) != SAHARA_HELLO_LENGTH ||
	    le32_to_cpu(sdev->rx->hello.version) != SAHARA_VERSION) {
		dev_err(dev, "Sahara: unexpected HELLO (len=%u ver=%u)\n",
			le32_to_cpu(sdev->rx->length),
			le32_to_cpu(sdev->rx->hello.version));
		return;
	}

	dev_info(dev, "Sahara: HELLO received, mode=%u\n",
		 le32_to_cpu(sdev->rx->hello.mode));

	sdev->ctrl_tx->cmd                      = cpu_to_le32(SAHARA_HELLO_RESP_CMD);
	sdev->ctrl_tx->length                   = cpu_to_le32(SAHARA_HELLO_LENGTH);
	sdev->ctrl_tx->hello_resp.version       = cpu_to_le32(SAHARA_VERSION);
	sdev->ctrl_tx->hello_resp.version_compat = cpu_to_le32(SAHARA_VERSION);
	sdev->ctrl_tx->hello_resp.status        = cpu_to_le32(SAHARA_SUCCESS);
	sdev->ctrl_tx->hello_resp.mode          = sdev->rx->hello.mode;

	mhi_queue_buf(sdev->mhi_dev, DMA_TO_DEVICE, sdev->ctrl_tx,
		      SAHARA_HELLO_LENGTH, MHI_EOT);
}

static int sahara_queue_read_data(struct mhi_sahara_dev *sdev)
{
	struct device *dev = &sdev->mhi_dev->dev;
	u32 chunk;
	int ret;

	if (!sdev->xfer_active || !sdev->xfer_remaining)
		return 0;

	chunk = min(sdev->xfer_remaining, (u32)SAHARA_MAX_PKT_SZ);
	memcpy(sdev->data_tx, sdev->fw->data + sdev->xfer_offset, chunk);

	if (sdev->xfer_offset == le32_to_cpu(sdev->rx->read_data.offset))
		dev_dbg(dev, "Sahara: tx[0][0..15] = %*ph\n",
			 min_t(u32, 16, chunk), sdev->data_tx);

	sdev->xfer_offset += chunk;
	sdev->xfer_remaining -= chunk;

	ret = mhi_queue_buf(sdev->mhi_dev, DMA_TO_DEVICE, sdev->data_tx,
			    chunk, MHI_EOT);
	if (ret) {
		if (ret != -EAGAIN)
			dev_err(dev, "Sahara: mhi_queue_buf TX failed: %d\n", ret);
		else
			dev_dbg(dev, "Sahara: TX ring full, retrying chunk later\n");

		sdev->xfer_offset -= chunk;
		sdev->xfer_remaining += chunk;
		return ret;
	}

	if (!sdev->xfer_remaining)
		sdev->xfer_active = false;

	return 0;
}

static void sahara_read_data(struct mhi_sahara_dev *sdev)
{
	struct device *dev = &sdev->mhi_dev->dev;
	u32 offset, length;
	int ret;

	if (le32_to_cpu(sdev->rx->length) != SAHARA_READ_DATA_LENGTH) {
		dev_err(dev, "Sahara: malformed READ_DATA (len=%u)\n",
			le32_to_cpu(sdev->rx->length));
		sahara_send_reset(sdev);
		return;
	}

	offset = le32_to_cpu(sdev->rx->read_data.offset);
	length = le32_to_cpu(sdev->rx->read_data.length);

	dev_info(dev, "Sahara: READ_DATA image=%u offset=%u length=%u\n",
		 le32_to_cpu(sdev->rx->read_data.image), offset, length);

	if (!sdev->fw) {
		u32 image_id = le32_to_cpu(sdev->rx->read_data.image);
		const char *fw_name = sahara_image_to_fw(sdev, image_id);

		if (!fw_name) {
			dev_err(dev, "Sahara: no firmware configured for image %u\n",
				image_id);
			sahara_send_reset(sdev);
			return;
		}

		ret = request_firmware(&sdev->fw, fw_name, dev);
		if (ret) {
			dev_err(dev, "Sahara: failed to load %s: %d\n",
				fw_name, ret);
			sahara_send_reset(sdev);
			return;
		}

		dev_info(dev, "Sahara: image %u -> loaded %s (%zu bytes)\n",
			 image_id, fw_name, sdev->fw->size);
	}

	if (length > SAHARA_MAX_XFER_SZ) {
		dev_err(dev, "Sahara: READ_DATA length %u exceeds max %u\n",
			length, SAHARA_MAX_XFER_SZ);
		sahara_send_reset(sdev);
		return;
	}

	if (offset >= sdev->fw->size ||
	    (u64)offset + length > sdev->fw->size) {
		dev_err(dev, "Sahara: READ_DATA out of range (off=%u len=%u fwsz=%zu)\n",
			offset, length, sdev->fw->size);
		sahara_send_reset(sdev);
		return;
	}

	dev_dbg(dev, "Sahara: fw[0x%x..+%u] = %*ph\n",
		 offset, length, min_t(u32, 16, length), sdev->fw->data + offset);

	sdev->xfer_offset = offset;
	sdev->xfer_remaining = length;
	sdev->xfer_active = true;

	ret = sahara_queue_read_data(sdev);
	if (ret == -EAGAIN)
		sahara_schedule_xfer_retry(sdev);
	else if (ret)
		sahara_send_reset(sdev);
}

static void sahara_end_of_image(struct mhi_sahara_dev *sdev)
{
	struct device *dev = &sdev->mhi_dev->dev;

	if (le32_to_cpu(sdev->rx->end_of_image.status)) {
		dev_err(dev, "Sahara: END_OF_IMAGE status %u\n",
			le32_to_cpu(sdev->rx->end_of_image.status));
		return;
	}

	dev_info(dev, "Sahara: END_OF_IMAGE OK (image=%u), sending DONE\n",
		 le32_to_cpu(sdev->rx->end_of_image.image));

	sdev->ctrl_tx->cmd    = cpu_to_le32(SAHARA_DONE_CMD);
	sdev->ctrl_tx->length = cpu_to_le32(SAHARA_DONE_LENGTH);
	mhi_queue_buf(sdev->mhi_dev, DMA_TO_DEVICE, sdev->ctrl_tx,
		      SAHARA_DONE_LENGTH, MHI_EOT);
}

static void sahara_work(struct work_struct *work)
{
	struct mhi_sahara_dev *sdev =
		container_of(work, struct mhi_sahara_dev, work);
	int ret;

	switch (le32_to_cpu(sdev->rx->cmd)) {
	case SAHARA_HELLO_CMD:
		sahara_hello(sdev);
		break;
	case SAHARA_READ_DATA_CMD:
		sahara_read_data(sdev);
		break;
	case SAHARA_END_OF_IMAGE_CMD:
		sahara_end_of_image(sdev);
		break;
	case SAHARA_DONE_RESP_CMD:
		dev_info(&sdev->mhi_dev->dev,
			 "Sahara: DONE_RESP received, session complete\n");
		/*
		 * Release current firmware so the next READ_DATA re-evaluates
		 * which file to load (the modem may start a second SAHARA
		 * session requesting different image IDs).
		 */
		if (sdev->fw) {
			release_firmware(sdev->fw);
			sdev->fw = NULL;
		}
		sdev->xfer_active = false;
		sdev->xfer_offset = 0;
		sdev->xfer_remaining = 0;
		cancel_delayed_work(&sdev->xfer_work);
		break;	/* re-queue RX to listen for a possible next HELLO */
	case SAHARA_CMD_READY_CMD:
		/*
		 * Modem is in command mode and ready for host commands.
		 * We don't need to execute any commands — just ask the
		 * modem to switch to image transfer mode.  It will reply
		 * with a fresh HELLO (mode=0).
		 */
		dev_info(&sdev->mhi_dev->dev,
			 "Sahara: CMD_READY, sending CMD_SWITCH_MODE -> image TX\n");
		sdev->ctrl_tx->cmd    = cpu_to_le32(SAHARA_CMD_SWITCH_MODE_CMD);
		sdev->ctrl_tx->length = cpu_to_le32(SAHARA_CMD_SWITCH_MODE_LENGTH);
		sdev->ctrl_tx->switch_mode.mode = cpu_to_le32(SAHARA_MODE_IMAGE_TX_PENDING);
		mhi_queue_buf(sdev->mhi_dev, DMA_TO_DEVICE, sdev->ctrl_tx,
			      SAHARA_CMD_SWITCH_MODE_LENGTH, MHI_EOT);
		break;
	case SAHARA_RESET_CMD:
		/* Device-initiated reset — just re-queue and wait */
		break;
	default:
		dev_err(&sdev->mhi_dev->dev,
			"Sahara: unknown command 0x%x\n",
			le32_to_cpu(sdev->rx->cmd));
		break;
	}

	/* Refill the single RX slot */
	ret = mhi_queue_buf(sdev->mhi_dev, DMA_FROM_DEVICE, sdev->rx,
			    SAHARA_MAX_PKT_SZ, MHI_EOT);
	if (ret)
		dev_err(&sdev->mhi_dev->dev,
			"Sahara: failed to requeue RX buf: %d\n", ret);
}

static int sahara_mhi_probe(struct mhi_device *mhi_dev,
			    const struct mhi_device_id *id)
{
	struct mhi_sahara_dev *sdev;
	int ret;

	sdev = devm_kzalloc(&mhi_dev->dev, sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	sdev->rx = devm_kzalloc(&mhi_dev->dev, SAHARA_MAX_PKT_SZ, GFP_KERNEL);
	if (!sdev->rx)
		return -ENOMEM;

	sdev->ctrl_tx = devm_kzalloc(&mhi_dev->dev, SAHARA_MAX_PKT_SZ, GFP_KERNEL);
	if (!sdev->ctrl_tx)
		return -ENOMEM;

	sdev->data_tx = devm_kzalloc(&mhi_dev->dev, SAHARA_MAX_PKT_SZ, GFP_KERNEL);
	if (!sdev->data_tx)
		return -ENOMEM;

	sdev->mhi_dev = mhi_dev;
	INIT_WORK(&sdev->work, sahara_work);
	INIT_DELAYED_WORK(&sdev->xfer_work, sahara_xfer_retry_work);
	dev_set_drvdata(&mhi_dev->dev, sdev);

	ret = mhi_prepare_for_transfer(mhi_dev);
	if (ret)
		return ret;

	ret = mhi_queue_buf(mhi_dev, DMA_FROM_DEVICE, sdev->rx,
			    SAHARA_MAX_PKT_SZ, MHI_EOT);
	if (ret) {
		mhi_unprepare_from_transfer(mhi_dev);
		return ret;
	}

	dev_info(&mhi_dev->dev, "Sahara: MHI channels ready, waiting for modem\n");
	return 0;
}

static void sahara_mhi_remove(struct mhi_device *mhi_dev)
{
	struct mhi_sahara_dev *sdev = dev_get_drvdata(&mhi_dev->dev);

	cancel_work_sync(&sdev->work);
	cancel_delayed_work_sync(&sdev->xfer_work);
	if (sdev->fw)
		release_firmware(sdev->fw);
	mhi_unprepare_from_transfer(mhi_dev);
}

static void sahara_mhi_ul_xfer_cb(struct mhi_device *mhi_dev,
				  struct mhi_result *result)
{
	struct mhi_sahara_dev *sdev = dev_get_drvdata(&mhi_dev->dev);
	int ret;

	if (result->transaction_status) {
		dev_err(&mhi_dev->dev, "Sahara: TX completion failed: %d\n",
			result->transaction_status);
		sdev->xfer_active = false;
		return;
	}

	if (result->buf_addr != sdev->data_tx || !sdev->xfer_remaining)
		return;

	ret = sahara_queue_read_data(sdev);
	if (ret == -EAGAIN)
		sahara_schedule_xfer_retry(sdev);
	else if (ret)
		sahara_send_reset(sdev);
}

static void sahara_xfer_retry_work(struct work_struct *work)
{
	struct mhi_sahara_dev *sdev =
		container_of(to_delayed_work(work), struct mhi_sahara_dev, xfer_work);
	int ret;

	if (!sdev->xfer_active || !sdev->xfer_remaining)
		return;

	ret = sahara_queue_read_data(sdev);
	if (ret == -EAGAIN)
		sahara_schedule_xfer_retry(sdev);
	else if (ret)
		sahara_send_reset(sdev);
}

static void sahara_mhi_dl_xfer_cb(struct mhi_device *mhi_dev,
				  struct mhi_result *result)
{
	struct mhi_sahara_dev *sdev = dev_get_drvdata(&mhi_dev->dev);

	if (result->transaction_status)
		return;

	sdev->rx_size = result->bytes_xferd;
	schedule_work(&sdev->work);
}

static const struct mhi_device_id sahara_mhi_match_table[] = {
	{ .chan = "SAHARA" },
	{}
};
MODULE_DEVICE_TABLE(mhi, sahara_mhi_match_table);

static struct mhi_driver sahara_mhi_driver = {
	.id_table    = sahara_mhi_match_table,
	.probe       = sahara_mhi_probe,
	.remove      = sahara_mhi_remove,
	.ul_xfer_cb  = sahara_mhi_ul_xfer_cb,
	.dl_xfer_cb  = sahara_mhi_dl_xfer_cb,
	.driver      = {
		.name = "mhi_sahara_modem",
	},
};
module_mhi_driver(sahara_mhi_driver);

MODULE_DESCRIPTION("MHI Sahara firmware loader for Qualcomm modems");
MODULE_VERSION("kebab-oemnv-1");
MODULE_LICENSE("GPL");
