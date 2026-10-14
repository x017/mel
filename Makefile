PREFIX ?= /usr/local
CC     ?= cc
PKGS    = x11 xrandr cairo cairo-xlib
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -DPREFIX=\"$(PREFIX)\" $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS)) -lpam -lm
OBJ     = src/main.o src/config.o src/auth.o

mel: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

install: mel
	install -Dm755 mel $(DESTDIR)$(PREFIX)/bin/mel
	install -Dm644 mel.1 $(DESTDIR)$(PREFIX)/share/man/man1/mel.1
	install -d $(DESTDIR)$(PREFIX)/share/mel/themes
	install -m644 themes/* $(DESTDIR)$(PREFIX)/share/mel/themes/
	install -Dm644 config.example $(DESTDIR)$(PREFIX)/share/mel/config.example

clean:
	rm -f mel $(OBJ)
.PHONY: install clean
