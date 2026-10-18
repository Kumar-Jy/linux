// SPDX-License-Identifier: GPL-2.0
// Simplified IDTP9418 driver for reverse charging and I2C communication
// Supports touch pen charging

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/power_supply.h>
#include <linux/pinctrl/consumer.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/kfifo.h>
#include "idtp9418.h"

struct idtp9418_event {
	__u8 soc;
	__u8 is_charging;
	__u8 is_attached;
	__u8 charge_limit;
	__u8 pen_mac[MAC_LEN];
	__u8 state;
};

struct idtp9418_device_info {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;

	struct alarm reverse_dping_alarm;
	struct alarm reverse_chg_alarm;

	struct kfifo kfifo;
	wait_queue_head_t wait_queue;

	struct delayed_work irq_work;
	struct delayed_work hall_irq_work;
	struct delayed_work charge_monitor_work;
	struct delayed_work reverse_ept_type_work;

	struct gpio_desc *irq_gpiod;
	struct gpio_desc *hall3_gpiod;
	struct gpio_desc *hall4_gpiod;
	struct gpio_desc *reverse_gpiod;
	struct gpio_desc *reverse_boost_en_gpiod;

	bool is_attached;
	bool is_charging;
	bool is_reverse_mode;
	bool charge_monitor_first;
	u8 pen_mac[MAC_LEN];
	int reverse_pen_soc;
	int charge_limit;
};

static enum power_supply_property idtp9418_props[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_ONLINE,
};

static struct regmap_config i2c_idtp9418_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xFFFF,
};

static struct idtp9418_device_info *idtp9418_global_di;

static void idtp9418_push_event(struct idtp9418_device_info *di, u8 state)
{
	struct idtp9418_event evt;

	evt.soc = (u8)di->reverse_pen_soc;
	evt.is_charging = (u8)di->is_charging;
	evt.is_attached = (u8)di->is_attached;
	evt.charge_limit = (u8)di->charge_limit;
	memcpy(evt.pen_mac, di->pen_mac, MAC_LEN);
	evt.state = state;

	if (kfifo_avail(&di->kfifo) < sizeof(evt))
		kfifo_skip(&di->kfifo);
	kfifo_in(&di->kfifo, &evt, sizeof(evt));

	wake_up(&di->wait_queue);
}

static int idtp9418_kfifo_open(struct inode *inode, struct file *filp)
{
	filp->private_data = idtp9418_global_di;
	return 0;
}

static ssize_t idtp9418_kfifo_read(struct file *filp, char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct idtp9418_device_info *di = filp->private_data;
	struct idtp9418_event evt;

	if (count < sizeof(evt))
		return -EINVAL;

	if (kfifo_is_empty(&di->kfifo)) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(di->wait_queue,
					     !kfifo_is_empty(&di->kfifo)))
			return -ERESTARTSYS;
	}

	if (kfifo_out(&di->kfifo, &evt, sizeof(evt)) != sizeof(evt))
		return -EIO;

	if (copy_to_user(buf, &evt, sizeof(evt)))
		return -EFAULT;

	return sizeof(evt);
}

static const struct file_operations idtp9418_fops = {
	.owner = THIS_MODULE,
	.open = idtp9418_kfifo_open,
	.read = idtp9418_kfifo_read,
};

static struct miscdevice idtp9418_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "idtp9418",
	.fops = &idtp9418_fops,
	.mode = 0660,
};

static int idtp9418_read(struct idtp9418_device_info *di, u16 reg, u8 *val)
{
	unsigned int temp;
	int rc;

	rc = regmap_read(di->regmap, reg, &temp);
	if (rc >= 0)
		*val = (u8)temp;
	return rc;
}

static int idtp9418_write(struct idtp9418_device_info *di, u16 reg, u8 val)
{
	int rc;

	rc = regmap_write(di->regmap, reg, val);
	if (rc < 0)
		dev_err(di->dev, "idtp9418 write error: %d\n", rc);
	return rc;
}

static int idtp9418_read_buffer(struct idtp9418_device_info *di, u16 reg,
				u8 *buf, u32 size)
{
	int rc;

	rc = regmap_bulk_read(di->regmap, reg, buf, size);
	if (rc < 0)
		dev_err(di->dev, "read buf error at 0x%04x: %d\n", reg, rc);
	return rc;
}

static int idtp9418_write_buffer(struct idtp9418_device_info *di, u16 reg,
				 u8 *buf, u32 size)
{
	int rc;

	rc = regmap_bulk_write(di->regmap, reg, buf, size);
	if (rc < 0)
		dev_err(di->dev, "write buf error at 0x%04x: %d\n", reg, rc);
	return rc;
}

static void reverse_clrInt(struct idtp9418_device_info *di, u8 *buf, u32 size)
{
	idtp9418_write_buffer(di, REG_SYS_INT_CLR, buf, size);
	idtp9418_write(di, REG_TX_CMD, TX_FOD_EN | TX_CLRINT);
}

static void idtp9418_set_reverse_gpio(struct idtp9418_device_info *di,
				      int enable)
{
	if (enable) {
		gpiod_set_value(di->reverse_gpiod, true);
		gpiod_set_value(di->reverse_boost_en_gpiod, true);
		di->is_reverse_mode = true;
	} else {
		di->is_reverse_mode = false;
		gpiod_set_value(di->reverse_boost_en_gpiod, false);
		gpiod_set_value(di->reverse_gpiod, false);
	}
}

static void idtp9418_reverse_stop(struct idtp9418_device_info *di)
{
	idtp9418_set_reverse_gpio(di, false);
	di->is_charging = false;
	power_supply_changed(di->psy);
}

static void idt_set_reverse_fod(struct idtp9418_device_info *di, int mw)
{
	u8 mw_l = mw & 0xff;
	u8 mw_h = mw >> 8;

	idtp9418_write(di, REG_FOD_LOW, mw_l);
	idtp9418_write(di, REG_FOD_HIGH, mw_h);
}

static void idt_get_reverse_soc(struct idtp9418_device_info *di)
{
	u8 soc;

	if (idtp9418_read(di, REG_CHG_STATUS, &soc) < 0)
		return;
	if (soc > 0x64 && soc != 0xFF) {
		dev_err(di->dev, "soc illegal: %d\n", soc);
		return;
	}
	di->reverse_pen_soc = soc;
}

static void idtp9418_reverse_charge_enable(struct idtp9418_device_info *di)
{
	u8 mode;

	usleep_range(1000, 2000);
	idt_set_reverse_fod(di, REVERSE_FOD);
	for (int i = 0; i < 3; i++) {
		idtp9418_write(di, REG_TX_CMD, TX_EN | TX_FOD_EN);
		idtp9418_read(di, REG_TX_DATA, &mode);
		if (mode & BIT(0)) {
			di->is_charging = true;
			power_supply_changed(di->psy);
			return;
		}
	}
	dev_err(di->dev, "reverse charging failed start\n");
	idtp9418_reverse_stop(di);
}

static void idtp9418_get_pen_mac(struct idtp9418_device_info *di)
{
	int rc;

	rc = idtp9418_read_buffer(di, REG_MAC_ADDR, di->pen_mac, MAC_LEN);
	if (rc < 0)
		dev_err(di->dev, "read pen mac error: %d\n", rc);
}

static void idtp9418_charge_monitor_work(struct work_struct *work)
{
	struct idtp9418_device_info *di = container_of(
		work, struct idtp9418_device_info, charge_monitor_work.work);

	idt_get_reverse_soc(di);

	if (di->is_attached && di->reverse_pen_soc >= 0 &&
	    di->reverse_pen_soc < di->charge_limit)
		goto queue_work;

	idtp9418_reverse_stop(di);
	di->charge_monitor_first = false;
	goto push;

queue_work:
	schedule_delayed_work(&di->charge_monitor_work,
			      msecs_to_jiffies(CHARGE_MONITOR_INTERVAL));
	if (!di->charge_monitor_first)
		return;
push:
	idtp9418_push_event(di, IDTP9418_STATE_COMPLETE);
	di->charge_monitor_first = !di->charge_monitor_first;
}

static void reverse_chg_alarm_cb(struct alarm *alarm, ktime_t now)
{
	struct idtp9418_device_info *di = container_of(
		alarm, struct idtp9418_device_info, reverse_chg_alarm);

	dev_info(di->dev, "Reverse Chg Alarm Triggered %lld\n",
		 ktime_to_ms(now));
	idtp9418_reverse_stop(di);
}

static void reverse_dping_alarm_cb(struct alarm *alarm, ktime_t now)
{
	struct idtp9418_device_info *di = container_of(
		alarm, struct idtp9418_device_info, reverse_dping_alarm);

	dev_info(di->dev,
		 "Reverse Dping Alarm Triggered %lld, is_charging=%d\n",
		 ktime_to_ms(now), di->is_charging);
	idtp9418_reverse_stop(di);
}

#define EPT_FATAL_MASK \
	(EPT_FOD | EPT_CMD | EPT_OCP | EPT_OVP | EPT_LVP | EPT_OTP | EPT_POCP)

static void reverse_ept_type_get_work(struct work_struct *work)
{
	struct idtp9418_device_info *di = container_of(
		work, struct idtp9418_device_info, reverse_ept_type_work.work);
	int rc;
	u8 buf[2] = { 0 };
	u16 ept_val;

	rc = idtp9418_read_buffer(di, REG_EPT_TYPE, buf, 2);
	if (rc < 0) {
		dev_err(di->dev, "read tx ept type error: %d\n", rc);
		return;
	}

	ept_val = buf[0] | (buf[1] << 8);
	dev_info(di->dev, "tx ept type: 0x%04x\n", ept_val);

	if (!ept_val)
		return;

	if (ept_val & EPT_FATAL_MASK) {
		dev_info(di->dev, "TX mode in ept, disable reverse charging\n");
		idtp9418_reverse_stop(di);
	} else if (ept_val & EPT_CEP_TIMEOUT) {
		dev_info(di->dev, "recheck ping state\n");
	}
}

static void idtp9418_hall_irq_work(struct work_struct *work)
{
	struct idtp9418_device_info *di = container_of(
		work, struct idtp9418_device_info, hall_irq_work.work);
	bool attached;

	attached = !gpiod_get_value(di->hall3_gpiod) ||
		   !gpiod_get_value(di->hall4_gpiod);
	di->is_attached = attached;

	if (!attached) {
		dev_info(di->dev, "Pen detached! Turning off reverse charging...\n");
		idtp9418_reverse_stop(di);
		idtp9418_push_event(di, IDTP9418_STATE_COMPLETE);
		goto out;
	}

	dev_info(di->dev, "Pen attached! Starting reverse wireless charging...\n");
	idtp9418_set_reverse_gpio(di, true);
	idtp9418_reverse_charge_enable(di);
	idtp9418_push_event(di, IDTP9418_STATE_ATTACHING);
	alarm_start_timer(&di->reverse_dping_alarm,
			     ms_to_ktime(REVERSE_DPING_CHECK_DELAY_MS), true);
out:
	pm_relax(di->dev);
}

static irqreturn_t idtp9418_irq_handler(int irq, void *dev_id)
{
	struct idtp9418_device_info *di = dev_id;

	pm_stay_awake(di->dev);
	schedule_delayed_work(&di->irq_work, msecs_to_jiffies(10));
	return IRQ_HANDLED;
}

static irqreturn_t idtp9418_hall_irq_handler(int irq, void *dev_id)
{
	struct idtp9418_device_info *di = dev_id;

	pm_stay_awake(di->dev);
	schedule_delayed_work(&di->hall_irq_work, msecs_to_jiffies(10));
	return IRQ_HANDLED;
}

static bool reverse_need_irq_cleared(struct idtp9418_device_info *di, u32 val)
{
	u8 int_buf[4];
	u32 int_val;

	if (idtp9418_read_buffer(di, REG_SYS_INT, int_buf, 4) < 0)
		return false;
	int_val = int_buf[0] | (int_buf[1] << 8) | (int_buf[2] << 16) |
		  (int_buf[3] << 24);
	if (int_val && int_val == val)
		return true;
	return false;
}

static void idtp9418_irq_work(struct work_struct *work)
{
	struct idtp9418_device_info *di =
		container_of(work, struct idtp9418_device_info, irq_work.work);
	u8 int_buf[4] = { 0 };
	u8 clr_buf[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	u32 int_val;

	if (gpiod_get_value(di->irq_gpiod))
		goto out;

	if (idtp9418_read_buffer(di, REG_SYS_INT, int_buf, 4) < 0)
		goto out;

	if (!di->is_reverse_mode)
		goto out;

	reverse_clrInt(di, int_buf, 4);
	int_val = int_buf[0] | (int_buf[1] << 8) | (int_buf[2] << 16) |
		  (int_buf[3] << 24);

	msleep(5);
	if (reverse_need_irq_cleared(di, int_val)) {
		reverse_clrInt(di, clr_buf, 4);
		msleep(5);
	}

	if (int_val & INT_EPT_TYPE)
		schedule_delayed_work(&di->reverse_ept_type_work, 0);

	if (int_val & INT_GET_DPING) {
		dev_info(di->dev, "TRX get dping, disable reverse charging\n");
		idtp9418_reverse_stop(di);
	}

	if (int_val & INT_START_DPING) {
		alarm_start_timer(&di->reverse_chg_alarm,
				     ms_to_ktime(REVERSE_CHG_CHECK_DELAY_MS), true);
		alarm_cancel(&di->reverse_dping_alarm);
	}

	if (int_val & INT_GET_CFG)
		alarm_cancel(&di->reverse_chg_alarm);

	if (int_val & INT_GET_BLE_ADDR) {
		idtp9418_get_pen_mac(di);
		schedule_delayed_work(&di->charge_monitor_work,
				      msecs_to_jiffies(1500));
	}

out:
	pm_relax(di->dev);
}

static int idtp9418_pinctrl_init(struct idtp9418_device_info *di)
{
	struct pinctrl *pinctrl = devm_pinctrl_get(di->dev);
	struct pinctrl_state *pins_active;

	if (IS_ERR(pinctrl)) {
		dev_err(di->dev, "failed to get pinctrl\n");
		return PTR_ERR(pinctrl);
	}
	pins_active = pinctrl_lookup_state(pinctrl, "idt_active");
	if (IS_ERR(pins_active)) {
		dev_err(di->dev, "failed to get active pinctrl state\n");
		return PTR_ERR(pins_active);
	}
	return pinctrl_select_state(pinctrl, pins_active);
}

static int idtp9418_parse_dt(struct idtp9418_device_info *di)
{
	di->irq_gpiod = devm_gpiod_get(di->dev, "idt,irq", GPIOD_IN);
	if (IS_ERR(di->irq_gpiod)) {
		dev_err(di->dev, "get irq gpio failed: %ld\n",
			PTR_ERR(di->irq_gpiod));
		return PTR_ERR(di->irq_gpiod);
	}

	di->hall3_gpiod =
		devm_gpiod_get_optional(di->dev, "idt,hall3", GPIOD_IN);
	if (IS_ERR(di->hall3_gpiod)) {
		dev_err(di->dev, "get hall3 gpio failed: %ld\n",
			PTR_ERR(di->hall3_gpiod));
		di->hall3_gpiod = NULL;
	}

	di->hall4_gpiod =
		devm_gpiod_get_optional(di->dev, "idt,hall4", GPIOD_IN);
	if (IS_ERR(di->hall4_gpiod)) {
		dev_err(di->dev, "get hall4 gpio failed: %ld\n",
			PTR_ERR(di->hall4_gpiod));
		di->hall4_gpiod = NULL;
	}

	di->reverse_gpiod =
		devm_gpiod_get(di->dev, "idt,reverse-enable", GPIOD_OUT_LOW);
	if (IS_ERR(di->reverse_gpiod)) {
		dev_err(di->dev, "get reverse gpio failed: %ld\n",
			PTR_ERR(di->reverse_gpiod));
		return PTR_ERR(di->reverse_gpiod);
	}

	di->reverse_boost_en_gpiod = devm_gpiod_get(
		di->dev, "idt,reverse-boost-enable", GPIOD_OUT_LOW);
	if (IS_ERR(di->reverse_boost_en_gpiod)) {
		dev_err(di->dev, "get reverse_boost_enable gpio failed: %ld\n",
			PTR_ERR(di->reverse_boost_en_gpiod));
		return PTR_ERR(di->reverse_boost_en_gpiod);
	}

	return 0;
}

static int idtp9418_irq_request(struct idtp9418_device_info *di)
{
	int irq = gpiod_to_irq(di->irq_gpiod);
	int ret;

	if (irq < 0) {
		dev_err(di->dev, "gpiod_to_irq failed: %d\n", irq);
		return irq;
	}
	ret = devm_request_irq(di->dev, irq, idtp9418_irq_handler,
			       IRQF_TRIGGER_FALLING, IDT_DRIVER_NAME, di);
	if (ret < 0) {
		dev_err(di->dev, "request irq failed: %d\n", ret);
		return ret;
	}
	return enable_irq_wake(irq);
}

static int idtp9418_hall_gpio_request(struct idtp9418_device_info *di,
				      struct gpio_desc *gpiod, const char *name)
{
	int irq = gpiod_to_irq(gpiod);
	int ret;

	if (irq < 0)
		return irq;
	ret = devm_request_irq(di->dev, irq, idtp9418_hall_irq_handler,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, name,
			       di);
	if (ret < 0)
		return ret;
	enable_irq_wake(irq);
	return 0;
}

static int idtp9418_hall_irq_request(struct idtp9418_device_info *di)
{
	int ret;

	ret = idtp9418_hall_gpio_request(di, di->hall3_gpiod, "idtp_hall3");
	if (ret < 0)
		return ret;
	return idtp9418_hall_gpio_request(di, di->hall4_gpiod, "idtp_hall4");
}

static int idtp9418_get_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
	struct idtp9418_device_info *di = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_CAPACITY:
		if (!di->is_attached)
			return 0;
		val->intval = di->reverse_pen_soc;
		if (val->intval < 0)
			val->intval = 0;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = di->charge_limit;
		return 0;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = di->is_charging;
		return 0;
	default:
		return -EINVAL;
	}
}

static int idtp9418_property_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
}

static int idtp9418_set_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 const union power_supply_propval *val)
{
	struct idtp9418_device_info *di = power_supply_get_drvdata(psy);

	if (psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD) {
		if (val->intval < 0 || val->intval > 95)
			return -EINVAL;
		di->charge_limit = val->intval;
		dev_info(di->dev, "Charge limit set to %d%%\n",
			 di->charge_limit);
		return 0;
	}
	return -EINVAL;
}

static void idtp9418_cancel_works(struct idtp9418_device_info *di)
{
	cancel_delayed_work_sync(&di->irq_work);
	cancel_delayed_work_sync(&di->hall_irq_work);
	cancel_delayed_work_sync(&di->reverse_ept_type_work);
	cancel_delayed_work_sync(&di->charge_monitor_work);
}

static int idtp9418_probe(struct i2c_client *client)
{
	struct idtp9418_device_info *di;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct power_supply_config idtp_cfg = { 0 };
	struct power_supply_desc *psy_desc;
	int ret = 0;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	di = devm_kzalloc(&client->dev, sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	di->dev = &client->dev;
	di->reverse_pen_soc = 255;
	di->charge_limit = LIMIT_SOC;
	di->is_charging = false;
	di->is_attached = false;
	di->is_reverse_mode = false;
	di->charge_monitor_first = true;

	di->regmap = devm_regmap_init_i2c(client, &i2c_idtp9418_regmap_config);
	if (IS_ERR(di->regmap)) {
		dev_err(&client->dev, "Failed to init regmap: %ld\n",
			PTR_ERR(di->regmap));
		return PTR_ERR(di->regmap);
	}

	INIT_DELAYED_WORK(&di->irq_work, idtp9418_irq_work);
	INIT_DELAYED_WORK(&di->hall_irq_work, idtp9418_hall_irq_work);
	INIT_DELAYED_WORK(&di->reverse_ept_type_work,
			  reverse_ept_type_get_work);
	INIT_DELAYED_WORK(&di->charge_monitor_work,
			  idtp9418_charge_monitor_work);
	alarm_init(&di->reverse_dping_alarm, ALARM_BOOTTIME,
		   reverse_dping_alarm_cb);
	alarm_init(&di->reverse_chg_alarm, ALARM_BOOTTIME,
		   reverse_chg_alarm_cb);
	init_waitqueue_head(&di->wait_queue);

	ret = kfifo_alloc(&di->kfifo, IDTP_KFIFO_SIZE, GFP_KERNEL);
	if (ret) {
		dev_err(di->dev, "Failed to allocate kfifo\n");
		return ret;
	}

	device_init_wakeup(&client->dev, true);
	i2c_set_clientdata(client, di);

	ret = idtp9418_parse_dt(di);
	if (ret < 0)
		goto cleanup;

	ret = idtp9418_pinctrl_init(di);
	if (ret < 0)
		goto cleanup;

	ret = idtp9418_irq_request(di);
	if (ret < 0)
		goto cleanup;

	ret = idtp9418_hall_irq_request(di);
	if (ret < 0)
		goto cleanup;

	psy_desc = devm_kzalloc(&client->dev, sizeof(*psy_desc), GFP_KERNEL);
	if (!psy_desc) {
		ret = -ENOMEM;
		goto cleanup;
	}
	psy_desc->name = IDT_DRIVER_NAME;
	psy_desc->type = POWER_SUPPLY_TYPE_WIRELESS;
	psy_desc->properties = idtp9418_props;
	psy_desc->num_properties = ARRAY_SIZE(idtp9418_props);
	psy_desc->get_property = idtp9418_get_property;
	psy_desc->set_property = idtp9418_set_property;
	psy_desc->property_is_writeable = idtp9418_property_is_writeable;

	idtp_cfg.drv_data = di;
	di->psy = devm_power_supply_register(&client->dev, psy_desc, &idtp_cfg);
	if (IS_ERR(di->psy)) {
		dev_err(&client->dev, "Failed to register power supply\n");
		ret = PTR_ERR(di->psy);
		goto cleanup;
	}

	idtp9418_global_di = di;

	ret = misc_register(&idtp9418_miscdev);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to register misc device\n");
		goto cleanup;
	}

	/* Boot dock auto-detection: start charging if pen is already attached at boot */
	schedule_delayed_work(&di->hall_irq_work, msecs_to_jiffies(500));

	dev_info(di->dev, "idtp9418 probe success (with /dev/idtp9418)\n");
	return 0;

cleanup:
	idtp9418_cancel_works(di);
	kfifo_free(&di->kfifo);
	return ret;
}

static void idtp9418_remove(struct i2c_client *client)
{
	struct idtp9418_device_info *di = i2c_get_clientdata(client);

	misc_deregister(&idtp9418_miscdev);
	idtp9418_global_di = NULL;
	idtp9418_cancel_works(di);
	alarm_cancel(&di->reverse_dping_alarm);
	alarm_cancel(&di->reverse_chg_alarm);
	kfifo_free(&di->kfifo);
}

static const struct i2c_device_id idtp9418_id[] = {
	{ IDT_DRIVER_NAME, 0 },
	{}
};

static const struct of_device_id idt_match_table[] = {
	{ .compatible = "idt,p9418" },
	{}
};
MODULE_DEVICE_TABLE(of, idt_match_table);
MODULE_DEVICE_TABLE(i2c, idtp9418_id);

static struct i2c_driver idtp9418_driver = {
	.driver = {
		.name = IDT_DRIVER_NAME,
		.of_match_table = idt_match_table,
	},
	.probe = idtp9418_probe,
	.remove = idtp9418_remove,
	.id_table = idtp9418_id,
};

module_i2c_driver(idtp9418_driver);
MODULE_DESCRIPTION("IDTP9418 Reverse Charging Driver");
MODULE_LICENSE("GPL");
