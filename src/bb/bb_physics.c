/* physics_RT.dll: Terratools' physics Building Blocks and their manager (6bed328b:141f5148) over the Ipion
   engine, here src/phys/ (docs/physics.md). The manager keeps the entity -> body registry and the collision
   surfaces cached by name, retries the deferred commands (constraints, forces, wake-ups: they wait until
   their bodies are physicalized) every frame, steps the world in its PostProcess and writes the awake
   bodies back to their entities. */
#include "bb.h"
#include "../ck/ck_3d.h"
#include "../phys/phys.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct CcInst;

typedef struct {
    CkId entity;
    PhysBody *body;
    float scale[3];
    struct CcInst *cc;        /* +0x2c: the PhysicsContinuousContact state of the object */
    PhysShape *own;           /* a ball's shape: the original's ball object has no surface, this one lives
                                 and dies with the record */
} Record;

typedef struct {
    char *name;
    PhysShape *shape;
} Surface;

/* a PhysicsCollDetection listener (0x30 bytes: last time, min, max, sleep, object, behavior, id) */
typedef struct {
    CkId behavior, entity;
    float min, max, sleep;
    int32_t id;
    bool use_id;
    double last;
} Listener;

typedef struct {
    PhysWorld *w;
    Record *recs;
    uint32_t nrecs;
    Surface *surfs;
    uint32_t nsurfs;
    CkIds pending;            /* behaviors with a queued command (the installer queue, FUN_10008160) */
    Listener *listeners;
    uint32_t nlisteners;
    int32_t coll_id_attr, cc_id_attr;
    /* mgr +0x54: the continuous-contact timers {time, group, record} and the group count of the last install */
    struct CcTimer *timers;
    uint32_t ntimers, captimers;
    int32_t cc_groups;
    PhysBody *dying;          /* the body being deleted: its own listeners are gone (0x10009ab0) */
} Physics;

static void on_impact(void *user, PhysImpactEvent *ev);
static void on_contact(void *user, PhysBody *a, PhysBody *b, bool begin);
static void cc_timers(CkContext *ctx);
static void cc_forget(Physics *p, CkId entity);

static void physics_frame(CkContext *ctx);
static void physics_free(CkContext *ctx);

static Physics *physics(CkContext *ctx)
{
    if (!ctx->physics) {
        Physics *p = calloc(1, sizeof *p);
        p->w = phys_world_create();
        p->coll_id_attr = ck_attribute_type(ctx, "Coll Detection ID");
        p->cc_id_attr = ck_attribute_type(ctx, "Continuous Contact ID");
        phys_set_listeners(p->w, on_impact, on_contact, ctx);
        ctx->physics = p;
        ctx->physics_frame = physics_frame;
        ctx->physics_free = physics_free;
    }
    return ctx->physics;
}

static Record *record(Physics *p, CkId e)
{
    for (uint32_t i = 0; i < p->nrecs; i++)
        if (p->recs[i].entity == e) return &p->recs[i];
    return NULL;
}

/* FUN_10007800: the registry lookup */
static PhysBody *body_of(CkContext *ctx, CkId e)
{
    Record *r = ctx->physics ? record(ctx->physics, e) : NULL;
    return r ? r->body : NULL;
}

static void unregister(Physics *p, CkId e)
{
    for (uint32_t i = 0; i < p->nrecs; i++)
        if (p->recs[i].entity == e) {
            p->dying = p->recs[i].body;
            phys_body_destroy(p->w, p->recs[i].body);
            p->dying = NULL;
            phys_shape_free(p->recs[i].own);
            cc_forget(p, e);
            p->recs[i] = p->recs[--p->nrecs];
            return;
        }
}

static void physics_free(CkContext *ctx)
{
    Physics *p = ctx->physics;
    if (!p) return;
    phys_world_destroy(p->w);
    for (uint32_t i = 0; i < p->nrecs; i++) phys_shape_free(p->recs[i].own);
    for (uint32_t i = 0; i < p->nsurfs; i++) free(p->surfs[i].name), phys_shape_free(p->surfs[i].shape);
    free(p->surfs);
    free(p->recs);
    free(p->pending.v);
    free(p->listeners);
    free(p->timers);
    free(p);
    ctx->physics = NULL;
}

/* ---- entity helpers ---- */

static void entity_scale(const Ck3dEntity *e, float s[3])
{
    for (int i = 0; i < 3; i++) s[i] = sqrtf(e->world[i][0] * e->world[i][0] + e->world[i][1] * e->world[i][1] + e->world[i][2] * e->world[i][2]);
}

static void unscaled_world(const Ck3dEntity *e, float m[4][4])
{
    float s[3];
    entity_scale(e, s);
    memcpy(m, e->world, 64);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) m[i][j] = s[i] > 0 ? m[i][j] / s[i] : 0;
}

/* CK3dEntity::Transform (+0x17c) / TransformVector (+0x184) with a referential (NULL: world) */
static void transform(const Ck3dEntity *ref, const float in[3], float out[3], bool vector)
{
    if (!ref) {
        memcpy(out, in, 12);
        return;
    }
    for (int j = 0; j < 3; j++)
        out[j] = in[0] * ref->world[0][j] + in[1] * ref->world[1][j] + in[2] * ref->world[2][j] + (vector ? 0 : ref->world[3][j]);
}

static void write_back(void *user, uint32_t entity, const float world[4][4])
{
    CkContext *ctx = user;
    Ck3dEntity *e = ck_entity(ctx, entity);
    if (e) ck_entity_set_world(ctx, e, world, false);   /* SetWorldMatrix(M, FALSE) */
}

/* deferred commands of the pending behaviors: retried until they report done */
typedef bool (*CommandFn)(CkContext *ctx, CkBehavior *b);

typedef struct {
    CommandFn command;        /* the queued Execute, NULL when none */
    PhysForce *force;
    PhysJoint *joint;
    struct CcInst *cc;        /* PhysicsContinuousContact: local 1 */
} CmdState;

static CmdState *cmd_state(CkBehavior *b)
{
    if (!b->bb_state) b->bb_state = calloc(1, sizeof(CmdState));
    return b->bb_state;
}

static void queue(CkContext *ctx, CkBehavior *b, CommandFn fn)
{
    CmdState *s = cmd_state(b);
    if (fn(ctx, b)) {
        s->command = NULL;
        return;
    }
    s->command = fn;
    Physics *p = physics(ctx);
    if (!ck_ids_has(&p->pending, b->h.id)) ck_ids_push(&p->pending, b->h.id);
}

/* FUN_10007ce0 */
static void physics_frame(CkContext *ctx)
{
    Physics *p = ctx->physics;
    for (uint32_t i = 0; i < p->pending.n;) {       /* FUN_100081d0 */
        CkBehavior *b = ck_behavior(ctx, p->pending.v[i]);
        CmdState *s = b ? b->bb_state : NULL;
        if (!s || !s->command || s->command(ctx, b)) {
            if (s) s->command = NULL;
            p->pending.v[i] = p->pending.v[--p->pending.n];
        } else {
            i++;
        }
    }
    phys_frame(p->w, ctx->delta_ms, write_back, ctx);
    cc_timers(ctx);                               /* FUN_100017a0 */
}

/* ---- Physicalize ---- */

static PhysShape *surface(Physics *p, const char *name)
{
    for (uint32_t i = 0; i < p->nsurfs; i++)
        if (!strcmp(p->surfs[i].name, name)) return p->surfs[i].shape;
    return NULL;
}

static void mesh_points(const CkMesh *m, const float s[3], float *out)
{
    for (uint32_t i = 0; i < m->nverts; i++)
        out[i * 3] = m->verts[i].pos.x * s[0], out[i * 3 + 1] = m->verts[i].pos.y * s[1], out[i * 3 + 2] = m->verts[i].pos.z * s[2];
}

/* ---- Physicalize 7522370e:37ec15ec (FUN_100026b0, creation core FUN_10002380): pins 0 Fixed, 1 Friction,
   2 Elasticity, 3 Mass, 4 Collision Group, 5 Start Frozen, 6 Enable Collision, 7 Automatic Mass Center,
   8 / 9 linear / rotational damping, 10 Collision Surface, then the convex meshes, the balls (position,
   radius: only the last radius counts) and the concave meshes (settings 0..2 their counts, 3 the mass
   centre shift). A ball when there is one (or no surface name), else the surface cached by name, built
   from the meshes scaled by the entity (convex: point clouds, concave: triangles). Physicalize -> Out1
   (an already physicalized target isn't changed), Unphysicalize -> Out2. ---- */
static int bb_physicalize(CkContext *ctx, CkBehavior *b)
{
    Physics *p = physics(ctx);
    Ck3dEntity *e = ck_entity(ctx, bb_target(ctx, b));
    if (!e) return CKBR_OK;
    if (!ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 1, false);
        unregister(p, e->be.h.id);
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    ck_activate_input(ctx, b, 0, false);
    if (record(p, e->be.h.id)) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    PhysBodyDesc d;
    memset(&d, 0, sizeof d);
    int32_t fixed = 0, frozen = 0, collide = 1, automass = 1;
    d.friction = 0.4f, d.elasticity = 0.5f, d.mass = 1;
    bb_get_in(ctx, b, 0, &fixed, 4);
    bb_get_in(ctx, b, 1, &d.friction, 4);
    bb_get_in(ctx, b, 2, &d.elasticity, 4);
    bb_get_in(ctx, b, 3, &d.mass, 4);
    const char *group = bb_in_string(ctx, b, 4);
    if (group) snprintf(d.group, sizeof d.group, "%s", group);
    bb_get_in(ctx, b, 5, &frozen, 4);
    bb_get_in(ctx, b, 6, &collide, 4);
    bb_get_in(ctx, b, 7, &automass, 4);
    bb_get_in(ctx, b, 8, &d.lin_damp, 4);
    bb_get_in(ctx, b, 9, &d.rot_damp, 4);
    d.fixed = fixed != 0, d.start_frozen = frozen != 0, d.collide = collide != 0;
    const char *surf = bb_in_string(ctx, b, 10);
    int32_t nconvex = 1, nball = 0, nconcave = 0;
    bb_get_local(ctx, b, 0, &nconvex, 4);
    bb_get_local(ctx, b, 1, &nball, 4);
    bb_get_local(ctx, b, 2, &nconcave, 4);
    if (!automass) {
        d.has_shift = true;
        bb_get_local(ctx, b, 3, d.shift, 12);
    }
    float scale[3], world[4][4];
    entity_scale(e, scale);
    unscaled_world(e, world);
    PhysShape *shape = NULL;
    bool own = false;
    if (nball > 0 || !surf || !*surf) {
        float radius = 1;
        for (int32_t i = 0; i < nball; i++) bb_get_in(ctx, b, 11 + (uint32_t)nconvex + (uint32_t)i * 2 + 1, &radius, 4);
        shape = phys_shape_sphere(radius * scale[0]);
        own = true;
        if (!surf || !*surf) d.collide = false;       /* the empty-name branch doesn't enable collision */
    } else {
        shape = surface(p, surf);
        if (!shape) {
            shape = phys_shape_convex();
            for (int32_t i = 0; i < nconvex; i++) {
                CkMesh *m = ck_mesh(ctx, bb_in_object(ctx, b, 11 + (uint32_t)i));
                if (!m || !m->nverts) continue;
                float *pts = malloc(m->nverts * 12);
                mesh_points(m, scale, pts);
                phys_shape_add_convex(shape, pts, m->nverts);
                free(pts);
            }
            for (int32_t i = 0; i < nconcave; i++) {
                CkMesh *m = ck_mesh(ctx, bb_in_object(ctx, b, 11 + (uint32_t)nconvex + (uint32_t)nball * 2 + (uint32_t)i));
                if (!m || !m->nverts || !m->nfaces) continue;
                float *pts = malloc(m->nverts * 12);
                uint16_t *tri = malloc(m->nfaces * 6);
                mesh_points(m, scale, pts);
                for (uint32_t f = 0; f < m->nfaces; f++)
                    for (int k = 0; k < 3; k++) tri[f * 3 + k] = m->faces[f].i[k];
                phys_shape_add_triangles(shape, pts, m->nverts, tri, m->nfaces);
                free(pts);
                free(tri);
            }
            if (phys_shape_empty(shape)) {
                char msg[160];
                snprintf(msg, sizeof msg, "Error: incorrect mesh for %s", e->be.h.name);
                if (ctx->log) ctx->log(msg);
                phys_shape_free(shape);
                ck_activate_output(ctx, b, 0, true);
                return CKBR_OK;
            }
            p->surfs = realloc(p->surfs, (p->nsurfs + 1) * sizeof *p->surfs);
            p->surfs[p->nsurfs++] = (Surface){strdup(surf), shape};
        }
    }
    PhysBody *body = phys_body_create(p->w, &d, shape, world, e->be.h.id);
    p->recs = realloc(p->recs, (p->nrecs + 1) * sizeof *p->recs);
    p->recs[p->nrecs++] = (Record){e->be.h.id, body, {scale[0], scale[1], scale[2]}, NULL, own ? shape : NULL};
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- SetPhysicsForce 56e20c57:0b926068 (FUN_10004800, command FUN_10004930): Create adds a controller
   pushing, every PSI, pIn Force Value along pIn Direction (through Direction Ref, normalised; (1,0,0) when
   too short) at pIn Position (object-local when Pos Referential is the target, else through it to world);
   Destroy removes it. ---- */
static bool cmd_force(CkContext *ctx, CkBehavior *b)
{
    CkId t = bb_target(ctx, b);
    PhysBody *body = body_of(ctx, t);
    if (!body) return false;
    float pos[3] = {0, 0, 0}, dir[3] = {0, 0, 1}, f = 10, wd[3], wp[3];
    bb_get_in(ctx, b, 0, pos, 12);
    bb_get_in(ctx, b, 2, dir, 12);
    bb_get_in(ctx, b, 4, &f, 4);
    CkId pref = bb_in_object(ctx, b, 1);
    transform(ck_entity(ctx, bb_in_object(ctx, b, 3)), dir, wd, true);
    /* f64: |d|^2 > 1e-4 normalized (0x1000e120), else (1, 0, 0); force = (f32)(d * Force Value) */
    double d[3] = {wd[0], wd[1], wd[2]}, l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (l2 > 1e-4)
        for (int k = 0; k < 3; k++) d[k] /= sqrt(l2);
    else
        d[0] = 1, d[1] = d[2] = 0;
    for (int k = 0; k < 3; k++) wd[k] = (float)(d[k] * f);
    bool local = pref == t;
    if (local) memcpy(wp, pos, 12);
    else transform(ck_entity(ctx, pref), pos, wp, false);
    cmd_state(b)->force = phys_force_create(physics(ctx)->w, body, wd, wp, local);
    return true;
}

static void release(CkContext *ctx, CkBehavior *b)
{
    CmdState *s = cmd_state(b);
    Physics *p = physics(ctx);
    /* the destroy calls ignore handles no longer in the world (unphysicalizing deletes a body's controllers
       and constraints) */
    if (s->force) {
        phys_force_destroy(p->w, s->force);
        s->force = NULL;
    }
    if (s->joint) {
        phys_joint_destroy(p->w, s->joint);
        s->joint = NULL;
    }
    s->command = NULL;
}

static int create_destroy(CkContext *ctx, CkBehavior *b, CommandFn fn)
{
    CmdState *s = cmd_state(b);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        if (!s->force && !s->joint) queue(ctx, b, fn);
        ck_activate_output(ctx, b, 0, true);
    }
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        release(ctx, b);
        ck_activate_output(ctx, b, 1, true);
    }
    return CKBR_OK;
}

static int bb_set_physics_force(CkContext *ctx, CkBehavior *b) { return create_destroy(ctx, b, cmd_force); }

/* FUN_10005ee0: reset clears the handle without deleting the IVP object */
static void cb_handle(CkContext *ctx, CkBehavior *b, int msg)
{
    (void)ctx;
    if (msg == CKM_BEHAVIORRESET && b->bb_state) memset(b->bb_state, 0, sizeof(CmdState));
}

/* ---- Set Physics Hinge 41cd3653:0de60c1d (FUN_10005850, command FUN_100059d0): target (objR) and pIn Object2
   (objA; either NULL: nothing is made) hinged at pIn Joint Referential's position about its Z axis ((0, 0, 1)
   without a referential; the anchor then falls back to the target's position, unverified). Limits (pins
   2..4) are never enabled by the game. ---- */
static bool cmd_hinge(CkContext *ctx, CkBehavior *b)
{
    CkId t = bb_target(ctx, b), o2 = bb_in_object(ctx, b, 0);
    if (!ck_entity(ctx, t) || !ck_entity(ctx, o2)) return true;
    PhysBody *a = body_of(ctx, t), *c = body_of(ctx, o2);
    if (!a || !c) return false;
    Ck3dEntity *ref = ck_entity(ctx, bb_in_object(ctx, b, 1)), *at = ref ? ref : ck_entity(ctx, t);
    float anchor[3] = {at->world[3][0], at->world[3][1], at->world[3][2]}, axis[3] = {0, 0, 1};
    if (ref) {
        float l = sqrtf(ref->world[2][0] * ref->world[2][0] + ref->world[2][1] * ref->world[2][1] + ref->world[2][2] * ref->world[2][2]);
        for (int k = 0; k < 3; k++) axis[k] = l > 0 ? ref->world[2][k] / l : 0;
    }
    cmd_state(b)->joint = phys_hinge_create(physics(ctx)->w, a, c, anchor, axis);
    return true;
}

static int bb_set_physics_hinge(CkContext *ctx, CkBehavior *b) { return create_destroy(ctx, b, cmd_hinge); }

/* ---- Set Physics Slider 2973360e:23d31aa7 (FUN_10005d90, command FUN_10005f10): along the axis from pIn
   Axis first Point to Axis second Point (entities), with pIn Limitations, Lower / Upper Limit (metres along
   the axis; the error is Object2's anchor minus the target's). A NULL target or Object2 makes nothing. ---- */
static bool cmd_slider(CkContext *ctx, CkBehavior *b)
{
    CkId t = bb_target(ctx, b), o2 = bb_in_object(ctx, b, 0);
    if (!ck_entity(ctx, t) || !ck_entity(ctx, o2)) return true;
    PhysBody *a = body_of(ctx, t), *c = body_of(ctx, o2);
    if (!a || !c) return false;
    Ck3dEntity *p0 = ck_entity(ctx, bb_in_object(ctx, b, 1)), *p1 = ck_entity(ctx, bb_in_object(ctx, b, 2));
    if (!p0 || !p1) return true;
    int32_t lim = 0;
    float lo = -1, hi = 1;
    bb_get_in(ctx, b, 3, &lim, 4);
    bb_get_in(ctx, b, 4, &lo, 4);
    bb_get_in(ctx, b, 5, &hi, 4);
    float a0[3] = {p0->world[3][0], p0->world[3][1], p0->world[3][2]}, a1[3] = {p1->world[3][0], p1->world[3][1], p1->world[3][2]};
    cmd_state(b)->joint = phys_slider_create(physics(ctx)->w, a, c, a0, a1, lim != 0, lo, hi);
    return true;
}

static int bb_set_physics_slider(CkContext *ctx, CkBehavior *b) { return create_destroy(ctx, b, cmd_slider); }

/* ---- Set Physics Ball Joint 5e624f0a:35160450 (FUN_10005120, command FUN_100052f0): at pIn Position 1 in
   Referential 1 ---- */
static bool cmd_balljoint(CkContext *ctx, CkBehavior *b)
{
    CkId t = bb_target(ctx, b), o2 = bb_in_object(ctx, b, 0);
    PhysBody *a = body_of(ctx, t), *c = ck_entity(ctx, o2) ? body_of(ctx, o2) : NULL;
    if (!a || (ck_entity(ctx, o2) && !c)) return false;
    float pos[3] = {0, 0, 0}, wp[3];
    bb_get_in(ctx, b, 1, pos, 12);
    transform(ck_entity(ctx, bb_in_object(ctx, b, 2)), pos, wp, false);
    cmd_state(b)->joint = phys_balljoint_create(physics(ctx)->w, a, c, wp);
    return true;
}

static int bb_set_physics_ball_joint(CkContext *ctx, CkBehavior *b) { return create_destroy(ctx, b, cmd_balljoint); }

/* ---- Set Physics Spring 24a06a3a:07100fce (FUN_10006360, command FUN_10006490): between pIn Position 1 in
   Referential 1 on the target and Position 2 in Referential 2 on Object2; Length = the rest length, Constant
   = k, Linear Dampening damps the relative anchor velocity along the spring, Global Dampening all of it
   (docs/ivp_core.md 12.3). No target: done; Object2 NULL or a body not physicalized: retried. ---- */
static bool cmd_spring(CkContext *ctx, CkBehavior *b)
{
    CkId t = bb_target(ctx, b), o2 = bb_in_object(ctx, b, 0);
    if (!ck_entity(ctx, t)) return true;
    PhysBody *a = body_of(ctx, t), *c = ck_entity(ctx, o2) ? body_of(ctx, o2) : NULL;
    if (!a || !c) return false;
    float p1[3] = {0, 0, 0}, p2[3] = {0, 0, 0}, w1[3], w2[3], len = 1, k = 1, ld = 0.1f, gd = 0.1f;
    bb_get_in(ctx, b, 1, p1, 12);
    bb_get_in(ctx, b, 3, p2, 12);
    bb_get_in(ctx, b, 5, &len, 4);
    bb_get_in(ctx, b, 6, &k, 4);
    bb_get_in(ctx, b, 7, &ld, 4);
    bb_get_in(ctx, b, 8, &gd, 4);
    transform(ck_entity(ctx, bb_in_object(ctx, b, 2)), p1, w1, false);
    transform(ck_entity(ctx, bb_in_object(ctx, b, 4)), p2, w2, false);
    cmd_state(b)->joint = phys_spring_create(physics(ctx)->w, a, w1, c, w2, len, k, ld, gd);
    return true;
}

static int bb_set_physics_spring(CkContext *ctx, CkBehavior *b) { return create_destroy(ctx, b, cmd_spring); }

/* ---- Physics WakeUp 38b851b5:72ca74ac (FUN_10004d50, command FUN_10004de0) ---- */
static bool cmd_wakeup(CkContext *ctx, CkBehavior *b)
{
    PhysBody *body = body_of(ctx, bb_target(ctx, b));
    if (!body) return false;
    phys_body_wake(physics(ctx)->w, body);
    return true;
}

static int bb_physics_wakeup(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    queue(ctx, b, cmd_wakeup);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Set Physics Globals 72af347c:03da71e1 (FUN_100055a0): Set Values: pIn Gravity and Physic Time Factor
   (the game: (0, -20, 0), 2 playing / 0 paused); Clean Physics World: a new world (gravity back to -9.81;
   the original keeps its registry, which would dangle: here it is emptied) ---- */
static int bb_set_physics_globals(CkContext *ctx, CkBehavior *b)
{
    Physics *p = physics(ctx);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        float g[3] = {0, -9.81f, 0}, f = 1;
        bb_get_in(ctx, b, 0, g, 12);
        bb_get_in(ctx, b, 1, &f, 4);
        phys_set_gravity(p->w, g);
        phys_set_time_factor(p->w, f);
    }
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        phys_world_destroy(p->w);
        p->w = phys_world_create();
        phys_set_listeners(p->w, on_impact, on_contact, ctx);
        for (uint32_t i = 0; i < p->nrecs; i++) phys_shape_free(p->recs[i].own);
        p->nrecs = 0;
        p->ntimers = 0;
        p->nlisteners = 0;
    }
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Physics Impulse 0c7e39bb:16db20d5 (FUN_10002f90): wakes the target (not fixed ones) and gives it pIn
   Impulse along pIn Direction (through Direction Ref, normalised) at pIn Position (object-local when
   Referential is the target, else through Referential to world): an async push, committed at the next PSI
   (after the queued revive of a frozen body). The settings (two points, constant force) are unused by the
   game. ---- */
static int bb_physics_impulse(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkId t = bb_target(ctx, b);
    PhysBody *body = body_of(ctx, t);
    if (!body || phys_body_fixed(body)) return CKBR_OK;
    Physics *p = physics(ctx);
    phys_body_wake(p->w, body);
    float pos[3] = {0, 0, 0}, dir[3] = {0, 0, 1}, imp = 10, wd[3], wp[3];
    bb_get_in(ctx, b, 0, pos, 12);
    bb_get_in(ctx, b, 2, dir, 12);
    bb_get_in(ctx, b, 4, &imp, 4);
    transform(ck_entity(ctx, bb_in_object(ctx, b, 3)), dir, wd, true);
    /* f64, normalized unless |d|^2 < 1e-19 (0x1000e120) */
    double d[3] = {wd[0], wd[1], wd[2]}, l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (l2 >= 1e-19)
        for (int k = 0; k < 3; k++) d[k] /= sqrt(l2);
    for (int k = 0; k < 3; k++) wd[k] = (float)(d[k] * imp);
    CkId ref = bb_in_object(ctx, b, 1);
    if (ref == t) {
        phys_impulse(p->w, body, wd, pos, true);
    } else {
        transform(ck_entity(ctx, ref), pos, wp, false);
        phys_impulse(p->w, body, wd, wp, false);
    }
    return CKBR_OK;
}

/* ---- PhysicsCollDetection 7435488d:201d1188 (FUN_10004080, event FUN_100042b0): Create listens to the
   target's impacts: a body with a "Coll Detection ID" attribute (else -1; with setting 0 it must equal pIn
   Collision ID) hitting faster than pIn Min Speed, at most every pIn Sleep afterwards seconds, sets pOut
   Entity, Speed (0-1) = speed / Max Speed, the normal (toward the target), the position (z = 0: an
   original bug) and fires Collision. Stop ends it. Active while listening. ---- */
static int bb_physics_coll_detection(CkContext *ctx, CkBehavior *b)
{
    Physics *p = physics(ctx);
    CkId t = bb_target(ctx, b);
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        bool found = false;
        for (uint32_t i = 0; i < p->nlisteners && !found; i++) found = p->listeners[i].behavior == b->h.id;
        if (!found) {
            Listener l = {b->h.id, t, 0.3f, 10, 0.5f, 1, false, -1e9};
            bb_get_in(ctx, b, 0, &l.min, 4);
            bb_get_in(ctx, b, 1, &l.max, 4);
            bb_get_in(ctx, b, 2, &l.sleep, 4);
            bb_get_in(ctx, b, 3, &l.id, 4);
            int32_t use = 0;
            bb_get_local(ctx, b, 1, &use, 4);
            l.use_id = use != 0;
            p->listeners = realloc(p->listeners, (p->nlisteners + 1) * sizeof *p->listeners);
            p->listeners[p->nlisteners++] = l;
        }
    }
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        for (uint32_t i = 0; i < p->nlisteners; i++)
            if (p->listeners[i].behavior == b->h.id) p->listeners[i--] = p->listeners[--p->nlisteners];
        return CKBR_OK;
    }
    for (uint32_t i = 0; i < p->nlisteners; i++)
        if (p->listeners[i].behavior == b->h.id) return CKBR_ACTIVATENEXTFRAME;
    return CKBR_OK;
}

static int32_t coll_id(CkContext *ctx, Physics *p, CkId e)
{
    CkParameter *ap = ck_param(ctx, ck_attribute_parameter(ctx, e, p->coll_id_attr));
    int32_t id = -1;
    if (ck_has_attribute(ctx, e, p->coll_id_attr)) {
        id = 0;
        if (ap && ap->value && ap->size >= 4) memcpy(&id, ap->value, 4);
    }
    return id;
}

/* FUN_100042b0 on the post-collision event, for a listener of `self`; the shared normal is negated in place
   when self is not the contact's first object, so later listeners see the flipped vector */
static void coll_event(CkContext *ctx, Physics *p, Listener *l, PhysBody *self, PhysImpactEvent *ev)
{
    PhysBody *other = ev->obj[0] == self ? ev->obj[1] : ev->obj[0];
    CkId oe = phys_body_entity(other);
    if (l->use_id && coll_id(ctx, p, oe) != l->id) return;
    if (!body_of(ctx, l->entity)) return;
    CkBehavior *beh = ck_behavior(ctx, l->behavior);
    double now = phys_time(p->w);
    if (!beh || now - l->last < l->sleep) return;
    CkBehaviorIO *o0 = beh->out.n ? ck_io(ctx, beh->out.v[0]) : NULL;
    if (o0 && o0->active) return;                 /* IsOutputActive(0) */
    float speed = sqrtf(ev->rel[0] * ev->rel[0] + ev->rel[1] * ev->rel[1] + ev->rel[2] * ev->rel[2]);   /* 0x1000e480 */
    if (speed <= l->min) return;
    float s01;
    if (speed <= 1e-4f) s01 = 1e-4f;
    else if (l->max < 1e-4f) s01 = 1;
    else {
        s01 = speed / l->max;
        if (s01 > 1) s01 = 1;
    }
    bb_set_out(ctx, beh, 0, &oe, 4);
    bb_set_out(ctx, beh, 1, &s01, 4);
    if (ev->obj[0] != self)
        for (int k = 0; k < 3; k++) ev->normal[k] = (float)(ev->normal[k] * -1.0);
    float n[3] = {ev->normal[0], ev->normal[1], ev->normal[2]};
    float wp[3] = {(float)ev->pos[0], (float)ev->pos[1], 0};   /* y written twice, z = 0 (an original bug) */
    bb_set_out(ctx, beh, 2, n, 12);
    bb_set_out(ctx, beh, 3, wp, 12);
    l->last = now;
    ck_activate_output(ctx, beh, 0, true);
}

/* the object listeners of the event's objects (0x1000a770): the mindist's first object's, then the second's,
   each from the most recently added */
static void on_impact(void *user, PhysImpactEvent *ev)
{
    CkContext *ctx = user;
    Physics *p = ctx->physics;
    for (int k = 0; k < 2; k++) {
        PhysBody *self = ev->order[k];
        CkId e = phys_body_entity(self);
        for (uint32_t i = p->nlisteners; i-- > 0;)
            if (i < p->nlisteners && p->listeners[i].entity == e) coll_event(ctx, p, &p->listeners[i], self, ev);
    }
}

/* ---- PhysicsContinuousContact 199e4cf1:545a78fe (FUN_100011e0): On installs (command FUN_10001470) a
   friction listener on the target (vtable 0x100631b8) with pIn 0 / 1 Time Delay Start / End and setting 0
   groups (outputs 'contact on n' / 'contact off n' in pairs); Off removes it and turns every group off
   (FUN_100015d0). The listener counts the target's friction contacts per group, the other body's
   "Continuous Contact ID" minus 1 (created FUN_100018f0, deleted FUN_10001af0); the manager's timers
   (FUN_100017a0, every frame after the step) fire "on n" once a contact has lasted Time Delay Start and
   "off n" after Time Delay End without one. The outputs are activated from the manager; the BB stays
   active. ---- */
typedef struct CcInst {
    float t_on, t_off;        /* +0x00 / +0x04 */
    CkId behavior;            /* +0x0c */
    CkId entity;              /* the target */
    int32_t ngroups;
    struct {
        int32_t on, count;
    } *groups;                /* +0x10 */
} CcInst;

typedef struct CcTimer {
    double time;
    int32_t group;
    CkId entity;              /* the registry record */
} CcTimer;

static void cc_remove_timer(Physics *p, uint32_t i)
{
    memmove(&p->timers[i], &p->timers[i + 1], (p->ntimers - i - 1) * sizeof *p->timers);
    p->ntimers--;
}

/* FUN_10001720: one timer per (record, group) */
static void cc_add_timer(Physics *p, CkId entity, int32_t g, double now)
{
    for (uint32_t i = 0; i < p->ntimers; i++)
        if (p->timers[i].group == g && p->timers[i].entity == entity) return;
    if (p->ntimers == p->captimers) p->captimers = p->captimers * 2 + 1, p->timers = realloc(p->timers, p->captimers * sizeof *p->timers);
    p->timers[p->ntimers++] = (CcTimer){now, g, entity};
}

static void cc_forget(Physics *p, CkId entity)
{
    for (uint32_t i = p->ntimers; i-- > 0;)
        if (p->timers[i].entity == entity) cc_remove_timer(p, i);
    Record *r = record(p, entity);
    if (r) r->cc = NULL;
}

/* the group of the other body: its "Continuous Contact ID" minus 1 (no attribute: none) */
static int32_t cc_group(CkContext *ctx, Physics *p, PhysBody *other)
{
    CkId oe = phys_body_entity(other);
    if (p->cc_id_attr < 0 || !ck_has_attribute(ctx, oe, p->cc_id_attr)) return -1;
    CkParameter *ap = ck_param(ctx, ck_attribute_parameter(ctx, oe, p->cc_id_attr));
    if (!ap) return -1;
    int32_t id = 0;
    if (ap->value && ap->size >= 4) memcpy(&id, ap->value, 4);
    return id - 1;
}

static void cc_event(CkContext *ctx, Physics *p, PhysBody *self, PhysBody *other, bool begin)
{
    int32_t g = cc_group(ctx, p, other);
    if (g < 0) return;
    Record *r = record(p, phys_body_entity(self));
    if (!r || !r->cc || g >= r->cc->ngroups) return;
    CcInst *cc = r->cc;
    CkBehavior *beh = ck_behavior(ctx, cc->behavior);
    double now = phys_time(p->w);
    if (!begin) {                                 /* FUN_10001af0 */
        if (--cc->groups[g].count < 0) cc->groups[g].count = 0;
        if (cc->groups[g].count == 0 && cc->groups[g].on == 1) cc_add_timer(p, r->entity, g, now);
        return;
    }
    /* FUN_100018f0 */
    if (++cc->groups[g].count != 1) return;
    if (cc->groups[g].on == 0) {
        bool fired = false;
        for (uint32_t i = 0; i < p->ntimers; i++)  /* (the element after a removal is skipped) */
            if (p->timers[i].entity == r->entity && p->timers[i].group == g && cc->t_on < now - p->timers[i].time) {
                cc_remove_timer(p, i);
                cc->groups[g].on = 1;
                if (beh) ck_activate_output(ctx, beh, (uint32_t)g * 2, true);
                fired = true;
            }
        if (!fired) cc_add_timer(p, r->entity, g, now);
    } else {
        for (uint32_t i = 0; i < p->ntimers; i++)
            if (p->timers[i].entity == r->entity && p->timers[i].group == g) cc_remove_timer(p, i);
    }
}

/* friction contact created / deleted: the objects' listeners, the first object's then the second's */
static void on_contact(void *user, PhysBody *a, PhysBody *b, bool begin)
{
    CkContext *ctx = user;
    Physics *p = ctx->physics;
    if (a != p->dying) cc_event(ctx, p, a, b, begin);
    if (b != p->dying) cc_event(ctx, p, b, a, begin);
}

/* FUN_100017a0 */
static void cc_timers(CkContext *ctx)
{
    Physics *p = ctx->physics;
    double now = phys_time(p->w);
    for (uint32_t i = p->ntimers; i-- > 0;) {
        if (i >= p->ntimers) continue;
        CcTimer *t = &p->timers[i];
        Record *r = record(p, t->entity);
        CcInst *cc = r ? r->cc : NULL;
        if (!cc || t->group >= cc->ngroups) {
            cc_remove_timer(p, i);
            continue;
        }
        CkBehavior *beh = ck_behavior(ctx, cc->behavior);
        double dt = now - t->time;
        int32_t g = t->group;
        if (cc->groups[g].on == 1) {
            if (cc->groups[g].count == 0) {
                if (dt > cc->t_off) {
                    cc->groups[g].on = 0;
                    if (beh) ck_activate_output(ctx, beh, (uint32_t)g * 2 + 1, true);
                    cc_remove_timer(p, i);
                }
            } else {
                cc_remove_timer(p, i);
            }
        } else if (cc->groups[g].count > 0) {
            if (dt > cc->t_on) {
                cc->groups[g].on = 1;
                if (beh) ck_activate_output(ctx, beh, (uint32_t)g * 2, true);
                cc_remove_timer(p, i);
            }
        } else if (cc->t_on < dt * 0.5) {
            cc_remove_timer(p, i);
        }
    }
}

/* FUN_10001470: the install, once the target is physicalized (else nothing) */
static bool cc_install(CkContext *ctx, CkBehavior *b)
{
    Physics *p = physics(ctx);
    CkId t = bb_target(ctx, b);
    if (!t) return true;
    float t_on = 0.1f, t_off = 0.1f;
    int32_t ng = 5;
    bb_get_in(ctx, b, 0, &t_on, 4);
    bb_get_in(ctx, b, 1, &t_off, 4);
    bb_get_local(ctx, b, 0, &ng, 4);
    p->cc_groups = ng;                            /* mgr +0x54 +0: shared by every listener */
    Record *r = record(p, t);
    if (!r || r->cc) return true;
    CcInst *cc = calloc(1, sizeof *cc);
    cc->t_on = t_on, cc->t_off = t_off;
    cc->behavior = b->h.id;
    cc->entity = t;
    cc->ngroups = ng > 0 ? ng : 0;
    cc->groups = calloc((size_t)(cc->ngroups ? cc->ngroups : 1), sizeof *cc->groups);
    r->cc = cc;
    cmd_state(b)->cc = cc;
    return true;
}

static int bb_physics_continuous_contact(CkContext *ctx, CkBehavior *b)
{
    Physics *p = physics(ctx);
    if (ck_input_active(ctx, b, 0)) {
        if (!bb_target(ctx, b)) return 0xa004;   /* CKBR_PARAMETERERROR */
        if (!cmd_state(b)->cc) queue(ctx, b, cc_install);
        ck_activate_input(ctx, b, 0, false);
        return CKBR_ACTIVATENEXTFRAME;
    }
    if (!ck_input_active(ctx, b, 1)) return CKBR_ACTIVATENEXTFRAME;
    CmdState *s = cmd_state(b);
    CcInst *cc = s->cc;
    if (cc) {
        Record *r = record(p, cc->entity);
        if (r && r->cc == cc) cc_forget(p, cc->entity);   /* the listener goes, then FUN_100015d0 */
        s->cc = NULL;
        int32_t ng = p->cc_groups < cc->ngroups ? p->cc_groups : cc->ngroups;
        for (int32_t g = 0; g < ng; g++)
            if (cc->groups[g].on == 1) cc->groups[g].on = 0, ck_activate_output(ctx, b, (uint32_t)g * 2 + 1, true);
        free(cc->groups);
        free(cc);
    }
    ck_activate_input(ctx, b, 1, false);
    return CKBR_OK;
}

/* ---- DeleteCollisionSurfaces 53bf75aa:770c7021 (FUN_10001d30): frees the surface cache, unless something is
   still physicalized ---- */
static int bb_delete_collision_surfaces(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    Physics *p = physics(ctx);
    if (p->nrecs) {
        if (ctx->log) ctx->log("Please dephysicalize all objects");
        return CKBR_OK;
    }
    for (uint32_t i = 0; i < p->nsurfs; i++) free(p->surfs[i].name), phys_shape_free(p->surfs[i].shape);
    p->nsurfs = 0;
    return CKBR_OK;
}

BB_DECL(d_physicalize, 7522370e, 37ec15ec, "Physicalize", bb_physicalize);
BB_DECL_CB(d_set_physics_force, 56e20c57, 0b926068, "SetPhysicsForce", bb_set_physics_force, cb_handle);
BB_DECL_CB(d_set_physics_hinge, 41cd3653, 0de60c1d, "Set Physics Hinge", bb_set_physics_hinge, cb_handle);
BB_DECL_CB(d_set_physics_slider, 2973360e, 23d31aa7, "Set Physics Slider", bb_set_physics_slider, cb_handle);
BB_DECL_CB(d_set_physics_ball_joint, 5e624f0a, 35160450, "Set Physics Ball Joint", bb_set_physics_ball_joint, cb_handle);
BB_DECL_CB(d_set_physics_spring, 24a06a3a, 07100fce, "Set Physics Spring", bb_set_physics_spring, cb_handle);
BB_DECL(d_physics_wakeup, 38b851b5, 72ca74ac, "Physics WakeUp", bb_physics_wakeup);
BB_DECL(d_set_physics_globals, 72af347c, 03da71e1, "Set Physics Globals", bb_set_physics_globals);
BB_DECL(d_physics_impulse, 0c7e39bb, 16db20d5, "Physics Impulse", bb_physics_impulse);
/* ---- Get Profiler Values 1c8e61d1:32723c6f (FUN_10001fd0): Reset zeroes the manager's call counters and
   timers -> ResetOut; Start writes them out -> Out. The counters (Physicalize / DePhysicalize / HasPhysics
   calls and their QueryPerformanceCounter times) aren't kept here: the outputs stay 0. ---- */
static int bb_get_profiler_values(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

BB_DECL(d_get_profiler_values, 1c8e61d1, 32723c6f, "Get Profiler Values", bb_get_profiler_values);
BB_DECL(d_physics_coll_detection, 7435488d, 201d1188, "PhysicsCollDetection", bb_physics_coll_detection);
BB_DECL(d_physics_continuous_contact, 199e4cf1, 545a78fe, "PhysicsContinuousContact", bb_physics_continuous_contact);
BB_DECL(d_delete_collision_surfaces, 53bf75aa, 770c7021, "DeleteCollisionSurfaces", bb_delete_collision_surfaces);

const CkBBDecl *const bb_physics[] = {&d_get_profiler_values, 
    &d_physicalize, &d_set_physics_force, &d_set_physics_hinge, &d_set_physics_slider, &d_set_physics_ball_joint,
    &d_set_physics_spring, &d_physics_wakeup, &d_set_physics_globals, &d_physics_impulse, &d_physics_coll_detection,
    &d_physics_continuous_contact, &d_delete_collision_surfaces,
};
const unsigned bb_physics_count = sizeof bb_physics / sizeof *bb_physics;
