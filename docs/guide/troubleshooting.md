# Troubleshooting

**"game data not found"**: point the runner at a disc image (`--rom`), the CD or an installed game
(`--asset-dir`). See [Game data](/guide/game-data).

**"Too many open files"** with `--asset-dir`: raise the limit (`ulimit -n 4096`) or use the disc image.

**The browser player says there is no WebGPU**: use a recent Chrome or Edge, Safari 26 or newer, or
Firefox 141 or newer, or run it with `gasm-run`.

**Pressing Enter opens the quit dialog or Esc closes the window**: Esc is the runner's quit key; the game's
Esc is on Z. See [Controls](/guide/controls).

**Level 1 starts facing away from the track**: that is the original game; the tutorial asks you to turn the
view (W + Left / Right) until the track with the two towers is in front of you.
