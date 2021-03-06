/*
 * Copyright (C) 2011-2012 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */
/*
 * SH-Mobile High-Definition Multimedia Interface (HDMI) driver
 * for SLISHDMI13T and SLIPHDMIT IP cores
 *
 * Copyright (C) 2010, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define IPU_PIX_FMT_RGB24 0 /* FIXME */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/export.h>
#include <linux/module.h>
#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder_slave.h>

#include <linux/console.h>
#include <linux/of_i2c.h>
#include <linux/types.h>

/*
#include "mxc_dispdrv.h"
#include "mxc-hdmi-core.h"
#include "mxc_edid.h"
*/
#include <drm/imx-ipu-v3.h>

#include <mach/mxc_hdmi.h>

#include "imx-drm.h"

#define DISPDRV_HDMI	"hdmi"
#define HDMI_EDID_LEN		512

/* status codes for reading edid */
#define HDMI_EDID_SUCCESS	0
#define HDMI_EDID_FAIL		-1
#define HDMI_EDID_SAME		-2
#define HDMI_EDID_NO_MODES	-3

#define NUM_CEA_VIDEO_MODES	64
#define DEFAULT_VIDEO_MODE	16 /* 1080P */

#define RGB			0
#define YCBCR444		1
#define YCBCR422_16BITS		2
#define YCBCR422_8BITS		3
#define XVYCC444            4

#define MX6Q_IOMUXC_BASE_ADDR  0x020e0000

static void __iomem *hdmi_base;
static void __iomem *iomuxc_base;

#define IOMUXC_IOMUXC_GPR3	0x000c
#define GPR3_HDMI_MUX_CTL_SHIFT	2

static void imx_hdmi_set_ipu_di_mux(int ipu_di)
{
	u32 tmp;

	tmp = readl(iomuxc_base + IOMUXC_IOMUXC_GPR3) & ~0xc;
	writel(ipu_di << GPR3_HDMI_MUX_CTL_SHIFT,
		iomuxc_base + IOMUXC_IOMUXC_GPR3);
}

static inline void hdmi_writeb(u8 val, int offset)
{
	writeb(val, hdmi_base + offset);
}

static inline u8 hdmi_readb(int offset)
{
	return readb(hdmi_base + offset);
}

void hdmi_mask_writeb(u8 data, unsigned int reg, u8 shift, u8 mask)
{
	u8 value = hdmi_readb(reg) & ~mask;
	value |= (data << shift) & mask;
	hdmi_writeb(value, reg);
}

/*
 * We follow a flowchart which is in the "Synopsys DesignWare Courses
 * HDMI Transmitter Controller User Guide, 1.30a", section 3.1
 * (dwc_hdmi_tx_user.pdf)
 *
 * Below are notes that say "HDMI Initialization Step X"
 * These correspond to the flowchart.
 */

enum hdmi_datamap {
	RGB444_8B = 0x01,
	RGB444_10B = 0x03,
	RGB444_12B = 0x05,
	RGB444_16B = 0x07,
	YCbCr444_8B = 0x09,
	YCbCr444_10B = 0x0B,
	YCbCr444_12B = 0x0D,
	YCbCr444_16B = 0x0F,
	YCbCr422_8B = 0x16,
	YCbCr422_10B = 0x14,
	YCbCr422_12B = 0x12,
};

enum hdmi_colorimetry {
	eITU601,
	eITU709,
};

struct hdmi_vmode {
	bool mDVI;
	bool mHSyncPolarity;
	bool mVSyncPolarity;
	bool mInterlaced;
	bool mDataEnablePolarity;

	unsigned int mPixelClock;
	unsigned int mPixelRepetitionInput;
	unsigned int mPixelRepetitionOutput;
};

struct hdmi_data_info {
	unsigned int enc_in_format;
	unsigned int enc_out_format;
	unsigned int enc_color_depth;
	unsigned int colorimetry;
	unsigned int pix_repet_factor;
	unsigned int hdcp_enable;
	struct hdmi_vmode video_mode;
};

struct imx_hdmi {
        struct drm_connector connector;
        struct imx_drm_connector *imx_drm_connector;
        struct drm_encoder encoder;
        struct imx_drm_encoder *imx_drm_encoder;

	struct device *dev;
	struct clk *hdmi_isfr_clk;
	struct clk *hdmi_iahb_clk;

	struct hdmi_data_info hdmi_data;
	int vic;
#if 0
	struct mxc_edid_cfg edid_cfg;
#endif
	u8 edid[HDMI_EDID_LEN];
	bool fb_reg;
	bool cable_plugin;
	bool dft_mode_set;
	char *dft_mode_str;
	int default_bpp;
	u8 latest_intr_stat;
	bool irq_enabled;
	spinlock_t irq_lock;
	bool phy_enabled;
	struct drm_display_mode previous_mode;
	struct fb_videomode previous_non_vga_mode;
	bool requesting_vga_for_initialization;

	struct i2c_adapter *ddc;
	void __iomem *regs;

	struct fb_videomode fb_mode;
};

static unsigned long pixel_clk_rate = 74250000;
static unsigned int sample_rate = 48000;
static int hdmi_ratio = 100;
struct clk *pixel_clk;

static void hdmi_set_clock_regenerator_n(unsigned int value)
{
	u8 val;

	hdmi_writeb(value & 0xff, HDMI_AUD_N1);
	hdmi_writeb((value >> 8) & 0xff, HDMI_AUD_N2);
	hdmi_writeb((value >> 16) & 0x0f, HDMI_AUD_N3);

	/* nshift factor = 0 */
	val = hdmi_readb(HDMI_AUD_CTS3);
	val &= ~HDMI_AUD_CTS3_N_SHIFT_MASK;
	hdmi_writeb(val, HDMI_AUD_CTS3);
}

static void hdmi_set_clock_regenerator_cts(unsigned int cts)
{
	u8 val;

	/* Must be set/cleared first */
	val = hdmi_readb(HDMI_AUD_CTS3);
	val &= ~HDMI_AUD_CTS3_CTS_MANUAL;
	hdmi_writeb(val, HDMI_AUD_CTS3);

	hdmi_writeb(cts & 0xff, HDMI_AUD_CTS1);
	hdmi_writeb((cts >> 8) & 0xff, HDMI_AUD_CTS2);
	hdmi_writeb(((cts >> 16) & HDMI_AUD_CTS3_AUDCTS19_16_MASK) |
		    HDMI_AUD_CTS3_CTS_MANUAL, HDMI_AUD_CTS3);
}

static unsigned int hdmi_compute_n(unsigned int freq, unsigned long pixel_clk,
				   unsigned int ratio)
{
	unsigned int n = (128 * freq) / 1000;

	switch (freq) {
	case 32000:
		if (pixel_clk == 25170000)
			n = (ratio == 150) ? 9152 : 4576;
		else if (pixel_clk == 27020000)
			n = (ratio == 150) ? 8192 : 4096;
		else if (pixel_clk == 74170000 || pixel_clk == 148350000)
			n = 11648;
		else
			n = 4096;
		break;

	case 44100:
		if (pixel_clk == 25170000)
			n = 7007;
		else if (pixel_clk == 74170000)
			n = 17836;
		else if (pixel_clk == 148350000)
			n = (ratio == 150) ? 17836 : 8918;
		else
			n = 6272;
		break;

	case 48000:
		if (pixel_clk == 25170000)
			n = (ratio == 150) ? 9152 : 6864;
		else if (pixel_clk == 27020000)
			n = (ratio == 150) ? 8192 : 6144;
		else if (pixel_clk == 74170000)
			n = 11648;
		else if (pixel_clk == 148350000)
			n = (ratio == 150) ? 11648 : 5824;
		else
			n = 6144;
		break;

	case 88200:
		n = hdmi_compute_n(44100, pixel_clk, ratio) * 2;
		break;

	case 96000:
		n = hdmi_compute_n(48000, pixel_clk, ratio) * 2;
		break;

	case 176400:
		n = hdmi_compute_n(44100, pixel_clk, ratio) * 4;
		break;

	case 192000:
		n = hdmi_compute_n(48000, pixel_clk, ratio) * 4;
		break;

	default:
		break;
	}

	return n;
}

static unsigned int hdmi_compute_cts(unsigned int freq, unsigned long pixel_clk,
				     unsigned int ratio)
{
	unsigned int cts = 0;

	printk("%s: freq: %d pixel_clk: %ld ratio: %d\n", __func__, freq,
			pixel_clk, ratio);

	switch (freq) {
	case 32000:
		if (pixel_clk == 297000000) {
			cts = 222750;
			break;
		}
	case 48000:
	case 96000:
	case 192000:
		switch (pixel_clk) {
		case 25200000:
		case 27000000:
		case 54000000:
		case 74250000:
		case 148500000:
			cts = pixel_clk / 1000;
			break;
		case 297000000:
			cts = 247500;
			break;
		/*
		 * All other TMDS clocks are not supported by
		 * DWC_hdmi_tx. The TMDS clocks divided or
		 * multiplied by 1,001 coefficients are not
		 * supported.
		 */
		default:
			break;
		}
		break;
	case 44100:
	case 88200:
	case 176400:
		switch (pixel_clk) {
		case 25200000:
			cts = 28000;
			break;
		case 27000000:
			cts = 30000;
			break;
		case 54000000:
			cts = 60000;
			break;
		case 74250000:
			cts = 82500;
			break;
		case 148500000:
			cts = 165000;
			break;
		case 297000000:
			cts = 247500;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
	if (ratio == 100)
		return cts;
	else
		return (cts * ratio) / 100;
}

static void hdmi_get_pixel_clk(void)
{
	unsigned long rate;

	rate = 65000000; /* FIXME */

	if (rate != 0)
		pixel_clk_rate = rate;
}

static void hdmi_set_clk_regenerator(void)
{
	unsigned int clk_n, clk_cts;

	clk_n = hdmi_compute_n(sample_rate, pixel_clk_rate, hdmi_ratio);
	clk_cts = hdmi_compute_cts(sample_rate, pixel_clk_rate, hdmi_ratio);

	if (clk_cts == 0) {
		pr_debug("%s: pixel clock not supported: %d\n",
			__func__, (int)pixel_clk_rate);
		return;
	}

	pr_debug("%s: samplerate=%d  ratio=%d  pixelclk=%d  N=%d  cts=%d\n",
		__func__, sample_rate, hdmi_ratio, (int)pixel_clk_rate,
		clk_n, clk_cts);

	hdmi_set_clock_regenerator_n(clk_n);
	hdmi_set_clock_regenerator_cts(clk_cts);
}

static void hdmi_init_clk_regenerator(struct imx_hdmi *hdmi)
{
	unsigned int clk_n, clk_cts;

	clk_n = hdmi_compute_n(sample_rate, pixel_clk_rate, hdmi_ratio);
	clk_cts = hdmi_compute_cts(sample_rate, pixel_clk_rate, hdmi_ratio);

	if (clk_cts == 0) {
		dev_info(hdmi->dev, "%s: pixel clock not supported: %d\n",
			__func__, (int)pixel_clk_rate);
		return;
	}

	dev_info(hdmi->dev, "%s: samplerate=%d  ratio=%d  pixelclk=%d  N=%d  cts=%d\n",
		__func__, sample_rate, hdmi_ratio, (int)pixel_clk_rate,
		clk_n, clk_cts);

	hdmi_set_clock_regenerator_n(clk_n);
	hdmi_set_clock_regenerator_cts(clk_cts);
}

void hdmi_clk_regenerator_update_pixel_clock(void)
{
	/* Get pixel clock from ipu */
	hdmi_get_pixel_clk();
	hdmi_set_clk_regenerator();
}

extern const struct fb_videomode mxc_cea_mode[64];

static void dump_fb_videomode(struct fb_videomode *m)
{
	pr_debug("fb_videomode = %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
		m->refresh, m->xres, m->yres, m->pixclock, m->left_margin,
		m->right_margin, m->upper_margin, m->lower_margin,
		m->hsync_len, m->vsync_len, m->sync, m->vmode, m->flag);
}

/*!
 * this submodule is responsible for the video data synchronization.
 * for example, for RGB 4:4:4 input, the data map is defined as
 *			pin{47~40} <==> R[7:0]
 *			pin{31~24} <==> G[7:0]
 *			pin{15~8}  <==> B[7:0]
 */
static void hdmi_video_sample(struct imx_hdmi *hdmi)
{
	int color_format = 0;
	u8 val;

	if (hdmi->hdmi_data.enc_in_format == RGB) {
		if (hdmi->hdmi_data.enc_color_depth == 8)
			color_format = 0x01;
		else if (hdmi->hdmi_data.enc_color_depth == 10)
			color_format = 0x03;
		else if (hdmi->hdmi_data.enc_color_depth == 12)
			color_format = 0x05;
		else if (hdmi->hdmi_data.enc_color_depth == 16)
			color_format = 0x07;
		else
			return;
	} else if (hdmi->hdmi_data.enc_in_format == YCBCR444) {
		if (hdmi->hdmi_data.enc_color_depth == 8)
			color_format = 0x09;
		else if (hdmi->hdmi_data.enc_color_depth == 10)
			color_format = 0x0B;
		else if (hdmi->hdmi_data.enc_color_depth == 12)
			color_format = 0x0D;
		else if (hdmi->hdmi_data.enc_color_depth == 16)
			color_format = 0x0F;
		else
			return;
	} else if (hdmi->hdmi_data.enc_in_format == YCBCR422_8BITS) {
		if (hdmi->hdmi_data.enc_color_depth == 8)
			color_format = 0x16;
		else if (hdmi->hdmi_data.enc_color_depth == 10)
			color_format = 0x14;
		else if (hdmi->hdmi_data.enc_color_depth == 12)
			color_format = 0x12;
		else
			return;
	}

	val = HDMI_TX_INVID0_INTERNAL_DE_GENERATOR_DISABLE |
		((color_format << HDMI_TX_INVID0_VIDEO_MAPPING_OFFSET) &
		HDMI_TX_INVID0_VIDEO_MAPPING_MASK);
	hdmi_writeb(val, HDMI_TX_INVID0);

	/* Enable TX stuffing: When DE is inactive, fix the output data to 0 */
	val = HDMI_TX_INSTUFFING_BDBDATA_STUFFING_ENABLE |
		HDMI_TX_INSTUFFING_RCRDATA_STUFFING_ENABLE |
		HDMI_TX_INSTUFFING_GYDATA_STUFFING_ENABLE;
	hdmi_writeb(val, HDMI_TX_INSTUFFING);
	hdmi_writeb(0x0, HDMI_TX_GYDATA0);
	hdmi_writeb(0x0, HDMI_TX_GYDATA1);
	hdmi_writeb(0x0, HDMI_TX_RCRDATA0);
	hdmi_writeb(0x0, HDMI_TX_RCRDATA1);
	hdmi_writeb(0x0, HDMI_TX_BCBDATA0);
	hdmi_writeb(0x0, HDMI_TX_BCBDATA1);
}

static int isColorSpaceConversion(struct imx_hdmi *hdmi)
{
	return (hdmi->hdmi_data.enc_in_format !=
		hdmi->hdmi_data.enc_out_format);
}

static int isColorSpaceDecimation(struct imx_hdmi *hdmi)
{
	return ((hdmi->hdmi_data.enc_out_format == YCBCR422_8BITS) &&
		(hdmi->hdmi_data.enc_in_format == RGB ||
		hdmi->hdmi_data.enc_in_format == YCBCR444));
}

static int isColorSpaceInterpolation(struct imx_hdmi *hdmi)
{
	return ((hdmi->hdmi_data.enc_in_format == YCBCR422_8BITS) &&
		(hdmi->hdmi_data.enc_out_format == RGB
		|| hdmi->hdmi_data.enc_out_format == YCBCR444));
}

/*!
 * update the color space conversion coefficients.
 */
static void update_csc_coeffs(struct imx_hdmi *hdmi)
{
	unsigned short csc_coeff[3][4];
	unsigned int csc_scale = 1;
	u8 val;
	bool coeff_selected = false;

	if (isColorSpaceConversion(hdmi)) { /* csc needed */
		if (hdmi->hdmi_data.enc_out_format == RGB) {
			if (hdmi->hdmi_data.colorimetry == eITU601) {
				csc_coeff[0][0] = 0x2000;
				csc_coeff[0][1] = 0x6926;
				csc_coeff[0][2] = 0x74fd;
				csc_coeff[0][3] = 0x010e;

				csc_coeff[1][0] = 0x2000;
				csc_coeff[1][1] = 0x2cdd;
				csc_coeff[1][2] = 0x0000;
				csc_coeff[1][3] = 0x7e9a;

				csc_coeff[2][0] = 0x2000;
				csc_coeff[2][1] = 0x0000;
				csc_coeff[2][2] = 0x38b4;
				csc_coeff[2][3] = 0x7e3b;

				csc_scale = 1;
				coeff_selected = true;
			} else if (hdmi->hdmi_data.colorimetry == eITU709) {
				csc_coeff[0][0] = 0x2000;
				csc_coeff[0][1] = 0x7106;
				csc_coeff[0][2] = 0x7a02;
				csc_coeff[0][3] = 0x00a7;

				csc_coeff[1][0] = 0x2000;
				csc_coeff[1][1] = 0x3264;
				csc_coeff[1][2] = 0x0000;
				csc_coeff[1][3] = 0x7e6d;

				csc_coeff[2][0] = 0x2000;
				csc_coeff[2][1] = 0x0000;
				csc_coeff[2][2] = 0x3b61;
				csc_coeff[2][3] = 0x7e25;

				csc_scale = 1;
				coeff_selected = true;
			}
		} else if (hdmi->hdmi_data.enc_in_format == RGB) {
			if (hdmi->hdmi_data.colorimetry == eITU601) {
				csc_coeff[0][0] = 0x2591;
				csc_coeff[0][1] = 0x1322;
				csc_coeff[0][2] = 0x074b;
				csc_coeff[0][3] = 0x0000;

				csc_coeff[1][0] = 0x6535;
				csc_coeff[1][1] = 0x2000;
				csc_coeff[1][2] = 0x7acc;
				csc_coeff[1][3] = 0x0200;

				csc_coeff[2][0] = 0x6acd;
				csc_coeff[2][1] = 0x7534;
				csc_coeff[2][2] = 0x2000;
				csc_coeff[2][3] = 0x0200;

				csc_scale = 0;
				coeff_selected = true;
			} else if (hdmi->hdmi_data.colorimetry == eITU709) {
				csc_coeff[0][0] = 0x2dc5;
				csc_coeff[0][1] = 0x0d9b;
				csc_coeff[0][2] = 0x049e;
				csc_coeff[0][3] = 0x0000;

				csc_coeff[1][0] = 0x62f0;
				csc_coeff[1][1] = 0x2000;
				csc_coeff[1][2] = 0x7d11;
				csc_coeff[1][3] = 0x0200;

				csc_coeff[2][0] = 0x6756;
				csc_coeff[2][1] = 0x78ab;
				csc_coeff[2][2] = 0x2000;
				csc_coeff[2][3] = 0x0200;

				csc_scale = 0;
				coeff_selected = true;
			}
		}
	}

	if (!coeff_selected) {
		csc_coeff[0][0] = 0x2000;
		csc_coeff[0][1] = 0x0000;
		csc_coeff[0][2] = 0x0000;
		csc_coeff[0][3] = 0x0000;

		csc_coeff[1][0] = 0x0000;
		csc_coeff[1][1] = 0x2000;
		csc_coeff[1][2] = 0x0000;
		csc_coeff[1][3] = 0x0000;

		csc_coeff[2][0] = 0x0000;
		csc_coeff[2][1] = 0x0000;
		csc_coeff[2][2] = 0x2000;
		csc_coeff[2][3] = 0x0000;

		csc_scale = 1;
	}

	/* Update CSC parameters in HDMI CSC registers */
	hdmi_writeb((unsigned char)(csc_coeff[0][0] & 0xFF),
		HDMI_CSC_COEF_A1_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][0] >> 8),
		HDMI_CSC_COEF_A1_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][1] & 0xFF),
		HDMI_CSC_COEF_A2_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][1] >> 8),
		HDMI_CSC_COEF_A2_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][2] & 0xFF),
		HDMI_CSC_COEF_A3_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][2] >> 8),
		HDMI_CSC_COEF_A3_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][3] & 0xFF),
		HDMI_CSC_COEF_A4_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[0][3] >> 8),
		HDMI_CSC_COEF_A4_MSB);

	hdmi_writeb((unsigned char)(csc_coeff[1][0] & 0xFF),
		HDMI_CSC_COEF_B1_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][0] >> 8),
		HDMI_CSC_COEF_B1_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][1] & 0xFF),
		HDMI_CSC_COEF_B2_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][1] >> 8),
		HDMI_CSC_COEF_B2_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][2] & 0xFF),
		HDMI_CSC_COEF_B3_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][2] >> 8),
		HDMI_CSC_COEF_B3_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][3] & 0xFF),
		HDMI_CSC_COEF_B4_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[1][3] >> 8),
		HDMI_CSC_COEF_B4_MSB);

	hdmi_writeb((unsigned char)(csc_coeff[2][0] & 0xFF),
		HDMI_CSC_COEF_C1_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][0] >> 8),
		HDMI_CSC_COEF_C1_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][1] & 0xFF),
		HDMI_CSC_COEF_C2_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][1] >> 8),
		HDMI_CSC_COEF_C2_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][2] & 0xFF),
		HDMI_CSC_COEF_C3_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][2] >> 8),
		HDMI_CSC_COEF_C3_MSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][3] & 0xFF),
		HDMI_CSC_COEF_C4_LSB);
	hdmi_writeb((unsigned char)(csc_coeff[2][3] >> 8),
		HDMI_CSC_COEF_C4_MSB);

	val = hdmi_readb(HDMI_CSC_SCALE);
	val &= ~HDMI_CSC_SCALE_CSCSCALE_MASK;
	val |= csc_scale & HDMI_CSC_SCALE_CSCSCALE_MASK;
	hdmi_writeb(val, HDMI_CSC_SCALE);
}

static void hdmi_video_csc(struct imx_hdmi *hdmi)
{
	int color_depth = 0;
	int interpolation = HDMI_CSC_CFG_INTMODE_DISABLE;
	int decimation = 0;
	u8 val;

	/* YCC422 interpolation to 444 mode */
	if (isColorSpaceInterpolation(hdmi))
		interpolation = HDMI_CSC_CFG_INTMODE_CHROMA_INT_FORMULA1;
	else if (isColorSpaceDecimation(hdmi))
		decimation = HDMI_CSC_CFG_DECMODE_CHROMA_INT_FORMULA3;

	if (hdmi->hdmi_data.enc_color_depth == 8)
		color_depth = HDMI_CSC_SCALE_CSC_COLORDE_PTH_24BPP;
	else if (hdmi->hdmi_data.enc_color_depth == 10)
		color_depth = HDMI_CSC_SCALE_CSC_COLORDE_PTH_30BPP;
	else if (hdmi->hdmi_data.enc_color_depth == 12)
		color_depth = HDMI_CSC_SCALE_CSC_COLORDE_PTH_36BPP;
	else if (hdmi->hdmi_data.enc_color_depth == 16)
		color_depth = HDMI_CSC_SCALE_CSC_COLORDE_PTH_48BPP;
	else
		return;

	/*configure the CSC registers */
	hdmi_writeb(interpolation | decimation, HDMI_CSC_CFG);
	val = hdmi_readb(HDMI_CSC_SCALE);
	val &= ~HDMI_CSC_SCALE_CSC_COLORDE_PTH_MASK;
	val |= color_depth;
	hdmi_writeb(val, HDMI_CSC_SCALE);

	update_csc_coeffs(hdmi);
}

/*!
 * HDMI video packetizer is used to packetize the data.
 * for example, if input is YCC422 mode or repeater is used,
 * data should be repacked this module can be bypassed.
 */
static void hdmi_video_packetize(struct imx_hdmi *hdmi)
{
	unsigned int color_depth = 0;
	unsigned int remap_size = HDMI_VP_REMAP_YCC422_16bit;
	unsigned int output_select = HDMI_VP_CONF_OUTPUT_SELECTOR_PP;
	struct hdmi_data_info *hdmi_data = &hdmi->hdmi_data;
	u8 val;

	if (hdmi_data->enc_out_format == RGB
		|| hdmi_data->enc_out_format == YCBCR444) {
		if (hdmi_data->enc_color_depth == 0)
			output_select = HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS;
		else if (hdmi_data->enc_color_depth == 8) {
			color_depth = 4;
			output_select = HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS;
		} else if (hdmi_data->enc_color_depth == 10)
			color_depth = 5;
		else if (hdmi_data->enc_color_depth == 12)
			color_depth = 6;
		else if (hdmi_data->enc_color_depth == 16)
			color_depth = 7;
		else
			return;
	} else if (hdmi_data->enc_out_format == YCBCR422_8BITS) {
		if (hdmi_data->enc_color_depth == 0 ||
			hdmi_data->enc_color_depth == 8)
			remap_size = HDMI_VP_REMAP_YCC422_16bit;
		else if (hdmi_data->enc_color_depth == 10)
			remap_size = HDMI_VP_REMAP_YCC422_20bit;
		else if (hdmi_data->enc_color_depth == 12)
			remap_size = HDMI_VP_REMAP_YCC422_24bit;
		else
			return;
		output_select = HDMI_VP_CONF_OUTPUT_SELECTOR_YCC422;
	} else
		return;

	/* set the packetizer registers */
	val = ((color_depth << HDMI_VP_PR_CD_COLOR_DEPTH_OFFSET) &
		HDMI_VP_PR_CD_COLOR_DEPTH_MASK) |
		((hdmi_data->pix_repet_factor <<
		HDMI_VP_PR_CD_DESIRED_PR_FACTOR_OFFSET) &
		HDMI_VP_PR_CD_DESIRED_PR_FACTOR_MASK);
	hdmi_writeb(val, HDMI_VP_PR_CD);

	val = hdmi_readb(HDMI_VP_STUFF);
	val &= ~HDMI_VP_STUFF_PR_STUFFING_MASK;
	val |= HDMI_VP_STUFF_PR_STUFFING_STUFFING_MODE;
	hdmi_writeb(val, HDMI_VP_STUFF);

	/* Data from pixel repeater block */
	if (hdmi_data->pix_repet_factor > 1) {
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_PR_EN_MASK |
			HDMI_VP_CONF_BYPASS_SELECT_MASK);
		val |= HDMI_VP_CONF_PR_EN_ENABLE |
			HDMI_VP_CONF_BYPASS_SELECT_PIX_REPEATER;
		hdmi_writeb(val, HDMI_VP_CONF);
	} else { /* data from packetizer block */
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_PR_EN_MASK |
			HDMI_VP_CONF_BYPASS_SELECT_MASK);
		val |= HDMI_VP_CONF_PR_EN_DISABLE |
			HDMI_VP_CONF_BYPASS_SELECT_VID_PACKETIZER;
		hdmi_writeb(val, HDMI_VP_CONF);
	}

	val = hdmi_readb(HDMI_VP_STUFF);
	val &= ~HDMI_VP_STUFF_IDEFAULT_PHASE_MASK;
	val |= 1 << HDMI_VP_STUFF_IDEFAULT_PHASE_OFFSET;
	hdmi_writeb(val, HDMI_VP_STUFF);

	hdmi_writeb(remap_size, HDMI_VP_REMAP);

	if (output_select == HDMI_VP_CONF_OUTPUT_SELECTOR_PP) {
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_BYPASS_EN_MASK |
			HDMI_VP_CONF_PP_EN_ENMASK |
			HDMI_VP_CONF_YCC422_EN_MASK);
		val |= HDMI_VP_CONF_BYPASS_EN_DISABLE |
			HDMI_VP_CONF_PP_EN_ENABLE |
			HDMI_VP_CONF_YCC422_EN_DISABLE;
		hdmi_writeb(val, HDMI_VP_CONF);
	} else if (output_select == HDMI_VP_CONF_OUTPUT_SELECTOR_YCC422) {
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_BYPASS_EN_MASK |
			HDMI_VP_CONF_PP_EN_ENMASK |
			HDMI_VP_CONF_YCC422_EN_MASK);
		val |= HDMI_VP_CONF_BYPASS_EN_DISABLE |
			HDMI_VP_CONF_PP_EN_DISABLE |
			HDMI_VP_CONF_YCC422_EN_ENABLE;
		hdmi_writeb(val, HDMI_VP_CONF);
	} else if (output_select == HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS) {
		val = hdmi_readb(HDMI_VP_CONF);
		val &= ~(HDMI_VP_CONF_BYPASS_EN_MASK |
			HDMI_VP_CONF_PP_EN_ENMASK |
			HDMI_VP_CONF_YCC422_EN_MASK);
		val |= HDMI_VP_CONF_BYPASS_EN_ENABLE |
			HDMI_VP_CONF_PP_EN_DISABLE |
			HDMI_VP_CONF_YCC422_EN_DISABLE;
		hdmi_writeb(val, HDMI_VP_CONF);
	} else {
		return;
	}

	val = hdmi_readb(HDMI_VP_STUFF);
	val &= ~(HDMI_VP_STUFF_PP_STUFFING_MASK |
		HDMI_VP_STUFF_YCC422_STUFFING_MASK);
	val |= HDMI_VP_STUFF_PP_STUFFING_STUFFING_MODE |
		HDMI_VP_STUFF_YCC422_STUFFING_STUFFING_MODE;
	hdmi_writeb(val, HDMI_VP_STUFF);

	val = hdmi_readb(HDMI_VP_CONF);
	val &= ~HDMI_VP_CONF_OUTPUT_SELECTOR_MASK;
	val |= output_select;
	hdmi_writeb(val, HDMI_VP_CONF);
}

#if 0
/* Force a fixed color screen */
static void hdmi_video_force_output(struct imx_hdmi *hdmi, unsigned char force)
{
	u8 val;

	dev_dbg(hdmi->dev, "%s\n", __func__);

	if (force) {
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS2);   /* R */
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS1);   /* G */
		hdmi_writeb(0xFF, HDMI_FC_DBGTMDS0);   /* B */
		val = hdmi_readb(HDMI_FC_DBGFORCE);
		val |= HDMI_FC_DBGFORCE_FORCEVIDEO;
		hdmi_writeb(val, HDMI_FC_DBGFORCE);
	} else {
		val = hdmi_readb(HDMI_FC_DBGFORCE);
		val &= ~HDMI_FC_DBGFORCE_FORCEVIDEO;
		hdmi_writeb(val, HDMI_FC_DBGFORCE);
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS2);   /* R */
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS1);   /* G */
		hdmi_writeb(0x00, HDMI_FC_DBGTMDS0);   /* B */
	}
}
#endif

static inline void hdmi_phy_test_clear(struct imx_hdmi *hdmi,
						unsigned char bit)
{
	u8 val = hdmi_readb(HDMI_PHY_TST0);
	val &= ~HDMI_PHY_TST0_TSTCLR_MASK;
	val |= (bit << HDMI_PHY_TST0_TSTCLR_OFFSET) &
		HDMI_PHY_TST0_TSTCLR_MASK;
	hdmi_writeb(val, HDMI_PHY_TST0);
}

static inline void hdmi_phy_test_enable(struct imx_hdmi *hdmi,
						unsigned char bit)
{
	u8 val = hdmi_readb(HDMI_PHY_TST0);
	val &= ~HDMI_PHY_TST0_TSTEN_MASK;
	val |= (bit << HDMI_PHY_TST0_TSTEN_OFFSET) &
		HDMI_PHY_TST0_TSTEN_MASK;
	hdmi_writeb(val, HDMI_PHY_TST0);
}

static inline void hdmi_phy_test_clock(struct imx_hdmi *hdmi,
						unsigned char bit)
{
	u8 val = hdmi_readb(HDMI_PHY_TST0);
	val &= ~HDMI_PHY_TST0_TSTCLK_MASK;
	val |= (bit << HDMI_PHY_TST0_TSTCLK_OFFSET) &
		HDMI_PHY_TST0_TSTCLK_MASK;
	hdmi_writeb(val, HDMI_PHY_TST0);
}

static inline void hdmi_phy_test_din(struct imx_hdmi *hdmi,
						unsigned char bit)
{
	hdmi_writeb(bit, HDMI_PHY_TST1);
}

static inline void hdmi_phy_test_dout(struct imx_hdmi *hdmi,
						unsigned char bit)
{
	hdmi_writeb(bit, HDMI_PHY_TST2);
}

static bool hdmi_phy_wait_i2c_done(struct imx_hdmi *hdmi, int msec)
{
	unsigned char val = 0;
	val = hdmi_readb(HDMI_IH_I2CMPHY_STAT0) & 0x3;
	while (val == 0) {
		udelay(1000);
		if (msec-- == 0)
			return false;
		val = hdmi_readb(HDMI_IH_I2CMPHY_STAT0) & 0x3;
	}
	return true;
}

static void __hdmi_phy_i2c_write(struct imx_hdmi *hdmi, unsigned short data,
			      unsigned char addr)
{
	hdmi_writeb(0xFF, HDMI_IH_I2CMPHY_STAT0);
	hdmi_writeb(addr, HDMI_PHY_I2CM_ADDRESS_ADDR);
	hdmi_writeb((unsigned char)(data >> 8),
		HDMI_PHY_I2CM_DATAO_1_ADDR);
	hdmi_writeb((unsigned char)(data >> 0),
		HDMI_PHY_I2CM_DATAO_0_ADDR);
	hdmi_writeb(HDMI_PHY_I2CM_OPERATION_ADDR_WRITE,
		HDMI_PHY_I2CM_OPERATION_ADDR);
	hdmi_phy_wait_i2c_done(hdmi, 1000);
}

static unsigned short hdmi_phy_i2c_read(struct imx_hdmi *hdmi,
					unsigned char addr)
{
	unsigned short data;
	unsigned char msb = 0, lsb = 0;
	hdmi_writeb(0xFF, HDMI_IH_I2CMPHY_STAT0);
	hdmi_writeb(addr, HDMI_PHY_I2CM_ADDRESS_ADDR);
	hdmi_writeb(HDMI_PHY_I2CM_OPERATION_ADDR_READ,
		HDMI_PHY_I2CM_OPERATION_ADDR);
	hdmi_phy_wait_i2c_done(hdmi, 1000);
	msb = hdmi_readb(HDMI_PHY_I2CM_DATAI_1_ADDR);
	lsb = hdmi_readb(HDMI_PHY_I2CM_DATAI_0_ADDR);
	data = (msb << 8) | lsb;
	return data;
}

static int hdmi_phy_i2c_write(struct imx_hdmi *hdmi, unsigned short data,
				     unsigned char addr)
{
	unsigned short val = 0;
	__hdmi_phy_i2c_write(hdmi, data, addr);
	return 0;
	val = hdmi_phy_i2c_read(hdmi, addr);
	if (val != data)
		printk("hdmi i2c write: addr: 0x%08x 0x%04x != 0x%04x\n",
				addr, data, val);
	return (val == data);
}

/* "Power-down enable (active low)"
 * That mean that power up == 1! */
static void imx_hdmi_phy_enable_power(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_PDZ_OFFSET,
			HDMI_PHY_CONF0_PDZ_MASK);
}

static void imx_hdmi_phy_enable_tmds(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_ENTMDS_OFFSET,
			HDMI_PHY_CONF0_ENTMDS_MASK);
}

static void imx_hdmi_phy_gen2_pddq(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_GEN2_PDDQ_OFFSET,
			HDMI_PHY_CONF0_GEN2_PDDQ_MASK);
}

static void imx_hdmi_phy_gen2_txpwron(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_GEN2_TXPWRON_OFFSET,
			HDMI_PHY_CONF0_GEN2_TXPWRON_MASK);
}

#if 0
static void imx_hdmi_phy_gen2_enhpdrxsense(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_GEN2_ENHPDRXSENSE_OFFSET,
			HDMI_PHY_CONF0_GEN2_ENHPDRXSENSE_MASK);
}
#endif

static void imx_hdmi_phy_sel_data_en_pol(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_SELDATAENPOL_OFFSET,
			HDMI_PHY_CONF0_SELDATAENPOL_MASK);
}

static void imx_hdmi_phy_sel_interface_control(u8 enable)
{
	hdmi_mask_writeb(enable, HDMI_PHY_CONF0,
			HDMI_PHY_CONF0_SELDIPIF_OFFSET,
			HDMI_PHY_CONF0_SELDIPIF_MASK);
}

static int hdmi_phy_configure(struct imx_hdmi *hdmi, unsigned char pRep,
			      unsigned char cRes, int cscOn)
{
	u8 val;
	u8 msec;

	dev_info(hdmi->dev, "%s\n", __func__);

	/* color resolution 0 is 8 bit colour depth */
	if (cRes == 0)
		cRes = 8;

	if (pRep != 0)
		return -EINVAL;
	else if (cRes != 8 && cRes != 12)
		return -EINVAL;

	/* Enable csc path */
	if (cscOn)
		val = HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_IN_PATH;
	else
		val = HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_BYPASS;

	hdmi_writeb(val, HDMI_MC_FLOWCTRL);

	/* gen2 tx power off */
	imx_hdmi_phy_gen2_txpwron(0);

	/* gen2 pddq */
	imx_hdmi_phy_gen2_pddq(1);

	/* PHY reset */
	hdmi_writeb(HDMI_MC_PHYRSTZ_DEASSERT, HDMI_MC_PHYRSTZ);
	hdmi_writeb(HDMI_MC_PHYRSTZ_ASSERT, HDMI_MC_PHYRSTZ);

	hdmi_writeb(HDMI_MC_HEACPHY_RST_ASSERT, HDMI_MC_HEACPHY_RST);

	hdmi_phy_test_clear(hdmi, 1);
	hdmi_writeb(HDMI_PHY_I2CM_SLAVE_ADDR_PHY_GEN2,
			HDMI_PHY_I2CM_SLAVE_ADDR);
	hdmi_phy_test_clear(hdmi, 0);

	if (hdmi->hdmi_data.video_mode.mPixelClock < 0) {
		dev_dbg(hdmi->dev, "Pixel clock (%d) must be positive\n",
			hdmi->hdmi_data.video_mode.mPixelClock);
		return -EINVAL;
	}

	if (hdmi->hdmi_data.video_mode.mPixelClock <= 45250000) {
		switch (cRes) {
		case 8:
			/* PLL/MPLL Cfg */
			hdmi_phy_i2c_write(hdmi, 0x01e0, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0000, 0x15);  /* GMPCTRL */
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x21e1, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0000, 0x15);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x41e2, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0000, 0x15);
			break;
		default:
			return -EINVAL;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 92500000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x0140, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0005, 0x15);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x2141, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0005, 0x15);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x4142, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x0005, 0x15);
		default:
			return -EINVAL;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 148500000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x00a0, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000a, 0x15);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x20a1, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000a, 0x15);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x40a2, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000a, 0x15);
		default:
			return -EINVAL;
		}
	} else {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x00a0, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000a, 0x15);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x2001, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000f, 0x15);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x4002, 0x06);
			hdmi_phy_i2c_write(hdmi, 0x000f, 0x15);
		default:
			return -EINVAL;
		}
	}

	if (hdmi->hdmi_data.video_mode.mPixelClock <= 54000000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);  /* CURRCTRL */
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		default:
			return -EINVAL;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 58400000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		default:
			return -EINVAL;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 72000000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		default:
			return -EINVAL;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 74250000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x0b5c, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		default:
			return -EINVAL;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 118800000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		default:
			return -EINVAL;
		}
	} else if (hdmi->hdmi_data.video_mode.mPixelClock <= 216000000) {
		switch (cRes) {
		case 8:
			hdmi_phy_i2c_write(hdmi, 0x06dc, 0x10);
			break;
		case 10:
			hdmi_phy_i2c_write(hdmi, 0x0b5c, 0x10);
			break;
		case 12:
			hdmi_phy_i2c_write(hdmi, 0x091c, 0x10);
			break;
		default:
			return -EINVAL;
		}
	} else {
		dev_err(hdmi->dev,
				"Pixel clock %d - unsupported by HDMI\n",
				hdmi->hdmi_data.video_mode.mPixelClock);
		return -EINVAL;
	}

	hdmi_phy_i2c_write(hdmi, 0x0000, 0x13);  /* PLLPHBYCTRL */
	hdmi_phy_i2c_write(hdmi, 0x0006, 0x17);
	/* RESISTANCE TERM 133Ohm Cfg */
	hdmi_phy_i2c_write(hdmi, 0x0005, 0x19);  /* TXTERM */
	/* PREEMP Cgf 0.00 */
	hdmi_phy_i2c_write(hdmi, 0x8009, 0x09);  /* CKSYMTXCTRL */
	/* TX/CK LVL 10 */
	hdmi_phy_i2c_write(hdmi, 0x0210, 0x0E);  /* VLEVCTRL */
	/* REMOVE CLK TERM */
	hdmi_phy_i2c_write(hdmi, 0x8000, 0x05);  /* CKCALCTRL */

	if (hdmi->hdmi_data.video_mode.mPixelClock > 148500000) {
			hdmi_phy_i2c_write(hdmi, 0x800b, 0x09);
			hdmi_phy_i2c_write(hdmi, 0x0129, 0x0E);
	}

	imx_hdmi_phy_enable_power(1);

	/* toggle TMDS enable */
	imx_hdmi_phy_enable_tmds(0);
	imx_hdmi_phy_enable_tmds(1);

	/* gen2 tx power on */
	imx_hdmi_phy_gen2_txpwron(1);
	imx_hdmi_phy_gen2_pddq(0);

	/*Wait for PHY PLL lock */
	msec = 4;
	val = hdmi_readb(HDMI_PHY_STAT0) & HDMI_PHY_TX_PHY_LOCK;
	while (val == 0) {
		udelay(1000);
		if (msec-- == 0) {
			dev_err(hdmi->dev, "PHY PLL not locked\n");
			return -EINVAL;
		}
		val = hdmi_readb(HDMI_PHY_STAT0) & HDMI_PHY_TX_PHY_LOCK;
	}

	return 0;
}

static void imx_hdmi_phy_init(struct imx_hdmi *hdmi)
{
	int i;
	bool cscon = false;
	int ret;

	dev_info(hdmi->dev, "%s\n", __func__);

	/*check csc whether needed activated in HDMI mode */
	cscon = (isColorSpaceConversion(hdmi) &&
			!hdmi->hdmi_data.video_mode.mDVI);

	/* HDMI Phy spec says to do the phy initialization sequence twice */
	for (i = 0 ; i < 2 ; i++) {
		imx_hdmi_phy_sel_data_en_pol(1);
		imx_hdmi_phy_sel_interface_control(0);
		imx_hdmi_phy_enable_tmds(0);
		imx_hdmi_phy_enable_power(0);

		/* Enable CSC */
		ret = hdmi_phy_configure(hdmi, 0, 8, cscon);
		dev_info(hdmi->dev, "%s: hdmi_phy_configure returned %d\n", __func__, ret);
	}

	hdmi->phy_enabled = true;
}

static void hdmi_tx_hdcp_config(struct imx_hdmi *hdmi)
{
	u8 de, val;

	if (hdmi->hdmi_data.video_mode.mDataEnablePolarity)
		de = HDMI_A_VIDPOLCFG_DATAENPOL_ACTIVE_HIGH;
	else
		de = HDMI_A_VIDPOLCFG_DATAENPOL_ACTIVE_LOW;

	/* disable rx detect */
	val = hdmi_readb(HDMI_A_HDCPCFG0);
	val &= HDMI_A_HDCPCFG0_RXDETECT_MASK;
	val |= HDMI_A_HDCPCFG0_RXDETECT_DISABLE;
	hdmi_writeb(val, HDMI_A_HDCPCFG0);

	val = hdmi_readb(HDMI_A_VIDPOLCFG);
	val &= HDMI_A_VIDPOLCFG_DATAENPOL_MASK;
	val |= de;
	hdmi_writeb(val, HDMI_A_VIDPOLCFG);

	val = hdmi_readb(HDMI_A_HDCPCFG1);
	val &= HDMI_A_HDCPCFG1_ENCRYPTIONDISABLE_MASK;
	val |= HDMI_A_HDCPCFG1_ENCRYPTIONDISABLE_DISABLE;
	hdmi_writeb(val, HDMI_A_HDCPCFG1);
}

static void hdmi_config_AVI(struct imx_hdmi *hdmi)
{
	u8 val;
	u8 pix_fmt;
	u8 under_scan;
	u8 act_ratio, coded_ratio, colorimetry, ext_colorimetry;
	bool aspect_16_9;

	dev_info(hdmi->dev, "set up AVI frame\n");

	aspect_16_9 = false; /* FIXME */

	/********************************************
	 * AVI Data Byte 1
	 ********************************************/
	if (hdmi->hdmi_data.enc_out_format == YCBCR444)
		pix_fmt = HDMI_FC_AVICONF0_PIX_FMT_YCBCR444;
	else if (hdmi->hdmi_data.enc_out_format == YCBCR422_8BITS)
		pix_fmt = HDMI_FC_AVICONF0_PIX_FMT_YCBCR422;
	else
		pix_fmt = HDMI_FC_AVICONF0_PIX_FMT_RGB;

#if 0
	if (hdmi->edid_cfg.cea_underscan)
		under_scan = HDMI_FC_AVICONF0_SCAN_INFO_UNDERSCAN;
	else
#endif
		under_scan =  HDMI_FC_AVICONF0_SCAN_INFO_NODATA;

	/*
	 * Active format identification data is present in the AVI InfoFrame.
	 * Under scan info, no bar data
	 */
	val = pix_fmt | under_scan |
		HDMI_FC_AVICONF0_ACTIVE_FMT_INFO_PRESENT |
		HDMI_FC_AVICONF0_BAR_DATA_NO_DATA;

	hdmi_writeb(val, HDMI_FC_AVICONF0);

	/********************************************
	 * AVI Data Byte 2
	 ********************************************/

	/*  Set the Aspect Ratio */
	if (aspect_16_9) {
		act_ratio = HDMI_FC_AVICONF1_ACTIVE_ASPECT_RATIO_16_9;
		coded_ratio = HDMI_FC_AVICONF1_CODED_ASPECT_RATIO_16_9;
	} else {
		act_ratio = HDMI_FC_AVICONF1_ACTIVE_ASPECT_RATIO_4_3;
		coded_ratio = HDMI_FC_AVICONF1_CODED_ASPECT_RATIO_4_3;
	}

	/* Set up colorimetry */
	if (hdmi->hdmi_data.enc_out_format == XVYCC444) {
		colorimetry = HDMI_FC_AVICONF1_COLORIMETRY_EXTENDED_INFO;
		if (hdmi->hdmi_data.colorimetry == eITU601)
			ext_colorimetry =
				HDMI_FC_AVICONF2_EXT_COLORIMETRY_XVYCC601;
		else /* hdmi->hdmi_data.colorimetry == eITU709 */
			ext_colorimetry =
				HDMI_FC_AVICONF2_EXT_COLORIMETRY_XVYCC709;
	} else if (hdmi->hdmi_data.enc_out_format != RGB) {
		if (hdmi->hdmi_data.colorimetry == eITU601)
			colorimetry = HDMI_FC_AVICONF1_COLORIMETRY_SMPTE;
		else /* hdmi->hdmi_data.colorimetry == eITU709 */
			colorimetry = HDMI_FC_AVICONF1_COLORIMETRY_ITUR;
		ext_colorimetry = HDMI_FC_AVICONF2_EXT_COLORIMETRY_XVYCC601;
	} else { /* Carries no data */
		colorimetry = HDMI_FC_AVICONF1_COLORIMETRY_NO_DATA;
		ext_colorimetry = HDMI_FC_AVICONF2_EXT_COLORIMETRY_XVYCC601;
	}

	val = colorimetry | coded_ratio | act_ratio;
	hdmi_writeb(val, HDMI_FC_AVICONF1);

	/********************************************
	 * AVI Data Byte 3
	 ********************************************/

	val = HDMI_FC_AVICONF2_IT_CONTENT_NO_DATA | ext_colorimetry |
		HDMI_FC_AVICONF2_RGB_QUANT_DEFAULT |
		HDMI_FC_AVICONF2_SCALING_NONE;
	hdmi_writeb(val, HDMI_FC_AVICONF2);

	/********************************************
	 * AVI Data Byte 4
	 ********************************************/
	hdmi_writeb(hdmi->vic, HDMI_FC_AVIVID);

	/********************************************
	 * AVI Data Byte 5
	 ********************************************/

	/* Set up input and output pixel repetition */
	val = (((hdmi->hdmi_data.video_mode.mPixelRepetitionInput + 1) <<
		HDMI_FC_PRCONF_INCOMING_PR_FACTOR_OFFSET) &
		HDMI_FC_PRCONF_INCOMING_PR_FACTOR_MASK) |
		((hdmi->hdmi_data.video_mode.mPixelRepetitionOutput <<
		HDMI_FC_PRCONF_OUTPUT_PR_FACTOR_OFFSET) &
		HDMI_FC_PRCONF_OUTPUT_PR_FACTOR_MASK);
	hdmi_writeb(val, HDMI_FC_PRCONF);

	/* IT Content and quantization range = don't care */
	val = HDMI_FC_AVICONF3_IT_CONTENT_TYPE_GRAPHICS |
		HDMI_FC_AVICONF3_QUANT_RANGE_LIMITED;
	hdmi_writeb(val, HDMI_FC_AVICONF3);

	/********************************************
	 * AVI Data Bytes 6-13
	 ********************************************/
	hdmi_writeb(0, HDMI_FC_AVIETB0);
	hdmi_writeb(0, HDMI_FC_AVIETB1);
	hdmi_writeb(0, HDMI_FC_AVISBB0);
	hdmi_writeb(0, HDMI_FC_AVISBB1);
	hdmi_writeb(0, HDMI_FC_AVIELB0);
	hdmi_writeb(0, HDMI_FC_AVIELB1);
	hdmi_writeb(0, HDMI_FC_AVISRB0);
	hdmi_writeb(0, HDMI_FC_AVISRB1);
}

/*!
 * this submodule is responsible for the video/audio data composition.
 */
static void hdmi_av_composer(struct imx_hdmi *hdmi)
{
	u8 inv_val;
	struct fb_videomode *fb_mode = &hdmi->fb_mode;
	struct hdmi_vmode *vmode = &hdmi->hdmi_data.video_mode;
	int hblank, vblank;

	dev_info(hdmi->dev, "%s\n", __func__);

	vmode->mHSyncPolarity = ((fb_mode->sync & FB_SYNC_HOR_HIGH_ACT) != 0);
	vmode->mVSyncPolarity = ((fb_mode->sync & FB_SYNC_VERT_HIGH_ACT) != 0);
	vmode->mInterlaced = ((fb_mode->vmode & FB_VMODE_INTERLACED) != 0);
	vmode->mPixelClock = (fb_mode->xres + fb_mode->left_margin +
		fb_mode->right_margin + fb_mode->hsync_len) * (fb_mode->yres +
		fb_mode->upper_margin + fb_mode->lower_margin +
		fb_mode->vsync_len) * fb_mode->refresh;

	dev_info(hdmi->dev, "final pixclk = %d\n", vmode->mPixelClock);

	/* Set up HDMI_FC_INVIDCONF */
	inv_val = (hdmi->hdmi_data.hdcp_enable ?
		HDMI_FC_INVIDCONF_HDCP_KEEPOUT_ACTIVE :
		HDMI_FC_INVIDCONF_HDCP_KEEPOUT_INACTIVE);

	inv_val |= (vmode->mVSyncPolarity ?
		HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_HIGH :
		HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_LOW);

	inv_val |= (vmode->mHSyncPolarity ?
		HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_HIGH :
		HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_LOW);

	inv_val |= (vmode->mDataEnablePolarity ?
		HDMI_FC_INVIDCONF_DE_IN_POLARITY_ACTIVE_HIGH :
		HDMI_FC_INVIDCONF_DE_IN_POLARITY_ACTIVE_LOW);

	if (hdmi->vic == 39)
		inv_val |= HDMI_FC_INVIDCONF_R_V_BLANK_IN_OSC_ACTIVE_HIGH;
	else
		inv_val |= (vmode->mInterlaced ?
			HDMI_FC_INVIDCONF_R_V_BLANK_IN_OSC_ACTIVE_HIGH :
			HDMI_FC_INVIDCONF_R_V_BLANK_IN_OSC_ACTIVE_LOW);

	inv_val |= (vmode->mInterlaced ?
		HDMI_FC_INVIDCONF_IN_I_P_INTERLACED :
		HDMI_FC_INVIDCONF_IN_I_P_PROGRESSIVE);

	inv_val |= (vmode->mDVI ?
		HDMI_FC_INVIDCONF_DVI_MODEZ_DVI_MODE :
		HDMI_FC_INVIDCONF_DVI_MODEZ_HDMI_MODE);

	hdmi_writeb(inv_val, HDMI_FC_INVIDCONF);

	/* Set up horizontal active pixel region width */
	hdmi_writeb(fb_mode->xres >> 8, HDMI_FC_INHACTV1);
	hdmi_writeb(fb_mode->xres, HDMI_FC_INHACTV0);

	/* Set up vertical blanking pixel region width */
	hdmi_writeb(fb_mode->yres >> 8, HDMI_FC_INVACTV1);
	hdmi_writeb(fb_mode->yres, HDMI_FC_INVACTV0);

	/* Set up horizontal blanking pixel region width */
	hblank = fb_mode->left_margin + fb_mode->right_margin +
		fb_mode->hsync_len;
	hdmi_writeb(hblank >> 8, HDMI_FC_INHBLANK1);
	hdmi_writeb(hblank, HDMI_FC_INHBLANK0);

	/* Set up vertical blanking pixel region width */
	vblank = fb_mode->upper_margin + fb_mode->lower_margin +
		fb_mode->vsync_len;
	hdmi_writeb(vblank, HDMI_FC_INVBLANK);

	/* Set up HSYNC active edge delay width (in pixel clks) */
	hdmi_writeb(fb_mode->right_margin >> 8, HDMI_FC_HSYNCINDELAY1);
	hdmi_writeb(fb_mode->right_margin, HDMI_FC_HSYNCINDELAY0);

	/* Set up VSYNC active edge delay (in pixel clks) */
	hdmi_writeb(fb_mode->lower_margin, HDMI_FC_VSYNCINDELAY);

	/* Set up HSYNC active pulse width (in pixel clks) */
	hdmi_writeb(fb_mode->hsync_len >> 8, HDMI_FC_HSYNCINWIDTH1);
	hdmi_writeb(fb_mode->hsync_len, HDMI_FC_HSYNCINWIDTH0);

	/* Set up VSYNC active edge delay (in pixel clks) */
	hdmi_writeb(fb_mode->vsync_len, HDMI_FC_VSYNCINWIDTH);

	dev_info(hdmi->dev, "%s exit\n", __func__);
}

static void imx_hdmi_phy_disable(struct imx_hdmi *hdmi)
{
	dev_info(hdmi->dev, "%s\n", __func__);

	if (!hdmi->phy_enabled)
		return;

	imx_hdmi_phy_enable_tmds(0);
	imx_hdmi_phy_enable_power(0);

	hdmi->phy_enabled = false;
	dev_info(hdmi->dev, "%s - exit\n", __func__);
}

/* HDMI Initialization Step B.4 */
static void imx_hdmi_enable_video_path(struct imx_hdmi *hdmi)
{
	u8 clkdis;

	dev_info(hdmi->dev, "%s\n", __func__);

	/* control period minimum duration */
	hdmi_writeb(12, HDMI_FC_CTRLDUR);
	hdmi_writeb(32, HDMI_FC_EXCTRLDUR);
	hdmi_writeb(1, HDMI_FC_EXCTRLSPAC);

	/* Set to fill TMDS data channels */
	hdmi_writeb(0x0B, HDMI_FC_CH0PREAM);
	hdmi_writeb(0x16, HDMI_FC_CH1PREAM);
	hdmi_writeb(0x21, HDMI_FC_CH2PREAM);

	/* Enable pixel clock and tmds data path */
	clkdis = 0x7F;
	clkdis &= ~HDMI_MC_CLKDIS_PIXELCLK_DISABLE;
	hdmi_writeb(clkdis, HDMI_MC_CLKDIS);

	clkdis &= ~HDMI_MC_CLKDIS_TMDSCLK_DISABLE;
	hdmi_writeb(clkdis, HDMI_MC_CLKDIS);

	/* Enable csc path */
	if (isColorSpaceConversion(hdmi)) {
		clkdis &= ~HDMI_MC_CLKDIS_CSCCLK_DISABLE;
		hdmi_writeb(clkdis, HDMI_MC_CLKDIS);
	}
}

static void hdmi_enable_audio_clk(struct imx_hdmi *hdmi)
{
	u8 clkdis;

	dev_info(hdmi->dev, "%s\n", __func__);

	clkdis = hdmi_readb(HDMI_MC_CLKDIS);
	clkdis &= ~HDMI_MC_CLKDIS_AUDCLK_DISABLE;
	hdmi_writeb(clkdis, HDMI_MC_CLKDIS);
}

/* Workaround to clear the overflow condition */
static void imx_hdmi_clear_overflow(void)
{
	int count;
	u8 val;

	val = hdmi_readb(HDMI_FC_INVIDCONF);

	for (count = 0 ; count < 5 ; count++)
		hdmi_writeb(val, HDMI_FC_INVIDCONF);

	/* TMDS software reset */
	hdmi_writeb((u8)~HDMI_MC_SWRSTZ_TMDSSWRST_REQ, HDMI_MC_SWRSTZ);
}

static void hdmi_enable_overflow_interrupts(void)
{
	pr_debug("%s\n", __func__);
	hdmi_writeb(0, HDMI_FC_MASK2);
	hdmi_writeb(0, HDMI_IH_MUTE_FC_STAT2);
}

static void hdmi_disable_overflow_interrupts(void)
{
	pr_debug("%s\n", __func__);
	hdmi_writeb(HDMI_IH_MUTE_FC_STAT2_OVERFLOW_MASK,
		    HDMI_IH_MUTE_FC_STAT2);
}

static inline void
convert_to_video_timing(struct fb_videomode *timing,
			struct drm_display_mode *mode)
{
	DRM_DEBUG_KMS("%s\n", __FILE__);

	memset(timing, 0, sizeof(*timing));

	timing->pixclock = mode->clock * 1000;
	timing->refresh = drm_mode_vrefresh(mode);

	timing->xres = mode->hdisplay;
	timing->left_margin = mode->hsync_start - mode->hdisplay;
	timing->hsync_len = mode->hsync_end - mode->hsync_start;
	timing->right_margin = mode->htotal - mode->hsync_end;

	timing->yres = mode->vdisplay;
	timing->upper_margin = mode->vsync_start - mode->vdisplay;
	timing->vsync_len = mode->vsync_end - mode->vsync_start;
	timing->lower_margin = mode->vtotal - mode->vsync_end;

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		timing->vmode = FB_VMODE_INTERLACED;
	else
		timing->vmode = FB_VMODE_NONINTERLACED;

	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		timing->vmode |= FB_VMODE_DOUBLE;
}

static void imx_hdmi_setup(struct imx_hdmi *hdmi, struct drm_display_mode *mode)
{
	dev_info(hdmi->dev, "%s\n", __func__);

	convert_to_video_timing(&hdmi->fb_mode, mode);
	dump_fb_videomode(&hdmi->fb_mode);

	dev_info(hdmi->dev, "%s - video mode changed\n", __func__);

	hdmi_disable_overflow_interrupts();

	hdmi->vic = 0;

	if (hdmi->vic == 0) {
		dev_dbg(hdmi->dev, "Non-CEA mode used in HDMI\n");
		hdmi->hdmi_data.video_mode.mDVI = true;
	} else {
		dev_dbg(hdmi->dev, "CEA mode used vic=%d\n", hdmi->vic);
		hdmi->hdmi_data.video_mode.mDVI = false;
	}

	if ((hdmi->vic == 6) || (hdmi->vic == 7) ||
		(hdmi->vic == 21) || (hdmi->vic == 22) ||
		(hdmi->vic == 2) || (hdmi->vic == 3) ||
		(hdmi->vic == 17) || (hdmi->vic == 18))
		hdmi->hdmi_data.colorimetry = eITU601;
	else
		hdmi->hdmi_data.colorimetry = eITU709;

	if ((hdmi->vic == 10) || (hdmi->vic == 11) ||
		(hdmi->vic == 12) || (hdmi->vic == 13) ||
		(hdmi->vic == 14) || (hdmi->vic == 15) ||
		(hdmi->vic == 25) || (hdmi->vic == 26) ||
		(hdmi->vic == 27) || (hdmi->vic == 28) ||
		(hdmi->vic == 29) || (hdmi->vic == 30) ||
		(hdmi->vic == 35) || (hdmi->vic == 36) ||
		(hdmi->vic == 37) || (hdmi->vic == 38))
		hdmi->hdmi_data.video_mode.mPixelRepetitionOutput = 1;
	else
		hdmi->hdmi_data.video_mode.mPixelRepetitionOutput = 0;

	hdmi->hdmi_data.video_mode.mPixelRepetitionInput = 0;

	/* TODO: Get input format from IPU (via FB driver iface) */
	hdmi->hdmi_data.enc_in_format = RGB;

	hdmi->hdmi_data.enc_out_format = RGB;
	/*DVI mode not support non-RGB */
#if 0
	if (!hdmi->hdmi_data.video_mode.mDVI)
		if (hdmi->edid_cfg.hdmi_cap) {
			if (hdmi->edid_cfg.cea_ycbcr444)
				hdmi->hdmi_data.enc_out_format = YCBCR444;
			else if (hdmi->edid_cfg.cea_ycbcr422)
				hdmi->hdmi_data.enc_out_format = YCBCR422_8BITS;
		}
#endif

	hdmi->hdmi_data.enc_color_depth = 8;
	hdmi->hdmi_data.pix_repet_factor = 0;
	hdmi->hdmi_data.hdcp_enable = 0;
	hdmi->hdmi_data.video_mode.mDataEnablePolarity = true;

	/* HDMI Initialization Step B.1 */
	hdmi_av_composer(hdmi);

	/* HDMI Initializateion Step B.2 */
	imx_hdmi_phy_init(hdmi);

	/* HDMI Initialization Step B.3 */
	imx_hdmi_enable_video_path(hdmi);

	/* not for DVI mode */
	if (hdmi->hdmi_data.video_mode.mDVI)
		dev_dbg(hdmi->dev, "%s DVI mode\n", __func__);
	else {
		dev_dbg(hdmi->dev, "%s CEA mode\n", __func__);

		/* HDMI Initialization Step E - Configure audio */
		hdmi_clk_regenerator_update_pixel_clock();
		hdmi_enable_audio_clk(hdmi);

		/* HDMI Initialization Step F - Configure AVI InfoFrame */
		hdmi_config_AVI(hdmi);
	}

	hdmi_video_packetize(hdmi);
	hdmi_video_csc(hdmi);
	hdmi_video_sample(hdmi);
	hdmi_tx_hdcp_config(hdmi);

	imx_hdmi_clear_overflow();
	if (hdmi->cable_plugin && !hdmi->hdmi_data.video_mode.mDVI)
		hdmi_enable_overflow_interrupts();

	dev_dbg(hdmi->dev, "%s exit\n\n", __func__);
}

/* Wait until we are registered to enable interrupts */
static void imx_hdmi_fb_registered(struct imx_hdmi *hdmi)
{
	unsigned long flags;

	if (hdmi->fb_reg)
		return;

	spin_lock_irqsave(&hdmi->irq_lock, flags);

	dev_dbg(hdmi->dev, "%s\n", __func__);

	clk_prepare_enable(hdmi->hdmi_iahb_clk);

	hdmi_writeb(HDMI_PHY_I2CM_INT_ADDR_DONE_POL,
		    HDMI_PHY_I2CM_INT_ADDR);

	hdmi_writeb(HDMI_PHY_I2CM_CTLINT_ADDR_NAC_POL |
		    HDMI_PHY_I2CM_CTLINT_ADDR_ARBITRATION_POL,
		    HDMI_PHY_I2CM_CTLINT_ADDR);

	/* enable cable hot plug irq */
	hdmi_writeb((u8)~HDMI_PHY_HPD, HDMI_PHY_MASK0);

	/* Clear Hotplug interrupts */
	hdmi_writeb(HDMI_IH_PHY_STAT0_HPD, HDMI_IH_PHY_STAT0);

	/* Unmute interrupts */
	hdmi_writeb(~HDMI_IH_PHY_STAT0_HPD, HDMI_IH_MUTE_PHY_STAT0);

	hdmi->fb_reg = true;

	clk_disable_unprepare(hdmi->hdmi_iahb_clk);

	spin_unlock_irqrestore(&hdmi->irq_lock, flags);
}

static void initialize_hdmi_ih_mutes(void)
{
	u8 ih_mute;

	/*
	 * Boot up defaults are:
	 * HDMI_IH_MUTE   = 0x03 (disabled)
	 * HDMI_IH_MUTE_* = 0x00 (enabled)
	 */

	/* Disable top level interrupt bits in HDMI block */
	ih_mute = hdmi_readb(HDMI_IH_MUTE) |
		  HDMI_IH_MUTE_MUTE_WAKEUP_INTERRUPT |
		  HDMI_IH_MUTE_MUTE_ALL_INTERRUPT;

	hdmi_writeb(ih_mute, HDMI_IH_MUTE);

	/* by default mask all interrupts */
	hdmi_writeb(0xff, HDMI_VP_MASK);
	hdmi_writeb(0xff, HDMI_FC_MASK0);
	hdmi_writeb(0xff, HDMI_FC_MASK1);
	hdmi_writeb(0xff, HDMI_FC_MASK2);
	hdmi_writeb(0xff, HDMI_PHY_MASK0);
	hdmi_writeb(0xff, HDMI_PHY_I2CM_INT_ADDR);
	hdmi_writeb(0xff, HDMI_PHY_I2CM_CTLINT_ADDR);
	hdmi_writeb(0xff, HDMI_AUD_INT);
	hdmi_writeb(0xff, HDMI_AUD_SPDIFINT);
	hdmi_writeb(0xff, HDMI_AUD_HBR_MASK);
	hdmi_writeb(0xff, HDMI_GP_MASK);
	hdmi_writeb(0xff, HDMI_A_APIINTMSK);
	hdmi_writeb(0xff, HDMI_CEC_MASK);
	hdmi_writeb(0xff, HDMI_I2CM_INT);
	hdmi_writeb(0xff, HDMI_I2CM_CTLINT);

	/* Disable interrupts in the IH_MUTE_* registers */
	hdmi_writeb(0xff, HDMI_IH_MUTE_FC_STAT0);
	hdmi_writeb(0xff, HDMI_IH_MUTE_FC_STAT1);
	hdmi_writeb(0xff, HDMI_IH_MUTE_FC_STAT2);
	hdmi_writeb(0xff, HDMI_IH_MUTE_AS_STAT0);
	hdmi_writeb(0xff, HDMI_IH_MUTE_PHY_STAT0);
	hdmi_writeb(0xff, HDMI_IH_MUTE_I2CM_STAT0);
	hdmi_writeb(0xff, HDMI_IH_MUTE_CEC_STAT0);
	hdmi_writeb(0xff, HDMI_IH_MUTE_VP_STAT0);
	hdmi_writeb(0xff, HDMI_IH_MUTE_I2CMPHY_STAT0);
	hdmi_writeb(0xff, HDMI_IH_MUTE_AHBDMAAUD_STAT0);

	/* Enable top level interrupt bits in HDMI block */
	ih_mute &= ~(HDMI_IH_MUTE_MUTE_WAKEUP_INTERRUPT |
		    HDMI_IH_MUTE_MUTE_ALL_INTERRUPT);
	hdmi_writeb(ih_mute, HDMI_IH_MUTE);
}

/* ------------------------------------ crap barrier --------------- */

static void imx_hdmi_poweron(struct imx_hdmi *hdmi)
{
	printk("%s\n", __func__);

	imx_hdmi_setup(hdmi, &hdmi->previous_mode);
}

static void imx_hdmi_poweroff(struct imx_hdmi *hdmi)
{
	printk("%s\n", __func__);

	imx_hdmi_phy_disable(hdmi);
}

static enum drm_connector_status imx_hdmi_connector_detect(struct drm_connector *connector, bool force)
{
	/* FIXME */
	return connector_status_connected;
}

static void imx_hdmi_connector_destroy(struct drm_connector *connector)
{
}

static int imx_hdmi_connector_get_modes(struct drm_connector *connector)
{
	struct imx_hdmi *hdmi = container_of(connector, struct imx_hdmi, connector);
	struct edid *edid;
	int ret;

	if (!hdmi->ddc)
		return 0;

	edid = drm_get_edid(connector, hdmi->ddc);
	if (edid) {
		pr_info("got edid: width[%d] x height[%d]\n", edid->width_cm, edid->height_cm);

		drm_mode_connector_update_edid_property(connector, edid);
		ret = drm_add_edid_modes(connector, edid);
		connector->display_info.raw_edid = NULL;
		kfree(edid);
	} else {
		pr_info("no edid\n");
	}

	return 0;
}

static int imx_hdmi_connector_mode_valid(struct drm_connector *connector,
			  struct drm_display_mode *mode)
{
	printk("%s\n", __func__);

	return MODE_OK;
}

static struct drm_encoder *imx_hdmi_connector_best_encoder(struct drm_connector *connector)
{
	struct imx_hdmi *hdmi = container_of(connector, struct imx_hdmi, connector);

	return &hdmi->encoder;
}

static void imx_hdmi_encoder_mode_set(struct drm_encoder *encoder,
			struct drm_display_mode *mode,
			struct drm_display_mode *adjusted_mode)
{
	struct imx_hdmi *hdmi = container_of(encoder, struct imx_hdmi, encoder);

	printk("%s\n", __func__);

	imx_hdmi_setup(hdmi, mode);

	/* Store the display mode for plugin/DKMS poweron events */
	memcpy(&hdmi->previous_mode, mode, sizeof(hdmi->previous_mode));
}

static bool imx_hdmi_encoder_mode_fixup(struct drm_encoder *encoder,
				struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void imx_hdmi_encoder_disable(struct drm_encoder *encoder)
{
}

static void imx_hdmi_encoder_dpms(struct drm_encoder *encoder, int mode)
{
	struct imx_hdmi *hdmi = container_of(encoder, struct imx_hdmi, encoder);

	printk("%s\n", __func__);

	if (mode)
		imx_hdmi_poweroff(hdmi);
	else
		imx_hdmi_poweron(hdmi);
}

static void imx_hdmi_encoder_prepare(struct drm_encoder *encoder)
{
	struct imx_hdmi *hdmi = container_of(encoder, struct imx_hdmi, encoder);

	printk("%s\n", __func__);

	imx_hdmi_poweroff(hdmi);

	imx_drm_crtc_panel_format(encoder->crtc, DRM_MODE_ENCODER_NONE, V4L2_PIX_FMT_RGB24);
}

static void imx_hdmi_encoder_commit(struct drm_encoder *encoder)
{
	struct imx_hdmi *hdmi = container_of(encoder, struct imx_hdmi, encoder);
	int mux = imx_drm_encoder_get_mux_id(hdmi->imx_drm_encoder, encoder->crtc);

	imx_hdmi_set_ipu_di_mux(mux);

	imx_hdmi_poweron(hdmi);
}

static void imx_hdmi_encoder_destroy(struct drm_encoder *encoder)
{
}

static struct drm_encoder_funcs imx_hdmi_encoder_funcs = {
	.destroy = imx_hdmi_encoder_destroy,
};

static struct drm_encoder_helper_funcs imx_hdmi_encoder_helper_funcs = {
	.dpms = imx_hdmi_encoder_dpms,
	.prepare = imx_hdmi_encoder_prepare,
	.commit = imx_hdmi_encoder_commit,
	.mode_set = imx_hdmi_encoder_mode_set,
	.mode_fixup = imx_hdmi_encoder_mode_fixup,
	.disable = imx_hdmi_encoder_disable,
};

static struct drm_connector_funcs imx_hdmi_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = imx_hdmi_connector_detect,
	.destroy = imx_hdmi_connector_destroy,
};

static struct drm_connector_helper_funcs imx_hdmi_connector_helper_funcs = {
	.get_modes = imx_hdmi_connector_get_modes,
	.mode_valid = imx_hdmi_connector_mode_valid,
	.best_encoder = imx_hdmi_connector_best_encoder,
};

static irqreturn_t imx_hdmi_irq(int irq, void *dev_id)
{
	struct imx_hdmi *hdmi = dev_id;
	u8 intr_stat;
	u8 phy_int_pol;
	u8 val;

	intr_stat = hdmi_readb(HDMI_IH_PHY_STAT0);

	printk("%s: 0x%02x\n", __func__, intr_stat);

	phy_int_pol = hdmi_readb(HDMI_PHY_POL0);

	if (intr_stat & HDMI_IH_PHY_STAT0_HPD) {
		if (phy_int_pol & HDMI_PHY_HPD) {
			dev_info(hdmi->dev, "EVENT=plugin\n");

			val = hdmi_readb(HDMI_PHY_POL0);
			val &= ~HDMI_PHY_HPD;
			hdmi_writeb(val, HDMI_PHY_POL0);

			imx_hdmi_poweron(hdmi);
		} else {
			dev_info(hdmi->dev, "EVENT=plugout\n");

			val = hdmi_readb(HDMI_PHY_POL0);
			val |= HDMI_PHY_HPD;
			hdmi_writeb(val, HDMI_PHY_POL0);

			imx_hdmi_poweroff(hdmi);
		}
	}

	hdmi_writeb(intr_stat, HDMI_IH_PHY_STAT0);

	return IRQ_HANDLED;
}

static int __devinit imx_hdmi_register(struct imx_hdmi *hdmi)
{
	int ret;

	drm_mode_connector_attach_encoder(&hdmi->connector, &hdmi->encoder);

	hdmi->connector.funcs = &imx_hdmi_connector_funcs;
	hdmi->encoder.funcs = &imx_hdmi_encoder_funcs;

	hdmi->encoder.encoder_type = DRM_MODE_ENCODER_TMDS;
	hdmi->connector.connector_type = DRM_MODE_CONNECTOR_HDMIA;

	drm_encoder_helper_add(&hdmi->encoder, &imx_hdmi_encoder_helper_funcs);
	ret = imx_drm_add_encoder(&hdmi->encoder, &hdmi->imx_drm_encoder,
			THIS_MODULE);
	if (ret) {
		dev_err(hdmi->dev, "adding encoder failed with %d\n", ret);
		return ret;
	}

	drm_connector_helper_add(&hdmi->connector,
			&imx_hdmi_connector_helper_funcs);

	ret = imx_drm_add_connector(&hdmi->connector,
			&hdmi->imx_drm_connector, THIS_MODULE);
	if (ret) {
		imx_drm_remove_encoder(hdmi->imx_drm_encoder);
		dev_err(hdmi->dev, "adding connector failed with %d\n", ret);
		return ret;
	}

	hdmi->connector.encoder = &hdmi->encoder;

	return 0;
}

static int __devinit imx_hdmi_platform_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *ddc_node;
	struct imx_hdmi *hdmi;
	int ret;
	struct resource *iores;
	int irq;

	iores = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!iores)
		return -EINVAL;

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return -EINVAL;

	hdmi = devm_kzalloc(&pdev->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	hdmi->dev = &pdev->dev;

	ddc_node = of_parse_phandle(np, "ddc", 0);
	if (ddc_node) {
		hdmi->ddc = of_find_i2c_adapter_by_node(ddc_node);
		of_node_put(ddc_node);
	}

	ret = devm_request_irq(&pdev->dev, irq,
                                       imx_hdmi_irq, 0,
                                       dev_name(&pdev->dev), hdmi);

	hdmi->regs = devm_ioremap(&pdev->dev, iores->start, resource_size(iores));
	if (!hdmi->regs) {
		dev_err(&pdev->dev, "failed to map IO/CTL base\n");
		return -ENOMEM;
	}

	hdmi_base = hdmi->regs;

	iomuxc_base = devm_ioremap(&pdev->dev, MX6Q_IOMUXC_BASE_ADDR, 0x4000);
	if (!iomuxc_base)
		return -ENOMEM;

	hdmi->hdmi_isfr_clk = clk_get(hdmi->dev, "isfr");
	if (IS_ERR(hdmi->hdmi_isfr_clk)) {
		ret = PTR_ERR(hdmi->hdmi_isfr_clk);
		dev_err(hdmi->dev,
			"Unable to get HDMI isfr clk: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(hdmi->hdmi_isfr_clk);
	if (ret < 0) {
		dev_err(hdmi->dev,
			"Cannot enable HDMI isfr clock: %d\n", ret);
		return ret;
	}

	hdmi->hdmi_iahb_clk = clk_get(hdmi->dev, "iahb");
	if (IS_ERR(hdmi->hdmi_iahb_clk)) {
		ret = PTR_ERR(hdmi->hdmi_iahb_clk);
		dev_err(hdmi->dev,
			"Unable to get HDMI iahb clk: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(hdmi->hdmi_iahb_clk);
	if (ret < 0) {
		dev_err(hdmi->dev,
			"Cannot enable HDMI iahb clock: %d\n", ret);
		return ret;
	}

	/* Product and revision IDs */
	dev_info(&pdev->dev,
		"Detected HDMI controller 0x%x:0x%x:0x%x:0x%x\n",
		hdmi_readb(HDMI_DESIGN_ID),
		hdmi_readb(HDMI_REVISION_ID),
		hdmi_readb(HDMI_PRODUCT_ID0),
		hdmi_readb(HDMI_PRODUCT_ID1));

	initialize_hdmi_ih_mutes();

	/* To prevent overflows in HDMI_IH_FC_STAT2, set the clk regenerator
	 * N and cts values before enabling phy */
	hdmi_init_clk_regenerator(hdmi);

	spin_lock_init(&hdmi->irq_lock);

	/* Configure registers related to HDMI interrupt
	 * generation before registering IRQ. */
	hdmi_writeb(HDMI_PHY_HPD, HDMI_PHY_POL0);

	/* Clear Hotplug interrupts */
	hdmi_writeb(HDMI_IH_PHY_STAT0_HPD, HDMI_IH_PHY_STAT0);

	imx_hdmi_fb_registered(hdmi);

	ret = imx_hdmi_register(hdmi);
	if (ret)
		return ret;

	imx_drm_encoder_add_possible_crtcs(hdmi->imx_drm_encoder, np);

	platform_set_drvdata(pdev, hdmi);

	dev_info(&pdev->dev, "initialized\n");

	return 0;
}

static const struct of_device_id imx_ipu_dt_ids[] = {
	{ .compatible = "fsl,imx6q-hdmi", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_ipu_dt_ids);

static int __devexit imx_hdmi_platform_remove(struct platform_device *pdev)
{
	struct imx_hdmi *hdmi = platform_get_drvdata(pdev);

	i2c_put_adapter(hdmi->ddc);

	return 0;
}

static struct platform_driver imx_hdmi_driver = {
	.driver = {
		.name = "imx-hdmi",
		.owner = THIS_MODULE,
		.of_match_table = imx_ipu_dt_ids,
	},

	.probe = imx_hdmi_platform_probe,
	.remove = __devexit_p(imx_hdmi_platform_remove),
};

module_platform_driver(imx_hdmi_driver);

MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_DESCRIPTION("Silicon Image sii902x HDMI transmitter driver");
MODULE_LICENSE("GPL");
