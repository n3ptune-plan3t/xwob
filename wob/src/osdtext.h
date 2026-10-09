/* osdtext - tiny dependency-free text renderer for OSD percentages.
 *
 * Renders digits and '%' from a built-in anti-aliased stroke font into an
 * 8-bit coverage mask, so it works with no font files, no FreeType and no
 * file-system access (important for sandboxed / seccomp'd programs).
 *
 * Also provides the "color aware" helpers: pick black or white text that
 * contrasts with whatever is underneath.
 */
#ifndef OSDTEXT_H
#define OSDTEXT_H

#include <stdint.h>

struct osd_rgba {
	float r, g, b, a; /* straight (non-premultiplied), 0.0 - 1.0 */
};

/* Size in pixels of the mask needed to draw `text` with a cap-height of
 * `height` pixels (digits are `height` tall). */
void osd_text_measure(const char *text, int height, int *out_w, int *out_h);

/* Allocation-free variant: calls `put(x, y, coverage, ctx)` for every pixel
 * with non-zero coverage (0-255), within the w*h area from osd_text_measure().
 * Each pixel is reported at most once. */
typedef void (*osd_put_fn)(int x, int y, uint8_t coverage, void *ctx);
void osd_text_render_cb(const char *text, int height, osd_put_fn put, void *ctx);

/* Draw `text` into `mask` (w*h bytes, zero-initialised by this call).
 * Use osd_text_measure() to get w and h. 255 = fully covered. */
void osd_text_render(const char *text, int height, uint8_t *mask, int w, int h);

/* Perceived luminance (WCAG relative luminance) of an sRGB colour. */
float osd_luminance(float r, float g, float b);

/* Colour of whatever is visible under a (possibly translucent) colour.
 * Translucent colours are assumed to sit on a mid-dark backdrop. */
float osd_effective_luminance(struct osd_rgba under);

/* Black or white, whichever contrasts best with `under`. */
struct osd_rgba osd_contrast(struct osd_rgba under);

/* ---- label layout: pick a size; vertical bars stack the characters ---- */
struct osd_label {
	int height;  /* digit height in pixels */
	int stacked; /* 1 = upright characters, one per line (vertical bars) */
	int w, h;    /* footprint of the actual text */
};

/* Fit `text` into an avail_w x avail_h area. The size is derived from the
 * widest label ("100%") so it does not change as digits change. Vertical bars
 * always get the characters stacked upright, one per line (1/0/0/%).
 * Returns 0 if nothing legible fits. */
int osd_label_layout(const char *text, int avail_w, int avail_h, int bar_vertical, struct osd_label *out);

/* Like osd_text_render_cb() but honours the layout (incl. stacking); the
 * coordinates passed to `put` lie within out->w x out->h. */
void osd_label_render(const char *text, const struct osd_label *label, osd_put_fn put, void *ctx);

#endif
