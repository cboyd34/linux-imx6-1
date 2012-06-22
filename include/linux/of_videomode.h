/*
 * Copyright 2012 Sascha Hauer <s.hauer@pengutronix.de>
 *
 * OF helpers for videomodes.
 *
 * This file is released under the GPLv2
 */

#ifndef __LINUX_OF_VIDEOMODE_H
#define __LINUX_OF_VIDEOMODE_H

struct device_node;
struct fb_videomode;
struct drm_display_mode;

int of_get_video_mode(struct device_node *np, struct drm_display_mode *dmode,
		struct fb_videomode *fbmode);

#endif /* __LINUX_OF_VIDEOMODE_H */
