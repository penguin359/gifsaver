# Copilot instructions for gifsaver

## Workflow

- Before starting any new task, check for uncommitted changes
  (`git status`). If anything from a previous task is not yet
  committed, pause and ask the user whether to continue that previous
  task, commit the changes, or leave them for the user to review and
  commit — do not start the new task on a dirty tree without asking.
- As long as the task was started from a clean state: when creating a
  new file, register it with `git add -N <file>` (intent-to-add) so it
  is tracked and shows up in diffs, while its content still appears as
  to-be-added in the staging area for the user to review and commit.

## Project notes

- Portable core (`anim.c`, `wav.c`, `gifsaver.h`) must stay free of
  platform APIs (X11, ALSA, Win32, etc.); platform-specific code
  belongs in `platform_*.c`.
- Compile-time features are configured in `config.h`; its defaults can
  be overridden with `-D` compiler flags.
- Verify changes with a headless test: run the binary against `Xvfb`
  and use `AUDIODEV=null` for audio paths.
