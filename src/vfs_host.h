/* Host-filesystem backends of the file layer (POSIX): directories and disc-image files. Used by the
   SDL build and the tests; a backend without a filesystem (gasm) mounts a VfsSource instead. */
#pragma once
#include "vfs.h"

/* Mounts a data root: a directory (an installed game, or the Ballance CD's files), a disc image (.iso,
   raw .bin), or a .cue sheet (its FILE line names the image, its TRACK line the sector format). Without
   base.cmo at its root but with the CD's InstallShield cabinets (Setup/data1.hdr), the game folder
   inside them is mounted instead (vfs_find_game). False if it can't be mounted; whether it holds the
   game is up to the caller (vfs_exists("base.cmo")). */
bool vfs_mount_path(const char *path);
/* $OPENBALLANCE_DATA if set, else ./data (the installed game directory; tests and tools run from the
   project root). Either may be the installed game folder, the CD folder or the CD image. */
bool vfs_mount_default(void);
