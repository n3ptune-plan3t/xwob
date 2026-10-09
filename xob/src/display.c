/* xob - A lightweight overlay volume/anything bar for the X Window System.
 * Copyright (C) 2021 Florent Ch.
 *
 * xob is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * xob is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with xob.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "display.h"
#include "osdtext.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep value in range */
static int fit_in(int value, int min, int max)
{
    value = (value < min) ? min : value;
    value = (value > max) ? max : value;
    return value;
}

/* Horizontal and vertical size depending on orientation */
static int size_x(Geometry_context g)
{
    return g.orientation == HORIZONTAL ? g.length : g.thickness;
}
static int size_y(Geometry_context g)
{
    return g.orientation == HORIZONTAL ? g.thickness : g.length;
}

/* Draw an empty bar with the given colors */
static void draw_empty(X_context x, Geometry_context g, Colors colors)
{
    /* Outline */
    fill_rectangle(x, colors.bg, 0, 0,
                   2 * (g.outline + g.border + g.padding) + size_x(g),
                   2 * (g.outline + g.border + g.padding) + size_y(g));
    /* Border */
    fill_rectangle(x, colors.border, g.outline, g.outline,
                   2 * (g.border + g.padding) + size_x(g),
                   2 * (g.border + g.padding) + size_y(g));
    /* Padding */
    fill_rectangle(x, colors.bg, g.outline + g.border, g.outline + g.border,
                   2 * g.padding + size_x(g), 2 * g.padding + size_y(g));
}

/* Draw a given length of filled bar with the given color */
static void draw_content(X_context x, Geometry_context g, int filled_length,
                         Color color)
{
    if (g.orientation == HORIZONTAL)
    {
        fill_rectangle(x, color, g.outline + g.border + g.padding,
                       g.outline + g.border + g.padding, filled_length,
                       g.thickness);
    }
    else
    {
        fill_rectangle(x, color, g.outline + g.border + g.padding,
                       g.outline + g.border + g.padding + g.length -
                           filled_length,
                       g.thickness, filled_length);
    }
}

/* Draw a separator (padding-sized gap) at the given position */
static void draw_separator(X_context x, Geometry_context g, int position,
                           Color color)
{
    if (g.orientation == HORIZONTAL)
    {
        fill_rectangle(
            x, color, g.outline + g.border + (g.padding / 2) + position,
            g.outline + g.border + g.padding, g.padding, g.thickness);
    }
    else
    {
        fill_rectangle(x, color, g.outline + g.border + g.padding,
                       g.outline + g.border + (g.padding / 2) + g.length -
                           position,
                       g.thickness, g.padding);
    }
}

/* ---- percentage label ---------------------------------------------------
 *
 * The label is composited into a small image (bar colours underneath, text on
 * top) and sent with XPutImage. For every text pixel the ink is picked from
 * what is under it (fill colour or background colour), so the label stays
 * readable on both sides of the fill edge: it is "color aware".
 */

typedef struct
{
    int fill_len;      /* filled length along the bar axis (0 = start) */
    int sep_lo;        /* proportional-overflow separator, [lo, hi) */
    int sep_hi;        /* lo == hi: no separator */
    int prop_len;      /* length filled with the "normal" colour in overflow */
    Colors colors;     /* colours in use */
    Colors prop_colors;/* colours for the proportional part */
    int overflow_prop; /* proportional overflow active */
} Fill_info;

static struct osd_rgba rgba_of(Color c)
{
    return (struct osd_rgba){.r = c.red / 255.0f,
                             .g = c.green / 255.0f,
                             .b = c.blue / 255.0f,
                             .a = c.alpha / 255.0f};
}

/* Colour drawn at position `a` along the bar axis (0 = start of the fill). */
static struct osd_rgba under_color(const Fill_info *f, Geometry_context g,
                                   int a)
{
    if (f->sep_hi > f->sep_lo && a >= f->sep_lo && a < f->sep_hi)
        return rgba_of(f->colors.bg);
    if (a >= 0 && a < f->fill_len)
    {
        if (f->overflow_prop && a < f->prop_len)
            return rgba_of(f->prop_colors.fg);
        return rgba_of(f->colors.fg);
    }
    (void)g;
    return rgba_of(f->colors.bg);
}

typedef struct
{
    uint32_t *px; /* premultiplied ARGB, w*h */
    int w;
    int x0, y0;   /* position of the label box in content coordinates */
    const Fill_info *fill;
    Geometry_context g;
} Label_ctx;

static uint32_t premul_argb(float r, float gr, float b, float a)
{
    unsigned ia = (unsigned)(a * 255.0f + 0.5f);
    unsigned ir = (unsigned)(r * a * 255.0f + 0.5f);
    unsigned ig = (unsigned)(gr * a * 255.0f + 0.5f);
    unsigned ib = (unsigned)(b * a * 255.0f + 0.5f);
    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

/* position of content-space pixel (cx, cy) along the bar axis */
static int axis_pos(Geometry_context g, int cx, int cy)
{
    return g.orientation == HORIZONTAL ? cx : g.length - 1 - cy;
}

static void label_put(int x, int y, uint8_t cov, void *data)
{
    Label_ctx *c = data;
    int cx = c->x0 + x, cy = c->y0 + y;
    struct osd_rgba under =
        under_color(c->fill, c->g, axis_pos(c->g, cx, cy));
    struct osd_rgba ink = osd_contrast(under);
    float k = cov / 255.0f;
    float oa = k + under.a * (1.0f - k);
    /* text is opaque; blend in premultiplied space */
    float r = ink.r * k + under.r * under.a * (1.0f - k);
    float gg = ink.g * k + under.g * under.a * (1.0f - k);
    float b = ink.b * k + under.b * under.a * (1.0f - k);
    unsigned ia = (unsigned)(oa * 255.0f + 0.5f);
    unsigned ir = (unsigned)(r * 255.0f + 0.5f);
    unsigned ig = (unsigned)(gg * 255.0f + 0.5f);
    unsigned ib = (unsigned)(b * 255.0f + 0.5f);
    c->px[(size_t)y * c->w + x] = (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

static unsigned pack_channel(unsigned v8, unsigned long mask)
{
    unsigned shift = 0, bits = 0;
    if (mask == 0)
        return 0;
    while (!((mask >> shift) & 1))
        shift++;
    while ((mask >> (shift + bits)) & 1)
        bits++;
    return (bits >= 8 ? (v8 << (bits - 8)) : (v8 >> (8 - bits))) << shift;
}

/* Send premultiplied-ARGB pixels to the window. Works for any TrueColor
 * visual: 32-bit alpha visuals keep alpha, others get straight RGB. */
static void put_pixels(X_context xc, int x, int y, int w, int h,
                       const uint32_t *argb)
{
    XWindowAttributes attrib;
    XGetWindowAttributes(xc.display, xc.window, &attrib);
    Visual *vis = attrib.visual;
    if (vis->class != TrueColor && vis->class != DirectColor)
        return;

    XImage *img = XCreateImage(xc.display, vis, attrib.depth, ZPixmap, 0,
                               NULL, w, h, 32, 0);
    if (img == NULL)
        return;
    img->data = calloc((size_t)img->bytes_per_line, h);
    if (img->data == NULL)
    {
        XFree(img);
        return;
    }

    unsigned long alpha_mask =
        ~(vis->red_mask | vis->green_mask | vis->blue_mask) & 0xffffffffUL;
    int has_alpha = attrib.depth == 32;

    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            uint32_t p = argb[(size_t)j * w + i];
            unsigned a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff,
                     b = p & 0xff;
            if (!has_alpha && a > 0 && a < 255)
            {
                r = r * 255 / a > 255 ? 255 : r * 255 / a;
                g = g * 255 / a > 255 ? 255 : g * 255 / a;
                b = b * 255 / a > 255 ? 255 : b * 255 / a;
            }
            unsigned long pixel = pack_channel(r, vis->red_mask) |
                                  pack_channel(g, vis->green_mask) |
                                  pack_channel(b, vis->blue_mask);
            if (has_alpha)
                pixel |= pack_channel(a, alpha_mask);
            XPutPixel(img, i, j, pixel);
        }
    }

    GC gc = XCreateGC(xc.display, xc.window, 0, NULL);
    XPutImage(xc.display, xc.window, gc, img, 0, 0, x, y, w, h);
    XFreeGC(xc.display, gc);
    XDestroyImage(img); /* frees img->data too */
}

static void draw_percentage(Display_context dc, int value, int cap,
                            Overflow_mode overflow_mode, Colors colors,
                            Colors prop_colors)
{
    Geometry_context g = dc.geometry;
    if (cap <= 0 || g.length <= 0)
        return;

    int bar_w = size_x(g), bar_h = size_y(g);
    int pct = (int)lround(100.0 * value / cap);
    char text[16];
    snprintf(text, sizeof text, "%d%%", pct);

    struct osd_label lay;
    if (!osd_label_layout(text, bar_w, bar_h, g.orientation == VERTICAL, &lay))
        return; /* bar too small for a legible label */

    Fill_info f = {.colors = colors, .prop_colors = prop_colors};
    f.fill_len = fit_in(value, 0, cap) * g.length / cap;
    if (value > cap && overflow_mode == PROPORTIONAL &&
        cap * g.length / value > g.padding)
    {
        int pos = cap * g.length / value;
        f.overflow_prop = 1;
        f.prop_len = pos;
        if (g.orientation == HORIZONTAL)
        {
            f.sep_lo = pos - (g.padding - g.padding / 2);
            f.sep_hi = pos + g.padding / 2;
        }
        else
        {
            f.sep_lo = pos - g.padding / 2;
            f.sep_hi = pos + (g.padding - g.padding / 2);
        }
    }

    Label_ctx ctx = {.w = lay.w,
                     .x0 = (bar_w - lay.w) / 2,
                     .y0 = (bar_h - lay.h) / 2,
                     .fill = &f,
                     .g = g};
    ctx.px = malloc((size_t)lay.w * lay.h * sizeof(uint32_t));
    if (ctx.px == NULL)
        return;

    /* start from the bar as drawn, then let the glyphs blend on top */
    for (int j = 0; j < lay.h; j++)
    {
        for (int i = 0; i < lay.w; i++)
        {
            struct osd_rgba u = under_color(
                &f, g, axis_pos(g, ctx.x0 + i, ctx.y0 + j));
            ctx.px[(size_t)j * lay.w + i] = premul_argb(u.r, u.g, u.b, u.a);
        }
    }
    osd_label_render(text, &lay, label_put, &ctx);

    int off = g.outline + g.border + g.padding;
    put_pixels(dc.x, off + ctx.x0, off + ctx.y0, lay.w, lay.h, ctx.px);
    free(ctx.px);
}

void compute_geometry(Style conf, Display_context *dc, int *topleft_x,
                      int *topleft_y, int *fat_layer, int *available_length)
{
    dc->geometry.outline = conf.outline;
    dc->geometry.border = conf.border;
    dc->geometry.padding = conf.padding;
    dc->geometry.thickness = conf.thickness;
    dc->geometry.orientation = conf.orientation;
    *fat_layer =
        dc->geometry.padding + dc->geometry.border + dc->geometry.outline;

    /* Orientation-related dimensions */
    *available_length = dc->geometry.orientation == HORIZONTAL
                            ? WidthOfScreen(dc->x.screen)
                            : HeightOfScreen(dc->x.screen);

    dc->geometry.length =
        fit_in(*available_length * conf.length.rel + conf.length.abs, 0,
               *available_length - 2 * *fat_layer);

    /* Compute position of the top-left corner */
    *topleft_x = fit_in(WidthOfScreen(dc->x.screen) * conf.x.rel -
                            (size_x(dc->geometry) + 2 * *fat_layer) / 2,
                        0,
                        WidthOfScreen(dc->x.screen) -
                            (size_x(dc->geometry) + 2 * *fat_layer)) +
                 conf.x.abs;
    *topleft_y = fit_in(HeightOfScreen(dc->x.screen) * conf.y.rel -
                            (size_y(dc->geometry) + 2 * *fat_layer) / 2,
                        0,
                        HeightOfScreen(dc->x.screen) -
                            (size_y(dc->geometry) + 2 * *fat_layer)) +
                 conf.y.abs;
}

/* PUBLIC Returns a new display context from a given configuration. If the
 * .x.display field of the returned display context is NULL, display could not
 * have been opened.*/
Display_context init(Style conf)
{
    Display_context dc;
    Depth dc_depth;
    Window root;
    int topleft_x;
    int topleft_y;
    int fat_layer;
    int available_length;
    XSetWindowAttributes window_attributes;
    static long window_attributes_flags =
        CWColormap | CWBorderPixel | CWOverrideRedirect;

    dc.x.display = XOpenDisplay(NULL);
    if (dc.x.display != NULL)
    {
        dc.x.screen_number = DefaultScreen(dc.x.display);
        dc.x.screen = ScreenOfDisplay(dc.x.display, dc.x.screen_number);
        root = RootWindow(dc.x.display, dc.x.screen_number);

        dc_depth = get_display_context_depth(dc);

        window_attributes.colormap =
            XCreateColormap(dc.x.display, root, dc_depth.visuals, AllocNone);
        window_attributes.border_pixel = 0;
        window_attributes.override_redirect = True;

        compute_geometry(conf, &dc, &topleft_x, &topleft_y, &fat_layer,
                         &available_length);

        /* Creation of the window */
        dc.x.window = XCreateWindow(
            dc.x.display, root, topleft_x, topleft_y,
            size_x(dc.geometry) + 2 * fat_layer,
            size_y(dc.geometry) + 2 * fat_layer, 0, dc_depth.depth, InputOutput,
            dc_depth.visuals, window_attributes_flags, &window_attributes);

        /* Set a WM_CLASS for the window */
        XClassHint *class_hint = XAllocClassHint();
        if (class_hint != NULL)
        {
            class_hint->res_name = DEFAULT_CONFIG_APPNAME;
            class_hint->res_class = DEFAULT_CONFIG_APPNAME;
            XSetClassHint(dc.x.display, dc.x.window, class_hint);
            XFree(class_hint);
        }

        /* The new window is not mapped yet */
        dc.x.mapped = False;

        /* Colorscheme */
        dc.colorscheme = conf.colorscheme;
        dc.percentage = conf.percentage;
    }

    return dc;
}

/* PUBLIC Cleans the X memory buffers. */
void display_context_destroy(Display_context dc)
{
    XCloseDisplay(dc.x.display);
}

/* PUBLIC Show a bar filled at value/cap in normal or alternative mode */
Display_context show(Display_context dc, int value, int cap,
                     Overflow_mode overflow_mode, Show_mode show_mode)
{
    Display_context newdc = dc;

    Colors colors;
    Colors colors_overflow_proportional;

    if (!dc.x.mapped)
    {
        XMapWindow(dc.x.display, dc.x.window);
        XRaiseWindow(dc.x.display, dc.x.window);
        newdc.x.mapped = True;
    }

    switch (show_mode)
    {
    case NORMAL:
        colors_overflow_proportional = dc.colorscheme.normal;
        if (value <= cap)
            colors = dc.colorscheme.normal;
        else
            colors = dc.colorscheme.overflow;
        break;

    case ALTERNATIVE:
        colors_overflow_proportional = dc.colorscheme.alt;
        if (value <= cap)
            colors = dc.colorscheme.alt;
        else
            colors = dc.colorscheme.altoverflow;
        break;
    }

    /* Empty bar */
    draw_empty(dc.x, dc.geometry, colors);

    /* Content */
    draw_content(dc.x, dc.geometry,
                 fit_in(value, 0, cap) * dc.geometry.length / cap, colors.fg);

    /* Proportional overflow : draw separator */
    if (value > cap && overflow_mode == PROPORTIONAL &&
        cap * dc.geometry.length / value > dc.geometry.padding)
    {
        draw_content(dc.x, dc.geometry, cap * dc.geometry.length / value,
                     colors_overflow_proportional.fg);
        draw_separator(dc.x, dc.geometry, cap * dc.geometry.length / value,
                       colors.bg);
    }

    /* Percentage label, drawn last so it sits on top of the bar */
    if (dc.percentage)
        draw_percentage(dc, value, cap, overflow_mode, colors,
                        colors_overflow_proportional);

    XFlush(dc.x.display);

    return newdc;
}

/* PUBLIC Hide the window */
Display_context hide(Display_context dc)
{
    Display_context newdc = dc;

    if (dc.x.mapped)
    {
        XUnmapWindow(dc.x.display, dc.x.window);
        newdc.x.mapped = False;
        XFlush(dc.x.display);
    }

    return newdc;
}
