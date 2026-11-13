# gifsaver

An [xscreensaver](https://www.jwz.org/xscreensaver/)-compatible screensaver
hack that loads an animated GIF with **giflib** and tiles it across the
entire screen, honoring the GIF's own frame timing, transparency,
interlacing and disposal methods.

## Building

Requires `libx11-dev` and `libgif-dev`:

```sh
make
```

## Trying it out

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
| `-display dpy` | X display to connect to |

The GIF file may also be given via the `GIFSAVER_GIF` environment
variable instead of a positional argument.

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
