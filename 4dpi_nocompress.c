/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 4dpi_nocompress.c - minimal non-compressed framebuffer driver for 4D Systems 4DPi-32
 *
 * Derived conceptually from trichner/4d-hats 4d-hats.c, but removes the
 * external compress-v6.o / compress-v7.o path and always performs raw SPI
 * framebuffer updates using the 4D control-byte protocol.
 *
 * Target: Raspberry Pi OS Trixie / kernel 6.x as an out-of-tree module.
 * Notes:
 *   - This is intentionally 4DPi-32 only: ILI9341, 320x240, RGB565.
 *   - No touchscreen support.
 *   - No external GPIO DC/RESET is used. 4DPi uses a control byte before
 *     each 16-bit command/data word.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>
#include <linux/fb.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

/* ILI9341 commands used by the original 32-HAT setup */
#define ILI9341_SLEEP_OUT                    0x11
#define ILI9341_GAMMA_SET                    0x26
#define ILI9341_DISPLAY_ON                   0x29
#define ILI9341_SET_COLUMN_ADDRESS           0x2A
#define ILI9341_SET_PAGE_ADDRESS             0x2B
#define ILI9341_WRITE_MEMORY                 0x2C
#define ILI9341_MEMORY_ACCESS_CONTROL        0x36
#define ILI9341_PIXEL_FORMAT_SET             0x3A
#define ILI9341_FRAME_RATE_CONTROL           0xB1
#define ILI9341_DISPLAY_FUNCTION_CONTROL     0xB6
#define ILI9341_POWER_CONTROL_1              0xC0
#define ILI9341_POWER_CONTROL_2              0xC1
#define ILI9341_VCOM_CONTROL_1               0xC5
#define ILI9341_VCOM_CONTROL_2               0xC7
#define ILI9341_POWER_CONTROL_A              0xCB
#define ILI9341_POWER_CONTROL_B              0xCF
#define ILI9341_POSITIVE_GAMMA_CORRECTION    0xE0
#define ILI9341_NEGATIVE_GAMMA_CORRECTION    0xE1
#define ILI9341_DRIVER_TIMING_CONTROL_A      0xE8
#define ILI9341_DRIVER_TIMING_CONTROL_B      0xEA
#define ILI9341_POWER_ON_SEQUENCE_CONTROL    0xED
#define ILI9341_UNDOCUMENTED_0xEF            0xEF
#define ILI9341_ENABLE_3G                    0xF2
#define ILI9341_INTERFACE_CONTROL            0xF6

/* 4D control byte bits. These come from the original 4d-hats.c protocol. */
#define LCDPI_LONG   (1 << 7)
#define LCDPI_BLOCK  (1 << 6)
#define LCDPI_RESET  (1 << 5)
#define LCDPI_RS     (1 << 4)  /* command/data select: 0=command, 1=data */
#define LCDPI_BL     (1 << 3)  /* backlight */
#define LCDPI_RD     (1 << 2)

#define LCDPI_XRES       320
#define LCDPI_YRES       240
#define LCDPI_BPP        16
#define LCDPI_LINE_LEN   (LCDPI_XRES * 2)
#define LCDPI_FB_SIZE    (LCDPI_XRES * LCDPI_YRES * 2)
#define LCDPI_TX_PIXELS  2048
#define LCDPI_TX_LEN     (1 + LCDPI_TX_PIXELS * 2 + 1)

static int rotate = 0;
module_param(rotate, int, 0444);
MODULE_PARM_DESC(rotate, "Screen rotation: 0/90/180/270");

static ulong sclk = 48000000;
module_param(sclk, ulong, 0444);
MODULE_PARM_DESC(sclk, "SPI clock frequency");

struct lcdpi {
	struct device *dev;
	struct spi_device *spi;
	struct fb_info *info;
	void *vmem;
	u8 *txbuf;
	u8 backlight;
};

static int lcdpi_spi_write_word(struct lcdpi *item, u16 value, bool isdata)
{
	u8 b[3];

	b[0] = (isdata ? LCDPI_RS : 0) |
	       (item->backlight ? LCDPI_BL : 0) |
	       LCDPI_RESET | LCDPI_RD;
	b[1] = value >> 8;
	b[2] = value & 0xff;

	return spi_write(item->spi, b, sizeof(b));
}

static int lcdpi_cmd(struct lcdpi *item, u8 cmd)
{
	return lcdpi_spi_write_word(item, cmd, false);
}

static int lcdpi_data8(struct lcdpi *item, u8 data)
{
	return lcdpi_spi_write_word(item, data, true);
}

static int lcdpi_write_array(struct lcdpi *item, const u8 *seq, size_t len)
{
	size_t i;
	int ret;

	if (!seq || !len)
		return -EINVAL;

	ret = lcdpi_cmd(item, seq[0]);
	if (ret)
		return ret;

	for (i = 1; i < len; i++) {
		ret = lcdpi_data8(item, seq[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int lcdpi_ctrl_write(struct lcdpi *item, u8 control)
{
	u8 b[3] = { control, 0, 0 };
	return spi_write(item->spi, b, sizeof(b));
}

static int lcdpi_reset_panel(struct lcdpi *item)
{
	int ret;

	ret = lcdpi_ctrl_write(item, LCDPI_RESET | LCDPI_RD);
	if (ret)
		return ret;
	mdelay(50);

	ret = lcdpi_ctrl_write(item, LCDPI_RD);
	if (ret)
		return ret;
	mdelay(50);

	ret = lcdpi_ctrl_write(item, LCDPI_RESET | LCDPI_RD);
	if (ret)
		return ret;
	mdelay(50);

	return 0;
}

/* Original 4DPi-32 / 32-HAT ILI9341 initialization values. */
static const u8 seq_interface[]    = { ILI9341_INTERFACE_CONTROL,         0x01, 0x01, 0x00 };
static const u8 seq_undoc[]        = { ILI9341_UNDOCUMENTED_0xEF,         0x03, 0x80, 0x02 };
static const u8 seq_power_b[]      = { ILI9341_POWER_CONTROL_B,           0x00, 0xF2, 0xA0 };
static const u8 seq_power_on[]     = { ILI9341_POWER_ON_SEQUENCE_CONTROL, 0x64, 0x03, 0x12, 0x81 };
static const u8 seq_power_a[]      = { ILI9341_POWER_CONTROL_A,           0x39, 0x2C, 0x00, 0x34, 0x02 };
static const u8 seq_timing_b[]     = { ILI9341_DRIVER_TIMING_CONTROL_B,   0x00, 0x00 };
static const u8 seq_timing_a[]     = { ILI9341_DRIVER_TIMING_CONTROL_A,   0x85, 0x10, 0x7A };
static const u8 seq_power1[]       = { ILI9341_POWER_CONTROL_1,           0x21 };
static const u8 seq_power2[]       = { ILI9341_POWER_CONTROL_2,           0x11 };
static const u8 seq_vcom1[]        = { ILI9341_VCOM_CONTROL_1,            0x3F, 0x3C };
static const u8 seq_vcom2[]        = { ILI9341_VCOM_CONTROL_2,            0xC6 };
static const u8 seq_pixfmt[]       = { ILI9341_PIXEL_FORMAT_SET,          0x55 };
static const u8 seq_framerate[]    = { ILI9341_FRAME_RATE_CONTROL,        0x00, 0x1B };
static const u8 seq_dispfunc[]     = { ILI9341_DISPLAY_FUNCTION_CONTROL,  0x0A, 0xA2 };
static const u8 seq_enable_3g[]    = { ILI9341_ENABLE_3G,                 0x00 };
static const u8 seq_gamma_set[]    = { ILI9341_GAMMA_SET,                 0x01 };
static const u8 seq_gamma_p[]      = { ILI9341_POSITIVE_GAMMA_CORRECTION, 0x0f, 0x24, 0x21, 0x0F, 0x13, 0x0A, 0x52, 0xC9, 0x3B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 seq_gamma_n[]      = { ILI9341_NEGATIVE_GAMMA_CORRECTION, 0x00, 0x1B, 0x1E, 0x00, 0x0C, 0x04, 0x2F, 0x36, 0x44, 0x0a, 0x1F, 0x0F, 0x3F, 0x3F, 0x0F };
static const u8 seq_sleep_out[]    = { ILI9341_SLEEP_OUT };
static const u8 seq_display_on[]   = { ILI9341_DISPLAY_ON };

static u8 lcdpi_madctl_from_rotate(void)
{
	switch (rotate % 360) {
	case 90:  return 0xC8;
	case 270: return 0x08;
	case 180: return 0x68;
	default:  return 0xA8;
	}
}

static int ili9341_32_setup(struct lcdpi *item)
{
	int ret;
	u8 madctl[] = { ILI9341_MEMORY_ACCESS_CONTROL, lcdpi_madctl_from_rotate() };

	dev_info(item->dev, "initializing 4DPi-32 ILI9341, rotate=%d\n", rotate % 360);

	ret = lcdpi_reset_panel(item); if (ret) return ret;

#define WRSEQ(s) do { ret = lcdpi_write_array(item, s, ARRAY_SIZE(s)); if (ret) return ret; } while (0)
	WRSEQ(seq_interface);
	WRSEQ(seq_undoc);
	WRSEQ(seq_power_b);
	WRSEQ(seq_power_on);
	WRSEQ(seq_power_a);
	WRSEQ(seq_timing_b);
	WRSEQ(seq_timing_a);
	WRSEQ(seq_power1);
	WRSEQ(seq_power2);
	WRSEQ(seq_vcom1);
	WRSEQ(seq_vcom2);
	WRSEQ(seq_pixfmt);
	ret = lcdpi_write_array(item, madctl, ARRAY_SIZE(madctl)); if (ret) return ret;
	WRSEQ(seq_framerate);
	WRSEQ(seq_dispfunc);
	WRSEQ(seq_enable_3g);
	WRSEQ(seq_gamma_set);
	WRSEQ(seq_gamma_p);
	WRSEQ(seq_gamma_n);
	ret = lcdpi_cmd(item, ILI9341_WRITE_MEMORY); if (ret) return ret;
	WRSEQ(seq_sleep_out);
	msleep(120);
	WRSEQ(seq_display_on);
	usleep_range(5000, 7000);
	ret = lcdpi_cmd(item, ILI9341_WRITE_MEMORY); if (ret) return ret;
#undef WRSEQ
	return 0;
}

static int lcdpi_set_addr_window(struct lcdpi *item, u16 x0, u16 y0, u16 x1, u16 y1)
{
	int ret;

	ret = lcdpi_cmd(item, ILI9341_SET_COLUMN_ADDRESS); if (ret) return ret;
	ret = lcdpi_data8(item, x0 >> 8); if (ret) return ret;
	ret = lcdpi_data8(item, x0 & 0xff); if (ret) return ret;
	ret = lcdpi_data8(item, x1 >> 8); if (ret) return ret;
	ret = lcdpi_data8(item, x1 & 0xff); if (ret) return ret;

	ret = lcdpi_cmd(item, ILI9341_SET_PAGE_ADDRESS); if (ret) return ret;
	ret = lcdpi_data8(item, y0 >> 8); if (ret) return ret;
	ret = lcdpi_data8(item, y0 & 0xff); if (ret) return ret;
	ret = lcdpi_data8(item, y1 >> 8); if (ret) return ret;
	ret = lcdpi_data8(item, y1 & 0xff); if (ret) return ret;

	return lcdpi_cmd(item, ILI9341_WRITE_MEMORY);
}

static void lcdpi_update_all(struct lcdpi *item)
{
	u16 *src = item->vmem;
	int pixels_left = LCDPI_XRES * LCDPI_YRES;
	int ret;

	ret = lcdpi_set_addr_window(item, 0, 0, LCDPI_XRES - 1, LCDPI_YRES - 1);
	if (ret) {
		dev_err(item->dev, "set address window failed: %d\n", ret);
		return;
	}

	while (pixels_left > 0) {
		int n = min(pixels_left, LCDPI_TX_PIXELS);
		int i;

		item->txbuf[0] = LCDPI_RS |
			(item->backlight ? LCDPI_BL : 0) |
			LCDPI_RESET | LCDPI_RD | LCDPI_LONG;

		for (i = 0; i < n; i++) {
			u16 v = src[i];
			item->txbuf[1 + i * 2] = v >> 8;
			item->txbuf[2 + i * 2] = v & 0xff;
		}
		item->txbuf[1 + n * 2] = 0;

		ret = spi_write(item->spi, item->txbuf, 1 + n * 2 + 1);
		if (ret) {
			dev_err(item->dev, "raw framebuffer SPI write failed: %d\n", ret);
			return;
		}

		src += n;
		pixels_left -= n;
	}
}

static void lcdpi_deferred_io(struct fb_info *info, struct list_head *pagelist)
{
	struct lcdpi *item = info->par;
	lcdpi_update_all(item);
}

static void lcdpi_schedule_update(struct fb_info *info)
{
	if (info && info->fbdefio)
		schedule_delayed_work(&info->deferred_work, info->fbdefio->delay);
}

static ssize_t lcdpi_write(struct fb_info *info, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	ssize_t ret = fb_sys_write(info, buf, count, ppos);
	if (ret > 0)
		lcdpi_schedule_update(info);
	return ret;
}

static int lcdpi_blank(int blank, struct fb_info *info)
{
	struct lcdpi *item = info->par;
	item->backlight = (blank == FB_BLANK_UNBLANK) ? 1 : 0;
	lcdpi_schedule_update(info);
	return 0;
}

static int lcdpi_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;
	u32 value;

	if (regno >= 16)
		return -EINVAL;

	red >>= 11;
	green >>= 10;
	blue >>= 11;
	value = (red << 11) | (green << 5) | blue;
	pal[regno] = value;
	return 0;
}

static void lcdpi_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
	lcdpi_schedule_update(info);
}

static void lcdpi_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	lcdpi_schedule_update(info);
}

static void lcdpi_imageblit(struct fb_info *info, const struct fb_image *image)
{
	sys_imageblit(info, image);
	lcdpi_schedule_update(info);
}

static struct fb_ops lcdpi_fbops = {
	.owner        = THIS_MODULE,
	.fb_read      = fb_sys_read,
	.fb_write     = lcdpi_write,
	.fb_fillrect  = lcdpi_fillrect,
	.fb_copyarea  = lcdpi_copyarea,
	.fb_imageblit = lcdpi_imageblit,
	.fb_blank     = lcdpi_blank,
	.fb_setcolreg = lcdpi_setcolreg,
  	.fb_mmap      = fb_deferred_io_mmap,
};

static struct fb_deferred_io lcdpi_defio = {
	.delay = HZ / 20,
	.deferred_io = lcdpi_deferred_io,
};

static int lcdpi_probe(struct spi_device *spi)
{
	struct lcdpi *item;
	struct fb_info *info;
	int ret;

	item = devm_kzalloc(&spi->dev, sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;

	item->dev = &spi->dev;
	item->spi = spi;
	item->backlight = 1;
	spi_set_drvdata(spi, item);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi->max_speed_hz = sclk;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "spi_setup failed\n");

	item->txbuf = devm_kmalloc(&spi->dev, LCDPI_TX_LEN, GFP_KERNEL);
	if (!item->txbuf)
		return -ENOMEM;

	info = framebuffer_alloc(0, &spi->dev);
	if (!info)
		return -ENOMEM;

	item->info = info;
	info->par = item;
	info->fbops = &lcdpi_fbops;

	info->flags = FBINFO_VIRTFB;
	//info->flags = FBINFO_DEFAULT | FBINFO_VIRTFB;
	info->pseudo_palette = devm_kcalloc(&spi->dev, 16, sizeof(u32), GFP_KERNEL);
	if (!info->pseudo_palette) {
		ret = -ENOMEM;
		goto err_fb_release;
	}

	item->vmem = vzalloc(LCDPI_FB_SIZE);
	if (!item->vmem) {
		ret = -ENOMEM;
		goto err_fb_release;
	}

	strscpy(info->fix.id, "4dpi32nc", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.line_length = LCDPI_LINE_LEN;
	info->fix.smem_start = (unsigned long)item->vmem;
	info->fix.smem_len = LCDPI_FB_SIZE;

	info->var.xres = LCDPI_XRES;
	info->var.yres = LCDPI_YRES;
	info->var.xres_virtual = LCDPI_XRES;
	info->var.yres_virtual = LCDPI_YRES;
	info->var.bits_per_pixel = LCDPI_BPP;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	info->screen_base = (char __iomem *)item->vmem;
	info->screen_size = LCDPI_FB_SIZE;

	info->fbdefio = &lcdpi_defio;
	fb_deferred_io_init(info);

	ret = ili9341_32_setup(item);
	if (ret)
		goto err_defio;

	ret = register_framebuffer(info);
	if (ret)
		goto err_defio;

	lcdpi_update_all(item); //431行目から移動

	dev_info(&spi->dev, "4DPi-32 non-compressed framebuffer registered as fb%d\n", info->node);
	return 0;

err_defio:
	fb_deferred_io_cleanup(info);
	vfree(item->vmem);
err_fb_release:
	framebuffer_release(info);
	return ret;
}

static void lcdpi_remove(struct spi_device *spi)
{
	struct lcdpi *item = spi_get_drvdata(spi);

	if (!item || !item->info)
		return;

	unregister_framebuffer(item->info);
	fb_deferred_io_cleanup(item->info);
	vfree(item->vmem);
	framebuffer_release(item->info);
}

static const struct of_device_id lcdpi_of_match[] = {
	{ .compatible = "4dsystems,4dpi-32-nocompress" },
	{ .compatible = "4dsystems,4dpi-32" },
	{ }
};
MODULE_DEVICE_TABLE(of, lcdpi_of_match);

static struct spi_driver lcdpi_driver = {
	.driver = {
		.name = "4dpi_nocompress",
		.of_match_table = lcdpi_of_match,
	},
	.probe = lcdpi_probe,
	.remove = lcdpi_remove,
};
module_spi_driver(lcdpi_driver);

MODULE_DESCRIPTION("4D Systems 4DPi-32 non-compressed framebuffer driver");
MODULE_AUTHOR("Generated for Raspberry Pi OS 6.x migration work");
MODULE_LICENSE("GPL");
