// SPDX-License-Identifier: GPL-2.0-only
/*
 * Quectel Modems GPIO Power sequences
 *
 * Copyright (C) 2026 Victor Bertin <victor.bertin@b-on.com>
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/kstrtox.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>

#define VBUS_OFF_SETTLE_MS	500

struct pwrseq_quectel_data {
	u32 power_on_pulse_ms;
	u32 power_off_pulse_ms;
	u32 reset_pulse_ms;
};

struct pwrseq_quectel {
	struct device *dev;
	struct mutex lock;
	struct gpio_desc *power_gpio;
	struct gpio_desc *reset_gpio;
	struct regulator *vbus;
	struct notifier_block pm_nb;
	bool vbus_off;
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

static ssize_t power_on_pulse_ms_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct pwrseq_quectel *pwrseq = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", pwrseq->power_on_pulse_ms);
}

static ssize_t power_on_pulse_ms_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct pwrseq_quectel *pwrseq = dev_get_drvdata(dev);
	int ret;

	ret = kstrtou32(buf, 0, &pwrseq->power_on_pulse_ms);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(power_on_pulse_ms);

static ssize_t power_off_pulse_ms_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct pwrseq_quectel *pwrseq = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", pwrseq->power_off_pulse_ms);
}

static ssize_t power_off_pulse_ms_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct pwrseq_quectel *pwrseq = dev_get_drvdata(dev);
	int ret;

	ret = kstrtou32(buf, 0, &pwrseq->power_off_pulse_ms);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(power_off_pulse_ms);

static ssize_t reset_pulse_ms_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct pwrseq_quectel *pwrseq = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", pwrseq->reset_pulse_ms);
}

static ssize_t reset_pulse_ms_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct pwrseq_quectel *pwrseq = dev_get_drvdata(dev);
	int ret;

	ret = kstrtou32(buf, 0, &pwrseq->reset_pulse_ms);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(reset_pulse_ms);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct pwrseq_quectel *pwrseq = dev_get_drvdata(dev);

	if (!sysfs_streq(buf, "reset")) {
		dev_err(dev,
			"attempt to trigger modem reset with invalid keyword '%s'\n",
			buf);
		return -EINVAL;
	}

	if (!pwrseq->reset_gpio)
		return -ENODEV;

	guard(mutex)(&pwrseq->lock);

	dev_info(dev, "modem reset\n");
	pwrseq_quectel_pulse(pwrseq->reset_gpio, pwrseq->reset_pulse_ms);

	return count;
}
static DEVICE_ATTR_WO(reset);

static struct attribute *pwrseq_quectel_attrs[] = {
	&dev_attr_power_on_pulse_ms.attr, &dev_attr_power_off_pulse_ms.attr,
	&dev_attr_reset_pulse_ms.attr, &dev_attr_reset.attr, NULL
};
ATTRIBUTE_GROUPS(pwrseq_quectel);

/*
 * The modem only enters sleep mode while its USB link is suspended or
 * disconnected. In Standby the USB PHY loses power and cannot hold the link
 * suspended, so cut VBUS before suspend to make the modem see a disconnect.
 */
static int pwrseq_quectel_pm_notify(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	struct pwrseq_quectel *pwrseq =
		container_of(nb, struct pwrseq_quectel, pm_nb);
	int ret;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		ret = regulator_disable(pwrseq->vbus);
		if (ret) {
			dev_err(pwrseq->dev, "failed to cut VBUS: %d\n", ret);
			break;
		}
		pwrseq->vbus_off = true;
		dev_info(pwrseq->dev, "modem VBUS off\n");
		/* Let the USB core handle the disconnect before tasks freeze */
		msleep(VBUS_OFF_SETTLE_MS);
		break;
	case PM_POST_SUSPEND:
		if (!pwrseq->vbus_off)
			break;
		ret = regulator_enable(pwrseq->vbus);
		if (ret) {
			dev_err(pwrseq->dev, "failed to restore VBUS: %d\n", ret);
			break;
		}
		pwrseq->vbus_off = false;
		dev_info(pwrseq->dev, "modem VBUS on\n");
		break;
	}

	return NOTIFY_OK;
}

static void pwrseq_quectel_vbus_release(void *data)
{
	struct pwrseq_quectel *pwrseq = data;

	unregister_pm_notifier(&pwrseq->pm_nb);
	if (!pwrseq->vbus_off)
		regulator_disable(pwrseq->vbus);
}

static int pwrseq_quectel_vbus_init(struct pwrseq_quectel *pwrseq)
{
	struct device *dev = pwrseq->dev;
	int ret;

	pwrseq->vbus = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(pwrseq->vbus)) {
		ret = PTR_ERR(pwrseq->vbus);
		pwrseq->vbus = NULL;
		if (ret == -ENODEV)
			return 0;
		return dev_err_probe(dev, ret, "Could not get VBUS supply\n");
	}

	ret = regulator_enable(pwrseq->vbus);
	if (ret)
		return dev_err_probe(dev, ret, "Could not enable VBUS\n");

	pwrseq->pm_nb.notifier_call = pwrseq_quectel_pm_notify;
	ret = register_pm_notifier(&pwrseq->pm_nb);
	if (ret) {
		regulator_disable(pwrseq->vbus);
		return dev_err_probe(dev, ret, "Could not register PM notifier\n");
	}

	return devm_add_action_or_reset(dev, pwrseq_quectel_vbus_release,
					pwrseq);
}

static int pwrseq_quectel_probe(struct platform_device *pdev)
{
	const struct pwrseq_quectel_data *data = device_get_match_data(&pdev->dev);
	struct pwrseq_quectel *pwrseq;
	int ret;

	pwrseq = devm_kzalloc(&pdev->dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

	pwrseq->dev = &pdev->dev;
	mutex_init(&pwrseq->lock);

	pwrseq->power_gpio =
		devm_gpiod_get_optional(&pdev->dev, "power", GPIOD_OUT_LOW);
	ret = PTR_ERR_OR_ZERO(pwrseq->power_gpio);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Could not get power GPIO\n");
		return ret;
	}

	pwrseq->reset_gpio =
		devm_gpiod_get_optional(&pdev->dev, "reset", GPIOD_OUT_LOW);
	ret = PTR_ERR_OR_ZERO(pwrseq->reset_gpio);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Could not get reset GPIO\n");
		return ret;
	}

	pwrseq->power_on_pulse_ms = data->power_on_pulse_ms;
	pwrseq->power_off_pulse_ms = data->power_off_pulse_ms;
	pwrseq->reset_pulse_ms = data->reset_pulse_ms;
	device_property_read_u32(&pdev->dev, "power-on-pulse-ms",
				 &pwrseq->power_on_pulse_ms);
	device_property_read_u32(&pdev->dev, "power-off-pulse-ms",
				 &pwrseq->power_off_pulse_ms);
	device_property_read_u32(&pdev->dev, "reset-pulse-ms",
				 &pwrseq->reset_pulse_ms);

	ret = pwrseq_quectel_vbus_init(pwrseq);
	if (ret)
		return ret;

	if (pwrseq->power_gpio) {
		dev_info(&pdev->dev, "modem power on\n");
		pwrseq_quectel_pulse(pwrseq->power_gpio,
				     pwrseq->power_on_pulse_ms);
	}

	platform_set_drvdata(pdev, pwrseq);

	return 0;
}

static void pwrseq_quectel_shutdown(struct platform_device *pdev)
{
	struct pwrseq_quectel *pwrseq = platform_get_drvdata(pdev);

	if (!pwrseq->power_gpio)
		return;

	guard(mutex)(&pwrseq->lock);

	dev_info(&pdev->dev, "modem power off\n");
	pwrseq_quectel_pulse(pwrseq->power_gpio, pwrseq->power_off_pulse_ms);
}

static const struct pwrseq_quectel_data ag35_data = {
	.power_on_pulse_ms = 500,
	.power_off_pulse_ms = 650,
	.reset_pulse_ms = 460,
};

static const struct pwrseq_quectel_data eg915k_data = {
	.power_on_pulse_ms = 700,
	.power_off_pulse_ms = 650,
	.reset_pulse_ms = 300,
};

static const struct pwrseq_quectel_data eg915n_data = {
	.power_on_pulse_ms = 500,
	.power_off_pulse_ms = 650,
	.reset_pulse_ms = 300,
};

static const struct of_device_id pwrseq_quectel_of_match[] = {
	{ .compatible = "quectel,ag35-pwrseq", .data = &ag35_data },
	{ .compatible = "quectel,eg915k-pwrseq", .data = &eg915k_data },
	{ .compatible = "quectel,eg915n-pwrseq", .data = &eg915n_data },
	{ }
};
MODULE_DEVICE_TABLE(of, pwrseq_quectel_of_match);

static struct platform_driver pwrseq_quectel_driver = {
	.probe = pwrseq_quectel_probe,
	.shutdown = pwrseq_quectel_shutdown,
	.driver = {
		.name = "pwrseq-quectel",
		.of_match_table = pwrseq_quectel_of_match,
		.dev_groups = pwrseq_quectel_groups,
	},
};
module_platform_driver(pwrseq_quectel_driver);

MODULE_AUTHOR("Victor Bertin <victor.bertin@b-on.com>");
MODULE_DESCRIPTION("Quectel Modems GPIO Power sequences");
MODULE_LICENSE("GPL");
