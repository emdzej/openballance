#include "ck_3d.h"
#include "../movie.h"
#include "../vfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ck_is_3dentity_class(uint32_t cid) { return ck_class_derives(cid, CKCID_3DENTITY); }

size_t ck_3d_obj_size(uint32_t cid)
{
    if (cid == CKCID_MESH || cid == 53 || cid == 54) return sizeof(CkMesh);
    if (cid == CKCID_MATERIAL) return sizeof(CkMaterial);
    if (cid == CKCID_TEXTURE) return sizeof(CkTexture);
    if (cid == CKCID_CAMERA || cid == CKCID_TARGETCAMERA) return sizeof(CkCamera);
    if (cid == CKCID_LIGHT || cid == CKCID_TARGETLIGHT) return sizeof(CkLight);
    if (cid == CKCID_SPRITE3D) return sizeof(CkSprite3D);
    if (ck_is_3dentity_class(cid)) return sizeof(Ck3dEntity);
    if (ck_is_2dentity_class(cid)) return sizeof(Ck2dEntity);
    return 0;
}

void ck_3d_free(CkObj *o)
{
    if (o->cid == CKCID_MESH) {
        CkMesh *m = (CkMesh *)o;
        free(m->verts);
        free(m->faces);
        free(m->materials.v);
        for (uint32_t i = 0; i < m->nchannels; i++) free(m->channels[i].uv);
        free(m->channels);
    } else if (o->cid == CKCID_TEXTURE) {
        CkTexture *t = (CkTexture *)o;
        for (uint32_t i = 0; i < t->nfiles; i++) free(t->files[i]);
        free(t->files);
        free(t->rgba);
        movie_close(t->movie);
    } else if (ck_is_3dentity_class(o->cid)) {
        free(((Ck3dEntity *)o)->meshes.v);
    }
}

static CkColor argb(uint32_t c)
{
    CkColor k = {(c >> 16 & 0xff) / 255.0f, (c >> 8 & 0xff) / 255.0f, (c & 0xff) / 255.0f, (c >> 24) / 255.0f};
    return k;
}

/* CKMesh load, data version >= 9 (CK2_3D.dll FUN_1002816a; vertices FUN_10027e1e) */
static void load_mesh(CkMesh *m, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (ck_seek(&r, 0x2000)) m->flags = ck_read_dword(&r);
    if (ck_seek(&r, 0x100000)) {
        int32_t n = ck_read_int(&r);
        for (int32_t i = 0; i < n && !r.error; i++) {
            CkId mat = remap(L, ck_read_object(&r));
            ck_read_int(&r);
            ck_ids_push(&m->materials, mat);
        }
    }
    if (ck_seek(&r, 0x20000)) {
        uint32_t n = ck_read_dword(&r);
        if (n && n < 1u << 24) {
            uint32_t save = ck_read_dword(&r);
            m->vertex_save_flags = save;
            ck_read_dword(&r);                     /* buffer size */
            m->verts = calloc(n, sizeof *m->verts);
            m->nverts = n;
            if (!(save & 0x10))
                for (uint32_t i = 0; i < n; i++) {
                    m->verts[i].pos.x = ck_read_float(&r);
                    m->verts[i].pos.y = ck_read_float(&r);
                    m->verts[i].pos.z = ck_read_float(&r);
                }
            if (!(save & 1)) for (uint32_t i = 0; i < n; i++) m->verts[i].diffuse = ck_read_dword(&r);
            else { uint32_t v = ck_read_dword(&r); for (uint32_t i = 0; i < n; i++) m->verts[i].diffuse = v; }
            if (!(save & 2)) for (uint32_t i = 0; i < n; i++) m->verts[i].specular = ck_read_dword(&r);
            else { uint32_t v = ck_read_dword(&r); for (uint32_t i = 0; i < n; i++) m->verts[i].specular = v; }
            if (!(save & 4))
                for (uint32_t i = 0; i < n; i++) {
                    m->verts[i].normal.x = ck_read_float(&r);
                    m->verts[i].normal.y = ck_read_float(&r);
                    m->verts[i].normal.z = ck_read_float(&r);
                }
            if (!(save & 8)) {
                for (uint32_t i = 0; i < n; i++) {
                    m->verts[i].u = ck_read_float(&r);
                    m->verts[i].v = ck_read_float(&r);
                }
            } else {
                float u = ck_read_float(&r), v = ck_read_float(&r);
                for (uint32_t i = 0; i < n; i++) m->verts[i].u = u, m->verts[i].v = v;
            }
        }
    }
    if (ck_seek(&r, 0x10000)) {
        uint32_t n = ck_read_dword(&r);
        if (n < 1u << 24) {
            m->faces = calloc(n ? n : 1, sizeof *m->faces);
            m->nfaces = n;
            for (uint32_t i = 0; i < n && !r.error; i++) {
                uint32_t a = ck_read_dword(&r), b = ck_read_dword(&r);   /* ReadDwordAsWords x2 */
                m->faces[i].i[0] = (uint16_t)a;
                m->faces[i].i[1] = (uint16_t)(a >> 16);
                m->faces[i].i[2] = (uint16_t)b;
                m->faces[i].material = (uint16_t)(b >> 16);
            }
        }
    }
    if (r.error) {
        free(m->verts);
        free(m->faces);
        m->verts = NULL;
        m->faces = NULL;
        m->nverts = m->nfaces = 0;
    }
    /* vertices saved without normals (save flag 4): CKMesh::BuildNormals (vtable +0x1b0) */
    if (m->nverts && (m->vertex_save_flags & 4)) ck_mesh_build_normals(m);
}

void ck_mesh_resize(CkMesh *m, uint32_t nverts, uint32_t nfaces)
{
    m->verts = realloc(m->verts, (nverts ? nverts : 1) * sizeof *m->verts);
    if (nverts > m->nverts) memset(m->verts + m->nverts, 0, (nverts - m->nverts) * sizeof *m->verts);
    m->nverts = nverts;
    m->faces = realloc(m->faces, (nfaces ? nfaces : 1) * sizeof *m->faces);
    if (nfaces > m->nfaces) memset(m->faces + m->nfaces, 0, (nfaces - m->nfaces) * sizeof *m->faces);
    m->nfaces = nfaces;
    m->version++;
}

int32_t ck_mesh_channel_by_material(const CkMesh *m, CkId material)
{
    for (uint32_t i = 0; i < m->nchannels; i++)
        if (m->channels[i].material == material) return (int32_t)i;
    return -1;
}

float *ck_mesh_channel_uv(CkMesh *m, int32_t c)
{
    if (c < 0 || (uint32_t)c >= m->nchannels) return NULL;
    CkMeshChannel *ch = &m->channels[c];
    if (ch->nuv != m->nverts) {
        ch->uv = realloc(ch->uv, (m->nverts ? m->nverts : 1) * 8);
        if (m->nverts > ch->nuv) memset(ch->uv + ch->nuv * 2, 0, (m->nverts - ch->nuv) * 8);
        ch->nuv = m->nverts;
    }
    return ch->uv;
}

int32_t ck_mesh_add_channel(CkMesh *m, CkId material, bool copy_uv)
{
    m->channels = realloc(m->channels, (m->nchannels + 1) * sizeof *m->channels);
    CkMeshChannel *ch = &m->channels[m->nchannels];
    memset(ch, 0, sizeof *ch);
    ch->material = material;
    ch->src_blend = VXBLEND_ZERO, ch->dst_blend = VXBLEND_SRCCOLOR;
    int32_t c = (int32_t)m->nchannels++;
    float *uv = ck_mesh_channel_uv(m, c);
    if (copy_uv)
        for (uint32_t i = 0; i < m->nverts; i++) uv[i * 2] = m->verts[i].u, uv[i * 2 + 1] = m->verts[i].v;
    ch->version = 1;
    return c;
}

void ck_mesh_remove_channel(CkMesh *m, int32_t c)
{
    if (c < 0 || (uint32_t)c >= m->nchannels) return;
    free(m->channels[c].uv);
    memmove(&m->channels[c], &m->channels[c + 1], (m->nchannels - (uint32_t)c - 1) * sizeof *m->channels);
    m->nchannels--;
}

bool ck_mesh_set_face_material(CkMesh *m, uint32_t face, CkId material)
{
    if (face >= m->nfaces) return false;
    uint32_t slot = 0;
    while (slot < m->materials.n && m->materials.v[slot] != material) slot++;
    if (slot == m->materials.n) ck_ids_push(&m->materials, material);
    if (m->faces[face].material == slot) return false;
    m->faces[face].material = (uint16_t)slot;
    m->version++;
    return true;
}

void ck_mesh_build_normals(CkMesh *m)
{
    for (uint32_t i = 0; i < m->nverts; i++) m->verts[i].normal = (CkVec3){0, 0, 0};
    for (uint32_t f = 0; f < m->nfaces; f++) {
        const uint16_t *ix = m->faces[f].i;
        if (ix[0] >= m->nverts || ix[1] >= m->nverts || ix[2] >= m->nverts) continue;
        CkVec3 a = m->verts[ix[0]].pos, b = m->verts[ix[1]].pos, c = m->verts[ix[2]].pos;
        float e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z}, e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 0) continue;
        for (int k = 0; k < 3; k++) {
            CkVec3 *v = &m->verts[ix[k]].normal;
            v->x += n[0] / len, v->y += n[1] / len, v->z += n[2] / len;
        }
    }
    for (uint32_t i = 0; i < m->nverts; i++) {
        CkVec3 *v = &m->verts[i].normal;
        float len = sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
        if (len > 0) v->x /= len, v->y /= len, v->z /= len;
    }
}

/* Material (CK2_3D.dll FUN_100655e1): identifier 0x1000 */
static void load_material(CkMaterial *m, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (!ck_seek(&r, 0x1000)) return;
    m->diffuse = argb(ck_read_dword(&r));
    m->ambient = argb(ck_read_dword(&r));
    m->specular = argb(ck_read_dword(&r));
    m->emissive = argb(ck_read_dword(&r));
    m->power = ck_read_float(&r);
    m->texture = remap(L, ck_read_object(&r));
    m->blend_color = ck_read_dword(&r);
    uint32_t modes = ck_read_dword(&r), fl = ck_read_dword(&r);
    m->texture_blend = modes & 0xf;
    m->min_filter = modes >> 4 & 0xf;
    m->mag_filter = modes >> 8 & 0xf;
    m->src_blend = modes >> 12 & 0xf;
    m->dst_blend = modes >> 16 & 0xf;
    m->shade = modes >> 20 & 0xf;
    m->fill = modes >> 24 & 0xf;
    m->address = modes >> 28;
    m->flags = fl & 0xff;
    m->z_func = fl >> 8 & 0xf;
    m->alpha_func = fl >> 16 & 0xf;
    m->alpha_ref = fl >> 24;
    if (m->shade > 2) m->shade = 2;   /* the loader clamps the shade mode (no Phong) */
}

/* Texture: slot file names under 0x10000 (count, then strings) */
static void load_texture(CkTexture *t, const CkChunk *c)
{
    CkReader r;
    ck_reader_init(&r, c);
    uint32_t n = 0;
    if (ck_seek(&r, 0x10000) && (n = ck_read_dword(&r)) <= 64) {
        t->files = calloc(n ? n : 1, sizeof *t->files);
        for (uint32_t i = 0; i < n && !r.error; i++) t->files[t->nfiles++] = strdup(ck_read_string(&r));
    }
    n = t->nfiles;
    /* CKTexture::Load (CK2_3D, the 0x2ff000 branch): a packed dword (low byte mipmap, 0x100 transparent,
       0x200 desired video format follows, bits 16..23 video format), then by the identifier's size: 12
       bytes transparent colour, current slot, video format; 8 bytes the colour (one slot or no video
       format) and then slot / video format; 4 bytes the colour (one slot, no video format) */
    if (ck_seek(&r, 0x2ff000)) {
        uint32_t size = (r.end - r.pos) * 4;
        uint32_t mix = ck_read_dword(&r);
        t->transparent = (mix & 0x100) != 0;
        t->mipmap = (mix & 0xff) != 0;
        size -= 4;
        if (size == 12) t->transparent_color = ck_read_dword(&r);
        else if (size == 8 && (n < 2 || !(mix & 0x200))) t->transparent_color = ck_read_dword(&r);
        else if (size == 4 && !(mix & 0x200) && n < 2) t->transparent_color = ck_read_dword(&r);
    }
}

/* CK3dEntity load (CK2_3D.dll FUN_1000a7b9), current format: 0x100000 = flags, moveable flags, the four
   matrix rows; 0x4000 = current mesh, mesh list */
static void load_entity(Ck3dEntity *e, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    CkReader r;
    ck_reader_init(&r, c);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) e->world[i][j] = i == j ? 1.0f : 0.0f;
    if (ck_seek(&r, 0x4000)) {
        e->mesh = remap(L, ck_read_object(&r));
        uint32_t n = ck_read_dword(&r);
        for (uint32_t i = 0; i < n && !r.error; i++) {
            CkId id = remap(L, ck_read_object(&r));
            if (id) ck_ids_push(&e->meshes, id);
        }
    }
    if (ck_seek(&r, 0x100000)) {
        e->flags = ck_read_dword(&r);
        e->moveable = ck_read_dword(&r);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 3; j++) e->world[i][j] = ck_read_float(&r);
        e->world[0][3] = e->world[1][3] = e->world[2][3] = 0;
        e->world[3][3] = 1;
        /* FUN_1000a7b9, current format: 0x10000 skips an object, 0x20000 = the parent, 0x100000 = priority */
        if (e->flags & 0x10000) ck_read_object(&r);
        if (e->flags & 0x20000) e->parent = remap(L, ck_read_object(&r));
    }
}

/* Camera, data version >= 5: 0xfc00000 = projection, fov, ortho zoom, aspect (w | h << 16), near, far.
   Older: one identifier per field (0x400000 fov, 0x800000 projection, 0x1000000 zoom, 0x2000000 aspect,
   0x4000000 planes). */
static void load_camera(CkCamera *k, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    CkReader r;
    ck_reader_init(&r, c);
    k->projection = CK_PERSPECTIVE;
    k->fov = 0.5f;
    k->ortho_zoom = 1;
    k->aspect_w = 4, k->aspect_h = 3;
    k->znear = 1, k->zfar = 4000;
    if (c->data_version < 5) {
        if (ck_seek(&r, 0x400000)) k->fov = ck_read_float(&r);
        if (ck_seek(&r, 0x800000)) k->projection = ck_read_int(&r);
        if (ck_seek(&r, 0x1000000)) k->ortho_zoom = ck_read_float(&r);
        if (ck_seek(&r, 0x2000000)) k->aspect_w = ck_read_dword(&r), k->aspect_h = ck_read_dword(&r);
        if (ck_seek(&r, 0x4000000)) k->znear = ck_read_float(&r), k->zfar = ck_read_float(&r);
    } else if (ck_seek(&r, 0xfc00000)) {
        k->projection = ck_read_int(&r);
        k->fov = ck_read_float(&r);
        k->ortho_zoom = ck_read_float(&r);
        uint32_t a = ck_read_dword(&r);
        k->aspect_w = a & 0xffff, k->aspect_h = a >> 16;
        k->znear = ck_read_float(&r);
        k->zfar = ck_read_float(&r);
    }
    if (k->e.be.h.cid == CKCID_TARGETCAMERA && ck_seek(&r, 0x10000000)) k->target = remap(L, ck_read_object(&r));
}

/* Light, data version >= 5: 0x400000 = type | flags << 8 (CK_LIGHT_*), colour (ARGB), attenuation 0, 1,
   2, range, and for spot lights hotspot, falloff, falloff shape; 0x800000 = power (default 1). A type out
   of 1..3 becomes a point light. */
static void load_light(CkLight *l, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    CkReader r;
    ck_reader_init(&r, c);
    l->type = VX_LIGHTPOINT;
    l->flags = CK_LIGHT_ACTIVE;
    l->color = (CkColor){1, 1, 1, 1};
    l->att0 = 1;
    l->range = 5000;
    l->power = 1;
    if (c->data_version >= 5) {
        if (ck_seek(&r, 0x400000)) {
            uint32_t v = ck_read_dword(&r);
            l->type = (int32_t)(v & 0xff);
            l->flags = v & 0xffffff00u;
            l->color = argb(ck_read_dword(&r));
            l->att0 = ck_read_float(&r), l->att1 = ck_read_float(&r), l->att2 = ck_read_float(&r);
            l->range = ck_read_float(&r);
            if (l->type == VX_LIGHTSPOT)
                l->hotspot = ck_read_float(&r), l->falloff = ck_read_float(&r), l->falloff_shape = ck_read_float(&r);
        }
        if (ck_seek(&r, 0x800000)) l->power = ck_read_float(&r);
    }
    if (l->type < 1 || l->type > 3) l->type = VX_LIGHTPOINT;
    if (l->e.be.h.cid == CKCID_TARGETLIGHT && ck_seek(&r, 0x80000000)) l->target = remap(L, ck_read_object(&r));
}

static void load_2dentity(Ck2dEntity *e, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    CkReader r;
    ck_reader_init(&r, c);
    e->src[2] = e->src[3] = 1;
    if (ck_seek(&r, 0x10f000)) {
        uint32_t f = ck_read_dword(&r);
        e->flags = f & 0xfff8f7ff;
        for (int i = 0; i < 4; i++) e->rect[i] = ck_read_float(&r);
        if (f & 0x10000)
            for (int i = 0; i < 4; i++) e->src[i] = ck_read_float(&r);
        else if (e->be.h.cid != 27)
            e->src[2] = e->src[3] = 0;
        if (f & 0x20000) e->zorder = ck_read_int(&r);
        if (f & 0x40000) e->material = remap(L, ck_read_object(&r));
    }
    if (e->be.h.cid == 27 && ck_seek(&r, 0x200000)) e->material = remap(L, ck_read_object(&r));
}

static void load_sprite3d(CkSprite3D *s, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    CkReader r;
    ck_reader_init(&r, c);
    /* the constructor (0x10042120): size 2 x 2, offset 0, UVs 0..1 */
    s->size[0] = s->size[1] = 2;
    s->uv[0] = s->uv[1] = 0, s->uv[2] = s->uv[3] = 1;
    if (!ck_seek(&r, 0x400000)) return;
    s->mode = ck_read_dword(&r);
    s->size[0] = ck_read_float(&r) * 2;
    s->size[1] = ck_read_float(&r) * 2;
    s->offset[0] = ck_read_float(&r);
    s->offset[1] = ck_read_float(&r);
    for (int i = 0; i < 4; i++) s->uv[i] = ck_read_float(&r);
    s->material = remap(L, ck_read_object(&r));
}

static void norm3(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
}

void ck_sprite3d_quad(CkSprite3D *s, const float cam[4][4], float pos[4][3], float uv[4][2])
{
    float (*w)[4] = s->e.world;
    if (s->mode == 0) {
        for (int i = 0; i < 3; i++) memcpy(w[i], cam[i], 12);
    } else if (s->mode == 1) {
        /* X rotate: right = X; dir = normalize(0, -cam.up.z, cam.up.y); up = normalize(0, dir.z, -dir.y) */
        float x[3] = {1, 0, 0}, d[3] = {0, -cam[1][2], cam[1][1]};
        norm3(d);
        float u[3] = {0, d[2], -d[1]};
        norm3(u);
        memcpy(w[0], x, 12), memcpy(w[2], d, 12), memcpy(w[1], u, 12);
    } else if (s->mode == 2) {
        /* Y rotate: up = Y; right = normalize(cam.dir.z, 0, -cam.dir.x); dir = normalize(-right.z, 0, right.x) */
        float y[3] = {0, 1, 0}, rr[3] = {cam[2][2], 0, -cam[2][0]};
        norm3(rr);
        float d[3] = {-rr[2], 0, rr[0]};
        norm3(d);
        memcpy(w[1], y, 12), memcpy(w[0], rr, 12), memcpy(w[2], d, 12);
    }
    /* the quad: right = X axis * width, up = Y axis * height, from the local box's minimum corner
       ((offset - 1) / 2 of the size) */
    float rt[3], up[3];
    for (int j = 0; j < 3; j++) rt[j] = w[0][j] * s->size[0], up[j] = w[1][j] * s->size[1];
    float a = (s->offset[0] - 1) * 0.5f, b = (s->offset[1] - 1) * 0.5f;
    for (int j = 0; j < 3; j++) {
        pos[0][j] = w[3][j] + rt[j] * a + up[j] * b;
        pos[1][j] = pos[0][j] + up[j];
        pos[2][j] = pos[1][j] + rt[j];
        pos[3][j] = pos[0][j] + rt[j];
    }
    uv[0][0] = s->uv[0], uv[0][1] = s->uv[3];
    uv[1][0] = s->uv[0], uv[1][1] = s->uv[1];
    uv[2][0] = s->uv[2], uv[2][1] = s->uv[1];
    uv[3][0] = s->uv[2], uv[3][1] = s->uv[3];
}

void ck_3d_load(CkObj *o, const CkChunk *c, CkId (*remap)(const void *, uint32_t), const void *L)
{
    if (ck_is_2dentity_class(o->cid)) {
        load_2dentity((Ck2dEntity *)o, c, remap, L);
        return;
    }
    if (o->cid == CKCID_MESH) load_mesh((CkMesh *)o, c, remap, L);
    else if (o->cid == CKCID_MATERIAL) load_material((CkMaterial *)o, c, remap, L);
    else if (o->cid == CKCID_TEXTURE) load_texture((CkTexture *)o, c);
    else if (ck_is_3dentity_class(o->cid)) {
        load_entity((Ck3dEntity *)o, c, remap, L);
        if (o->cid == CKCID_CAMERA || o->cid == CKCID_TARGETCAMERA) load_camera((CkCamera *)o, c, remap, L);
        if (o->cid == CKCID_LIGHT || o->cid == CKCID_TARGETLIGHT) load_light((CkLight *)o, c, remap, L);
        if (o->cid == CKCID_SPRITE3D) load_sprite3d((CkSprite3D *)o, c, remap, L);
    }
}

bool ck_texture_load_movie(CkTexture *t, const char *path)
{
    Movie *m = movie_open(path);
    if (!m) return false;
    movie_close(t->movie);
    t->movie = m;
    t->width = movie_width(m);
    t->height = movie_height(m);
    free(t->rgba);
    t->rgba = calloc((size_t)t->width * t->height, 4);
    t->current_slot = -1;
    ck_texture_set_slot(t, 0);
    return true;
}

bool ck_texture_load_image(CkTexture *t, const char *path, int32_t slot)
{
    char alt[512];
    snprintf(alt, sizeof alt, "Textures/%s", path);
    const char *p = vfs_exists(path) ? path : vfs_exists(alt) ? alt : NULL;
    if (!p || slot < 0 || slot > 64) return false;
    if ((uint32_t)slot >= t->nfiles) {
        t->files = realloc(t->files, (size_t)(slot + 1) * sizeof *t->files);
        for (uint32_t i = t->nfiles; i <= (uint32_t)slot; i++) t->files[i] = NULL;
        t->nfiles = (uint32_t)slot + 1;
    }
    free(t->files[slot]);
    t->files[slot] = strdup(p);
    t->version++;
    return true;
}

static void mat_mul4(const float a[4][4], const float b[4][4], float out[4][4])
{
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, t, sizeof t);
}

/* the inverse of an affine row-vector matrix (3x3 part inverted, translation row) */
static void mat_inverse(const float m[4][4], float out[4][4])
{
    float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], k = m[2][2];
    float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    float inv[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (det != 0) {
        inv[0][0] = (e * k - f * h) / det, inv[0][1] = (c * h - b * k) / det, inv[0][2] = (b * f - c * e) / det;
        inv[1][0] = (f * g - d * k) / det, inv[1][1] = (a * k - c * g) / det, inv[1][2] = (c * d - a * f) / det;
        inv[2][0] = (d * h - e * g) / det, inv[2][1] = (b * g - a * h) / det, inv[2][2] = (a * e - b * d) / det;
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[i][j] = inv[i][j];
        out[i][3] = 0;
    }
    for (int j = 0; j < 3; j++) out[3][j] = -(m[3][0] * inv[0][j] + m[3][1] * inv[1][j] + m[3][2] * inv[2][j]);
    out[3][3] = 1;
}

/* children by parent id: kids[start[p] .. start[p + 1]) in object order (so the walk visits entities in the
   order the full scan did); rebuilt when the hierarchy generation or the object count changed */
typedef struct {
    uint32_t gen, nobjs;
    uint32_t *start;
    CkId *kids;
} HierCache;

static const HierCache *hier_cache(const CkContext *cctx)
{
    CkContext *ctx = (CkContext *)cctx;   /* the cache is derived state */
    HierCache *h = ctx->hier_cache;
    if (h && h->gen == ctx->hier_gen && h->nobjs == ctx->nobjs) return h;
    if (!h) h = ctx->hier_cache = calloc(1, sizeof *h);
    free(h->start);
    free(h->kids);
    uint32_t n = ctx->nobjs;
    h->start = calloc(n + 2, sizeof *h->start);
    for (uint32_t i = 0; i < n; i++) {
        CkObj *o = ctx->objs[i];
        if (o && ck_is_3dentity_class(o->cid) && ((Ck3dEntity *)o)->parent && ((Ck3dEntity *)o)->parent <= n)
            h->start[((Ck3dEntity *)o)->parent + 1]++;
    }
    for (uint32_t p = 1; p <= n + 1; p++) h->start[p] += h->start[p - 1];
    h->kids = malloc((h->start[n + 1] ? h->start[n + 1] : 1) * sizeof *h->kids);
    uint32_t *fill = malloc((n + 1) * sizeof *fill);
    memcpy(fill, h->start, (n + 1) * sizeof *fill);
    for (uint32_t i = 0; i < n; i++) {
        CkObj *o = ctx->objs[i];
        if (o && ck_is_3dentity_class(o->cid) && ((Ck3dEntity *)o)->parent && ((Ck3dEntity *)o)->parent <= n)
            h->kids[fill[((Ck3dEntity *)o)->parent]++] = o->id;
    }
    free(fill);
    h->gen = ctx->hier_gen, h->nobjs = n;
    return h;
}

void ck_hier_cache_free(CkContext *ctx)
{
    HierCache *h = ctx->hier_cache;
    if (!h) return;
    free(h->start);
    free(h->kids);
    free(h);
    ctx->hier_cache = NULL;
}

void ck_entity_descendants(const CkContext *ctx, CkId id, CkIds *out)
{
    const HierCache *h = hier_cache(ctx);
    if (!id || id > h->nobjs) return;
    for (uint32_t k = h->start[id]; k < h->start[id + 1]; k++) {
        CkId c = h->kids[k];
        Ck3dEntity *e = ck_entity(ctx, c);
        if (!e || e->parent != id || ck_ids_has(out, c)) continue;
        ck_ids_push(out, c);
        ck_entity_descendants(ctx, c, out);
    }
}

void ck_entity_set_world(CkContext *ctx, Ck3dEntity *e, const float m[4][4], bool keep_children)
{
    float old[4][4], inv[4][4], delta[4][4];
    memcpy(old, e->world, sizeof old);
    memcpy(e->world, m, sizeof e->world);
    if (keep_children) return;
    /* children keep their local matrices: world' = world * inverse(old parent) * new parent */
    CkIds kids = {0};
    ck_entity_descendants(ctx, e->be.h.id, &kids);
    if (kids.n) {
        mat_inverse(old, inv);
        mat_mul4(inv, e->world, delta);
        for (uint32_t i = 0; i < kids.n; i++) {
            Ck3dEntity *c = ck_entity(ctx, kids.v[i]);
            if (c) mat_mul4(c->world, delta, c->world);
        }
    }
    free(kids.v);
}

void ck_entity_set_position(CkContext *ctx, Ck3dEntity *e, const float pos[3], CkId ref, bool keep_children)
{
    float m[4][4], p[3] = {pos[0], pos[1], pos[2]};
    Ck3dEntity *r = ck_entity(ctx, ref);
    if (r)
        for (int j = 0; j < 3; j++) p[j] = pos[0] * r->world[0][j] + pos[1] * r->world[1][j] + pos[2] * r->world[2][j] + r->world[3][j];
    memcpy(m, e->world, sizeof m);
    m[3][0] = p[0], m[3][1] = p[1], m[3][2] = p[2];
    ck_entity_set_world(ctx, e, m, keep_children);
}

void ck_entity_get_position(const CkContext *ctx, const Ck3dEntity *e, CkId ref, float out[3])
{
    const Ck3dEntity *r = ck_entity(ctx, ref);
    if (!r) {
        memcpy(out, e->world[3], 12);
        return;
    }
    float inv[4][4];
    mat_inverse(r->world, inv);
    const float *w = e->world[3];
    for (int j = 0; j < 3; j++) out[j] = w[0] * inv[0][j] + w[1] * inv[1][j] + w[2] * inv[2][j] + inv[3][j];
}

void ck_entity_local_box(const CkContext *ctx, const Ck3dEntity *e, float box[6])
{
    memset(box, 0, 24);
    const CkMesh *m = ck_mesh(ctx, e->mesh);
    if (!m || !m->nverts) return;
    for (int k = 0; k < 3; k++) box[k] = box[3 + k] = (&m->verts[0].pos.x)[k];
    for (uint32_t i = 1; i < m->nverts; i++)
        for (int k = 0; k < 3; k++) {
            float v = (&m->verts[i].pos.x)[k];
            if (v < box[k]) box[k] = v;
            if (v > box[3 + k]) box[3 + k] = v;
        }
}

void ck_entity_world_box(const CkContext *ctx, const Ck3dEntity *e, float box[6])
{
    float l[6];
    ck_entity_local_box(ctx, e, l);
    memset(box, 0, 24);
    const CkMesh *m = ck_mesh(ctx, e->mesh);
    if (!m || !m->nverts) return;
    const float *lo = l, *hi = l + 3;
    /* VxBbox::TransformFrom: centre transformed, half extents through the absolute matrix */
    for (int j = 0; j < 3; j++) {
        float c = e->world[3][j], h = 0;
        for (int k = 0; k < 3; k++) {
            c += (lo[k] + hi[k]) * 0.5f * e->world[k][j];
            h += (hi[k] - lo[k]) * 0.5f * fabsf(e->world[k][j]);
        }
        box[j] = c - h, box[3 + j] = c + h;
    }
}

void ck_entity_local_scale(const CkContext *ctx, const Ck3dEntity *e, float s[3])
{
    float m[4][4];
    memcpy(m, e->world, sizeof m);
    const Ck3dEntity *p = ck_entity(ctx, e->parent);
    if (p) {
        float inv[4][4];
        mat_inverse(p->world, inv);
        mat_mul4(e->world, inv, m);
    }
    for (int i = 0; i < 3; i++) s[i] = sqrtf(m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2]);
}

void ck_entity_scale(CkContext *ctx, Ck3dEntity *e, const float s[3], bool absolute, bool keep_children)
{
    float m[4][4];
    memcpy(m, e->world, sizeof m);
    for (int i = 0; i < 3; i++) {
        float len = sqrtf(m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2]);
        float k = absolute ? (len > 0 ? s[i] / len : 0) : s[i];
        for (int j = 0; j < 3; j++) m[i][j] *= k;
    }
    ck_entity_set_world(ctx, e, m, keep_children);
}

CkId ck_pick_2d(const CkContext *ctx, float x, float y)
{
    CkId best = 0;
    int32_t bz = 0;
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkObj *o = ctx->objs[i];
        if (!o || !ck_is_2dentity_class(o->cid) || !(o->flags & CK_OBJECT_VISIBLE)) continue;
        Ck2dEntity *e = (Ck2dEntity *)o;
        if (e->flags & CK2D_NOTPICKABLE) continue;
        float r[4];
        ck_2d_get_homogeneous_rect(e, r);
        if (x < r[0] * CK_SCREEN_W || x >= r[2] * CK_SCREEN_W || y < r[1] * CK_SCREEN_H || y >= r[3] * CK_SCREEN_H) continue;
        if (!best || e->zorder >= bz) best = o->id, bz = e->zorder;
    }
    return best;
}

void ck_3d_init(CkObj *o)
{
    if (ck_is_3dentity_class(o->cid)) {
        Ck3dEntity *e = (Ck3dEntity *)o;
        for (int i = 0; i < 4; i++) e->world[i][i] = 1;
    }
    if (ck_is_2dentity_class(o->cid)) ((Ck2dEntity *)o)->src[2] = ((Ck2dEntity *)o)->src[3] = 1;
}

uint32_t ck_texture_slot_count(const CkTexture *t)
{
    return t->movie ? movie_frames(t->movie) : (t->nfiles ? t->nfiles : 1);
}

void ck_texture_set_slot(CkTexture *t, int32_t slot)
{
    if (slot == t->current_slot) return;
    t->current_slot = slot;
    if (t->movie && slot >= 0) {
        const uint8_t *px = movie_frame(t->movie, (uint32_t)slot);
        if (px) memcpy(t->rgba, px, (size_t)t->width * t->height * 4);
        t->version++;
    }
}
