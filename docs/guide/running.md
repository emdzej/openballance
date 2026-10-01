# Running on gasm

OpenBallance is a gasm game: `openballance.wasm` runs on the gasm runner `gasm-run` (version 0.4.0 or
newer, which has the graphics API OpenBallance needs). The [bundles](/guide/install) include the runner
and a launcher; you can also call it yourself:

```sh
# a disc image (one file)
gasm-run openballance.wasm --rom Ballance.iso

# the mounted CD, a copy of it, or an installed game
gasm-run openballance.wasm --asset-dir /Volumes/BALLANCE

# OpenBallance's keyboard layout (Shift + arrows rotate the view; see Controls)
gasm-run openballance.wasm --rom Ballance.iso --keymap keymap.txt

# keep the saves in a folder of your choice and unlock every level
gasm-run openballance.wasm --rom Ballance.iso --storage-dir saves --param unlockall=1
```

`--asset-dir` keeps every file of the folder open. If the runner reports "Too many open files",
raise the limit first (`ulimit -n 4096`) or use the disc image.

Esc quits `gasm-run`. The game's own Esc (the pause menu) is on Z: see [Controls](/guide/controls).
