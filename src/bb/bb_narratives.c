/* Narratives.dll Building Blocks (script and object management). */
#include "bb.h"
#include "../ck/ck_sound.h"
#include "../ck/ck_3d.h"
#include <stdlib.h>

/* ---- Activate Script 4c7e7bc3:0b693155 (FUN_25685290): pIn Reset?, Script...; setting Awake Object ---- */
static int bb_activate_script(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkParameter *p0 = b->pin.n ? ck_param(ctx, b->pin.v[0]) : NULL;
    if (!p0 || !ck_guid_eq(p0->type, CKPGUID_BOOL)) {
        /* old layout: pIn 0 = script, pIn 1 = reset */
        int32_t reset = 1;
        bb_get_in(ctx, b, 1, &reset, 4);
        CkBehavior *s = ck_behavior(ctx, bb_in_object(ctx, b, 0));
        if (!s) return CKBR_OK;
        ck_scene_activate(ctx, s->h.id, reset != 0);
        if (!ck_scene_is_active(ctx, s->owner)) ck_scene_activate(ctx, s->owner, false);
        return CKBR_OK;
    }
    int32_t reset = 1, awake = 1;
    bb_get_in(ctx, b, 0, &reset, 4);
    bb_get_local(ctx, b, 0, &awake, 4);
    for (uint32_t i = 1; i < b->pin.n; i++) {
        CkBehavior *s = ck_behavior(ctx, bb_in_object(ctx, b, i));
        if (!s) continue;
        ck_scene_activate(ctx, s->h.id, reset != 0);
        if (awake && !ck_scene_is_active(ctx, s->owner)) ck_scene_activate(ctx, s->owner, false);
    }
    return CKBR_OK;
}

/* ---- Deactivate Script 14367c05:635b24f9 (FUN_25686190) ---- */
static int bb_deactivate_script(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    for (uint32_t i = 0; i < b->pin.n; i++) ck_scene_deactivate(ctx, bb_in_object(ctx, b, i));
    return CKBR_OK;
}

/* ---- Execute Script 706c5a40:5bb31a0b (FUN_25686390): activates the script, then waits until it is no
   longer active ---- */
static int bb_execute_script(CkContext *ctx, CkBehavior *b)
{
    CkBehavior *s = ck_behavior(ctx, bb_in_object(ctx, b, 1));
    if (!s) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        ck_activate_output(ctx, b, 0, false);
        int32_t reset = 0, awake = 1;
        bb_get_in(ctx, b, 0, &reset, 4);
        ck_scene_activate(ctx, s->h.id, reset != 0);
        bb_get_local(ctx, b, 0, &awake, 4);
        if (awake && !ck_scene_is_active(ctx, s->owner)) ck_scene_activate(ctx, s->owner, false);
    } else if (!(s->bflags & CKBF_ACTIVE)) {
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    return CKBR_ACTIVATENEXTFRAME;
}

/* ---- Object Load 7bd977d7:26396c0c (FUN_25681c30): inputs Load, Unload; outputs Loaded, Unloaded,
   Failed; pIn File, Master Object Name, Master Object Class (default 3D Object), Add to Scene, Reuse Meshes,
   Reuse Materials; setting Dynamic; pOut Loaded Objects (object array), Master Object.
   Every loaded object joins the level (CKLevel::AddObject) and so its scene with its saved activity; the
   current scene is always the level scene here, so "Add to Scene" has no further effect. A file that
   holds a level would be merged (CKLevel::Merge) - the game's object files don't. ---- */
static int bb_object_load(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 0)) {
        ck_activate_input(ctx, b, 0, false);
        const char *file = bb_in_string(ctx, b, 0), *master = bb_in_string(ctx, b, 1);
        int32_t cls = CKCID_3DOBJECT;
        bb_get_in(ctx, b, 2, &cls, 4);
        CkIds loaded = {0};
        char err[160];
        if (!file || !ck_load(ctx, file, &loaded, err, sizeof err)) {
            if (ctx->log) ctx->log(file ? err : "Object Load: no file");
            free(loaded.v);
            ck_activate_output(ctx, b, 2, true);
            return CKBR_OK;
        }
        ck_activate_output(ctx, b, 0, true);
        CkId master_obj = 0;
        for (uint32_t i = 0; i < loaded.n; i++) {
            CkObj *o = ck_obj(ctx, loaded.v[i]);
            ck_level_add_object(ctx, o->id);
            if (!ck_class_derives(o->cid, (uint32_t)cls)) continue;
            bool pick;
            if (!master || !*master) {
                /* first root entity: a 3D entity without parent, or a 2D entity without parent */
                if (ck_class_derives(o->cid, CKCID_3DENTITY)) pick = ((Ck3dEntity *)o)->parent == 0;
                else pick = o->cid == 27 || o->cid == 28 || o->cid == 29;   /* TODO: 2D entity parent */
            } else {
                pick = !strcmp(o->name, master);
            }
            if (pick) master_obj = o->id;
        }
        bb_set_out(ctx, b, 0, loaded.v, loaded.n * 4);
        bb_set_out(ctx, b, 1, &master_obj, 4);
        free(loaded.v);
    }
    if (ck_input_active(ctx, b, 1)) {
        ck_activate_input(ctx, b, 1, false);
        ck_activate_output(ctx, b, 1, true);
        CkParameter *p = bb_out(ctx, b, 0);
        if (p && p->value) {
            for (uint32_t i = 0; i + 4 <= p->size; i += 4) {
                CkId id;
                memcpy(&id, p->value + i, 4);
                ck_destroy(ctx, id);
            }
        }
        bb_set_out(ctx, b, 0, NULL, 0);
    }
    return CKBR_OK;
}

/* ---- Object Delete 74120ded:76524673 (FUN_25681930): CKContext::DestroyObject of pIn Object with pIn
   Dependency Options; without the options input, of the target. ---- */
static int bb_object_delete(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t deps = CK_DEPENDENCIES_NONE;
    CkId id;
    if (b->pin.n > 1) {
        bb_get_in(ctx, b, 1, &deps, 4);
        id = bb_in_object(ctx, b, 0);
    } else {
        id = bb_target(ctx, b);
    }
    if (id) ck_destroy_with_dependencies(ctx, id, deps);
    return CKBR_OK;
}

/* ---- Deactivate Object 160f4b7d:67de224e (FUN_25686020): CKScene::DeActivate of pIn Object in the
   current (level) scene ---- */
static int bb_deactivate_object(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkId id = bb_in_object(ctx, b, 0);
    if (id) ck_scene_deactivate(ctx, id);
    return CKBR_OK;
}

/* ---- Restore IC 766e4e44:4fac6d52 (FUN_256844d0): the target back to its initial value in the current
   scene (CKReadObjectState with CKScene::GetObjectInitialValue) ---- */
static int bb_restore_ic(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    ck_restore_initial_state(ctx, bb_target(ctx, b));
    return CKBR_OK;
}

/* ---- Texture Load 00e85ab1:312a731c (FUN_256835b0): CKTexture::LoadImage(pIn Texture File resolved by the
   path manager, pIn Slot) on the target texture; Loaded (with pOut the texture) or Error. The setting
   "asynchronous" loads on a thread and reports on a later frame; here the load is always immediate. ---- */
static int bb_texture_load(CkContext *ctx, CkBehavior *b)
{
    CkTexture *t = ck_texture(ctx, bb_target(ctx, b));
    if (!t) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    if (!ck_input_active(ctx, b, 0)) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    const char *file = bb_in_string(ctx, b, 0);
    int32_t slot = 0;
    bb_get_in(ctx, b, 1, &slot, 4);
    bool ok = file && ck_texture_load_image(t, file, slot);
    if (ok) {
        CkId id = t->be.h.id;
        bb_set_out(ctx, b, 0, &id, 4);
    }
    ck_activate_output(ctx, b, ok ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Object Create 271538e6:2fae49ac (FUN_25681720): CKContext::CreateObject(pIn Class, pIn Name; setting
   Dynamic = CK_OBJECTCREATION_DYNAMIC) added to the level (scenes: CKLevel::AddScene, not modelled);
   pOut the object. Levels can't be created. ---- */
static int bb_object_create(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t cid = 33;
    bb_get_in(ctx, b, 0, &cid, 4);
    if (ck_class_derives((uint32_t)cid, CKCID_LEVEL)) {
        if (ctx->log) ctx->log("Cannot create a level");
        return CKBR_OK;
    }
    const char *name = bb_in_string(ctx, b, 1);
    CkObj *o = ck_create(ctx, (uint32_t)cid, name);
    if (!ck_class_derives((uint32_t)cid, CKCID_SCENE)) ck_level_add_object(ctx, o->id);
    CkId id = o->id;
    bb_set_out(ctx, b, 0, &id, 4);
    return CKBR_OK;
}

/* ---- Sound Load 02bc537c:147c22e0 (FUN_25682440): the target wave sound gets pIn File (resolved by the
   path manager; pIn Streamed? is irrelevant here) -> Out, or Failed. The asynchronous setting loads on a
   thread in the original; here it is immediate. ---- */
static int bb_sound_load(CkContext *ctx, CkBehavior *b)
{
    CkObj *o = ck_obj(ctx, bb_target(ctx, b));
    if (!o || o->cid != 25) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    if (!ck_input_active(ctx, b, 0)) return CKBR_OK;
    ck_activate_input(ctx, b, 0, false);
    const char *file = bb_in_string(ctx, b, 0);
    ck_activate_output(ctx, b, file && ck_sound_set_file((CkWaveSound *)o, file) ? 0 : 1, true);
    return CKBR_OK;
}

/* ---- Object Copy 3f6b0ac7:47d20f78 (FUN_25681380): CKContext::CopyObjects of pIn 1... (levels excluded)
   with pIn 0's copy dependencies; setting 0 Dynamic, setting 1 activate (always before version 2.0); pOut i
   = the copy of the i-th object ---- */
static int bb_object_copy(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    int32_t dynamic = 1, activate = 1;
    bb_get_local(ctx, b, 0, &dynamic, 4);
    bb_get_local(ctx, b, 1, &activate, 4);
    if (b->proto_version < 0x20000) activate = 1;
    CkIds objs = {0}, copies = {0};
    for (uint32_t i = 1; i < b->pin.n; i++) {
        CkObj *o = ck_obj(ctx, bb_in_object(ctx, b, i));
        if (o && !ck_class_derives(o->cid, CKCID_LEVEL)) ck_ids_push(&objs, o->id);
    }
    CkDependencies deps;
    memset(&deps, 0, sizeof deps);
    deps.mode = CK_DEPENDENCIES_FULL;
    CkParameter *dp = bb_in(ctx, b, 0);
    if (dp && dp->value) memcpy(&deps, dp->value, dp->size < sizeof deps ? dp->size : sizeof deps);
    ck_copy_objects(ctx, &objs, &deps, dynamic != 0, activate != 0, &copies);
    for (uint32_t i = 0; i < copies.n; i++) bb_set_out(ctx, b, i, &copies.v[i], 4);
    free(objs.v);
    free(copies.v);
    return CKBR_OK;
}

/* TT Save IC / TT Restore IC with Hierarchy?: the target, then (3D entities) its descendants */
static void ic_targets(CkContext *ctx, CkBehavior *b, CkIds *out)
{
    CkId t = bb_target(ctx, b);
    ck_ids_push(out, t);
    int32_t hier = 0;
    bb_get_in(ctx, b, 0, &hier, 1);
    if ((hier & 0xff) && ck_entity(ctx, t)) ck_entity_descendants(ctx, t, out);
}

/* ---- TT Save IC 30362f34:1935316f (TT_Toolbox_RT FUN_1001cee0): the target's current state becomes its
   initial value in the scene (CKSaveObjectState + CKScene::SetObjectInitialValue), with pIn Hierarchy? also
   for its children ---- */
static int bb_tt_save_ic(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkIds t = {0};
    ic_targets(ctx, b, &t);
    for (uint32_t i = 0; i < t.n; i++) ck_save_initial_state(ctx, t.v[i]);
    free(t.v);
    return CKBR_OK;
}

/* ---- TT Restore IC 7e0f2f58:232d3109 (TT_Toolbox_RT FUN_1001cbc0): back to the initial value, with pIn
   Hierarchy? also the children ---- */
static int bb_tt_restore_ic(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    CkIds t = {0};
    ic_targets(ctx, b, &t);
    for (uint32_t i = 0; i < t.n; i++) ck_restore_initial_state(ctx, t.v[i]);
    free(t.v);
    return CKBR_OK;
}

BB_DECL(d_activate_script, 4c7e7bc3, 0b693155, "Activate Script", bb_activate_script);
BB_DECL(d_deactivate_script, 14367c05, 635b24f9, "Deactivate Script", bb_deactivate_script);
BB_DECL(d_execute_script, 706c5a40, 5bb31a0b, "Execute Script", bb_execute_script);
BB_DECL(d_object_load, 7bd977d7, 26396c0c, "Object Load", bb_object_load);
BB_DECL(d_object_delete, 74120ded, 76524673, "Object Delete", bb_object_delete);
BB_DECL(d_deactivate_object, 160f4b7d, 67de224e, "Deactivate Object", bb_deactivate_object);
/* ---- Delete Dynamic Objects 0cba3195:53440e4c (FUN_256810f0): CKContext::DestroyAllDynamicObjects, Out. The
   game runs it only on the way out (before TT Exit to System); dynamic objects aren't tracked separately
   here, so nothing is destroyed. ---- */
static int bb_delete_dynamic_objects(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

BB_DECL(d_delete_dynamic_objects, 0cba3195, 53440e4c, "Delete Dynamic Objects", bb_delete_dynamic_objects);
BB_DECL(d_restore_ic, 766e4e44, 4fac6d52, "Restore IC", bb_restore_ic);
BB_DECL(d_texture_load, 00e85ab1, 312a731c, "Texture Load", bb_texture_load);
BB_DECL(d_object_create, 271538e6, 2fae49ac, "Object Create", bb_object_create);
BB_DECL(d_sound_load, 02bc537c, 147c22e0, "Sound Load", bb_sound_load);
BB_DECL(d_object_copy, 3f6b0ac7, 47d20f78, "Object Copy", bb_object_copy);
BB_DECL(d_tt_save_ic, 30362f34, 1935316f, "TT Save IC", bb_tt_save_ic);
BB_DECL(d_tt_restore_ic, 7e0f2f58, 232d3109, "TT Restore IC", bb_tt_restore_ic);

const CkBBDecl *const bb_narratives[] = {&d_delete_dynamic_objects, &d_activate_script, &d_deactivate_script, &d_execute_script, &d_object_load, &d_object_delete,
                                        &d_deactivate_object, &d_restore_ic, &d_texture_load, &d_object_create, &d_sound_load,
                                        &d_object_copy, &d_tt_save_ic, &d_tt_restore_ic};
const unsigned bb_narratives_count = sizeof bb_narratives / sizeof *bb_narratives;
