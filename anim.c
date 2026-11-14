/*
 * anim.c - portable GIF decoding for gifsaver.
 *
 * Copyright (c) 2026 Loren M. Lang
 * SPDX-License-Identifier: MIT
 *
 * Composes every GIF frame onto a canvas-sized RGB buffer, honoring
 * transparency, interlacing and disposal methods. Depends only on
 * giflib and the C library — no platform code.
 */

#include "config.h"
#include "gifsaver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gif_lib.h>

#if GIFSAVER_EMBED_GIF
#include "default_gif.h"
#endif

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
 * Decode an already-opened GIF into *out and close it. Returns 0 on
 * success, -1 on error (message already printed).
 */
static int anim_decode(GifFileType *gif, const char *label,
                       long bg_override, Animation *out)
{
    int err = 0;
    unsigned int *canvas = NULL, *prev = NULL, bg_color = 0;
    AnimFrame *frames = NULL;
    int cw, ch, i, nframes;

    if (DGifSlurp(gif) != GIF_OK) {
        fprintf(stderr, "%s: cannot decode %s: %s\n", progname, label,
                GifErrorString(gif->Error));
        DGifCloseFile(gif, &err);
        return -1;
    }

    cw = gif->SWidth;
    ch = gif->SHeight;
    nframes = gif->ImageCount;
    if (cw <= 0 || ch <= 0 || nframes <= 0) {
        fprintf(stderr, "%s: %s has no usable frames\n", progname, label);
        DGifCloseFile(gif, &err);
        return -1;
    }

    if (gif->SColorMap)
        bg_color = color_of(gif->SColorMap, gif->SBackGroundColor);

    /*
     * If the background color index is also the first frame's transparent
     * color (or there is no global colormap), its palette entry carries no
     * meaningful color.  Such GIFs are drawn by browsers over the page
     * behind them, which is usually light, so default to white; use
     * bg_override to pick something else.
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
        goto oom;

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
            goto oom;
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
        frames[i].rgb = malloc((size_t) cw * ch * sizeof *canvas);
        if (!frames[i].rgb)
            goto oom;
        memcpy(frames[i].rgb, canvas, (size_t) cw * ch * sizeof *canvas);

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

    out->width = cw;
    out->height = ch;
    out->nframes = nframes;
    out->frames = frames;
    return 0;

oom:
    fprintf(stderr, "%s: out of memory decoding %s\n", progname, label);
    if (frames) {
        for (i = 0; i < nframes; i++)
            free(frames[i].rgb);
        free(frames);
    }
    free(canvas);
    free(prev);
    DGifCloseFile(gif, &err);
    return -1;
}

int anim_load_file(const char *path, long bg_override, Animation *out)
{
    int err = 0;
    GifFileType *gif = DGifOpenFileName(path, &err);

    if (!gif) {
        fprintf(stderr, "%s: cannot open %s: %s\n", progname, path,
                GifErrorString(err));
        return -1;
    }
    return anim_decode(gif, path, bg_override, out);
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

int anim_load_builtin(long bg_override, Animation *out)
{
    int err = 0;
    MemReader mr = { gifsaver_default_gif, sizeof gifsaver_default_gif, 0 };
    GifFileType *gif = DGifOpen(&mr, mem_read, &err);

    if (!gif) {
        fprintf(stderr, "%s: cannot open built-in GIF: %s\n", progname,
                GifErrorString(err));
        return -1;
    }
    return anim_decode(gif, "built-in GIF", bg_override, out);
}

#endif /* GIFSAVER_EMBED_GIF */

void anim_free(Animation *anim)
{
    int i;

    if (!anim->frames)
        return;
    for (i = 0; i < anim->nframes; i++)
        free(anim->frames[i].rgb);
    free(anim->frames);
    anim->frames = NULL;
    anim->nframes = 0;
}
