/* Application modes (launch param "mode"):
   - game (default): base.cmo boots like Player.exe: the level scene is launched and every frame runs the
     behavior manager, then renders the scene and mixes the sounds.
   - viewer: param "level" (1-12) loads 3D Entities/Level/Level_NN.NMO and flies a camera through it
     (pad: d-pad move/turn, L/R strafe, A/B up/down, X/Y look up/down).
   Param "unlockall=1" (testing): every level counts as unlocked (the DB_Levelfreischaltung array). */
#include "app.h"
#include "bb/bb.h"
#include "ck/ck_3d.h"
#include "ck/ck_sound.h"
#include "platform.h"
#include "render/gpu.h"
#include "render/render.h"
#include "vfs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CkContext ctx;
static Renderer *renderer;
static Camera cam;
static bool viewer;

static void log_line(const char *m) { plat_log(m); }

/* Saves come from the platform's storage; before the first save, the file the installer copied (in the
   game data) is used, as the original reads Database.tdb from its folder. */
static uint8_t *load_user(CkContext *c, const char *name, size_t *size)
{
    uint8_t *b = plat_load_user_file(name, size);
    return b ? b : vfs_read_all(name, size);
}
static bool save_user(CkContext *c, const char *name, const void *data, size_t size) { return plat_save_user_file(name, data, size); }

/* The values Ballance's installer writes under HKEY_LOCAL_MACHINE\Software\Ballance\Settings.
   Language: 0 German, 1 English, 2 Spanish, 3 Italian, 4 French (launch param "language"). */
static const char *registry(CkContext *c, const char *section, const char *entry)
{
    static char lang[8];
    if (!strcmp(entry, "Language")) {
        if (!plat_param("language", lang, sizeof lang) || !*lang) strcpy(lang, "1");
        return lang;
    }
    return NULL;
}

static void start_camera(void)
{
    cam.fov_y = 60.0f * 3.14159265f / 180.0f;
    cam.znear = 1.0f;
    cam.zfar = 5000.0f;
    cam.pos[0] = 0, cam.pos[1] = 60, cam.pos[2] = -100;
    CkId g = ck_find(&ctx, "PS_Levelstart", CKCID_GROUP);
    CkObj *go = ck_obj(&ctx, g);
    if (go && ((CkGroup *)go)->members.n) {
        Ck3dEntity *e = ck_entity(&ctx, ((CkGroup *)go)->members.v[0]);
        if (e) {
            float *p = e->world[3];
            cam.pos[0] = p[0] - 34, cam.pos[1] = p[1] + 31, cam.pos[2] = p[2] + 43;
            float dx = p[0] - cam.pos[0], dy = p[1] - cam.pos[1], dz = p[2] - cam.pos[2];
            cam.yaw = atan2f(dx, dz);
            cam.pitch = -atan2f(dy, sqrtf(dx * dx + dz * dz));
        }
    }
}

bool app_init(void)
{
    char buf[32] = "", path[128], err[160];
    viewer = plat_param("mode", buf, sizeof buf) && !strcmp(buf, "viewer");
    ck_init(&ctx);
    bb_register_all(&ctx);
    ctx.log = log_line;
    ctx.load_user_file = load_user;
    ctx.save_user_file = save_user;
    ctx.registry = registry;
    if (viewer) {
        strcpy(buf, "1");
        plat_param("level", buf, sizeof buf);
        int level = atoi(buf);
        if (level < 1 || level > 12) level = 1;
        snprintf(path, sizeof path, "3D Entities/Level/Level_%02d.NMO", level);
    } else {
        snprintf(path, sizeof path, "base.cmo");
    }
    if (!ck_load(&ctx, path, NULL, err, sizeof err)) {
        plat_log(err);
        return false;
    }
    renderer = render_create();
    render_upload(renderer, &ctx, "Textures");
    if (viewer) start_camera();
    else ck_launch_level_scene(&ctx);
    return true;
}

static void fly(void)
{
    uint32_t p = plat_pad(0);
    /* the raw keyboard as the same buttons: arrows, Q / W strafe, X / Z up / down, S / A look down / up */
    uint8_t k[256];
    if (plat_dik_keys(k)) {
        static const struct { uint8_t dik; uint32_t pad; } keys[] = {
            {0xc8, PAD_UP}, {0xd0, PAD_DOWN}, {0xcb, PAD_LEFT}, {0xcd, PAD_RIGHT}, {0x10, PAD_L}, {0x11, PAD_R},
            {0x2d, PAD_A}, {0x2c, PAD_B}, {0x1f, PAD_X}, {0x1e, PAD_Y},
        };
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            if (k[keys[i].dik]) p |= keys[i].pad;
    }
    float speed = 2.5f, turn = 0.03f;
    float fx = sinf(cam.yaw), fz = cosf(cam.yaw);
    if (p & PAD_UP) cam.pos[0] += fx * speed, cam.pos[2] += fz * speed;
    if (p & PAD_DOWN) cam.pos[0] -= fx * speed, cam.pos[2] -= fz * speed;
    if (p & PAD_LEFT) cam.yaw -= turn;
    if (p & PAD_RIGHT) cam.yaw += turn;
    if (p & PAD_L) cam.pos[0] -= fz * speed, cam.pos[2] += fx * speed;
    if (p & PAD_R) cam.pos[0] += fz * speed, cam.pos[2] -= fx * speed;
    if (p & PAD_A) cam.pos[1] += speed;
    if (p & PAD_B) cam.pos[1] -= speed;
    if (p & PAD_X) cam.pitch -= turn;
    if (p & PAD_Y) cam.pitch += turn;
}

static uint32_t frame_no;

/* The keyboard the game reads (DirectInput key codes): the runner's raw keyboard (gasm 0.5 key_state), so
   every key works as in the original, including Shift + arrows and keys rebound in Options. Gamepads come
   in as pad 1: d-pad = arrows, A and Start = Enter, B = Escape, X = Space, L and R = left Shift (the view
   rotation key), Y = Q, Select = F1. Without a keyboard, typed characters press their key for the frame.
   The mouse: drawable pixels mapped into the 640x480 render context (the 4:3 picture as render.c letterboxes
   it). */
enum { DIK_ESCAPE = 0x01, DIK_1 = 0x02, DIK_0 = 0x0b, DIK_BACK = 0x0e, DIK_Q = 0x10, DIK_RETURN = 0x1c,
       DIK_LSHIFT = 0x2a, DIK_SPACE = 0x39, DIK_F1 = 0x3b, DIK_UP = 0xc8, DIK_LEFT = 0xcb,
       DIK_RIGHT = 0xcd, DIK_DOWN = 0xd0 };

static uint8_t dik_of_char(char c)
{
    static const char *rows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const uint8_t first[] = {0x02, 0x10, 0x1e, 0x2c};
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (int r = 0; r < 4; r++) {
        const char *p = strchr(rows[r], c);
        if (p && c) return (uint8_t)(first[r] + (p - rows[r]));
    }
    return c == ' ' ? DIK_SPACE : c == '\n' ? DIK_RETURN : c == '\b' ? DIK_BACK : 0;
}

static void read_input(void)
{
    memcpy(ctx.keys_prev, ctx.keys, sizeof ctx.keys);
    bool raw = plat_dik_keys(ctx.keys);
    uint32_t p = plat_pad(0);
    static const struct { uint32_t pad; uint8_t key; } map[] = {
        {PAD_UP, DIK_UP}, {PAD_DOWN, DIK_DOWN}, {PAD_LEFT, DIK_LEFT}, {PAD_RIGHT, DIK_RIGHT}, {PAD_A, DIK_RETURN},
        {PAD_B, DIK_ESCAPE}, {PAD_START, DIK_RETURN}, {PAD_X, DIK_SPACE}, {PAD_L, DIK_LSHIFT}, {PAD_R, DIK_LSHIFT},
        {PAD_Y, DIK_Q}, {PAD_SELECT, DIK_F1},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (p & map[i].pad) ctx.keys[map[i].key] = 1;
    if (!raw) {
        char text[64];
        int n = plat_text_input(text, sizeof text);
        for (int i = 0; i < n; i++) {
            uint8_t k = dik_of_char(text[i]);
            if (k) ctx.keys[k] = 1;
        }
    }
    memcpy(ctx.mouse_prev, ctx.mouse, sizeof ctx.mouse);
    ctx.mouse_buttons_prev = ctx.mouse_buttons;
    float x, y;
    uint32_t buttons;
    if (plat_pointer(&x, &y, &buttons)) {
        float w = (float)gpu_width(), h = (float)gpu_height(), vw = w, vh = h;
        if (vw * 3 > vh * 4) vw = vh * 4 / 3;
        else vh = vw * 3 / 4;
        if (vw > 0 && vh > 0) {
            ctx.mouse[0] = (x - (w - vw) / 2) / vw * CK_SCREEN_W;
            ctx.mouse[1] = (y - (h - vh) / 2) / vh * CK_SCREEN_H;
        }
        ctx.mouse_buttons = buttons & 7;
    }
    plat_cursor(ctx.cursor_visible);
}

/* param dump2d=N: the 2D entities at frame N (debugging) */
static void dump_2d(void)
{
    char v[16];
    if (!plat_param("dump2d", v, sizeof v) || (uint32_t)atoi(v) != frame_no) return;
    for (uint32_t i = 0; i < ctx.nobjs; i++) {
        CkObj *o = ctx.objs[i];
        if (!o || !ck_is_2dentity_class(o->cid) || !(o->flags & CK_OBJECT_VISIBLE)) continue;
        Ck2dEntity *e = (Ck2dEntity *)o;
        CkMaterial *m = ck_material(&ctx, e->material);
        CkTexture *t = m ? ck_texture(&ctx, m->texture) : NULL;
        char line[256];
        snprintf(line, sizeof line, "2d %s z=%d mat=%s tex=%s movie=%d slot=%d version=%u size=%ux%u", o->name, e->zorder,
                 m ? m->be.h.name : "-", t ? t->be.h.name : "-", t && t->movie, t ? t->current_slot : -1, t ? t->version : 0,
                 t ? t->width : 0, t ? t->height : 0);
        plat_log(line);
    }
}

/* param unlockall=1: DB_Levelfreischaltung's "Freigeschaltet?" column forced TRUE before each frame (the
   database is reloaded from the save at boot, so it's re-applied every frame) */
static void unlock_all(void)
{
    static int on = -1;
    if (on < 0) {
        char v[8];
        on = plat_param("unlockall", v, sizeof v) && atoi(v) != 0;
    }
    if (!on) return;
    for (uint32_t i = 0; i < ctx.nobjs; i++) {
        CkObj *o = ctx.objs[i];
        if (!o || o->cid != CKCID_DATAARRAY || strcmp(o->name, "DB_Levelfreischaltung")) continue;
        CkDataArray *a = (CkDataArray *)o;
        for (uint32_t r = 0; r < a->nrows && a->ncols; r++) ck_array_cell(a, r, 0)->i = 1;
    }
}

bool app_frame(void)
{
    frame_no++;
    if (viewer) fly();
    else {
        read_input();
        unlock_all();
        ck_process(&ctx, 1000.0f / 60);
    }
    dump_2d();
    render_frame(renderer, &ctx, viewer ? &cam : NULL);
    return !ctx.quit;
}

void app_audio(float *out, unsigned frames) { ck_sound_mix(&ctx, out, frames); }

void app_exit(void)
{
    render_destroy(renderer);
    renderer = NULL;
    ck_free(&ctx);
}
