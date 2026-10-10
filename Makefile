# xwob - overlay bar for Wayland (wob fork) and X11 (xob fork) behind one command.
#
#   make                       build both backends
#   make WITH_X11=no           build only the Wayland backend
#   make WITH_WAYLAND=no       build only the X11 backend
#   make WITH_GUI=no           skip the Qt6 settings GUI (default: built if Qt6 is found)
#   make install [PREFIX=/usr] [DESTDIR=...]
#
# Package maintainers: libexecdir, sysconfdir, unitdir etc. are all overridable.

PREFIX      ?= /usr/local
prefix      ?= $(PREFIX)
bindir      ?= $(prefix)/bin
libexecdir  ?= $(prefix)/libexec
sysconfdir  ?= $(prefix)/etc
datarootdir ?= $(prefix)/share
mandir      ?= $(datarootdir)/man
unitdir     ?= $(prefix)/lib/systemd/user

WITH_X11     ?= yes
WITH_WAYLAND ?= yes
WITH_SYSTEMD ?= yes
# settings GUI (Qt6 Widgets): yes, no, or auto = build it when Qt6 is found
WITH_GUI     ?= auto

PKG_CONFIG ?= pkg-config
CXX        ?= g++
ifeq ($(WITH_GUI),auto)
WITH_GUI := $(if $(shell $(PKG_CONFIG) --exists Qt6Widgets && echo y),yes,no)
endif

MESON ?= meson
NINJA ?= ninja

TARGETS := bin/xwob
ifeq ($(WITH_X11),yes)
TARGETS += xob/xob
endif
ifeq ($(WITH_WAYLAND),yes)
TARGETS += wob/build/wob
endif
ifeq ($(WITH_GUI),yes)
TARGETS += gui/xwob-settings
endif

all: $(TARGETS)

bin/xwob: bin/xwob.in
	sed -e 's#@libexecdir@#$(libexecdir)#g' -e 's#@sysconfdir@#$(sysconfdir)#g' $< > $@
	chmod 755 $@

gui/xwob-settings: gui/xwob-settings.cpp
	$(CXX) $(CXXFLAGS) -std=c++17 -fPIC $(shell $(PKG_CONFIG) --cflags Qt6Widgets) $< -o $@ \
		$(LDFLAGS) $(shell $(PKG_CONFIG) --libs Qt6Widgets)

xob/xob: FORCE
	$(MAKE) -C xob sysconfdir=$(sysconfdir)

wob/build/wob: FORCE
	@[ -d wob/build ] || $(MESON) setup wob/build wob --prefix=$(prefix) --sysconfdir=$(sysconfdir) \
		-Dtests=disabled -Dsystemd-unit-files=disabled
	$(NINJA) -C wob/build

install: all
	install -Dm755 bin/xwob $(DESTDIR)$(bindir)/xwob
	install -Dm644 doc/xwob.1 $(DESTDIR)$(mandir)/man1/xwob.1
ifeq ($(WITH_X11),yes)
	install -Dm755 xob/xob $(DESTDIR)$(libexecdir)/xwob/xwob-x11
	install -Dm644 xob/doc/xob.1 $(DESTDIR)$(mandir)/man1/xwob-x11.1
endif
ifeq ($(WITH_WAYLAND),yes)
	install -Dm755 wob/build/wob $(DESTDIR)$(libexecdir)/xwob/xwob-wayland
	@[ ! -f wob/build/wob.1 ] || install -Dm644 wob/build/wob.1 $(DESTDIR)$(mandir)/man1/xwob-wayland.1
	@[ ! -f wob/build/wob.ini.5 ] || install -Dm644 wob/build/wob.ini.5 $(DESTDIR)$(mandir)/man5/xwob-wayland.ini.5
endif
ifeq ($(WITH_GUI),yes)
	install -Dm755 gui/xwob-settings $(DESTDIR)$(bindir)/xwob-settings
	install -Dm644 gui/xwob-settings.desktop $(DESTDIR)$(datarootdir)/applications/xwob-settings.desktop
	install -Dm644 doc/xwob-settings.1 $(DESTDIR)$(mandir)/man1/xwob-settings.1
endif
	install -Dm644 examples/xwob.ini $(DESTDIR)$(sysconfdir)/xwob/xwob.ini
ifeq ($(WITH_SYSTEMD),yes)
	install -Dm644 systemd/xwob.socket $(DESTDIR)$(unitdir)/xwob.socket
	sed 's#@bindir@#$(bindir)#g' systemd/xwob.service.in > systemd/xwob.service
	install -Dm644 systemd/xwob.service $(DESTDIR)$(unitdir)/xwob.service
endif

uninstall:
	rm -f $(DESTDIR)$(bindir)/xwob $(DESTDIR)$(mandir)/man1/xwob.1
	rm -rf $(DESTDIR)$(libexecdir)/xwob $(DESTDIR)$(sysconfdir)/xwob
	rm -f $(DESTDIR)$(bindir)/xwob-settings $(DESTDIR)$(datarootdir)/applications/xwob-settings.desktop $(DESTDIR)$(mandir)/man1/xwob-settings.1
	rm -f $(DESTDIR)$(mandir)/man1/xwob-x11.1 $(DESTDIR)$(mandir)/man1/xwob-wayland.1 $(DESTDIR)$(mandir)/man5/xwob-wayland.ini.5
	rm -f $(DESTDIR)$(unitdir)/xwob.socket $(DESTDIR)$(unitdir)/xwob.service

clean:
	$(MAKE) -C xob clean
	rm -rf wob/build bin/xwob systemd/xwob.service gui/xwob-settings

FORCE:
.PHONY: all install uninstall clean FORCE
