#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/consumer.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include "xiaomi_keyboard.h"
#include <drm/drm_notifier.h>
#include <linux/gpio_keys.h>
#include <linux/notifier.h>
#include <linux/input/usbkbd-xiaomi.h>

static struct xiaomi_keyboard_data *mdata;

static void xiaomi_keyboard_reset(void)
{
	struct gpio_desc *rst_desc;

	if (!mdata || !mdata->pdata) {
		MI_KB_ERR("reset failed!Invalid Memory\n");
		return;
	}

	rst_desc = gpio_to_desc(mdata->pdata->rst_gpio);
	if (IS_ERR(rst_desc)) {
		MI_KB_ERR("failed to get rst gpio desc\n");
		return;
	}

	MI_KB_INFO("xiaomi keyboard IC Reset\n");
	gpiod_direction_output(rst_desc, 0);
	msleep(2);
	gpiod_direction_output(rst_desc, 1);
}

static ssize_t xiaomi_keyboard_enabled_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	if (!mdata)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%d", mdata->user_enabled);
}

static ssize_t xiaomi_keyboard_enabled_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	int value;

	if (!mdata || kstrtoint(buf, 10, &value))
		return -EINVAL;

	if (value != 0 && value != 1) {
		mutex_lock(&mdata->lock);
		xiaomi_keyboard_reset();
		mutex_unlock(&mdata->lock);
		return count;
	}

	mutex_lock(&mdata->lock);
	mdata->user_enabled = value;
	mutex_unlock(&mdata->lock);

	schedule_work(&mdata->state_work);

	return count;
}

DEVICE_ATTR(xiaomi_keyboard_enabled, (S_IRUGO | S_IWUSR | S_IWGRP),
	    xiaomi_keyboard_enabled_show, xiaomi_keyboard_enabled_store);

static struct attribute *sysfs_attrs[] = {
	&dev_attr_xiaomi_keyboard_enabled.attr,
	NULL,
};

static const struct attribute_group xiaomi_attribute_group = {
	.attrs = sysfs_attrs,
};

static irqreturn_t xiaomi_keyboard_irq_func(int irq, void *data)
{
	pm_wakeup_event(&mdata->pdev->dev, 500);
	return IRQ_HANDLED;
}

static int xiaomi_keyboard_gpio_config(struct xiaomi_keyboard_platdata *pdata,
					struct device *dev)
{
	int ret = 0;
	struct gpio_desc *rst_desc, *irq_desc;

	rst_desc = gpio_to_desc(pdata->rst_gpio);
	if (IS_ERR(rst_desc)) {
		MI_KB_ERR("failed to get rst gpio desc\n");
		return PTR_ERR(rst_desc);
	}

	ret = gpiod_direction_output(rst_desc, 0);
	if (ret) {
		MI_KB_ERR("Failed to set rst gpio direction\n");
		return ret;
	}

	irq_desc = gpio_to_desc(pdata->in_irq_gpio);
	if (IS_ERR(irq_desc)) {
		MI_KB_ERR("failed to get irq gpio desc\n");
		return PTR_ERR(irq_desc);
	}

	ret = gpiod_direction_input(irq_desc);
	if (ret) {
		MI_KB_ERR("Failed to set irq gpio direction\n");
		return ret;
	}

	return ret;
}

static int xiaomi_keyboard_setup_gpio(struct xiaomi_keyboard_platdata *pdata)
{
	int ret = 0;
	struct gpio_desc *rst_desc, *irq_desc;

	if (!pdata) {
		MI_KB_ERR("xiaomi keyboard platdata is NULL\n");
		return -EINVAL;
	}

	rst_desc = gpio_to_desc(pdata->rst_gpio);
	if (IS_ERR(rst_desc)) {
		MI_KB_ERR("failed to get rst gpio desc\n");
		return PTR_ERR(rst_desc);
	}

	ret = gpiod_direction_output(rst_desc, 1);
	if (ret) {
		MI_KB_ERR("Failed to set rst gpio direction\n");
		return ret;
	}

	irq_desc = gpio_to_desc(pdata->in_irq_gpio);
	if (IS_ERR(irq_desc)) {
		MI_KB_ERR("failed to get irq gpio desc\n");
		return PTR_ERR(irq_desc);
	}

	mdata->irq = gpiod_to_irq(irq_desc);
	if (mdata->irq > 0) {
		ret = request_threaded_irq(mdata->irq, NULL,
					   xiaomi_keyboard_irq_func,
					   IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					   "MiKB-IRQ", mdata);
		if (ret != 0) {
			MI_KB_ERR("request threaded irq failed\n");
			return ret;
		}
	}

	return ret;
}

static int xiaomi_keyboard_resetup_gpio(struct xiaomi_keyboard_platdata *pdata)
{
	int ret = 0;
	struct gpio_desc *rst_desc;

	if (!mdata || !pdata) {
		MI_KB_ERR("mdata or pdata not ready, return!");
		return -EINVAL;
	}

	rst_desc = gpio_to_desc(pdata->rst_gpio);
	if (IS_ERR(rst_desc)) {
		MI_KB_ERR("failed to get rst gpio desc\n");
		return PTR_ERR(rst_desc);
	}

	ret = gpiod_direction_output(rst_desc, 0);
	if (ret) {
		MI_KB_ERR("Failed to set rst_gpio to output low\n");
		return ret;
	}

	free_irq(mdata->irq, mdata);

	return ret;
}

#ifdef CONFIG_OF
static int xiaomi_keyboard_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct xiaomi_keyboard_platdata *pdata;

	pdata = mdata->pdata;

	pdata->rst_gpio = of_get_named_gpio(np, "xiaomi-keyboard,rst-gpio", 0);
	if (pdata->rst_gpio < 0) {
		MI_KB_ERR("failed to get rst gpio: %d\n", pdata->rst_gpio);
		return pdata->rst_gpio;
	}
	MI_KB_INFO("xiaomi-kb,reset-gpio=%d\n", pdata->rst_gpio);

	pdata->in_irq_gpio =
		of_get_named_gpio(np, "xiaomi-keyboard,in-irq-gpio", 0);
	if (pdata->in_irq_gpio < 0) {
		MI_KB_ERR("failed to get in-irq gpio: %d\n", pdata->in_irq_gpio);
		return pdata->in_irq_gpio;
	}
	MI_KB_INFO("xiaomi-kb,in-irq-gpio=%d\n", pdata->in_irq_gpio);

	pdata->vdd_gpio = of_get_named_gpio(np, "xiaomi-keyboard,vdd-gpio", 0);
	if (pdata->vdd_gpio < 0) {
		MI_KB_ERR("failed to get vdd gpio: %d\n", pdata->vdd_gpio);
		return pdata->vdd_gpio;
	}
	MI_KB_INFO("xiaomi-kb,vdd-gpio=%d\n", pdata->vdd_gpio);

	return 0;
}
#else
static int xiaomi_keyboard_parse_dt(struct device *dev)
{
	MI_KB_ERR("Xiaomi Keyboard dev is not defined\n");
	return -EINVAL;
}
#endif

static int xiaomi_keyboard_pinctrl_init(struct device *dev)
{
	int ret = 0;

	mdata->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(mdata->pinctrl)) {
		MI_KB_ERR("Failed to get pinctrl, please check dts\n");
		ret = PTR_ERR(mdata->pinctrl);
		goto err_pinctrl_get;
	}
	mdata->pins_active =
		pinctrl_lookup_state(mdata->pinctrl, "pm_kb_active");
	if (IS_ERR_OR_NULL(mdata->pins_active)) {
		MI_KB_ERR("Pin state [active] not found\n");
		ret = PTR_ERR(mdata->pins_active);
		goto err_pinctrl_lookup;
	}

	mdata->pins_suspend =
		pinctrl_lookup_state(mdata->pinctrl, "pm_kb_suspend");
	if (IS_ERR_OR_NULL(mdata->pins_suspend)) {
		MI_KB_ERR("Pin state [suspend] not found\n");
		ret = PTR_ERR(mdata->pins_suspend);
		goto err_pinctrl_lookup;
	}

	return 0;
err_pinctrl_lookup:
	if (mdata->pinctrl) {
		devm_pinctrl_put(mdata->pinctrl);
	}
err_pinctrl_get:
	return ret;
}

static int xiaomi_keyboard_power_on(void)
{
	int ret = 0;
	struct gpio_desc *vdd_desc;
	struct xiaomi_keyboard_platdata *pdata;

	pdata = mdata->pdata;
	MI_KB_INFO("Power On\n");

	vdd_desc = gpio_to_desc(pdata->vdd_gpio);
	if (IS_ERR(vdd_desc)) {
		MI_KB_ERR("failed to get vdd gpio desc\n");
		return PTR_ERR(vdd_desc);
	}

	ret = gpiod_direction_output(vdd_desc, 1);
	if (ret) {
		MI_KB_ERR("Failed to set vdd gpio direction\n");
		return ret;
	}

	return ret;
}

static void xiaomi_keyboard_power_off(void)
{
	struct gpio_desc *vdd_desc;
	struct xiaomi_keyboard_platdata *pdata;

	pdata = mdata->pdata;
	MI_KB_INFO("Power Off\n");

	vdd_desc = gpio_to_desc(pdata->vdd_gpio);
	if (IS_ERR(vdd_desc)) {
		MI_KB_ERR("failed to get vdd gpio desc\n");
		return;
	}

	gpiod_direction_output(vdd_desc, 0);
}

static int xiaomi_keyboard_pm_suspend(struct device *dev)
{
	mutex_lock(&mdata->lock);
	if (mdata->powered_on && mdata->irq > 0 &&
	    !mdata->irq_wake_enabled) {
		if (enable_irq_wake(mdata->irq)) {
			MI_KB_ERR("enable irq wake failed\n");
		} else {
			mdata->irq_wake_enabled = true;
		}
	}
	mutex_unlock(&mdata->lock);
	return 0;
}

static int xiaomi_keyboard_pm_resume(struct device *dev)
{
	mutex_lock(&mdata->lock);
	if (mdata->irq_wake_enabled) {
		disable_irq_wake(mdata->irq);
		mdata->irq_wake_enabled = false;
	}
	mutex_unlock(&mdata->lock);
	return 0;
}

static const struct dev_pm_ops xiaomi_keyboard_pm_ops = {
	.suspend = xiaomi_keyboard_pm_suspend,
	.resume = xiaomi_keyboard_pm_resume,
};

/* Called with mdata->lock held */
static void keyboard_pinctrl_apply_locked(bool active)
{
	struct pinctrl_state *state;
	int ret;

	state = active ? mdata->pins_active : mdata->pins_suspend;
	ret = pinctrl_select_state(mdata->pinctrl, state);
	if (ret < 0) {
		MI_KB_ERR("Set %s pin state error: %d\n",
			  active ? "active" : "suspend", ret);
		return;
	}
	mdata->is_in_suspend = !active;
}

/*
 * Bring the hardware to the state described by user_enabled,
 * lid_is_closed and screen_is_on. Called with mdata->lock held.
 */
static void keyboard_apply_state_locked(void)
{
	bool on = mdata->user_enabled && !mdata->lid_is_closed;
	int ret;

	if (on && !mdata->powered_on) {
		ret = xiaomi_keyboard_power_on();
		if (ret) {
			MI_KB_ERR("Init 3.3V power failed\n");
			return;
		}
		msleep(1);
		ret = xiaomi_keyboard_setup_gpio(mdata->pdata);
		if (ret) {
			MI_KB_ERR("setup gpio failed\n");
			xiaomi_keyboard_power_off();
			return;
		}
		msleep(2);

		keyboard_pinctrl_apply_locked(mdata->screen_is_on);
		mdata->powered_on = true;
		xiaomi_keyboard_connection_change(true);
	} else if (!on && mdata->powered_on) {
		xiaomi_keyboard_connection_change(false);
		keyboard_pinctrl_apply_locked(false);
		ret = xiaomi_keyboard_resetup_gpio(mdata->pdata);
		if (ret < 0)
			MI_KB_ERR("resetup gpio failed\n");
		xiaomi_keyboard_power_off();
		mdata->powered_on = false;
	} else if (on && mdata->is_in_suspend == mdata->screen_is_on) {
		/* Screen blanked or unblanked while the keyboard stays powered */
		keyboard_pinctrl_apply_locked(mdata->screen_is_on);
	} else {
		MI_KB_INFO("keyboard status do not need change!");
	}
}

static void keyboard_state_work(struct work_struct *work)
{
	struct xiaomi_keyboard_data *data =
		container_of(work, struct xiaomi_keyboard_data, state_work);

	mutex_lock(&data->lock);
	keyboard_apply_state_locked();
	mutex_unlock(&data->lock);
}

static int keyboard_drm_notifier_callback(struct notifier_block *self,
					  unsigned long event, void *data)
{
	struct xiaomi_keyboard_data *kbdata =
		container_of(self, struct xiaomi_keyboard_data, drm_notif);
	int blank;
	bool screen_is_on;
	bool changed = false;

	if (!data)
		return NOTIFY_OK;

	blank = *(int *)data;

	if (event == MI_DRM_EARLY_EVENT_BLANK) {
		if (blank == MI_DRM_BLANK_POWERDOWN) {
			screen_is_on = false;
			changed = true;
		}
	} else if (event == MI_DRM_EVENT_BLANK) {
		if (blank == MI_DRM_BLANK_UNBLANK) {
			screen_is_on = true;
			changed = true;
		}
	}

	if (!changed)
		return NOTIFY_OK;

	mutex_lock(&kbdata->lock);
	kbdata->screen_is_on = screen_is_on;
	mutex_unlock(&kbdata->lock);

	schedule_work(&kbdata->state_work);

	return NOTIFY_OK;
}

static int xiaomi_keyboard_lid_notifier_callback(struct notifier_block *self,
						 unsigned long code,
						 void *state)
{
	struct xiaomi_keyboard_data *kbdata =
		container_of(self, struct xiaomi_keyboard_data, lid_notif);
	bool lid_is_closed = *(int *)state;
	bool changed = false;

	mutex_lock(&kbdata->lock);
	if (lid_is_closed != kbdata->lid_is_closed) {
		kbdata->lid_is_closed = lid_is_closed;
		changed = true;
	}
	mutex_unlock(&kbdata->lock);

	if (changed) {
		MI_KB_INFO("lid state: %s\n",
			   lid_is_closed ? "closed" : "open");
		schedule_work(&kbdata->state_work);
	}

	return NOTIFY_OK;
}

/*******************************************************
Description:
	xiami pad keyboard driver probe function.

return:
	Executive outcomes. 0---succeed. negative---failed
*******************************************************/
static int xiaomi_keyboard_probe(struct platform_device *pdev)
{
	struct xiaomi_keyboard_platdata *pdata;
	int ret = 0;
	mdata = kzalloc(sizeof(struct xiaomi_keyboard_data), GFP_KERNEL);
	if (!mdata) {
		MI_KB_ERR("Alloc Memory for xiaomi_keyboard_data failed\n");
		return -ENOMEM;
	}

	pdata = devm_kzalloc(&pdev->dev,
			     sizeof(struct xiaomi_keyboard_platdata),
			     GFP_KERNEL);
	if (!pdata) {
		MI_KB_ERR("Alloc Memory for xiaomi_keyboard_platdata failed\n");
		ret = -ENOMEM;
		goto err_free_mdata;
	}

	mdata->pdev = pdev;
	mdata->pdata = pdata;
	mutex_init(&mdata->lock);
	INIT_WORK(&mdata->state_work, keyboard_state_work);

	ret = xiaomi_keyboard_parse_dt(&pdev->dev);
	if (ret) {
		MI_KB_ERR("parse device tree failed\n");
		goto err_free_mdata;
	}

	ret = xiaomi_keyboard_pinctrl_init(&pdev->dev);
	if (ret) {
		MI_KB_ERR("Pinctrl init failed\n");
		goto err_free_mdata;
	}

	pdata = mdata->pdata;
	ret = xiaomi_keyboard_gpio_config(pdata, &pdev->dev);
	if (ret) {
		MI_KB_ERR("set gpio config failed\n");
		goto err_pinctrl_put;
	}

	mdata->powered_on = false;
	mdata->is_in_suspend = false;
	mdata->lid_is_closed = false;
	mdata->screen_is_on = true;
	mdata->user_enabled = true;
	mdata->irq_wake_enabled = false;

	mdata->drm_notif.notifier_call = keyboard_drm_notifier_callback;
	ret = mi_drm_register_client(&mdata->drm_notif);
	if (ret) {
		MI_KB_ERR("register drm_notifier failed. ret=%d\n", ret);
		goto err_pinctrl_put;
	}

	mdata->lid_notif.notifier_call = xiaomi_keyboard_lid_notifier_callback;
	ret = gpio_keys_lid_notifier_register(&mdata->lid_notif);
	if (ret) {
		MI_KB_ERR("register lid_notifier failed. ret=%d\n", ret);
		goto err_drm_notif_unreg;
	}

	ret = sysfs_create_group(&mdata->pdev->dev.kobj,
				 &xiaomi_attribute_group);
	if (ret < 0) {
		MI_KB_ERR("Create sysfs group Failed\n");
		goto err_lid_notif_unreg;
	}

	return ret;

err_lid_notif_unreg:
	gpio_keys_lid_notifier_unregister(&mdata->lid_notif);
err_drm_notif_unreg:
	if (mi_drm_unregister_client(&mdata->drm_notif))
		MI_KB_ERR("Error occurred while unregistering drm_notifier\n");
err_pinctrl_put:
	cancel_work_sync(&mdata->state_work);
	devm_pinctrl_put(mdata->pinctrl);
err_free_mdata:
	kfree(mdata);
	mdata = NULL;
	MI_KB_ERR("Failed\n");
	return ret;
}

static void xiaomi_keyboard_remove(struct platform_device *pdev)
{
	gpio_keys_lid_notifier_unregister(&mdata->lid_notif);
	mi_drm_unregister_client(&mdata->drm_notif);
	sysfs_remove_group(&mdata->pdev->dev.kobj, &xiaomi_attribute_group);
	cancel_work_sync(&mdata->state_work);

	mutex_lock(&mdata->lock);
	if (mdata->powered_on) {
		xiaomi_keyboard_connection_change(false);
		if (xiaomi_keyboard_resetup_gpio(mdata->pdata) < 0)
			MI_KB_ERR("resetup gpio failed\n");
		mdata->powered_on = false;
	}
	xiaomi_keyboard_power_off();
	mutex_unlock(&mdata->lock);

	devm_pinctrl_put(mdata->pinctrl);
	kfree(mdata);
	mdata = NULL;
}

#ifdef CONFIG_OF
static const struct of_device_id xiaomi_keyboard_dt_match[] = {
	{ .compatible = "xiaomi,keyboard" },
	{},
};
MODULE_DEVICE_TABLE(of, xiaomi_keyboard_dt_match);
#endif

static const struct platform_device_id xiaomi_keyboard_driver_ids[] = {
	{
		.name = "xiaomi-keyboard",
		.driver_data = 0,
	},
	{}
};
MODULE_DEVICE_TABLE(platform, xiaomi_keyboard_driver_ids);

static struct platform_driver xiaomi_keyboard_driver = {
	.probe        = xiaomi_keyboard_probe,
	.remove       = xiaomi_keyboard_remove,
	.driver       = {
		.name = "xiaomi-keyboard",
		.of_match_table = of_match_ptr(xiaomi_keyboard_dt_match),
		.pm = &xiaomi_keyboard_pm_ops,
	},
	.id_table     = xiaomi_keyboard_driver_ids,
};

module_platform_driver(xiaomi_keyboard_driver);

MODULE_DESCRIPTION("Xiaomi Keyboard Control-driver");
MODULE_AUTHOR("Tonghui Wang<wangtonghui@xiaomi.com>");
MODULE_LICENSE("GPL");
