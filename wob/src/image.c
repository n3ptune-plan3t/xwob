#define WOB_FILE "image.c"

#include <math.h>
#include <stdio.h>

#include "image.h"
#include "osdtext.h"

void
fill_rectangle(uint32_t *pixels, size_t width, size_t height, size_t stride, uint32_t color)
{
	for (size_t y = 0; y < height; ++y) {
		for (size_t x = 0; x < width; ++x) {
			pixels[x] = color;
		}
		pixels += stride;
	}
}

static struct osd_rgba
argb_premul_to_rgba(uint32_t p)
{
	float a = ((p >> 24) & 0xff) / 255.0f;
	struct osd_rgba out = {0, 0, 0, a};
	if (a > 0.0f) {
		out.r = ((p >> 16) & 0xff) / 255.0f / a;
		out.g = ((p >> 8) & 0xff) / 255.0f / a;
		out.b = (p & 0xff) / 255.0f / a;
		if (out.r > 1.0f) out.r = 1.0f;
		if (out.g > 1.0f) out.g = 1.0f;
		if (out.b > 1.0f) out.b = 1.0f;
	}
	return out;
}

struct label_ctx {
	uint32_t *image;
	size_t stride;
	int x0, y0;
};

static void
label_put(int x, int y, uint8_t cov, void *data)
{
	struct label_ctx *c = data;
	uint32_t *px = &c->image[(size_t) (c->y0 + y) * c->stride + (size_t) (c->x0 + x)];
	uint32_t under = *px;
	struct osd_rgba ink = osd_contrast(argb_premul_to_rgba(under));
	/* text is opaque: result = ink*c + under*(1-c), all premultiplied */
	float k = cov / 255.0f;
	float ua = ((under >> 24) & 0xff) / 255.0f;
	float ur = ((under >> 16) & 0xff) / 255.0f;
	float ug = ((under >> 8) & 0xff) / 255.0f;
	float ub = (under & 0xff) / 255.0f;
	float oa = k + ua * (1.0f - k);
	float orr = ink.r * k + ur * (1.0f - k);
	float og = ink.g * k + ug * (1.0f - k);
	float ob = ink.b * k + ub * (1.0f - k);
	*px = ((uint32_t) (oa * 255.0f + 0.5f) << 24) | ((uint32_t) (orr * 255.0f + 0.5f) << 16) |
	      ((uint32_t) (og * 255.0f + 0.5f) << 8) | (uint32_t) (ob * 255.0f + 0.5f);
}

/*
 * Draw "NN%" centred on the bar. The text colour is chosen per pixel from what
 * is already drawn underneath (bar colour vs. background colour), so the label
 * stays readable on both sides of the fill edge ("colour aware").
 */
static void
draw_percentage(uint32_t *image_data, struct wob_dimensions d, double percentage)
{
	size_t offset = d.border_offset + d.border_size + d.bar_padding;
	if (d.width <= 2 * offset || d.height <= 2 * offset) {
		return;
	}
	int bar_w = (int) (d.width - 2 * offset);
	int bar_h = (int) (d.height - 2 * offset);

	int pct = (int) lround(percentage * 100.0);
	char label[16];
	snprintf(label, sizeof(label), "%d%%", pct);

	struct osd_label lay;
	if (!osd_label_layout(label, bar_w, bar_h, d.orientation == WOB_ORIENTATION_VERTICAL, &lay)) {
		return; /* bar too small to hold a legible label */
	}

	struct label_ctx ctx = {
		.image = image_data,
		.stride = d.width,
		.x0 = (int) offset + (bar_w - lay.w) / 2,
		.y0 = (int) offset + (bar_h - lay.h) / 2,
	};
	/* No heap allocation here: the seccomp sandbox does not allow brk(). */
	osd_label_render(label, &lay, label_put, &ctx);
}

void
wob_image_draw(uint32_t *image_data, struct wob_dimensions dimensions, struct wob_colors colors, double percentage, bool show_percentage)
{
	uint32_t bar_color = wob_color_to_argb(wob_color_premultiply_alpha(colors.value));
	uint32_t background_color = wob_color_to_argb(wob_color_premultiply_alpha(colors.background));
	uint32_t border_color = wob_color_to_argb(wob_color_premultiply_alpha(colors.border));

	uint32_t *data;
	uint32_t height;
	uint32_t width;
	uint32_t offset;
	uint32_t stride = dimensions.width;

	height = dimensions.height;
	width = dimensions.width;
	data = image_data;
	fill_rectangle(data, width, height, stride, background_color);

	offset = dimensions.border_offset;
	height = dimensions.height - (2 * offset);
	width = dimensions.width - (2 * offset);
	data = image_data + (offset * (dimensions.width + 1));
	fill_rectangle(data, width, height, stride, border_color);

	offset = dimensions.border_offset + dimensions.border_size;
	height = dimensions.height - (2 * offset);
	width = dimensions.width - (2 * offset);
	data = image_data + (offset * (dimensions.width + 1));
	fill_rectangle(data, width, height, stride, background_color);

	offset = dimensions.border_offset + dimensions.border_size + dimensions.bar_padding;
	size_t bar_width = dimensions.width - 2 * offset;
	size_t bar_height = dimensions.height - 2 * offset;
	switch (dimensions.orientation) {
		case WOB_ORIENTATION_HORIZONTAL:
			height = bar_height;
			width = bar_width * percentage;
			data = image_data + (offset * (dimensions.width + 1));
			fill_rectangle(data, width, height, stride, bar_color);
			break;
		case WOB_ORIENTATION_VERTICAL:
			height = bar_height * percentage;
			width = bar_width;
			data = image_data + (offset * (dimensions.width + 1)) + (bar_height - height) * dimensions.width;
			fill_rectangle(data, width, height, stride, bar_color);
			break;
	}

	if (show_percentage) {
		draw_percentage(image_data, dimensions, percentage);
	}
}
