/*
 * gifsaver - an xscreensaver-compatible hack that tiles an animated GIF
 *            across the entire screen.
 *
 * Usage:
 *   gifsaver [-root] [-window-id 0xNNNNNN] [-delay ms] [-window] file.gif
 *
 * When run by xscreensaver, the target window is taken from the
 * XSCREENSAVER_WINDOW environment variable (or -window-id). With -root
 * it draws on the (virtual) root window. With -window (the default when
 * run by hand) it opens its own top-level window for testing.
 *
 * Requires: libX11, giflib (>= 5.0).
 */

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

/*
 * Decode `filename` and return an array of composed, canvas-sized
 * frames as Pixmaps. Returns the frame count; fills *out_frames.
 * bg_override is an 0xRRGGBB backdrop for transparent areas, or -1
 * to pick one automatically.
 */
static int load_gif_frames(const char *filename, Display *dpy, Window win,
                           Visual *visual, int depth, long bg_override,
                           Frame **out_frames)
{
    int err = 0;
    GifFileType *gif = DGifOpenFileName(filename, &err);
    unsigned int *canvas, *prev, bg_color = 0;
    Frame *frames;
    int cw, ch, i, nframes;

    if (!gif) {
        fprintf(stderr, "%s: cannot open %s: %s\n", progname, filename,
                GifErrorString(err));
        exit(1);
    }
    if (DGifSlurp(gif) != GIF_OK) {
        fprintf(stderr, "%s: cannot decode %s: %s\n", progname, filename,
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

static void usage(void)
{
    fprintf(stderr,
        "usage: %s [options] file.gif\n"
        "  -root              draw on the root window\n"
        "  -window-id 0xNNN   draw on an existing window (xscreensaver)\n"
        "  -window            open a test window (default)\n"
        "  -delay ms          override all frame delays\n"
        "  -background color  backdrop for transparent areas\n"
        "                     (X color name or #rrggbb)\n",
        progname);
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
        else
            usage();
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

    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);

    for (;;) {
        struct timeval tv;
        fd_set fds;
        int xfd = ConnectionNumber(dpy);

        XSetTile(dpy, gc, frames[cur].pixmap);
        XFillRectangle(dpy, win, gc, 0, 0,
                       (unsigned) xgwa.width, (unsigned) xgwa.height);
        XFlush(dpy);

        /* Wait out the frame delay while staying responsive to events. */
        tv.tv_sec = frames[cur].delay_ms / 1000;
        tv.tv_usec = (frames[cur].delay_ms % 1000) * 1000;
        for (;;) {
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
            if (nframes == 1) {
                /* Static image: just block on events. */
                XEvent ev;
                XNextEvent(dpy, &ev);
                XPutBackEvent(dpy, &ev);
                continue;
            }
            if (tv.tv_sec == 0 && tv.tv_usec == 0)
                break;
            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            if (select(xfd + 1, &fds, NULL, NULL, &tv) == 0)
                break;     /* delay elapsed */
        }
        cur = (cur + 1) % nframes;
    }
}
