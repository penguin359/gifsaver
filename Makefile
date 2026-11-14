CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
LDLIBS    = -lX11 -lgif
PREFIX   ?= /usr/local
HACKDIR  ?= $(PREFIX)/libexec/xscreensaver

# Platform layer: x11 today; win32/macos planned.
PLATFORM ?= x11

# Portable core + one platform file.
SRCS = anim.c wav.c platform_$(PLATFORM).c
OBJS = $(SRCS:.c=.o)

# Link libasound only when audio support is enabled in config.h.
AUDIO := $(shell grep -Eq '^[[:space:]]*\#[[:space:]]*define[[:space:]]+GIFSAVER_AUDIO[[:space:]]+1' config.h 2>/dev/null && echo 1)
ifeq ($(AUDIO),1)
LDLIBS += -lasound
endif

all: gifsaver

gifsaver: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): gifsaver.h config.h

install: gifsaver
	install -d $(DESTDIR)$(HACKDIR)
	install -m 755 gifsaver $(DESTDIR)$(HACKDIR)/gifsaver

clean:
	rm -f gifsaver *.o

.PHONY: all install clean
