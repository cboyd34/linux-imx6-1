/*
 * i.MX LCDC crtc driver
 *
 * Copyright (C) 2012 Sascha Hauer, Pengutronix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/clk.h>
#include <linux/fb.h>
#include <drm/drmP.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>

#include "imx-drm.h"

#define LCDC_SSA		0x00
#define LCDC_SIZE		0x04
#define LCDC_VPW		0x08
#define LCDC_CPOS		0x0C
#define LCDC_LCWHB		0x10
#define LCDC_LCHCC		0x14
#define LCDC_PCR		0x18
#define LCDC_HCR		0x1C
#define LCDC_VCR		0x20
#define LCDC_POS		0x24
#define LCDC_LSCR1		0x28
#define LCDC_PWMR		0x2C
#define LCDC_DMACR		0x30
#define LCDC_RMCR		0x34
#define LCDC_LCDICR		0x38
#define LCDC_LIER		0x3c
#define LCDC_LISR		0x40

#define SIZE_XMAX(x)		((((x) >> 4) & 0x3f) << 20)

#define SIZE_YMAX_MX1(y)	((y) & 0x1ff)
#define SIZE_YMAX_MX21(y)	((y) & 0x3ff)

#define VPW_VPW(x)		((x) & 0x3ff)

#define PCR_TFT		(1 << 31)
#define PCR_COLOR	(1 << 30)
#define PCR_PBSIZ_1	(0 << 28)
#define PCR_PBSIZ_2	(1 << 28)
#define PCR_PBSIZ_4	(2 << 28)
#define PCR_PBSIZ_8	(3 << 28)
#define PCR_BPIX_1	(0 << 25)
#define PCR_BPIX_2	(1 << 25)
#define PCR_BPIX_4	(2 << 25)
#define PCR_BPIX_8	(3 << 25)
#define PCR_BPIX_12	(4 << 25)
#define PCR_BPIX_16	(5 << 25)
#define PCR_BPIX_18	(6 << 25)
#define PCR_PIXPOL	(1 << 24)
#define PCR_FLMPOL	(1 << 23)
#define PCR_LPPOL	(1 << 22)
#define PCR_CLKPOL	(1 << 21)
#define PCR_OEPOL	(1 << 20)
#define PCR_SCLKIDLE	(1 << 19)
#define PCR_END_SEL	(1 << 18)
#define PCR_END_BYTE_SWAP (1 << 17)
#define PCR_REV_VS	(1 << 16)
#define PCR_ACD_SEL	(1 << 15)
#define PCR_ACD(x)	(((x) & 0x7f) << 8)
#define PCR_SCLK_SEL	(1 << 7)
#define PCR_SHARP	(1 << 6)
#define PCR_PCD(x)	((x) & 0x3f)

#define HCR_H_WIDTH(x)		(((x) & 0x3f) << 26)
#define HCR_H_WAIT_1(x)		(((x) & 0xff) << 8)
#define HCR_H_WAIT_2(x)		((x) & 0xff)

#define VCR_V_WIDTH(x)		(((x) & 0x3f) << 26)
#define VCR_V_WAIT_1(x)		(((x) & 0xff) << 8)
#define VCR_V_WAIT_2(x)		((x) & 0xff)

#define RMCR_LCDC_EN_MX1	(1 << 1)

#define RMCR_SELF_REF		(1 << 0)

#define LIER_EOF		(1 << 1)

enum imx_lcdc_version {
	LCDC_VERSION_IMX1,
	LCDC_VERSION_IMX21,
};

struct imx_crtc {
	struct drm_crtc		base;
	struct imx_drm_crtc	*imx_drm_crtc;
	int			di_no;
	int			enabled;
	void __iomem		*regs;
	u32			pwmr;
	u32			lscr1;
	u32			dmacr;
	u32			pcr;
	struct clk		*clk;
	struct device		*dev;
	int			vblank_enable;
	enum imx_lcdc_version	version;

	struct drm_pending_vblank_event *page_flip_event;
	struct drm_framebuffer	*newfb;
};

#define to_imx_crtc(x) container_of(x, struct imx_crtc, base)

static void imx_crtc_load_lut(struct drm_crtc *crtc)
{
}

#define PCR_BPIX_8		(3 << 25)
#define PCR_BPIX_12		(4 << 25)
#define PCR_BPIX_16		(5 << 25)
#define PCR_BPIX_18		(6 << 25)
#define PCR_END_SEL		(1 << 18)
#define PCR_END_BYTE_SWAP	(1 << 17)

static const char *fourcc_to_str(u32 fourcc)
{
	static char buf[5];

	*(u32 *)buf = fourcc;
	buf[4] = 0;

	return buf;
}

static int imx_drm_crtc_set(struct drm_crtc *crtc,
		struct drm_display_mode *mode)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);
	struct drm_framebuffer *fb = crtc->fb;
	int lower_margin = mode->vsync_start - mode->vdisplay;
	int upper_margin = mode->vtotal - mode->vsync_end;
	int vsync_len = mode->vsync_end - mode->vsync_start;
	int hsync_len = mode->hsync_end - mode->hsync_start;
	int right_margin = mode->hsync_start - mode->hdisplay;
	int left_margin = mode->htotal - mode->hsync_end;
	u32 lcdc_size;
	unsigned long lcd_clk;
	u32 pcr;

	lcd_clk = clk_get_rate(imx_crtc->clk) / 1000;

	if (!mode->clock)
		return -EINVAL;

	pcr = DIV_ROUND_CLOSEST(lcd_clk, mode->clock);
	if (--pcr > 0x3f)
		pcr = 0x3f;

	switch (fb->pixel_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		pcr |= PCR_BPIX_18;
		pcr |= PCR_END_SEL | PCR_END_BYTE_SWAP;
		break;
	case DRM_FORMAT_RGB565:
		if (imx_crtc->version == LCDC_VERSION_IMX1)
			pcr |= PCR_BPIX_12;
		else
			pcr |= PCR_BPIX_16;
		break;
	case DRM_FORMAT_RGB332:
		pcr |= PCR_BPIX_8;
		break;
	default:
		dev_err(imx_crtc->dev, "unsupported pixel format %s\n",
				fourcc_to_str(fb->pixel_format));
		return -EINVAL;
	}

	/* add sync polarities */
	pcr |= imx_crtc->pcr & ~(0x3f | (7 << 25));

	dev_dbg(imx_crtc->dev,
			"xres=%d hsync_len=%d left_margin=%d right_margin=%d\n",
			mode->hdisplay, hsync_len,
			left_margin, right_margin);
	dev_dbg(imx_crtc->dev,
			"yres=%d vsync_len=%d upper_margin=%d lower_margin=%d\n",
			mode->vdisplay, vsync_len,
			upper_margin, lower_margin);

	writel(VPW_VPW(mode->hdisplay * fb->bits_per_pixel / 8 / 4),
		imx_crtc->regs + LCDC_VPW);

	writel(HCR_H_WIDTH(hsync_len - 1) |
		HCR_H_WAIT_1(right_margin - 1) |
		HCR_H_WAIT_2(left_margin - 3),
		imx_crtc->regs + LCDC_HCR);

	writel(VCR_V_WIDTH(vsync_len) |
		VCR_V_WAIT_1(lower_margin) |
		VCR_V_WAIT_2(upper_margin),
		imx_crtc->regs + LCDC_VCR);

	lcdc_size = SIZE_XMAX(mode->hdisplay);
	if (imx_crtc->version == LCDC_VERSION_IMX1)
		lcdc_size |= SIZE_YMAX_MX1(mode->vdisplay);
	else
		lcdc_size |= SIZE_YMAX_MX21(mode->vdisplay);
	writel(lcdc_size, imx_crtc->regs + LCDC_SIZE);

	writel(pcr, imx_crtc->regs + LCDC_PCR);
	writel(imx_crtc->pwmr, imx_crtc->regs + LCDC_PWMR);
	writel(imx_crtc->lscr1, imx_crtc->regs + LCDC_LSCR1);
	/* reset default */
	writel(0x00040060, imx_crtc->regs + LCDC_DMACR);

	return 0;
}

static int imx_drm_set_base(struct drm_crtc *crtc, int x, int y)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);
	struct drm_gem_cma_object *cma_obj;
	struct drm_framebuffer *fb = crtc->fb;
	unsigned long phys;

	cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	if (!cma_obj)
		return -EFAULT;

	phys = cma_obj->paddr;
	phys += x * (fb->bits_per_pixel >> 3);
	phys += y * fb->pitches[0];

	dev_dbg(imx_crtc->dev, "%s: phys: 0x%lx\n", __func__, phys);
	dev_dbg(imx_crtc->dev, "%s: xy: %dx%d\n", __func__, x, y);

	writel(phys, imx_crtc->regs + LCDC_SSA);

	return 0;
}

static int imx_crtc_mode_set(struct drm_crtc *crtc,
			       struct drm_display_mode *mode,
			       struct drm_display_mode *adjusted_mode,
			       int x, int y,
			       struct drm_framebuffer *old_fb)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);

	imx_drm_set_base(crtc, x, y);

	dev_dbg(imx_crtc->dev, "mode->hdisplay: %d\n", mode->hdisplay);
	dev_dbg(imx_crtc->dev, "mode->vdisplay: %d\n", mode->vdisplay);

	return imx_drm_crtc_set(crtc, mode);
}

static void imx_crtc_enable(struct imx_crtc *imx_crtc)
{
	if (!imx_crtc->enabled) {
		clk_prepare_enable(imx_crtc->clk);

		/*
		 * RMCR_LCDC_EN_MX1 is present on i.MX1 only, but doesn't hurt
		 * on other SoCs
		 */
		writel(RMCR_LCDC_EN_MX1, imx_crtc->regs + LCDC_RMCR);
	}

	imx_crtc->enabled = 1;
}

static void imx_crtc_disable(struct imx_crtc *imx_crtc)
{
	if (imx_crtc->enabled) {
		writel(0, imx_crtc->regs + LCDC_RMCR);
		clk_disable_unprepare(imx_crtc->clk);
	}

	imx_crtc->enabled = 0;
}

static void imx_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);

	dev_dbg(imx_crtc->dev, "%s mode: %d\n", __func__, mode);

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		imx_crtc_enable(imx_crtc);
		break;
	default:
		imx_crtc_disable(imx_crtc);
		break;
	}
}

static bool imx_crtc_mode_fixup(struct drm_crtc *crtc,
				  struct drm_display_mode *mode,
				  struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void imx_crtc_prepare(struct drm_crtc *crtc)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);

	imx_crtc_disable(imx_crtc);
}

static void imx_crtc_commit(struct drm_crtc *crtc)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);

	imx_crtc_enable(imx_crtc);
}

static struct drm_crtc_helper_funcs imx_helper_funcs = {
	.dpms = imx_crtc_dpms,
	.mode_fixup = imx_crtc_mode_fixup,
	.mode_set = imx_crtc_mode_set,
	.prepare = imx_crtc_prepare,
	.commit = imx_crtc_commit,
	.load_lut = imx_crtc_load_lut,
};

static void imx_drm_handle_pageflip(struct imx_crtc *imx_crtc)
{
	struct drm_pending_vblank_event *e;
	struct timeval now;
	unsigned long flags;
	struct drm_device *drm = imx_crtc->base.dev;

	spin_lock_irqsave(&drm->event_lock, flags);

	e = imx_crtc->page_flip_event;

	if (!e) {
		spin_unlock_irqrestore(&drm->event_lock, flags);
		return;
	}

	do_gettimeofday(&now);
	e->event.sequence = 0;
	e->event.tv_sec = now.tv_sec;
	e->event.tv_usec = now.tv_usec;
	imx_crtc->page_flip_event = NULL;

	list_add_tail(&e->base.link, &e->base.file_priv->event_list);

	wake_up_interruptible(&e->base.file_priv->event_wait);

	spin_unlock_irqrestore(&drm->event_lock, flags);
}

static int imx_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);

	writel(LIER_EOF, imx_crtc->regs + LCDC_LIER);

	return 0;
}

static void imx_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);

	writel(0, imx_crtc->regs + LCDC_LIER);
}

static irqreturn_t imx_irq_handler(int irq, void *dev_id)
{
	struct imx_crtc *imx_crtc = dev_id;
	struct drm_device *drm = imx_crtc->base.dev;

	/* Acknowledge interrupt */
	readl(imx_crtc->regs + LCDC_LISR);

	drm_handle_vblank(drm, 0);

	if (imx_crtc->newfb) {
		imx_crtc->base.fb = imx_crtc->newfb;
		imx_crtc->newfb = NULL;
		imx_drm_set_base(&imx_crtc->base, 0, 0);
		imx_drm_handle_pageflip(imx_crtc);
		imx_drm_crtc_vblank_put(imx_crtc->imx_drm_crtc);
	}

	return IRQ_HANDLED;
}

static int imx_page_flip(struct drm_crtc *crtc,
		struct drm_framebuffer *fb,
		struct drm_pending_vblank_event *event)
{
	struct imx_crtc *imx_crtc = to_imx_crtc(crtc);

	if (imx_crtc->newfb)
		return -EBUSY;

	imx_crtc->newfb = fb;
	imx_crtc->page_flip_event = event;
	imx_drm_crtc_vblank_get(imx_crtc->imx_drm_crtc);

	return 0;
}

static const struct drm_crtc_funcs imx_crtc_funcs = {
	.set_config = drm_crtc_helper_set_config,
	.destroy = drm_crtc_cleanup,
	.page_flip = imx_page_flip,
};

static const struct imx_drm_crtc_helper_funcs imx_imx_drm_helper = {
	.enable_vblank = imx_crtc_enable_vblank,
	.disable_vblank = imx_crtc_disable_vblank,
	.crtc_funcs = &imx_crtc_funcs,
	.crtc_helper_funcs = &imx_helper_funcs,
};

#define DRIVER_NAME "imx-lcdc-crtc"

/*
 * the pcr bits to be allowed to set in platform data
 */
#define PDATA_PCR	(PCR_PIXPOL | PCR_FLMPOL | PCR_LPPOL | \
			PCR_CLKPOL | PCR_OEPOL | PCR_TFT | PCR_COLOR | \
			PCR_PBSIZ_8 | PCR_ACD(0x7f) | PCR_ACD_SEL | \
			PCR_SCLK_SEL | PCR_SHARP)

static const struct of_device_id imx_lcdc_dt_ids[] = {
	{ .compatible = "fsl,imx1-lcdc", .data = (void *)LCDC_VERSION_IMX1, },
	{ .compatible = "fsl,imx21-lcdc", .data = (void *)LCDC_VERSION_IMX21, },
	{ /* sentinel */ }
};

static int __devinit imx_crtc_probe(struct platform_device *pdev)
{
	const struct of_device_id *of_id =
		of_match_device(imx_lcdc_dt_ids, &pdev->dev);
	struct imx_crtc *imx_crtc;
	struct resource *res;
	int ret, irq;
	u32 pcr_value = 0xf00080c0;
	u32 lscr1_value = 0x00120300;
	u32 pwmr_value = 0x00a903ff;

	imx_crtc = devm_kzalloc(&pdev->dev, sizeof(*imx_crtc), GFP_KERNEL);
	if (!imx_crtc)
		return -ENOMEM;

	imx_crtc->dev = &pdev->dev;
	imx_crtc->version = (enum imx_lcdc_version)of_id->data;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	res = devm_request_mem_region(&pdev->dev, res->start,
			resource_size(res), DRIVER_NAME);
	if (!res)
		return -EBUSY;

	imx_crtc->regs = devm_ioremap(&pdev->dev, res->start,
			resource_size(res));
	if (!imx_crtc->regs) {
		dev_err(&pdev->dev, "Cannot map frame buffer registers\n");
		return -EBUSY;
	}

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq, imx_irq_handler, 0, "imx_drm",
			imx_crtc);
	if (ret < 0) {
		dev_err(&pdev->dev, "irq request failed with %d\n", ret);
		return ret;
	}

	imx_crtc->clk = clk_get(&pdev->dev, NULL);
	if (IS_ERR(imx_crtc->clk)) {
		ret = PTR_ERR(imx_crtc->clk);
		dev_err(&pdev->dev, "unable to get clock: %d\n", ret);
		return ret;
	}

	clk_prepare_enable(imx_crtc->clk);
	imx_crtc->enabled = 1;

	platform_set_drvdata(pdev, imx_crtc);

	imx_crtc->pcr = pcr_value & PDATA_PCR;

	if (imx_crtc->pcr != pcr_value)
		dev_err(&pdev->dev, "invalid bits set in pcr: 0x%08x\n",
				pcr_value & ~PDATA_PCR);

	imx_crtc->lscr1 = lscr1_value;
	imx_crtc->pwmr = pwmr_value;

	ret = imx_drm_add_crtc(&imx_crtc->base,
			&imx_crtc->imx_drm_crtc,
			&imx_imx_drm_helper, THIS_MODULE,
			pdev->dev.of_node, 0);
	if (ret)
		goto err_init;

	dev_info(&pdev->dev, "probed\n");

	return 0;

err_init:
	clk_disable_unprepare(imx_crtc->clk);
	clk_put(imx_crtc->clk);

	return ret;
}

static int __devexit imx_crtc_remove(struct platform_device *pdev)
{
	struct imx_crtc *imx_crtc = platform_get_drvdata(pdev);

	imx_drm_remove_crtc(imx_crtc->imx_drm_crtc);

	writel(0, imx_crtc->regs + LCDC_LIER);

	clk_disable_unprepare(imx_crtc->clk);
	clk_put(imx_crtc->clk);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver imx_crtc_driver = {
	.remove		= __devexit_p(imx_crtc_remove),
	.probe		= imx_crtc_probe,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = imx_lcdc_dt_ids,
	},
};

static int __init imx_lcdc_init(void)
{
	return platform_driver_register(&imx_crtc_driver);
}

static void __exit imx_lcdc_exit(void)
{
	platform_driver_unregister(&imx_crtc_driver);
}

module_init(imx_lcdc_init);
module_exit(imx_lcdc_exit)

MODULE_DESCRIPTION("Freescale i.MX framebuffer driver");
MODULE_AUTHOR("Sascha Hauer, Pengutronix");
MODULE_LICENSE("GPL");
