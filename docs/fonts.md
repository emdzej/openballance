# Fonts and 2D text (Interface.dll font manager, 2D Text, TT CreateFontEx)

How Ballance draws text. There are two unrelated systems:

- The **font manager** of `Interface.dll` (texture fonts: a glyph table over a bitmap) used by
  `TT CreateFontEx`, `Set Font Properties`, `Create System Font` and `2D Text`. This is all the visible
  game text (menus, highscores, HUD, tutorial).
- `Text Display` wraps a CK2 `CKSpriteText` (GDI-rendered sprite). Only debug scripts use it.

`TT PushButton2` has no text at all; it is the menu buttons' mouse logic (described at the end).

Addresses: `Interface.dll` has image base `0x25380000` (Ghidra program `Interface.dll.0`), `TT_Toolbox_RT.dll`
`0x10000000`, `CK2_3D.dll` `0x10000000`, `Dx5InputManager.dll` `0x24ac0000`. Most of the font-manager
methods were missing from `re/Interface.dll.c` because Ghidra had not made functions at the vtable
targets. They now exist in the Ghidra project (`tools/ghidra/MakeFunctions.java` was run on the vtable
entries). `objdump -d --x86-asm-syntax=intel` on the DLL is reliable wherever the decompiler got the stack
wrong (the draw functions).

## 1. Parameter types

| GUID | Name | Kind / values |
|---|---|---|
| `64fb5811:33862d3b` | Font Type | enum, rebuilt every time a font is added or removed as `"<name>=<index>,..."` (`FUN_2538bdc0`); `"No Font=0"` when there are none. **The value is the 1-based font index**, 0 = none. Its type description has the font manager GUID as "saver manager" (`FUN_2538b7d0`), so files save it as a manager int (`value_mode = 0x64fb5810`). No game file contains font-manager data, and every saved Font value is 0. The CreateFontEx outputs write the real indices into locals at runtime. |
| `2e1e2209:47da44b5` | Alignment | enum `Center=0, Left=1, Right=2, Top=4, Bottom=8` (combinations: Top-Left=5, Top-Right=6, Bottom-Left=9, Bottom-Right=10) |
| `4157001d:4cc82922` | Text Properties (2D Text setting) | flags `1 Screen Proportional, 2 Background, 4 Clip To Dimension, 8 Resize Vertically, 0x10 Resize Horizontally, 0x20 WordWrap, 0x40 Justified, 0x80 Compiled, 0x100 Multiple, 0x200 Show Caret`. Internal bits used by the draw call: `0x400` 3D text (Text 3D BB, not in Ballance), `0x800` clip rectangle valid. |
| `63223dd5:6b5f68fc` | Font Properties (Set Font Properties setting) | flags `1 Gradient, 2 Shadow, 4 Lighting, 8 Disable Filter` |
| `4376013f:0b3462c0` | Font Weight | enum `THIN=100 ... NORMAL=400 ... HEAVY=900` |
| `7157091d:4fc82932` | Font Resolution | enum `128x128=1, 256x256=2, 512x512=4, 1024x1024=8` |
| `7167091a:7f482632` | Font Name (system fonts) | enum filled with the installed Windows fonts (`FUN_2538e7a0`) |
| `024d52f1:678223b2` | Array (CKDataArray) | the "FontCoordinatesData" pin of TT CreateFontEx |
| `6e49509a:2f067425` | Sprite Text Alignment (CK2) | `Center=1, Left=2, Right=4, Top=8, Bottom=16, VCenter=32, HCenter=64` (Text Display only) |

All of these except the Font Type and the CK2 enum are registered in `FUN_25392b10` (Interface plugin
init). The font manager is also created there.

## 2. Font manager (`CKFontManager`, GUID `64fb5810:73262d3b`)

Constructor `FUN_2538b230` (object size `0xd4`, name "Font Manager", vtable `0x25394308`). Fields:

| Offset | Meaning |
|---|---|
| `+0x28` | "new spacing" flag: set by 2D Text before every draw to `behavior version >= 0x20000` (see letter spacing) |
| `+0x2c` | 2D entity under the mouse (PreProcess `FUN_2538b8a0`: `RenderContext::Pick2D(mouse)`); not used by the BBs below |
| `+0x30/+0x34/+0x38` | `XArray<CKTextureFont*>` begin/end/capacity; index `i` (1-based) is element `i-1` |
| `+0x3c/+0x40/+0x44` | scratch array of 16-byte line records built by every draw (see 5.1) |
| `+0x78..+0x90` | hash table entity id -> compiled text geometry (Compiled flag) |
| `+0xb8` | GDI memory DC (system fonts) |
| `+0xbc..` | hash of system (GDI) fonts by texture name |

Vtable methods used (vtable `0x25394308`):

| Slot | Function | Semantics |
|---|---|---|
| `+0x78` | `FUN_2538c030` | `int CreateTextureFont(const char *name, CKTexture *tex, VxRect *fontRect, Vx2DVector *charCount, BOOL fixed, int firstChar, float emptyWidth)`. Returns -1 if `tex` or `name` is NULL. Allocates a `CKTextureFont` (0x18d4 bytes; ctor `FUN_2538ffe0`) and initializes it (`FUN_25390130`, see 4). If a font with the same name exists (`+0x7c`), the new one **replaces it at the same index** (the old one is freed). Otherwise it is appended (`FUN_2538bf40`) and the enum is rebuilt. Returns the 1-based index. |
| `+0x7c` | `FUN_2538bed0` | `int GetFontIndex(name)`: 1-based index of the first font with that exact name (`strcmp`), 0 if none. |
| `+0x80` | `FUN_2538c130` | `CKTextureFont *GetFont(int index)`: NULL if `index == 0` or `index > count`, **or if the font's `+0xc4 & 4` (built) flag is clear**. |
| `+0x84` | `FUN_2538d7c0` | creates a GDI font for a system-font texture (Create System Font only) |
| `+0x88` | `FUN_2538dcd0` | `CKTexture *CreateSystemFontTexture(sysFontIndex, resolution, fullCharset, weight, italic, underline, renderControlChars, dynamic, fontSize)` (see 6.3) |

CKBaseManager callbacks (same vtable; the valid-function mask is `0x180c0e5`):

- PostClearAll `+0x10` = `FUN_2538b760`: re-registers the Font Type enum, drops fonts whose texture is gone
  or dynamic, clears caches and the system-font hash.
- PreProcess `+0x14` = `FUN_2538b8a0`: the Pick2D above.
- OnCKReset `+0x34` = `FUN_2538b960`: `FUN_2538b610` destroys the fonts whose texture no longer exists or
  is dynamic (`flags & 0x108 == 0x108`, i.e. system-font textures). `FUN_2538b6a0` frees the compiled
  caches. Texture fonts over file textures (the game's) survive a reset.
- SequenceToBeDeleted `+0x44` = `FUN_2538b980`: removes the fonts whose texture is being deleted
  (`FUN_2538bc30`). This also fixes every Font-typed ParameterLocal and ParameterOut in the context:
  a value equal to the removed index becomes 0, larger values are decremented. It also drops the
  compiled caches of deleted entities.
- OnPostRender `+0x68` / OnPostSpriteRender `+0x6c` (`FUN_2538d190` / `FUN_2538cec0`): draw the texts
  queued by the "Multiple" mode (not used by Ballance).
- SaveData/LoadData `+0x4/+0x8` (`FUN_2538c2b0`/`FUN_2538c440`): identifier `0x8002` with per font name,
  char counts, rect, texture, flags&3, first char, and the 0x1800-byte glyph table if proportional. Absent
  from all Ballance files.

## 3. `CKTextureFont` layout (0x18d4 bytes)

Constructor defaults are in parentheses (`FUN_2538ffe0`). "BB" says who writes the field.

| Offset | Field | Notes |
|---|---|---|
| `+0x00` | vtable `0x253943b0` | a single method: `Draw` = `FUN_25391bf0` |
| `+0x04` | name | strdup |
| `+0x0c` | first character | index of the glyph at grid cell 0 |
| `+0x10, +0x14` | spacing x, y (0,0) | Set Font Properties "Space": x = letter spacing, y = extra line spacing (leading), both in pixels |
| `+0x18, +0x1c` | scale x, y (1,1) | Set Font Properties "Scale" |
| `+0x20` | italic offset (0) | top vertices move right by `italic * sx` px |
| `+0x24, +0x28` | paragraph indentation x, y (0,0) | written by 2D Text |
| `+0x2c, +0x30` | offset x, y (0,0) | written by 2D Text |
| `+0x34` | start color ARGB (`0xffffffff`) | Set Font Properties "Color" |
| `+0x38` | end color ARGB (`0xff000000`) | gradient bottom color |
| `+0x3c` | shadow color ARGB (`0x80000000`) | |
| `+0x40, +0x44` | shadow offset x, y in px (4,4) | `x = -cos(angle)*dist`, `y = sin(angle)*dist` |
| `+0x48, +0x4c` | shadow scale x, y (1,1) | replaces the scale for the shadow pass |
| `+0x50, +0x54` | basis width, height in px | texture size, or the render context size with Screen Proportional (recomputed at every draw) |
| `+0x58` | lit material id | Lighting flag |
| `+0x5c` | Font Properties flags | |
| `+0x60..+0x6c` | margins l, t, r, b | written by 2D Text |
| `+0x70..+0x7c` | text extents rect | output of the last draw |
| `+0x80..+0x8c` | clip rect | written by 2D Text (clip to camera / parent) |
| `+0x90` | empty-glyph width ratio (0.3) | CreateTextureFont argument |
| `+0x94` | line count of the last draw | |
| `+0x98` | caret material id, `+0x9c` caret size (fraction of the line height) | written by 2D Text |
| `+0xa0, +0xa4, +0xa8` | per-line state during a draw: space advance, letter spacing, line width | |
| `+0xac, +0xb0` | char count horizontal, vertical (floats) | |
| `+0xb4..+0xc0` | font rect in texture pixels l, t, r, b (floats) | |
| `+0xc4` | flags: `1` fixed width, `2` proportional widths computed, `4` built/ready | |
| `+0xc8` | texture id | |
| `+0xcc` | font manager | |
| `+0xd0` | glyph table, 256 entries x 6 floats (0x18 bytes) | see below |
| `+0x18d0` | CKContext | |

**Glyph entry** `g[c]` at `+0xd0 + c*0x18`, all values in normalized texture coordinates (0..1):

| Float | Meaning |
|---|---|
| 0 `u` | left edge of the glyph box |
| 1 `v` | top edge |
| 2 `w` | glyph box width (u extent) |
| 3 `pre` | advance before the box (like GDI ABC "A"; can be negative) |
| 4 `post` | advance after the box ("C") |
| 5 `h` | glyph box height (v extent); `h == 0` means "no glyph": skipped **without advancing** |

With `sx = basisW * scale.x` and `sy = basisH * scale.y`, a glyph is `w*sx` by `g[0].h*sy` pixels. **All
glyphs use the height of glyph 0.** Its advance is `(pre + w + post) * sx + letterSpacing`. The TT
CreateFontEx array columns are exactly these six floats, named `ustart, vstart, uwidth, uprewidth,
upostwidth, vwidth`.

## 4. Building the glyph table (`FUN_25390130` + `FUN_25390200`)

`FUN_25390130(font, tex, rect, counts, fixed, first, emptyWidth)` stores counts (`+0xac/+0xb0`), `fixed != 0`
into bit 1 of `+0xc4`, first (`+0xc`), texture, basis = texture size and `emptyWidth` (`+0x90`). **If the
rect has zero width or height it becomes the whole texture** `(0,0,texW,texH)`. Then `FUN_25390200`
builds the table, but only if the font is not built yet and the texture exists:

1. `cellW = (rect.r - rect.l) / (texW * countX)`, `cellH = (rect.b - rect.t) / (texH * countY)`.
2. All 256 entries = `{rect.l/texW, rect.t/texH, cellW, 0, 0, cellH}`.
3. For row `r < countY`, column `c < countX`, char `k = first + r*countX + c` (row-major, sequential):
   `u = rect.l/texW + c*cellW`, `v = rect.t/texH + r*cellH`.
   - Fixed (`+0xc4 & 1`): `w = cellW`, `pre = post = 0`.
   - Proportional, texture created by Create System Font (`FUN_2538dba0`: the name is in the system-font
     hash): use the GDI widths. For a TrueType font (`FUN_2538db70`), `GetCharABCWidths`
     (`FUN_2538dc70`) gives `u += A*px`, `pre = A*px`, `w = B*px`, `post = C*px` with `px = 1/texW`.
     Otherwise `GetCharWidth` (`FUN_2538dca0`) gives `w = width*px`.
   - Proportional, ordinary texture: scan the cell's pixels (the surface is locked as 32-bit ARGB).
     A pixel is opaque if its alpha is non-zero. If the texture has a color key (`CKTexture +0x70 & 2`),
     a pixel is opaque when it differs from the key (`+0x74`) instead. Let `L` be the first column from
     the left that has an opaque pixel in any row of the cell. If there is none, `w = cellW * emptyWidth`.
     Otherwise `u += L*px`, `w = cellW - L*px`. Then let `R` be the number of fully transparent columns
     counted from the right: `w -= R*px`, clamped at 0. `pre` and `post` stay 0.
   Sets `+0xc4 |= 2` for the proportional path.
4. `+0xc4 |= 4` (built; GetFont now returns it).

**For Ballance this step barely matters.** Every TT CreateFontEx call overwrites entries 0..254 from its
data array right afterwards (see 6.1). Only entry 255 keeps the step 2 default.

## 5. The draw path (`CKTextureFont::Draw`, `FUN_25391bf0`)

`Draw(dev, entity, text, align, VxRect *rect, CKMaterial *bgMat, DWORD flags, BOOL draw)` (`ret 0x20`).
2D Text calls it from a post-render callback with `draw = 1`. `rect` is the entity rectangle in render
context pixels and is modified in place.

### 5.1 Setup and line breaking

1. Compiled (`flags & 0x80`): if `draw` and a cache exists for the entity (`FUN_2538ca20`), draw the
   background (if flag 2), set the states (`FUN_25391a30`), replay the cached geometry (`FUN_25392ae0`)
   and return. Otherwise create a cache (`FUN_2538c750`) that the line draws fill. Without the flag,
   drop any cache of the entity (`FUN_2538c920`). A compiled text is therefore built once and **never
   changes again**, even if the text changes.
2. Keep `orig = *rect`. Apply the margins: `rect.l += m.l`, `rect.t += m.t`, `rect.r -= m.r`, `rect.b -= m.b`.
3. `k = 1` (`1/32` for the 3D flag `0x400`). Basis: with Screen Proportional (`flags & 1`),
   `basisW = dev->GetWidth()*k`, `basisH = dev->GetHeight()*k` (render context `+0xd4` / `+0xd0`).
   Otherwise the texture size times `k`.
4. WordWrap or Justified (`flags & 0x60`): return (draw nothing) if
   `(fontRect.w * k) / (countX * basisW * scale.x) > rect.w`. This degenerate guard never fires with
   Ballance's values.
5. `sx = scale.x * basisW`.
   - Letter spacing `ls`: if manager `+0x28` (the 2D Text behavior version is >= 2.0),
     `ls = (g[' '].w + g[' '].pre + g[' '].post) * spacing.x * sx * 0.1`. Otherwise `ls = spacing.x`
     (pixels).
   - Space advance `adv = (g[' '].w + g[' '].pre + g[' '].post) * sx + ls`.
   - Available width `avail = rect.r - rect.l`.
6. Split into line records `{start, count, spaces, width}` (manager `+0x3c` array; cleared first). The
   state is initialized once and **persists across lines**: `inWord = true`, `breakPtr = ""` (static
   empty string `0x25398304`), `breakCount = 0`, `breakW = 0`, `paraStart = true`.

```
p = text
while *p:
    rec = {start=p, count=0, spaces=0}
    w = paraStart ? indent.x * g[0].w * sx : 0
    for (; *p && *p != '\n'; p++, rec.count++):          # rec.count = index of *p within the line
        c = *p (unsigned)
        if c == ' ':
            inWord = true; breakW = w; breakCount = rec.count; rec.spaces++; breakPtr = p + 1
            w += adv
        else:
            if inWord: breakW = w - adv; breakCount = rec.count - 1; breakPtr = p; inWord = false
            w += (g[c].pre + g[c].w + g[c].post) * sx + ls
        if (flags & 0x60) and w > avail:                  # overflow
            if breakCount > 0:                             # break at the last word boundary
                rec.count = breakCount; w = breakW; rec.spaces--; p = breakPtr
            else:                                          # word longer than the line: break inside it
                if c != ' ': w -= (g[c].pre + g[c].w + g[c].post) * sx + ls
                if breakPtr == p: p++                      # always consume at least one char
                breakPtr = p
            inWord = true
            break
    rec.width = w
    if paraStart: rec.count = -rec.count; paraStart = false   # negative = first line of a paragraph
    if *p == '\n':
        paraStart = true
        if *breakPtr == '\n': p++
    push rec
    if *p == 0: break
    if p != breakPtr and *breakPtr != '\n': p++            # skip the '\n' (or nothing after a wrap)
```

A line's width includes its trailing spaces and the letter spacing after the last glyph. A trailing
`'\n'` makes no extra line, and `"\n\n"` makes an empty line (count 0).

Quirk: after a line that ends at `'\n'` inside a word, `inWord` stays false. The next paragraph's
first word then does not update the break state. If that line overflows before its first space, the
*previous* line's `breakCount/breakPtr` is used, and `p` jumps backwards (the original can loop). Our
port should treat a `breakPtr` outside the current line as "no break point". Ballance's wrapped texts
never have a first word wider than the line.

7. Measure (`FUN_25390910`): `maxW = max(rec.width)`, `lineH = basisH * scale.y * g[0].h + spacing.y`,
   `totalH = nLines * lineH`. `+0x94 = nLines`.
8. Vertical start: `y0 = rect.t` if `align & 4` (Top); `rect.b - totalH` if `align & 8` (Bottom); else
   `(rect.t + rect.b)/2 - totalH/2`.
   The extents box's x **wrongly tests the same bits**: `x0 = rect.l` if `align & 4`,
   `rect.r - maxW` if `align & 8`, else centered. `+0x70..+0x7c = {x0, y0, x0+maxW, y0+totalH}`. This
   is the "Text Extents" output. Line placement below uses the correct bits.
9. Resize (`flags & 0x18`; unused by Ballance): with `orig`, `new.t = y0`. If Justified,
   `new.b = y0 + totalH`. Otherwise `0x10` sets `new.r = orig.l + maxW` and `0x08` sets
   `new.b = y0 + totalH`. For 2D, `entity->GetSize` / `SetSize` (`+0x90` / `+0x94`) with width `maxW`
   (0x10) and height `totalH` (0x48). The background then uses the updated `orig`.
10. If `align & 4`: `y = rect.t + offset.y`, else `y = y0`. **The offset only applies to Top (y) and
    Left (x) alignment.**
11. If `draw`: background if `flags & 2` (`FUN_25390980`, 5.4), then the text states (`FUN_25391a30`,
    5.5).

### 5.2 Per line

For each record `i`: `+0xa0 = adv`, `+0xa4 = ls`, `+0xa8 = rec.width`, then:

- Justified (`0x40`): `x = rect.l`. If `rec.spaces > 0`, `+0xa0 = adv - (rec.width - avail)/rec.spaces`
  (the spaces absorb the slack). Otherwise, if `|count| > 1`, `+0xa4 = ls + (avail - rec.width)/(|count|-1)`.
  This applies to every line, the last one included.
- Else: Left (`align & 1`) `x = rect.l + offset.x`; Right (`align & 2`) `x = rect.r - rec.width`;
  else `x = (rect.l + rect.r)/2 - rec.width/2`.
- If `rec.count < 0` (paragraph start): negate it. If Left, `x += indent.x * g[0].w * sx`. If `i > 0`,
  `y += basisH * scale.y * indent.y * g[0].h`. This extra space is not part of `totalH`.
- If `draw`: shadow flag (`+0x5c & 2`) calls `FUN_25391930`, else `FUN_25390b70`, with
  `(dev, rec.start, rec.count, x, y, 0, rect, flags, cache)`.
- `y += lineH`.

### 5.3 Drawing one line (`FUN_25390b70`)

`drawLine(dev, s, n, x, y, z, clipSrc = rect (after margins), flags, cache)`. Return if `n == 0`.

1. Clip rect `C = rect`. If `flags & 0x800`: return if `C` does not overlap `font.clip` (`FUN_253924a0`),
   else `C` = the intersection.
2. `sx = basisW*scale.x`, `sy = basisH*scale.y`, `italic = sx * font.italic`,
   `du = 0.25/texW`, `dv = 0.25/texH`.
3. 2D pixel snapping (not 3D): `x = (int)(x + 0.5)`, `y = (int)(y + 0.5)`. If not Justified,
   `ls = +0xa4 = (int)(ls + 0.5)`.
4. `gh = sy * g[0].h`, `top = y`, `bot = y + gh`, `t0 = 0`, `t1 = 1`.
5. Clip To Dimension (`flags & 4`): return if `C.b < y`, `bot < C.t`, `C.r < x` or
   `x + lineWidth < C.l`. If `y < C.t`: `top = C.t`, `t0 = (C.t - y)/gh`. If `bot > C.b`: `bot = C.b`,
   `t1 = 1 - (bot_orig - C.b)/gh`. (Without flag 4 there is no glyph clipping. Ballance never sets it.)
6. For each byte `c` of the `n` chars:
   - `' '`: `x += +0xa0` (a pending caret is drawn over the space: `x .. x + +0xa0`).
   - `0x08` (caret marker, inserted by TT InputString "Use Caret"): if it is the line's last char and
     Show Caret (`0x200`), draw the caret at `x` with width `+0xa0`. Otherwise mark it pending: the
     next glyph or space gets it. No advance.
   - Other: skip if `g[c].h == 0`. `x0 = x + sx*g.pre`, `x1 = x0 + sx*g.w`, `u0 = g.u`, `u1 = g.u + g.w`.
     With flag 4: stop the line if `C.r < x0`. If `x1 < C.l`, advance and continue. If `x < C.l`,
     `u0 += (C.l - x0)/sx` and `x0 = C.l`. If `x1 > C.r`, `u1 -= (x1 - C.r)/sx` and `x1 = C.r`.
     Emit a quad (screen space, `z = 0.01`, `rhw = 1`):

     | vertex | x (2D) | y | u | v |
     |---|---|---|---|---|
     | 0 | `(int)(x0 + italic + 0.5) - 0.25` | top | `u0 + du` | `g.v + t0*g.h + dv` |
     | 1 | `(int)(x1 + italic + 0.5) - 0.25` | top | `u1` | same |
     | 2 | `(int)(x1 + 0.5) - 0.25` | bot | `u1` | `g.v + t1*g.h` |
     | 3 | `(int)(x0 + 0.5) - 0.25` | bot | `u0 + du` | same |

     Indices `{0,1,2, 0,2,3}` per quad. A pending caret is drawn over `x0..x1`. Then
     `x = x1 + sx*g.post + +0xa4`.
7. Colors, filled for all quads with the pattern `{top, top, bottom, bottom}`:
   - Not lit: Gradient (`+0x5c & 1`) gives `top = start (+0x34)`, `bottom = end (+0x38)`. When the line
     was clipped vertically, they are re-interpolated to `lerp(start, end, t0)` and `lerp(start, end, t1)`.
     Without Gradient, both are `start`.
   - Lit (`+0x5c & 4`): no colors, normals `(0,0,1)`.
8. One `DrawPrimitive(TRIANGLELIST, indices, 6*nquads)` per line (also appended to the compiled cache).

**Shadow** (`FUN_25391930`): if the shadow color's alpha byte (`+0x3f`) is non-zero, first draw the line
at `(x + shadow.x, y + shadow.y)` with `start = shadow color`, Gradient off and `scale = shadow scale`
(`+0x48/+0x4c`). Only the glyph sizes change. The advances `+0xa0/+0xa4` keep the normal scale. Then
restore the fields and draw the normal pass.

**Caret** (`FUN_25391700(dev, x, y, w, h)`): if a caret material exists, `SetAsCurrent` it. Draw a
triangle-fan quad `x..x+w` by `y + (1 - caretSize)*h .. y + h`, i.e. a bar of `caretSize` of the glyph
height at the bottom. UV is the unit square, the color is the caret material's diffuse (default
`(1,1,1,0.5)`). Then re-apply the text states (`FUN_25391a30`). The caret is drawn before the line's
glyph batch, so it ends up under the text.

### 5.4 Background (`FUN_25390980`)

Only with flag 2 and a background material (unused by Ballance). `SetAsCurrent(material)`, Z write off,
a fan quad over `orig` (the unmargined, possibly resized entity rect), UV `(0,0)(1,0)(1,1)(0,1)`, and the
material diffuse as the vertex color. The lit variant uses normals `(0,0,1)`.

### 5.5 Text render states (`FUN_25391a30`)

Solid fill, no specular. Not lit: cull none, alpha blend `SRCALPHA / INVSRCALPHA`. Lit: the lit material's
`SetAsCurrent`. Texture = the font texture, alpha blending on, Z write off, Z test off (2D), Gouraud
shading with Gradient and flat otherwise, clamp addressing. Filtering is nearest with Disable Filter;
otherwise linear mag and linear min (linear-mipmap if the texture has mipmaps). In short: **alpha-blended
textured quads with per-vertex ARGB color modulating the texture, no depth**.

### 5.6 When it runs: the 2D entity's post-render callback

- 2D Text's execute registers `FUN_253869a0` with
  `CKRenderObject::AddPostRenderCallBack(fn, behaviorId, temporary=TRUE)` (vtable `+0x7c`, `CK2_3D`
  `0x10076c5b`) **every frame while active**. The callback is removed after one call.
- `CK2dEntity::Render` (`CK2_3D 0x1005ed00`), for a visible entity in the current scene, runs: pre-render
  callbacks, `Draw` (the entity's material quad; nothing if it has no material or is off screen), the
  children's `Render`, then the **post-render callbacks** (the text).
- So the text appears in the entity's 2D Z-order slot, after its own quad and its children, and before
  the next entity. **Entities without a material still get their text drawn.** Hidden entities (or ones
  not in the scene) draw nothing.

2D Text's render callback `FUN_253869a0(dev, entity, behaviorId)`:

1. Get the behavior, then the font manager. Set `manager+0x28 = (version >= 0x20000)`.
2. `font = GetFont(pin 0)`, and return if NULL. `text = pin 1` read pointer, and return if NULL.
3. Read Alignment (pin 2) and Margins (pin 3, default `(2,2,2,2)`) into `font+0x60`, Offset (pin 4) into
   `+0x2c`, Paragraph Indentation (pin 5) into `+0x24`, Caret Size (pin 7) into `+0x9c`, Caret Material
   (pin 8) into `+0x98`, and the setting `flags` (local 0).
4. `r = entity->GetRect()` (`+0x9c`, render-context pixels; for our homogeneous entities
   `rect * (640, 480)`). If `entity->IsRatioOffset()` (`+0xe0`, 2D flag `0x100`), add the view rect's
   left/top.
5. `dev->SetState(FOGENABLE(0x1c), 0)`.
6. If `entity->IsClipToCamera()` (`+0x10c`, flag `0x400`): `flags |= 0x800`, `font.clip = dev->GetViewRect()`.
   If the entity has Clip To Parent (`GetFlags() & 0x1000`) and a parent: intersect the clip with the
   parent's extents (`GetExtents` `+0x118`). The code replaces the clip with the clamped rect even when
   they don't overlap. If only parent clipping applies, the clip is the parent's extents.
7. Background material = pin 6.
8. Multiple (`0x100`): queue for the manager's post-render (`FUN_2538cb80`) and return. Unused.
9. If the entity is **not** clip-to-camera, temporarily set the viewport to the whole window
   (`GetWindowRect` translated to the origin, `SetViewRect`) so the text can leave the 3D viewport. It is
   restored after the draw.
10. `font->Draw(dev, entity, text, align, &r, bgMat, flags, 1)`.
11. Outputs: "Text Extents" (pOut 0) = `font+0x70` rect, "Line Count" (pOut 1) = `font+0x94`. They are
    updated at render time.
12. If `dev->vtable+0x148()` (the scene's fog state), re-enable fog.

## 6. Building Blocks

### 6.1 TT CreateFontEx `260e4eb0:0e256b90` (TT_Toolbox_RT; decl fn thunk `0x100013ed` -> `FUN_10026540`)

Pins: 0 Font Name (String), 1 Font Texture (Texture), 2 Horizontal/Vertical Character Number (2D Vector,
default `(16, 8)`), 3 Proportionnal (Bool, TRUE), 4 Font Bounds (Rect, default the whole texture), 5 First
Character (Int, 0), 6 FontCoordinatesData (Array). pOut 0 Font. In/Out.

1. Activate Out immediately. Return if the name is NULL. If the texture is NULL, return `0xa008`
   without creating a font.
2. `idx = CreateTextureFont(name, tex, &bounds, &counts, fixed = !proportional, first, 0.3)`. Write it to
   pOut 0.
3. `font = GetFont(idx)` (no NULL check). For `row = 0..254`, read columns 0..5 of the array (float cells)
   into `g[row] = {u, v, w, pre, post, h}`. Rows the array lacks keep the last values read. Entry 255 is
   not touched.

Game: 10 uses.

| Font | Where | Inputs |
|---|---|---|
| `GameFont_01`, `GameFont_02`, `GameFont_03`, `GameFont_03a`, `GameFont_04`, `GameFont_Credits_Small`, `GameFont_Credits_Big` | All_Menu "Fonts" | texture `Font_1` (512x512 32-bit TGA), array `M_FontData_01`, counts (16,8), proportional, bounds (0,0,0,0) = whole texture, first 0 |
| `Font_Punktezähler`, `Font_Tutorial` | All_Gameplay "Init" | texture and array from Gameplay_Init locals set at runtime |
| `Eval Copy Font` | Level | from operations; the evaluation-copy notice |

`M_FontData_01` has 255 rows in a 16x16 grid: `v = row/16`, `h = 0.0625`, and u values in 1/512 steps.
The glyph table for Ballance is therefore fully data-driven, and the alpha scan (section 4) is unused.

### 6.2 Set Font Properties `dacfbd61:7a6e65e7` (Interface `FUN_2538a760`, callback `FUN_2538a8f0`)

Pins: 0 Font, 1 Space (2D Vector `0,0`), 2 Scale (`1,1`), 3 Italic Offset (Float 0), 4 Color, 5 End Color
(`0,0,0,255`), 6 Shadow Color, 7 Shadow Angle (Angle, 120 degrees), 8 Shadow Distance (Float 4), 9 Shadow
Size (2D Vector `1,1`), 10 Lit Material. Setting: Font Properties flags, stored in `+0x5c`. In/Out.

Activate Out first. `font = GetFont(pin 0)`. If NULL, do nothing. Otherwise: Space into `+0x10`, Scale
into `+0x18`, Italic into `+0x20`. Color, End Color and Shadow Color go through `RGBAFTOCOLOR` into
`+0x34/+0x38/+0x3c`, each only if its read succeeds. If the angle reads (default 1.0 rad), distance
(default 4) gives `+0x40 = -cos(angle)*dist` and `+0x44 = sin(angle)*dist`. Shadow Size into `+0x48`,
Lit Material into `+0x58`, the setting into `+0x5c`. The callback only enables pins: End Color iff
Gradient; pins 6..9 iff Shadow; Lit Material iff Lighting (which disables Color and End Color).

The parameter reads copy raw bytes by pin **index**. Several game instances (All_Menu "Fonts" and "Fade
In and Out") have lost the Shadow Distance pin: pin 8 is a 2D Vector `(4,0)`, pin 9 is the Lit Material
(id 0, copied into `+0x48`), and pin 10 does not exist. All of those have flags 0, so this is harmless.

Game: 16 uses. Flags are 0 (13) or 2 = Shadow (3). Italic is always 0. Color is often driven by an
Interpolator (fades; executed every frame). Examples: menu fonts use Scale (0.45,0.55), (0.35,0.4),
(0.7,0.8), (0.6,0.6) with shadow, and (0.4,0.45). Gameplay uses Space (1.5,1) with Scale (0.8,0.9), and
Space (-1.3,-1) with Scale (0.4,0.5). Shadows use angle 120 degrees and distance 2 to 4 (so `+0x40/+0x44`
= `(1.0, 1.73)` for distance 2), color alpha 0.5.

### 6.3 Create System Font `936334fc:f243684f` (Interface `FUN_25389a60`, callback `FUN_25389c50`)

Pins: 0 Font Name (String), 1 System Font Name (enum `7167091a`, index into the installed fonts), 2 Font
Weight (400), 3 Italic, 4 Underline. pOut 0 Font Created. Settings: 0 Texture Resolution (enum, 4 =
512), 1 Full Charset (TRUE), 2 Render Control Characters (FALSE), 3 Dynamic Texture (TRUE), 5 Font Size
(0 = automatic). Local 4 is Current Font (String, the name that fixes up the enum index at load). Outputs:
Success and Error.

Execute: `tex = CreateSystemFontTexture(...)` (`FUN_2538dcd0`). This makes (or reuses by name
`"FONT <face> <res> ..."`) a 32-bit texture of `128*res` by `(fullCharset ? 128 : 64)*res` pixels. Each
char of a 16-column grid (16 or 8 rows; chars < 32 apparently only with "render control chars") is drawn with GDI
`DrawText` in white on black into square cells of `width/16` px. The result becomes RGB white with alpha
= luminance. The texture is dynamic (`0x10` creation option) if the setting says so, and is added to the
level. Then `CreateTextureFont(name, tex, (0,0,texW,texH), (16, fullCharset ? 16 : 8), fixed = 0,
first = 0, 0.3)`, which takes the GDI ABC-width path of section 4. Output the index and activate Success.
On any failure it returns `0xa008` **without activating Error**.

Without GDI, we need: a pre-rasterized atlas with the same grid layout (Arial, 512x512, 16x16 cells of
32x32 px, white with alpha coverage) and per-char `A/B/C` widths in texels (u = cell.u + A, pre = A,
w = B, post = C, h = 1/16). Alternatively, alias this font to a TT CreateFontEx-style table over
`Font_1`. **Game: one use**, Level "create DebugFont" -> `BaseFont` (Arial, NORMAL, 512, full charset,
dynamic). It feeds a Set Font Properties (scale 0.4) in the same debug script. It is low priority.

### 6.4 2D Text `055b29fe:662d5ca0` (Interface execute `FUN_253868e0`, callback `FUN_25387360`, render `FUN_253869a0`)

Target: a 2D entity. IO: On, Off -> Exit On, Exit Off. With "Multiple" (0x100) the callback changes these to
In/Out and removes the outputs. Pins: 0 Font, 1 Text (String), 2 Alignment (`Top-Left`), 3 Margins (Rect
`(2,2),(2,2)`), 4 Offset (2D Vector), 5 Paragraph Indentation (2D Vector), 6 Background Material, 7 Caret
Size (Percentage, 0.1), 8 Caret Material. pOut 0 Text Extents (Rect), 1 Line Count (Int). Setting 0 Text
Properties (flags). Behavior flags `0xf040000`.

Execute:

- Off active: clear it, activate Exit Off, return 0 (done). No callback is registered, so the text is
  gone from the next frame.
- On active: clear it, activate Exit On. With Multiple, draw once through the queue and return 0.
- If there is a target, `AddPostRenderCallBack(FUN_253869a0, behaviorId, TRUE)`. Return 1 (active next
  frame).

All parameters are read at render time (5.6). The settings callback only enables pins: 6 iff Background;
7 and 8 iff Show Caret.

**Game: 110 uses** (109 v2.0, 1 v1.0 = All_Gameplay "Tutorial Text", which uses `ls = spacing.x`).

| What | Values used (count) |
|---|---|
| Text Properties | `0x1` Screen Proportional (102), `0x21` + WordWrap (6: Tutorial Text, menu Initialize, Text Fader x2, Menu_Highscore, Menu_HighscoreEntry), `0x81` + Compiled (1: Level "show Version"), `0x201` + Show Caret (1: Menu_HighscoreEntry, caret material `M_Caret`, size 0.1) |
| Alignment | 0 Center (58), 1 Left (24), 2 Right (19), 4 Top (7), 5 Top-Left (2) |
| Margins | `(2,2,2,2)` (100), `(2,2,10,2)` (10) |
| Offset, Paragraph Indentation | always `(0,0)` |
| Background Material | none (the Background flag is never set) |
| Font | from `Fonts`/`Init` locals written by TT CreateFontEx (97), Parameter Selector (12), Get Cell (1) |

Never used: Background, Clip To Dimension, Resize, Justified, Multiple, the 3D mode, and Lighting (Set
Font Properties). Also never used: Gradient, italic, non-zero offsets and indents. The needed feature
set is: screen-proportional fonts; center/left/right/top alignment; margins; letter spacing (v1/v2);
leading; word wrap with `'\n'` paragraphs; shadow; the caret; vertex color alpha fades; and "compiled"
(treat as uncompiled if the text is static, which "show Version" is).

### 6.5 Text Display `f22d010a:f2cd010a` (Interface `FUN_25385d90`, callback `FUN_25386080`)

Pins: 0 Offset (2D Vector), 1 Color, 2 Alignment (Sprite Text Alignment, `Left`), 3 Size (Int, 10), 4 Text
(String), plus any extra pins. Locals: 0 the sprite, 1 font `{name[?], weight, flags}` ("Arial", 400), 4
Sprite Size (2D Vector, default `(320,32)`). IO: On/Off -> Exit On/Exit Off.

- The callback, on attach/load, creates a `CKSpriteText` (class `0x1d`) named `"TextDisplay Sprite"`.
  It is created dynamic (option `0x10`) iff the behavior itself is dynamic (`flags & 0x108`). Its flags get `|= 0x23`, it gets
  `EnableClipToCamera(FALSE)`, `ReleaseAllSlots` (`+0x180`) and `Create(size.x, size.y, ...)`
  (`+0x124`), and it is added to the level. The sprite is shown/hidden on On/Off and scene
  (de)activation (`CKObject::Show` = vtable `+0`), and destroyed on detach/delete.
- Execute: Off hides the sprite and returns 0. On shows it. Every frame: the text is the concatenation
  of the string forms of pins 4.. each followed by a space. `SetPosition(offset)` (`+0x8c`, pixels).
  If the text differs from `GetText()` (`+0x1e4`): `SetTextColor(RGBA(pin 1))` (`+0x1e8`),
  `SetAlign(pin 2)` (`+0x1fc`), `SetFont(name, size, weight, italic = flags&1, underline = flags&2)`
  (`+0x1f8`), `SetText` (`+0x1e0`). Returns 1.

Game: 7 uses, all debug (Level scripts "Player Active?", "Display Faces", "Display FPS", "Display Textures"
and one All_Gameplay "Display Textures"). Alignment 2 (Left), size 10, sprite size 320x25. It can be skipped,
or drawn with any built-in debug font. (The 10 "Text Display" objects under All_Menu Highscore are
behavior graphs with that name, not this BB.)

### 6.6 TT PushButton2 `14d325d1:6748654e` (TT_Toolbox_RT; decl fn thunk `0x1000169a` -> `FUN_10021130`)

"push button without material change". Target: a 2D entity (compatible class `0x1b`). IO: On, Off ->
Released, Roll Over, Mouse Down. No pins. Locals: 0 state (Int: bit 0 = mouse over), 1 pressed (Bool). No
text, materials or sounds: the menu scripts do those around it.

Per frame (returns 1 = stays active):

1. On active: clear it, and set state = 0 and pressed = FALSE (then continue the same frame). Off active:
   clear it and return 0.
2. Input manager `f787c904:0` and the player render context. If either is missing, return 1.
3. `over = (dev->Pick2D(mouse) == target) && input->GetCursorVisibility()`.
   - The mouse comes from `GetMousePosition(pos, FALSE)` (window-relative), plus the window rect's
     left/top from `GetWindowRect(r, FALSE)`.
   - `Pick2D` is render context `+0x1c8`: the topmost pickable 2D entity under the point.
   - The input manager slots are: `+0xe4` GetCursorVisibility (`Dx5InputManager 0x24ac2020`), `+0xa4`
     IsMouseButtonDown (`0x24ac1360`: state[button] & 1), `+0xb4` GetMousePosition (`0x24ac13c0`).
4. `down = IsMouseButtonDown(0)` (left). If `down` and not pressed: pressed = TRUE, `changed = 1`. If
   not `down` and pressed: pressed = FALSE, `changed = 1`.
5. If `over != state`: store it. Over -> not over activates output 0 **"Released"** (it really means
   roll-out). Not over -> over activates output 1 "Roll Over".
6. If `changed && over && down`: activate output 2 "Mouse Down" (the press edge while over the button).

Game: 67 uses, all All_Menu.

## 7. Implementation notes for OpenBallance

- Keep a `CkFontManager` with a 1-based `CkTextureFont*` array and the Font-type enum semantics
  (index values, 0 = none, removal fix-ups if levels delete font textures). Fonts are only created at
  runtime. `GetFont` must require "built".
- The glyph table is `float g[256][6]`. For Ballance fonts, fill it from the data array. Implement the
  alpha scan only if a font without an array ever shows up.
- Run the 2D Text layout (5.1/5.2) exactly as above, including the quirks: the offset only for Left/Top,
  the extents x using the vertical bits, the glyph height from `g[0]`, the v2 letter spacing
  `0.1 * spaceAdvance * spacing.x`, and the rounding of the line origin and letter spacing.
- Rendering: add a "post-render" hook per 2D entity to the 2D pass in `src/render/render.c`. It runs in
  Z order right after the entity's quad, also for entities without a material, and must re-register
  every frame (the BB's execute). The text becomes textured quads in render-context pixels: the
  `Font_1` texture, vertex colors ARGB, `SRCALPHA/INVSRCALPHA`, no depth. Screen-proportional sizes use
  the render context size. With the 640x480 logical context, lay out in 640x480 pixels and scale the
  quads. The `-0.25` px / `+0.25` texel offsets are D3D pixel-center tweaks. Keep the UV one (it avoids
  bleeding from the neighbor cell) and the position one is optional.
- Text is bytes (`unsigned char`); German umlauts index the table directly (Latin-1 / cp1252). The caret
  is byte `0x08`.

## Uncertain

- Render context vtable `+0x148` (queried after the draw to re-enable fog) is assumed to be the fog state.
- Whether the Level debug scripts (Create System Font, Text Display, "show Version", "Show Evaluation
  Copy") ever run in the retail game is not checked.
- The texture-stage state `0x27` set to 4 in `FUN_25391a30` is not identified. It is irrelevant for us.
