/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility shim for out-of-tree drivers that still use the legacy
 * of_get_named_gpio() API, which was removed in the 7.x series.
 *
 * Returns a legacy global GPIO number (not a descriptor), matching the old
 * semantics, so callers can keep using gpio_is_valid()/gpio_request_one()
 * etc.  Requires CONFIG_GPIOLIB_LEGACY.
 */
#ifndef __LINUX_OF_GPIO_H
#define __LINUX_OF_GPIO_H

#include <linux/err.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/gpio/consumer.h>

static inline int of_get_named_gpio(struct device_node *np,
				    const char *propname, int index)
{
	struct gpio_desc *desc;
	char con_id[64];
	size_t len;

	if (!np || !propname)
		return -EINVAL;

	len = strlen(propname);
	if (len >= 6 && !strcmp(propname + len - 6, "-gpios"))
		len -= 6;
	else if (len >= 5 && !strcmp(propname + len - 5, "-gpio"))
		len -= 5;

	if (len >= sizeof(con_id))
		return -EINVAL;

	memcpy(con_id, propname, len);
	con_id[len] = '\0';

	desc = fwnode_gpiod_get_index(&np->fwnode, con_id, index,
				      GPIOD_ASIS, NULL);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	index = desc_to_gpio(desc);
	gpiod_put(desc);

	return index;
}

#endif /* __LINUX_OF_GPIO_H */
