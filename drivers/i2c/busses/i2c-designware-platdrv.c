/*
 * Synopsys DesignWare I2C adapter driver (master only).
 *
 * Based on the TI DAVINCI I2C adapter driver.
 *
 * Copyright (C) 2006 Texas Instruments.
 * Copyright (C) 2007 MontaVista Software Inc.
 * Copyright (C) 2009 Provigent Ltd.
 *
 * ----------------------------------------------------------------------------
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * ----------------------------------------------------------------------------
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/platform_data/i2c-designware.h>
#include "i2c-designware-core.h"

static u32 i2c_dw_get_clk_rate_khz(struct dw_i2c_dev *dev)
{
	return clk_get_rate(dev->clk)/1000;
}

#ifdef CONFIG_ACPI
/*
 * The HCNT/LCNT information coming from ACPI should be the most accurate
 * for given platform. However, some systems get it wrong. On such systems
 * we get better results by calculating those based on the input clock.
 */
static const struct dmi_system_id dw_i2c_no_acpi_params[] = {
	{
		.ident = "Dell Inspiron 7348",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Inspiron 7348"),
		},
	},
	{ }
};

static void dw_i2c_acpi_params(struct platform_device *pdev, char method[],
			       u16 *hcnt, u16 *lcnt, u32 *sda_hold)
{
	struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER };
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	union acpi_object *obj;

	if (dmi_check_system(dw_i2c_no_acpi_params))
		return;

	if (ACPI_FAILURE(acpi_evaluate_object(handle, method, NULL, &buf)))
		return;

	obj = (union acpi_object *)buf.pointer;
	if (obj->type == ACPI_TYPE_PACKAGE && obj->package.count == 3) {
		const union acpi_object *objs = obj->package.elements;

		*hcnt = (u16)objs[0].integer.value;
		*lcnt = (u16)objs[1].integer.value;
		if (sda_hold)
			*sda_hold = (u32)objs[2].integer.value;
	}

	kfree(buf.pointer);
}

static int dw_i2c_acpi_configure(struct platform_device *pdev)
{
	struct dw_i2c_dev *dev = platform_get_drvdata(pdev);
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	const struct acpi_device_id *id;
	struct acpi_device *adev;
	const char *uid;

	dev->adapter.nr = -1;
	dev->tx_fifo_depth = 32;
	dev->rx_fifo_depth = 32;

	/*
	 * Try to get SDA hold time and *CNT values from an ACPI method if
	 * it exists for both supported speed modes.
	 */
	dw_i2c_acpi_params(pdev, "SSCN", &dev->ss_hcnt, &dev->ss_lcnt, NULL);
	dw_i2c_acpi_params(pdev, "FMCN", &dev->fs_hcnt, &dev->fs_lcnt,
			   &dev->sda_hold_time);

	id = acpi_match_device(pdev->dev.driver->acpi_match_table, &pdev->dev);
	if (id && id->driver_data)
		dev->flags |= (u32)id->driver_data;

	if (acpi_bus_get_device(handle, &adev))
		return -ENODEV;

	/*
	 * Cherrytrail I2C7 gets used for the PMIC which gets accessed
	 * through ACPI opregions during late suspend / early resume
	 * disable pm for it.
	 */
	uid = adev->pnp.unique_id;
	if ((dev->flags & MODEL_CHERRYTRAIL) && !strcmp(uid, "7"))
		dev->pm_disabled = true;

	return 0;
}

static const struct acpi_device_id dw_i2c_acpi_match[] = {
	{ "INT33C2", 0 },
	{ "INT33C3", 0 },
	{ "INT3432", 0 },
	{ "INT3433", 0 },
	{ "80860F41", 0 },
	{ "808622C1", MODEL_CHERRYTRAIL },
	{ "AMD0010", ACCESS_INTR_MASK },
	{ }
};
MODULE_DEVICE_TABLE(acpi, dw_i2c_acpi_match);
#else
static inline int dw_i2c_acpi_configure(struct platform_device *pdev)
{
	return -ENODEV;
}
#endif

static void dw_i2c_set_fifo_size(struct dw_i2c_dev *dev, int id)
{
	u32 param, tx_fifo_depth, rx_fifo_depth;

	/*
	 * Try to detect the FIFO depth if not set by interface driver,
	 * the depth could be from 2 to 256 from HW spec.
	 */
	param = i2c_dw_read_comp_param(dev);
	tx_fifo_depth = ((param >> 16) & 0xff) + 1;
	rx_fifo_depth = ((param >> 8)  & 0xff) + 1;
	if (!dev->tx_fifo_depth) {
		dev->tx_fifo_depth = tx_fifo_depth;
		dev->rx_fifo_depth = rx_fifo_depth;
		dev->adapter.nr = id;
	} else if (tx_fifo_depth >= 2) {
		dev->tx_fifo_depth = min_t(u32, dev->tx_fifo_depth,
				tx_fifo_depth);
		dev->rx_fifo_depth = min_t(u32, dev->rx_fifo_depth,
				rx_fifo_depth);
	}
}

static int dw_i2c_plat_probe(struct platform_device *pdev)
{
	struct dw_i2c_dev *dev;
	struct i2c_adapter *adap;
	struct resource *mem;
	struct dw_i2c_platform_data *pdata;
	int irq, r;
	u32 clk_freq, ht = 0;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dev = devm_kzalloc(&pdev->dev, sizeof(struct dw_i2c_dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dev->base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);

	dev->dev = &pdev->dev;
	dev->irq = irq;
	platform_set_drvdata(pdev, dev);

	/* fast mode by default because of legacy reasons */
	clk_freq = 400000;

	if (has_acpi_companion(&pdev->dev)) {
		dw_i2c_acpi_configure(pdev);
	} else if (pdev->dev.of_node) {
		of_property_read_u32(pdev->dev.of_node,
					"i2c-sda-hold-time-ns", &ht);

		of_property_read_u32(pdev->dev.of_node,
				     "i2c-sda-falling-time-ns",
				     &dev->sda_falling_time);
		of_property_read_u32(pdev->dev.of_node,
				     "i2c-scl-falling-time-ns",
				     &dev->scl_falling_time);

		of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				     &clk_freq);

		/* Only standard mode at 100kHz and fast mode at 400kHz
		 * are supported.
		 */
		if (clk_freq != 100000 && clk_freq != 400000) {
			dev_err(&pdev->dev, "Only 100kHz and 400kHz supported");
			return -EINVAL;
		}
	} else {
		pdata = dev_get_platdata(&pdev->dev);
		if (pdata)
			clk_freq = pdata->i2c_scl_freq;
	}

	r = i2c_dw_probe_lock_support(dev);
	if (r)
		return r;

	dev->functionality =
		I2C_FUNC_I2C |
		I2C_FUNC_10BIT_ADDR |
		I2C_FUNC_SMBUS_BYTE |
		I2C_FUNC_SMBUS_BYTE_DATA |
		I2C_FUNC_SMBUS_WORD_DATA |
		I2C_FUNC_SMBUS_BLOCK_DATA |
		I2C_FUNC_SMBUS_I2C_BLOCK;
	if (clk_freq == 100000)
		dev->master_cfg =  DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE |
			DW_IC_CON_RESTART_EN | DW_IC_CON_SPEED_STD;
	else
		dev->master_cfg =  DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE |
			DW_IC_CON_RESTART_EN | DW_IC_CON_SPEED_FAST;

	dev->clk = devm_clk_get(&pdev->dev, NULL);
	dev->get_clk_rate_khz = i2c_dw_get_clk_rate_khz;
	if (IS_ERR(dev->clk))
		return PTR_ERR(dev->clk);
	clk_prepare_enable(dev->clk);

	if (!dev->sda_hold_time && ht) {
		u32 ic_clk = dev->get_clk_rate_khz(dev);

		dev->sda_hold_time = div_u64((u64)ic_clk * ht + 500000,
					     1000000);
	}

	dw_i2c_set_fifo_size(dev, pdev->id);

	adap = &dev->adapter;
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_DEPRECATED;
	ACPI_COMPANION_SET(&adap->dev, ACPI_COMPANION(&pdev->dev));
	adap->dev.of_node = pdev->dev.of_node;

	if (dev->pm_disabled) {
		pm_runtime_forbid(&pdev->dev);
	} else {
		pm_runtime_set_autosuspend_delay(&pdev->dev, 1000);
		pm_runtime_use_autosuspend(&pdev->dev);
		pm_runtime_set_active(&pdev->dev);
		pm_runtime_enable(&pdev->dev);
	}

	r = i2c_dw_probe(dev);
	if (r && !dev->pm_disabled)
		pm_runtime_disable(&pdev->dev);

	return r;
}

static int dw_i2c_plat_remove(struct platform_device *pdev)
{
	struct dw_i2c_dev *dev = platform_get_drvdata(pdev);

	pm_runtime_get_sync(&pdev->dev);

	i2c_del_adapter(&dev->adapter);

	i2c_dw_disable(dev);

	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_put_sync(&pdev->dev);
	if (!dev->pm_disabled)
		pm_runtime_disable(&pdev->dev);

	i2c_dw_remove_lock_support(dev);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id dw_i2c_of_match[] = {
	{ .compatible = "snps,designware-i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, dw_i2c_of_match);
#endif

#ifdef CONFIG_PM_SLEEP
static int dw_i2c_plat_prepare(struct device *dev)
{
	return pm_runtime_suspended(dev);
}

static void dw_i2c_plat_complete(struct device *dev)
{
	if (dev->power.direct_complete)
		pm_request_resume(dev);
}
#else
#define dw_i2c_plat_prepare	NULL
#define dw_i2c_plat_complete	NULL
#endif

#ifdef CONFIG_PM
static int dw_i2c_plat_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_i2c_dev *i_dev = platform_get_drvdata(pdev);

	i2c_dw_disable(i_dev);
	clk_disable_unprepare(i_dev->clk);

	return 0;
}

static int dw_i2c_plat_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dw_i2c_dev *i_dev = platform_get_drvdata(pdev);

	clk_prepare_enable(i_dev->clk);
	i2c_dw_init(i_dev);

	return 0;
}

static const struct dev_pm_ops dw_i2c_dev_pm_ops = {
	.prepare = dw_i2c_plat_prepare,
	.complete = dw_i2c_plat_complete,
	SET_SYSTEM_SLEEP_PM_OPS(dw_i2c_plat_suspend, dw_i2c_plat_resume)
	SET_RUNTIME_PM_OPS(dw_i2c_plat_suspend, dw_i2c_plat_resume, NULL)
};

#define DW_I2C_DEV_PMOPS (&dw_i2c_dev_pm_ops)
#else
#define DW_I2C_DEV_PMOPS NULL
#endif

/* work with hotplug and coldplug */
MODULE_ALIAS("platform:i2c_designware");

static struct platform_driver dw_i2c_driver = {
	.probe = dw_i2c_plat_probe,
	.remove = dw_i2c_plat_remove,
	.driver		= {
		.name	= "i2c_designware",
		.of_match_table = of_match_ptr(dw_i2c_of_match),
		.acpi_match_table = ACPI_PTR(dw_i2c_acpi_match),
		.pm	= DW_I2C_DEV_PMOPS,
	},
};

static int __init dw_i2c_init_driver(void)
{
	return platform_driver_register(&dw_i2c_driver);
}
subsys_initcall(dw_i2c_init_driver);

static void __exit dw_i2c_exit_driver(void)
{
	platform_driver_unregister(&dw_i2c_driver);
}
module_exit(dw_i2c_exit_driver);

MODULE_AUTHOR("Baruch Siach <baruch@tkos.co.il>");
MODULE_DESCRIPTION("Synopsys DesignWare I2C bus adapter");
MODULE_LICENSE("GPL");
