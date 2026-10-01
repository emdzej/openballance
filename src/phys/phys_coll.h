/* IVP collision detection runtime (docs/ivp_collision.md; chapter numbers below refer to it): the min list
   and time manager, the per-object hull manager, the OV-tree broadphase, the OO watcher, mindists and their
   manager, the minimize ("closest feature") and event ("time of impact") solvers. Included by
   phys_internal.h; PhysWorld / PhysBody embed the per-environment and per-object parts. */
#pragma once
#include "phys_ledge.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct PhysWorld PhysWorld;
typedef struct PhysBody PhysBody;
typedef struct IvpCore IvpCore;

#define IVP_NO_INDEX 0xffffffffu

/* ---- IVP_U_Min_List (2.1.5): sorted, ties newest first; handles are element indices ---- */
typedef struct {
    uint32_t next, prev;
    float value;
    void *payload;
} IvpMinListElem;

typedef struct {
    IvpMinListElem *e;
    uint32_t cap, free_head, first, last, n;
    float min_value;              /* the first value, 1e10 when empty */
} IvpMinList;

void ivp_minlist_init(IvpMinList *l);
void ivp_minlist_free(IvpMinList *l);
uint32_t ivp_minlist_add(IvpMinList *l, void *payload, float value);   /* 0x10030180 */
void ivp_minlist_remove(IvpMinList *l, uint32_t i);                    /* 0x10030470 */
static inline void *ivp_minlist_first(const IvpMinList *l) { return l->n ? l->e[l->first].payload : (void *)0; }

/* ---- hull listeners (2.1.6): a synapse of a HULL mindist, an OV element, an OO watcher's hull synapse ---- */
enum { IVP_HL_SYNAPSE, IVP_HL_OV, IVP_HL_WATCHER };
typedef struct {
    uint8_t kind;                 /* vtable 0x100637a0 / 0x10063a00 / 0x10063b90 */
    uint32_t index;               /* +0x04: index in the hull manager's list */
} IvpHullListener;

/* IVP_Hull_Manager (2.1.6, embedded in the object at +0x48) */
typedef struct {
    double last_time;
    float gradient, center_gradient;
    float hull_value, center_hull_value, hull_value_next_psi;
    int32_t reset_time;
    IvpMinList list;
} IvpHullManager;

/* ---- time events (2.1.4): the PSI event and the mindists ---- */
enum { IVP_EV_PSI, IVP_EV_MINDIST };
typedef struct {
    uint8_t kind;
    uint32_t index;               /* +0x04: index in the time manager's list */
} IvpTimeEvent;

/* the time manager (2.1.4, 0x1002f0b0) */
typedef struct {
    IvpMinList list;              /* +0x08 */
    IvpTimeEvent psi_event;       /* +0x0c */
    double last_rel;              /* +0x10 */
    double base;                  /* +0x18 */
} IvpTimeManager;

/* ---- the object cache (4.2.7): m_world_f_object and the core position at env time ---- */
typedef struct {
    bool valid;
    uint32_t time_code;           /* +0x00 */
    double r[3][3];               /* +0x30: m_world_f_object, world = r p + vv */
    double vv[3];                 /* +0x90 */
    double core_pos[3];           /* +0xb0 */
} IvpCache;

/* ---- collisions: what an OV element's vector holds (a mindist created by the root, or an OO watcher) ---- */
enum { IVP_COLL_MINDIST, IVP_COLL_WATCHER };
typedef struct IvpWatcher IvpWatcher;
typedef struct {
    uint8_t kind;
    IvpWatcher *owner;            /* +0x08 delegator: NULL = the root delegator (0x1002f5a0) */
    int32_t fvec[2];              /* +0x0c / +0x10: index in the owner's vectors, -1 = none */
} IvpCollision;

/* synapse status (2.1.7) */
enum { IVP_ST_POINT = 0, IVP_ST_EDGE = 1, IVP_ST_FACE = 2, IVP_ST_BALL = 3, IVP_ST_BACKSIDE = 5 };

typedef struct IvpMindist IvpMindist;

/* IVP_Synapse_Real (4.2.4, 0x1c bytes) */
typedef struct IvpSynapse {
    IvpHullListener hl;           /* +0x00 vtable 0x100637a0, +0x04 hull index */
    struct IvpSynapse *next, *prev;   /* +0x08 / +0x0c: the object's EXACT or INVALID list */
    PhysBody *obj;                /* +0x10 */
    const IvpEdge *edge;          /* +0x14: P = start, K = edge, F = its triangle; ball: dummy ledge + 0x14 */
    IvpMindist *md;               /* +0x18 (mindist - synapse) */
    int16_t status;               /* +0x1a */
} IvpSynapse;

/* mindist flags (4.2.3) */
#define IVP_MDF_COLL_TYPE 0xffu
#define IVP_MDF_SORT 0x100u
#define IVP_MDF_FUNCTION 0x3000u          /* 0x1000 phantom (never in Ballance) */
#define IVP_MDF_RECALC 0xc000u            /* 0x4000: the last recalc did not converge */
#define IVP_MDF_STATUS_SHIFT 18
#define IVP_MDF_STATUS 0x3c0000u
enum { IVP_MD_INVALID = 2, IVP_MD_EXACT = 3, IVP_MD_HULL_RECURSIVE = 4, IVP_MD_HULL = 5 };
#define IVP_MDF_COLL_DIST_SHIFT 22
#define IVP_MDF_COLL_DIST 0x3fc00000u

/* IVP_Mindist (4.2.1, 0x88 bytes). Its output for the contact system (docs/ivp_contact.md 2.5, CP7): the two
   objects and their features (synapse obj, edge, status) ordered by the sort flag (ball, then point / edge
   first), the gap len_numerator (distance minus the extra radii) and the unit world normal from the second
   sorted synapse's object toward the first's. */
struct IvpMindist {
    IvpCollision coll;
    IvpTimeEvent ev;              /* +0x04 */
    uint32_t flags;               /* +0x14 */
    IvpSynapse syn[2];            /* +0x18, +0x34 */
    float sum_extra_radius;       /* +0x50 */
    float len_numerator;          /* +0x54 */
    float contact_dot_diff_center;/* +0x58 */
    double sum_angular_hull_time; /* +0x60 */
    float normal[3];              /* +0x68 */
    uint32_t recalc_time_stamp;   /* +0x78 */
    IvpMindist *next, *prev;      /* +0x7c / +0x80: the manager's EXACT or INVALID list */
    const IvpEdge *wheel_edge;    /* +0x84: the triangle the wheel pass last handled */
};

static inline int ivp_md_status(const IvpMindist *m) { return (int)((m->flags & IVP_MDF_STATUS) >> IVP_MDF_STATUS_SHIFT); }
static inline void ivp_md_set_status(IvpMindist *m, int s)
{
    m->flags = (m->flags & ~IVP_MDF_STATUS) | ((uint32_t)s << IVP_MDF_STATUS_SHIFT);
}
/* the sorted synapses: syn[sel ^ k] */
static inline int ivp_md_sel(const IvpMindist *m) { return (int)((m->flags >> 8) & 1); }
static inline IvpSynapse *ivp_md_sorted(IvpMindist *m, int k) { return &m->syn[ivp_md_sel(m) ^ k]; }

/* the OO watcher's hull synapse (3.1.8, 16 bytes) */
typedef struct {
    IvpHullListener hl;           /* vtable 0x10063b90 */
    PhysBody *obj;
    IvpWatcher *w;
} IvpWatcherSynapse;

/* IVP_OO_Watcher (3.1.8, 0x40 bytes) */
struct IvpWatcher {
    IvpCollision coll;
    IvpWatcherSynapse syn[2];     /* +0x18 / +0x28 */
    IvpMindist **children;        /* +0x38 / +0x3c */
    uint32_t n, cap;
};

/* ---- OV tree (2.1.9-2.1.10) ---- */
typedef struct IvpOvNode IvpOvNode;
typedef struct {
    IvpHullListener hl;           /* +0x00 vtable 0x10063a00, +0x04 */
    IvpOvNode *node;              /* +0x08 */
    IvpHullManager *hull;         /* +0x0c */
    float center[3];              /* +0x10 */
    float radius;                 /* +0x20 */
    PhysBody *obj;                /* +0x24 */
    IvpCollision **coll;          /* +0x28.. */
    uint32_t ncoll, capcoll;
} IvpOvElement;

struct IvpOvNode {
    int32_t x, y, z, L, raster;
    IvpOvNode *parent;
    IvpOvNode **ch;
    uint32_t nch, capch;
    IvpOvElement **el;
    uint32_t nel, capel;
    IvpOvNode *hnext;             /* hash chain */
};

typedef struct {
    double powerlist[81];         /* 2^(j-40) */
    int32_t sx, sy, sz, sL, sraster;   /* the search node */
    IvpOvNode *hash[256];
    IvpOvNode *root;
    IvpOvElement ***cand;         /* the candidate vector being filled (NULL: none) */
    uint32_t *ncand, *capcand;
} IvpOvTree;

/* ---- the mindist manager (4.2.5) and the environment's collision part ---- */
typedef struct {
    int in_recursion;             /* +0x00 */
    IvpMindist *exact;            /* +0x08 */
    IvpMindist **wheel;           /* +0x0c: the EXACT mindists of car-wheel cores (core +0x10): none in Ballance */
    uint32_t nwheel, capwheel;
    IvpMindist *invalid;          /* +0x14 */
} IvpMindistManager;

typedef struct {
    IvpTimeManager tm;            /* env +0x04 */
    IvpMindistManager mm;         /* env +0x10 */
    IvpOvTree ov;                 /* env +0x14 */
    IvpHullManager **check;       /* the hull managers to check after integration (0x1001ecd0) */
    uint32_t ncheck, capcheck;
    uint32_t impact_counter;      /* env +0x13c */
    /* statistics (env +0x78, +0x7c, +0x80, +0x84, +0x88) */
    uint32_t md_live, md_created, md_deleted, watcher_checks, ov_rechecks;
    uint32_t impacts;
} IvpCollWorld;

/* the per-object part (IVP_Real_Object) */
typedef struct {
    int type;                     /* +0x04: 2 polygon, 3 ball */
    IvpSynapse *exact_syn;        /* +0x20 */
    IvpSynapse *invalid_syn;      /* +0x24 */
    IvpCache cache;               /* +0x40 */
    IvpHullManager hull;          /* +0x48 */
    IvpOvElement *ov;             /* +0x9c */
} IvpCollObject;

enum { IVP_OBJ_POLYGON = 2, IVP_OBJ_BALL = 3 };

/* ---- mindist settings (4.1, the f32 values of 0x10015fe0(0.01f)) ---- */
#define IVP_REAL_COLL_DIST 0.001f          /* +0x000 */
#define IVP_MIN_COLL_DIST 0.01f            /* +0x004 */
#define IVP_COLL_DIST 0.01f                /* +0x008.. every coll_dists[i] */
#define IVP_FRICTION_DIST 0.02f            /* +0x10c */
#define IVP_KEEPER_DIST 0.023f             /* +0x110 */
#define IVP_SPEED_AFTER_KEEPER 0.50503469f /* +0x114 */
#define IVP_MAX_DIST_FOR_FRICTION 0x1.70a3d6p-5f   /* +0x11c: 0x3d3851eb, 2.5b + friction_dist rounded once */
#define IVP_MAX_DIST_FOR_IMPACT 0.22f      /* +0x120 */
#define IVP_MIN_FRICTION_DIST 0x1.0624dcp-10f      /* +0x124: 0x3a83126e, 0.1 min_coll_dist */

/* ---- phys_surface.c / phys_convex.c (1.3, 1.6) ---- */
void *ivp_aligned_alloc(size_t size);                 /* 0x10020100(size, 16) */
void ivp_aligned_free(void *p);                       /* 0x10020130 */
IvpCompactLedge *ivp_ledge_from_points(const double (*p)[3], uint32_t n);   /* 0x1003ae20 */
IvpCompactLedge *ivp_ledge_triangle(const double p0[3], const double p1[3], const double p2[3]);   /* 0x1003ac00 */
IvpCompactLedge *ivp_ledge_convex_hull(const double (*p)[3], uint32_t n);   /* 0x1003a750 */
IvpCompactLedge *ivp_ledge_generate(const float (*pts)[3], const int (*tri)[3], const int *pierce, uint32_t ntri);
IvpCompactSurface *ivp_surface_compile(IvpCompactLedge **ledges, uint32_t n);   /* 0x100384b0 */
const IvpCompactLedge *ivp_surface_single_convex(const IvpCompactSurface *cs);   /* 0x1000bcd0 */
void ivp_surface_radius_dev(const IvpCompactSurface *cs, const float c[3], float *radius, float *dev);   /* 0x1000bc40 */
typedef struct {
    const IvpCompactLedge **v;
    uint32_t n, cap;
} IvpLedgeVec;
void ivp_surface_ledges_within_radius(const IvpCompactSurface *cs, const double c[3], double r, IvpLedgeVec *out);   /* 0x1000bb30 */
const IvpCompactLedge *ivp_ball_ledge(void);          /* the global ball surface manager's dummy ledge */
/* the tree builder's guard against the empty-side split (9.6): how often it fired */
extern uint32_t ivp_tree_split_guards;

/* ---- phys_time.c (2.1.4-2.3) ---- */
void ivp_time_init(PhysWorld *w);
void ivp_time_simulate_until(PhysWorld *w, double target);   /* 0x1002f250 */
void ivp_time_insert(PhysWorld *w, IvpTimeEvent *ev, double t);   /* 0x1002f1d0 */
void ivp_time_remove(PhysWorld *w, IvpTimeEvent *ev);              /* 0x1002f200 */
void ivp_hull_init(IvpHullManager *h);
float ivp_hull_value_at(const IvpHullManager *h, double t);
void ivp_hull_insert(IvpHullManager *h, IvpHullListener *l, float key);
void ivp_hull_remove(IvpHullManager *h, IvpHullListener *l);
void ivp_hull_update(IvpHullManager *h, double now, double dt, float speed, float center_speed);   /* 0x1001e750 */
void ivp_hull_phase(PhysWorld *w);                                 /* 0x1001eb10 */
void ivp_hull_reset(IvpHullManager *h);                            /* 0x1001a820 */

/* ---- phys_ov.c (2.4, 2.6) ---- */
void ivp_ov_init(IvpOvTree *ov);
void ivp_ov_free(IvpOvTree *ov);
void ivp_enable_collision_detection(PhysWorld *w, PhysBody *b);    /* 0x100176e0 */
void ivp_disable_collision_detection(PhysWorld *w, PhysBody *b);   /* 0x1002dc70 */
void ivp_recheck_ov_element(PhysWorld *w, PhysBody *b);            /* 0x10017140 */
void ivp_recheck_ov_element_forced(PhysWorld *w, PhysBody *b);     /* 0x100099f0 */
void ivp_range_intra(PhysWorld *w, PhysBody *a, PhysBody *b, double *ra, double *rb);   /* 0x1002d990 */
void ivp_collision_deleted(PhysWorld *w, IvpCollision *c);         /* root delegator slot 0 (0x1002f3f0) */
void ivp_collision_delete(PhysWorld *w, IvpCollision *c);
void ivp_ov_add_collision(IvpOvElement *e, IvpCollision *c);       /* 0x1002dde0 */

/* ---- phys_watcher.c (3.4-3.5) ---- */
IvpWatcher *ivp_watcher_create(PhysWorld *w, PhysBody *a, PhysBody *b);   /* 0x10038000 */
void ivp_watcher_check(PhysWorld *w, IvpWatcher *wt);                     /* 0x10037e30 */
void ivp_watcher_delete(PhysWorld *w, IvpWatcher *wt);                    /* 0x10038100 */
void ivp_watcher_child_deleted(IvpWatcher *wt, IvpMindist *md);           /* 0x100381d0 */

/* ---- phys_mindist.c (4) ---- */
IvpCache *ivp_cache_get(PhysWorld *w, PhysBody *b);                       /* 0x1001a190 */
void ivp_cache_invalidate(PhysBody *b);                                   /* 0x10018910 */
void ivp_core_pos_at(const IvpCore *c, double t, double out[3]);          /* 0x10012470 */
IvpMindist *ivp_mindist_create(PhysWorld *w, IvpWatcher *owner, PhysBody *a, PhysBody *b, const IvpEdge *ea,
                               const IvpEdge *eb);                        /* 0x10016290 + 0x10016490 */
void ivp_mindist_delete(PhysWorld *w, IvpMindist *md);                    /* 0x100162f0 */
const IvpCompactLedge *ivp_mindist_ledge(const IvpMindist *md, int k);    /* get_ledges 0x10016430 */
int ivp_recalc_mindist(PhysWorld *w, IvpMindist *md);                     /* 0x10019950 */
int ivp_recalc_mindist_once(PhysWorld *w, IvpMindist *md);                /* 0x100197f0 */
void ivp_update_mindist_events(PhysWorld *w, IvpMindist *md, int allow_hull, int hint);   /* 0x10017870 */
void ivp_mindist_simulate_event(PhysWorld *w, IvpMindist *md);            /* 0x100181b0 */
void ivp_mindist_hull_exceeded(PhysWorld *w, IvpMindist *md, float intrusion);   /* 0x10017d70 */
void ivp_mindist_hull_reset(IvpMindist *md, float dh, float dc);          /* 0x10017d50 */
void ivp_short_pass(PhysWorld *w);                                        /* 0x10017850 */
void ivp_critic_pass(PhysWorld *w);                                       /* 0x10017770 */
void ivp_wheel_pass(PhysWorld *w);                                        /* 0x10017790 */
void ivp_object_recalc_after_impact(PhysWorld *w, PhysBody *b);           /* 0x10009610 */
void ivp_object_after_controllers(PhysWorld *w, PhysBody *b);             /* 0x100099a0 */
void ivp_mm_free(PhysWorld *w);

/* ---- phys_minimize.c (3.9-3.10, 5.5) ---- */
typedef struct {
    IvpMindist *md;
    int count;                    /* +0x04: loop countdown */
    double pos[3];                /* +0x08: the backside point (in the status-5 synapse's object space) */
    uintptr_t hash[256][2];       /* +0x28 */
    int nhash;                    /* +0x828 */
    PhysWorld *w;
} IvpMinSolver;
int ivp_minimize_step(IvpMinSolver *s);              /* table 0x10075ee0 */
void ivp_fix_backside(IvpMinSolver *s);              /* 0x10019790 */
/* 0x10020ad0: the unscaled barycentric weights of X (object space) for the edges e, next(e), prev(e) and det */
void ivp_tri_qr_vals(const IvpCompactLedge *l, const IvpEdge *e, const double X[3], float out[4]);

/* ---- phys_event.c (5.2-6) ---- */
typedef struct {
    double rot_sum;               /* +0x00 */
    double proj_v;                /* +0x08 */
    double worst_approach;        /* +0x10 */
    double worst_total;           /* +0x18 */
    IvpMindist *md;               /* +0x20 */
    PhysWorld *env;               /* +0x24 */
    double t_now, t_max;          /* +0x28, +0x30 */
    int type;                     /* +0x38 */
    double time;                  /* +0x40 */
} IvpEventSolver;
void ivp_event_solve(IvpEventSolver *s);             /* table 0x1007632c */

/* ---- math helpers shared by the solvers ---- */
double ivp_isqrt4(float x);       /* 0x1000dae0: the argument rounded to f32 */
double ivp_isqrt5(double x);      /* 0x1000db80 */
int ivp_normalize4(double v[3]);  /* 0x1000dd50 */
double ivp_normalize_len(double v[3]);   /* 0x1000de30 */
int ivp_ftol(double x);           /* x87 _ftol: truncation, INT_MIN when out of range */

