/* Rigid-body physics for the physics_RT.dll Building Blocks (docs/physics.md). The dynamics are a port of the
   Ipion (IVP) core as the original runs it (docs/ivp_core.md): PSIs of 1/66 s of simulated time driven by the
   time manager (the first at t = 0), simulation units whose controllers run by priority (forces and springs
   1500, gravity with damping and the push commit 1000, constraints 405; the friction system 2000 / 600 / 0),
   semi-implicit integration with the gyroscopic sub-steps, sleep on a world-wide countdown, and poses
   extrapolated from the last PSI for the write-back.

   Collision detection is IVP's (docs/ivp_collision.md): compact surfaces with ledge trees, the OV-tree
   broadphase, OO watchers, mindists with the closest-feature and time-of-impact solvers, all scheduled by
   the time manager's event list and the hull managers. The contact response is IVP's (docs/ivp_contact.md):
   persistent contact points left by the impacts, friction systems with the tangential friction spring, the
   normal force holding the rest gap and the multi-contact LCP, and the impact solver run at the exact event
   time with the impact system chaining it through touching bodies.

   Math is in world space with column vectors; entity matrices (row vectors) are converted at the API. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct PhysWorld PhysWorld;
typedef struct PhysBody PhysBody;
typedef struct PhysShape PhysShape;
typedef struct PhysJoint PhysJoint;
typedef struct PhysForce PhysForce;

/* Collision geometry in body space (the entity's scale baked in), shared by name like IVP's compact
   surfaces: a sphere; or a compact surface built from convex meshes (one hull ledge each) and concave
   meshes (one two-sided triangle ledge per face), compiled when the first body uses it. */
PhysShape *phys_shape_sphere(float radius);
PhysShape *phys_shape_convex(void);
void phys_shape_add_convex(PhysShape *s, const float *xyz, uint32_t n);
void phys_shape_add_triangles(PhysShape *s, const float *xyz, uint32_t nverts, const uint16_t *tri, uint32_t ntris);
void phys_shape_free(PhysShape *s);
bool phys_shape_empty(const PhysShape *s);

typedef struct {
    bool fixed;               /* unmovable */
    float friction, elasticity, mass;
    char group[9];            /* no-collision group: equal non-empty groups don't collide */
    bool start_frozen, collide;
    float lin_damp, rot_damp;
    bool has_shift;           /* mass centre = object origin + shift (body space) */
    float shift[3];
} PhysBodyDesc;

PhysWorld *phys_world_create(void);
void phys_world_destroy(PhysWorld *w);
void phys_set_gravity(PhysWorld *w, const float g[3]);
/* Set Physics Globals' time factor (the game: 2 while playing, 0 paused) */
void phys_set_time_factor(PhysWorld *w, float f);
/* PostProcess (FUN_10007ce0): smooth the frame time, advance the PSIs, then write back the awake bodies */
typedef void (*PhysWriteBack)(void *user, uint32_t entity, const float world[4][4]);
void phys_frame(PhysWorld *w, float delta_ms, PhysWriteBack wb, void *user);

/* world: the entity's world matrix (row vectors, its scale removed); the shape is borrowed (kept alive by
   the caller's cache) */
PhysBody *phys_body_create(PhysWorld *w, const PhysBodyDesc *d, PhysShape *shape, const float world[4][4], uint32_t entity);
void phys_body_destroy(PhysWorld *w, PhysBody *b);
/* an awake body restarts its sleep timers; a frozen one is revived at the start of the next PSI */
void phys_body_wake(PhysWorld *w, PhysBody *b);
uint32_t phys_body_entity(const PhysBody *b);
bool phys_body_fixed(const PhysBody *b);
/* world-space point <-> body queries */
void phys_body_velocity_at(const PhysBody *b, const float p[3], float v[3]);

/* SetPhysicsForce's controller (priority 1500): every PSI an impulse of the world force at the point
   (body-local, used as core coordinates, when local; else a world point taken into core space at creation).
   It acts only while the body is awake. */
PhysForce *phys_force_create(PhysWorld *w, PhysBody *b, const float force[3], const float point[3], bool local);
void phys_force_destroy(PhysWorld *w, PhysForce *f);
/* Physics Impulse: at a world point, or body-local; an async push committed at the next PSI */
void phys_impulse(PhysWorld *w, PhysBody *b, const float impulse[3], const float point[3], bool local);

/* Constraints (anchors and axes in world space at creation; b may be NULL = the world). a is the BB target
   (IVP's objR), b Object2 (objA); joined bodies share one sim unit and sleep and wake together. */
PhysJoint *phys_hinge_create(PhysWorld *w, PhysBody *a, PhysBody *b, const float anchor[3], const float axis[3]);
PhysJoint *phys_slider_create(PhysWorld *w, PhysBody *a, PhysBody *b, const float p0[3], const float p1[3], bool limits,
                              float lo, float hi);
PhysJoint *phys_balljoint_create(PhysWorld *w, PhysBody *a, PhysBody *b, const float anchor[3]);
PhysJoint *phys_spring_create(PhysWorld *w, PhysBody *a, const float pa[3], PhysBody *b, const float pb[3], float length,
                              float constant, float lin_damp, float glob_damp);
void phys_joint_destroy(PhysWorld *w, PhysJoint *j);

/* Contact events for the listener BBs (docs/ivp_contact.md 7.6). The post-collision event of a mindist
   impact: the contact's objects (the normal points from obj[0] to obj[1]), the objects in listener order
   (their object listeners run obj order[0]'s then order[1]'s), the full relative velocity at the contact
   before the impact, the shared normal (a listener may negate it in place, later listeners see that), the
   contact point and the time since the pair's last impact. Friction contacts are reported as they are
   created (begin) and deleted. */
typedef struct {
    PhysBody *obj[2];
    PhysBody *order[2];
    float rel[3];
    float *normal;
    double pos[3];
    float dt_last;
} PhysImpactEvent;
typedef void (*PhysImpactFn)(void *user, PhysImpactEvent *ev);
typedef void (*PhysContactFn)(void *user, PhysBody *a, PhysBody *b, bool begin);
void phys_set_listeners(PhysWorld *w, PhysImpactFn impact, PhysContactFn contact, void *user);
double phys_time(const PhysWorld *w);

/* debugging: body count, awake count, contact points; the i-th body's entity, position (the object
   origin at the current time), awake; its core's linear (world) and angular (core axes) velocity and
   movement state */
void phys_debug(const PhysWorld *w, uint32_t *bodies, uint32_t *awake, uint32_t *contacts);
bool phys_debug_body(const PhysWorld *w, uint32_t i, uint32_t *entity, float pos[3], bool *awake, bool *fixed, uint32_t *shape_kind,
                     uint32_t *ntris);
bool phys_debug_core(const PhysWorld *w, uint32_t i, float speed[3], float rot_speed[3], int *movement_state);
/* the i-th contact point: its bodies' entities, feature types, gap, normal (from a toward b), point */
bool phys_debug_contact(const PhysWorld *w, uint32_t i, uint32_t *ea, uint32_t *eb, int types[2], float *gap, float n[3], float p[3]);
/* collision counters: live / created mindists, OV rechecks, watcher checks, impacts, tree split guards */
void phys_debug_collision(const PhysWorld *w, uint32_t out[6]);
