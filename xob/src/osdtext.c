#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "osdtext.h"

/* ---- glyph data (box 0..6 wide, 0..10 tall, y down) ---- */
struct osd_stroke {
	const float *pts; /* x,y pairs */
	int n;
};

struct osd_glyph {
	const struct osd_stroke *strokes;
	int nstrokes;
	float advance;
};

#include "glyphs.inc"

#define GLYPH_H 10.0f
#define STROKE_HALF 0.62f /* half stroke width in glyph units */

static const struct osd_glyph *
glyph_for(char c)
{
	if (c >= '0' && c <= '9') {
		return &osd_glyphs[c - '0'];
	}
	if (c == '%') {
		return &osd_glyphs[10];
	}
	return NULL;
}

static float
text_advance_units(const char *text)
{
	float w = 0;
	for (const char *p = text; *p; ++p) {
		const struct osd_glyph *g = glyph_for(*p);
		if (g) {
			w += g->advance;
		}
	}
	return w;
}

void
osd_text_measure(const char *text, int height, int *out_w, int *out_h)
{
	float scale = (float) height / GLYPH_H;
	float pad = STROKE_HALF * scale + 1.0f;
	float w = (text_advance_units(text) - 1.5f) * scale + 2 * pad;
	*out_w = (int) ceilf(w);
	*out_h = (int) ceilf(height + 2 * pad);
}

static float
seg_dist2(float px, float py, float ax, float ay, float bx, float by)
{
	float dx = bx - ax, dy = by - ay;
	float l2 = dx * dx + dy * dy;
	float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
	t = t < 0 ? 0 : (t > 1 ? 1 : t);
	float cx = ax + t * dx - px, cy = ay + t * dy - py;
	return cx * cx + cy * cy;
}

static void
render_glyph(const struct osd_glyph *g, float pen, float ytop, float scale, int w, int h, int x_min, osd_put_fn put, void *ctx)
{
	float half = STROKE_HALF * scale;
	int x0 = (int) floorf(pen - half - 1);
	int x1 = (int) ceilf(pen + 6.0f * scale + half + 1);
	int y0 = (int) floorf(ytop - half - 1);
	int y1 = (int) ceilf(ytop + GLYPH_H * scale + half + 1);
	if (x0 < x_min) x0 = x_min;
	if (x1 > w) x1 = w;
	if (y0 < 0) y0 = 0;
	if (y1 > h) y1 = h;

	for (int y = y0; y < y1; ++y) {
		for (int x = x0; x < x1; ++x) {
			float gx = ((float) x + 0.5f - pen) / scale;
			float gy = ((float) y + 0.5f - ytop) / scale;
			float best = 1e9f;
			for (int s = 0; s < g->nstrokes; ++s) {
				const float *pt = g->strokes[s].pts;
				for (int i = 0; i + 1 < g->strokes[s].n; ++i) {
					float d = seg_dist2(gx, gy, pt[2 * i], pt[2 * i + 1], pt[2 * i + 2], pt[2 * i + 3]);
					if (d < best) best = d;
				}
			}
			float cov = half - sqrtf(best) * scale + 0.5f; /* 1px wide AA ramp */
			cov = cov < 0 ? 0 : (cov > 1 ? 1 : cov);
			int v = (int) (cov * 255.0f + 0.5f);
			if (v > 0) {
				put(x, y, (uint8_t) v, ctx);
			}
		}
	}
}

void
osd_text_render_cb(const char *text, int height, osd_put_fn put, void *ctx)
{
	int w, h;
	osd_text_measure(text, height, &w, &h);

	float scale = (float) height / GLYPH_H;
	float pad = STROKE_HALF * scale + 1.0f;
	float pen = pad;
	int next_x = 0; /* first column not yet owned by a previous glyph */

	for (const char *p = text; *p; ++p) {
		const struct osd_glyph *g = glyph_for(*p);
		if (!g) {
			continue;
		}
		render_glyph(g, pen, pad, scale, w, h, next_x, put, ctx);
		next_x = (int) ceilf(pen + 6.0f * scale + STROKE_HALF * scale + 1);
		if (next_x > w) next_x = w;
		pen += g->advance * scale;
	}
}

struct mask_ctx {
	uint8_t *mask;
	int w;
};

static void
mask_put(int x, int y, uint8_t cov, void *ctx)
{
	struct mask_ctx *m = ctx;
	m->mask[(size_t) y * m->w + x] = cov;
}

void
osd_text_render(const char *text, int height, uint8_t *mask, int w, int h)
{
	memset(mask, 0, (size_t) w * h);
	struct mask_ctx m = {mask, w};
	osd_text_render_cb(text, height, mask_put, &m);
}

#define MIN_LEGIBLE 6
#define LINE_PITCH 1.3f /* stacked mode: line spacing, in digit heights */

static int
glyph_count(const char *text)
{
	int n = 0;
	for (const char *p = text; *p; ++p) {
		if (glyph_for(*p)) n++;
	}
	return n;
}

/* footprint of `text` stacked one character per line */
static void
stacked_measure(const char *text, int height, int *out_w, int *out_h)
{
	float scale = (float) height / GLYPH_H;
	float pad = STROKE_HALF * scale + 1.0f;
	int n = glyph_count(text);
	*out_w = (int) ceilf(6.0f * scale + 2 * pad);
	*out_h = (int) ceilf((n > 0 ? (n - 1) * LINE_PITCH * height : 0) + height + 2 * pad);
}

static int
fit(int aw, int ah, int cap, int stacked)
{
	int tw, th;
	for (int hgt = cap; hgt >= MIN_LEGIBLE; --hgt) {
		if (stacked) {
			stacked_measure("100%", hgt, &tw, &th);
		} else {
			osd_text_measure("100%", hgt, &tw, &th);
		}
		if (tw <= aw - 2 && th <= ah - 2) {
			return hgt;
		}
	}
	return 0;
}

int
osd_label_layout(const char *text, int avail_w, int avail_h, int bar_vertical, struct osd_label *out)
{
	int hgt, stacked = 0;
	if (!bar_vertical) {
		hgt = fit(avail_w, avail_h, (int) (avail_h * 0.62f), 0);
	} else {
		/* vertical bar: upright characters, one per line, top to bottom */
		hgt = fit(avail_w, avail_h, avail_w * 2, 1);
		stacked = 1;
	}
	if (hgt < MIN_LEGIBLE) {
		return 0;
	}
	int tw, th;
	if (stacked) {
		stacked_measure(text, hgt, &tw, &th);
	} else {
		osd_text_measure(text, hgt, &tw, &th);
	}
	out->height = hgt;
	out->stacked = stacked;
	out->w = tw;
	out->h = th;
	return 1;
}

void
osd_label_render(const char *text, const struct osd_label *label, osd_put_fn put, void *ctx)
{
	if (!label->stacked) {
		osd_text_render_cb(text, label->height, put, ctx);
		return;
	}
	float scale = (float) label->height / GLYPH_H;
	float pad = STROKE_HALF * scale + 1.0f;
	int line = 0;
	for (const char *p = text; *p; ++p) {
		const struct osd_glyph *g = glyph_for(*p);
		if (!g) {
			continue;
		}
		/* centre the glyph's own ink box in the column */
		float minx = 1e9f, maxx = -1e9f;
		for (int s = 0; s < g->nstrokes; ++s) {
			for (int i = 0; i < g->strokes[s].n; ++i) {
				float x = g->strokes[s].pts[2 * i];
				if (x < minx) minx = x;
				if (x > maxx) maxx = x;
			}
		}
		float pen = label->w / 2.0f - (minx + maxx) / 2.0f * scale;
		float ytop = pad + line * LINE_PITCH * label->height;
		render_glyph(g, pen, ytop, scale, label->w, label->h, 0, put, ctx);
		line++;
	}
}

static float
srgb_to_linear(float c)
{
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

float
osd_luminance(float r, float g, float b)
{
	return 0.2126f * srgb_to_linear(r) + 0.7152f * srgb_to_linear(g) + 0.0722f * srgb_to_linear(b);
}

float
osd_effective_luminance(struct osd_rgba under)
{
	const float backdrop = 0.05f; /* assume a dark-ish desktop behind translucency */
	float l = osd_luminance(under.r, under.g, under.b);
	return under.a * l + (1.0f - under.a) * backdrop;
}

struct osd_rgba
osd_contrast(struct osd_rgba under)
{
	/* contrast(white) = 1.05/(L+0.05), contrast(black) = (L+0.05)/0.05
	 * they are equal at L ~= 0.179 */
	float l = osd_effective_luminance(under);
	struct osd_rgba out = {0, 0, 0, 1};
	if (l < 0.179f) {
		out.r = out.g = out.b = 1.0f;
	}
	return out;
}
