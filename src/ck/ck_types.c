#include "ck_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define GUID(a, b) {0x##a##u, 0x##b##u}

const CkGuid CKPGUID_NONE = GUID(1cb10760, 419f50c5), CKPGUID_INT = GUID(5a5716fd, 44e276d7),
             CKPGUID_FLOAT = GUID(47884c3f, 432c2c20), CKPGUID_BOOL = GUID(1ad52a8e, 5e741920),
             CKPGUID_STRING = GUID(6bd010e2, 115617ea);

#define OBJ(a, b, n, pa, pb, cid) {GUID(a, b), n, GUID(pa, pb), CKT_OBJECT, 4, cid}
static const CkParamType TYPES[] = {
    {GUID(1cb10760, 419f50c5), "None", {0, 0}, CKT_RAW, 0, 0},
    {GUID(47884c3f, 432c2c20), "Float", {0, 0}, CKT_FLOAT, 4, 0},
    {GUID(f3c84b4e, 0ffacc34), "Percentage", GUID(47884c3f, 432c2c20), CKT_FLOAT, 4, 0},
    {GUID(11262cf5, 30b0233a), "Angle", GUID(47884c3f, 432c2c20), CKT_FLOAT, 4, 0},
    {GUID(54b4422b, 730f0f4f), "Time", GUID(47884c3f, 432c2c20), CKT_FLOAT, 4, 0},
    {GUID(5a5716fd, 44e276d7), "Integer", {0, 0}, CKT_INT, 4, 0},
    {GUID(1ad52a8e, 5e741920), "Boolean", GUID(5a5716fd, 44e276d7), CKT_BOOL, 4, 0},
    {GUID(6bd010e2, 115617ea), "String", {0, 0}, CKT_STRING, 64, 0},
    {GUID(48824eae, 2fe47960), "Vector", {0, 0}, CKT_VECTOR, 12, 0},
    {GUID(13b01b3c, 1942583e), "Euler Angles", GUID(48824eae, 2fe47960), CKT_VECTOR, 12, 0},
    {GUID(06c439ee, 45b50fc2), "Quaternion", {0, 0}, CKT_RAW, 16, 0},
    {GUID(7ab20d20, 693044a9), "Rectangle", {0, 0}, CKT_RECT, 16, 0},
    {GUID(4efcb34a, 6079e42f), "2D Vector", {0, 0}, CKT_VECTOR2D, 8, 0},
    {GUID(643f046e, 65211b71), "Matrix", {0, 0}, CKT_RAW, 64, 0},
    {GUID(57d42fee, 7cbb3b91), "Color", {0, 0}, CKT_COLOR, 16, 0},
    {GUID(668649c8, 283e2ee1), "Box", {0, 0}, CKT_RAW, 24, 0},
    {GUID(03881e12, 5ba34e2b), "Message", {0, 0}, CKT_MESSAGE, 4, 0},
    {GUID(3ea34ee9, 09fa5366), "Attribute", {0, 0}, CKT_INT, 4, 0},
    OBJ(30ec20ab, 6df6517d, "Object", 0, 0, CKCID_OBJECT),
    OBJ(71d80779, 402f42f3, "Behavioral Object", 30ec20ab, 6df6517d, CKCID_BEOBJECT),
    OBJ(7ea4176d, 1b405d30, "Script", 30ec20ab, 6df6517d, CKCID_BEHAVIOR),
    OBJ(5b8a05d5, 31ea28d4, "3D Entity", 71d80779, 402f42f3, CKCID_3DENTITY),
    OBJ(362e4df8, 17443539, "3D Object", 5b8a05d5, 31ea28d4, CKCID_3DOBJECT),
    OBJ(3cf24d6f, 216204f9, "Camera", 5b8a05d5, 31ea28d4, CKCID_CAMERA),
    OBJ(4b6d412f, 5d1e1416, "Light", 5b8a05d5, 31ea28d4, CKCID_LIGHT),
    OBJ(181671e3, 1bdc1503, "2D Entity", 71d80779, 402f42f3, 27),
    OBJ(55ab12cd, 22ae6a8b, "Material", 71d80779, 402f42f3, CKCID_MATERIAL),
    OBJ(155b2870, 183679f8, "Texture", 71d80779, 402f42f3, CKCID_TEXTURE),
    OBJ(24535345, 65d15229, "Mesh", 71d80779, 402f42f3, CKCID_MESH),
    OBJ(5c0f151b, 6f0547fd, "Group", 71d80779, 402f42f3, CKCID_GROUP),
    OBJ(024d52f1, 678223b2, "Array", 71d80779, 402f42f3, CKCID_DATAARRAY),
    OBJ(40194410, 3a773f80, "Sound", 71d80779, 402f42f3, 24),
    OBJ(4bf74e5e, 45f409ef, "Wave Sound", 40194410, 3a773f80, 25),
    OBJ(10584787, 76932f77, "Level", 71d80779, 402f42f3, CKCID_LEVEL),
};

const CkParamType *ck_type(CkGuid g)
{
    for (size_t i = 0; i < sizeof TYPES / sizeof *TYPES; i++)
        if (ck_guid_eq(TYPES[i].guid, g)) return &TYPES[i];
    return NULL;
}

CkGuid ck_type_parent(CkGuid g)
{
    const CkParamType *t = ck_type(g);
    CkGuid none = {0, 0};
    return t ? t->parent : none;
}

bool ck_type_derives(CkGuid g, CkGuid base)
{
    for (int depth = 0; depth < 16 && (g.a || g.b); depth++) {
        if (ck_guid_eq(g, base)) return true;
        g = ck_type_parent(g);
    }
    return false;
}

uint32_t ck_type_class(CkGuid g)
{
    const CkParamType *t = ck_type(g);
    return t && t->kind == CKT_OBJECT ? t->class_id : 0;
}

/* ---- string functions (CK2.dll: Integer FUN_24033ba2 "%d", Float FUN_240342ce "%g" / "%f",
   Boolean FUN_24034228 "TRUE"/"FALSE") ---- */

static int32_t as_int(const CkParameter *p) { int32_t v = 0; if (p && p->value && p->size >= 4) memcpy(&v, p->value, 4); return v; }
static float as_float(const CkParameter *p) { float v = 0; if (p && p->value && p->size >= 4) memcpy(&v, p->value, 4); return v; }

uint32_t ck_param_to_string(CkContext *ctx, const CkParameter *p, char *out, uint32_t cap)
{
    char buf[256];
    buf[0] = 0;
    const CkParamType *t = ck_type(p->type);
    CkTypeKind k = t ? t->kind : CKT_RAW;
    switch (k) {
    case CKT_INT: snprintf(buf, sizeof buf, "%d", as_int(p)); break;
    case CKT_FLOAT: snprintf(buf, sizeof buf, "%g", (double)as_float(p)); break;
    case CKT_BOOL: snprintf(buf, sizeof buf, "%s", as_int(p) ? "TRUE" : "FALSE"); break;
    case CKT_STRING:
        if (p->value) snprintf(buf, sizeof buf, "%.*s", (int)p->size, (const char *)p->value);
        break;
    case CKT_OBJECT: {
        CkObj *o = ck_obj(ctx, (CkId)as_int(p));
        snprintf(buf, sizeof buf, "%s", o ? o->name : "NULL");   /* TODO: exact CK2 object string function */
        break;
    }
    case CKT_MESSAGE: snprintf(buf, sizeof buf, "%s", ck_message_name(ctx, as_int(p))); break;
    default: break;   /* TODO: vector/color/rect formats */
    }
    uint32_t n = (uint32_t)strlen(buf);
    if (out && cap) snprintf(out, cap, "%s", buf);
    return n;
}

void ck_param_from_string(CkContext *ctx, CkParameter *p, const char *s)
{
    const CkParamType *t = ck_type(p->type);
    CkTypeKind k = t ? t->kind : CKT_RAW;
    if (!s) s = "";
    switch (k) {
    case CKT_INT: { int32_t v = 0; sscanf(s, "%d", &v); ck_param_set(p, &v, 4); break; }
    case CKT_FLOAT: { float v = 0; sscanf(s, "%f", &v); ck_param_set(p, &v, 4); break; }
    case CKT_BOOL: { char b[8] = {0}; strncpy(b, s, 5); int32_t v = !strcasecmp(b, "TRUE"); ck_param_set(p, &v, 4); break; }
    case CKT_STRING: ck_param_set(p, s, (uint32_t)strlen(s) + 1); break;
    case CKT_OBJECT: {
        /* object string function: the object of the type's class with that name */
        CkId id = 0;
        uint32_t cid = t->class_id;
        for (uint32_t i = 0; i < ctx->nobjs && !id; i++) {
            CkObj *o = ctx->objs[i];
            if (o && !strcmp(o->name, s) && ck_class_derives(o->cid, cid)) id = o->id;
        }
        ck_param_set(p, &id, 4);
        break;
    }
    case CKT_MESSAGE: { int32_t v = ck_message_type(ctx, s); ck_param_set(p, &v, 4); break; }
    default: break;
    }
}
