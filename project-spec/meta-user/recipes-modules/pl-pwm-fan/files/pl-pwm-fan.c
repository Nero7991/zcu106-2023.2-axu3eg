/*  pl-pwm-fan.c - thermal cooling device driven by the PL axi_pwm peripheral.
 *
 * Copyright (c) 2024 Oren Collaco.  GPL-2.0-only.
 *
 * Phase B of the AXU3EG fan work: instead of software-toggling a GPIO
 * (soft-pwm-fan), this writes a duty register in the custom axi_pwm PL IP
 * (25 kHz hardware PWM on fan pin AA11). Registers as a thermal cooling device
 * so the same device-tree thermal staircase drives proportional fan speed.
 *
 * Register map (axi_pwm):
 *   0x00 CTRL   : bit0 ENABLE, bit1 POLARITY (1 = invert, for active-low fan)
 *   0x04 PERIOD : carrier period in PL clock cycles (200 MHz / 25 kHz = 8000)
 *   0x08 DUTY   : high-time in cycles, 0..PERIOD (before polarity inversion)
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/bits.h>
#include <linux/thermal.h>

#define REG_CTRL	0x00
#define REG_PERIOD	0x04
#define REG_DUTY	0x08
#define CTRL_ENABLE	BIT(0)
#define CTRL_POLARITY	BIT(1)

#define MAX_LEVELS	32

struct plpwm {
	void __iomem *base;
	u32 period;
	u32 num_levels;			/* max cooling state */
	u32 levels[MAX_LEVELS + 1];	/* per-state duty in per-mille (0..1000) */
	bool active_low;
	struct thermal_cooling_device *cdev;
	unsigned long cur;
};

static void plpwm_apply(struct plpwm *p, unsigned long state)
{
	u32 duty, permille, ctrl = CTRL_ENABLE | (p->active_low ? CTRL_POLARITY : 0);

	if (state > p->num_levels)
		state = p->num_levels;

	permille = p->levels[state];		/* explicit duty table */
	if (permille > 1000)
		permille = 1000;
	duty = (u32)((u64)p->period * permille / 1000);

	writel(p->period, p->base + REG_PERIOD);
	writel(duty,      p->base + REG_DUTY);
	writel(ctrl,      p->base + REG_CTRL);
	p->cur = state;
}

static int plpwm_get_max(struct thermal_cooling_device *c, unsigned long *s)
{
	*s = ((struct plpwm *)c->devdata)->num_levels;
	return 0;
}

static int plpwm_get_cur(struct thermal_cooling_device *c, unsigned long *s)
{
	*s = ((struct plpwm *)c->devdata)->cur;
	return 0;
}

static int plpwm_set_cur(struct thermal_cooling_device *c, unsigned long s)
{
	plpwm_apply(c->devdata, s);
	return 0;
}

static const struct thermal_cooling_device_ops plpwm_ops = {
	.get_max_state = plpwm_get_max,
	.get_cur_state = plpwm_get_cur,
	.set_cur_state = plpwm_set_cur,
};

static int plpwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct plpwm *p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	int n, i;

	if (!p)
		return -ENOMEM;

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	if (of_property_read_u32(np, "pwm-period-cycles", &p->period) || !p->period)
		p->period = 8000;
	p->active_low = of_property_read_bool(np, "active-low");

	/* Duty table (per-mille) indexed by cooling state; entry 0 must be off. */
	n = of_property_count_u32_elems(np, "cooling-levels");
	if (n < 2 || n > MAX_LEVELS + 1) {
		dev_err(dev, "cooling-levels must have 2..%d entries (got %d)\n",
			MAX_LEVELS + 1, n);
		return -EINVAL;
	}
	if (of_property_read_u32_array(np, "cooling-levels", p->levels, n))
		return -EINVAL;
	p->num_levels = n - 1;		/* states 0..num_levels */

	plpwm_apply(p, 0);		/* start off */

	p->cdev = devm_thermal_of_cooling_device_register(dev, np,
			(char *)pdev->name, p, &plpwm_ops);
	if (IS_ERR(p->cdev))
		return PTR_ERR(p->cdev);

	platform_set_drvdata(pdev, p);
	dev_info(dev, "pl-pwm-fan: period=%u states=0..%u active_low=%d levels[0..N]=",
		 p->period, p->num_levels, p->active_low);
	for (i = 0; i <= p->num_levels; i++)
		pr_cont("%u ", p->levels[i]);
	pr_cont("(per-mille)\n");
	return 0;
}

static int plpwm_remove(struct platform_device *pdev)
{
	struct plpwm *p = platform_get_drvdata(pdev);

	if (p)
		plpwm_apply(p, 0);	/* fan off */
	return 0;
}

static const struct of_device_id plpwm_of[] = {
	{ .compatible = "user,axi-pwm-fan" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, plpwm_of);

static struct platform_driver plpwm_driver = {
	.driver = {
		.name = "pl-pwm-fan",
		.of_match_table = plpwm_of,
	},
	.probe = plpwm_probe,
	.remove = plpwm_remove,
};
module_platform_driver(plpwm_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Oren Collaco");
MODULE_DESCRIPTION("PL AXI-PWM fan cooling device (25 kHz hardware PWM)");
