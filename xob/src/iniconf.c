/* xwob - unified configuration for the X11 backend. See iniconf.h.
 *
 * The accepted keys, value syntax, validation and defaults mirror
 * wob/src/config.c so that one xwob.ini produces the same bar on X11 and on
 * Wayland. Keep the two in sync.
 *
 * This file is part of xob and is released under the same license (GPL-3.0+).
 */

#include "iniconf.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 512

/* Same limits as wob */
#define MIN_BAR_LENGTH 1
#define MAX_VALUE_LIMIT 10000UL

#define ANCHOR_TOP 1
#define ANCHOR_BOTTOM 2
#define ANCHOR_LEFT 4
#define ANCHOR_RIGHT 8

typedef struct
{
    unsigned long width;
    unsigned long height;
    unsigned long border_offset;
    unsigned long border_size;
    unsigned long bar_padding;
    Orientation orientation;
    unsigned anchor;
    unsigned long margin[4]; /* top, right, bottom, left */
} Geometry_cfg;

typedef struct
{
    Colors colors;
    Colors overflow;
    /* set when the style defines its own background, i.e. it no longer
     * follows the global background colour */
    bool bg_explicit;
    bool overflow_bg_explicit;
} Color_cfg;

typedef struct
{
    unsigned long max;
    unsigned long timeout;
    bool max_set;
    bool timeout_set;
    bool percentage;
    Overflow_mode overflow;
    Geometry_cfg geo;
    Color_cfg def; /* global keys */
    Color_cfg alt; /* [style.alt] (built-in unless redefined) */
} Ini_cfg;

static Color rgba(unsigned char r, unsigned char g, unsigned char b,
                  unsigned char a)
{
    Color c = {.red = r, .green = g, .blue = b, .alpha = a};
    return c;
}

static Colors colors_of(Color fg, Color bg, Color border)
{
    Colors c = {.fg = fg, .bg = bg, .border = border};
    return c;
}

/* Built-in defaults. Values are the ones documented in examples/xwob.ini. */
static void cfg_defaults(Ini_cfg *cfg)
{
    memset(cfg, 0, sizeof *cfg);

    cfg->max = UNIFIED_DEFAULT_MAX;
    cfg->timeout = UNIFIED_DEFAULT_TIMEOUT;
    cfg->percentage = true;
    cfg->overflow = WRAP;

    cfg->geo.width = 400;
    cfg->geo.height = 50;
    cfg->geo.border_offset = 4;
    cfg->geo.border_size = 4;
    cfg->geo.bar_padding = 4;
    cfg->geo.orientation = HORIZONTAL;
    cfg->geo.anchor = 0; /* center */

    Color bg = rgba(0x00, 0x00, 0x00, 0x90);
    cfg->def.colors = colors_of(rgba(0xff, 0xff, 0xff, 0xff), bg,
                                rgba(0xff, 0xff, 0xff, 0xff));
    cfg->def.overflow = colors_of(rgba(0xff, 0x00, 0x00, 0xff), bg,
                                  rgba(0xff, 0x00, 0x00, 0xff));
    cfg->alt.colors = colors_of(rgba(0x55, 0x55, 0x55, 0xff), bg,
                                rgba(0x55, 0x55, 0x55, 0xff));
    cfg->alt.overflow = colors_of(rgba(0x55, 0x00, 0x00, 0xff), bg,
                                  rgba(0x55, 0x00, 0x00, 0xff));
}

/* ---- value parsers (same grammar as wob) ------------------------------ */

static bool parse_number(const char *str, unsigned long *value)
{
    char *end;
    if (*str == '\0' || !isdigit((unsigned char)*str))
        return false;
    unsigned long ul = strtoul(str, &end, 10);
    if (*end != '\0')
        return false;
    *value = ul;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* RRGGBB or RRGGBBAA */
static bool parse_color(const char *str, Color *color)
{
    size_t len = strlen(str);
    int d[8];
    if (len != 6 && len != 8)
        return false;
    for (size_t i = 0; i < len; i++)
    {
        d[i] = hex_digit(str[i]);
        if (d[i] < 0)
            return false;
    }
    color->red = (unsigned char)(d[0] * 16 + d[1]);
    color->green = (unsigned char)(d[2] * 16 + d[3]);
    color->blue = (unsigned char)(d[4] * 16 + d[5]);
    color->alpha = len == 8 ? (unsigned char)(d[6] * 16 + d[7]) : 0xff;
    return true;
}

static bool parse_bool(const char *str, bool *value)
{
    if (!strcmp(str, "yes") || !strcmp(str, "true") || !strcmp(str, "1"))
        *value = true;
    else if (!strcmp(str, "no") || !strcmp(str, "false") || !strcmp(str, "0"))
        *value = false;
    else
        return false;
    return true;
}

static bool parse_margin(const char *str, unsigned long margin[4])
{
    char buf[MAX_LINE];
    unsigned long v[4] = {0};
    size_t n = 0;

    strncpy(buf, str, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " "))
    {
        if (n >= 4 || !parse_number(tok, &v[n]))
            return false;
        n++;
    }
    if (n == 4)
        memcpy(margin, v, sizeof v);
    else if (n == 1)
        margin[0] = margin[1] = margin[2] = margin[3] = v[0];
    else
        return false;
    return true;
}

static bool parse_anchor(const char *str, unsigned *anchor)
{
    char buf[MAX_LINE];
    strncpy(buf, str, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    *anchor = 0;
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " "))
    {
        if (!strcmp(tok, "left"))
            *anchor |= ANCHOR_LEFT;
        else if (!strcmp(tok, "right"))
            *anchor |= ANCHOR_RIGHT;
        else if (!strcmp(tok, "top"))
            *anchor |= ANCHOR_TOP;
        else if (!strcmp(tok, "bottom"))
            *anchor |= ANCHOR_BOTTOM;
        else if (strcmp(tok, "center"))
            return false;
    }
    return true;
}

/* ---- key handlers ----------------------------------------------------- */

static bool fail(const char *path, int line, const char *msg)
{
    fprintf(stderr, "Error: %s:%d: %s\n", path, line, msg);
    return false;
}

/* Colour keys, valid globally and inside [style.alt]. `which` selects the
 * colour set; is_default tells whether global background keys have to be
 * propagated to the style that follows them. */
static bool handle_color_key(Ini_cfg *cfg, Color_cfg *target, bool is_default,
                             const char *name, const char *value,
                             const char *path, int line, bool *known)
{
    Color c;
    Color *dst = NULL;
    const char *what = NULL;
    bool is_bg = false, is_overflow_bg = false;

    *known = true;
    if (!strcmp(name, "background_color"))
    {
        dst = &target->colors.bg;
        what = "Background color must be in RRGGBB[AA] format";
        is_bg = true;
    }
    else if (!strcmp(name, "border_color"))
    {
        dst = &target->colors.border;
        what = "Border color must be in RRGGBB[AA] format.";
    }
    else if (!strcmp(name, "bar_color"))
    {
        dst = &target->colors.fg;
        what = "Bar color must be in RRGGBB[AA] format.";
    }
    else if (!strcmp(name, "overflow_background_color"))
    {
        dst = &target->overflow.bg;
        what = "Overflow background color must be in RRGGBB[AA] format.";
        is_overflow_bg = true;
    }
    else if (!strcmp(name, "overflow_border_color"))
    {
        dst = &target->overflow.border;
        what = "Overflow border color must be in RRGGBB[AA] format.";
    }
    else if (!strcmp(name, "overflow_bar_color"))
    {
        dst = &target->overflow.fg;
        what = "Overflow bar color must be in RRGGBB[AA] format.";
    }
    else
    {
        *known = false;
        return true;
    }

    if (!parse_color(value, &c))
        return fail(path, line, what);
    *dst = c;

    if (is_default)
    {
        /* the built-in alt style follows the global backgrounds until it
         * defines its own */
        if (is_bg && !cfg->alt.bg_explicit)
            cfg->alt.colors.bg = c;
        if (is_overflow_bg && !cfg->alt.overflow_bg_explicit)
            cfg->alt.overflow.bg = c;
    }
    else
    {
        if (is_bg)
            target->bg_explicit = true;
        if (is_overflow_bg)
            target->overflow_bg_explicit = true;
    }
    return true;
}

static bool handle_global(Ini_cfg *cfg, const char *name, const char *value,
                          const char *path, int line)
{
    unsigned long ul;
    bool known;

    if (!strcmp(name, "max"))
    {
        if (!parse_number(value, &ul) || ul < 1 || ul > MAX_VALUE_LIMIT)
            return fail(path, line, "Maximum must be a value between 1 and "
                                    "10000.");
        cfg->max = ul;
        cfg->max_set = true;
        return true;
    }
    if (!strcmp(name, "timeout"))
    {
        if (!parse_number(value, &ul) || ul < 1 || ul > MAX_VALUE_LIMIT)
            return fail(path, line, "Timeout must be a value between 1 and "
                                    "10000.");
        cfg->timeout = ul;
        cfg->timeout_set = true;
        return true;
    }
    if (!strcmp(name, "width") || !strcmp(name, "height") ||
        !strcmp(name, "border_offset") || !strcmp(name, "border_size") ||
        !strcmp(name, "bar_padding"))
    {
        unsigned long *dst =
            !strcmp(name, "width")           ? &cfg->geo.width
            : !strcmp(name, "height")        ? &cfg->geo.height
            : !strcmp(name, "border_offset") ? &cfg->geo.border_offset
            : !strcmp(name, "border_size")   ? &cfg->geo.border_size
                                             : &cfg->geo.bar_padding;
        if (!parse_number(value, &ul))
            return fail(path, line, "Dimensions must be positive values.");
        *dst = ul;
        return true;
    }
    if (!strcmp(name, "margin"))
    {
        if (!parse_margin(value, cfg->geo.margin))
            return fail(path, line,
                        "Margin must be in format <value> or <value_top> "
                        "<value_right> <value_bottom> <value_left>.");
        return true;
    }
    if (!strcmp(name, "anchor"))
    {
        if (!parse_anchor(value, &cfg->geo.anchor))
            return fail(path, line,
                        "Anchor must be one of 'top', 'bottom', 'left', "
                        "'right', 'center'.");
        return true;
    }
    if (!strcmp(name, "orientation"))
    {
        if (!strcmp(value, "horizontal"))
            cfg->geo.orientation = HORIZONTAL;
        else if (!strcmp(value, "vertical"))
            cfg->geo.orientation = VERTICAL;
        else
            return fail(path, line, "Invalid argument for orientation. Valid "
                                    "options are horizontal and vertical");
        return true;
    }
    if (!strcmp(name, "show_percentage"))
    {
        if (!parse_bool(value, &cfg->percentage))
            return fail(path, line, "Invalid argument for show_percentage. "
                                    "Valid options are yes and no.");
        return true;
    }
    if (!strcmp(name, "overflow_mode"))
    {
        if (!strcmp(value, "wrap"))
            cfg->overflow = WRAP;
        else if (!strcmp(value, "nowrap"))
            cfg->overflow = HIDDEN;
        else
            return fail(path, line, "Invalid argument for overflow-mode. "
                                    "Valid options are wrap and nowrap.");
        return true;
    }
    if (!strcmp(name, "output_mode"))
    {
        fprintf(stderr, "Warning: %s:%d: output_mode was removed, now it "
                        "always behaves as \"focused\"\n", path, line);
        return fail(path, line, "output_mode is not supported.");
    }

    if (!handle_color_key(cfg, &cfg->def, true, name, value, path, line,
                          &known))
        return false;
    if (!known)
        fprintf(stderr, "Warning: %s:%d: Unknown config key %s\n", path, line,
                name);
    return true;
}

static bool handle_alt_style(Ini_cfg *cfg, const char *name, const char *value,
                             const char *path, int line)
{
    bool known;
    Color_cfg *alt = &cfg->alt;

    if (!handle_color_key(cfg, alt, false, name, value, path, line, &known))
        return false;
    if (!known)
        fprintf(stderr, "Warning: %s:%d: Unknown config key %s\n", path, line,
                name);
    return true;
}

/* ---- file parser ------------------------------------------------------ */

typedef enum
{
    SECTION_GLOBAL,
    SECTION_ALT,
    SECTION_IGNORED /* [output.*] and styles other than alt: Wayland only */
} Section;

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* inline comments: " ;" like inih */
static void strip_inline_comment(char *s)
{
    for (char *p = s; *p; p++)
    {
        if (*p == ';' && p > s && isspace((unsigned char)p[-1]))
        {
            *p = '\0';
            return;
        }
    }
}

static bool geometry_to_style(const Ini_cfg *cfg, const char *path, Style *style)
{
    const Geometry_cfg *g = &cfg->geo;
    unsigned long fat = g->border_offset + g->border_size + g->bar_padding;

    if (g->width < MIN_BAR_LENGTH + 2 * fat)
    {
        fprintf(stderr, "Error: %s: Invalid geometry: width is too small for "
                        "given parameters\n", path);
        return false;
    }
    if (g->height < MIN_BAR_LENGTH + 2 * fat)
    {
        fprintf(stderr, "Error: %s: Invalid geometry: height is too small for "
                        "given parameters\n", path);
        return false;
    }

    /* width and height are the size of the whole bar (outline included) and
     * are not swapped for vertical bars, as in wob */
    unsigned long inner_w = g->width - 2 * fat;
    unsigned long inner_h = g->height - 2 * fat;

    style->outline = (int)g->border_offset;
    style->border = (int)g->border_size;
    style->padding = (int)g->bar_padding;
    style->orientation = g->orientation;
    if (g->orientation == HORIZONTAL)
    {
        style->length = (Dim){.rel = 0.0, .abs = (int)inner_w};
        style->thickness = (int)inner_h;
    }
    else
    {
        style->length = (Dim){.rel = 0.0, .abs = (int)inner_h};
        style->thickness = (int)inner_w;
    }

    /* anchor + margin -> x/y position. Opposite anchors (or none) centre the
     * bar on that axis, which is what a layer-shell surface with an explicit
     * size does. */
    bool left = g->anchor & ANCHOR_LEFT, right = g->anchor & ANCHOR_RIGHT;
    bool top = g->anchor & ANCHOR_TOP, bottom = g->anchor & ANCHOR_BOTTOM;

    if (left && !right)
        style->x = (Dim){.rel = 0.0, .abs = (int)g->margin[3]};
    else if (right && !left)
        style->x = (Dim){.rel = 1.0, .abs = -(int)g->margin[1]};
    else
        style->x = (Dim){.rel = 0.5, .abs = 0};

    if (top && !bottom)
        style->y = (Dim){.rel = 0.0, .abs = (int)g->margin[0]};
    else if (bottom && !top)
        style->y = (Dim){.rel = 1.0, .abs = -(int)g->margin[2]};
    else
        style->y = (Dim){.rel = 0.5, .abs = 0};

    return true;
}

static void cfg_to_style(const Ini_cfg *cfg, Style *style)
{
    style->percentage = cfg->percentage;
    style->overflow = cfg->overflow;
    style->colorscheme.normal = cfg->def.colors;
    style->colorscheme.overflow = cfg->def.overflow;
    style->colorscheme.alt = cfg->alt.colors;
    style->colorscheme.altoverflow = cfg->alt.overflow;
}

Style unified_default_style(void)
{
    Ini_cfg cfg;
    Style style;

    cfg_defaults(&cfg);
    memset(&style, 0, sizeof style);
    geometry_to_style(&cfg, "built-in defaults", &style);
    cfg_to_style(&cfg, &style);
    return style;
}

bool parse_unified_config(FILE *file, const char *path, Style *style, int *max,
                          int *timeout)
{
    Ini_cfg cfg;
    char raw[MAX_LINE];
    Section section = SECTION_GLOBAL;
    int line = 0;

    cfg_defaults(&cfg);

    while (fgets(raw, sizeof raw, file) != NULL)
    {
        line++;
        if (strchr(raw, '\n') == NULL && !feof(file))
            return fail(path, line, "Line too long.");

        char *s = trim(raw);
        /* UTF-8 BOM on the first line */
        if (line == 1 && (unsigned char)s[0] == 0xEF &&
            (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3;
        if (*s == '\0' || *s == ';' || *s == '#')
            continue;

        if (*s == '[')
        {
            char *close = strchr(s, ']');
            if (close == NULL)
                return fail(path, line, "Malformed section header.");
            *close = '\0';
            const char *name = s + 1;
            if (!strcmp(name, "style.alt"))
                section = SECTION_ALT;
            else if (!strncmp(name, "output.", 7) ||
                     !strncmp(name, "style.", 6))
                section = SECTION_IGNORED;
            else
            {
                fprintf(stderr, "Warning: %s:%d: Unknown config section %s\n",
                        path, line, name);
                section = SECTION_IGNORED;
            }
            continue;
        }

        strip_inline_comment(s);
        char *eq = strchr(s, '=');
        if (eq == NULL)
            return fail(path, line, "Expected key = value.");
        *eq = '\0';
        char *name = trim(s);
        char *value = trim(eq + 1);

        bool ok = true;
        switch (section)
        {
        case SECTION_GLOBAL:
            ok = handle_global(&cfg, name, value, path, line);
            break;
        case SECTION_ALT:
            ok = handle_alt_style(&cfg, name, value, path, line);
            break;
        case SECTION_IGNORED:
            break;
        }
        if (!ok)
            return false;
    }

    if (!geometry_to_style(&cfg, path, style))
        return false;
    cfg_to_style(&cfg, style);

    if (cfg.max_set)
        *max = (int)cfg.max;
    if (cfg.timeout_set)
        *timeout = (int)cfg.timeout;
    return true;
}
