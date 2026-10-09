# xwob

One command that shows an overlay volume / brightness / progress bar on **both**
X11 and Wayland, now with a **percentage label whose colour follows what it is
drawn on**.

```
echo 50 | xwob
```

`xwob` looks at your session and starts the right program:

| Session | Runs | Source in this repo |
|---------|------|---------------------|
| Wayland (`WAYLAND_DISPLAY` set) | fork of [wob](https://github.com/francma/wob) | `wob/` |
| X11 (`DISPLAY` set)             | fork of [xob](https://github.com/florentc/xob) | `xob/` |

The two forks stay separate programs with their own config format; xwob adds a
small launcher, a single systemd socket/service, and one build. Input on stdin
and all options are handed to the program that gets started.

## What changed in the forks

* **`NN%` label** on the bar. Each pixel of the text is black or white,
  whichever contrasts with what is under it (fill colour or background), so the
  label stays readable on both sides of the fill edge.
  Horizontal bars show `45%`; **vertical bars stack the characters upright, one
  per line** (`4`, `5`, `%`).
  Built-in vector font: no font files, no FreeType (wob runs under seccomp).
  Turn off with `percentage = false;` (xob.cfg) / `show_percentage = no` (wob.ini).
* **xob:** fixed a crash when no config file exists anywhere (it now uses the
  built-in defaults).
* **wob:** understands xob's `50!` shorthand (uses a built-in `alt` style, which
  you can restyle with `[style.alt]`), so the same input lines work on both.

## Build and install

Dependencies: a C compiler, `make`, plus
* X11 backend: `libx11`, `libxrender`, `libconfig`
* Wayland backend: `meson`, `ninja`, `wayland`, `wayland-protocols`, `inih`, optionally `libseccomp`, `scdoc` (man pages)

```
make                      # both backends
make WITH_X11=no          # Wayland only
make WITH_WAYLAND=no      # X11 only
sudo make install PREFIX=/usr sysconfdir=/etc
```

Packagers: `DESTDIR`, `bindir`, `libexecdir`, `sysconfdir`, `unitdir`,
`WITH_SYSTEMD=no` are all honoured. The backends install as
`libexec/xwob/xwob-x11` and `libexec/xwob/xwob-wayland`, so they never clash
with a distribution's own `xob`/`wob` packages.

You can also run it straight from the source tree after `make`: `./bin/xwob`.

## Usage

```
xwob [-b auto|wayland|x11] [--print-backend] [backend options...]
```

* Input, one value per line on stdin: `50`, `50!` (alt colours), `50 style` (Wayland).
* `-b x11` / `XWOB_BACKEND=x11` forces a backend. Use it on a Wayland compositor
  that has no `wlr-layer-shell` (GNOME) to go through XWayland. Under XWayland
  both variables are set and `auto` prefers Wayland.
* Config is picked up automatically from `~/.config/xwob/xob.cfg` (X11) and
  `~/.config/xwob/wob.ini` (Wayland), then `/etc/xwob/`. `-c file` overrides.
  Formats are xob's (libconfig) and wob's (ini); see `examples/wob.ini`,
  `xob/styles.cfg`, `man xwob-x11`, `man xwob-wayland.ini`.

## systemd (the same way wob does it)

systemd owns a FIFO and starts xwob on the first write:

```
systemctl --user enable --now xwob.socket
systemctl --user import-environment WAYLAND_DISPLAY DISPLAY XAUTHORITY   # from your session startup
echo 50 > $XDG_RUNTIME_DIR/xwob.sock
```

Stop with `systemctl --user stop xwob.service` (the socket keeps listening and
will start it again on the next write) or `systemctl --user disable --now xwob.socket`
to remove it entirely. Do **not** also start xwob from your WM config.

The service has `ConditionEnvironment=|WAYLAND_DISPLAY` / `|DISPLAY`, so it runs
in either kind of session and is skipped when there is no display.

### Without systemd

```
mkfifo "$XDG_RUNTIME_DIR/xwob.sock"
tail -f "$XDG_RUNTIME_DIR/xwob.sock" | xwob &
```

(or run it under your supervisor of choice with the FIFO as stdin).

## Notes

* seccomp sandboxing is wob's and applies to the Wayland backend only.
* Layout and colours differ between the backends because they keep their own
  config: xob positions with `x`/`y`/`length`/`thickness`, wob with
  `anchor`/`margin`/`width`/`height`.
* Licensing: see `LICENSE.md` (xob is GPL-3.0, wob is ISC).
