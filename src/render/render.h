/* Scene renderer on the GPU interface (gpu.h): draws the 3D entities of a CK context with their meshes,
   materials and textures, DX7 fixed-function style (WGSL). */
#pragma once
#include "../ck/ck_3d.h"

typedef struct {
    float pos[3];
    float yaw, pitch;         /* radians; yaw 0 looks along +Z (D3D left-handed, Y up) */
    float fov_y;              /* radians */
    float znear, zfar;
} Camera;

typedef struct Renderer Renderer;

Renderer *render_create(void);
void render_destroy(Renderer *r);
/* Uploads textures (decoding their files through the file layer, dir = texture folder), meshes and
   materials of every object currently in the context. Call again after loading more objects. */
void render_upload(Renderer *r, CkContext *ctx, const char *texture_dir);
/* One frame: clear, draw every visible 3D entity and 2D entity, present. cam = the free viewer camera, or
   NULL for the context's viewpoint, lights and fog (the game). */
void render_frame(Renderer *r, CkContext *ctx, const Camera *cam);
