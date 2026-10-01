/* The physics API (phys.h) over the IVP core (phys_core.c, docs/ivp_core.md): the environment and its time
   manager, real objects, the glue force controller, impulses and the write-back. */
#include "phys_internal.h"
#include <stdlib.h>

/* ---- world: the environment ctor (0x10012a60) as the manager creates it (0x10006bb0) ---- */

PhysWorld *phys_world_create(void)
{
    PhysWorld *w = calloc(1, sizeof *w);
    w->time_factor = 0.001f;
    w->delta_psi = 1.0 / 66.0;                     /* 0x10013240: 0x3f8f07c1f07c1f08 */
    w->inv_delta_psi = 1.0 / w->delta_psi;
    w->current_time = 0;
    w->time_of_last_psi = 0;
    w->time_of_next_psi = w->delta_psi;
    w->time_code = 1;
    w->sleep_countdown = 10;
    w->state = 5;
    w->freeze_check_time = 0.3f;                   /* 0x10012a50 */
    /* the time manager (0x1002f0b0) queues the PSI event at 0: the first PSI runs at t = 0 */
    ivp_time_init(w);
    ivp_ov_init(&w->coll.ov);
    /* anomaly limits (0x1002f5e0) */
    w->max_velocity = 2000.0f;
    w->max_collisions_per_psi = 70000;
    w->max_angular_velocity_per_psi = 1.57079637f;
    w->gravity.ctrl.vt = &ivp_gravity_vt;
    v3set(w->gravity.g, 0, -9.81f, 0);             /* IVP's (0, 9.83, 0) replaced by the manager (0x10006bb0) */
    return w;
}

void phys_world_destroy(PhysWorld *w)
{
    if (!w) return;
    while (w->bodies) phys_body_destroy(w, w->bodies);
    while (w->joints) ivp_joint_free(w, w->joints);
    ivp_mm_free(w);
    ivp_ov_free(&w->coll.ov);
    ivp_minlist_free(&w->coll.tm.list);
    free(w->coll.check);
    free(w->revive);
    free(w);
}

/* 0x10011fe0 */
void phys_set_gravity(PhysWorld *w, const float g[3]) { v3cpy(w->gravity.g, g); }
void phys_set_time_factor(PhysWorld *w, float f) { w->time_factor = f * 0.001f; }
double phys_time(const PhysWorld *w) { return w->current_time; }
void phys_set_listeners(PhysWorld *w, PhysImpactFn impact, PhysContactFn contact, void *user)
{
    w->impact_fn = impact, w->contact_fn = contact, w->listen_user = user;
}

/* ---- shapes: the glue's collision surfaces (docs/ivp_collision.md 1.6.1) ---- */

PhysShape *phys_shape_sphere(float radius)
{
    PhysShape *s = calloc(1, sizeof *s);
    s->kind = PHYS_SPHERE;
    s->radius = radius;
    s->built = true;
    return s;
}

PhysShape *phys_shape_convex(void)
{
    PhysShape *s = calloc(1, sizeof *s);
    s->kind = PHYS_COMPOUND;
    return s;
}

static void add_ledge(PhysShape *s, IvpCompactLedge *l)
{
    if (!l) return;
    if (s->nledges == s->capledges) {
        s->capledges = s->capledges ? s->capledges * 2 : 16;
        s->ledges = realloc(s->ledges, s->capledges * sizeof *s->ledges);
    }
    s->ledges[s->nledges++] = l;
    s->built = false;
}

/* FUN_10007260: the convex mesh's points (scaled by the caller), exact duplicates removed, widened to f64, one
   hull ledge (or a triangle ledge for 3 points; nothing for fewer) */
void phys_shape_add_convex(PhysShape *s, const float *xyz, uint32_t n)
{
    double (*p)[3] = malloc((n ? n : 1) * sizeof *p);
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        bool dup = false;
        for (uint32_t k = 0; k < i && !dup; k++) dup = !memcmp(&xyz[k * 3], &xyz[i * 3], 12);
        if (dup) continue;
        for (int k = 0; k < 3; k++) p[m][k] = xyz[i * 3 + k];
        m++;
    }
    IvpCompactLedge *l = ivp_ledge_from_points((const double (*)[3])p, m);
    add_ledge(s, l);
    free(p);
}

/* FUN_10006fb0: one triangle ("pancake") ledge per face, degenerate faces skipped */
void phys_shape_add_triangles(PhysShape *s, const float *xyz, uint32_t nverts, const uint16_t *tri, uint32_t ntris)
{
    for (uint32_t i = 0; i < ntris; i++) {
        double p[3][3];
        bool ok = true;
        for (int v = 0; v < 3; v++) {
            if (tri[i * 3 + v] >= nverts) ok = false;
            else
                for (int k = 0; k < 3; k++) p[v][k] = xyz[tri[i * 3 + v] * 3 + k];
        }
        if (ok) add_ledge(s, ivp_ledge_triangle(p[0], p[1], p[2]));
    }
    s->ntri += ntris;
}

bool phys_shape_empty(const PhysShape *s) { return s->kind == PHYS_COMPOUND && !s->nledges && !s->cs; }

/* FUN_100384b0: the compact surface, compiled when the first body uses the (complete) surface */
static void shape_build(PhysShape *s)
{
    if (s->built || s->kind == PHYS_SPHERE) return;
    if (!s->cs) s->cs = ivp_surface_compile(s->ledges, s->nledges);   /* frees the input ledges */
    else
        for (uint32_t i = 0; i < s->nledges; i++) ivp_aligned_free(s->ledges[i]);
    s->nledges = 0;
    s->built = true;
}

void phys_shape_free(PhysShape *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < s->nledges; i++) ivp_aligned_free(s->ledges[i]);
    free(s->ledges);
    ivp_aligned_free(s->cs);
    free(s);
}

/* ---- bodies: the real object ctor (0x10009690; ball 0x10030010, polygon 0x1002fd00) ---- */

PhysBody *phys_body_create(PhysWorld *w, const PhysBodyDesc *d, PhysShape *shape, const float world[4][4], uint32_t entity)
{
    PhysBody *b = calloc(1, sizeof *b);
    b->world = w;
    b->entity = entity;
    b->shape = shape;
    b->fixed = d->fixed;
    b->collide = d->collide;
    b->friction = d->friction, b->elasticity = d->elasticity;
    memcpy(b->group, d->group, sizeof b->group);
    b->group[8] = 0;
    shape_build(shape);
    /* the pose: rotation from the world matrix (VxQuaternion::FromMatrix of the transposed 3x3), position =
       row 3 */
    double m[3][3], pos[3] = {world[3][0], world[3][1], world[3][2]};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) m[r][c] = world[c][r];
    IvpQuat q;
    ivp_matrix_to_quat(m, &q);
    ivp_core_init(w, &b->core, b, &q, pos, d->fixed);
    b->object_state = d->fixed ? IVP_MT_STATIC : IVP_MT_NOT_SIM;
    b->coll.type = shape->kind == PHYS_SPHERE ? IVP_OBJ_BALL : IVP_OBJ_POLYGON;
    ivp_hull_init(&b->coll.hull);
    /* the ball's extra radius is its radius */
    b->extra_radius = shape->kind == PHYS_SPHERE ? shape->radius : 0;
    ivp_object_init_core(w, b, d);
    b->next = w->bodies;
    if (w->bodies) w->bodies->prev = b;
    w->bodies = b;
    /* Enable Collision (0x10009350 -> 0x100176e0) */
    if (b->collide && (shape->kind == PHYS_SPHERE || shape->cs)) ivp_enable_collision_detection(w, b);
    /* Physicalize wakes the body unless Start Frozen (0x1000a460): revived at the next PSI */
    if (!d->fixed && !d->start_frozen) ivp_body_wake(w, b);
    return b;
}

void phys_body_destroy(PhysWorld *w, PhysBody *b)
{
    for (PhysForce *f = w->forces, *n; f; f = n) {
        n = f->next;
        if (f->body == b) phys_force_destroy(w, f);
    }
    for (PhysJoint *j = w->joints, *n; j; j = n) {
        n = j->next;
        if (j->a == b || j->b == b) ivp_joint_free(w, j);
    }
    phys_contacts_forget(w, b);                  /* 0x10009ab0 -> 0x10009a40(1) */
    ivp_disable_collision_detection(w, b);
    ivp_minlist_free(&b->coll.hull.list);
    ivp_core_remove(w, &b->core);
    free(b->core.fis);
    if (b->prev) b->prev->next = b->next;
    else w->bodies = b->next;
    if (b->next) b->next->prev = b->prev;
    free(b);
}

void phys_body_wake(PhysWorld *w, PhysBody *b) { ivp_body_wake(w, b); }
uint32_t phys_body_entity(const PhysBody *b) { return b->entity; }
bool phys_body_fixed(const PhysBody *b) { return b->core.unmovable; }

/* the velocity of a world point fixed to the body (at the pose of the last PSI) */
void phys_body_velocity_at(const PhysBody *b, const float p[3], float v[3])
{
    const IvpCore *c = &b->core;
    float r[3], pc[3];
    for (int k = 0; k < 3; k++) r[k] = (float)(p[k] - c->m_world_f_core.vv[k]);
    ivp_rtmul_f(c->m_world_f_core.r, r, pc);
    ivp_surface_speed(c, pc, v);
}

/* ---- the glue force controller (7.2, vtable 0x10063240) ---- */

/* 0x100046b0 */
static void force_simulate(IvpController *ctrl, const IvpEventSim *ev, IvpCore **cores, uint32_t n)
{
    if (n < 1) return;
    PhysForce *f = (PhysForce *)ctrl;
    IvpCore *c = &f->body->core;
    IvpMatrix m;
    ivp_core_m_world_f_core_at(c, ev->env->current_time, &m);
    float f_cs[3], p_cs[3] = {(float)f->point_cs[0], (float)f->point_cs[1], (float)f->point_cs[2]};
    ivp_rtmul_f(m.r, f->force_ws, f_cs);
    ivp_async_push_core(c, p_cs, f_cs, f->force_ws);
}

static const IvpControllerVt force_vt = {1500, force_simulate, false};

/* SetPhysicsForce Execute (0x10004930): the point is object-local (used as core coordinates, no Shift
   correction) when local, else a world point taken into core space at the current time */
PhysForce *phys_force_create(PhysWorld *w, PhysBody *b, const float force[3], const float point[3], bool local)
{
    PhysForce *f = calloc(1, sizeof *f);
    f->ctrl.vt = &force_vt;
    f->body = b;
    v3cpy(f->force_ws, force);
    if (local) {
        for (int k = 0; k < 3; k++) f->point_cs[k] = point[k];
    } else {
        IvpMatrix m;
        ivp_core_m_world_f_core_at(&b->core, w->current_time, &m);
        double d[3] = {point[0] - m.vv[0], point[1] - m.vv[1], point[2] - m.vv[2]};
        ivp_rtmul(m.r, d, f->point_cs);           /* 0x1000f3e0 */
    }
    ivp_core_add_controller(&b->core, &f->ctrl);    /* 0x10011b10: no merge, no wake */
    f->next = w->forces;
    w->forces = f;
    return f;
}

/* ignores handles no longer in the world */
void phys_force_destroy(PhysWorld *w, PhysForce *f)
{
    for (PhysForce **p = &w->forces; *p; p = &(*p)->next)
        if (*p == f) {
            *p = f->next;
            ivp_core_remove_controller(&f->body->core, &f->ctrl);
            free(f);
            return;
        }
}

/* Physics Impulse (13.1), after the glue's wake: body-local (the position used as core coordinates, the
   impulse rotated by the pose at the current time) or at a world point (0x1000a3e0). Committed at the next PSI. */
void phys_impulse(PhysWorld *w, PhysBody *b, const float impulse[3], const float point[3], bool local)
{
    if (local) {
        IvpMatrix m;
        ivp_core_m_world_f_core_at(&b->core, w->current_time, &m);
        float imp_cs[3];
        ivp_rtmul_f(m.r, impulse, imp_cs);
        ivp_async_push_core(&b->core, point, imp_cs, impulse);
    } else {
        double p[3] = {point[0], point[1], point[2]};
        ivp_body_async_push_ws(w, b, p, impulse);
    }
}

/* ---- the frame: PostProcess (FUN_10007ce0) and the time manager (11) ---- */

/* 0x10006ec0: the entity matrix of a body at the current time (0x10009d70), row vectors */
static void body_matrix(const PhysWorld *w, const PhysBody *b, float out[4][4])
{
    IvpMatrix m;
    ivp_body_m_world_f_object_at(b, w->current_time, &m);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) out[c][r] = (float)m.r[r][c];
        out[r][3] = 0;
    }
    for (int k = 0; k < 3; k++) out[3][k] = (float)m.vv[k];
    out[3][3] = 1;
}

void phys_frame(PhysWorld *w, float delta_ms, PhysWriteBack wb, void *user)
{
    w->smoothed_ms = (delta_ms + 3 * w->smoothed_ms) * 0.25f;
    double target = w->current_time + (double)(w->smoothed_ms * w->time_factor);   /* 0x100138c0 */
    /* 0x1002f250: the PSI events and the mindist events strictly before the target */
    ivp_time_simulate_until(w, target);
    if (!wb) return;
    /* the write-back set: the revived (simulated) objects */
    for (PhysBody *b = w->bodies; b; b = b->next) {
        if (b->object_state != IVP_MT_MOVING) continue;
        float m[4][4];
        body_matrix(w, b, m);
        wb(user, b->entity, m);
    }
}

bool phys_debug_body(const PhysWorld *w, uint32_t i, uint32_t *entity, float pos[3], bool *awake, bool *fixed, uint32_t *shape_kind,
                     uint32_t *ntris)
{
    const PhysBody *b = w->bodies;
    while (b && i--) b = b->next;
    if (!b) return false;
    IvpMatrix m;
    ivp_body_m_world_f_object_at(b, w->current_time, &m);
    *entity = b->entity;
    for (int k = 0; k < 3; k++) pos[k] = (float)m.vv[k];
    *awake = b->object_state == IVP_MT_MOVING, *fixed = b->fixed, *shape_kind = (uint32_t)b->shape->kind, *ntris = b->shape->ntri;
    return true;
}

bool phys_debug_core(const PhysWorld *w, uint32_t i, float speed[3], float rot_speed[3], int *movement_state)
{
    const PhysBody *b = w->bodies;
    while (b && i--) b = b->next;
    if (!b) return false;
    v3cpy(speed, b->core.speed);
    v3cpy(rot_speed, b->core.rot_speed);
    *movement_state = b->core.movement_state;
    return true;
}

void phys_debug_collision(const PhysWorld *w, uint32_t out[6])
{
    out[0] = w->coll.md_live, out[1] = w->coll.md_created, out[2] = w->coll.ov_rechecks, out[3] = w->coll.watcher_checks;
    out[4] = w->coll.impacts, out[5] = ivp_tree_split_guards;
}

