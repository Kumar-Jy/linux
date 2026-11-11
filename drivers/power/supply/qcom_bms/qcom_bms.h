/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Internal API for the monolithic Qualcomm battery/charger manager.
 * This header is shared between qcom_bms.c and its sub-modules.
 */

#ifndef __QCOM_BMS_H__
#define __QCOM_BMS_H__

#include <linux/device.h>
#include <linux/types.h>

struct qcom_bms_fg_ops {
	int (*get_capacity)(void *priv, int *capacity);
	int (*get_capacity_level)(void *priv, int *level);
	int (*get_current)(void *priv, int *current_ua);
	int (*get_voltage)(void *priv, int *voltage_uv);
	int (*get_power)(void *priv, int *power_uw);
	int (*get_voltage_min_design)(void *priv, int *voltage_uv);
	int (*get_voltage_max_design)(void *priv, int *voltage_uv);
	int (*get_charge_full_design)(void *priv, int *uah);
	int (*get_charge_full)(void *priv, int *uah);
	int (*get_charge_now)(void *priv, int *uah);
	int (*get_energy_full_design)(void *priv, int *uwh);
	int (*get_energy_full)(void *priv, int *uwh);
	int (*get_energy_now)(void *priv, int *uwh);
	int (*get_cycle_count)(void *priv, int *count);
	int (*get_health)(void *priv, int *health);
	int (*get_present)(void *priv, int *present);
	int (*get_temp)(void *priv, int *temp);
	int (*get_scope)(void *priv, int *scope);
	int (*get_technology)(void *priv, int *technology);
	int (*get_manufacturer)(void *priv, const char **name);
	int (*get_model_name)(void *priv, const char **name);

	/* Optional setter forwarded from qcom-bms */
	int (*set_charge_full)(void *priv, int uah);

	/* Charging state supplied by the coordinator's charger backends. */
	void (*charging_state_changed)(void *priv, int status, bool charge_done,
				       bool input_present);
};

struct qcom_bms_charger_ops {
	int (*get_status)(void *priv, int *status);
	int (*get_online)(void *priv, int *online);
	int (*get_current_now)(void *priv, int *current_ua);
	int (*get_voltage_now)(void *priv, int *voltage_uv);
	int (*get_input_current_limit)(void *priv, int *current_ua);
	int (*get_usb_type)(void *priv, int *type);
	int (*get_health)(void *priv, int *health);

	/*
	 * true  -> allow PMIC USBIN charging
	 * false -> suspend PMIC USBIN so direct charger can take over
	 */
	int (*set_usbin_suspend)(void *priv, bool suspend);

	/* Inform the charger whether all required batteries are authentic. */
	int (*set_authenticated)(void *priv, bool authenticated);

	/*
	 * Turn the direct-charge path on/off.  The coordinator decides when (based
	 * on charger connection state, authentication, faults, VBAT) and owns the
	 * PMIC USBIN handover; the charger implements only the SC standby mechanism.
	 */
	int (*set_charging_enabled)(void *priv, bool en);
};

enum qcom_bms_charger_role {
	QCOM_BMS_CHARGER_PMIC,
	QCOM_BMS_CHARGER_DIRECT,
};

enum qcom_bms_auth_role {
	QCOM_BMS_AUTH_PRIMARY,
	QCOM_BMS_AUTH_SECONDARY,
	QCOM_BMS_AUTH_MAX,
};

int qcom_bms_register_fg(struct device *dev, const struct qcom_bms_fg_ops *ops,
			 void *priv);
int qcom_bms_register_charger(struct device *dev,
			      enum qcom_bms_charger_role role,
			      const struct qcom_bms_charger_ops *ops,
			      void *priv);
void qcom_bms_unregister_fg(struct device *dev);
void qcom_bms_unregister_charger(struct device *dev);
int qcom_bms_register_authenticator(struct device *dev,
				    enum qcom_bms_auth_role role);
void qcom_bms_unregister_authenticator(struct device *dev);
void qcom_bms_authentication_changed(struct device *dev, bool authenticated);
void qcom_bms_notify_changed(struct device *dev);

#endif /* __QCOM_BMS_H__ */
