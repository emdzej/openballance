/* Building Blocks that talk to the host: TT_InterfaceManager_RT.dll, the system queries of
   TT_Toolbox_RT.dll, Visuals' cursor, WorldEnvironments' background, BuildingBlocksAddons1's time
   settings. There is no Windows underneath, so each emulates what the game needs and says how. */
#include "bb.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- TT Player Active 0694658d:7ffd236f (FUN_10004fa0): Active unless running inside the Virtools
   editor (CKContext::IsInInterfaceMode) - never here ---- */
static int bb_tt_player_active(CkContext *ctx, CkBehavior *b)
{
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT_ReadRegistry 460044b5:6e927b66 (FUN_10007a00): pIn Reg-Section, Reg-Entry, Destination-Array,
   Array to load; setting SaveArray-Mode. The value (typed by pOut Data: Integer, Float, Boolean or String)
   comes from the app's registry emulation (ctx->registry: the values Ballance's installer writes under
   Software\Ballance\Settings). Array mode isn't used by the game and fails. ---- */
static int bb_tt_read_registry(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    int32_t array_mode = 0;
    bb_get_local(ctx, b, 0, &array_mode, 4);
    const char *section = bb_in_string(ctx, b, 0), *entry = bb_in_string(ctx, b, 1);
    const char *v = !array_mode && ctx->registry && section && entry ? ctx->registry(ctx, section, entry) : NULL;
    CkParameter *out = bb_out(ctx, b, 0);
    if (!v || !out) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    if (ck_guid_eq(out->type, CKPGUID_STRING)) {
        bb_set_out(ctx, b, 0, v, (uint32_t)strlen(v) + 1);
    } else if (ck_guid_eq(out->type, CKPGUID_FLOAT)) {
        float f = (float)atof(v);
        bb_set_out(ctx, b, 0, &f, 4);
    } else {
        int32_t i = atoi(v);
        bb_set_out(ctx, b, 0, &i, 4);
    }
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT List Driver 62d00456:30eb4245 (FUN_10004920): the array gets DriverName, DriverID, DriverDesc
   rows; one driver here. pOut Installed Driver = 0 ---- */
static int bb_tt_list_driver(CkContext *ctx, CkBehavior *b)
{
    CkDataArray *a = ck_array(ctx, bb_in_object(ctx, b, 0));
    if (!a) {
        if (ctx->log) ctx->log("ListDriver: Kein DataArray-Objekt uebergeben.");
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    CkGuid none = {0, 0};
    ck_array_clear(ctx, a);
    while (a->ncols) ck_array_remove_column(ctx, a, 0);
    ck_array_insert_column(ctx, a, -1, CKARRAYTYPE_STRING, "DriverDesc", none);
    ck_array_insert_column(ctx, a, 0, CKARRAYTYPE_STRING, "DriverName", none);
    ck_array_insert_column(ctx, a, 1, CKARRAYTYPE_INT, "DriverID", none);
    uint32_t r = ck_array_add_row(ctx, a);
    CkCell *c = ck_array_cell(a, r, 0);
    free(c->s);
    c->s = strdup("OpenBallance");
    ck_array_cell(a, r, 1)->i = 0;
    c = ck_array_cell(a, r, 2);
    free(c->s);
    c->s = strdup("gasm:gfx (WebGPU)");
    int32_t installed = 0;
    bb_set_out(ctx, b, 0, &installed, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT List ScreenModes 4e7a0194:040328fd (FUN_10004c20): Mode, Width, Height, Bpp for the driver's
   modes above 8 bpp (duplicates dropped). The renderer draws at the host's drawable size whatever the
   scripts choose, so the list is the common DirectX set; pOut Installed Mode = 0 ---- */
static int bb_tt_list_screenmodes(CkContext *ctx, CkBehavior *b)
{
    static const int32_t modes[][3] = {{640, 480, 16}, {640, 480, 32}, {800, 600, 16}, {800, 600, 32},
                                       {1024, 768, 16}, {1024, 768, 32}, {1152, 864, 32}, {1280, 960, 32},
                                       {1280, 1024, 32}, {1600, 1200, 32}};
    CkDataArray *a = ck_array(ctx, bb_in_object(ctx, b, 1));
    if (!a) {
        if (ctx->log) ctx->log("ListScreenMode: Kein DataArray-Objekt uebergeben.");
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    CkGuid none = {0, 0};
    ck_array_clear(ctx, a);
    while (a->ncols) ck_array_remove_column(ctx, a, 0);
    ck_array_insert_column(ctx, a, -1, CKARRAYTYPE_INT, "Bpp", none);
    ck_array_insert_column(ctx, a, 0, CKARRAYTYPE_INT, "Mode", none);
    ck_array_insert_column(ctx, a, 1, CKARRAYTYPE_INT, "Width", none);
    ck_array_insert_column(ctx, a, 2, CKARRAYTYPE_INT, "Height", none);
    for (int32_t i = 0; i < (int32_t)(sizeof modes / sizeof *modes); i++) {
        uint32_t r = ck_array_add_row(ctx, a);
        ck_array_cell(a, r, 0)->i = i;
        ck_array_cell(a, r, 1)->i = modes[i][0];
        ck_array_cell(a, r, 2)->i = modes[i][1];
        ck_array_cell(a, r, 3)->i = modes[i][2];
    }
    int32_t installed = 0;
    bb_set_out(ctx, b, 0, &installed, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT OperationSystem 4c94621d:24fe2cf3 (FUN_10027050): GetVersionEx; reported as Windows XP
   (platform 2 = NT, 5.1) ---- */
static int bb_tt_operation_system(CkContext *ctx, CkBehavior *b)
{
    int32_t platform = 2, minor = 1, major = 5;
    bb_set_out(ctx, b, 0, &platform, 4);
    bb_set_out(ctx, b, 1, &minor, 4);
    bb_set_out(ctx, b, 2, &major, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT GetMemoryStatus 3b826e04:6e764285 (FUN_10026d90): GlobalMemoryStatus in MB; reported as a
   512 MB machine with half free ---- */
static int bb_tt_get_memory_status(CkContext *ctx, CkBehavior *b)
{
    int32_t load = 50;
    float total = 512, avail = 256, vtotal = 2048, vavail = 2000;
    bb_set_out(ctx, b, 0, &load, 4);
    bb_set_out(ctx, b, 1, &total, 4);
    bb_set_out(ctx, b, 2, &avail, 4);
    bb_set_out(ctx, b, 3, &vtotal, 4);
    bb_set_out(ctx, b, 4, &vavail, 4);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT_Debug 4a446c43:66fa2375 (FUN_10022b50): pIn DebugText, Console, Log-File, Enable. Prints only when
   the Terratools debug flag is on (TT_DebugON); the input is cleared only then (the original leaves it
   active otherwise). Debug.log is not written. ---- */
static int bb_tt_debug(CkContext *ctx, CkBehavior *b)
{
    int32_t enable = 1;
    bb_get_in(ctx, b, 3, &enable, 4);
    if (enable && ctx->tt_debug) {
        int32_t console = 1;
        bb_get_in(ctx, b, 1, &console, 4);
        const char *text = bb_in_string(ctx, b, 0);
        if (console == 1 && ctx->log) {
            char msg[768];
            CkObj *owner = ck_obj(ctx, b->owner);
            snprintf(msg, sizeof msg, "Debug Output Behavior \n Location \t: %s \n Owner \t\t: %s \n Comment \t: %s",
                     b->h.name, owner ? owner->name : "", text ? text : "");
            ctx->log(msg);
        }
        ck_activate_input(ctx, b, 0, false);
    }
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT_DebugON 3d00718f:2c8b33a2 (FUN_10023000) ---- */
static int bb_tt_debug_on(CkContext *ctx, CkBehavior *b)
{
    ctx->tt_debug = true;
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Show Mouse Cursor 16f6368f:506b60fc (Visuals FUN_25782730) ---- */
static int bb_show_mouse_cursor(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t show = 0;
    bb_get_in(ctx, b, 0, &show, 4);
    ctx->cursor_visible = show != 0;
    return CKBR_OK;
}

/* VxColor (r, g, b, a floats) -> packed ARGB */
static uint32_t rgba_to_argb(const float c[4])
{
    uint32_t k = 0;
    for (int i = 0; i < 4; i++) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        uint32_t byte = (uint32_t)(v * 255 + 0.5f);
        k |= byte << (i == 3 ? 24 : 16 - 8 * i);
    }
    return k;
}

/* ---- Set Background Color f5faaaaa:fdd5bd00 (WorldEnvironments FUN_25882180): render context background ---- */
static int bb_set_background_color(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    float c[4] = {0, 0, 0, 0};
    if (bb_get_in(ctx, b, 0, c, 16)) ctx->background = rgba_to_argb(c);
    return CKBR_OK;
}

/* ---- Time Settings 060a0f8c:09203b5f (BuildingBlocksAddons1 FUN_25114f10): frame rate limits and time
   scale for CKTimeManager. The platform runs a fixed 60 Hz step; the settings are kept for reference. ---- */
static int bb_time_settings(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- TT Change ScreenMode 38b84d97:13932f28 (FUN_10003ce0): pIn Driver ID, ScreenMode ID; the original
   asks the player window to switch. The host owns the window and draws at its size: valid modes succeed. ---- */
static int bb_tt_change_screenmode(CkContext *ctx, CkBehavior *b)
{
    int32_t driver = 0, mode = 0;
    bb_get_in(ctx, b, 0, &driver, 4);
    bb_get_in(ctx, b, 1, &mode, 4);
    ck_activate_output(ctx, b, driver == 0 && mode >= 0 && mode < 10 ? 0 : 1, true);
    return CKBR_OK;
}

BB_DECL(d_tt_player_active, 0694658d, 7ffd236f, "TT Player Active", bb_tt_player_active);
BB_DECL(d_tt_read_registry, 460044b5, 6e927b66, "TT_ReadRegistry", bb_tt_read_registry);
BB_DECL(d_tt_list_driver, 62d00456, 30eb4245, "TT List Driver", bb_tt_list_driver);
BB_DECL(d_tt_list_screenmodes, 4e7a0194, 040328fd, "TT List ScreenModes", bb_tt_list_screenmodes);
BB_DECL(d_tt_operation_system, 4c94621d, 24fe2cf3, "TT OperationSystem", bb_tt_operation_system);
BB_DECL(d_tt_get_memory_status, 3b826e04, 6e764285, "TT GetMemoryStatus", bb_tt_get_memory_status);
BB_DECL(d_tt_debug, 4a446c43, 66fa2375, "TT_Debug", bb_tt_debug);
BB_DECL(d_tt_debug_on, 3d00718f, 2c8b33a2, "TT_DebugON", bb_tt_debug_on);
BB_DECL(d_show_mouse_cursor, 16f6368f, 506b60fc, "Show Mouse Cursor", bb_show_mouse_cursor);
/* ---- Set Fog 151aaaaa:ffd5bdbc (WorldEnvironments; the callback FUN_25882790 picks FUN_25882930 for
   version 2, the game's): render context fog mode, start, end, density, colour from the inputs. (Version 1,
   FUN_258827d0, scaled start and end by 2.8 when it had no local parameter.) ---- */
static int bb_set_fog(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t mode = 0;
    float start = 0, end = 10, density = 1, c[4] = {0, 0, 0, 0};
    bb_get_in(ctx, b, 0, &mode, 4);
    bb_get_in(ctx, b, 1, &start, 4);
    bb_get_in(ctx, b, 2, &end, 4);
    bb_get_in(ctx, b, 3, &density, 4);
    bb_get_in(ctx, b, 4, c, 16);
    if (b->proto_version < 0x20000 && !b->local.n) start *= 2.8f, end *= 2.8f;
    ctx->fog_mode = (uint32_t)mode;
    ctx->fog_start = start, ctx->fog_end = end, ctx->fog_density = density;
    ctx->fog_color = rgba_to_argb(c);
    return CKBR_OK;
}

/* ---- TT Exit to System 5bc50af4:41e92536 (TT_InterfaceManager_RT FUN_10005720): posts message 0x5fa to
   the render window (the player quits), Out. Input 0 isn't cleared. ---- */
static int bb_tt_exit_to_system(CkContext *ctx, CkBehavior *b)
{
    ctx->quit = true;
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

BB_DECL(d_tt_exit_to_system, 5bc50af4, 41e92536, "TT Exit to System", bb_tt_exit_to_system);
BB_DECL(d_set_background_color, f5faaaaa, fdd5bd00, "Set Background Color", bb_set_background_color);
BB_DECL(d_set_fog, 151aaaaa, ffd5bdbc, "Set Fog", bb_set_fog);
BB_DECL(d_time_settings, 060a0f8c, 09203b5f, "Time Settings", bb_time_settings);
BB_DECL(d_tt_change_screenmode, 38b84d97, 13932f28, "TT Change ScreenMode", bb_tt_change_screenmode);

const CkBBDecl *const bb_environment[] = {&d_tt_exit_to_system, 
    &d_tt_player_active, &d_tt_read_registry, &d_tt_list_driver, &d_tt_list_screenmodes, &d_tt_operation_system,
    &d_tt_get_memory_status, &d_tt_debug, &d_tt_debug_on, &d_show_mouse_cursor, &d_set_background_color, &d_set_fog, &d_time_settings,
    &d_tt_change_screenmode,
};
const unsigned bb_environment_count = sizeof bb_environment / sizeof *bb_environment;
