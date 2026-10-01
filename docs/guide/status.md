# Status and compatibility

## What works

- The boot sequence, the intro and the full main menu (start, highscores, options, credits).
- All twelve levels load and play: modules, transformers, extra points and lives, checkpoints, sectors,
  falling off, game over, restarting, the pause menu and leaving a level.
- The level 1 tutorial.
- The physics: a port of the Ipion (IVP) engine's core, collision detection, friction and impact systems.
  Balls roll, rest, bounce and fall asleep the way the original's code says they should.
- Sound effects and music, including rolling and collision sounds.
- The original's keyboard and mouse controls, including the key settings in Options.
- Highscores and settings, saved through gasm's storage.
- Reading the data from the CD image, the mounted CD (InstallShield cabinets) or an installed copy.

## Known gaps

- The physics has not yet been compared numerically against recordings from the original game.
- `Text Display` (a debug overlay, F3 in some levels) is not drawn.
- 3D sprites are drawn unlit in their material's colour.
- `Delete Dynamic Objects` (only used when quitting) does not destroy anything.
- Debug-only Building Blocks (profiler values, statistics) report zeros.

Found something that behaves differently from the original? Please
[open an issue](https://github.com/emdzej/openballance/issues).
