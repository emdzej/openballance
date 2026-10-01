/* gasm platform backend for openballance.wasm (gasm ABI 0 with gasm:gfx and raw input as of 0.5.0). The only file that
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

/* ---- raw keyboard and pointer (gasm 0.5) ---- */

/* GASM_KEY_* (W3C KeyboardEvent.code) -> DirectInput scan code (DIK_*), what Ballance's input manager reads */
static const struct { uint8_t key, dik; } DIK_OF[] = {
    {GASM_KEY_ESCAPE, 0x01}, {GASM_KEY_DIGIT1, 0x02}, {GASM_KEY_DIGIT2, 0x03}, {GASM_KEY_DIGIT3, 0x04},
    {GASM_KEY_DIGIT4, 0x05}, {GASM_KEY_DIGIT5, 0x06}, {GASM_KEY_DIGIT6, 0x07}, {GASM_KEY_DIGIT7, 0x08},
    {GASM_KEY_DIGIT8, 0x09}, {GASM_KEY_DIGIT9, 0x0a}, {GASM_KEY_DIGIT0, 0x0b}, {GASM_KEY_MINUS, 0x0c},
    {GASM_KEY_EQUAL, 0x0d}, {GASM_KEY_BACKSPACE, 0x0e}, {GASM_KEY_TAB, 0x0f}, {GASM_KEY_KEY_Q, 0x10},
    {GASM_KEY_KEY_W, 0x11}, {GASM_KEY_KEY_E, 0x12}, {GASM_KEY_KEY_R, 0x13}, {GASM_KEY_KEY_T, 0x14},
    {GASM_KEY_KEY_Y, 0x15}, {GASM_KEY_KEY_U, 0x16}, {GASM_KEY_KEY_I, 0x17}, {GASM_KEY_KEY_O, 0x18},
    {GASM_KEY_KEY_P, 0x19}, {GASM_KEY_BRACKET_LEFT, 0x1a}, {GASM_KEY_BRACKET_RIGHT, 0x1b},
    {GASM_KEY_ENTER, 0x1c}, {GASM_KEY_CONTROL_LEFT, 0x1d}, {GASM_KEY_KEY_A, 0x1e}, {GASM_KEY_KEY_S, 0x1f},
    {GASM_KEY_KEY_D, 0x20}, {GASM_KEY_KEY_F, 0x21}, {GASM_KEY_KEY_G, 0x22}, {GASM_KEY_KEY_H, 0x23},
    {GASM_KEY_KEY_J, 0x24}, {GASM_KEY_KEY_K, 0x25}, {GASM_KEY_KEY_L, 0x26}, {GASM_KEY_SEMICOLON, 0x27},
    {GASM_KEY_QUOTE, 0x28}, {GASM_KEY_BACKQUOTE, 0x29}, {GASM_KEY_SHIFT_LEFT, 0x2a}, {GASM_KEY_BACKSLASH, 0x2b},
    {GASM_KEY_KEY_Z, 0x2c}, {GASM_KEY_KEY_X, 0x2d}, {GASM_KEY_KEY_C, 0x2e}, {GASM_KEY_KEY_V, 0x2f},
    {GASM_KEY_KEY_B, 0x30}, {GASM_KEY_KEY_N, 0x31}, {GASM_KEY_KEY_M, 0x32}, {GASM_KEY_COMMA, 0x33},
    {GASM_KEY_PERIOD, 0x34}, {GASM_KEY_SLASH, 0x35}, {GASM_KEY_SHIFT_RIGHT, 0x36}, {GASM_KEY_NUMPAD_MULTIPLY, 0x37},
    {GASM_KEY_ALT_LEFT, 0x38}, {GASM_KEY_SPACE, 0x39}, {GASM_KEY_CAPS_LOCK, 0x3a}, {GASM_KEY_F1, 0x3b},
    {GASM_KEY_F2, 0x3c}, {GASM_KEY_F3, 0x3d}, {GASM_KEY_F4, 0x3e}, {GASM_KEY_F5, 0x3f}, {GASM_KEY_F6, 0x40},
    {GASM_KEY_F7, 0x41}, {GASM_KEY_F8, 0x42}, {GASM_KEY_F9, 0x43}, {GASM_KEY_F10, 0x44}, {GASM_KEY_NUM_LOCK, 0x45},
    {GASM_KEY_SCROLL_LOCK, 0x46}, {GASM_KEY_NUMPAD7, 0x47}, {GASM_KEY_NUMPAD8, 0x48}, {GASM_KEY_NUMPAD9, 0x49},
    {GASM_KEY_NUMPAD_SUBTRACT, 0x4a}, {GASM_KEY_NUMPAD4, 0x4b}, {GASM_KEY_NUMPAD5, 0x4c}, {GASM_KEY_NUMPAD6, 0x4d},
    {GASM_KEY_NUMPAD_ADD, 0x4e}, {GASM_KEY_NUMPAD1, 0x4f}, {GASM_KEY_NUMPAD2, 0x50}, {GASM_KEY_NUMPAD3, 0x51},
    {GASM_KEY_NUMPAD0, 0x52}, {GASM_KEY_NUMPAD_DECIMAL, 0x53}, {GASM_KEY_INTL_BACKSLASH, 0x56}, {GASM_KEY_F11, 0x57},
    {GASM_KEY_F12, 0x58}, {GASM_KEY_F13, 0x64}, {GASM_KEY_F14, 0x65}, {GASM_KEY_F15, 0x66}, {GASM_KEY_INTL_RO, 0x73},
    {GASM_KEY_INTL_YEN, 0x7d}, {GASM_KEY_NUMPAD_EQUAL, 0x8d}, {GASM_KEY_NUMPAD_ENTER, 0x9c},
    {GASM_KEY_CONTROL_RIGHT, 0x9d}, {GASM_KEY_NUMPAD_COMMA, 0xb3}, {GASM_KEY_NUMPAD_DIVIDE, 0xb5},
    {GASM_KEY_PRINT_SCREEN, 0xb7}, {GASM_KEY_ALT_RIGHT, 0xb8}, {GASM_KEY_PAUSE, 0xc5}, {GASM_KEY_HOME, 0xc7},
    {GASM_KEY_ARROW_UP, 0xc8}, {GASM_KEY_PAGE_UP, 0xc9}, {GASM_KEY_ARROW_LEFT, 0xcb}, {GASM_KEY_ARROW_RIGHT, 0xcd},
    {GASM_KEY_END, 0xcf}, {GASM_KEY_ARROW_DOWN, 0xd0}, {GASM_KEY_PAGE_DOWN, 0xd1}, {GASM_KEY_INSERT, 0xd2},
    {GASM_KEY_DELETE, 0xd3}, {GASM_KEY_META_LEFT, 0xdb}, {GASM_KEY_META_RIGHT, 0xdc}, {GASM_KEY_CONTEXT_MENU, 0xdd},
};

bool plat_dik_keys(uint8_t dik[256])
{
    uint8_t bits[GASM_KEY_STATE_BYTES];
    memset(dik, 0, 256);
    if (gasm_key_state(bits, sizeof bits) < 0) return false;
    for (size_t i = 0; i < sizeof DIK_OF / sizeof *DIK_OF; i++)
        if (bits[DIK_OF[i].key >> 3] & (1u << (DIK_OF[i].key & 7))) dik[DIK_OF[i].dik] = 1;
    return true;
}

bool plat_pointer(float *x, float *y, uint32_t *buttons)
{
    uint8_t p[GASM_POINTER_BYTES];
    if (gasm_pointer(p, sizeof p) < 0) return false;
    memcpy(x, p + 0, 4);
    memcpy(y, p + 4, 4);
    memcpy(buttons, p + 32, 4);
    return true;
}

/* input_mode: raw keys always (Ballance reads the keyboard itself), the cursor as the game shows it */
static void input_mode(bool cursor)
{
    gasm_input_mode(GASM_INPUT_KEYS_RAW | (cursor ? 0u : GASM_INPUT_POINTER_HIDDEN));
}

void plat_cursor(bool visible)
{
    static int last = -1;
    if (last == (int)visible) return;
    last = visible;
    input_mode(visible);
}

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
    input_mode(false);
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
