/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OF helpers for network devices.
 */

#ifndef __LINUX_OF_NET_H
#define __LINUX_OF_NET_H

#include <linux/phy.h>

#if defined(CONFIG_OF) && defined(CONFIG_NET)
#include <linux/of.h>

struct net_device;
extern int of_get_phy_mode(struct device_node *np, phy_interface_t *interface);
extern int of_get_mac_address(struct device_node *np, u8 *mac);
int of_get_ethdev_address(struct device_node *np, struct net_device *dev);
#else
static inline int of_get_phy_mode(struct device_node *np,
				  phy_interface_t *interface)
{
	return -ENODEV;
}

static inline int of_get_mac_address(struct device_node *np, u8 *mac)
{
	return -ENODEV;
}

static inline int of_get_ethdev_address(struct device_node *np, struct net_device *dev)
{
	return -ENODEV;
}
#endif

#endif /* __LINUX_OF_NET_H */
