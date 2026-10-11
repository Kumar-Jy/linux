// SPDX-License-Identifier: GPL-2.0-only
/*
 * Chipnext CN3927 camera voice-coil motor driver
 * Copyright (C) 2022 Rockchip Electronics Co., Ltd.
 *
 * The actuator uses an 8-bit register address.  The focus DAC is a 10-bit
 * value written as a big-endian 16-bit value starting at register 0x03.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>

#define CN3927_NAME			"cn3927"
#define CN3927_MAX_FOCUS_POS		1023
#define CN3927_PARK_STEP		32
#define CN3927_PARK_POSITION		32
#define CN3927_MOVE_DELAY_US		1000
#define CN3927_POWER_DELAY_US		10000
#define CN3927_INIT_RETRIES		3
#define CN3927_RETRY_DELAY_US		5000

struct cn3927_device {
	struct v4l2_ctrl_handler controls;
	struct v4l2_subdev subdev;
	struct regulator_bulk_data supplies[2];
	u16 focus;
};

static inline struct cn3927_device *to_cn3927(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct cn3927_device, subdev);
}

static int cn3927_write(struct cn3927_device *cn3927, u8 reg, u16 value,
			unsigned int value_size)
{
	struct i2c_client *client = v4l2_get_subdevdata(&cn3927->subdev);
	u8 buf[3] = { reg, value >> 8, value };
	unsigned int offset = sizeof(buf) - value_size - 1;
	int ret;

	buf[offset] = reg;
	ret = i2c_master_send(client, &buf[offset], value_size + 1);
	if (ret == value_size + 1)
		return 0;

	if (ret >= 0)
		ret = -EIO;

	return dev_err_probe(&client->dev, ret,
			     "failed to write register 0x%02x value 0x%04x\n",
			     reg, value);
}

static int cn3927_set_focus(struct cn3927_device *cn3927, u16 focus)
{
	return cn3927_write(cn3927, 0x03, focus, 2);
}

static int cn3927_init(struct cn3927_device *cn3927)
{
	static const struct {
		u8 reg;
		u8 value;
		u16 delay_us;
	} init_sequence[] = {
		{ 0xed, 0xab, 0 },
		{ 0x02, 0x01, 0 },
		{ 0x02, 0x00, 100 },
		{ 0x06, 0x84, 0 },
		{ 0x07, 0x01, 0 },
		{ 0x08, 0x59, 0 },
	};
	unsigned int attempt;
	unsigned int i;
	int ret;

	for (attempt = 0; attempt < CN3927_INIT_RETRIES; attempt++) {
		for (i = 0; i < ARRAY_SIZE(init_sequence); i++) {
			ret = cn3927_write(cn3927, init_sequence[i].reg,
					    init_sequence[i].value, 1);
			if (ret)
				break;

			if (init_sequence[i].delay_us)
				usleep_range(init_sequence[i].delay_us,
					     init_sequence[i].delay_us + 100);
		}

		if (!ret)
			return 0;

		if (attempt + 1 < CN3927_INIT_RETRIES)
			usleep_range(CN3927_RETRY_DELAY_US,
				     CN3927_RETRY_DELAY_US + 1000);
	}

	return ret;
}

static int cn3927_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct cn3927_device *cn3927 =
		container_of(ctrl->handler, struct cn3927_device, controls);
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	ret = pm_runtime_get_if_in_use(cn3927->subdev.dev);
	if (ret < 0)
		return ret;
	if (!ret) {
		cn3927->focus = ctrl->val;
		return 0;
	}

	ret = cn3927_set_focus(cn3927, ctrl->val);
	pm_runtime_put(cn3927->subdev.dev);
	if (!ret)
		cn3927->focus = ctrl->val;

	return ret;
}

static const struct v4l2_ctrl_ops cn3927_ctrl_ops = {
	.s_ctrl = cn3927_set_ctrl,
};

static int cn3927_open(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(subdev->dev);
}

static int cn3927_close(struct v4l2_subdev *subdev, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(subdev->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops cn3927_internal_ops = {
	.open = cn3927_open,
	.close = cn3927_close,
};

static const struct v4l2_subdev_core_ops cn3927_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops cn3927_subdev_ops = {
	.core = &cn3927_core_ops,
};

static int cn3927_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *subdev = dev_get_drvdata(dev);
	struct cn3927_device *cn3927 = to_cn3927(subdev);
	int focus;
	int err;
	int ret = 0;

	focus = cn3927->focus & ~(CN3927_PARK_STEP - 1);
	if (focus < CN3927_PARK_POSITION)
		focus = CN3927_PARK_POSITION;

	for (; focus >= CN3927_PARK_POSITION; focus -= CN3927_PARK_STEP) {
		err = cn3927_set_focus(cn3927, focus);

		if (err && !ret)
			ret = err;
		usleep_range(CN3927_MOVE_DELAY_US,
			     CN3927_MOVE_DELAY_US + 100);
	}

	regulator_bulk_disable(ARRAY_SIZE(cn3927->supplies),
			       cn3927->supplies);

	return ret;
}

static int cn3927_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *subdev = dev_get_drvdata(dev);
	struct cn3927_device *cn3927 = to_cn3927(subdev);
	int focus;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(cn3927->supplies),
				    cn3927->supplies);
	if (ret)
		return ret;

	/* Allow the VCM supply and its UVLO reset circuit to settle. */
	usleep_range(CN3927_POWER_DELAY_US,
		     CN3927_POWER_DELAY_US + 1000);

	ret = cn3927_init(cn3927);
	if (ret)
		goto disable_supplies;

	for (focus = 0; focus < cn3927->focus; focus += CN3927_PARK_STEP) {
		ret = cn3927_set_focus(cn3927, focus);
		if (ret)
			goto disable_supplies;
		usleep_range(CN3927_MOVE_DELAY_US,
			     CN3927_MOVE_DELAY_US + 100);
	}

	ret = cn3927_set_focus(cn3927, cn3927->focus);
	if (!ret)
		return 0;

disable_supplies:
	regulator_bulk_disable(ARRAY_SIZE(cn3927->supplies),
			       cn3927->supplies);

	return ret;
}

static void cn3927_cleanup(struct cn3927_device *cn3927)
{
	v4l2_async_unregister_subdev(&cn3927->subdev);
	v4l2_ctrl_handler_free(&cn3927->controls);
	media_entity_cleanup(&cn3927->subdev.entity);
}

static int cn3927_probe(struct i2c_client *client)
{
	struct cn3927_device *cn3927;
	int ret;

	cn3927 = devm_kzalloc(&client->dev, sizeof(*cn3927), GFP_KERNEL);
	if (!cn3927)
		return -ENOMEM;

	cn3927->supplies[0].supply = "vio";
	cn3927->supplies[1].supply = "vcc";
	ret = devm_regulator_bulk_get(&client->dev,
				      ARRAY_SIZE(cn3927->supplies),
				      cn3927->supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to get VCM supplies\n");

	v4l2_i2c_subdev_init(&cn3927->subdev, client, &cn3927_subdev_ops);
	cn3927->subdev.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
				  V4L2_SUBDEV_FL_HAS_EVENTS;
	cn3927->subdev.internal_ops = &cn3927_internal_ops;
	cn3927->subdev.entity.function = MEDIA_ENT_F_LENS;
	strscpy(cn3927->subdev.name, "cn3927 focus",
		sizeof(cn3927->subdev.name));

	v4l2_ctrl_handler_init(&cn3927->controls, 1);
	v4l2_ctrl_new_std(&cn3927->controls, &cn3927_ctrl_ops,
			  V4L2_CID_FOCUS_ABSOLUTE, 0,
			  CN3927_MAX_FOCUS_POS, 1, 0);
	if (cn3927->controls.error) {
		ret = cn3927->controls.error;
		goto err_free_controls;
	}
	cn3927->subdev.ctrl_handler = &cn3927->controls;

	ret = media_entity_pads_init(&cn3927->subdev.entity, 0, NULL);
	if (ret)
		goto err_free_controls;

	/*
	 * The VCM is physically powered off at this point.  Initialise runtime
	 * PM before publishing the subdevice: publishing it first allows udev or
	 * libcamera to open the node while runtime PM still considers the device
	 * active and skips the resume callback entirely.
	 */
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_enable(&client->dev);

	ret = v4l2_async_register_subdev(&cn3927->subdev);
	if (ret)
		goto err_disable_pm;

	return 0;

err_disable_pm:
	pm_runtime_disable(&client->dev);
	media_entity_cleanup(&cn3927->subdev.entity);
err_free_controls:
	v4l2_ctrl_handler_free(&cn3927->controls);
	return ret;
}

static void cn3927_remove(struct i2c_client *client)
{
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct cn3927_device *cn3927 = to_cn3927(subdev);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		cn3927_runtime_suspend(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	cn3927_cleanup(cn3927);
}

static const struct of_device_id cn3927_of_match[] = {
	{ .compatible = "chipnext,cn3927" },
	{ }
};
MODULE_DEVICE_TABLE(of, cn3927_of_match);

static const struct i2c_device_id cn3927_id_table[] = {
	{ CN3927_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cn3927_id_table);

static const struct dev_pm_ops cn3927_pm_ops = {
	SET_RUNTIME_PM_OPS(cn3927_runtime_suspend, cn3927_runtime_resume, NULL)
};

static struct i2c_driver cn3927_i2c_driver = {
	.driver = {
		.name = CN3927_NAME,
		.of_match_table = cn3927_of_match,
		.pm = &cn3927_pm_ops,
	},
	.probe = cn3927_probe,
	.remove = cn3927_remove,
	.id_table = cn3927_id_table,
};
module_i2c_driver(cn3927_i2c_driver);

MODULE_AUTHOR("Nabu Linux contributors");
MODULE_DESCRIPTION("Chipnext CN3927 camera lens driver");
MODULE_LICENSE("GPL");
