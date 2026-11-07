# SpinProj: make            (finds SDL2 and libdrm by itself; either can be missing)
#           make CROSS=aarch64-linux-gnu-   (or Buildroot sets CC / PKG_CONFIG)
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
# our own flags live apart from CFLAGS: Buildroot passes CFLAGS on the command line, which would
# silently drop anything added to it here (and with it HAVE_DRM)
SP_CFLAGS = -std=gnu11 -Wall -Wextra -Wno-unused-parameter
SP_LIBS = -llz4 -lpthread -lm

ifneq ($(NO_SDL),1)
ifneq ($(shell $(PKG_CONFIG) --exists sdl2 && echo y),)
SP_CFLAGS += -DHAVE_SDL $(shell $(PKG_CONFIG) --cflags sdl2)
SP_LIBS += $(shell $(PKG_CONFIG) --libs sdl2)
endif
endif
ifneq ($(NO_DRM),1)
ifneq ($(shell $(PKG_CONFIG) --exists libdrm && echo y),)
SP_CFLAGS += -DHAVE_DRM $(shell $(PKG_CONFIG) --cflags libdrm)
SP_LIBS += $(shell $(PKG_CONFIG) --libs libdrm)
endif
endif

SRC = spinproj.c out_null.c out_sdl.c out_drm.c

spinproj: $(SRC) out.h Makefile
	$(CC) $(CFLAGS) $(SP_CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(SP_LIBS) $(LDLIBS)

install: spinproj
	install -D -m 755 spinproj $(DESTDIR)/usr/bin/spinproj

clean:
	rm -f spinproj

.PHONY: install clean
