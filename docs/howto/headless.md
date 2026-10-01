# Headless runs and screenshots

`gasm-run` can run the game without a window, with scripted pad input, and write the last frame as a PNG:

```sh
gasm-run build-gasm/openballance.wasm --rom Ballance.iso --headless 2072 --no-hash \
  --input "1000-1005:A,1150-1155:A,1750-1755:Y,1900-2400:DOWN" --screenshot level1.png
```

`--input` takes frame ranges with buttons (`A B X Y L R SELECT START UP DOWN LEFT RIGHT`). Useful moments:

| Frame | |
|---|---|
| ~990 | the main menu is up |
| 1000 + Enter | the level selection |
| 1150 + Enter | level 1 loads (press DOWN N−1 times before it, with `--param unlockall=1`, for level N) |
| ~1750 | the start prompt (Y = Q dismisses the tutorial) |
| ~1930 | the ball appears |

The gallery's screenshots were made this way.
