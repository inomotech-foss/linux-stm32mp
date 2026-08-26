// SPDX-License-Identifier: GPL-2.0-only
/*
 * Quectel Modems GPIO Power sequences
 *
 * Copyright (C) 2026 Victor Bertin <victor.bertin@b-on.com>
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define POWER_ON_PULSE_MS		500
#define POWER_OFF_PULSE_MS		2000
#define RESET_PULSE_MS          300

struct pwrseq_quectel {
	struct gpio_desc *power_gpio;
	struct gpio_desc *reset_gpio;
	/* reserved for future use */
	struct gpio_desc *sleep_gpio;
	u32 power_on_pulse_ms;
	u32 power_off_pulse_ms;
    u32 reset_pulse_ms;
};

static void pwrseq_quectel_pulse(struct gpio_desc *gpiod, u32 pulse_ms)
{
	gpiod_set_value_cansleep(gpiod, 1);
	msleep(pulse_ms);
	gpiod_set_value_cansleep(gpiod, 0);
}

/* TODO: create sysfs attribute to write to to trigger a reset */
static void __maybe_unused pwrseq_quectel_reset(struct pwrseq_quectel *pwrseq)
{
	if (pwrseq->reset_gpio)
		pwrseq_quectel_pulse(pwrseq->reset_gpio,
				     pwrseq->reset_pulse_ms);
}

static int pwrseq_quectel_probe(struct platform_device *pdev)
{
	struct pwrseq_quectel *pwrseq;
	int ret;

	pwrseq = devm_kzalloc(&pdev->dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

	pwrseq->power_gpio = devm_gpiod_get(&pdev->dev, "power", GPIOD_OUT_LOW);
	ret = PTR_ERR_OR_ZERO(pwrseq->power_gpio);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Could not get power GPIO\n");
		return ret;
	}

	pwrseq->reset_gpio = devm_gpiod_get_optional(&pdev->dev, "reset",
						     GPIOD_OUT_LOW);
	ret = PTR_ERR_OR_ZERO(pwrseq->reset_gpio);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Could not get reset GPIO\n");
		return ret;
	}

	pwrseq->sleep_gpio = devm_gpiod_get_optional(&pdev->dev, "sleep",
						     GPIOD_OUT_LOW);
	ret = PTR_ERR_OR_ZERO(pwrseq->sleep_gpio);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Could not get sleep GPIO\n");
		return ret;
	}

	pwrseq->power_on_pulse_ms = POWER_ON_PULSE_MS;
	device_property_read_u32(&pdev->dev, "power-on-pulse-ms",
				 &pwrseq->power_on_pulse_ms);
	pwrseq->power_off_pulse_ms = POWER_OFF_PULSE_MS;
	device_property_read_u32(&pdev->dev, "power-off-pulse-ms",
				 &pwrseq->power_off_pulse_ms);
    pwrseq->reset_pulse_ms = RESET_PULSE_MS;
    device_property_read_u32(&pdev->dev, "reset-pulse-ms",
                &pwrseq->reset_pulse_ms);

	platform_set_drvdata(pdev, pwrseq);

	/* Power the modem up. */
	pwrseq_quectel_pulse(pwrseq->power_gpio, pwrseq->power_on_pulse_ms);

	return 0;
}

static void pwrseq_quectel_shutdown(struct platform_device *pdev)
{
	struct pwrseq_quectel *pwrseq = platform_get_drvdata(pdev);

	/*
	 * Power the modem down unconditionally: after a warm reboot it
	 * is still powered, and probing the fresh kernel must find it in
	 * a known powered-off state.
	 */
	pwrseq_quectel_pulse(pwrseq->power_gpio, pwrseq->power_off_pulse_ms);
}

static int pwrseq_quectel_suspend(struct device *dev)
{
	/*
	 * Insertion point for the modem sleep sequence once it is defined.
	 * Nothing to do on resume: the modem wakes up by itself.
	 */
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pwrseq_quectel_pm_ops,
				 pwrseq_quectel_suspend, NULL);

static const struct of_device_id pwrseq_quectel_of_match[] = {
	{ .compatible = "quectel,pwrseq" },
	{ }
};
MODULE_DEVICE_TABLE(of, pwrseq_quectel_of_match);

static struct platform_driver pwrseq_quectel_driver = {
	.probe = pwrseq_quectel_probe,
	.shutdown = pwrseq_quectel_shutdown,
	.driver = {
		.name = "pwrseq-quectel",
		.of_match_table = pwrseq_quectel_of_match,
		.pm = pm_sleep_ptr(&pwrseq_quectel_pm_ops),
	},
};
module_platform_driver(pwrseq_quectel_driver);

MODULE_AUTHOR("Victor Bertin <victor.bertin@b-on.com>");
MODULE_DESCRIPTION("Quectel Modems GPIO Power sequences");
MODULE_LICENSE("GPL");
