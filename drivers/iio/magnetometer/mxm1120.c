// SPDX-License-Identifier: GPL-2.0-only
/*
 * MagnaChip MXM1120 and iSentek IST8801 single-axis Hall sensors.
 * Register initialization is derived from the corresponding GPL-2.0
 * sensor drivers published by Oplus (Copyright 2018-2020 Oplus).
 *
 * This driver handles one firmware-described sensor, not a particular
 * board, sensor pair, selector geometry, or input policy.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include "mxm1120-codec.h"

#define MXM1120_REG_INTSRS	0x01
#define MXM1120_REG_SRST		0x07
#define MXM1120_REG_OPF		0x08
#define MXM1120_REG_DID		0x09
#define MXM1120_REG_ST1		0x10
#define MXM1120_DATA_READY	BIT(0)
#define IST8801_DATA_OVERRUN	BIT(2)
#define MXM1120_SAMPLE_HZ	20
#define MXM1120_RESET_MS		20
#define MXM1120_SAMPLE_MS	60

struct mxm1120_chip_info {
	const char *name;
	u8 id;
	u8 measurement_mode;
	bool ist8801;
};

struct mxm1120_data {
	struct regmap *regmap;
	struct mutex lock;
	const struct mxm1120_chip_info *chip;
};

static const struct mxm1120_chip_info mxm1120_chip = {
	.name = "mxm1120",
	.id = 0x9c,
	.measurement_mode = 0x01, /* 20 Hz, 10-bit, HSSON enabled */
};

static const struct mxm1120_chip_info ist8801_chip = {
	.name = "ist8801",
	.id = 0x81,
	.measurement_mode = 0x70, /* 20 Hz */
	.ist8801 = true,
};

static const struct regmap_config mxm1120_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x87,
	.cache_type = REGCACHE_NONE,
};

static void mxm1120_disable_supply(void *supply)
{
	regulator_disable(supply);
}

static int mxm1120_enable_supply(struct device *dev, const char *name)
{
	struct regulator *supply;
	int ret;

	supply = devm_regulator_get(dev, name);
	if (IS_ERR(supply))
		return dev_err_probe(dev, PTR_ERR(supply), "cannot get %s supply\n", name);
	/* Voltage constraints are supplied by the board, not guessed here. */
	ret = regulator_enable(supply);
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable %s supply\n", name);
	return devm_add_action_or_reset(dev, mxm1120_disable_supply, supply);
}

static int mxm1120_check_id(struct mxm1120_data *data)
{
	unsigned int value;
	int ret;

	ret = regmap_read(data->regmap, MXM1120_REG_DID, &value);
	if (ret)
		return ret;
	return value == data->chip->id ? 0 : -ENODEV;
}

static int mxm1120_init(struct mxm1120_data *data)
{
	unsigned int value;
	int ret;

	ret = regmap_write(data->regmap, MXM1120_REG_SRST, 1);
	if (ret)
		return ret;
	msleep(MXM1120_RESET_MS);
	ret = mxm1120_check_id(data);
	if (ret)
		return ret;
	/* Direct reads only: no latched threshold interrupts to leave asserted. */
	ret = regmap_write(data->regmap, MXM1120_REG_INTSRS, 0);
	if (ret)
		return ret;
	if (data->chip->ist8801) {
		ret = regmap_write(data->regmap, 0x76, 0x04); /* TSTCNTL */
		if (ret)
			return ret;
		ret = regmap_write(data->regmap, 0x6c, 0x05); /* OSRCNTL */
		if (ret)
			return ret;
		ret = regmap_write(data->regmap, 0x20, 0); /* ACTION */
		if (ret)
			return ret;
		usleep_range(5000, 6000);
		ret = regmap_write(data->regmap, MXM1120_REG_OPF, 0);
		if (ret)
			return ret;
		ret = regmap_read(data->regmap, 0x40, &value); /* IFCNTL */
		if (ret)
			return ret;
		ret = regmap_write(data->regmap, 0x40, value | BIT(2));
		if (ret)
			return ret;
		/* GAIN=2, 10-bit ADC; keep the factory GAINCNTL trim unchanged. */
		ret = regmap_write(data->regmap, 0x0d, 0x2c);
		if (ret)
			return ret;
	}
	return regmap_write(data->regmap, MXM1120_REG_OPF, data->chip->measurement_mode);
}

static void mxm1120_stop(void *private)
{
	struct mxm1120_data *data = private;

	regmap_write(data->regmap, MXM1120_REG_INTSRS, 0);
	regmap_write(data->regmap, MXM1120_REG_OPF, 0);
}

static int mxm1120_measure(struct mxm1120_data *data, int *value)
{
	unsigned int status;
	u8 sample[3];
	int ready_mask, ret, raw;

	ready_mask = MXM1120_DATA_READY;
	if (data->chip->ist8801)
		ready_mask |= IST8801_DATA_OVERRUN;
	ret = regmap_read_poll_timeout(data->regmap, MXM1120_REG_ST1, status,
				       status & ready_mask, 5000, 100000);
	if (ret)
		return ret;
	/* Read status and both data bytes in one transaction. */
	ret = regmap_bulk_read(data->regmap, MXM1120_REG_ST1, sample, sizeof(sample));
	if (ret)
		return ret;
	if (!(sample[0] & ready_mask))
		return -EAGAIN;
	if (data->chip->ist8801)
		raw = ist8801_decode(sample[1], sample[2]);
	else
		raw = mxm1120_decode(sample[1], sample[2]);
	if (raw < -512 || raw > 511)
		return -ERANGE;
	*value = raw;
	return 0;
}

static int mxm1120_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *channel,
			    int *value, int *value2, long mask)
{
	struct mxm1120_data *data = iio_priv(indio_dev);
	struct device *dev = indio_dev->dev.parent;
	int ret;

	if (mask == IIO_CHAN_INFO_SAMP_FREQ) {
		*value = MXM1120_SAMPLE_HZ;
		return IIO_VAL_INT;
	}
	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	mutex_lock(&data->lock);
	ret = mxm1120_measure(data, value);
	mutex_unlock(&data->lock);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret ? ret : IIO_VAL_INT;
}

static const int mxm1120_raw_range[] = { -512, 1, 511 };

static int mxm1120_read_avail(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *channel,
			     const int **values, int *type, int *length, long mask)
{
	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;
	*values = mxm1120_raw_range;
	*type = IIO_VAL_INT;
	*length = ARRAY_SIZE(mxm1120_raw_range);
	return IIO_AVAIL_RANGE;
}

static const struct iio_info mxm1120_info = {
	.read_raw = mxm1120_read_raw,
	.read_avail = mxm1120_read_avail,
};

static const struct iio_chan_spec mxm1120_channels[] = {
	{
		.type = IIO_MAGN,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	},
};

static int mxm1120_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mxm1120_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;
	data = iio_priv(indio_dev);
	data->chip = i2c_get_match_data(client);
	if (!data->chip)
		return -EINVAL;
	mutex_init(&data->lock);
	data->regmap = devm_regmap_init_i2c(client, &mxm1120_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap), "cannot create regmap\n");
	/* No dummy supply: do not communicate with an undescribed power domain. */
	if (!device_property_present(dev, "vdd-supply"))
		return dev_err_probe(dev, -EINVAL, "missing vdd-supply\n");
	ret = mxm1120_enable_supply(dev, "vdd");
	if (ret)
		return ret;
	msleep(MXM1120_RESET_MS);
	if (device_property_present(dev, "vio-supply")) {
		ret = mxm1120_enable_supply(dev, "vio");
		if (ret)
			return ret;
		msleep(MXM1120_RESET_MS);
	}
	/* Validate this client's ID before writing reset/configuration registers. */
	ret = mxm1120_check_id(data);
	if (ret)
		return dev_err_probe(dev, ret, "not a responding %s sensor\n", data->chip->name);
	ret = devm_add_action_or_reset(dev, mxm1120_stop, data);
	if (ret)
		return ret;
	ret = mxm1120_init(data);
	if (ret)
		return dev_err_probe(dev, ret, "cannot initialize sensor\n");

	indio_dev->name = data->chip->name;
	indio_dev->info = &mxm1120_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = mxm1120_channels;
	indio_dev->num_channels = ARRAY_SIZE(mxm1120_channels);
	i2c_set_clientdata(client, indio_dev);
	ret = pm_runtime_set_active(dev);
	if (ret)
		return ret;
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ret;
	pm_runtime_mark_last_busy(dev);
	pm_runtime_idle(dev);
	return 0;
}

static int mxm1120_runtime_suspend(struct device *dev)
{
	struct mxm1120_data *data = iio_priv(dev_get_drvdata(dev));
	int ret;

	mutex_lock(&data->lock);
	ret = regmap_write(data->regmap, MXM1120_REG_OPF, 0);
	mutex_unlock(&data->lock);
	return ret;
}

static int mxm1120_runtime_resume(struct device *dev)
{
	struct mxm1120_data *data = iio_priv(dev_get_drvdata(dev));
	int ret;

	mutex_lock(&data->lock);
	ret = regmap_write(data->regmap, MXM1120_REG_OPF, data->chip->measurement_mode);
	if (!ret)
		msleep(MXM1120_SAMPLE_MS);
	mutex_unlock(&data->lock);
	return ret;
}

static DEFINE_RUNTIME_DEV_PM_OPS(mxm1120_pm_ops, mxm1120_runtime_suspend,
				 mxm1120_runtime_resume, NULL);

static const struct of_device_id mxm1120_of_match[] = {
	{ .compatible = "magnachip,mxm1120", .data = &mxm1120_chip },
	{ .compatible = "isentek,ist8801", .data = &ist8801_chip },
	{ }
};
MODULE_DEVICE_TABLE(of, mxm1120_of_match);

static const struct i2c_device_id mxm1120_ids[] = {
	{ "mxm1120", (kernel_ulong_t)&mxm1120_chip },
	{ "ist8801", (kernel_ulong_t)&ist8801_chip },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mxm1120_ids);

static struct i2c_driver mxm1120_driver = {
	.driver = {
		.name = "mxm1120",
		.of_match_table = mxm1120_of_match,
		.pm = pm_ptr(&mxm1120_pm_ops),
	},
	.probe = mxm1120_probe,
	.id_table = mxm1120_ids,
};
module_i2c_driver(mxm1120_driver);

MODULE_DESCRIPTION("MXM1120 and IST8801 single-axis Hall sensors");
MODULE_LICENSE("GPL");
