// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fuel gauge driver for the Silicon Mitus SM5703 PMIC.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/bits.h>

#define SM5703_FG_REG_DEVICE_ID	0x00
#define SM5703_FG_REG_STATUS		0x04
#define SM5703_FG_REG_SOC		0x05
#define SM5703_FG_REG_OCV		0x06
#define SM5703_FG_REG_VOLTAGE		0x07
#define SM5703_FG_REG_CURRENT		0x08
#define SM5703_FG_REG_TEMPERATURE	0x09

#define SM5703_FG_SOC_INT_MASK		0xFF00
#define SM5703_FG_SOC_INT_SHIFT	8
#define SM5703_FG_WORD_INT_MASK	0x0700
#define SM5703_FG_WORD_INT_SHIFT	8
#define SM5703_FG_WORD_FRAC_MASK	0x00FF
#define SM5703_FG_WORD_SIGN_BIT	BIT(15)
#define SM5703_FG_TEMP_INT_MASK	0x7F00
#define SM5703_FG_TEMP_INT_SHIFT	8

struct sm5703_fg_data {
	struct i2c_client *client;
	struct power_supply *psy;
};

static int sm5703_fg_read_word(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_word_data(client, reg);
}

/* returns millivolts */
static int sm5703_fg_get_voltage(struct sm5703_fg_data *fg, u8 reg)
{
	int val = sm5703_fg_read_word(fg->client, reg);
	int mv;

	if (val < 0)
		return val;

	mv = ((val & SM5703_FG_WORD_INT_MASK) >> SM5703_FG_WORD_INT_SHIFT) * 1000;
	mv += ((val & SM5703_FG_WORD_FRAC_MASK) * 1000) / 256;

	return mv;
}

/* returns milliamps, signed */
static int sm5703_fg_get_current(struct sm5703_fg_data *fg)
{
	int val = sm5703_fg_read_word(fg->client, SM5703_FG_REG_CURRENT);
	int ma;

	if (val < 0)
		return val;

	ma = ((val & SM5703_FG_WORD_INT_MASK) >> SM5703_FG_WORD_INT_SHIFT) * 1000;
	ma += ((val & SM5703_FG_WORD_FRAC_MASK) * 1000) / 256;

	if (val & SM5703_FG_WORD_SIGN_BIT)
		ma = -ma;

	return ma;
}

/* returns tenths of a degree C, signed */
static int sm5703_fg_get_temp(struct sm5703_fg_data *fg)
{
	int val = sm5703_fg_read_word(fg->client, SM5703_FG_REG_TEMPERATURE);
	int decidegc;

	if (val < 0)
		return val;

	decidegc = ((val & SM5703_FG_TEMP_INT_MASK) >> SM5703_FG_TEMP_INT_SHIFT) * 10;
	decidegc += ((val & SM5703_FG_WORD_FRAC_MASK) * 10) / 256;

	if (val & SM5703_FG_WORD_SIGN_BIT)
		decidegc = -decidegc;

	return decidegc;
}

/* returns tenths of a percent, i.e. 1000 == 100.0% */
static int sm5703_fg_get_capacity(struct sm5703_fg_data *fg)
{
	int val = sm5703_fg_read_word(fg->client, SM5703_FG_REG_SOC);
	int permille;

	if (val < 0)
		return val;

	permille = ((val & SM5703_FG_SOC_INT_MASK) >> SM5703_FG_SOC_INT_SHIFT) * 10;
	permille += ((val & SM5703_FG_WORD_FRAC_MASK) * 10) / 256;

	return permille;
}

static enum power_supply_property sm5703_fg_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static int sm5703_fg_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct sm5703_fg_data *fg = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		ret = sm5703_fg_read_word(fg->client, SM5703_FG_REG_DEVICE_ID);
		val->intval = ret >= 0;
		return 0;

	case POWER_SUPPLY_PROP_CAPACITY:
		ret = sm5703_fg_get_capacity(fg);
		if (ret < 0)
			return ret;
		val->intval = ret / 10;
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = sm5703_fg_get_voltage(fg, SM5703_FG_REG_VOLTAGE);
		if (ret < 0)
			return ret;
		val->intval = ret * 1000;
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		ret = sm5703_fg_get_voltage(fg, SM5703_FG_REG_OCV);
		if (ret < 0)
			return ret;
		val->intval = ret * 1000;
		return 0;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = sm5703_fg_get_current(fg);
		if (ret < 0)
			return ret;
		val->intval = ret * 1000;
		return 0;

	case POWER_SUPPLY_PROP_TEMP:
		ret = sm5703_fg_get_temp(fg);
		if (ret < 0)
			return ret;
		val->intval = ret;
		return 0;

	case POWER_SUPPLY_PROP_STATUS:
		ret = sm5703_fg_get_current(fg);
		if (ret < 0)
			return ret;
		/*
		 * the fuel gauge alone has no reliable view of charger
		 * state, this is a coarse heuristic based on current
		 * sign only. a charger-side driver should normally take
		 * precedence for POWER_SUPPLY_PROP_STATUS via
		 * power-supply "supplied-from" links.
		 */
		if (ret > 50)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (ret < -50)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;

	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc sm5703_fg_desc = {
	.name		= "sm5703-fuel-gauge",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= sm5703_fg_props,
	.num_properties	= ARRAY_SIZE(sm5703_fg_props),
	.get_property	= sm5703_fg_get_property,
};

static int sm5703_fg_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config psy_cfg = {};
	struct sm5703_fg_data *fg;
	int dev_id;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->client = client;
	i2c_set_clientdata(client, fg);

	dev_id = sm5703_fg_read_word(client, SM5703_FG_REG_DEVICE_ID);
	if (dev_id < 0)
		return dev_err_probe(dev, dev_id, "Failed to read device ID\n");

	psy_cfg.drv_data = fg;
	psy_cfg.fwnode = dev_fwnode(dev);

	fg->psy = devm_power_supply_register(dev, &sm5703_fg_desc, &psy_cfg);
	if (IS_ERR(fg->psy))
		return dev_err_probe(dev, PTR_ERR(fg->psy),
				     "Failed to register power supply\n");

	return 0;
}

static const struct of_device_id sm5703_fg_of_match[] = {
	{ .compatible = "siliconmitus,sm5703-fuel-gauge", },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5703_fg_of_match);

static const struct i2c_device_id sm5703_fg_i2c_id[] = {
	{ "sm5703-fuel-gauge", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm5703_fg_i2c_id);

static struct i2c_driver sm5703_fg_driver = {
	.driver = {
		.name = "sm5703-fuel-gauge",
		.of_match_table = sm5703_fg_of_match,
	},
	.probe = sm5703_fg_probe,
	.id_table = sm5703_fg_i2c_id,
};
module_i2c_driver(sm5703_fg_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5703 fuel gauge driver");
MODULE_AUTHOR("unsoil");
MODULE_LICENSE("GPL");
