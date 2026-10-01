# Controls

gasm gives every game the same gamepad-style controls; OpenBallance maps them onto the keys Ballance
expects. Its keyboard layout (`tools/gasm-bundle/keymap.txt`, used by the bundles and the browser
player) puts them where the original had them:

| Key | Game |
|---|---|
| Arrow keys | roll the ball, move in menus |
| Enter or X | select, continue the tutorial |
| Z | pause menu, back (Ballance's Esc) |
| Shift (or W) held + Left / Right | rotate the view by 90° |
| Space or S | raise the view |
| A | skip the tutorial (Ballance's Q) |
| F1 | F1 |

Esc quits `gasm-run` itself, so Ballance's Esc is on Z.

Running `gasm-run` yourself? Pass the layout: `gasm-run openballance.wasm --rom Ballance.iso --keymap keymap.txt`
(it's in every bundle and at [`/play/keymap.txt`](/play/keymap.txt){target="_self"}). Without it gasm's
default layout applies: the shoulder buttons are Q and W, so hold **W** + Left / Right to rotate the view
(Q works too, but also types a "q", which the tutorial reads as "skip").

A gamepad works as well: the d-pad rolls the ball, A and Start select, B pauses, the shoulder buttons rotate
the view, X raises it.
