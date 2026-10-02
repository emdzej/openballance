/* Building runtime objects from a loaded file (the per-class Load functions, CK2.dll; see
   docs/ck-runtime.md). Object references in the file are file indices; they are remapped to runtime ids
   once every object of the file exists. */
#include "ck_3d.h"
#include "ck_anim.h"
#include "ck_curve.h"
#include "ck_font.h"
#include "ck_types.h"
#include "ck_sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ck_ids_push(CkIds *l, CkId id)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = id;
}

bool ck_ids_has(const CkIds *l, CkId id)
{
    for (uint32_t i = 0; i < l->n; i++)
        if (l->v[i] == id) return true;
    return false;
}

static void ids_free(CkIds *l)
{
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* Parent class of each CKCID (the Virtools class hierarchy). */
static uint32_t class_parent(uint32_t c)
{
    switch (c) {
    case 2: case 4: case 5: case 6: case 8: case 9: case 11: case 12: case 13: case 46: return 1;
    case 3: case 45: return 46;
    case 15: case 16: case 19: return 11;
    case 18: return 16;
    case 10: case 21: case 22: case 23: case 24: case 30: case 31: case 32: case 47: case 52: return 19;
    case 25: case 26: return 24;
    case 27: case 33: return 47;
    case 28: return 27;
    case 29: return 28;
    case 34: case 36: case 37: case 38: case 40: case 41: case 43: case 50: return 33;
    case 35: return 34;
    case 39: return 38;
    case 42: return 41;
    case 53: case 54: return 32;
    }
    return 0;
}

bool ck_class_derives(uint32_t cid, uint32_t base)
{
    for (int d = 0; d < 16 && cid; d++, cid = class_parent(cid))
        if (cid == base) return true;
    return false;
}

bool ck_is_beobject_class(uint32_t c)
{
    /* CKBeObject and its descendants (CKScene .. CKGrid), not behaviors, parameters or links */
    switch (c) {
    case 10: case 19: case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
    case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39: case 40:
    case 41: case 42: case 43: case 47: case 50: case 52: case 53: case 54:
        return true;
    }
    return false;
}

static size_t obj_size(uint32_t cid)
{
    switch (cid) {
    case CKCID_BEHAVIOR: return sizeof(CkBehavior);
    case CKCID_BEHAVIORIO: return sizeof(CkBehaviorIO);
    case CKCID_BEHAVIORLINK: return sizeof(CkBehaviorLink);
    case CKCID_PARAMETERIN: case CKCID_PARAMETEROUT: case CKCID_PARAMETERLOCAL: case CKCID_PARAMETER:
        return sizeof(CkParameter);
    case CKCID_PARAMETEROPERATION: return sizeof(CkParameterOperation);
    case CKCID_GROUP: return sizeof(CkGroup);
    case CKCID_DATAARRAY: return sizeof(CkDataArray);
    }
    if (cid == 25) return sizeof(CkWaveSound);
    size_t sa = ck_anim_obj_size(cid);
    if (sa) return sa;
    size_t s3 = ck_3d_obj_size(cid);
    if (s3) return s3;
    return ck_is_beobject_class(cid) ? sizeof(CkBeObject) : sizeof(CkObj);
}

void ck_init(CkContext *ctx)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->max_iterations = 8000;
    ctx->rand_seed = 1;
}

static void array_free(CkDataArray *a)
{
    for (uint32_t c = 0; c < a->ncols; c++) {
        if (a->cols[c].type == CKARRAYTYPE_STRING)
            for (uint32_t r = 0; r < a->nrows; r++) free(a->cells[r * a->ncols + c].s);
        free(a->cols[c].name);
    }
    free(a->cols);
    free(a->cells);
    a->cols = NULL;
    a->cells = NULL;
    a->ncols = a->nrows = a->cap_rows = 0;
}

static void obj_free(CkObj *o)
{
    switch (o->cid) {
    case CKCID_BEHAVIOR: {
        CkBehavior *b = (CkBehavior *)o;
        CkIds *l[] = {&b->sub, &b->links, &b->ops, &b->pin, &b->pout, &b->local, &b->in, &b->out, &b->delayed, &b->stack};
        for (size_t i = 0; i < sizeof l / sizeof *l; i++) ids_free(l[i]);
        free(b->bb_state);
        break;
    }
    case CKCID_BEHAVIORIO: ids_free(&((CkBehaviorIO *)o)->links); break;
    case CKCID_PARAMETERIN: case CKCID_PARAMETEROUT: case CKCID_PARAMETERLOCAL: case CKCID_PARAMETER:
        free(((CkParameter *)o)->value);
        ids_free(&((CkParameter *)o)->dests);
        break;
    default:
        ck_anim_free(o);
        if (ck_is_beobject_class(o->cid)) {
            CkBeObject *be = (CkBeObject *)o;
            ck_3d_free(o);
            if (o->cid == 25) ck_sound_free((CkWaveSound *)o);
            free(be->last_frame);
            free(be->attrs);
            ids_free(&be->scripts);
            for (uint32_t k = 0; k < be->nscene; k++) free(be->scene[k].snap);
            free(be->scene);
            if (o->cid == CKCID_GROUP) ids_free(&((CkGroup *)o)->members);
            if (o->cid == CKCID_DATAARRAY) array_free((CkDataArray *)o);
        }
    }
    free(o->name);
    free(o);
}

void ck_free(CkContext *ctx)
{
    /* the behaviors' delete callbacks while every object still exists */
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkBehavior *b = ctx->objs[i] && ctx->objs[i]->cid == CKCID_BEHAVIOR ? (CkBehavior *)ctx->objs[i] : NULL;
        if (b && b->bb && b->bb->callback) b->bb->callback(ctx, b, CKM_BEHAVIORDELETE);
    }
    for (uint32_t i = 0; i < ctx->nobjs; i++)
        if (ctx->objs[i]) obj_free(ctx->objs[i]);
    free(ctx->objs);
    for (uint32_t i = 0; i < ctx->nfiles; i++) ck_file_free(&ctx->files[i]);
    if (ctx->physics_free) ctx->physics_free(ctx);
    ck_hier_cache_free(ctx);
    ck_fonts_free(ctx);
    ids_free(&ctx->text_draws);
    ids_free(&ctx->particle_systems);
    ids_free(&ctx->planar_filters);
    free(ctx->pre_render);
    free(ctx->files);
    free(ctx->file_base);
    ids_free(&ctx->managed);
    for (uint32_t i = 0; i < ctx->nmessages; i++) free(ctx->messages[i]);
    free(ctx->messages);
    for (uint32_t i = 0; i < ctx->nattr_types; i++)
        free(ctx->attr_types[i].name), free(ctx->attr_types[i].category), free(ctx->attr_types[i].objects.v);
    free(ctx->attr_types);
    for (uint32_t i = 0; i < ctx->nqueue; i++)
        if (ctx->queue[i]) ids_free(&ctx->queue[i]->params), free(ctx->queue[i]);
    free(ctx->queue);
    for (uint32_t i = 0; i < ctx->nwait_types; i++) free(ctx->waits[i]);
    free(ctx->waits);
    free(ctx->nwaits);
    ids_free(&ctx->received);
    free(ctx->minions);
    free(ctx->db_file);
    for (uint32_t i = 0; i < ctx->ndb_arrays; i++) free(ctx->db_arrays[i]);
    free(ctx->db_arrays);
    memset(ctx, 0, sizeof *ctx);
}

CkObj *ck_obj(const CkContext *ctx, CkId id)
{
    return id && id <= ctx->nobjs ? ctx->objs[id - 1] : NULL;
}

#define CK_CAST(fn, type, test)                            \
    type *fn(const CkContext *ctx, CkId id)                \
    {                                                      \
        CkObj *o = ck_obj(ctx, id);                        \
        return o && (test) ? (type *)o : NULL;             \
    }
CK_CAST(ck_behavior, CkBehavior, o->cid == CKCID_BEHAVIOR)
CK_CAST(ck_io, CkBehaviorIO, o->cid == CKCID_BEHAVIORIO)
CK_CAST(ck_link, CkBehaviorLink, o->cid == CKCID_BEHAVIORLINK)
CK_CAST(ck_param, CkParameter,
        o->cid == CKCID_PARAMETERIN || o->cid == CKCID_PARAMETEROUT || o->cid == CKCID_PARAMETERLOCAL || o->cid == CKCID_PARAMETER)
CK_CAST(ck_beobject, CkBeObject, ck_is_beobject_class(o->cid))

CkObj *ck_create(CkContext *ctx, uint32_t cid, const char *name)
{
    if (ctx->nobjs == ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : 1024;
        ctx->objs = realloc(ctx->objs, ctx->cap * sizeof *ctx->objs);
    }
    CkObj *o = calloc(1, obj_size(cid));
    o->id = ctx->nobjs + 1;
    o->cid = cid;
    o->name = strdup(name ? name : "");
    o->flags = CK_OBJECT_VISIBLE;
    ctx->objs[ctx->nobjs++] = o;
    ck_3d_init(o);
    if (cid == CKCID_DATAARRAY) ((CkDataArray *)o)->key_column = -1;
    return o;
}

static void ids_remove(CkIds *l, CkId id)
{
    for (uint32_t i = 0; i < l->n;)
        if (l->v[i] == id) memmove(&l->v[i], &l->v[i + 1], (--l->n - i) * sizeof *l->v);
        else i++;
}

/* CKContext::DestroyObject with dependencies (the classes' PrepareDependencies, CK2.dll 0x2401c680 ...):
   full dependencies also destroy a BeObject's scripts, a group's members, a 3D entity's meshes, a mesh's
   materials, a material's texture and a 2D entity's material, recursively (shared ones too, as in the
   original). */
static void collect_dependencies(CkContext *ctx, CkId id, CkIds *out)
{
    CkObj *o = ck_obj(ctx, id);
    if (!o || ck_ids_has(out, id)) return;
    ck_ids_push(out, id);
    if (ck_is_beobject_class(o->cid))
        for (uint32_t i = 0; i < ((CkBeObject *)o)->scripts.n; i++) collect_dependencies(ctx, ((CkBeObject *)o)->scripts.v[i], out);
    if (o->cid == CKCID_GROUP) {
        CkIds m = {0};
        for (uint32_t i = 0; i < ((CkGroup *)o)->members.n; i++) ck_ids_push(&m, ((CkGroup *)o)->members.v[i]);
        for (uint32_t i = 0; i < m.n; i++) collect_dependencies(ctx, m.v[i], out);
        ids_free(&m);
    }
    if (ck_is_3dentity_class(o->cid)) {
        Ck3dEntity *e = (Ck3dEntity *)o;
        collect_dependencies(ctx, e->mesh, out);
        for (uint32_t i = 0; i < e->meshes.n; i++) collect_dependencies(ctx, e->meshes.v[i], out);
    }
    if (o->cid == CKCID_MESH)
        for (uint32_t i = 0; i < ((CkMesh *)o)->materials.n; i++) collect_dependencies(ctx, ((CkMesh *)o)->materials.v[i], out);
    if (o->cid == CKCID_MATERIAL) collect_dependencies(ctx, ((CkMaterial *)o)->texture, out);
    if (ck_is_2dentity_class(o->cid)) collect_dependencies(ctx, ((Ck2dEntity *)o)->material, out);
    if (o->cid == CKCID_BEHAVIOR) {
        CkBehavior *b = (CkBehavior *)o;
        CkIds *l[] = {&b->sub, &b->links, &b->ops, &b->pin, &b->pout, &b->local, &b->in, &b->out};
        for (size_t k = 0; k < sizeof l / sizeof *l; k++)
            for (uint32_t i = 0; i < l[k]->n; i++) collect_dependencies(ctx, l[k]->v[i], out);
    }
}

void ck_destroy_with_dependencies(CkContext *ctx, CkId id, int32_t mode)
{
    if (mode != CK_DEPENDENCIES_FULL) {
        ck_destroy(ctx, id);
        return;
    }
    CkIds all = {0};
    collect_dependencies(ctx, id, &all);
    for (uint32_t i = 0; i < all.n; i++) ck_destroy(ctx, all.v[i]);
    ids_free(&all);
}

void ck_destroy(CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    if (!o) return;
    ctx->hier_gen++;
    /* CKBehavior's destruction sends its building block CKM_BEHAVIORDELETE first (the BB frees what it
       owns: particle pools, created entities ...) */
    if (o->cid == CKCID_BEHAVIOR) {
        CkBehavior *b = (CkBehavior *)o;
        if (b->bb && b->bb->callback) b->bb->callback(ctx, b, CKM_BEHAVIORDELETE);
        o = ck_obj(ctx, id);
        if (!o) return;
    }
    CkBeObject *level = ck_beobject(ctx, ctx->level);
    for (uint32_t i = 0; level && i < level->nscene; i++)
        if (level->scene[i].obj == id) {
            free(level->scene[i].snap);
            memmove(&level->scene[i], &level->scene[i + 1], (level->nscene - i - 1) * sizeof *level->scene);
            level->nscene--;
            break;
        }
    ids_remove(&ctx->managed, id);
    ids_remove(&ctx->received, id);
    if (ck_is_beobject_class(o->cid))
        for (uint32_t i = 0; i < ((CkBeObject *)o)->nattrs; i++) {
            int32_t t = ((CkBeObject *)o)->attrs[i].type;
            if (t >= 0 && (uint32_t)t < ctx->nattr_types) ids_remove(&ctx->attr_types[t].objects, id);
        }
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkObj *g = ctx->objs[i];
        if (g && g->cid == CKCID_GROUP) ids_remove(&((CkGroup *)g)->members, id);
    }
    if (id == ctx->level) ctx->level = 0;
    if (id == ctx->camera) ctx->camera = 0;
    obj_free(o);
    ctx->objs[id - 1] = NULL;
}

CkId ck_find(const CkContext *ctx, const char *name, uint32_t cid)
{
    for (uint32_t i = 0; i < ctx->nobjs; i++) {
        CkObj *o = ctx->objs[i];
        if (o && (!cid || o->cid == cid) && !strcmp(o->name, name)) return o->id;
    }
    return 0;
}

/* ---- per-class readers ---- */

typedef struct {
    CkContext *ctx;
    const CkFile *f;
    CkId base;                /* runtime id of file object 0 */
    int32_t *msg;             /* file message index -> global message type */
    uint32_t nmsg;
    int32_t *attr;            /* file attribute index -> global attribute type */
    uint32_t nattr;
    const CkId *map_from, *map_to;   /* copying: originals (sorted) -> their copies */
    uint32_t nmap;
} Loader;

int32_t ck_attribute_type(CkContext *ctx, const char *name)
{
    for (uint32_t i = 0; i < ctx->nattr_types; i++)
        if (!strcmp(ctx->attr_types[i].name, name)) return (int32_t)i;
    ctx->attr_types = realloc(ctx->attr_types, (ctx->nattr_types + 1) * sizeof *ctx->attr_types);
    CkAttributeType *t = &ctx->attr_types[ctx->nattr_types];
    memset(t, 0, sizeof *t);
    t->name = strdup(name);
    t->category = strdup("");
    t->compatible_class = CKCID_BEOBJECT;
    return (int32_t)ctx->nattr_types++;
}

const CkAttributeType *ck_attribute_info(const CkContext *ctx, int32_t type)
{
    return type >= 0 && (uint32_t)type < ctx->nattr_types ? &ctx->attr_types[type] : NULL;
}

static struct CkAttribute *find_attribute(const CkContext *ctx, CkId obj, int32_t type)
{
    CkObj *o = ck_obj(ctx, obj);
    if (!o || !ck_is_beobject_class(o->cid)) return NULL;
    CkBeObject *be = (CkBeObject *)o;
    for (uint32_t i = 0; i < be->nattrs; i++)
        if (be->attrs[i].type == type) return &be->attrs[i];
    return NULL;
}

bool ck_has_attribute(const CkContext *ctx, CkId obj, int32_t type) { return find_attribute(ctx, obj, type) != NULL; }

CkId ck_attribute_parameter(const CkContext *ctx, CkId obj, int32_t type)
{
    struct CkAttribute *a = find_attribute(ctx, obj, type);
    return a ? a->param : 0;
}

/* CKBeObject::SetAttribute (0x2401b... ): a present attribute is left alone; the parameter is a new
   ParameterOut of the type's parameter type owned by the object, unless one is given */
bool ck_set_attribute(CkContext *ctx, CkId obj, int32_t type, CkId param)
{
    CkObj *o = ck_obj(ctx, obj);
    const CkAttributeType *t = ck_attribute_info(ctx, type);
    if (!o || !ck_is_beobject_class(o->cid) || !t) return false;
    if (find_attribute(ctx, obj, type)) return true;
    CkBeObject *be = (CkBeObject *)o;
    if (!param && (t->param_type.a || t->param_type.b)) {
        CkParameter *p = (CkParameter *)ck_create(ctx, CKCID_PARAMETEROUT, t->name);
        p->kind = CKP_OUT;
        p->type = t->param_type;
        p->owner = obj;
        const CkParamType *pt = ck_type(p->type);
        if (pt && pt->size) {
            p->value = calloc(1, pt->size);
            p->size = pt->size;
        }
        param = p->h.id;
    }
    be->attrs = realloc(be->attrs, (be->nattrs + 1) * sizeof *be->attrs);
    be->attrs[be->nattrs++] = (struct CkAttribute){type, param};
    ck_ids_push(&ctx->attr_types[type].objects, obj);
    return true;
}

bool ck_remove_attribute(CkContext *ctx, CkId obj, int32_t type)
{
    struct CkAttribute *a = find_attribute(ctx, obj, type);
    if (!a) return false;
    CkBeObject *be = (CkBeObject *)ck_obj(ctx, obj);
    CkParameter *p = ck_param(ctx, a->param);
    if (p && p->owner == obj) ck_destroy(ctx, p->h.id);
    uint32_t i = (uint32_t)(a - be->attrs);
    memmove(&be->attrs[i], &be->attrs[i + 1], (be->nattrs - i - 1) * sizeof *be->attrs);
    be->nattrs--;
    ids_remove(&ctx->attr_types[type].objects, obj);
    return true;
}

/* Attribute manager chunk (0x3d242466), identifier 0x52: category count, attribute count; per category
   slot: exists, then name, flags; per attribute slot (the file's attribute index): exists, then name,
   parameter type GUID, category index, compatible class id, flags. */
static void load_attribute_types(Loader *L)
{
    for (uint32_t i = 0; i < L->f->nmanagers; i++) {
        const CkFileManager *m = &L->f->managers[i];
        if (m->guid.a != CK_ATTRIBUTE_MANAGER || !m->has_chunk) continue;
        CkReader r;
        ck_reader_init(&r, &m->chunk);
        if (!ck_seek(&r, 0x52)) return;
        uint32_t ncat = ck_read_dword(&r), n = ck_read_dword(&r);
        if (ncat > 4096 || n > 4096) return;
        char **cats = calloc(ncat ? ncat : 1, sizeof *cats);
        for (uint32_t k = 0; k < ncat && !r.error; k++)
            if (ck_read_dword(&r)) {
                cats[k] = strdup(ck_read_string(&r));
                ck_read_dword(&r);
            }
        L->attr = malloc((n ? n : 1) * sizeof *L->attr);
        L->nattr = n;
        for (uint32_t k = 0; k < n; k++) {
            L->attr[k] = -1;
            if (r.error || !ck_read_dword(&r)) continue;
            const char *name = ck_read_string(&r);
            CkGuid g = ck_read_guid(&r);
            uint32_t cat = ck_read_dword(&r), cls = ck_read_dword(&r), flags = ck_read_dword(&r);
            int32_t t = ck_attribute_type(L->ctx, name);
            CkAttributeType *at = &L->ctx->attr_types[t];
            at->param_type = g;
            at->compatible_class = cls;
            at->flags = flags;
            if (cat < ncat && cats[cat]) {
                free(at->category);
                at->category = strdup(cats[cat]);
            }
            L->attr[k] = t;
        }
        for (uint32_t k = 0; k < ncat; k++) free(cats[k]);
        free(cats);
    }
}


int32_t ck_message_type(CkContext *ctx, const char *name)
{
    for (uint32_t i = 0; i < ctx->nmessages; i++)
        if (!strcmp(ctx->messages[i], name)) return (int32_t)i;
    ctx->messages = realloc(ctx->messages, (ctx->nmessages + 1) * sizeof *ctx->messages);
    ctx->messages[ctx->nmessages] = strdup(name);
    return (int32_t)ctx->nmessages++;
}

const char *ck_message_name(const CkContext *ctx, int32_t type)
{
    return type >= 0 && (uint32_t)type < ctx->nmessages ? ctx->messages[type] : "";
}

/* Message manager chunk, identifier 0x53: count, then the names (strings) in file index order. */
static void load_messages(Loader *L)
{
    for (uint32_t i = 0; i < L->f->nmanagers; i++) {
        const CkFileManager *m = &L->f->managers[i];
        if (m->guid.a != CK_MESSAGE_MANAGER || !m->has_chunk) continue;
        CkReader r;
        ck_reader_init(&r, &m->chunk);
        if (!ck_seek(&r, 0x53)) return;
        uint32_t n = ck_read_dword(&r);
        L->msg = calloc(n ? n : 1, sizeof *L->msg);
        for (uint32_t k = 0; k < n && !r.error; k++) L->msg[k] = ck_message_type(L->ctx, ck_read_string(&r));
        L->nmsg = n;
    }
}

static CkId remap(const Loader *L, uint32_t file_index)
{
    CkId id = file_index < L->f->nobjects ? L->base + file_index : 0;
    if (L->nmap && id) {
        uint32_t lo = 0, hi = L->nmap;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (L->map_from[mid] < id) lo = mid + 1;
            else hi = mid;
        }
        if (lo < L->nmap && L->map_from[lo] == id) return L->map_to[lo];
    }
    return id;
}

static CkId remap_cb(const void *L, uint32_t file_index) { return remap(L, file_index); }

static void read_array(const Loader *L, CkReader *r, CkIds *out)
{
    uint32_t n = ck_read_dword(r);
    for (uint32_t i = 0; i < n && !r->error; i++) {
        CkId id = remap(L, ck_read_object(r));
        if (id) ck_ids_push(out, id);
    }
}

static void load_object_flags(CkObj *o, const CkChunk *c)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (ck_seek(&r, 0x4)) o->flags &= ~(CK_OBJECT_VISIBLE | CK_OBJECT_HIERARCHICALHIDE);
    else if (ck_seek(&r, 0x18)) o->flags = (o->flags & ~CK_OBJECT_VISIBLE) | CK_OBJECT_HIERARCHICALHIDE;
    else o->flags = (o->flags & ~CK_OBJECT_HIERARCHICALHIDE) | CK_OBJECT_VISIBLE;
}

static void load_behavior(const Loader *L, CkBehavior *b, const CkChunk *c)
{
    /* CKBehavior::Save 0x24004e1e (the load reads the same layout) */
    CkReader r;
    ck_reader_init(&r, c);
    if (!ck_seek(&r, 0x20)) return;
    uint32_t fl = ck_read_dword(&r);
    b->bflags = fl;
    if (fl & 0x8000) {
        b->has_proto = true;
        b->proto = ck_read_guid(&r);
        b->proto_version = ck_read_dword(&r);
    }
    if (fl & 0x4) b->priority = ck_read_int(&r);
    if (fl & 0x10) b->compat_class = ck_read_dword(&r);
    if (fl & 0x40000) b->target = remap(L, ck_read_object(&r));
    uint32_t mask = ck_read_dword(&r);
    static const uint32_t bits[8] = {0x100, 0x80000, 0x4000, 0x200, 0x400, 0x20000, 0x800, 0x1000};
    CkIds *arrays[8] = {&b->sub, &b->links, &b->ops, &b->pin, &b->pout, &b->local, &b->in, &b->out};
    for (int i = 0; i < 8; i++)
        if (mask & bits[i]) read_array(L, &r, arrays[i]);
}

static void load_io(CkBehaviorIO *io, const CkChunk *c)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (ck_seek(&r, 0x8)) io->input = (ck_read_dword(&r) & 1) != 0;
}

static void load_link(const Loader *L, CkBehaviorLink *l, const CkChunk *c)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (!ck_seek(&r, 0x20)) return;
    uint32_t d = ck_read_dword(&r);
    l->cur_delay = (int16_t)(d & 0xffff);
    l->init_delay = (int16_t)(d >> 16);
    l->src = remap(L, ck_read_object(&r));
    l->dst = remap(L, ck_read_object(&r));
}

/* CKParameter value (FUN_2400867c) */
static void load_param_value(const Loader *L, CkParameter *p, const CkChunk *c)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (!ck_seek(&r, 0x40)) return;
    p->type = ck_read_guid(&r);
    p->value_mode = ck_read_dword(&r);
    uint32_t v;
    switch (p->value_mode) {
    case 0: { /* type-specific sub-chunk, read by the type's own reader */
        CkChunk sub, curve;
        CkReader s;
        /* 2DCurve: identifier 0xe holding the CK2dCurve chunk */
        if (ck_guid_eq(p->type, CK_GUID_2DCURVE) && ck_read_subchunk(&r, &sub) && (ck_reader_init(&s, &sub), ck_seek(&s, 0xe)) &&
            ck_read_subchunk(&s, &curve)) {
            free(p->value);
            p->value = (uint8_t *)ck_curve_read(&curve, &p->size);
            if (!p->value) p->size = 0;
        }
        /* Dependencies (delete, copy, ...; CK2.dll FUN_240325c5): identifier 0x12248766 none, 0x12248767
           full, 0x12248768 custom: (class id, flags) pairs -> a CkDependencies */
        else if ((ck_guid_eq(p->type, CK_GUID_DEPENDENCIES) || ck_guid_eq(p->type, CK_GUID_COPY_DEPENDENCIES)) &&
                 ck_read_subchunk(&r, &sub)) {
            CkDependencies d;
            memset(&d, 0, sizeof d);
            ck_reader_init(&s, &sub);
            if (ck_seek(&s, 0x12248766)) d.mode = CK_DEPENDENCIES_NONE;
            else if (ck_seek(&s, 0x12248767)) d.mode = CK_DEPENDENCIES_FULL;
            else if (ck_seek(&s, 0x12248768))
                for (uint32_t n = ck_remaining(&s) / 2; n; n--) {
                    uint32_t cls = ck_read_dword(&s), fl = ck_read_dword(&s);
                    if (cls < CK_MAX_CLASSES) d.flags[cls] = fl;
                }
            ck_param_set(p, &d, sizeof d);
        }
        return;
    }
    case 1: {
        uint32_t n;
        const void *src = ck_read_buffer(&r, &n);
        if (src && n) {
            p->value = malloc(n);
            memcpy(p->value, src, n);
            p->size = n;
        }
        return;
    }
    case 2:
        v = remap(L, ck_read_object(&r));
        ck_param_set(p, &v, 4);
        return;
    case 3:
        return;
    default: /* the "mode" is a manager GUID: the value is that manager's index (remapped) */
        ck_read_dword(&r);
        v = ck_read_dword(&r);
        if (p->value_mode == CK_MESSAGE_MANAGER) v = v < L->nmsg ? (uint32_t)L->msg[v] : 0xffffffff;
        if (p->value_mode == CK_ATTRIBUTE_MANAGER) v = v < L->nattr ? (uint32_t)L->attr[v] : 0xffffffff;
        ck_param_set(p, &v, 4);
        return;
    }
}

/* CKParameterLocal load (FUN_240096e6): identifier 0x200 makes it a "myself" parameter
   (CKParameterLocal::SetAsMyselfParameter 0x240095a6): its value is the owner behavior's owner object. */
static void load_param_local(const Loader *L, CkParameter *p, const CkChunk *c)
{
    load_param_value(L, p, c);
    CkReader r;
    ck_reader_init(&r, c);
    if (p->h.cid == CKCID_PARAMETERLOCAL && ck_seek(&r, 0x200)) p->myself = true;
}

static void set_myself(CkContext *ctx, CkParameter *p)
{
    CkBehavior *b = ck_behavior(ctx, p->owner);
    CkId v = b ? b->owner : 0;
    ck_param_set(p, &v, 4);
}

static void load_param_in(const Loader *L, CkParameter *p, const CkChunk *c)
{
    /* CKParameterIn load (FUN_24009005): 0x800 shared source, 0x1000 direct source, 0x2000 disabled */
    CkReader r;
    ck_reader_init(&r, c);
    p->kind = CKP_IN;
    if (ck_seek(&r, 0x800)) {
        p->type = ck_read_guid(&r);
        p->source = remap(L, ck_read_object(&r));
        p->shared = true;
    } else if (ck_seek(&r, 0x1000)) {
        p->type = ck_read_guid(&r);
        p->source = remap(L, ck_read_object(&r));
    }
    if (ck_seek(&r, 0x2000)) p->h.flags |= 0x1000000;   /* disabled */
}

static void load_param_out(const Loader *L, CkParameter *p, const CkChunk *c)
{
    load_param_value(L, p, c);
    CkReader r;
    ck_reader_init(&r, c);
    if (ck_seek(&r, 0x20)) read_array(L, &r, &p->dests);
}

static void load_operation(const Loader *L, CkParameterOperation *op, const CkChunk *c)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (!ck_seek(&r, 0x400)) return;
    op->op = ck_read_guid(&r);
    uint32_t n = ck_read_dword(&r);
    CkId args[3] = {0};
    for (uint32_t i = 0; i < n; i++) {
        CkId id = remap(L, ck_read_object(&r));
        if (i < 3) args[i] = id;
    }
    op->in1 = args[0];
    op->in2 = args[1];
    op->out = args[2];
}

static void load_scene(const Loader *L, CkBeObject *level, const CkChunk *scene)
{
    /* CKScene load (FUN_2402e6b4), identifier 0x20000: level, count, sequence, object ids, sequence,
       per object initial-value sub-chunk + a discarded sub-chunk, then (data version >= 8) flags;
       0x40000 scene flags; 0x80000 environment: background, ambient, fog mode, fog colour, fog start,
       end, density, background texture, starting camera. */
    CkReader r;
    ck_reader_init(&r, scene);
    if (!ck_seek(&r, 0x20000)) return;
    ck_read_object(&r);
    uint32_t n = ck_read_dword(&r);
    ck_read_dword(&r);
    CkSceneEntry *e = calloc(n ? n : 1, sizeof *e);
    for (uint32_t i = 0; i < n; i++) e[i].obj = remap(L, ck_read_object(&r));
    ck_read_dword(&r);
    for (uint32_t i = 0; i < n; i++) {
        e[i].has_initial = ck_read_subchunk(&r, &e[i].initial);
        CkChunk skip;
        ck_read_subchunk(&r, &skip);
    }
    for (uint32_t i = 0; i < n; i++) e[i].flags = ck_read_dword(&r), e[i].file = L->ctx->nfiles;
    if (r.error) {
        free(e);
        return;
    }
    level->scene = e;
    level->nscene = n;
    CkContext *ctx = L->ctx;
    if (ck_seek(&r, 0x40000)) ctx->scene_flags = ck_read_dword(&r);
    if (ck_seek(&r, 0x80000)) {
        ctx->scene_env.background = ck_read_dword(&r);
        ctx->scene_env.ambient = ck_read_dword(&r);
        ctx->scene_env.fog_mode = ck_read_dword(&r);
        ctx->scene_env.fog_color = ck_read_dword(&r);
        ctx->scene_env.fog_start = ck_read_float(&r);
        ctx->scene_env.fog_end = ck_read_float(&r);
        ctx->scene_env.fog_density = ck_read_float(&r);
        ctx->scene_env.background_texture = remap(L, ck_read_object(&r));
        ctx->scene_env.camera = remap(L, ck_read_object(&r));
        /* files older than CK 2.1 had no exp fog: 1 and 2 meant linear */
        if (L->f->ck_version < 0x2010000 && (ctx->scene_env.fog_mode == 1 || ctx->scene_env.fog_mode == 2))
            ctx->scene_env.fog_mode = 3;
    }
}

/* CKDataArray load (0x24029..., the CKDataArray vtable Load; 0x4000 at 0x2402a03c): 0x1000 columns (count; name, type[,
   parameter GUID, 4a4d4867:3c28773f read as Time]), 0x2000 rows (count; cells by column type: int/float
   dword, string, object, parameter: a reference when loading a file, else (initial values) a sub-chunk
   with the value for a new ParameterOut owned by the array), 0x4000 sort state and key column. */
static void load_array(const Loader *L, CkDataArray *a, const CkChunk *c, bool from_file)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (!ck_seek(&r, 0x1000)) return;
    uint32_t nc = ck_read_dword(&r);
    if (r.error || nc > 4096) return;
    a->cols = calloc(nc ? nc : 1, sizeof *a->cols);
    a->ncols = nc;
    for (uint32_t i = 0; i < nc; i++) {
        a->cols[i].name = strdup(ck_read_string(&r));
        a->cols[i].type = ck_read_dword(&r);
        if (a->cols[i].type == CKARRAYTYPE_PARAMETER) {
            CkGuid g = ck_read_guid(&r);
            if (g.a == 0x4a4d4867 && g.b == 0x3c28773f) g = (CkGuid){0x54b4422b, 0x730f0f4f};
            a->cols[i].param_type = g;
        }
    }
    if (ck_seek(&r, 0x2000)) {
        uint32_t nr = ck_read_dword(&r);
        if (nr > 1 << 20) nr = 0;
        a->cells = calloc((size_t)(nr ? nr : 1) * (nc ? nc : 1), sizeof *a->cells);
        a->nrows = a->cap_rows = nr;
        for (uint32_t row = 0; row < nr && !r.error; row++)
            for (uint32_t col = 0; col < nc; col++) {
                CkCell *cell = &a->cells[row * nc + col];
                switch (a->cols[col].type) {
                case CKARRAYTYPE_STRING: cell->s = strdup(ck_read_string(&r)); break;
                case CKARRAYTYPE_PARAMETER:
                    if (!from_file) {
                        CkChunk sub;
                        CkParameter *p = (CkParameter *)ck_create(L->ctx, CKCID_PARAMETEROUT, a->cols[col].name);
                        p->kind = CKP_OUT;
                        p->type = a->cols[col].param_type;
                        p->owner = a->be.h.id;
                        if (ck_read_subchunk(&r, &sub)) load_param_value(L, p, &sub);
                        p->type = a->cols[col].param_type;
                        cell->obj = p->h.id;
                        break;
                    }
                    /* fall through */
                case CKARRAYTYPE_OBJECT: cell->obj = remap(L, ck_read_object(&r)); break;
                default: cell->i = ck_read_int(&r);
                }
            }
    }
    /* 0x4000: sort order (+0x68), sort column (+0x6c), key column (+0x64; from a file, or data version >= 5) */
    a->key_column = -1;
    if (ck_seek(&r, 0x4000)) {
        a->sort_order = ck_read_int(&r);
        a->sort_column = ck_read_int(&r);
        if (from_file || c->data_version >= 5) a->key_column = ck_read_int(&r);
    }
}

static void load_beobject(const Loader *L, CkBeObject *be, const CkChunk *c)
{
    CkReader r;
    ck_reader_init(&r, c);
    if (ck_seek(&r, 0x800)) read_array(L, &r, &be->scripts);
    /* attributes (CKBeObject::Load 0x2401bf56), identifier 0x11: count, their parameters, then the attribute
       manager's sequence (count, GUID) of the file attribute indices */
    if (ck_seek(&r, 0x11)) {
        uint32_t n = ck_read_dword(&r);
        if (n < 1024) {
            CkId *params = calloc(n ? n : 1, sizeof *params);
            for (uint32_t i = 0; i < n; i++) params[i] = remap(L, ck_read_object(&r));
            uint32_t m = ck_read_dword(&r);
            CkGuid g = ck_read_guid(&r);
            if (m == n && g.a == CK_ATTRIBUTE_MANAGER && !r.error)
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t k = ck_read_dword(&r);
                    if (k < L->nattr && L->attr[k] >= 0) ck_set_attribute(L->ctx, be->h.id, L->attr[k], params[i]);
                }
            free(params);
        }
    }
    if (be->h.cid == CKCID_LEVEL && ck_seek(&r, 0x80000000)) {
        ck_read_object(&r);
        ck_read_object(&r);
        CkChunk scene;
        if (ck_read_subchunk(&r, &scene)) load_scene(L, be, &scene);
    }
    if (be->h.cid == CKCID_DATAARRAY) load_array(L, (CkDataArray *)be, c, true);
    if (be->h.cid == CKCID_GROUP) {
        ck_reader_init(&r, c);
        if (ck_seek(&r, 0xfffff)) read_array(L, &r, &((CkGroup *)be)->members);
    }
}

void ck_read_object_state(CkContext *ctx, CkId id, const CkChunk *c, uint32_t file)
{
    ctx->hier_gen++;
    CkObj *o = ck_obj(ctx, id);
    if (!o || !c || !file || file > ctx->nfiles) return;
    Loader L = {ctx, &ctx->files[file - 1], ctx->file_base[file - 1], NULL, 0, NULL, 0, NULL, NULL, 0};
    load_messages(&L);
    load_attribute_types(&L);
    load_object_flags(o, c);
    switch (o->cid) {
    case CKCID_PARAMETERLOCAL: case CKCID_PARAMETER:
        load_param_local(&L, (CkParameter *)o, c);
        if (((CkParameter *)o)->myself) set_myself(ctx, (CkParameter *)o);
        break;
    case CKCID_PARAMETEROUT: load_param_out(&L, (CkParameter *)o, c); break;
    case CKCID_DATAARRAY: {
        CkDataArray *a = (CkDataArray *)o;
        CkIds owned = {0};             /* the parameter cells the array created */
        for (uint32_t col = 0; col < a->ncols; col++)
            if (a->cols[col].type == CKARRAYTYPE_PARAMETER)
                for (uint32_t row = 0; row < a->nrows; row++) {
                    CkParameter *p = ck_param(ctx, a->cells[row * a->ncols + col].obj);
                    if (p && p->owner == a->be.h.id) ck_ids_push(&owned, p->h.id);
                }
        array_free(a);
        for (uint32_t k = 0; k < owned.n; k++) ck_destroy(ctx, owned.v[k]);
        ids_free(&owned);
        load_array(&L, a, c, false);
        break;
    }
    case CKCID_GROUP: {
        CkReader r;
        ck_reader_init(&r, c);
        ((CkGroup *)o)->members.n = 0;
        if (ck_seek(&r, 0xfffff)) read_array(&L, &r, &((CkGroup *)o)->members);
        break;
    }
    case CKCID_MESH: case CKCID_TEXTURE: break;   /* geometry and images are not restored (not needed) */
    default:
        if (ck_is_3dentity_class(o->cid)) ((Ck3dEntity *)o)->meshes.n = 0;
        if (ck_is_beobject_class(o->cid)) ck_3d_load(o, c, remap_cb, &L);
    }
    free(L.msg);
    free(L.attr);
}

static void load_one(Loader *L, CkObj *o, const CkChunk *c)
{
    o->chunk = c;
    load_object_flags(o, c);
    {
        CkReader ar;
        ck_reader_init(&ar, c);
        uint32_t ident = o->cid == CKCID_BEHAVIOR ? 0x200000 : ck_is_beobject_class(o->cid) ? 0x400 : 0;
        if (ident && ck_seek(&ar, ident)) {
            o->activity = ck_read_dword(&ar);
            o->has_activity = o->activity != 0 || o->cid == CKCID_BEHAVIOR;
        }
    }
    switch (o->cid) {
    case CKCID_BEHAVIOR: load_behavior(L, (CkBehavior *)o, c); break;
    case CKCID_BEHAVIORIO: load_io((CkBehaviorIO *)o, c); break;
    case CKCID_BEHAVIORLINK: load_link(L, (CkBehaviorLink *)o, c); break;
    case CKCID_PARAMETERIN: load_param_in(L, (CkParameter *)o, c); break;
    case CKCID_PARAMETEROUT: ((CkParameter *)o)->kind = CKP_OUT; load_param_out(L, (CkParameter *)o, c); break;
    case CKCID_PARAMETERLOCAL: case CKCID_PARAMETER:
        ((CkParameter *)o)->kind = CKP_LOCAL;
        load_param_local(L, (CkParameter *)o, c);
        break;
    case CKCID_PARAMETEROPERATION: load_operation(L, (CkParameterOperation *)o, c); break;
    default:
        if (ck_is_beobject_class(o->cid)) {
            load_beobject(L, (CkBeObject *)o, c);
            ck_3d_load(o, c, remap_cb, L);
            if (o->cid == 25) ck_sound_load_chunk((CkWaveSound *)o, c);
        }
        ck_anim_load(o, c, remap_cb, L);
    }
}

/* after the chunks of a set of new objects are read: back-references, script owners, Building Blocks and
   their CKM_BEHAVIORLOAD callbacks */
static void link_objects(CkContext *ctx, const CkIds *ids)
{
    /* pass 3: back-references: IO owners, link lists, parameter/operation owners, behavior parents */
    for (uint32_t i = 0; i < ids->n; i++) {
        CkObj *o = ck_obj(ctx, ids->v[i]);
        if (!o) continue;
        if (o->cid == CKCID_BEHAVIOR) {
            CkBehavior *b = (CkBehavior *)o;
            for (uint32_t k = 0; k < b->in.n; k++) {
                CkBehaviorIO *io = ck_io(ctx, b->in.v[k]);
                if (io) io->owner = b->h.id, io->input = true;
            }
            for (uint32_t k = 0; k < b->out.n; k++) {
                CkBehaviorIO *io = ck_io(ctx, b->out.v[k]);
                if (io) io->owner = b->h.id, io->input = false;
            }
            for (uint32_t k = 0; k < b->sub.n; k++) {
                CkBehavior *s = ck_behavior(ctx, b->sub.v[k]);
                if (s) s->parent = b->h.id;
            }
            for (uint32_t k = 0; k < b->links.n; k++) {
                CkBehaviorLink *l = ck_link(ctx, b->links.v[k]);
                CkBehaviorIO *src = l ? ck_io(ctx, l->src) : NULL;
                if (src) ck_ids_push(&src->links, l->h.id);
            }
            CkIds *pl[3] = {&b->pin, &b->pout, &b->local};
            for (int a = 0; a < 3; a++)
                for (uint32_t k = 0; k < pl[a]->n; k++) {
                    CkParameter *p = ck_param(ctx, pl[a]->v[k]);
                    if (p) p->owner = b->h.id;
                }
            for (uint32_t k = 0; k < b->ops.n; k++) {
                CkParameterOperation *op = (CkParameterOperation *)ck_obj(ctx, b->ops.v[k]);
                if (!op || op->h.cid != CKCID_PARAMETEROPERATION) continue;
                op->owner = b->h.id;
                CkParameter *out = ck_param(ctx, op->out);
                if (out) out->op = op->h.id;
                CkParameter *a1 = ck_param(ctx, op->in1), *a2 = ck_param(ctx, op->in2);
                if (a1) a1->owner = op->h.id;
                if (a2) a2->owner = op->h.id;
                if (out) out->owner = op->h.id;
            }
        }
    }
    /* scripts: owner BeObject for the whole tree */
    for (uint32_t i = 0; i < ids->n; i++) {
        CkObj *o = ck_obj(ctx, ids->v[i]);
        if (!o) continue;
        if (!ck_is_beobject_class(o->cid)) continue;
        CkBeObject *be = (CkBeObject *)o;
        for (uint32_t k = 0; k < be->scripts.n; k++) {
            CkBehavior *s = ck_behavior(ctx, be->scripts.v[k]);
            if (!s) continue;
            s->bflags |= CKBF_SCRIPT;
            /* depth-first over the tree, setting the owner */
            CkIds todo = {0};
            ck_ids_push(&todo, s->h.id);
            while (todo.n) {
                CkBehavior *t = ck_behavior(ctx, todo.v[--todo.n]);
                if (!t) continue;
                t->owner = be->h.id;
                for (uint32_t j = 0; j < t->sub.n; j++) ck_ids_push(&todo, t->sub.v[j]);
            }
            ids_free(&todo);
        }
        if (o->cid == CKCID_LEVEL && !ctx->level) ctx->level = o->id;
    }
    for (uint32_t i = 0; i < ids->n; i++) {
        CkParameter *p = ck_param(ctx, ids->v[i]);
        if (p && p->myself) set_myself(ctx, p);
    }
    /* resolve Building Blocks */
    for (uint32_t i = 0; i < ids->n; i++) {
        CkBehavior *b = ck_behavior(ctx, ids->v[i]);
        if (!b || !b->has_proto) continue;
        for (uint32_t k = 0; k < ctx->nbbs; k++)
            if (ctx->bbs[k]->guid.a == b->proto.a && ctx->bbs[k]->guid.b == b->proto.b) {
                b->bb = ctx->bbs[k];
                break;
            }
    }
    /* BB load callbacks (CKM_BEHAVIORLOAD) */
    for (uint32_t i = 0; i < ids->n; i++) {
        CkBehavior *b = ck_behavior(ctx, ids->v[i]);
        if (b && b->bb && b->bb->callback) b->bb->callback(ctx, b, CKM_BEHAVIORLOAD);
    }
}


/* ---- copying (CKContext::CopyObjects) ---- */

static uint32_t dep_flags(const CkDependencies *d, uint32_t cls)
{
    return d->mode == CK_DEPENDENCIES_FULL ? 0xffffffffu : d->mode == CK_DEPENDENCIES_NONE ? 0 : cls < CK_MAX_CLASSES ? d->flags[cls] : 0;
}

/* The classes' PrepareDependencies in copy mode: a behavior brings its whole graph; a BeObject its scripts
   (1) and attribute parameters (2); a 3D entity its meshes (1) and children (2); a mesh its materials (1);
   a material its texture (1); a 2D entity its material (1). */
static void collect_copy(CkContext *ctx, CkId id, const CkDependencies *d, CkIds *out)
{
    CkObj *o = ck_obj(ctx, id);
    if (!o || ck_ids_has(out, id) || o->cid == CKCID_LEVEL) return;
    ck_ids_push(out, id);
    if (o->cid == CKCID_BEHAVIOR) {
        CkBehavior *b = (CkBehavior *)o;
        CkIds *l[] = {&b->sub, &b->links, &b->ops, &b->pin, &b->pout, &b->local, &b->in, &b->out};
        for (size_t k = 0; k < sizeof l / sizeof *l; k++)
            for (uint32_t i = 0; i < l[k]->n; i++) collect_copy(ctx, l[k]->v[i], d, out);
        if (b->target) collect_copy(ctx, b->target, d, out);
        return;
    }
    if (o->cid == CKCID_PARAMETEROPERATION) {
        CkParameterOperation *op = (CkParameterOperation *)o;
        collect_copy(ctx, op->in1, d, out), collect_copy(ctx, op->in2, d, out), collect_copy(ctx, op->out, d, out);
        return;
    }
    if (ck_is_beobject_class(o->cid)) {
        CkBeObject *be = (CkBeObject *)o;
        uint32_t f = dep_flags(d, CKCID_BEOBJECT);
        if (f & 1)
            for (uint32_t i = 0; i < be->scripts.n; i++) collect_copy(ctx, be->scripts.v[i], d, out);
        if (f & 2)
            for (uint32_t i = 0; i < be->nattrs; i++) collect_copy(ctx, be->attrs[i].param, d, out);
    }
    if (ck_is_3dentity_class(o->cid)) {
        Ck3dEntity *e = (Ck3dEntity *)o;
        uint32_t f = dep_flags(d, CKCID_3DENTITY);
        if (f & 1) {
            collect_copy(ctx, e->mesh, d, out);
            for (uint32_t i = 0; i < e->meshes.n; i++) collect_copy(ctx, e->meshes.v[i], d, out);
        }
        if (f & 2)
            for (uint32_t i = 0; i < ctx->nobjs; i++) {
                CkObj *c = ctx->objs[i];
                if (c && ck_is_3dentity_class(c->cid) && ((Ck3dEntity *)c)->parent == id) collect_copy(ctx, c->id, d, out);
            }
    }
    if (o->cid == CKCID_MESH && (dep_flags(d, CKCID_MESH) & 1))
        for (uint32_t i = 0; i < ((CkMesh *)o)->materials.n; i++) collect_copy(ctx, ((CkMesh *)o)->materials.v[i], d, out);
    if (o->cid == CKCID_MATERIAL && (dep_flags(d, CKCID_MATERIAL) & 1)) collect_copy(ctx, ((CkMaterial *)o)->texture, d, out);
    if (ck_is_2dentity_class(o->cid) && (dep_flags(d, 27) & 1)) collect_copy(ctx, ((Ck2dEntity *)o)->material, d, out);
}

typedef struct {
    CkId from, to;
} CopyPair;

static int by_from(const void *a, const void *b)
{
    CkId x = ((const CopyPair *)a)->from, y = ((const CopyPair *)b)->from;
    return x < y ? -1 : x > y;
}

static CkId mapped(const CopyPair *m, uint32_t n, CkId id)
{
    for (uint32_t lo = 0, hi = n; lo < hi;) {
        uint32_t mid = (lo + hi) / 2;
        if (m[mid].from < id) lo = mid + 1;
        else if (m[mid].from > id) hi = mid;
        else return m[mid].to;
    }
    return id;
}

void ck_copy_objects(CkContext *ctx, const CkIds *objects, const CkDependencies *deps, bool dynamic, bool activate, CkIds *out)
{
    (void)dynamic;
    CkIds set = {0};
    for (uint32_t i = 0; i < objects->n; i++) collect_copy(ctx, objects->v[i], deps, &set);
    CopyPair *pairs = malloc((set.n ? set.n : 1) * sizeof *pairs);
    for (uint32_t i = 0; i < set.n; i++) {
        CkObj *o = ck_obj(ctx, set.v[i]);
        CkObj *c = ck_create(ctx, o->cid, o->name);
        c->file = o->file;
        o = ck_obj(ctx, set.v[i]);          /* ck_create may have moved the table */
        pairs[i] = (CopyPair){o->id, c->id};
    }
    qsort(pairs, set.n, sizeof *pairs, by_from);
    CkId *from = malloc((set.n ? set.n : 1) * sizeof *from), *to = malloc((set.n ? set.n : 1) * sizeof *to);
    for (uint32_t i = 0; i < set.n; i++) from[i] = pairs[i].from, to[i] = pairs[i].to;
    CkIds copies = {0};
    for (uint32_t i = 0; i < set.n; i++) {
        CkObj *o = ck_obj(ctx, set.v[i]), *c = ck_obj(ctx, mapped(pairs, set.n, set.v[i]));
        ck_ids_push(&copies, c->id);
        if (o->chunk && o->file) {
            Loader L = {ctx, &ctx->files[o->file - 1], ctx->file_base[o->file - 1], NULL, 0, NULL, 0, from, to, set.n};
            load_messages(&L);
            load_attribute_types(&L);
            load_one(&L, c, o->chunk);
            free(L.msg);
            free(L.attr);
        }
        /* CKObject::Copy: the current state */
        c->flags = o->flags;
        c->has_activity = false;
        if (ck_is_3dentity_class(o->cid)) {
            Ck3dEntity *e = (Ck3dEntity *)o, *ce = (Ck3dEntity *)c;
            memcpy(ce->world, e->world, sizeof e->world);
            ce->flags = e->flags, ce->moveable = e->moveable;
            ce->parent = mapped(pairs, set.n, e->parent);
            ce->mesh = mapped(pairs, set.n, e->mesh);
        }
        if (ck_is_2dentity_class(o->cid)) {
            Ck2dEntity *e = (Ck2dEntity *)o, *ce = (Ck2dEntity *)c;
            ce->flags = e->flags, ce->zorder = e->zorder;
            memcpy(ce->rect, e->rect, sizeof e->rect);
            memcpy(ce->src, e->src, sizeof e->src);
            ce->material = mapped(pairs, set.n, e->material);
            ce->parent = mapped(pairs, set.n, e->parent);
        }
        if (o->cid == CKCID_MATERIAL) {
            CkMaterial *m = (CkMaterial *)o, *cm = (CkMaterial *)c;
            CkBeObject keep = cm->be;
            *cm = *m;
            cm->be = keep;
            cm->texture = mapped(pairs, set.n, m->texture);
        }
        if (o->cid == CKCID_PARAMETERIN || o->cid == CKCID_PARAMETEROUT || o->cid == CKCID_PARAMETERLOCAL || o->cid == CKCID_PARAMETER) {
            CkParameter *p = (CkParameter *)o, *cp = (CkParameter *)c;
            cp->kind = p->kind;
            cp->type = p->type;
            cp->myself = p->myself;
            if (p->kind != CKP_IN && p->value) {
                ck_param_set(cp, p->value, p->size);
                uint32_t cls = ck_type_class(p->type);
                if (cls && cp->size >= 4) {           /* object references among the copies */
                    CkId v;
                    memcpy(&v, cp->value, 4);
                    v = mapped(pairs, set.n, v);
                    memcpy(cp->value, &v, 4);
                }
            }
        }
    }
    link_objects(ctx, &copies);
    ctx->hier_gen++;
    /* CKBeObject::Copy (0x2401c801) with the groups dependency (BeObject flag 4): the copy joins every group
       (in the class list's order) that the original is in. Groups being copied themselves are left to
       their own copies. */
    if (dep_flags(deps, CKCID_BEOBJECT) & 4) {
        uint32_t nobjs = ctx->nobjs;
        for (uint32_t i = 0; i < set.n; i++) {
            CkObj *o = ck_obj(ctx, set.v[i]);
            CkId cid = mapped(pairs, set.n, set.v[i]);
            if (!o || !ck_is_beobject_class(o->cid) || o->cid == CKCID_GROUP || cid == o->id) continue;
            for (uint32_t k = 0; k < nobjs; k++) {
                CkObj *g = ctx->objs[k];
                if (!g || g->cid != CKCID_GROUP || mapped(pairs, set.n, g->id) != g->id) continue;
                CkGroup *grp = (CkGroup *)g;
                if (ck_ids_has(&grp->members, o->id) && !ck_ids_has(&grp->members, cid)) ck_ids_push(&grp->members, cid);
            }
        }
    }
    /* the level scene (CKDependenciesContext::Copy, end) */
    for (uint32_t i = 0; i < set.n; i++) {
        CkObj *o = ck_obj(ctx, set.v[i]);
        CkId cid = mapped(pairs, set.n, set.v[i]);
        CkObj *c = ck_obj(ctx, cid);
        const CkSceneEntry *e = ck_scene_entry(ctx, o->id);
        if (!c || !e || !ck_class_derives(c->cid, CKCID_SCENEOBJECT)) continue;
        uint32_t flags = e->flags;
        bool on = (activate && ck_is_beobject_class(c->cid)) || (flags & 8);
        ck_scene_add_copy(ctx, cid, flags & ~8u);
        if (on) ck_scene_activate(ctx, cid, false);
    }
    if (out)
        for (uint32_t i = 0; i < objects->n; i++) ck_ids_push(out, ck_obj(ctx, objects->v[i]) ? mapped(pairs, set.n, objects->v[i]) : 0);
    ids_free(&set);
    ids_free(&copies);
    free(pairs);
    free(from);
    free(to);
}

bool ck_load(CkContext *ctx, const char *path, CkIds *loaded, char *err, size_t errlen)
{
    CkFile f;
    if (!ck_file_load(&f, path, err, errlen)) return false;
    ctx->files = realloc(ctx->files, (ctx->nfiles + 1) * sizeof *ctx->files);
    ctx->files[ctx->nfiles++] = f;
    ctx->file_base = realloc(ctx->file_base, ctx->nfiles * sizeof *ctx->file_base);
    ctx->file_base[ctx->nfiles - 1] = ctx->nobjs + 1;
    const CkFile *pf = &ctx->files[ctx->nfiles - 1];

    Loader L = {ctx, pf, ctx->nobjs + 1, NULL, 0, NULL, 0, NULL, NULL, 0};
    load_messages(&L);
    load_attribute_types(&L);
    uint32_t need = ctx->nobjs + pf->nobjects;
    if (need > ctx->cap) {
        ctx->cap = need + need / 2;
        ctx->objs = realloc(ctx->objs, ctx->cap * sizeof *ctx->objs);
    }
    /* pass 1: create every object (references may point forward) */
    for (uint32_t i = 0; i < pf->nobjects; i++) {
        const CkFileObject *fo = &pf->objects[i];
        CkObj *o = calloc(1, obj_size(fo->class_id));
        o->id = ctx->nobjs + 1;
        o->cid = fo->class_id;
        o->name = strdup(fo->name);
        o->file_id = fo->id;
        o->file = ctx->nfiles;
        o->flags = CK_OBJECT_VISIBLE;
        ctx->objs[ctx->nobjs++] = o;
        if (loaded) ck_ids_push(loaded, o->id);
    }
    /* pass 2: read the chunks */
    CkIds ids = {0};
    for (uint32_t i = 0; i < pf->nobjects; i++) {
        const CkFileObject *fo = &pf->objects[i];
        CkObj *o = ctx->objs[L.base - 1 + i];
        ck_ids_push(&ids, o->id);
        if (fo->has_chunk) load_one(&L, o, &fo->chunk);
    }
    link_objects(ctx, &ids);
    ids_free(&ids);
    free(L.msg);
    free(L.attr);
    return true;
}
