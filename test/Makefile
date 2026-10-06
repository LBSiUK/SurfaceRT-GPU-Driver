# Builds the test clients for armv7. Run it inside the grate-build
# container (see the README) or natively on the device:
#
#   make            # all three
#   make dri3_test  # just one
#
# dri3_test and flipdemo only need XCB; gltri_bench needs EGL, GLESv2
# and Xlib headers (mesa-dev, libx11-dev).

CFLAGS ?= -O2 -Wall
PKG_CONFIG ?= pkg-config

PROGS = dri3_test flipdemo gltri_bench

all: $(PROGS)

dri3_test: dri3_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) \
	    $(shell $(PKG_CONFIG) --cflags --libs xcb xcb-dri3 xcb-present xcb-sync)

flipdemo: flipdemo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) \
	    $(shell $(PKG_CONFIG) --cflags --libs xcb xcb-present)

gltri_bench: gltri_bench.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) \
	    $(shell $(PKG_CONFIG) --cflags --libs egl glesv2 x11) -lm

clean:
	rm -f $(PROGS)

.PHONY: all clean
