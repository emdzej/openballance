/* The IVP core and the per-PSI dynamics (docs/ivp_core.md): core creation, pose queries, pushes, damping,
   gravity, simulation units and controllers, the PSI step, integration, anomaly clamps, sleep / freeze /
   revive. */
#include "phys_internal.h"
#include <stdlib.h>

#define GROW(arr, n, cap)                                                   \
    do {                                                                    \
        if ((n) == (cap)) {                                                 \
            (cap) = (cap) ? (cap) * 2 : 4;                                  \
            (arr) = realloc((arr), (cap) * sizeof *(arr));                  \
        }                                                                   \
    } while (0)

/* 0x1002fcd0: the DLL-global LCG (seed 0x100685b4, initial 1, never reseeded, shared by all worlds) */
static uint32_t rng_seed = 1;
static float ivp_rand(void)
{
    rng_seed *= 75u;
    return (float)(rng_seed & 0xffff) * 1.52587890625e-05f;
}

/* ---- the sim-unit manager (10.0) ---- */

/* 0x10011ef0: push-front onto the list chosen by the state byte */
static void unit_list_add(PhysWorld *w, IvpSimUnit *su)
{
    IvpSimUnit **head = su->state < 8 ? &w->active : &w->frozen;
    su->prev = NULL;
    su->next = *head;
    if (*head) (*head)->prev = su;
    *head = su;
}

/* 0x10011f50 */
static void unit_list_remove(PhysWorld *w, IvpSimUnit *su)
{
    if (su->prev) su->prev->next = su->next;
    else if (w->active == su) w->active = su->next;
    else if (w->frozen == su) w->frozen = su->next;
    if (su->next) su->next->prev = su->prev;
    su->prev = su->next = NULL;
}

static void unit_set_state(PhysWorld *w, IvpSimUnit *su, int8_t state)
{
    unit_list_remove(w, su);
    su->state = state;
    unit_list_add(w, su);
}

/* 0x10011920 */
static IvpSimUnit *unit_new(void)
{
    IvpSimUnit *su = calloc(1, sizeof *su);
    su->state = IVP_MT_NOT_SIM;
    return su;
}

static void unit_free(IvpSimUnit *su)
{
    for (uint32_t i = 0; i < su->nentries; i++) free(su->entries[i].cores);
    free(su->entries);
    free(su->cores);
    free(su);
}

/* 0x100119d0 */
static void unit_add_core(IvpSimUnit *su, IvpCore *c)
{
    GROW(su->cores, su->ncores, su->capcores);
    su->cores[su->ncores++] = c;
    c->unit = su;
}

/* 0x10011db0: insertion sort to ascending priority, swapping only when strictly greater (equal priorities keep
   their insertion order) */
static void unit_sort(IvpSimUnit *su)
{
    for (uint32_t i = 1; i < su->nentries; i++)
        for (uint32_t j = i; j > 0 && su->entries[j - 1].c->vt->priority > su->entries[j].c->vt->priority; j--) {
            IvpCtrlEntry t = su->entries[j];
            su->entries[j] = su->entries[j - 1];
            su->entries[j - 1] = t;
        }
}

/* 0x10011cc0: find the entry (0x10011370) or append one (0x10011410), add the core (0x100113b0), re-sort */
static void unit_add_controller_core(IvpSimUnit *su, IvpController *ctrl, IvpCore *c)
{
    IvpCtrlEntry *e = NULL;
    for (uint32_t i = 0; i < su->nentries && !e; i++)
        if (su->entries[i].c == ctrl) e = &su->entries[i];
    if (!e) {
        GROW(su->entries, su->nentries, su->capentries);
        e = &su->entries[su->nentries++];
        memset(e, 0, sizeof *e);
        e->c = ctrl;
    }
    GROW(e->cores, e->n, e->cap);
    e->cores[e->n++] = c;
    unit_sort(su);
}

/* 0x10011d00: the core leaves the controller's entry; an empty entry is deleted */
static void unit_remove_controller_core(IvpSimUnit *su, IvpController *ctrl, IvpCore *c)
{
    for (uint32_t i = 0; i < su->nentries; i++) {
        IvpCtrlEntry *e = &su->entries[i];
        if (e->c != ctrl) continue;
        for (uint32_t k = 0; k < e->n; k++)
            if (e->cores[k] == c) {
                memmove(&e->cores[k], &e->cores[k + 1], (e->n - k - 1) * sizeof *e->cores);
                e->n--;
                break;
            }
        if (!e->n) {
            free(e->cores);
            memmove(e, e + 1, (su->nentries - i - 1) * sizeof *e);
            su->nentries--;
        }
        return;
    }
}

/* 0x10011c70 */
void ivp_core_add_controller(IvpCore *c, IvpController *ctrl)
{
    GROW(c->controllers, c->ncontrollers, c->capcontrollers);
    c->controllers[c->ncontrollers++] = ctrl;
    unit_add_controller_core(c->unit, ctrl, c);
}

/* 0x10011c00 */
void ivp_core_remove_controller(IvpCore *c, IvpController *ctrl)
{
    for (uint32_t i = 0; i < c->ncontrollers; i++)
        if (c->controllers[i] == ctrl) {
            memmove(&c->controllers[i], &c->controllers[i + 1], (c->ncontrollers - i - 1) * sizeof *c->controllers);
            c->ncontrollers--;
            unit_remove_controller_core(c->unit, ctrl, c);
            return;
        }
}

/* 0x10011720: the cores of `from` (with their controllers) join `into`; `from` is deleted */
void ivp_sim_unit_merge(PhysWorld *w, IvpSimUnit *into, IvpSimUnit *from)
{
    if (into == from) return;
    for (uint32_t i = 0; i < from->ncores; i++) {
        IvpCore *c = from->cores[i];
        unit_add_core(into, c);
        for (uint32_t k = 0; k < c->ncontrollers; k++) unit_add_controller_core(into, c->controllers[k], c);
    }
    unit_list_remove(w, from);
    unit_free(from);
}

static void revive_sim_unit(PhysWorld *w, IvpSimUnit *su);

/* 0x10011b30: merge the units of the controller's (movable) cores, attach it to each core, and revive the
   unit when any of them is simulated (the state bytes ANDed starting from 8) */
void ivp_controller_attach_merge(PhysWorld *w, IvpController *ctrl, IvpCore **cores, uint32_t n)
{
    if (!n) return;
    IvpSimUnit *into = cores[0]->unit;
    for (uint32_t i = 1; i < n; i++)
        if (cores[i]->unit != into) ivp_sim_unit_merge(w, into, cores[i]->unit);
    int acc = 8;
    for (uint32_t i = 0; i < n; i++) {
        ivp_core_add_controller(cores[i], ctrl);
        acc &= (uint8_t)cores[i]->movement_state;
    }
    if (acc < 8) revive_sim_unit(w, into);
}

/* 0x10011a60: detach; the unit is marked changed (0x100) */
void ivp_controller_detach(PhysWorld *w, IvpController *ctrl, IvpCore **cores, uint32_t n)
{
    (void)w;
    for (uint32_t i = 0; i < n; i++) {
        ivp_core_remove_controller(cores[i], ctrl);
        cores[i]->unit->changed = true;
    }
}

/* ---- core creation (2) ---- */

/* 0x1000cc70 (2.8) */
static void init_core_for_simulation(IvpCore *c, double i_delta_time)
{
    c->i_delta_time = (float)i_delta_time;
    c->q_next = c->q_last;
    c->abs_omega = 0;
    v3set(c->speed, 0, 0, 0);
    v3set(c->delta_pos, 0, 0, 0);
    c->current_speed = 0;
    c->max_surface_rot_speed = 0;
    v3set(c->rotation_axis_world, 1, 0, 0);
}

static double inv_dt(double dt) { return dt > 1e-10 ? 1.0 / dt : 9999999866.485682; }

/* 0x1000d400 / 0x1000d2f0 (2.2): the core at the object origin, in a new frozen sim unit, attached to gravity */
void ivp_core_init(PhysWorld *w, IvpCore *c, PhysBody *b, const IvpQuat *q, const double pos[3], bool unmovable)
{
    memset(c, 0, sizeof *c);
    c->body = b;
    IvpSimUnit *su = unit_new();
    unit_add_core(su, c);
    su->state = IVP_MT_NOT_SIM;
    ivp_core_add_controller(c, &w->gravity.ctrl);
    c->movement_state = IVP_MT_NOT_SIM;
    c->unmovable = unmovable;
    unit_list_add(w, su);
    c->q_next = *q;
    ivp_quat_normalize(&c->q_next);
    c->q_last = c->q_next;
    memcpy(c->pos_last, pos, sizeof c->pos_last);
    ivp_quat_to_matrix(&c->q_last, c->m_world_f_core.r);
    memcpy(c->m_world_f_core.vv, pos, sizeof c->m_world_f_core.vv);
    c->upper_limit_radius = 1000.0f;
    c->max_surface_deviation = 1000.0f;
}

/* 0x1000d1a0 (2.6) */
static void calc_calc(IvpCore *c)
{
    for (int i = 0; i < 3; i++) c->inv_rot_inertia[i] = (float)(1.0f / c->rot_inertia[i]);
    c->inv_mass = 1.0f / c->mass;
    const float *a = c->inv_rot_inertia, *I = c->rot_inertia;
    double d = ((double)a[1] - a[2]) * ((double)a[1] - a[2]) + ((double)a[0] - a[1]) * ((double)a[0] - a[1]) +
               ((double)a[2] - a[0]) * ((double)a[2] - a[0]);
    /* quirk: the spread of the inverse inertias against the squared inertias */
    c->rot_inertias_equal = d < ((double)I[2] * I[2] + (double)I[1] * I[1] + (double)I[0] * I[0]) * (double)0.01f;
    c->inv_object_diameter = 0.5f / c->upper_limit_radius;
}

/* the surface manager's virtuals used at creation (vtable +4, +8, +0xc; docs/ivp_collision.md 1.3.1, 1.6.7):
   the compact surface's mass centre, radius and deviation about the given centre, unit inertia */
static void surface_mass_center(const PhysShape *s, float mc[3])
{
    if (s->kind == PHYS_SPHERE) v3set(mc, 0, 0, 0);     /* 0x1002fe60 */
    else v3cpy(mc, s->cs->mass_center);                 /* 0x1000bc00 */
}

/* 0x100093d0 (2.5) for an identity rotation: the object origin in core coordinates (q_core_f_object stays
   NULL: the override's rotation is ignored, so it is always identity in Ballance) */
static void set_new_m_object_f_core(PhysBody *b, const IvpMatrix *m_object_f_core)
{
    IvpMatrix m_core_f_object;
    ivp_mat_inverse(&m_core_f_object, m_object_f_core);
    const double *v = m_core_f_object.vv;
    if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 1e-16) {
        b->shift_is_zero = true;
        v3set(b->shift_core_f_object, 0, 0, 0);
    } else {
        b->shift_is_zero = false;
        v3set(b->shift_core_f_object, (float)v[0], (float)v[1], (float)v[2]);
    }
}

/* 0x1000d270 (2.7) */
static void transform_to_mass_center(IvpCore *c, const IvpMatrix *m_object_f_core)
{
    ivp_mat_mul(&c->m_world_f_core, &c->m_world_f_core, m_object_f_core);
    memcpy(c->pos_last, c->m_world_f_core.vv, sizeof c->pos_last);
    ivp_matrix_to_quat(c->m_world_f_core.r, &c->q_last);
    c->q_next = c->q_last;
}

/* 0x10009f00 (2.4): mass centre, radius, mass, inertia, damping. The glue passes the template of 0x10007460:
   mass, unit inertia factors (1, 1, 1), auto check 0.03, the damping, the Shift override when not automatic. */
void ivp_object_init_core(PhysWorld *w, PhysBody *b, const PhysBodyDesc *d)
{
    const PhysShape *s = b->shape;
    float mc[3];
    if (d->has_shift) v3cpy(mc, d->shift);       /* only the override's translation */
    else surface_mass_center(s, mc);              /* vtable +4 */
    IvpMatrix m_object_f_core;
    ivp_mat_identity(&m_object_f_core);
    for (int k = 0; k < 3; k++) m_object_f_core.vv[k] = mc[k];
    set_new_m_object_f_core(b, &m_object_f_core);

    IvpCore *c = &b->core;
    float radius = 0, radius_dev = 0;
    if (s->kind != PHYS_SPHERE) ivp_surface_radius_dev(s->cs, mc, &radius, &radius_dev);   /* vtable +8 (0x1000bc40) */
    radius = (float)((double)b->extra_radius + radius);
    c->upper_limit_radius = radius;          /* 0x1000c4f0 */
    c->max_surface_deviation = radius_dev;

    double mass = (double)d->mass;
    if (mass < 1e-8) mass = 1.0;
    float I[3];
    if (s->kind == PHYS_SPHERE) {
        float i = (float)(radius * radius * 0.4f);
        v3set(I, i, i, i);
    } else {
        v3cpy(I, s->cs->rotation_inertia);   /* vtable +0xc (0x1000bc20) */
    }
    for (int i = 0; i < 3; i++) c->rot_inertia[i] = (float)((double)I[i] * 1.0f * mass);
    /* auto_check_rot_inertia 0.03f: no axis below 0.03 |I| */
    double m = (double)v3len(c->rot_inertia) * 0.03f;
    for (int i = 0; i < 3; i++)
        if (c->rot_inertia[i] < m) c->rot_inertia[i] = (float)m;
    c->mass = (float)mass;
    c->speed_damp = (float)(double)d->lin_damp;
    for (int i = 0; i < 3; i++) c->rot_speed_damp[i] = (float)(double)d->rot_damp;
    v3set(c->rot_speed, 0, 0, 0);
    v3set(c->speed, 0, 0, 0);
    calc_calc(c);
    transform_to_mass_center(c, &m_object_f_core);
    init_core_for_simulation(c, inv_dt(w->time_of_next_psi - w->current_time));
}

/* ---- pose queries (4) ---- */

/* 0x1000c510 */
void ivp_core_m_world_f_core_at(const IvpCore *c, double t, IvpMatrix *out)
{
    double dt = t - c->time_of_last_psi;
    if (c->movement_state < 8 && dt != 0.0) {
        IvpQuat q;
        ivp_quat_slerp(&q, &c->q_last, &c->q_next, (double)c->i_delta_time * dt);
        ivp_quat_to_matrix(&q, out->r);
        for (int k = 0; k < 3; k++) out->vv[k] = c->pos_last[k] + (double)c->delta_pos[k] * dt;
    } else {
        *out = c->m_world_f_core;
    }
}

/* 0x10009d70 (no movement-state test) */
void ivp_body_m_world_f_object_at(const PhysBody *b, double t, IvpMatrix *out)
{
    const IvpCore *c = &b->core;
    double dt = t - c->time_of_last_psi;
    IvpQuat q;
    ivp_quat_slerp(&q, &c->q_last, &c->q_next, dt * (double)c->i_delta_time);
    for (int k = 0; k < 3; k++) out->vv[k] = c->pos_last[k] + (double)c->delta_pos[k] * dt;
    ivp_quat_to_matrix(&q, out->r);
    if (!b->shift_is_zero) {                 /* 0x1000f5f0 */
        double s[3] = {b->shift_core_f_object[0], b->shift_core_f_object[1], b->shift_core_f_object[2]};
        ivp_rmul(out->r, s, s);
        for (int k = 0; k < 3; k++) out->vv[k] += s[k];
    }
}

/* 0x10012470: the pose at this PSI */
void ivp_core_sync_psi_pose(IvpCore *c, double t)
{
    double dt = t - c->time_of_last_psi;
    ivp_quat_to_matrix(&c->q_next, c->m_world_f_core.r);
    for (int k = 0; k < 3; k++) c->m_world_f_core.vv[k] = c->pos_last[k] + (double)c->delta_pos[k] * dt;
}

/* ---- pushes (5), all f32 ---- */

void ivp_calc_push_core(const IvpCore *c, const float p_cs[3], const float imp_cs[3], const float imp_ws[3], float dv[3],
                        float dw[3])
{
    float t[3];
    v3cross(t, p_cs, imp_cs);
    for (int i = 0; i < 3; i++) dw[i] = c->inv_rot_inertia[i] * t[i];
    v3scale(dv, imp_ws, c->inv_mass);
}

void ivp_async_push_core(IvpCore *c, const float p_cs[3], const float imp_cs[3], const float imp_ws[3])
{
    float dv[3], dw[3];
    ivp_calc_push_core(c, p_cs, imp_cs, imp_ws, dv, dw);
    v3add(c->rot_speed_change, c->rot_speed_change, dw);
    v3add(c->speed_change, c->speed_change, dv);
}

void ivp_push_core(IvpCore *c, const float p_cs[3], const float imp_cs[3], const float imp_ws[3])
{
    float dv[3], dw[3];
    ivp_calc_push_core(c, p_cs, imp_cs, imp_ws, dv, dw);
    v3add(c->rot_speed, c->rot_speed, dw);
    v3add(c->speed, c->speed, dv);
}

void ivp_async_push_core_ws(IvpCore *c, const double p_ws[3], const float imp_ws[3])
{
    float r[3], t[3], tc[3];
    for (int k = 0; k < 3; k++) r[k] = (float)(p_ws[k] - c->m_world_f_core.vv[k]);
    v3cross(t, r, imp_ws);
    ivp_rtmul_f(c->m_world_f_core.r, t, tc);
    for (int i = 0; i < 3; i++) c->rot_speed_change[i] += c->inv_rot_inertia[i] * tc[i];
    v3mad(c->speed_change, c->speed_change, imp_ws, c->inv_mass);
}

void ivp_rot_push_core_cs(IvpCore *c, const float t_cs[3])
{
    for (int i = 0; i < 3; i++) c->rot_speed[i] += c->inv_rot_inertia[i] * t_cs[i];
}

void ivp_surface_speed(const IvpCore *c, const float p_cs[3], float out[3])
{
    float t[3], tw[3];
    v3cross(t, c->rot_speed, p_cs);
    ivp_rmul_f(c->m_world_f_core.r, t, tw);       /* 0x1000bf90 */
    v3add(out, c->speed, tw);
}

void ivp_commit_pushes(IvpCore *c)
{
    v3add(c->rot_speed, c->rot_speed, c->rot_speed_change);
    v3add(c->speed, c->speed, c->speed_change);
    v3set(c->rot_speed_change, 0, 0, 0);
    v3set(c->speed_change, 0, 0, 0);
}

/* 0x1000a3e0 */
void ivp_body_async_push_ws(PhysWorld *w, PhysBody *b, const double p_ws[3], const float imp_ws[3])
{
    ivp_body_wake(w, b);
    IvpCore *c = &b->core;
    IvpMatrix m;
    ivp_core_m_world_f_core_at(c, w->current_time, &m);
    double d[3] = {p_ws[0] - m.vv[0], p_ws[1] - m.vv[1], p_ws[2] - m.vv[2]};
    ivp_rtmul(m.r, d, d);
    float p_cs[3] = {(float)d[0], (float)d[1], (float)d[2]}, imp_cs[3];
    ivp_rtmul_f(m.r, imp_ws, imp_cs);
    ivp_async_push_core(c, p_cs, imp_cs, imp_ws);
}

/* ---- damping and gravity (7.1, 7.3) ---- */

/* 0x1000c6a0 */
static void apply_damping(IvpCore *c, double dt, const float rf[3], double lf)
{
    double a[3] = {rf[0] * dt, (float)(rf[1] * dt), (float)(rf[2] * dt)};
    float k[3];
    if (a[0] * a[0] + a[1] * a[1] + a[2] * a[2] < 0.5)
        for (int i = 0; i < 3; i++) k[i] = (float)(1.0f - a[i]);
    else
        for (int i = 0; i < 3; i++) k[i] = (float)exp(-a[i]);
    double b = dt * lf, kl = b < 0.25 ? 1.0 - b : exp(-b);
    for (int i = 0; i < 3; i++) {
        c->rot_speed[i] *= k[i];
        c->speed[i] = (float)(c->speed[i] * kl);
    }
}

/* 0x1000c610: +0.1 on every factor when slow or calm */
static void damp_object(IvpCore *c, double dt)
{
    if (c->movement_state >= IVP_MT_SLOW) {
        float rf[3];
        for (int i = 0; i < 3; i++) rf[i] = (float)(c->rot_speed_damp[i] + 0.1f);
        apply_damping(c, dt, rf, (double)c->speed_damp + (double)0.1f);
    } else {
        apply_damping(c, dt, c->rot_speed_damp, c->speed_damp);
    }
}

/* 0x10012010 (vtable 0x10063598, priority 1000): damp the velocities of the previous PSI, commit the pushes
   (undamped), add g dt */
static void gravity_simulate(IvpController *ctrl, const IvpEventSim *ev, IvpCore **cores, uint32_t n)
{
    const float *g = ((const IvpGravity *)ctrl)->g;
    for (uint32_t i = n; i-- > 0;) {
        IvpCore *c = cores[i];
        damp_object(c, ev->delta_time);
        ivp_commit_pushes(c);
        for (int k = 0; k < 3; k++) c->speed[k] = (float)(c->speed[k] + g[k] * ev->delta_time);
    }
}

const IvpControllerVt ivp_gravity_vt = {1000, gravity_simulate, false};

/* ---- sleep, freeze, revive (10) ---- */

/* 0x10013610 */
static bool sleep_countdown(PhysWorld *w)
{
    if (--w->sleep_countdown != 0) return false;
    float r = ivp_rand();
    w->sleep_countdown = (uint16_t)(15 - (int)(r * -5.0f));
    return true;
}

/* 0x1000cec0: restart the timers, the references are kept */
static void reset_freeze_check_values(PhysWorld *w, IvpCore *c) { c->time_calm_ref[0] = c->time_calm_ref[1] = w->current_time; }
void ivp_reset_freeze_check(PhysWorld *w, IvpCore *c) { reset_freeze_check_values(w, c); }

static double qdot_f(const float a[4], const IvpQuat *b) { return a[0] * b->x + a[1] * b->y + a[2] * b->z + a[3] * b->w; }
static void qstore_f(float d[4], const IvpQuat *q) { d[0] = (float)q->x, d[1] = (float)q->y, d[2] = (float)q->z, d[3] = (float)q->w; }

/* 0x1000d680 */
static int calc_movement_state(PhysWorld *w, IvpCore *c, double t)
{
    double R = c->upper_limit_radius, d0[3], d1[3];
    for (int k = 0; k < 3; k++) d0[k] = c->pos_last[k] - c->pos_calm_ref[0][k];
    if (d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2] <= 9.999999552965169e-05) {
        double dot = qdot_f(c->q_calm_ref[0], &c->q_next);
        if ((1.0 - dot * dot) * 2 * R * R <= 2.4999998882412923e-05)
            return t - c->time_calm_ref[0] > (double)w->freeze_check_time ? IVP_MT_CALM : IVP_MT_SLOW;
    }
    /* the short window moved: restart it */
    qstore_f(c->q_calm_ref[0], &c->q_next);
    for (int k = 0; k < 3; k++) c->pos_calm_ref[0][k] = (float)c->pos_last[k];
    c->time_calm_ref[0] = t;
    for (int k = 0; k < 3; k++) d1[k] = c->pos_last[k] - c->pos_calm_ref[1][k];
    double dot = qdot_f(c->q_calm_ref[1], &c->q_last);
    if (d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2] > 0.010000000298023226 ||
        (1.0 - dot * dot) * 2 * R * R > 0.040000001192092904) {
        qstore_f(c->q_calm_ref[1], &c->q_last);
        for (int k = 0; k < 3; k++) c->pos_calm_ref[1][k] = (float)c->pos_last[k];
        c->time_calm_ref[1] = t;
        return IVP_MT_MOVING;
    }
    return t - c->time_calm_ref[1] > 4.0 ? IVP_MT_CALM : IVP_MT_MOVING;
}

/* 0x1000afe0 = 0x1000ce20 (stop_physical_movement 0x1000cd80) + 0x1000af90 (listeners: the glue's write-back
   set follows object_state) */
static void freeze_core(PhysWorld *w, IvpCore *c)
{
    c->movement_state = IVP_MT_NOT_SIM;
    v3set(c->speed, 0, 0, 0), v3set(c->rot_speed, 0, 0, 0);
    v3set(c->speed_change, 0, 0, 0), v3set(c->rot_speed_change, 0, 0, 0);
    v3set(c->delta_pos, 0, 0, 0);
    init_core_for_simulation(c, w->inv_delta_psi);
    c->body->object_state = IVP_MT_NOT_SIM;
    phys_collision_object_frozen(w, c->body);     /* 0x10017140, the hull frozen, the cache dropped */
}

/* 0x100120f0 */
static bool try_to_freeze(PhysWorld *w, IvpSimUnit *su)
{
    double t = w->current_time;
    int acc = 3;
    for (uint32_t i = su->ncores; i-- > 0;) {
        IvpCore *c = su->cores[i];
        int s = calc_movement_state(w, c, t);
        c->movement_state = (int8_t)s;          /* kept even if the unit stays awake (damping reads it) */
        acc &= s;
    }
    if (acc != 3) return false;
    for (uint32_t i = su->ncores; i-- > 0;) freeze_core(w, su->cores[i]);
    unit_set_state(w, su, IVP_MT_NOT_SIM);
    return true;
}

/* 0x1000aea0 (with 0x1000cf20) */
static int revive_core(PhysWorld *w, IvpCore *c)
{
    c->body->object_state = IVP_MT_MOVING;
    c->movement_state = IVP_MT_MOVING;
    reset_freeze_check_values(w, c);
    c->time_of_last_psi = w->current_time;
    phys_collision_object_revived(w, c->body);   /* 0x100099f0 */
    init_core_for_simulation(c, inv_dt(w->time_of_next_psi - w->current_time));
    return phys_contact_revive_core(w, c);   /* 0x1001d4d0 */
}

/* 0x10011f80 */
static void revive_sim_unit(PhysWorld *w, IvpSimUnit *su)
{
restart:
    for (uint32_t i = su->ncores; i-- > 0;) {
        IvpCore *c = su->cores[i];
        if (c->movement_state < 8) continue;
        if (revive_core(w, c) == 1) goto restart;
    }
    if (su->state == IVP_MT_NOT_SIM) unit_set_state(w, su, IVP_MT_MOVING);
}

/* 0x1000dab0 */
void ivp_ensure_in_simulation(PhysWorld *w, IvpCore *c)
{
    if (c->unmovable) return;
    if (c->movement_state != IVP_MT_NOT_SIM)
        for (uint32_t i = 0; i < c->unit->ncores; i++) reset_freeze_check_values(w, c->unit->cores[i]);   /* 0x100120b0 */
    else
        revive_sim_unit(w, c->unit);
}

/* 0x1000b540 */
static void enqueue_revive(PhysWorld *w, IvpCore *c)
{
    if (c->in_revive_list) return;
    GROW(w->revive, w->nrevive, w->caprevive);
    w->revive[w->nrevive++] = c;
    c->in_revive_list = true;
}

/* 0x1000b590 */
static void revive_cores_psi(PhysWorld *w)
{
    for (uint32_t i = w->nrevive; i-- > 0;) {
        IvpCore *c = w->revive[i];
        ivp_ensure_in_simulation(w, c);
        c->in_revive_list = false;
    }
    w->nrevive = 0;
}

/* 0x1000a460: an awake body restarts its timers, a frozen one is revived at the start of the next PSI */
void ivp_body_wake(PhysWorld *w, PhysBody *b)
{
    if (b->object_state != IVP_MT_NOT_SIM) reset_freeze_check_values(w, &b->core);
    else enqueue_revive(w, &b->core);
}

/* 0x10009670: immediate revive (collision code) */
void ivp_body_revive_now(PhysWorld *w, PhysBody *b)
{
    if (b->object_state == IVP_MT_NOT_SIM && !b->core.unmovable) revive_sim_unit(w, b->core.unit);
}

/* the body is going away: out of the revive list, its unit and its global controllers */
void ivp_core_remove(PhysWorld *w, IvpCore *c)
{
    for (uint32_t i = 0; i < w->nrevive; i++)
        if (w->revive[i] == c) {
            memmove(&w->revive[i], &w->revive[i + 1], (w->nrevive - i - 1) * sizeof *w->revive);
            w->nrevive--;
            break;
        }
    while (c->ncontrollers) ivp_core_remove_controller(c, c->controllers[c->ncontrollers - 1]);
    free(c->controllers);
    c->controllers = NULL;
    IvpSimUnit *su = c->unit;
    for (uint32_t i = 0; i < su->ncores; i++)
        if (su->cores[i] == c) {
            memmove(&su->cores[i], &su->cores[i + 1], (su->ncores - i - 1) * sizeof *su->cores);
            su->ncores--;
            break;
        }
    if (!su->ncores) {
        unit_list_remove(w, su);
        unit_free(su);
    } else {
        su->changed = true;
    }
    c->unit = NULL;
}

/* ---- the unit split (0x100120d0): the connected components of the cores, linked by the controllers that
   associate their cores (get_associated_controlled_cores: springs, constraints, the friction system's
   priority-600 controller) ---- */
static bool cores_linked(const IvpSimUnit *su, const IvpCore *a, const IvpCore *b)
{
    for (uint32_t i = 0; i < su->nentries; i++) {
        const IvpCtrlEntry *e = &su->entries[i];
        if (!e->c->vt->links_cores) continue;
        bool ha = false, hb = false;
        for (uint32_t k = 0; k < e->n; k++) ha |= e->cores[k] == a, hb |= e->cores[k] == b;
        if (ha && hb) return true;
    }
    return false;
}

static void split_or_merge(PhysWorld *w, IvpSimUnit *su)
{
    su->changed = false;
    uint32_t n = su->ncores;
    if (n < 2) return;
    int32_t *comp = malloc(n * sizeof *comp);
    for (uint32_t i = 0; i < n; i++) comp[i] = -1;
    int32_t ncomp = 0;
    uint32_t *stack = malloc(n * sizeof *stack);
    for (uint32_t s = 0; s < n; s++) {
        if (comp[s] >= 0) continue;
        uint32_t sp = 0;
        stack[sp++] = s;
        comp[s] = ncomp;
        while (sp) {
            uint32_t i = stack[--sp];
            for (uint32_t k = 0; k < n; k++)
                if (comp[k] < 0 && cores_linked(su, su->cores[i], su->cores[k])) comp[k] = ncomp, stack[sp++] = k;
        }
        ncomp++;
    }
    free(stack);
    if (ncomp > 1) {
        IvpCore **cores = malloc(n * sizeof *cores);
        memcpy(cores, su->cores, n * sizeof *cores);
        IvpSimUnit **units = calloc((size_t)ncomp, sizeof *units);
        units[0] = su;
        for (uint32_t i = 0; i < su->nentries; i++) free(su->entries[i].cores);
        su->nentries = 0;
        su->ncores = 0;
        for (int32_t k = 1; k < ncomp; k++) {
            units[k] = unit_new();
            units[k]->state = su->state;
            units[k]->fast = su->fast, units[k]->fast_prev = su->fast_prev;
            unit_list_add(w, units[k]);
        }
        for (uint32_t i = 0; i < n; i++) {
            IvpSimUnit *u = units[comp[i]];
            unit_add_core(u, cores[i]);
            for (uint32_t k = 0; k < cores[i]->ncontrollers; k++) unit_add_controller_core(u, cores[i]->controllers[k], cores[i]);
        }
        free(units);
        free(cores);
    }
    free(comp);
}

/* ---- the PSI (6, 8) ---- */

typedef struct {
    IvpCore **v;
    uint32_t n, cap;
} CoreVec;

/* 0x100121b0 */
static void sim_unit_psi(PhysWorld *w, IvpSimUnit *su, IvpEventSim *ev, CoreVec *out)
{
    ev->sim_unit = su;
    double t = w->current_time;
    bool fast = false;
    for (uint32_t i = su->ncores; i-- > 0;) {
        IvpCore *c = su->cores[i];
        ivp_core_sync_psi_pose(c, t);
        ivp_commit_pushes(c);               /* pushes made between PSIs land here */
        c->temporarily_unmovable = 0;       /* byte +0x61 */
        c->impacts_psi = 0;                 /* u16 +0x64 */
        double v2 = (double)c->speed[0] * c->speed[0] + (double)c->speed[1] * c->speed[1] + (double)c->speed[2] * c->speed[2];
        if ((float)(1.0 - v2) < 0) fast = true;   /* |speed| > 1 m/s */
    }
    bool test_sleep;
    if (fast) {
        su->fast = true;
        test_sleep = false;
    } else {
        su->fast_prev = su->fast;
        if (su->fast_prev)
            for (uint32_t i = su->ncores; i-- > 0;) reset_freeze_check_values(w, su->cores[i]);
        su->fast = false;
        test_sleep = sleep_countdown(w);
    }
    /* controllers by descending priority, among equal priorities the most recently added first */
    for (uint32_t i = su->nentries; i-- > 0;) {
        IvpCtrlEntry *e = &su->entries[i];
        e->c->vt->simulate(e->c, ev, e->cores, e->n);
    }
    for (uint32_t i = su->ncores; i-- > 0;) {
        GROW(out->v, out->n, out->cap);
        out->v[out->n++] = su->cores[i];
    }
    if (test_sleep) try_to_freeze(w, su);
    phys_collision_after_controllers(w, su);   /* 0x100099a0 per object */
    if (su->changed) split_or_merge(w, su);
}

/* 0x1002f6d0, 0x1002f720: the default anomaly manager */
void ivp_max_velocity_exceeded(PhysWorld *w, float v[3])
{
    double k = (w->max_velocity * 0.99000000953674) / sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
    for (int i = 0; i < 3; i++) v[i] = (float)(v[i] * k);
}

void ivp_max_angular_velocity_exceeded(PhysWorld *w, float v[3])
{
    double k = (w->inv_delta_psi * w->max_angular_velocity_per_psi * 0.8999999761581) /
               sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
    for (int i = 0; i < 3; i++) v[i] = (float)(v[i] * k);
}

/* 0x1001e870 (8.3) */
static void calc_rotation_info(IvpCore *c, const IvpQuat *Q)
{
    double n = Q->x * Q->x + Q->y * Q->y + Q->z * Q->z;
    if (n <= 1e-19) {
        c->abs_omega = 0;
        v3set(c->rotation_axis_world, 1, 0, 0);
    } else {
        double y = ivp_rsqrt(n), s = y * n;
        double as = s + s * s * s * (double)(1.0f / 6.0f) + s * s * s * s * s * (double)0.40413999557495117f;
        c->abs_omega = (float)(2 * as * (double)c->i_delta_time);
        double ax[3] = {Q->x * y, Q->y * y, Q->z * y};
        ivp_rmul(c->m_world_f_core.r, ax, ax);
        v3set(c->rotation_axis_world, (float)ax[0], (float)ax[1], (float)ax[2]);
    }
    c->max_surface_rot_speed = c->abs_omega * c->max_surface_deviation;
}

/* 0x1001e300 (8.2): clamps, the rotation of this PSI, the move to the next PSI, the hull update */
void ivp_calc_next_psi_matrix(PhysWorld *w, IvpCore *c, const IvpEventSim *ev)
{
    float *rs = c->rot_speed;
    /* angular clamp (max_rot_speed, +0x4c, is never set in Ballance) */
    double a = w->inv_delta_psi * w->max_angular_velocity_per_psi;
    if ((double)rs[0] * rs[0] + (double)rs[1] * rs[1] + (double)rs[2] * rs[2] > a * a) ivp_max_angular_velocity_exceeded(w, rs);
    double v2 = (double)c->speed[0] * c->speed[0] + (double)c->speed[1] * c->speed[1] + (double)c->speed[2] * c->speed[2];
    if (v2 > (double)w->max_velocity * w->max_velocity) {
        ivp_max_velocity_exceeded(w, c->speed);
        v2 = (double)c->speed[0] * c->speed[0] + (double)c->speed[1] * c->speed[1] + (double)c->speed[2] * c->speed[2];
    }
    c->current_speed = (float)sqrt(v2);
    double dt = ev->delta_time;
    c->i_delta_time = (float)ev->i_delta_time;
    IvpQuat Q;
    if (!c->rot_inertias_equal && w->state != 5) {
        /* unequal inertias: gyroscopic sub-steps of Euler's equations without torques */
        int n = 1;
        double x = ((double)rs[0] * rs[0] + (double)rs[1] * rs[1] + (double)rs[2] * rs[2]) * dt * dt;
        if (x > 1.0 / 36.0) {
            n = (int)sqrt(x * 144.0) + 1;
            dt = dt / n;
        }
        ivp_quat_from_rot_poly(&Q, rs, dt);
        const float *I = c->rot_inertia, *iI = c->inv_rot_inertia;
        double e[3] = {((double)I[1] - I[2]) * iI[0], ((double)I[2] - I[0]) * iI[1], ((double)I[0] - I[1]) * iI[2]};
        for (int i = 1;; i++) {
            float g[3] = {(float)(rs[2] * (double)rs[1] * e[0]), (float)(rs[2] * (double)rs[0] * e[1]), (float)(rs[0] * (double)rs[1] * e[2])};
            float fdt = (float)dt;
            for (int k = 0; k < 3; k++) rs[k] = rs[k] + g[k] * fdt;    /* 0x10010290 */
            if (i >= n) break;
            IvpQuat s;
            ivp_quat_from_rot_poly(&s, rs, dt);
            ivp_quat_mul(&Q, &s, &Q);            /* the new sub-step on the left */
        }
    } else {
        ivp_quat_from_rot_sin(&Q, rs, dt);   /* balls, equal inertia */
    }
    double dtp = w->current_time - c->time_of_last_psi;
    c->time_of_last_psi = w->current_time;
    for (int k = 0; k < 3; k++) c->pos_last[k] += (double)c->delta_pos[k] * dtp;
    v3cpy(c->delta_pos, c->speed);
    c->q_last = c->q_next;
    ivp_quat_mul(&c->q_next, &c->q_next, &Q);
    ivp_quat_normalize_iter(&c->q_next);
    calc_rotation_info(c, &Q);
    phys_collision_core_integrated(w, c, ev->delta_time);   /* 0x1001e750 */
}

/* 0x10013cb0 */
void ivp_simulate_psi(PhysWorld *w)
{
    w->state = 0;
    if (w->nrevive) revive_cores_psi(w);
    /* the collision delegator's begin-PSI hook, PSI listeners and the mindist pre-PSI pass */
    phys_collision_psi_start(w);
    /* 0x100124c0: the active units in list order, except the last three which go in reverse (the loop's
       3-deep look-ahead) */
    uint32_t nu = 0;
    for (IvpSimUnit *u = w->active; u; u = u->next) nu++;
    IvpSimUnit **units = malloc((nu ? nu : 1) * sizeof *units), **order = malloc((nu ? nu : 1) * sizeof *order);
    nu = 0;
    for (IvpSimUnit *u = w->active; u; u = u->next) units[nu++] = u;
    uint32_t head = nu > 3 ? nu - 3 : 0, no = 0;
    for (uint32_t i = 0; i < head; i++) order[no++] = units[i];
    for (uint32_t i = nu; i-- > head;) order[no++] = units[i];
    IvpEventSim ev = {w->delta_psi, w->inv_delta_psi, w, NULL};
    CoreVec cores = {0};
    for (uint32_t i = 0; i < no; i++) sim_unit_psi(w, order[i], &ev, &cores);
    free(units);
    free(order);
    /* 0x1001ea50 */
    IvpEventSim iev = {w->delta_psi, inv_dt(w->delta_psi), w, NULL};
    for (uint32_t i = cores.n; i-- > 0;) ivp_calc_next_psi_matrix(w, cores.v[i], &iev);
    w->state = 2;
    phys_collision_psi_end(w);
    w->state = 5;
    free(cores.v);
}
