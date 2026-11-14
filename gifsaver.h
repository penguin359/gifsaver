/*
 * gifsaver.h - shared types and interfaces between the portable core
 *              (anim.c, wav.c) and the platform layers (platform_*.c).
 *
 * Copyright (c) 2026 Loren M. Lang
 * SPDX-License-Identifier: MIT
 *
 * The portable core knows nothing about X11, Win32 or any audio API:
 * it decodes GIFs into plain RGB frame buffers and WAVs into described
 * sample buffers. Each platform layer owns main(), presentation
 * (windows, drawing, timing) and the audio output device.
 */

#ifndef GIFSAVER_H
#define GIFSAVER_H

#define GIFSAVER_VERSION "1.0"

/* Defined by the platform layer; used in diagnostics. */
extern const char *progname;

/* ------------------------------------------------------------------ */
/* Animation: a decoded GIF as canvas-sized RGB frames.               */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned int *rgb;      /* width*height pixels, 0xRRGGBB */
    int delay_ms;           /* time to show this frame */
} AnimFrame;

typedef struct {
    int width, height;
    int nframes;
    AnimFrame *frames;
} Animation;

/*
 * Decode a GIF into *out. bg_override is an 0xRRGGBB backdrop for
 * transparent areas, or -1 to pick one automatically. Returns 0 on
 * success, -1 on error (message already printed).
 */
int anim_load_file(const char *path, long bg_override, Animation *out);

/* Decode the embedded GIF (only with GIFSAVER_EMBED_GIF). */
int anim_load_builtin(long bg_override, Animation *out);

void anim_free(Animation *anim);

/* ------------------------------------------------------------------ */
/* WAV: a parsed RIFF/WAVE clip described independently of any        */
/* audio API.                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    WAV_FMT_U8,             /* PCM unsigned 8-bit */
    WAV_FMT_S16LE,          /* PCM signed 16-bit little-endian */
    WAV_FMT_S24LE3,         /* PCM signed 24-bit packed little-endian */
    WAV_FMT_S32LE,          /* PCM signed 32-bit little-endian */
    WAV_FMT_F32LE           /* IEEE float 32-bit little-endian */
} WavFormat;

typedef struct {
    WavFormat format;
    unsigned int channels;
    unsigned int rate;          /* sample frames per second */
    unsigned int frame_bytes;   /* bytes per interleaved frame */
    const unsigned char *data;  /* interleaved sample data */
    unsigned long nframes;      /* total frames in data */
    unsigned char *owned;       /* heap buffer backing data, or NULL */
} WavClip;

/*
 * Parse an in-memory RIFF/WAVE image (`label` names it in messages).
 * The clip's data points into buf, which must outlive the clip.
 * Returns 0 on success, -1 on error (message already printed).
 */
int wav_parse(const unsigned char *buf, unsigned long nbytes,
              const char *label, WavClip *out);

/* Load and parse a WAV file; the clip owns the file buffer. */
int wav_load_file(const char *path, WavClip *out);

/* Parse the embedded WAV (only with GIFSAVER_EMBED_WAV). */
int wav_load_builtin(WavClip *out);

void wav_free(WavClip *clip);

#endif /* GIFSAVER_H */
