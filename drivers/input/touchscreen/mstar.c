// SPDX-License-Identifier: GPL-2.0-only
/*
 * Touchscreen driver for
 * MStar Semiconductor touchscreens
 * Copyright (c) 2021 Er2 <er2@dismail.de>
 */

#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include "mstar.h"

MODULE_AUTHOR("Er2 <er2@dismail.de>");
MODULE_DESCRIPTION("MStar touchscreens driver");
MODULE_LICENSE("GPL v2");

/* Functions declarations */

static irqreturn_t tpd_irq_handler(int irq, void *d) {
	struct tpd_data *data = d;

	struct i2c_client *client = data->client;
	struct touch_event tev;
	struct point coord;
	struct i2c_msg msg[1];
	struct packet *p;

	u32 len = sizeof(struct touch_event);
	int ret;
	int i;

	memset(&tev, 0, len);
	tev.pkt = kmalloc(sizeof(struct packet) * data->data->max_fingers, GFP_ATOMIC);

	/* Write message to touch */
	msg[0].addr = client->addr;
	msg[0].flags = I2C_M_RD;
	msg[0].len = len;
	msg[0].buf = (u8*)&tev;

	ret = i2c_transfer(client->adapter, msg, 1);
	if(ret != 1) {
		dev_err(&client->dev, "Failed I2C transfer in irq handler!\n");
		goto out;
	}

	if(tev.mode != data->data->data_raw)
		goto out;

	if(tpd_checksum((u8*)&tev, len - 1) != tev.checksum) {
		dev_err(&client->dev, "Failed checksum!\n");
		goto out;
	}

	for(i = 0; i < data->data->max_fingers; i++) {
		p = &tev.pkt[i];
		/* Ignore non-pressed finger data */
		if (p->xy_hi == 0xFF && p->x_low == 0xFF && p->y_low == 0xFF)
			continue;

		/* Write coords and add finger */
		coord.x = ((p->xy_hi & 0xF0) << 4) | p->x_low;
		coord.y = ((p->xy_hi & 0x0F) << 8) | p->y_low;
		tpd_finger(data, i, &coord);
	}

	/* End */
	input_mt_sync_frame(data->idev);
	input_sync(data->idev);

	out:
		kfree(&tev.pkt);
		return IRQ_HANDLED;
}

static int tpd_probe(struct i2c_client *client, const struct i2c_device_id *did) {
	/* did was created bcz its old linux */
	struct tpd_data *data;
	int ret;

	if(!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "Failed to assert adapter's support for plain I2C.\n");
		return -ENXIO;
	}

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if(!data)
		return -ENOMEM;

	data->data = of_match_device(tpd_of_match, &client->dev)[0].data;
	data->client = client;
	i2c_set_clientdata(client, data);

	ret = tpd_init_regulators(data);
	if(ret) {
		dev_err(&client->dev, "Failed to initialize regulators: %d\n", ret);
		return ret;
	}

	data->reset_gpiod = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_LOW);
	if(IS_ERR(data->reset_gpiod)) {
		ret = PTR_ERR(data->reset_gpiod);
		dev_err(&client->dev, "Failed to request reset GPIO: %d\n", ret);
		return ret;
	}

	ret = tpd_init_input_dev(data);
	if(ret) {
		dev_err(&client->dev, "Failed to initialize input device: %d\n", ret);
		return ret;
	}

	irq_set_status_flags(client->irq, IRQ_NOAUTOEN);
	ret = devm_request_threaded_irq(&client->dev, client->irq,
					NULL, tpd_irq_handler,
					IRQF_ONESHOT, client->name, data);
	if(ret) {
		dev_err(&client->dev, "Failed to request IRQ: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tpd_init_input_dev(struct tpd_data *data) {
	struct input_dev *idev;
	int ret;

	idev = devm_input_allocate_device(&data->client->dev);
	if(!idev) {
		dev_err(&data->client->dev, "Failed to allocate input device.\n");
		return -ENOMEM;
	}

	input_set_drvdata(idev, data);
	data->idev = idev;

	idev->name = "MStar TouchScreen";
	idev->phys = "input/ts";
	idev->id.bustype = BUS_I2C;
	idev->open = tpd_input_open;
	idev->close = tpd_input_close;

	input_set_capability(idev, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(idev, EV_ABS, ABS_MT_POSITION_Y);
	/* dev, axis, min, max, fuzz, flat */
	input_set_abs_params(idev, ABS_MT_WIDTH_MAJOR, 0, 15, 0, 0);
	input_set_abs_params(idev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	
	struct device_node *np = idev->dev.parent->of_node;
	struct point *p = kmalloc(sizeof(struct point), GFP_KERNEL);

	of_property_read_u32(np, "touchscreen-size-x", &p->x);
	of_property_read_u32(np, "touchscreen-size-y", &p->y);

	if(!p->x || !p->y) {
		dev_err(&data->client->dev,
			"touchscreen-size-x and/or touchscreen-size-y not set in dts\n");
		return -EINVAL;
	}

	ret = input_mt_init_slots(idev, data->data->max_fingers, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if(ret) {
		dev_err(&data->client->dev, "Failed to initialize MT slots: %d\n", ret);
		return ret;
	}

	ret = input_register_device(idev);
	if(ret) {
		dev_err(&data->client->dev, "Failed to register input device: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tpd_init_regulators(struct tpd_data *data) {
	struct i2c_client *client = data->client;
	int ret;

	data->supplies[0].supply = "vdd";
	data->supplies[1].supply = "vddio";

	ret = devm_regulator_bulk_get(
		&client->dev,
		ARRAY_SIZE(data->supplies),
		data->supplies
	);
	if(ret < 0) {
		dev_err(&client->dev, "Failed to get regulators: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tpd_start(struct tpd_data *data) {
	int ret = regulator_bulk_enable(ARRAY_SIZE(data->supplies), data->supplies);

	if(ret) {
		dev_err(&data->client->dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	msleep(data->data->chip_on);
	/* Power on */
	gpiod_set_value_cansleep(data->reset_gpiod, 1);
	usleep_range(data->data->reset_min, data->data->reset_max);
	gpiod_set_value_cansleep(data->reset_gpiod, 0);
	msleep(data->data->firmware_on);
	enable_irq(data->client->irq);

	return 0;
}

static int tpd_stop(struct tpd_data *data) {
	int ret;
	disable_irq(data->client->irq);

	ret = regulator_bulk_disable(ARRAY_SIZE(data->supplies), data->supplies);
	if(ret) {
		dev_err(&data->client->dev, "Failed to disable regulators: %d\n", ret);
		return ret;
	}

	return 0;
}

/* Small functions */

static u8 tpd_checksum(u8 *data, u32 length) {
	s32 sum = 0;
	u32 i;

	for (i = 0; i < length; i++)
		sum += data[i];

	return (u8)((-sum) & 0xFF);
}

static void tpd_finger(struct tpd_data *data, int slot, const struct point *p) {
	input_mt_slot(data->idev, slot);
	input_mt_report_slot_state(data->idev, MT_TOOL_FINGER, true);
	input_report_abs(data->idev, ABS_MT_POSITION_X, p->x);
	input_report_abs(data->idev, ABS_MT_POSITION_Y, p->y);
	input_report_abs(data->idev, ABS_MT_TOUCH_MAJOR, 1);
}

static int tpd_input_open(struct input_dev *dev) {
	struct tpd_data *data = input_get_drvdata(dev);
	return tpd_start(data);
}

static void tpd_input_close(struct input_dev *dev) {
	struct tpd_data *data = input_get_drvdata(dev);
	tpd_stop(data);
}

static int __maybe_unused tpd_suspend(struct device *dev) {
	struct i2c_client *client = to_i2c_client(dev);
	struct tpd_data *data = i2c_get_clientdata(client);

	mutex_lock(&data->idev->mutex);

	/* if(input_device_enabled(data->idev)) */
		tpd_stop(data);

	mutex_unlock(&data->idev->mutex);

	return 0;
}

static int __maybe_unused tpd_resume(struct device *dev) {
	struct i2c_client *client = to_i2c_client(dev);
	struct tpd_data *data = i2c_get_clientdata(client);
	int ret = 0;

	mutex_lock(&data->idev->mutex);

	/* if(input_device_enabled(data->idev)) */
		ret = tpd_start(data);

	mutex_unlock(&data->idev->mutex);

	return ret;
}
