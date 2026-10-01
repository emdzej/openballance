/* InstallShield cabinet reader (clean room) and the file-layer backend built on it: the Ballance CD
   ships the game as Setup/data1.hdr + data1.cab + data2.cab (InstallShield 6, header version
   0x0100600c), and this lets the game run straight from the CD folder or its ISO image.

   Format notes: written from the format description in the unshield project's documentation and
   headers (github.com/twogood/unshield: lib/cabfile.h, the field layout comments of lib/libunshield.c,
   lib/file.c, lib/component.c, lib/file_group.c; MIT licensed) and checked field by field against
   Ballance's data1.hdr; no code was taken from it. See iscab.c for the layouts.

   Reading goes through a VfsBackend (the mount underneath: a host directory, the ISO 9660 reader or
   gasm assets), so the cabinet works on any source the file layer has. */
#pragma once
#include "vfs.h"

typedef struct IsCab IsCab;

/* File descriptor flags (cabfile.h). */
enum { ISCAB_SPLIT = 1, ISCAB_OBFUSCATED = 2, ISCAB_COMPRESSED = 4, ISCAB_INVALID = 8 };

typedef struct {
    const char *name;     /* file name */
    const char *dir;      /* its directory inside the group's target, '\\'-separated ("" = the target) */
    uint16_t flags;       /* ISCAB_* */
    uint64_t size;        /* expanded */
    uint64_t stored;      /* bytes in the volumes (compressed size, or size) */
    uint64_t offset;      /* of the stored data in its first volume */
    uint16_t volume;      /* first volume (N of dataN.cab) */
    uint8_t md5[16];      /* of the expanded data */
} IsCabFile;

/* Opens <hdr> (e.g. "Setup/data1.hdr") through io; volumes are <dir>/<prefix>N.cab next to it. io is
   borrowed (it must outlive the cabinet). NULL if the header isn't a supported cabinet (major version
   5 or later); err (may be NULL) says why. */
IsCab *iscab_open(const VfsBackend *io, const char *hdr, char *err, size_t errcap);
void iscab_close(IsCab *c);
int iscab_version(const IsCab *c);                    /* major version (6 for Ballance) */

uint32_t iscab_file_count(const IsCab *c);
const IsCabFile *iscab_file(const IsCab *c, uint32_t index);
/* The expanded file into dst (iscab_file()->size bytes): reassembled across volumes, deobfuscated,
   inflated and checked against its MD5. False on any error (missing volume, corrupt data). */
bool iscab_extract(IsCab *c, uint32_t index, uint8_t *dst);

/* File groups: a name and a range of file indices; components list the groups they install. */
int iscab_group_count(const IsCab *c);
const char *iscab_group(const IsCab *c, int g, uint32_t *first, uint32_t *last);
int iscab_component_count(const IsCab *c);
const char *iscab_component(const IsCab *c, int k, int *ngroups);
const char *iscab_component_group(const IsCab *c, int k, int i);

/* ---- file-layer backend ---- */

/* Mounts the installed game folder held by the cabinets of the current mount, as a layer over it: the
   files of every non-InstallShield file group (the ones whose name isn't "<...>": here only
   "Programmdateien der Anwendung", the program files), at their install paths. hdr: "Setup/data1.hdr" or
   "data1.hdr". On failure the current mount stays as it was. */
bool vfs_mount_cab(const char *hdr);

/* Makes the current mount the game folder: kept as it is if it has base.cmo, else its cabinets
   (Setup/data1.hdr, a CD) are mounted over it. False (the mount is left as it was) if neither. */
bool vfs_find_game(void);

/* Counters of the cabinet backend since it was mounted (for measurements). */
typedef struct { uint32_t opens, extracts, cache_hits; uint64_t bytes_out; } IsCabStats;
bool vfs_cab_stats(IsCabStats *s);
