#include "movie.h"
#include "vfs.h"
#include <stdlib.h>
#include <string.h>

struct Movie {
    uint8_t *file;
    size_t size;
    uint32_t w, h, bpp, nframes;
    float fps;
    uint32_t *offsets, *lengths;       /* frame chunks ('00dc'/'00db') */
    uint8_t palette[256][3];
    uint16_t *pix;                     /* current frame (RGB555 or palette index), top-down */
    uint8_t *rgba;
    int32_t current;
};

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void parse(Movie *m, size_t p, size_t end)
{
    while (p + 8 <= end) {
        uint32_t sz = rd32(m->file + p + 4);
        const uint8_t *id = m->file + p, *body = m->file + p + 8;
        if (sz > end - p - 8) sz = (uint32_t)(end - p - 8);
        if (!memcmp(id, "LIST", 4) || !memcmp(id, "RIFF", 4)) {
            if (sz >= 4 && !memcmp(body, "movi", 4)) {
                for (size_t q = p + 12; q + 8 <= p + 8 + sz;) {
                    uint32_t s = rd32(m->file + q + 4);
                    if (!memcmp(m->file + q + 2, "dc", 2) || !memcmp(m->file + q + 2, "db", 2)) {
                        m->offsets = realloc(m->offsets, (m->nframes + 1) * 4);
                        m->lengths = realloc(m->lengths, (m->nframes + 1) * 4);
                        m->offsets[m->nframes] = (uint32_t)(q + 8);
                        m->lengths[m->nframes] = s;
                        m->nframes++;
                    }
                    q += 8 + s + (s & 1);
                }
            } else {
                parse(m, p + 12, p + 8 + sz);
            }
        } else if (!memcmp(id, "strh", 4) && sz >= 28 && !memcmp(body, "vids", 4)) {
            uint32_t scale = rd32(body + 20), rate = rd32(body + 24);
            m->fps = scale ? (float)rate / (float)scale : 25.0f;
        } else if (!memcmp(id, "strf", 4) && sz >= 40 && !m->w) {
            m->w = rd32(body + 4);
            m->h = rd32(body + 8);
            m->bpp = rd16(body + 14);
            if (memcmp(body + 16, "CRAM", 4) && memcmp(body + 16, "cram", 4) && memcmp(body + 16, "MSVC", 4) &&
                memcmp(body + 16, "msvc", 4) && memcmp(body + 16, "WHAM", 4))
                m->bpp = 0;   /* unsupported codec */
            if (m->bpp == 8)
                for (uint32_t i = 0; i < 256 && 40 + i * 4 + 3 <= sz; i++) {
                    m->palette[i][0] = body[40 + i * 4 + 2];
                    m->palette[i][1] = body[40 + i * 4 + 1];
                    m->palette[i][2] = body[40 + i * 4];
                }
        }
        p += 8 + sz + (sz & 1);
    }
}

Movie *movie_open(const char *path)
{
    Movie *m = calloc(1, sizeof *m);
    m->file = vfs_read_all(path, &m->size);
    if (!m->file || m->size < 12 || memcmp(m->file, "RIFF", 4) || memcmp(m->file + 8, "AVI ", 4)) goto fail;
    parse(m, 12, m->size);
    if (!m->w || !m->h || m->w > 4096 || m->h > 4096 || (m->bpp != 16 && m->bpp != 8) || !m->nframes) goto fail;
    if (m->w % 4 || m->h % 4) goto fail;
    m->pix = calloc((size_t)m->w * m->h, sizeof *m->pix);
    m->rgba = calloc((size_t)m->w * m->h, 4);
    m->current = -1;
    return m;
fail:
    movie_close(m);
    return NULL;
}

void movie_close(Movie *m)
{
    if (!m) return;
    free(m->file);
    free(m->offsets);
    free(m->lengths);
    free(m->pix);
    free(m->rgba);
    free(m);
}

uint32_t movie_width(const Movie *m) { return m->w; }
uint32_t movie_height(const Movie *m) { return m->h; }
uint32_t movie_frames(const Movie *m) { return m->nframes; }
float movie_fps(const Movie *m) { return m->fps; }

/* Microsoft Video 1: 4x4 blocks, block rows from the bottom of the image, each block's pixel rows from
   its bottom; codes: skip n blocks (0x84xx..0x87xx), 1 colour (>= 0x8000), 2 colours (flags, 2 colours),
   8 colours (flags, colour 0 with bit 15 set, 8 colours: two per quadrant). 8-bit streams use bytes. */
static void decode(Movie *m, const uint8_t *s, uint32_t len)
{
    uint32_t bw = m->w / 4, bh = m->h / 4, total = bw * bh, skip = 0;
    const uint8_t *end = s + len;
    bool wide = m->bpp == 16;
    uint32_t csize = wide ? 2 : 1;
    for (int32_t by = (int32_t)bh - 1; by >= 0; by--)
        for (uint32_t bx = 0; bx < bw; bx++) {
            if (skip) {
                skip--;
                total--;
                continue;
            }
            if (s + 2 > end) return;
            uint32_t a = s[0], b = s[1];
            s += 2;
            if (a == 0 && b == 0 && total == 0) return;
            uint16_t col[8];
            uint32_t flags = 0, mode;
            if ((b & 0xfc) == 0x84) {
                skip = ((b - 0x84) << 8) + a - 1;
                total--;
                continue;
            } else if (b < 0x80) {
                flags = b << 8 | a;
                if (s + 2 * csize > end) return;
                col[0] = (uint16_t)(wide ? rd16(s) : s[0]);
                col[1] = (uint16_t)(wide ? rd16(s + csize) : s[1]);
                s += 2 * csize;
                bool eight = wide ? (col[0] & 0x8000) != 0 : false;
                if (!wide && (b & 0x80) == 0 && 0) eight = true;
                if (eight) {
                    if (s + 6 * csize > end) return;
                    for (int k = 2; k < 8; k++, s += csize) col[k] = (uint16_t)(wide ? rd16(s) : s[0]);
                    mode = 8;
                } else {
                    mode = 2;
                }
            } else {
                col[0] = (uint16_t)(wide ? (b << 8 | a) : a);
                mode = 1;
            }
            for (uint32_t py = 0; py < 4; py++) {           /* py = 0 is the block's bottom row */
                uint16_t *row = m->pix + (size_t)(by * 4 + 3 - py) * m->w + bx * 4;
                for (uint32_t px = 0; px < 4; px++, flags >>= 1) {
                    uint16_t c;
                    if (mode == 1) c = col[0];
                    else if (mode == 2) c = col[(flags & 1) ^ 1];
                    else c = col[((py & 2) << 1) + (px & 2) + ((flags & 1) ^ 1)];
                    row[px] = wide ? (uint16_t)(c & 0x7fff) : c;
                }
            }
            total--;
        }
}

const uint8_t *movie_frame(Movie *m, uint32_t i)
{
    if (i >= m->nframes) i = m->nframes - 1;
    if ((int32_t)i < m->current) {
        m->current = -1;
        memset(m->pix, 0, (size_t)m->w * m->h * sizeof *m->pix);
    }
    while (m->current < (int32_t)i) {
        m->current++;
        if (m->offsets[m->current] + m->lengths[m->current] <= m->size)
            decode(m, m->file + m->offsets[m->current], m->lengths[m->current]);
    }
    for (size_t k = 0; k < (size_t)m->w * m->h; k++) {
        uint16_t c = m->pix[k];
        uint8_t *d = m->rgba + k * 4;
        if (m->bpp == 16) {
            d[0] = (uint8_t)((c >> 10 & 31) * 255 / 31);
            d[1] = (uint8_t)((c >> 5 & 31) * 255 / 31);
            d[2] = (uint8_t)((c & 31) * 255 / 31);
        } else {
            d[0] = m->palette[c & 0xff][0];
            d[1] = m->palette[c & 0xff][1];
            d[2] = m->palette[c & 0xff][2];
        }
        d[3] = 255;
    }
    return m->rgba;
}
