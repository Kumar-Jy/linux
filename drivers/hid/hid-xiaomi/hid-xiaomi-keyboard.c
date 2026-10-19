// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for the Xiaomi Mi Pad 5 keyboard.
 *
 *  Copyright (c) 2024 Sebastiano Barezzi <seba@sebaubuntu.dev>
 */

/**
 * This driver is needed to support the Xiaomi Mi Pad 5 keyboard due to their engineering mess.
 * The keyboard controller is in the tablet, and the keyboard connects through pogo pins.
 * The controllers appears as a USB HID device, but without being aware of its presence.
 * The driver, thanks to the original Xiaomi driver needed for power juice, will add and remove the
 * Linux input device when the keyboard is connected and disconnected.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/input/usbkbd-xiaomi.h>

#include "../hid-ids.h"

struct xiaomi_priv {
	struct list_head list;
	struct hid_device *hdev;

	bool enabled;
};

// List of all instances
static struct list_head instances = LIST_HEAD_INIT(instances);
static DEFINE_MUTEX(instances_lock);

// Keyboard connection status, false by default
static bool connected = false;

/**
 * Updates the HID device status in a safe way.
 * Needed to avoid a double-free in the HID core.
 */
static void xiaomi_safe_toggle(struct hid_device *hdev, bool enable)
{
	struct xiaomi_priv *priv = hid_get_drvdata(hdev);

	if (priv->enabled == enable)
		return;

	if (enable) {
		if (hid_hw_start(hdev, HID_CONNECT_DEFAULT)) {
			hid_err(hdev, "hid_hw_start failed\n");
			return;
		}
	} else {
		hid_hw_stop(hdev);
	}

	priv->enabled = enable;
}

// Called by vendor driver
void xiaomi_keyboard_connection_change(bool _connected)
{
	struct xiaomi_priv *instance;

	connected = _connected;

	mutex_lock(&instances_lock);
	list_for_each_entry(instance, &instances, list) {
		xiaomi_safe_toggle(instance->hdev, connected);
	}
	mutex_unlock(&instances_lock);
}
EXPORT_SYMBOL_GPL(xiaomi_keyboard_connection_change);

static int xiaomi_input_configured(struct hid_device *hdev,
				   struct hid_input *hi)
{
	struct input_dev *input = hi->input;

	// Drop EV_REL, the keyboard doesn't have a touchpad
	__clear_bit(EV_REL, input->evbit);

	return 0;
}

static int xiaomi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct xiaomi_priv *priv;
	int ret;

	if (!hid_is_usb(hdev)) {
		ret = -EINVAL;
		goto err_1;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_1;
	}

	priv->hdev = hdev;
	priv->enabled = false;

	hid_set_drvdata(hdev, priv);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid_parse failed\n");
		goto err_2;
	}

	// Add the instance to the list
	mutex_lock(&instances_lock);
	list_add(&priv->list, &instances);
	mutex_unlock(&instances_lock);

	xiaomi_safe_toggle(hdev, connected);

	return 0;

err_2:
	kfree(priv);
err_1:
	return ret;
}

static void xiaomi_remove(struct hid_device *hdev)
{
	struct xiaomi_priv *priv = hid_get_drvdata(hdev);

	// Remove the instance from the list
	mutex_lock(&instances_lock);
	list_del(&priv->list);
	mutex_unlock(&instances_lock);

	// Stop the HID device
	xiaomi_safe_toggle(hdev, false);

	kfree(priv);
}

static const struct hid_device_id xiaomi_keyboard[] = {
	{
		HID_USB_DEVICE(USB_VENDOR_ID_NANO_IC, 0x3FFC),
	},
	{}
};
MODULE_DEVICE_TABLE(hid, xiaomi_keyboard);

static struct hid_driver hid_xiaomi_keyboard = {
	.name = "hid-xiaomi-keyboard",
	.id_table = xiaomi_keyboard,
	.probe = xiaomi_probe,
	.remove = xiaomi_remove,
	.input_configured = xiaomi_input_configured,
};
module_hid_driver(hid_xiaomi_keyboard);

MODULE_AUTHOR("Sebastiano Barezzi <seba@sebaubuntu.dev>");
MODULE_DESCRIPTION("Xiaomi Mi Pad 5 keyboard HID driver");
MODULE_LICENSE("GPL");
