// SPDX-License-Identifier: GPL-2.0
/*
 * Generic IIO access driver via I2C
 *
 * Copyright 2019 Analog Devices Inc.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "iio-regmap.h"

static const struct regmap_config iio_regmap_config = {
};

static int iio_regmap_i2c_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct regmap *regmap;

	regmap = devm_regmap_init_i2c(client, &iio_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "devm_regmap_init_i2c failed!\n");
		return PTR_ERR(regmap);
	}

	return iio_regmap_probe(&client->dev, regmap, client->name);
}

static const struct i2c_device_id iio_regmap_i2c_id[] = {
	{
		.name = "iio-regmap-i2c",
	},
	{}
};

static struct i2c_driver iio_regmap_i2c_driver = {
	.driver = {
		.name	= "iio-regmap-i2c",
	},
	.probe	       = iio_regmap_i2c_probe,
	.id_table      = iio_regmap_i2c_id
};

module_i2c_driver(iio_regmap_i2c_driver);

MODULE_AUTHOR("Alexandru Tachici <alexandru.tachici@analog.com>");
MODULE_DESCRIPTION("IIO Regmap I2C");
MODULE_LICENSE("GPL v2");
