// SPDX-License-Identifier: GPL-2.0-only
/* A policy-free three-position selector using two IIO Hall channels. */
#include <linux/device.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/slab.h>

#include "dual-hall-slider.h"

struct hall_slider {
	struct iio_channel *channels[HALL_SLIDER_CHANNELS];
	struct input_dev *input;
	struct mutex lock;
	struct hall_slider_parameters parameters;
	struct hall_slider_calibration calibration;
	struct hall_slider_state state;
	unsigned long debounce;
	unsigned int axis;
	int limits[HALL_SLIDER_CHANNELS][2];
	bool have_limits[HALL_SLIDER_CHANNELS];
	bool sample_valid;
	bool suspended;
};

static bool hall_slider_in_range(struct hall_slider *slider, unsigned int channel, int value)
{
	if (!slider->have_limits[channel])
		return true;
	/* Full-scale output cannot distinguish a detent from magnetic saturation. */
	return value > slider->limits[channel][0] && value < slider->limits[channel][1];
}

static int hall_slider_read(struct hall_slider *slider, int raw[HALL_SLIDER_CHANNELS])
{
	unsigned int i;
	int ret;

	for (i = 0; i < HALL_SLIDER_CHANNELS; i++) {
		ret = iio_read_channel_raw(slider->channels[i], &raw[i]);
		if (ret < 0)
			return ret;
		if (!hall_slider_in_range(slider, i, raw[i]))
			return -ERANGE;
	}
	return 0;
}

static void hall_slider_poll(struct input_dev *input)
{
	struct hall_slider *slider = input_get_drvdata(input);
	int raw[HALL_SLIDER_CHANNELS], position, ret;

	mutex_lock(&slider->lock);
	if (slider->suspended)
		goto out;
	ret = hall_slider_read(slider, raw);
	if (ret) {
		position = HALL_SLIDER_UNKNOWN;
		if (ret != -EAGAIN && ret != -ERANGE)
			dev_warn_ratelimited(input->dev.parent, "Hall read failed: %d\n", ret);
	} else {
		position = hall_slider_classify(raw, slider->state.position,
						&slider->parameters, &slider->calibration);
	}
	slider->sample_valid = position != HALL_SLIDER_UNKNOWN;
	if (hall_slider_update(&slider->state, position, jiffies, slider->debounce)) {
		input_report_abs(input, slider->axis, slider->state.position);
		input_sync(input);
		sysfs_notify(&input->dev.parent->kobj, NULL, "position");
	}
 out:
	mutex_unlock(&slider->lock);
}

static ssize_t position_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hall_slider *slider = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&slider->lock);
	ret = sysfs_emit(buf, "%d\n", slider->state.position);
	mutex_unlock(&slider->lock);
	return ret;
}
static DEVICE_ATTR_RO(position);

static ssize_t sample_valid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hall_slider *slider = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&slider->lock);
	ret = sysfs_emit(buf, "%u\n", slider->sample_valid);
	mutex_unlock(&slider->lock);
	return ret;
}
static DEVICE_ATTR_RO(sample_valid);

static ssize_t hall_raw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hall_slider *slider = dev_get_drvdata(dev);
	int raw[HALL_SLIDER_CHANNELS], ret;

	mutex_lock(&slider->lock);
	ret = slider->suspended ? -EBUSY : hall_slider_read(slider, raw);
	if (!ret)
		ret = sysfs_emit(buf, "%d %d\n", raw[0], raw[1]);
	mutex_unlock(&slider->lock);
	return ret;
}
static DEVICE_ATTR_RO(hall_raw);

static ssize_t calibration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hall_slider *slider = dev_get_drvdata(dev);
	struct hall_slider_calibration *cal = &slider->calibration;
	ssize_t ret;

	mutex_lock(&slider->lock);
	if (!cal->valid)
		ret = sysfs_emit(buf, "uncalibrated\n");
	else
		ret = sysfs_emit(buf, "%d %d %d %d %d %d\n", cal->raw[0][0], cal->raw[0][1],
				 cal->raw[1][0], cal->raw[1][1], cal->raw[2][0], cal->raw[2][1]);
	mutex_unlock(&slider->lock);
	return ret;
}

static ssize_t calibration_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct hall_slider *slider = dev_get_drvdata(dev);
	struct hall_slider_calibration cal = { };
	char *text, *cursor, *token;
	unsigned int n = 0;
	int ret = 0;

	if (!sysfs_streq(buf, "reset")) {
		text = kmemdup_nul(buf, count, GFP_KERNEL);
		if (!text)
			return -ENOMEM;
		cursor = text;
		while ((token = strsep(&cursor, " ,\t\n"))) {
			if (!*token)
				continue;
			if (n == HALL_SLIDER_POSITIONS * HALL_SLIDER_CHANNELS) {
				ret = -EINVAL;
				goto free;
			}
			ret = kstrtoint(token, 10, &cal.raw[n / 2][n % 2]);
			if (ret)
				goto free;
			if (!hall_slider_in_range(slider, n % 2, cal.raw[n / 2][n % 2])) {
				ret = -ERANGE;
				goto free;
			}
			n++;
		}
		ret = n == 6 ? hall_slider_validate_calibration(&cal, &slider->parameters) :
			      -EINVAL;
 free:
		kfree(text);
		if (ret)
			return ret;
		cal.valid = true;
	}
	mutex_lock(&slider->lock);
	if (slider->suspended) {
		mutex_unlock(&slider->lock);
		return -EBUSY;
	}
	slider->calibration = cal;
	slider->state.pending = HALL_SLIDER_UNKNOWN;
	slider->sample_valid = false;
	mutex_unlock(&slider->lock);
	return count;
}
static DEVICE_ATTR_RW(calibration);

static struct attribute *hall_slider_attrs[] = {
	&dev_attr_position.attr,
	&dev_attr_sample_valid.attr,
	&dev_attr_hall_raw.attr,
	&dev_attr_calibration.attr,
	NULL,
};

static const struct attribute_group hall_slider_group = {
	.attrs = hall_slider_attrs,
};

static int hall_slider_get_channel(struct device *dev, struct hall_slider *slider,
				   unsigned int index, const char *name)
{
	const int *range;
	enum iio_chan_type channel_type;
	int ret, type, length;

	slider->channels[index] = devm_iio_channel_get(dev, name);
	if (IS_ERR(slider->channels[index]))
		return dev_err_probe(dev, PTR_ERR(slider->channels[index]),
				     "cannot get %s channel\n", name);
	ret = iio_get_channel_type(slider->channels[index], &channel_type);
	if (ret)
		return ret;
	if (channel_type != IIO_MAGN)
		return dev_err_probe(dev, -EINVAL, "%s is not a magnetic channel\n", name);
	ret = iio_read_avail_channel_attribute(slider->channels[index], &range,
					      &type, &length, IIO_CHAN_INFO_RAW);
	if (ret == IIO_AVAIL_RANGE && type == IIO_VAL_INT && length == 3) {
		if (range[0] >= range[2])
			return -EINVAL;
		slider->limits[index][0] = range[0];
		slider->limits[index][1] = range[2];
		slider->have_limits[index] = true;
	}
	return 0;
}

static int hall_slider_open(struct input_dev *input)
{
	struct hall_slider *slider = input_get_drvdata(input);

	mutex_lock(&slider->lock);
	slider->state.pending = HALL_SLIDER_UNKNOWN;
	slider->sample_valid = false;
	mutex_unlock(&slider->lock);
	return 0;
}

static void hall_slider_close(struct input_dev *input)
{
	struct hall_slider *slider = input_get_drvdata(input);

	mutex_lock(&slider->lock);
	slider->state.pending = HALL_SLIDER_UNKNOWN;
	slider->sample_valid = false;
	mutex_unlock(&slider->lock);
}

static int hall_slider_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hall_slider *slider;
	unsigned int poll_ms = 100, debounce_ms = 100;
	int ret;

	slider = devm_kzalloc(dev, sizeof(*slider), GFP_KERNEL);
	if (!slider)
		return -ENOMEM;
	mutex_init(&slider->lock);
	slider->state.position = HALL_SLIDER_UNKNOWN;
	slider->state.pending = HALL_SLIDER_UNKNOWN;
	platform_set_drvdata(pdev, slider);
	ret = hall_slider_get_channel(dev, slider, 0, "end-0");
	if (ret)
		return ret;
	ret = hall_slider_get_channel(dev, slider, 1, "end-2");
	if (ret)
		return ret;
	if (slider->channels[0]->indio_dev == slider->channels[1]->indio_dev &&
	    slider->channels[0]->channel == slider->channels[1]->channel)
		return dev_err_probe(dev, -EINVAL, "selector channels must be distinct\n");
	/* Counts describe magnetic geometry; do not encode board defaults here. */
	ret = device_property_read_u32(dev, "position-threshold", &slider->parameters.threshold);
	if (ret)
		return dev_err_probe(dev, ret, "missing position-threshold\n");
	ret = device_property_read_u32(dev, "hysteresis", &slider->parameters.hysteresis);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "interference-threshold", &slider->parameters.interference);
	if (ret)
		return ret;
	ret = device_property_read_u32(dev, "linux,axis", &slider->axis);
	if (ret)
		return ret;
	device_property_read_u32(dev, "poll-interval", &poll_ms);
	device_property_read_u32(dev, "debounce-interval", &debounce_ms);
	if (!slider->parameters.threshold ||
	    slider->parameters.hysteresis >= slider->parameters.threshold ||
	    !slider->parameters.interference || slider->axis >= ABS_MT_SLOT ||
	    poll_ms < 50 || poll_ms > 1000 || debounce_ms > 5000)
		return dev_err_probe(dev, -EINVAL, "invalid selector parameters\n");
	slider->debounce = msecs_to_jiffies(debounce_ms);

	slider->input = devm_input_allocate_device(dev);
	if (!slider->input)
		return -ENOMEM;
	slider->input->name = "Dual Hall Slider";
	slider->input->phys = devm_kasprintf(dev, GFP_KERNEL, "%s/input0", dev_name(dev));
	if (!slider->input->phys)
		return -ENOMEM;
	slider->input->id.bustype = BUS_HOST;
	slider->input->open = hall_slider_open;
	slider->input->close = hall_slider_close;
	input_set_drvdata(slider->input, slider);
	input_set_abs_params(slider->input, slider->axis, HALL_SLIDER_UNKNOWN, 2, 0, 0);
	input_abs_set_val(slider->input, slider->axis, HALL_SLIDER_UNKNOWN);
	ret = input_setup_polling(slider->input, hall_slider_poll);
	if (ret)
		return ret;
	input_set_poll_interval(slider->input, poll_ms);
	input_set_min_poll_interval(slider->input, 50);
	input_set_max_poll_interval(slider->input, 1000);
	ret = input_register_device(slider->input);
	if (ret)
		return ret;
	return devm_device_add_group(dev, &hall_slider_group);
}

static int hall_slider_suspend(struct device *dev)
{
	struct hall_slider *slider = dev_get_drvdata(dev);

	mutex_lock(&slider->lock);
	slider->suspended = true;
	slider->sample_valid = false;
	slider->state.pending = HALL_SLIDER_UNKNOWN;
	mutex_unlock(&slider->lock);
	return 0;
}

static int hall_slider_resume(struct device *dev)
{
	struct hall_slider *slider = dev_get_drvdata(dev);

	mutex_lock(&slider->lock);
	slider->suspended = false;
	slider->state.pending = HALL_SLIDER_UNKNOWN;
	mutex_unlock(&slider->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(hall_slider_pm_ops, hall_slider_suspend, hall_slider_resume);

static const struct of_device_id hall_slider_of_match[] = {
	{ .compatible = "dual-hall-slider" },
	{ }
};
MODULE_DEVICE_TABLE(of, hall_slider_of_match);

static struct platform_driver hall_slider_driver = {
	.probe = hall_slider_probe,
	.driver = {
		.name = "dual-hall-slider",
		.of_match_table = hall_slider_of_match,
		.pm = pm_sleep_ptr(&hall_slider_pm_ops),
	},
};
module_platform_driver(hall_slider_driver);

MODULE_DESCRIPTION("Generic three-position selector from two IIO Hall channels");
MODULE_LICENSE("GPL");
