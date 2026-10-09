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

#define _XOPEN_SOURCE 500

#include "main.h"
#include "conf.h"
#include "display.h"
#include "iniconf.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int cap = 100;
    int timeout = 1000;
    bool cap_from_cli = false;
    bool timeout_from_cli = false;

    char *arg_config_file_path = NULL;
    char *style_name = DEFAULT_STYLE;

    /* Command-line arguments */
    int opt;
    while ((opt = getopt(argc, argv, "m:t:c:s:qvh")) != -1)
    {
        switch (opt)
        {
        case 'm':
            cap = atoi(optarg);
            cap_from_cli = true;
            if (cap <= 0)
            {
                fprintf(
                    stderr,
                    "Invalid cap (maximum value): must be a natural number.\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 't':
            timeout = atoi(optarg);
            timeout_from_cli = true;
            if (timeout < 0)
            {
                fprintf(stderr, "Invalid timeout: must be a natural number.\n");
                exit(EXIT_FAILURE);
            }
            else if (timeout == 0)
            {
                fprintf(stderr, "Info: no timeout, the bar will "
                                "remain on-screen.\n");
            }
            else if (timeout < 100)
            {
                fprintf(
                    stderr,
                    "Warning: timeout is low, the bar may not be visible.\n");
            }
            break;
        case 'c':
            arg_config_file_path = optarg;
            break;
        case 's':
            style_name = optarg;
            break;
        case 'q':
            freopen("/dev/null", "w", stdout);
            break;
        case 'v':
            printf("Version %s\n", VERSION_NUMBER);
            exit(EXIT_SUCCESS);
            break;
        default:
            fprintf(stderr,
                    "Usage: %s [-m maximum] [-t timeout] [-c configfile] [-s "
                    "style]\n\n",
                    argv[0]);
            fprintf(stderr, "    -m <non-zero natural>"
                            " maximum value (0 is always the minimum)\n");
            fprintf(stderr,
                    "    -t <natural>         "
                    " duration in milliseconds between an update and the "
                    "vanishing of the bar "
                    "after an update or 0 if always on screen\n");
            fprintf(stderr, "    -c <filepath>        "
                            " configuration file specifying styles\n");
            fprintf(stderr, "    -s <style name>      "
                            " style to use from the configuration file\n");
            fprintf(stderr, "    -q                   "
                            " suppress all normal output\n");
            fprintf(stderr, "    -v                   "
                            " display version number\n");
            exit(EXIT_FAILURE);
        }
    }

    /* Style */
    FILE *config_file = NULL;
    Style style = unified_default_style();
    char real_config_file_path[PATH_MAX];

    /* Case #1: config file in argument */
    if (arg_config_file_path != NULL)
    {
        if (realpath(arg_config_file_path, real_config_file_path) != NULL)
            config_file = fopen(real_config_file_path, "r");
        else
        {
            fprintf(stderr,
                    "Error: could not open specified configuration file.\n");
            fprintf(stderr,
                    "Info: falling back to standard configuration files.\n");
        }
    }

    /* Cases #2-#4: the unified xwob.ini shared with the Wayland backend,
     * then the legacy xob styles.cfg. In each, the user directory
     * ($XDG_CONFIG_HOME, else ~/.config) comes before the system one. */
    if (config_file == NULL)
    {
        static const char *const names[][2] = {
            {UNIFIED_CONFIG_APPNAME, UNIFIED_CONFIG_FILENAME},
            {DEFAULT_CONFIG_APPNAME, DEFAULT_CONFIG_FILENAME}};
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        char candidate[PATH_MAX];

        for (size_t i = 0; i < 2 && config_file == NULL; i++)
        {
            for (int where = 0; where < 3 && config_file == NULL; where++)
            {
                int n = -1;
                if (where == 0 && xdg != NULL && *xdg != '\0')
                    n = snprintf(candidate, PATH_MAX, "%s/%s/%s", xdg,
                                 names[i][0], names[i][1]);
                else if (where == 1 && home != NULL)
                    n = snprintf(candidate, PATH_MAX, "%s/.config/%s/%s", home,
                                 names[i][0], names[i][1]);
                else if (where == 2)
                    n = snprintf(candidate, PATH_MAX, "%s/%s/%s", SYSCONFDIR,
                                 names[i][0], names[i][1]);
                if (n > 0 && n < PATH_MAX &&
                    realpath(candidate, real_config_file_path) != NULL)
                    config_file = fopen(real_config_file_path, "r");
            }
        }
    }

    /* Parsing the config file (none found: keep the built-in defaults).
     * *.cfg is the legacy libconfig format; anything else is xwob.ini. */
    if (config_file != NULL)
    {
        size_t len = strlen(real_config_file_path);
        bool legacy = len >= 4 && strcmp(real_config_file_path + len - 4,
                                         ".cfg") == 0;
        printf("Info: reading configuration from %s.\n",
               real_config_file_path);
        if (legacy)
        {
            style = parse_style_config(config_file, style_name,
                                       DEFAULT_CONFIGURATION);
        }
        else
        {
            int file_cap = cap, file_timeout = timeout;
            bool ok = parse_unified_config(config_file, real_config_file_path,
                                           &style, &file_cap, &file_timeout);
            if (!ok)
            {
                fclose(config_file);
                exit(EXIT_FAILURE);
            }
            /* command-line -m and -t win over the file */
            if (!cap_from_cli)
                cap = file_cap;
            if (!timeout_from_cli)
                timeout = file_timeout;
        }
        fclose(config_file);
    }
    else
    {
        fprintf(stderr,
                "Info: no configuration file found, using built-in defaults.\n");
    }

    /* Display */
    bool displayed = false;
    bool listening = true;
    Input_value input_value;
    Display_context display_context = init(style);

    if (display_context.x.display == NULL)
    {
        fprintf(stderr, "Error: Cannot open display\n");
        exit(EXIT_FAILURE);
    }
    else
    {
        fd_set fds;
        struct timeval tv;

        /* Main loop */
        while (listening)
        {
            /* Waiting for input on stdin or time to hide the gauge */
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            tv.tv_sec = timeout / 1000;
            tv.tv_usec = 1000 * (timeout % 1000);
            switch (select(1, &fds, NULL, NULL,
                           /* No timeout if already hidden */
                           displayed && timeout > 0 ? &tv : NULL))
            {
            case -1:
                perror("select()");
                exit(EXIT_FAILURE);
            case 0:
                /* Time to hide the gauge */
                display_context = hide(display_context);
                displayed = false;
                break;
            default:
                /* Update display using new input value */
                input_value = parse_input();
                if (input_value.valid)
                {
                    display_context =
                        show(display_context, input_value.value, cap,
                             style.overflow, input_value.show_mode);
                    printf("Update: %d/%d %s\n", input_value.value, cap,
                           (input_value.show_mode == ALTERNATIVE) ? "[ALT]"
                                                                  : "");
                    displayed = true;
                    listening = true;
                }
                else
                {
                    /* Stop after unexpected input */
                    listening = false;
                }
                break;
            }
        }

        /* Clean the memory */
        display_context_destroy(display_context);
    }
    return EXIT_SUCCESS;
}

Input_value parse_input(void)
{
    Input_value input_value;
    char altflag;

    input_value.valid = false;

    if (scanf("%d", &(input_value.value)) > 0)
    {
        /* Checking for the "alternative mode" flag : '!' */
        if (scanf("%c", &altflag) > 0 && altflag == '!')
            input_value.show_mode = ALTERNATIVE;
        else
            input_value.show_mode = NORMAL;

        input_value.valid = true;
    }

    return input_value;
}
