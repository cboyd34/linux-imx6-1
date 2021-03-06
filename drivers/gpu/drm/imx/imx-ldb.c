/*
 * i.MX drm driver - LVDS display bridge
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <drm/drmP.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_crtc_helper.h>
#include <linux/videodev2.h>

#include "imx-drm.h"

#define DRIVER_NAME "imx-ldb"

#define LDB_CH0_MODE_EN_TO_DI0		(1 << 0)
#define LDB_CH0_MODE_EN_TO_DI1		(3 << 0)
#define LDB_CH1_MODE_EN_TO_DI0		(1 << 2)
#define LDB_CH1_MODE_EN_TO_DI1		(3 << 2)
#define LDB_SPLIT_MODE_EN		(1 << 4)
#define LDB_DATA_WIDTH_CH0_24		(1 << 5)
#define LDB_BIT_MAP_CH0_JEIDA		(1 << 6)
#define LDB_DATA_WIDTH_CH1_24		(1 << 7)
#define LDB_BIT_MAP_CH1_JEIDA		(1 << 8)
#define LDB_DI0_VS_POL_ACT_LOW		(1 << 9)
#define LDB_DI1_VS_POL_ACT_LOW		(1 << 10)
#define LDB_BGREF_RMODE_INT		(1 << 15)

#define con_to_imx_ldb_ch(x) container_of(x, struct imx_ldb_channel, connector)
#define enc_to_imx_ldb_ch(x) container_of(x, struct imx_ldb_channel, encoder)

struct imx_ldb;

struct imx_ldb_channel {
	struct imx_ldb *ldb;
	struct drm_connector connector;
	struct imx_drm_connector *imx_drm_connector;
	struct drm_encoder encoder;
	struct imx_drm_encoder *imx_drm_encoder;
	int chno;
	void *edid;
	int edid_len;
	struct clk *clk; /* our own clock */
	struct clk *clk_sel; /* parent of display clock */
	struct clk *clk_pll; /* upstream clock we can adjust */
};

struct imx_ldb {
	void __iomem *base;
	struct device *dev;
	struct imx_ldb_channel channel[2];
};

static enum drm_connector_status imx_ldb_connector_detect(
		struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static void imx_ldb_connector_destroy(struct drm_connector *connector)
{
	/* do not free here */
}

static int imx_ldb_connector_get_modes(struct drm_connector *connector)
{
	struct imx_ldb_channel *imx_ldb_ch = con_to_imx_ldb_ch(connector);
	int ret;

	if (!imx_ldb_ch->edid)
		return 0;

	drm_mode_connector_update_edid_property(connector, imx_ldb_ch->edid);
	ret = drm_add_edid_modes(connector, imx_ldb_ch->edid);
	connector->display_info.raw_edid = NULL;

	return ret;
}

static int imx_ldb_connector_mode_valid(struct drm_connector *connector,
			  struct drm_display_mode *mode)
{
	return 0;
}

static struct drm_encoder *imx_ldb_connector_best_encoder(
		struct drm_connector *connector)
{
	struct imx_ldb_channel *imx_ldb_ch = con_to_imx_ldb_ch(connector);

	return &imx_ldb_ch->encoder;
}

static void imx_ldb_encoder_dpms(struct drm_encoder *encoder, int mode)
{
}

static bool imx_ldb_encoder_mode_fixup(struct drm_encoder *encoder,
			   struct drm_display_mode *mode,
			   struct drm_display_mode *adjusted_mode)
{
	struct imx_ldb_channel *imx_ldb_ch = enc_to_imx_ldb_ch(encoder);

/*
	adjusted_mode->clock = clk_round_rate(imx_ldb_ch->clk_pll,
			adjusted_mode->clock * 1000) / 1000;
*/
	return true;
}

static void imx_ldb_encoder_prepare(struct drm_encoder *encoder)
{
	struct imx_ldb_channel *imx_ldb_ch = enc_to_imx_ldb_ch(encoder);
	struct imx_ldb *ldb = imx_ldb_ch->ldb;
	int ret;
	struct drm_display_mode *mode = &encoder->crtc->mode;

	dev_dbg(ldb->dev, "%s: now: %ld want: %ld\n", __func__,
			clk_get_rate(imx_ldb_ch->clk_pll),
			mode->clock * 1000 * 7);
	clk_set_rate(imx_ldb_ch->clk_pll, mode->clock * 1000 * 7);

	dev_dbg(ldb->dev, "%s after: %ld\n", __func__,
			clk_get_rate(imx_ldb_ch->clk_pll));

	/* set display clock mux to LDB input clock */
	ret = clk_set_parent(imx_ldb_ch->clk_sel, imx_ldb_ch->clk);
	if (ret) {
		dev_err(ldb->dev, "unable to set di%d parent clock\n",
				imx_ldb_ch->chno);
	}

	imx_drm_crtc_panel_format(encoder->crtc, DRM_MODE_ENCODER_LVDS,
			V4L2_PIX_FMT_RGB24);
}

static void imx_ldb_encoder_commit(struct drm_encoder *encoder)
{
}

static void imx_ldb_encoder_mode_set(struct drm_encoder *encoder,
			 struct drm_display_mode *mode,
			 struct drm_display_mode *adjusted_mode)
{
}

static void imx_ldb_encoder_disable(struct drm_encoder *encoder)
{
}

static void imx_ldb_encoder_destroy(struct drm_encoder *encoder)
{
	/* do not free here */
}

static struct drm_connector_funcs imx_ldb_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = imx_ldb_connector_detect,
	.destroy = imx_ldb_connector_destroy,
};

static struct drm_connector_helper_funcs imx_ldb_connector_helper_funcs = {
	.get_modes = imx_ldb_connector_get_modes,
	.best_encoder = imx_ldb_connector_best_encoder,
	.mode_valid = imx_ldb_connector_mode_valid,
};

static struct drm_encoder_funcs imx_ldb_encoder_funcs = {
	.destroy = imx_ldb_encoder_destroy,
};

static struct drm_encoder_helper_funcs imx_ldb_encoder_helper_funcs = {
	.dpms = imx_ldb_encoder_dpms,
	.mode_fixup = imx_ldb_encoder_mode_fixup,
	.prepare = imx_ldb_encoder_prepare,
	.commit = imx_ldb_encoder_commit,
	.mode_set = imx_ldb_encoder_mode_set,
	.disable = imx_ldb_encoder_disable,
};

static int imx_ldb_register(struct imx_ldb_channel *imx_ldb_ch, void *edid,
		int edid_len, u32 crtcs)
{
	int ret;
	struct imx_ldb *ldb = imx_ldb_ch->ldb;
	char clkname[16];

	sprintf(clkname, "di%d", imx_ldb_ch->chno);
	imx_ldb_ch->clk = devm_clk_get(ldb->dev, clkname);
	if (IS_ERR(imx_ldb_ch->clk))
		return PTR_ERR(imx_ldb_ch->clk);

	sprintf(clkname, "di%d_sel", imx_ldb_ch->chno);
	imx_ldb_ch->clk_sel = devm_clk_get(ldb->dev, clkname);
	if (IS_ERR(imx_ldb_ch->clk_sel))
		return PTR_ERR(imx_ldb_ch->clk_sel);

	sprintf(clkname, "di%d_pll", imx_ldb_ch->chno);
	imx_ldb_ch->clk_pll = devm_clk_get(ldb->dev, clkname);
	if (IS_ERR(imx_ldb_ch->clk_pll))
		return PTR_ERR(imx_ldb_ch->clk_pll);

	drm_mode_connector_attach_encoder(&imx_ldb_ch->connector,
			&imx_ldb_ch->encoder);

	imx_ldb_ch->connector.funcs = &imx_ldb_connector_funcs;
	imx_ldb_ch->encoder.funcs = &imx_ldb_encoder_funcs;

	imx_ldb_ch->encoder.possible_crtcs = crtcs;
	imx_ldb_ch->encoder.possible_clones = crtcs;
	imx_ldb_ch->encoder.encoder_type = DRM_MODE_ENCODER_LVDS;
	imx_ldb_ch->connector.connector_type = DRM_MODE_CONNECTOR_LVDS;

	drm_encoder_helper_add(&imx_ldb_ch->encoder,
			&imx_ldb_encoder_helper_funcs);
	ret = imx_drm_add_encoder(&imx_ldb_ch->encoder,
			&imx_ldb_ch->imx_drm_encoder, THIS_MODULE);
	if (ret) {
		dev_err(ldb->dev, "adding encoder failed with %d\n", ret);
		return ret;
	}

	drm_connector_helper_add(&imx_ldb_ch->connector,
			&imx_ldb_connector_helper_funcs);

	ret = imx_drm_add_connector(&imx_ldb_ch->connector,
			&imx_ldb_ch->imx_drm_connector, THIS_MODULE);
	if (ret) {
		imx_drm_remove_encoder(imx_ldb_ch->imx_drm_encoder);
		dev_err(ldb->dev, "adding connector failed with %d\n", ret);
		return ret;
	}

	imx_ldb_ch->connector.encoder = &imx_ldb_ch->encoder;

	return 0;
}

static int __devinit imx_ldb_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const u8 *edidp;
	struct imx_ldb *imx_ldb;
	struct resource *res;
	int ret;
	u32 crtcs[2];
	void *edid[2];
	u32 edid_len[2];
	int i;

	imx_ldb = devm_kzalloc(&pdev->dev, sizeof(*imx_ldb), GFP_KERNEL);
	if (!imx_ldb)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	imx_ldb->base = devm_ioremap(&pdev->dev, res->start,
			resource_size(res));

	edidp = of_get_property(np, "edid_ch0", &edid_len[0]);
	if (edidp)
		edid[0] = kmemdup(edidp, edid_len[0], GFP_KERNEL);

	edidp = of_get_property(np, "edid_ch1", &edid_len[1]);
	if (edidp)
		edid[1] = kmemdup(edidp, edid_len[1], GFP_KERNEL);

	ret = of_property_read_u32_array(np, "crtcs", crtcs, 2);
	if (ret)
		return ret;

	imx_ldb->dev = &pdev->dev;

	for (i = 0; i < 2; i++) {
		struct imx_ldb_channel *channel = &imx_ldb->channel[i];
		channel->ldb = imx_ldb;
		channel->chno = i;

		ret = imx_ldb_register(channel, edid[i], edid_len[i], crtcs[i]);
		if (ret)
			return ret;
	}

	platform_set_drvdata(pdev, imx_ldb);

	return 0;
}

static int __devexit imx_ldb_remove(struct platform_device *pdev)
{
	struct imx_ldb *imx_ldb = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < 2; i++) {
		struct imx_ldb_channel *channel = &imx_ldb->channel[i];
		struct drm_connector *connector = &channel->connector;
		struct drm_encoder *encoder = &channel->encoder;

		drm_mode_connector_detach_encoder(connector, encoder);

		imx_drm_remove_connector(channel->imx_drm_connector);
		imx_drm_remove_encoder(channel->imx_drm_encoder);
	}

	return 0;
}

static const struct of_device_id imx_ldb_dt_ids[] = {
	{ .compatible = "fsl,imx-ldb", .data = NULL, },
	{ /* sentinel */ }
};

static struct platform_driver imx_ldb_driver = {
	.probe		= imx_ldb_probe,
	.remove		= __devexit_p(imx_ldb_remove),
	.driver		= {
		.of_match_table = imx_ldb_dt_ids,
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
};

module_platform_driver(imx_ldb_driver);

MODULE_DESCRIPTION("i.MX LVDS driver");
MODULE_AUTHOR("Sascha Hauer, Pengutronix");
MODULE_LICENSE("GPL");
