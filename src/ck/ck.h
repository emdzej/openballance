/* The Virtools runtime subset: objects loaded from .cmo/.nmo files, behavior graphs and their scheduler,
   parameters, scenes. Semantics and addresses: docs/ck-runtime.md. */
#pragma once
#include "ck_file.h"
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t CkId;   /* runtime object id, 1-based; 0 = none */

typedef struct {
    CkId *v;
    uint32_t n, cap;
} CkIds;

enum {
    CKCID_OBJECT = 1, CKCID_PARAMETERIN = 2, CKCID_PARAMETEROUT = 3, CKCID_PARAMETEROPERATION = 4,
    CKCID_BEHAVIORLINK = 6, CKCID_BEHAVIOR = 8, CKCID_BEHAVIORIO = 9, CKCID_SCENE = 10,
    CKCID_SCENEOBJECT = 11, CKCID_BEOBJECT = 19, CKCID_LEVEL = 21, CKCID_GROUP = 23,
    CKCID_MATERIAL = 30, CKCID_TEXTURE = 31, CKCID_MESH = 32, CKCID_3DENTITY = 33,
    CKCID_CAMERA = 34, CKCID_TARGETCAMERA = 35, CKCID_LIGHT = 38, CKCID_TARGETLIGHT = 39, CKCID_3DOBJECT = 41, CKCID_PARAMETERLOCAL = 45,
    CKCID_PARAMETER = 46, CKCID_DATAARRAY = 52,
};

/* CKObject flags (+0x0c) */
enum { CK_OBJECT_VISIBLE = 0x40, CK_OBJECT_HIERARCHICALHIDE = 0x200 };

/* CKBehavior flags (+0x54), as far as the scheduler uses them */
enum {
    CKBF_ACTIVE = 0x1,
    CKBF_SCRIPT = 0x2,
    CKBF_BUILDINGBLOCK = 0x8,          /* has a prototype function */
    CKBF_STACKED = 0x100000,           /* on the parent graph's execution stack */
    CKBF_REQ_RESET = 0x800000,         /* pending (de)activation requests, applied by the manager */
    CKBF_REQ_DEACTIVATE = 0x400000,
    CKBF_REQ_ACTIVATE = 0x10000000,
    CKBF_RESET_DONE = 0x80000000,
};

typedef struct CkContext CkContext;

typedef struct {
    CkId id;
    uint32_t cid;
    char *name;
    uint32_t flags;           /* CK_OBJECT_* */
    uint32_t file_id;         /* the id the object was saved with */
    uint32_t file;            /* 1 + index of the file it was loaded from (ctx->files), 0 if created */
    const CkChunk *chunk;     /* the chunk it was loaded from (its initial state), NULL if created */
    uint32_t activity;        /* scene flags saved with the object (CKObjectManager SingleObjectActivity):
                                 BeObjects identifier 0x400, behaviors 0x200000 */
    bool has_activity;
} CkObj;

typedef struct CkBehavior CkBehavior;

/* A Building Block: prototype GUID -> function. The function reads and clears its active inputs,
   activates outputs, and returns CKBR_* (bit 0 set = stay active next frame). */
enum { CKBR_OK = 0, CKBR_ACTIVATENEXTFRAME = 1 };
typedef int (*CkBBFn)(CkContext *ctx, CkBehavior *b);
/* CKM_BEHAVIOR* callback messages */
enum { CKM_BEHAVIORDELETE = 2, CKM_BEHAVIORATTACH = 3, CKM_BEHAVIORDETACH = 4, CKM_BEHAVIORPAUSE = 5, CKM_BEHAVIORRESUME = 6, CKM_BEHAVIORRESET = 9, CKM_BEHAVIORLOAD = 11,
       CKM_BEHAVIORACTIVATESCRIPT = 16, CKM_BEHAVIORDEACTIVATESCRIPT = 17 };
typedef void (*CkBBCallback)(CkContext *ctx, CkBehavior *b, int message);
typedef struct {
    CkGuid guid;
    const char *name;
    CkBBFn fn;
    CkBBCallback callback;    /* optional */
} CkBBDecl;

typedef struct {
    CkObj h;
    bool input;               /* saved flag 1 = input, 2 = output */
    bool active;              /* +0x0c bit 0x40000000 */
    CkId owner;               /* behavior */
    CkIds links;              /* links leaving this IO (as source) */
} CkBehaviorIO;

typedef struct {
    CkObj h;
    int16_t cur_delay, init_delay;
    uint32_t lflags;          /* 1 = queued on the delayed list, 2 = fired this frame */
    CkId src, dst;            /* BehaviorIO */
} CkBehaviorLink;

typedef enum { CKP_IN, CKP_OUT, CKP_LOCAL } CkParamKind;

typedef struct {
    CkObj h;
    CkParamKind kind;
    CkGuid type;
    uint8_t *value;           /* ParameterOut / ParameterLocal value */
    uint32_t size;
    uint32_t value_mode;      /* as saved: 0 sub-chunk, 1 buffer, 2 object, 3 none, other = int */
    CkId source;              /* ParameterIn: direct source (Out/Local) or shared ParameterIn */
    bool shared;
    CkId op;                  /* ParameterOut: the operation writing it (lazy evaluation) */
    CkId owner;               /* behavior, operation or BeObject (attribute) */
    CkIds dests;              /* ParameterOut destinations */
} CkParameter;

typedef struct {
    CkObj h;
    CkGuid op;
    CkId in1, in2, out;
    CkId owner;               /* behavior */
    void *fn;                 /* resolved CkOpFn (ck_types.h), looked up on first use */
    bool swap, resolved;      /* inputs swapped: the function was registered the other way round */
} CkParameterOperation;

struct CkBehavior {
    CkObj h;
    uint32_t bflags;          /* CKBF_* */
    int32_t priority;
    bool has_proto;
    CkGuid proto;
    uint32_t proto_version;
    uint32_t compat_class;
    CkId target;              /* 0xffffffff in the file = none */
    CkId parent, owner;       /* parent graph; owning BeObject (scripts and their sub-behaviors) */
    CkIds sub, links, ops, pin, pout, local, in, out;
    /* graph execution state (+0x5c block) */
    CkIds delayed;            /* +0x24 queued links */
    CkIds stack;              /* +0x30 execution stack */
    const CkBBDecl *bb;       /* resolved prototype, NULL if not implemented */
    void *bb_state;           /* per-instance state owned by the BB */
};

typedef struct {
    CkId obj;
    uint32_t flags;           /* CK_SCENEOBJECT_*: 1 start activate, 8 active, 0x20 start leave, 0x40 start reset */
    CkChunk initial;          /* initial value (IC), if any */
    bool has_initial;
    uint32_t file;            /* 1 + index of the file whose object indices the IC chunk uses */
    struct CkEntitySnapshot *snap;   /* an initial value saved at runtime (TT Save IC); replaces the chunk */
} CkSceneEntry;

typedef struct {
    int32_t type;
    uint32_t send;            /* 1 broadcast (recipient = class id), 2 single, 3 group */
    CkId sender;
    uint32_t recipient;
    CkIds params;             /* parameters attached with CKMessage::AddParameter */
} CkMessage;

typedef struct {
    CkObj h;
    CkIds scripts;
    CkMessage **last_frame;   /* CKBeObject::GetLastFrameMessage: delivered at the end of the previous frame */
    uint32_t nlast;
    bool waiting;             /* CKBeObject::IsWaitingForMessages */
    /* attributes (CKBeObject::SetAttribute): global attribute type and its parameter (0 if the type has none) */
    struct CkAttribute { int32_t type; CkId param; } *attrs;
    uint32_t nattrs;
    /* Level: its scene descriptors */
    CkSceneEntry *scene;
    uint32_t nscene;
} CkBeObject;

typedef struct {
    CkBeObject be;
    CkIds members;
} CkGroup;

/* CKDataArray: columns of int, float, string, object or parameter cells. */
enum { CKARRAYTYPE_INT = 1, CKARRAYTYPE_FLOAT = 2, CKARRAYTYPE_STRING = 3, CKARRAYTYPE_OBJECT = 4, CKARRAYTYPE_PARAMETER = 5 };
typedef struct {
    char *name;
    uint32_t type;
    CkGuid param_type;        /* CKARRAYTYPE_PARAMETER */
} CkArrayColumn;
typedef union {
    int32_t i;
    float f;
    char *s;                  /* owned */
    CkId obj;                 /* object, or the CkParameter holding a parameter cell */
} CkCell;
typedef struct {
    CkBeObject be;
    CkArrayColumn *cols;
    uint32_t ncols;
    CkCell *cells;            /* rows * ncols */
    uint32_t nrows, cap_rows;
    int32_t key_column;
    int32_t sort_order, sort_column;   /* as saved (+0x68, +0x6c); the game never sorts on load */
} CkDataArray;

CkDataArray *ck_array(const CkContext *ctx, CkId id);
CkCell *ck_array_cell(CkDataArray *a, uint32_t row, uint32_t col);   /* NULL if out of range */
uint32_t ck_array_add_row(CkContext *ctx, CkDataArray *a);            /* new row index */
void ck_array_remove_row(CkContext *ctx, CkDataArray *a, uint32_t row);
void ck_array_clear(CkContext *ctx, CkDataArray *a);
/* CKDataArray::InsertColumn 0x2402696f (index -1 = append; parameter columns get a parameter per row),
   RemoveColumn 0x24026c3d */
void ck_array_insert_column(CkContext *ctx, CkDataArray *a, int32_t index, uint32_t type, const char *name, CkGuid ptype);
void ck_array_remove_column(CkContext *ctx, CkDataArray *a, int32_t index);
/* CKDataArray::SetElementValueFromParameter 0x240274b6: string columns take the parameter's string value,
   parameter columns copy its value, others its first dword. false if out of range or no parameter. */
bool ck_array_set_from_param(CkContext *ctx, CkDataArray *a, uint32_t row, uint32_t col, CkParameter *p);
/* CKDataArray::FindRowIndex 0x24028128: first row >= start whose cell in col compares (CK_COMPOPERATOR
   1..6) with the key: int/object/float columns use the key bits, string and parameter columns keyptr
   (+ size). -1 if none. */
int32_t ck_array_find_row(CkContext *ctx, CkDataArray *a, uint32_t col, int32_t op, uint32_t key, const void *keyptr,
                          uint32_t size, int32_t start);
bool ck_array_test_cell(CkContext *ctx, CkDataArray *a, int32_t row, int32_t col, int32_t op, uint32_t key,
                        const void *keyptr, uint32_t size);
int32_t ck_array_count(CkContext *ctx, CkDataArray *a, int32_t col, int32_t op, uint32_t key, const void *keyptr,
                       uint32_t size);
/* column compare (CKDataArray column +0x10): > 0 when row cand's cell is higher than row best's */
int ck_array_compare_rows(CkContext *ctx, const CkDataArray *a, uint32_t col, uint32_t best, uint32_t cand);
void ck_array_sort(CkContext *ctx, CkDataArray *a, int32_t col, bool ascending);   /* CKDataArray::Sort */
/* Comparison Operator (Equal=1, Not Equal, Less, Less or equal, Greater, Greater or equal) on a <=> 0 */
bool ck_compare_int(int32_t op, int32_t a, int32_t b);
bool ck_compare_float(int32_t op, float a, float b);

/* Attribute types (CKAttributeManager): registered by name, globally; a file's attribute indices (its
   attribute manager chunk) are remapped by name when it loads. */
typedef struct {
    char *name, *category;
    CkGuid param_type;        /* {0,0}: no parameter */
    uint32_t compatible_class, flags;
    CkIds objects;            /* the objects that have it, in the order it was set (GetAttributeListPtr) */
} CkAttributeType;

struct CkContext {
    CkObj **objs;             /* objs[id - 1] */
    uint32_t nobjs, cap;
    CkFile *files;            /* loaded files, kept alive (chunks point into them) */
    CkId *file_base;          /* per file: runtime id of its object 0 */
    uint32_t nfiles;
    CkId level;
    CkIds managed;            /* BeObjects with scripts, registration order (behavior manager +0x34) */
    float delta_ms;           /* current frame's time step */
    uint32_t frame;
    uint32_t rand_seed;       /* the C library's rand() state (MSVC), seeded 1 */
    int max_iterations;       /* CKBehaviorManager::GetBehaviorMaxIteration, default 8000 */
    /* messages (CKMessageManager): global message types by name, the queue, the waits */
    char **messages;
    uint32_t nmessages;
    CkAttributeType *attr_types;
    uint32_t nattr_types;
    CkMessage **queue;
    uint32_t nqueue, capqueue;
    struct CkWait { CkId obj, beh, io; } **waits;   /* waits[type]: array terminated by nwaits[type] */
    uint32_t *nwaits;
    uint32_t nwait_types;
    CkIds received;           /* objects holding last-frame messages (manager +0x44) */
    /* the font manager (Interface.dll, docs/fonts.md): 1-based font indices; the 2D Text behaviors that
       registered their entity's post-render text draw this frame */
    struct CkFont **fonts;
    uint32_t nfonts;
    CkIds text_draws;
    /* particle systems with a registered post-render callback on their owner frame (behavior ids; the
       emitter is the behavior's bb_state, a CkParticleSystem) */
    CkIds particle_systems;
    /* Planar Filter behaviors with a post-sprite render callback on the render context */
    CkIds planar_filters;
    /* the render context's temporary pre-render callbacks (CKRenderContext::AddPreRenderCallBack with
       temporary = TRUE): run once before the next frame is drawn, then dropped */
    struct CkPreRender { void (*fn)(CkContext *ctx, CkBehavior *b); CkId beh; } *pre_render;
    uint32_t npre_render;
    /* TT Exit to System posted the player window's quit message (0x5fa): the host stops after this frame */
    bool quit;
    /* TT_Toolbox_RT's TT_Sceneanager: the one TT_Timer chronometer */
    bool tt_timer_paused;
    float tt_timer_ms;
    /* the physics manager (physics_RT.dll, src/bb/bb_physics.c): its state, PostProcess and cleanup */
    /* the 3D hierarchy's generation (bumped wherever a parent can change: loads, copies, IC restores, Set
       Parent, destruction) and the children index built from it (ck_entity_descendants) */
    uint32_t hier_gen;
    void *hier_cache;
    void *physics;
    void (*physics_frame)(CkContext *ctx);
    void (*physics_free)(CkContext *ctx);
    /* BB registry */
    const CkBBDecl *const *bbs;
    uint32_t nbbs;
    /* user files (saves: Database.tdb), provided by the app: malloc'd bytes or NULL / success */
    uint8_t *(*load_user_file)(CkContext *ctx, const char *name, size_t *size);
    bool (*save_user_file)(CkContext *ctx, const char *name, const void *data, size_t size);
    /* Windows registry values the game reads (TT_ReadRegistry), provided by the app: returns the value
       as a string, or NULL if absent. section like "Software\\Ballance\\Settings\\", entry "Language". */
    const char *(*registry)(CkContext *ctx, const char *section, const char *entry);
    /* sound minions (CKWaveSound::PlayMinion): one-shot copies of sounds */
    void *minions;            /* Voice[] (audio.h) */
    uint32_t nminions;
    /* render environment set by scripts */
    /* input (CKInputManager): DirectInput key codes (DIK_*), down this frame / the previous one; the mouse
       in render context pixels (640x480) and its buttons (bit 0 left, 1 right, 2 middle) */
    uint8_t keys[256], keys_prev[256];
    float mouse[2], mouse_prev[2];
    uint32_t mouse_buttons, mouse_buttons_prev;
    /* the render context (CKRenderContext): background, ambient light, fog, viewpoint */
    uint32_t background;      /* ARGB (Set Background Color) */
    uint32_t ambient;         /* ARGB (SetAmbientLight) */
    uint32_t fog_mode;        /* VXFOG_*: 0 none, 1 exp, 2 exp2, 3 linear */
    uint32_t fog_color;       /* ARGB */
    float fog_start, fog_end, fog_density;
    CkId camera;              /* the viewpoint (AttachViewpointToCamera), 0 = the default one */
    /* the level scene's environment (CKScene 0x40000 flags, 0x80000), applied at launch with flag 2 */
    uint32_t scene_flags;
    struct {
        uint32_t background, ambient, fog_mode, fog_color;
        float fog_start, fog_end, fog_density;
        CkId background_texture, camera;
    } scene_env;
    bool cursor_visible;
    bool tt_debug;            /* TT_DebugON (Terratools debug manager 47510373:711473d7) */
    /* TT_DatabaseManager (manager 4db6188e:287e1410) */
    char *db_file;
    int32_t db_crypted;
    char **db_arrays;         /* registered array names */
    uint32_t ndb_arrays;
    void (*trace)(CkContext *ctx, CkBehavior *b, const char *what);
    void (*log)(const char *msg);
};

/* ---- lifetime ---- */
void ck_init(CkContext *ctx);
void ck_free(CkContext *ctx);
/* Loads a composition (base.cmo) or an object file (.nmo) into the context. Returns false with a
   message on error. The new objects' ids are appended to *loaded if not NULL. */
bool ck_load(CkContext *ctx, const char *path, CkIds *loaded, char *err, size_t errlen);

/* Manager GUIDs (first dword; the second is 0) */
enum { CK_ATTRIBUTE_MANAGER = 0x3d242466, CK_MESSAGE_MANAGER = 0x466a0fac };

int32_t ck_attribute_type(CkContext *ctx, const char *name);   /* registers it if new */
const CkAttributeType *ck_attribute_info(const CkContext *ctx, int32_t type);   /* NULL if invalid */
bool ck_has_attribute(const CkContext *ctx, CkId obj, int32_t type);
/* CKBeObject::SetAttribute: adds the attribute (with a new parameter of the type's parameter type, or
   param when given) unless present; false if obj isn't a BeObject or the type is invalid */
bool ck_set_attribute(CkContext *ctx, CkId obj, int32_t type, CkId param);
CkId ck_attribute_parameter(const CkContext *ctx, CkId obj, int32_t type);   /* 0 if none */
bool ck_remove_attribute(CkContext *ctx, CkId obj, int32_t type);

/* Global message type for a name (registered on first use), CKMessageManager::AddMessageType. */
int32_t ck_message_type(CkContext *ctx, const char *name);
const char *ck_message_name(const CkContext *ctx, int32_t type);

/* CKMessageManager::SendMessageSingle 0x2400d211 / SendMessageGroup / SendMessageBroadcast: queue a
   message (delivered in the manager's PostProcess, FUN_2400dc9a). Returns the message for AddParameter. */
CkMessage *ck_send_message(CkContext *ctx, int32_t type, uint32_t send, uint32_t recipient, CkId sender);
/* CKMessageManager::RegisterWait 0x2400d99f / UnRegisterWait 0x2400daa6 */
void ck_register_wait(CkContext *ctx, int32_t type, CkBehavior *b, uint32_t output, CkId obj);
void ck_unregister_wait(CkContext *ctx, int32_t type, CkBehavior *b, int32_t output);
/* The message manager's PostProcess (FUN_2400dc9a): deliver the queue. */
void ck_deliver_messages(CkContext *ctx);

/* ---- objects ---- */
CkObj *ck_obj(const CkContext *ctx, CkId id);
CkBehavior *ck_behavior(const CkContext *ctx, CkId id);
CkBehaviorIO *ck_io(const CkContext *ctx, CkId id);
CkBehaviorLink *ck_link(const CkContext *ctx, CkId id);
CkParameter *ck_param(const CkContext *ctx, CkId id);
CkBeObject *ck_beobject(const CkContext *ctx, CkId id);
bool ck_is_beobject_class(uint32_t cid);
/* CKIsChildClassOf: cid is base or derives from it (Virtools class tree). */
bool ck_class_derives(uint32_t cid, uint32_t base);
CkId ck_find(const CkContext *ctx, const char *name, uint32_t cid);  /* first match, 0 if none */
/* CKContext::DestroyObject: frees the object; drops it from the scene, the manager's list and groups. */
/* CKDependencies: mode, and with custom per-class flags (CK_DEPENDENCIES_* of each class: BeObject 1
   scripts, 2 attributes; 3dEntity 1 meshes, 2 children; Mesh 1 materials; Material 1 textures; 2dEntity
   1 material ...) */
enum { CK_DEPENDENCIES_CUSTOM = 0, CK_DEPENDENCIES_NONE = 1, CK_DEPENDENCIES_FULL = 2, CK_MAX_CLASSES = 64 };
typedef struct {
    int32_t mode;
    uint32_t flags[CK_MAX_CLASSES];
} CkDependencies;
#define CK_GUID_DEPENDENCIES ((CkGuid){0x69590c80u, 0x30b61630u})
#define CK_GUID_COPY_DEPENDENCIES ((CkGuid){0x748d4e0du, 0x3bf7195bu})
/* CKContext::CopyObjects (CK2.dll 0x24034dc1, CKDependenciesContext::Copy 0x2401cd90): copies of the objects
   and their dependencies (re-read from their chunks with references among them remapped, then the
   current state of matrices, rectangles, materials and parameter values). Scene objects join the level
   scene like their original (activated when the original is active, or BeObjects with activate). The
   copies of `objects` go to `out` in order. */
void ck_copy_objects(CkContext *ctx, const CkIds *objects, const CkDependencies *deps, bool dynamic, bool activate,
                     CkIds *out);
void ck_destroy_with_dependencies(CkContext *ctx, CkId id, int32_t mode);
void ck_destroy(CkContext *ctx, CkId id);
/* Creates an object at runtime (CKContext::CreateObject). */
CkObj *ck_create(CkContext *ctx, uint32_t cid, const char *name);
void ck_ids_push(CkIds *l, CkId id);
bool ck_ids_has(const CkIds *l, CkId id);

/* ---- behaviors (the CKBehavior API used by BBs) ---- */
void ck_behavior_activate(CkContext *ctx, CkBehavior *b, bool active, bool reset);
bool ck_behavior_execute(CkContext *ctx, CkBehavior *b);   /* false: iteration limit hit */
bool ck_input_active(CkContext *ctx, CkBehavior *b, uint32_t i);
void ck_activate_input(CkContext *ctx, CkBehavior *b, uint32_t i, bool on);
void ck_activate_output(CkContext *ctx, CkBehavior *b, uint32_t i, bool on);

/* ---- parameters ---- */
/* Value of input parameter i of b, after resolving sources and running operations. NULL if none. */
const void *ck_input_value(CkContext *ctx, CkBehavior *b, uint32_t i, uint32_t *size);
int32_t ck_input_int(CkContext *ctx, CkBehavior *b, uint32_t i);
float ck_input_float(CkContext *ctx, CkBehavior *b, uint32_t i);
void ck_output_set(CkContext *ctx, CkBehavior *b, uint32_t i, const void *v, uint32_t size);
void ck_param_set(CkParameter *p, const void *v, uint32_t size);
/* Resolves a ParameterIn to the parameter holding its value (runs operations on the way). */
CkParameter *ck_param_resolve(CkContext *ctx, CkParameter *pin);
/* CKBehavior::GetTarget: the target parameter's object, else the owner */
CkId ck_behavior_target(CkContext *ctx, CkBehavior *b);

/* ---- frames and scenes ---- */
/* CKLevel::LaunchScene(level scene, default flags): activates scripts per scene descriptors. */
void ck_launch_level_scene(CkContext *ctx);
/* CKScene::Activate / DeActivate on the current (level) scene: behaviors get activation requests applied
   by the next ck_process (FUN_2402eeb0), BeObjects become active and managed (FUN_2402ed90). */
/* rand() as the original's C library (msvcrt): 0..0x7fff */
static inline int ck_rand(CkContext *ctx)
{
    ctx->rand_seed = ctx->rand_seed * 214013u + 2531011u;
    return (int)(ctx->rand_seed >> 16 & 0x7fff);
}
/* CKReadObjectState: reloads an object's state from a chunk saved in file (1-based; see CkObj.file)
   without its scripts: flags, parameter values, arrays, group members, 3D/2D entities, materials. */
void ck_read_object_state(CkContext *ctx, CkId id, const CkChunk *c, uint32_t file);
/* CKScene::SetObjectInitialValue(obj, CKSaveObjectState(obj)) for 3D entities (their matrix, flags, mesh,
   parent, visibility), and restoring the initial value (the saved state, else the IC chunk) */
void ck_save_initial_state(CkContext *ctx, CkId id);
void ck_restore_initial_state(CkContext *ctx, CkId id);
/* The level scene's initial value of an object (CKScene::GetObjectInitialValue), NULL if none. */
const CkSceneEntry *ck_scene_entry(const CkContext *ctx, CkId id);
/* CKBehavior::CallSubBehaviorsCallbackFunction(msg) over the behavior's tree */
void ck_behavior_callback_tree(CkContext *ctx, CkBehavior *b, uint32_t msg);
/* CKScene::AddObject + SetObjectFlags (no initial value) */
void ck_scene_add_copy(CkContext *ctx, CkId obj, uint32_t flags);
void ck_scene_activate(CkContext *ctx, CkId obj, bool reset);
/* CKRenderContext::AddPreRenderCallBack(fn, beh, temporary = TRUE) / RemovePreRenderCallBack; the renderer
   runs (and drops) them before drawing */
void ck_add_pre_render(CkContext *ctx, void (*fn)(CkContext *ctx, CkBehavior *b), CkId beh);
void ck_remove_pre_render(CkContext *ctx, void (*fn)(CkContext *ctx, CkBehavior *b), CkId beh);
void ck_run_pre_render(CkContext *ctx);
void ck_scene_deactivate(CkContext *ctx, CkId obj);
bool ck_scene_is_active(const CkContext *ctx, CkId obj);   /* CKSceneObject::IsActiveInCurrentScene */
/* CKLevel::AddObject 0x2402c49a -> CKScene::AddObject 0x2402dc8a on the level scene (BeObjects bring their
   scripts, CKBeObject::AddToScene 0x2401b8ca): a new scene entry takes the object's saved activity and
   activates/deactivates it accordingly. */
void ck_level_add_object(CkContext *ctx, CkId obj);
/* One CKBehaviorManager::Execute step. */
void ck_process(CkContext *ctx, float delta_ms);
