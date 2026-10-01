/* Read-only file layer (from OpenRF). Every access to the game data (an installed Ballance) goes through
   here, so the data can come from a directory, a disc image, or a host that has no filesystem at all
   (a gasm runner's named assets).

   Paths are relative to the data root, use '/' (or '\'), and match case-insensitively: the scripts name
   files in any case (Level_01.NMO, Textures/floor_top_flat.bmp).

   One source is mounted at a time (a layer can sit on top of it, see vfs_detach):
   - a host directory (vfs_host.c: vfs_mount_path(dir)),
   - an ISO 9660 disc image (vfs_mount_image) read through a VfsSource, i.e. any "read bytes at offset"
     function: vfs_host.c backs it with a file (.iso / .bin / .cue), a gasm backend with asset_read_at.
     Plain images (2048-byte sectors) and raw MODE1/2352 or MODE2/2352 images (sector headers skipped)
     are both accepted and detected from the data.
   - the InstallShield cabinets of the CD (Setup/data1.hdr, iscab.h: vfs_mount_cab / vfs_find_game) as
     the installed game folder, layered over a directory, an image or gasm assets.

   Thread safety: vfs_read_at on an open file may be called from the audio thread while the main thread
   opens and reads other files (sources and backends read positionally, without a shared cursor). Mounting
   and unmounting must not race with anything. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct VfsFile VfsFile;

VfsFile *vfs_open(const char *rel);                  /* NULL if missing */
uint64_t vfs_file_size(const VfsFile *f);
/* Up to len bytes at off: returns the count (0 at or after the end), -1 on error. */
int64_t vfs_read_at(VfsFile *f, uint64_t off, void *dst, size_t len);
void vfs_close(VfsFile *f);

bool vfs_exists(const char *rel);                    /* a file (not a directory) */
int64_t vfs_size(const char *rel);                   /* -1 if missing */
/* Whole file, malloc'd (free() it); NULL if missing or unreadable. size may be NULL. */
uint8_t *vfs_read_all(const char *rel, size_t *size);
/* Calls fn for every entry of a directory (names as on the medium, ISO version suffix removed; no
   "." / ".."), in medium order. Returns false if the directory does not exist. */
typedef void (*VfsListFn)(const char *name, bool is_dir, void *user);
bool vfs_list(const char *dir, VfsListFn fn, void *user);

/* What is mounted, for messages ("data", "Ballance.iso (ISO 9660)", ...). */
const char *vfs_describe(void);
bool vfs_mounted(void);
void vfs_unmount(void);

/* ---- backends ---- */

/* A mounted file system. open returns a backend handle (NULL = missing) and the file size. */
typedef struct {
    void *ctx;
    void *(*open)(void *ctx, const char *rel, uint64_t *size);
    int64_t (*read_at)(void *ctx, void *file, uint64_t off, void *dst, size_t len);
    void (*close)(void *ctx, void *file);
    bool (*list)(void *ctx, const char *dir, VfsListFn fn, void *user);
    void (*unmount)(void *ctx);
} VfsBackend;
/* Replaces the current mount (the backend is copied). desc is copied too. */
void vfs_mount(const VfsBackend *b, const char *desc);
/* Takes the current mount away without unmounting it, for a layer that reads through it (iscab.c): the
   caller owns *b afterwards (b->unmount) or hands it back with vfs_mount. False if nothing is mounted. */
bool vfs_detach(VfsBackend *b, char *desc, size_t cap);

/* Random-access bytes of a disc image. read_at: up to len bytes at off, count or -1 (see thread safety
   above). close may be NULL. */
typedef struct {
    void *ctx;
    uint64_t size;
    int64_t (*read_at)(void *ctx, uint64_t off, void *dst, size_t len);
    void (*close)(void *ctx);
} VfsSource;
/* Mounts the ISO 9660 file system of an image. sector_hint: 2048, 2352 or 0 = detect. On failure
   returns false (the source is closed) and nothing new is mounted. */
bool vfs_mount_image(const VfsSource *src, int sector_hint, const char *desc);
