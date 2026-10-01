/* TT_ParticleSystems_RT.dll: the four particle system Building Blocks the game uses, one execute
   (FUN_250853a0) and one callback (FUN_25085770) for all; the emitter is src/ck/ck_particles.c.
   docs/particles.md has the details. */
#include "bb.h"
#include "../ck/ck_particles.h"
#include <stdlib.h>

static int kind_of(const CkBehavior *b)
{
    switch (b->proto.a) {
    case 0x569d2cc2: return CKPS_TIMEDEPENDENT;
    case 0x49957bfe: return CKPS_PLANAR;
    case 0x67c88f47: return CKPS_SPHERICAL;
    default: return CKPS_POINT;
    }
}

static void set_registered(CkContext *ctx, CkBehavior *b, CkParticleSystem *ps, bool on)
{
    if (ps->registered == on) return;
    ps->registered = on;
    if (on) {
        ck_ids_push(&ctx->particle_systems, b->h.id);
    } else {
        for (uint32_t i = 0; i < ctx->particle_systems.n; i++)
            if (ctx->particle_systems.v[i] == b->h.id) {
                ctx->particle_systems.v[i] = ctx->particle_systems.v[--ctx->particle_systems.n];
                break;
            }
    }
}

/* FUN_25082a00: the settings (locals 3..9) */
static void read_settings(CkContext *ctx, CkBehavior *b, CkParticleSystem *ps)
{
    int32_t pool = 100, render = 3, src = 5, dst = 2, evol = 7, var = 0x1fff;
    bb_get_local(ctx, b, 3, &pool, 4);
    bb_get_local(ctx, b, 4, &render, 4);
    bb_get_local(ctx, b, 5, &src, 4);
    bb_get_local(ctx, b, 6, &dst, 4);
    bb_get_local(ctx, b, 8, &evol, 4);
    bb_get_local(ctx, b, 9, &var, 4);
    if (pool > 0 && pool != ps->pool_size) ck_ps_reset(ps, pool);
    ps->render = render, ps->src_blend = src, ps->dst_blend = dst, ps->evolutions = evol, ps->variances = var;
}

/* FUN_25082770: the pins copied into the emitter at every execute */
static void read_pins(CkContext *ctx, CkBehavior *b, CkParticleSystem *ps)
{
    bb_get_in(ctx, b, 2, &ps->yaw_var, 4);
    bb_get_in(ctx, b, 3, &ps->pitch_var, 4);
    bb_get_in(ctx, b, 4, &ps->speed, 4);
    bb_get_in(ctx, b, 5, &ps->speed_var, 4);
    bb_get_in(ctx, b, 6, &ps->angspeed, 4);
    bb_get_in(ctx, b, 7, &ps->angspeed_var, 4);
    bb_get_in(ctx, b, 8, &ps->life, 4);
    bb_get_in(ctx, b, 9, &ps->life_var, 4);
    bb_get_in(ctx, b, 10, &ps->max_live, 4);
    bb_get_in(ctx, b, 11, &ps->emission, 4);
    bb_get_in(ctx, b, 12, &ps->emission_var, 4);
    bb_get_in(ctx, b, 13, &ps->size0, 4);
    bb_get_in(ctx, b, 14, &ps->size0_var, 4);
    bb_get_in(ctx, b, 15, &ps->size1, 4);
    bb_get_in(ctx, b, 16, &ps->size1_var, 4);
    bb_get_in(ctx, b, 23, ps->color0, 16);
    bb_get_in(ctx, b, 24, ps->color0_var, 16);
    bb_get_in(ctx, b, 25, ps->color1, 16);
    bb_get_in(ctx, b, 26, ps->color1_var, 16);
    ps->texture = bb_in_object(ctx, b, 27);
    bb_get_in(ctx, b, 28, &ps->frame0, 4);
    bb_get_in(ctx, b, 29, &ps->frame0_var, 4);
    bb_get_in(ctx, b, 30, &ps->tex_speed, 4);
    bb_get_in(ctx, b, 31, &ps->tex_speed_var, 4);
    bb_get_in(ctx, b, 32, &ps->tex_count, 4);
    bb_get_in(ctx, b, 33, &ps->tex_loop, 4);
    if (ps->kind == CKPS_TIMEDEPENDENT) {
        float rate = 0;
        bb_get_in(ctx, b, 0, &rate, 4);
        ps->trail_rate = (int32_t)rate;
    }
}

static CkParticleSystem *emitter(CkContext *ctx, CkBehavior *b)
{
    if (!b->bb_state) {
        CkParticleSystem *ps = malloc(sizeof *ps);
        ck_ps_init(ps, kind_of(b), b->owner);
        read_settings(ctx, b, ps);
        read_pins(ctx, b, ps);
        b->bb_state = ps;
    }
    return b->bb_state;
}

/* ---- the execute (FUN_250853a0): On starts emitting (EmissionTime = the delay, the render callback
   registered), Off stops emitting (the live particles finish; then Exit Off and the callback goes),
   Freeze toggles (frozen: no emission, no update, the BB stops). Emission events every Emission Delay (+-
   variance, one rand() per active frame), then the update. ---- */
static int bb_particle_system(CkContext *ctx, CkBehavior *b)
{
    CkParticleSystem *ps = emitter(ctx, b);
    ps->owner = b->owner;
    int32_t act = 0, realtime = 1;
    float dt_fixed = 20;
    bb_get_local(ctx, b, 1, &act, 4);
    if (b->pin.n > 35) {
        bb_get_in(ctx, b, 34, &realtime, 4);
        bb_get_in(ctx, b, 35, &dt_fixed, 4);
    }
    float dt = realtime ? ctx->delta_ms : dt_fixed;
    float delay = 0, delay_var = 0, et = 0;
    if (ck_input_active(ctx, b, 2)) {
        ck_activate_input(ctx, b, 2, false);
        ck_activate_output(ctx, b, 2, true);
        if (act & 2) act &= ~2, ps->emitting = true;
        else act |= 2, ps->emitting = false;
    } else if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, true);
        bb_get_in(ctx, b, 0, &delay, 4);
        bb_set_local(ctx, b, 2, &delay, 4);
        ps->emitting = true;
        act |= 1;
        set_registered(ctx, b, ps, true);
    } else if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        act = 0;
        ps->emitting = false;
    }
    bb_set_local(ctx, b, 1, &act, 4);
    if (act & 2) return CKBR_OK;
    read_pins(ctx, b, ps);
    ps->last_dt = dt;
    bb_get_local(ctx, b, 2, &et, 4);
    if (act) {
        bb_get_in(ctx, b, 0, &delay, 4);
        bb_get_in(ctx, b, 1, &delay_var, 4);
        float t = et + dt;
        float d = delay + (float)(ck_rand(ctx) - 0x3fff) * 6.1038903e-05f * delay_var;
        if (d <= 0) {
            ck_ps_emit_event(ctx, ps);
        } else {
            while (d < t) {
                t -= d;
                ck_ps_emit_event(ctx, ps);
            }
        }
        et = t;
    }
    if (ps->live) ck_ps_update(ctx, ps, dt);
    bb_set_local(ctx, b, 2, &et, 4);
    if (!act && ps->live == 0) {
        set_registered(ctx, b, ps, false);
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* FUN_25085770 */
static void cb_particle_system(CkContext *ctx, CkBehavior *b, int msg)
{
    CkParticleSystem *ps = b->bb_state;
    switch (msg) {
    case CKM_BEHAVIORATTACH:
    case CKM_BEHAVIORLOAD:
        if (ps) set_registered(ctx, b, ps, false), ck_ps_free(ps), free(ps), b->bb_state = NULL;
        emitter(ctx, b);
        {
            float zero = 0;
            bb_set_local(ctx, b, 2, &zero, 4);
        }
        break;
    case CKM_BEHAVIORDETACH:
    case CKM_BEHAVIORDELETE:
        if (ps) set_registered(ctx, b, ps, false), ck_ps_free(ps), free(ps), b->bb_state = NULL;
        break;
    case CKM_BEHAVIORPAUSE:
    case CKM_BEHAVIORDEACTIVATESCRIPT:
        if (ps) set_registered(ctx, b, ps, false);
        break;
    case CKM_BEHAVIORRESUME:
    case CKM_BEHAVIORACTIVATESCRIPT:
        if (ps && (ps->live || ps->emitting)) set_registered(ctx, b, ps, true);
        break;
    case CKM_BEHAVIORRESET:
        if (ps) ck_ps_reset(ps, ps->pool_size);
        break;
    }
}

BB_DECL_CB(d_point_ps, 506b40f7, 30852e46, "Point Particle System", bb_particle_system, cb_particle_system);
BB_DECL_CB(d_timedependent_ps, 569d2cc2, 3bcb01b9, "TT_TimedependentPointParticlesystem", bb_particle_system, cb_particle_system);
BB_DECL_CB(d_spherical_ps, 67c88f47, 8880721e, "SphericalParticleSystem", bb_particle_system, cb_particle_system);
BB_DECL_CB(d_planar_ps, 49957bfe, 0ff27ffc, "PlanarParticleSystem", bb_particle_system, cb_particle_system);

const CkBBDecl *const bb_particles[] = {&d_point_ps, &d_timedependent_ps, &d_spherical_ps, &d_planar_ps};
const unsigned bb_particles_count = sizeof bb_particles / sizeof *bb_particles;
