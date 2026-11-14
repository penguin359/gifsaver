/*
 * config.h - compile-time feature defaults for gifsaver.
 *
 * Features are configured from the build system: the Makefile injects
 * -DGIFSAVER_...=0/1 flags from its AUDIO / EMBED_GIF / EMBED_WAV
 * variables, which can be set on the command line (make AUDIO=0) or
 * persistently in an optional config.mk. The values below are only
 * fallback defaults for builds that pass no -D flags at all.
 */

#ifndef GIFSAVER_CONFIG_H
#define GIFSAVER_CONFIG_H

/*
 * Audio support: loop a WAV file (-sound file.wav) while the animation
 * plays. On Linux this goes through ALSA and requires libasound
 * (libasound2-dev) at build time; on Windows it uses winmm. Configure
 * with the AUDIO make variable.
 */
#ifndef GIFSAVER_AUDIO
#define GIFSAVER_AUDIO 1
#endif

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
 * WAV plays whenever no -sound option is given. Configure with the
 * EMBED_GIF / EMBED_WAV make variables.
 */
#ifndef GIFSAVER_EMBED_GIF
#define GIFSAVER_EMBED_GIF 0
#endif

#ifndef GIFSAVER_EMBED_WAV
#define GIFSAVER_EMBED_WAV 0
#endif

#endif /* GIFSAVER_CONFIG_H */
