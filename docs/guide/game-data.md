# Game data

OpenBallance reads the game from your own copy of Ballance. Any of these works:

| Source | What it is |
|---|---|
| A disc image | `Ballance.iso`, or the `.bin` of a `.bin`/`.cue` pair |
| The CD | the mounted disc, or a folder you copied it to: it has `Setup/data1.hdr` |
| An installed game | the folder with `base.cmo`, `3D Entities/`, `BuildingBlocks/`, `Sounds/`, `Textures/` … |

The CD keeps the game inside InstallShield cabinets (`Setup/data1.hdr`, `data1.cab`, `data2.cab`).
OpenBallance reads them directly, decompressing and checking each file as the game opens it, so you
don't need to install or extract anything.

The CD's German installer also covers English, French, Spanish and Italian: pick the language with the
[`language` parameter](/guide/parameters).
