---
layout: home

hero:
  name: OpenBallance
  text: Ballance, rolling again
  tagline: A faithful reimplementation of the 2004 Virtools classic. The original game scripts run on a new Virtools-compatible runtime with a port of the game's own physics engine, using the data from your own CD. Runs on macOS, Linux and Windows, and in your browser, through the gasm WebAssembly runtime.
  image:
    src: /screenshots/level01.jpg
    alt: The wooden ball rolling through level 1
  actions:
    - theme: brand
      text: Get started
      link: /guide/
    - theme: alt
      text: Play in the browser
      link: /play/
      target: _self
    - theme: alt
      text: How it works
      link: /internals/

features:
  - title: Faithful
    details: Ballance's game logic lives in Virtools behaviour graphs, so OpenBallance runs those original graphs. The Building Blocks they call are ported one by one from the original DLLs, quirks included.
  - title: The original physics
    details: The ball's feel comes from the Ipion physics engine inside physics_RT.dll. Its core, collision detection, friction and impact solvers are ported from the disassembly, constant for constant.
  - title: Your original data
    details: Reads the levels, textures, sounds and music straight from your Ballance CD image, the mounted CD or an installed copy, including InstallShield cabinets. No assets are included or converted.
  - title: Runs on gasm
    details: One openballance.wasm for macOS, Linux, Windows and the browser, on the gasm WebAssembly game runtime with its WebGPU-style graphics.
    link: /guide/running
    linkText: Running on gasm
  - title: In the browser
    details: Choose your Ballance ISO or game folder once and play in a WebGPU-capable browser. Your data stays on your device.
    link: /play/
    linkText: Play
    target: _self
  - title: All twelve levels
    details: Intro, menus, the tutorial, all twelve levels with their modules, transformers and extras, lives and score, pause, game over and highscores.
---

<div class="vp-doc" style="max-width: 1152px; margin: 48px auto 0; padding: 0 24px;">

## Built for gasm

<p><a class="gasm-badge" href="https://gasm.emdzej.pl"><img class="gasm-badge-light" src="https://gasm.emdzej.pl/badge/built-for-gasm-light.svg" alt="Built for gasm" width="120" height="44"><img class="gasm-badge-dark" src="https://gasm.emdzej.pl/badge/built-for-gasm-dark.svg" alt="Built for gasm" width="120" height="44"></a></p>

OpenBallance is one WebAssembly module, `openballance.wasm`, running on [gasm](https://gasm.emdzej.pl), a
portable game runtime: the same engine natively (`gasm-run`) and in the browser. Read
[Running on gasm](/guide/running) or [play in the browser](/play/){target="_self"}.

## Screenshots

<div class="shots">
  <img src="/screenshots/menu.jpg" alt="The main menu over the menu level">
  <img src="/screenshots/level08-extra.jpg" alt="An extra-points pickup in level 8">
  <img src="/screenshots/level05.jpg" alt="Level 5">
  <img src="/screenshots/tutorial.jpg" alt="The level 1 tutorial">
</div>

[More in the gallery →](/gallery)

</div>

<style>
.shots { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 12px; }
.shots img { width: 100%; border-radius: 6px; }
</style>
