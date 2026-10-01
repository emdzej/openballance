/* The particle emitters of TT_ParticleSystems_RT.dll (docs/particles.md): a fixed pool of particles moving in
   straight lines in world space, emitted by a 3D frame, drawn from the frame's post-render callback as
   camera-facing sprites, triangles, streaks or lines. The four Building Blocks the game uses share this
   (src/bb/bb_particles.c). */
#pragma once
#include "ck_3d.h"

enum { CKPS_POINT, CKPS_TIMEDEPENDENT, CKPS_PLANAR, CKPS_SPHERICAL };
/* render modes (setting 4) */
enum { CKPS_RENDER_POINT = 1, CKPS_RENDER_LINE = 2, CKPS_RENDER_SPRITE = 3, CKPS_RENDER_OBJECT = 4,
       CKPS_RENDER_ORIENTABLE = 5, CKPS_RENDER_FAST = 7 };
/* Evolutions (setting 8), Variances (setting 9) */
enum { CKPS_EVOL_SIZE = 1, CKPS_EVOL_COLOR = 2, CKPS_EVOL_TEXTURE = 4 };
enum { CKPS_VAR_SPEED = 1, CKPS_VAR_ANGSPEED = 2, CKPS_VAR_LIFE = 4, CKPS_VAR_EMISSION = 8, CKPS_VAR_SIZE0 = 0x10,
       CKPS_VAR_SIZE1 = 0x20, CKPS_VAR_COLOR0 = 0x200, CKPS_VAR_COLOR1 = 0x400, CKPS_VAR_FRAME0 = 0x800,
       CKPS_VAR_TEXSPEED = 0x1000 };

typedef struct {
    float color[4], dcolor[4];    /* RGBA, per ms */
    float pos[3], angle, vel[3], angspeed;
    float life, last_dt;
    float size, dsize;
    int32_t frame;
    float tex_acc, tex_speed;
    int32_t prev, next;           /* live list (newest at the head) / free list */
} CkParticle;

typedef struct CkParticleSystem {
    int kind;                     /* CKPS_* */
    CkId owner;                   /* the emitting 3D frame */
    /* pins (FUN_25082770, read every execute) */
    float yaw_var, pitch_var, speed, speed_var, angspeed, angspeed_var, life, life_var;
    int32_t max_live, emission, emission_var;
    float size0, size0_var, size1, size1_var;
    float color0[4], color0_var[4], color1[4], color1_var[4];
    CkId texture;
    int32_t frame0, frame0_var, tex_speed, tex_speed_var, tex_count, tex_loop;
    /* settings (FUN_25082a00) */
    int32_t pool_size, render, src_blend, dst_blend, evolutions, variances;
    /* the pool */
    CkParticle *pool;
    int32_t live_head, free_head, live;
    /* state */
    bool emitting, registered;
    float last_dt;
    /* time-dependent trail (+0x118..) */
    bool first_trail;
    float trail_prev[3], trail_acc;
    int32_t trail_rate;
} CkParticleSystem;

void ck_ps_init(CkParticleSystem *ps, int kind, CkId owner);
void ck_ps_free(CkParticleSystem *ps);
/* FUN_25081a60: the pool (re)allocated and emptied */
void ck_ps_reset(CkParticleSystem *ps, int32_t pool_size);
/* FUN_250820e0: one emission event */
void ck_ps_emit_event(CkContext *ctx, CkParticleSystem *ps);
/* FUN_25081af0: integrate, age, kill */
void ck_ps_update(CkContext *ctx, CkParticleSystem *ps, float dt);
/* FUN_25083540: the time-dependent trail, from the render callback */
void ck_ps_trail(CkContext *ctx, CkParticleSystem *ps);

/* Geometry for the renderer: world-space vertices (position, uv, ARGB colour), triangle lists (or line lists
   for CKPS_RENDER_LINE). cam_r / cam_u / cam_f: the viewpoint's world axes. Returns the vertex count
   written (at most cap). */
typedef struct {
    float pos[3], uv[2];
    uint32_t color;
} CkParticleVertex;
uint32_t ck_ps_geometry(const CkParticleSystem *ps, const float cam_r[3], const float cam_u[3], const float cam_f[3],
                        CkParticleVertex *out, uint32_t cap);
