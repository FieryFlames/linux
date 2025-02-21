// SPDX-License-Identifier: GPL-2.0
// Copyright (C) Linus Walleij <linus.walleij@linaro.org>
// Copyright (C) 2024 Jack Matthews <jm5112356@gmail.com>

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/led-class-flash.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-flash-led-class.h>

#define AW3640_MAX_STEP 16

// TODO: go over the timeout stuff
#define AW3640_TIMEOUT_US 250000U
#define AW3640_MAX_TIMEOUT_US 300000U

struct aw3640 {
	struct led_classdev_flash fled;
	struct device *dev;
	struct v4l2_flash *v4l2_flash;
	struct mutex lock;
	struct regulator *reg;
	struct gpio_desc *enable_flash;
	struct timer_list powerdown_timer;
	enum led_brightness brightness;
	u32 max_timeout; /* Flash max timeout */
};

static struct aw3640 *to_aw3640(struct led_classdev_flash *fled)
{
	return container_of(fled, struct aw3640, fled);
}

static void aw3640_gpio_led_off(struct aw3640 *rt)
{
	gpiod_set_value(rt->enable_flash, 0);
}

static void aw3640_gpio_reset(struct gpio_desc *gpiod)
{
	gpiod_set_value(gpiod, 0);
	mdelay(3);
	gpiod_set_value(gpiod, 1);
	udelay(25);
}

static void aw3640_gpio_pulse(struct gpio_desc *gpiod)
{
	gpiod_set_value(gpiod, 0);
	udelay(1);
	gpiod_set_value(gpiod, 1);
	udelay(1);
}

/* This is setting the torch light level */
static int aw3640_led_brightness_set(struct led_classdev *led,
				     enum led_brightness brightness)
{
	struct led_classdev_flash *fled = lcdev_to_flcdev(led);
	struct aw3640 *rt = to_aw3640(fled);
	int current_brightness, pulses, i;

	mutex_lock(&rt->lock);

	if (brightness == LED_OFF) {
		/* Off */
		aw3640_gpio_led_off(rt);
	} else {
		current_brightness = rt->brightness;
		if (current_brightness == LED_OFF) {
			aw3640_gpio_reset(rt->enable_flash);
			current_brightness = AW3640_MAX_STEP;
		}

		pulses = (current_brightness - brightness + AW3640_MAX_STEP) %
			 AW3640_MAX_STEP;

		for (i = 0; i < pulses; i++) {
			aw3640_gpio_pulse(rt->enable_flash);
		}
	}

	rt->brightness = brightness;

	mutex_unlock(&rt->lock);

	return 0;
}

static int aw3640_led_flash_strobe_set(struct led_classdev_flash *fled,
				       bool state)
{
	struct aw3640 *rt = to_aw3640(fled);
	struct led_flash_setting *timeout = &fled->timeout;

	mutex_lock(&rt->lock);

	if (state) {
		/* Reset flash to max brightness*/
		aw3640_gpio_reset(rt->enable_flash);
		/* Set timeout */
		mod_timer(&rt->powerdown_timer,
			  jiffies + usecs_to_jiffies(timeout->val));
	} else {
		del_timer_sync(&rt->powerdown_timer);
		/* Turn the LED off */
		aw3640_gpio_led_off(rt);
	}

	fled->led_cdev.brightness = LED_OFF;
	/* After this the torch LED will be disabled */

	mutex_unlock(&rt->lock);

	return 0;
}

static int aw3640_led_flash_strobe_get(struct led_classdev_flash *fled,
				       bool *state)
{
	struct aw3640 *rt = to_aw3640(fled);

	*state = timer_pending(&rt->powerdown_timer);

	return 0;
}

static int aw3640_led_flash_timeout_set(struct led_classdev_flash *fled,
					u32 timeout)
{
	/* The timeout is stored in the led-class-flash core */
	return 0;
}

static const struct led_flash_ops aw3640_flash_ops = {
	.strobe_set = aw3640_led_flash_strobe_set,
	.strobe_get = aw3640_led_flash_strobe_get,
	.timeout_set = aw3640_led_flash_timeout_set,
};

static void aw3640_powerdown_timer(struct timer_list *t)
{
	struct aw3640 *rt = from_timer(rt, t, powerdown_timer);

	/* Turn the LED off */
	aw3640_gpio_led_off(rt);
}

static void aw3640_init_flash_timeout(struct aw3640 *rt)
{
	struct led_classdev_flash *fled = &rt->fled;
	struct led_flash_setting *s;

	/* Init flash timeout setting */
	s = &fled->timeout;
	s->min = 1;
	s->max = rt->max_timeout;
	s->step = 1;
	/*
	 * Set default timeout to AW3640_TIMEOUT_US except if
	 * max_timeout from DT is lower.
	 */
	s->val = min(rt->max_timeout, AW3640_TIMEOUT_US);
}

// TODO: test V4L2
#if IS_ENABLED(CONFIG_V4L2_FLASH_LED_CLASS)
/* Configure the V$L2 flash subdevice */
static void aw3640_init_v4l2_flash_config(struct aw3640 *rt,
					  struct v4l2_flash_config *v4l2_sd_cfg)
{
	struct led_classdev *led = &rt->fled.led_cdev;
	struct led_flash_setting *s;

	strscpy(v4l2_sd_cfg->dev_name, led->dev->kobj.name,
		sizeof(v4l2_sd_cfg->dev_name));

	/*
	 * Init flash intensity setting: this is a linear scale
	 * capped from the device tree max intensity setting
	 * 1..flash_max_intensity
	 */
	s = &v4l2_sd_cfg->intensity;
	s->min = 1;
	s->max = AW3640_MAX_STEP;
	s->step = 1;
	s->val = s->max;
}

static void aw3640_v4l2_flash_release(struct aw3640 *rt)
{
	v4l2_flash_release(rt->v4l2_flash);
}

#else
static void aw3640_init_v4l2_flash_config(struct aw3640 *rt,
					  struct v4l2_flash_config *v4l2_sd_cfg)
{
}

static void aw3640_v4l2_flash_release(struct aw3640 *rt)
{
}
#endif

static int aw3640_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *child;
	struct aw3640 *rt;
	struct led_classdev *led;
	struct led_classdev_flash *fled;
	struct led_init_data init_data = {};
	struct v4l2_flash_config v4l2_sd_cfg = {};
	int ret;

	rt = devm_kzalloc(dev, sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	rt->dev = dev;
	fled = &rt->fled;
	led = &fled->led_cdev;

	/* ENF - Enable Flash line */
	rt->enable_flash = devm_gpiod_get(dev, "enf", GPIOD_OUT_LOW);
	if (IS_ERR(rt->enable_flash))
		return dev_err_probe(dev, PTR_ERR(rt->enable_flash),
				     "cannot get ENF (enable flash) GPIO\n");

	child = fwnode_get_next_available_child_node(dev->fwnode, NULL);
	if (!child) {
		dev_err(dev, "No fwnode child node found for connected LED.\n");
		return -EINVAL;
	}
	init_data.fwnode = child;

	ret = fwnode_property_read_u32(child, "flash-max-timeout-us",
				       &rt->max_timeout);
	if (ret) {
		rt->max_timeout = AW3640_MAX_TIMEOUT_US;
		dev_warn(dev, "flash-max-timeout-us property missing\n");
	}
	timer_setup(&rt->powerdown_timer, aw3640_powerdown_timer, 0);
	aw3640_init_flash_timeout(rt);

	fled->ops = &aw3640_flash_ops;

	led->max_brightness = AW3640_MAX_STEP;
	led->brightness_set_blocking = aw3640_led_brightness_set;
	led->flags |= LED_CORE_SUSPENDRESUME | LED_DEV_CAP_FLASH;

	mutex_init(&rt->lock);

	platform_set_drvdata(pdev, rt);

	ret = devm_led_classdev_flash_register_ext(dev, fled, &init_data);
	if (ret) {
		fwnode_handle_put(child);
		mutex_destroy(&rt->lock);
		dev_err(dev, "can't register LED %s\n", led->name);
		return ret;
	}

	aw3640_init_v4l2_flash_config(rt, &v4l2_sd_cfg);

	/* Create a V4L2 Flash device if V4L2 flash is enabled */
	rt->v4l2_flash = v4l2_flash_init(dev, child, fled, NULL, &v4l2_sd_cfg);
	if (IS_ERR(rt->v4l2_flash)) {
		ret = PTR_ERR(rt->v4l2_flash);
		dev_err(dev, "failed to register V4L2 flash device (%d)\n",
			ret);
		/*
		 * Continue without the V4L2 flash
		 * (we still have the classdev)
		 */
	}

	fwnode_handle_put(child);
	return 0;
}

static void aw3640_remove(struct platform_device *pdev)
{
	struct aw3640 *rt = platform_get_drvdata(pdev);

	aw3640_v4l2_flash_release(rt);
	del_timer_sync(&rt->powerdown_timer);
	mutex_destroy(&rt->lock);
}

// TODO: driver is very applicable to AW36402 and AW36404 (Added FLASH/TORCH gpio, 64 steps of brightness)
// TODO: Maybe applicable to AW36406 (Added FLASH/TORCH gpio, PWM control)
static const struct of_device_id aw3640_match[] = {
	{
		.compatible = "awinic,aw3640",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, aw3640_match);

static struct platform_driver aw3640_driver = {
	.driver = {
		.name  = "aw3640",
		.of_match_table = aw3640_match,
	},
	.probe  = aw3640_probe,
	.remove = aw3640_remove,
};
module_platform_driver(aw3640_driver);

MODULE_AUTHOR("Jack Matthews <jm5112356@gmail.com>");
MODULE_DESCRIPTION("AWINIC AW3640 LED driver");
MODULE_LICENSE("GPL");
