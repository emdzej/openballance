# Introduction

OpenBallance is a reimplementation of **Ballance** (Cyparade / Atari, 2004), the marble game in which you
roll a ball of wood, stone or paper along tracks floating in the sky.

Ballance was built with **Virtools**: almost all of its game logic lives in behaviour graphs stored in the
game's `.cmo` and `.nmo` files, not in native code. So OpenBallance does not rewrite the game. It runs the
original graphs on a new runtime:

- a **Virtools (CK2) runtime** that loads the original files and executes their behaviour graphs, parameter
  operations and messages the way `CK2.dll` does;
- the **Building Blocks** the graphs call (about 180 of them), ported from the original DLLs with their quirks;
- a **renderer** for the gasm graphics API, matching the original Direct3D 7 fixed-function look;
- a port of the **Ipion physics engine** from `physics_RT.dll`, which gives the ball its feel.

It needs your own copy of Ballance: OpenBallance contains no game code or assets.

- [Status and compatibility](/guide/status): what works and what doesn't yet.
- [Installing](/guide/install) and [Game data](/guide/game-data): getting set up.
- [Running on gasm](/guide/running) or [Playing in the browser](/guide/browser).
- [Controls](/guide/controls) and [Launch parameters](/guide/parameters).
