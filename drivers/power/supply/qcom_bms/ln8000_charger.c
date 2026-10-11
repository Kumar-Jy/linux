// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the LION Semiconductor LN8000 switched-capacitor direct charger.
 */

#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
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

#define LN8000_REG_DEVICE_ID 0x00
#define LN8000_REG_INT1 0x01
#define LN8000_REG_INT1_MSK 0x02
#define LN8000_REG_SYS_STS 0x03
#define LN8000_REG_ADC01_STS 0x09
#define LN8000_REG_ADC03_STS 0x0B
#define LN8000_REG_ADC06_STS 0x0E
#define LN8000_REG_IIN_CTRL 0x1B
#define LN8000_REG_REGULATION_CTRL 0x1C
#define LN8000_REG_SYS_CTRL 0x1E
#define LN8000_REG_GLITCH_CTRL 0x20
#define LN8000_REG_FAULT_CTRL 0x21
#define LN8000_REG_NTC_CTRL 0x22
#define LN8000_REG_ADC_CTRL 0x23
#define LN8000_REG_ADC_CFG 0x24
#define LN8000_REG_RECOVERY_CTRL 0x25
#define LN8000_REG_TIMER_CTRL 0x26
#define LN8000_REG_THRESHOLD_CTRL 0x27
#define LN8000_REG_V_FLOAT_CTRL 0x28
#define LN8000_REG_CHARGE_CTRL 0x29
#define LN8000_REG_MAX 0x4D

#define LN8000_DEVICE_ID 0x42

#define LN8000_MASK_FAULT_INT BIT(7)
#define LN8000_MASK_NTC_PROT_INT BIT(6)
#define LN8000_MASK_CHARGE_PHASE_INT BIT(5)
#define LN8000_MASK_MODE_INT BIT(4)
#define LN8000_MASK_REV_CURR_INT BIT(3)
#define LN8000_MASK_TEMP_INT BIT(2)
#define LN8000_MASK_ADC_DONE_INT BIT(1)
#define LN8000_MASK_TIMER_INT BIT(0)

#define LN8000_MASK_SWITCHING_ENABLED BIT(2)
#define LN8000_MASK_STANDBY_STS (BIT(1) | BIT(0))

#define LN8000_MASK_TEMP_MAX_STS BIT(6)

#define LN8000_MASK_WATCHDOG_TIMER_STS BIT(7)
#define LN8000_MASK_VBAT_OV_STS BIT(6)
#define LN8000_MASK_VIN_OV_STS BIT(1)

#define LN8000_MASK_IIN_OC_DETECTED BIT(7)

#define LN8000_BIT_ENABLE_VFLOAT_LOOP_INT 7
#define LN8000_BIT_ENABLE_IIN_LOOP_INT 6
#define LN8000_BIT_DISABLE_VFLOAT_LOOP 5
#define LN8000_BIT_DISABLE_IIN_LOOP 4

#define LN8000_BIT_STANDBY_EN 3
#define LN8000_BIT_REV_IIN_DET 2

#define LN8000_BIT_DISABLE_IIN_OCP 6
#define LN8000_BIT_DISABLE_VBAT_OV 5
#define LN8000_BIT_DISABLE_VAC_OV 4

#define LN8000_BIT_PAUSE_ADC_UPDATE 1
#define LN8000_BIT_CLEAR_LATCHED_STS 2

#define LN8000_NTC_SHUTDOWN_CFG 2

#define LN8000_VBAT_FLOAT_MIN 3725000 /* uV */
#define LN8000_VBAT_FLOAT_MAX 5000000
#define LN8000_VBAT_FLOAT_LSB 5000
#define LN8000_ADC_VIN_STEP 16000 /* uV */
#define LN8000_ADC_VBAT_STEP 5000 /* uV */
#define LN8000_ADC_IIN_STEP 4890 /* uA */
#define LN8000_IIN_CFG_MIN 500000 /* uA */
#define LN8000_IIN_CFG_LSB 50000 /* uA */

#define LN8000_BAT_OVP_DEFAULT 4440000 /* uV */
#define LN8000_BUS_OVP_DEFAULT 9500000
#define LN8000_BUS_OCP_DEFAULT 2000000 /* uA */
#define LN8000_NTC_ALARM_CFG_DEFAULT 226

#define LN8000_VAC_OVP_6P5V 0x0
#define LN8000_VAC_OVP_11V 0x1
#define LN8000_VAC_OVP_12V 0x2
#define LN8000_VAC_OVP_13V 0x3

enum ln8000_opmode {
	LN8000_OPMODE_UNKNOWN = 0,
	LN8000_OPMODE_STANDBY,
	LN8000_OPMODE_SWITCHING,
};

enum ln8000_adc_channel {
	LN8000_ADC_CH_VIN = 2,
	LN8000_ADC_CH_VBAT = 3,
	LN8000_ADC_CH_VAC,
	LN8000_ADC_CH_IIN,
	LN8000_ADC_CH_DIETEMP,
	LN8000_ADC_CH_TSBAT,
	LN8000_ADC_CH_TSBUS,
	LN8000_ADC_CH_ALL,
};

enum ln8000_adc_mode {
	ADC_AUTO_HIB_MODE = 0x0,
	ADC_SHUTDOWN_MODE = 0x2,
};

enum ln8000_adc_hib_delay {
	ADC_HIBERNATE_4S = 0x3,
};

/* Cooldown after a reverse-current trip before re-enabling direct charge. */
#define LN8000_DC_BACKOFF_MS 10000

struct ln8000_pdata {
	u32 bat_ovp_th; /* uV */
	u32 bus_ovp_th; /* uV */
	u32 bus_ocp_th; /* uA */
	u32 ntc_alarm_cfg;

	bool vbat_ovp_disable;
	bool vbat_reg_disable;
	bool iin_ocp_disable;
	bool iin_reg_disable;
	bool tbus_mon_disable;
	bool tbat_mon_disable;
};

struct ln8000_info {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock; /* protects cached status */

	struct ln8000_pdata pdata;

	unsigned int op_mode;

	bool tdie_fault;
	bool wdt_fault;
	bool vbat_ov;
	bool vbus_ov;
	bool iin_oc;
	bool chg_en;
	bool rcp_en;
	unsigned long dc_backoff_until;
};

static int ln8000_read(struct ln8000_info *info, u8 reg, unsigned int *val)
{
	int ret, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(info->regmap, reg, val);
		if (ret >= 0)
			return ret;
	}
	return ret;
}

static int ln8000_bulk_read(struct ln8000_info *info, u8 reg, u8 *buf,
			    int count)
{
	int ret, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_bulk_read(info->regmap, reg, buf, count);
		if (ret >= 0)
			return ret;
	}
	return ret;
}

static int ln8000_write(struct ln8000_info *info, u8 reg, u8 val)
{
	int ret, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_write(info->regmap, reg, val);
		if (ret >= 0)
			return ret;
	}
	return ret;
}

static int ln8000_update(struct ln8000_info *info, u8 reg, u8 mask, u8 val)
{
	int ret, i;

	for (i = 0; i < 3; i++) {
		ret = regmap_update_bits(info->regmap, reg, mask, val);
		if (ret >= 0)
			return ret;
	}
	return ret;
}

static int ln8000_set_vac_ovp(struct ln8000_info *info, unsigned int ovp_th)
{
	unsigned int cfg;

	if (ovp_th <= 6500000)
		cfg = LN8000_VAC_OVP_6P5V;
	else if (ovp_th <= 11000000)
		cfg = LN8000_VAC_OVP_11V;
	else if (ovp_th <= 12000000)
		cfg = LN8000_VAC_OVP_12V;
	else
		cfg = LN8000_VAC_OVP_13V;

	return ln8000_update(info, LN8000_REG_GLITCH_CTRL, 0x3 << 2, cfg << 2);
}

static int ln8000_set_vbat_float(struct ln8000_info *info, unsigned int cfg)
{
	unsigned int val;

	if (cfg < LN8000_VBAT_FLOAT_MIN)
		val = 0x00;
	else if (cfg > LN8000_VBAT_FLOAT_MAX)
		val = 0xFF;
	else
		val = (cfg - LN8000_VBAT_FLOAT_MIN) / LN8000_VBAT_FLOAT_LSB;

	return ln8000_write(info, LN8000_REG_V_FLOAT_CTRL, val);
}

static int ln8000_set_iin_limit(struct ln8000_info *info, unsigned int cfg)
{
	unsigned int val;

	cfg = min(cfg, LN8000_IIN_CFG_LSB * 0x7F);
	val = cfg / LN8000_IIN_CFG_LSB;

	return ln8000_update(info, LN8000_REG_IIN_CTRL, 0x7F, val);
}

static int ln8000_get_iin_limit(struct ln8000_info *info)
{
	unsigned int val;
	int ret, iin;

	ret = ln8000_read(info, LN8000_REG_IIN_CTRL, &val);
	if (ret < 0)
		return ret;

	iin = (val & 0x7F) * LN8000_IIN_CFG_LSB;
	if (iin < LN8000_IIN_CFG_MIN)
		iin = LN8000_IIN_CFG_MIN;

	return iin;
}

static int ln8000_set_ntc_alarm(struct ln8000_info *info, unsigned int cfg)
{
	int ret;

	ret = ln8000_write(info, LN8000_REG_NTC_CTRL, cfg & 0xFF);
	if (ret < 0)
		return ret;

	return ln8000_update(info, LN8000_REG_ADC_CTRL, 0x3, cfg >> 8);
}

static int ln8000_set_adc_ch(struct ln8000_info *info, unsigned int ch, bool en)
{
	u8 mask, val;

	if (ch < 1 || ch > LN8000_ADC_CH_ALL)
		return -EINVAL;

	if (ch == LN8000_ADC_CH_ALL) {
		val = en ? 0x3E : 0x00;
		return ln8000_write(info, LN8000_REG_ADC_CFG, val);
	}

	mask = 1 << (ch - 1);
	val = (en ? 1 : 0) << (ch - 1);
	return ln8000_update(info, LN8000_REG_ADC_CFG, mask, val);
}

static int ln8000_enable_tbus_monitor(struct ln8000_info *info, bool en)
{
	int ret;

	ret = ln8000_update(info, LN8000_REG_RECOVERY_CTRL, BIT(1), en << 1);
	if (ret < 0 || !en)
		return ret;

	return ln8000_set_adc_ch(info, LN8000_ADC_CH_TSBUS, true);
}

static int ln8000_enable_tbat_monitor(struct ln8000_info *info, bool en)
{
	int ret;

	ret = ln8000_update(info, LN8000_REG_RECOVERY_CTRL, BIT(0), en);
	if (ret < 0 || !en)
		return ret;

	return ln8000_set_adc_ch(info, LN8000_ADC_CH_TSBAT, true);
}

static int ln8000_get_adc_data(struct ln8000_info *info, unsigned int ch,
			       int *res)
{
	u8 sts[2] = { 0 };
	int adc_raw, adc_final = 0;
	int resume_ret;
	int ret;

	ret = ln8000_update(info, LN8000_REG_TIMER_CTRL,
			    BIT(LN8000_BIT_PAUSE_ADC_UPDATE),
			    BIT(LN8000_BIT_PAUSE_ADC_UPDATE));
	if (ret < 0)
		return ret;

	switch (ch) {
	case LN8000_ADC_CH_VIN:
		ret = ln8000_bulk_read(info, LN8000_REG_ADC03_STS, sts, 2);
		adc_raw = ((sts[1] & 0x3F) << 4) | ((sts[0] & 0xF0) >> 4);
		adc_final = adc_raw * LN8000_ADC_VIN_STEP;
		break;
	case LN8000_ADC_CH_VBAT:
		ret = ln8000_bulk_read(info, LN8000_REG_ADC06_STS, sts, 2);
		adc_raw = ((sts[1] & 0x03) << 8) | (sts[0] & 0xFF);
		adc_final = adc_raw * LN8000_ADC_VBAT_STEP;
		break;
	case LN8000_ADC_CH_IIN:
		ret = ln8000_bulk_read(info, LN8000_REG_ADC01_STS, sts, 2);
		adc_raw = ((sts[1] & 0x03) << 8) | (sts[0] & 0xFF);
		adc_final = adc_raw * LN8000_ADC_IIN_STEP;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	resume_ret = ln8000_update(info, LN8000_REG_TIMER_CTRL,
				   BIT(LN8000_BIT_PAUSE_ADC_UPDATE), 0);

	if (ret < 0)
		return ret;
	if (resume_ret < 0)
		return resume_ret;

	*res = adc_final;
	return 0;
}

static int ln8000_check_status(struct ln8000_info *info)
{
	u8 val[4];
	bool volt_qual;
	bool clear_latched = false;
	int ret;

	ret = ln8000_bulk_read(info, LN8000_REG_SYS_STS, val, 4);
	if (ret < 0)
		return ret;

	volt_qual = !(val[2] & 0x7F);

	mutex_lock(&info->lock);

	info->tdie_fault = val[1] & LN8000_MASK_TEMP_MAX_STS;
	info->wdt_fault = val[2] & LN8000_MASK_WATCHDOG_TIMER_STS;
	info->vbat_ov = val[2] & LN8000_MASK_VBAT_OV_STS;
	info->vbus_ov = val[2] & LN8000_MASK_VIN_OV_STS;
	if (volt_qual && info->chg_en) {
		volt_qual = !(val[3] & BIT(5));
		if (!volt_qual)
			clear_latched = true;
	}
	info->iin_oc = val[3] & LN8000_MASK_IIN_OC_DETECTED;

	mutex_unlock(&info->lock);

	if (clear_latched) {
		/* clear latched status */
		ln8000_update(info, LN8000_REG_TIMER_CTRL,
			      BIT(LN8000_BIT_CLEAR_LATCHED_STS),
			      BIT(LN8000_BIT_CLEAR_LATCHED_STS));
		ln8000_update(info, LN8000_REG_TIMER_CTRL,
			      BIT(LN8000_BIT_CLEAR_LATCHED_STS), 0);
	}

	return 0;
}

static int ln8000_enable_rcp(struct ln8000_info *info, bool en)
{
	WRITE_ONCE(info->rcp_en, en);
	return ln8000_update(info, LN8000_REG_SYS_CTRL,
			     BIT(LN8000_BIT_REV_IIN_DET),
			     en ? BIT(LN8000_BIT_REV_IIN_DET) : 0);
}

static int ln8000_update_opmode(struct ln8000_info *info)
{
	unsigned int op_mode;
	unsigned int val;
	int ret;

	ret = ln8000_read(info, LN8000_REG_SYS_STS, &val);
	if (ret < 0)
		return ret;

	if (val & LN8000_MASK_STANDBY_STS)
		op_mode = LN8000_OPMODE_STANDBY;
	else if (val & LN8000_MASK_SWITCHING_ENABLED)
		op_mode = LN8000_OPMODE_SWITCHING;
	else
		op_mode = LN8000_OPMODE_UNKNOWN;

	WRITE_ONCE(info->op_mode, op_mode);

	return 0;
}

static int ln8000_init_device(struct ln8000_info *info)
{
	struct ln8000_pdata *p = &info->pdata;
	unsigned int vbat_float;
	int ret;

	vbat_float = p->bat_ovp_th * 100 / 102; /* OVP = v_float x 1.02 */
	vbat_float = (vbat_float / 1000) * 1000;

	ret = ln8000_set_vbat_float(info, vbat_float);
	if (ret < 0)
		return ret;

	ret = ln8000_set_vac_ovp(info, p->bus_ovp_th);
	if (ret < 0)
		return ret;

	ret = ln8000_set_iin_limit(info, p->bus_ocp_th - 700000);
	if (ret < 0)
		return ret;

	ret = ln8000_set_ntc_alarm(info, p->ntc_alarm_cfg);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_REGULATION_CTRL, 0x3 << 2,
			    LN8000_NTC_SHUTDOWN_CFG);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_RECOVERY_CTRL, 0xF << 4, 0);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_FAULT_CTRL,
			    BIT(LN8000_BIT_DISABLE_VBAT_OV),
			    p->vbat_ovp_disable << LN8000_BIT_DISABLE_VBAT_OV);
	if (ret < 0)
		return ret;

	ret = ln8000_update(
		info, LN8000_REG_REGULATION_CTRL,
		BIT(LN8000_BIT_DISABLE_VFLOAT_LOOP) |
			BIT(LN8000_BIT_ENABLE_VFLOAT_LOOP_INT),
		p->vbat_reg_disable << LN8000_BIT_DISABLE_VFLOAT_LOOP |
			!p->vbat_reg_disable
				<< LN8000_BIT_ENABLE_VFLOAT_LOOP_INT);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_FAULT_CTRL,
			    BIT(LN8000_BIT_DISABLE_IIN_OCP),
			    p->iin_ocp_disable << LN8000_BIT_DISABLE_IIN_OCP);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_REGULATION_CTRL,
			    BIT(LN8000_BIT_DISABLE_IIN_LOOP) |
				    BIT(LN8000_BIT_ENABLE_IIN_LOOP_INT),
			    p->iin_reg_disable << LN8000_BIT_DISABLE_IIN_LOOP |
				    !p->iin_reg_disable
					    << LN8000_BIT_ENABLE_IIN_LOOP_INT);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_SYS_CTRL,
			    BIT(LN8000_BIT_REV_IIN_DET), 0);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_SYS_CTRL,
			    BIT(LN8000_BIT_STANDBY_EN),
			    BIT(LN8000_BIT_STANDBY_EN));
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_FAULT_CTRL,
			    BIT(LN8000_BIT_DISABLE_VAC_OV), 0);
	if (ret < 0)
		return ret;

	/* watchdog off, ADC channels on */
	ret = ln8000_update(info, LN8000_REG_TIMER_CTRL, BIT(7), 0);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_ADC_CTRL, 0x7 << 5,
			    ADC_SHUTDOWN_MODE << 5);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_ADC_CTRL, 0x3 << 3,
			    ADC_HIBERNATE_4S << 3);
	if (ret < 0)
		return ret;

	ret = ln8000_set_adc_ch(info, LN8000_ADC_CH_ALL, true);
	if (ret < 0)
		return ret;

	ret = ln8000_enable_tbus_monitor(info, !p->tbus_mon_disable);
	if (ret < 0)
		return ret;

	ret = ln8000_enable_tbat_monitor(info, !p->tbat_mon_disable);
	if (ret < 0)
		return ret;

	ret = ln8000_update(info, LN8000_REG_ADC_CTRL, 0x7 << 5,
			    ADC_AUTO_HIB_MODE << 5);
	if (ret < 0)
		return ret;

	/* mark SW-initialised (CHARGE_CTRL bit 7) */
	ret = ln8000_update(info, LN8000_REG_CHARGE_CTRL, BIT(7), BIT(7));
	if (ret < 0)
		return ret;

	ret = ln8000_write(info, LN8000_REG_THRESHOLD_CTRL, 0x0E);
	if (ret < 0)
		return ret;

	return 0;
}

static int ln8000_enable_charging(struct ln8000_info *info, bool en)
{
	int ret;

	/*
	 * Serialize against the coordinator's evaluate_direct_charge() and the
	 * threaded IRQ handler - both can reach this now that the coordinator
	 * owns the enable/disable decision.
	 */
	guard(mutex)(&info->lock);

	if (en) {
		/* Disable reverse-current protection at start-up; re-armed below
		 * once switching is confirmed.
		 */
		ret = ln8000_enable_rcp(info, false);
		if (ret < 0)
			return ret;

		ret = ln8000_update(info, LN8000_REG_SYS_CTRL,
				    BIT(LN8000_BIT_STANDBY_EN), 0);
		if (ret < 0)
			return ret;

		usleep_range(10000, 11000);

		ret = ln8000_update_opmode(info);
		if (ret < 0)
			return ret;

		if (READ_ONCE(info->op_mode) != LN8000_OPMODE_SWITCHING) {
			/*
			 * Not switching yet - usually VBUS hasn't settled at
			 * the HV PDO during PD negotiation.  Roll back to standby
			 * (so the next attempt re-arms the standby->switching
			 * transition) and tell the coordinator to retry: -EAGAIN
			 * is silent there.  Visible at debug level.
			 */
			dev_dbg(info->dev,
				"LN8000 failed to enter switching mode\n");
			ln8000_update(info, LN8000_REG_SYS_CTRL,
				      BIT(LN8000_BIT_STANDBY_EN),
				      BIT(LN8000_BIT_STANDBY_EN));
			return -EAGAIN;
		}

		/*
		 * The coordinator suspends the PMIC USBIN charger to avoid
		 * parallel charging (it owns the handover now).
		 */
		WRITE_ONCE(info->chg_en, true);

		/*
		 * Charging established (switching confirmed, startup transient
		 * passed): arm reverse-current protection.  RCP only trips on
		 * battery back-feed, so arming is safe regardless of forward
		 * current.  Was armed by the periodic monitor; event-driven here.
		 */
		if (ln8000_enable_rcp(info, true))
			dev_warn(info->dev,
				 "failed to arm reverse-current protection\n");
	} else {
		ret = ln8000_update(info, LN8000_REG_SYS_CTRL,
				    BIT(LN8000_BIT_STANDBY_EN),
				    BIT(LN8000_BIT_STANDBY_EN));
		if (ret < 0)
			return ret;

		WRITE_ONCE(info->rcp_en, false);
		WRITE_ONCE(info->chg_en, false);
	}

	return 0;
}

static bool ln8000_has_fault(struct ln8000_info *info)
{
	return READ_ONCE(info->vbat_ov) || READ_ONCE(info->vbus_ov) ||
	       READ_ONCE(info->iin_oc) || READ_ONCE(info->tdie_fault) ||
	       READ_ONCE(info->wdt_fault);
}

static int ln8000_get_status(void *priv, int *status)
{
	struct ln8000_info *info = priv;

	if (READ_ONCE(info->chg_en) &&
	    READ_ONCE(info->op_mode) == LN8000_OPMODE_SWITCHING)
		*status = POWER_SUPPLY_STATUS_CHARGING;
	else
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

static int ln8000_get_online(void *priv, int *online)
{
	struct ln8000_info *info = priv;

	*online = READ_ONCE(info->chg_en) &&
		  READ_ONCE(info->op_mode) == LN8000_OPMODE_SWITCHING;
	return 0;
}

static int ln8000_get_current_now(void *priv, int *val)
{
	struct ln8000_info *info = priv;
	int iin;
	int ret;

	ret = ln8000_get_adc_data(info, LN8000_ADC_CH_IIN, &iin);
	if (ret < 0)
		return ret;

	*val = iin * 2;
	return 0;
}

static int ln8000_get_voltage_now(void *priv, int *val)
{
	struct ln8000_info *info = priv;

	return ln8000_get_adc_data(info, LN8000_ADC_CH_VIN, val);
}

static int ln8000_get_input_current_limit(void *priv, int *val)
{
	struct ln8000_info *info = priv;
	int ret;

	ret = ln8000_get_iin_limit(info);
	if (ret < 0)
		return ret;

	*val = ret;
	return 0;
}

static int ln8000_get_health(void *priv, int *val)
{
	struct ln8000_info *info = priv;

	if (READ_ONCE(info->vbat_ov) || READ_ONCE(info->vbus_ov))
		*val = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else if (READ_ONCE(info->iin_oc))
		*val = POWER_SUPPLY_HEALTH_OVERCURRENT;
	else if (READ_ONCE(info->tdie_fault))
		*val = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (READ_ONCE(info->wdt_fault))
		*val = POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE;
	else
		*val = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static int ln8000_set_charging_enabled(void *priv, bool en)
{
	struct ln8000_info *info = priv;

	/*
	 * Refuse to re-enable during the cooldown after a reverse-current trip;
	 * the coordinator retries on its next tick.  Disables always proceed.
	 */
	if (en && time_before(jiffies, READ_ONCE(info->dc_backoff_until)))
		return -EAGAIN;

	return ln8000_enable_charging(info, en);
}

static const struct qcom_bms_charger_ops ln8000_charger_ops = {
	.get_status = ln8000_get_status,
	.get_online = ln8000_get_online,
	.get_current_now = ln8000_get_current_now,
	.get_voltage_now = ln8000_get_voltage_now,
	.get_input_current_limit = ln8000_get_input_current_limit,
	.get_health = ln8000_get_health,
	.set_charging_enabled = ln8000_set_charging_enabled,
};

static irqreturn_t ln8000_irq_handler(int irq, void *data)
{
	struct ln8000_info *info = data;
	unsigned int int_reg;
	unsigned int int_msk;
	unsigned int masked;
	int resume_ret;
	int ret;

	/* Pause interrupt updates while reading the latched status. */
	ret = ln8000_update(info, LN8000_REG_TIMER_CTRL, BIT(0), BIT(0));
	if (ret < 0)
		goto out_reschedule;

	usleep_range(1000, 2000);
	ret = ln8000_read(info, LN8000_REG_INT1, &int_reg);
	resume_ret = ln8000_update(info, LN8000_REG_TIMER_CTRL, BIT(0), 0);
	if (ret < 0)
		goto out_reschedule;
	if (resume_ret < 0) {
		ret = resume_ret;
		goto out_reschedule;
	}

	ret = ln8000_read(info, LN8000_REG_INT1_MSK, &int_msk);
	if (ret < 0)
		goto out_reschedule;
	masked = int_reg & ~int_msk;

	ret = ln8000_check_status(info);
	if (ret < 0)
		goto out_reschedule;

	/* Any fault must stop direct charging immediately. */
	if (masked & (LN8000_MASK_FAULT_INT | LN8000_MASK_TEMP_INT |
		      LN8000_MASK_NTC_PROT_INT) &&
	    ln8000_has_fault(info) && READ_ONCE(info->chg_en)) {
		ret = ln8000_enable_charging(info, false);
		if (ret < 0)
			dev_err(info->dev,
				"failed to disable direct charge after IRQ: %d\n",
				ret);
		else
			qcom_bms_notify_changed(info->dev);
	}

	/*
	 * Reverse-current trip: the SC was back-fed by the battery.  Stop
	 * switching and arm a cooldown so the coordinator's re-enable attempts
	 * (set_charging_enabled returns -EAGAIN) don't oscillate while the
	 * adapter recovers or the PMIC lifts VBAT.
	 */
	if ((masked & LN8000_MASK_REV_CURR_INT) && READ_ONCE(info->chg_en)) {
		dev_warn(info->dev,
			 "reverse current detected, disabling direct charge\n");
		WRITE_ONCE(info->dc_backoff_until,
			   jiffies + msecs_to_jiffies(LN8000_DC_BACKOFF_MS));
		ret = ln8000_enable_charging(info, false);
		if (ret < 0)
			dev_err(info->dev,
				"failed to disable direct charge after RCP trip: %d\n",
				ret);
		else
			qcom_bms_notify_changed(info->dev);
	}

	/*
	 * Mode change (e.g. the SC dropped switching without a fault): the
	 * cached op_mode was refreshed above by check_status(); re-evaluate so
	 * the coordinator can recover (re-enable direct charge, or fall back to
	 * the PMIC charger via the USBIN handover).  Was the periodic monitor's
	 * job; event-driven here.
	 */
	if (masked & LN8000_MASK_MODE_INT)
		qcom_bms_notify_changed(info->dev);

out_reschedule:
	if (ret < 0) {
		dev_warn_ratelimited(
			info->dev, "failed to process charger IRQ: %d\n", ret);

		/*
		 * Communication failure: cached state is stale; safely disable
		 * direct charge (was the periodic monitor's job) and let the PMIC
		 * charger take over.
		 */
		if (READ_ONCE(info->chg_en)) {
			if (ln8000_enable_charging(info, false))
				dev_err(info->dev,
					"failed to disable direct charge after I2C error\n");
			else
				qcom_bms_notify_changed(info->dev);
		}
	}
	return IRQ_HANDLED;
}

static int ln8000_irq_init(struct ln8000_info *info)
{
	struct ln8000_pdata *p = &info->pdata;
	unsigned int mask;

	mask = LN8000_MASK_ADC_DONE_INT | LN8000_MASK_TIMER_INT;
	if (p->iin_reg_disable && p->vbat_reg_disable)
		mask |= LN8000_MASK_CHARGE_PHASE_INT;
	if (p->tbat_mon_disable && p->tbus_mon_disable)
		mask |= LN8000_MASK_NTC_PROT_INT;

	return ln8000_write(info, LN8000_REG_INT1_MSK, mask);
}

static void ln8000_parse_dt(struct ln8000_info *info)
{
	struct device *dev = info->dev;
	struct ln8000_pdata *p = &info->pdata;
	u32 prop;

	p->bat_ovp_th = LN8000_BAT_OVP_DEFAULT;
	if (!device_property_read_u32(
		    dev, "lionsemi,bat-ovp-threshold-microvolt", &prop))
		p->bat_ovp_th = prop;

	p->bus_ovp_th = LN8000_BUS_OVP_DEFAULT;
	if (!device_property_read_u32(
		    dev, "lionsemi,bus-ovp-threshold-microvolt", &prop))
		p->bus_ovp_th = prop;

	p->bus_ocp_th = LN8000_BUS_OCP_DEFAULT;
	if (!device_property_read_u32(
		    dev, "lionsemi,bus-ocp-threshold-microamp", &prop))
		p->bus_ocp_th = prop;

	if (p->bus_ocp_th < 700000 + LN8000_IIN_CFG_MIN) {
		dev_warn(dev,
			 "bus-ocp-threshold too low (%u uA), using default\n",
			 p->bus_ocp_th);
		p->bus_ocp_th = LN8000_BUS_OCP_DEFAULT;
	}

	p->ntc_alarm_cfg = LN8000_NTC_ALARM_CFG_DEFAULT;
	device_property_read_u32(dev, "lionsemi,ntc-alarm-cfg",
				 &p->ntc_alarm_cfg);

	p->vbat_ovp_disable =
		device_property_read_bool(dev, "lionsemi,vbat-ovp-disable");
	p->vbat_reg_disable =
		device_property_read_bool(dev, "lionsemi,vbat-reg-disable");
	p->iin_ocp_disable =
		device_property_read_bool(dev, "lionsemi,iin-ocp-disable");
	p->iin_reg_disable =
		device_property_read_bool(dev, "lionsemi,iin-reg-disable");
	p->tbus_mon_disable =
		device_property_read_bool(dev, "lionsemi,tbus-mon-disable");
	p->tbat_mon_disable =
		device_property_read_bool(dev, "lionsemi,tbat-mon-disable");
}

static const struct regmap_config ln8000_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LN8000_REG_MAX,
};

static int ln8000_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ln8000_info *info;
	unsigned int dev_id;
	int ret;

	ret = i2c_smbus_read_byte_data(client, LN8000_REG_DEVICE_ID);
	if (ret < 0) {
		dev_err(dev, "no LN8000 at addr 0x%02x\n", client->addr);
		return -ENODEV;
	}
	dev_id = ret;
	if (dev_id != LN8000_DEVICE_ID) {
		dev_err(dev, "unknown device id 0x%02x at addr 0x%02x\n",
			dev_id, client->addr);
		return -ENODEV;
	}
	dev_info(dev, "LN8000 detected, device id=0x%02x\n", dev_id);

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	mutex_init(&info->lock);

	ln8000_parse_dt(info);

	info->regmap = devm_regmap_init_i2c(client, &ln8000_regmap_config);
	if (IS_ERR(info->regmap))
		return dev_err_probe(dev, PTR_ERR(info->regmap),
				     "failed to init regmap\n");

	i2c_set_clientdata(client, info);

	ret = ln8000_init_device(info);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to init device\n");

	ret = ln8000_irq_init(info);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to init IRQ\n");

	if (client->irq) {
		ret = devm_request_threaded_irq(
			dev, client->irq, NULL, ln8000_irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "ln8000-charger",
			info);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "failed to request IRQ\n");
		enable_irq_wake(client->irq);
	}

	ret = qcom_bms_register_charger(dev, QCOM_BMS_CHARGER_DIRECT,
					&ln8000_charger_ops, info);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to register with qcom_bms\n");

	device_init_wakeup(dev, client->irq > 0);

	return 0;
}

static void ln8000_remove(struct i2c_client *client)
{
	struct ln8000_info *info = i2c_get_clientdata(client);

	if (READ_ONCE(info->chg_en))
		ln8000_enable_charging(info, false);
	qcom_bms_unregister_charger(&client->dev);
}

static void ln8000_shutdown(struct i2c_client *client)
{
	struct ln8000_info *info = i2c_get_clientdata(client);

	ln8000_enable_charging(info, false);
}

static const struct of_device_id ln8000_of_match[] = {
	{ .compatible = "lionsemi,ln8000" },
	{}
};
MODULE_DEVICE_TABLE(of, ln8000_of_match);

static const struct i2c_device_id ln8000_i2c_ids[] = { { "ln8000" }, {} };
MODULE_DEVICE_TABLE(i2c, ln8000_i2c_ids);

static struct i2c_driver ln8000_driver = {
	.driver = {
		.name = "ln8000-charger",
		.of_match_table = ln8000_of_match,
	},
	.probe = ln8000_probe,
	.remove = ln8000_remove,
	.shutdown = ln8000_shutdown,
	.id_table = ln8000_i2c_ids,
};
module_i2c_driver(ln8000_driver);

MODULE_AUTHOR("sungdae choi <sungdae@lionsemi.com>");
MODULE_AUTHOR("TwinbornPlate75 <3342733415@qq.com>");
MODULE_DESCRIPTION("LION Semiconductor LN8000 switched-capacitor charger");
MODULE_LICENSE("GPL");
