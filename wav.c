/*
 * wav.c - portable RIFF/WAVE parsing for gifsaver.
 *
 * Copyright (c) 2026 Loren M. Lang
 * SPDX-License-Identifier: MIT
 *
 * Parses a WAV into a WavClip describing the samples independently of
 * any audio API. The platform layer feeds the clip to its own audio
 * device (ALSA on Linux, and so on). Depends only on the C library.
 */

#include "config.h"
#include "gifsaver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if GIFSAVER_EMBED_WAV
#include "default_wav.h"
#endif

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

int wav_parse(const unsigned char *buf, unsigned long nbytes,
              const char *label, WavClip *out)
{
    unsigned long pos;
    unsigned int wformat = 0, channels = 0, rate = 0, bits = 0;
    const unsigned char *data = NULL;
    unsigned long datalen = 0;
    WavFormat format;
    unsigned int frame_bytes;

    if (nbytes < 44 ||
        memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: %s: not a RIFF/WAVE file\n", progname, label);
        return -1;
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
        return -1;
    }

    if (wformat == WAVE_FORMAT_PCM && bits == 8)
        format = WAV_FMT_U8;
    else if (wformat == WAVE_FORMAT_PCM && bits == 16)
        format = WAV_FMT_S16LE;
    else if (wformat == WAVE_FORMAT_PCM && bits == 24)
        format = WAV_FMT_S24LE3;
    else if (wformat == WAVE_FORMAT_PCM && bits == 32)
        format = WAV_FMT_S32LE;
    else if (wformat == WAVE_FORMAT_IEEE_FLOAT && bits == 32)
        format = WAV_FMT_F32LE;
    else {
        fprintf(stderr, "%s: %s: unsupported WAV format %u/%u-bit\n",
                progname, label, wformat, bits);
        return -1;
    }

    frame_bytes = channels * bits / 8;
    if (datalen / frame_bytes == 0) {
        fprintf(stderr, "%s: %s: empty data chunk\n", progname, label);
        return -1;
    }

    out->format = format;
    out->channels = channels;
    out->rate = rate;
    out->frame_bytes = frame_bytes;
    out->data = data;
    out->nframes = datalen / frame_bytes;
    out->owned = NULL;
    return 0;
}

int wav_load_file(const char *path, WavClip *out)
{
    FILE *f;
    long filesize;
    unsigned char *buf;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: cannot open %s: %s\n", progname, path,
                strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    filesize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (filesize < 44) {
        fprintf(stderr, "%s: %s: not a WAV file\n", progname, path);
        fclose(f);
        return -1;
    }
    buf = malloc((size_t) filesize);
    if (!buf || fread(buf, 1, (size_t) filesize, f) != (size_t) filesize) {
        fprintf(stderr, "%s: cannot read %s\n", progname, path);
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);

    if (wav_parse(buf, (unsigned long) filesize, path, out) < 0) {
        free(buf);
        return -1;
    }
    out->owned = buf;
    return 0;
}

#if GIFSAVER_EMBED_WAV

int wav_load_builtin(WavClip *out)
{
    return wav_parse(gifsaver_default_wav, sizeof gifsaver_default_wav,
                     "built-in WAV", out);
}

#endif /* GIFSAVER_EMBED_WAV */

void wav_free(WavClip *clip)
{
    free(clip->owned);
    clip->owned = NULL;
    clip->data = NULL;
    clip->nframes = 0;
}
