CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
LDLIBS    = -lX11 -lgif
PREFIX   ?= /usr/local
HACKDIR  ?= $(PREFIX)/libexec/xscreensaver

# Optional local configuration; silently skipped when missing.
# Put feature settings there (e.g. "AUDIO = 0") to persist them.
-include config.mk

# --- Feature configuration -------------------------------------------
# Override on the command line (make AUDIO=0) or in config.mk.

# Audio support: loop a WAV (-sound) through ALSA. Needs libasound.
AUDIO     ?= 1
# Compile default_gif.h / default_wav.h (from embed2c.py) into the
# binary. EMBED_WAV requires AUDIO=1.
EMBED_GIF ?= 0
EMBED_WAV ?= 0

FEATURES = -DGIFSAVER_AUDIO=$(AUDIO) \
           -DGIFSAVER_EMBED_GIF=$(EMBED_GIF) \
           -DGIFSAVER_EMBED_WAV=$(EMBED_WAV)
CPPFLAGS += $(FEATURES)

ifeq ($(AUDIO),1)
LDLIBS += -lasound
endif

# ---------------------------------------------------------------------

# Platform layer: x11 today; win32/macos planned.
PLATFORM ?= x11

# Portable core + one platform file.
SRCS = anim.c wav.c platform_$(PLATFORM).c
OBJS = $(SRCS:.c=.o)

all: gifsaver

gifsaver: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): gifsaver.h config.h .features

# Rebuild everything when the feature set changes between runs.
.features: FORCE
	@echo '$(FEATURES)' | cmp -s - $@ || echo '$(FEATURES)' > $@
FORCE:

install: gifsaver
	install -d $(DESTDIR)$(HACKDIR)
	install -m 755 gifsaver $(DESTDIR)$(HACKDIR)/gifsaver

clean:
	rm -f gifsaver *.o .features

.PHONY: all install clean FORCE
