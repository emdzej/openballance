/* 3D curves (CKCurve 43 / CKCurvePoint 36) and object animations (CKObjectAnimation 15) of CK2_3D.dll,
   as docs/animation.md describes them. */
#pragma once
#include "ck_3d.h"

typedef struct {
    Ck3dEntity e;
    CkId curve;
    float tension, continuity, bias;
    bool user_tangents, linear;   /* not_tcb (+0x1ec), the segment leaving this point is linear (+0x1f0) */
    CkVec3 in, out;               /* curve-local tangents */
    CkVec3 raw, pos;              /* curve-local position, fitted position (computed) */
    float length;                 /* arc length from the first point (computed) */
} Ck3dCurvePoint;

typedef struct {
    Ck3dEntity e;
    CkIds points;                 /* in curve order */
    bool open;
    float fitting;
    uint32_t steps;
    float length;
    bool dirty;
} CkCurve3d;

typedef struct { float t; CkVec3 v; } CkPosKey;
typedef struct { float t; float q[4]; } CkRotKey;   /* x, y, z, w */

typedef struct {
    CkObj h;                      /* a SceneObject, not a BeObject */
    uint32_t flags;               /* 4 ignore position, 8 ignore rotation, 0x80 merged */
    CkId entity;
    float length, step;           /* frames; current step 0..1 */
    CkPosKey *pos, *scale;
    uint32_t npos, nscale;
    CkRotKey *rot;
    uint32_t nrot;
    CkVec3 offset;                /* the saved vector (app data when nonzero; taken by a keyed animation) */
} CkObjectAnimation;

/* CKKeyedAnimation (18; CKAnimation load FUN_10047adc + FUN_10049e10): object animations played together */
typedef struct {
    CkObj h;
    float length;                 /* CKAnimation +0x20 */
    CkId root_entity;             /* +0x28 */
    CkIds anims;                  /* +0x34 */
    int32_t root_index;           /* +0x3c */
    float merge;                  /* +0x40 */
    CkVec3 offset;                /* +0x48, added to the root animation's position */
    bool linked;                  /* the object animations' offsets taken over */
    float step;                   /* +0x24 */
} CkKeyedAnimation;

size_t ck_anim_obj_size(uint32_t cid);   /* 0 if not one of these classes */
void ck_anim_load(CkObj *o, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L);
void ck_anim_free(CkObj *o);

/* CKCurve::GetPos (FUN_10014195): world position (and direction, may be NULL) at step 0..1; false with
   fewer than 2 points */
bool ck_curve3d_get_pos(CkContext *ctx, CkCurve3d *c, float step, CkVec3 *pos, CkVec3 *dir);
/* CKObjectAnimation::SetStep (FUN_100578ca) without merging, morphs or keyed animations: the entity's
   world matrix from the keys at frame step * length */
void ck_objanim_set_step(CkContext *ctx, CkObjectAnimation *a, float step);

/* CKKeyedAnimation::SetStep (vtable +0x64, FUN_10048366): step clamped to [0,1], then every object animation
   SetStep(step, keyed) in hierarchy order: they drive their entities' local matrices (LocalMatrixChanged:
   world = local * parent world, children follow); the root entity's animation gets the keyed offset. */
void ck_keyed_set_step(CkContext *ctx, CkKeyedAnimation *k, float step);

static inline CkKeyedAnimation *ck_keyedanim(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == 18 ? (CkKeyedAnimation *)o : NULL;
}
static inline CkCurve3d *ck_curve3d(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == 43 ? (CkCurve3d *)o : NULL;
}
static inline CkObjectAnimation *ck_objanim(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == 15 ? (CkObjectAnimation *)o : NULL;
}
