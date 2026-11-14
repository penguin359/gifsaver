/*
 * gifsaver - an xscreensaver-compatible hack that tiles an animated GIF
 *            across the entire screen, optionally looping a WAV file.
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
 * Requires: libX11, giflib (>= 5.0).
 * Optional: ALSA (libasound) for -sound; see config.h.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <gif_lib.h>

#if GIFSAVER_AUDIO
#include <alsa/asoundlib.h>
#endif

#if GIFSAVER_EMBED_WAV && !GIFSAVER_AUDIO
#error "GIFSAVER_EMBED_WAV requires GIFSAVER_AUDIO (see config.h)"
#endif

#if GIFSAVER_EMBED_GIF
#include "default_gif.h"
#endif
#if GIFSAVER_EMBED_WAV
#include "default_wav.h"
#endif

typedef struct {
    Pixmap pixmap;      /* composed frame, canvas-sized */
    int delay_ms;       /* time to show this frame */
} Frame;

static const char *progname = "gifsaver";

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
/* GIF decoding: compose every frame onto a canvas-sized RGB buffer,  */
/* honoring transparency, interlacing and disposal methods.           */
/* ------------------------------------------------------------------ */

static const int interlace_offset[] = { 0, 4, 2, 1 };
static const int interlace_jump[]   = { 8, 8, 4, 2 };

/* Map a destination row index to the raster row for interlaced images. */
static void build_row_map(int height, int interlaced, int *map)
{
    int i, j, row = 0;
    if (!interlaced) {
        for (i = 0; i < height; i++)
            map[i] = i;
        return;
    }
    for (i = 0; i < 4; i++)
        for (j = interlace_offset[i]; j < height; j += interlace_jump[i])
            map[j] = row++;
}

static unsigned int color_of(const ColorMapObject *cmap, int idx)
{
    if (!cmap || idx < 0 || idx >= cmap->ColorCount)
        return 0;
    return ((unsigned int) cmap->Colors[idx].Red << 16) |
           ((unsigned int) cmap->Colors[idx].Green << 8) |
           ((unsigned int) cmap->Colors[idx].Blue);
}

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

#if GIFSAVER_EMBED_GIF
/* giflib read callback for decoding the embedded GIF from memory. */
typedef struct {
    const unsigned char *data;
    unsigned long len, pos;
} MemReader;

static int mem_read(GifFileType *gf, GifByteType *out, int n)
{
    MemReader *mr = gf->UserData;
    if ((unsigned long) n > mr->len - mr->pos)
        n = (int) (mr->len - mr->pos);
    memcpy(out, mr->data + mr->pos, (size_t) n);
    mr->pos += (unsigned long) n;
    return n;
}
#endif

/*
 * Decode `filename` (or the built-in GIF when filename is NULL) and
 * return an array of composed, canvas-sized frames as Pixmaps.
 * Returns the frame count; fills *out_frames. bg_override is an
 * 0xRRGGBB backdrop for transparent areas, or -1 to pick one
 * automatically.
 */
static int load_gif_frames(const char *filename, Display *dpy, Window win,
                           Visual *visual, int depth, long bg_override,
                           Frame **out_frames)
{
    int err = 0;
    const char *label = filename ? filename : "built-in GIF";
    GifFileType *gif = NULL;
    unsigned int *canvas, *prev, bg_color = 0;
    Frame *frames;
    int cw, ch, i, nframes;
#if GIFSAVER_EMBED_GIF
    MemReader mr = { gifsaver_default_gif, sizeof gifsaver_default_gif, 0 };

    if (!filename)
        gif = DGifOpen(&mr, mem_read, &err);
#endif
    if (filename)
        gif = DGifOpenFileName(filename, &err);

    if (!gif) {
        fprintf(stderr, "%s: cannot open %s: %s\n", progname, label,
                GifErrorString(err));
        exit(1);
    }
    if (DGifSlurp(gif) != GIF_OK) {
        fprintf(stderr, "%s: cannot decode %s: %s\n", progname, label,
                GifErrorString(gif->Error));
        exit(1);
    }

    cw = gif->SWidth;
    ch = gif->SHeight;
    nframes = gif->ImageCount;
    if (cw <= 0 || ch <= 0 || nframes <= 0)
        die("GIF has no usable frames");

    if (gif->SColorMap)
        bg_color = color_of(gif->SColorMap, gif->SBackGroundColor);

    /*
     * If the background color index is also the first frame's transparent
     * color (or there is no global colormap), its palette entry carries no
     * meaningful color.  Such GIFs are drawn by browsers over the page
     * behind them, which is usually light, so default to white; use
     * -background to override.
     */
    {
        GraphicsControlBlock gcb0;
        if (!gif->SColorMap ||
            (DGifSavedExtensionToGCB(gif, 0, &gcb0) == GIF_OK &&
             gcb0.TransparentColor == gif->SBackGroundColor))
            bg_color = 0xffffff;
    }
    if (bg_override >= 0)
        bg_color = (unsigned int) bg_override;

    canvas = malloc((size_t) cw * ch * sizeof *canvas);
    prev = malloc((size_t) cw * ch * sizeof *prev);
    frames = calloc((size_t) nframes, sizeof *frames);
    if (!canvas || !prev || !frames)
        die("out of memory");

    for (i = 0; i < cw * ch; i++)
        canvas[i] = bg_color;

    for (i = 0; i < nframes; i++) {
        SavedImage *si = &gif->SavedImages[i];
        GifImageDesc *desc = &si->ImageDesc;
        ColorMapObject *cmap = desc->ColorMap ? desc->ColorMap
                                              : gif->SColorMap;
        GraphicsControlBlock gcb;
        int transparent = NO_TRANSPARENT_COLOR;
        int disposal = DISPOSAL_UNSPECIFIED;
        int delay_ms = 100;
        int fx = desc->Left, fy = desc->Top;
        int fw = desc->Width, fh = desc->Height;
        int *rowmap;
        int x, y;

        if (DGifSavedExtensionToGCB(gif, i, &gcb) == GIF_OK) {
            transparent = gcb.TransparentColor;
            disposal = gcb.DisposalMode;
            delay_ms = gcb.DelayTime * 10;
        }
        if (delay_ms < 20)
            delay_ms = 100;    /* browsers' convention for 0/short delays */

        /* Clip frame rectangle to the canvas. */
        if (fx < 0 || fy < 0 || fx + fw > cw || fy + fh > ch) {
            fprintf(stderr, "%s: frame %d out of bounds, clipping\n",
                    progname, i);
            if (fx < 0) fx = 0;
            if (fy < 0) fy = 0;
            if (fw > cw - fx) fw = cw - fx;
            if (fh > ch - fy) fh = ch - fy;
        }

        if (disposal == DISPOSE_PREVIOUS)
            memcpy(prev, canvas, (size_t) cw * ch * sizeof *canvas);

        rowmap = malloc((size_t) fh * sizeof *rowmap);
        if (!rowmap)
            die("out of memory");
        build_row_map(fh, desc->Interlace, rowmap);

        for (y = 0; y < fh; y++) {
            const GifByteType *row =
                si->RasterBits + (size_t) rowmap[y] * desc->Width;
            for (x = 0; x < fw; x++) {
                int idx = row[x];
                if (idx == transparent)
                    continue;
                canvas[(fy + y) * cw + (fx + x)] = color_of(cmap, idx);
            }
        }
        free(rowmap);

        frames[i].delay_ms = delay_ms;
        frames[i].pixmap = canvas_to_pixmap(dpy, win, visual, depth,
                                            canvas, cw, ch);

        /* Prepare canvas for the next frame per the disposal method. */
        if (disposal == DISPOSE_BACKGROUND) {
            for (y = 0; y < fh; y++)
                for (x = 0; x < fw; x++)
                    canvas[(fy + y) * cw + (fx + x)] = bg_color;
        } else if (disposal == DISPOSE_PREVIOUS) {
            memcpy(canvas, prev, (size_t) cw * ch * sizeof *canvas);
        }
    }

    free(canvas);
    free(prev);
    DGifCloseFile(gif, &err);
    *out_frames = frames;
    return nframes;
}

/* ------------------------------------------------------------------ */
/* Looping WAV playback through ALSA, fed non-blockingly from the     */
/* main event loop — no extra process or thread.                      */
/* ------------------------------------------------------------------ */

#if GIFSAVER_AUDIO

typedef struct {
    snd_pcm_t *pcm;             /* NULL when audio is off */
    const unsigned char *data;  /* interleaved sample data */
    snd_pcm_uframes_t total;    /* total frames in the WAV */
    snd_pcm_uframes_t pos;      /* next frame to hand to ALSA */
    snd_pcm_uframes_t buffer;   /* device buffer size in frames */
    unsigned int frame_bytes;
} AudioState;

static AudioState audio;

static void audio_stop(const char *why)
{
    if (why)
        fprintf(stderr, "%s: %s, continuing without sound\n",
                progname, why);
    if (audio.pcm) {
        snd_pcm_close(audio.pcm);
        audio.pcm = NULL;
    }
}

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8);
}

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8) |
           ((unsigned int) p[2] << 16) | ((unsigned int) p[3] << 24);
}

#define WAVE_FORMAT_PCM        1
#define WAVE_FORMAT_IEEE_FLOAT 3
#define WAVE_FORMAT_EXTENSIBLE 0xfffe

/*
 * Parse an in-memory RIFF/WAVE image (`label` names it in messages)
 * and open a non-blocking ALSA stream for it. `buf` must stay alive
 * for the lifetime of playback. On any error the animation simply
 * carries on without sound (audio.pcm stays NULL).
 */
static void audio_start(const unsigned char *buf, unsigned long nbytes,
                        const char *label)
{
    unsigned long pos;
    unsigned int wformat = 0, channels = 0, rate = 0, bits = 0;
    const unsigned char *data = NULL;
    unsigned long datalen = 0;
    unsigned int frame_bytes;
    snd_pcm_uframes_t total;
    snd_pcm_format_t format;
    snd_pcm_t *pcm;
    const char *device;
    int err;

    if (nbytes < 44 ||
        memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: %s: not a RIFF/WAVE file\n", progname, label);
        return;
    }

    /* Walk the chunk list for "fmt " and "data". */
    for (pos = 12; pos + 8 <= nbytes; ) {
        unsigned long csize = rd32(buf + pos + 4);
        const unsigned char *cdata = buf + pos + 8;
        if (csize > nbytes - pos - 8)
            csize = nbytes - pos - 8;
        if (!memcmp(buf + pos, "fmt ", 4) && csize >= 16) {
            wformat = rd16(cdata);
            channels = rd16(cdata + 2);
            rate = rd32(cdata + 4);
            bits = rd16(cdata + 14);
            /* WAVE_FORMAT_EXTENSIBLE: real format tag is in the GUID. */
            if (wformat == WAVE_FORMAT_EXTENSIBLE && csize >= 26)
                wformat = rd16(cdata + 24);
        } else if (!memcmp(buf + pos, "data", 4)) {
            data = cdata;
            datalen = csize;
        }
        pos += 8 + csize + (csize & 1);
    }

    if (!data || !datalen || !channels || !rate) {
        fprintf(stderr, "%s: %s: missing fmt/data chunk\n", progname,
                label);
        return;
    }

    if (wformat == WAVE_FORMAT_PCM && bits == 8)
        format = SND_PCM_FORMAT_U8;
    else if (wformat == WAVE_FORMAT_PCM && bits == 16)
        format = SND_PCM_FORMAT_S16_LE;
    else if (wformat == WAVE_FORMAT_PCM && bits == 24)
        format = SND_PCM_FORMAT_S24_3LE;
    else if (wformat == WAVE_FORMAT_PCM && bits == 32)
        format = SND_PCM_FORMAT_S32_LE;
    else if (wformat == WAVE_FORMAT_IEEE_FLOAT && bits == 32)
        format = SND_PCM_FORMAT_FLOAT_LE;
    else {
        fprintf(stderr, "%s: %s: unsupported WAV format %u/%u-bit\n",
                progname, label, wformat, bits);
        return;
    }

    frame_bytes = channels * bits / 8;
    total = datalen / frame_bytes;
    if (!total) {
        fprintf(stderr, "%s: %s: empty data chunk\n", progname, label);
        return;
    }

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
    err = snd_pcm_set_params(pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                             channels, rate, 1 /* allow resampling */,
                             500000 /* 0.5 s buffer */);
    if (err < 0) {
        fprintf(stderr, "%s: cannot configure audio device: %s\n",
                progname, snd_strerror(err));
        snd_pcm_close(pcm);
        return;
    }

    audio.pcm = pcm;
    audio.data = data;
    audio.total = total;
    audio.pos = 0;
    audio.frame_bytes = frame_bytes;
    {
        snd_pcm_uframes_t bufsz = 0, period = 0;
        if (snd_pcm_get_params(pcm, &bufsz, &period) < 0 || !bufsz)
            bufsz = rate / 2;   /* ~the requested 0.5 s */
        audio.buffer = bufsz;
    }
}

/*
 * Load a WAV file from disk and start looping it.
 */
static void audio_init(const char *path)
{
    FILE *f;
    long filesize;
    unsigned char *buf;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: cannot open %s: %s\n", progname, path,
                strerror(errno));
        return;
    }
    fseek(f, 0, SEEK_END);
    filesize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (filesize < 44) {
        fprintf(stderr, "%s: %s: not a WAV file\n", progname, path);
        fclose(f);
        return;
    }
    buf = malloc((size_t) filesize);
    if (!buf || fread(buf, 1, (size_t) filesize, f) != (size_t) filesize) {
        fprintf(stderr, "%s: cannot read %s\n", progname, path);
        fclose(f);
        free(buf);
        return;
    }
    fclose(f);

    audio_start(buf, (unsigned long) filesize, path);
    if (!audio.pcm)
        free(buf);
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
        if (chunk > audio.total - audio.pos)
            chunk = audio.total - audio.pos;

        n = snd_pcm_writei(audio.pcm,
                           audio.data + (size_t) audio.pos *
                                        audio.frame_bytes,
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
        if (audio.pos == audio.total)
            audio.pos = 0;
    }
}

#else /* !GIFSAVER_AUDIO */

/* Stubs so the main loop needs no conditionals. */
static const struct { void *pcm; } audio = { NULL };

static void audio_init(const char *path)
{
    (void) path;
    fprintf(stderr, "%s: compiled without audio support (see config.h), "
            "-sound ignored\n", progname);
}

static void audio_pump(void)
{
}

#endif /* GIFSAVER_AUDIO */

/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
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
        , progname);
    exit(1);
}

int main(int argc, char **argv)
{
    Display *dpy;
    Window win = None;
    XWindowAttributes xgwa;
    GC gc;
    Frame *frames = NULL;
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
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-root")) {
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

    nframes = load_gif_frames(gif_file, dpy, win, xgwa.visual,
                              xgwa.depth, bg_override, &frames);
    if (delay_override >= 0)
        for (i = 0; i < nframes; i++)
            frames[i].delay_ms = delay_override;

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFillStyle(dpy, gc, FillTiled);

    if (wav_file)
        audio_init(wav_file);
#if GIFSAVER_EMBED_WAV
    else
        audio_start(gifsaver_default_wav, sizeof gifsaver_default_wav,
                    "built-in WAV");
#endif

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
