CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lX11 -lgif
PREFIX  ?= /usr/local
HACKDIR ?= $(PREFIX)/libexec/xscreensaver

# Link libasound only when audio support is enabled in config.h.
AUDIO := $(shell grep -Eq '^[[:space:]]*\#[[:space:]]*define[[:space:]]+GIFSAVER_AUDIO[[:space:]]+1' config.h 2>/dev/null && echo 1)
ifeq ($(AUDIO),1)
LDLIBS += -lasound
endif

all: gifsaver

gifsaver: gifsaver.c config.h
	$(CC) $(CFLAGS) -o $@ gifsaver.c $(LDLIBS)

install: gifsaver
	install -d $(DESTDIR)$(HACKDIR)
	install -m 755 gifsaver $(DESTDIR)$(HACKDIR)/gifsaver

clean:
	rm -f gifsaver

.PHONY: all install clean
