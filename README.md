# gifsaver

An [xscreensaver](https://www.jwz.org/xscreensaver/)-compatible screensaver
hack that loads an animated GIF with **giflib** and tiles it across the
entire screen, honoring the GIF's own frame timing, transparency,
interlacing and disposal methods. It can also loop a WAV file in the
background while the animation plays.

## Building All The Things

Requires `libx11-dev` and `libgif-dev`, plus `libasound2-dev` when
audio support is enabled (the default):

```sh
make
```

### Source layout

The code is split into a portable core and a platform layer so other
screensaver environments (Win32, macOS) can be added later by writing
one new platform file:

| File | Role |
| --- | --- |
| [gifsaver.h](./gifsaver.h) | Shared types/interfaces between core and platform |
| [anim.c](./anim.c) | Portable: GIF → RGB frame buffers (giflib only) |
| [wav.c](./wav.c) | Portable: RIFF/WAVE → described sample buffer |
| [platform_x11.c](./platform_x11.c) | Linux/X11: `main()`, xscreensaver conventions, windowing, tiled drawing, event/timing loop, ALSA output |

The platform layer is selected with `make PLATFORM=x11` (the default);
a future Windows port would add `platform_win32.c` and build with
`PLATFORM=win32`.

Compile-time features are configured through Makefile variables, set
on the command line or persistently in an optional `config.mk` (which
is included when present and silently skipped when missing):

| Variable | Default | Effect |
| --- | --- | --- |
| `AUDIO` | `1` | ALSA sound support (`-sound`); `AUDIO=0` drops the `libasound` dependency and `-sound` is ignored with a notice |
| `EMBED_GIF` | `0` | Compile `default_gif.h` into the binary as the default animation |
| `EMBED_WAV` | `0` | Compile `default_wav.h` into the binary as the default sound (needs `AUDIO=1`) |

```sh
make AUDIO=0                 # one-off build without ALSA
echo 'EMBED_GIF = 1' > config.mk && make    # persistent local setting
```

The Makefile injects these as `-D` flags and tracks them in a stamp
file, so changing a feature automatically rebuilds the affected
objects. [config.h](./config.h) only holds fallback defaults for
builds that bypass the Makefile.

### Dev container / Codespaces

A [dev container configuration](./.devcontainer/devcontainer.json) is
included: open the repo in a GitHub Codespace (or VS Code's "Reopen in
Container") and all build dependencies, headless-test tools (`Xvfb`,
`xdpyinfo`) and recommended extensions are set up automatically; the
project is built once the container is created.

### Static binary (portable, musl-linked)

[build-static.sh](./build-static.sh) builds a fully static executable
in an Alpine Linux container — no glibc or shared-library dependencies,
so the resulting `gifsaver-static` can be copied to any x86-64 Linux
machine and run as-is:

```sh
./build-static.sh                 # needs Docker; writes ./gifsaver-static
```

Audio is always disabled in static builds (ALSA's plugin system needs
`dlopen()`, which static executables cannot do); `-sound` is accepted
but ignored with a notice. Combine with `make EMBED_GIF=1` for a
single self-contained screensaver binary
with the animation baked in. The script pins and statically builds
libXau from source, since Alpine does not package it as a static
library.

## Trying it out, Failing Fast!

Run it standalone in a test window:

```sh
./gifsaver /path/to/animation.gif
```

Or paint the whole root window:

```sh
./gifsaver -root /path/to/animation.gif
```

## Options

| Option | Description |
| --- | --- |
| `-root` | Draw on the (virtual) root window |
| `-window-id 0xNNN` | Draw on an existing window (used by xscreensaver) |
| `-window` | Open a test window (default when run by hand) |
| `-fit` | Size the test window to one tile (the GIF's own dimensions) |
| `-delay ms` | Override all frame delays with a fixed value |
| `-background color` | Backdrop for transparent areas (X color name or `#rrggbb`) |
| `-sound file.wav` | Loop a WAV file in the background while animating |
| `-help` | Show usage and exit |
| `-version` | Show the version and exit |
| `-display dpy` | X display to connect to |

The GIF file may also be given via the `GIFSAVER_GIF` environment
variable instead of a positional argument.

### Transparent GIFs

Many GIFs mark their background as transparent and rely on whatever is
behind them (e.g., a web page) to show through. When the GIF declares no
usable background color, gifsaver composites transparent areas over
white, like a browser on a typical page. Use `-background` to pick a
different backdrop, e.g. `-background black` or `-background '#336699'`.

### Sound

```sh
./gifsaver -sound music.wav image.gif
```

The WAV file (PCM 8/16/24/32-bit or 32-bit float, any rate/channels) is
looped through ALSA for as long as the screensaver runs — fed
non-blockingly from the same event loop that drives the animation, so
everything stays in a single process. Set the `AUDIODEV` environment
variable to pick a specific ALSA device (default: `default`). Audio
problems (missing device, unsupported file) are reported but never stop
the animation.

### Embedding default media in the binary

A default GIF and/or WAV can be compiled directly into the executable
so gifsaver runs with no files on disk. Convert them to C headers with
the bundled script, then enable the features at build time:

```sh
python3 embed2c.py image.gif      # writes default_gif.h
python3 embed2c.py cchord.wav     # writes default_wav.h
make EMBED_GIF=1 EMBED_WAV=1
./gifsaver                        # no arguments needed
```

With `GIFSAVER_EMBED_GIF` the `file.gif` argument becomes optional and
the built-in GIF is used when it is omitted. With `GIFSAVER_EMBED_WAV`
(requires `GIFSAVER_AUDIO`) the built-in WAV plays whenever no `-sound`
option is given. Command-line files always take precedence. The
generated headers are listed in `.gitignore` and should not be
committed.

## Installing into xscreensaver

1. Install the binary where xscreensaver can find it (anywhere on
   `$PATH` works too):

   ```sh
   sudo make install        # installs to /usr/local/libexec/xscreensaver
   ```

2. Add a line to the `programs:` list in `~/.xscreensaver`:

   ```
   programs:  ...
              gifsaver -root /home/you/pictures/animation.gif  \n\
   ```

   (Or pick it in the `xscreensaver-settings` / `xscreensaver-demo` GUI
   after restarting the daemon: `xscreensaver-command -restart`.)

3. Optionally install the settings-dialog description:

   ```sh
   sudo cp gifsaver.xml /usr/share/xscreensaver/config/
   ```

When xscreensaver activates, it runs the hack with the target window in
the `XSCREENSAVER_WINDOW` environment variable; gifsaver detects this
automatically and draws on that window.

## License

This project is licensed under the [MIT License](./LICENSE).
