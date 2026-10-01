#include "image.h"
#include "vfs.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool bmp(const uint8_t *d, size_t n, Image *o)
{
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return false;
    uint32_t off = rd32(d + 10), hs = rd32(d + 14);
    int32_t w = (int32_t)rd32(d + 18), h = (int32_t)rd32(d + 22);
    uint32_t bpp = rd16(d + 28), comp = rd32(d + 30);
    if (w <= 0 || h == 0 || w > 8192 || h > 8192 || h < -8192 || (comp != 0 && comp != 3)) return false;
    bool topdown = h < 0;
    if (topdown) h = -h;
    const uint8_t *pal = d + 14 + hs;
    uint32_t stride = ((uint32_t)w * bpp + 31) / 32 * 4;
    if (off + (size_t)stride * h > n) return false;
    o->w = (uint32_t)w;
    o->h = (uint32_t)h;
    o->rgba = malloc((size_t)w * h * 4);
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *row = d + off + (size_t)stride * (topdown ? y : h - 1 - y);
        uint8_t *dst = o->rgba + (size_t)y * w * 4;
        for (int32_t x = 0; x < w; x++, dst += 4) {
            if (bpp == 24) {
                dst[0] = row[x * 3 + 2], dst[1] = row[x * 3 + 1], dst[2] = row[x * 3], dst[3] = 255;
            } else if (bpp == 32) {
                dst[0] = row[x * 4 + 2], dst[1] = row[x * 4 + 1], dst[2] = row[x * 4], dst[3] = row[x * 4 + 3];
            } else if (bpp == 8) {
                const uint8_t *c = pal + row[x] * 4;
                dst[0] = c[2], dst[1] = c[1], dst[2] = c[0], dst[3] = 255;
            } else {
                free(o->rgba);
                o->rgba = NULL;
                return false;
            }
        }
    }
    return true;
}

static bool tga(const uint8_t *d, size_t n, Image *o)
{
    if (n < 18) return false;
    uint32_t idlen = d[0], cmap = d[1], type = d[2];
    uint32_t w = rd16(d + 12), h = rd16(d + 14), bpp = d[16], desc = d[17];
    if (cmap || (type != 2 && type != 10) || (bpp != 24 && bpp != 32) || !w || !h || w > 8192 || h > 8192) return false;
    size_t p = 18 + idlen + 0;
    uint32_t bytes = bpp / 8;
    o->w = w;
    o->h = h;
    o->rgba = malloc((size_t)w * h * 4);
    uint8_t *px = malloc((size_t)w * h * 4);
    size_t total = (size_t)w * h, k = 0;
    while (k < total) {
        if (type == 2) {
            if (p + bytes > n) break;
            memcpy(px + k * 4, d + p, bytes);
            p += bytes;
            k++;
            continue;
        }
        if (p >= n) break;
        uint8_t hdr = d[p++];
        uint32_t cnt = (hdr & 0x7f) + 1;
        if (hdr & 0x80) {
            if (p + bytes > n) break;
            for (uint32_t i = 0; i < cnt && k < total; i++, k++) memcpy(px + k * 4, d + p, bytes);
            p += bytes;
        } else {
            for (uint32_t i = 0; i < cnt && k < total; i++, k++) {
                if (p + bytes > n) break;
                memcpy(px + k * 4, d + p, bytes);
                p += bytes;
            }
        }
    }
    bool top = (desc & 0x20) != 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = px + (size_t)(top ? y : h - 1 - y) * w * 4;
        uint8_t *dst = o->rgba + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            dst[x * 4] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4];
            dst[x * 4 + 3] = bytes == 4 ? src[x * 4 + 3] : 255;
        }
    }
    free(px);
    return k == total;
}

bool image_decode(const uint8_t *data, size_t len, Image *out)
{
    memset(out, 0, sizeof *out);
    if (bmp(data, len, out)) return true;
    image_free(out);
    return tga(data, len, out);
}

bool image_load(const char *path, Image *out)
{
    size_t n;
    uint8_t *d = vfs_read_all(path, &n);
    if (!d) {
        memset(out, 0, sizeof *out);
        return false;
    }
    bool ok = image_decode(d, n, out);
    free(d);
    return ok;
}

void image_free(Image *img)
{
    free(img->rgba);
    img->rgba = NULL;
    img->w = img->h = 0;
}

bool image_downsample(const Image *s, Image *d)
{
    if (s->w <= 1 && s->h <= 1) return false;
    d->w = s->w > 1 ? s->w / 2 : 1;
    d->h = s->h > 1 ? s->h / 2 : 1;
    d->rgba = malloc((size_t)d->w * d->h * 4);
    for (uint32_t y = 0; y < d->h; y++)
        for (uint32_t x = 0; x < d->w; x++)
            for (int c = 0; c < 4; c++) {
                uint32_t x0 = x * 2, y0 = y * 2;
                uint32_t x1 = x0 + 1 < s->w ? x0 + 1 : x0, y1 = y0 + 1 < s->h ? y0 + 1 : y0;
                uint32_t sum = s->rgba[((size_t)y0 * s->w + x0) * 4 + c] + s->rgba[((size_t)y0 * s->w + x1) * 4 + c] +
                               s->rgba[((size_t)y1 * s->w + x0) * 4 + c] + s->rgba[((size_t)y1 * s->w + x1) * 4 + c];
                d->rgba[((size_t)y * d->w + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
            }
    return true;
}
