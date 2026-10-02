// SPDX-License-Identifier: GPL-2.0-only
/*
 * Conservative MP2650 charger support.
 *
 * The register definitions and programming model are based on the downstream
 * OnePlus MP2650 driver used on SM8250 devices.  Charge control is deliberately
 * opt-in while the driver is being brought up on mainline: loading the module
 * only exposes read-only telemetry unless enable_control=1 is requested.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define MP2650_REG_INPUT_CURRENT_1	0x00
#define MP2650_REG_INPUT_VOLTAGE		0x01
#define MP2650_REG_CHARGE_CURRENT	0x02
#define MP2650_REG_PRECHARGE_CURRENT	0x03
#define MP2650_REG_PRECHARGE_VOLTAGE	0x07
#define MP2650_REG_CONTROL		0x08
#define MP2650_REG_TIMER		0x09
#define MP2650_REG_STATUS		0x13
#define MP2650_REG_FAULT		0x14
#define MP2650_REG_DEVICE_ID		0x15
#define MP2650_REG_VBUS_ADC_L		0x1c
#define MP2650_REG_IBUS_ADC_L		0x1e
#define MP2650_REG_INPUT_CURRENT_2	0x0f
#define MP2650_REG_PRECHARGE_OPTION	0x30
#define MP2650_MAX_REGISTER		0x53

#define MP2650_INPUT_CURRENT_MASK	GENMASK(6, 0)
#define MP2650_INPUT_CURRENT_STEP_UA	50000
#define MP2650_CHARGE_CURRENT_STEP_UA	50000
#define MP2650_PRECHARGE_CURRENT_MASK	GENMASK(7, 4)
#define MP2650_PRECHARGE_CURRENT_480MA	9
#define MP2650_PRECHARGE_VOLTAGE_MASK	GENMASK(5, 4)
#define MP2650_PRECHARGE_VOLTAGE_CODE	3
#define MP2650_PRECHARGE_OPTION_MASK	BIT(3)
#define MP2650_CHARGE_ENABLE		BIT(4)
#define MP2650_OTG_ENABLE		BIT(5)
#define MP2650_CHARGE_TIMER_ENABLE	BIT(3)
#define MP2650_WATCHDOG_MASK		GENMASK(5, 4)
#define MP2650_CHARGE_STATUS_MASK	GENMASK(3, 2)
#define MP2650_CHARGE_PRECHARGE		BIT(2)
#define MP2650_CHARGE_FAST		BIT(3)
#define MP2650_CHARGE_DONE		GENMASK(3, 2)
#define MP2650_BATTERY_FAULT		BIT(3)
#define MP2650_CHARGE_FAULT_MASK	GENMASK(5, 4)
#define MP2650_CHARGE_FAULT_INPUT	BIT(4)
#define MP2650_CHARGE_FAULT_THERMAL	BIT(5)
#define MP2650_CHARGE_FAULT_TIMER	GENMASK(5, 4)
#define MP2650_WATCHDOG_FAULT		BIT(7)
#define MP2650_NTC_FAULT_MASK		GENMASK(2, 0)
#define MP2650_NTC_COLD			1
#define MP2650_NTC_COOL			2
#define MP2650_NTC_WARM			3
#define MP2650_NTC_HOT			4

#define MP2650_DEFAULT_INPUT_MAX_UA	2000000
#define MP2650_DEFAULT_CHARGE_MAX_UA	1100000
#define MP2650_SAFE_FALLBACK_UA		500000
#define MP2650_MIN_VBUS_UV		4500000
#define MP2650_MAX_BATTERY_UV		8900000
#define MP2650_CONTROL_INTERVAL		(10 * HZ)
#define MP2650_MONITOR_INTERVAL		(30 * HZ)

static bool enable_control;
module_param(enable_control, bool, 0600);
MODULE_PARM_DESC(enable_control,
		 "Enable conservative source-aware charging control (default: false)");

static unsigned int input_current_max_ua;
module_param(input_current_max_ua, uint, 0600);
MODULE_PARM_DESC(input_current_max_ua,
		 "Override the firmware input-current limit in microamps");

static unsigned int charge_current_max_ua;
module_param(charge_current_max_ua, uint, 0600);
MODULE_PARM_DESC(charge_current_max_ua,
		 "Override the firmware battery charge-current limit in microamps");

struct mp2650_device {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply *input;
	struct power_supply *battery;
	struct power_supply_desc psy_desc;
	struct notifier_block psy_nb;
	struct delayed_work control_work;
	struct mutex lock; /* Protects charging control updates. */
	unsigned int input_max_ua;
	unsigned int charge_max_ua;
	enum power_supply_charge_behaviour charge_behaviour;
	bool control_default;
	bool control_active;
	bool charge_inhibited;
	bool safety_timer_suspended;
};

static const struct regmap_config mp2650_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MP2650_MAX_REGISTER,
	.cache_type = REGCACHE_NONE,
};

static unsigned int mp2650_effective_input_max(struct mp2650_device *mp)
{
	unsigned int limit = READ_ONCE(input_current_max_ua);

	if (!limit)
		return mp->input_max_ua;

	return clamp_val(limit, MP2650_SAFE_FALLBACK_UA, 3000000);
}

static unsigned int mp2650_effective_charge_max(struct mp2650_device *mp)
{
	unsigned int limit = READ_ONCE(charge_current_max_ua);

	if (!limit)
		return mp->charge_max_ua;

	return clamp_val(limit, 100000, 3000000);
}

static int mp2650_read_adc(struct mp2650_device *mp, unsigned int reg,
			   unsigned int step_uv_or_ua, int *value)
{
	u8 data[2];
	unsigned int raw;
	int ret;

	ret = regmap_bulk_read(mp->regmap, reg, data, sizeof(data));
	if (ret)
		return ret;

	/* MP2650 stores these ten-bit ADC values low byte first. */
	raw = ((unsigned int)data[1] << 8) | data[0];
	raw >>= 6;
	*value = raw * step_uv_or_ua;

	return 0;
}

static int mp2650_get_vbus(struct mp2650_device *mp, int *uv)
{
	return mp2650_read_adc(mp, MP2650_REG_VBUS_ADC_L, 25000, uv);
}

static int mp2650_get_ibus(struct mp2650_device *mp, int *ua)
{
	return mp2650_read_adc(mp, MP2650_REG_IBUS_ADC_L, 6250, ua);
}

static int mp2650_get_input_limit(struct mp2650_device *mp, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(mp->regmap, MP2650_REG_INPUT_CURRENT_1, &val);
	if (!ret)
		*ua = FIELD_GET(MP2650_INPUT_CURRENT_MASK, val) *
		      MP2650_INPUT_CURRENT_STEP_UA;

	return ret;
}

static int mp2650_get_charge_current(struct mp2650_device *mp, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(mp->regmap, MP2650_REG_CHARGE_CURRENT, &val);
	if (!ret)
		*ua = val * MP2650_CHARGE_CURRENT_STEP_UA;

	return ret;
}

static int mp2650_set_input_limit(struct mp2650_device *mp, int ua)
{
	unsigned int val;
	int ret;

	ua = clamp_val(ua, MP2650_INPUT_CURRENT_STEP_UA,
		       FIELD_MAX(MP2650_INPUT_CURRENT_MASK) *
		       MP2650_INPUT_CURRENT_STEP_UA);
	val = ua / MP2650_INPUT_CURRENT_STEP_UA;

	ret = regmap_update_bits(mp->regmap, MP2650_REG_INPUT_CURRENT_1,
				 MP2650_INPUT_CURRENT_MASK, val);
	if (ret)
		return ret;

	return regmap_update_bits(mp->regmap, MP2650_REG_INPUT_CURRENT_2,
				  MP2650_INPUT_CURRENT_MASK, val);
}

static int mp2650_set_charge_current(struct mp2650_device *mp, int ua)
{
	unsigned int val;

	ua = clamp_val(ua, 0,
		       U8_MAX * MP2650_CHARGE_CURRENT_STEP_UA);
	val = ua / MP2650_CHARGE_CURRENT_STEP_UA;

	return regmap_write(mp->regmap, MP2650_REG_CHARGE_CURRENT, val);
}

static int mp2650_configure_precharge(struct mp2650_device *mp)
{
	int ret;

	/*
	 * Match the OnePlus hardware_init sequence, including the threshold
	 * option bank.  REG02 only controls fast charge; leaving REG03 at its
	 * reset value limits a depleted battery to the 180 mA precharge tier.
	 * MP2762A encodes 480 mA as 9, not 480 / 60 (the table is nonlinear).
	 * Preserve the termination-current and OTG-current fields.
	 */
	ret = regmap_update_bits(mp->regmap, MP2650_REG_PRECHARGE_CURRENT,
				 MP2650_PRECHARGE_CURRENT_MASK,
				 FIELD_PREP(MP2650_PRECHARGE_CURRENT_MASK,
					    MP2650_PRECHARGE_CURRENT_480MA));
	if (ret)
		return ret;

	ret = regmap_update_bits(mp->regmap, MP2650_REG_PRECHARGE_VOLTAGE,
				 MP2650_PRECHARGE_VOLTAGE_MASK,
				 FIELD_PREP(MP2650_PRECHARGE_VOLTAGE_MASK,
					    MP2650_PRECHARGE_VOLTAGE_CODE));
	if (ret)
		return ret;

	/*
	 * Select option 1, as downstream mp2650_set_option(1) does.  On
	 * MP2762A, voltage code 3 then selects the 6.4 V threshold bank.
	 * With option 2 inherited from firmware, the same code needs about
	 * 7.4 V on the rising edge, although its falling threshold is 7.2 V.
	 * The hardware still selects precharge for a deeply depleted pack.
	 */
	return regmap_update_bits(mp->regmap, MP2650_REG_PRECHARGE_OPTION,
				  MP2650_PRECHARGE_OPTION_MASK, 0);
}

static int mp2650_set_charge_enabled(struct mp2650_device *mp, bool enabled)
{
	return regmap_update_bits(mp->regmap, MP2650_REG_CONTROL,
				  MP2650_CHARGE_ENABLE,
				  enabled ? MP2650_CHARGE_ENABLE : 0);
}

static int mp2650_set_safety_timer(struct mp2650_device *mp, bool enabled)
{
	int ret;

	ret = regmap_update_bits(mp->regmap, MP2650_REG_TIMER,
				 MP2650_CHARGE_TIMER_ENABLE,
				 enabled ? MP2650_CHARGE_TIMER_ENABLE : 0);
	if (!ret)
		mp->safety_timer_suspended = !enabled;

	return ret;
}

static int mp2650_set_vindpm(struct mp2650_device *mp, int source_uv)
{
	int vindpm_uv;

	/* Match the downstream 4.4 V and 8.5 V conservative thresholds. */
	vindpm_uv = source_uv >= 7000000 ? source_uv - 500000 : 4400000;
	vindpm_uv = clamp_val(vindpm_uv, 0, 25500000);

	return regmap_write(mp->regmap, MP2650_REG_INPUT_VOLTAGE,
			    vindpm_uv / 100000);
}

static int mp2650_set_fallback(struct mp2650_device *mp)
{
	int ret;

	/* Restore timeout protection before requesting battery current again. */
	ret = mp2650_set_safety_timer(mp, true);
	if (ret)
		return ret;

	ret = mp2650_set_input_limit(mp, MP2650_SAFE_FALLBACK_UA);
	if (ret)
		return ret;

	return mp2650_set_charge_current(mp, MP2650_SAFE_FALLBACK_UA);
}

static int mp2650_psy_get_status(struct mp2650_device *mp,
				 union power_supply_propval *val)
{
	unsigned int status;
	int ret;

	if (READ_ONCE(mp->charge_behaviour) ==
	    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}

	ret = regmap_read(mp->regmap, MP2650_REG_STATUS, &status);
	if (ret)
		return ret;

	switch (status & MP2650_CHARGE_STATUS_MASK) {
	case MP2650_CHARGE_PRECHARGE:
	case MP2650_CHARGE_FAST:
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case MP2650_CHARGE_DONE:
		val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	default:
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	}

	return 0;
}

static int mp2650_psy_get_charge_type(struct mp2650_device *mp,
				      union power_supply_propval *val)
{
	unsigned int status;
	int ret;

	if (READ_ONCE(mp->charge_behaviour) ==
	    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
		val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		return 0;
	}

	ret = regmap_read(mp->regmap, MP2650_REG_STATUS, &status);
	if (ret)
		return ret;

	switch (status & MP2650_CHARGE_STATUS_MASK) {
	case MP2650_CHARGE_PRECHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case MP2650_CHARGE_FAST:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	default:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	}

	return 0;
}

static int mp2650_psy_get_health(struct mp2650_device *mp,
				 union power_supply_propval *val)
{
	unsigned int fault;
	int ret;

	ret = regmap_read(mp->regmap, MP2650_REG_FAULT, &fault);
	if (ret)
		return ret;

	if (fault & MP2650_WATCHDOG_FAULT)
		val->intval = POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE;
	else if ((fault & MP2650_CHARGE_FAULT_MASK) ==
		 MP2650_CHARGE_FAULT_TIMER)
		val->intval = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
	else if ((fault & MP2650_CHARGE_FAULT_MASK) ==
		 MP2650_CHARGE_FAULT_THERMAL)
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if ((fault & MP2650_CHARGE_FAULT_MASK) ==
		 MP2650_CHARGE_FAULT_INPUT)
		val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
	else if (fault & MP2650_BATTERY_FAULT)
		val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else if ((fault & MP2650_NTC_FAULT_MASK) == MP2650_NTC_COLD)
		val->intval = POWER_SUPPLY_HEALTH_COLD;
	else if ((fault & MP2650_NTC_FAULT_MASK) == MP2650_NTC_COOL)
		val->intval = POWER_SUPPLY_HEALTH_COOL;
	else if ((fault & MP2650_NTC_FAULT_MASK) == MP2650_NTC_WARM)
		val->intval = POWER_SUPPLY_HEALTH_WARM;
	else if ((fault & MP2650_NTC_FAULT_MASK) == MP2650_NTC_HOT)
		val->intval = POWER_SUPPLY_HEALTH_HOT;
	else
		val->intval = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static enum power_supply_property mp2650_psy_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static int mp2650_psy_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct mp2650_device *mp = power_supply_get_drvdata(psy);
	union power_supply_propval input;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		return mp2650_psy_get_status(mp, val);
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		return mp2650_psy_get_charge_type(mp, val);
	case POWER_SUPPLY_PROP_HEALTH:
		return mp2650_psy_get_health(mp, val);
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = power_supply_get_property(mp->input,
						POWER_SUPPLY_PROP_ONLINE, &input);
		if (!ret)
			val->intval = !!input.intval;
		return ret;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return mp2650_get_vbus(mp, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return mp2650_get_ibus(mp, &val->intval);
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return mp2650_get_input_limit(mp, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return mp2650_get_charge_current(mp, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = mp2650_effective_charge_max(mp);
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		mutex_lock(&mp->lock);
		val->intval = mp->charge_behaviour;
		mutex_unlock(&mp->lock);
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Monolithic Power Systems";
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "MP2650";
		return 0;
	default:
		return -EINVAL;
	}
}

static int mp2650_psy_set_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	struct mp2650_device *mp = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR)
		return -EINVAL;

	if (val->intval != POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO &&
	    val->intval != POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE)
		return -EOPNOTSUPP;

	mutex_lock(&mp->lock);
	mp->charge_behaviour = val->intval;
	mutex_unlock(&mp->lock);

	mod_delayed_work(system_freezable_power_efficient_wq,
			 &mp->control_work, 0);

	return 0;
}

static int mp2650_psy_property_is_writeable(struct power_supply *psy,
					    enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR;
}

static void mp2650_control_work(struct work_struct *work)
{
	struct mp2650_device *mp = container_of(to_delayed_work(work),
						 struct mp2650_device,
						 control_work);
	union power_supply_propval prop;
	s64 input_power_uw;
	int source_uv, source_ua, battery_uv, temp;
	int input_limit, charge_limit, fault;
	bool requested, charge_ok = true;
	unsigned long delay = MP2650_MONITOR_INTERVAL;
	int ret;

	requested = enable_control || mp->control_default;

	mutex_lock(&mp->lock);
	if (!requested) {
		if (mp->control_active) {
			if (mp->charge_behaviour ==
			    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
				ret = mp2650_set_charge_enabled(mp, false);
				if (ret)
					goto control_error;
			}
			ret = mp2650_set_fallback(mp);
			if (ret)
				goto control_error;
			dev_info(mp->dev, "charge control disabled; restored 500 mA limits\n");
			mp->control_active = false;
		}

		if (mp->charge_behaviour ==
		    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
			ret = mp2650_set_charge_enabled(mp, false);
			if (ret)
				goto control_error;
			if (!mp->charge_inhibited)
				dev_info(mp->dev, "battery charging inhibited\n");
			mp->charge_inhibited = true;
			goto out_changed;
		}

		if (mp->safety_timer_suspended) {
			ret = mp2650_set_safety_timer(mp, true);
			if (ret)
				goto control_error;
		}
		if (mp->charge_inhibited) {
			ret = mp2650_set_charge_enabled(mp, true);
			if (ret)
				goto control_error;
			dev_info(mp->dev, "automatic battery charging restored\n");
			mp->charge_inhibited = false;
			goto out_changed;
		}
		goto out_unlock;
	}

	ret = power_supply_get_property(mp->input, POWER_SUPPLY_PROP_ONLINE,
					&prop);
	if (ret || !prop.intval)
		goto offline;

	ret = power_supply_get_property(mp->input, POWER_SUPPLY_PROP_VOLTAGE_NOW,
					&prop);
	if (ret)
		goto property_error;
	source_uv = prop.intval;

	ret = power_supply_get_property(mp->input, POWER_SUPPLY_PROP_CURRENT_MAX,
					&prop);
	if (ret)
		goto property_error;
	source_ua = prop.intval;

	ret = power_supply_get_property(mp->battery, POWER_SUPPLY_PROP_VOLTAGE_NOW,
					&prop);
	if (ret)
		goto property_error;
	battery_uv = prop.intval;

	ret = power_supply_get_property(mp->battery, POWER_SUPPLY_PROP_TEMP, &prop);
	if (ret)
		goto property_error;
	temp = prop.intval;

	ret = regmap_read(mp->regmap, MP2650_REG_FAULT, &fault);
	if (ret)
		goto property_error;

	if (source_uv < MP2650_MIN_VBUS_UV || battery_uv >= MP2650_MAX_BATTERY_UV ||
	    temp < 0 || temp >= 500 ||
	    (fault & MP2650_BATTERY_FAULT) ||
	    (fault & MP2650_CHARGE_FAULT_MASK) == MP2650_CHARGE_FAULT_THERMAL ||
	    (fault & MP2650_NTC_FAULT_MASK) == MP2650_NTC_COLD ||
	    (fault & MP2650_NTC_FAULT_MASK) == MP2650_NTC_HOT)
		charge_ok = false;

	input_limit = clamp_val(source_ua, MP2650_SAFE_FALLBACK_UA,
				mp2650_effective_input_max(mp));
	input_limit = rounddown(input_limit, MP2650_INPUT_CURRENT_STEP_UA);

	/* Keep at least 15% input-power headroom for conversion and system load. */
	input_power_uw = div_s64((s64)source_uv * input_limit, 1000000);
	charge_limit = div_s64(input_power_uw * 85 * 10000, battery_uv);
	charge_limit = min_t(int, charge_limit,
			     mp2650_effective_charge_max(mp));

	if (temp < 50)
		charge_limit = min(charge_limit, 500000);
	else if (temp >= 450)
		charge_limit = min(charge_limit, 600000);
	else if (temp >= 430)
		charge_limit = min(charge_limit, 1000000);
	else if (temp >= 400)
		charge_limit = min(charge_limit, 1500000);
	charge_limit = rounddown(charge_limit,
				 MP2650_CHARGE_CURRENT_STEP_UA);

	if (charge_limit < 100000)
		charge_ok = false;

	/* Never leave an inherited watchdog running without a kernel kicker. */
	ret = regmap_update_bits(mp->regmap, MP2650_REG_TIMER,
				 MP2650_WATCHDOG_MASK, 0);
	if (ret)
		goto control_error;
	ret = regmap_update_bits(mp->regmap, MP2650_REG_CONTROL,
				 MP2650_OTG_ENABLE, 0);
	if (ret)
		goto control_error;
	ret = mp2650_set_vindpm(mp, source_uv);
	if (ret)
		goto control_error;
	ret = mp2650_set_input_limit(mp, input_limit);
	if (ret)
		goto control_error;
	ret = mp2650_configure_precharge(mp);
	if (ret)
		goto control_error;

	if (mp->charge_behaviour ==
	    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
		/*
		 * Keep the buck converter active to supply VSYS, but request no
		 * battery current.  Clearing CHG_EN stops active regulation and
		 * makes the battery cover load transients even with valid VBUS.
		 * Zero current does not stop the charging safety timer, so suspend
		 * it only after the current has successfully been set to zero.
		 */
		ret = mp2650_set_charge_current(mp, 0);
		if (ret)
			goto control_error;
		ret = mp2650_set_safety_timer(mp, false);
	} else {
		ret = mp2650_set_safety_timer(mp, true);
		if (ret)
			goto control_error;
		if (charge_ok)
			ret = mp2650_set_charge_current(mp, charge_limit);
	}
	if (ret)
		goto control_error;
	ret = mp2650_set_charge_enabled(mp, charge_ok);
	if (ret)
		goto control_error;

	if (!mp->control_active)
		dev_info(mp->dev,
			 "charge control enabled: source=%d mV/%d mA, limits=%d/%d mA\n",
			 source_uv / 1000, source_ua / 1000,
			 input_limit / 1000, charge_limit / 1000);
	if (mp->charge_behaviour ==
	    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
		if (!mp->charge_inhibited)
			dev_info(mp->dev,
				 "battery charging inhibited; input power path remains active\n");
		mp->charge_inhibited = true;
	} else {
		if (mp->charge_inhibited)
			dev_info(mp->dev, "automatic battery charging restored\n");
		mp->charge_inhibited = false;
	}
	mp->control_active = true;
	delay = MP2650_CONTROL_INTERVAL;
	goto out_changed;

offline:
	if (mp->charge_behaviour ==
	    POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
		ret = mp2650_set_charge_enabled(mp, false);
		if (ret)
			goto control_error;
		mp->charge_inhibited = true;
	}
	if (mp->control_active) {
		ret = mp2650_set_fallback(mp);
		if (ret)
			goto control_error;
	}
	if (mp->safety_timer_suspended) {
		ret = mp2650_set_safety_timer(mp, true);
		if (ret)
			goto control_error;
	}
	if (mp->charge_behaviour == POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO &&
	    mp->charge_inhibited) {
		ret = mp2650_set_charge_enabled(mp, true);
		if (ret)
			goto control_error;
		mp->charge_inhibited = false;
	}
	mp->control_active = false;
	goto out_changed;

property_error:
	dev_err_ratelimited(mp->dev, "cannot read charging inputs: %d\n", ret);
	goto out_unlock;

control_error:
	dev_err_ratelimited(mp->dev, "cannot update charging limits: %d\n", ret);
	goto out_unlock;

out_changed:
	power_supply_changed(mp->psy);
out_unlock:
	mutex_unlock(&mp->lock);
	mod_delayed_work(system_freezable_power_efficient_wq,
			 &mp->control_work, delay);
}

static int mp2650_psy_notifier(struct notifier_block *nb,
			       unsigned long event, void *data)
{
	struct mp2650_device *mp = container_of(nb, struct mp2650_device,
						psy_nb);
	struct power_supply *psy = data;

	if (event == PSY_EVENT_PROP_CHANGED &&
	    (psy == mp->input || psy == mp->battery))
		mod_delayed_work(system_freezable_power_efficient_wq,
				 &mp->control_work, 0);

	return NOTIFY_OK;
}

static void mp2650_cleanup(void *data)
{
	struct mp2650_device *mp = data;
	int ret;

	power_supply_unreg_notifier(&mp->psy_nb);
	cancel_delayed_work_sync(&mp->control_work);

	mutex_lock(&mp->lock);
	if (mp->safety_timer_suspended) {
		ret = mp2650_set_safety_timer(mp, true);
		if (ret) {
			dev_err(mp->dev, "failed to restore safety timer: %d\n", ret);
			goto out_unlock;
		}
	}
	if (mp->control_active) {
		ret = mp2650_set_fallback(mp);
		if (ret) {
			dev_err(mp->dev, "failed to restore safe limits: %d\n", ret);
			goto out_unlock;
		}
	}
	if (mp->charge_inhibited) {
		ret = mp2650_set_charge_enabled(mp, true);
		if (ret)
			dev_err(mp->dev, "failed to restore charger: %d\n", ret);
	}
out_unlock:
	mutex_unlock(&mp->lock);
}

static int mp2650_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = {};
	struct device *dev = &client->dev;
	struct mp2650_device *mp;
	unsigned int device_id;
	int ret;

	mp = devm_kzalloc(dev, sizeof(*mp), GFP_KERNEL);
	if (!mp)
		return -ENOMEM;

	mp->dev = dev;
	mp->charge_behaviour = POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;
	mp->input_max_ua = MP2650_DEFAULT_INPUT_MAX_UA;
	mp->charge_max_ua = MP2650_DEFAULT_CHARGE_MAX_UA;
	device_property_read_u32(dev, "input-current-limit-microamp",
				 &mp->input_max_ua);
	device_property_read_u32(dev, "constant-charge-current-max-microamp",
				 &mp->charge_max_ua);
	mp->input_max_ua = clamp_val(mp->input_max_ua,
				     MP2650_SAFE_FALLBACK_UA, 3000000);
	mp->charge_max_ua = clamp_val(mp->charge_max_ua, 100000, 3000000);
	mp->control_default =
		device_property_read_bool(dev, "mps,enable-charge-control");

	mp->regmap = devm_regmap_init_i2c(client, &mp2650_regmap_config);
	if (IS_ERR(mp->regmap))
		return dev_err_probe(dev, PTR_ERR(mp->regmap),
				     "cannot initialize regmap\n");

	ret = regmap_read(mp->regmap, MP2650_REG_DEVICE_ID, &device_id);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read device ID register\n");

	mp->input = devm_power_supply_get_by_phandle(dev, "power-supplies");
	if (IS_ERR(mp->input))
		return dev_err_probe(dev, PTR_ERR(mp->input),
				     "cannot find input power supply\n");
	if (!mp->input)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "input power supply is not ready\n");

	mp->battery =
		devm_power_supply_get_by_phandle(dev, "mps,battery-power-supply");
	if (IS_ERR(mp->battery))
		return dev_err_probe(dev, PTR_ERR(mp->battery),
				     "cannot find battery power supply\n");
	if (!mp->battery)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "battery power supply is not ready\n");

	mutex_init(&mp->lock);
	INIT_DELAYED_WORK(&mp->control_work, mp2650_control_work);

	mp->psy_desc.name = "mp2650-charger";
	mp->psy_desc.type = POWER_SUPPLY_TYPE_MAINS;
	mp->psy_desc.properties = mp2650_psy_properties;
	mp->psy_desc.num_properties = ARRAY_SIZE(mp2650_psy_properties);
	mp->psy_desc.get_property = mp2650_psy_get_property;
	mp->psy_desc.set_property = mp2650_psy_set_property;
	mp->psy_desc.property_is_writeable = mp2650_psy_property_is_writeable;
	mp->psy_desc.charge_behaviours =
		BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
		BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE);
	psy_cfg.drv_data = mp;
	psy_cfg.of_node = dev->of_node;
	mp->psy = devm_power_supply_register(dev, &mp->psy_desc, &psy_cfg);
	if (IS_ERR(mp->psy))
		return dev_err_probe(dev, PTR_ERR(mp->psy),
				     "cannot register power supply\n");

	mp->psy_nb.notifier_call = mp2650_psy_notifier;
	ret = power_supply_reg_notifier(&mp->psy_nb);
	if (ret)
		return dev_err_probe(dev, ret,
				     "cannot register power supply notifier\n");

	ret = devm_add_action_or_reset(dev, mp2650_cleanup, mp);
	if (ret)
		return ret;

	i2c_set_clientdata(client, mp);
	mod_delayed_work(system_freezable_power_efficient_wq,
			 &mp->control_work, 0);

	dev_info(dev,
		 "MP2650 found (ID 0x%02x), charge control is %s\n",
		 device_id, (enable_control || mp->control_default) ?
		 "enabled" : "read-only");

	return 0;
}

static const struct of_device_id mp2650_of_match[] = {
	{ .compatible = "mps,mp2650" },
	{ }
};
MODULE_DEVICE_TABLE(of, mp2650_of_match);

static const struct i2c_device_id mp2650_i2c_ids[] = {
	{ "mp2650" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mp2650_i2c_ids);

static struct i2c_driver mp2650_driver = {
	.driver = {
		.name = "mp2650-charger",
		.of_match_table = mp2650_of_match,
	},
	.probe = mp2650_probe,
	.id_table = mp2650_i2c_ids,
};
module_i2c_driver(mp2650_driver);

MODULE_AUTHOR("Mainline OnePlus 8T community");
MODULE_DESCRIPTION("Monolithic Power Systems MP2650 charger driver");
MODULE_LICENSE("GPL");
