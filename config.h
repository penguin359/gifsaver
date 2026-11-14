/*
 * config.h - compile-time feature configuration for gifsaver.
 *
 * Each feature is a macro set to 1 (enabled) or 0 (disabled).
 * Commenting a define out also disables the feature.
 */

#ifndef GIFSAVER_CONFIG_H
#define GIFSAVER_CONFIG_H

/*
 * Audio support: loop a WAV file (-sound file.wav) through ALSA while
 * the animation plays. Requires libasound (libasound2-dev) at build
 * time. Set to 0 or comment out to build without the ALSA dependency;
 * the Makefile picks up the change automatically.
 */
#define GIFSAVER_AUDIO 1

/*
 * Embedded default media: compile a GIF and/or WAV directly into the
 * binary so gifsaver can run with no files on disk. Generate the
 * headers first, e.g.:
 *
 *     python3 embed2c.py image.gif      # writes default_gif.h
 *     python3 embed2c.py cchord.wav     # writes default_wav.h
 *
 * With GIFSAVER_EMBED_GIF enabled, the file.gif argument becomes
 * optional and the built-in GIF is used when it is omitted. With
 * GIFSAVER_EMBED_WAV enabled (requires GIFSAVER_AUDIO), the built-in
 * WAV plays whenever no -sound option is given.
 */
#define GIFSAVER_EMBED_GIF 0
#define GIFSAVER_EMBED_WAV 0

#endif /* GIFSAVER_CONFIG_H */
