/* Runs base.cmo headless: launches the level scene and executes frames, tracing Building Block
   executions. Usage: run_test [frames] [-v]  (-v prints every execution of the first frames). */
#include "bb/bb.h"
#include "ck/ck.h"
#include "ck/ck_types.h"
#include "phys/phys.h"
#include "ck/ck_sound.h"
#include "ck/ck_3d.h"
#include "vfs_host.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif
#include <sys/resource.h>

static int verbose, verbose_from;   /* trace frames [verbose_from, verbose) (env TRACE_FROM) */
static CkContext *g_ctx;

typedef struct {
    char name[64];
    int exec, missing, first_frame, order;
} Stat;
static int order_counter;
static Stat stats[512];
static int nstats;

static Stat *stat_for(const char *name)
{
    for (int i = 0; i < nstats; i++)
        if (!strcmp(stats[i].name, name)) return &stats[i];
    if (nstats == 512) return &stats[511];
    snprintf(stats[nstats].name, sizeof stats[nstats].name, "%s", name);
    stats[nstats].first_frame = (int)g_ctx->frame;
    stats[nstats].order = order_counter++;
    return &stats[nstats++];
}

static void path_of(CkContext *ctx, CkBehavior *b, char *out, size_t n)
{
    const char *parts[32];
    int k = 0;
    for (CkBehavior *t = b; t && k < 32; t = ck_behavior(ctx, t->parent)) parts[k++] = t->h.name;
    out[0] = 0;
    for (int i = k - 1; i >= 0; i--) {
        strncat(out, parts[i], n - strlen(out) - 1);
        if (i) strncat(out, " / ", n - strlen(out) - 1);
    }
}

static void trace(CkContext *ctx, CkBehavior *b, const char *what)
{
    Stat *s = stat_for(b->h.name);
    if (!strcmp(what, "missing")) s->missing++;
    else s->exec++;
    if (verbose && ctx->frame >= (unsigned)verbose_from && ctx->frame < (unsigned)verbose && strcmp(what, "missing")) {
        char p[512];
        path_of(ctx, b, p, sizeof p);
        printf("  f%u %s%s\n", ctx->frame, p, b->bb ? "" : "  [missing]");
    }
}

static void log_line(const char *m) { printf("log: %s\n", m); }

/* saves: read from ./out/save/<name> if present, else the game data; written to ./out/save */
static uint8_t *load_user(CkContext *ctx, const char *name, size_t *size)
{
    char path[512];
    snprintf(path, sizeof path, "out/save/%s", name);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc(n ? (size_t)n : 1);
        *size = fread(b, 1, (size_t)n, f);
        fclose(f);
        return b;
    }
    return vfs_read_all(name, size);
}
static bool save_user(CkContext *ctx, const char *name, const void *data, size_t size)
{
    char path[512];
    snprintf(path, sizeof path, "out/save/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(data, 1, size, f);
    fclose(f);
    return true;
}

static const char *registry(CkContext *ctx, const char *section, const char *entry)
{
    if (!strcmp(entry, "Language")) return "1";   /* English */
    return NULL;
}

static int by_exec(const void *a, const void *b) { return ((const Stat *)b)->exec - ((const Stat *)a)->exec; }

/* env PHYSPROBE=1: the player balls on a flat floor (docs/physics.md 5): the acceleration and terminal speed
   under the ball force, and the bounce height ratio of a drop */
static void probe_step(PhysWorld *w, int psis)
{
    for (int i = 0; i < psis; i++) phys_frame(w, 1000.0f / 66, NULL, NULL);
}

static uint32_t probe_index(PhysWorld *w, uint32_t entity)
{
    uint32_t ent, kind, ntri;
    float pos[3];
    bool aw, fx;
    for (uint32_t i = 0; phys_debug_body(w, i, &ent, pos, &aw, &fx, &kind, &ntri); i++)
        if (ent == entity) return i;
    return 0;
}

static void probe_world(PhysWorld **w, PhysShape *floor, PhysBody **fb)
{
    *w = phys_world_create();
    float g[3] = {0, -20, 0};
    phys_set_gravity(*w, g);
    phys_set_time_factor(*w, 1);
    PhysBodyDesc d = {.fixed = true, .friction = 0.7f, .elasticity = 0.3f, .mass = 1, .collide = true, .lin_damp = 0.1f, .rot_damp = 0.1f};
    memcpy(d.group, "Floor", 6);
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    *fb = phys_body_create(*w, &d, floor, m, 1);
}

static int phys_probe(void)
{
    static const struct {
        const char *name;
        float friction, elasticity, mass, lin, rot, force;
    } balls[] = {{"stone", 0.5f, 0.1f, 10, 0.3f, 0.1f, 0.92f}, {"wood", 0.8f, 0.2f, 1.9f, 0.9f, 0.1f, 0.43f}, {"paper", 0.5f, 0.4f, 0.2f, 1.5f, 0.1f, 0.065f}};
    float fv[] = {-50, 0, -50, 1000, 0, -50, 1000, 0, 50, -50, 0, 50};
    /* PHYSPROBE=2: a 4000 m floor (the size-dependent tests of the ledge, the hull and the OV tree) */
    if (atoi(getenv("PHYSPROBE")) == 2)
        for (int i = 0; i < 12; i += 3) fv[i] = fv[i] < 0 ? -2000 : 2000, fv[i + 2] = fv[i + 2] < 0 ? -2000 : 2000;
    uint16_t ft[] = {0, 2, 1, 0, 3, 2};
    PhysShape *floor = phys_shape_convex();
    phys_shape_add_triangles(floor, fv, 4, ft, 2);
    for (int k = 0; k < 3; k++) {
        PhysShape *sph = phys_shape_sphere(2);
        PhysBodyDesc d = {.friction = balls[k].friction, .elasticity = balls[k].elasticity, .mass = balls[k].mass, .collide = true,
                          .lin_damp = balls[k].lin, .rot_damp = balls[k].rot};
        memcpy(d.group, "Ball", 5);
        /* rolling: settle 2 s, then the force along +x */
        PhysWorld *w;
        PhysBody *fb;
        probe_world(&w, floor, &fb);
        float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 2.03f, 0, 1}};
        PhysBody *b = phys_body_create(w, &d, sph, m, 2);
        probe_step(w, 132);
        uint32_t bi = probe_index(w, 2);
        float v[3], wv[3];
        int ms;
        uint32_t nb, na, nc;
        phys_debug(w, &nb, &na, &nc);
        printf("probe %s: settled, %u contacts\n", balls[k].name, nc);
        float f[3] = {balls[k].force, 0, 0}, pt[3] = {0, 0, 0};
        phys_body_wake(w, b);
        PhysForce *pf = phys_force_create(w, b, f, pt, true);
        double t = 0, vprev = 0, acc0 = 0;
        for (int i = 1; i <= 66 * 30; i++) {
            probe_step(w, 1);
            t += 1.0 / 66;
            phys_debug_core(w, bi, v, wv, &ms);
            if (i == 33) acc0 = v[0] / t;
            if (i % 330 == 0) {
                uint32_t ent, kind, ntri;
                float pos[3];
                bool aw, fx;
                phys_debug_body(w, bi, &ent, pos, &aw, &fx, &kind, &ntri);
                printf("probe %s: t=%5.1fs vx=%7.3f (dv/dt %6.3f) y=%.4f vy=%.4f\n", balls[k].name, t, v[0], (v[0] - vprev) / 5.0, pos[1], v[1]);
                vprev = v[0];
            }
        }
        printf("probe %s: initial acceleration %.3f m/s2, speed after 30 s %.3f m/s\n", balls[k].name, acc0, v[0]);
        phys_force_destroy(w, pf);
        phys_world_destroy(w);
        /* bounce: dropped from 10 m (the ball's bottom), the first apex */
        probe_world(&w, floor, &fb);
        float m2[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 12, 0, 1}};
        b = phys_body_create(w, &d, sph, m2, 2);
        bi = probe_index(w, 2);
        int phase = 0;
        float apex = 0, ymin = 1e9f, vin = 0, vout = 0;
        for (int i = 0; i < 66 * 8; i++) {
            probe_step(w, 1);
            uint32_t ent, kind, ntri;
            float pos[3];
            bool aw, fx;
            phys_debug_body(w, bi, &ent, pos, &aw, &fx, &kind, &ntri);
            phys_debug_core(w, bi, v, wv, &ms);
            if (getenv("PROBEV") && i < 400) {
                uint32_t nb, na, nc;
                phys_debug(w, &nb, &na, &nc);
                printf("  drop %d y=%.4f vy=%.4f aw=%d contacts %u t=%.4f\n", i, pos[1], v[1], aw, nc, phys_time(w));
                uint32_t ea, eb;
                int ty[2];
                float gap, n[3], p[3];
                for (uint32_t c = 0; phys_debug_contact(w, c, &ea, &eb, ty, &gap, n, p); c++)
                    printf("    contact %u(%d)-%u(%d) gap %.4f n %.3f,%.3f,%.3f p %.2f,%.2f,%.2f\n", ea, ty[0], eb, ty[1], gap, n[0], n[1], n[2], p[0], p[1], p[2]);
            }
            if (phase == 0 && v[1] > 0) phase = 1, vout = v[1];
            if (phase == 0 && pos[1] < ymin) ymin = pos[1], vin = v[1];
            if (phase == 1) {
                if (pos[1] > apex) apex = pos[1];
                if (v[1] < 0) break;
            }
        }
        printf("probe %s: drop 10 m: %.3f m/s in, %.3f m/s out (ratio %.4f, sqrt e = %.4f), first apex %.4f m: height ratio %.4f\n",
               balls[k].name, -vin, vout, vout / -vin, sqrtf(balls[k].elasticity * 0.3f), apex - 2, (apex - 2) / 10);
        phys_world_destroy(w);
        phys_shape_free(sph);
    }
    phys_shape_free(floor);
    return 0;
}

int main(int argc, char **argv)
{
    if (getenv("PHYSPROBE")) return phys_probe();
    int frames = argc > 1 ? atoi(argv[1]) : 60;
    verbose = argc > 2 && !strncmp(argv[2], "-v", 2) ? (argv[2][2] ? atoi(argv[2] + 2) : 3) : 0;
    if (getenv("TRACE_FROM")) verbose_from = atoi(getenv("TRACE_FROM"));
    if (!vfs_mount_default() || !vfs_exists("base.cmo")) {
        printf("SKIP: no game data\n");
        return 0;
    }
    CkContext ctx;
    ck_init(&ctx);
    g_ctx = &ctx;
    bb_register_all(&ctx);
    ctx.trace = trace;
    ctx.load_user_file = load_user;
    ctx.registry = registry;
    ctx.save_user_file = save_user;
    ctx.log = log_line;
    char err[128];
    if (!ck_load(&ctx, "base.cmo", NULL, err, sizeof err)) {
        printf("FAIL: %s\n", err);
        return 1;
    }
    CkBeObject *level = ck_beobject(&ctx, ctx.level);
    printf("objects %u, level %s: %u scripts, %u scene entries\n", ctx.nobjs, level ? level->h.name : "-",
           level ? level->scripts.n : 0, level ? level->nscene : 0);
    ck_launch_level_scene(&ctx);
    for (uint32_t i = 0; level && i < level->scripts.n; i++) {
        CkBehavior *s = ck_behavior(&ctx, level->scripts.v[i]);
        printf("  script %-24s %s\n", s->h.name, s->bflags & CKBF_ACTIVE ? "active" : "-");
    }
    static float audio[735 * 2];
    for (int f = 0; f < frames; f++) {
        /* env KEYS="from-to:key,...": DirectInput key codes (hex) held for the frame range */
        memcpy(ctx.keys_prev, ctx.keys, sizeof ctx.keys);
        memset(ctx.keys, 0, sizeof ctx.keys);
        for (const char *k = getenv("KEYS"); k && *k;) {
            int a, z, key, n = 0;
            if (sscanf(k, "%d-%d:%x%n", &a, &z, &key, &n) != 3) break;
            if (f >= a && f <= z) ctx.keys[key & 0xff] = 1;
            k += n;
            if (*k == ',') k++;
        }
        /* env SETCELLS="array:col:int": every row's cell forced each frame (testing, e.g. unlocking levels) */
        const char *sc = getenv("SETCELLS");
        if (sc) {
            char name[128];
            int col = 0, val = 0;
            if (sscanf(sc, "%127[^:]:%d:%d", name, &col, &val) == 3)
                for (uint32_t i = 0; i < ctx.nobjs; i++) {
                    CkObj *o = ctx.objs[i];
                    if (!o || o->cid != CKCID_DATAARRAY || strcmp(o->name, name)) continue;
                    CkDataArray *a = (CkDataArray *)o;
                    for (uint32_t r = 0; r < a->nrows && (uint32_t)col < a->ncols; r++) ck_array_cell(a, r, (uint32_t)col)->i = val;
                }
        }
        ck_process(&ctx, 1000.0f / 60);
        ck_run_pre_render(&ctx);          /* what render_frame does first */
        ck_sound_mix(&ctx, audio, 735);   /* 44100 / 60: playback advances like on the platform */
        /* env DUMPARRAY="name:frame": the data array's rows after that frame */
        const char *da = getenv("DUMPARRAY");
        if (da && strchr(da, ':') && atoi(strchr(da, ':') + 1) == f) {
            char name[128];
            snprintf(name, sizeof name, "%.*s", (int)(strchr(da, ':') - da), da);
            for (uint32_t i = 0; i < ctx.nobjs; i++) {
                CkObj *o = ctx.objs[i];
                if (!o || o->cid != CKCID_DATAARRAY || strcmp(o->name, name)) continue;
                CkDataArray *a = (CkDataArray *)o;
                printf("array %s (id %u) key=%d rows=%u\n", name, o->id, a->key_column, a->nrows);
                for (uint32_t r = 0; r < a->nrows; r++) {
                    printf("  %u:", r);
                    for (uint32_t c = 0; c < a->ncols; c++) {
                        CkCell *x = ck_array_cell(a, r, c);
                        char v[128];
                        if (a->cols[c].type == CKARRAYTYPE_STRING) snprintf(v, sizeof v, "'%s'", x->s ? x->s : "");
                        else if (a->cols[c].type == CKARRAYTYPE_FLOAT) snprintf(v, sizeof v, "%g", x->f);
                        else if (a->cols[c].type == CKARRAYTYPE_PARAMETER) {
                            CkParameter *p = ck_param(&ctx, x->obj);
                            v[0] = 0;
                            if (p) ck_param_to_string(&ctx, p, v, sizeof v);
                        } else if (a->cols[c].type == CKARRAYTYPE_OBJECT) {
                            CkObj *ob = ck_obj(&ctx, x->obj);
                            snprintf(v, sizeof v, "#%s(%u)", ob ? ob->name : "-", x->obj);
                        } else snprintf(v, sizeof v, "%d", x->i);
                        printf(" %s=%s", a->cols[c].name, v);
                    }
                    printf("\n");
                }
            }
        }
        /* env WATCHPOS=name: the named 3D entities' world positions when they change */
        const char *wpos = getenv("WATCHPOS");
        if (wpos) {
            static float lastp[64][16]; static bool seenp[64];
            uint32_t k = 0;
            for (uint32_t i = 0; i < ctx.nobjs && k < 64; i++) {
                CkObj *o = ctx.objs[i];
                if (!o || strcmp(o->name, wpos) || !ck_is_3dentity_class(o->cid)) continue;
                Ck3dEntity *e = (Ck3dEntity *)o;
                if (memcmp(lastp[k], e->world, 64) || !seenp[k])
                    printf("f%d pos %s #%u = %.2f,%.2f,%.2f  x=%.2f,%.2f,%.2f z=%.2f,%.2f,%.2f parent=%s\n", f, wpos, o->id, e->world[3][0], e->world[3][1], e->world[3][2], e->world[0][0], e->world[0][1], e->world[0][2], e->world[2][0], e->world[2][1], e->world[2][2], ck_obj(&ctx, e->parent) ? ck_obj(&ctx, e->parent)->name : "-"), memcpy(lastp[k], e->world, 64), seenp[k] = true;
                k++;
            }
        }
        /* env BOXES="prefix:frame": world boxes of the 3D entities whose names start with prefix */
        const char *bx = getenv("BOXES");
        if (bx && strchr(bx, ':') && atoi(strchr(bx, ':') + 1) == f) {
            size_t pl = (size_t)(strchr(bx, ':') - bx);
            for (uint32_t i = 0; i < ctx.nobjs; i++) {
                CkObj *o = ctx.objs[i];
                if (!o || !ck_is_3dentity_class(o->cid) || strncmp(o->name, bx, pl)) continue;
                float box[6];
                ck_entity_world_box(&ctx, (Ck3dEntity *)o, box);
                printf("box %-24s %8.1f..%-8.1f %6.1f..%-6.1f %8.1f..%-8.1f\n", o->name, box[0], box[3], box[1], box[4], box[2], box[5]);
            }
        }
        /* env DUMPENT="name:frame": the entity's mesh and its materials */
        const char *de = getenv("DUMPENT");
        if (de && strchr(de, ':') && atoi(strchr(de, ':') + 1) == f) {
            size_t nl = (size_t)(strchr(de, ':') - de);
            for (uint32_t i = 0; i < ctx.nobjs; i++) {
                CkObj *o = ctx.objs[i];
                if (!o || !ck_is_3dentity_class(o->cid) || strlen(o->name ? o->name : "") != nl || strncmp(o->name, de, nl)) continue;
                Ck3dEntity *e = (Ck3dEntity *)o;
                CkMesh *m = ck_mesh(&ctx, e->mesh);
                printf("ent %s #%u cid %u mesh %u (%s) nmeshes %u mats", o->name, o->id, o->cid, e->mesh, m ? m->be.h.name : "-", e->meshes.n);
                for (uint32_t k = 0; m && k < m->materials.n; k++) printf(" %u", m->materials.v[k]);
                printf("\n");
            }
        }
        /* env DUMPGROUP="name:frame": the group's members with their world positions */
        const char *dg = getenv("DUMPGROUP");
        if (dg && strchr(dg, ':') && atoi(strchr(dg, ':') + 1) == f) {
            size_t gl = (size_t)(strchr(dg, ':') - dg);
            for (uint32_t i = 0; i < ctx.nobjs; i++) {
                CkObj *o = ctx.objs[i];
                if (!o || o->cid != CKCID_GROUP || (dg[gl - 1] == '*' ? strncmp(o->name, dg, gl - 1) : strlen(o->name) != gl || strncmp(o->name, dg, gl))) continue;
                CkGroup *g = (CkGroup *)o;
                printf("group %s (id %u) %u members\n", o->name, o->id, g->members.n);
                for (uint32_t k = 0; k < g->members.n; k++) {
                    CkObj *m = ck_obj(&ctx, g->members.v[k]);
                    Ck3dEntity *e = m && ck_is_3dentity_class(m->cid) ? (Ck3dEntity *)m : NULL;
                    printf("  #%u %-28s %s", g->members.v[k], m ? m->name : "-", m && (m->flags & CK_OBJECT_VISIBLE) ? "vis " : "    ");
                    if (e) printf("%.2f,%.2f,%.2f", e->world[3][0], e->world[3][1], e->world[3][2]);
                    printf("\n");
                }
            }
        }
        /* env MEMSTAT=n: heap bytes in use every n frames (macOS) */
#ifdef __APPLE__
        const char *ms = getenv("MEMSTAT");
        if (ms && atoi(ms) > 0 && f % atoi(ms) == 0) {
            malloc_statistics_t st;
            malloc_zone_statistics(NULL, &st);
            printf("f%d heap %zu bytes in use\n", f, st.size_in_use);
        }
#endif
        /* env WATCHPARAM=name: the named parameters' values when they change */
        const char *wp = getenv("WATCHPARAM");
        if (wp) {
            static char last[64][128];
            uint32_t k = 0;
            for (uint32_t i = 0; i < ctx.nobjs && k < 64; i++) {
                CkObj *o = ctx.objs[i];
                if (!o || strcmp(o->name, wp) || !(o->cid == CKCID_PARAMETERIN || o->cid == CKCID_PARAMETEROUT || o->cid == CKCID_PARAMETERLOCAL || o->cid == CKCID_PARAMETER)) continue;
                char v[128] = "";
                ck_param_to_string(&ctx, (CkParameter *)o, v, sizeof v);
                if (strcmp(v, last[k])) printf("f%d param %s #%u = '%s'\n", f, wp, o->id, v), snprintf(last[k], sizeof last[k], "%s", v);
                k++;
            }
        }
        /* env PHYSCHECK=n: every frame, non-finite body states abort the run; every n frames the fastest awake
           bodies (linear and angular speed), the live mindist count and the peak RSS (growth over restarts) */
        const char *pc = getenv("PHYSCHECK");
        if (pc && ctx.physics) {
            PhysWorld *pw = *(PhysWorld **)ctx.physics;
            static float vmax_seen;
            static uint32_t vmax_ent;
            static int vmax_frame;
            float top_v = 0, top_w = 0;
            uint32_t top_ve = 0, top_we = 0;
            for (uint32_t i = 0;; i++) {
                uint32_t ent, kind, ntri;
                float pos[3], v[3], wv[3];
                bool aw, fx;
                int ms;
                if (!phys_debug_body(pw, i, &ent, pos, &aw, &fx, &kind, &ntri)) break;
                phys_debug_core(pw, i, v, wv, &ms);
                for (int k = 0; k < 3; k++)
                    if (!isfinite(pos[k]) || !isfinite(v[k]) || !isfinite(wv[k])) {
                        CkObj *eo = ck_obj(&ctx, ent);
                        printf("f%d PHYSCHECK: non-finite state of %s (#%u)\n", f, eo ? eo->name : "?", ent);
                        return 1;
                    }
                float sv = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), sw = sqrtf(wv[0] * wv[0] + wv[1] * wv[1] + wv[2] * wv[2]);
                if (aw && sv > top_v) top_v = sv, top_ve = ent;
                if (aw && sw > top_w) top_w = sw, top_we = ent;
                if (sv > vmax_seen) vmax_seen = sv, vmax_ent = ent, vmax_frame = f;
            }
            if (atoi(pc) > 0 && f % atoi(pc) == 0) {
                uint32_t cs[6];
                struct rusage ru;
                phys_debug_collision(pw, cs);
                getrusage(RUSAGE_SELF, &ru);
                CkObj *ov = ck_obj(&ctx, top_ve), *ow = ck_obj(&ctx, top_we), *om = ck_obj(&ctx, vmax_ent);
                printf("f%d check: fastest %s %.2f m/s, spinning %s %.2f rad/s, max so far %s %.2f (f%d), %u mindists, maxrss %ld\n", f,
                       ov ? ov->name : "-", top_v, ow ? ow->name : "-", top_w, om ? om->name : "-", vmax_seen, vmax_frame, cs[0], (long)ru.ru_maxrss);
            }
        }
        /* env PHYSDEBUG=n: the physics world every n frames */
        const char *pd = getenv("PHYSDEBUG");
        if (pd && ctx.physics && f % atoi(pd) == 0) {
            PhysWorld *pw = *(PhysWorld **)ctx.physics;
            uint32_t nb, na, nc;
            phys_debug(pw, &nb, &na, &nc);
            printf("f%d phys: %u bodies, %u awake, %u contacts\n", f, nb, na, nc);
            for (uint32_t i = 0; i < nb; i++) {
                uint32_t ent, kind, ntri;
                float pos[3];
                bool aw, fx;
                phys_debug_body(pw, i, &ent, pos, &aw, &fx, &kind, &ntri);
                CkObj *eo = ck_obj(&ctx, ent);
                if (aw || kind == 0 || getenv("PHYSALL"))
                    printf("   %-24s %s%s kind=%u tris=%u pos=%.2f,%.2f,%.2f\n", eo ? eo->name : "?", aw ? "awake " : "", fx ? "fixed " : "", kind, ntri, pos[0], pos[1], pos[2]);
            }
            uint32_t cs[6];
            phys_debug_collision(pw, cs);
            printf("   coll: %u mindists (%u created), %u ov rechecks, %u watcher checks, %u impacts, %u tree guards\n", cs[0], cs[1], cs[2], cs[3],
                   cs[4], cs[5]);
            if (getenv("PHYSCONTACTS")) {
                uint32_t ea, eb;
                int ty[2];
                float gap, n[3], p[3];
                for (uint32_t i = 0; phys_debug_contact(pw, i, &ea, &eb, ty, &gap, n, p); i++) {
                    CkObj *oa = ck_obj(&ctx, ea), *ob = ck_obj(&ctx, eb);
                    printf("   contact %s(%d) - %s(%d) gap=%.4f n=%.3f,%.3f,%.3f p=%.2f,%.2f,%.2f\n", oa ? oa->name : "?", ty[0], ob ? ob->name : "?", ty[1], gap,
                           n[0], n[1], n[2], p[0], p[1], p[2]);
                }
            }
        }
        const char *d2 = getenv("DUMP2D");
        if (d2 && atoi(d2) == f) {
            for (uint32_t i = 0; i < ctx.nobjs; i++) {
                CkObj *o = ctx.objs[i];
                if (!o || !ck_is_2dentity_class(o->cid)) continue;
                Ck2dEntity *e = (Ck2dEntity *)o;
                CkMaterial *m = ck_material(&ctx, e->material);
                CkTexture *t = m ? ck_texture(&ctx, m->texture) : NULL;
                printf("2d %-16s vis=%d z=%d flags=%#x rect=%.2f,%.2f,%.2f,%.2f mat=%s diffuse=%.2f,%.2f,%.2f,%.2f tex=%s movie=%d slot=%d\n",
                       o->name, (o->flags & CK_OBJECT_VISIBLE) != 0, e->zorder, e->flags, e->rect[0], e->rect[1], e->rect[2],
                       e->rect[3], m ? m->be.h.name : "-", m ? m->diffuse.r : 0, m ? m->diffuse.g : 0, m ? m->diffuse.b : 0,
                       m ? m->diffuse.a : 0, t ? t->be.h.name : "-", t && t->movie, t ? t->current_slot : -1);
            }
        }
    }
    printf("\nmissing Building Blocks in the order they were first reached:\n");
    for (int i = 0; i < nstats; i++)
        if (stats[i].missing) printf("  f%-4d %s\n", stats[i].first_frame, stats[i].name);
    qsort(stats, nstats, sizeof *stats, by_exec);
    printf("\nBuilding Blocks executed in %d frames (missing = not implemented):\n", frames);
    for (int i = 0; i < nstats; i++)
        printf("  %6d %s%s\n", stats[i].exec, stats[i].name, stats[i].missing ? "  [missing]" : "");
    ck_free(&ctx);
    return 0;
}
