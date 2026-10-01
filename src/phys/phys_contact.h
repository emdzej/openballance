/* IVP's contact response (docs/ivp_contact.md; section numbers below refer to it): contact points, friction
   systems with their core pairs and per-core friction infos, the friction linear solver and the impact
   solver. The layouts follow section 2; offsets in the comments are the original's. */
#pragma once
#include "phys_internal.h"
#include "ivp_const.h"
#include <stdlib.h>

/* ---- IVP_U_Vector: append, remove by shifting the tail down, search from the end ---- */
typedef struct {
    void **v;
    uint32_t n, cap;
} IvpVec;

static inline void ivp_vec_add(IvpVec *v, void *e)
{
    if (v->n == v->cap) {
        v->cap = 2 * v->cap + 1;                   /* 0x1000b600 */
        v->v = realloc(v->v, v->cap * sizeof *v->v);
    }
    v->v[v->n++] = e;
}
static inline int ivp_vec_find(const IvpVec *v, const void *e)
{
    for (uint32_t i = v->n; i-- > 0;)
        if (v->v[i] == e) return (int)i;
    return -1;
}
static inline void ivp_vec_remove_at(IvpVec *v, uint32_t i)
{
    memmove(&v->v[i], &v->v[i + 1], (v->n - i - 1) * sizeof *v->v);
    v->n--;
}
static inline void ivp_vec_remove(IvpVec *v, const void *e)
{
    int i = ivp_vec_find(v, e);
    if (i >= 0) ivp_vec_remove_at(v, (uint32_t)i);
}
static inline void ivp_vec_free(IvpVec *v)
{
    free(v->v);
    v->v = NULL;
    v->n = v->cap = 0;
}

typedef struct IvpContactPoint IvpContactPoint;
typedef struct IvpFrictionPair IvpFrictionPair;

/* a contact point's synapse (cp+0x08 / +0x1c, 0x14 bytes): in the object's friction-synapse list (obj+0x28) */
typedef struct IvpFricSyn {
    struct IvpFricSyn *next, *prev;   /* +0x0 / +0x4 */
    PhysBody *obj;                    /* +0x8 */
    IvpContactPoint *cp;              /* +0xc (the back offset) */
    uint8_t type;                     /* +0xe: IVP_ST_POINT / EDGE / FACE / BALL */
    const IvpEdge *g;                 /* +0x10: the compact edge (a ball: the dummy ledge's) */
} IvpFricSyn;

/* tmp_contact_info (2.7, 0xe0 bytes). The +0x60..+0x6c union is split: the friction solver's and the impact
   path's uses never meet (both are rewritten before they are read). */
typedef struct {
    float n[3];                       /* +0x00 surf_normal, obj0 -> obj1 */
    float rel[3];                     /* +0x10 v1 - v0 before the impact (the event speed) */
    double cp_ws[3];                  /* +0x20 on obj0's surface */
    PhysBody *obj[2];                 /* +0x40 / +0x44 */
    const IvpEdge *g[2];              /* +0x48 / +0x4c */
    float mat_friction[2], mat_elasticity[2];   /* +0x50 / +0x54: the synapses' materials */
    int16_t index;                    /* +0x58 solver index */
    int16_t pushes;                   /* +0x5a impacts of this impact system */
    uint32_t flags;                   /* +0x5c byte 0 prediction evaluated; bits 8..9 feature left */
    IvpFrictionInfo *fi[2];           /* +0x60 / +0x64 (solver) */
    float rescue, pred_gap;           /* +0x60 / +0x64 (impact) */
    int32_t key;                      /* +0x68 (solver) */
    float elasticity;                 /* +0x68 (read_materials) */
    float gap;                        /* +0x6c (solver) */
    float virt_mass, inv_virt_mass;   /* +0x70 / +0x74 */
    IvpCore *core[2];                 /* +0x78 / +0x7c, NULL when fixed */
    float span[2][3];                 /* +0x80 / +0x90 */
    float cp_cs[2][3];                /* +0xa0 / +0xb0 */
    float cross_cs[2][3];             /* +0xc0 / +0xd0 */
} IvpContactInfo;

#define IVP_CI_LEFT_FEATURE(t) (((t)->flags & 0x300) == 0x100)

/* IVP_Contact_Point (2.6, 0x78 bytes) */
struct IvpContactPoint {
    IvpContactPoint *next, *prev;     /* +0x00 / +0x04: the friction system's list */
    IvpFricSyn syn[2];                /* +0x08 / +0x1c */
    float inv_vm_no_dir;              /* +0x30 */
    bool two_friction;                /* +0x34 (never set in Ballance) */
    float s[2];                       /* +0x38 / +0x3c span_friction_s */
    IvpContactInfo *tmp;              /* +0x40 */
    float mu;                         /* +0x44 */
    float destroyed;                  /* +0x48 integrated_destroyed_energy */
    float inv_tri_det;                /* +0x4c */
    float old_energy;                 /* +0x50 */
    float pressure;                   /* +0x54 now_friction_pressure */
    float gap;                        /* +0x58 */
    uint16_t keeper;                  /* +0x5c (no reader) */
    uint8_t recheck;                  /* +0x60 friction broken / recheck the feature */
    int32_t key;                      /* +0x64 has_negative_pull_since */
    double time;                      /* +0x68 */
    IvpFrictionSystem *fs;            /* +0x70 */
    IvpContactInfo info;              /* the short-term tmp (rebuilt by every update) */
};

/* IVP_Friction_Core_Pair (2.8, 0x38 bytes) */
struct IvpFrictionPair {
    IvpVec cps;                       /* +0x00 */
    int32_t next_ease;                /* +0x18 */
    double last_impact;               /* +0x20 */
    float anti_energy;                /* +0x28 */
    IvpCore *c[2];                    /* +0x2c / +0x30 */
};

/* the friction info of a core (2.8, 0xc bytes) */
struct IvpFrictionInfo {
    IvpVec cps;
    IvpFrictionSystem *fs;            /* +0x08 */
};

/* IVP_Friction_System (2.8, 0x50 bytes): three controllers */
struct IvpFrictionSystem {
    IvpController spring;             /* +0x00 vtable 0x100633d0, priority 600 */
    IvpController normal;             /* +0x08 vtable 0x10063408, priority 0 */
    IvpController update;             /* +0x10 vtable 0x100633ec, priority 2000 */
    PhysWorld *env;                   /* +0x04 */
    IvpContactPoint *first;           /* +0x20 */
    IvpVec cores;                     /* +0x24 all cores, fixed ones included */
    IvpVec movable;                   /* +0x2c */
    IvpVec pairs;                     /* +0x34 */
    int16_t n_cores, n_contacts;      /* +0x3c / +0x3e */
    bool uf_needed;                   /* +0x44 */
    IvpFrictionSystem *wnext, *wprev; /* the world's list (debugging, teardown) */
};

static inline bool ivp_core_fixed(const IvpCore *c) { return c->unmovable; }
static inline IvpCore *ivp_cp_core(const IvpContactPoint *cp, int k) { return &cp->syn[k].obj->core; }
static inline float ivp_dotf(const float a[3], const float b[3])   /* 0x1001b060 order */
{
    return (float)(((double)a[2] * b[2] + (double)a[1] * b[1]) + (double)a[0] * b[0]);
}
static inline double ivp_dotd(const float a[3], const float b[3]) { return ((double)a[2] * b[2] + (double)a[1] * b[1]) + (double)a[0] * b[0]; }

/* ---- phys_contact.c: contact points (CP) ---- */
IvpContactPoint *ivp_cp_find_or_create(PhysWorld *w, IvpMindist *md, bool *created);   /* 0x10020030 */
void ivp_cp_update(PhysWorld *w, IvpContactPoint *cp);                                  /* 0x1001f860 */
void ivp_cp_read_materials(PhysWorld *w, IvpContactPoint *cp);                          /* 0x10024040 */
void ivp_cp_init_constants(IvpContactPoint *cp);                                        /* 0x1001d3d0 */
void ivp_cp_destroy(PhysWorld *w, IvpContactPoint *cp);                                 /* 0x1001c230 + free */
double ivp_worst_vm(const IvpCore *c, const float p[3]);                                /* 0x1000c200 */
void ivp_ball_transfer(PhysWorld *w, IvpMindist *md, PhysBody *ball, float gap);        /* 0x10018040 */

/* ---- phys_friction.c: friction systems (FS) ---- */
IvpContactPoint *ivp_try_generate_friction(PhysWorld *w, IvpMindist *md, IvpFrictionSystem **out_fs, bool *created,
                                           IvpSimUnit *keep, bool update);              /* 0x10022180 */
IvpFrictionInfo *ivp_core_fi(const IvpCore *c, const IvpFrictionSystem *fs);            /* 0x1000d960 */
IvpFrictionPair *ivp_fs_find_pair(const IvpFrictionSystem *fs, const IvpCore *a, const IvpCore *b);   /* 0x1001d380 */
void ivp_fs_remove_cp(PhysWorld *w, IvpFrictionSystem *fs, IvpContactPoint *cp);       /* 0x1001c460 */
void ivp_fs_unlink(IvpFrictionSystem *fs, IvpContactPoint *cp);                         /* 0x1000b290 */
void ivp_fs_push_front(IvpFrictionSystem *fs, IvpContactPoint *cp);                     /* 0x1000b360 */
int ivp_fs_recalc_pair(PhysWorld *w, IvpFrictionPair *p, IvpFrictionSystem *fs);       /* 0x1001cc20 */
void ivp_fs_delete(PhysWorld *w, IvpFrictionSystem *fs);                                /* 0x1000b100 */
void ivp_object_remove_contacts(PhysWorld *w, PhysBody *b, bool keep_asleep);           /* 0x10009a40 */
int ivp_fs_revive_core(PhysWorld *w, IvpCore *c);                                       /* 0x1001d4d0 */
double ivp_core_energy(const IvpCore *c, const float v[3], const float w[3]);           /* 0x1000c480 */
void ivp_core_point_velocity(const IvpCore *c, const float p_cs[3], const float v[3], const float w[3], float out[3]);   /* 0x1000bf90 */

/* ---- phys_fsolver.c: the friction linear solver (LS) ---- */
void ivp_fs_solve(PhysWorld *w, IvpFrictionSystem *fs, const IvpEventSim *es);         /* 0x10036d70 */
/* IVP_Solver_Core_Reaction (LS1.9), 1 or 2 directions as the tangential friction uses it */
typedef struct {
    const float *dir[2];
    float cr_out[2][2][4];            /* [core][dir]: core-space r x dir, 1 */
    float cr_mult[2][2][4];           /* [core][dir]: cr_out (.) inv_I, inv_mass */
    double m00, m01, m11;
    float dv[2];
} IvpCoreReaction;
void ivp_core_reaction_init(IvpCoreReaction *t, IvpCore *c0, IvpCore *c1, const double p_ws[3], const float *d0,
                            const float *d1);                                           /* 0x10033a30 */
void ivp_core_reaction_push2(const IvpCoreReaction *t, IvpCore *c0, IvpCore *c1, const float imp[2]);   /* 0x10033ad0 */

/* ---- phys_impact.c: the impact solver and system (IM) ---- */
float ivp_cp_rescue_speed(PhysWorld *w, IvpContactPoint *cp);                           /* 0x10024930 */
