# Troubleshooting

**"game data not found"**: point the runner at a disc image (`--rom`), the CD or an installed game
(`--asset-dir`). See [Game data](/guide/game-data).

**"Too many open files"** with `--asset-dir`: raise the limit (`ulimit -n 4096`) or use the disc image.

**The browser player says there is no WebGPU**: use a recent Chrome or Edge, Safari 26 or newer, or
Firefox 141 or newer, or run it with `gasm-run`.

**The window closes when I press Esc**: holding Esc quits `gasm-run`; tap it for the game's pause menu.

**"unknown import", the module won't load, or `gasm-run` stops in `gpu_destroy` when a level starts**:
OpenBallance needs gasm 0.6.0 or newer.

**Level 1 starts facing away from the track**: that is the original game; the tutorial asks you to turn the
view (Shift + Left / Right) until the track with the two towers is in front of you.
