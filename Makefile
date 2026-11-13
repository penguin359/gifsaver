CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lX11 -lgif
PREFIX  ?= /usr/local
HACKDIR ?= $(PREFIX)/libexec/xscreensaver

all: gifsaver

gifsaver: gifsaver.c
	$(CC) $(CFLAGS) -o $@ gifsaver.c $(LDLIBS)

install: gifsaver
	install -d $(DESTDIR)$(HACKDIR)
	install -m 755 gifsaver $(DESTDIR)$(HACKDIR)/gifsaver

clean:
	rm -f gifsaver

.PHONY: all install clean
