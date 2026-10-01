/* gasm platform backend for openballance.wasm (gasm ABI 0 with gasm:gfx as of 0.4.0). The only file that
   includes gasm.h. Exports: gasm_init (mount the data, app_init), gasm_frame (app_frame), gasm_exit.

   Game data, the first that has the game (see mount_game):
   - the installed game's files as assets named by their paths (gasm-run --asset-dir <game folder>:
     base.cmo, 3D Entities/..., Textures/...). Lookups go as the game spells them; gasm runners fold
     case for folder assets;
   - the Ballance CD's files the same way (--asset-dir <CD folder>: Setup/data1.hdr, Setup/data1.cab,
     ...): the game folder is read from its InstallShield cabinets (iscab.c);
   - the CD image as the asset "rom" (--rom Ballance.iso, i.e. --asset rom=Ballance.iso): its ISO 9660
     file system, then the cabinets on it. */
#include "app.h"
#include "iscab.h"
#include "platform.h"
#include "render/gpu.h"
#include "vfs.h"
#include <gasm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wasi/api.h>

_Static_assert(PAD_A == GASM_BTN_A && PAD_B == GASM_BTN_B && PAD_X == GASM_BTN_X && PAD_Y == GASM_BTN_Y &&
               PAD_L == GASM_BTN_L && PAD_R == GASM_BTN_R && PAD_SELECT == GASM_BTN_SELECT &&
               PAD_START == GASM_BTN_START && PAD_UP == GASM_BTN_UP && PAD_DOWN == GASM_BTN_DOWN &&
               PAD_LEFT == GASM_BTN_LEFT && PAD_RIGHT == GASM_BTN_RIGHT, "PAD_* must be the gasm button bits");

static uint32_t pads[4];

void plat_log(const char *msg) { gasm_log(msg, (uint32_t)strlen(msg)); }
uint32_t plat_pad(int player) { return player >= 0 && player < 4 ? pads[player] : 0; }
static char text[64];
static int32_t ntext;
int plat_text_input(char *dst, size_t cap)
{
    if (ntext < 0) return -1;
    size_t n = (size_t)ntext < cap ? (size_t)ntext : cap;
    memcpy(dst, text, n);
    return (int)n;
}
bool plat_param(const char *name, char *dst, size_t cap) { return gasm_param_str(name, dst, (uint32_t)cap); }

uint8_t *plat_load_user_file(const char *name, size_t *size)
{
    int32_t n = gasm_storage_get(name, (uint32_t)strlen(name), NULL, 0);
    if (n < 0) return NULL;
    uint8_t *b = malloc(n ? (size_t)n : 1);
    if (gasm_storage_get(name, (uint32_t)strlen(name), b, (uint32_t)n) != n) { free(b); return NULL; }
    *size = (size_t)n;
    return b;
}

bool plat_save_user_file(const char *name, const void *data, size_t size)
{
    return gasm_storage_set(name, (uint32_t)strlen(name), data, (uint32_t)size) == 0;
}

/* ---- GPU: render/gpu.h over gasm:gfx ---- */

uint32_t gpu_width(void) { return gasm_gfx_width(); }
uint32_t gpu_height(void) { return gasm_gfx_height(); }
uint32_t gpu_create_shader(const char *wgsl) { return gasm_gfx_create_shader(wgsl, (uint32_t)strlen(wgsl)); }
uint32_t gpu_create_buffer(uint32_t size, uint32_t usage) { return gasm_gfx_create_buffer(size, usage); }
void gpu_write_buffer(uint32_t buf, uint32_t offset, const void *data, uint32_t len) { gasm_gfx_write_buffer(buf, offset, data, len); }
uint32_t gpu_create_texture(const char *json) { return gasm_gfx_create_texture(json, (uint32_t)strlen(json)); }
void gpu_write_texture(uint32_t tex, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *rgba)
{
    gasm_gfx_write_texture(tex, mip, x, y, w, h, rgba, w * h * 4);
}
uint32_t gpu_create_sampler(const char *json) { return gasm_gfx_create_sampler(json, (uint32_t)strlen(json)); }
uint32_t gpu_create_bind_group_layout(const char *json) { return gasm_gfx_create_bind_group_layout(json, (uint32_t)strlen(json)); }
uint32_t gpu_create_pipeline(const char *json) { return gasm_gfx_create_pipeline(json, (uint32_t)strlen(json)); }
uint32_t gpu_create_bind_group(const char *json) { return gasm_gfx_create_bind_group(json, (uint32_t)strlen(json)); }
bool gpu_begin_frame(float r, float g, float b, float a) { return gasm_gfx_begin_frame(r, g, b, a) != 0; }
void gpu_set_pipeline(uint32_t p) { gasm_gfx_set_pipeline(p); }
void gpu_set_bind_group(uint32_t index, uint32_t bg) { gasm_gfx_set_bind_group(index, bg); }
void gpu_set_bind_group_offsets(uint32_t index, uint32_t bg, const uint32_t *o, uint32_t n) { gasm_gfx_set_bind_group_offsets(index, bg, o, n); }
void gpu_set_viewport(float x, float y, float w, float h, float a, float b) { gasm_gfx_set_viewport(x, y, w, h, a, b); }
void gpu_set_scissor_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { gasm_gfx_set_scissor_rect(x, y, w, h); }
void gpu_set_vertex_buffer(uint32_t slot, uint32_t buf, uint32_t offset) { gasm_gfx_set_vertex_buffer(slot, buf, offset); }
void gpu_set_index_buffer(uint32_t buf, uint32_t format, uint32_t offset) { gasm_gfx_set_index_buffer(buf, format, offset); }
void gpu_draw(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { gasm_gfx_draw(a, b, c, d); }
void gpu_draw_indexed(uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) { gasm_gfx_draw_indexed(a, b, c, d, e); }
void gpu_end_frame(void) { gasm_gfx_end_frame(); }

/* ---- data: the game's files as assets ---- */

typedef struct { int32_t size; char name[]; } FileAsset;

/* Game path -> asset name: '\\' -> '/', "." and ".." resolved (the original runs from Bin\\, so "..\\x"
   names the game folder; leading ".." components are dropped). */
static void norm_path(const char *rel, char *out, size_t cap)
{
    size_t n = 0;
    while (*rel && n + 1 < cap) {
        while (*rel == '/' || *rel == '\\') rel++;
        const char *seg = rel;
        while (*rel && *rel != '/' && *rel != '\\') rel++;
        size_t len = (size_t)(rel - seg);
        if (!len || (len == 1 && seg[0] == '.')) continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n && out[n - 1] != '/') n--;
            if (n) n--;
            continue;
        }
        if (n && n + 1 < cap) out[n++] = '/';
        for (size_t k = 0; k < len && n + 1 < cap; k++) out[n++] = seg[k];
    }
    out[n] = 0;
}

static void *files_open(void *ctx, const char *rel, uint64_t *size)
{
    (void)ctx;
    char name[512];
    norm_path(rel, name, sizeof name);
    int32_t n = gasm_asset_size(name, (uint32_t)strlen(name));
    if (n < 0) return NULL;
    size_t l = strlen(name);
    FileAsset *f = malloc(sizeof *f + l + 1);
    if (!f) return NULL;
    f->size = n;
    memcpy(f->name, name, l + 1);
    *size = (uint64_t)n;
    return f;
}

static int64_t files_read_at(void *ctx, void *file, uint64_t off, void *dst, size_t len)
{
    (void)ctx;
    const FileAsset *f = file;
    size_t done = 0;
    while (done < len && off + done <= UINT32_MAX) {
        int32_t k = gasm_asset_read_at(f->name, (uint32_t)strlen(f->name), (uint32_t)(off + done),
                                       (uint8_t *)dst + done, (uint32_t)(len - done));
        if (k < 0) return done ? (int64_t)done : -1;
        if (k == 0) break;
        done += (size_t)k;
    }
    return (int64_t)done;
}

static void files_close(void *ctx, void *file) { (void)ctx; free(file); }

/* ---- the CD image: asset "rom" as a VfsSource ---- */

static const char ROM[] = "rom";

static int64_t rom_read_at(void *ctx, uint64_t off, void *dst, size_t len)
{
    (void)ctx;
    size_t done = 0;
    while (done < len && off + done <= UINT32_MAX) {
        uint32_t want = len - done > (1u << 30) ? 1u << 30 : (uint32_t)(len - done);
        int32_t k = gasm_asset_read_at(ROM, sizeof ROM - 1, (uint32_t)(off + done), (uint8_t *)dst + done, want);
        if (k < 0) return done ? (int64_t)done : -1;
        if (k == 0) break;
        done += (size_t)k;
    }
    return (int64_t)done;
}

static bool mount_game(void)
{
    VfsBackend b = {NULL, files_open, files_read_at, files_close, NULL, NULL};
    vfs_mount(&b, "gasm assets");
    if (vfs_find_game()) return true;            /* the installed folder, or a CD folder's cabinets */
    vfs_unmount();
    int32_t n = gasm_asset_size(ROM, sizeof ROM - 1);
    if (n < 0) return false;
    VfsSource src = {NULL, (uint64_t)(uint32_t)n, rom_read_at, NULL};
    if (!vfs_mount_image(&src, 0, "gasm asset rom")) return false;
    if (vfs_find_game()) return true;
    vfs_unmount();
    return false;
}

/* ---- exports ---- */

static bool running;

GASM_EXPORT("gasm_abi_version") int32_t ob_gasm_abi_version(void) { return GASM_ABI_VERSION; }

static float audio_buf[735 * 2];

GASM_EXPORT("gasm_init") int32_t ob_gasm_init(void)
{
    gasm_set_frame_rate(60);
    gasm_audio_config(44100, 2);
    if (!mount_game()) {
        plat_log("OpenBallance: game data not found. Pass one of: the installed Ballance folder "
                 "(gasm-run openballance.wasm --asset-dir <folder with base.cmo>), the Ballance CD's files "
                 "(--asset-dir <CD folder with Setup/data1.hdr>) or the CD image (--rom Ballance.iso)");
        return 1;
    }
    char m[640];
    snprintf(m, sizeof m, "OpenBallance: game data from %s", vfs_describe());
    plat_log(m);
    if (!app_init()) return 1;
    running = true;
    return 0;
}

GASM_EXPORT("gasm_frame") void ob_gasm_frame(void)
{
    if (!running) return;
    for (uint32_t p = 0; p < 4; p++) pads[p] = gasm_input_pad(p);
    ntext = gasm_text_input(text, sizeof text);
    if (ntext > (int32_t)sizeof text) ntext = 0;   /* longer than a frame's worth: dropped */
    if (!app_frame()) {
        running = false;
        app_exit();
        __wasi_proc_exit(0);
    }
    app_audio(audio_buf, 735);   /* 44100 / 60 */
    gasm_audio_push(audio_buf, 735);
}

GASM_EXPORT("gasm_exit") void ob_gasm_exit(void)
{
    if (!running) return;
    running = false;
    app_exit();
}
