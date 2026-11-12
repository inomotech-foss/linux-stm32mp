// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Liebherr-Electronics and Drives GmbH
 */
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <linux/phy/phy.h>
#include <linux/spi/spi.h>

#define TJA1145_MODE_CTRL		0x01
#define TJA1145_MODE_CTRL_MC		GENMASK(2, 0)
#define TJA1145_MODE_CTRL_SLEEP		BIT(0)
#define TJA1145_MODE_CTRL_STBY		BIT(2)
#define TJA1145_MODE_CTRL_NORMAL	TJA1145_MODE_CTRL_MC

#define TJA1145_CAN_CTRL		0x20
#define TJA1145_CAN_CTRL_CMC		GENMASK(1, 0)
#define TJA1145_CAN_CTRL_ACTIVE		BIT(1)

#define TJA1145_TRX_EVENT_EN		0x23
#define TJA1145_TRX_EVENT_EN_CWE	BIT(0)

#define TJA1145_WAKE_EVENT_EN		0x4c

#define TJA1145_SYS_EVENT_STATUS	0x61
#define TJA1145_TRX_EVENT_STATUS	0x63
#define TJA1145_WAKE_EVENT_STATUS	0x64

#define TJA1145_IDENT			0x7e
#define TJA1145_IDENT_TJA1145T		0x70
#define TJA1145_IDENT_TJA1145T_FD	0x74

#define TJA1145_SPI_READ_BIT		BIT(0)
#define TJA1145T_MAX_BITRATE		1000000

struct tja1145 {
	struct regmap *map;
	unsigned int saved_mode;
};

static int tja1145_phy_power_on(struct phy *phy)
{
	struct regmap *map = phy_get_drvdata(phy);
	int ret;

	/*
	 * Switch operating mode to normal which is the active operating mode.
	 * In this mode, the device is fully operational.
	 */
	ret = regmap_update_bits(map, TJA1145_MODE_CTRL, TJA1145_MODE_CTRL_MC,
				 TJA1145_MODE_CTRL_NORMAL);
	if (ret)
		return ret;

	/*
	 * Switch to CAN operating mode active where the PHY can transmit and
	 * receive data.
	 */
	return regmap_update_bits(map, TJA1145_CAN_CTRL, TJA1145_CAN_CTRL_CMC,
				  TJA1145_CAN_CTRL_ACTIVE);
}

static int tja1145_phy_power_off(struct phy *phy)
{
	struct regmap *map = phy_get_drvdata(phy);

	/*
	 * Switch to operating mode standby, the PHY is unable to transmit or
	 * receive data in standby mode.
	 */
	return regmap_update_bits(map, TJA1145_MODE_CTRL, TJA1145_MODE_CTRL_MC,
				  TJA1145_MODE_CTRL_STBY);
}

static const struct phy_ops tja1145_phy_ops = {
	.power_on = tja1145_phy_power_on,
	.power_off = tja1145_phy_power_off,
	.owner = THIS_MODULE,
};

static const struct regmap_range tja1145_wr_holes_ranges[] = {
	regmap_reg_range(0x00, 0x00),
	regmap_reg_range(0x02, 0x03),
	regmap_reg_range(0x05, 0x05),
	regmap_reg_range(0x0b, 0x1f),
	regmap_reg_range(0x21, 0x22),
	regmap_reg_range(0x24, 0x25),
	regmap_reg_range(0x30, 0x4b),
	regmap_reg_range(0x4d, 0x60),
	regmap_reg_range(0x62, 0x62),
	regmap_reg_range(0x65, 0x67),
	regmap_reg_range(0x70, 0xff),
};

static const struct regmap_access_table tja1145_wr_table = {
	.no_ranges = tja1145_wr_holes_ranges,
	.n_no_ranges = ARRAY_SIZE(tja1145_wr_holes_ranges),
};

static const struct regmap_range tja1145_rd_holes_ranges[] = {
	regmap_reg_range(0x00, 0x00),
	regmap_reg_range(0x02, 0x02),
	regmap_reg_range(0x05, 0x05),
	regmap_reg_range(0x0b, 0x1f),
	regmap_reg_range(0x21, 0x21),
	regmap_reg_range(0x24, 0x25),
	regmap_reg_range(0x30, 0x4a),
	regmap_reg_range(0x4d, 0x5f),
	regmap_reg_range(0x62, 0x62),
	regmap_reg_range(0x65, 0x67),
	regmap_reg_range(0x70, 0x7d),
	regmap_reg_range(0x7f, 0xff),
};

static const struct regmap_access_table tja1145_rd_table = {
	.no_ranges = tja1145_rd_holes_ranges,
	.n_no_ranges = ARRAY_SIZE(tja1145_rd_holes_ranges),
};

static const struct regmap_config tja1145_regmap_config = {
	.reg_bits = 8,
	.reg_shift = -1,
	.val_bits = 8,
	.wr_table = &tja1145_wr_table,
	.rd_table = &tja1145_rd_table,
	.read_flag_mask = TJA1145_SPI_READ_BIT,
	.max_register = TJA1145_IDENT,
};

static int tja1145_check_ident(struct device *dev, struct regmap *map)
{
	unsigned int val;
	int ret;

	ret = regmap_read(map, TJA1145_IDENT, &val);
	if (ret)
		return ret;

	switch (val) {
	case TJA1145_IDENT_TJA1145T:
	case TJA1145_IDENT_TJA1145T_FD:
		return 0;
	default:
		dev_err(dev, "Unexpected device id: 0x%02x\n", val);
		return -ENODEV;
	}
}

static int tja1145_clear_events(struct regmap *map)
{
	static const unsigned int regs[] = {
		TJA1145_SYS_EVENT_STATUS,
		TJA1145_TRX_EVENT_STATUS,
		TJA1145_WAKE_EVENT_STATUS,
	};
	unsigned int val;
	int ret;
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_read(map, regs[i], &val);
		if (ret)
			return ret;

		/* Status bits are write-1-to-clear. */
		ret = regmap_write(map, regs[i], val);
		if (ret)
			return ret;
	}

	return 0;
}

static int tja1145_suspend(struct device *dev)
{
	struct tja1145 *priv = dev_get_drvdata(dev);
	struct regmap *map = priv->map;
	unsigned int val;
	int ret;

	if (!device_may_wakeup(dev))
		return 0;

	ret = regmap_read(map, TJA1145_MODE_CTRL, &priv->saved_mode);
	if (ret)
		return ret;

	ret = regmap_write(map, TJA1145_WAKE_EVENT_EN, 0);
	if (ret)
		return ret;

	ret = regmap_set_bits(map, TJA1145_TRX_EVENT_EN,
			      TJA1145_TRX_EVENT_EN_CWE);
	if (ret)
		return ret;

	/* The device refuses to enter sleep mode while an event is pending. */
	ret = tja1145_clear_events(map);
	if (ret)
		goto err_restore;

	ret = regmap_write(map, TJA1145_MODE_CTRL, TJA1145_MODE_CTRL_SLEEP);
	if (ret)
		goto err_restore;

	/* A wake-up that raced with the command leaves the device in standby. */
	ret = regmap_read(map, TJA1145_MODE_CTRL, &val);
	if (ret)
		goto err_restore;

	if (FIELD_GET(TJA1145_MODE_CTRL_MC, val) != TJA1145_MODE_CTRL_SLEEP) {
		ret = -EBUSY;
		goto err_restore;
	}

	return 0;

err_restore:
	regmap_write(map, TJA1145_MODE_CTRL, priv->saved_mode);
	regmap_clear_bits(map, TJA1145_TRX_EVENT_EN, TJA1145_TRX_EVENT_EN_CWE);

	return ret;
}

static int tja1145_resume(struct device *dev)
{
	struct tja1145 *priv = dev_get_drvdata(dev);
	struct regmap *map = priv->map;
	int ret;

	if (!device_may_wakeup(dev))
		return 0;

	ret = regmap_write(map, TJA1145_MODE_CTRL, priv->saved_mode);
	if (ret)
		return ret;

	ret = regmap_clear_bits(map, TJA1145_TRX_EVENT_EN,
				TJA1145_TRX_EVENT_EN_CWE);
	if (ret)
		return ret;

	return tja1145_clear_events(map);
}

static DEFINE_SIMPLE_DEV_PM_OPS(tja1145_pm_ops, tja1145_suspend,
				tja1145_resume);

static int tja1145_probe(struct spi_device *spi)
{
	struct phy_provider *phy_provider;
	struct device *dev = &spi->dev;
	struct tja1145 *priv;
	struct regmap *map;
	struct phy *phy;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	map = devm_regmap_init_spi(spi, &tja1145_regmap_config);
	if (IS_ERR(map))
		return dev_err_probe(dev, PTR_ERR(map), "failed to init regmap\n");

	ret = tja1145_check_ident(dev, map);
	if (ret)
		return dev_err_probe(dev, ret, "failed to identify device\n");

	priv->map = map;
	spi_set_drvdata(spi, priv);
	device_init_wakeup(dev, device_property_read_bool(dev, "wakeup-source"));

	phy = devm_phy_create(dev, dev->of_node, &tja1145_phy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy), "failed to create PHY\n");

	phy->attrs.max_link_rate = TJA1145T_MAX_BITRATE;
	phy_set_drvdata(phy, map);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct spi_device_id tja1145_spi_id[] = {
	{ "tja1145" },
	{ }
};
MODULE_DEVICE_TABLE(spi, tja1145_spi_id);

static const struct of_device_id tja1145_of_match[] = {
	{ .compatible = "nxp,tja1145" },
	{ }
};
MODULE_DEVICE_TABLE(of, tja1145_of_match);

static struct spi_driver tja1145_driver = {
	.driver = {
		.name = "tja1145",
		.of_match_table = tja1145_of_match,
		.pm = pm_sleep_ptr(&tja1145_pm_ops),
	},
	.probe = tja1145_probe,
	.id_table = tja1145_spi_id,
};
module_spi_driver(tja1145_driver);

MODULE_DESCRIPTION("NXP TJA1145 CAN transceiver PHY driver");
MODULE_AUTHOR("Dimitri Fedrau <dimitri.fedrau@liebherr.com>");
MODULE_LICENSE("GPL");
