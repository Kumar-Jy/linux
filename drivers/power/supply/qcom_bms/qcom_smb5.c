// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Linaro Ltd.
 * Author: Casey Connolly <casey.connolly@linaro.org>
 *
 * This driver is for the switch-mode battery charger and boost
 * hardware found in PM8150b / PM7250b PMICs (SMB5 generation).
 */

#include <linux/bits.h>
#include <linux/devm-helpers.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "qcom_bms.h"

#define SMB_BASE 0x100

#define BATTERY_CHARGER_STATUS_1 0x06
#define BATTERY_CHARGER_STATUS_MASK GENMASK(2, 0)

#define SMB5_CHARGER_ERROR_STATUS_BAT_OV_BIT BIT(1)

#define BATTERY_CHARGER_STATUS_7 0x0D
#define SMB5_BAT_TEMP_STATUS_HOT_SOFT_BIT BIT(5)
#define SMB5_BAT_TEMP_STATUS_COLD_SOFT_BIT BIT(4)
#define SMB5_BAT_TEMP_STATUS_TOO_HOT_BIT BIT(3)
#define SMB5_BAT_TEMP_STATUS_TOO_COLD_BIT BIT(2)

#define CHARGING_ENABLE_CMD 0x42
#define CHARGING_ENABLE_CMD_BIT BIT(0)

#define FAST_CHARGE_CURRENT_CFG 0x61
#define FAST_CHARGE_CURRENT_SETTING_MASK GENMASK(7, 0)

#define FLOAT_VOLTAGE_CFG 0x70
#define FLOAT_VOLTAGE_SETTING_MASK GENMASK(7, 0)

#define SMB5_CHARGE_RCHG_SOC_THRESHOLD_CFG_REG 0x7D
#define SMB5_CHARGE_RCHG_SOC_THRESHOLD_CFG_MASK GENMASK(7, 0)

#define OTG_CFG 0x153
#define OTG_EN_SRC_CFG_BIT BIT(1)

#define APSD_STATUS 0x307
#define APSD_DTC_STATUS_DONE_BIT BIT(0)

#define APSD_RESULT_STATUS 0x308
#define APSD_RESULT_STATUS_MASK GENMASK(6, 0)
#define FLOAT_CHARGER_BIT BIT(4)
#define DCP_CHARGER_BIT BIT(3)
#define CDP_CHARGER_BIT BIT(2)
#define OCP_CHARGER_BIT BIT(1)

#define USBIN_CMD_IL 0x340
#define USBIN_SUSPEND_BIT BIT(0)

#define CMD_APSD 0x341
#define APSD_RERUN_BIT BIT(0)

#define CMD_ICL_OVERRIDE 0x342
#define ICL_OVERRIDE_BIT BIT(0)

#define TYPE_C_CFG 0x358
#define APSD_START_ON_CC_BIT BIT(7)

#define USBIN_OPTIONS_1_CFG 0x362
#define AUTO_SRC_DETECT_BIT BIT(3)

#define USBIN_LOAD_CFG 0x65
#define ICL_OVERRIDE_AFTER_APSD_BIT BIT(4)

#define USBIN_CURRENT_LIMIT_CFG 0x370
#define USBIN_ICL_OPTIONS 0x366
#define USBIN_MODE_CHG_BIT BIT(0)

#define USBIN_AICL_OPTIONS_CFG 0x380
#define SUSPEND_ON_COLLAPSE_USBIN_BIT BIT(7)
#define USBIN_AICL_PERIODIC_RERUN_EN_BIT BIT(4)
#define USBIN_AICL_ADC_EN_BIT BIT(3)
#define USBIN_AICL_EN_BIT BIT(2)

#define ICL_STATUS (SMB_BASE + 0x07)

#define POWER_PATH_STATUS (SMB_BASE + 0x0B)
#define P_PATH_USE_USBIN_BIT BIT(4)
#define P_PATH_VALID_INPUT_POWER_SOURCE_STS_BIT BIT(0)

/* 0x5xx region is PM8150b only Type-C registers */

#define SMB5_TYPE_C_MODE_CFG 0x544
#define SMB5_EN_TRY_SNK_BIT BIT(4)
#define SMB5_EN_SNK_ONLY_BIT BIT(1)

#define SMB5_TYPEC_TYPE_C_VCONN_CONTROL 0x546
#define SMB5_VCONN_EN_ORIENTATION_BIT BIT(2)
#define SMB5_VCONN_EN_VALUE_BIT BIT(1)
#define SMB5_VCONN_EN_SRC_BIT BIT(0)

#define SMB5_TYPE_C_DEBUG_ACCESS_SINK 0x54a
#define SMB5_TYPEC_DEBUG_ACCESS_SINK_MASK GENMASK(4, 0)

#define SMB5_DEBUG_ACCESS_SRC_CFG 0x54C
#define SMB5_EN_UNORIENTED_DEBUG_ACCESS_SRC_BIT BIT(0)

#define SMB5_TYPE_C_EXIT_STATE_CFG 0x550
#define SMB5_SEL_SRC_UPPER_REF_BIT BIT(2)

#define WD_CFG 0x651
#define WATCHDOG_TRIGGER_AFP_EN_BIT BIT(7)
#define BARK_WDOG_INT_EN_BIT BIT(6)
#define WDOG_TIMER_EN_ON_PLUGIN_BIT BIT(1)

#define SNARL_BARK_BITE_WD_CFG 0x653

#define AICL_RERUN_TIME_CFG 0x661
#define AICL_RERUN_TIME_MASK GENMASK(1, 0)
#define AIC_RERUN_TIME_3_SECS 0x0

#define SDP_CURRENT_UA 500000
#define CDP_CURRENT_UA 1500000
#define DCP_CURRENT_UA 1500000
#define UNVERIFIED_CURRENT_UA 2000000

/* SMB5 (PM8150b/PM7250b) registers represent current in 1/40th amp increments */
#define CURRENT_SCALE_FACTOR 25000

enum charger_status {
	TRICKLE_CHARGE = 0,
	PRE_CHARGE,
	FAST_CHARGE,
	FULLON_CHARGE,
	TAPER_CHARGE,
	TERMINATE_CHARGE,
	INHIBIT_CHARGE,
	DISABLE_CHARGE,
};

struct smb_init_register {
	u16 addr;
	u8 mask;
	u8 val;
};

/**
 * struct smb_chip - smb chip structure
 * @dev:		Device reference
 * @base:		Base address for smb registers
 * @regmap:		Register map
 * @status_change_work: Worker to handle plug/unplug events
 * @cable_irq:		USB plugin IRQ
 * @usb_in_i_chan:	USB_IN current measurement channel
 * @usb_in_v_chan:	USB_IN voltage measurement channel
 * @constant_charge_current_max_ua: maximum fast charge current from battery DT
 * @authenticated: all required battery authenticators have passed
 * @voltage_max_design_uv:	maximum battery voltage from battery DT
 */
struct smb_chip {
	struct device *dev;
	unsigned int base;
	struct regmap *regmap;

	struct delayed_work status_change_work;
	int cable_irq;
	struct iio_channel *usb_in_i_chan;
	struct iio_channel *usb_in_v_chan;

	u32 constant_charge_current_max_ua;
	bool authenticated;
	u32 voltage_max_design_uv;
};

static void smb_get_batt_info(struct smb_chip *chip)
{
	struct device_node *batt_np;
	u32 val;

	batt_np = of_parse_phandle(chip->dev->of_node, "monitored-battery", 0);
	if (!batt_np) {
		dev_warn(chip->dev,
			 "No monitored-battery, using default charge limits\n");
		chip->constant_charge_current_max_ua = DCP_CURRENT_UA;
		chip->voltage_max_design_uv = 4480000;
		return;
	}

	if (of_property_read_u32(batt_np,
				 "constant-charge-current-max-microamp", &val))
		val = DCP_CURRENT_UA;
	chip->constant_charge_current_max_ua = val;

	if (of_property_read_u32(batt_np, "voltage-max-design-microvolt", &val))
		val = 4480000;
	chip->voltage_max_design_uv = val;

	of_node_put(batt_np);
}

static int smb_get_prop_usb_online(void *priv, int *val)
{
	struct smb_chip *chip = priv;
	unsigned int stat;
	int rc;

	rc = regmap_read(chip->regmap, chip->base + POWER_PATH_STATUS, &stat);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't read power path status: %d\n", rc);
		return rc;
	}

	*val = (stat & P_PATH_USE_USBIN_BIT) &&
	       (stat & P_PATH_VALID_INPUT_POWER_SOURCE_STS_BIT);
	return 0;
}

/*
 * Qualcomm "automatic power source detection" aka APSD
 * tells us what type of charger we're connected to.
 */
static int smb_apsd_get_charger_type(void *priv, int *val)
{
	struct smb_chip *chip = priv;
	unsigned int apsd_stat, stat;
	int usb_online = 0;
	int rc;

	rc = smb_get_prop_usb_online(chip, &usb_online);
	if (!usb_online) {
		*val = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return rc;
	}

	rc = regmap_read(chip->regmap, chip->base + APSD_STATUS, &apsd_stat);
	if (rc < 0) {
		dev_err(chip->dev, "failed to read APSD status: %d\n", rc);
		return rc;
	}
	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT)) {
		dev_dbg(chip->dev, "APSD not ready\n");
		return -EAGAIN;
	}

	rc = regmap_read(chip->regmap, chip->base + APSD_RESULT_STATUS, &stat);
	if (rc < 0) {
		dev_err(chip->dev, "failed to read APSD result: %d\n", rc);
		return rc;
	}

	stat &= APSD_RESULT_STATUS_MASK;

	if (stat & CDP_CHARGER_BIT)
		*val = POWER_SUPPLY_USB_TYPE_CDP;
	else if (stat & (DCP_CHARGER_BIT | OCP_CHARGER_BIT | FLOAT_CHARGER_BIT))
		*val = POWER_SUPPLY_USB_TYPE_DCP;
	else /* SDP or unknown charger type */
		*val = POWER_SUPPLY_USB_TYPE_SDP;

	return 0;
}

/* Return 1 when in overvoltage state, else 0 or -errno */
static int smbx_ov_status(struct smb_chip *chip)
{
	u32 val;
	int rc;

	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_7,
			 &val);
	if (rc)
		return rc;

	return !!(val & SMB5_CHARGER_ERROR_STATUS_BAT_OV_BIT);
}

static int smb_get_prop_status(void *priv, int *val)
{
	struct smb_chip *chip = priv;
	u32 stat;
	int usb_online = 0;
	int rc;

	rc = smb_get_prop_usb_online(chip, &usb_online);
	if (!usb_online) {
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
		return rc;
	}

	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_1,
			 &stat);
	if (rc < 0) {
		dev_err(chip->dev, "Failed to read charging status ret=%d\n",
			rc);
		return rc;
	}

	rc = smbx_ov_status(chip);
	if (rc < 0)
		return rc;

	/* In overvoltage state */
	if (rc == 1) {
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}

	stat &= BATTERY_CHARGER_STATUS_MASK;

	switch (stat) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FAST_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		*val = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case DISABLE_CHARGE:
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case TERMINATE_CHARGE:
	case INHIBIT_CHARGE:
		*val = POWER_SUPPLY_STATUS_FULL;
		break;
	default:
		*val = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	return 0;
}

static inline int smb_get_current_limit(struct smb_chip *chip,
					unsigned int *val)
{
	int rc = regmap_read(chip->regmap, chip->base + ICL_STATUS, val);

	if (rc >= 0)
		*val *= CURRENT_SCALE_FACTOR;
	return rc;
}

static int smb_set_current_limit(struct smb_chip *chip, unsigned int val)
{
	unsigned char val_raw;

	if (val > 4800000) {
		dev_err(chip->dev,
			"can't set current limit higher than 4800000 uA\n");
		return -EINVAL;
	}
	val_raw = val / CURRENT_SCALE_FACTOR;

	return regmap_write(chip->regmap, chip->base + USBIN_CURRENT_LIMIT_CFG,
			    val_raw);
}

static int smb_get_input_current_limit(void *priv, int *val)
{
	struct smb_chip *chip = priv;
	unsigned int limit;
	int rc;

	rc = smb_get_current_limit(chip, &limit);
	if (rc < 0)
		return rc;

	*val = (int)limit;
	return 0;
}

static int smb_set_usbin_suspend(void *priv, bool suspend)
{
	struct smb_chip *chip = priv;

	return regmap_update_bits(chip->regmap, chip->base + USBIN_CMD_IL,
				  USBIN_SUSPEND_BIT,
				  suspend ? USBIN_SUSPEND_BIT : 0);
}

static void smb_status_change_work(struct work_struct *work)
{
	unsigned int charger_type, current_ua;
	int usb_online = 0;
	int count, rc;
	struct smb_chip *chip;

	chip = container_of(work, struct smb_chip, status_change_work.work);

	rc = smb_get_prop_usb_online(chip, &usb_online);
	if (rc < 0) {
		dev_err(chip->dev, "failed to read USB status: %d\n", rc);
		return;
	}
	if (!usb_online) {
		/*
		 * Charger removed: trigger a re-evaluation once VBUS has
		 * collapsed and the FG current has settled, so the status
		 * does not stay latched at CHARGING from a transient reading.
		 */
		qcom_bms_notify_changed(chip->dev);
		return;
	}

	for (count = 0; count < 3; count++) {
		dev_dbg(chip->dev, "get charger type retry %d\n", count);
		rc = smb_apsd_get_charger_type(chip, &charger_type);
		if (rc != -EAGAIN)
			break;
		msleep(100);
	}

	if (rc < 0 && rc != -EAGAIN) {
		dev_err(chip->dev, "get charger type failed: %d\n", rc);
		return;
	}

	if (rc < 0) {
		rc = regmap_update_bits(chip->regmap, chip->base + CMD_APSD,
					APSD_RERUN_BIT, APSD_RERUN_BIT);
		if (rc < 0) {
			dev_err(chip->dev, "failed to rerun APSD: %d\n", rc);
			return;
		}
		mod_delayed_work(system_wq, &chip->status_change_work,
				 msecs_to_jiffies(1000));
		dev_dbg(chip->dev, "get charger type failed, rerun apsd\n");
		return;
	}

	switch (charger_type) {
	case POWER_SUPPLY_USB_TYPE_CDP:
		current_ua = CDP_CURRENT_UA;
		break;
	case POWER_SUPPLY_USB_TYPE_DCP:
		current_ua = chip->constant_charge_current_max_ua;
		if (!READ_ONCE(chip->authenticated))
			current_ua = min(current_ua, UNVERIFIED_CURRENT_UA);
		break;
	case POWER_SUPPLY_USB_TYPE_SDP:
	default:
		current_ua = SDP_CURRENT_UA;
		break;
	}

	rc = smb_set_current_limit(chip, current_ua);
	if (rc < 0) {
		dev_err(chip->dev, "failed to set input current limit: %d\n",
			rc);
		return;
	}
	qcom_bms_notify_changed(chip->dev);
}

static int smb_get_iio_chan(struct smb_chip *chip, struct iio_channel *chan,
			    int *val)
{
	int rc;
	int status;

	rc = smb_get_prop_status(chip, &status);
	if (rc < 0 || status != POWER_SUPPLY_STATUS_CHARGING) {
		*val = 0;
		return 0;
	}

	return iio_read_channel_processed(chan, val);
}

static int smb_get_prop_health(void *priv, int *val)
{
	struct smb_chip *chip = priv;
	int rc;
	unsigned int stat;

	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_7,
			 &stat);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't read charger status 7 rc=%d\n",
			rc);
		return rc;
	}

	if (stat & SMB5_CHARGER_ERROR_STATUS_BAT_OV_BIT)
		*val = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else if (stat & SMB5_BAT_TEMP_STATUS_TOO_COLD_BIT)
		*val = POWER_SUPPLY_HEALTH_COLD;
	else if (stat & SMB5_BAT_TEMP_STATUS_TOO_HOT_BIT)
		*val = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (stat & SMB5_BAT_TEMP_STATUS_COLD_SOFT_BIT)
		*val = POWER_SUPPLY_HEALTH_COOL;
	else if (stat & SMB5_BAT_TEMP_STATUS_HOT_SOFT_BIT)
		*val = POWER_SUPPLY_HEALTH_WARM;
	else
		*val = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static int smb_get_voltage_now(void *priv, int *val)
{
	struct smb_chip *chip = priv;
	int ret;

	ret = smb_get_iio_chan(chip, chip->usb_in_v_chan, val);
	if (!ret)
		*val *= 16;

	return ret;
}

static int smb_op_current_now(void *priv, int *val)
{
	struct smb_chip *chip = priv;

	return smb_get_iio_chan(chip, chip->usb_in_i_chan, val);
}

static int smb_set_authenticated(void *priv, bool authenticated)
{
	struct smb_chip *chip = priv;

	WRITE_ONCE(chip->authenticated, authenticated);
	mod_delayed_work(system_wq, &chip->status_change_work, 0);

	return 0;
}

static const struct qcom_bms_charger_ops smb_charger_ops = {
	.get_status = smb_get_prop_status,
	.get_online = smb_get_prop_usb_online,
	.get_current_now = smb_op_current_now,
	.get_voltage_now = smb_get_voltage_now,
	.get_input_current_limit = smb_get_input_current_limit,
	.get_usb_type = smb_apsd_get_charger_type,
	.get_health = smb_get_prop_health,
	.set_usbin_suspend = smb_set_usbin_suspend,
	.set_authenticated = smb_set_authenticated,
};

static irqreturn_t smb_handle_batt_overvoltage(int irq, void *data)
{
	struct smb_chip *chip = data;

	if (smbx_ov_status(chip) == 1) {
		/* The hardware stops charging automatically */
		dev_err(chip->dev, "battery overvoltage detected\n");
		qcom_bms_notify_changed(chip->dev);
	}

	return IRQ_HANDLED;
}

static irqreturn_t smb_handle_usb_plugin(int irq, void *data)
{
	struct smb_chip *chip = data;

	qcom_bms_notify_changed(chip->dev);

	schedule_delayed_work(&chip->status_change_work,
			      msecs_to_jiffies(1500));

	return IRQ_HANDLED;
}

static irqreturn_t smb_handle_usb_icl_change(int irq, void *data)
{
	struct smb_chip *chip = data;

	qcom_bms_notify_changed(chip->dev);

	return IRQ_HANDLED;
}

/* Init sequence derived from vendor downstream driver */
static const struct smb_init_register smb5_init_seq[] = {
	{ .addr = USBIN_CMD_IL, .mask = USBIN_SUSPEND_BIT, .val = 0 },
	/*
	 * By default configure us as an upstream facing port
	 * FIXME: This will be handled by the type-c driver
	 */
	{ .addr = SMB5_TYPE_C_MODE_CFG,
	  .mask = SMB5_EN_TRY_SNK_BIT | SMB5_EN_SNK_ONLY_BIT,
	  .val = SMB5_EN_TRY_SNK_BIT },
	{ .addr = SMB5_TYPEC_TYPE_C_VCONN_CONTROL,
	  .mask = SMB5_VCONN_EN_ORIENTATION_BIT | SMB5_VCONN_EN_SRC_BIT |
		  SMB5_VCONN_EN_VALUE_BIT,
	  .val = 0 },
	{ .addr = SMB5_DEBUG_ACCESS_SRC_CFG,
	  .mask = SMB5_EN_UNORIENTED_DEBUG_ACCESS_SRC_BIT,
	  .val = SMB5_EN_UNORIENTED_DEBUG_ACCESS_SRC_BIT },
	{ .addr = SMB5_TYPE_C_EXIT_STATE_CFG,
	  .mask = SMB5_SEL_SRC_UPPER_REF_BIT,
	  .val = SMB5_SEL_SRC_UPPER_REF_BIT },
	/*
	 * Disable Type-C factory mode and stay in Attached.SRC state when VCONN
	 * over-current happens
	 */
	{ .addr = TYPE_C_CFG, .mask = APSD_START_ON_CC_BIT, .val = 0 },
	{ .addr = SMB5_TYPE_C_DEBUG_ACCESS_SINK,
	  .mask = SMB5_TYPEC_DEBUG_ACCESS_SINK_MASK,
	  .val = 0x17 },
	/* Configure VBUS for software control */
	{ .addr = OTG_CFG, .mask = OTG_EN_SRC_CFG_BIT, .val = 0 },
	/*
	 * Recharge when State Of Charge drops below 98%.
	 */
	{ .addr = SMB5_CHARGE_RCHG_SOC_THRESHOLD_CFG_REG,
	  .mask = SMB5_CHARGE_RCHG_SOC_THRESHOLD_CFG_MASK,
	  .val = 250 },
	/* Enable charging */
	{ .addr = CHARGING_ENABLE_CMD,
	  .mask = CHARGING_ENABLE_CMD_BIT,
	  .val = CHARGING_ENABLE_CMD_BIT },
	/* Enable BC1P2 auto Src detect */
	{ .addr = USBIN_OPTIONS_1_CFG,
	  .mask = AUTO_SRC_DETECT_BIT,
	  .val = AUTO_SRC_DETECT_BIT },
	/* Set the default SDP charger type to a 500ma USB 2.0 port */
	{ .addr = USBIN_ICL_OPTIONS,
	  .mask = USBIN_MODE_CHG_BIT,
	  .val = USBIN_MODE_CHG_BIT },
	{ .addr = CMD_ICL_OVERRIDE, .mask = ICL_OVERRIDE_BIT, .val = 0 },
	{ .addr = USBIN_LOAD_CFG,
	  .mask = ICL_OVERRIDE_AFTER_APSD_BIT,
	  .val = 0 },
	/* Disable watchdog */
	{ .addr = SNARL_BARK_BITE_WD_CFG, .mask = 0xff, .val = 0 },
	{ .addr = WD_CFG,
	  .mask = WATCHDOG_TRIGGER_AFP_EN_BIT | WDOG_TIMER_EN_ON_PLUGIN_BIT |
		  BARK_WDOG_INT_EN_BIT,
	  .val = 0 },
	/*
	 * Enable Automatic Input Current Limit, this will slowly ramp up the current
	 * When connected to a wall charger, and automatically stop when it detects
	 * the charger current limit (voltage drop?) or it reaches the programmed limit.
	 */
	{ .addr = USBIN_AICL_OPTIONS_CFG,
	  .mask = USBIN_AICL_PERIODIC_RERUN_EN_BIT | USBIN_AICL_ADC_EN_BIT |
		  USBIN_AICL_EN_BIT | SUSPEND_ON_COLLAPSE_USBIN_BIT,
	  .val = USBIN_AICL_PERIODIC_RERUN_EN_BIT | USBIN_AICL_ADC_EN_BIT |
		 USBIN_AICL_EN_BIT | SUSPEND_ON_COLLAPSE_USBIN_BIT },
	/*
	 * This overrides all of the other current limit configs and is
	 * expected to be used for setting limits based on temperature.
	 * We set some relatively safe default value while still allowing
	 * a comfortably fast charging rate.
	 */
	{ .addr = FAST_CHARGE_CURRENT_CFG,
	  .mask = FAST_CHARGE_CURRENT_SETTING_MASK,
	  .val = 1950000 / CURRENT_SCALE_FACTOR },
};

static int smb_init_hw(struct smb_chip *chip,
		       const struct smb_init_register *init_seq, size_t len)
{
	size_t i;
	int rc;

	for (i = 0; i < len; i++) {
		dev_dbg(chip->dev, "%zu: writing 0x%02x to 0x%02x\n", i,
			init_seq[i].val, init_seq[i].addr);
		rc = regmap_update_bits(chip->regmap,
					chip->base + init_seq[i].addr,
					init_seq[i].mask, init_seq[i].val);
		if (rc < 0)
			return dev_err_probe(chip->dev, rc,
					     "%s: init command %zu failed\n",
					     __func__, i);
	}

	return 0;
}

static int smb_init_irq(struct smb_chip *chip, int *irq, const char *name,
			irqreturn_t (*handler)(int irq, void *data))
{
	int irqnum;
	int rc;

	irqnum = platform_get_irq_byname(to_platform_device(chip->dev), name);
	if (irqnum < 0)
		return irqnum;

	rc = devm_request_threaded_irq(chip->dev, irqnum, NULL, handler,
				       IRQF_ONESHOT, name, chip);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc, "Couldn't request irq %s\n",
				     name);

	if (irq)
		*irq = irqnum;

	return 0;
}

static int smb_probe(struct platform_device *pdev)
{
	struct smb_chip *chip;
	int rc;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;

	chip->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->regmap)
		return dev_err_probe(chip->dev, -ENODEV,
				     "failed to locate the regmap\n");

	rc = device_property_read_u32(chip->dev, "reg", &chip->base);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc,
				     "Couldn't read base address\n");

	chip->usb_in_v_chan = devm_iio_channel_get(chip->dev, "usbin_v");
	if (IS_ERR(chip->usb_in_v_chan))
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb_in_v_chan),
				     "Couldn't get usbin_v IIO channel\n");

	chip->usb_in_i_chan = devm_iio_channel_get(chip->dev, "usbin_i");
	if (IS_ERR(chip->usb_in_i_chan))
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb_in_i_chan),
				     "Couldn't get usbin_i IIO channel\n");

	smb_get_batt_info(chip);

	rc = smb_init_hw(chip, smb5_init_seq, ARRAY_SIZE(smb5_init_seq));
	if (rc < 0)
		return rc;

	rc = devm_delayed_work_autocancel(chip->dev, &chip->status_change_work,
					  smb_status_change_work);
	if (rc)
		return dev_err_probe(chip->dev, rc,
				     "Failed to init status change work\n");

	{
		u32 vbat_max;

		if (chip->voltage_max_design_uv > 3487500)
			vbat_max = chip->voltage_max_design_uv - 3487500;
		else
			vbat_max = 0;
		vbat_max = vbat_max / 7500 + 1;
		if (vbat_max > FLOAT_VOLTAGE_SETTING_MASK)
			vbat_max = FLOAT_VOLTAGE_SETTING_MASK;

		rc = regmap_update_bits(chip->regmap,
					chip->base + FLOAT_VOLTAGE_CFG,
					FLOAT_VOLTAGE_SETTING_MASK, vbat_max);
		if (rc < 0)
			return dev_err_probe(chip->dev, rc,
					     "Couldn't set vbat max\n");
	}

	rc = smb_init_irq(chip, NULL, "bat-ov", smb_handle_batt_overvoltage);
	if (rc < 0)
		return rc;

	rc = smb_init_irq(chip, &chip->cable_irq, "usb-plugin",
			  smb_handle_usb_plugin);
	if (rc < 0)
		return rc;

	rc = smb_init_irq(chip, NULL, "usbin-icl-change",
			  smb_handle_usb_icl_change);
	if (rc < 0)
		return rc;

	devm_device_init_wakeup(chip->dev);

	rc = devm_pm_set_wake_irq(chip->dev, chip->cable_irq);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc, "Couldn't set wake irq\n");

	platform_set_drvdata(pdev, chip);

	rc = regmap_write_bits(chip->regmap, chip->base + AICL_RERUN_TIME_CFG,
			       AICL_RERUN_TIME_MASK, AIC_RERUN_TIME_3_SECS);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc,
				     "Couldn't write fast AICL rerun time\n");

	rc = qcom_bms_register_charger(&pdev->dev, QCOM_BMS_CHARGER_PMIC,
				       &smb_charger_ops, chip);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc,
				     "Failed to register with qcom_bms\n");

	/* Initialise charger state */
	mod_delayed_work(system_wq, &chip->status_change_work, 0);

	return 0;
}

static void smb_remove(struct platform_device *pdev)
{
	qcom_bms_unregister_charger(&pdev->dev);
}

static const struct of_device_id smb_match_id_table[] = {
	{ .compatible = "qcom,pm7250b-charger" },
	{ .compatible = "qcom,pm8150b-charger" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, smb_match_id_table);

static struct platform_driver qcom_spmi_smb5 = {
	.probe = smb_probe,
	.remove = smb_remove,
	.driver = {
		.name = "qcom-smb5-charger",
		.of_match_table = smb_match_id_table,
	},
};

module_platform_driver(qcom_spmi_smb5);

MODULE_AUTHOR("Casey Connolly <casey.connolly@linaro.org>");
MODULE_AUTHOR("TwinbornPlate75 <3342733415@qq.com>");
MODULE_DESCRIPTION("Qualcomm SMB5 Charger Driver (qcom_bms sub-module)");
MODULE_LICENSE("GPL");
