#include "ck_file.h"
#include "../inflate.h"
#include "../vfs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *err, size_t n, const char *fmt, ...)
{
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return false;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A section: stored as-is when packed == size, else zlib. Returns a malloc'd copy. */
static uint8_t *section(const uint8_t *src, uint32_t packed, uint32_t size)
{
    uint8_t *dst = malloc(size ? size : 1);
    if (!dst) return NULL;
    if (packed == size) {
        memcpy(dst, src, size);
    } else if (zlib_inflate(src, packed, dst, size) != (long)size) {
        free(dst);
        return NULL;
    }
    return dst;
}

/* Parses a serialized chunk at p (4-byte aligned, in f->data). Returns bytes used or 0. */
static size_t parse_chunk(CkChunk *c, const uint8_t *p, size_t len)
{
    if (len < 8) return 0;
    uint32_t v = rd32(p);
    memset(c, 0, sizeof *c);
    c->data_version = v & 0xff;
    c->class_id = v >> 8 & 0xff;
    c->chunk_version = v >> 16 & 0xff;
    c->options = v >> 24;
    c->ndw = rd32(p + 4);
    size_t q = 8;
    if ((uint64_t)c->ndw * 4 > len - q) return 0;
    c->dw = (const uint32_t *)(p + q);
    q += (size_t)c->ndw * 4;
    const uint32_t **lists[3] = {&c->ids, &c->sub, &c->man};
    uint32_t *counts[3] = {&c->nids, &c->nsub, &c->nman};
    static const int bits[3] = {1, 4, 2};           /* read order: IDs, sub-chunks, managers */
    for (int i = 0; i < 3; i++) {
        if (!(c->options & bits[i])) continue;
        if (q + 4 > len) return 0;
        *counts[i] = rd32(p + q);
        q += 4;
        if ((uint64_t)*counts[i] * 4 > len - q) return 0;
        *lists[i] = (const uint32_t *)(p + q);
        q += (size_t)*counts[i] * 4;
    }
    return q;
}

bool ck_file_parse(CkFile *f, const uint8_t *b, size_t len, char *err, size_t errlen)
{
    memset(f, 0, sizeof *f);
    if (len < 64 || memcmp(b, "Nemo Fi\0", 8)) return fail(err, errlen, "not a Virtools file");
    f->ck_version = rd32(b + 12);
    f->file_version = rd32(b + 16);
    f->write_mode = rd32(b + 24);
    uint32_t h1pack = rd32(b + 28), dpack = rd32(b + 32), dsize = rd32(b + 36);
    f->nmanagers = rd32(b + 40);
    f->nobjects = rd32(b + 44);
    uint32_t h1size = rd32(b + 60);
    if (f->file_version != 8) return fail(err, errlen, "file version %u (only 8 is supported)", f->file_version);
    if ((uint64_t)64 + h1pack + dpack > len) return fail(err, errlen, "truncated");
    f->header1 = section(b + 64, h1pack, h1size);
    f->data = section(b + 64 + h1pack, dpack, dsize);
    if (!f->header1 || !f->data) {
        ck_file_free(f);
        return fail(err, errlen, "corrupt compressed section");
    }

    /* header1: object table */
    f->objects = calloc(f->nobjects ? f->nobjects : 1, sizeof *f->objects);
    f->names = malloc(h1size + f->nobjects + 1);
    f->managers = calloc(f->nmanagers ? f->nmanagers : 1, sizeof *f->managers);
    if (!f->objects || !f->names || !f->managers) {
        ck_file_free(f);
        return fail(err, errlen, "out of memory");
    }
    size_t p = 0, np = 0;
    for (uint32_t i = 0; i < f->nobjects; i++) {
        if (p + 16 > h1size) goto bad_h1;
        CkFileObject *o = &f->objects[i];
        o->id = rd32(f->header1 + p);
        o->class_id = rd32(f->header1 + p + 4);
        uint32_t nlen = rd32(f->header1 + p + 12);
        p += 16;
        if (nlen > h1size - p) goto bad_h1;
        o->name = f->names + np;
        memcpy(f->names + np, f->header1 + p, nlen);
        f->names[np + nlen] = 0;
        np += nlen + 1;
        p += nlen;
    }

    /* data: managers, then objects */
    p = 0;
    for (uint32_t i = 0; i < f->nmanagers; i++) {
        if (p + 12 > dsize) goto bad_data;
        CkFileManager *m = &f->managers[i];
        m->guid.a = rd32(f->data + p);
        m->guid.b = rd32(f->data + p + 4);
        uint32_t sz = rd32(f->data + p + 8);
        p += 12;
        if (sz > dsize - p) goto bad_data;
        if (sz) {
            if (parse_chunk(&m->chunk, f->data + p, sz) != sz) goto bad_data;
            m->has_chunk = true;
        }
        p += sz;
    }
    for (uint32_t i = 0; i < f->nobjects; i++) {
        if (p + 4 > dsize) goto bad_data;
        uint32_t sz = rd32(f->data + p);
        p += 4;
        if (sz > dsize - p) goto bad_data;
        if (sz) {
            CkFileObject *o = &f->objects[i];
            if (parse_chunk(&o->chunk, f->data + p, sz) != sz) goto bad_data;
            o->has_chunk = true;
        }
        p += sz;
    }
    if (p != dsize) goto bad_data;
    return true;
bad_h1:
    ck_file_free(f);
    return fail(err, errlen, "corrupt object table");
bad_data:
    ck_file_free(f);
    return fail(err, errlen, "corrupt data section at %zu", p);
}

bool ck_file_load(CkFile *f, const char *path, char *err, size_t errlen)
{
    size_t n;
    uint8_t *b = vfs_read_all(path, &n);
    if (!b) {
        memset(f, 0, sizeof *f);
        return fail(err, errlen, "%s: not found", path);
    }
    bool ok = ck_file_parse(f, b, n, err, errlen);
    free(b);
    return ok;
}

void ck_file_free(CkFile *f)
{
    free(f->header1);
    free(f->data);
    free(f->names);
    free(f->objects);
    free(f->managers);
    memset(f, 0, sizeof *f);
}

/* ---- reader ---- */

void ck_reader_init(CkReader *r, const CkChunk *c)
{
    r->c = c;
    r->pos = 0;
    r->end = c->ndw;
    r->error = false;
}

bool ck_seek(CkReader *r, uint32_t id)
{
    /* CKStateChunk::SeekIdentifier: walk the identifier list from the start. */
    const CkChunk *c = r->c;
    uint32_t i = 0;
    while (i + 1 < c->ndw) {
        uint32_t next = c->dw[i + 1];
        if (c->dw[i] == id) {
            r->pos = i + 2;
            r->end = next ? next : c->ndw;
            if (r->end > c->ndw || r->end < r->pos) r->end = c->ndw;
            return true;
        }
        if (!next || next <= i || next >= c->ndw) break;
        i = next;
    }
    return false;
}

uint32_t ck_remaining(const CkReader *r)
{
    return r->pos < r->end ? r->end - r->pos : 0;
}

uint32_t ck_read_dword(CkReader *r)
{
    if (r->pos >= r->c->ndw) {
        r->error = true;
        return 0;
    }
    return r->c->dw[r->pos++];
}

int32_t ck_read_int(CkReader *r) { return (int32_t)ck_read_dword(r); }

float ck_read_float(CkReader *r)
{
    uint32_t u = ck_read_dword(r);
    float v;
    memcpy(&v, &u, 4);
    return v;
}

CkGuid ck_read_guid(CkReader *r)
{
    CkGuid g;
    g.a = ck_read_dword(r);
    g.b = ck_read_dword(r);
    return g;
}

uint32_t ck_read_object(CkReader *r) { return ck_read_dword(r); }

const void *ck_read_buffer(CkReader *r, uint32_t *size)
{
    uint32_t n = ck_read_dword(r);
    uint32_t words = (n + 3) / 4;
    if (r->error || words > r->c->ndw - r->pos) {
        r->error = true;
        *size = 0;
        return NULL;
    }
    const void *p = r->c->dw + r->pos;
    r->pos += words;
    *size = n;
    return p;
}

const char *ck_read_string(CkReader *r)
{
    uint32_t n;
    const char *s = ck_read_buffer(r, &n);
    if (!s || !n) return "";
    return memchr(s, 0, n) ? s : "";   /* the original always stores the terminator */
}

bool ck_read_subchunk(CkReader *r, CkChunk *out)
{
    /* CKStateChunk::ReadSubChunk (CK2.dll 0x24023208), parent chunk version >= 4:
       size, class id, version (data | chunk << 16), dword count, file flag, id count, sub-chunk count,
       [manager count if parent chunk version > 4], dwords, ids, sub-chunks, managers. */
    const CkChunk *c = r->c;
    int32_t size = ck_read_int(r);
    if (r->error || size < 1 || (uint32_t)size > c->ndw - r->pos) return false;
    uint32_t start = r->pos;
    memset(out, 0, sizeof *out);
    out->class_id = (uint8_t)ck_read_dword(r);
    if (c->chunk_version < 4) return false;          /* older layouts don't occur in Ballance */
    uint32_t v = ck_read_dword(r);
    out->data_version = (uint8_t)v;
    out->chunk_version = (uint8_t)(v >> 16);
    out->ndw = ck_read_dword(r);
    uint32_t file = ck_read_dword(r);
    out->nids = ck_read_dword(r);
    out->nsub = ck_read_dword(r);
    out->nman = c->chunk_version > 4 ? ck_read_dword(r) : 0;
    out->options = (file == 1 ? 8 : 0) | (out->nids ? 1 : 0) | (out->nsub ? 4 : 0) | (out->nman ? 2 : 0);
    uint64_t need = (uint64_t)out->ndw + out->nids + out->nsub + out->nman;
    if (r->error || need > c->ndw - r->pos) return false;
    out->dw = c->dw + r->pos;
    r->pos += out->ndw;
    out->ids = c->dw + r->pos;
    r->pos += out->nids;
    out->sub = c->dw + r->pos;
    r->pos += out->nsub;
    out->man = c->dw + r->pos;
    r->pos += out->nman;
    r->pos = start + (uint32_t)size;
    return true;
}

void ck_skip(CkReader *r, uint32_t n)
{
    if (n > r->c->ndw - r->pos) {
        r->error = true;
        r->pos = r->c->ndw;
    } else {
        r->pos += n;
    }
}
