# Gallery

All screenshots are taken from OpenBallance itself, with the gasm headless runner
(see [Headless runs and screenshots](/howto/headless)).

<script setup>
const shots = [
  ["intro", "The intro"],
  ["menu", "Main menu over the menu level"],
  ["level-select", "Level selection"],
  ["options", "Options"],
  ["highscore", "Highscores"],
  ["tutorial", "Level 1: the tutorial"],
  ["level01", "Level 1: rolling down from the start tower"],
  ["level02", "Level 2"],
  ["level03", "Level 3"],
  ["level04", "Level 4"],
  ["level05", "Level 5"],
  ["level06", "Level 6"],
  ["level07", "Level 7"],
  ["level08-extra", "Level 8: an extra-points pickup, its silver balls orbiting the glow"],
  ["level09", "Level 9"],
  ["level10", "Level 10"],
  ["level11", "Level 11"],
  ["level12", "Level 12"],
  ["pause", "The pause menu"],
  ["game-over", "Game over"],
  ["viewer", "The level viewer (mode=viewer)"],
];
</script>

<div class="gallery">
  <figure v-for="[f, cap] in shots" :key="f">
    <img :src="`/screenshots/${f}.jpg`" :alt="cap">
    <figcaption>{{ cap }}</figcaption>
  </figure>
</div>

<style>
.gallery { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 16px; }
.gallery img { width: 100%; border-radius: 6px; }
.gallery figcaption { font-size: 0.85em; color: var(--vp-c-text-2); }
</style>
