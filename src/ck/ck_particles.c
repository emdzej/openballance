#include "ck_particles.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* R(x): (rand() - 0x3fff) / 16383 * x, in [-x, 1.00006 x]; U(n): trunc(rand() / 32767 * n) */
static float R(CkContext *ctx, float x) { return (float)(ck_rand(ctx) - 0x3fff) * 6.1038903e-05f * x; }
static int32_t U(CkContext *ctx, int32_t n) { return (int32_t)((float)ck_rand(ctx) * 3.0518498e-05f * (float)n); }

void ck_ps_init(CkParticleSystem *ps, int kind, CkId owner)
{
    memset(ps, 0, sizeof *ps);
    ps->kind = kind;
    ps->owner = owner;
    ps->render = CKPS_RENDER_SPRITE;
    ps->color0[0] = ps->color0[1] = 0.6f, ps->color0[2] = 1;
    ps->first_trail = true;
    ps->live_head = ps->free_head = -1;
    ck_ps_reset(ps, 100);
}

void ck_ps_free(CkParticleSystem *ps)
{
    free(ps->pool);
    ps->pool = NULL;
}

void ck_ps_reset(CkParticleSystem *ps, int32_t pool_size)
{
    if (pool_size > 0 && pool_size != ps->pool_size) {
        free(ps->pool);
        ps->pool = calloc((size_t)pool_size, sizeof *ps->pool);
        ps->pool_size = pool_size;
    }
    for (int32_t i = 0; i < ps->pool_size; i++) ps->pool[i].next = i + 1 < ps->pool_size ? i + 1 : -1;
    ps->free_head = ps->pool_size ? 0 : -1;
    ps->live_head = -1;
    ps->live = 0;
}

static CkParticle *pop_free(CkParticleSystem *ps)
{
    if (ps->free_head < 0 || ps->live >= ps->max_live) return NULL;
    int32_t i = ps->free_head;
    CkParticle *p = &ps->pool[i];
    ps->free_head = p->next;
    p->prev = -1;
    p->next = ps->live_head;
    if (ps->live_head >= 0) ps->pool[ps->live_head].prev = i;
    ps->live_head = i;
    ps->live++;
    return p;
}

static void entity_point(const Ck3dEntity *e, const float l[3], float out[3])
{
    for (int j = 0; j < 3; j++) out[j] = l[0] * e->world[0][j] + l[1] * e->world[1][j] + l[2] * e->world[2][j] + e->world[3][j];
}

static void entity_vector(const Ck3dEntity *e, const float l[3], float out[3])
{
    for (int j = 0; j < 3; j++) out[j] = l[0] * e->world[0][j] + l[1] * e->world[1][j] + l[2] * e->world[2][j];
}

static void normalize(float v[3])
{
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0) v[0] /= n, v[1] /= n, v[2] /= n;
}

/* InitPosition: 0x250830a0 point, 0x25083020 planar, 0x250830d0 spherical (which also sets the direction) */
static void init_position(CkContext *ctx, CkParticleSystem *ps, const Ck3dEntity *e, CkParticle *p, float dir[3])
{
    if (ps->kind == CKPS_PLANAR) {
        float a = R(ctx, 1), b = R(ctx, 1), l[3] = {b, a, 0};
        entity_point(e, l, p->pos);
    } else if (ps->kind == CKPS_SPHERICAL) {
        float a = R(ctx, 1), b = R(ctx, 1), c = R(ctx, 1), v[3] = {c, b, a};
        normalize(v);
        entity_point(e, v, p->pos);
        entity_vector(e, v, dir);
        normalize(dir);
    } else {
        for (int j = 0; j < 3; j++) p->pos[j] = e->world[3][j];
    }
}

/* InitDirection 0x250826a0 (point, planar, time-dependent): independent random yaw and pitch terms, not
   normalised (the frame's scale scales the speed) */
static void init_direction(CkContext *ctx, CkParticleSystem *ps, const Ck3dEntity *e, float dir[3])
{
    if (ps->kind == CKPS_SPHERICAL) return;
    float cp = cosf(R(ctx, ps->pitch_var));
    float x = sinf(R(ctx, ps->yaw_var)) * cp;
    float z = cosf(R(ctx, ps->yaw_var)) * cp;
    float y = sinf(R(ctx, ps->pitch_var));
    float l[3] = {x, y, z};
    entity_vector(e, l, dir);
}

/* the rest of a new particle (after its position and direction): speed, angle, life, colours, size, frame */
static void init_particle(CkContext *ctx, CkParticleSystem *ps, CkParticle *p, const float dir[3], bool trail)
{
    float s = ps->speed + ((ps->variances & CKPS_VAR_SPEED) ? R(ctx, ps->speed_var) : 0);
    for (int j = 0; j < 3; j++) p->vel[j] = dir[j] * s;
    int m = ps->render;
    if (m == CKPS_RENDER_LINE || m == CKPS_RENDER_ORIENTABLE || (m == 8 && !trail)) {
        p->angle = ps->angspeed, p->angspeed = ps->angspeed_var;
    } else if (m == CKPS_RENDER_SPRITE || m == CKPS_RENDER_FAST || m == CKPS_RENDER_OBJECT) {
        if (m == CKPS_RENDER_OBJECT) p->frame = U(ctx, 0);
        p->angle = 0;
        p->angspeed = (ps->variances & CKPS_VAR_ANGSPEED) ? ps->angspeed + R(ctx, ps->angspeed_var) : 0;
    } else {
        p->angle = p->angspeed = 0;
    }
    p->life = ps->life + ((ps->variances & CKPS_VAR_LIFE) ? R(ctx, ps->life_var) : 0);
    float inv = 1.0f / p->life;
    for (int c = 0; c < 4; c++)
        p->color[c] = ps->color0[c] + ((ps->variances & CKPS_VAR_COLOR0) ? R(ctx, ps->color0_var[c]) : 0);
    for (int c = 0; c < 4; c++) {
        if (ps->evolutions & CKPS_EVOL_COLOR) {
            float end = ps->color1[c] + ((ps->variances & CKPS_VAR_COLOR1) ? R(ctx, ps->color1_var[c]) : 0);
            p->dcolor[c] = (end - p->color[c]) * inv;
        } else {
            p->dcolor[c] = 0;
        }
    }
    p->size = ps->size0 + ((ps->variances & CKPS_VAR_SIZE0) ? R(ctx, ps->size0_var) : 0);
    if (ps->evolutions & CKPS_EVOL_SIZE) {
        float end = ps->size1 + ((ps->variances & CKPS_VAR_SIZE1) ? R(ctx, ps->size1_var) : 0);
        p->dsize = (end - p->size) * inv;
    } else {
        p->dsize = 0;
    }
    bool tex_modes = trail ? (m == 3 || (m >= 5 && m <= 9)) : (m == 3 || (m >= 5 && m <= 9));
    if (tex_modes) {
        p->frame = ps->frame0 + ((ps->variances & CKPS_VAR_FRAME0) ? U(ctx, ps->frame0_var) : 0);
        p->tex_acc = 0;
        bool anim = (ps->evolutions & CKPS_EVOL_TEXTURE) && (trail || ps->tex_count >= 2);
        p->tex_speed = anim ? (float)ps->tex_speed + ((ps->variances & CKPS_VAR_TEXSPEED) ? R(ctx, (float)ps->tex_speed_var) : 0) : 0;
    } else {
        p->tex_speed = 0;
    }
    p->last_dt = 0.01f;
}

void ck_ps_emit_event(CkContext *ctx, CkParticleSystem *ps)
{
    Ck3dEntity *e = ck_entity(ctx, ps->owner);
    int32_t n = ps->emission + ((ps->variances & CKPS_VAR_EMISSION) ? (int32_t)R(ctx, (float)ps->emission_var) : 0);
    for (int32_t i = 0; i < n; i++) {
        CkParticle *p = pop_free(ps);
        if (!p) break;
        float dir[3] = {0, 0, 1};
        if (e) {
            init_position(ctx, ps, e, p, dir);
            init_direction(ctx, ps, e, dir);
        }
        init_particle(ctx, ps, p, dir, false);
    }
}

void ck_ps_update(CkContext *ctx, CkParticleSystem *ps, float dt)
{
    (void)ctx;
    for (int32_t i = ps->live_head; i >= 0;) {
        CkParticle *p = &ps->pool[i];
        int32_t next = p->next;
        if (p->life <= 0) {                  /* unlink, back on the free list */
            if (p->prev >= 0) ps->pool[p->prev].next = p->next;
            else ps->live_head = p->next;
            if (p->next >= 0) ps->pool[p->next].prev = p->prev;
            p->next = ps->free_head;
            ps->free_head = i;
            ps->live--;
            i = next;
            continue;
        }
        float h = dt < p->life ? dt : p->life;
        p->angle += h * p->angspeed;
        for (int j = 0; j < 3; j++) p->pos[j] += h * p->vel[j];
        if (ps->evolutions & CKPS_EVOL_COLOR)
            for (int c = 0; c < 4; c++) {
                p->color[c] += h * p->dcolor[c];
                p->color[c] = p->color[c] < 0 ? 0 : p->color[c] > 1 ? 1 : p->color[c];
            }
        if (ps->evolutions & CKPS_EVOL_SIZE) p->size += h * p->dsize;
        if (p->tex_speed != 0 && (ps->evolutions & CKPS_EVOL_TEXTURE) && ps->render != CKPS_RENDER_OBJECT) {
            p->tex_acc += h;
            int32_t step = p->tex_speed > 0 ? 1 : -1;
            float s = fabsf(p->tex_speed);
            int32_t count = ps->tex_count;
            while (s < p->tex_acc) {
                p->frame += step;
                if (p->frame >= count) {
                    if (ps->tex_loop == 0) p->frame = count - 1;
                    else if (ps->tex_loop == 1) p->frame = 0;
                    else p->frame = count - 2, p->tex_speed = -p->tex_speed;
                } else if (p->frame < 0) {
                    if (ps->tex_loop == 0) p->frame = 0;
                    else if (ps->tex_loop == 1) p->frame = count - 1;
                    else p->frame = 1, p->tex_speed = -p->tex_speed;
                }
                p->tex_acc -= s;
            }
        }
        p->life -= h;
        p->last_dt = h;
        i = next;
    }
}

void ck_ps_trail(CkContext *ctx, CkParticleSystem *ps)
{
    if (ps->kind != CKPS_TIMEDEPENDENT || !ps->emitting) return;
    Ck3dEntity *e = ck_entity(ctx, ps->owner);
    if (!e) return;
    float k = (ps->trail_acc + ps->last_dt) * 0.001f;
    float r = (ps->variances & CKPS_VAR_EMISSION) ? (float)ps->trail_rate + R(ctx, (float)ps->emission_var) : (float)ps->trail_rate;
    if (r * k > 0.9f && !ps->first_trail) {
        int32_t n = (int32_t)(r * k);
        if (n < 1) n = 1;
        ps->trail_acc = 0;
        float cur[3] = {e->world[3][0], e->world[3][1], e->world[3][2]}, step[3];
        for (int j = 0; j < 3; j++) step[j] = (ps->trail_prev[j] - cur[j]) / (float)n;
        for (int32_t i = 0; i < n; i++) {
            CkParticle *p = pop_free(ps);
            if (!p) break;
            for (int j = 0; j < 3; j++) p->pos[j] = cur[j] + (float)i * step[j];
            float dir[3];
            init_direction(ctx, ps, e, dir);
            init_particle(ctx, ps, p, dir, true);
        }
        memcpy(ps->trail_prev, cur, sizeof cur);
    } else {
        ps->trail_acc += ps->last_dt;
        if (ps->first_trail) {
            ps->first_trail = false;
            for (int j = 0; j < 3; j++) ps->trail_prev[j] = e->world[3][j];
        }
    }
}

/* ---- geometry (the renderers 0x2508e240 sprite, 0x2508ea60 fast sprite, 0x2508f150 orientable,
   0x2508df80 line) ---- */

static uint32_t argb(const float c[4])
{
    uint32_t k = 0;
    for (int i = 0; i < 4; i++) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        k |= ((uint32_t)(int32_t)(v * 255.0f) & 0xff) << (i == 3 ? 24 : 16 - 8 * i);
    }
    return k;
}

/* atlas cell of a frame (§7.5) */
static void atlas(const CkParticleSystem *ps, int32_t f, float *u0, float *v0, float *du)
{
    if (ps->tex_count > 1) {
        int32_t n = (int32_t)sqrtf((float)(ps->tex_count - 1)) + 1;
        *du = 1.0f / (float)n;
        int32_t row = 0;
        while (f >= n) f -= n, row++;
        *u0 = (float)f * *du, *v0 = (float)row * *du;
    } else {
        *u0 = *v0 = 0, *du = 1;
    }
}

static void put(CkParticleVertex *v, const float p[3], float u, float t, uint32_t c)
{
    memcpy(v->pos, p, 12);
    v->uv[0] = u, v->uv[1] = t;
    v->color = c;
}

uint32_t ck_ps_geometry(const CkParticleSystem *ps, const float cam_r[3], const float cam_u[3], const float cam_f[3],
                        CkParticleVertex *out, uint32_t cap)
{
    uint32_t n = 0;
    for (int32_t i = ps->live_head; i >= 0; i = ps->pool[i].next) {
        const CkParticle *p = &ps->pool[i];
        uint32_t c = argb(p->color);
        float u0, v0, du;
        atlas(ps, p->frame, &u0, &v0, &du);
        if (ps->render == CKPS_RENDER_SPRITE || ps->render == CKPS_RENDER_FAST) {
            float r[3], u[3], ca = cosf(p->angle), sa = sinf(p->angle);
            for (int j = 0; j < 3; j++) {
                float rr = cam_r[j] * p->size * 0.5f, uu = cam_u[j] * p->size * 0.5f;
                r[j] = p->angle != 0 ? rr * ca - uu * sa : rr;
                u[j] = p->angle != 0 ? uu * ca + rr * sa : uu;
            }
            float a[3], b[3], d[3], e[3];
            for (int j = 0; j < 3; j++) {
                a[j] = p->pos[j] - r[j] + u[j], b[j] = p->pos[j] + r[j] + u[j];
                d[j] = p->pos[j] + r[j] - u[j], e[j] = p->pos[j] - r[j] - u[j];
            }
            if (ps->render == CKPS_RENDER_FAST) {
                if (n + 3 > cap) break;
                float top[3];
                for (int j = 0; j < 3; j++) top[j] = p->pos[j] + u[j];
                put(&out[n++], top, u0 + du / 2, v0, c);
                put(&out[n++], d, u0 + du, v0 + du, c);
                put(&out[n++], e, u0, v0 + du, c);
            } else {
                if (n + 6 > cap) break;
                put(&out[n++], a, u0, v0, c), put(&out[n++], b, u0 + du, v0, c), put(&out[n++], d, u0 + du, v0 + du, c);
                put(&out[n++], a, u0, v0, c), put(&out[n++], d, u0 + du, v0 + du, c), put(&out[n++], e, u0, v0 + du, c);
            }
        } else if (ps->render == CKPS_RENDER_ORIENTABLE) {
            if (n + 6 > cap) break;
            float t[3], h[3], d[3], s[3], c0f[4];
            for (int j = 0; j < 3; j++) t[j] = p->pos[j] - p->vel[j] * p->last_dt, h[j] = p->pos[j] + p->vel[j] * p->angle;
            for (int j = 0; j < 3; j++) d[j] = h[j] - t[j];
            normalize(d);
            s[0] = cam_f[1] * d[2] - cam_f[2] * d[1], s[1] = cam_f[2] * d[0] - cam_f[0] * d[2], s[2] = cam_f[0] * d[1] - cam_f[1] * d[0];
            normalize(s);
            float sz = p->size, s0 = p->size - p->dsize * p->last_dt;
            for (int k = 0; k < 4; k++) c0f[k] = p->color[k] - p->dcolor[k] * p->last_dt;
            uint32_t c0 = argb(c0f);
            float q0[3], q1[3], q2[3], q3[3];
            for (int j = 0; j < 3; j++) {
                q0[j] = h[j] + d[j] * sz + s[j] * sz, q1[j] = h[j] + d[j] * sz - s[j] * sz;
                q2[j] = t[j] - d[j] * s0 - s[j] * s0, q3[j] = t[j] - d[j] * s0 + s[j] * s0;
            }
            put(&out[n++], q0, u0, v0, c), put(&out[n++], q1, u0 + du, v0, c), put(&out[n++], q2, u0 + du, v0 + du, c0);
            put(&out[n++], q0, u0, v0, c), put(&out[n++], q2, u0 + du, v0 + du, c0), put(&out[n++], q3, u0, v0 + du, c0);
        } else if (ps->render == CKPS_RENDER_LINE) {
            if (n + 2 > cap) break;
            float a[3], b[3], c0f[4];
            for (int j = 0; j < 3; j++) a[j] = p->pos[j] - p->vel[j] * p->last_dt, b[j] = p->pos[j] + p->vel[j] * p->angle;
            for (int k = 0; k < 4; k++) c0f[k] = p->color[k] - p->dcolor[k] * p->last_dt;
            put(&out[n++], a, 0, 0, argb(c0f));
            put(&out[n++], b, 0, 0, c);
        }
    }
    return n;
}
