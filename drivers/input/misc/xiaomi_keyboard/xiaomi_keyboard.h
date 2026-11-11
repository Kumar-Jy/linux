#ifndef __XIAOMI_KEYBOARD_H
#define __XIAOMI_KEYBOARD_H

#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>

#define XIAOMI_KB_TAG "xiaomi-keyboard"
#define MI_KB_INFO(fmt, args...) \
	pr_info("[%s] %s %d: " fmt, XIAOMI_KB_TAG, __func__, __LINE__, ##args)
#define MI_KB_ERR(fmt, args...) \
	pr_err("[%s] %s %d: " fmt, XIAOMI_KB_TAG, __func__, __LINE__, ##args)

struct xiaomi_keyboard_platdata {
	u32 rst_gpio;
	u32 rst_flags;
	u32 in_irq_gpio;
	u32 in_irq_flags;
	u32 vdd_gpio;
};

struct xiaomi_keyboard_data {
	struct pinctrl *pinctrl;
	struct platform_device *pdev;
	struct pinctrl_state *pins_active;
	struct pinctrl_state *pins_suspend;
	struct notifier_block lid_notif;
	struct notifier_block drm_notif;
	/* Single work applying the state below, serialized by lock */
	struct work_struct state_work;
	struct mutex lock;
	struct xiaomi_keyboard_platdata *pdata;

	int irq;
	bool irq_wake_enabled;
	bool lid_is_closed;
	/* Keyboard hardware currently powered on */
	bool powered_on;
	bool is_in_suspend;
	/* Enable switch set from userspace via sysfs */
	bool user_enabled;
	bool screen_is_on;
};
#endif
