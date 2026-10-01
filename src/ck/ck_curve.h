/* CK2dCurve: the value of 2DCurve parameters (GUID 20ad345d:1afb25b1), e.g. Bezier Progression's
   progression curve. Points sorted by x, Hermite segments with TCB (Kochanek-Bartels) tangents. */
#pragma once
#include "ck_file.h"
#include <stdint.h>

#define CK_GUID_2DCURVE ((CkGuid){0x20ad345du, 0x1afb25b1u})

enum { CK_CURVEPOINT_USER_TANGENTS = 1, CK_CURVEPOINT_LINEAR = 2 };

typedef struct {
    float tension, continuity, bias;
    float pos[2];
    float in[2], out[2];      /* tangents */
    uint32_t flags;           /* CK_CURVEPOINT_* */
} CkCurvePoint;

typedef struct {
    uint32_t n;
    float fitting;            /* fitting coefficient (only the arc length approximation uses it) */
    CkCurvePoint p[];
} CkCurve2d;

/* CK2dCurve::Read: a malloc'd curve (size in *size) from the parameter's sub-chunk; NULL if empty. */
CkCurve2d *ck_curve_read(const CkChunk *c, uint32_t *size);
/* CK2dCurve::GetY: y in [0,1] at x (clamped to [0,1]) */
float ck_curve_get_y(const CkCurve2d *c, float x);
