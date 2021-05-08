// SPDX-License-Identifier: GPL-2.0-only
/*
 * Header file for driver for
 * MStar Semiconductor touchscreens
 * Copyright (c) 2021 Er2 <er2@dismail.de>
 */

#pragma once

/* Structures */

struct tpd_compat {
	u8  max_fingers;
	u8  data_raw;
	u8  chip_on; /* ms */
	u8  firmware_on; /* ms */
	u16  reset_min; /* us */
	u16  reset_max; /* us */
};

struct point {
	u32	x;
	u32 y;
};

struct packet {
	u8	xy_hi; /* higher bits of x and y coordinates */
	u8	x_low;
	u8	y_low;
	u8	pressure;
};

struct touch_event {
	u8 mode;
	struct packet *pkt; /* "*" instaed of [MAX_FINGERS] */
	u8  proximity;
	u8  checksum;
};

struct tpd_data {
	struct i2c_client *client;
	struct input_dev *idev;
	struct regulator_bulk_data supplies[2];
	struct input_absinfo *prop;
	struct gpio_desc *reset_gpiod;
	const struct tpd_compat *data;
};

/* Function and variable prototypes */

static irqreturn_t tpd_irq_handler(int irq, void *d);
static int tpd_probe(struct i2c_client *client, const struct i2c_device_id *);
static int tpd_init_input_dev(struct tpd_data *data);

static int tpd_init_regulators(struct tpd_data *data);

static int tpd_start(struct tpd_data *data);
static int tpd_stop(struct tpd_data *data);

static u8 tpd_checksum(u8 *data, u32 length);
static void tpd_finger(struct tpd_data *data, int slot, const struct point *p);
static int tpd_input_open(struct input_dev *dev);
static void tpd_input_close(struct input_dev *dev);
static int __maybe_unused tpd_suspend(struct device *dev);
static int __maybe_unused tpd_resume(struct device *dev);

/****/

static const struct tpd_compat msg2238_compat = {
	/* TODO: Check this values */
	.max_fingers = 2,
	.data_raw = 0x62,
	.chip_on = 10,
	.firmware_on = 20,
	.reset_min = 10000,
	.reset_max = 11000,
};

static const struct tpd_compat msg2638_compat = {
	.max_fingers = 5,
	.data_raw = 0x5A,
	.chip_on = 15,
	.firmware_on = 50,
	.reset_min = 10000,
	.reset_max = 11000,
};

static const struct of_device_id tpd_of_match[] = {
	{ .compatible = "mstar,msg2238", .data = &msg2238_compat },
	{ .compatible = "mstar,msg2638", .data = &msg2638_compat },
	{ /* Mustn't be filled */ }
};

MODULE_DEVICE_TABLE(of, tpd_of_match);
static SIMPLE_DEV_PM_OPS(tpd_pm_ops, tpd_suspend, tpd_resume);

static struct i2c_driver tpd_driver = {
	.probe = tpd_probe,
	.driver = {
		.name = "MStar-TS",
		.pm = &tpd_pm_ops,
		.of_match_table = of_match_ptr(tpd_of_match),
	},
};
module_i2c_driver(tpd_driver);
