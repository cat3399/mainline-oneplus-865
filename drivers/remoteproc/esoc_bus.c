// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2015, 2017-2018, 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/idr.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include "esoc.h"

static DEFINE_IDA(esoc_ida);

/* SYSFS */
static ssize_t esoc_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, ESOC_NAME_LEN, "%s", to_esoc_clink(dev)->name);
}

static ssize_t esoc_link_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, ESOC_LINK_LEN, "%s", to_esoc_clink(dev)->link_name);
}

static ssize_t esoc_link_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, ESOC_LINK_LEN, "%s", to_esoc_clink(dev)->link_info);
}

static DEVICE_ATTR_RO(esoc_name);
static DEVICE_ATTR_RO(esoc_link);
static DEVICE_ATTR_RO(esoc_link_info);

static ssize_t modalias_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct esoc_clink *esoc = to_esoc_clink(dev);
	int len;

	/*
	 * Userspace rmtfs on postmarketOS only recognizes modem remoteprocs
	 * through the legacy qcom-q6v5-mss modalias path. Apollo exposes the
	 * modem through the ESOC bus, so provide a compatible OF modalias for
	 * the external SDX55M modem until userspace learns the newer ESOC
	 * layout.
	 */
	if (esoc->np && of_device_is_compatible(esoc->np, "qcom,ext-sdx55m"))
		return sysfs_emit(buf,
				  "of:Nqcom,ext-sdx55m-mss-pilT(null)Cqcom,ext-sdx55m-mss-pil\n");

	len = of_device_modalias(dev, buf, PAGE_SIZE);
	if (len != -ENODEV)
		return len;

	return sysfs_emit(buf, "esoc:%s\n", esoc->name);
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *esoc_clink_attrs[] = {
	&dev_attr_esoc_name.attr,
	&dev_attr_esoc_link.attr,
	&dev_attr_esoc_link_info.attr,
	&dev_attr_modalias.attr,
	NULL
};

static struct attribute_group esoc_clink_attr_group = {
	.attrs = esoc_clink_attrs,
};

const struct attribute_group *esoc_clink_attr_groups[] = {
	&esoc_clink_attr_group,
	NULL,
};

static int esoc_bus_match(struct device *dev, const struct device_driver *drv)
{
	int i = 0, match = 1;
	struct esoc_clink *esoc_clink = to_esoc_clink(dev);
	struct esoc_drv *esoc_drv = to_esoc_drv(drv);
	int entries = esoc_drv->compat_entries;
	struct esoc_compat *table = esoc_drv->compat_table;

	for (i = 0; i < entries; i++) {
		if (strcasecmp(esoc_clink->name, table[i].name) == 0)
			return match;
	}
	return 0;
}

void *esoc_device_get_match_data(struct device *dev)
{
	int i = 0;
	struct esoc_clink *esoc_clink = to_esoc_clink(dev);
	struct esoc_drv *esoc_drv = to_esoc_drv(dev->driver);
	int entries = esoc_drv->compat_entries;
	struct esoc_compat *table = esoc_drv->compat_table;

	for (i = 0; i < entries; i++) {
		if (strcasecmp(esoc_clink->name, table[i].name) == 0)
			return table[i].data;
	}

	return ERR_PTR(-EINVAL);
}

static int esoc_bus_probe(struct device *dev)
{
	int ret;
	struct esoc_clink *esoc_clink = to_esoc_clink(dev);
	struct esoc_drv *esoc_drv = to_esoc_drv(dev->driver);

	ret = esoc_drv->probe(esoc_clink, esoc_drv);
	if (ret)
		pr_err("failed to probe %s dev\n", esoc_clink->name);
	return ret;
}

static int esoc_bus_remove(struct device *dev)
{
	int ret;
	struct esoc_clink *esoc_clink = to_esoc_clink(dev);
	struct esoc_drv *esoc_drv = to_esoc_drv(dev->driver);

	ret = esoc_drv->remove(esoc_clink, esoc_drv);
	if (ret)
		pr_err("failed to remove %s dev\n", esoc_clink->name);
	return ret;
}

static int esoc_bus_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	struct esoc_clink *esoc = to_esoc_clink(dev);
	int ret;

	if (esoc->np && of_device_is_compatible(esoc->np, "qcom,ext-sdx55m"))
		return add_uevent_var(env,
				      "MODALIAS=of:Nqcom,ext-sdx55m-mss-pilT(null)Cqcom,ext-sdx55m-mss-pil");

	ret = of_device_uevent_modalias(dev, env);
	if (ret != -ENODEV)
		return ret;

	return add_uevent_var(env, "MODALIAS=esoc:%s", esoc->name);
}

struct bus_type esoc_bus_type = {
	.name = "esoc",
	.match = esoc_bus_match,
	.dev_groups = esoc_clink_attr_groups,
	.uevent = esoc_bus_uevent,
};

struct device esoc_bus = {
	.init_name = "esoc-bus"
};

/* bus accessor */
static void esoc_clink_release(struct device *dev)
{
	struct esoc_clink *esoc_clink = to_esoc_clink(dev);

	ida_simple_remove(&esoc_ida, esoc_clink->id);
	kfree(esoc_clink);
}

static int esoc_clink_match_id(struct device *dev, const void *id)
{
	struct esoc_clink *esoc_clink = to_esoc_clink(dev);
	int *esoc_id = (int *)id;

	if (esoc_clink->id == *esoc_id) {
		if (!try_module_get(esoc_clink->owner))
			return 0;
		return 1;
	}
	return 0;
}

static int esoc_clink_match_node(struct device *dev, const void *id)
{
	struct esoc_clink *esoc_clink = to_esoc_clink(dev);

	if (esoc_clink->np == id) {
		if (!try_module_get(esoc_clink->owner))
			return 0;
		return 1;
	}
	return 0;
}

void esoc_for_each_dev(void *data, esoc_func_t fn)
{
	bus_for_each_dev(&esoc_bus_type, NULL, data, fn);
}


struct esoc_clink *get_esoc_clink(int id)
{
	struct esoc_clink *esoc_clink;
	struct device *dev;

	dev = bus_find_device(&esoc_bus_type, NULL, &id, esoc_clink_match_id);
	if (IS_ERR_OR_NULL(dev))
		return NULL;
	esoc_clink = to_esoc_clink(dev);
	return esoc_clink;
}

struct esoc_clink *get_esoc_clink_by_node(struct device_node *node)
{
	struct esoc_clink *esoc_clink;
	struct device *dev;

	dev = bus_find_device(&esoc_bus_type, NULL, node, esoc_clink_match_node);
	if (IS_ERR_OR_NULL(dev))
		return NULL;
	esoc_clink = to_esoc_clink(dev);
	return esoc_clink;
}

void put_esoc_clink(struct esoc_clink *esoc_clink)
{
	module_put(esoc_clink->owner);
}

bool esoc_req_eng_enabled(struct esoc_clink *esoc_clink)
{
	return !esoc_clink->req_eng ? false : true;
}

bool esoc_cmd_eng_enabled(struct esoc_clink *esoc_clink)
{
	return !esoc_clink->cmd_eng ? false : true;
}
/* ssr operations */
int esoc_clink_register_rproc(struct esoc_clink *esoc_clink)
{
	int ret;
	int len;
	char *rproc_name;

	len = strlen("esoc") + sizeof(esoc_clink->id);
	rproc_name = kzalloc(len, GFP_KERNEL);
	if (IS_ERR_OR_NULL(rproc_name))
		return PTR_ERR(rproc_name);

	snprintf(rproc_name, len, "esoc%d", esoc_clink->id);
	esoc_clink->dev.of_node = esoc_clink->np;
	esoc_clink->rproc = rproc_alloc(&esoc_clink->dev, rproc_name,
					&esoc_clink->ops, esoc_clink->fw, 0);
	if (!esoc_clink->rproc) {
		dev_err(&esoc_clink->dev, "unable to allocate remoteproc\n");
		ret = -ENOMEM;
		goto rproc_err;
	}

	esoc_clink->rproc->recovery_disabled = false;
	esoc_clink->rproc->auto_boot = esoc_clink->auto_boot;

	dev_info(&esoc_clink->dev,
		 "esoc: adding sysmon subdev name=%s ssctl_id=0x%x\n",
		 esoc_clink->sysmon_name, esoc_clink->ssctl_id);
	esoc_clink->rproc_sysmon = qcom_add_sysmon_subdev(esoc_clink->rproc,
							  esoc_clink->sysmon_name,
							  esoc_clink->ssctl_id);
	if (IS_ERR(esoc_clink->rproc_sysmon)) {
		dev_err(&esoc_clink->dev, "Failed to register sysmon\n");
		ret = PTR_ERR(esoc_clink->rproc_sysmon);
		goto rproc_err;
	}
	dev_info(&esoc_clink->dev, "esoc: sysmon subdev added OK\n");

	ret = rproc_add(esoc_clink->rproc);
	if (ret) {
		dev_err(&esoc_clink->dev, "unable to add remoteproc\n");
		goto remove_subdev;
	}

	return 0;

remove_subdev:
	if (esoc_clink->rproc_sysmon)
		qcom_remove_sysmon_subdev(esoc_clink->rproc_sysmon);
rproc_err:
	kfree(rproc_name);
	return ret;
}

void esoc_clink_unregister_rproc(struct esoc_clink *esoc_clink)
{
	rproc_del(esoc_clink->rproc);
	if (esoc_clink->rproc_sysmon)
		qcom_remove_sysmon_subdev(esoc_clink->rproc_sysmon);
	rproc_free(esoc_clink->rproc);
}

int esoc_clink_request_ssr(struct esoc_clink *esoc_clink)
{
	if (esoc_clink->rproc->recovery_disabled) {
		dev_err(&esoc_clink->dev,
			"remoteproc %s crashed but recovery is disabled\n",
			esoc_clink->rproc->name);
		return -EOPNOTSUPP;
	}
	rproc_report_crash(esoc_clink->rproc, RPROC_FATAL_ERROR);
	return 0;
}

/* bus operations */
void esoc_clink_evt_notify(enum esoc_evt evt, struct esoc_clink *esoc_clink)
{
	unsigned long flags;

	spin_lock_irqsave(&esoc_clink->notify_lock, flags);
	notify_esoc_clients(esoc_clink, evt);
	if (esoc_clink->req_eng && esoc_clink->req_eng->handle_clink_evt)
		esoc_clink->req_eng->handle_clink_evt(evt, esoc_clink->req_eng);
	if (esoc_clink->cmd_eng && esoc_clink->cmd_eng->handle_clink_evt)
		esoc_clink->cmd_eng->handle_clink_evt(evt, esoc_clink->cmd_eng);
	spin_unlock_irqrestore(&esoc_clink->notify_lock, flags);
}

void *get_esoc_clink_data(struct esoc_clink *esoc)
{
	return esoc->clink_data;
}

void set_esoc_clink_data(struct esoc_clink *esoc, void *data)
{
	esoc->clink_data = data;
}

void esoc_clink_queue_request(enum esoc_req req, struct esoc_clink *esoc_clink)
{
	unsigned long flags;
	struct esoc_eng *req_eng;

	spin_lock_irqsave(&esoc_clink->notify_lock, flags);
	if (esoc_clink->req_eng != NULL) {
		req_eng = esoc_clink->req_eng;
		req_eng->handle_clink_req(req, req_eng);
	}
	spin_unlock_irqrestore(&esoc_clink->notify_lock, flags);
}

void esoc_set_drv_data(struct esoc_clink *esoc_clink, void *data)
{
	dev_set_drvdata(&esoc_clink->dev, data);
}

void *esoc_get_drv_data(struct esoc_clink *esoc_clink)
{
	return dev_get_drvdata(&esoc_clink->dev);
}

/* bus registration functions */
void esoc_clink_unregister(struct esoc_clink *esoc_clink)
{
	if (get_device(&esoc_clink->dev) != NULL) {
		esoc_clink_del_device(&esoc_clink->dev, NULL);
		device_unregister(&esoc_clink->dev);
		put_device(&esoc_clink->dev);
		ida_simple_remove(&esoc_ida, esoc_clink->id);
	}
}

int esoc_clink_register(struct esoc_clink *esoc_clink)
{
	int id, err;
	struct device *dev;

	if (!esoc_clink->name || !esoc_clink->link_name || !esoc_clink->clink_ops) {
		dev_err(esoc_clink->parent, "invalid esoc arguments\n");
		return -EINVAL;
	}
	id = ida_simple_get(&esoc_ida, 0, ESOC_DEV_MAX, GFP_KERNEL);
	if (id < 0) {
		err = id;
		goto exit_ida;
	}
	esoc_clink->id = id;
	dev = &esoc_clink->dev;
	dev->bus = &esoc_bus_type;
	dev->release = esoc_clink_release;
	spin_lock_init(&esoc_clink->notify_lock);
	if (!esoc_clink->parent)
		dev->parent = &esoc_bus;
	else
		dev->parent = esoc_clink->parent;
	dev_set_name(dev, "esoc%d", id);
	err = device_register(dev);
	if (err) {
		dev_err(esoc_clink->parent, "esoc device register failed\n");
		put_device(dev);
		goto exit_ida;
	}

	err = esoc_clink_add_device(&esoc_clink->dev, NULL);
	if (err)
		goto exit_dev_add;

	return 0;
exit_dev_add:
	device_unregister(dev);
exit_ida:
	ida_simple_remove(&esoc_ida, id);
	pr_err("unable to register %s, err = %d\n", esoc_clink->name, err);
	return err;
}

int esoc_clink_register_req_eng(struct esoc_clink *esoc_clink,
						struct esoc_eng *eng)
{
	if (esoc_clink->req_eng)
		return -EBUSY;
	if (!eng->handle_clink_req)
		return -EINVAL;
	esoc_clink->req_eng = eng;
	eng->esoc_clink = esoc_clink;
	esoc_clink_evt_notify(ESOC_REQ_ENG_ON, esoc_clink);
	return 0;
}

int esoc_clink_register_cmd_eng(struct esoc_clink *esoc_clink,
						struct esoc_eng *eng)
{
	if (esoc_clink->cmd_eng)
		return -EBUSY;
	esoc_clink->cmd_eng = eng;
	eng->esoc_clink = esoc_clink;
	esoc_clink_evt_notify(ESOC_CMD_ENG_ON, esoc_clink);
	return 0;
}

void esoc_clink_unregister_req_eng(struct esoc_clink *esoc_clink,
						struct esoc_eng *eng)
{
	esoc_clink->req_eng = NULL;
	esoc_clink_evt_notify(ESOC_REQ_ENG_OFF, esoc_clink);
}

void esoc_clink_unregister_cmd_eng(struct esoc_clink *esoc_clink,
						struct esoc_eng *eng)
{
	esoc_clink->cmd_eng = NULL;
	esoc_clink_evt_notify(ESOC_CMD_ENG_OFF, esoc_clink);
}

int esoc_driver_register(struct esoc_drv *driver)
{
	int ret;

	driver->driver.bus = &esoc_bus_type;
	driver->driver.probe = esoc_bus_probe;
	driver->driver.remove = esoc_bus_remove;
	ret = driver_register(&driver->driver);
	if (ret)
		return ret;
	return 0;
}

void esoc_driver_unregister(struct esoc_drv *driver)
{
	driver_unregister(&driver->driver);
}

int esoc_bus_init(void)
{
	int ret;

	ret = device_register(&esoc_bus);
	if (ret) {
		pr_err("esoc bus device register fail\n");
		return ret;
	}
	ret = bus_register(&esoc_bus_type);
	if (ret) {
		pr_err("esoc bus register fail\n");
		return ret;
	}
	pr_debug("esoc bus registration done\n");
	//TODO: add cleanup path
	ret = esoc_dev_init();
	if (ret) {
		pr_err("esoc userspace driver initialization failed\n");
		return ret;
	}
	ret = mdm_drv_init();
	if (ret) {
		pr_err("esoc failed to initialize ssr driver\n");
		return ret;
	}
	return 0;
}

void esoc_bus_exit(void)
{
	mdm_drv_exit();
	esoc_dev_exit();
	bus_unregister(&esoc_bus_type);
	device_unregister(&esoc_bus);
}

EXPORT_SYMBOL(esoc_bus_init);
EXPORT_SYMBOL(esoc_bus_exit);
EXPORT_SYMBOL(esoc_driver_register);
EXPORT_SYMBOL(esoc_driver_unregister);
EXPORT_SYMBOL(esoc_req_eng_enabled);
EXPORT_SYMBOL(get_esoc_clink_data);
EXPORT_SYMBOL(set_esoc_clink_data);
EXPORT_SYMBOL(esoc_clink_register);
EXPORT_SYMBOL(esoc_clink_unregister);
EXPORT_SYMBOL(esoc_clink_request_ssr);
EXPORT_SYMBOL(esoc_get_drv_data);
EXPORT_SYMBOL(esoc_set_drv_data);
EXPORT_SYMBOL(esoc_clink_register_rproc);
EXPORT_SYMBOL(esoc_clink_unregister_rproc);
EXPORT_SYMBOL(esoc_clink_register_req_eng);
EXPORT_SYMBOL(esoc_clink_unregister_req_eng);
EXPORT_SYMBOL(esoc_clink_register_cmd_eng);
EXPORT_SYMBOL(esoc_clink_unregister_cmd_eng);
EXPORT_SYMBOL(esoc_clink_queue_request);
EXPORT_SYMBOL(esoc_clink_evt_notify);
EXPORT_SYMBOL(esoc_device_get_match_data);
EXPORT_SYMBOL(get_esoc_clink);
EXPORT_SYMBOL(put_esoc_clink);
EXPORT_SYMBOL(esoc_for_each_dev);
EXPORT_SYMBOL(get_esoc_clink_by_node);

MODULE_LICENSE("GPL");
