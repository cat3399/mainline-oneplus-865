// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/mhi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/qrtr.h>
#include <linux/skbuff.h>
#include <linux/soc/qcom/qmi.h>
#include <net/sock.h>

#include "qrtr.h"

#define QRTR_PROTO_VER_1 1
#define QRTR_PROTO_VER_2 3
#define SERVREG_GET_DOMAIN_LIST_REQ 0x21
#define MSG_0026_REQ 0x0026

struct qrtr_hdr_v1 {
	__le32 version;
	__le32 type;
	__le32 src_node_id;
	__le32 src_port_id;
	__le32 confirm_rx;
	__le32 size;
	__le32 dst_node_id;
	__le32 dst_port_id;
} __packed;

struct qrtr_hdr_v2 {
	u8 version;
	u8 type;
	u8 flags;
	u8 optlen;
	__le32 size;
	__le16 src_node_id;
	__le16 src_port_id;
	__le16 dst_node_id;
	__le16 dst_port_id;
} __packed;

struct qrtr_mhi_dev {
	struct qrtr_endpoint ep;
	struct mhi_device *mhi_dev;
	struct device *dev;
	atomic_t dl_count;
	atomic_t ul_count;
};

static const char *qrtr_pkt_type_name(u32 type)
{
	switch (type) {
	case QRTR_TYPE_DATA:
		return "data";
	case QRTR_TYPE_HELLO:
		return "hello";
	case QRTR_TYPE_BYE:
		return "bye";
	case QRTR_TYPE_NEW_SERVER:
		return "new-server";
	case QRTR_TYPE_DEL_SERVER:
		return "del-server";
	case QRTR_TYPE_DEL_CLIENT:
		return "del-client";
	case QRTR_TYPE_RESUME_TX:
		return "resume-tx";
	case QRTR_TYPE_EXIT:
		return "exit";
	case QRTR_TYPE_PING:
		return "ping";
	case QRTR_TYPE_NEW_LOOKUP:
		return "new-lookup";
	case QRTR_TYPE_DEL_LOOKUP:
		return "del-lookup";
	default:
		return "unknown";
	}
}

static const char *qmi_type_name(u8 type)
{
	switch (type) {
	case QMI_REQUEST:
		return "req";
	case QMI_RESPONSE:
		return "resp";
	case QMI_INDICATION:
		return "ind";
	default:
		return "unknown";
	}
}

static void qcom_mhi_qrtr_log_ascii_tlv(struct device *dev, const char *dir,
					int n, u8 tlv_type,
					const u8 *val, u16 tlv_len)
{
	char str[128];
	size_t copy_len, i;

	if (!tlv_len)
		return;

	copy_len = min_t(size_t, tlv_len, sizeof(str) - 1);
	for (i = 0; i < copy_len; i++) {
		if (val[i] < 32 || val[i] > 126)
			return;
		str[i] = (char)val[i];
	}
	str[copy_len] = '\0';

	dev_dbg(dev, "qrtr %s #%d qmi tlv=0x%02x str=\"%s\"\n",
		dir, n, tlv_type, str);
}

static void qcom_mhi_qrtr_log_servreg_domain_list_tlv(struct device *dev,
						      const char *dir, int n,
						      u8 qmi_type, u8 tlv_type,
						      const u8 *val, u16 tlv_len)
{
	if (qmi_type == QMI_REQUEST) {
		if (tlv_type == 0x01)
			qcom_mhi_qrtr_log_ascii_tlv(dev, dir, n, tlv_type, val,
						    tlv_len);

		if (tlv_type == 0x10 && tlv_len >= 5) {
			u8 valid = val[0];
			u32 offset = val[1] | (val[2] << 8) | (val[3] << 16) |
				     (val[4] << 24);

			dev_dbg(dev,
				"qrtr %s #%d servreg offset_valid=%u offset=%u\n",
				dir, n, valid, offset);
		}
		return;
	}

	if (qmi_type != QMI_RESPONSE)
		return;

	if (tlv_type == 0x02 && tlv_len >= 4) {
		u16 result = val[0] | (val[1] << 8);
		u16 error = val[2] | (val[3] << 8);

		dev_dbg(dev,
			"qrtr %s #%d servreg result=%u error=%u\n",
			dir, n, result, error);
		return;
	}

	if (tlv_type == 0x10 && tlv_len >= 3) {
		u8 valid = val[0];
		u16 total = val[1] | (val[2] << 8);

		dev_dbg(dev,
			"qrtr %s #%d servreg total_domains_valid=%u total_domains=%u\n",
			dir, n, valid, total);
		return;
	}

	if (tlv_type == 0x11 && tlv_len >= 3) {
		u8 valid = val[0];
		u16 rev = val[1] | (val[2] << 8);

		dev_dbg(dev,
			"qrtr %s #%d servreg db_rev_valid=%u db_rev=%u\n",
			dir, n, valid, rev);
		return;
	}

	if (tlv_type == 0x12 && tlv_len >= 2) {
		u8 valid = val[0];
		u8 count = val[1];
		size_t off = 2;
		u8 i;

		dev_dbg(dev,
			"qrtr %s #%d servreg domain_list_valid=%u count=%u\n",
			dir, n, valid, count);

		for (i = 0; i < count && off < tlv_len; i++) {
			u8 raw_name_len;
			size_t name_len;
			char name[129];
			u32 instance;
			u8 service_data_valid;
			u32 service_data = 0;

			raw_name_len = val[off++];
			name_len = min_t(size_t, raw_name_len, sizeof(name) - 1);
			if (off + raw_name_len + 4 + 1 > tlv_len)
				break;

			memcpy(name, &val[off], name_len);
			name[name_len] = '\0';
			off += raw_name_len;

			instance = val[off] | (val[off + 1] << 8) |
				   (val[off + 2] << 16) | (val[off + 3] << 24);
			off += 4;

			service_data_valid = val[off++];
			if (service_data_valid) {
				if (off + 4 > tlv_len)
					break;
				service_data = val[off] | (val[off + 1] << 8) |
					       (val[off + 2] << 16) |
					       (val[off + 3] << 24);
				off += 4;
			}

			dev_dbg(dev,
				"qrtr %s #%d servreg domain[%u] name=\"%s\" instance=%u service_data_valid=%u service_data=%u\n",
				dir, n, i, name, instance, service_data_valid,
				service_data);
		}
	}
}

static void qcom_mhi_qrtr_log_msg_0026_tlv(struct device *dev, const char *dir,
					   int n, u8 qmi_type, u8 tlv_type,
					   const u8 *val, u16 tlv_len)
{
	char tag[48];
	size_t dump;

	/* Most plausible decoded fields. msg=0x0026 is the unidentified
	 * service that AP port 16407 streams 1065-byte payloads into until
	 * the modem stops responding around txn=49. Print enough to
	 * identify the content (TLV 0x01 head) and any seq/status u32
	 * (TLV 0x10).
	 */
	if (tlv_type == 0x10 && tlv_len == 4) {
		u32 v = val[0] | (val[1] << 8) | (val[2] << 16) | (val[3] << 24);

		dev_dbg(dev,
			"qrtr %s #%d msg26 tlv=0x10 u32=%u (0x%08x)\n",
			dir, n, v, v);
		return;
	}

	if (tlv_type == 0x02 && qmi_type == QMI_RESPONSE && tlv_len >= 4) {
		u16 result = val[0] | (val[1] << 8);
		u16 error = val[2] | (val[3] << 8);

		dev_dbg(dev,
			"qrtr %s #%d msg26 result=%u error=%u\n",
			dir, n, result, error);
		return;
	}

	if (tlv_type == 0x01 && tlv_len > 0) {
		dump = min_t(size_t, tlv_len,
			     qmi_type == QMI_REQUEST ? 32 : 16);
		scnprintf(tag, sizeof(tag), "msg26 %s #%d tlv01: ", dir, n);
		print_hex_dump_debug(tag, DUMP_PREFIX_NONE, 16, 1,
				     val, dump, true);
	}
}

static void qcom_mhi_qrtr_log_qmi(struct device *dev, const char *dir, int n,
				  const u8 *buf, size_t len)
{
	u8 type;
	u16 txn, msg, msg_len;
	size_t off;

	if (len < 7)
		return;

	type = buf[0];
	txn = buf[1] | (buf[2] << 8);
	msg = buf[3] | (buf[4] << 8);
	msg_len = buf[5] | (buf[6] << 8);

	dev_dbg(dev,
		"qrtr %s #%d qmi type=%u(%s) txn=%u msg=0x%04x len=%u total=%zu\n",
		dir, n, type, qmi_type_name(type), txn, msg, msg_len, len);

	if (7 + msg_len > len)
		msg_len = len > 7 ? len - 7 : 0;

	off = 7;
	while (off + 3 <= 7 + msg_len) {
		u8 tlv_type = buf[off];
		u16 tlv_len = buf[off + 1] | (buf[off + 2] << 8);
		const u8 *val = &buf[off + 3];

		off += 3;
		if (off + tlv_len > 7 + msg_len)
			break;

		dev_dbg(dev, "qrtr %s #%d qmi tlv=0x%02x len=%u\n",
			dir, n, tlv_type, tlv_len);
		qcom_mhi_qrtr_log_ascii_tlv(dev, dir, n, tlv_type, val, tlv_len);
		if (msg == SERVREG_GET_DOMAIN_LIST_REQ)
			qcom_mhi_qrtr_log_servreg_domain_list_tlv(dev, dir, n,
								  type,
								  tlv_type, val,
								  tlv_len);
		if (msg == MSG_0026_REQ)
			qcom_mhi_qrtr_log_msg_0026_tlv(dev, dir, n, type,
						       tlv_type, val, tlv_len);
		off += tlv_len;
	}
}

static void qcom_mhi_qrtr_log_ctrl(struct device *dev, const char *dir,
				   int n, const struct qrtr_ctrl_pkt *pkt,
				   size_t len)
{
	u32 cmd;

	if (len < sizeof(*pkt))
		return;

	cmd = le32_to_cpu(pkt->cmd);
	dev_dbg(dev,
		"qrtr %s #%d ctrl cmd=%u(%s) srv=%u inst=0x%x node=%u port=%u\n",
		dir, n, cmd, qrtr_pkt_type_name(cmd),
		le32_to_cpu(pkt->server.service),
		le32_to_cpu(pkt->server.instance),
		le32_to_cpu(pkt->server.node),
		le32_to_cpu(pkt->server.port));
}

static void qcom_mhi_qrtr_log_packet(struct device *dev, const char *dir, int n,
				     const void *data, size_t len)
{
	const struct qrtr_ctrl_pkt *pkt;
	u32 version, type, src_node, src_port, dst_node, dst_port, payload_len;
	const void *payload;
	size_t hdr_len;

	if (!data)
		return;

	if (len >= sizeof(struct qrtr_hdr_v1) &&
	    le32_to_cpup((__le32 *)data) == QRTR_PROTO_VER_1) {
		const struct qrtr_hdr_v1 *hdr = data;

		version = le32_to_cpu(hdr->version);
		type = le32_to_cpu(hdr->type);
		src_node = le32_to_cpu(hdr->src_node_id);
		src_port = le32_to_cpu(hdr->src_port_id);
		dst_node = le32_to_cpu(hdr->dst_node_id);
		dst_port = le32_to_cpu(hdr->dst_port_id);
		payload_len = le32_to_cpu(hdr->size);
		hdr_len = sizeof(*hdr);
		payload = hdr + 1;
	} else if (len >= sizeof(struct qrtr_hdr_v2) &&
		   ((const struct qrtr_hdr_v2 *)data)->version == QRTR_PROTO_VER_2) {
		const struct qrtr_hdr_v2 *hdr = data;

		version = hdr->version;
		type = hdr->type;
		src_node = le16_to_cpu(hdr->src_node_id);
		src_port = le16_to_cpu(hdr->src_port_id);
		dst_node = le16_to_cpu(hdr->dst_node_id);
		dst_port = le16_to_cpu(hdr->dst_port_id);
		/* v2 carries ports in 16 bits; normalize the control
		 * sentinel (0xFFFE) to QRTR_PORT_CTRL so the ctrl-pkt
		 * decode below fires for modem-side announcements too.
		 */
		if (src_port == 0xFFFE)
			src_port = QRTR_PORT_CTRL;
		if (dst_port == 0xFFFE)
			dst_port = QRTR_PORT_CTRL;
		payload_len = le32_to_cpu(hdr->size);
		hdr_len = sizeof(*hdr) + hdr->optlen;
		payload = (const u8 *)data + hdr_len;
	} else {
		dev_dbg(dev, "qrtr %s #%d undecoded len=%zu\n", dir, n, len);
		return;
	}

	dev_dbg(dev,
		"qrtr %s #%d v%u type=%u(%s) src=%u:%u dst=%u:%u payload=%u len=%zu\n",
		dir, n, version, type, qrtr_pkt_type_name(type),
		src_node, src_port, dst_node, dst_port, payload_len, len);

	if (hdr_len + payload_len > len)
		payload_len = len > hdr_len ? len - hdr_len : 0;

	if ((src_port == QRTR_PORT_CTRL || dst_port == QRTR_PORT_CTRL) &&
	    payload_len >= sizeof(*pkt)) {
		pkt = payload;
		qcom_mhi_qrtr_log_ctrl(dev, dir, n, pkt, payload_len);
	} else if (type == QRTR_TYPE_DATA && payload_len >= 7) {
		qcom_mhi_qrtr_log_qmi(dev, dir, n, payload, payload_len);
	}
}

/* From MHI to QRTR */
static void qcom_mhi_qrtr_dl_callback(struct mhi_device *mhi_dev,
				      struct mhi_result *mhi_res)
{
	struct qrtr_mhi_dev *qdev = dev_get_drvdata(&mhi_dev->dev);
	int n;
	int rc;

	if (!qdev)
		return;

	n = atomic_inc_return(&qdev->dl_count);
	if (mhi_res->transaction_status) {
		dev_warn(qdev->dev,
			 "qrtr-mhi: DL #%d status=%d bytes=%zu (drop)\n",
			 n, mhi_res->transaction_status,
			 mhi_res->bytes_xferd);
		return;
	}

	if (n <= 400) {
		size_t dump = min_t(size_t, mhi_res->bytes_xferd, 48);
		char tag[40];

		qcom_mhi_qrtr_log_packet(qdev->dev, "DL", n, mhi_res->buf_addr,
					 mhi_res->bytes_xferd);
		scnprintf(tag, sizeof(tag), "qrtr DL #%d (%zu B): ",
			  n, mhi_res->bytes_xferd);
		print_hex_dump_debug(tag, DUMP_PREFIX_NONE,
				     16, 1, mhi_res->buf_addr, dump, false);
	}

	rc = qrtr_endpoint_post(&qdev->ep, mhi_res->buf_addr,
				mhi_res->bytes_xferd);
	if (rc == -EINVAL)
		dev_err(qdev->dev, "invalid ipcrouter packet\n");
}

/* From QRTR to MHI */
static void qcom_mhi_qrtr_ul_callback(struct mhi_device *mhi_dev,
				      struct mhi_result *mhi_res)
{
	struct qrtr_mhi_dev *qdev = dev_get_drvdata(&mhi_dev->dev);
	struct sk_buff *skb = mhi_res->buf_addr;
	int n;

	if (qdev) {
		n = atomic_inc_return(&qdev->ul_count);
		if (n <= 400) {
			size_t dump = min_t(size_t, skb->len, 48);
			char tag[40];

			qcom_mhi_qrtr_log_packet(qdev->dev, "UL", n, skb->data,
					 skb->len);
			scnprintf(tag, sizeof(tag),
				  "qrtr UL #%d (%u B): ", n, skb->len);
			print_hex_dump_debug(tag, DUMP_PREFIX_NONE,
					     16, 1, skb->data, dump, false);
		}
	}

	if (skb->sk)
		sock_put(skb->sk);
	consume_skb(skb);
}

/* Send data over MHI */
static int qcom_mhi_qrtr_send(struct qrtr_endpoint *ep, struct sk_buff *skb)
{
	struct qrtr_mhi_dev *qdev = container_of(ep, struct qrtr_mhi_dev, ep);
	int rc;

	if (skb->sk)
		sock_hold(skb->sk);

	rc = skb_linearize(skb);
	if (rc)
		goto free_skb;

	rc = mhi_queue_skb(qdev->mhi_dev, DMA_TO_DEVICE, skb, skb->len,
			   MHI_EOT);
	if (rc)
		goto free_skb;

	return rc;

free_skb:
	if (skb->sk)
		sock_put(skb->sk);
	kfree_skb(skb);

	return rc;
}

static int qcom_mhi_qrtr_probe(struct mhi_device *mhi_dev,
			       const struct mhi_device_id *id)
{
	struct qrtr_mhi_dev *qdev;
	int rc;

	qdev = devm_kzalloc(&mhi_dev->dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return -ENOMEM;

	qdev->mhi_dev = mhi_dev;
	qdev->dev = &mhi_dev->dev;
	qdev->ep.xmit = qcom_mhi_qrtr_send;

	dev_set_drvdata(&mhi_dev->dev, qdev);
	rc = qrtr_endpoint_register(&qdev->ep, QRTR_EP_NID_AUTO);
	if (rc)
		return rc;

	/* start channels */
	rc = mhi_prepare_for_transfer_autoqueue(mhi_dev);
	if (rc) {
		qrtr_endpoint_unregister(&qdev->ep);
		return rc;
	}

	dev_dbg(qdev->dev, "Qualcomm MHI QRTR driver probed\n");

	return 0;
}

static void qcom_mhi_qrtr_remove(struct mhi_device *mhi_dev)
{
	struct qrtr_mhi_dev *qdev = dev_get_drvdata(&mhi_dev->dev);

	qrtr_endpoint_unregister(&qdev->ep);
	mhi_unprepare_from_transfer(mhi_dev);
	dev_set_drvdata(&mhi_dev->dev, NULL);
}

static const struct mhi_device_id qcom_mhi_qrtr_id_table[] = {
	{ .chan = "IPCR" },
	{}
};
MODULE_DEVICE_TABLE(mhi, qcom_mhi_qrtr_id_table);

static int __maybe_unused qcom_mhi_qrtr_pm_suspend_late(struct device *dev)
{
	struct mhi_device *mhi_dev = container_of(dev, struct mhi_device, dev);
	enum mhi_state state;

	state = mhi_get_mhi_state(mhi_dev->mhi_cntrl);
	/*
	 * Channels only have to be torn down if the device is going away.
	 * M3 means it suspended with its context intact; M0 means the
	 * controller declined to suspend it at all (some modems, such as a
	 * PCIe attached SDX55, are kept running because they cannot reach
	 * M3). Either way the channel stays valid, and tearing it down would
	 * force a needless RESET/START round trip with a device that never
	 * actually went to sleep -- which times out and takes the link with it.
	 */
	if (state == MHI_STATE_M3 || state == MHI_STATE_M0)
		return 0;

	mhi_unprepare_from_transfer(mhi_dev);

	return 0;
}

static int __maybe_unused qcom_mhi_qrtr_pm_resume_early(struct device *dev)
{
	struct mhi_device *mhi_dev = container_of(dev, struct mhi_device, dev);
	enum mhi_state state;
	int rc;

	state = mhi_get_mhi_state(mhi_dev->mhi_cntrl);
	/*
	 * Mirror the suspend side: if the channel was never unprepared there
	 * is nothing to restore.
	 */
	if (state == MHI_STATE_M3 || state == MHI_STATE_M0)
		return 0;

	rc = mhi_prepare_for_transfer_autoqueue(mhi_dev);
	if (rc)
		dev_err(dev, "failed to prepare for autoqueue transfer %d\n", rc);

	return rc;
}

static const struct dev_pm_ops qcom_mhi_qrtr_pm_ops = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(qcom_mhi_qrtr_pm_suspend_late,
				     qcom_mhi_qrtr_pm_resume_early)
};

static struct mhi_driver qcom_mhi_qrtr_driver = {
	.probe = qcom_mhi_qrtr_probe,
	.remove = qcom_mhi_qrtr_remove,
	.dl_xfer_cb = qcom_mhi_qrtr_dl_callback,
	.ul_xfer_cb = qcom_mhi_qrtr_ul_callback,
	.id_table = qcom_mhi_qrtr_id_table,
	.driver = {
		.name = "qcom_mhi_qrtr",
		.pm = &qcom_mhi_qrtr_pm_ops,
	},
};

module_mhi_driver(qcom_mhi_qrtr_driver);

MODULE_AUTHOR("Chris Lew <clew@codeaurora.org>");
MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_DESCRIPTION("Qualcomm IPC-Router MHI interface driver");
MODULE_LICENSE("GPL v2");
