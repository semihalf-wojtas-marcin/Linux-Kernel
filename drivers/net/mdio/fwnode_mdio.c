// SPDX-License-Identifier: GPL-2.0-only
/*
 * fwnode helpers for the MDIO (Ethernet PHY) API
 *
 * This file provides helper functions for extracting PHY device information
 * out of the fwnode and using it to populate an mii_bus.
 */

#include <linux/acpi.h>
#include <linux/fwnode_mdio.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/phy_fixed.h>
#include <linux/pse-pd/pse.h>

MODULE_AUTHOR("Calvin Johnson <calvin.johnson@oss.nxp.com>");
MODULE_LICENSE("GPL");

static struct pse_control *
fwnode_find_pse_control(struct fwnode_handle *fwnode)
{
	struct pse_control *psec;
	struct device_node *np;

	if (!IS_ENABLED(CONFIG_PSE_CONTROLLER))
		return NULL;

	np = to_of_node(fwnode);
	if (!np)
		return NULL;

	psec = of_pse_control_get(np);
	if (PTR_ERR(psec) == -ENOENT)
		return NULL;

	return psec;
}

static struct mii_timestamper *
fwnode_find_mii_timestamper(struct fwnode_handle *fwnode)
{
	struct of_phandle_args arg;
	int err;

	if (is_acpi_node(fwnode))
		return NULL;

	err = of_parse_phandle_with_fixed_args(to_of_node(fwnode),
					       "timestamper", 1, 0, &arg);
	if (err == -ENOENT)
		return NULL;
	else if (err)
		return ERR_PTR(err);

	if (arg.args_count != 1)
		return ERR_PTR(-EINVAL);

	return register_mii_timestamper(arg.np, arg.args[0]);
}

int fwnode_mdiobus_phy_device_register(struct mii_bus *mdio,
				       struct phy_device *phy,
				       struct fwnode_handle *child, u32 addr)
{
	int rc;

	rc = fwnode_irq_get(child, 0);
	/* Don't wait forever if the IRQ provider doesn't become available,
	 * just fall back to poll mode
	 */
	if (rc == -EPROBE_DEFER)
		rc = driver_deferred_probe_check_state(&phy->mdio.dev);
	if (rc == -EPROBE_DEFER)
		return rc;

	if (rc > 0) {
		phy->irq = rc;
		mdio->irq[addr] = rc;
	} else {
		phy->irq = mdio->irq[addr];
	}

	if (fwnode_property_read_bool(child, "broken-turn-around"))
		mdio->phy_ignore_ta_mask |= 1 << addr;

	fwnode_property_read_u32(child, "reset-assert-us",
				 &phy->mdio.reset_assert_delay);
	fwnode_property_read_u32(child, "reset-deassert-us",
				 &phy->mdio.reset_deassert_delay);

	/* Associate the fwnode with the device structure so it
	 * can be looked up later
	 */
	fwnode_handle_get(child);
	device_set_node(&phy->mdio.dev, child);

	/* All data is now stored in the phy struct;
	 * register it
	 */
	rc = phy_device_register(phy);
	if (rc) {
		device_set_node(&phy->mdio.dev, NULL);
		fwnode_handle_put(child);
		return rc;
	}

	dev_dbg(&mdio->dev, "registered phy %p fwnode at address %i\n",
		child, addr);
	return 0;
}
EXPORT_SYMBOL(fwnode_mdiobus_phy_device_register);

int fwnode_mdiobus_register_phy(struct mii_bus *bus,
				struct fwnode_handle *child, u32 addr)
{
	struct mii_timestamper *mii_ts = NULL;
	struct pse_control *psec = NULL;
	struct phy_device *phy;
	bool is_c45 = false;
	u32 phy_id;
	int rc;

	psec = fwnode_find_pse_control(child);
	if (IS_ERR(psec))
		return PTR_ERR(psec);

	mii_ts = fwnode_find_mii_timestamper(child);
	if (IS_ERR(mii_ts)) {
		rc = PTR_ERR(mii_ts);
		goto clean_pse;
	}

	rc = fwnode_property_match_string(child, "compatible",
					  "ethernet-phy-ieee802.3-c45");
	if (rc >= 0)
		is_c45 = true;

	if (is_c45 || fwnode_get_phy_id(child, &phy_id))
		phy = get_phy_device(bus, addr, is_c45);
	else
		phy = phy_device_create(bus, addr, phy_id, 0, NULL);
	if (IS_ERR(phy)) {
		rc = PTR_ERR(phy);
		goto clean_mii_ts;
	}

	if (is_acpi_node(child)) {
		phy->irq = bus->irq[addr];

		/* Associate the fwnode with the device structure so it
		 * can be looked up later.
		 */
		phy->mdio.dev.fwnode = fwnode_handle_get(child);

		/* All data is now stored in the phy struct, so register it */
		rc = phy_device_register(phy);
		if (rc) {
			phy->mdio.dev.fwnode = NULL;
			fwnode_handle_put(child);
			goto clean_phy;
		}
	} else if (is_of_node(child)) {
		rc = fwnode_mdiobus_phy_device_register(bus, phy, child, addr);
		if (rc)
			goto clean_phy;
	}

	phy->psec = psec;

	/* phy->mii_ts may already be defined by the PHY driver. A
	 * mii_timestamper probed via the device tree will still have
	 * precedence.
	 */
	if (mii_ts)
		phy->mii_ts = mii_ts;

	return 0;

clean_phy:
	phy_device_free(phy);
clean_mii_ts:
	unregister_mii_timestamper(mii_ts);
clean_pse:
	pse_control_put(psec);

	return rc;
}
EXPORT_SYMBOL(fwnode_mdiobus_register_phy);

/*
 * fwnode_phy_is_fixed_link() and fwnode_phy_register_fixed_link() must
 * support two bindings:
 * - the old binding, where 'fixed-link' was a property with 5
 *   cells encoding various information about the fixed PHY
 * - the new binding, where 'fixed-link' is a sub-node of the
 *   Ethernet device.
 */
bool fwnode_phy_is_fixed_link(struct fwnode_handle *fwnode)
{
	struct fwnode_handle *fixed_link_node;
	const char *managed;

	/* New binding */
	fixed_link_node = fwnode_get_named_child_node(fwnode, "fixed-link");
	if (fixed_link_node) {
		fwnode_handle_put(fixed_link_node);
		return true;
	}

	if (fwnode_property_read_string(fwnode, "managed", &managed) == 0 &&
	    strcmp(managed, "auto") != 0)
		return true;

	/* Old binding */
	return fwnode_property_count_u32(fwnode, "fixed-link") == 5;
}
EXPORT_SYMBOL(fwnode_phy_is_fixed_link);

int fwnode_phy_register_fixed_link(struct fwnode_handle *fwnode)
{
	struct fixed_phy_status status = {};
	struct fwnode_handle *fixed_link_node;
	u32 fixed_link_prop[5];
	const char *managed;
	int rc;

	if (fwnode_property_read_string(fwnode, "managed", &managed) == 0 &&
	    strcmp(managed, "in-band-status") == 0) {
		/* status is zeroed, namely its .link member */
		goto register_phy;
	}

	/* New binding */
	fixed_link_node = fwnode_get_named_child_node(fwnode, "fixed-link");
	if (fixed_link_node) {
		status.link = 1;
		status.duplex = fwnode_property_present(fixed_link_node,
							"full-duplex");
		rc = fwnode_property_read_u32(fixed_link_node, "speed",
					      &status.speed);
		if (rc) {
			fwnode_handle_put(fixed_link_node);
			return rc;
		}
		status.pause = fwnode_property_present(fixed_link_node, "pause");
		status.asym_pause = fwnode_property_present(fixed_link_node,
							    "asym-pause");
		fwnode_handle_put(fixed_link_node);

		goto register_phy;
	}

	/* Old binding */
	rc = fwnode_property_read_u32_array(fwnode, "fixed-link", fixed_link_prop,
					    ARRAY_SIZE(fixed_link_prop));
	if (rc)
		return rc;

	status.link = 1;
	status.duplex = fixed_link_prop[1];
	status.speed  = fixed_link_prop[2];
	status.pause  = fixed_link_prop[3];
	status.asym_pause = fixed_link_prop[4];

register_phy:
	return PTR_ERR_OR_ZERO(fixed_phy_register(PHY_POLL, &status, fwnode));
}
EXPORT_SYMBOL(fwnode_phy_register_fixed_link);

void fwnode_phy_deregister_fixed_link(struct fwnode_handle *fwnode)
{
	struct phy_device *phydev;

	phydev = fwnode_phy_find_device(fwnode);
	if (!phydev)
		return;

	fixed_phy_unregister(phydev);

	put_device(&phydev->mdio.dev);	/* fwnode_phy_find_device() */
	phy_device_free(phydev);	/* fixed_phy_register() */
}
EXPORT_SYMBOL(fwnode_phy_deregister_fixed_link);
