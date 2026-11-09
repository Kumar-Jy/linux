// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the Texas Instruments BQ25970 / BQ2597x switched-capacitor
 * direct charger (used in secondary hardware revisions of Xiaomi Pad 5 nabu).
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include "qcom_bms.h"

/* BQ25970 Register Definitions */
#define BQ25970_REG_BATOVP		0x00
#define BQ25970_REG_BATOVP_ALM		0x01
#define BQ25970_REG_BATOCP		0x02
#define BQ25970_REG_BATOCP_ALM		0x03
#define BQ25970_REG_BATUCP_ALM		0x04
#define BQ25970_REG_CHRGR_CTRL_1	0x05
#define BQ25970_REG_BUSOVP		0x06
#define BQ25970_REG_BUSOVP_ALM		0x07
#define BQ25970_REG_BUSOCP		0x08
#define BQ25970_REG_BUSOCP_ALM		0x09
#define BQ25970_REG_TEMP_CONTROL	0x0A
#define BQ25970_REG_TDIE_ALM		0x0B
#define BQ25970_REG_TSBUS_FLT		0x0C
#define BQ25970_REG_TSBAT_FLT		0x0D
#define BQ25970_REG_VAC_CONTROL		0x0E
#define BQ25970_REG_CHRGR_CTRL_2	0x0F
#define BQ25970_REG_CHRGR_CTRL_3	0x10
#define BQ25970_REG_CHRGR_CTRL_4	0x11
#define BQ25970_REG_CHRGR_CTRL_5	0x12
#define BQ25970_REG_STAT1		0x13
#define BQ25970_REG_STAT2		0x14
#define BQ25970_REG_STAT3		0x15
#define BQ25970_REG_STAT4		0x16
#define BQ25970_REG_STAT5		0x17
#define BQ25970_REG_FLAG1		0x18
#define BQ25970_REG_FLAG2		0x19
#define BQ25970_REG_FLAG3		0x1A
#define BQ25970_REG_FLAG4		0x1B
#define BQ25970_REG_FLAG5		0x1C
#define BQ25970_REG_MASK1		0x1D
#define BQ25970_REG_MASK2		0x1E
#define BQ25970_REG_MASK3		0x1F
#define BQ25970_REG_MASK4		0x20
#define BQ25970_REG_MASK5		0x21
#define BQ25970_REG_DEVICE_INFO		0x22
#define BQ25970_REG_ADC_CONTROL1	0x23
#define BQ25970_REG_ADC_CONTROL2	0x24
#define BQ25970_REG_IBUS_ADC_MSB	0x25
#define BQ25970_REG_IBUS_ADC_LSB	0x26
#define BQ25970_REG_VBUS_ADC_MSB	0x27
#define BQ25970_REG_VBUS_ADC_LSB	0x28
#define BQ25970_REG_VAC1_ADC_MSB	0x29
#define BQ25970_REG_VAC1_ADC_LSB	0x2A
#define BQ25970_REG_VAC2_ADC_MSB	0x2B
#define BQ25970_REG_VAC2_ADC_LSB	0x2C
#define BQ25970_REG_VOUT_ADC_MSB	0x2D
#define BQ25970_REG_VOUT_ADC_LSB	0x2E
#define BQ25970_REG_VBAT_ADC_MSB	0x2F
#define BQ25970_REG_VBAT_ADC_LSB	0x30
#define BQ25970_REG_IBAT_ADC_MSB	0x31
#define BQ25970_REG_IBAT_ADC_LSB	0x32
#define BQ25970_REG_TSBUS_ADC_MSB	0x33
#define BQ25970_REG_TSBUS_ADC_LSB	0x34
#define BQ25970_REG_TSBAT_ADC_MSB	0x35
#define BQ25970_REG_TSBAT_ADC_LSB	0x36
#define BQ25970_REG_TDIE_ADC_MSB	0x37
#define BQ25970_REG_TDIE_ADC_LSB	0x38
#define BQ25970_REG_MAX			0x3A

/* Register bits and masks */
#define BQ25970_CHG_EN			BIT(4)
#define BQ25970_EN_HIZ			BIT(6)
#define BQ25970_ADC_EN			BIT(7)
#define BQ25970_ADC_RATE_CONT		BIT(6)
#define BQ25970_WATCHDOG_DIS		BIT(2)
#define BQ25970_WATCHDOG_MASK		GENMASK(4, 3)

#define BQ25970_STAT1_VBUS_OVP_STAT	BIT(6)
#define BQ25970_STAT1_VBAT_OVP_STAT	BIT(5)
#define BQ25970_STAT1_IBUS_OCP_STAT	BIT(3)
#define BQ25970_STAT2_BAT_OCP_STAT	BIT(6)
#define BQ25970_STAT4_TSBUS_FLT_STAT	GENMASK(5, 3)
#define BQ25970_STAT4_TSBAT_FLT_STAT	GENMASK(2, 0)

/* Scale factors and conversion steps */
#define BQ25970_BATOVP_OFFSET_uV	3500000
#define BQ25970_BATOVP_STEP_uV		10000
#define BQ25970_BATOVP_MIN_uV		3500000
#define BQ25970_BATOVP_MAX_uV		4770000

#define BQ25970_BUSOVP_OFFSET_uV	7000000
#define BQ25970_BUSOVP_STEP_uV		50000
#define BQ25970_BUSOVP_MIN_uV		7000000
#define BQ25970_BUSOVP_MAX_uV		12750000

#define BQ25970_BUSOCP_OFFSET_uA	1000000
#define BQ25970_BUSOCP_STEP_uA		250000
#define BQ25970_BUSOCP_MIN_uA		1000000
#define BQ25970_BUSOCP_MAX_uA		5750000

#define BQ25970_ADC_VOLT_STEP_uV	1000 /* 1 mV */
#define BQ25970_ADC_CURR_STEP_uA	1000 /* 1 mA */
#define BQ25970_ADC_POLARITY_BIT	BIT(7)

/* Default configuration for Nabu Sunwoda 8720mAh battery */
#define BQ25970_BAT_OVP_DEFAULT_uV	4560000
#define BQ25970_BUS_OVP_DEFAULT_uV	12750000
#define BQ25970_BUS_OCP_DEFAULT_uA	3750000

struct bq25970_pdata {
	u32 bat_ovp_th;
	u32 bus_ovp_th;
	u32 bus_ocp_th;
};

struct bq25970_info {
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex lock;
	struct bq25970_pdata pdata;

	bool chg_en;
	bool online;
	bool vbat_ov;
	bool vbus_ov;
	bool ibus_oc;
	bool tdie_fault;
};

static const struct regmap_config bq25970_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = BQ25970_REG_MAX,
};

static int bq25970_set_bat_ovp(struct bq25970_info *info, u32 uV)
{
	u32 val;

	uV = clamp_val(uV, BQ25970_BATOVP_MIN_uV, BQ25970_BATOVP_MAX_uV);
	val = (uV - BQ25970_BATOVP_OFFSET_uV) / BQ25970_BATOVP_STEP_uV;

	return regmap_write(info->regmap, BQ25970_REG_BATOVP, val);
}

static int bq25970_set_bus_ovp(struct bq25970_info *info, u32 uV)
{
	u32 val;

	uV = clamp_val(uV, BQ25970_BUSOVP_MIN_uV, BQ25970_BUSOVP_MAX_uV);
	val = (uV - BQ25970_BUSOVP_OFFSET_uV) / BQ25970_BUSOVP_STEP_uV;

	return regmap_write(info->regmap, BQ25970_REG_BUSOVP, val);
}

static int bq25970_set_bus_ocp(struct bq25970_info *info, u32 uA)
{
	u32 val;

	uA = clamp_val(uA, BQ25970_BUSOCP_MIN_uA, BQ25970_BUSOCP_MAX_uA);
	val = (uA - BQ25970_BUSOCP_OFFSET_uA) / BQ25970_BUSOCP_STEP_uA;

	return regmap_write(info->regmap, BQ25970_REG_BUSOCP, val);
}

static int bq25970_enable_adc(struct bq25970_info *info, bool en)
{
	u8 val = en ? (BQ25970_ADC_EN | BQ25970_ADC_RATE_CONT) : 0;

	return regmap_update_bits(info->regmap, BQ25970_REG_ADC_CONTROL1,
				  BQ25970_ADC_EN | BQ25970_ADC_RATE_CONT, val);
}

static int bq25970_read_adc_ibus(struct bq25970_info *info, int *val)
{
	unsigned int msb, lsb;
	u16 code;
	int ret;

	ret = regmap_read(info->regmap, BQ25970_REG_IBUS_ADC_MSB, &msb);
	if (ret < 0)
		return ret;

	ret = regmap_read(info->regmap, BQ25970_REG_IBUS_ADC_LSB, &lsb);
	if (ret < 0)
		return ret;

	code = ((u16)msb << 8) | lsb;
	if (msb & BQ25970_ADC_POLARITY_BIT)
		*val = -((int)((code ^ 0xffff) + 1) * BQ25970_ADC_CURR_STEP_uA);
	else
		*val = (int)code * BQ25970_ADC_CURR_STEP_uA;

	return 0;
}

static int bq25970_read_adc_vbus(struct bq25970_info *info, int *val)
{
	unsigned int msb, lsb;
	u16 code;
	int ret;

	ret = regmap_read(info->regmap, BQ25970_REG_VBUS_ADC_MSB, &msb);
	if (ret < 0)
		return ret;

	ret = regmap_read(info->regmap, BQ25970_REG_VBUS_ADC_LSB, &lsb);
	if (ret < 0)
		return ret;

	code = ((u16)msb << 8) | lsb;
	*val = (int)code * BQ25970_ADC_VOLT_STEP_uV;

	return 0;
}

static int bq25970_check_status(struct bq25970_info *info)
{
	unsigned int stat1, stat2, stat4;
	int ret;

	ret = regmap_read(info->regmap, BQ25970_REG_STAT1, &stat1);
	if (ret < 0)
		return ret;

	ret = regmap_read(info->regmap, BQ25970_REG_STAT2, &stat2);
	if (ret < 0)
		return ret;

	ret = regmap_read(info->regmap, BQ25970_REG_STAT4, &stat4);
	if (ret < 0)
		return ret;

	WRITE_ONCE(info->vbus_ov, !!(stat1 & BQ25970_STAT1_VBUS_OVP_STAT));
	WRITE_ONCE(info->vbat_ov, !!(stat1 & BQ25970_STAT1_VBAT_OVP_STAT));
	WRITE_ONCE(info->ibus_oc, !!(stat1 & BQ25970_STAT1_IBUS_OCP_STAT));
	WRITE_ONCE(info->tdie_fault, !!(stat4 & (BQ25970_STAT4_TSBUS_FLT_STAT |
						 BQ25970_STAT4_TSBAT_FLT_STAT)));

	return 0;
}

static int bq25970_enable_charging(struct bq25970_info *info, bool en)
{
	int ret;

	mutex_lock(&info->lock);

	if (en) {
		bq25970_check_status(info);
		if (info->vbus_ov || info->vbat_ov || info->ibus_oc) {
			dev_warn(info->dev, "cannot enable charging: fault active\n");
			mutex_unlock(&info->lock);
			return -EBUSY;
		}

		/* Disable watchdog during autonomous charging management */
		regmap_update_bits(info->regmap, BQ25970_REG_CHRGR_CTRL_3,
				   BQ25970_WATCHDOG_MASK, BQ25970_WATCHDOG_DIS);

		/* Enable charging */
		ret = regmap_update_bits(info->regmap, BQ25970_REG_CHRGR_CTRL_2,
					 BQ25970_CHG_EN, BQ25970_CHG_EN);
		if (ret < 0) {
			mutex_unlock(&info->lock);
			return ret;
		}

		WRITE_ONCE(info->chg_en, true);
		WRITE_ONCE(info->online, true);
	} else {
		/* Disable charging */
		ret = regmap_update_bits(info->regmap, BQ25970_REG_CHRGR_CTRL_2,
					 BQ25970_CHG_EN, 0);
		if (ret < 0) {
			mutex_unlock(&info->lock);
			return ret;
		}

		WRITE_ONCE(info->chg_en, false);
		WRITE_ONCE(info->online, false);
	}

	mutex_unlock(&info->lock);
	return 0;
}

/* Coordinator ops */
static int bq25970_get_status(void *priv, int *status)
{
	struct bq25970_info *info = priv;

	if (READ_ONCE(info->chg_en) && READ_ONCE(info->online))
		*status = POWER_SUPPLY_STATUS_CHARGING;
	else
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

static int bq25970_get_online(void *priv, int *online)
{
	struct bq25970_info *info = priv;

	*online = READ_ONCE(info->chg_en) && READ_ONCE(info->online);
	return 0;
}

static int bq25970_get_current_now(void *priv, int *val)
{
	struct bq25970_info *info = priv;

	return bq25970_read_adc_ibus(info, val);
}

static int bq25970_get_voltage_now(void *priv, int *val)
{
	struct bq25970_info *info = priv;

	return bq25970_read_adc_vbus(info, val);
}

static int bq25970_get_input_current_limit(void *priv, int *val)
{
	struct bq25970_info *info = priv;

	*val = (int)info->pdata.bus_ocp_th;
	return 0;
}

static int bq25970_get_health(void *priv, int *val)
{
	struct bq25970_info *info = priv;

	bq25970_check_status(info);

	if (READ_ONCE(info->vbat_ov) || READ_ONCE(info->vbus_ov))
		*val = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else if (READ_ONCE(info->ibus_oc))
		*val = POWER_SUPPLY_HEALTH_OVERCURRENT;
	else if (READ_ONCE(info->tdie_fault))
		*val = POWER_SUPPLY_HEALTH_OVERHEAT;
	else
		*val = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static int bq25970_set_charging_enabled(void *priv, bool en)
{
	struct bq25970_info *info = priv;

	return bq25970_enable_charging(info, en);
}

static const struct qcom_bms_charger_ops bq25970_charger_ops = {
	.get_status = bq25970_get_status,
	.get_online = bq25970_get_online,
	.get_current_now = bq25970_get_current_now,
	.get_voltage_now = bq25970_get_voltage_now,
	.get_input_current_limit = bq25970_get_input_current_limit,
	.get_health = bq25970_get_health,
	.set_charging_enabled = bq25970_set_charging_enabled,
};

static irqreturn_t bq25970_irq_handler(int irq, void *data)
{
	struct bq25970_info *info = data;
	unsigned int flag1, flag2, flag3, flag4, flag5;

	/* Reading FLAG registers clears latched interrupt events */
	regmap_read(info->regmap, BQ25970_REG_FLAG1, &flag1);
	regmap_read(info->regmap, BQ25970_REG_FLAG2, &flag2);
	regmap_read(info->regmap, BQ25970_REG_FLAG3, &flag3);
	regmap_read(info->regmap, BQ25970_REG_FLAG4, &flag4);
	regmap_read(info->regmap, BQ25970_REG_FLAG5, &flag5);

	bq25970_check_status(info);

	if ((info->vbus_ov || info->vbat_ov || info->ibus_oc) &&
	    READ_ONCE(info->chg_en)) {
		dev_warn(info->dev, "fault triggered in IRQ, disabling charging\n");
		bq25970_enable_charging(info, false);
	}

	qcom_bms_notify_changed(info->dev);
	return IRQ_HANDLED;
}

static void bq25970_parse_dt(struct bq25970_info *info)
{
	struct device *dev = info->dev;
	struct bq25970_pdata *p = &info->pdata;

	p->bat_ovp_th = BQ25970_BAT_OVP_DEFAULT_uV;
	device_property_read_u32(dev, "ti,bat-ovp-threshold-microvolt",
				 &p->bat_ovp_th);

	p->bus_ovp_th = BQ25970_BUS_OVP_DEFAULT_uV;
	device_property_read_u32(dev, "ti,bus-ovp-threshold-microvolt",
				 &p->bus_ovp_th);

	p->bus_ocp_th = BQ25970_BUS_OCP_DEFAULT_uA;
	device_property_read_u32(dev, "ti,bus-ocp-threshold-microamp",
				 &p->bus_ocp_th);
}

static int bq25970_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct bq25970_info *info;
	unsigned int stat1;
	int ret;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	info->client = client;
	mutex_init(&info->lock);

	info->regmap = devm_regmap_init_i2c(client, &bq25970_regmap_config);
	if (IS_ERR(info->regmap))
		return PTR_ERR(info->regmap);

	/*
	 * Probing verification:
	 * If BQ25970 is not populated (e.g., standard LN8000 hardware revision),
	 * reading STAT1 register fails with -ENXIO. Return -ENODEV so the kernel
	 * exits cleanly without error noise.
	 */
	ret = regmap_read(info->regmap, BQ25970_REG_STAT1, &stat1);
	if (ret < 0) {
		dev_info(dev, "BQ25970 not found (I2C read failed: %d)\n", ret);
		return -ENODEV;
	}

	dev_info(dev, "Found BQ25970 charge pump at 0x%02x\n", client->addr);

	bq25970_parse_dt(info);

	/* Initialize safety thresholds */
	ret = bq25970_set_bat_ovp(info, info->pdata.bat_ovp_th);
	if (ret < 0)
		return ret;

	ret = bq25970_set_bus_ovp(info, info->pdata.bus_ovp_th);
	if (ret < 0)
		return ret;

	ret = bq25970_set_bus_ocp(info, info->pdata.bus_ocp_th);
	if (ret < 0)
		return ret;

	/* Enable continuous ADC monitoring */
	ret = bq25970_enable_adc(info, true);
	if (ret < 0)
		dev_warn(dev, "failed to enable ADC: %d\n", ret);

	/* Request IRQ if present */
	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						bq25970_irq_handler,
						IRQF_ONESHOT | IRQF_TRIGGER_LOW,
						"bq25970-charger", info);
		if (ret < 0)
			dev_warn(dev, "failed to request IRQ %d: %d\n", client->irq, ret);
	}

	i2c_set_clientdata(client, info);

	/* Register with Qualcomm BMS coordinator as DIRECT charger */
	ret = qcom_bms_register_charger(dev, QCOM_BMS_CHARGER_DIRECT,
					&bq25970_charger_ops, info);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to register with qcom_bms\n");

	return 0;
}

static void bq25970_remove(struct i2c_client *client)
{
	struct bq25970_info *info = i2c_get_clientdata(client);

	if (info && READ_ONCE(info->chg_en))
		bq25970_enable_charging(info, false);

	qcom_bms_unregister_charger(&client->dev);
}

static void bq25970_shutdown(struct i2c_client *client)
{
	struct bq25970_info *info = i2c_get_clientdata(client);

	if (info)
		bq25970_enable_charging(info, false);
}

static const struct of_device_id bq25970_of_match[] = {
	{ .compatible = "ti,bq25970" },
	{ .compatible = "ti,bq25975" },
	{ }
};
MODULE_DEVICE_TABLE(of, bq25970_of_match);

static const struct i2c_device_id bq25970_i2c_ids[] = {
	{ "bq25970" },
	{ "bq25975" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bq25970_i2c_ids);

static struct i2c_driver bq25970_driver = {
	.driver = {
		.name = "bq25970-charger",
		.of_match_table = bq25970_of_match,
	},
	.probe = bq25970_probe,
	.remove = bq25970_remove,
	.shutdown = bq25970_shutdown,
	.id_table = bq25970_i2c_ids,
};
module_i2c_driver(bq25970_driver);

MODULE_DESCRIPTION("TI BQ25970 / BQ2597x Switched-Capacitor Direct Charger Driver");
MODULE_AUTHOR("Kumar Jy <20044626+Kumar-Jy@users.noreply.github.com>");
MODULE_LICENSE("GPL");
