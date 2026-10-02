// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2021, Linaro Ltd <loic.poulain@linaro.org> */
#include <linux/kernel.h>
#include <linux/mhi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/wwan.h>

/* MHI wwan flags */
enum mhi_wwan_flags {
	MHI_WWAN_DL_CAP,
	MHI_WWAN_UL_CAP,
	MHI_WWAN_RX_REFILL,
};

#define MHI_WWAN_MAX_MTU	0x8000

struct mhi_wwan_dev {
	/* Lower level is a mhi dev, upper level is a wwan port */
	struct mhi_device *mhi_dev;
	struct wwan_port *wwan_port;

	/* State and capabilities */
	unsigned long flags;
	size_t mtu;

	/* Protect against concurrent TX and TX-completion (bh) */
	spinlock_t tx_lock;

	/* Protect RX budget and rx_refill scheduling */
	spinlock_t rx_lock;
	struct work_struct rx_refill;

	/* RX budget is initially set to the size of the MHI RX queue and is
	 * used to limit the number of allocated and queued packets. It is
	 * decremented on data queueing and incremented on data release.
	 */
	unsigned int rx_budget;
	bool is_qmi;

	/* Debug counters for narrowing down stuck early-QMI requests */
	unsigned int dbg_tx_cnt;
	unsigned int dbg_rx_cnt;
};

#define MHI_WWAN_QMI_DBG_LIMIT	16
#define MHI_WWAN_QMI_DBG_BYTES	64

static bool mhi_wwan_is_qmi(struct mhi_wwan_dev *mhiwwan)
{
	return mhiwwan->is_qmi;
}

static bool mhi_wwan_skip_raw_qmi(struct mhi_device *mhi_dev,
				  const struct mhi_device_id *id)
{
	/*
	 * Historical workaround: Apollo SDX55 fusion accepted QMI requests
	 * but never replied (no-reply ERRFATAL chain) when the modem was fed
	 * dummy EFS via Sahara, because the modem-side QMI dispatcher needs
	 * NV state.  With real EFS images served from /lib/firmware/sdx55m/
	 * (sourced from mdm1m9kefs1/2/3), the modem replies normally and the
	 * raw QMI WWAN port is functional and required by ModemManager's
	 * qcom-soc plugin.  Keep the upstream behaviour.
	 */
	return false;
}

static void mhi_wwan_dbg_dump(struct mhi_wwan_dev *mhiwwan, const char *tag,
			      const void *buf, size_t len, unsigned int cnt)
{
	struct mhi_device *mhi_dev = mhiwwan->mhi_dev;
	size_t dump_len;

	if (!mhi_wwan_is_qmi(mhiwwan) || cnt >= MHI_WWAN_QMI_DBG_LIMIT || !buf)
		return;

	dump_len = min(len, (size_t)MHI_WWAN_QMI_DBG_BYTES);
	dev_info(&mhi_dev->dev, "qmi_dbg[%u] %s len=%zu dump=%zu\n",
		 cnt, tag, len, dump_len);
	print_hex_dump(KERN_INFO, "qmi_dbg: ", DUMP_PREFIX_OFFSET, 16, 1,
		       buf, dump_len, false);
}

/* Increment RX budget and schedule RX refill if necessary */
static void mhi_wwan_rx_budget_inc(struct mhi_wwan_dev *mhiwwan)
{
	spin_lock_bh(&mhiwwan->rx_lock);

	mhiwwan->rx_budget++;

	if (test_bit(MHI_WWAN_RX_REFILL, &mhiwwan->flags))
		schedule_work(&mhiwwan->rx_refill);

	spin_unlock_bh(&mhiwwan->rx_lock);
}

/* Decrement RX budget if non-zero and return true on success */
static bool mhi_wwan_rx_budget_dec(struct mhi_wwan_dev *mhiwwan)
{
	bool ret = false;

	spin_lock_bh(&mhiwwan->rx_lock);

	if (mhiwwan->rx_budget) {
		mhiwwan->rx_budget--;
		if (test_bit(MHI_WWAN_RX_REFILL, &mhiwwan->flags))
			ret = true;
	}

	spin_unlock_bh(&mhiwwan->rx_lock);

	return ret;
}

static void __mhi_skb_destructor(struct sk_buff *skb)
{
	/* RX buffer has been consumed, increase the allowed budget */
	mhi_wwan_rx_budget_inc(skb_shinfo(skb)->destructor_arg);
}

static void mhi_wwan_ctrl_refill_work(struct work_struct *work)
{
	struct mhi_wwan_dev *mhiwwan = container_of(work, struct mhi_wwan_dev, rx_refill);
	struct mhi_device *mhi_dev = mhiwwan->mhi_dev;

	while (mhi_wwan_rx_budget_dec(mhiwwan)) {
		struct sk_buff *skb;

		skb = alloc_skb(mhiwwan->mtu, GFP_KERNEL);
		if (!skb) {
			mhi_wwan_rx_budget_inc(mhiwwan);
			break;
		}

		/* To prevent unlimited buffer allocation if nothing consumes
		 * the RX buffers (passed to WWAN core), track their lifespan
		 * to not allocate more than allowed budget.
		 */
		skb->destructor = __mhi_skb_destructor;
		skb_shinfo(skb)->destructor_arg = mhiwwan;

		if (mhi_queue_skb(mhi_dev, DMA_FROM_DEVICE, skb, mhiwwan->mtu, MHI_EOT)) {
			dev_err(&mhi_dev->dev, "Failed to queue buffer\n");
			kfree_skb(skb);
			break;
		}
	}
}

static int mhi_wwan_ctrl_start(struct wwan_port *port)
{
	struct mhi_wwan_dev *mhiwwan = wwan_port_get_drvdata(port);
	int ret;

	dev_info(&mhiwwan->mhi_dev->dev,
		 "mhi_wwan_ctrl_start: opened by comm=%s pid=%d tgid=%d\n",
		 current->comm, task_pid_nr(current), task_tgid_nr(current));

	/* Start mhi device's channel(s) */
	ret = mhi_prepare_for_transfer(mhiwwan->mhi_dev);
	if (ret)
		return ret;

	/* Don't allocate more buffers than MHI channel queue size */
	mhiwwan->rx_budget = mhi_get_free_desc_count(mhiwwan->mhi_dev, DMA_FROM_DEVICE);

	/* Add buffers to the MHI inbound queue */
	if (test_bit(MHI_WWAN_DL_CAP, &mhiwwan->flags)) {
		set_bit(MHI_WWAN_RX_REFILL, &mhiwwan->flags);
		mhi_wwan_ctrl_refill_work(&mhiwwan->rx_refill);
	}

	return 0;
}

static void mhi_wwan_ctrl_stop(struct wwan_port *port)
{
	struct mhi_wwan_dev *mhiwwan = wwan_port_get_drvdata(port);

	dev_info(&mhiwwan->mhi_dev->dev,
		 "mhi_wwan_ctrl_stop: closed by comm=%s pid=%d tgid=%d\n",
		 current->comm, task_pid_nr(current), task_tgid_nr(current));
	dump_stack();

	spin_lock_bh(&mhiwwan->rx_lock);
	clear_bit(MHI_WWAN_RX_REFILL, &mhiwwan->flags);
	spin_unlock_bh(&mhiwwan->rx_lock);

	cancel_work_sync(&mhiwwan->rx_refill);

	mhi_unprepare_from_transfer(mhiwwan->mhi_dev);
}

static int mhi_wwan_ctrl_tx(struct wwan_port *port, struct sk_buff *skb)
{
	struct mhi_wwan_dev *mhiwwan = wwan_port_get_drvdata(port);
	int ret;

	if (skb->len > mhiwwan->mtu)
		return -EMSGSIZE;

	if (!test_bit(MHI_WWAN_UL_CAP, &mhiwwan->flags))
		return -EOPNOTSUPP;

	mhi_wwan_dbg_dump(mhiwwan, "tx", skb->data, skb->len,
			  mhiwwan->dbg_tx_cnt++);

	/* Queue the packet for MHI transfer and check fullness of the queue */
	spin_lock_bh(&mhiwwan->tx_lock);
	ret = mhi_queue_skb(mhiwwan->mhi_dev, DMA_TO_DEVICE, skb, skb->len, MHI_EOT);
	if (mhi_queue_is_full(mhiwwan->mhi_dev, DMA_TO_DEVICE))
		wwan_port_txoff(port);
	spin_unlock_bh(&mhiwwan->tx_lock);

	return ret;
}

static const struct wwan_port_ops wwan_pops = {
	.start = mhi_wwan_ctrl_start,
	.stop = mhi_wwan_ctrl_stop,
	.tx = mhi_wwan_ctrl_tx,
};

static void mhi_ul_xfer_cb(struct mhi_device *mhi_dev,
			   struct mhi_result *mhi_result)
{
	struct mhi_wwan_dev *mhiwwan = dev_get_drvdata(&mhi_dev->dev);
	struct wwan_port *port = mhiwwan->wwan_port;
	struct sk_buff *skb = mhi_result->buf_addr;

	dev_dbg(&mhi_dev->dev, "%s: status: %d xfer_len: %zu\n", __func__,
		mhi_result->transaction_status, mhi_result->bytes_xferd);

	mhi_wwan_dbg_dump(mhiwwan, "tx-complete", skb->data, skb->len,
			  mhiwwan->dbg_tx_cnt);

	/* MHI core has done with the buffer, release it */
	consume_skb(skb);

	/* There is likely new slot available in the MHI queue, re-allow TX */
	spin_lock_bh(&mhiwwan->tx_lock);
	if (!mhi_queue_is_full(mhiwwan->mhi_dev, DMA_TO_DEVICE))
		wwan_port_txon(port);
	spin_unlock_bh(&mhiwwan->tx_lock);
}

static void mhi_dl_xfer_cb(struct mhi_device *mhi_dev,
			   struct mhi_result *mhi_result)
{
	struct mhi_wwan_dev *mhiwwan = dev_get_drvdata(&mhi_dev->dev);
	struct wwan_port *port = mhiwwan->wwan_port;
	struct sk_buff *skb = mhi_result->buf_addr;

	dev_dbg(&mhi_dev->dev, "%s: status: %d receive_len: %zu\n", __func__,
		mhi_result->transaction_status, mhi_result->bytes_xferd);

	if (mhi_result->transaction_status &&
	    mhi_result->transaction_status != -EOVERFLOW) {
		kfree_skb(skb);
		return;
	}

	/* MHI core does not update skb->len, do it before forward */
	skb_put(skb, mhi_result->bytes_xferd);
	mhi_wwan_dbg_dump(mhiwwan, "rx", skb->data, skb->len,
			  mhiwwan->dbg_rx_cnt++);
	wwan_port_rx(port, skb);

	/* Do not increment rx budget nor refill RX buffers now, wait for the
	 * buffer to be consumed. Done from __mhi_skb_destructor().
	 */
}

static int mhi_wwan_ctrl_probe(struct mhi_device *mhi_dev,
			       const struct mhi_device_id *id)
{
	struct mhi_controller *cntrl = mhi_dev->mhi_cntrl;
	struct mhi_wwan_dev *mhiwwan;
	struct wwan_port *port;

	if (mhi_wwan_skip_raw_qmi(mhi_dev, id)) {
		dev_info(&mhi_dev->dev,
			 "skipping raw QMI WWAN port on %s\n",
			 cntrl->name ?: "unknown-controller");
		return -ENODEV;
	}

	mhiwwan = kzalloc(sizeof(*mhiwwan), GFP_KERNEL);
	if (!mhiwwan)
		return -ENOMEM;

	mhiwwan->mhi_dev = mhi_dev;
	mhiwwan->mtu = MHI_WWAN_MAX_MTU;
	mhiwwan->is_qmi = id->driver_data == WWAN_PORT_QMI;
	INIT_WORK(&mhiwwan->rx_refill, mhi_wwan_ctrl_refill_work);
	spin_lock_init(&mhiwwan->tx_lock);
	spin_lock_init(&mhiwwan->rx_lock);

	if (mhi_dev->dl_chan)
		set_bit(MHI_WWAN_DL_CAP, &mhiwwan->flags);
	if (mhi_dev->ul_chan)
		set_bit(MHI_WWAN_UL_CAP, &mhiwwan->flags);

	dev_set_drvdata(&mhi_dev->dev, mhiwwan);

	/* Register as a wwan port, id->driver_data contains wwan port type */
	port = wwan_create_port(&cntrl->mhi_dev->dev, id->driver_data,
				&wwan_pops, NULL, mhiwwan);
	if (IS_ERR(port)) {
		kfree(mhiwwan);
		return PTR_ERR(port);
	}

	mhiwwan->wwan_port = port;

	return 0;
};

static void mhi_wwan_ctrl_remove(struct mhi_device *mhi_dev)
{
	struct mhi_wwan_dev *mhiwwan = dev_get_drvdata(&mhi_dev->dev);

	wwan_remove_port(mhiwwan->wwan_port);
	kfree(mhiwwan);
}

static const struct mhi_device_id mhi_wwan_ctrl_match_table[] = {
	{ .chan = "DUN", .driver_data = WWAN_PORT_AT },
	{ .chan = "DUN2", .driver_data = WWAN_PORT_AT },
	{ .chan = "MBIM", .driver_data = WWAN_PORT_MBIM },
	{ .chan = "QMI", .driver_data = WWAN_PORT_QMI },
	{ .chan = "DIAG", .driver_data = WWAN_PORT_QCDM },
	{ .chan = "EFS", .driver_data = WWAN_PORT_EFS },
	{ .chan = "FIREHOSE", .driver_data = WWAN_PORT_FIREHOSE },
	{},
};
MODULE_DEVICE_TABLE(mhi, mhi_wwan_ctrl_match_table);

static struct mhi_driver mhi_wwan_ctrl_driver = {
	.id_table = mhi_wwan_ctrl_match_table,
	.remove = mhi_wwan_ctrl_remove,
	.probe = mhi_wwan_ctrl_probe,
	.ul_xfer_cb = mhi_ul_xfer_cb,
	.dl_xfer_cb = mhi_dl_xfer_cb,
	.driver = {
		.name = "mhi_wwan_ctrl",
	},
};

module_mhi_driver(mhi_wwan_ctrl_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MHI WWAN CTRL Driver");
MODULE_AUTHOR("Loic Poulain <loic.poulain@linaro.org>");
