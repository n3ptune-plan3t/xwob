# Licensing

xwob is a repository that carries two separate programs plus glue:

| Path        | What                                              | License        |
|-------------|---------------------------------------------------|----------------|
| `xob/`      | fork of xob (X11 backend)                         | GPL-3.0 (`xob/LICENSE`) |
| `wob/`      | fork of wob (Wayland backend)                     | ISC (`wob/LICENSE`)     |
| `osdtext.*`, `glyphs.inc` (copies in both `xob/src` and `wob/src`) | % label renderer | each copy follows the license of the program it sits in |
| everything else at top level (`bin/`, `systemd/`, `Makefile`, `doc/`, `examples/`) | launcher and packaging | ISC |

The programs are only ever run as separate processes (the launcher `exec`s one
of them), so the licenses do not mix.
