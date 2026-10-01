/* Loads every Virtools file of the game and checks the object tables and chunk structure.
   Expected counts come from tools/ck.py (see design/analysis.md). */
#include "ck/ck_file.h"
#include "vfs_host.h"
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
            failures++;                          \
        }                                        \
    } while (0)

typedef struct {
    const char *path;
    uint32_t objects, behaviors;
} Expect;

/* Identifier chain must be well formed in every chunk. */
static bool chain_ok(const CkChunk *c)
{
    uint32_t i = 0;
    while (i + 1 < c->ndw) {
        uint32_t next = c->dw[i + 1];
        if (!next) return true;
        if (next <= i || next >= c->ndw) return false;
        i = next;
    }
    return c->ndw == 0;
}

int main(void)
{
    if (!vfs_mount_default() || !vfs_exists("base.cmo")) {
        printf("SKIP: no game data (./data or OPENBALLANCE_DATA)\n");
        return 0;
    }
    static const Expect files[] = {
        {"base.cmo", 6111, 611},
        {"3D Entities/Gameplay.nmo", 10731, 1038},
        {"3D Entities/Menu.nmo", 15409, 1394},
        {"3D Entities/Level/Level_01.NMO", 356, 0},
        {"3D Entities/Level/Level_12.NMO", 0, 0},
        {"3D Entities/PH/P_Modul_18.nmo", 663, 44},
    };
    for (size_t k = 0; k < sizeof files / sizeof *files; k++) {
        CkFile f;
        char err[128];
        bool ok = ck_file_load(&f, files[k].path, err, sizeof err);
        CHECK(ok, "%s: %s", files[k].path, err);
        if (!ok) continue;
        uint32_t beh = 0, bad = 0;
        for (uint32_t i = 0; i < f.nobjects; i++) {
            const CkFileObject *o = &f.objects[i];
            if (o->class_id == 8) beh++;
            if (o->has_chunk && !chain_ok(&o->chunk)) bad++;
        }
        if (files[k].objects) CHECK(f.nobjects == files[k].objects, "%s: %u objects", files[k].path, f.nobjects);
        CHECK(beh == files[k].behaviors || !files[k].objects, "%s: %u behaviors", files[k].path, beh);
        CHECK(!bad, "%s: %u chunks with a broken identifier chain", files[k].path, bad);
        printf("%-34s objects %5u  behaviors %4u  managers %u\n", files[k].path, f.nobjects, beh, f.nmanagers);
        ck_file_free(&f);
    }
    if (failures) printf("%d failure(s)\n", failures);
    else printf("PASS\n");
    return failures != 0;
}
