/* Building Blocks: native reimplementations of the Virtools BB DLLs, one file per original DLL.
   Each BB cites its prototype GUID and the DLL function it is ported from (re/bb_map.txt).

   The helpers below are the CKBehavior API as BBs use it. Behavior context fields of the original
   (CKBehaviorContext): Behavior, DeltaTime (ms) -> ctx->delta_ms, Context, ParameterManager,
   MessageManager. */
#pragma once
#include "../ck/ck.h"
#include "../ck/ck_types.h"
#include <string.h>

/* Installs every implemented BB into the context's registry (call before ck_load). */
void bb_register_all(CkContext *ctx);

/* Per-DLL tables */
extern const CkBBDecl *const bb_logics[];
extern const unsigned bb_logics_count;
extern const CkBBDecl *const bb_narratives[];
extern const unsigned bb_narratives_count;
extern const CkBBDecl *const bb_ttdatabase[];
extern const unsigned bb_ttdatabase_count;
extern const CkBBDecl *const bb_environment[];
extern const unsigned bb_environment_count;
extern const CkBBDecl *const bb_visuals[];
extern const unsigned bb_visuals_count;
extern const CkBBDecl *const bb_sounds[];
extern const unsigned bb_sounds_count;
extern const CkBBDecl *const bb_controllers[];
extern const unsigned bb_controllers_count;
extern const CkBBDecl *const bb_interface[];
extern const unsigned bb_interface_count;
extern const CkBBDecl *const bb_tt_gravity[];
extern const unsigned bb_tt_gravity_count;
extern const CkBBDecl *const bb_3dtransfo[];
extern const unsigned bb_3dtransfo_count;
extern const CkBBDecl *const bb_particles[];
extern const unsigned bb_particles_count;
extern const CkBBDecl *const bb_tt_toolbox[];
extern const unsigned bb_tt_toolbox_count;
extern const CkBBDecl *const bb_tt_misc[];
extern const unsigned bb_tt_misc_count;
extern const CkBBDecl *const bb_physics[];
extern const unsigned bb_physics_count;

#define BB_DECL(var, a, b, name, fn) static const CkBBDecl var = {{0x##a##u, 0x##b##u}, name, fn, NULL}
#define BB_DECL_CB(var, a, b, name, fn, cb) static const CkBBDecl var = {{0x##a##u, 0x##b##u}, name, fn, cb}

/* ---- parameters ---- */

/* Input parameter i resolved to the parameter holding its value (operations run), NULL without source. */
static inline CkParameter *bb_in(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    CkParameter *pin = i < b->pin.n ? ck_param(ctx, b->pin.v[i]) : NULL;
    if (!pin) return NULL;
    CkParameter *p = ck_param_resolve(ctx, pin);
    return p && p->kind != CKP_IN ? p : NULL;
}
/* CKBehavior::GetInputParameterValue: copies up to size bytes; leaves dst alone without a source. */
static inline bool bb_get_in(CkContext *ctx, CkBehavior *b, uint32_t i, void *dst, uint32_t size)
{
    CkParameter *p = bb_in(ctx, b, i);
    if (!p || !p->value) return false;
    memcpy(dst, p->value, p->size < size ? p->size : size);
    return true;
}
static inline CkId bb_in_object(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    CkId id = 0;
    bb_get_in(ctx, b, i, &id, 4);
    return id;
}
static inline const char *bb_in_string(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    CkParameter *p = bb_in(ctx, b, i);
    return p && p->value && p->size ? (const char *)p->value : NULL;
}
static inline CkParameter *bb_out(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    return i < b->pout.n ? ck_param(ctx, b->pout.v[i]) : NULL;
}
static inline bool bb_get_out(CkContext *ctx, CkBehavior *b, uint32_t i, void *dst, uint32_t size)
{
    CkParameter *p = bb_out(ctx, b, i);
    if (!p || !p->value) return false;
    memcpy(dst, p->value, p->size < size ? p->size : size);
    return true;
}
/* CKBehavior::SetOutputParameterValue (+ CKParameterOut::DataChanged) */
static inline void bb_set_out(CkContext *ctx, CkBehavior *b, uint32_t i, const void *v, uint32_t size)
{
    ck_output_set(ctx, b, i, v, size);
}
static inline CkParameter *bb_local(CkContext *ctx, CkBehavior *b, uint32_t i)
{
    return i < b->local.n ? ck_param(ctx, b->local.v[i]) : NULL;
}
static inline bool bb_get_local(CkContext *ctx, CkBehavior *b, uint32_t i, void *dst, uint32_t size)
{
    CkParameter *p = bb_local(ctx, b, i);
    if (!p || !p->value) return false;
    memcpy(dst, p->value, p->size < size ? p->size : size);
    return true;
}
static inline void bb_set_local(CkContext *ctx, CkBehavior *b, uint32_t i, const void *v, uint32_t size)
{
    CkParameter *p = bb_local(ctx, b, i);
    if (p) ck_param_set(p, v, size);
}

/* CKBehavior::GetTarget: the target parameter's object if the behavior has one, else the owner. */
static inline CkId bb_target(CkContext *ctx, CkBehavior *b) { return ck_behavior_target(ctx, b); }
