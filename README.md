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

Compile-time features live in [config.h](./config.h). To build without
the ALSA dependency, set `GIFSAVER_AUDIO` to `0` (or comment it out)
there and rebuild — the Makefile then drops `-lasound` automatically,
and `-sound` is ignored with a notice at runtime.

### Dev container / Codespaces

A [dev container configuration](./.devcontainer/devcontainer.json) is
included: open the repo in a GitHub Codespace (or VS Code's "Reopen in
Container") and all build dependencies, headless-test tools (`Xvfb`,
`xdpyinfo`) and recommended extensions are set up automatically; the
project is built once the container is created.

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
| `-delay ms` | Override all frame delays with a fixed value |
| `-background color` | Backdrop for transparent areas (X color name or `#rrggbb`) |
| `-sound file.wav` | Loop a WAV file in the background while animating |
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
the bundled script, then enable the features in [config.h](./config.h):

```sh
python3 embed2c.py image.gif      # writes default_gif.h
python3 embed2c.py cchord.wav     # writes default_wav.h
# set GIFSAVER_EMBED_GIF / GIFSAVER_EMBED_WAV to 1 in config.h
make
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
