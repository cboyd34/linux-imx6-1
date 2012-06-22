/*
 * OF helpers for parsing display modes
 *
 * Copyright (c) 2012 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * This file is released under the GPLv2
 */
#include <linux/of.h>
#include <linux/fb.h>
#include <linux/export.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>

int of_get_video_mode(struct device_node *np, struct drm_display_mode *dmode,
		struct fb_videomode *fbmode)
{
	int ret = 0;
	u32 left_margin, xres, right_margin, hsync_len;
	u32 upper_margin, yres, lower_margin, vsync_len;
	u32 width_mm = 0, height_mm = 0;
	u32 clock;
	bool hah = false, vah = false, interlaced = false, doublescan = false;

	if (!np)
		return -EINVAL;

	ret |= of_property_read_u32(np, "left_margin", &left_margin);
	ret |= of_property_read_u32(np, "xres", &xres);
	ret |= of_property_read_u32(np, "right_margin", &right_margin);
	ret |= of_property_read_u32(np, "hsync_len", &hsync_len);
	ret |= of_property_read_u32(np, "upper_margin", &upper_margin);
	ret |= of_property_read_u32(np, "yres", &yres);
	ret |= of_property_read_u32(np, "lower_margin", &lower_margin);
	ret |= of_property_read_u32(np, "vsync_len", &vsync_len);
	ret |= of_property_read_u32(np, "clock", &clock);
	if (ret)
		return -EINVAL;

	of_property_read_u32(np, "width_mm", &width_mm);
	of_property_read_u32(np, "height_mm", &height_mm);

	hah = of_property_read_bool(np, "hsync_active_high");
	vah = of_property_read_bool(np, "vsync_active_high");
	interlaced = of_property_read_bool(np, "interlaced");
	doublescan = of_property_read_bool(np, "doublescan");

	if (dmode) {
		memset(dmode, 0, sizeof(*dmode));

		dmode->hdisplay = xres;
		dmode->hsync_start = xres + right_margin;
		dmode->hsync_end = xres + right_margin + hsync_len;
		dmode->htotal = xres + right_margin + hsync_len + left_margin;

		dmode->vdisplay = yres;
		dmode->vsync_start = yres + lower_margin;
		dmode->vsync_end = yres + lower_margin + vsync_len;
		dmode->vtotal = yres + lower_margin + vsync_len + upper_margin;

		dmode->width_mm = width_mm;
		dmode->height_mm = height_mm;

		dmode->clock = clock / 1000;

		if (hah)
			dmode->flags |= DRM_MODE_FLAG_PHSYNC;
		else
			dmode->flags |= DRM_MODE_FLAG_NHSYNC;
		if (vah)
			dmode->flags |= DRM_MODE_FLAG_PVSYNC;
		else
			dmode->flags |= DRM_MODE_FLAG_NVSYNC;
		if (interlaced)
			dmode->flags |= DRM_MODE_FLAG_INTERLACE;
		if (doublescan)
			dmode->flags |= DRM_MODE_FLAG_DBLSCAN;

		drm_mode_set_name(dmode);
	}

	if (fbmode) {
		memset(fbmode, 0, sizeof(*fbmode));

		fbmode->xres = xres;
		fbmode->left_margin = left_margin;
		fbmode->right_margin = right_margin;
		fbmode->hsync_len = hsync_len;

		fbmode->yres = yres;
		fbmode->upper_margin = upper_margin;
		fbmode->lower_margin = lower_margin;
		fbmode->vsync_len = vsync_len;

		fbmode->pixclock = KHZ2PICOS(clock / 1000);

		if (hah)
			fbmode->sync |= FB_SYNC_HOR_HIGH_ACT;
		if (vah)
			fbmode->sync |= FB_SYNC_VERT_HIGH_ACT;
		if (interlaced)
			fbmode->vmode |= FB_VMODE_INTERLACED;
		if (doublescan)
			fbmode->vmode |= FB_VMODE_DOUBLE;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(of_get_video_mode);
