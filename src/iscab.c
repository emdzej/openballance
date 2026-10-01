/* InstallShield cabinet reader and its file-layer backend (see iscab.h).

   Layouts (little endian; "cab descriptor" offsets are relative to the descriptor's start, as are all
   string and table offsets in the header). Sources: the unshield project's documentation of the format
   (see iscab.h), every field checked against Ballance's Setup/data1.hdr (version 0x0100600c = major 6).

   Common header (data1.hdr and every dataN.cab), 20 bytes:
     0 "ISc(" | 4 version | 8 volume info | 12 cab descriptor offset | 16 cab descriptor size
     major version: version >> 24 == 1 ? (version >> 12) & 15 : (version & 0xffff) / 100 (when nonzero)

   Cab descriptor (in data1.hdr):
     0x0c file table offset | 0x18 file table size | 0x1c directory count | 0x28 file count
     0x2c file table offset 2 (major >= 6: where the fixed-size file descriptors start, from the file
     table) | 0x3e 71 file group list offsets | 0x15a 71 component list offsets
   The file table starts with directory_count u32 name offsets (relative to the file table); major <= 5
   continues with one u32 descriptor offset per file (also relative to the file table).
   Each list offset heads a linked list of {u32 name, u32 descriptor, u32 next} (next 0 = end).

   File descriptor, major >= 6 (0x57 bytes, at file table + offset 2 + index * 0x57):
     0x00 u16 flags | 0x02 u64 expanded size | 0x0a u64 compressed size | 0x12 u64 data offset
     0x1a md5[16] | 0x2a 16 unknown | 0x3a u32 name (from the file table) | 0x3e u16 directory
     0x40 12 unknown | 0x4c u32 link previous | 0x50 u32 link next | 0x54 u8 link flags | 0x55 u16 volume
   File descriptor, major 5 (at file table + the file's table entry):
     0x00 u32 name | 0x04 u32 directory | 0x08 u16 flags | 0x0a u32 expanded size | 0x0e u32 compressed
     size | 0x12 20 unknown | 0x26 u32 data offset | 0x2a md5[16]; the volume is the header's own.
   Linked files (link flags 1 = previous, 2 = next) are duplicates of one stored file; in Ballance each
   copy carries the shared data offset and sizes itself, so a file with zero sizes and a previous link
   takes them from the previous one.

   File group descriptor: 0x00 u32 name | major <= 5: 0x4c, else 0x16: u32 first file, u32 last file.
   Component descriptor: 0x00 u32 name | major <= 5: 0x70, else 0x6f: u16 group count, u32 offset of
   that many u32 group-name offsets.

   Volume header (dataN.cab, after the common header):
     0x14 u32 data offset | 0x18 unknown | 0x1c u32 first file index | 0x20 u32 last file index, then
     first file offset, expanded size, compressed size, last file offset, expanded size, compressed
     size: u64 each for major >= 6, u32 for major 5.
   A file split across volumes starts as its volume's "last file" (at last file offset, last file
   compressed size bytes there) and continues as the "first file" of the next volumes. Ballance has no
   split files (all those fields are 0), so that path is untested.

   Stored data: obfuscated files are XORed with a running key (byte = ror8(byte ^ 0xd5, 2) - (i % 0x47),
   i counting from 0 over the stored bytes); compressed files are a series of chunks, each a u16 length
   then that many bytes of raw DEFLATE ending with a sync flush (every chunk has its own window). The
   MD5 of the expanded data is in the descriptor. */
#include "iscab.h"
#include "inflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

enum { MAX_LISTS = 71, MAX_VOLUMES = 64 };

typedef struct {
    bool tried, ok;
    void *h;
    uint64_t size;
    uint32_t first_index, last_index;
    uint64_t first[3], last[3];   /* the volume's first / last file: offset, expanded size, compressed size */
} Volume;

typedef struct { const char *name; uint32_t first, last; } Group;
typedef struct { const char *name; int ngroups; const char **groups; } Component;

struct IsCab {
    VfsBackend io;
    char dir[512], prefix[64];   /* volumes: <dir><prefix><n>.cab */
    int major;
    uint8_t *hdr;
    size_t hdr_size;
    uint32_t desc;               /* cab descriptor offset in hdr */
    uint32_t nfiles, ndirs;
    IsCabFile *files;
    const char **dirs;
    Group *groups;
    int ngroups;
    Component *comps;
    int ncomps;
    Volume vol[MAX_VOLUMES];
};

/* ---- header access (bounds checked: a bad offset reads as 0 / "") ---- */

static const uint8_t *at(const IsCab *c, uint64_t off, size_t len)
{
    uint64_t o = (uint64_t)c->desc + off;
    return o + len <= c->hdr_size ? c->hdr + o : NULL;
}
static uint32_t u32at(const IsCab *c, uint64_t off) { const uint8_t *p = at(c, off, 4); return p ? rd32(p) : 0; }
static uint16_t u16at(const IsCab *c, uint64_t off) { const uint8_t *p = at(c, off, 2); return p ? rd16(p) : 0; }

/* NUL-terminated string at a descriptor offset (the header buffer has a NUL appended). */
static const char *str(const IsCab *c, uint64_t off)
{
    const uint8_t *p = at(c, off, 1);
    return p ? (const char *)p : "";
}

static void *read_whole(const VfsBackend *io, const char *path, size_t *size)
{
    uint64_t n = 0;
    void *h = io->open(io->ctx, path, &n);
    if (!h) return NULL;
    uint8_t *b = n < (64u << 20) ? malloc((size_t)n + 1) : NULL;
    bool ok = b && io->read_at(io->ctx, h, 0, b, (size_t)n) == (int64_t)n;
    io->close(io->ctx, h);
    if (!ok) { free(b); return NULL; }
    b[n] = 0;
    *size = (size_t)n;
    return b;
}

static int major_version(uint32_t v)
{
    if (v >> 24 == 1) return (int)((v >> 12) & 15);
    int m = (int)(v & 0xffff);
    return m ? m / 100 : 0;
}

static bool fail(char *err, size_t cap, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg);
    return false;
}

/* Walks the 71 linked lists at list (descriptor offset of the head offsets): calls fn with each
   entry's descriptor offset; returns the count. */
static int walk_lists(IsCab *c, uint32_t list, void (*fn)(IsCab *c, int n, uint32_t desc))
{
    int n = 0;
    for (int k = 0; k < MAX_LISTS; k++)
        for (uint32_t x = u32at(c, list + 4u * k), guard = 0; x && guard < 4096; guard++) {
            if (fn) fn(c, n, u32at(c, x + 4));
            x = u32at(c, x + 8);
            n++;
        }
    return n;
}

static void add_group(IsCab *c, int n, uint32_t d)
{
    uint32_t o = c->major >= 6 ? 0x16 : 0x4c;
    c->groups[n] = (Group){ str(c, u32at(c, d)), u32at(c, d + o), u32at(c, d + o + 4) };
}

static void add_component(IsCab *c, int n, uint32_t d)
{
    uint32_t o = c->major >= 6 ? 0x6f : 0x70;
    Component *m = &c->comps[n];
    m->name = str(c, u32at(c, d));
    m->ngroups = u16at(c, d + o);
    uint32_t tab = u32at(c, d + o + 2);
    m->groups = calloc((size_t)m->ngroups + 1, sizeof *m->groups);
    if (!m->groups) m->ngroups = 0;
    for (int i = 0; i < m->ngroups; i++) m->groups[i] = str(c, u32at(c, tab + 4ull * i));
}

static bool parse(IsCab *c, char *err, size_t errcap)
{
    if (c->hdr_size < 20 || memcmp(c->hdr, "ISc(", 4)) return fail(err, errcap, "not an InstallShield header");
    c->major = major_version(rd32(c->hdr + 4));
    if (c->major < 5) return fail(err, errcap, "InstallShield cabinet older than version 5");
    c->desc = rd32(c->hdr + 12);
    if (c->desc >= c->hdr_size) return fail(err, errcap, "bad cab descriptor offset");
    uint32_t ft = u32at(c, 0x0c), ft2 = u32at(c, 0x2c);
    c->ndirs = u32at(c, 0x1c);
    c->nfiles = u32at(c, 0x28);
    if (c->ndirs > 65536 || c->nfiles > 1u << 20 || !at(c, ft, 4ull * c->ndirs))
        return fail(err, errcap, "bad file table");
    c->dirs = calloc(c->ndirs + 1, sizeof *c->dirs);
    c->files = calloc(c->nfiles + 1, sizeof *c->files);
    if (!c->dirs || !c->files) return fail(err, errcap, "out of memory");
    for (uint32_t i = 0; i < c->ndirs; i++) c->dirs[i] = str(c, (uint64_t)ft + u32at(c, (uint64_t)ft + 4 * i));
    for (uint32_t i = 0; i < c->nfiles; i++) {
        IsCabFile *f = &c->files[i];
        uint32_t dir;
        const uint8_t *p;
        if (c->major >= 6) {
            if (!(p = at(c, (uint64_t)ft + ft2 + (uint64_t)i * 0x57, 0x57))) return fail(err, errcap, "bad file descriptor");
            f->flags = rd16(p);
            f->size = rd64(p + 0x02);
            f->stored = rd64(p + 0x0a);
            f->offset = rd64(p + 0x12);
            memcpy(f->md5, p + 0x1a, 16);
            f->name = str(c, (uint64_t)ft + rd32(p + 0x3a));
            dir = rd16(p + 0x3e);
            f->volume = rd16(p + 0x55);
            uint32_t prev = rd32(p + 0x4c);
            if (!f->size && !f->stored && (p[0x54] & 1) && prev < i) {   /* a link without its own data */
                const IsCabFile *q = &c->files[prev];
                f->size = q->size, f->stored = q->stored, f->offset = q->offset, f->volume = q->volume;
                f->flags = q->flags;
                memcpy(f->md5, q->md5, 16);
            }
        } else {
            uint32_t e = u32at(c, (uint64_t)ft + 4ull * (c->ndirs + i));
            if (!(p = at(c, (uint64_t)ft + e, 0x3a))) return fail(err, errcap, "bad file descriptor");
            f->name = str(c, (uint64_t)ft + rd32(p));
            dir = rd32(p + 4);
            f->flags = rd16(p + 8);
            f->size = rd32(p + 0x0a);
            f->stored = rd32(p + 0x0e);
            f->offset = rd32(p + 0x26);
            memcpy(f->md5, p + 0x2a, 16);
            f->volume = 1;
        }
        if (!(f->flags & ISCAB_COMPRESSED)) f->stored = f->size;
        f->dir = dir < c->ndirs ? c->dirs[dir] : "";
    }
    c->ngroups = walk_lists(c, 0x3e, NULL);
    c->ncomps = walk_lists(c, 0x15a, NULL);
    c->groups = calloc((size_t)c->ngroups + 1, sizeof *c->groups);
    c->comps = calloc((size_t)c->ncomps + 1, sizeof *c->comps);
    if (!c->groups || !c->comps) return fail(err, errcap, "out of memory");
    walk_lists(c, 0x3e, add_group);
    walk_lists(c, 0x15a, add_component);
    return true;
}

IsCab *iscab_open(const VfsBackend *io, const char *hdr, char *err, size_t errcap)
{
    IsCab *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->io = *io;
    /* "<dir>/<prefix>1.hdr" -> volumes "<dir>/<prefix><n>.cab" */
    const char *slash = strrchr(hdr, '/'), *bs = strrchr(hdr, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
    const char *base = slash ? slash + 1 : hdr;
    size_t bl = strlen(base);
    if (bl < 5 || strcasecmp(base + bl - 5, "1.hdr") || bl - 5 >= sizeof c->prefix ||
        (size_t)(base - hdr) >= sizeof c->dir) {
        fail(err, errcap, "header name is not <name>1.hdr");
        free(c);
        return NULL;
    }
    snprintf(c->dir, sizeof c->dir, "%.*s", (int)(base - hdr), hdr);
    snprintf(c->prefix, sizeof c->prefix, "%.*s", (int)(bl - 5), base);
    c->hdr = read_whole(io, hdr, &c->hdr_size);
    if (!c->hdr) {
        fail(err, errcap, "can't read the header");
        free(c);
        return NULL;
    }
    if (!parse(c, err, errcap)) {
        iscab_close(c);
        return NULL;
    }
    return c;
}

void iscab_close(IsCab *c)
{
    if (!c) return;
    for (int v = 0; v < MAX_VOLUMES; v++)
        if (c->vol[v].h) c->io.close(c->io.ctx, c->vol[v].h);
    for (int k = 0; c->comps && k < c->ncomps; k++) free(c->comps[k].groups);
    free(c->comps);
    free(c->groups);
    free(c->files);
    free(c->dirs);
    free(c->hdr);
    free(c);
}

int iscab_version(const IsCab *c) { return c->major; }
uint32_t iscab_file_count(const IsCab *c) { return c->nfiles; }
const IsCabFile *iscab_file(const IsCab *c, uint32_t i) { return i < c->nfiles ? &c->files[i] : NULL; }
int iscab_group_count(const IsCab *c) { return c->ngroups; }
const char *iscab_group(const IsCab *c, int g, uint32_t *first, uint32_t *last)
{
    if (g < 0 || g >= c->ngroups) return NULL;
    if (first) *first = c->groups[g].first;
    if (last) *last = c->groups[g].last;
    return c->groups[g].name;
}
int iscab_component_count(const IsCab *c) { return c->ncomps; }
const char *iscab_component(const IsCab *c, int k, int *ngroups)
{
    if (k < 0 || k >= c->ncomps) return NULL;
    if (ngroups) *ngroups = c->comps[k].ngroups;
    return c->comps[k].name;
}
const char *iscab_component_group(const IsCab *c, int k, int i)
{
    if (k < 0 || k >= c->ncomps || i < 0 || i >= c->comps[k].ngroups) return NULL;
    return c->comps[k].groups[i];
}

/* ---- volumes ---- */

static Volume *volume(IsCab *c, unsigned n)
{
    if (n >= MAX_VOLUMES) return NULL;
    Volume *v = &c->vol[n];
    if (v->tried) return v->ok ? v : NULL;
    v->tried = true;
    char path[640];
    snprintf(path, sizeof path, "%s%s%u.cab", c->dir, c->prefix, n);
    v->h = c->io.open(c->io.ctx, path, &v->size);
    if (!v->h) return NULL;
    uint8_t h[0x54] = { 0 };
    if (c->io.read_at(c->io.ctx, v->h, 0, h, sizeof h) < 0x3c || memcmp(h, "ISc(", 4)) return NULL;
    v->first_index = rd32(h + 0x1c);
    v->last_index = rd32(h + 0x20);
    for (int k = 0; k < 6; k++) {   /* first offset, expanded, compressed; last offset, expanded, compressed */
        uint64_t x = c->major >= 6 ? rd64(h + 0x24 + 8 * k) : rd32(h + 0x24 + 4 * k);
        if (k < 3) v->first[k] = x;
        else v->last[k - 3] = x;
    }
    v->ok = true;
    return v;
}

static bool vol_read(IsCab *c, Volume *v, uint64_t off, uint8_t *dst, uint64_t n)
{
    if (off > v->size || n > v->size - off) return false;
    while (n) {
        size_t k = n > (1u << 30) ? 1u << 30 : (size_t)n;
        if (c->io.read_at(c->io.ctx, v->h, off, dst, k) != (int64_t)k) return false;
        off += k, dst += k, n -= k;
    }
    return true;
}

/* The stored bytes of file i (f->stored of them) into dst, across volumes. */
static bool gather(IsCab *c, uint32_t i, uint8_t *dst)
{
    const IsCabFile *f = &c->files[i];
    int sz = f->flags & ISCAB_COMPRESSED ? 2 : 1;           /* which size field counts stored bytes */
    unsigned n = f->volume;
    Volume *v = volume(c, n);
    if (!v) return false;
    uint64_t left = f->stored, off = f->offset, k = left;
    if (i == v->last_index && v->last[sz] && v->last[sz] < left) {   /* starts split at the volume's end */
        off = v->last[0];
        k = v->last[sz];
    }
    for (;;) {
        if (k > left) k = left;
        if (!vol_read(c, v, off, dst, k)) return false;
        dst += k, left -= k;
        if (!left) return true;
        if (!(v = volume(c, ++n)) || v->first_index != i || !v->first[sz]) return false;
        off = v->first[0];
        k = v->first[sz];
    }
}

static void deobfuscate(uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        uint8_t x = p[i] ^ 0xd5;
        p[i] = (uint8_t)(((x >> 2) | (x << 6)) - (uint8_t)(i % 0x47));
    }
}

/* ---- MD5 (RFC 1321) ---- */

static void md5_block(uint32_t h[4], const uint8_t *b)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    uint32_t m[16], a = h[0], bb = h[1], cc = h[2], d = h[3];
    for (int i = 0; i < 16; i++) m[i] = rd32(b + 4 * i);
#define MD5_STEP(F, g, r)                                         \
    do {                                                          \
        uint32_t t = a + (F) + K[i] + m[g];                       \
        a = d, d = cc, cc = bb;                                   \
        bb += (t << (r)) | (t >> (32 - (r)));                     \
    } while (0)
    static const uint8_t R[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    int i = 0;
    for (; i < 16; i++) MD5_STEP((bb & cc) | (~bb & d), i, R[0][i & 3]);
    for (; i < 32; i++) MD5_STEP((d & bb) | (~d & cc), (5 * i + 1) & 15, R[1][i & 3]);
    for (; i < 48; i++) MD5_STEP(bb ^ cc ^ d, (3 * i + 5) & 15, R[2][i & 3]);
    for (; i < 64; i++) MD5_STEP(cc ^ (bb | ~d), (7 * i) & 15, R[3][i & 3]);
#undef MD5_STEP
    h[0] += a, h[1] += bb, h[2] += cc, h[3] += d;
}

static void md5(const uint8_t *p, uint64_t n, uint8_t out[16])
{
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t i = 0;
    for (; i + 64 <= n; i += 64) md5_block(h, p + i);
    uint8_t t[128] = {0};
    size_t r = (size_t)(n - i);
    memcpy(t, p + i, r);
    t[r] = 0x80;
    size_t tl = r < 56 ? 64 : 128;
    uint64_t bits = n * 8;
    for (int k = 0; k < 8; k++) t[tl - 8 + k] = (uint8_t)(bits >> (8 * k));
    md5_block(h, t);
    if (tl == 128) md5_block(h, t + 64);
    for (int k = 0; k < 16; k++) out[k] = (uint8_t)(h[k / 4] >> (8 * (k % 4)));
}

bool iscab_extract(IsCab *c, uint32_t i, uint8_t *dst)
{
    if (i >= c->nfiles) return false;
    const IsCabFile *f = &c->files[i];
    if (f->flags & ISCAB_INVALID) return false;
    bool packed = f->flags & ISCAB_COMPRESSED;
    uint8_t *raw = packed ? malloc(f->stored ? (size_t)f->stored : 1) : dst;
    if (!raw || !gather(c, i, raw)) { if (raw != dst) free(raw); return false; }
    if (f->flags & ISCAB_OBFUSCATED) deobfuscate(raw, f->stored);
    bool ok = true;
    if (packed) {
        uint64_t p = 0, out = 0;
        while (ok && p < f->stored) {
            if (f->stored - p < 2) { ok = false; break; }
            uint32_t len = rd16(raw + p);
            p += 2;
            if (len > f->stored - p) { ok = false; break; }
            long n = deflate_inflate_flushed(raw + p, len, dst + out, (size_t)(f->size - out));
            if (n < 0) ok = false;
            else out += (uint64_t)n;
            p += len;
        }
        ok = ok && out == f->size;
        free(raw);
    }
    if (ok) {
        static const uint8_t zero[16];
        uint8_t h[16];
        if (memcmp(f->md5, zero, 16)) {           /* (all zero: no checksum recorded) */
            md5(dst, f->size, h);
            ok = !memcmp(h, f->md5, 16);
        }
    }
    return ok;
}

/* ------------------------------------------------------------------ file-layer backend */

/* The install tree: nodes linked to their first child and next sibling (node 0 = the root). */
typedef struct { const char *name; size_t len; int parent, child, next; int64_t file; } Node;
/* Expanded files, per file index: kept while open and, up to CACHE_BYTES, after (least recently used
   go first). The game reads whole files (vfs_read_all); measured on level 1 (menu, load, 2500 frames):
   110 opens, 98 reads of 81 distinct files (36.5 MB expanded); the 17 repeated reads are small files
   (~0.2 MB together), so a small cache catches all of them. */
typedef struct { uint8_t *data; int refs; uint32_t stamp; } Slot;
typedef struct {
    IsCab *cab;
    VfsBackend under;            /* the mount the cabinets are read from (owned) */
    char under_desc[512];
    Node *node;
    int nnodes, cap;
    Slot *slot;
    uint64_t idle_bytes;         /* expanded data of closed files still cached */
    uint32_t clock;
    IsCabStats stats;
} CabFs;
typedef struct { uint32_t file; bool held; } CabHandle;

enum { CACHE_BYTES = 8 << 20 };
static CabFs *active;

static int node_child(const CabFs *fs, int dir, const char *name, size_t len)
{
    for (int i = fs->node[dir].child; i >= 0; i = fs->node[i].next)
        if (fs->node[i].len == len && !strncasecmp(fs->node[i].name, name, len)) return i;
    return -1;
}

static int node_add(CabFs *fs, int dir, const char *name, size_t len, int64_t file)
{
    if (fs->nnodes == fs->cap) {
        int c = fs->cap ? fs->cap * 2 : 256;
        Node *n = realloc(fs->node, (size_t)c * sizeof *n);
        if (!n) return -1;
        fs->node = n;
        fs->cap = c;
    }
    int i = fs->nnodes++;
    fs->node[i] = (Node){ name, len, dir, -1, -1, file };
    if (dir >= 0) {                            /* append: listing keeps the cabinet's order */
        int *p = &fs->node[dir].child;
        while (*p >= 0) p = &fs->node[*p].next;
        *p = i;
    }
    return i;
}

/* Adds file i at <dir>\<name>; the first file at a path wins (the installer would ask before
   overwriting; no game group repeats a path, see vfs_mount_cab). */
static bool tree_add(CabFs *fs, uint32_t i)
{
    const IsCabFile *f = iscab_file(fs->cab, i);
    int cur = 0;
    for (const char *p = f->dir; *p;) {
        while (*p == '\\' || *p == '/') p++;
        const char *q = p;
        while (*q && *q != '\\' && *q != '/') q++;
        if (q > p) {
            int k = node_child(fs, cur, p, (size_t)(q - p));
            if (k >= 0 && fs->node[k].file >= 0) return true;   /* a file where a directory should be */
            if (k < 0 && (k = node_add(fs, cur, p, (size_t)(q - p), -1)) < 0) return false;
            cur = k;
        }
        p = q;
    }
    size_t len = strlen(f->name);
    if (!len || node_child(fs, cur, f->name, len) >= 0) return true;
    return node_add(fs, cur, f->name, len, i) >= 0;
}

static int lookup(const CabFs *fs, const char *rel)
{
    int cur = 0;
    for (const char *p = rel; *p;) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != '/' && *q != '\\') q++;
        if (!(q - p == 1 && *p == '.')) {
            if (fs->node[cur].file >= 0) return -1;
            if ((cur = node_child(fs, cur, p, (size_t)(q - p))) < 0) return -1;
        }
        p = q;
    }
    return cur;
}

static void *cab_open(void *ctx, const char *rel, uint64_t *size)
{
    CabFs *fs = ctx;
    int n = lookup(fs, rel);
    if (n < 0 || fs->node[n].file < 0) return NULL;
    CabHandle *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->file = (uint32_t)fs->node[n].file;
    *size = iscab_file(fs->cab, h->file)->size;
    fs->stats.opens++;
    return h;                                  /* expanded on the first read: vfs_exists costs nothing */
}

static void evict(CabFs *fs)
{
    while (fs->idle_bytes > CACHE_BYTES) {
        int64_t best = -1;
        for (uint32_t i = 0; i < iscab_file_count(fs->cab); i++)
            if (fs->slot[i].data && !fs->slot[i].refs && (best < 0 || fs->slot[i].stamp < fs->slot[best].stamp)) best = i;
        if (best < 0) return;
        free(fs->slot[best].data);
        fs->slot[best].data = NULL;
        fs->idle_bytes -= iscab_file(fs->cab, (uint32_t)best)->size;
    }
}

static int64_t cab_read_at(void *ctx, void *file, uint64_t off, void *dst, size_t len)
{
    CabFs *fs = ctx;
    CabHandle *h = file;
    Slot *s = &fs->slot[h->file];
    uint64_t size = iscab_file(fs->cab, h->file)->size;
    if (!h->held) {
        if (s->data) {
            fs->stats.cache_hits++;
            if (!s->refs) fs->idle_bytes -= size;
        } else {
            uint8_t *d = malloc(size ? (size_t)size : 1);
            if (!d || !iscab_extract(fs->cab, h->file, d)) { free(d); return -1; }
            s->data = d;
            fs->stats.extracts++;
            fs->stats.bytes_out += size;
        }
        s->refs++;
        h->held = true;
    }
    s->stamp = ++fs->clock;
    if (off >= size) return 0;
    if (len > size - off) len = (size_t)(size - off);
    memcpy(dst, s->data + off, len);
    return (int64_t)len;
}

static void cab_close(void *ctx, void *file)
{
    CabFs *fs = ctx;
    CabHandle *h = file;
    if (h->held && !--fs->slot[h->file].refs) {
        fs->idle_bytes += iscab_file(fs->cab, h->file)->size;
        evict(fs);
    }
    free(h);
}

static bool cab_list(void *ctx, const char *dir, VfsListFn fn, void *user)
{
    CabFs *fs = ctx;
    int d = lookup(fs, dir);
    if (d < 0 || fs->node[d].file >= 0) return false;
    for (int i = fs->node[d].child; i >= 0; i = fs->node[i].next) {
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)fs->node[i].len, fs->node[i].name);
        fn(name, fs->node[i].file < 0, user);
    }
    return true;
}

/* Frees the layer; the mount underneath is unmounted too unless keep_under. */
static void cab_free(CabFs *fs, bool keep_under)
{
    if (active == fs) active = NULL;
    for (uint32_t i = 0; fs->slot && i < iscab_file_count(fs->cab); i++) free(fs->slot[i].data);
    free(fs->slot);
    free(fs->node);
    iscab_close(fs->cab);
    if (!keep_under && fs->under.unmount) fs->under.unmount(fs->under.ctx);
    free(fs);
}

static void cab_unmount(void *ctx) { cab_free(ctx, false); }

bool vfs_mount_cab(const char *hdr)
{
    VfsBackend under;
    char desc[512];
    if (!vfs_detach(&under, desc, sizeof desc)) return false;
    CabFs *fs = calloc(1, sizeof *fs);
    char err[128] = "out of memory";
    if (fs) fs->under = under, memcpy(fs->under_desc, desc, sizeof desc);
    if (!fs || !(fs->cab = iscab_open(&fs->under, hdr, err, sizeof err))) {
        free(fs);
        vfs_mount(&under, desc);
        return false;
    }
    /* InstallShield's own groups are named "<Support>...", "<Engine>...", "<Disk1>..."; the setup
       author's groups (Ballance: "Programmdateien der Anwendung", installed to the game folder) are
       the rest, and they hold the game. */
    bool ok = (fs->slot = calloc(iscab_file_count(fs->cab) + 1, sizeof *fs->slot)) && node_add(fs, -1, "", 0, -1) == 0;
    uint32_t files = 0;
    for (int g = 0; ok && g < iscab_group_count(fs->cab); g++) {
        uint32_t first, last;
        const char *name = iscab_group(fs->cab, g, &first, &last);
        if (name[0] == '<') continue;
        for (uint32_t i = first; ok && i <= last && i < iscab_file_count(fs->cab); i++)
            if (!(iscab_file(fs->cab, i)->flags & ISCAB_INVALID)) ok = tree_add(fs, i), files++;
    }
    if (!ok || !files) {
        cab_free(fs, true);
        vfs_mount(&under, desc);
        return false;
    }
    char d[768];
    snprintf(d, sizeof d, "%s: %s (InstallShield %d cabinets, %u files)", desc, hdr, iscab_version(fs->cab), files);
    VfsBackend b = { fs, cab_open, cab_read_at, cab_close, cab_list, cab_unmount };
    vfs_mount(&b, d);
    active = fs;
    return true;
}

bool vfs_cab_stats(IsCabStats *s)
{
    if (!active) return false;
    *s = active->stats;
    return true;
}

bool vfs_find_game(void)
{
    if (!vfs_mounted()) return false;
    if (vfs_exists("base.cmo")) return true;
    static const char *const hdrs[] = { "Setup/data1.hdr", "data1.hdr" };
    for (size_t k = 0; k < sizeof hdrs / sizeof *hdrs; k++) {
        if (!vfs_exists(hdrs[k]) || !vfs_mount_cab(hdrs[k])) continue;
        if (vfs_exists("base.cmo")) return true;
        /* cabinets of something else: back to the mount underneath */
        VfsBackend b;
        vfs_detach(&b, NULL, 0);
        CabFs *fs = b.ctx;
        VfsBackend under = fs->under;
        char desc[512];
        memcpy(desc, fs->under_desc, sizeof desc);
        cab_free(fs, true);
        vfs_mount(&under, desc);
    }
    return false;
}
