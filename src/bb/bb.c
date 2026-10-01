#include "bb.h"
#include <stdlib.h>

#define TABLE(x) {x, &x##_count}
static const struct {
    const CkBBDecl *const *decls;
    const unsigned *count;
} TABLES[] = {TABLE(bb_logics), TABLE(bb_narratives), TABLE(bb_ttdatabase), TABLE(bb_environment), TABLE(bb_visuals), TABLE(bb_sounds),
                TABLE(bb_controllers), TABLE(bb_physics), TABLE(bb_interface), TABLE(bb_tt_gravity),
                TABLE(bb_3dtransfo), TABLE(bb_particles), TABLE(bb_tt_toolbox), TABLE(bb_tt_misc)};

void bb_register_all(CkContext *ctx)
{
    static const CkBBDecl **all;
    static unsigned n;
    if (!all) {
        for (size_t t = 0; t < sizeof TABLES / sizeof *TABLES; t++) n += *TABLES[t].count;
        all = malloc(n * sizeof *all);
        unsigned k = 0;
        for (size_t t = 0; t < sizeof TABLES / sizeof *TABLES; t++)
            for (unsigned i = 0; i < *TABLES[t].count; i++) all[k++] = TABLES[t].decls[i];
    }
    ctx->bbs = all;
    ctx->nbbs = n;
}
