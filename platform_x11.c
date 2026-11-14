/*
 * platform_x11.c - Linux/X11 platform layer for gifsaver.
 *
 * Copyright (c) 2026 Loren M. Lang
 * SPDX-License-Identifier: MIT
 *
 * An xscreensaver-compatible hack that tiles the decoded animation
 * across the entire screen, optionally looping a WAV through ALSA.
 *
 * Usage:
 *   gifsaver [-root] [-window-id 0xNNNNNN] [-delay ms] [-window]
 *            [-background color] [-sound file.wav] file.gif
 *
 * When run by xscreensaver, the target window is taken from the
 * XSCREENSAVER_WINDOW environment variable (or -window-id). With -root
 * it draws on the (virtual) root window. With -window (the default when
 * run by hand) it opens its own top-level window for testing.
 *
 * Everything X11-, ALSA- and xscreensaver-specific lives here; GIF and
 * WAV handling are in the portable anim.c and wav.c. A future Win32 or
 * macOS port replaces only this file.
 *
 * Requires: libX11, giflib (>= 5.0).
 * Optional: ALSA (libasound) for -sound; see config.h.
 */

#include "config.h"
#include "gifsaver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#if GIFSAVER_AUDIO
#include <alsa/asoundlib.h>
#endif

#if GIFSAVER_EMBED_WAV && !GIFSAVER_AUDIO
#error "GIFSAVER_EMBED_WAV requires GIFSAVER_AUDIO (see config.h)"
#endif

const char *progname = "gifsaver";

typedef struct {
    Pixmap pixmap;      /* composed frame, canvas-sized */
    int delay_ms;       /* time to show this frame */
} XFrame;

static void die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", progname, msg);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Virtual root lookup (equivalent of xscreensaver's vroot.h), so     */
/* -root works under older xscreensaver setups and virtual WMs.       */
/* ------------------------------------------------------------------ */
static Window virtual_root(Display *dpy, int screen)
{
    Window root = RootWindow(dpy, screen);
    Atom swm_vroot = XInternAtom(dpy, "__SWM_VROOT", True);
    Window parent, *children = NULL;
    unsigned int nchildren, i;

    if (swm_vroot == None)
        return root;

    if (!XQueryTree(dpy, root, &root, &parent, &children, &nchildren))
        return root;

    for (i = 0; i < nchildren; i++) {
        Atom type;
        int format;
        unsigned long nitems, bytesafter;
        unsigned char *data = NULL;

        if (XGetWindowProperty(dpy, children[i], swm_vroot, 0, 1, False,
                               XA_WINDOW, &type, &format, &nitems,
                               &bytesafter, &data) == Success && data) {
            Window vroot = *(Window *) data;
            XFree(data);
            if (type == XA_WINDOW) {
                XFree(children);
                return vroot;
            }
        }
    }
    if (children)
        XFree(children);
    return root;
}

/* ------------------------------------------------------------------ */
/* Presentation: RGB frames to server-side Pixmaps, tiled drawing.    */
/* ------------------------------------------------------------------ */

/*
 * Convert one composed RGB canvas into a server-side Pixmap, going
 * through XPutPixel so any TrueColor/DirectColor depth works.
 */
static Pixmap canvas_to_pixmap(Display *dpy, Window win, Visual *visual,
                               int depth, const unsigned int *canvas,
                               int w, int h)
{
    XImage *img;
    Pixmap pm;
    GC gc;
    int x, y;
    unsigned long rmask = visual->red_mask;
    unsigned long gmask = visual->green_mask;
    unsigned long bmask = visual->blue_mask;
    int rshift = 0, gshift = 0, bshift = 0;
    int rbits = 0, gbits = 0, bbits = 0;
    unsigned long m;

    for (m = rmask; m && !(m & 1); m >>= 1) rshift++;
    for (; m & 1; m >>= 1) rbits++;
    for (m = gmask; m && !(m & 1); m >>= 1) gshift++;
    for (; m & 1; m >>= 1) gbits++;
    for (m = bmask; m && !(m & 1); m >>= 1) bshift++;
    for (; m & 1; m >>= 1) bbits++;

    img = XCreateImage(dpy, visual, depth, ZPixmap, 0, NULL, w, h, 32, 0);
    if (!img)
        die("XCreateImage failed");
    img->data = malloc((size_t) img->bytes_per_line * h);
    if (!img->data)
        die("out of memory");

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned int rgb = canvas[y * w + x];
            unsigned long r = (rgb >> 16) & 0xff;
            unsigned long g = (rgb >> 8) & 0xff;
            unsigned long b = rgb & 0xff;
            /* Scale 8-bit channels up or down to the visual's depth. */
            unsigned long rv = rbits >= 8 ? r << (rbits - 8) : r >> (8 - rbits);
            unsigned long gv = gbits >= 8 ? g << (gbits - 8) : g >> (8 - gbits);
            unsigned long bv = bbits >= 8 ? b << (bbits - 8) : b >> (8 - bbits);
            unsigned long pixel = ((rv << rshift) & rmask) |
                                  ((gv << gshift) & gmask) |
                                  ((bv << bshift) & bmask);
            XPutPixel(img, x, y, pixel);
        }
    }

    pm = XCreatePixmap(dpy, win, w, h, depth);
    gc = XCreateGC(dpy, pm, 0, NULL);
    XPutImage(dpy, pm, gc, img, 0, 0, 0, 0, w, h);
    XFreeGC(dpy, gc);
    XDestroyImage(img);        /* also frees img->data */
    return pm;
}

/* Upload every decoded frame as a Pixmap; the RGB data can go after. */
static XFrame *upload_frames(Display *dpy, Window win, Visual *visual,
                             int depth, const Animation *anim)
{
    XFrame *xframes = calloc((size_t) anim->nframes, sizeof *xframes);
    int i;

    if (!xframes)
        die("out of memory");
    for (i = 0; i < anim->nframes; i++) {
        xframes[i].delay_ms = anim->frames[i].delay_ms;
        xframes[i].pixmap = canvas_to_pixmap(dpy, win, visual, depth,
                                             anim->frames[i].rgb,
                                             anim->width, anim->height);
    }
    return xframes;
}

/* ------------------------------------------------------------------ */
/* Looping WAV playback through ALSA, fed non-blockingly from the     */
/* main event loop — no extra process or thread.                      */
/* ------------------------------------------------------------------ */

#if GIFSAVER_AUDIO

static struct {
    snd_pcm_t *pcm;             /* NULL when audio is off */
    WavClip clip;
    snd_pcm_uframes_t pos;      /* next frame to hand to ALSA */
    snd_pcm_uframes_t buffer;   /* device buffer size in frames */
} audio;

static void audio_stop(const char *why)
{
    if (why)
        fprintf(stderr, "%s: %s, continuing without sound\n",
                progname, why);
    if (audio.pcm) {
        snd_pcm_close(audio.pcm);
        audio.pcm = NULL;
    }
    wav_free(&audio.clip);
}

static snd_pcm_format_t alsa_format(WavFormat f)
{
    switch (f) {
    case WAV_FMT_U8:     return SND_PCM_FORMAT_U8;
    case WAV_FMT_S16LE:  return SND_PCM_FORMAT_S16_LE;
    case WAV_FMT_S24LE3: return SND_PCM_FORMAT_S24_3LE;
    case WAV_FMT_S32LE:  return SND_PCM_FORMAT_S32_LE;
    case WAV_FMT_F32LE:  return SND_PCM_FORMAT_FLOAT_LE;
    }
    return SND_PCM_FORMAT_UNKNOWN;
}

/*
 * Open a non-blocking ALSA stream for the clip. On success the audio
 * engine takes ownership of the clip; on failure the caller keeps it.
 * Any error only means the animation carries on without sound.
 */
static void audio_start(const WavClip *clip)
{
    snd_pcm_t *pcm;
    const char *device;
    int err;

    device = getenv("AUDIODEV");
    if (!device || !*device)
        device = "default";

    err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK,
                       SND_PCM_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "%s: cannot open audio device %s: %s\n",
                progname, device, snd_strerror(err));
        return;
    }
    err = snd_pcm_set_params(pcm, alsa_format(clip->format),
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             clip->channels, clip->rate,
                             1 /* allow resampling */,
                             500000 /* 0.5 s buffer */);
    if (err < 0) {
        fprintf(stderr, "%s: cannot configure audio device: %s\n",
                progname, snd_strerror(err));
        snd_pcm_close(pcm);
        return;
    }

    audio.pcm = pcm;
    audio.clip = *clip;
    audio.pos = 0;
    {
        snd_pcm_uframes_t bufsz = 0, period = 0;
        if (snd_pcm_get_params(pcm, &bufsz, &period) < 0 || !bufsz)
            bufsz = clip->rate / 2;     /* ~the requested 0.5 s */
        audio.buffer = bufsz;
    }
}

/*
 * Top up the ALSA buffer with as many frames as it will take right
 * now, wrapping around the WAV data for a seamless loop. Called from
 * the main event loop; never blocks.
 */
static void audio_pump(void)
{
    snd_pcm_uframes_t budget = audio.buffer;   /* bound work per call */

    while (audio.pcm && budget > 0) {
        snd_pcm_uframes_t chunk;
        snd_pcm_sframes_t n, avail;

        avail = snd_pcm_avail_update(audio.pcm);
        if (avail < 0) {
            if (snd_pcm_recover(audio.pcm, (int) avail, 1) < 0) {
                audio_stop("audio device lost");
                return;
            }
            continue;
        }
        if (avail == 0)
            return;

        chunk = (snd_pcm_uframes_t) avail;
        if (chunk > budget)
            chunk = budget;
        if (chunk > audio.clip.nframes - audio.pos)
            chunk = audio.clip.nframes - audio.pos;

        n = snd_pcm_writei(audio.pcm,
                           audio.clip.data + (size_t) audio.pos *
                                             audio.clip.frame_bytes,
                           chunk);
        if (n == -EAGAIN || n == 0)
            return;
        if (n < 0) {
            if (snd_pcm_recover(audio.pcm, (int) n, 1) < 0) {
                audio_stop("audio write failed");
                return;
            }
            continue;
        }
        audio.pos += (snd_pcm_uframes_t) n;
        budget -= (snd_pcm_uframes_t) n;
        if (audio.pos == audio.clip.nframes)
            audio.pos = 0;
    }
}

/* Load the requested (or built-in) clip and start playing it. */
static void audio_setup(const char *wav_file)
{
    WavClip clip;
    int have = 0;

    if (wav_file)
        have = wav_load_file(wav_file, &clip) == 0;
#if GIFSAVER_EMBED_WAV
    else
        have = wav_load_builtin(&clip) == 0;
#endif
    if (!have)
        return;
    audio_start(&clip);
    if (!audio.pcm)
        wav_free(&clip);
}

#else /* !GIFSAVER_AUDIO */

/* Stubs so the main loop needs no conditionals. */
static const struct { void *pcm; } audio = { NULL };

static void audio_setup(const char *wav_file)
{
    if (wav_file)
        fprintf(stderr, "%s: compiled without audio support "
                "(see config.h), -sound ignored\n", progname);
}

static void audio_pump(void)
{
}

#endif /* GIFSAVER_AUDIO */

/* ------------------------------------------------------------------ */

static void print_usage(FILE *out)
{
    fprintf(out,
#if GIFSAVER_EMBED_GIF
        "usage: %s [options] [file.gif]   (default: built-in GIF)\n"
#else
        "usage: %s [options] file.gif\n"
#endif
        "  -root              draw on the root window\n"
        "  -window-id 0xNNN   draw on an existing window (xscreensaver)\n"
        "  -window            open a test window (default)\n"
        "  -delay ms          override all frame delays\n"
        "  -background color  backdrop for transparent areas\n"
        "                     (X color name or #rrggbb)\n"
#if GIFSAVER_AUDIO
        "  -sound file.wav    loop a WAV file while animating\n"
#endif
        "  -help              show this help and exit\n"
        "  -version           show the version and exit\n"
        , progname);
}

static void usage(void)
{
    print_usage(stderr);
    exit(1);
}

int main(int argc, char **argv)
{
    Display *dpy;
    Window win = None;
    XWindowAttributes xgwa;
    GC gc;
    Animation anim;
    XFrame *frames;
    int nframes, cur = 0;
    int screen;
    int use_root = 0;
    int delay_override = -1;
    long bg_override = -1;
    const char *bg_name = NULL;
    const char *wav_file = NULL;
    const char *gif_file = NULL;
    const char *display_name = NULL;
    const char *env;
    int i, r;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-help") || !strcmp(argv[i], "--help") ||
            !strcmp(argv[i], "-h")) {
            print_usage(stdout);
            exit(0);
        } else if (!strcmp(argv[i], "-version") ||
                   !strcmp(argv[i], "--version")) {
            printf("%s %s\n", progname, GIFSAVER_VERSION);
            exit(0);
        } else if (!strcmp(argv[i], "-root")) {
            use_root = 1;
        } else if (!strcmp(argv[i], "-window")) {
            use_root = 0;
        } else if (!strcmp(argv[i], "-window-id") && i + 1 < argc) {
            win = (Window) strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-delay") && i + 1 < argc) {
            delay_override = atoi(argv[++i]);
        } else if ((!strcmp(argv[i], "-background") ||
                    !strcmp(argv[i], "-bg")) && i + 1 < argc) {
            bg_name = argv[++i];
        } else if (!strcmp(argv[i], "-sound") && i + 1 < argc) {
            wav_file = argv[++i];
        } else if ((!strcmp(argv[i], "-display") || !strcmp(argv[i], "-d"))
                   && i + 1 < argc) {
            display_name = argv[++i];
        } else if (argv[i][0] == '-') {
            usage();
        } else {
            gif_file = argv[i];
        }
    }

    if (!gif_file) {
        env = getenv("GIFSAVER_GIF");
        if (env && *env)
            gif_file = env;
#if !GIFSAVER_EMBED_GIF
        else
            usage();    /* with no embedded GIF, a file is mandatory */
#endif
    }

    dpy = XOpenDisplay(display_name);
    if (!dpy)
        die("cannot open display");
    screen = DefaultScreen(dpy);

    /* xscreensaver >= 5 passes the target window in the environment. */
    env = getenv("XSCREENSAVER_WINDOW");
    if (win == None && env && *env)
        win = (Window) strtoul(env, NULL, 0);

    if (win == None) {
        if (use_root) {
            win = virtual_root(dpy, screen);
        } else {
            /* Standalone test window. */
            XSetWindowAttributes attrs;
            attrs.background_pixel = BlackPixel(dpy, screen);
            win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0,
                                800, 600, 0, CopyFromParent, InputOutput,
                                CopyFromParent, CWBackPixel, &attrs);
            XStoreName(dpy, win, "gifsaver");
            XMapWindow(dpy, win);
        }
    }

    if (!XGetWindowAttributes(dpy, win, &xgwa))
        die("cannot query target window");

    {
        int class = xgwa.visual->class;
        if (class != TrueColor && class != DirectColor)
            die("only TrueColor/DirectColor visuals are supported");
    }

    if (bg_name) {
        XColor xc;
        if (!XParseColor(dpy, DefaultColormap(dpy, screen), bg_name, &xc)) {
            fprintf(stderr, "%s: unrecognized color \"%s\"\n",
                    progname, bg_name);
            exit(1);
        }
        bg_override = ((long) (xc.red >> 8) << 16) |
                      ((long) (xc.green >> 8) << 8) |
                      (long) (xc.blue >> 8);
    }

#if GIFSAVER_EMBED_GIF
    r = gif_file ? anim_load_file(gif_file, bg_override, &anim)
                 : anim_load_builtin(bg_override, &anim);
#else
    r = anim_load_file(gif_file, bg_override, &anim);
#endif
    if (r < 0)
        exit(1);

    if (delay_override >= 0)
        for (i = 0; i < anim.nframes; i++)
            anim.frames[i].delay_ms = delay_override;

    frames = upload_frames(dpy, win, xgwa.visual, xgwa.depth, &anim);
    nframes = anim.nframes;
    anim_free(&anim);           /* pixel data lives server-side now */

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFillStyle(dpy, gc, FillTiled);

    audio_setup(wav_file);

    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);

    for (;;) {
        struct timeval deadline, now;
        int xfd = ConnectionNumber(dpy);

        XSetTile(dpy, gc, frames[cur].pixmap);
        XFillRectangle(dpy, win, gc, 0, 0,
                       (unsigned) xgwa.width, (unsigned) xgwa.height);
        XFlush(dpy);

        gettimeofday(&deadline, NULL);
        deadline.tv_sec += frames[cur].delay_ms / 1000;
        deadline.tv_usec += (frames[cur].delay_ms % 1000) * 1000;
        if (deadline.tv_usec >= 1000000) {
            deadline.tv_sec++;
            deadline.tv_usec -= 1000000;
        }

        /*
         * Wait out the frame delay while keeping the ALSA buffer full
         * and staying responsive to events. With a 0.5 s audio buffer,
         * topping it up at least every 100 ms is comfortable.
         */
        for (;;) {
            struct timeval tv;
            long remaining_ms;
            fd_set fds;

            while (XPending(dpy)) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == ConfigureNotify) {
                    xgwa.width = ev.xconfigure.width;
                    xgwa.height = ev.xconfigure.height;
                } else if (ev.type == Expose && ev.xexpose.count == 0) {
                    XSetTile(dpy, gc, frames[cur].pixmap);
                    XFillRectangle(dpy, win, gc, 0, 0,
                                   (unsigned) xgwa.width,
                                   (unsigned) xgwa.height);
                } else if (ev.type == DestroyNotify &&
                           ev.xdestroywindow.window == win) {
                    exit(0);
                }
            }

            audio_pump();

            gettimeofday(&now, NULL);
            remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                           (deadline.tv_usec - now.tv_usec) / 1000;
            if (nframes > 1 && remaining_ms <= 0)
                break;          /* time for the next frame */

            if (nframes == 1 && !audio.pcm) {
                /* Static image, no sound: just block on events. */
                XEvent ev;
                XNextEvent(dpy, &ev);
                XPutBackEvent(dpy, &ev);
                continue;
            }

            if (nframes == 1 || (audio.pcm && remaining_ms > 100))
                remaining_ms = 100;     /* wake up to refill audio */
            tv.tv_sec = remaining_ms / 1000;
            tv.tv_usec = (remaining_ms % 1000) * 1000;

            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            select(xfd + 1, &fds, NULL, NULL, &tv);
        }
        cur = (cur + 1) % nframes;
    }
}
