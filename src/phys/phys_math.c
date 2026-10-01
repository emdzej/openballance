/* IVP vector, matrix and quaternion helpers (docs/ivp_core.md 3 and 12, intro). The original computes in x87
   extended precision; f64 here. */
#include "phys_internal.h"

/* 0x100190c0: quaternion -> rotation (vv untouched) */
void ivp_quat_to_matrix(const IvpQuat *q, double r[3][3])
{
    double x2 = q->x + q->x, y2 = q->y + q->y, z2 = q->z + q->z;
    r[0][0] = 1.0 - (z2 * q->z + y2 * q->y);
    r[0][1] = y2 * q->x - z2 * q->w;
    r[0][2] = y2 * q->w + z2 * q->x;
    r[1][0] = z2 * q->w + y2 * q->x;
    r[1][1] = 1.0 - (z2 * q->z + x2 * q->x);
    r[1][2] = z2 * q->y - x2 * q->w;
    r[2][0] = z2 * q->x - y2 * q->w;
    r[2][1] = x2 * q->w + z2 * q->y;
    r[2][2] = 1.0 - (y2 * q->y + x2 * q->x);
}

/* 0x100191b0: rotation -> quaternion, then the cheap normalize */
void ivp_matrix_to_quat(const double r[3][3], IvpQuat *q)
{
    double t = r[0][0] + r[1][1] + r[2][2];
    if (t > 0) {
        double s = sqrt(t + 1.0);
        q->w = s * 0.5;
        s = 0.5 / s;
        q->x = (r[2][1] - r[1][2]) * s;
        q->y = (r[0][2] - r[2][0]) * s;
        q->z = (r[1][0] - r[0][1]) * s;
    } else {
        int i = r[0][0] < r[1][1] ? 1 : 0;
        if (r[i][i] < r[2][2]) i = 2;
        int j = (i + 1) % 3, k = (j + 1) % 3;
        double v[3], s = sqrt(r[i][i] - (r[k][k] + r[j][j]) + 1.0);
        v[i] = s * 0.5;
        if (s != 0) s = 0.5 / s;
        q->w = (r[k][j] - r[j][k]) * s;
        v[j] = (r[j][i] + r[i][j]) * s;
        v[k] = (r[k][i] + r[i][k]) * s;
        q->x = v[0], q->y = v[1], q->z = v[2];
    }
    ivp_quat_normalize(q);
}

/* 0x100194e0 (2.3) */
void ivp_quat_normalize(IvpQuat *q)
{
    double n2 = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    if (n2 > 1e-19) {
        double s = 1.0 / sqrt(n2);
        q->x *= s, q->y *= s, q->z *= s, q->w *= s;
    }
}

/* 0x10019540: Newton iteration of 1/sqrt around 1 */
void ivp_quat_normalize_iter(IvpQuat *q)
{
    double n = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    if (fabs(1.0 - n) <= 1e-12) return;
    double y = 1.5 - 0.5 * n;
    while (fabs(1.0 - y * y * n) > 1e-12) y = (1.0 - y * y * n) * 0.5 + y;
    q->x *= y, q->y *= y, q->z *= y, q->w *= y;
}

/* 0x1001e7c0: r = a (x) b, r may alias */
void ivp_quat_mul(IvpQuat *r, const IvpQuat *a, const IvpQuat *b)
{
    double x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
    double y = a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x;
    double z = a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w;
    double w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
    r->x = x, r->y = y, r->z = z, r->w = w;
}

/* 0x10019320 */
void ivp_quat_slerp(IvpQuat *r, const IvpQuat *a, const IvpQuat *b, double t)
{
    double d = a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w, sign = 1.0;
    if (d <= 0) d = -d, sign = -1.0;
    if (d < (double)0.999f) {
        double th = acos(d), k = 1.0 / sqrt(1.0 - d * d);
        double ca = sin((1.0 - t) * th) * k, cb = sin(t * th) * sign * k;
        double x = cb * b->x + ca * a->x, y = cb * b->y + ca * a->y, z = cb * b->z + ca * a->z, w = cb * b->w + ca * a->w;
        r->x = x, r->y = y, r->z = z, r->w = w;
    } else {
        double x = (sign * b->x - a->x) * t + a->x, y = (sign * b->y - a->y) * t + a->y, z = (sign * b->z - a->z) * t + a->z,
               w = (sign * b->w - a->w) * t + a->w;
        double n = (x * x + y * y + z * z + w * w) * 0.5, s = 1.5 - n;
        s = (0.5 - s * s * n) + s;
        s = (0.5 - s * s * n) + s;
        r->x = x * s, r->y = y * s, r->z = z * s, r->w = w * s;
    }
}

/* 0x10018f80: third-order sine of the half angle per component */
void ivp_quat_from_rot_poly(IvpQuat *q, const float w[3], double dt)
{
    double h = dt * 0.5, s[3];
    for (int i = 0; i < 3; i++) {
        s[i] = w[i] * h;
        s[i] = s[i] - s[i] * s[i] * s[i] * (double)(1.0f / 6.0f);
    }
    q->x = s[0], q->y = s[1], q->z = s[2];
    q->w = sqrt(1.0 - (s[0] * s[0] + s[1] * s[1] + s[2] * s[2]));
}

/* 0x10019010 */
void ivp_quat_from_rot_sin(IvpQuat *q, const float w[3], double dt)
{
    double h = dt * 0.5, s[3];
    for (int i = 0; i < 3; i++) s[i] = sin(w[i] * h);
    double n = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
    if (n > 1.0) {
        double k = 0.99999999 / sqrt(n);
        for (int i = 0; i < 3; i++) s[i] *= k;
    }
    q->x = s[0], q->y = s[1], q->z = s[2];
    q->w = sqrt(1.0 - (s[0] * s[0] + s[1] * s[1] + s[2] * s[2]));
}

/* the initial guess of 0x1000e120 (hi32 = (0x7ff00000 - hi32(n)) / 2 + 0x1ff00000) and 5 Newton steps */
double ivp_rsqrt(double n)
{
    uint64_t bits;
    memcpy(&bits, &n, 8);
    uint32_t hi = (uint32_t)(bits >> 32);
    uint64_t g = (uint64_t)((0x7ff00000u - hi) / 2 + 0x1ff00000u) << 32;
    double y;
    memcpy(&y, &g, 8);
    for (int i = 0; i < 5; i++) y = (0.5 - 0.5 * n * y * y + 1.0) * y;
    return y;
}

/* 0x1000e120 */
int ivp_normalize_d(double v[3])
{
    double n = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (n < 1e-19) return 0;
    double y = ivp_rsqrt(n);
    v[0] *= y, v[1] *= y, v[2] *= y;
    return 1;
}

/* 0x1000df30 */
float ivp_normalize_f(float v[3])
{
    double n = (double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2];
    if (n < 1e-19) return 0;
    double y = ivp_rsqrt(n);
    v[0] = (float)(v[0] * y), v[1] = (float)(v[1] * y), v[2] = (float)(v[2] * y);
    return (float)(n * y);
}

void ivp_mat_identity(IvpMatrix *m)
{
    memset(m, 0, sizeof *m);
    m->r[0][0] = m->r[1][1] = m->r[2][2] = 1;
}

/* 0x1000ec90: C = A B */
void ivp_mat_mul(IvpMatrix *c, const IvpMatrix *a, const IvpMatrix *b)
{
    IvpMatrix t;
    ivp_mat_rr(t.r, a->r, b->r);
    ivp_rmul(a->r, b->vv, t.vv);
    for (int k = 0; k < 3; k++) t.vv[k] += a->vv[k];
    *c = t;
}

/* 0x1000f2c0: R' = R^T, vv' = -(R^T vv) */
void ivp_mat_inverse(IvpMatrix *out, const IvpMatrix *m)
{
    IvpMatrix t;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) t.r[i][j] = m->r[j][i];
    ivp_rmul(t.r, m->vv, t.vv);
    for (int k = 0; k < 3; k++) t.vv[k] = -t.vv[k];
    *out = t;
}

/* 0x1000e8f0: general inverse by cofactors; 0 (out untouched) when |det| < eps */
int ivp_mat_inverse_general(IvpMatrix *out, const IvpMatrix *m, double eps)
{
    const double (*a)[3] = m->r;
    double c[3][3];
    c[0][0] = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    c[0][1] = a[0][2] * a[2][1] - a[0][1] * a[2][2];
    c[0][2] = a[0][1] * a[1][2] - a[0][2] * a[1][1];
    c[1][0] = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    c[1][1] = a[0][0] * a[2][2] - a[0][2] * a[2][0];
    c[1][2] = a[0][2] * a[1][0] - a[0][0] * a[1][2];
    c[2][0] = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    c[2][1] = a[0][1] * a[2][0] - a[0][0] * a[2][1];
    c[2][2] = a[0][0] * a[1][1] - a[0][1] * a[1][0];
    double det = a[0][0] * c[0][0] + a[0][1] * c[1][0] + a[0][2] * c[2][0];
    if (fabs(det) < eps) return 0;
    double id = 1.0 / det;
    IvpMatrix t;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) t.r[i][j] = c[i][j] * id;
    ivp_rmul(t.r, m->vv, t.vv);
    for (int k = 0; k < 3; k++) t.vv[k] = -t.vv[k];
    *out = t;
    return 1;
}

/* 0x1000eb60: orthonormal basis with column k = the normalized axis */
void ivp_basis(double r[3][3], const double axis[3], int k)
{
    double a[3] = {axis[0], axis[1], axis[2]};
    ivp_normalize_d(a);
    double v[3] = {a[1], a[2] - a[0], -a[1]};
    if (!ivp_normalize_d(v)) {
        v[0] = a[2], v[1] = -a[2], v[2] = a[1] - a[0];
        ivp_normalize_d(v);
    }
    double u[3] = {v[1] * a[2] - v[2] * a[1], v[2] * a[0] - v[0] * a[2], v[0] * a[1] - v[1] * a[0]};   /* v x a */
    int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
    for (int i = 0; i < 3; i++) r[i][k] = a[i], r[i][k1] = u[i], r[i][k2] = v[i];
}
