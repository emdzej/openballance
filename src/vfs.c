/* File layer core (see vfs.h): mount dispatch and the ISO 9660 disc-image reader. No host file APIs
   here; vfs_host.c adds directories and image files. */
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct VfsFile { void *h; uint64_t size; };

static VfsBackend mnt;
static bool mounted;
static char mnt_desc[512];

void vfs_unmount(void)
{
    if (mounted && mnt.unmount) mnt.unmount(mnt.ctx);
    memset(&mnt, 0, sizeof mnt);
    mounted = false;
    mnt_desc[0] = 0;
}

void vfs_mount(const VfsBackend *b, const char *desc)
{
    vfs_unmount();
    mnt = *b;
    mounted = true;
    snprintf(mnt_desc, sizeof mnt_desc, "%s", desc ? desc : "");
}

bool vfs_detach(VfsBackend *b, char *desc, size_t cap)
{
    if (!mounted) return false;
    *b = mnt;
    if (desc && cap) snprintf(desc, cap, "%s", mnt_desc);
    memset(&mnt, 0, sizeof mnt);
    mounted = false;
    mnt_desc[0] = 0;
    return true;
}

bool vfs_mounted(void) { return mounted; }
const char *vfs_describe(void) { return mnt_desc; }

/* "." and ".." components resolved; ".." at the root stays there (the game's paths are relative to its Bin
   directory, e.g. "..\\Text\\Tutorial2.txt", and the mount root stands in for both) */
static void normalize(const char *rel, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = rel; *p;) {
        while (*p == '/' || *p == '\\') p++;
        const char *q = p;
        while (*q && *q != '/' && *q != '\\') q++;
        size_t len = (size_t)(q - p);
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '/') n--;
            if (n > 0) n--;
        } else if (len && !(len == 1 && *p == '.') && n + len + 2 < cap) {
            if (n) out[n++] = '/';
            memcpy(out + n, p, len);
            n += len;
        }
        p = q;
    }
    out[n] = 0;
}

VfsFile *vfs_open(const char *rel)
{
    if (!mounted || !rel) return NULL;
    char norm[1024];
    normalize(rel, norm, sizeof norm);
    rel = norm;
    uint64_t size = 0;
    void *h = mnt.open(mnt.ctx, rel, &size);
    if (!h) return NULL;
    VfsFile *f = malloc(sizeof *f);
    if (!f) { mnt.close(mnt.ctx, h); return NULL; }
    f->h = h;
    f->size = size;
    return f;
}

uint64_t vfs_file_size(const VfsFile *f) { return f ? f->size : 0; }

int64_t vfs_read_at(VfsFile *f, uint64_t off, void *dst, size_t len)
{
    if (!f) return -1;
    if (off >= f->size || !len) return 0;
    if (len > f->size - off) len = (size_t)(f->size - off);
    return mnt.read_at(mnt.ctx, f->h, off, dst, len);
}

void vfs_close(VfsFile *f)
{
    if (!f) return;
    if (mounted) mnt.close(mnt.ctx, f->h);
    free(f);
}

int64_t vfs_size(const char *rel)
{
    VfsFile *f = vfs_open(rel);
    if (!f) return -1;
    int64_t n = (int64_t)f->size;
    vfs_close(f);
    return n;
}

bool vfs_exists(const char *rel) { return vfs_size(rel) >= 0; }

uint8_t *vfs_read_all(const char *rel, size_t *size)
{
    VfsFile *f = vfs_open(rel);
    if (!f) return NULL;
    size_t n = (size_t)f->size;
    uint8_t *buf = malloc(n ? n : 1);
    bool ok = buf != NULL;
    for (size_t got = 0; ok && got < n; ) {
        int64_t r = vfs_read_at(f, got, buf + got, n - got);
        if (r <= 0) ok = false;
        else got += (size_t)r;
    }
    vfs_close(f);
    if (!ok) { free(buf); return NULL; }
    if (size) *size = n;
    return buf;
}

bool vfs_list(const char *dir, VfsListFn fn, void *user)
{
    return mounted && mnt.list && mnt.list(mnt.ctx, dir ? dir : "", fn, user);
}

/* ------------------------------------------------------------------ ISO 9660 */

/* The directory tree is read once at mount time (an installed game or a disc has a few hundred entries). */
typedef struct { char name[40]; int parent; bool dir; uint32_t lba, size; } IsoEntry;
typedef struct {
    VfsSource src;
    int sector;          /* bytes per sector in the image: 2048 or 2352 */
    int hdr;             /* bytes before the 2048 user-data bytes of a raw sector: 0, 16 (MODE1), 24 (MODE2 form 1) */
    IsoEntry *e;
    int n, cap;
} Iso;
typedef struct { uint32_t lba, size; } IsoFile;

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* User data of count sectors from lba (count * 2048 bytes). */
static bool iso_sectors(const Iso *iso, uint32_t lba, uint32_t count, uint8_t *dst)
{
    if (iso->sector == 2048) {
        size_t len = (size_t)count * 2048;
        return iso->src.read_at(iso->src.ctx, (uint64_t)lba * 2048, dst, len) == (int64_t)len;
    }
    enum { BATCH = 32 };
    uint8_t *raw = malloc((size_t)BATCH * 2352);
    if (!raw) return false;
    bool ok = true;
    while (ok && count) {
        uint32_t k = count < BATCH ? count : BATCH;
        size_t len = (size_t)k * 2352;
        ok = iso->src.read_at(iso->src.ctx, (uint64_t)lba * 2352, raw, len) == (int64_t)len;
        for (uint32_t i = 0; ok && i < k; i++) memcpy(dst + (size_t)i * 2048, raw + (size_t)i * 2352 + iso->hdr, 2048);
        dst += (size_t)k * 2048;
        lba += k;
        count -= k;
    }
    free(raw);
    return ok;
}

static int64_t iso_read(const Iso *iso, uint32_t lba, uint64_t off, void *dst, size_t len)
{
    if (iso->sector == 2048)
        return iso->src.read_at(iso->src.ctx, (uint64_t)lba * 2048 + off, dst, len);
    uint8_t *out = dst, sec[2048];
    size_t done = 0;
    while (done < len) {
        uint64_t p = off + done;
        uint32_t s = lba + (uint32_t)(p / 2048), in = (uint32_t)(p % 2048);
        size_t left = len - done;
        if (in == 0 && left >= 2048) {                        /* whole sectors straight into dst */
            uint32_t k = (uint32_t)(left / 2048);
            if (!iso_sectors(iso, s, k, out + done)) return done ? (int64_t)done : -1;
            done += (size_t)k * 2048;
            continue;
        }
        if (!iso_sectors(iso, s, 1, sec)) return done ? (int64_t)done : -1;
        size_t n = 2048 - in;
        if (n > left) n = left;
        memcpy(out + done, sec + in, n);
        done += n;
    }
    return (int64_t)done;
}

static bool iso_add(Iso *iso, const IsoEntry *e)
{
    if (iso->n >= 65536) return false;
    if (iso->n == iso->cap) {
        int c = iso->cap ? iso->cap * 2 : 256;
        IsoEntry *ne = realloc(iso->e, (size_t)c * sizeof *ne);
        if (!ne) return false;
        iso->e = ne;
        iso->cap = c;
    }
    iso->e[iso->n++] = *e;
    return true;
}

/* Directory records of entry `dir` -> children (ECMA-119 9.1; records never cross a sector). */
static bool iso_read_dir(Iso *iso, int dir)
{
    uint32_t lba = iso->e[dir].lba, size = iso->e[dir].size;
    if (size > 16u << 20) return false;
    uint32_t nsec = (size + 2047) / 2048;
    uint8_t *d = malloc((size_t)nsec * 2048 + 1);
    if (!d || !iso_sectors(iso, lba, nsec, d)) { free(d); return false; }
    for (uint32_t s = 0; s < nsec; s++)
        for (uint32_t p = 0; p < 2048; ) {
            const uint8_t *r = d + (size_t)s * 2048 + p;
            if (r[0] == 0) break;                             /* rest of the sector is padding */
            if (r[0] < 34 || p + r[0] > 2048) break;
            int nl = r[32];
            p += r[0];
            if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;   /* "." and ".." */
            if (33 + nl > r[0]) continue;                     /* name does not fit the record */
            IsoEntry e = { .parent = dir, .dir = (r[25] & 2) != 0, .lba = rd32le(r + 2), .size = rd32le(r + 10) };
            int k = 0;
            for (int i = 0; i < nl && k < (int)sizeof e.name - 1; i++) {
                if (r[33 + i] == ';') break;                  /* version suffix */
                e.name[k++] = (char)r[33 + i];
            }
            if (k > 0 && e.name[k - 1] == '.') k--;           /* "NAME." = no extension */
            e.name[k] = 0;
            if (k && !iso_add(iso, &e)) { free(d); return false; }
        }
    free(d);
    return true;
}

static int iso_child(const Iso *iso, int dir, const char *name, size_t len)
{
    for (int i = 1; i < iso->n; i++)
        if (iso->e[i].parent == dir && strlen(iso->e[i].name) == len && !strncasecmp(iso->e[i].name, name, len)) return i;
    return -1;
}

static int iso_lookup(const Iso *iso, const char *rel)
{
    int cur = 0;
    const char *p = rel;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != '/' && *q != '\\') q++;
        if (!(q - p == 1 && *p == '.')) {
            if (!iso->e[cur].dir) return -1;
            cur = iso_child(iso, cur, p, (size_t)(q - p));
            if (cur < 0) return -1;
        }
        p = q;
    }
    return cur;
}

static void *iso_open(void *ctx, const char *rel, uint64_t *size)
{
    Iso *iso = ctx;
    int i = iso_lookup(iso, rel);
    if (i < 0 || iso->e[i].dir) return NULL;
    IsoFile *f = malloc(sizeof *f);
    if (!f) return NULL;
    f->lba = iso->e[i].lba;
    f->size = iso->e[i].size;
    *size = f->size;
    return f;
}

static int64_t iso_read_at(void *ctx, void *file, uint64_t off, void *dst, size_t len)
{
    const IsoFile *f = file;
    return iso_read(ctx, f->lba, off, dst, len);
}

static void iso_close(void *ctx, void *file) { free(file); }

static bool iso_list(void *ctx, const char *dir, VfsListFn fn, void *user)
{
    Iso *iso = ctx;
    int d = iso_lookup(iso, dir);
    if (d < 0 || !iso->e[d].dir) return false;
    for (int i = 1; i < iso->n; i++)
        if (iso->e[i].parent == d) fn(iso->e[i].name, iso->e[i].dir, user);
    return true;
}

static void iso_unmount(void *ctx)
{
    Iso *iso = ctx;
    if (iso->src.close) iso->src.close(iso->src.ctx);
    free(iso->e);
    free(iso);
}

/* Primary volume descriptor (sector 16: type 1, "CD001") under a sector layout. */
static bool iso_pvd(Iso *iso, uint8_t *pvd)
{
    return iso_sectors(iso, 16, 1, pvd) && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5);
}

bool vfs_mount_image(const VfsSource *src, int sector_hint, const char *desc)
{
    Iso *iso = calloc(1, sizeof *iso);
    if (!iso) { if (src->close) src->close(src->ctx); return false; }
    iso->src = *src;
    uint8_t pvd[2048];
    static const struct { int sector, hdr; } layouts[] = { { 2048, 0 }, { 2352, 16 }, { 2352, 24 } };
    bool found = false;
    /* A raw sector starts with the sync pattern 00 ff*10 00 and has the mode in byte 15. */
    uint8_t sync[16] = { 0 };
    if (src->read_at(src->ctx, 0, sync, 16) == 16 && sync[0] == 0 && sync[11] == 0) {
        bool is_sync = true;
        for (int i = 1; i < 11; i++) is_sync &= sync[i] == 0xff;
        if (is_sync && sector_hint != 2048) {
            iso->sector = 2352;
            iso->hdr = sync[15] == 2 ? 24 : 16;
            found = iso_pvd(iso, pvd);
        }
    }
    for (size_t i = 0; !found && i < sizeof layouts / sizeof *layouts; i++) {
        if (sector_hint && layouts[i].sector != sector_hint) continue;
        iso->sector = layouts[i].sector;
        iso->hdr = layouts[i].hdr;
        found = iso_pvd(iso, pvd);
    }
    if (!found) { iso_unmount(iso); return false; }
    const uint8_t *root = pvd + 156;                          /* root directory record */
    IsoEntry r = { .name = "", .parent = -1, .dir = true, .lba = rd32le(root + 2), .size = rd32le(root + 10) };
    if (!iso_add(iso, &r)) { iso_unmount(iso); return false; }
    for (int i = 0; i < iso->n; i++)                          /* breadth first: children are appended */
        if (iso->e[i].dir && !iso_read_dir(iso, i)) { iso_unmount(iso); return false; }
    char d[512];
    snprintf(d, sizeof d, "%s (ISO 9660, %d-byte sectors)", desc ? desc : "disc image", iso->sector);
    VfsBackend b = { iso, iso_open, iso_read_at, iso_close, iso_list, iso_unmount };
    vfs_mount(&b, d);
    return true;
}
