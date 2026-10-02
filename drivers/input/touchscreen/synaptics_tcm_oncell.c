// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Synaptics TCM Oncell Touchscreens
 *
 * Copyright (c) 2024 Frieder Hannenheim <frieder.hannenheim@proton.me>
 * Copyright (c) 2024 Caleb Connolly <caleb@postmarketos.org>
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#include "synaptics_tcm_report.h"

/* A distinct name permits testing alongside an older built-in driver. */
#ifndef TCM_DRIVER_NAME
#define TCM_DRIVER_NAME "synaptics-tcm"
#endif

#define TCM_MESSAGE_MARKER			0xa5
#define TCM_MESSAGE_SIZE			256

/* TCM commands: command byte, little-endian payload length, then payload. */
#define TCM_ENABLE_REPORT			0x05
#define TCM_DISABLE_REPORT			0x06
#define TCM_RUN_APPLICATION_FIRMWARE		0x14
#define TCM_GET_APPLICATION_INFO			0x20
#define TCM_GET_TOUCH_REPORT_CONFIG		0x25

#define MODE_APPLICATION			0x01
#define APP_STATUS_OK				0x00
#define APP_STATUS_BOOTING			0x01
#define APP_STATUS_UPDATING			0x02

#define REPORT_IDLE				0x00
#define REPORT_OK				0x01
#define REPORT_BUSY				0x02
#define REPORT_CONTINUED_READ			0x03
#define REPORT_RECEIVE_BUFFER_OVERFLOW		0x0c
#define REPORT_NOT_IMPLEMENTED			0x0e
#define REPORT_ERROR				0x0f
#define REPORT_IDENTIFY				0x10
#define REPORT_TOUCH				0x11

struct tcm_message_header {
	u8 marker;
	u8 code;
	__le16 length;
} __packed;

struct tcm_identification {
	struct tcm_message_header header;
	u8 version;
	u8 mode;
	char part_number[16];
	u8 build_id[4];
	u8 max_write_size[2];
} __packed;

struct tcm_app_info {
	struct tcm_message_header header;
	u8 version[2];
	__le16 status;
	u8 static_config_size[2];
	u8 dynamic_config_size[2];
	u8 app_config_start_write_block[2];
	u8 app_config_size[2];
	u8 max_touch_report_config_size[2];
	u8 max_touch_report_payload_size[2];
	char customer_config_id[16];
	__le16 max_x;
	__le16 max_y;
	u8 max_objects[2];
	u8 num_of_buttons[2];
	u8 num_of_image_rows[2];
	u8 num_of_image_cols[2];
	u8 has_hybrid_data[2];
} __packed;

struct tcm_data {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct completion response;
	struct mutex command_lock;
	struct touchscreen_properties props;
	struct regulator_bulk_data supplies[2];
	struct tcm_report_config report_config;
	struct tcm_report_object objects[TCM_MAX_OBJECTS];
	unsigned int max_objects;
	u16 max_x;
	u16 max_y;
	bool command_pending;
	bool input_ready;
	bool stopping;
	u8 expected_response;
	int command_error;
	u16 buf_size;
	u8 buf[TCM_MESSAGE_SIZE];
};

static int tcm_send_cmd(struct tcm_data *tcm, u8 cmd,
			const u8 *data, size_t length)
{
	struct i2c_client *client = tcm->client;
	u8 buf[TCM_MESSAGE_SIZE];
	struct i2c_msg msg = {
		.addr = client->addr,
		.len = 3 + length,
		.buf = buf,
	};
	int ret;

	if (length > sizeof(buf) - 3)
		return -EMSGSIZE;

	dev_dbg(&client->dev, "sending command %#x\n", cmd);
	buf[0] = cmd;
	put_unaligned_le16(length, buf + 1);
	if (length)
		memcpy(buf + 3, data, length);

	ret = i2c_transfer(client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int tcm_recv_report(struct tcm_data *tcm, void *buf, size_t length)
{
	struct i2c_client *client = tcm->client;
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = I2C_M_RD,
		.len = length,
		.buf = buf,
	};
	int ret;

	ret = i2c_transfer(client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int tcm_read_message(struct tcm_data *tcm, u8 cmd,
			    const u8 *data, size_t data_length,
			    void *buf, size_t capacity, size_t *received)
{
	int ret;

	mutex_lock(&tcm->command_lock);
	if (tcm->stopping) {
		ret = -ENODEV;
		goto out;
	}

	reinit_completion(&tcm->response);
	tcm->buf_size = 0;
	tcm->command_error = 0;
	tcm->expected_response = cmd == TCM_RUN_APPLICATION_FIRMWARE ?
				 REPORT_IDENTIFY : REPORT_OK;
	/* Publish the expected response before the IRQ sees a pending command. */
	smp_store_release(&tcm->command_pending, true);

	ret = tcm_send_cmd(tcm, cmd, data, data_length);
	if (ret)
		goto cancel;

	if (!wait_for_completion_timeout(&tcm->response,
					 msecs_to_jiffies(1000))) {
		ret = -ETIMEDOUT;
		goto cancel;
	}

	ret = tcm->command_error;
	if (ret)
		goto out;
	if (received)
		*received = tcm->buf_size;
	/* Some firmware revisions append fields to IDENTIFY/application info. */
	if (buf)
		memcpy(buf, tcm->buf, min_t(size_t, capacity, tcm->buf_size));
	goto out;

cancel:
	WRITE_ONCE(tcm->command_pending, false);
out:
	mutex_unlock(&tcm->command_lock);
	return ret;
}

static void tcm_power_off(void *data)
{
	struct tcm_data *tcm = data;

	/* IRQ/input devres are released before this power resource. */
	gpiod_set_value_cansleep(tcm->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(tcm->supplies), tcm->supplies);
}

static int tcm_set_reporting(struct tcm_data *tcm, bool enabled)
{
	int ret;

	/* Preserve the existing no-ACK sensing interface; do not reconfigure it. */
	mutex_lock(&tcm->command_lock);
	if (tcm->stopping)
		ret = -ENODEV;
	else
		ret = i2c_smbus_write_byte(tcm->client,
					   enabled ? TCM_ENABLE_REPORT : TCM_DISABLE_REPORT);
	mutex_unlock(&tcm->command_lock);
	return ret;
}

static int tcm_input_open(struct input_dev *dev)
{
	return tcm_set_reporting(input_get_drvdata(dev), true);
}

static void tcm_input_close(struct input_dev *dev)
{
	struct tcm_data *tcm = input_get_drvdata(dev);
	int ret;

	if (READ_ONCE(tcm->stopping))
		return;

	ret = tcm_set_reporting(tcm, false);
	if (ret)
		dev_err(&tcm->client->dev, "failed to turn off sensing: %d\n", ret);
}

static int tcm_handle_touch_report(struct tcm_data *tcm, const u8 *buf, size_t len)
{
	const struct tcm_report_object *object;
	unsigned int i, major_width, minor_width;
	int ret;

	/* Acquire the fully initialized input device and report configuration. */
	if (!smp_load_acquire(&tcm->input_ready))
		return 0;

	ret = tcm_parse_touch_report(&tcm->report_config, buf, len,
				     tcm->objects, tcm->max_objects);
	if (ret)
		return ret;

	/* Validate the whole frame before changing any input slots. */
	for (i = 0; i < tcm->max_objects; i++) {
		object = &tcm->objects[i];
		if (object->classification != TCM_TOUCH_FINGER &&
		    object->classification != TCM_TOUCH_GLOVED_FINGER)
			continue;
		if (object->x > tcm->max_x || object->y > tcm->max_y)
			return -ERANGE;
	}

	for (i = 0; i < tcm->max_objects; i++) {
		object = &tcm->objects[i];
		if (object->classification != TCM_TOUCH_FINGER &&
		    object->classification != TCM_TOUCH_GLOVED_FINGER)
			continue;

		dev_dbg(&tcm->client->dev, "touch: slot %u x %u y %u\n",
			i, object->x, object->y);
		input_mt_slot(tcm->input, i);
		input_mt_report_slot_state(tcm->input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(tcm->input, &tcm->props,
				       object->x, object->y, true);

		if (tcm->report_config.bits[TOUCH_OBJECT_N_X_WIDTH] ||
		    tcm->report_config.bits[TOUCH_OBJECT_N_Y_WIDTH]) {
			major_width = max(object->width_x, object->width_y);
			minor_width = min(object->width_x, object->width_y);
			input_report_abs(tcm->input, ABS_MT_TOUCH_MAJOR,
					 min_t(u32, major_width, INT_MAX));
			input_report_abs(tcm->input, ABS_MT_TOUCH_MINOR,
					 min_t(u32, minor_width, INT_MAX));
		}
		if (tcm->report_config.bits[TOUCH_OBJECT_N_Z])
			input_report_abs(tcm->input, ABS_MT_PRESSURE,
					 min_t(u32, object->z, INT_MAX));
	}

	/* DROP_UNUSED releases absent/lifted contacts, including empty frames. */
	input_mt_sync_frame(tcm->input);
	input_sync(tcm->input);
	return 0;
}

static void tcm_complete_command(struct tcm_data *tcm, int error,
				 const u8 *buf, size_t length)
{
	/* Pairs with command publication before sending to the controller. */
	if (!smp_load_acquire(&tcm->command_pending))
		return;

	tcm->command_error = error;
	if (!error) {
		tcm->buf_size = length;
		memcpy(tcm->buf, buf, length);
	}
	WRITE_ONCE(tcm->command_pending, false);
	complete(&tcm->response);
}

static irqreturn_t tcm_report_irq(int irq, void *data)
{
	struct tcm_data *tcm = data;
	const struct tcm_message_header *header;
	u8 buf[TCM_MESSAGE_SIZE];
	u16 len;
	int ret;

	ret = tcm_recv_report(tcm, buf, sizeof(buf));
	if (ret) {
		dev_err_ratelimited(&tcm->client->dev, "failed to read report: %d\n", ret);
		tcm_complete_command(tcm, ret, NULL, 0);
		return IRQ_HANDLED;
	}

	header = (const struct tcm_message_header *)buf;
	if (header->marker != TCM_MESSAGE_MARKER) {
		dev_warn_ratelimited(&tcm->client->dev, "invalid message marker\n");
		tcm_complete_command(tcm, -EPROTO, NULL, 0);
		return IRQ_HANDLED;
	}

	if (header->code == REPORT_IDLE || header->code == REPORT_BUSY ||
	    header->code == REPORT_CONTINUED_READ)
		return IRQ_HANDLED;

	len = le16_to_cpu(header->length);
	if (len > sizeof(buf) - sizeof(*header)) {
		dev_err_ratelimited(&tcm->client->dev, "report too long: %u\n", len);
		/* An unsolicited report cannot fail an unrelated command. */
		if (smp_load_acquire(&tcm->command_pending) &&
		    header->code == tcm->expected_response)
			tcm_complete_command(tcm, -EMSGSIZE, NULL, 0);
		return IRQ_HANDLED;
	}

	if (header->code == REPORT_TOUCH) {
		/* An unsolicited touch must never satisfy a command completion. */
		ret = tcm_handle_touch_report(tcm, buf + sizeof(*header), len);
		if (ret)
			dev_warn_ratelimited(&tcm->client->dev,
					     "discarding malformed touch report: %d\n", ret);
		return IRQ_HANDLED;
	}

	/* Pairs with command publication before sending to the controller. */
	if (!smp_load_acquire(&tcm->command_pending))
		return IRQ_HANDLED;

	if (header->code == REPORT_NOT_IMPLEMENTED) {
		tcm_complete_command(tcm, -EOPNOTSUPP, NULL, 0);
	} else if (header->code >= REPORT_RECEIVE_BUFFER_OVERFLOW &&
		   header->code <= REPORT_ERROR) {
		tcm_complete_command(tcm, -EIO, NULL, 0);
	} else if (header->code == tcm->expected_response) {
		/* Discard a queued bootloader IDENTIFY while starting the application. */
		if (header->code == REPORT_IDENTIFY &&
		    len >= sizeof(struct tcm_identification) - sizeof(*header) &&
		    ((const struct tcm_identification *)buf)->mode != MODE_APPLICATION)
			return IRQ_HANDLED;
		tcm_complete_command(tcm, 0, buf, len + sizeof(*header));
	} else if (header->code == REPORT_IDENTIFY &&
		   tcm->expected_response != REPORT_IDENTIFY) {
		tcm_complete_command(tcm, -EAGAIN, NULL, 0);
	}

	return IRQ_HANDLED;
}

static int tcm_hw_init(struct tcm_data *tcm)
{
	struct tcm_identification id = { 0 };
	struct tcm_app_info app_info = { 0 };
	unsigned long deadline;
	size_t received;
	u16 status;
	int ret;

	ret = tcm_read_message(tcm, TCM_RUN_APPLICATION_FIRMWARE, NULL, 0,
				&id, sizeof(id), &received);
	if (ret)
		return ret;
	if (received < sizeof(id) || id.mode != MODE_APPLICATION)
		return -ENODEV;

	dev_dbg(&tcm->client->dev, "Synaptics TCM %.16s v%d mode %d\n",
		id.part_number, id.version, id.mode);

	deadline = jiffies + msecs_to_jiffies(2000);
	do {
		msleep(20);
		ret = tcm_read_message(tcm, TCM_GET_APPLICATION_INFO, NULL, 0,
					&app_info, sizeof(app_info), &received);
		if (ret)
			return ret;
		if (received < offsetof(struct tcm_app_info, max_objects) +
			       sizeof(app_info.max_objects))
			return -EPROTO;
		status = le16_to_cpu(app_info.status);
		if (status != APP_STATUS_BOOTING && status != APP_STATUS_UPDATING)
			break;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
	} while (true);

	if (status != APP_STATUS_OK)
		return -ENODEV;

	tcm->max_x = le16_to_cpu(app_info.max_x);
	tcm->max_y = le16_to_cpu(app_info.max_y);
	tcm->max_objects = get_unaligned_le16(app_info.max_objects);
	if (!tcm->max_x || !tcm->max_y || !tcm->max_objects ||
	    tcm->max_objects > TCM_MAX_OBJECTS)
		return -EINVAL;

	dev_info(&tcm->client->dev,
		 "TCM %.16s, firmware %.16s, coordinates %ux%u, %u touch slots\n",
		 id.part_number, app_info.customer_config_id,
		 tcm->max_x + 1, tcm->max_y + 1, tcm->max_objects);
	return 0;
}

static int tcm_get_report_config(struct tcm_data *tcm)
{
	u8 buf[TCM_MESSAGE_SIZE];
	size_t received;
	int ret;

	ret = tcm_read_message(tcm, TCM_GET_TOUCH_REPORT_CONFIG, NULL, 0,
				buf, sizeof(buf), &received);
	if (ret)
		return ret;
	if (received <= sizeof(struct tcm_message_header))
		return -EPROTO;

	ret = tcm_parse_report_config(&tcm->report_config,
				      buf + sizeof(struct tcm_message_header),
				      received - sizeof(struct tcm_message_header));
	if (ret)
		return ret;

	dev_info(&tcm->client->dev, "using controller touch report configuration (%u bytes)\n",
		 tcm->report_config.size);
	dev_dbg(&tcm->client->dev, "touch report configuration: %*ph\n",
		 tcm->report_config.size, tcm->report_config.data);
	return 0;
}

static unsigned int tcm_field_max(const struct tcm_report_config *config, u8 code)
{
	unsigned int bits = config->bits[code];

	return bits >= 31 ? INT_MAX : ((1U << bits) - 1);
}

static int tcm_power_on(struct tcm_data *tcm)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(tcm->supplies), tcm->supplies);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(tcm->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(tcm->reset_gpio, 0);
	usleep_range(80000, 81000);
	return 0;
}

static int tcm_probe(struct i2c_client *client)
{
	struct tcm_data *tcm;
	unsigned int width_max, pressure_max;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	tcm = devm_kzalloc(&client->dev, sizeof(*tcm), GFP_KERNEL);
	if (!tcm)
		return -ENOMEM;

	i2c_set_clientdata(client, tcm);
	tcm->client = client;
	init_completion(&tcm->response);
	mutex_init(&tcm->command_lock);

	tcm->supplies[0].supply = "vdd";
	tcm->supplies[1].supply = "vcc";
	ret = devm_regulator_bulk_get(&client->dev, ARRAY_SIZE(tcm->supplies),
				      tcm->supplies);
	if (ret)
		return ret;

	tcm->reset_gpio = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(tcm->reset_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(tcm->reset_gpio),
				     "failed to get reset GPIO\n");

	ret = tcm_power_on(tcm);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&client->dev, tcm_power_off, tcm);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					tcm_report_irq, IRQF_ONESHOT,
					"synaptics_tcm_report", tcm);
	if (ret)
		return ret;

	ret = tcm_hw_init(tcm);
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to initialize hardware\n");

	ret = tcm_get_report_config(tcm);
	if (ret)
		return dev_err_probe(&client->dev, ret, "failed to get touch report configuration\n");

	tcm->input = devm_input_allocate_device(&client->dev);
	if (!tcm->input)
		return -ENOMEM;

	tcm->input->name = "Synaptics TCM Oncell Touchscreen";
	tcm->input->id.bustype = BUS_I2C;
	tcm->input->open = tcm_input_open;
	tcm->input->close = tcm_input_close;
	input_set_drvdata(tcm->input, tcm);

	input_set_abs_params(tcm->input, ABS_MT_POSITION_X, 0, tcm->max_x, 0, 0);
	input_set_abs_params(tcm->input, ABS_MT_POSITION_Y, 0, tcm->max_y, 0, 0);
	width_max = max(tcm_field_max(&tcm->report_config, TOUCH_OBJECT_N_X_WIDTH),
			tcm_field_max(&tcm->report_config, TOUCH_OBJECT_N_Y_WIDTH));
	if (width_max) {
		input_set_abs_params(tcm->input, ABS_MT_TOUCH_MAJOR, 0, width_max, 0, 0);
		input_set_abs_params(tcm->input, ABS_MT_TOUCH_MINOR, 0, width_max, 0, 0);
	}
	pressure_max = tcm_field_max(&tcm->report_config, TOUCH_OBJECT_N_Z);
	if (pressure_max)
		input_set_abs_params(tcm->input, ABS_MT_PRESSURE, 0, pressure_max, 0, 0);

	touchscreen_parse_properties(tcm->input, true, &tcm->props);
	ret = input_mt_init_slots(tcm->input, tcm->max_objects,
				 INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(tcm->input);
	if (ret)
		return ret;

	/* Publish input/config initialization to the IRQ's acquire load. */
	smp_store_release(&tcm->input_ready, true);
	return 0;
}

static void tcm_remove(struct i2c_client *client)
{
	struct tcm_data *tcm = i2c_get_clientdata(client);

	/* Finish any open/close command before stopping the response IRQ. */
	mutex_lock(&tcm->command_lock);
	WRITE_ONCE(tcm->stopping, true);
	mutex_unlock(&tcm->command_lock);
	WRITE_ONCE(tcm->input_ready, false);
	disable_irq(client->irq);
	/* Managed IRQ/input teardown follows; power is removed last. */
}

static const struct of_device_id syna_driver_ids[] = {
	{ .compatible = "syna,s3908" },
	{ }
};
MODULE_DEVICE_TABLE(of, syna_driver_ids);

static const struct i2c_device_id syna_i2c_ids[] = {
	{ "synaptics-tcm", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, syna_i2c_ids);

static struct i2c_driver syna_i2c_driver = {
	.probe = tcm_probe,
	.remove = tcm_remove,
	.id_table = syna_i2c_ids,
	.driver = {
		.name = TCM_DRIVER_NAME,
		.of_match_table = syna_driver_ids,
	},
};
module_i2c_driver(syna_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Frieder Hannenheim <frieder.hannenheim@proton.me>");
MODULE_AUTHOR("Caleb Connolly <caleb@postmarketos.org>");
MODULE_DESCRIPTION("A driver for Synaptics TCM Oncell Touchpanels");
