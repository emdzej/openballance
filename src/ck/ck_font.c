#include "ck_font.h"
#include "../image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The texture's pixel size: known for movies, else read from the current slot's file (as the renderer
   finds it: as named, then in Textures/). */
static void texture_size(CkTexture *t, uint32_t *w, uint32_t *h)
{
    if (!t->width && t->nfiles && t->files[0]) {
        char path[512];
        Image img;
        snprintf(path, sizeof path, "Textures/%s", t->files[0]);
        if (image_load(t->files[0], &img) || image_load(path, &img)) {
            t->width = img.w, t->height = img.h;
            image_free(&img);
        }
    }
    *w = t->width ? t->width : 1;
    *h = t->height ? t->height : 1;
}

/* FUN_25390130 + FUN_25390200: the default glyph table, cells of the font rect in row-major order from the
   first character. The proportional path scans the texture's alpha; Ballance overwrites every entry it
   uses from its FontCoordinatesData array right after creation (TT CreateFontEx), so the scan is left out
   and proportional fonts get the full cell width here. */
static void build_glyphs(CkFont *f)
{
    float tw = (float)f->tex_w, th = (float)f->tex_h;
    float cw = (f->rect[2] - f->rect[0]) / (tw * f->counts[0]), ch = (f->rect[3] - f->rect[1]) / (th * f->counts[1]);
    for (int i = 0; i < 256; i++) {
        f->g[i][0] = f->rect[0] / tw, f->g[i][1] = f->rect[1] / th, f->g[i][2] = cw;
        f->g[i][3] = f->g[i][4] = 0, f->g[i][5] = ch;
    }
    for (int r = 0; r < (int)f->counts[1]; r++)
        for (int c = 0; c < (int)f->counts[0]; c++) {
            int k = f->first + r * (int)f->counts[0] + c;
            if (k < 0 || k > 255) continue;
            f->g[k][0] = f->rect[0] / tw + (float)c * cw;
            f->g[k][1] = f->rect[1] / th + (float)r * ch;
        }
    if (!(f->flags & 1)) f->flags |= 2;
    f->flags |= 4;
}

int32_t ck_font_create(CkContext *ctx, const char *name, CkId texture, const float rect[4], const float counts[2],
                       bool fixed, int32_t first, float empty_width)
{
    CkTexture *t = ck_texture(ctx, texture);
    if (!name || !t) return -1;
    CkFont *f = calloc(1, sizeof *f);
    f->name = strdup(name);
    f->scale[0] = f->scale[1] = 1;
    f->start_color = 0xffffffff, f->end_color = 0xff000000, f->shadow_color = 0x80000000;
    f->shadow_offset[0] = f->shadow_offset[1] = 4;
    f->shadow_scale[0] = f->shadow_scale[1] = 1;
    f->counts[0] = counts[0], f->counts[1] = counts[1];
    f->flags = fixed ? 1 : 0;
    f->first = first;
    f->texture = texture;
    f->empty_width = empty_width;
    texture_size(t, &f->tex_w, &f->tex_h);
    memcpy(f->rect, rect, sizeof f->rect);
    if (rect[2] - rect[0] == 0 || rect[3] - rect[1] == 0)
        f->rect[0] = f->rect[1] = 0, f->rect[2] = (float)f->tex_w, f->rect[3] = (float)f->tex_h;
    build_glyphs(f);
    for (uint32_t i = 0; i < ctx->nfonts; i++)
        if (!strcmp(ctx->fonts[i]->name, name)) {
            free(ctx->fonts[i]->name);
            free(ctx->fonts[i]);
            ctx->fonts[i] = f;
            return (int32_t)i + 1;
        }
    ctx->fonts = realloc(ctx->fonts, (ctx->nfonts + 1) * sizeof *ctx->fonts);
    ctx->fonts[ctx->nfonts++] = f;
    return (int32_t)ctx->nfonts;
}

CkFont *ck_font_get(const CkContext *ctx, int32_t index)
{
    if (index < 1 || (uint32_t)index > ctx->nfonts) return NULL;
    CkFont *f = ctx->fonts[index - 1];
    return f->flags & 4 ? f : NULL;
}

void ck_fonts_free(CkContext *ctx)
{
    for (uint32_t i = 0; i < ctx->nfonts; i++) {
        free(ctx->fonts[i]->name);
        free(ctx->fonts[i]);
    }
    free(ctx->fonts);
    ctx->fonts = NULL;
    ctx->nfonts = 0;
}

/* ---- layout and drawing (CKTextureFont::Draw FUN_25391bf0 and its helpers) ---- */

/* CKBehavior::GetInputParameterValue on the resolved source */
static CkParameter *input_source(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    CkParameter *pin = i < b->pin.n ? ck_param(ctx, b->pin.v[i]) : NULL;
    CkParameter *p = pin ? ck_param_resolve(ctx, pin) : NULL;
    return p && p->kind != CKP_IN ? p : NULL;
}

static bool get_in(CkContext *ctx, CkBehavior *b, uint32_t i, void *dst, uint32_t size)
{
    CkParameter *p = input_source(ctx, b, i);
    if (!p || !p->value) return false;
    memcpy(dst, p->value, p->size < size ? p->size : size);
    return true;
}

typedef struct {
    const unsigned char *start;
    int32_t count;                /* negative: first line of a paragraph */
    int32_t spaces;
    float width;
} Line;

typedef struct {
    CkContext *ctx;
    CkFont *f;
    CkTextEmit emit;
    void *user;
    uint32_t flags;
    float basis[2];
    float space_adv, ls;          /* +0xa0, +0xa4 for the current line */
} Draw;

static float glyph_adv(const CkFont *f, unsigned c, float sx, float ls)
{
    return (f->g[c][3] + f->g[c][2] + f->g[c][4]) * sx + ls;
}

static void emit_caret(Draw *d, float x, float y, float w, float h)
{
    CkMaterial *m = ck_material(d->ctx, d->f->caret_material);
    CkTextQuad q;
    memset(&q, 0, sizeof q);
    float top = y + (1 - d->f->caret_size) * h, bot = y + h;
    q.x[0] = q.x[3] = x, q.x[1] = q.x[2] = x + w;
    q.y[0] = q.y[1] = top, q.y[2] = q.y[3] = bot;
    q.u[1] = q.u[2] = 1, q.v[2] = q.v[3] = 1;
    uint32_t col = 0x80ffffff;   /* default (1, 1, 1, 0.5) */
    if (m) {
        float c[4] = {m->diffuse.r, m->diffuse.g, m->diffuse.b, m->diffuse.a};
        col = 0;
        for (int i = 0; i < 4; i++) {
            float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
            col |= (uint32_t)(v * 255) << (i == 3 ? 24 : 16 - 8 * i);
        }
        q.texture = m->texture;
    }
    for (int i = 0; i < 4; i++) q.color[i] = col;
    q.caret_material = d->f->caret_material ? d->f->caret_material : (CkId)-1;
    d->emit(d->user, &q);
}

/* FUN_25390b70: one line at (x, y) with the given glyph scale and colour */
static void draw_line(Draw *d, const unsigned char *s, int32_t n, float x, float y, const float scale[2], uint32_t color)
{
    CkFont *f = d->f;
    if (n == 0) return;
    float sx = d->basis[0] * scale[0], sy = d->basis[1] * scale[1], italic = sx * f->italic;
    float du = 0.25f / (float)f->tex_w, dv = 0.25f / (float)f->tex_h;
    x = (float)(int)(x + 0.5f);
    y = (float)(int)(y + 0.5f);
    if (!(d->flags & CKTEXT_JUSTIFIED)) d->ls = (float)(int)(d->ls + 0.5f);
    float gh = sy * f->g[0][5], top = y, bot = y + gh;
    bool caret_pending = false;
    for (int32_t i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c == ' ') {
            if (caret_pending) emit_caret(d, x, y, d->space_adv, gh), caret_pending = false;
            x += d->space_adv;
            continue;
        }
        if (c == 0x08) {
            if (i == n - 1 && (d->flags & CKTEXT_CARET)) emit_caret(d, x, y, d->space_adv, gh);
            else caret_pending = true;
            continue;
        }
        const float *g = f->g[c];
        if (g[5] == 0) continue;
        float x0 = x + sx * g[3], x1 = x0 + sx * g[2], u0 = g[0], u1 = g[0] + g[2];
        if (caret_pending) emit_caret(d, x0, y, x1 - x0, gh), caret_pending = false;
        CkTextQuad q;
    memset(&q, 0, sizeof q);
        q.x[0] = (float)(int)(x0 + italic + 0.5f) - 0.25f;
        q.x[1] = (float)(int)(x1 + italic + 0.5f) - 0.25f;
        q.x[2] = (float)(int)(x1 + 0.5f) - 0.25f;
        q.x[3] = (float)(int)(x0 + 0.5f) - 0.25f;
        q.y[0] = q.y[1] = top, q.y[2] = q.y[3] = bot;
        q.u[0] = q.u[3] = u0 + du, q.u[1] = q.u[2] = u1;
        q.v[0] = q.v[1] = g[1] + dv, q.v[2] = q.v[3] = g[1] + g[5];
        uint32_t bottom = (f->props & CKFONT_GRADIENT) ? f->end_color : color;
        q.color[0] = q.color[1] = color, q.color[2] = q.color[3] = bottom;
        q.texture = f->texture;
        d->emit(d->user, &q);
        x = x1 + sx * g[4] + d->ls;
    }
}

/* FUN_25391bf0 with draw = 1, uncompiled (the one compiled text, "show Version", never changes) */
static void font_draw(Draw *d, const unsigned char *text, int32_t align, float rect[4])
{
    CkFont *f = d->f;
    rect[0] += f->margins[0], rect[1] += f->margins[1], rect[2] -= f->margins[2], rect[3] -= f->margins[3];
    if (d->flags & CKTEXT_SCREEN) d->basis[0] = CK_SCREEN_W, d->basis[1] = CK_SCREEN_H;
    else d->basis[0] = (float)f->tex_w, d->basis[1] = (float)f->tex_h;
    float sx = f->scale[0] * d->basis[0];
    const float *sp = f->g[' '];
    float ls = d->ls;             /* set by the caller: v2 or v1 letter spacing, see ck_text_render */
    float adv = (sp[2] + sp[3] + sp[4]) * sx + ls, avail = rect[2] - rect[0];
    bool wrap = (d->flags & (CKTEXT_WORDWRAP | CKTEXT_JUSTIFIED)) != 0;

    /* line records; the break state persists across lines (a break point before the current line counts
       as none: the original would jump back there) */
    Line lines[256];
    int32_t nlines = 0;
    const unsigned char *p = text, *brk = (const unsigned char *)"";
    bool in_word = true, para = true;
    int32_t brk_count = 0;
    float brk_w = 0;
    while (*p && nlines < 256) {
        Line rec = {p, 0, 0, 0};
        float w = para ? f->indent[0] * f->g[0][2] * sx : 0;
        for (; *p && *p != '\n'; p++, rec.count++) {
            unsigned c = *p;
            if (c == ' ') {
                in_word = true;
                brk_w = w;
                brk_count = rec.count;
                rec.spaces++;
                brk = p + 1;
                w += adv;
            } else {
                if (in_word) {
                    brk_w = w - adv;
                    brk_count = rec.count - 1;
                    brk = p;
                    in_word = false;
                }
                w += glyph_adv(f, c, sx, ls);
            }
            if (wrap && w > avail) {
                if (brk_count > 0 && brk >= rec.start) {
                    rec.count = brk_count;
                    w = brk_w;
                    rec.spaces--;
                    p = brk;
                } else {
                    if (c != ' ') w -= glyph_adv(f, c, sx, ls);
                    if (brk == p) p++;
                    brk = p;
                }
                in_word = true;
                break;
            }
        }
        rec.width = w;
        if (para) rec.count = -rec.count, para = false;
        if (*p == '\n') {
            para = true;
            if (*brk == '\n') p++;
        }
        lines[nlines++] = rec;
        if (!*p) break;
        if (p != brk && *brk != '\n') p++;
    }

    /* FUN_25390910: extents */
    float maxw = 0;
    for (int32_t i = 0; i < nlines; i++)
        if (lines[i].width > maxw) maxw = lines[i].width;
    float line_h = d->basis[1] * f->scale[1] * f->g[0][5] + f->spacing[1], total_h = (float)nlines * line_h;
    f->line_count = nlines;
    float y0 = (align & CKALIGN_TOP) ? rect[1] : (align & CKALIGN_BOTTOM) ? rect[3] - total_h
                                                                          : (rect[1] + rect[3]) / 2 - total_h / 2;
    /* the extents' x tests the vertical bits (original quirk) */
    float x0 = (align & CKALIGN_TOP) ? rect[0] : (align & CKALIGN_BOTTOM) ? rect[2] - maxw : (rect[0] + rect[2]) / 2 - maxw / 2;
    f->extents[0] = x0, f->extents[1] = y0, f->extents[2] = x0 + maxw, f->extents[3] = y0 + total_h;
    float y = (align & CKALIGN_TOP) ? rect[1] + f->offset[1] : y0;

    for (int32_t i = 0; i < nlines; i++) {
        Line *l = &lines[i];
        d->space_adv = adv;
        d->ls = ls;
        int32_t n = l->count < 0 ? -l->count : l->count;
        float x;
        if (d->flags & CKTEXT_JUSTIFIED) {
            x = rect[0];
            if (l->spaces > 0) d->space_adv = adv - (l->width - avail) / (float)l->spaces;
            else if (n > 1) d->ls = ls + (avail - l->width) / (float)(n - 1);
        } else if (align & CKALIGN_LEFT) {
            x = rect[0] + f->offset[0];
        } else if (align & CKALIGN_RIGHT) {
            x = rect[2] - l->width;
        } else {
            x = (rect[0] + rect[2]) / 2 - l->width / 2;
        }
        if (l->count < 0) {
            if (align & CKALIGN_LEFT) x += f->indent[0] * f->g[0][2] * sx;
            if (i > 0) y += d->basis[1] * f->scale[1] * f->indent[1] * f->g[0][5];
        }
        if ((f->props & CKFONT_SHADOW) && (f->shadow_color >> 24)) {    /* FUN_25391930 */
            float keep_adv = d->space_adv, keep_ls = d->ls;
            uint32_t props = f->props;
            f->props &= ~(uint32_t)CKFONT_GRADIENT;
            draw_line(d, l->start, n, x + f->shadow_offset[0], y + f->shadow_offset[1], f->shadow_scale, f->shadow_color);
            f->props = props;
            d->space_adv = keep_adv, d->ls = keep_ls;
        }
        draw_line(d, l->start, n, x, y, f->scale, f->start_color);
        y += line_h;
    }
}

void ck_text_render(CkContext *ctx, CkBehavior *b, Ck2dEntity *e, CkTextEmit emit, void *user)
{
    int32_t index = 0;
    get_in(ctx, b, 0, &index, 4);
    CkFont *f = ck_font_get(ctx, index);
    CkParameter *tp = input_source(ctx, b, 1);
    if (!f || !tp || !tp->value || !tp->size) return;
    int32_t align = 5;
    float margins[4] = {2, 2, 2, 2};
    get_in(ctx, b, 2, &align, 4);
    if (get_in(ctx, b, 3, margins, 16)) memcpy(f->margins, margins, sizeof margins);
    else f->margins[0] = f->margins[1] = f->margins[2] = f->margins[3] = 2;
    get_in(ctx, b, 4, f->offset, 8);
    get_in(ctx, b, 5, f->indent, 8);
    get_in(ctx, b, 7, &f->caret_size, 4);
    CkId caret = 0;
    if (get_in(ctx, b, 8, &caret, 4)) f->caret_material = caret;
    uint32_t flags = 0;
    CkParameter *lp = b->local.n ? ck_param(ctx, b->local.v[0]) : NULL;
    if (lp && lp->value && lp->size >= 4) memcpy(&flags, lp->value, 4);
    float r[4];
    ck_2d_get_homogeneous_rect(e, r);
    r[0] *= CK_SCREEN_W, r[2] *= CK_SCREEN_W, r[1] *= CK_SCREEN_H, r[3] *= CK_SCREEN_H;
    /* letter spacing: behaviors from version 2.0 on use 0.1 x the space advance x spacing.x, older ones
       spacing.x pixels (manager +0x28) */
    float basis_w = (flags & CKTEXT_SCREEN) ? CK_SCREEN_W : (float)f->tex_w;
    const float *sp = f->g[' '];
    Draw d = {ctx, f, emit, user, flags, {0, 0}, 0, 0};
    d.ls = b->proto_version >= 0x20000 ? (sp[2] + sp[3] + sp[4]) * f->spacing[0] * f->scale[0] * basis_w * 0.1f : f->spacing[0];
    char *text = malloc(tp->size + 1);
    memcpy(text, tp->value, tp->size);
    text[tp->size] = 0;
    font_draw(&d, (const unsigned char *)text, align, r);
    free(text);
    /* outputs, updated at render time: Text Extents, Line Count */
    if (b->pout.n > 0) {
        CkParameter *o = ck_param(ctx, b->pout.v[0]);
        if (o) ck_param_set(o, f->extents, 16);
    }
    if (b->pout.n > 1) {
        CkParameter *o = ck_param(ctx, b->pout.v[1]);
        if (o) ck_param_set(o, &f->line_count, 4);
    }
}
