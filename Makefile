# mjpegtools: mjpegstore, mjpegmount
# Needs: gcc, libjpeg (libjpeg-dev), libfuse3 + pkg-config (mjpegmount only).
#   Debian/Ubuntu: apt install build-essential libjpeg-dev libfuse3-dev pkg-config
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
MANDIR  ?= $(PREFIX)/share/man
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS   := $(shell pkg-config --libs fuse3 2>/dev/null)
BINS = mjpegstore mjpegmount

all: $(BINS)

mjpegstore: mjpegstore.c
	$(CC) $(CFLAGS) -pthread -o $@ $< -ljpeg -lm

mjpegmount: mjpegmount.c
	$(CC) $(CFLAGS) -pthread $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS) -ljpeg -lm

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BINS) $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 644 man/*.1 $(DESTDIR)$(MANDIR)/man1

clean:
	rm -f $(BINS)

.PHONY: all install clean
