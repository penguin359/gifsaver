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

#endif /* GIFSAVER_CONFIG_H */
