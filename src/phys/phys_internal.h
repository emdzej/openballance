/* Internals shared by the physics sources. The dynamics are the IVP core of physics_RT.dll as specified in
   docs/ivp_core.md (section numbers below refer to it); addresses are entry points in physics_RT.dll. */
#pragma once
#include "phys.h"
#include "phys_coll.h"
#include <assert.h>
#include <math.h>
#include <string.h>

enum { PHYS_SPHERE, PHYS_COMPOUND };

/* a collision surface (the glue's cache entry): a ball radius, or IVP's compact surface (1.6) built from the
   ledges in insertion order (convex meshes as hull ledges, then one pancake ledge per concave face) */
struct PhysShape {
    int kind;
    float radius;
    IvpCompactLedge **ledges;     /* builder input until compiled */
    uint32_t nledges, capledges;
    IvpCompactSurface *cs;
    uint32_t ntri;                /* input faces (debug) */
    bool built;
};

/* ---- IVP math types (0.1): quaternions (x, y, z, w) and matrices in f64, column vectors:
   world = r * local + vv ---- */
typedef struct { double x, y, z, w; } IvpQuat;
typedef struct { double r[3][3]; double vv[3]; } IvpMatrix;

typedef struct IvpCore IvpCore;
typedef struct IvpController IvpController;
typedef struct IvpSimUnit IvpSimUnit;
typedef struct IvpFrictionInfo IvpFrictionInfo;
typedef struct IvpFrictionSystem IvpFrictionSystem;
struct IvpFricSyn;

/* IVP_Event_Sim (1.5) */
typedef struct {
    double delta_time, i_delta_time;
    PhysWorld *env;
    IvpSimUnit *sim_unit;
} IvpEventSim;

/* the controller vtable (6.4): priority (+0x14) and do_simulation_controller (+0x10) on the cores of one
   sim unit that the controller acts on. links_cores marks controllers that keep their cores in one unit
   (springs, constraints: 0x10011b30); the unit split uses it. */
typedef struct {
    int priority;
    void (*simulate)(IvpController *c, const IvpEventSim *ev, IvpCore **cores, uint32_t n);
    bool links_cores;
} IvpControllerVt;

struct IvpController {
    const IvpControllerVt *vt;
};

/* the gravity controller (1.7, vtable 0x10063598, 0x14 bytes) */
typedef struct {
    IvpController ctrl;
    float g[3];               /* +0x04 */
} IvpGravity;

/* movement states (0.4), compared as signed char */
enum { IVP_MT_MOVING = 1, IVP_MT_SLOW = 2, IVP_MT_CALM = 3, IVP_MT_NOT_SIM = 8, IVP_MT_STATIC = 0x10 };

/* IVP_Core (1.2, 0x238 bytes, ctor 0x1000d400); f32 fields are rounded on assignment as in the original */
struct IvpCore {
    bool unmovable;               /* flags bits 2-3 (physical_unmoveable) */
    bool rot_inertias_equal;      /* flags bits 6-7 (0x1000d1a0) */
    bool in_revive_list;          /* flags bits 4-5 (is_in_wakeup_vec) */
    float upper_limit_radius, max_surface_deviation;      /* +0x04, +0x08 */
    float rot_inertia[3], mass;                           /* +0x14, +0x20 */
    float rot_speed_damp[3];                              /* +0x24 */
    float inv_rot_inertia[3], inv_mass;                   /* +0x34, +0x40 */
    float speed_damp, inv_object_diameter;                /* +0x44, +0x48 */
    int8_t movement_state;                                /* +0x60 */
    double time_of_last_psi;                              /* +0x68 */
    float i_delta_time;                                   /* +0x70 */
    float rot_speed_change[3], speed_change[3];           /* +0x74, +0x84 */
    float rot_speed[3];                                   /* +0x94, core axes */
    float speed[3];                                       /* +0xa4, world */
    double pos_last[3];                                   /* +0xb8 pos_world_f_core_last_psi */
    float delta_pos[3];                                   /* +0xd8 delta_world_f_core_psis */
    IvpQuat q_last, q_next;                               /* +0xe8, +0x108 */
    IvpMatrix m_world_f_core;                             /* +0x128 m_world_f_core_last_psi */
    float rotation_axis_world[3];                         /* +0x1a8 */
    float current_speed, abs_omega, max_surface_rot_speed;   /* +0x1b8, +0x1bc, +0x1c0 */
    /* sleep test (10.0): +0x1d8.. */
    double time_calm_ref[2];
    float q_calm_ref[2][4], pos_calm_ref[2][3];
    IvpSimUnit *unit;                                     /* +0x1d4 */
    IvpController **controllers;                          /* +0x1c8 controllers_of_core */
    uint32_t ncontrollers, capcontrollers;
    PhysBody *body;               /* the core's one real object (Ballance never shares cores) */
    uint32_t impact_stamp;        /* +0x230 */
    /* the contact system's fields (docs/ivp_contact.md 2.3) */
    bool fast_piling;             /* flags bits 0-1 (never set in Ballance) */
    void *car_wheel;              /* +0x10 (never set in Ballance) */
    IvpFrictionInfo *fi;          /* +0x5c: a movable core's friction info */
    IvpFrictionInfo **fis;        /* +0x5c: an unmovable core's, one per friction system */
    uint32_t nfis, capfis;
    uint8_t temporarily_unmovable;    /* +0x61 */
    int16_t impacts_psi;          /* +0x64 */
    IvpCore *uf_parent;           /* +0x228 */
    /* +0x22c: the sync backup of 0x1000cfa0 (rot_speed, q_next) and its +0x30 "pushed" flag */
    bool synced, sync_pushed;
    float sync_rot_speed[3];
    IvpQuat sync_q;
};

/* controller entry of a sim unit (1.4): the cores of this unit the controller acts on */
typedef struct {
    IvpController *c;
    IvpCore **cores;
    uint32_t n, cap;
} IvpCtrlEntry;

/* IVP_Sim_Unit (1.4, ctor 0x10011920) */
struct IvpSimUnit {
    int8_t state;             /* low byte of the flags: 1 simulated, 8 frozen */
    bool changed;             /* flags 0x300: structure changed, split/merge (0x100120d0) */
    bool fast, fast_prev;     /* flags bits 10-11 and 12-13 */
    IvpCore **cores;
    uint32_t ncores, capcores;
    IvpCtrlEntry *entries;    /* ascending priority */
    uint32_t nentries, capentries;
    IvpSimUnit *prev, *next;  /* the manager's active / frozen list */
};

/* the real object (1.1) plus the glue-facing data */
struct PhysBody {
    PhysWorld *world;
    uint32_t entity;          /* client data (+0xb0) */
    PhysShape *shape;
    bool fixed, collide;
    float friction, elasticity;
    char group[9];            /* +0x90 */
    int8_t object_state;      /* +0x80 low byte: 1 simulated, 8 not simulated, 0x10 unmovable */
    float shift_core_f_object[3];   /* +0x30: the object origin in core coordinates */
    bool shift_is_zero;       /* +0x80 bits 10-11 */
    float extra_radius;       /* +0xa0 */
    IvpCore core;             /* +0xa4 */
    IvpCollObject coll;       /* collision detection */
    struct IvpFricSyn *fric_syn;      /* +0x28: the contact points' synapses (head insert) */
    PhysBody *next, *prev;
};

/* the glue force controller (7.2, vtable 0x10063240, priority 1500) */
struct PhysForce {
    IvpController ctrl;
    PhysBody *body;
    double point_cs[3];       /* +0x10, core coordinates */
    float force_ws[3];        /* +0x30, impulse per PSI */
    PhysForce *next;
};

enum { PHYS_HINGE, PHYS_SLIDER, PHYS_BALLJOINT, PHYS_SPRING };

/* springs (12.2, 0x98 bytes) and constraints (12.5, 0x190 bytes) */
struct PhysJoint {
    IvpController ctrl;
    int kind;
    PhysBody *a, *b;          /* spring: anchor 0 / anchor 1 bodies; constraint: objR / objA. NULL = world */
    IvpCore *cores[2];        /* controlled cores (the movable ones) */
    uint32_t ncores;
    /* spring */
    float anchor_core[2][3];  /* anchor +0x1c: positions in core space (world when the body is NULL) */
    float length, k, c_lin, c_rel;   /* +0x74, +0x7c, +0x80, +0x84 */
    /* constraint */
    float force_factor, damp_factor, limit_factor;   /* +0x18, +0x1c (damp / force), +0x68 */
    uint32_t type[6];         /* +0x20: trans x y z, rot x y z; bit 0 active, bit 1 limited */
    float lo[6], hi[6];       /* +0x38, +0x50 */
    IvpMatrix m_rcs_f_rcore, m_acs_f_acore;   /* +0x70, +0xf8 */
    uint8_t rot_order[3];     /* +0x183: fixed, limited, free */
    uint8_t nfixed_rot, nlimited_rot, nactive;   /* +0x187, +0x189, +0x18a */
    PhysJoint *next;
};

struct PhysWorld {
    float time_factor;        /* mgr+0xd0 = factor x 0.001 */
    float smoothed_ms;        /* mgr+0xc8 */
    /* environment (1.6) */
    double current_time;      /* +0x120 */
    double time_of_next_psi;  /* +0x128 */
    double time_of_last_psi;  /* +0x130 */
    double delta_psi, inv_delta_psi;   /* +0xc0, +0xc8 */
    uint32_t time_code;       /* +0x138 */
    uint16_t sleep_countdown; /* +0x140 */
    int32_t state;            /* +0x144 */
    float freeze_check_time;  /* +0x98 */
    /* anomaly limits (9) */
    float max_velocity, max_angular_velocity_per_psi;
    int32_t max_collisions_per_psi;
    IvpGravity gravity;       /* env +0 */
    IvpCollWorld coll;            /* collision detection */
    /* sim-unit manager (10.0) */
    IvpSimUnit *active, *frozen;
    IvpCore **revive;         /* +0x106/+0x108 core_revive_list */
    uint32_t nrevive, caprevive;
    PhysBody *bodies;
    PhysForce *forces;
    PhysJoint *joints;
    IvpFrictionSystem *fs_list;   /* every friction system (debugging, teardown) */
    PhysImpactFn impact_fn;
    PhysContactFn contact_fn;
    void *listen_user;
};

/* small f32 vector helpers */
static inline void v3set(float d[3], float x, float y, float z) { d[0] = x, d[1] = y, d[2] = z; }
static inline void v3cpy(float d[3], const float s[3]) { d[0] = s[0], d[1] = s[1], d[2] = s[2]; }
static inline float v3dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void v3sub(float d[3], const float a[3], const float b[3]) { d[0] = a[0] - b[0], d[1] = a[1] - b[1], d[2] = a[2] - b[2]; }
static inline void v3add(float d[3], const float a[3], const float b[3]) { d[0] = a[0] + b[0], d[1] = a[1] + b[1], d[2] = a[2] + b[2]; }
static inline void v3scale(float d[3], const float a[3], float k) { d[0] = a[0] * k, d[1] = a[1] * k, d[2] = a[2] * k; }
static inline void v3mad(float d[3], const float a[3], const float b[3], float k) { d[0] = a[0] + b[0] * k, d[1] = a[1] + b[1] * k, d[2] = a[2] + b[2] * k; }
static inline void v3cross(float d[3], const float a[3], const float b[3])
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    d[0] = x, d[1] = y, d[2] = z;
}
static inline float v3len(const float a[3]) { return sqrtf(v3dot(a, a)); }
static inline float v3norm(float a[3])
{
    float l = v3len(a);
    if (l > 1e-12f) a[0] /= l, a[1] /= l, a[2] /= l;
    return l;
}

/* ---- IVP math (3, phys_math.c) ---- */
void ivp_quat_to_matrix(const IvpQuat *q, double r[3][3]);                 /* 0x100190c0 */
void ivp_matrix_to_quat(const double r[3][3], IvpQuat *q);                 /* 0x100191b0 */
void ivp_quat_normalize(IvpQuat *q);                                       /* 0x100194e0 */
void ivp_quat_normalize_iter(IvpQuat *q);                                  /* 0x10019540 */
void ivp_quat_mul(IvpQuat *r, const IvpQuat *a, const IvpQuat *b);         /* 0x1001e7c0: r = a (x) b */
void ivp_quat_slerp(IvpQuat *r, const IvpQuat *a, const IvpQuat *b, double t);   /* 0x10019320 */
void ivp_quat_from_rot_poly(IvpQuat *q, const float w[3], double dt);      /* 0x10018f80 */
void ivp_quat_from_rot_sin(IvpQuat *q, const float w[3], double dt);       /* 0x10019010 */
double ivp_rsqrt(double n);                     /* the bit-trick guess and 5 Newton steps of 0x1000e120 */
int ivp_normalize_d(double v[3]);               /* 0x1000e120 */
float ivp_normalize_f(float v[3]);              /* 0x1000df30: returns the length, 0 if |v|^2 < 1e-19 */
void ivp_mat_identity(IvpMatrix *m);            /* 0x1000eb00 */
void ivp_mat_mul(IvpMatrix *c, const IvpMatrix *a, const IvpMatrix *b);   /* 0x1000ec90 */
void ivp_mat_inverse(IvpMatrix *out, const IvpMatrix *m);                 /* 0x1000f2c0, orthonormal */
int ivp_mat_inverse_general(IvpMatrix *out, const IvpMatrix *m, double eps);   /* 0x1000e8f0 */
void ivp_basis(double r[3][3], const double axis[3], int k);              /* 0x1000eb60 */
static inline void ivp_rmul(const double r[3][3], const double v[3], double o[3])     /* R v (0x1000f6f0) */
{
    double x = r[0][0] * v[0] + r[0][1] * v[1] + r[0][2] * v[2], y = r[1][0] * v[0] + r[1][1] * v[1] + r[1][2] * v[2],
           z = r[2][0] * v[0] + r[2][1] * v[1] + r[2][2] * v[2];
    o[0] = x, o[1] = y, o[2] = z;
}
static inline void ivp_rtmul(const double r[3][3], const double v[3], double o[3])    /* R^T v (0x1000f760) */
{
    double x = r[0][0] * v[0] + r[1][0] * v[1] + r[2][0] * v[2], y = r[0][1] * v[0] + r[1][1] * v[1] + r[2][1] * v[2],
           z = r[0][2] * v[0] + r[1][2] * v[1] + r[2][2] * v[2];
    o[0] = x, o[1] = y, o[2] = z;
}
static inline void ivp_rmul_f(const double r[3][3], const float v[3], float o[3])     /* R v, f32 (0x1000f690) */
{
    double d[3] = {v[0], v[1], v[2]};
    ivp_rmul(r, d, d);
    o[0] = (float)d[0], o[1] = (float)d[1], o[2] = (float)d[2];
}
static inline void ivp_rtmul_f(const double r[3][3], const float v[3], float o[3])    /* R^T v, f32 (0x1000f7d0) */
{
    double d[3] = {v[0], v[1], v[2]};
    ivp_rtmul(r, d, d);
    o[0] = (float)d[0], o[1] = (float)d[1], o[2] = (float)d[2];
}
static inline void ivp_mat_rr(double c[3][3], const double a[3][3], const double b[3][3])   /* rotation A B */
{
    double t[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(c, t, sizeof t);
}
static inline void ivp_mat_rrt(double c[3][3], const double a[3][3], const double b[3][3])  /* rotation A B^T (0x1000f140) */
{
    double t[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) t[i][j] = a[i][0] * b[j][0] + a[i][1] * b[j][1] + a[i][2] * b[j][2];
    memcpy(c, t, sizeof t);
}

/* ---- the core (phys_core.c) ---- */
void ivp_core_init(PhysWorld *w, IvpCore *c, PhysBody *b, const IvpQuat *q, const double pos[3], bool unmovable);
void ivp_object_init_core(PhysWorld *w, PhysBody *b, const PhysBodyDesc *d);
void ivp_core_m_world_f_core_at(const IvpCore *c, double t, IvpMatrix *out);       /* 0x1000c510 */
void ivp_body_m_world_f_object_at(const PhysBody *b, double t, IvpMatrix *out);    /* 0x10009d70 */
void ivp_core_sync_psi_pose(IvpCore *c, double t);                                 /* 0x10012470 */
void ivp_calc_push_core(const IvpCore *c, const float p_cs[3], const float imp_cs[3], const float imp_ws[3], float dv[3],
                        float dw[3]);                                              /* 0x1000ca80 */
void ivp_async_push_core(IvpCore *c, const float p_cs[3], const float imp_cs[3], const float imp_ws[3]);   /* 0x1000c830 */
void ivp_push_core(IvpCore *c, const float p_cs[3], const float imp_cs[3], const float imp_ws[3]);         /* 0x1000c8b0 */
void ivp_async_push_core_ws(IvpCore *c, const double p_ws[3], const float imp_ws[3]);                     /* 0x1000c940 */
void ivp_rot_push_core_cs(IvpCore *c, const float t_cs[3]);                                               /* 0x1000cb70 */
void ivp_surface_speed(const IvpCore *c, const float p_cs[3], float out[3]);                              /* 0x1000c810 */
void ivp_commit_pushes(IvpCore *c);                                                                       /* 0x1000cbd0 */
void ivp_body_async_push_ws(PhysWorld *w, PhysBody *b, const double p_ws[3], const float imp_ws[3]);      /* 0x1000a3e0 */
void ivp_body_wake(PhysWorld *w, PhysBody *b);                                                            /* 0x1000a460 */
void ivp_body_revive_now(PhysWorld *w, PhysBody *b);                                                      /* 0x10009670 */
void ivp_core_add_controller(IvpCore *c, IvpController *ctrl);                                            /* 0x10011c70 */
void ivp_core_remove_controller(IvpCore *c, IvpController *ctrl);                                         /* 0x10011c00 */
void ivp_controller_attach_merge(PhysWorld *w, IvpController *ctrl, IvpCore **cores, uint32_t n);         /* 0x10011b30 */
void ivp_controller_detach(PhysWorld *w, IvpController *ctrl, IvpCore **cores, uint32_t n);               /* 0x10011a60 */
void ivp_sim_unit_merge(PhysWorld *w, IvpSimUnit *into, IvpSimUnit *from);                                /* 0x10011720 */
void ivp_core_remove(PhysWorld *w, IvpCore *c);
void ivp_simulate_psi(PhysWorld *w);                                                                      /* 0x10013cb0 */
extern const IvpControllerVt ivp_gravity_vt;

/* ---- spring and constraints (phys_constraint.c) ---- */
void ivp_joint_free(PhysWorld *w, PhysJoint *j);

/* ---- the collision seam (docs/ivp_collision.md 7.1 / 8.3) and the contact system (docs/ivp_contact.md) ---- */
/* PSI start, after the PSI listeners: the mindist manager's wheel pass (0x10017790) */
void phys_collision_psi_start(PhysWorld *w);
/* after a unit's controllers and sleep test: update_synapses_after_controllers (0x100099a0) of its objects */
void phys_collision_after_controllers(PhysWorld *w, IvpSimUnit *su);
/* revive_core (0x1000aea0) asks the friction system to pull resting partners into the unit (0x1001d4d0);
   returns 1 when the unit changed (the revive restarts) */
int phys_contact_revive_core(PhysWorld *w, IvpCore *c);
/* the integrator's hull update of a core's object (0x1001e300 -> 0x1001e750, 0x1001ecd0) */
void phys_collision_core_integrated(PhysWorld *w, IvpCore *c, double dt);
/* after integration: hull events (0x1001eb10), short / critical mindist rechecks (0x10017850 / 0x10017770) */
void phys_collision_psi_end(PhysWorld *w);
/* a core froze (0x1000ce20) / revived (0x1000cf20): recheck its object's OV element, freeze its hull */
void phys_collision_object_frozen(PhysWorld *w, PhysBody *b);
void phys_collision_object_revived(PhysWorld *w, PhysBody *b);
/* the object is deleted: its contact points go (0x10009ab0 -> 0x10009a40(1)) */
void phys_contacts_forget(PhysWorld *w, PhysBody *b);
/* the mindist's do_impact (vtable slot 7, 0x100240a0) */
void phys_contact_do_impact(PhysWorld *w, IvpMindist *md);
/* the inter-penetration hook (vtable slot 5, 0x10019770 -> anomaly manager 0x1002f8f0) */
void phys_contact_inter_penetration(PhysWorld *w, IvpMindist *md);
/* 0x1001e300 for one core with the given event sim (the impact path recomputes the next PSI from now) */
void ivp_calc_next_psi_matrix(PhysWorld *w, IvpCore *c, const IvpEventSim *ev);
/* the anomaly manager's velocity clamps (0x1002f6d0 / 0x1002f720) */
void ivp_max_velocity_exceeded(PhysWorld *w, float v[3]);
void ivp_max_angular_velocity_exceeded(PhysWorld *w, float v[3]);
/* 0x1000cec0: restart a core's freeze-check timers */
void ivp_reset_freeze_check(PhysWorld *w, IvpCore *c);
/* 0x1000dab0: revive a frozen core's unit, or restart the timers of an awake one */
void ivp_ensure_in_simulation(PhysWorld *w, IvpCore *c);
/* the controller vtables of the friction system */
extern const IvpControllerVt ivp_fs_spring_vt, ivp_fs_normal_vt, ivp_fs_update_vt;

/* body space (the entity frame) <-> world at the pose of this PSI (m_world_f_core_last_psi) */
static inline void phys_body_point(const PhysBody *b, const float local[3], float world[3])
{
    const IvpMatrix *m = &b->core.m_world_f_core;
    double p[3] = {(double)local[0] + b->shift_core_f_object[0], (double)local[1] + b->shift_core_f_object[1],
                   (double)local[2] + b->shift_core_f_object[2]};
    ivp_rmul(m->r, p, p);
    for (int k = 0; k < 3; k++) world[k] = (float)(p[k] + m->vv[k]);
}
/* the mass centre at this PSI */
static inline void phys_body_center(const PhysBody *b, float c[3])
{
    for (int k = 0; k < 3; k++) c[k] = (float)b->core.m_world_f_core.vv[k];
}
