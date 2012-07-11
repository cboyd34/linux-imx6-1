/*
 * Copyright 2012 Freescale Semiconductor, Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>

struct usbmisc;

struct soc_data {
	int (*init) (struct usbmisc *usbmisc, int id);
	void (*exit) (struct usbmisc *usbmisc, int id);
};

struct usbmisc {
	struct soc_data *soc_data;
	void __iomem *base;
	struct clk *clk;

	int dis_oc:1;
};

/* Since we only have one usbmisc device at runtime, we global variables */
static struct usbmisc *usbmisc;

static int imx6q_usbmisc_init(struct usbmisc *usbmisc, int id)
{
	u32 reg;

	if (usbmisc->dis_oc) {
		reg = readl_relaxed(usbmisc->base + id * 4);
		writel_relaxed(reg | (1 << 7), usbmisc->base + id * 4);
	}

	return 0;
}

static struct soc_data imx6q_data = {
	.init = imx6q_usbmisc_init,
};


static const struct of_device_id imx_usbmisc_dt_ids[] = {
	{ .compatible = "fsl,imx6q-usbmisc", .data = &imx6q_data },
	{ /* sentinel */ }
};

static int __devinit imx_usbmisc_probe(struct platform_device *pdev)
{
	struct resource	*res;
	struct usbmisc *data;
	const struct of_device_id *of_id =
			of_match_device(imx_usbmisc_dt_ids, &pdev->dev);

	int ret;

	if (usbmisc)
		return -EBUSY;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	data->base = devm_request_and_ioremap(&pdev->dev, res);
	if (!data->base)
		return -EADDRNOTAVAIL;

	data->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(data->clk)) {
		dev_err(&pdev->dev,
			"failed to get clock, err=%ld\n", PTR_ERR(data->clk));
		return PTR_ERR(data->clk);
	}

	ret = clk_prepare_enable(data->clk);
	if (!ret) {
		if (of_find_property(pdev->dev.of_node,
			"fsl,disable-over-current", NULL))
			data->dis_oc = 1;
		data->soc_data = of_id->data;
		usbmisc = data;
	}

	return ret;
}

static int __devexit imx_usbmisc_remove(struct platform_device *pdev)
{
	clk_disable_unprepare(usbmisc->clk);
	return 0;
}

static struct platform_driver imx_usbmisc_driver = {
	.probe = imx_usbmisc_probe,
	.remove = __devexit_p(imx_usbmisc_remove),
	.driver = {
		.name = "imx_usbmisc",
		.owner = THIS_MODULE,
		.of_match_table = imx_usbmisc_dt_ids,
	 },
};

int imx_usbmisc_init(struct device *usbdev)
{
	struct device_node *np = usbdev->of_node;
	int ret;

	if (!usbmisc)
		return 0;

	ret = of_alias_get_id(np, "usb");
	if (ret < 0) {
		dev_err(usbdev, "failed to get alias id, errno %d\n", ret);
		return ret;
	}

	return usbmisc->soc_data->init(usbmisc, ret);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_init);

static int __init imx_usbmisc_drv_init(void)
{
	return platform_driver_register(&imx_usbmisc_driver);
}
subsys_initcall(imx_usbmisc_drv_init);

static void __exit imx_usbmisc_drv_exit(void)
{
	platform_driver_unregister(&imx_usbmisc_driver);
}
module_exit(imx_usbmisc_drv_exit);

MODULE_LICENSE("GPL v2");
