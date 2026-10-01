/* 3D classes of CK2_3D.dll: meshes, materials, textures and 3D entities, loaded from state chunks
   (formats in docs/ck-formats.md; verified with tools/view.py). */
#pragma once
#include "ck.h"

typedef struct { float x, y, z; } CkVec3;
typedef struct { float r, g, b, a; } CkColor;

typedef struct {
    CkVec3 pos, normal;
    float u, v;
    uint32_t diffuse, specular;   /* packed ARGB (prelit colours) */
} CkVertex;

typedef struct {
    uint16_t i[3];
    uint16_t material;            /* index into CkMesh.materials */
} CkFace;

/* A material channel (CKMesh::AddChannel): the faces drawn again with the channel's material, its own UVs and
   blend factors, after the mesh itself */
typedef struct {
    CkId material;
    float *uv;                    /* u, v per vertex */
    uint32_t nuv;                 /* vertices uv holds */
    uint8_t src_blend, dst_blend; /* VXBLEND_* */
    uint32_t version;             /* bumped by UVChanged (the renderer re-uploads the channel) */
} CkMeshChannel;

typedef struct {
    CkBeObject be;
    uint32_t flags;
    CkVertex *verts;
    uint32_t nverts;
    CkFace *faces;
    uint32_t nfaces;
    CkIds materials;              /* CkMaterial ids (0 = none) */
    uint32_t vertex_save_flags;
    uint32_t version;             /* bumped when built or changed at runtime (the renderer re-uploads) */
    CkMeshChannel *channels;
    uint32_t nchannels;
} CkMesh;

enum { VXMESH_PRELITMODE = 0x80 };
/* Building a mesh at runtime (CKMesh SetVertexCount / SetFaceCount / SetFaceMaterial ...) */
void ck_mesh_resize(CkMesh *m, uint32_t nverts, uint32_t nfaces);
/* CKMesh channels: GetChannelByMaterial (-1 if none), AddChannel (CopySrcUv: the base UVs, else zeros), RemoveChannel;
   ck_mesh_channel_uv: the channel's UVs sized to the vertex count (GetTextureCoordinatesPtr) */
int32_t ck_mesh_channel_by_material(const CkMesh *m, CkId material);
int32_t ck_mesh_add_channel(CkMesh *m, CkId material, bool copy_uv);
void ck_mesh_remove_channel(CkMesh *m, int32_t c);
float *ck_mesh_channel_uv(CkMesh *m, int32_t c);
/* CKMesh::SetFaceMaterial: the material's slot in the mesh's list (added if new); true if it changed */
bool ck_mesh_set_face_material(CkMesh *m, uint32_t face, CkId material);

/* Material (CK2_3D.dll FUN_100655e1, data version >= 5) */
enum { VXBLEND_ZERO = 1, VXBLEND_ONE = 2, VXBLEND_SRCCOLOR = 3, VXBLEND_INVSRCCOLOR = 4, VXBLEND_SRCALPHA = 5,
       VXBLEND_INVSRCALPHA = 6, VXBLEND_DESTALPHA = 7, VXBLEND_INVDESTALPHA = 8, VXBLEND_DESTCOLOR = 9,
       VXBLEND_INVDESTCOLOR = 10, VXBLEND_SRCALPHASAT = 11 };
/* flags (RCKMaterial +0xd8 low byte, as used by SetAsCurrent FUN_10064b0e) */
enum { CKMAT_TWOSIDED = 0x01, CKMAT_ZWRITE = 0x02, CKMAT_PERSPECTIVE = 0x04, CKMAT_ALPHABLEND = 0x08,
       CKMAT_ALPHATEST = 0x10 };
/* VXCMPFUNC (D3D): 1 never, 2 less, 3 equal, 4 less-equal, 5 greater, 6 not-equal, 7 greater-equal, 8 always */
typedef struct {
    CkBeObject be;
    CkColor diffuse, ambient, specular, emissive;
    float power;
    CkId texture;
    uint32_t blend_color;
    uint8_t texture_blend, min_filter, mag_filter, src_blend, dst_blend, shade, fill, address;
    uint8_t flags;                /* CKMAT_* */
    uint8_t z_func, alpha_func, alpha_ref;
    uint8_t effect;               /* bits 8..13 of the flags word: material effect (VX_EFFECT) */
} CkMaterial;

typedef struct {
    CkBeObject be;
    char **files;                 /* slot file names (external textures) */
    uint32_t nfiles;
    /* decoded image (RGBA8), filled by the texture loader */
    uint8_t *rgba;
    uint32_t width, height;
    /* movie texture (CKBitmapData::LoadMovieFile): its frames are the slots */
    struct Movie *movie;
    int32_t current_slot;
    uint32_t version;             /* bumped when rgba changes (the renderer re-uploads) */
    /* CKBitmapData transparency (CKTexture::Load, identifier 0x2ff000): texels of this colour (RGB, alpha
       ignored) get alpha 0 when the image is loaded (SetAlphaForTransparentColor) */
    bool transparent;
    uint32_t transparent_color;
    /* CKTexture::UseMipmap (+0x78; saved in the 0x2ff000 dword's low byte): mip levels are built */
    bool mipmap;
} CkTexture;

/* CKTexture::LoadMovie: false if the file isn't a supported movie. */
bool ck_texture_load_movie(CkTexture *t, const char *path);
uint32_t ck_texture_slot_count(const CkTexture *t);
/* CKTexture::LoadImage(name, slot): the slot's file (checked through the file layer: as given, then in
   Textures/); the renderer reloads the current slot. false if the file doesn't exist. */
bool ck_texture_load_image(CkTexture *t, const char *path, int32_t slot);
/* RenderContext::Pick2D: the topmost (last drawn: Z order, then id) visible, pickable 2D entity containing
   the point in render-context pixels, 0 if none */
CkId ck_pick_2d(const CkContext *ctx, float x, float y);
/* Vertex normals from the faces (normalized sum of the adjacent face normals) */
void ck_mesh_build_normals(CkMesh *m);
/* Initial state of a created object's 3D/2D part (CKContext::CreateObject) */
void ck_3d_init(CkObj *o);
/* CKBitmapData::SetCurrentSlot: movie textures decode that frame into rgba. */
void ck_texture_set_slot(CkTexture *t, int32_t slot);

/* moveable flags (VX_MOVEABLE_*) the renderer follows */
enum { VX_MOVEABLE_NOZBUFFERWRITE = 0x80000, VX_MOVEABLE_RENDERFIRST = 0x100000, VX_MOVEABLE_NOZBUFFERTEST = 0x200000 };
typedef struct {
    CkBeObject be;
    float world[4][4];            /* row vectors: [x axis, y axis, z axis, position] */
    float sky_distortion;         /* > 0: TT Sky's pre-render projection (docs/sky.md) */
    uint32_t flags, moveable;
    CkId mesh;                    /* current mesh */
    CkIds meshes;
    CkId parent;
} Ck3dEntity;

/* CKCamera (CK2_3D.dll FUN_1000f4a6; target camera FUN_10043ebc adds its target, identifier 0x10000000).
   The field of view is horizontal. */
enum { CK_PERSPECTIVE = 1, CK_ORTHOGRAPHIC = 2 };
typedef struct {
    Ck3dEntity e;
    int32_t projection;           /* CK_PERSPECTIVE / CK_ORTHOGRAPHIC */
    float fov;                    /* radians, horizontal */
    float ortho_zoom;
    uint32_t aspect_w, aspect_h;
    float znear, zfar;
    CkId target;                  /* target cameras */
} CkCamera;

/* CKLight (CK2_3D.dll FUN_1001b50e; target light FUN_1004454f adds its target, identifier 0x80000000):
   the D3D7 light it sets up each frame (FUN_1001b...: active lights only; point/spot lights only when an
   attenuation is set) has diffuse = colour * power, specular = the same when CK_LIGHT_SPECULAR. */
enum { VX_LIGHTPOINT = 1, VX_LIGHTSPOT = 2, VX_LIGHTDIREC = 3 };
enum { CK_LIGHT_ACTIVE = 0x100, CK_LIGHT_SPECULAR = 0x200 };
typedef struct {
    Ck3dEntity e;
    int32_t type;                 /* VX_LIGHT* */
    uint32_t flags;               /* CK_LIGHT_* */
    CkColor color;
    float att0, att1, att2, range;
    float hotspot, falloff, falloff_shape;   /* spot lights */
    float power;
    CkId target;                  /* target lights */
} CkLight;

/* CKSprite3D (class 37; CK2_3D.dll load 0x1004340f, identifier 0x400000): mode (0 billboard, 1 X rotate,
   2 Y rotate, 3 orientable), size (saved as half the size), offset of the quad in its plane (-1..1 units of
   the half size), UV rectangle (u0, v0, u1, v1), material */
enum { CKCID_SPRITE3D = 37 };
typedef struct {
    Ck3dEntity e;
    uint32_t mode;
    float size[2], offset[2], uv[4];
    CkId material;
} CkSprite3D;
static inline CkSprite3D *ck_sprite3d(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == CKCID_SPRITE3D ? (CkSprite3D *)o : NULL;
}
/* Before drawing: the orientation toward the viewpoint (0x10042ce8; cam = the camera's world matrix), then
   the quad's four world corners and their UVs (0x100428a8): bottom left, top left, top right, bottom right */
void ck_sprite3d_quad(CkSprite3D *s, const float cam[4][4], float pos[4][3], float uv[4][2]);

/* CK2dEntity (CK2_3D.dll FUN_1005f20d, identifier 0x10f000): flags, rectangle (homogeneous 0..1 of the
   viewport with 0x200), source UV rectangle (0x10000 in the saved flags; used with 0x20), Z order (0x20000),
   material (identifier 0x200000 for plain 2D entities). */
enum { CK2D_USESRCRECT = 0x20, CK2D_NOTPICKABLE = 0x80, CK2D_HOMOGENEOUS = 0x200, CK2D_CLIPTOCAMERA = 0x400,
       CK2D_CLIPTOPARENT = 0x1000 };
typedef struct {
    CkBeObject be;
    uint32_t flags;
    float rect[4];                /* left, top, right, bottom */
    float src[4];                 /* u0, v0, u1, v1 */
    int32_t zorder;
    CkId material;
    CkId parent;
} Ck2dEntity;

/* The render context's size in pixels (non-homogeneous 2D rectangles): the game's 640x480 mode */
enum { CK_SCREEN_W = 640, CK_SCREEN_H = 480 };
/* The rectangle as fractions of the render context (CK2dEntity::GetHomogeneousRect) */
static inline void ck_2d_get_homogeneous_rect(const Ck2dEntity *e, float r[4])
{
    float sx = (e->flags & CK2D_HOMOGENEOUS) ? 1.0f : 1.0f / CK_SCREEN_W, sy = (e->flags & CK2D_HOMOGENEOUS) ? 1.0f : 1.0f / CK_SCREEN_H;
    r[0] = e->rect[0] * sx, r[1] = e->rect[1] * sy, r[2] = e->rect[2] * sx, r[3] = e->rect[3] * sy;
}

static inline bool ck_is_2dentity_class(uint32_t cid) { return cid == 27 || cid == 28 || cid == 29; }

bool ck_is_3dentity_class(uint32_t cid);
size_t ck_3d_obj_size(uint32_t cid);      /* 0 if not a 3D class */
void ck_3d_free(CkObj *o);
/* Reads the 3D part of an object's chunk; remap turns file indices into runtime ids. */
void ck_3d_load(CkObj *o, const CkChunk *c, CkId (*remap)(const void *loader, uint32_t idx), const void *loader);

static inline CkMesh *ck_mesh(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == CKCID_MESH ? (CkMesh *)o : NULL;
}
static inline CkMaterial *ck_material(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == CKCID_MATERIAL ? (CkMaterial *)o : NULL;
}
static inline CkTexture *ck_texture(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == CKCID_TEXTURE ? (CkTexture *)o : NULL;
}
static inline CkCamera *ck_camera(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && (o->cid == CKCID_CAMERA || o->cid == CKCID_TARGETCAMERA) ? (CkCamera *)o : NULL;
}
static inline CkLight *ck_light(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && (o->cid == CKCID_LIGHT || o->cid == CKCID_TARGETLIGHT) ? (CkLight *)o : NULL;
}
static inline Ck2dEntity *ck_2dentity(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && ck_is_2dentity_class(o->cid) ? (Ck2dEntity *)o : NULL;
}
static inline Ck3dEntity *ck_entity(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && ck_is_3dentity_class(o->cid) ? (Ck3dEntity *)o : NULL;
}

/* CK3dEntity transforms. keep_children: the children keep their world matrices (else they move along, their
   local matrices kept); ref: a referential entity (0 = world). */
void ck_entity_set_world(CkContext *ctx, Ck3dEntity *e, const float m[4][4], bool keep_children);
void ck_entity_set_position(CkContext *ctx, Ck3dEntity *e, const float pos[3], CkId ref, bool keep_children);
/* GetPosition (+0x128): the world position in ref's frame (0 = world) */
void ck_entity_get_position(const CkContext *ctx, const Ck3dEntity *e, CkId ref, float out[3]);
/* GetBoundingBox(FALSE) (+0x1c0): the world box (min, max) of the current mesh's local box; zero without a mesh */
void ck_entity_world_box(const CkContext *ctx, const Ck3dEntity *e, float box[6]);
/* GetBoundingBox(TRUE): the current mesh's box (min, max) in its own frame; zero without a mesh */
void ck_entity_local_box(const CkContext *ctx, const Ck3dEntity *e, float box[6]);
/* GetScale(&s, local = TRUE) (+0x144): the axis lengths of the matrix relative to the parent */
void ck_entity_local_scale(const CkContext *ctx, const Ck3dEntity *e, float s[3]);
/* AddScale (+0x11c: axes multiplied) / SetScale (+0x140: axes set to these lengths), in local axes */
void ck_entity_scale(CkContext *ctx, Ck3dEntity *e, const float s[3], bool absolute, bool keep_children);
/* the children (entities whose parent is e), recursively in hierarchy order (CK3dEntity::HierarchyParser) */
void ck_entity_descendants(const CkContext *ctx, CkId e, CkIds *out);
void ck_hier_cache_free(CkContext *ctx);
typedef struct CkEntitySnapshot {
    float world[4][4];
    uint32_t objflags, flags, moveable;
    CkId mesh, parent;
} CkEntitySnapshot;
