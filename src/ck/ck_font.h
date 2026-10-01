/* The font manager of Interface.dll (CKFontManager, GUID 64fb5810:73262d3b) and texture fonts
   (CKTextureFont): a glyph table over a bitmap, laid out and drawn by 2D Text. See docs/fonts.md for the
   original functions and the quirks kept here. */
#pragma once
#include "ck.h"
#include "ck_3d.h"

/* Font Properties flags (Set Font Properties), Text Properties flags (2D Text) */
enum { CKFONT_GRADIENT = 1, CKFONT_SHADOW = 2, CKFONT_LIGHTING = 4, CKFONT_NOFILTER = 8 };
enum { CKTEXT_SCREEN = 1, CKTEXT_BACKGROUND = 2, CKTEXT_CLIP = 4, CKTEXT_RESIZE_V = 8, CKTEXT_RESIZE_H = 0x10,
       CKTEXT_WORDWRAP = 0x20, CKTEXT_JUSTIFIED = 0x40, CKTEXT_COMPILED = 0x80, CKTEXT_MULTIPLE = 0x100,
       CKTEXT_CARET = 0x200 };
/* Alignment enum (2e1e2209:47da44b5) */
enum { CKALIGN_LEFT = 1, CKALIGN_RIGHT = 2, CKALIGN_TOP = 4, CKALIGN_BOTTOM = 8 };

typedef struct CkFont {
    char *name;
    int32_t first;                /* +0x0c */
    float spacing[2], scale[2];   /* +0x10, +0x18 */
    float italic;                 /* +0x20 */
    float indent[2], offset[2];   /* +0x24, +0x2c */
    uint32_t start_color, end_color, shadow_color;   /* ARGB, +0x34.. */
    float shadow_offset[2], shadow_scale[2];         /* +0x40, +0x48 */
    CkId lit_material;            /* +0x58 */
    uint32_t props;               /* +0x5c CKFONT_* */
    float margins[4];             /* +0x60 l, t, r, b */
    float extents[4];             /* +0x70, output of the last draw */
    float empty_width;            /* +0x90 */
    int32_t line_count;           /* +0x94 */
    CkId caret_material;          /* +0x98 */
    float caret_size;             /* +0x9c */
    float counts[2];              /* +0xac */
    float rect[4];                /* +0xb4 font rect in texture pixels */
    uint32_t flags;               /* +0xc4: 1 fixed, 2 proportional computed, 4 built */
    CkId texture;
    uint32_t tex_w, tex_h;
    float g[256][6];              /* +0xd0: u, v, w, pre, post, h (texture units) */
} CkFont;

/* CreateTextureFont (vtable +0x78, FUN_2538c030): 1-based index, -1 without name or texture; a font of the
   same name is replaced in place. rect all zero = the whole texture. */
int32_t ck_font_create(CkContext *ctx, const char *name, CkId texture, const float rect[4], const float counts[2],
                       bool fixed, int32_t first, float empty_width);
CkFont *ck_font_get(const CkContext *ctx, int32_t index);   /* GetFont (+0x80): NULL unless built */
void ck_fonts_free(CkContext *ctx);

/* One glyph (or caret) quad in render-context pixels (640x480): corners top-left, top-right,
   bottom-right, bottom-left. Caret quads take the caret material's texture and diffuse. */
typedef struct {
    float x[4], y[4], u[4], v[4];
    uint32_t color[4];            /* ARGB */
    CkId texture;
    CkId caret_material;          /* != 0: a caret bar */
} CkTextQuad;
typedef void (*CkTextEmit)(void *user, const CkTextQuad *q);

/* 2D Text's post-render callback (FUN_253869a0) for behavior b on its entity: reads the pins, lays out
   and emits the quads, writes Text Extents and Line Count. */
void ck_text_render(CkContext *ctx, CkBehavior *b, Ck2dEntity *e, CkTextEmit emit, void *user);
