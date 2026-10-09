/* xwob - unified configuration for the X11 backend.
 *
 * The X11 backend (xob fork) reads the same file as the Wayland backend
 * (wob fork): xwob.ini, in wob.ini format. This module parses that file and
 * translates it into xob's native Style so both backends draw the same bar.
 *
 * This file is part of xob and is released under the same license (GPL-3.0+).
 */

#ifndef INICONF_H
#define INICONF_H

#include "conf.h"
#include <stdbool.h>
#include <stdio.h>

#define UNIFIED_CONFIG_APPNAME "xwob"
#define UNIFIED_CONFIG_FILENAME "xwob.ini"

/* Built-in defaults of the unified configuration. The Wayland backend uses
 * exactly the same values. */
#define UNIFIED_DEFAULT_MAX 100
#define UNIFIED_DEFAULT_TIMEOUT 1000

/* Style the bar has when no configuration file is given. */
Style unified_default_style(void);

/* Parses `file` (named `path` in messages) in wob.ini format on top of the
 * built-in unified defaults. `max` and `timeout` receive the values of the
 * "max" and "timeout" keys when present and are left untouched otherwise.
 * Returns false (after printing a message with the line number) on a syntax
 * error or an invalid value, exactly where the Wayland backend refuses the
 * file too. */
bool parse_unified_config(FILE *file, const char *path, Style *style, int *max,
                          int *timeout);

#endif /* INICONF_H */
