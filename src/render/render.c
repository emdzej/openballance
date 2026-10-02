#include "render.h"
#include "../ck/ck_font.h"
#include "../ck/ck_particles.h"
#include "../image.h"
#include "gpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WGSL: row-vector matrices uploaded as-is read as their transpose, so "M * v" computes v * M. */
static const char *SHADER =
    "struct Light { pos: vec4f, dir: vec4f, col: vec4f, att: vec4f };\n"
    "struct Frame { viewproj: mat4x4f, ambient: vec4f, fog_col: vec4f, fog: vec4f, lights: array<Light, 8>, eye: vec4f };\n"
    "struct Draw { world: mat4x4f, diffuse: vec4f, emissive: vec4f, params: vec4f, ambient: vec4f, vp: mat4x4f, spec: vec4f };\n"
    "@group(0) @binding(0) var<uniform> F: Frame;\n"
    "@group(0) @binding(1) var<uniform> D: Draw;\n"
    "@group(1) @binding(0) var tex: texture_2d<f32>;\n"
    "@group(1) @binding(1) var smp: sampler;\n"
    "struct VIn { @location(0) pos: vec3f, @location(1) nrm: vec3f, @location(2) uv: vec2f, @location(3) col: vec4f };\n"
    "struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f, @location(2) fogf: f32, @location(3) spec: vec3f };\n"
    /* D3D7 fixed function lighting per vertex: emissive + ambient * material ambient + for each light
       diffuse * material diffuse * N.L * attenuation (directional: none; point/spot: 1 / (a0 + a1 d + a2 d^2)
       within the range; spot: smooth falloff between the falloff and hotspot cones). Fog from the eye
       distance: linear (end - d) / (end - start), exp e^(-density d), exp2 e^(-(density d)^2). Specular (the
       material's specular colour and power, when not black / 0: D3DRS_SPECULARENABLE): per light with a
       local viewer, light colour * material specular * (N.H)^power * attenuation for lit faces, added
       after the texture stage. */
    "@vertex fn vs(v: VIn) -> VOut {\n"
    "  var o: VOut;\n"
    "  let w = D.world * vec4f(v.pos, 1.0);\n"
    "  var vp = F.viewproj;\n"
    "  if (D.params.w > 0.5) { vp = D.vp; }\n"
    "  o.pos = vp * w;\n"
    "  o.uv = v.uv;\n"
    "  var c: vec4f;\n"
    "  if (D.params.y > 0.5) {\n"
    "    let n = normalize((D.world * vec4f(v.nrm, 0.0)).xyz);\n"
    "    var acc = D.emissive.rgb + F.ambient.rgb * D.ambient.rgb;\n"
    "    let nl = i32(F.ambient.w);\n"
    "    for (var i = 0; i < nl; i++) {\n"
    "      let L = F.lights[i];\n"
    "      var ldir = -L.dir.xyz; var att = 1.0;\n"
    "      if (L.pos.w != 3.0) {\n"
    "        let d = L.pos.xyz - w.xyz; let dist = length(d);\n"
    "        ldir = d / max(dist, 0.0001);\n"
    "        att = select(0.0, 1.0 / max(L.att.x + L.att.y * dist + L.att.z * dist * dist, 0.0001), dist <= L.dir.w);\n"
    "        if (L.pos.w == 2.0) {\n"
    "          let cs = dot(-ldir, L.dir.xyz);\n"
    "          att = att * clamp((cs - L.att.w) / max(L.col.w - L.att.w, 0.0001), 0.0, 1.0);\n"
    "        }\n"
    "      }\n"
    "      let ndl = dot(n, ldir);\n"
    "      acc += L.col.rgb * D.diffuse.rgb * max(ndl, 0.0) * att;\n"
    "      if (D.spec.w > 0.0 && ndl > 0.0) {\n"
    "        let hv = normalize(ldir + normalize(F.eye.xyz - w.xyz));\n"
    "        o.spec += L.col.rgb * D.spec.rgb * pow(max(dot(n, hv), 0.0), D.spec.w) * att;\n"
    "      }\n"
    "    }\n"
    "    c = vec4f(clamp(acc, vec3f(0.0), vec3f(1.0)), D.diffuse.a);\n"
    "  } else {\n"
    "    c = v.col.zyxw;\n"
    "  }\n"
    "  o.col = c;\n"
    "  let dz = o.pos.w; let fm = i32(F.fog.z);\n"
    "  var ff = 1.0;\n"
    "  if (fm == 3) { ff = (F.fog.y - dz) / max(F.fog.y - F.fog.x, 0.001); }\n"
    "  else if (fm == 1) { ff = exp(-F.fog.w * dz); }\n"
    "  else if (fm == 2) { ff = exp(-(F.fog.w * dz) * (F.fog.w * dz)); }\n"
    "  o.fogf = clamp(ff, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n"
    "@fragment fn fs(i: VOut) -> @location(0) vec4f {\n"
    "  let t = textureSample(tex, smp, i.uv);\n"
    "  var c = i.col * t;\n"
    "  c = vec4f(min(c.rgb + i.spec, vec3f(1.0)), c.a);\n"
    "  let f = i32(D.params.z); let a = c.a; let r = D.params.x;\n"
    "  if ((f == 1) || (f == 2 && !(a < r)) || (f == 3 && a != r) || (f == 4 && a > r) ||\n"
    "      (f == 5 && !(a > r)) || (f == 6 && a == r) || (f == 7 && a < r)) { discard; }\n"
    "  if (F.fog.z > 0.5 && D.ambient.w > -0.5) { c = vec4f(mix(F.fog_col.rgb, c.rgb, i.fogf), c.a); }\n"
    "  return c;\n"
    "}\n"
    /* particles: world position, UV, ARGB colour (pre-lit), fogged */
    "@vertex fn vspart(@location(0) p: vec3f, @location(1) uv: vec2f, @location(2) col: vec4f) -> VOut {\n"
    "  var o: VOut;\n"
    "  o.pos = F.viewproj * vec4f(p, 1.0);\n"
    "  o.uv = uv;\n"
    "  o.col = col.zyxw;\n"
    "  let dz = o.pos.w; let fm = i32(F.fog.z);\n"
    "  var ff = 1.0;\n"
    "  if (fm == 3) { ff = (F.fog.y - dz) / max(F.fog.y - F.fog.x, 0.001); }\n"
    "  else if (fm == 1) { ff = exp(-F.fog.w * dz); }\n"
    "  else if (fm == 2) { ff = exp(-(F.fog.w * dz) * (F.fog.w * dz)); }\n"
    "  o.fogf = clamp(ff, 0.0, 1.0);\n"
    "  return o;\n"
    "}\n"
    /* text glyphs (2D Text): NDC position, UV, ARGB colour */
    "@vertex fn vstext(@location(0) p: vec2f, @location(1) uv: vec2f, @location(2) col: vec4f) -> VOut {\n"
    "  var o: VOut;\n"
    "  o.pos = vec4f(p, 0.0, 1.0);\n"
    "  o.uv = uv;\n"
    "  o.col = col.zyxw;\n"
    "  o.fogf = 1.0;\n"
    "  return o;\n"
    "}\n"
    /* 2D: world[0] = NDC rect (x0, y0, x1, y1), world[1] = UV rect; a quad from the vertex index */
    "@vertex fn vs2d(@builtin(vertex_index) vi: u32) -> VOut {\n"
    "  var o: VOut;\n"
    "  let r = D.world[0]; let t = D.world[1];\n"
    "  let cx = select(0.0, 1.0, vi == 1u || vi == 2u || vi == 4u);\n"
    "  let cy = select(0.0, 1.0, vi == 2u || vi == 4u || vi == 5u);\n"
    "  o.pos = vec4f(mix(r.x, r.z, cx), mix(r.y, r.w, cy), 0.0, 1.0);\n"
    "  o.uv = vec2f(mix(t.x, t.z, cx), mix(t.y, t.w, cy));\n"
    "  o.col = D.diffuse;\n"
    "  o.fogf = 1.0;\n"
    "  return o;\n"
    "}\n";

enum { MAX_DRAWS = 4096, DRAW_STRIDE = 256, VERTEX_STRIDE = 36, MAX_LIGHTS = 8, MAX_TEXT_VERTS = 6 * 8192, TEXT_STRIDE = 20, MAX_PART_VERTS = 65536,
       PART_STRIDE = 24 };

typedef struct {
    float viewproj[4][4];
    float ambient[4];         /* w = light count */
    float fog_col[4], fog[4]; /* start, end, mode, density */
    struct { float pos[4], dir[4], col[4], att[4]; } lights[MAX_LIGHTS];   /* pos.w type, dir.w range, col.w cos hotspot, att.w cos falloff */
    float eye[4];             /* the viewpoint (specular) */
} FrameUniforms;
enum { FRAME_SIZE = sizeof(FrameUniforms) };

typedef struct {
    uint32_t tex, bind_group;
    uint32_t bind_clamp;      /* with the clamp sampler (VXTEXTURE_ADDRESSCLAMP), made on first use */
    bool alpha;               /* has transparent texels (alpha test) */
    uint32_t version;         /* CkTexture.version uploaded */
    uint32_t w, h, mips;
} GpuTexture;

typedef struct {
    uint32_t first, count;    /* index range */
    uint32_t material_slot;
} Batch;

/* a material channel's vertices (the mesh's with the channel UVs) */
typedef struct {
    uint32_t vbuf, vsize;
    CkId material;
    uint32_t version;         /* CkMeshChannel.version uploaded */
} GpuChannel;

typedef struct {
    uint32_t vbuf, ibuf;
    uint32_t vsize, isize;
    Batch *batches;
    uint32_t nbatches;
    uint32_t version;         /* CkMesh.version uploaded */
    GpuChannel *channels;
    uint32_t nchannels;
} GpuMesh;

typedef struct {
    uint32_t key;
    uint32_t pipeline;
} PipelineEntry;

struct Renderer {
    uint32_t shader, layout0, layout1, frame_buf, draw_buf, bind0, sampler_wrap, sampler_clamp;
    uint32_t white_tex, white_bind;
    PipelineEntry pipes[64];
    uint32_t npipes;
    GpuTexture *textures;     /* by object id */
    GpuMesh *meshes;          /* by object id */
    uint32_t cap;
    uint8_t *draw_data;
    uint32_t uploaded;        /* objects [0, uploaded) have been looked at */
    const char *texture_dir;
    PipelineEntry pipes2d[16];
    uint32_t npipes2d;
    uint32_t text_pipe, text_vbuf;
    uint8_t *text_verts;
    PipelineEntry pipes_part[16];
    uint32_t npipes_part;
    uint32_t part_vbuf;
    CkParticleVertex *part_verts;
};

static void ensure_cap(Renderer *r, uint32_t n)
{
    if (n <= r->cap) return;
    uint32_t cap = n + 1024;
    r->textures = realloc(r->textures, cap * sizeof *r->textures);
    r->meshes = realloc(r->meshes, cap * sizeof *r->meshes);
    memset(r->textures + r->cap, 0, (cap - r->cap) * sizeof *r->textures);
    memset(r->meshes + r->cap, 0, (cap - r->cap) * sizeof *r->meshes);
    r->cap = cap;
}

Renderer *render_create(void)
{
    Renderer *r = calloc(1, sizeof *r);
    r->shader = gpu_create_shader(SHADER);
    r->layout0 = gpu_create_bind_group_layout(
        "{\"entries\":[{\"binding\":0,\"visibility\":3,\"buffer\":{\"type\":\"uniform\"}},"
        "{\"binding\":1,\"visibility\":3,\"buffer\":{\"type\":\"uniform\",\"hasDynamicOffset\":true,\"minBindingSize\":208}}]}");
    r->layout1 = gpu_create_bind_group_layout(
        "{\"entries\":[{\"binding\":0,\"visibility\":2,\"texture\":{\"sampleType\":\"float\",\"viewDimension\":\"2d\"}},"
        "{\"binding\":1,\"visibility\":2,\"sampler\":{\"type\":\"filtering\"}}]}");
    r->frame_buf = gpu_create_buffer(FRAME_SIZE, GPU_USAGE_UNIFORM);
    r->text_vbuf = gpu_create_buffer(MAX_TEXT_VERTS * TEXT_STRIDE, GPU_USAGE_VERTEX);
    r->text_verts = malloc(MAX_TEXT_VERTS * TEXT_STRIDE);
    r->part_vbuf = gpu_create_buffer(MAX_PART_VERTS * PART_STRIDE, GPU_USAGE_VERTEX);
    r->part_verts = malloc(MAX_PART_VERTS * sizeof *r->part_verts);
    r->draw_buf = gpu_create_buffer(MAX_DRAWS * DRAW_STRIDE, GPU_USAGE_UNIFORM);
    char json[512];
    snprintf(json, sizeof json,
             "{\"layout\":%u,\"entries\":[{\"binding\":0,\"buffer\":%u,\"offset\":0,\"size\":%u},"
             "{\"binding\":1,\"buffer\":%u,\"offset\":0,\"size\":208}]}",
             r->layout0, r->frame_buf, (unsigned)FRAME_SIZE, r->draw_buf);
    r->bind0 = gpu_create_bind_group(json);
    r->sampler_wrap = gpu_create_sampler("{\"addressModeU\":\"repeat\",\"addressModeV\":\"repeat\",\"magFilter\":\"linear\","
                                         "\"minFilter\":\"linear\",\"mipmapFilter\":\"linear\"}");
    r->sampler_clamp = gpu_create_sampler("{\"addressModeU\":\"clamp-to-edge\",\"addressModeV\":\"clamp-to-edge\","
                                          "\"magFilter\":\"linear\",\"minFilter\":\"linear\",\"mipmapFilter\":\"linear\"}");
    r->white_tex = gpu_create_texture("{\"size\":[1,1],\"format\":\"rgba8unorm\",\"mipLevelCount\":1}");
    uint8_t white[4] = {255, 255, 255, 255};
    gpu_write_texture(r->white_tex, 0, 0, 0, 1, 1, white);
    snprintf(json, sizeof json, "{\"layout\":%u,\"entries\":[{\"binding\":0,\"texture\":%u},{\"binding\":1,\"sampler\":%u}]}",
             r->layout1, r->white_tex, r->sampler_wrap);
    r->white_bind = gpu_create_bind_group(json);
    r->draw_data = calloc(MAX_DRAWS, DRAW_STRIDE);
    return r;
}

void render_destroy(Renderer *r)
{
    if (!r) return;
    for (uint32_t i = 0; i < r->cap; i++) free(r->meshes[i].batches), free(r->meshes[i].channels);
    free(r->textures);
    free(r->meshes);
    free(r->draw_data);
    free(r->text_verts);
    free(r->part_verts);
    free(r);
}

/* ---- pipelines by render state ---- */

static const char *blend_factor(uint8_t b)
{
    switch (b) {
    case VXBLEND_ZERO: return "zero";
    case VXBLEND_ONE: return "one";
    case VXBLEND_SRCCOLOR: return "src";
    case VXBLEND_INVSRCCOLOR: return "one-minus-src";
    case VXBLEND_SRCALPHA: return "src-alpha";
    case VXBLEND_INVSRCALPHA: return "one-minus-src-alpha";
    case VXBLEND_DESTALPHA: return "dst-alpha";
    case VXBLEND_INVDESTALPHA: return "one-minus-dst-alpha";
    case VXBLEND_DESTCOLOR: return "dst";
    case VXBLEND_INVDESTCOLOR: return "one-minus-dst";
    case VXBLEND_SRCALPHASAT: return "src-alpha-saturated";
    }
    return "one";
}

static const char *compare_fn(uint8_t f)
{
    static const char *names[] = {"always", "never", "less", "equal", "less-equal", "greater", "not-equal", "greater-equal", "always"};
    return f <= 8 ? names[f] : "less-equal";
}

/* Render state of RCKMaterial::SetAsCurrent (FUN_10064b0e): cull none if two-sided else CCW (= D3D's
   clockwise front faces), Z write, Z compare, alpha blending with the material's factors. */
static uint32_t pipeline_for(Renderer *r, bool blend, uint8_t src, uint8_t dst, bool zwrite, bool two_sided, uint8_t zfunc)
{
    if (!blend) src = VXBLEND_ONE, dst = VXBLEND_ZERO;
    uint32_t key = src | dst << 4 | (uint32_t)zwrite << 8 | (uint32_t)two_sided << 9 | (uint32_t)blend << 10 | (uint32_t)zfunc << 11;
    for (uint32_t i = 0; i < r->npipes; i++)
        if (r->pipes[i].key == key) return r->pipes[i].pipeline;
    char blendj[256] = "";
    if (blend)
        snprintf(blendj, sizeof blendj,
                 ",\"blend\":{\"color\":{\"srcFactor\":\"%s\",\"dstFactor\":\"%s\",\"operation\":\"add\"},"
                 "\"alpha\":{\"srcFactor\":\"%s\",\"dstFactor\":\"%s\",\"operation\":\"add\"}}",
                 blend_factor(src), blend_factor(dst), blend_factor(src), blend_factor(dst));
    char json[2048];
    snprintf(json, sizeof json,
             "{\"layout\":[%u,%u],"
             "\"vertex\":{\"module\":%u,\"entryPoint\":\"vs\",\"buffers\":[{\"arrayStride\":%d,\"attributes\":["
             "{\"format\":\"float32x3\",\"offset\":0,\"shaderLocation\":0},"
             "{\"format\":\"float32x3\",\"offset\":12,\"shaderLocation\":1},"
             "{\"format\":\"float32x2\",\"offset\":24,\"shaderLocation\":2},"
             "{\"format\":\"unorm8x4\",\"offset\":32,\"shaderLocation\":3}]}]},"
             "\"fragment\":{\"module\":%u,\"entryPoint\":\"fs\",\"targets\":[{\"format\":\"surface\"%s}]},"
             "\"primitive\":{\"topology\":\"triangle-list\",\"frontFace\":\"cw\",\"cullMode\":\"%s\"},"
             "\"depthStencil\":{\"format\":\"depth24plus\",\"depthWriteEnabled\":%s,\"depthCompare\":\"%s\"}}",
             r->layout0, r->layout1, r->shader, VERTEX_STRIDE, r->shader, blendj, two_sided ? "none" : "back",
             zwrite ? "true" : "false", compare_fn(zfunc));
    uint32_t p = gpu_create_pipeline(json);
    if (r->npipes < sizeof r->pipes / sizeof *r->pipes) r->pipes[r->npipes++] = (PipelineEntry){key, p};
    return p;
}

/* ---- uploads ---- */

static uint32_t mip_count(uint32_t w, uint32_t h)
{
    uint32_t n = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

static void bind_texture(Renderer *r, GpuTexture *g)
{
    char bj[256];
    snprintf(bj, sizeof bj, "{\"layout\":%u,\"entries\":[{\"binding\":0,\"texture\":%u},{\"binding\":1,\"sampler\":%u}]}",
             r->layout1, g->tex, r->sampler_wrap);
    g->bind_group = gpu_create_bind_group(bj);
}

/* Textures whose pixels live in the CkTexture (movies): one level, re-uploaded when they change. */
static void upload_dynamic_texture(Renderer *r, CkTexture *t)
{
    GpuTexture *g = &r->textures[t->be.h.id];
    if (!t->rgba || !t->width) return;
    if (!g->tex) {
        char json[128];
        snprintf(json, sizeof json, "{\"size\":[%u,%u],\"format\":\"rgba8unorm\",\"mipLevelCount\":1}", t->width, t->height);
        g->tex = gpu_create_texture(json);
        bind_texture(r, g);
        g->version = t->version - 1;
    }
    if (g->version != t->version) {
        gpu_write_texture(g->tex, 0, 0, 0, t->width, t->height, t->rgba);
        g->version = t->version;
    }
}

static void upload_texture(Renderer *r, CkTexture *t, const char *dir)
{
    GpuTexture *g = &r->textures[t->be.h.id];
    if (t->movie) {
        upload_dynamic_texture(r, t);
        return;
    }
    /* the current slot's file: loaded once, again when CKTexture::LoadImage changed it (version) */
    if ((g->tex && g->version == t->version) || !t->nfiles) return;
    g->version = t->version;
    uint32_t slot = t->current_slot > 0 && (uint32_t)t->current_slot < t->nfiles ? (uint32_t)t->current_slot : 0;
    if (!t->files[slot]) return;
    Image img;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, t->files[slot]);
    if (!image_load(t->files[slot], &img) && !image_load(path, &img)) return;
    /* CKBitmapData::SetAlphaForTransparentColor (CK2 0x24030617): the transparent colour's texels (RGB
       compared) get alpha 0, the alpha test then drops them */
    if (t->transparent)
        for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
            uint8_t *px = img.rgba + i * 4;
            uint32_t c = t->transparent_color;
            if (px[0] == ((c >> 16) & 0xff) && px[1] == ((c >> 8) & 0xff) && px[2] == (c & 0xff)) px[3] = 0;
        }
    bool alpha = false;
    for (size_t i = 0; i < (size_t)img.w * img.h; i++)
        if (img.rgba[i * 4 + 3] < 255) { alpha = true; break; }
    uint32_t mips = t->mipmap ? mip_count(img.w, img.h) : 1;
    bool fresh = !g->tex || g->w != img.w || g->h != img.h || g->mips != mips;
    if (fresh) {                   /* (gasm:gfx has no destroy: a replaced texture of another size leaks) */
        char json[128];
        snprintf(json, sizeof json, "{\"size\":[%u,%u],\"format\":\"rgba8unorm\",\"mipLevelCount\":%u}", img.w, img.h, mips);
        g->tex = gpu_create_texture(json);
        g->w = img.w, g->h = img.h, g->mips = mips;
    }
    g->alpha = alpha;
    Image cur = img;
    for (uint32_t m = 0; m < mips; m++) {
        gpu_write_texture(g->tex, m, 0, 0, cur.w, cur.h, cur.rgba);
        Image next;
        if (m + 1 < mips && image_downsample(&cur, &next)) {
            if (m) image_free(&cur);
            cur = next;
        }
    }
    if (cur.rgba != img.rgba) image_free(&cur);
    image_free(&img);
    if (fresh) bind_texture(r, g);
}

static uint32_t texture_bind_clamp(Renderer *r, GpuTexture *g)
{
    if (!g->bind_clamp && g->tex) {
        char bj[256];
        snprintf(bj, sizeof bj, "{\"layout\":%u,\"entries\":[{\"binding\":0,\"texture\":%u},{\"binding\":1,\"sampler\":%u}]}",
                 r->layout1, g->tex, r->sampler_clamp);
        g->bind_clamp = gpu_create_bind_group(bj);
    }
    return g->bind_clamp;
}

static void fill_vertices(const CkMesh *m, const float *uv, uint8_t *vb)
{
    for (uint32_t i = 0; i < m->nverts; i++) {
        const CkVertex *v = &m->verts[i];
        float f[8] = {v->pos.x, v->pos.y, v->pos.z, v->normal.x, v->normal.y, v->normal.z, uv ? uv[i * 2] : v->u, uv ? uv[i * 2 + 1] : v->v};
        memcpy(vb + i * VERTEX_STRIDE, f, 32);
        memcpy(vb + i * VERTEX_STRIDE + 32, &v->diffuse, 4);
    }
}

/* the mesh's material channels (CkMesh.channels) as vertex buffers, re-uploaded when their UVs change */
static void upload_channels(Renderer *r, CkMesh *m)
{
    GpuMesh *g = &r->meshes[m->be.h.id];
    if (g->nchannels < m->nchannels) {
        g->channels = realloc(g->channels, m->nchannels * sizeof *g->channels);
        memset(g->channels + g->nchannels, 0, (m->nchannels - g->nchannels) * sizeof *g->channels);
        g->nchannels = m->nchannels;
    }
    for (uint32_t c = 0; c < m->nchannels; c++) {
        CkMeshChannel *ch = &m->channels[c];
        GpuChannel *gc = &g->channels[c];
        if (gc->vbuf && gc->material == ch->material && gc->version == ch->version) continue;
        if (!ch->uv || ch->nuv < m->nverts || !m->nverts) continue;
        uint32_t vsize = (m->nverts * VERTEX_STRIDE + 3) & ~3u;
        uint8_t *vb = calloc(1, vsize);
        fill_vertices(m, ch->uv, vb);
        if (!gc->vbuf || gc->vsize < vsize) gc->vbuf = gpu_create_buffer(vsize, GPU_USAGE_VERTEX), gc->vsize = vsize;
        gpu_write_buffer(gc->vbuf, 0, vb, vsize);
        free(vb);
        gc->material = ch->material, gc->version = ch->version;
    }
}

static int by_material(const void *a, const void *b)
{
    return (int)((const CkFace *)a)->material - (int)((const CkFace *)b)->material;
}

static void upload_mesh(Renderer *r, CkMesh *m)
{
    GpuMesh *g = &r->meshes[m->be.h.id];
    if ((g->vbuf && g->version == m->version) || !m->nverts || !m->nfaces) return;
    g->version = m->version;
    g->nbatches = 0;
    uint32_t vsize = (m->nverts * VERTEX_STRIDE + 3) & ~3u;
    uint8_t *vb = calloc(1, vsize);
    fill_vertices(m, NULL, vb);
    for (uint32_t c = 0; c < g->nchannels; c++) g->channels[c].version = 0;   /* vertices moved: channels follow */
    /* a changed mesh is rewritten in place when it fits (gasm:gfx can't free buffers) */
    if (!g->vbuf || g->vsize < vsize) g->vbuf = gpu_create_buffer(vsize, GPU_USAGE_VERTEX), g->vsize = vsize;
    gpu_write_buffer(g->vbuf, 0, vb, vsize);
    free(vb);
    CkFace *faces = malloc(m->nfaces * sizeof *faces);
    memcpy(faces, m->faces, m->nfaces * sizeof *faces);
    qsort(faces, m->nfaces, sizeof *faces, by_material);
    uint32_t isize = (m->nfaces * 6 + 3) & ~3u;
    uint16_t *ib = calloc(1, isize);
    for (uint32_t i = 0; i < m->nfaces; i++) {
        ib[i * 3] = faces[i].i[0];
        ib[i * 3 + 1] = faces[i].i[1];
        ib[i * 3 + 2] = faces[i].i[2];
        if (!g->nbatches || g->batches[g->nbatches - 1].material_slot != faces[i].material) {
            g->batches = realloc(g->batches, (g->nbatches + 1) * sizeof *g->batches);
            g->batches[g->nbatches++] = (Batch){i * 3, 0, faces[i].material};
        }
        g->batches[g->nbatches - 1].count += 3;
    }
    if (!g->ibuf || g->isize < isize) g->ibuf = gpu_create_buffer(isize, GPU_USAGE_INDEX), g->isize = isize;
    gpu_write_buffer(g->ibuf, 0, ib, isize);
    free(ib);
    free(faces);
}

void render_upload(Renderer *r, CkContext *ctx, const char *texture_dir)
{
    ensure_cap(r, ctx->nobjs + 1);
    r->texture_dir = texture_dir;
    for (uint32_t i = r->uploaded; i < ctx->nobjs; i++) {
        CkObj *o = ctx->objs[i];
        if (!o) continue;
        if (o->cid == CKCID_TEXTURE) upload_texture(r, (CkTexture *)o, texture_dir);
        else if (o->cid == CKCID_MESH) upload_mesh(r, (CkMesh *)o);
    }
    r->uploaded = ctx->nobjs;
}

static uint32_t pipeline_2d(Renderer *r, bool blend, uint8_t src, uint8_t dst)
{
    if (!blend) src = VXBLEND_ONE, dst = VXBLEND_ZERO;
    uint32_t key = src | dst << 4 | (uint32_t)blend << 8;
    for (uint32_t i = 0; i < r->npipes2d; i++)
        if (r->pipes2d[i].key == key) return r->pipes2d[i].pipeline;
    char blendj[256] = "";
    if (blend)
        snprintf(blendj, sizeof blendj,
                 ",\"blend\":{\"color\":{\"srcFactor\":\"%s\",\"dstFactor\":\"%s\",\"operation\":\"add\"},"
                 "\"alpha\":{\"srcFactor\":\"%s\",\"dstFactor\":\"%s\",\"operation\":\"add\"}}",
                 blend_factor(src), blend_factor(dst), blend_factor(src), blend_factor(dst));
    char json[1024];
    snprintf(json, sizeof json,
             "{\"layout\":[%u,%u],\"vertex\":{\"module\":%u,\"entryPoint\":\"vs2d\"},"
             "\"fragment\":{\"module\":%u,\"entryPoint\":\"fs\",\"targets\":[{\"format\":\"surface\"%s}]},"
             "\"primitive\":{\"topology\":\"triangle-list\",\"cullMode\":\"none\"}}",
             r->layout0, r->layout1, r->shader, r->shader, blendj);
    uint32_t p = gpu_create_pipeline(json);
    if (r->npipes2d < sizeof r->pipes2d / sizeof *r->pipes2d) r->pipes2d[r->npipes2d++] = (PipelineEntry){key, p};
    return p;
}

/* The font's text states (FUN_25391a30): alpha blended SRCALPHA / INVSRCALPHA, no depth, vertex colour
   times the texture */
static uint32_t pipeline_text(Renderer *r)
{
    if (r->text_pipe) return r->text_pipe;
    char json[1536];
    snprintf(json, sizeof json,
             "{\"layout\":[%u,%u],"
             "\"vertex\":{\"module\":%u,\"entryPoint\":\"vstext\",\"buffers\":[{\"arrayStride\":%d,\"attributes\":["
             "{\"format\":\"float32x2\",\"offset\":0,\"shaderLocation\":0},"
             "{\"format\":\"float32x2\",\"offset\":8,\"shaderLocation\":1},"
             "{\"format\":\"unorm8x4\",\"offset\":16,\"shaderLocation\":2}]}]},"
             "\"fragment\":{\"module\":%u,\"entryPoint\":\"fs\",\"targets\":[{\"format\":\"surface\","
             "\"blend\":{\"color\":{\"srcFactor\":\"src-alpha\",\"dstFactor\":\"one-minus-src-alpha\",\"operation\":\"add\"},"
             "\"alpha\":{\"srcFactor\":\"src-alpha\",\"dstFactor\":\"one-minus-src-alpha\",\"operation\":\"add\"}}}]},"
             "\"primitive\":{\"topology\":\"triangle-list\",\"cullMode\":\"none\"}}",
             r->layout0, r->layout1, r->shader, TEXT_STRIDE, r->shader);
    r->text_pipe = gpu_create_pipeline(json);
    return r->text_pipe;
}

/* particle render states (FUN_25082ed0): the setting's blend factors, alpha blending (so no z-write) unless
   dst is ZERO with src ONE / SRCCOLOR / SRCALPHA, z test as the context, cull none; lines use line lists */
static uint32_t pipeline_particles(Renderer *r, uint8_t src, uint8_t dst, bool lines)
{
    bool opaque = dst == VXBLEND_ZERO && (src == VXBLEND_ONE || src == VXBLEND_SRCCOLOR || src == VXBLEND_SRCALPHA);
    uint32_t key = src | dst << 4 | (uint32_t)lines << 8;
    for (uint32_t i = 0; i < r->npipes_part; i++)
        if (r->pipes_part[i].key == key) return r->pipes_part[i].pipeline;
    char blendj[256] = "";
    if (!opaque)
        snprintf(blendj, sizeof blendj,
                 ",\"blend\":{\"color\":{\"srcFactor\":\"%s\",\"dstFactor\":\"%s\",\"operation\":\"add\"},"
                 "\"alpha\":{\"srcFactor\":\"%s\",\"dstFactor\":\"%s\",\"operation\":\"add\"}}",
                 blend_factor(src), blend_factor(dst), blend_factor(src), blend_factor(dst));
    char json[2048];
    snprintf(json, sizeof json,
             "{\"layout\":[%u,%u],"
             "\"vertex\":{\"module\":%u,\"entryPoint\":\"vspart\",\"buffers\":[{\"arrayStride\":%d,\"attributes\":["
             "{\"format\":\"float32x3\",\"offset\":0,\"shaderLocation\":0},"
             "{\"format\":\"float32x2\",\"offset\":12,\"shaderLocation\":1},"
             "{\"format\":\"unorm8x4\",\"offset\":20,\"shaderLocation\":2}]}]},"
             "\"fragment\":{\"module\":%u,\"entryPoint\":\"fs\",\"targets\":[{\"format\":\"surface\"%s}]},"
             "\"primitive\":{\"topology\":\"%s\",\"cullMode\":\"none\"},"
             "\"depthStencil\":{\"format\":\"depth24plus\",\"depthWriteEnabled\":%s,\"depthCompare\":\"less-equal\"}}",
             r->layout0, r->layout1, r->shader, PART_STRIDE, r->shader, blendj, lines ? "line-list" : "triangle-list",
             opaque ? "true" : "false");
    uint32_t p = gpu_create_pipeline(json);
    if (r->npipes_part < sizeof r->pipes_part / sizeof *r->pipes_part) r->pipes_part[r->npipes_part++] = (PipelineEntry){key, p};
    return p;
}

typedef struct {
    Ck2dEntity *e;
} Sorted2d;

static int by_zorder(const void *a, const void *b)
{
    const Ck2dEntity *x = ((const Sorted2d *)a)->e, *y = ((const Sorted2d *)b)->e;
    if (x->zorder != y->zorder) return x->zorder < y->zorder ? -1 : 1;
    return x->be.h.id < y->be.h.id ? -1 : x->be.h.id > y->be.h.id;
}

/* ---- frame ---- */

static void mat_mul(float out[4][4], const float a[4][4], const float b[4][4])
{
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i][k] * b[k][j];
            t[i][j] = s;
        }
    memcpy(out, t, sizeof t);
}

/* D3D left-handed view from an orthonormal basis (row vectors: x, y, z axes, position) and perspective
   with depth 0..1; fx, fy: 1 / tan(half field of view) horizontally and vertically */
static void view_proj_basis(const float x[3], const float y[3], const float z[3], const float pos[3], float fx,
                            float fy, float znear, float zfar, float out[4][4])
{
    float v[4][4] = {{x[0], y[0], z[0], 0}, {x[1], y[1], z[1], 0}, {x[2], y[2], z[2], 0}, {0, 0, 0, 1}};
    for (int j = 0; j < 3; j++) {
        const float *axis = j == 0 ? x : j == 1 ? y : z;
        v[3][j] = -(pos[0] * axis[0] + pos[1] * axis[1] + pos[2] * axis[2]);
    }
    float q = zfar / (zfar - znear);
    float p[4][4] = {{fx, 0, 0, 0}, {0, fy, 0, 0}, {0, 0, q, 1}, {0, 0, -q * znear, 0}};
    mat_mul(out, v, p);
}

static void normalize3(float v[3])
{
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0) v[0] /= n, v[1] /= n, v[2] /= n;
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1], out[1] = a[2] * b[0] - a[0] * b[2], out[2] = a[0] * b[1] - a[1] * b[0];
}

/* The free-flying viewer camera (yaw/pitch, vertical field of view) */
static void view_proj(const Camera *c, float aspect, float out[4][4])
{
    float cy = cosf(c->yaw), sy = sinf(c->yaw), cp = cosf(c->pitch), sp = sinf(c->pitch);
    float z[3] = {sy * cp, -sp, cy * cp};
    float x[3] = {cy, 0, -sy};
    float y[3];
    cross3(z, x, y);
    float fy = 1.0f / tanf(c->fov_y / 2);
    view_proj_basis(x, y, z, c->pos, fy / aspect, fy, c->znear, c->zfar, out);
}

/* The render context's viewpoint: a camera entity (horizontal field of view, like CKCamera); target
   cameras look at their target with the world's Y up (CKTargetCamera). */
static void camera_basis(const CkContext *ctx, const CkCamera *k, float x[3], float y[3], float z[3], float pos[3])
{
    const Ck3dEntity *e = &k->e;
    for (int i = 0; i < 3; i++) x[i] = e->world[0][i], y[i] = e->world[1][i], z[i] = e->world[2][i], pos[i] = e->world[3][i];
    const CkObj *to = ck_obj(ctx, k->target);
    const Ck3dEntity *t = to && ck_is_3dentity_class(to->cid) ? (const Ck3dEntity *)to : NULL;
    if (t) {
        float up[3] = {0, 1, 0};
        for (int i = 0; i < 3; i++) z[i] = t->world[3][i] - pos[i];
        normalize3(z);
        cross3(up, z, x);
        normalize3(x);
        cross3(z, x, y);
    } else {
        normalize3(x), normalize3(y), normalize3(z);
    }
}

static void camera_view_proj(const CkContext *ctx, const CkCamera *k, float aspect, float out[4][4])
{
    float x[3], y[3], z[3], pos[3];
    camera_basis(ctx, k, x, y, z, pos);
    float fx = 1.0f / tanf(k->fov / 2);
    view_proj_basis(x, y, z, pos, fx, fx * aspect, k->znear, k->zfar, out);
}

/* TT Sky's pre-render projection (0x10006220): the camera's horizontal field of view widened toward 180
   degrees by the distortion, near 1, far 200 */
static void sky_view_proj(const CkContext *ctx, const CkCamera *k, float distortion, float aspect, float out[4][4])
{
    float x[3], y[3], z[3], pos[3];
    camera_basis(ctx, k, x, y, z, pos);
    float fov = fabsf(k->fov), half = 0.5f * (fov + (3.141592654f - fov) * distortion);
    float c = 1.0f / tanf(half);
    view_proj_basis(x, y, z, pos, c, c * aspect, 1, 200, out);
}

static void argb_to_float(uint32_t c, float out[4])
{
    out[0] = (c >> 16 & 0xff) / 255.0f, out[1] = (c >> 8 & 0xff) / 255.0f, out[2] = (c & 0xff) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

/* The lights as CKLight sets them up (CK2_3D.dll FUN_1001b...: active lights; point and spot lights only
   with some attenuation), colour * power. Without a camera (the level viewer) a fixed sun and ambient. */
static void setup_lights(const CkContext *ctx, bool game, FrameUniforms *f)
{
    uint32_t n = 0;
    if (!game) {
        float ld[3] = {0.3f, -0.8f, 0.5f};
        normalize3(ld);
        memcpy(f->lights[0].dir, ld, 12);
        f->lights[0].pos[3] = VX_LIGHTDIREC;
        f->lights[0].col[0] = f->lights[0].col[1] = f->lights[0].col[2] = 0.75f;
        f->ambient[0] = f->ambient[1] = f->ambient[2] = 0.45f;
        f->ambient[3] = 1;
        return;
    }
    argb_to_float(ctx->ambient, f->ambient);
    for (uint32_t i = 0; i < ctx->nobjs && n < MAX_LIGHTS; i++) {
        CkObj *o = ctx->objs[i];
        if (!o || (o->cid != CKCID_LIGHT && o->cid != CKCID_TARGETLIGHT)) continue;
        const CkLight *l = (const CkLight *)o;
        if (!(l->flags & CK_LIGHT_ACTIVE) || !(o->flags & CK_OBJECT_VISIBLE)) continue;
        if (l->type != VX_LIGHTDIREC && l->att0 + l->att1 + l->att2 < 1e-6f) continue;
        const Ck3dEntity *e = &l->e;
        float dir[3] = {e->world[2][0], e->world[2][1], e->world[2][2]};
        const CkObj *to = ck_obj(ctx, l->target);
        if (to && ck_is_3dentity_class(to->cid))
            for (int k = 0; k < 3; k++) dir[k] = ((const Ck3dEntity *)to)->world[3][k] - e->world[3][k];
        normalize3(dir);
        f->lights[n].pos[0] = e->world[3][0], f->lights[n].pos[1] = e->world[3][1], f->lights[n].pos[2] = e->world[3][2];
        f->lights[n].pos[3] = (float)l->type;
        memcpy(f->lights[n].dir, dir, 12);
        f->lights[n].dir[3] = l->range;
        f->lights[n].col[0] = l->color.r * l->power, f->lights[n].col[1] = l->color.g * l->power;
        f->lights[n].col[2] = l->color.b * l->power;
        f->lights[n].col[3] = cosf(l->hotspot / 2);
        f->lights[n].att[0] = l->att0, f->lights[n].att[1] = l->att1, f->lights[n].att[2] = l->att2;
        f->lights[n].att[3] = cosf(l->falloff / 2);
        n++;
    }
    f->ambient[3] = (float)n;
}

_Static_assert(sizeof(CkParticleVertex) == PART_STRIDE, "particle vertex layout");

typedef struct { uint32_t mesh, batch, pipeline, bind; bool blended; uint32_t kind, vfirst, vcount; } DrawRec;   /* kind 0 mesh, 1 2D quad, 2 text, 3 particles,
                                                                                                              4 mesh channel vfirst */

/* Text quads from ck_text_render: two triangles each into the text vertex buffer, consecutive quads with
   the same texture and colour source batched into one draw */
typedef struct {
    Renderer *r;
    CkContext *ctx;
    DrawRec *recs;
    uint32_t *ndraws;
    uint32_t nverts;
} TextSink;

static void text_emit(void *user, const CkTextQuad *q)
{
    TextSink *s = user;
    Renderer *r = s->r;
    if (s->nverts + 6 > MAX_TEXT_VERTS) return;
    GpuTexture *t = q->texture && q->texture < r->cap ? &r->textures[q->texture] : NULL;
    uint32_t bind = t && t->bind_group ? t->bind_group : r->white_bind;
    DrawRec *last = *s->ndraws ? &s->recs[*s->ndraws - 1] : NULL;
    if (!last || last->kind != 2 || last->bind != bind || last->vfirst + last->vcount != s->nverts) {
        if (*s->ndraws >= MAX_DRAWS) return;
        memset(r->draw_data + *s->ndraws * DRAW_STRIDE, 0, 128);
        s->recs[*s->ndraws] = (DrawRec){0, 0, pipeline_text(r), bind, true, 2, s->nverts, 0};
        last = &s->recs[(*s->ndraws)++];
    }
    static const int corner[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; k++) {
        int c = corner[k];
        float v[4] = {q->x[c] / CK_SCREEN_W * 2 - 1, 1 - q->y[c] / CK_SCREEN_H * 2, q->u[c], q->v[c]};
        uint8_t *dst = r->text_verts + (s->nverts + (uint32_t)k) * TEXT_STRIDE;
        memcpy(dst, v, 16);
        memcpy(dst + 16, &q->color[c], 4);
    }
    s->nverts += 6;
    last->vcount += 6;
}

/* CKTexture's SetAsCurrent (CK2_3D, the function holding 0x10078f77): a transparent texture turns the alpha
   test on with reference 0 and NOTEQUAL whatever the material says (the material's own alpha test is then
   not applied); returns false when the texture doesn't decide */
static bool texture_alpha_test(const CkContext *ctx, CkId tex, float *ref, float *func)
{
    const CkTexture *t = ck_texture(ctx, tex);
    if (!t || !t->transparent) return false;
    *ref = 0, *func = 6;
    return true;
}

void render_frame(Renderer *r, CkContext *ctx, const Camera *cam)
{
    /* the render context's pre-render callbacks (TT Simple Shadow's channel UVs), run once and dropped */
    ck_run_pre_render(ctx);
    if (r->texture_dir) render_upload(r, ctx, r->texture_dir);
    ensure_cap(r, ctx->nobjs + 1);
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkObj *o = ctx->objs[i];
        if (o && o->cid == CKCID_MESH && r->meshes[o->id].version != ((CkMesh *)o)->version) upload_mesh(r, (CkMesh *)o);
        if (o && o->cid == CKCID_MESH && ((CkMesh *)o)->nchannels) upload_channels(r, (CkMesh *)o);
        if (!o || o->cid != CKCID_TEXTURE) continue;
        CkTexture *t = (CkTexture *)o;
        if (t->movie) upload_dynamic_texture(r, t);
        else if (r->texture_dir && r->textures[o->id].version != t->version) upload_texture(r, t, r->texture_dir);
    }
    uint32_t w = gpu_width(), h = gpu_height();
    /* 4:3 picture, pillar- or letterboxed into the drawable */
    float vw = (float)w, vh = (float)h;
    if (vw * 3 > vh * 4) vw = vh * 4 / 3;
    else vh = vw * 3 / 4;
    static FrameUniforms frame;
    memset(&frame, 0, sizeof frame);
    const CkCamera *k = cam ? NULL : ck_camera(ctx, ctx->camera), *game_cam = k;
    if (k) {
        camera_view_proj(ctx, k, 4.0f / 3.0f, frame.viewproj);
        float cx[3], cy[3], cz[3];
        camera_basis(ctx, k, cx, cy, cz, frame.eye);
    } else {
        Camera def = {{0, 0, 0}, 0, 0, 0.8f, 1, 4000};
        view_proj(cam ? cam : &def, 4.0f / 3.0f, frame.viewproj);
        memcpy(frame.eye, (cam ? cam : &def)->pos, 12);
    }
    setup_lights(ctx, !cam, &frame);
    if (cam) {
        frame.fog[0] = 1000, frame.fog[1] = 3000;
    } else {
        argb_to_float(ctx->fog_color, frame.fog_col);
        frame.fog[0] = ctx->fog_start, frame.fog[1] = ctx->fog_end;
        frame.fog[2] = (float)ctx->fog_mode, frame.fog[3] = ctx->fog_density;
    }
    gpu_write_buffer(r->frame_buf, 0, &frame, sizeof frame);

    /* per-draw uniforms: world matrix, diffuse, emissive, params (alpha ref, lit) */
    uint32_t ndraws = 0;
    static DrawRec recs[MAX_DRAWS];
    /* render-first entities (VX_MOVEABLE_RENDERFIRST: the sky), then opaque, then blended */
    for (int pass = 0; pass < 3; pass++)
        for (uint32_t i = 0; i < ctx->nobjs && ndraws < MAX_DRAWS; i++) {
            CkObj *o = ctx->objs[i];
            if (!o || !ck_is_3dentity_class(o->cid) || !(o->flags & CK_OBJECT_VISIBLE)) continue;
            Ck3dEntity *e = (Ck3dEntity *)o;
            bool first = (e->moveable & VX_MOVEABLE_RENDERFIRST) != 0;
            if (first != (pass == 0)) continue;
            CkMesh *m = ck_mesh(ctx, e->mesh);
            if (!m || !r->meshes[m->be.h.id].vbuf) continue;
            GpuMesh *g = &r->meshes[m->be.h.id];
            for (uint32_t b = 0; b < g->nbatches && ndraws < MAX_DRAWS; b++) {
                CkMaterial *mat = g->batches[b].material_slot < m->materials.n
                                      ? ck_material(ctx, m->materials.v[g->batches[b].material_slot]) : NULL;
                uint8_t src = mat ? mat->src_blend : VXBLEND_ONE, dst = mat ? mat->dst_blend : VXBLEND_ZERO;
                uint8_t mflags = mat ? mat->flags : (CKMAT_ZWRITE | CKMAT_PERSPECTIVE);
                bool blended = (mflags & CKMAT_ALPHABLEND) != 0;
                if (pass && blended != (pass == 2)) continue;
                bool zwrite = (mflags & CKMAT_ZWRITE) && !(e->moveable & VX_MOVEABLE_NOZBUFFERWRITE);
                uint8_t zfunc = (e->moveable & VX_MOVEABLE_NOZBUFFERTEST) ? 8 : mat ? mat->z_func : 4;
                GpuTexture *t = mat && mat->texture ? &r->textures[mat->texture] : NULL;
                float *u = (float *)(r->draw_data + ndraws * DRAW_STRIDE);
                memcpy(u, e->world, 64);
                CkColor d = mat ? mat->diffuse : (CkColor){1, 1, 1, 1};
                CkColor em = mat ? mat->emissive : (CkColor){0, 0, 0, 0};
                CkColor am = mat ? mat->ambient : (CkColor){1, 1, 1, 1};
                u[28] = am.r, u[29] = am.g, u[30] = am.b, u[31] = am.a;
                /* specular: the material's colour and power (CKMaterial::SetAsCurrent enables it when both are set) */
                CkColor sp = mat ? mat->specular : (CkColor){0, 0, 0, 0};
                bool spec_on = mat && mat->power > 0 && (sp.r > 0 || sp.g > 0 || sp.b > 0);
                u[48] = sp.r, u[49] = sp.g, u[50] = sp.b, u[51] = spec_on ? mat->power : 0;
                u[16] = d.r, u[17] = d.g, u[18] = d.b, u[19] = d.a;
                u[20] = em.r, u[21] = em.g, u[22] = em.b, u[23] = em.a;
                bool atest = (mflags & CKMAT_ALPHATEST) != 0;
                u[24] = atest ? mat->alpha_ref / 255.0f : 0.0f;       /* alpha reference */
                u[25] = (m->flags & 0x80) ? 0.0f : 1.0f;              /* lit unless VXMESH_PRELITMODE */
                u[26] = atest ? (float)mat->alpha_func : 0.0f;        /* alpha compare (0 = off) */
                if (mat) texture_alpha_test(ctx, mat->texture, &u[24], &u[26]);
                u[27] = 0;
                if (e->sky_distortion > 0 && game_cam) {
                    /* TT Sky's pre-render callback (0x10006220, step 8): SetPosition((0,0,0), camera) snaps
                       the sky onto the eye at draw time, whatever its execute did this frame */
                    for (int j = 0; j < 3; j++) e->world[3][j] = game_cam->e.world[3][j];
                    memcpy(u, e->world, 64);
                    sky_view_proj(ctx, game_cam, e->sky_distortion, 4.0f / 3.0f, (float (*)[4])(u + 32));
                    u[27] = 1;                                          /* own view-projection */
                }
                recs[ndraws] = (DrawRec){m->be.h.id, b,
                                         pipeline_for(r, blended, src, dst, zwrite, (mflags & CKMAT_TWOSIDED) != 0, zfunc),
                                         t && t->bind_group ? (mat->address == 3 ? texture_bind_clamp(r, t) : t->bind_group) : r->white_bind,
                                         blended, 0, 0, 0};
                ndraws++;
            }
            /* the material channels: every face again with the channel's UVs, material and blend factors
               (after the opaque pass of the entity; not z-written, depth equal or nearer) */
            if (pass != 1) continue;
            for (uint32_t c = 0; c < m->nchannels && c < g->nchannels; c++) {
                CkMaterial *cm = ck_material(ctx, m->channels[c].material);
                if (!cm || !g->channels[c].vbuf) continue;
                GpuTexture *t = cm->texture && cm->texture < r->cap ? &r->textures[cm->texture] : NULL;
                uint32_t bind = t && t->bind_group ? (cm->address == 3 ? texture_bind_clamp(r, t) : t->bind_group) : r->white_bind;
                uint32_t pipe = pipeline_for(r, true, m->channels[c].src_blend, m->channels[c].dst_blend, false, (cm->flags & CKMAT_TWOSIDED) != 0, 4);
                for (uint32_t b = 0; b < g->nbatches && ndraws < MAX_DRAWS; b++) {
                    float *u = (float *)(r->draw_data + ndraws * DRAW_STRIDE);
                    memset(u, 0, 208);
                    memcpy(u, e->world, 64);
                    u[16] = cm->diffuse.r, u[17] = cm->diffuse.g, u[18] = cm->diffuse.b, u[19] = cm->diffuse.a;
                    u[20] = cm->emissive.r, u[21] = cm->emissive.g, u[22] = cm->emissive.b, u[23] = cm->emissive.a;
                    u[28] = cm->ambient.r, u[29] = cm->ambient.g, u[30] = cm->ambient.b;
                    u[25] = 1;                                  /* lit (the AddChannel default) */
                    u[31] = 1;                                  /* fogged before blending, as D3D7's fixed-function fog */
                    recs[ndraws++] = (DrawRec){m->be.h.id, b, pipe, bind, true, 4, c, 0};
                }
            }
        }
    /* particles: the owner frames' post-render callbacks (drawn after the scene, no depth write) */
    uint32_t npart = 0;
    if (game_cam) {
        float cr[3], cu[3], cf[3], cp[3];
        camera_basis(ctx, game_cam, cr, cu, cf, cp);
        for (uint32_t i = 0; i < ctx->particle_systems.n && ndraws < MAX_DRAWS; i++) {
            CkBehavior *pb = ck_behavior(ctx, ctx->particle_systems.v[i]);
            CkParticleSystem *ps = pb ? pb->bb_state : NULL;
            Ck3dEntity *owner = ps ? ck_entity(ctx, ps->owner) : NULL;
            if (!owner || !(owner->be.h.flags & CK_OBJECT_VISIBLE)) continue;
            ck_ps_trail(ctx, ps);
            if (!ps->live) continue;
            uint32_t n = ck_ps_geometry(ps, cr, cu, cf, r->part_verts + npart, MAX_PART_VERTS - npart);
            if (!n) continue;
            memset(r->draw_data + ndraws * DRAW_STRIDE, 0, 208);
            texture_alpha_test(ctx, ps->texture, (float *)(r->draw_data + ndraws * DRAW_STRIDE) + 24,
                               (float *)(r->draw_data + ndraws * DRAW_STRIDE) + 26);
            GpuTexture *t = ps->texture && ps->texture < r->cap ? &r->textures[ps->texture] : NULL;
            recs[ndraws++] = (DrawRec){0, 0, pipeline_particles(r, (uint8_t)ps->src_blend, (uint8_t)ps->dst_blend, ps->render == CKPS_RENDER_LINE),
                                       t && t->bind_group ? t->bind_group : r->white_bind, true, 3, npart, n};
            npart += n;
        }
    }
    /* 3D sprites (CKSprite3D): oriented toward the viewpoint, one textured quad each with the material's
       blend factors (opaque without alpha blending); drawn unlit in the material's diffuse colour */
    if (game_cam) {
        for (uint32_t i = 0; i < ctx->nobjs && ndraws < MAX_DRAWS; i++) {
            CkObj *o = ctx->objs[i];
            if (!o || o->cid != CKCID_SPRITE3D || !(o->flags & CK_OBJECT_VISIBLE)) continue;
            CkSprite3D *sp = (CkSprite3D *)o;
            CkMaterial *mat = ck_material(ctx, sp->material);
            if (!mat || npart + 6 > MAX_PART_VERTS) continue;
            float pos[4][3], uv[4][2];
            ck_sprite3d_quad(sp, game_cam->e.world, pos, uv);
            float c[4] = {mat->diffuse.r, mat->diffuse.g, mat->diffuse.b, mat->diffuse.a};
            uint32_t argb = 0;
            for (int k = 0; k < 4; k++) {
                float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
                argb |= (uint32_t)(v * 255 + 0.5f) << (k == 3 ? 24 : 16 - 8 * k);
            }
            static const int corner[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; k++) {
                CkParticleVertex *v = &r->part_verts[npart + (uint32_t)k];
                memcpy(v->pos, pos[corner[k]], 12);
                memcpy(v->uv, uv[corner[k]], 8);
                v->color = argb;
            }
            bool blended = (mat->flags & CKMAT_ALPHABLEND) != 0;
            uint8_t src = blended ? mat->src_blend : VXBLEND_ONE, dst = blended ? mat->dst_blend : VXBLEND_ZERO;
            memset(r->draw_data + ndraws * DRAW_STRIDE, 0, 208);
            if (mat->flags & CKMAT_ALPHATEST) {
                float *u = (float *)(r->draw_data + ndraws * DRAW_STRIDE);
                u[24] = mat->alpha_ref / 255.0f, u[26] = (float)mat->alpha_func;
            }
            texture_alpha_test(ctx, mat->texture, (float *)(r->draw_data + ndraws * DRAW_STRIDE) + 24,
                               (float *)(r->draw_data + ndraws * DRAW_STRIDE) + 26);
            GpuTexture *t = mat->texture && mat->texture < r->cap ? &r->textures[mat->texture] : NULL;
            recs[ndraws++] = (DrawRec){0, 0, pipeline_particles(r, src, dst, false), t && t->bind_group ? t->bind_group : r->white_bind,
                                       true, 3, npart, 6};
            npart += 6;
        }
    }
    if (npart) gpu_write_buffer(r->part_vbuf, 0, r->part_verts, npart * PART_STRIDE);
    /* 2D entities (the render context's foreground), by Z order; homogeneous rectangles are fractions of the
       4:3 viewport, others pixels of the 640x480 render context */
    static Sorted2d sorted[MAX_DRAWS];
    uint32_t n2d = 0;
    for (uint32_t i = 0; i < ctx->nobjs && n2d < MAX_DRAWS; i++) {
        CkObj *o = ctx->objs[i];
        if (!o || !ck_is_2dentity_class(o->cid) || !(o->flags & CK_OBJECT_VISIBLE)) continue;
        bool text = false;
        for (uint32_t k = 0; k < ctx->text_draws.n && !text; k++) {
            CkBehavior *tb = ck_behavior(ctx, ctx->text_draws.v[k]);
            text = tb && ck_behavior_target(ctx, tb) == o->id;
        }
        if (((Ck2dEntity *)o)->material || text) sorted[n2d++].e = (Ck2dEntity *)o;
    }
    qsort(sorted, n2d, sizeof *sorted, by_zorder);
    TextSink sink = {r, ctx, recs, &ndraws, 0};
    for (uint32_t k = 0; k < n2d && ndraws < MAX_DRAWS; k++) {
        Ck2dEntity *e = sorted[k].e;
        CkMaterial *mat = ck_material(ctx, e->material);
        if (mat) {
            float x0 = e->rect[0], y0 = e->rect[1], x1 = e->rect[2], y1 = e->rect[3];
            if (!(e->flags & CK2D_HOMOGENEOUS)) x0 /= CK_SCREEN_W, x1 /= CK_SCREEN_W, y0 /= CK_SCREEN_H, y1 /= CK_SCREEN_H;
            float *u = (float *)(r->draw_data + ndraws * DRAW_STRIDE);
            memset(u, 0, 128);
            u[0] = x0 * 2 - 1, u[1] = 1 - y0 * 2, u[2] = x1 * 2 - 1, u[3] = 1 - y1 * 2;
            if (e->flags & CK2D_USESRCRECT) memcpy(u + 4, e->src, 16);
            else u[4] = 0, u[5] = 0, u[6] = 1, u[7] = 1;
            u[16] = mat->diffuse.r, u[17] = mat->diffuse.g, u[18] = mat->diffuse.b, u[19] = mat->diffuse.a;
            bool atest = (mat->flags & CKMAT_ALPHATEST) != 0;
            u[24] = atest ? mat->alpha_ref / 255.0f : 0.0f;
            u[26] = atest ? (float)mat->alpha_func : 0.0f;
            texture_alpha_test(ctx, mat->texture, &u[24], &u[26]);
            GpuTexture *t = mat->texture ? &r->textures[mat->texture] : NULL;
            recs[ndraws] = (DrawRec){0, 0, pipeline_2d(r, (mat->flags & CKMAT_ALPHABLEND) != 0, mat->src_blend, mat->dst_blend),
                                     t && t->bind_group ? t->bind_group : r->white_bind, true, 1, 0, 0};
            ndraws++;
        }
        /* the entity's post-render callbacks: its 2D Texts, in registration order */
        for (uint32_t q = 0; q < ctx->text_draws.n; q++) {
            CkBehavior *tb = ck_behavior(ctx, ctx->text_draws.v[q]);
            if (tb && ck_behavior_target(ctx, tb) == e->be.h.id) ck_text_render(ctx, tb, e, text_emit, &sink);
        }
    }
    /* Planar Filter: the render context's post-sprite callback, a full-viewport quad of the filter colour
       times the texture with the pins' blend factors (0x25787cd0) */
    for (uint32_t i = 0; i < ctx->planar_filters.n && ndraws < MAX_DRAWS; i++) {
        CkBehavior *fb = ck_behavior(ctx, ctx->planar_filters.v[i]);
        if (!fb) continue;
        float col[4] = {1, 1, 1, 0.5f};
        int32_t src = VXBLEND_SRCALPHA, dst = VXBLEND_INVSRCALPHA;
        CkId tex = 0;
        CkParameter *pp;
        if ((pp = fb->pin.n > 0 ? ck_param_resolve(ctx, ck_param(ctx, fb->pin.v[0])) : NULL) && pp->value && pp->size >= 16) memcpy(col, pp->value, 16);
        if ((pp = fb->pin.n > 2 ? ck_param_resolve(ctx, ck_param(ctx, fb->pin.v[2])) : NULL) && pp->value && pp->size >= 4) memcpy(&tex, pp->value, 4);
        if ((pp = fb->pin.n > 3 ? ck_param_resolve(ctx, ck_param(ctx, fb->pin.v[3])) : NULL) && pp->value && pp->size >= 4) memcpy(&src, pp->value, 4);
        if ((pp = fb->pin.n > 4 ? ck_param_resolve(ctx, ck_param(ctx, fb->pin.v[4])) : NULL) && pp->value && pp->size >= 4) memcpy(&dst, pp->value, 4);
        float *u = (float *)(r->draw_data + ndraws * DRAW_STRIDE);
        memset(u, 0, 128);
        u[0] = -1, u[1] = 1, u[2] = 1, u[3] = -1;
        u[4] = 0, u[5] = 0, u[6] = 1, u[7] = 1;
        for (int k = 0; k < 4; k++) u[16 + k] = col[k] < 0 ? 0 : col[k] > 1 ? 1 : col[k];
        GpuTexture *t = ck_texture(ctx, tex) && tex < r->cap ? &r->textures[tex] : NULL;
        recs[ndraws] = (DrawRec){0, 0, pipeline_2d(r, true, (uint8_t)src, (uint8_t)dst), t && t->bind_group ? t->bind_group : r->white_bind, true, 1, 0, 0};
        ndraws++;
    }
    if (ndraws) gpu_write_buffer(r->draw_buf, 0, r->draw_data, ndraws * DRAW_STRIDE);
    if (sink.nverts) gpu_write_buffer(r->text_vbuf, 0, r->text_verts, sink.nverts * TEXT_STRIDE);
    uint32_t bg = ctx->background;
    if (!gpu_begin_frame((bg >> 16 & 0xff) / 255.0f, (bg >> 8 & 0xff) / 255.0f, (bg & 0xff) / 255.0f, 1)) {
        gpu_end_frame();
        return;
    }
    gpu_set_viewport(((float)w - vw) / 2, ((float)h - vh) / 2, vw, vh, 0, 1);
    uint32_t last_pipe = 0;
    for (uint32_t i = 0; i < ndraws; i++) {
        DrawRec *d = &recs[i];
        if (d->pipeline != last_pipe) gpu_set_pipeline(last_pipe = d->pipeline);
        uint32_t off = i * DRAW_STRIDE;
        gpu_set_bind_group_offsets(0, r->bind0, &off, 1);
        gpu_set_bind_group(1, d->bind);
        if (d->kind == 1) {
            gpu_draw(6, 1, 0, 0);
            continue;
        }
        if (d->kind == 2 || d->kind == 3) {
            gpu_set_vertex_buffer(0, d->kind == 2 ? r->text_vbuf : r->part_vbuf, 0);
            gpu_draw(d->vcount, 1, d->vfirst, 0);
            continue;
        }
        GpuMesh *g = &r->meshes[d->mesh];
        gpu_set_vertex_buffer(0, d->kind == 4 ? g->channels[d->vfirst].vbuf : g->vbuf, 0);
        gpu_set_index_buffer(g->ibuf, GPU_INDEX_U16, 0);
        gpu_draw_indexed(g->batches[d->batch].count, 1, g->batches[d->batch].first, 0, 0);
    }
    gpu_end_frame();
}
